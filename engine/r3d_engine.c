/****************************************************************************
 *
 * Copyright (C) 2025 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

/*
 * r3d_engine.c — r3d 设备运行时封装（对照 rive_for_vglite/src/rive_engine.cpp）。
 * 打开 NuttX framebuffer + gpu_init + mmap，把 fb 包成 r3d_target_t，
 * 驱动 r3d 核心引擎(VGLite 后端)逐帧渲染并翻页上屏。
 * 轨道相机默认自旋(与 tools/demo/demo_viewer.c 一致)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <math.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <nuttx/video/fb.h>
#include <vg_lite.h>

#include "r3d/r3d_backend.h"
#include "r3d/r3d_model.h"
#include "r3d/r3d_math.h"
#include "r3d/r3d_anim.h"
#include "r3d/r3d_deform.h"
#include "r3d/r3d_skin.h"
#include "r3d/r3d_engine.h"

/* vg_lite GPU 初始化(与 LVGL/rive 共用)。完成 hw reset/内存/中断配置。 */
extern void gpu_init(void);

#define LOG_TAG "r3d_engine"

/* 流程日志：定位卡死/出错的阶段 */
#define R3D_TRACE(fmt, ...) \
    syslog(LOG_INFO, "%s: " fmt "\n", LOG_TAG, ##__VA_ARGS__)

/* 性能日志：每秒一次输出 CPU 顶点变形(morph/skin)耗时。
 * 后端(backend_vglite)已统计 collect/sort/submit/gpu；这里补齐变形阶段，
 * 让 morph 动画的完整 CPU 链路(变形→收集→提交)都可量化。 */
#define R3D_PERF(fmt, ...) \
    syslog(LOG_INFO, "%s: " fmt "\n", LOG_TAG, ##__VA_ARGS__)

/* 单调微秒，用于阶段计时 */
static long r3d_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000);
}

/****************************************************************************
 * 引擎上下文
 ****************************************************************************/

typedef struct {
    /* framebuffer */
    int                 fb_fd;
    void               *fb_mem;
    size_t              fb_mem_size;
    void               *fb_mem2;
    size_t              fb_mem2_size;
    struct fb_planeinfo_s pinfo;
    int                 fb_w, fb_h, fb_stride;
    r3d_pixel_format_t  fmt;
    int                 double_buffer;
    uint32_t            mem2_yoffset;
    int                 cur_buf;

    /* 两个渲染目标(双缓冲时各指一块 fb 内存) */
    r3d_target_t        rt[2];

    /* GPU 所有权 */
    int                 gpu_owned;

    /* 帧计数(用于限制前几帧的详细日志) */
    uint32_t            frame_no;

    /* CPU 顶点变形(morph/skin)耗时秒窗口统计(微秒) */
    long                perf_window_us;   /* 窗口起点 */
    uint32_t            perf_frames;      /* 窗口内帧数 */
    uint64_t            perf_deform_sum;  /* 变形累计耗时 */
    uint32_t            perf_deform_max;  /* 变形单帧峰值 */

    /* 上一帧的翻页(FBIOPAN_DISPLAY)耗时(微秒)。翻页发生在 end_frame 之后，
     * 本帧的还未知，故按 1 帧延迟随下一帧的 perf_frame_mark 上报。 */
    long                prev_pan_us;

    /* 画面垂直偏移(像素，+ 向下)。用投影矩阵的垂直镜头偏移实现：给 proj 的
     * y 行/z 列加常数等价于 ndc.y 减常数，是纯屏幕空间平移，物体轮廓仍是同
     * 半径的圆，只是圆心跟着移。圆屏表盘想在上方腾出空间时用。
     * 这一项必须在引擎里 —— 投影矩阵由引擎构造，外部拿不到。 */
    float               view_dy;

    /* 上一帧算出的物体屏幕半径(像素)。供上层按实际尺寸排布覆盖层，
     * 避免把布局写死成像素常量。首帧为 0，上层需兜底。 */
    float               last_r_screen;

    /* 渲染后端 */
    r3d_backend_t      *be;
} r3d_engine_ctx_t;

/* 单个已加载模型实例 */
typedef struct {
    r3d_model_t       *model;

    r3d_anim_set_t     aset;
    r3d_anim_state_t   ast;
    int                animated;

    r3d_deform_t       deform;
    int                has_morph;

    r3d_skin_t         skin;
    int                has_skin;

    /* orbit 相机 */
    r3d_vec3_t         center;
    float              radius;
    /* 相机姿态用四元数，不再用 yaw/pitch 欧拉角。
     * 球坐标 + 固定世界 up 在 pitch→±90° 处有两重退化：水平半径 dist*cos(pitch)
     * 趋 0 使 yaw 失效(万向锁)，且视线与 up 平行使 look_at 的 cross 退化。
     * 四元数下 eye = center + q*(0,0,dist)、up = q*(0,1,0)，增量按局部轴后乘，
     * 不存在被偏爱的世界轴，故无锁、无需夹角、可连续越过极点。 */
    r3d_quat_t         orient;
    float              dist0, dist;
    int                autospin;
} r3d_anim_instance_t;

static r3d_engine_ctx_t *g_ctx;

/****************************************************************************
 * framebuffer 辅助(对照 rive_engine.cpp)
 ****************************************************************************/

static r3d_pixel_format_t fb_fmt_to_r3d(uint8_t nuttx_fmt)
{
    switch (nuttx_fmt) {
        case FB_FMT_RGB32:     return R3D_FMT_BGRA8888; /* B,G,R,A in mem */
        case FB_FMT_RGB24:     return R3D_FMT_BGR888;   /* B,G,R 打包 3 字节/像素
                                                         * (p62: CONFIG_LCDC_L1_RGB888) */
        case FB_FMT_RGB16_565: return R3D_FMT_RGB565;
        default:
            syslog(LOG_WARNING, "%s: unknown FB fmt %d, assume BGRA8888\n",
                   LOG_TAG, nuttx_fmt);
            return R3D_FMT_BGRA8888;
    }
}

static void init_target(r3d_target_t *t, int w, int h, int stride,
                        r3d_pixel_format_t fmt, void *mem, uintptr_t phys)
{
    memset(t, 0, sizeof(*t));
    t->pixels         = mem;
    t->w              = (uint32_t)w;
    t->h              = (uint32_t)h;
    t->stride         = (uint32_t)stride;
    t->format         = fmt;
    t->phys_addr      = phys;
    t->native_surface = NULL;
}

static int fbdev_get_pinfo(int fd, struct fb_planeinfo_s *pinfo)
{
    if (ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)pinfo) < 0) {
        syslog(LOG_ERR, "%s: FBIOGET_PLANEINFO(disp %u) failed: %d\n",
               LOG_TAG, (unsigned)pinfo->display, errno);
        return -errno;
    }
    return 0;
}

static int fbdev_init_mem2(r3d_engine_ctx_t *ctx)
{
    struct fb_planeinfo_s pinfo1, pinfo2;

    memset(&pinfo1, 0, sizeof(pinfo1));
    pinfo1.display = ctx->pinfo.display;
    if (fbdev_get_pinfo(ctx->fb_fd, &pinfo1) < 0) return -1;

    memset(&pinfo2, 0, sizeof(pinfo2));
    pinfo2.display = ctx->pinfo.display + 1;
    if (fbdev_get_pinfo(ctx->fb_fd, &pinfo2) < 0) return -1;

    uintptr_t phy1 = (uintptr_t)pinfo1.fbmem;
    uintptr_t phy2 = (uintptr_t)pinfo2.fbmem;
    uintptr_t offset = phy2 - phy1;
    int consecutive = (offset == 0);

    off_t mmap_off;
    if (consecutive) {
        ctx->mem2_yoffset = ctx->fb_h;
        mmap_off = (off_t)(ctx->fb_h * ctx->pinfo.stride);
    } else {
        ctx->mem2_yoffset = offset / ctx->pinfo.stride;
        mmap_off = (off_t)offset;
    }

    size_t fblen = consecutive ? pinfo2.fblen / 2 : pinfo2.fblen;
    ctx->fb_mem2 = mmap(NULL, fblen, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FILE, ctx->fb_fd, mmap_off);
    if (ctx->fb_mem2 == MAP_FAILED) {
        syslog(LOG_ERR, "%s: mmap mem2 failed: %d\n", LOG_TAG, errno);
        ctx->fb_mem2 = NULL;
        return -1;
    }
    ctx->fb_mem2_size = fblen;
    return 0;
}

/****************************************************************************
 * 引擎生命周期
 ****************************************************************************/

int r3d_engine_init(const char *fb_dev)
{
    if (g_ctx != NULL) {
        syslog(LOG_ERR, "%s: already initialized\n", LOG_TAG);
        return R3D_ENGINE_ERR_INIT;
    }
    if (fb_dev == NULL) return R3D_ENGINE_ERR_PARAM;

    r3d_engine_ctx_t *ctx = (r3d_engine_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return R3D_ENGINE_ERR_NOMEM;
    ctx->fb_fd = -1;

    /* vg_lite GPU 初始化(与 LVGL/rive 共用 gpu_init)。
     * 运行期静态守卫(镜像 rive_engine.cpp 的 gpuInited)：gpu_init 幂等，
     * 第一个调用者执行初始化并拥有 GPU；后续使用方(如 LVGL 已先初始化)
     * 跳过 gpu_init 且不拥有 GPU，从而避免重复初始化或提前 vg_lite_close。 */
    static bool s_gpu_inited = false;
    if (!s_gpu_inited) {
        gpu_init();
        s_gpu_inited = true;
        ctx->gpu_owned = 1;
    } else {
        ctx->gpu_owned = 0;
    }

    ctx->fb_fd = open(fb_dev, O_RDWR);
    if (ctx->fb_fd < 0) {
        syslog(LOG_ERR, "%s: open(%s) failed: %d\n", LOG_TAG, fb_dev, errno);
        free(ctx);
        return R3D_ENGINE_ERR_FB;
    }

    int power = 1;
    if (ioctl(ctx->fb_fd, FBIOSET_POWER, (unsigned long)(uintptr_t)&power) < 0)
        syslog(LOG_WARNING, "%s: FBIOSET_POWER failed: %d (non-fatal)\n",
               LOG_TAG, errno);

    struct fb_videoinfo_s vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    if (ioctl(ctx->fb_fd, FBIOGET_VIDEOINFO, (unsigned long)(uintptr_t)&vinfo) < 0) {
        syslog(LOG_ERR, "%s: FBIOGET_VIDEOINFO failed: %d\n", LOG_TAG, errno);
        goto fail_fb;
    }

    struct fb_planeinfo_s pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    if (ioctl(ctx->fb_fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)&pinfo) < 0) {
        syslog(LOG_ERR, "%s: FBIOGET_PLANEINFO failed: %d\n", LOG_TAG, errno);
        goto fail_fb;
    }

    ctx->fb_mem_size = pinfo.fblen;
    ctx->fb_mem = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FILE, ctx->fb_fd, 0);
    if (ctx->fb_mem == MAP_FAILED) {
        syslog(LOG_ERR, "%s: mmap failed: %d\n", LOG_TAG, errno);
        ctx->fb_mem = NULL;
        goto fail_fb;
    }

    ctx->fb_w = vinfo.xres;
    ctx->fb_h = vinfo.yres;
    ctx->fb_stride = pinfo.stride;
    memcpy(&ctx->pinfo, &pinfo, sizeof(pinfo));
    ctx->fmt = fb_fmt_to_r3d(vinfo.fmt);

    init_target(&ctx->rt[0], vinfo.xres, vinfo.yres, pinfo.stride,
                ctx->fmt, ctx->fb_mem, (uintptr_t)pinfo.fbmem);

    /* 双缓冲检测：yres_virtual == yres*2 */
    ctx->double_buffer = (pinfo.yres_virtual == (vinfo.yres * 2));
    if (ctx->double_buffer) {
        if (fbdev_init_mem2(ctx) < 0) {
            syslog(LOG_WARNING, "%s: double buffer init failed, single\n", LOG_TAG);
            ctx->double_buffer = 0;
        } else {
            uintptr_t phys2 = (uintptr_t)pinfo.fbmem +
                              (uintptr_t)ctx->mem2_yoffset * pinfo.stride;
            init_target(&ctx->rt[1], vinfo.xres, vinfo.yres, pinfo.stride,
                        ctx->fmt, ctx->fb_mem2, phys2);
        }
    }
    ctx->cur_buf = 0;

    /* 创建 VGLite 后端(宿主模式：GPU 已由 gpu_init 初始化) */
    ctx->be = r3d_backend_vglite_create_hosted();
    if (!ctx->be || ctx->be->vt->init(ctx->be, NULL) != R3D_OK) {
        syslog(LOG_ERR, "%s: r3d vglite backend init failed\n", LOG_TAG);
        if (ctx->be) ctx->be->vt->destroy(ctx->be);
        goto fail_map;
    }

    g_ctx = ctx;
    syslog(LOG_INFO, "%s: initialized (%dx%d stride=%d fmt=%d %s)\n",
           LOG_TAG, ctx->fb_w, ctx->fb_h, ctx->fb_stride, (int)ctx->fmt,
           ctx->double_buffer ? "double" : "single");

    return R3D_ENGINE_OK;

fail_map:
    if (ctx->fb_mem2) munmap(ctx->fb_mem2, ctx->fb_mem2_size);
    if (ctx->fb_mem)  munmap(ctx->fb_mem, ctx->fb_mem_size);
fail_fb:
    if (ctx->fb_fd >= 0) close(ctx->fb_fd);
    if (ctx->gpu_owned) vg_lite_close();
    free(ctx);
    return R3D_ENGINE_ERR_FB;
}

int r3d_engine_deinit(void)
{
    if (g_ctx == NULL) return R3D_ENGINE_ERR_INIT;
    r3d_engine_ctx_t *ctx = g_ctx;
    g_ctx = NULL;

    if (ctx->be) ctx->be->vt->destroy(ctx->be);
    if (ctx->gpu_owned) vg_lite_close();
    if (ctx->fb_mem2) munmap(ctx->fb_mem2, ctx->fb_mem2_size);
    if (ctx->fb_mem)  munmap(ctx->fb_mem, ctx->fb_mem_size);
    if (ctx->fb_fd >= 0) close(ctx->fb_fd);
    free(ctx);
    syslog(LOG_INFO, "%s: deinitialized\n", LOG_TAG);
    return R3D_ENGINE_OK;
}

/****************************************************************************
 * 模型加载
 ****************************************************************************/

static void setup_camera(r3d_anim_instance_t *a)
{
    r3d_model_t *m = a->model;

    /* 计算“真实显示空间”的包围盒。
     * 关键：DYNAMIC_NODE 的 submesh 顶点未烘焙节点变换，存的是量化/原始空间的
     * 大坐标(facecap 顶点量级达数千~万)，运行时靠 node 链矩阵(含 scale)还原到
     * 真实尺寸(~2 单位)。而 header 的 bounding_sphere 是量化/原始空间的，直接拿
     * 来设相机会让相机对准错误位置、距离也差几个数量级 → 模型整片落在视锥外被
     * 裁掉(现象：tri/f=0，全黑)。
     * 故这里对每个 submesh 顶点套用其 node 矩阵(默认姿势)后再求 AABB，得到与
     * 运行时渲染一致的真实包围盒。 */
    float bmin[3] = { 1e30f, 1e30f, 1e30f };
    float bmax[3] = { -1e30f, -1e30f, -1e30f };
    int have_box = 0;

    for (uint32_t s = 0; s < m->submesh_count; s++) {
        r3d_submesh_t *sm = &m->submeshes[s];
        r3d_mat4_t nm;
        int dyn = (sm->mat_flags & R3D_MAT_DYNAMIC_NODE) && a->animated;
        if (dyn) r3d_anim_node_matrix(m, &a->ast, sm->node_id, &nm);
        else     r3d_mat4_identity(&nm);

        uint32_t end = sm->index_offset + sm->index_count;
        if (end > m->index_count) end = m->index_count;
        for (uint32_t i = sm->index_offset; i < end; i++) {
            uint32_t vi = r3d_index_at(m->indices, m->index_size, i);
            if (vi >= m->vertex_count) continue;
            r3d_vec3_t p = m->vertices[vi].pos;
            float wx, wy, wz;
            if (dyn) {
                /* world = nm × p (列主序) */
                wx = nm.m[0]*p.x + nm.m[4]*p.y + nm.m[8]*p.z  + nm.m[12];
                wy = nm.m[1]*p.x + nm.m[5]*p.y + nm.m[9]*p.z  + nm.m[13];
                wz = nm.m[2]*p.x + nm.m[6]*p.y + nm.m[10]*p.z + nm.m[14];
            } else { wx = p.x; wy = p.y; wz = p.z; }
            if (wx < bmin[0]) bmin[0] = wx;
            if (wx > bmax[0]) bmax[0] = wx;
            if (wy < bmin[1]) bmin[1] = wy;
            if (wy > bmax[1]) bmax[1] = wy;
            if (wz < bmin[2]) bmin[2] = wz;
            if (wz > bmax[2]) bmax[2] = wz;
            have_box = 1;
        }
    }

    if (!have_box) {
        /* 回退：用 header bounding_sphere 推得的 AABB */
        bmin[0] = m->bounds.min.x; bmin[1] = m->bounds.min.y; bmin[2] = m->bounds.min.z;
        bmax[0] = m->bounds.max.x; bmax[1] = m->bounds.max.y; bmax[2] = m->bounds.max.z;
    }

    /* 半径取三轴最大 extent，确保任意旋转角度都框得下(尤其 Z 方向很长的模型) */
    float rx = bmax[0] - bmin[0];
    float ry = bmax[1] - bmin[1];
    float rz = bmax[2] - bmin[2];
    float r = rx;
    if (ry > r) r = ry;
    if (rz > r) r = rz;
    if (r < 1e-3f) r = 1.0f;
    a->radius = r;
    a->center = (r3d_vec3_t){ (bmin[0] + bmax[0]) * 0.5f,
                              (bmin[1] + bmax[1]) * 0.5f,
                              (bmin[2] + bmax[2]) * 0.5f };
    /* 等价于原来的 yaw=0, pitch=0.4：q = Ry(yaw) * Rx(-pitch) */
    a->orient = r3d_quat_mul(
        r3d_quat_from_axis_angle((r3d_vec3_t){0.0f, 1.0f, 0.0f}, 0.0f),
        r3d_quat_from_axis_angle((r3d_vec3_t){1.0f, 0.0f, 0.0f}, -0.4f));
    a->dist0 = r * 1.6f + 0.5f;
    a->dist = a->dist0;
    a->autospin = 1;
}

/* 把已加载的 r3d_model 封装为播放实例(接管 model 所有权)。失败返回 NULL。 */
static r3d_engine_handle make_instance(r3d_model_t *model)
{
    r3d_anim_instance_t *a = (r3d_anim_instance_t *)calloc(1, sizeof(*a));
    if (!a) { r3d_model_free(model); return NULL; }
    a->model = model;

    /* 动画 */
    r3d_anim_set_parse(&a->aset, a->model->raw);
    r3d_anim_state_init(&a->ast);
    if (a->aset.clip_count > 0) {
        r3d_anim_play(&a->ast, &a->aset, NULL, true);
        a->animated = 1;
    }

    /* morph */
    if (a->model->morph_target_count > 0) {
        if (r3d_deform_init(&a->deform, a->model) == 0) a->has_morph = 1;
    }

    /* skin */
    if (a->model->joint_count > 0 && a->model->skinvtx && a->model->nodes) {
        if (r3d_skin_init(&a->skin, a->model) == 0) a->has_skin = 1;
    }

    setup_camera(a);

    syslog(LOG_INFO, "%s: loaded v=%u i=%u sm=%u tex=%u anim=%d morph=%d skin=%d\n",
           LOG_TAG, (unsigned)a->model->vertex_count, (unsigned)a->model->index_count,
           (unsigned)a->model->submesh_count, (unsigned)a->model->texture_count,
           a->animated, a->has_morph, a->has_skin);

    return (r3d_engine_handle)a;
}

r3d_engine_handle r3d_engine_load(const void *data, uint32_t size)
{
    if (g_ctx == NULL) return NULL;
    if (data == NULL || size == 0) return NULL;

    /* r3d_model_load_mem 接管 buffer 所有权(free 时一并释放 raw)，
     * 因此这里拷一份独立 buffer 交给它；加载失败时由本函数释放。 */
    void *buf = malloc(size);
    if (!buf) return NULL;
    memcpy(buf, data, size);

    r3d_model_t *model = r3d_model_load_mem(g_ctx->be, buf, size);
    if (!model) {
        syslog(LOG_ERR, "%s: r3d_model_load_mem failed\n", LOG_TAG);
        free(buf);
        return NULL;
    }
    return make_instance(model);
}

r3d_engine_handle r3d_engine_load_file(const char *path)
{
    if (g_ctx == NULL || path == NULL) return NULL;

    /* 直接走核心加载器：它自管文件 buffer 的所有权(避免二次拷贝)。 */
    r3d_model_t *model = r3d_model_load(g_ctx->be, path);
    if (!model) {
        syslog(LOG_ERR, "%s: r3d_model_load(%s) failed\n", LOG_TAG, path);
        return NULL;
    }
    /* 性能：为新模型分段——后端会转储上一个模型残留帧、打印 syslog 模型标记、
     * 并把帧号/缓冲/统计窗口清零，使每个 b3dm 逐帧数据自成一段(离线可精确分类)。 */
    if (g_ctx->be->vt->perf_model_begin) {
        const char *base = strrchr(path, '/');
        g_ctx->be->vt->perf_model_begin(g_ctx->be, base ? base + 1 : path);
    }
    return make_instance(model);
}

void r3d_engine_unload(r3d_engine_handle handle)
{
    if (handle == NULL) return;
    vg_lite_finish();
    r3d_anim_instance_t *a = (r3d_anim_instance_t *)handle;
    if (a->has_skin)  r3d_skin_free(&a->skin);
    if (a->has_morph) r3d_deform_free(&a->deform);
    r3d_anim_set_free(&a->aset);
    r3d_model_free(a->model);   /* 释放 model + raw buffer + 纹理 */
    free(a);
}

/****************************************************************************
 * 渲染
 ****************************************************************************/

int r3d_engine_set_autospin(r3d_engine_handle handle, int enable)
{
    if (!handle) return R3D_ENGINE_ERR_PARAM;
    ((r3d_anim_instance_t *)handle)->autospin = enable ? 1 : 0;
    return R3D_ENGINE_OK;
}

void r3d_engine_set_orbit(r3d_engine_handle handle,
                          float yaw, float pitch, float dist_scale)
{
    if (!handle) return;
    r3d_anim_instance_t *a = (r3d_anim_instance_t *)handle;
    a->orient = r3d_quat_normalize(r3d_quat_mul(
        r3d_quat_from_axis_angle((r3d_vec3_t){0.0f, 1.0f, 0.0f}, yaw),
        r3d_quat_from_axis_angle((r3d_vec3_t){1.0f, 0.0f, 0.0f}, -pitch)));
    if (dist_scale > 0.0f) a->dist = a->dist0 * dist_scale;
}

/* 增量旋转(轨迹球)。d_yaw 绕相机当前的局部 up 轴、d_pitch 绕局部 right 轴，
 * 均为后乘 —— 局部轴随姿态一起转，所以任何姿态下拖拽灵敏度都一致，
 * 也能连续越过极点，不需要夹角。交互式拖拽应优先用本接口而非 set_orbit
 * (后者经欧拉角，仍受其表达能力限制)。 */
void r3d_engine_orbit_delta(r3d_engine_handle handle,
                            float d_yaw, float d_pitch, float dist_scale)
{
    if (!handle) return;
    r3d_anim_instance_t *a = (r3d_anim_instance_t *)handle;
    if (d_yaw != 0.0f)
        a->orient = r3d_quat_mul(a->orient,
            r3d_quat_from_axis_angle((r3d_vec3_t){0.0f, 1.0f, 0.0f}, d_yaw));
    /* 注意 X 轴取负：后乘 Rx(θ) 会把偏移 (0,0,d) 变成 (0,-d*sinθ,d*cosθ)，
     * 即 θ>0 让相机向屏幕下方走、模型看着向上跑，与手指反向。取负后与
     * set_orbit 的 Rx(-pitch) 一致，也与旧球坐标 eye.y=d*sin(pitch) 同向。
     * (tests/test_drag_direction.c 端到端投影验证方向) */
    if (d_pitch != 0.0f)
        a->orient = r3d_quat_mul(a->orient,
            r3d_quat_from_axis_angle((r3d_vec3_t){1.0f, 0.0f, 0.0f}, -d_pitch));
    /* 增量长期累积会让模长漂移，每次归一化(成本可忽略) */
    a->orient = r3d_quat_normalize(a->orient);
    if (dist_scale > 0.0f) a->dist = a->dist0 * dist_scale;
}

void r3d_engine_set_zoom(r3d_engine_handle handle, float dist_scale)
{
    if (!handle || dist_scale <= 0.0f) return;
    r3d_anim_instance_t *a = (r3d_anim_instance_t *)handle;
    a->dist = a->dist0 * dist_scale;   /* 仅改距离，不动 yaw/pitch(不打断自旋) */
}

int r3d_engine_render_frame(r3d_engine_handle handle, float elapsed)
{
    if (g_ctx == NULL) return R3D_ENGINE_ERR_INIT;
    if (handle == NULL) return R3D_ENGINE_ERR_PARAM;

    r3d_engine_ctx_t *ctx = g_ctx;
    r3d_anim_instance_t *a = (r3d_anim_instance_t *)handle;
    r3d_model_t *m = a->model;

    /* 仅前几帧打详细流程日志，避免刷屏 */
    int trace = (ctx->frame_no < 3);
    if (trace) R3D_TRACE("render_frame #%u begin (elapsed=%d ms)",
                         (unsigned)ctx->frame_no, (int)(elapsed * 1000.0f));

    /* 引擎侧逐帧各阶段耗时(微秒)，帧末经 perf_frame_mark 上报后端。 */
    long wait_us = 0, anim_us = 0, node_us = 0;

    /* 双缓冲：等待空闲缓冲(vsync 相关空闲等待，非 CPU 工作) */
    if (ctx->double_buffer) {
        if (trace) R3D_TRACE("  [1] poll(POLLOUT) wait free buffer...");
        struct pollfd pfds[1];
        memset(pfds, 0, sizeof(pfds));
        pfds[0].fd = ctx->fb_fd;
        pfds[0].events = POLLOUT;
        long wt0 = r3d_now_us();
        poll(pfds, 1, -1);
        wait_us = r3d_now_us() - wt0;
        if (trace) R3D_TRACE("  [1] poll done");
    }

    int back = ctx->double_buffer ? (ctx->cur_buf ^ 1) : 0;

    /* 自旋(无外部交互时缓慢旋转，与 demo_viewer 一致：约 0.4 rad/s) */
    /* 自旋(约 0.4 rad/s)。这里必须"前乘"世界 Y：物体绕自身竖轴转，
     * 与相机当前朝向无关。若后乘局部 Y，拖拽改变姿态后自旋轴会跟着歪掉。 */
    if (a->autospin) {
        a->orient = r3d_quat_normalize(r3d_quat_mul(
            r3d_quat_from_axis_angle((r3d_vec3_t){0.0f, 1.0f, 0.0f},
                                     0.4f * elapsed),
            a->orient));
    }

    /* eye = center + q*(0,0,dist)，up = q*(0,1,0)。
     * up 由姿态给出而非固定世界 Y，故视线与 up 永不平行，look_at 不会退化。 */
    r3d_vec3_t off = r3d_quat_rotate_vec3(a->orient,
                                          (r3d_vec3_t){0.0f, 0.0f, a->dist});
    r3d_vec3_t eye = { a->center.x + off.x,
                       a->center.y + off.y,
                       a->center.z + off.z };
    r3d_vec3_t up = r3d_quat_rotate_vec3(a->orient,
                                         (r3d_vec3_t){0.0f, 1.0f, 0.0f});
    r3d_camera_t cam;
    r3d_mat4_look_at(&cam.view, eye, a->center, up);
    /* 近/远平面按相机距离+模型半径自适应：固定 0.05/100 对大模型(如 Fox，
     * 对角线可达 ~200)会让远端顶点超出远平面、近端跨近平面被整片丢弃 → 闪烁。
     * 远平面留足模型对角线余量，近平面取距离的小比例但不小于 0.05。 */
    float far_plane = a->dist + a->radius * 2.0f + 1.0f;
    float near_plane = a->dist * 0.05f;
    if (near_plane < 0.05f) near_plane = 0.05f;
    r3d_mat4_perspective(&cam.proj, 1.0f,
                         (float)ctx->fb_w / (float)ctx->fb_h, near_plane, far_plane);
    /* 垂直镜头偏移：m[9] 是列主序下的 (y 行, z 列)。clip.y = m[5]*vy + m[9]*vz，
     * clip.w = -vz，故 ndc.y = m[5]*vy/(-vz) - m[9]，即整幅画面平移。
     * 屏幕 y = (1-(ndc*0.5+0.5))*h，故 Δy_screen = 0.5*h*m[9] → m[9] = 2*dy/h。 */
    if (ctx->view_dy != 0.0f)
        cam.proj.m[9] = 2.0f * ctx->view_dy / (float)ctx->fb_h;
    cam.viewport = (r3d_viewport_t){ 0, 0, ctx->fb_w, ctx->fb_h };

    if (trace) R3D_TRACE("  [2] begin_frame back=%d", back);
    ctx->be->vt->begin_frame(ctx->be, &ctx->rt[back]);
    if (trace) R3D_TRACE("  [3] set_camera");
    ctx->be->vt->set_camera(ctx->be, &cam);

    /* 记录物体轮廓的屏幕半径，供上层(覆盖层排版等)查询。
     * 相机恒定看向 a->center，故圆心就是视口中心 + view_dy。
     * a->radius 存的是最大轴向 extent(直径)，取半才是包围球半径。
     * 轮廓的屏幕半径由切线张角决定：
     *   theta = asin(R/d)，r = (vp_h/2) * tan(theta) / tan(fovy/2)
     * 不能简单用 R/d 线性外推 —— 透视下切点不在球心平面上。 */
    {
        float sphere_r = a->radius * 0.5f;
        ctx->last_r_screen = 0.0f;
        if (a->dist > sphere_r) {
            float theta = asinf(sphere_r / a->dist);
            ctx->last_r_screen = ((float)ctx->fb_h * 0.5f)
                               * tanf(theta) / tanf(0.5f);
        }
    }

    /* model 矩阵：动画驱动 root，否则单位 */
    r3d_mat4_t model;
    r3d_mat4_identity(&model);
    if (a->animated) {
        long at0 = r3d_now_us();
        r3d_anim_update(&a->ast, elapsed);
        anim_us = r3d_now_us() - at0;
    }
    if (a->animated && m->node_count <= 1) model = a->ast.root_matrix;

    /* 顶点变形：skin 优先于 morph */
    const r3d_vertex_t *verts = m->vertices;
    int dynamic = 0;
    long deform_us = 0;                  /* 本帧 CPU 变形耗时(morph/skin) */
    if (a->has_skin) {
        if (trace) R3D_TRACE("  [4] skin_update");
        long t0 = r3d_now_us();
        r3d_skin_update(&a->skin, m, &a->ast);
        deform_us = r3d_now_us() - t0;
        verts = a->skin.out;
        r3d_mat4_identity(&model);
        dynamic = 1;
    } else if (a->has_morph) {
        if (trace) R3D_TRACE("  [4] deform_apply");
        long t0 = r3d_now_us();
        r3d_deform_apply(&a->deform, m, a->ast.morph_weights, a->ast.morph_weight_count);
        deform_us = r3d_now_us() - t0;
        verts = a->deform.out;
        dynamic = 1;
    }

    /* CPU 变形耗时每秒汇总一次(morph 动画的关键 CPU 成本之一)。 */
    {
        ctx->perf_frames++;
        ctx->perf_deform_sum += (uint64_t)(deform_us < 0 ? 0 : deform_us);
        if ((uint32_t)deform_us > ctx->perf_deform_max)
            ctx->perf_deform_max = (uint32_t)deform_us;
        long pnow = r3d_now_us();
        if (ctx->perf_window_us == 0) ctx->perf_window_us = pnow;
        if (pnow - ctx->perf_window_us >= 1000000L && ctx->perf_frames > 0) {
            R3D_PERF("perf-cpu(us): deform avg=%u max=%u over %u frames (%s)",
                     (unsigned)(ctx->perf_deform_sum / ctx->perf_frames),
                     (unsigned)ctx->perf_deform_max, (unsigned)ctx->perf_frames,
                     a->has_skin ? "skin" : (a->has_morph ? "morph" : "static"));
            /* deform 细分诊断：定位 37ms 根因——是 target 多(计算量)还是
             * reset/accum 分段哪个贵(cache)。仅 morph 路径有意义。 */
            if (a->has_morph) {
                R3D_PERF("perf-deform: targets=%u active=%u morph_verts=%u "
                         "reset=%uus accum=%uus (last frame)",
                         (unsigned)a->deform.stat_targets,
                         (unsigned)a->deform.stat_active,
                         (unsigned)a->deform.stat_morph_verts,
                         (unsigned)a->deform.stat_reset_us,
                         (unsigned)a->deform.stat_accum_us);
            }
            ctx->perf_window_us = pnow;
            ctx->perf_frames = 0;
            ctx->perf_deform_sum = 0;
            ctx->perf_deform_max = 0;
        }
    }

    /* 两趟：先不透明，后半透明 */
    uint32_t submitted = 0;
    if (trace) R3D_TRACE("  [5] draw submeshes (sm=%u)", (unsigned)m->submesh_count);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t s = 0; s < m->submesh_count; s++) {
            r3d_submesh_t *sm = &m->submeshes[s];
            int tl = (sm->mat_flags & R3D_MAT_TRANSLUCENT) ? 1 : 0;
            if (tl != pass) continue;

            r3d_mesh_t mesh = {0};
            mesh.vertices = verts;
            mesh.vertex_count = m->vertex_count;
            mesh.indices = (const uint8_t *)m->indices
                         + (size_t)sm->index_offset * m->index_size;
            mesh.index_count = sm->index_count;
            mesh.index_size = m->index_size;
            mesh.dynamic = dynamic;

            r3d_material_t mat = {0};
            mat.base_color = sm->base_color;
            mat.matcap = sm->matcap;
            mat.blend = sm->blend;
            mat.flags = sm->mat_flags | R3D_MAT_FLAT_SHADING;
            mat.base_color_factor = sm->base_color_factor;

            r3d_mat4_t smodel = model;
            if ((sm->mat_flags & R3D_MAT_DYNAMIC_NODE) && a->animated) {
                long nt0 = r3d_now_us();
                r3d_anim_node_matrix(m, &a->ast, sm->node_id, &smodel);
                node_us += r3d_now_us() - nt0;   /* 逐 submesh 累计 */
            }

            ctx->be->vt->draw(ctx->be, &mesh, &smodel, &mat);
            submitted++;
        }
    }
    if (trace) R3D_TRACE("  [5] draw done (%u submeshes submitted)", (unsigned)submitted);

    /* 把引擎侧各阶段耗时喂给后端，并入后端统一的逐帧原始性能记录与串口转储。
     * 此处 wait/anim/node/deform 均已知；pan 用上一帧的值(本帧翻页在 end_frame
     * 之后才发生)。后端不支持时该指针为 NULL，判空跳过。 */
    if (ctx->be->vt->perf_frame_mark) {
        r3d_engine_perf_t ep;
        ep.wait_us   = wait_us;
        ep.anim_us   = anim_us;
        ep.node_us   = node_us;
        ep.deform_us = deform_us;
        ep.pan_us    = ctx->prev_pan_us;
        ctx->be->vt->perf_frame_mark(ctx->be, &ep);
    }

    if (trace) R3D_TRACE("  [6] end_frame (GPU flush/finish)...");
    ctx->be->vt->end_frame(ctx->be);
    if (trace) R3D_TRACE("  [6] end_frame done");

    /* 翻页上屏(计时，供下一帧上报) */
    long pt0 = r3d_now_us();
    if (ctx->double_buffer) {
        ctx->pinfo.yoffset = (back == 0) ? 0 : ctx->mem2_yoffset;
        ioctl(ctx->fb_fd, FBIOPAN_DISPLAY, (unsigned long)(uintptr_t)&ctx->pinfo);
        ctx->cur_buf = back;
    } else {
        ioctl(ctx->fb_fd, FBIOPAN_DISPLAY, (unsigned long)(uintptr_t)&ctx->pinfo);
    }
    ctx->prev_pan_us = r3d_now_us() - pt0;
    if (trace) R3D_TRACE("  [7] pan display done, frame #%u end", (unsigned)ctx->frame_no);
    ctx->frame_no++;
    return R3D_ENGINE_OK;
}

/****************************************************************************
 * 截图(PPM)
 ****************************************************************************/

int r3d_engine_screenshot(const char *path)
{
    if (g_ctx == NULL || path == NULL) return R3D_ENGINE_ERR_PARAM;
    r3d_engine_ctx_t *ctx = g_ctx;

    const uint8_t *src = (const uint8_t *)ctx->rt[ctx->cur_buf].pixels;
    if (!src) return R3D_ENGINE_ERR_PARAM;

    FILE *fp = fopen(path, "wb");
    if (!fp) return R3D_ENGINE_ERR_FB;
    fprintf(fp, "P6\n%d %d\n255\n", ctx->fb_w, ctx->fb_h);

    /* 按字节偏移读取，兼容 24bpp(3 字节)与 32bpp(4 字节)。
     * 三种真机格式的内存字节序都是 B,G,R(,A)：
     *   BGRA8888 → 4 字节 B,G,R,A；BGR888 → 3 字节 B,G,R；
     *   ARGB8888(本实现按小端 uint32 读取时)同样是 byte0=B,byte1=G,byte2=R。 */
    int bpp = (ctx->fmt == R3D_FMT_BGR888) ? 3 : 4;
    for (int y = 0; y < ctx->fb_h; y++) {
        const uint8_t *row = src + (size_t)y * ctx->fb_stride;
        for (int x = 0; x < ctx->fb_w; x++) {
            const uint8_t *px = row + (size_t)x * bpp;
            uint8_t b = px[0], g = px[1], r = px[2];
            uint8_t rgb[3] = { r, g, b };
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
    return R3D_ENGINE_OK;
}

/****************************************************************************
 * 光照参数（运行时可调）
 ****************************************************************************/

int r3d_engine_set_lighting(const r3d_light_params_t *lp)
{
    if (g_ctx == NULL || g_ctx->be == NULL) return R3D_ENGINE_ERR_INIT;
    if (g_ctx->be->vt->set_lighting == NULL) return R3D_ENGINE_ERR_PARAM; /* 后端不支持 */
    g_ctx->be->vt->set_lighting(g_ctx->be, lp);
    return R3D_ENGINE_OK;
}

/* 画面垂直偏移(像素，+ 向下)。用投影的垂直镜头偏移实现，是纯屏幕平移，
 * 物体轮廓半径不变。 */
int r3d_engine_set_view_shift(float dy_px)
{
    if (g_ctx == NULL) return R3D_ENGINE_ERR_INIT;
    g_ctx->view_dy = dy_px;
    return R3D_ENGINE_OK;
}

/* 上一帧的物体屏幕半径(像素)，0 = 尚未渲染过。 */
float r3d_engine_get_screen_radius(void)
{
    if (g_ctx == NULL) return 0.0f;
    return g_ctx->last_r_screen;
}

int r3d_engine_set_render_hook(r3d_render_hook_t fn, void *user)
{
    if (g_ctx == NULL || g_ctx->be == NULL) return R3D_ENGINE_ERR_INIT;
    if (g_ctx->be->vt->set_render_hook == NULL) return R3D_ENGINE_ERR_PARAM;
    g_ctx->be->vt->set_render_hook(g_ctx->be, fn, user);
    return R3D_ENGINE_OK;
}

int r3d_engine_set_clear_color(uint32_t argb)
{
    if (g_ctx == NULL || g_ctx->be == NULL) return R3D_ENGINE_ERR_INIT;
    if (g_ctx->be->vt->set_clear_color == NULL) return R3D_ENGINE_ERR_PARAM;
    g_ctx->be->vt->set_clear_color(g_ctx->be, argb);
    return R3D_ENGINE_OK;
}

int r3d_engine_get_lighting(r3d_light_params_t *out)
{
    if (out == NULL) return R3D_ENGINE_ERR_PARAM;
    /* 当前后端不回读，直接给默认值供调用方在此基础上微调 */
    r3d_light_params_default(out);
    return R3D_ENGINE_OK;
}
