/*
 * backend_vglite.c — VGLite 后端(best1600，无可编程 shader)
 *
 * 设计要点(对应评估文档附录 C + VGLite 源码分析)：
 *  - VGLite 是 2.5D 矢量/位图加速器，无顶点/片元 shader、无 z-buffer。
 *  - 3D 网格渲染策略：CPU 端做 MVP 投影 + 透视除法，得到屏幕空间三角形；
 *    每个三角形用 vg_lite_draw_pattern 把烘焙好的纹理按 2D 仿射映射贴上去。
 *  - 纹理坐标→屏幕的映射：由三角形 3 个顶点的 (screen_xy, uv) 解出 2x3 仿射矩阵，
 *    设为 pattern_matrix。纹理本身已离线烘焙好(PBR/AO/matcap/编织)，无需 shader。
 *  - 无 z-buffer：收集整帧三角形，end_frame 时按平均深度排序(画家算法)后绘制，
 *    半透明三角形最后画。
 *  - matcap：CPU 按 view 空间法线算 UV(nv.xy*0.5+0.5)，与普通纹理同样走仿射映射。
 *  - baseColorFactor：用 draw_pattern 的 color 参数整体乘色染色。
 *  - 顶点光照(可选)：CPU 逐顶点算亮度，best1600 支持 LINEAR_GRADIENT，
 *    可用线性渐变近似三角形内亮度过渡；当前先用顶点平均亮度做整体调制(flat)。
 *
 * VGLite 关键约束(best1600_vg_lite_options.h 实测)：
 *  - 无 IM_REPEAT_REFLECT → 纹理不能 GPU 端 REPEAT；我们烘焙时已把平铺烘进纹理 +
 *    UV 归一化到[0,1]，故只需 PATTERN_PAD，契合此限制。
 *  - 无 COLOR_TRANSFORMATION → 不能逐像素颜色矩阵；染色用单一 color 参数(够用)。
 *  - 支持 3x3 矩阵透视项(transform 函数算 pt_w 除法)，但本后端在 CPU 投影后只用
 *    仿射映射贴图，更可控。
 */

#include "r3d/r3d_backend.h"
#include "r3d/r3d_math.h"
#include "vg_lite.h"

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* 日志：仅用于错误与每秒一次的性能统计。
 * 用 syslog；本机(softsim)无 syslog 时退化为 printf。 */
#if defined(__NuttX__) || defined(CONFIG_ARCH)
#include <syslog.h>
#define VGL_LOG(fmt, ...) syslog(LOG_INFO,  "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#define VGL_ERR(fmt, ...) syslog(LOG_ERR,   "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define VGL_LOG(fmt, ...) fprintf(stderr, "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#define VGL_ERR(fmt, ...) fprintf(stderr, "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#endif

/* 返回当前单调毫秒，用于每秒统计窗口计时 */
static long vgl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---------------- 内部数据 ---------------- */

#define VGL_MAX_TRIS_DEFAULT  20000

/* 投影安全阈值：
 *  - VGL_W_EPSILON：裁剪空间 w 小于此值视为在近平面前/相机后，整片丢弃，
 *    避免 1/w 爆炸产生超大屏幕坐标。
 *  - VGL_COORD_LIMIT：屏幕坐标绝对值上限。超大坐标会让 VGLite tessellation
 *    遍历范围爆炸/定点溢出，是已知的 GPU hang 来源；超限三角形直接丢弃。
 *    32768 远超任何真实视口，正常 on/部分 off-screen 三角形不受影响。 */
#define VGL_W_EPSILON   1e-4f
#define VGL_COORD_LIMIT 32768.0f

/* 三角形填充规则。
 * 逐三角形单独绘制，单个三角形 NON_ZERO 与 EVEN_ODD 结果一致；沿用 EVEN_ODD。 */
#define VGL_FILL_RULE  VG_LITE_FILL_EVEN_ODD

/* 周期 flush 阈值：每提交这么多次 draw 调用 flush 一次命令缓冲，
 * 防止超大模型(上万三角形)命令缓冲溢出；帧末再统一 finish 一次。
 * 不再每个 draw 都 flush(那是早期定位 hang 时的临时做法，开销巨大)。 */
#define VGL_FLUSH_BATCH  64

/* path 细分质量。VG_LITE_HIGH：保留三角形边缘抗锯齿，配合逐面 flat 光照
 * 呈现曲面的明暗层次(立体感来源)。 */
#define VGL_FILL_QUALITY  VG_LITE_HIGH

/* 一个待绘制的屏幕空间三角形(投影后收集，end_frame 排序绘制) */
typedef struct {
    float sx[3], sy[3];        /* 屏幕坐标 */
    float uv[3][2];            /* 对应纹理 UV(已归一化或 matcap UV) */
    float depth;               /* 平均视深度，用于画家算法排序 */
    vg_lite_buffer_t *tex;     /* 贴图(NULL=纯色) */
    vg_lite_color_t color;     /* 染色/纯色(ABGR8888，VGLite 内部序) */
    int translucent;           /* 半透明：最后绘制 */
    int blend;                 /* r3d_blend_t */
    uint32_t seq;              /* 收集顺序：深度相等时的稳定 tie-breaker(防止帧间排序抖动闪烁) */
} vgl_tri_t;

/* 纹理句柄实体：包一个 vg_lite_buffer */
typedef struct {
    vg_lite_buffer_t buf;
    int valid;
} vgl_tex_t;

typedef struct {
    /* 帧目标 */
    vg_lite_buffer_t target;
    int target_valid;

    /* GPU 所有权标记(信息用)：GPU 统一由外部 gpu_init 初始化/拥有，
     * 本后端 init/destroy 始终不调用 vg_lite_init/vg_lite_close。
     * 保留此字段仅为兼容 create/create_hosted 两个工厂入口。 */
    int hosted;

    /* 相机 */
    r3d_mat4_t view, proj, view_proj;
    int vp_w, vp_h;

    /* 帧三角形队列 */
    vgl_tri_t *tris;
    uint32_t   tri_count, tri_cap;

    /* 帧内 vg_lite_path_t 数组：每三角形一个，存活到帧末 finish。
     * 对齐 rive_for_vglite 做法：不调用 vg_lite_upload_path，path 顶点数据
     * 由 vg_lite_draw 编入主命令缓冲提交，GPU 随命令流读取。
     * 关键：path 数据缓冲不能放栈上(draw 是异步提交，finish 前 GPU 才真正
     * 读取)，故用一块常驻缓冲 path_data，每三角形 11 个 float。 */
    vg_lite_path_t *vgpaths;
    uint32_t        vgpaths_cap;
    float          *path_data;      /* [vgpaths_cap * 11]，每三角形一段，帧内存活 */

    /* 纹理表(句柄=指针) */
    /* 直接用 vgl_tex_t* 作句柄，无需表 */

    /* ---- 每帧计数(end_frame 末尾汇总进秒窗口) ---- */
    uint32_t draw_calls;       /* 本帧 vg_lite_draw / draw_pattern 调用次数 */
    uint32_t tex_binds;        /* 本帧纹理绑定次数(draw_pattern) */
    uint32_t frame_no;         /* 帧计数 */

    /* ---- 每秒性能统计窗口(行业标准 3D 指标) ---- */
    long     stat_window_ms;   /* 当前统计窗口起始时刻 */
    uint32_t stat_frames;      /* 窗口内帧数 */
    /* 三角面(提交绘制的三角形数) */
    uint64_t stat_tris_sum;
    uint32_t stat_tris_min, stat_tris_max;
    /* draw call 次数 */
    uint64_t stat_dc_sum;
    uint32_t stat_dc_min, stat_dc_max;
    /* 纹理绑定次数 */
    uint64_t stat_tex_sum;
    uint32_t stat_tex_min, stat_tex_max;
} vgl_impl_t;

/* ---------------- 工具 ---------------- */

/* vg_lite 错误码 → 可读名字，用于日志 */
static const char *vgl_err_str(vg_lite_error_t e)
{
    switch (e) {
        case VG_LITE_SUCCESS:             return "SUCCESS";
        case VG_LITE_INVALID_ARGUMENT:    return "INVALID_ARGUMENT";
        case VG_LITE_OUT_OF_MEMORY:       return "OUT_OF_MEMORY";
        case VG_LITE_NO_CONTEXT:          return "NO_CONTEXT";
        case VG_LITE_TIMEOUT:             return "TIMEOUT";
        case VG_LITE_OUT_OF_RESOURCES:    return "OUT_OF_RESOURCES";
        case VG_LITE_GENERIC_IO:          return "GENERIC_IO";
        case VG_LITE_NOT_SUPPORT:         return "NOT_SUPPORT";
        case VG_LITE_ALREADY_EXISTS:      return "ALREADY_EXISTS";
        case VG_LITE_NOT_ALIGNED:         return "NOT_ALIGNED";
        case VG_LITE_FLEXA_TIME_OUT:      return "FLEXA_TIME_OUT";
        case VG_LITE_FLEXA_HANDSHAKE_FAIL:return "FLEXA_HANDSHAKE_FAIL";
        default:                          return "UNKNOWN";
    }
}

/* r3d ARGB8888 → VGLite ABGR8888(VGLite color 内部为 0xAABBGGRR) */
static vg_lite_color_t argb_to_vgl(uint32_t argb)
{
    uint32_t a=(argb>>24)&0xFF, r=(argb>>16)&0xFF, g=(argb>>8)&0xFF, b=argb&0xFF;
    return (a<<24)|(b<<16)|(g<<8)|r;
}

/* 列主序 mat4 × vec4 */
static r3d_vec4_t mul_mv(const r3d_mat4_t *m, float x, float y, float z, float w)
{
    r3d_vec4_t o;
    o.x = m->m[0]*x + m->m[4]*y + m->m[8]*z  + m->m[12]*w;
    o.y = m->m[1]*x + m->m[5]*y + m->m[9]*z  + m->m[13]*w;
    o.z = m->m[2]*x + m->m[6]*y + m->m[10]*z + m->m[14]*w;
    o.w = m->m[3]*x + m->m[7]*y + m->m[11]*z + m->m[15]*w;
    return o;
}

/* r3d 像素格式 → VGLite buffer 格式 */
static vg_lite_buffer_format_t to_vgl_format(r3d_pixel_format_t f)
{
    switch (f) {
        case R3D_FMT_BGRA8888: return VG_LITE_BGRA8888;
        case R3D_FMT_RGB565:   return VG_LITE_BGR565;
        case R3D_FMT_ARGB8888:
        default:               return VG_LITE_ARGB8888;
    }
}

/* ---------------- 生命周期 ---------------- */

static r3d_result_t vgl_init(r3d_backend_t *self, const r3d_backend_cfg_t *cfg)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    uint32_t cap = (cfg && cfg->max_triangles) ? cfg->max_triangles : VGL_MAX_TRIS_DEFAULT;

    /* GPU 初始化由外部(gpu_init)统一负责，本后端不自持 GPU。
     *
     * 原因(对照 rive_for_vglite 与 vendor/bes gpu_port.c)：
     *  - 真机上把 GPU 跑起来需要 gpu_hw_reset() + gpu_memory_setup() +
     *    vg_lite_init(480,480) + set_gpu_done_interrupt_handler() 一整套，
     *    单独调 vg_lite_init 既不够(缺中断 handler，finish 必超时)，
     *    tessellation buffer 尺寸也无从正确设定(目标尺寸要到 begin_frame 才知道)。
     *  - 因此真机路径只能由 gpu_init 拥有 GPU(r3d_engine.c 已用 static-bool 守卫调用)，
     *    backend 不再调用 vg_lite_init/vg_lite_close，避免重复初始化或埋下
     *    16x16 tess buffer 这类隐患。
     *  - host/softsim 测试构建中 vg_lite 由软件桩实现，同样无需本后端初始化。
     * 这与 rive 一致：rive 渲染器也从不自己 vg_lite_init，只调 gpu_init。 */

    im->tri_cap = cap;
    im->tris = (vgl_tri_t *)malloc(sizeof(vgl_tri_t) * cap);
    if (!im->tris) { return R3D_ERR_NO_MEM; }
    im->tri_count = 0;

    /* 每三角形一个 vg_lite_path_t + 一段 11 float 的 path 数据(存活到帧末 finish) */
    im->vgpaths_cap = cap;
    im->vgpaths = (vg_lite_path_t *)malloc(sizeof(vg_lite_path_t) * cap);
    if (!im->vgpaths) {
        free(im->tris); im->tris = NULL;
        return R3D_ERR_NO_MEM;
    }

    /* 常驻 path 数据缓冲：每三角形 11 个 float(MOVE/LINE/LINE/CLOSE/END)。
     * vg_lite_draw 异步提交，GPU 直到 finish 才读取，故 path 数据必须存活整帧、
     * 不能放栈上。对齐 rive：数据随主命令缓冲提交，无需 vg_lite_upload_path。 */
    im->path_data = (float *)malloc(sizeof(float) * 11 * cap);
    if (!im->path_data) {
        free(im->vgpaths); im->vgpaths = NULL;
        free(im->tris);    im->tris = NULL;
        return R3D_ERR_NO_MEM;
    }

    /* 性能统计窗口初始化 */
    im->stat_window_ms = vgl_now_ms();
    im->stat_tris_min = im->stat_dc_min = im->stat_tex_min = 0xFFFFFFFFu;

    return R3D_OK;
}

static void vgl_destroy(r3d_backend_t *self)
{
    if (!self) return;
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (im) {
        free(im->tris);
        free(im->vgpaths);
        free(im->path_data);
        /* GPU 由 gpu_init 拥有，本后端不调用 vg_lite_close。 */
        free(im);
    }
    free(self);
}

/* ---------------- 纹理 ---------------- */

static r3d_texture_handle_t vgl_create_texture(r3d_backend_t *self, const r3d_image_t *img)
{
    (void)self;
    if (!img || !img->data || !img->w || !img->h) return R3D_TEXTURE_NONE;

    vgl_tex_t *t = (vgl_tex_t *)calloc(1, sizeof(vgl_tex_t));
    if (!t) return R3D_TEXTURE_NONE;

    /* 纹理内存用系统堆(memalign)分配，而非 vg_lite_allocate 的 GPU 私有堆。
     * 对照 rive_for_vglite 的 allocBufferMalloc / LVGL 的 lv_vg_lite_buffer_init：
     * 本 SoC 是统一寻址(flat-map)，GPU 可直接 DMA 访问系统内存，故无需占用
     * 容量有限、易 OOM 的 GPU 私有堆(256x256 纹理在私有堆常分配失败)。
     * 关键约束：
     *   - stride 必须 64 字节对齐(VGLite 硬件要求，同 LV_VG_LITE_BUF_ALIGN)
     *   - 起始地址 64 字节对齐(用 memalign)
     *   - address 直接用虚拟地址(统一寻址下 == GPU 可用地址)，handle=NULL */
    t->buf.width            = (vg_lite_int32_t)img->w;
    t->buf.height           = (vg_lite_int32_t)img->h;
    t->buf.format           = VG_LITE_BGRA8888;   /* 纹理像素在 b3dm 中按 BGRA 字节序存储(见 gltf2b3dm)，与 framebuffer 同序，避免 R/B 反 */
    t->buf.tiled            = VG_LITE_LINEAR;
    t->buf.image_mode       = VG_LITE_NORMAL_IMAGE_MODE;
    t->buf.transparency_mode= VG_LITE_IMAGE_OPAQUE;
    t->buf.stride           = ((vg_lite_int32_t)img->w * 4 + 63) & ~63;  /* 64 对齐 */
    t->buf.handle           = NULL;

    size_t size = (size_t)t->buf.stride * img->h;
    /* 多分配一行 stride 作为安全余量，防止 GPU 写越界(对照 rive) */
    void *mem = memalign(64, size + t->buf.stride);
    if (!mem) {
        VGL_ERR("create_texture: memalign(%dx%d, %zu bytes) failed",
                (int)img->w, (int)img->h, size + t->buf.stride);
        free(t);
        return R3D_TEXTURE_NONE;
    }
    t->buf.memory  = mem;
    t->buf.address = (vg_lite_uint32_t)(uintptr_t)mem;

    /* 拷贝像素(按行，处理 stride 差异) */
    uint32_t src_stride = img->stride ? img->stride : img->w * 4;
    uint8_t *dst = (uint8_t *)t->buf.memory;
    const uint8_t *src = (const uint8_t *)img->data;
    for (uint32_t y = 0; y < img->h; y++)
        memcpy(dst + (size_t)y * t->buf.stride, src + (size_t)y * src_stride, img->w * 4);

    t->valid = 1;
    return (r3d_texture_handle_t)t;
}

static void vgl_destroy_texture(r3d_backend_t *self, r3d_texture_handle_t h)
{
    (void)self;
    vgl_tex_t *t = (vgl_tex_t *)h;
    if (t && t->valid) {
        /* memalign 分配，用 free 释放(非 vg_lite_free) */
        if (t->buf.memory) free(t->buf.memory);
        free(t);
    }
}

/* ---------------- 帧 ---------------- */

static void vgl_begin_frame(r3d_backend_t *self, const r3d_target_t *target)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->tri_count = 0;
    im->draw_calls = 0;
    im->tex_binds = 0;

    if (!target) return;

    /* 包装宿主提供的目标内存为 vg_lite_buffer（字段对照 rive initRenderTargetBuffer）*/
    memset(&im->target, 0, sizeof(im->target));
    im->target.width  = (vg_lite_int32_t)target->w;
    im->target.height = (vg_lite_int32_t)target->h;
    im->target.stride = (vg_lite_int32_t)(target->stride ? target->stride : target->w * 4);
    im->target.format = to_vgl_format(target->format);
    im->target.tiled  = VG_LITE_LINEAR;
    im->target.memory = target->pixels;
    /* GPU 是独立硬件，DMA 访问 framebuffer 必须用物理地址。
     * 对齐 rive_for_vglite(initRenderTargetBuffer)：address 用 pinfo.fbmem 物理地址，
     * 而非 mmap 虚拟地址。本 SoC 上 fb 的虚拟地址≠物理地址时，若把 mmap 虚拟地址
     * 当 GPU 地址下发，GPU 会写到错误地址、命令跑完却不产生 END event，
     * 表现为 IDLE 但 INTR_STATUS=0、finish 超时挂死。
     * 故优先用 phys_addr；仅当其为 0(软件/宿主等价)时回退到虚拟地址。 */
    im->target.address = target->phys_addr
                         ? (vg_lite_uint32_t)target->phys_addr
                         : (vg_lite_uint32_t)(uintptr_t)target->pixels;
    im->target.handle = NULL;   /* 外部 framebuffer：无 vg_lite 分配句柄 */
    im->target.image_mode    = VG_LITE_NORMAL_IMAGE_MODE;
    im->target.transparency  = VG_LITE_IMAGE_OPAQUE;
    im->target.compress_mode = VG_LITE_DEC_DISABLE;
    im->target.fc_enable     = 0;
    im->target.premultiplied = 0;
    im->target_valid  = (target->pixels != NULL);
    im->vp_w = (int)target->w;
    im->vp_h = (int)target->h;

    /* 清屏(深色背景)。clear 命令与后续 draw 同批，帧末 end_frame 统一 finish。 */
    if (im->target_valid) {
        vg_lite_color_t bg = argb_to_vgl(0xFF1F1F26u);
        vg_lite_error_t e = vg_lite_clear(&im->target, NULL, bg);
        if (e != VG_LITE_SUCCESS)
            VGL_ERR("vg_lite_clear ret=%d(%s)", (int)e, vgl_err_str(e));
    }
}

static void vgl_set_camera(r3d_backend_t *self, const r3d_camera_t *cam)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->view = cam->view;
    im->proj = cam->proj;
    r3d_mat4_mul(&im->view_proj, &im->proj, &im->view);  /* VP = P * V */
}

/* ---------------- 绘制(收集三角形) ---------------- */

static void vgl_draw(r3d_backend_t *self, const r3d_mesh_t *mesh,
                     const r3d_mat4_t *model, const r3d_material_t *mat)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (!mesh || !mesh->vertices || !mesh->indices) return;

    r3d_mat4_t mvp;
    r3d_mat4_mul(&mvp, &im->view_proj, model);  /* MVP = VP * M */
    r3d_mat4_t mv;
    r3d_mat4_mul(&mv, &im->view, model);         /* MV = V * M(法线变到 view 空间) */

    int use_matcap   = (mat->flags & R3D_MAT_USE_MATCAP) ? 1 : 0;
    int double_sided = (mat->flags & R3D_MAT_DOUBLE_SIDED) ? 1 : 0;
    int translucent  = (mat->flags & R3D_MAT_TRANSLUCENT) ? 1 : 0;
    vgl_tex_t *tex   = use_matcap ? (vgl_tex_t *)mat->matcap
                                  : (vgl_tex_t *)mat->base_color;
    uint32_t base_argb = mat->base_color_factor ? mat->base_color_factor : 0xFFFFFFFFu;

    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t i0 = mesh->indices[i], i1 = mesh->indices[i+1], i2 = mesh->indices[i+2];
        /* 防御：索引越界(损坏/不匹配的模型，如 vertex_count 远小于 index 引用)
         * 会导致越界读垃圾内存当顶点坐标，进而 NaN/超大值喂给 GPU、看门狗复位。
         * 越界的三角形直接跳过。 */
        if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count ||
            i2 >= mesh->vertex_count)
            continue;
        const r3d_vertex_t *v0 = &mesh->vertices[i0];
        const r3d_vertex_t *v1 = &mesh->vertices[i1];
        const r3d_vertex_t *v2 = &mesh->vertices[i2];
        const r3d_vertex_t *vv[3] = { v0, v1, v2 };

        /* 投影到裁剪空间 */
        r3d_vec4_t c[3];
        int behind = 0;
        for (int k = 0; k < 3; k++) {
            c[k] = mul_mv(&mvp, vv[k]->pos.x, vv[k]->pos.y, vv[k]->pos.z, 1.0f);
            if (c[k].w <= VGL_W_EPSILON) behind = 1;
        }
        if (behind) { continue; }  /* 简化：整三角形任一顶点在近平面前/相机后则丢弃。
                                * 注：无近平面裁剪，靠较保守的 w 阈值 + 屏幕坐标
                                * 范围检查共同防止超大坐标喂给 GPU tessellation。 */
        /* view 空间线性深度(用于画家算法排序，比 NDC z 更稳健) */
        float vz[3];
        for (int k = 0; k < 3; k++) {
            r3d_vec4_t p = mul_mv(&mv, vv[k]->pos.x, vv[k]->pos.y, vv[k]->pos.z, 1.0f);
            vz[k] = p.z;  /* 相机看 -z：越负越远 */
        }

        /* 透视除法 → NDC → 屏幕 */
        float sx[3], sy[3];
        int bad = 0;
        for (int k = 0; k < 3; k++) {
            float inv = 1.0f / c[k].w;
            float ndc_x = c[k].x * inv;
            float ndc_y = c[k].y * inv;
            sx[k] = (ndc_x * 0.5f + 0.5f) * im->vp_w;
            sy[k] = (1.0f - (ndc_y * 0.5f + 0.5f)) * im->vp_h; /* Y 翻转 */
            /* 防御：NaN/Inf(坏顶点数据)或超大坐标会让 tessellation 跑飞 hang */
            if (!isfinite(sx[k]) || !isfinite(sy[k]) ||
                fabsf(sx[k]) > VGL_COORD_LIMIT || fabsf(sy[k]) > VGL_COORD_LIMIT) {
                bad = 1;
                break;
            }
        }
        if (bad) { continue; }

        /* 背面剔除(屏幕空间有向面积)；双面材质跳过 */
        float area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sx[2]-sx[0])*(sy[1]-sy[0]);
        if (!double_sided && area >= 0.0f) { continue; } /* 顺时针为正面(Y已翻转) */
        if (fabsf(area) < 0.01f) { continue; }            /* 退化三角形 */

        if (im->tri_count >= im->tri_cap) { break; }
        vgl_tri_t *T = &im->tris[im->tri_count];
        T->seq = im->tri_count;   /* 稳定排序序号 */
        im->tri_count++;

        for (int k = 0; k < 3; k++) {
            T->sx[k] = sx[k];
            T->sy[k] = sy[k];
            if (use_matcap) {
                /* matcap：view 空间法线 xy → UV(无 shader，CPU 算) */
                r3d_vec4_t nv = mul_mv(&im->view, vv[k]->normal.x, vv[k]->normal.y, vv[k]->normal.z, 0.0f);
                float nx = nv.x, ny = nv.y;
                float l = sqrtf(nx*nx + ny*ny + nv.z*nv.z);
                if (l > 1e-6f) { nx /= l; ny /= l; }
                T->uv[k][0] = nx * 0.5f + 0.5f;
                T->uv[k][1] = -ny * 0.5f + 0.5f;
            } else {
                T->uv[k][0] = vv[k]->uv.x;
                T->uv[k][1] = vv[k]->uv.y;
            }
        }
        T->depth = (vz[0] + vz[1] + vz[2]) * (1.0f/3.0f);  /* view 空间线性深度，越负越远 */
        T->tex = (tex && tex->valid) ? &tex->buf : NULL;

        /* CPU 光照。对齐 OpenGL：view 空间法线，光方向 view 空间 (0.3,0.5,0.8)。
           - 普通材质：完整 flat 光照 lit=(0.50+0.32*d+0.18*hemi)*ao。
           - matcap 材质：matcap 球已提供主反光，这里只叠加柔和的 AO + hemi 调制，
             给低模表壳的不同朝向面增加明暗层次(缓解"分面平涂"观感)，
             但不做强漫反射(否则压暗金属反光)。 */
        uint32_t draw_argb = base_argb;
        {
            float nx=0, ny=0, nz=0, ao=0;
            for (int k = 0; k < 3; k++) {
                r3d_vec4_t n = mul_mv(&mv, vv[k]->normal.x, vv[k]->normal.y, vv[k]->normal.z, 0.0f);
                nx += n.x; ny += n.y; nz += n.z; ao += vv[k]->ao;
            }
            float l = sqrtf(nx*nx + ny*ny + nz*nz);
            if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
            ao *= (1.0f/3.0f);
            float lx=0.3f, ly=0.5f, lz=0.8f;
            float ll = sqrtf(lx*lx+ly*ly+lz*lz); lx/=ll; ly/=ll; lz/=ll;
            float d = nx*lx + ny*ly + nz*lz; if (d < 0) d = 0;
            float hemi = 0.5f + 0.5f*ny;
            float lit;
            if (use_matcap)
                lit = (0.78f + 0.10f*d + 0.12f*hemi) * (0.6f + 0.4f*ao); /* 柔和：保金属亮度 */
            else
                lit = (0.50f + 0.32f*d + 0.18f*hemi) * ao;
            if (lit > 1.0f) lit = 1.0f;
            uint32_t a=(base_argb>>24)&0xFF, r=(base_argb>>16)&0xFF,
                     g=(base_argb>>8)&0xFF, b=base_argb&0xFF;
            r=(uint32_t)(r*lit); g=(uint32_t)(g*lit); b=(uint32_t)(b*lit);
            draw_argb = (a<<24)|(r<<16)|(g<<8)|b;
        }
        T->color = argb_to_vgl(draw_argb);
        T->translucent = translucent;
        T->blend = mat->blend;
    }
}

/* ---------------- 排序 + flush ---------------- */

/* 画家算法：远的先画。depth=view空间z，越负越远。半透明排在所有不透明之后。 */
static int tri_cmp(const void *a, const void *b)
{
    const vgl_tri_t *ta = (const vgl_tri_t *)a, *tb = (const vgl_tri_t *)b;
    if (ta->translucent != tb->translucent)
        return ta->translucent - tb->translucent;       /* 不透明(0)在前 */
    if (ta->depth < tb->depth) return -1;                /* 更负=更远，先画 */
    if (ta->depth > tb->depth) return 1;
    /* 深度相等：用收集顺序做稳定 tie-breaker。qsort 非稳定排序，相等深度的
     * 三角形相对次序帧间会变，导致前后覆盖关系跳变 → 画面闪烁。固定为 seq 序。 */
    if (ta->seq < tb->seq) return -1;
    if (ta->seq > tb->seq) return 1;
    return 0;
}

/* 解纹理 UV → 屏幕的 2x3 仿射矩阵：
 * 已知 3 个对应点 (u,v)->(sx,sy)，求 M 使得 [sx;sy] = M*[u;v;1]。
 * pattern_matrix 把 pattern 图像坐标(像素)映射到屏幕。VGLite pattern_matrix
 * 作用：屏幕点 → (经逆矩阵) → 采样 pattern 像素。这里我们给的是
 * pattern(纹理像素)→屏幕 的正向矩阵，驱动内部会取逆。 */
static int solve_affine(const vgl_tri_t *T, int tex_w, int tex_h, vg_lite_matrix_t *m)
{
    /* 纹理像素坐标 */
    float px[3], py[3];
    for (int k = 0; k < 3; k++) {
        px[k] = T->uv[k][0] * tex_w;
        py[k] = T->uv[k][1] * tex_h;
    }
    /* 求仿射: sx = a*px + b*py + c ; sy = d*px + e*py + f
     * 解 2 个 3x3 线性方程组(共享系数矩阵 [px py 1]) */
    float det = px[0]*(py[1]-py[2]) - py[0]*(px[1]-px[2]) + (px[1]*py[2]-px[2]*py[1]);
    if (fabsf(det) < 1e-6f) return 0;
    float inv = 1.0f / det;

    /* 系数矩阵 A=[px py 1] 的逆 × 屏幕坐标 */
    float i00 = (py[1]-py[2]) * inv;
    float i01 = (py[2]-py[0]) * inv;
    float i02 = (py[0]-py[1]) * inv;
    float i10 = (px[2]-px[1]) * inv;
    float i11 = (px[0]-px[2]) * inv;
    float i12 = (px[1]-px[0]) * inv;
    float i20 = (px[1]*py[2]-px[2]*py[1]) * inv;
    float i21 = (px[2]*py[0]-px[0]*py[2]) * inv;
    float i22 = (px[0]*py[1]-px[1]*py[0]) * inv;

    float a = i00*T->sx[0] + i01*T->sx[1] + i02*T->sx[2];
    float b = i10*T->sx[0] + i11*T->sx[1] + i12*T->sx[2];
    float c = i20*T->sx[0] + i21*T->sx[1] + i22*T->sx[2];
    float d = i00*T->sy[0] + i01*T->sy[1] + i02*T->sy[2];
    float e = i10*T->sy[0] + i11*T->sy[1] + i12*T->sy[2];
    float f = i20*T->sy[0] + i21*T->sy[1] + i22*T->sy[2];

    /* vg_lite_matrix m[row][col]，仿射(第三行 0 0 1) */
    m->m[0][0] = a; m->m[0][1] = b; m->m[0][2] = c;
    m->m[1][0] = d; m->m[1][1] = e; m->m[1][2] = f;
    m->m[2][0] = 0; m->m[2][1] = 0; m->m[2][2] = 1.0f;
    return 1;
}

static vg_lite_blend_t to_vgl_blend(int blend, int translucent)
{
    if (translucent) return VG_LITE_BLEND_SRC_OVER;
    switch (blend) {
        case R3D_BLEND_ADDITIVE: return VG_LITE_BLEND_ADDITIVE;
        case R3D_BLEND_MULTIPLY: return VG_LITE_BLEND_MULTIPLY;
        case R3D_BLEND_SCREEN:   return VG_LITE_BLEND_SCREEN;
        default:                 return VG_LITE_BLEND_NONE;
    }
}

/* 每秒一次输出标准 3D 性能统计：FPS + 三角面/draw call/纹理绑定的
 * 平均、最小、最大。把本帧计数并入窗口，跨过 1 秒边界时汇总并重置。 */
static void vgl_stats_tick(vgl_impl_t *im, uint32_t tris, uint32_t dc, uint32_t tex)
{
    im->stat_frames++;
    im->stat_tris_sum += tris;
    im->stat_dc_sum   += dc;
    im->stat_tex_sum  += tex;
    if (tris < im->stat_tris_min) im->stat_tris_min = tris;
    if (tris > im->stat_tris_max) im->stat_tris_max = tris;
    if (dc   < im->stat_dc_min)   im->stat_dc_min   = dc;
    if (dc   > im->stat_dc_max)   im->stat_dc_max   = dc;
    if (tex  < im->stat_tex_min)  im->stat_tex_min  = tex;
    if (tex  > im->stat_tex_max)  im->stat_tex_max  = tex;

    long now = vgl_now_ms();
    long dt  = now - im->stat_window_ms;
    if (dt < 1000 || im->stat_frames == 0) return;

    float fps = im->stat_frames * 1000.0f / (float)dt;
    VGL_LOG("perf: fps=%.1f frames=%u | tri/f avg=%u min=%u max=%u | "
            "drawcall/f avg=%u min=%u max=%u | tex/f avg=%u min=%u max=%u",
            fps, (unsigned)im->stat_frames,
            (unsigned)(im->stat_tris_sum / im->stat_frames),
            (unsigned)im->stat_tris_min, (unsigned)im->stat_tris_max,
            (unsigned)(im->stat_dc_sum / im->stat_frames),
            (unsigned)im->stat_dc_min, (unsigned)im->stat_dc_max,
            (unsigned)(im->stat_tex_sum / im->stat_frames),
            (unsigned)im->stat_tex_min, (unsigned)im->stat_tex_max);

    /* 重置窗口 */
    im->stat_window_ms = now;
    im->stat_frames = 0;
    im->stat_tris_sum = im->stat_dc_sum = im->stat_tex_sum = 0;
    im->stat_tris_min = im->stat_dc_min = im->stat_tex_min = 0xFFFFFFFFu;
    im->stat_tris_max = im->stat_dc_max = im->stat_tex_max = 0;
}

static void vgl_end_frame(r3d_backend_t *self)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    uint32_t submit_tris = im->tri_count;  /* 本帧提交绘制的三角面数 */

    if (!im->target_valid || im->tri_count == 0) {
        /* 即使没有三角形，begin_frame 里的 vg_lite_clear 命令仍在命令缓冲中，
         * 必须 finish 让其落地，否则随后翻页会显示一块未清屏的缓冲。 */
        if (im->target_valid) {
            vg_lite_error_t fe = vg_lite_finish();
            if (fe != VG_LITE_SUCCESS)
                VGL_ERR("end_frame(empty) vg_lite_finish ret=%d(%s)",
                        (int)fe, vgl_err_str(fe));
        }
        vgl_stats_tick(im, 0, 0, 0);
        im->frame_no++;
        return;
    }

    /* 画家算法排序：远→近，半透明最后。 */
    qsort(im->tris, im->tri_count, sizeof(vgl_tri_t), tri_cmp);

    /* 逐三角形绘制(保持画家算法的逐面遮挡 + 每面 flat 光照的明暗层次，
     * 这是立体感的来源；不做"同色合批"——合批会把曲面上相邻、量化后同色的
     * 面合成一片纯色，丢失曲率的明暗渐变，看起来变平)。
     * 性能优化只保留"批量 flush"：不再每个 draw 都 flush，而是每
     * VGL_FLUSH_BATCH 次 flush 一次 + 帧末统一 finish。 */
    int pending_draws = 0;

    for (uint32_t i = 0; i < im->tri_count; i++) {
        vgl_tri_t *T = &im->tris[i];

        vg_lite_path_t *pp = (i < im->vgpaths_cap) ? &im->vgpaths[i] : NULL;
        if (!pp) break;

        /* 三角形 path 顶点数据(FP32)：MOVE/LINE/LINE/CLOSE/END = 11 个 4 字节 slot。
         * opcode slot 按 uint32 整数位模式写、坐标 slot 按 IEEE754 float 写。 */
        float *pdata = &im->path_data[(size_t)i * 11];
        uint32_t *pop = (uint32_t *)pdata;
        pop[0] = (uint32_t)VLC_OP_MOVE;  pdata[1] = T->sx[0]; pdata[2] = T->sy[0];
        pop[3] = (uint32_t)VLC_OP_LINE;  pdata[4] = T->sx[1]; pdata[5] = T->sy[1];
        pop[6] = (uint32_t)VLC_OP_LINE;  pdata[7] = T->sx[2]; pdata[8] = T->sy[2];
        pop[9]  = (uint32_t)VLC_OP_CLOSE;
        pop[10] = (uint32_t)VLC_OP_END;

        float minx = T->sx[0], maxx = T->sx[0], miny = T->sy[0], maxy = T->sy[0];
        for (int k = 1; k < 3; k++) {
            if (T->sx[k] < minx) minx = T->sx[k];
            if (T->sx[k] > maxx) maxx = T->sx[k];
            if (T->sy[k] < miny) miny = T->sy[k];
            if (T->sy[k] > maxy) maxy = T->sy[k];
        }

        memset(pp, 0, sizeof(*pp));
        pp->format       = VG_LITE_FP32;
        /* 纹理(pattern)三角形用 LOW(关 AA)：GPU 抗锯齿产生分数边缘覆盖率，
         * 相邻三角形共享边各贡献部分覆盖，经 src-over 混合后不足 100%，缝隙
         * 像素透出底色形成可见黑线(对照 rive drawImageMeshPatternFill 的结论)。
         * 硬边(100%/0%)保证相邻三角形无缝拼接，仅外轮廓略锯齿。
         * 纯色三角形无此问题，保留 HIGH 以获得更平滑边缘/立体感。 */
        pp->quality      = T->tex ? VG_LITE_LOW : VGL_FILL_QUALITY;
        pp->path         = pdata;
        pp->path_length  = (vg_lite_uint32_t)(11 * sizeof(uint32_t));
        pp->path_changed = 1;
        pp->bounding_box[0] = minx;
        pp->bounding_box[1] = miny;
        pp->bounding_box[2] = maxx;
        pp->bounding_box[3] = maxy;

        vg_lite_matrix_t path_mat;
        vg_lite_identity(&path_mat);

        vg_lite_blend_t blend = to_vgl_blend(T->blend, T->translucent);

        vg_lite_error_t derr;
        if (T->tex) {
            vg_lite_matrix_t pat_mat;
            if (solve_affine(T, T->tex->width, T->tex->height, &pat_mat)) {
                derr = vg_lite_draw_pattern(&im->target, pp, VGL_FILL_RULE,
                             &path_mat, T->tex, &pat_mat, blend,
                             VG_LITE_PATTERN_PAD, 0, T->color, VG_LITE_FILTER_BI_LINEAR);
                im->tex_binds++;
            } else {
                derr = vg_lite_draw(&im->target, pp, VGL_FILL_RULE,
                             &path_mat, blend, T->color);
            }
        } else {
            derr = vg_lite_draw(&im->target, pp, VGL_FILL_RULE,
                         &path_mat, blend, T->color);
        }
        if (derr != VG_LITE_SUCCESS) {
            VGL_ERR("tri[%u] %s ret=%d(%s), abort frame", (unsigned)i,
                    T->tex ? "vg_lite_draw_pattern" : "vg_lite_draw",
                    (int)derr, vgl_err_str(derr));
            break;
        }
        im->draw_calls++;

        /* 批量 flush：每 VGL_FLUSH_BATCH 次 draw flush 一次(不再每个 draw 都 flush)。 */
        if (++pending_draws >= VGL_FLUSH_BATCH) {
            vg_lite_flush();
            pending_draws = 0;
        }
    }

    vg_lite_error_t fe = vg_lite_finish();
    if (fe != VG_LITE_SUCCESS)
        VGL_ERR("end_frame vg_lite_finish ret=%d(%s)", (int)fe, vgl_err_str(fe));

    /* path 数据写在常驻 im->path_data，下一帧直接覆写复用，destroy 时统一释放。 */
    vgl_stats_tick(im, submit_tris, im->draw_calls, im->tex_binds);
    im->frame_no++;
}

static void vgl_present(r3d_backend_t *self)
{
    (void)self;
    /* 宿主模式：目标即宿主缓冲，flush 已在 end_frame 完成。 */
    vg_lite_error_t fe = vg_lite_finish();
    if (fe != VG_LITE_SUCCESS)
        VGL_ERR("present vg_lite_finish ret=%d(%s)", (int)fe, vgl_err_str(fe));
}

static bool vgl_query_feature(r3d_backend_t *self, r3d_feature_t f)
{
    (void)self;
    switch (f) {
        case R3D_FEATURE_PERSPECTIVE_TEXTURE: return false; /* 仿射近似(CPU 投影) */
        case R3D_FEATURE_ZBUFFER:             return false; /* 画家算法 */
        case R3D_FEATURE_PER_PIXEL_LIGHT:     return false; /* 烘焙光照 */
        case R3D_FEATURE_BLEND_MULTIPLY:      return true;
        default:                              return false;
    }
}

static const r3d_backend_vtable_t VGL_VT = {
    .init            = vgl_init,
    .destroy         = vgl_destroy,
    .create_texture  = vgl_create_texture,
    .destroy_texture = vgl_destroy_texture,
    .begin_frame     = vgl_begin_frame,
    .set_camera      = vgl_set_camera,
    .draw            = vgl_draw,
    .end_frame       = vgl_end_frame,
    .present         = vgl_present,
    .query_feature   = vgl_query_feature,
};

r3d_backend_t *r3d_backend_vglite_create(void)
{
    r3d_backend_t *be = (r3d_backend_t *)calloc(1, sizeof(r3d_backend_t));
    if (!be) return NULL;
    be->impl = calloc(1, sizeof(vgl_impl_t));
    if (!be->impl) { free(be); return NULL; }
    be->vt = &VGL_VT;
    return be;
}

/* 宿主模式：vg_lite 已由外部(如 NuttX gpu_init / LVGL)初始化，
 * 本后端不再调用 vg_lite_init / vg_lite_close。 */
r3d_backend_t *r3d_backend_vglite_create_hosted(void)
{
    r3d_backend_t *be = r3d_backend_vglite_create();
    if (!be) return NULL;
    ((vgl_impl_t *)be->impl)->hosted = 1;
    return be;
}
