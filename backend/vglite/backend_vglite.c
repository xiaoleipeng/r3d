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
#include <string.h>
#include <math.h>
#include <time.h>

/* 流程日志：真机定位 GPU hang 在哪个 vg_lite 调用。
 * 用 syslog；本机(softsim)无 syslog 时退化为 printf。 */
#if defined(__NuttX__) || defined(CONFIG_ARCH)
#include <syslog.h>
#define VGL_TRACE(fmt, ...) syslog(LOG_INFO, "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define VGL_TRACE(fmt, ...) fprintf(stderr, "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#endif

/* 返回当前单调毫秒，用于给 vg_lite 调用计时 */
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
 * 对照 LVGL(lv_draw_vg_lite_triangle.c)：LVGL 在这块完全相同的 GPU 上画三角形
 * 用 VG_LITE_FILL_EVEN_ODD 并能正常上屏。NON_ZERO 与 EVEN_ODD 对单个简单三角形
 * 视觉结果一致，但在 tessellation 内走不同的填充/扫描分支。诊断 GPU finish 超时
 * 时，把填充规则对齐 LVGL 作为单变量实验。 */
#define VGL_FILL_RULE  VG_LITE_FILL_EVEN_ODD

/* path 细分质量。对齐 LVGL(lv_vg_lite_path_create 用 VG_LITE_HIGH)。
 * 三角形只有直线段，quality 仅影响曲线，这里与 LVGL 保持一致。 */
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

    uint32_t draw_calls;
    uint32_t frame_no;     /* 帧计数，用于限制 trace 日志量 */
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

/* 打印 vg_lite 调用返回值。ret!=SUCCESS 一律打印(WARNING)；
 * SUCCESS 仅在前几帧 trace 时打印，避免稳态刷屏。 */
#define VGL_LOG_RET(call, ret)                                            \
    do {                                                                  \
        vg_lite_error_t _r = (ret);                                       \
        if (_r != VG_LITE_SUCCESS)                                        \
            VGL_TRACE("%s ret=%d(%s)", (call), (int)_r, vgl_err_str(_r)); \
        else if (trace)                                                   \
            VGL_TRACE("%s ret=0(SUCCESS)", (call));                       \
    } while (0)

/* GPU 关键寄存器地址(对照 VGLiteKernel/vg_lite_hw.h)。
 * 用公开 API vg_lite_get_register 读取，不耦合驱动内部 hal。 */
#define VGL_REG_HW_IDLE       0x004   /* 全空闲态 = 0x0B05 */
#define VGL_REG_INTR_STATUS   0x010   /* 完成/错误中断状态 */
#define VGL_REG_INTR_ENABLE   0x014   /* 中断使能位 */
#define VGL_HW_IDLE_STATE     0x0B05

/* 打印 GPU 状态寄存器，用于诊断 finish 超时(done 中断未到达) hang。
 * tag 标识打印时机(如 "pre-finish"/"post-finish")。 */
static void vgl_dump_gpu_state(const char *tag)
{
    vg_lite_uint32_t idle = 0, istat = 0, ien = 0;
    vg_lite_error_t e_idle = vg_lite_get_register(VGL_REG_HW_IDLE, &idle);
    vg_lite_error_t e_stat = vg_lite_get_register(VGL_REG_INTR_STATUS, &istat);
    vg_lite_error_t e_en   = vg_lite_get_register(VGL_REG_INTR_ENABLE, &ien);

    if (e_idle != VG_LITE_SUCCESS || e_stat != VG_LITE_SUCCESS || e_en != VG_LITE_SUCCESS) {
        VGL_TRACE("gpu_state[%s]: get_register failed (idle=%d stat=%d en=%d)",
                  tag, (int)e_idle, (int)e_stat, (int)e_en);
        return;
    }

    VGL_TRACE("gpu_state[%s]: IDLE=0x%08lx(%s) INTR_STATUS=0x%08lx INTR_ENABLE=0x%08lx%s",
              tag,
              (unsigned long)idle,
              (idle == VGL_HW_IDLE_STATE) ? "all-idle"
                  : (idle & 0x80000000u) ? "AXI-BUS-ERR" : "busy/partial",
              (unsigned long)istat,
              (unsigned long)ien,
              (ien == 0) ? " [!! INTR NOT ENABLED]" : "");
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

    /* 初始化后立即 dump 一次 GPU 状态：确认(gpu_init 之后)中断是否已使能。
     * 若 INTR_ENABLE=0，则 GPU 干完活也无法通知 CPU，finish 必然超时。 */
    vgl_dump_gpu_state("post-init");
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

    t->buf.width  = (vg_lite_int32_t)img->w;
    t->buf.height = (vg_lite_int32_t)img->h;
    t->buf.format = VG_LITE_ARGB8888;   /* 与 r3d ARGB8888 一致 */
    t->buf.tiled  = VG_LITE_LINEAR;

    vg_lite_error_t ae = vg_lite_allocate(&t->buf);
    if (ae != VG_LITE_SUCCESS) {
        VGL_TRACE("create_texture: vg_lite_allocate(%dx%d) ret=%d(%s)",
                  (int)img->w, (int)img->h, (int)ae, vgl_err_str(ae));
        free(t);
        return R3D_TEXTURE_NONE;
    }
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
        vg_lite_free(&t->buf);
        free(t);
    }
}

/* ---------------- 帧 ---------------- */

static void vgl_begin_frame(r3d_backend_t *self, const r3d_target_t *target)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    int trace = (im->frame_no < 3);
    im->tri_count = 0;
    im->draw_calls = 0;

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

    if (im->frame_no < 3) {
        VGL_TRACE("begin_frame #%u: TARGET buffer fields:", (unsigned)im->frame_no);
        VGL_TRACE("  .width=%d .height=%d .stride=%d",
                  (int)im->target.width, (int)im->target.height, (int)im->target.stride);
        VGL_TRACE("  .format=%d .tiled=%d", (int)im->target.format, (int)im->target.tiled);
        VGL_TRACE("  .memory=%p .address=0x%lx .handle=%p",
                  im->target.memory, (unsigned long)im->target.address, im->target.handle);
        VGL_TRACE("  src pixels=%p phys_addr=0x%lx (address uses %s)",
                  target->pixels, (unsigned long)target->phys_addr,
                  target->phys_addr ? "phys_addr" : "virt(pixels)");
        VGL_TRACE("  .image_mode=%d .transparency=%d .compress_mode=%d .fc_enable=%d .premultiplied=%d",
                  (int)im->target.image_mode, (int)im->target.transparency,
                  (int)im->target.compress_mode, (int)im->target.fc_enable,
                  (int)im->target.premultiplied);
        VGL_TRACE("  valid=%d vp=%dx%d", im->target_valid, im->vp_w, im->vp_h);
    }

    /* 清屏(深色背景)。clear 命令与后续 draw 同批，帧末 end_frame 统一 finish。 */
    if (im->target_valid) {
        vg_lite_color_t bg = argb_to_vgl(0xFF1F1F26u);
        if (im->frame_no < 3)
            VGL_TRACE("  vg_lite_clear(target=%p rect=NULL(full) color=0x%08lx)",
                      (void *)&im->target, (unsigned long)bg);
        vg_lite_error_t e = vg_lite_clear(&im->target, NULL, bg);
        VGL_LOG_RET("  vg_lite_clear", e);
    }
}

static void vgl_set_camera(r3d_backend_t *self, const r3d_camera_t *cam)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->view = cam->view;
    im->proj = cam->proj;
    r3d_mat4_mul(&im->view_proj, &im->proj, &im->view);  /* VP = P * V */

    /* 诊断：前 3 帧 dump 三个矩阵(各 16 float)。set_camera 在 end_frame
     * frame_no++ 之前执行，故 gate 用 im->frame_no < 3。 */
    if (im->frame_no < 3) {
        VGL_TRACE("set_camera #%u: view (col-major 16f):", (unsigned)im->frame_no);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view.m[0], im->view.m[4], im->view.m[8],  im->view.m[12]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view.m[1], im->view.m[5], im->view.m[9],  im->view.m[13]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view.m[2], im->view.m[6], im->view.m[10], im->view.m[14]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view.m[3], im->view.m[7], im->view.m[11], im->view.m[15]);
        VGL_TRACE("set_camera #%u: proj (col-major 16f):", (unsigned)im->frame_no);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->proj.m[0], im->proj.m[4], im->proj.m[8],  im->proj.m[12]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->proj.m[1], im->proj.m[5], im->proj.m[9],  im->proj.m[13]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->proj.m[2], im->proj.m[6], im->proj.m[10], im->proj.m[14]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->proj.m[3], im->proj.m[7], im->proj.m[11], im->proj.m[15]);
        VGL_TRACE("set_camera #%u: view_proj (col-major 16f):", (unsigned)im->frame_no);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view_proj.m[0], im->view_proj.m[4], im->view_proj.m[8],  im->view_proj.m[12]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view_proj.m[1], im->view_proj.m[5], im->view_proj.m[9],  im->view_proj.m[13]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view_proj.m[2], im->view_proj.m[6], im->view_proj.m[10], im->view_proj.m[14]);
        VGL_TRACE("  [%.4f %.4f %.4f %.4f]", im->view_proj.m[3], im->view_proj.m[7], im->view_proj.m[11], im->view_proj.m[15]);
    }
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

    /* 诊断计数(frame0 末尾汇总)：collected=进入队列, skip_* 为各丢弃原因 */
    int frame0 = (im->frame_no == 0);
    uint32_t dbg_logged = 0;          /* 已打印数据的三角形个数(限前 5) */
    uint32_t cnt_total = 0, cnt_collected = 0;
    uint32_t cnt_behind = 0, cnt_bad = 0, cnt_backface = 0, cnt_degenerate = 0, cnt_capped = 0;

    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        cnt_total++;
        const r3d_vertex_t *v0 = &mesh->vertices[mesh->indices[i]];
        const r3d_vertex_t *v1 = &mesh->vertices[mesh->indices[i+1]];
        const r3d_vertex_t *v2 = &mesh->vertices[mesh->indices[i+2]];
        const r3d_vertex_t *vv[3] = { v0, v1, v2 };

        /* 投影到裁剪空间 */
        r3d_vec4_t c[3];
        int behind = 0;
        for (int k = 0; k < 3; k++) {
            c[k] = mul_mv(&mvp, vv[k]->pos.x, vv[k]->pos.y, vv[k]->pos.z, 1.0f);
            if (c[k].w <= VGL_W_EPSILON) behind = 1;
        }
        if (behind) { cnt_behind++; continue; }  /* 简化：整三角形任一顶点在近平面前/相机后则丢弃。
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
        if (bad) { cnt_bad++; continue; }

        /* 背面剔除(屏幕空间有向面积)；双面材质跳过 */
        float area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sx[2]-sx[0])*(sy[1]-sy[0]);
        if (!double_sided && area >= 0.0f) { cnt_backface++; continue; } /* 顺时针为正面(Y已翻转) */
        if (fabsf(area) < 0.01f) { cnt_degenerate++; continue; }          /* 退化三角形 */

        if (im->tri_count >= im->tri_cap) { cnt_capped++; break; }
        vgl_tri_t *T = &im->tris[im->tri_count++];
        cnt_collected++;

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

        /* 诊断：frame0 打印前 5 个被收集三角形的数据(将被转成 path 的源数据)。 */
        if (frame0 && dbg_logged < 5) {
            VGL_TRACE("draw collect tri[%u]: screen pts (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f)",
                      (unsigned)dbg_logged,
                      T->sx[0], T->sy[0], T->sx[1], T->sy[1], T->sx[2], T->sy[2]);
            VGL_TRACE("  uv (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f) depth=%.4f",
                      T->uv[0][0], T->uv[0][1], T->uv[1][0], T->uv[1][1],
                      T->uv[2][0], T->uv[2][1], T->depth);
            VGL_TRACE("  color=0x%08lx tex=%p translucent=%d blend=%d",
                      (unsigned long)T->color, (void *)T->tex,
                      T->translucent, T->blend);
            dbg_logged++;
        }
    }

    /* 诊断：frame0 汇总收集/丢弃统计，定位"喂给 GPU 的三角形总量"。 */
    if (frame0) {
        VGL_TRACE("draw summary frame0: total=%u collected=%u (queue now %u/%u) | "
                  "skip behind=%u bad=%u backface=%u degenerate=%u capped=%u",
                  (unsigned)cnt_total, (unsigned)cnt_collected,
                  (unsigned)im->tri_count, (unsigned)im->tri_cap,
                  (unsigned)cnt_behind, (unsigned)cnt_bad, (unsigned)cnt_backface,
                  (unsigned)cnt_degenerate, (unsigned)cnt_capped);
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

static void vgl_end_frame(r3d_backend_t *self)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    int trace = (im->frame_no < 3);
    /* 诊断：前几帧无条件详细日志(每个三角形)，定位 finish 超时由哪个 vg_lite
     * 调用/数据触发。frame0 对每个三角形都打印；为防刷屏，frame0 仅前 8 个 +
     * 之后每 32 个打印一次。 */
    int frame0 = (im->frame_no == 0);

    if (!im->target_valid || im->tri_count == 0) {
        if (trace) VGL_TRACE("end_frame #%u: nothing to draw (valid=%d tris=%u)",
                             (unsigned)im->frame_no, im->target_valid,
                             (unsigned)im->tri_count);
        /* 即使没有三角形，begin_frame 里的 vg_lite_clear 命令仍在命令缓冲中，
         * 必须 finish 让其落地，否则随后翻页会显示一块未清屏(残影/花屏)的缓冲。
         * 仅 target 有效时才有待落地的 clear。 */
        if (im->target_valid) {
            vg_lite_error_t fe = vg_lite_finish();
            VGL_LOG_RET("end_frame(empty) vg_lite_finish", fe);
        }
        im->frame_no++;
        return;
    }

    if (trace) VGL_TRACE("end_frame #%u: %u tris, sort + draw...",
                         (unsigned)im->frame_no, (unsigned)im->tri_count);

    /* 画家算法排序 */
    qsort(im->tris, im->tri_count, sizeof(vgl_tri_t), tri_cmp);

    for (uint32_t i = 0; i < im->tri_count; i++) {
        vgl_tri_t *T = &im->tris[i];

        /* 诊断：frame0 对每个三角形打印；为防刷屏，仅前 8 个 + 之后每 32 个。
         * 仍保留原 trace(前 3 帧)对 tri[0] 的打印行为，二者取并。 */
        int log_tri = frame0 && (i < 8 || (i % 32) == 0);

        vg_lite_path_t *pp = (i < im->vgpaths_cap) ? &im->vgpaths[i] : NULL;
        if (!pp) break;

        /* 三角形 path 顶点数据(FP32)：MOVE/LINE/LINE/CLOSE/END = 11 个 4 字节 slot。
         * 写入常驻缓冲 im->path_data 的第 i 段(非栈)：vg_lite_draw 异步提交，
         * GPU 直到 finish 才读取，故数据必须存活整帧。
         *
         * 关键(对照 rive VGLitePath::appendOpCode / appendFloat)：
         * 即便 path 格式是 VG_LITE_FP32，GPU 命令解析器(FE)对每个 4 字节 slot
         * 的解释是——opcode slot 按【uint32 整数位模式】读，坐标 slot 按【IEEE754
         * float】读。rive 正是这样：opcode 用 (uint32_t)2 写(字节 02 00 00 00)，
         * 坐标用 float 写。
         * 之前 r3d 误把 opcode 写成 (float)2.0f(位模式 0x40000000)，FE 读到非法
         * opcode 流，在浮点坐标下触发 tessellation 握手卡死(整数坐标偶然避开死区，
         * 故 roundf 能跑但不是真正修复)。现改为与 rive 完全一致：opcode=uint32。
         *
         * 坐标保持浮点(VG_LITE_FP32 本就支持，rive 也用浮点坐标)。 */
        float *pdata = &im->path_data[(size_t)i * 11];
        uint32_t *pop = (uint32_t *)pdata;  /* 同一缓冲的 uint32 视图，用于写 opcode */
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

        /* 手动填充 vg_lite_path_t —— 镜像 rive(VGLitePath::finalize)：不调用
         * vg_lite_init_path(它会强制 path_type=FILL_PATH 并做 CLOSE→END 改写)，
         * 只设必要字段，path_type 保持 0(VG_LITE_DRAW_ZERO)。
         * bounding_box 与 rive 一致：直接用顶点 min/max 浮点值，不取整、不 +1。 */
        memset(pp, 0, sizeof(*pp));
        pp->format       = VG_LITE_FP32;
        pp->quality      = VGL_FILL_QUALITY;
        pp->path         = pdata;
        pp->path_length  = (vg_lite_uint32_t)(11 * sizeof(uint32_t));
        pp->path_changed = 1;
        pp->bounding_box[0] = minx;
        pp->bounding_box[1] = miny;
        pp->bounding_box[2] = maxx;
        pp->bounding_box[3] = maxy;

        vg_lite_matrix_t path_mat;
        vg_lite_identity(&path_mat);  /* 屏幕坐标已是最终位置 */

        vg_lite_blend_t blend = to_vgl_blend(T->blend, T->translucent);

        if ((trace && i == 0) || log_tri) {
            VGL_TRACE("  tri[%u] init_path: fmt=FP32(%d) quality=%d len=%d pdata=%p bbox=[%.1f,%.1f,%.1f,%.1f]",
                      (unsigned)i, (int)VG_LITE_FP32, (int)VGL_FILL_QUALITY,
                      (int)(11 * sizeof(uint32_t)), (void *)pdata,
                      minx, miny, maxx, maxy);
            VGL_TRACE("  tri[%u] pdata: MOVE(%.3f,%.3f) LINE(%.3f,%.3f) LINE(%.3f,%.3f) CLOSE(op=%u) END(op=%u)",
                      (unsigned)i, pdata[1], pdata[2], pdata[4], pdata[5],
                      pdata[7], pdata[8], (unsigned)pop[9], (unsigned)pop[10]);
            VGL_TRACE("  tri[%u] bbox minx=%.3f miny=%.3f maxx=%.3f maxy=%.3f",
                      (unsigned)i, minx, miny, maxx, maxy);
            VGL_TRACE("  tri[%u] path.format=%d .quality=%d .path_length=%d .path=%p .path_changed=%d .path_type=%d",
                      (unsigned)i, (int)pp->format, (int)pp->quality, (int)pp->path_length,
                      pp->path, (int)pp->path_changed, (int)pp->path_type);
            VGL_TRACE("  tri[%u] path_mat (3x3 row-major):", (unsigned)i);
            VGL_TRACE("    [%.4f %.4f %.4f]", path_mat.m[0][0], path_mat.m[0][1], path_mat.m[0][2]);
            VGL_TRACE("    [%.4f %.4f %.4f]", path_mat.m[1][0], path_mat.m[1][1], path_mat.m[1][2]);
            VGL_TRACE("    [%.4f %.4f %.4f]", path_mat.m[2][0], path_mat.m[2][1], path_mat.m[2][2]);
            VGL_TRACE("  tri[%u] blend=%d fill=EVEN_ODD(%d) color=0x%08lx tex=%p",
                      (unsigned)i, (int)blend, (int)VGL_FILL_RULE,
                      (unsigned long)T->color, (void *)T->tex);
        }

        vg_lite_error_t derr = VG_LITE_SUCCESS;
        if (T->tex) {
            vg_lite_matrix_t pat_mat;
            if (solve_affine(T, T->tex->width, T->tex->height, &pat_mat)) {
                if ((trace && i == 0) || log_tri) {
                    VGL_TRACE("  tri[%u] tex w=%d h=%d pat_mat (3x3 row-major):",
                              (unsigned)i, (int)T->tex->width, (int)T->tex->height);
                    VGL_TRACE("    [%.4f %.4f %.4f]", pat_mat.m[0][0], pat_mat.m[0][1], pat_mat.m[0][2]);
                    VGL_TRACE("    [%.4f %.4f %.4f]", pat_mat.m[1][0], pat_mat.m[1][1], pat_mat.m[1][2]);
                    VGL_TRACE("    [%.4f %.4f %.4f]", pat_mat.m[2][0], pat_mat.m[2][1], pat_mat.m[2][2]);
                    VGL_TRACE("  tri[%u] CALL vg_lite_draw_pattern(tgt=%p path=%p fill=NON_ZERO(%d) mat=%p tex=%p patmat=%p blend=%d pad=PAD(%d) color=0x%08lx filter=BILINEAR)",
                              (unsigned)i, (void *)&im->target, (void *)pp, (int)VG_LITE_FILL_NON_ZERO,
                              (void *)&path_mat, (void *)T->tex, (void *)&pat_mat,
                              (int)blend, (int)VG_LITE_PATTERN_PAD,
                              (unsigned long)T->color);
                }
                derr = vg_lite_draw_pattern(&im->target, pp, VGL_FILL_RULE,
                                     &path_mat, T->tex, &pat_mat,
                                     blend, VG_LITE_PATTERN_PAD,
                                     0, T->color, VG_LITE_FILTER_BI_LINEAR);
            } else {
                if ((trace && i == 0) || log_tri)
                    VGL_TRACE("  tri[%u] solve_affine failed -> fallback CALL vg_lite_draw(tgt=%p path=%p fill=NON_ZERO(%d) mat=%p blend=%d color=0x%08lx)",
                              (unsigned)i, (void *)&im->target, (void *)pp, (int)VG_LITE_FILL_NON_ZERO,
                              (void *)&path_mat, (int)blend, (unsigned long)T->color);
                derr = vg_lite_draw(&im->target, pp, VGL_FILL_RULE,
                             &path_mat, blend, T->color);
            }
        } else {
            /* 纯色三角形 */
            if ((trace && i == 0) || log_tri)
                VGL_TRACE("  tri[%u] CALL vg_lite_draw(tgt=%p path=%p fill=NON_ZERO(%d) mat=%p blend=%d color=0x%08lx)",
                          (unsigned)i, (void *)&im->target, (void *)pp, (int)VG_LITE_FILL_NON_ZERO,
                          (void *)&path_mat, (int)blend, (unsigned long)T->color);
            derr = vg_lite_draw(&im->target, pp, VGL_FILL_RULE,
                         &path_mat, blend, T->color);
        }
        /* draw 失败(命令缓冲/资源异常)：继续提交后续 draw 只会把错误放大，
         * 中止本帧的绘制循环，走到末尾 finish 把已入队命令安全落地。 */
        VGL_LOG_RET(T->tex ? "  vg_lite_draw_pattern" : "  vg_lite_draw", derr);
        if (log_tri)
            VGL_TRACE("  tri[%u] %s ret=%d(%s)", (unsigned)i,
                      T->tex ? "vg_lite_draw_pattern" : "vg_lite_draw",
                      (int)derr, vgl_err_str(derr));
        if (derr != VG_LITE_SUCCESS) {
            VGL_TRACE("  tri[%u] draw failed ret=%d(%s), abort frame draw loop",
                      (unsigned)i, (int)derr, vgl_err_str(derr));
            break;
        }
        /* path 描述符存活在 im->vgpaths[i]，uploaded GPU 内存帧末统一释放。 */
        im->draw_calls++;

        /* 每个 draw 之后 flush —— 对齐 rive flushIfNeeded(FLUSH_MAX_COUNT=0 时
         * 每个 draw 都无条件 flush)。
         *
         * 关键(对照 rive 已验证序列)：rive 的 endFrame 之所以 finish 3ms 正常返回，
         * 是因为它在每个 drawPath 后 vg_lite_flush 把命令缓冲正式提交、开新缓冲，
         * 到 finish 时 GPU 正在跑(pre-finish 抓到 IDLE=0x7ffffefa busy)，
         * finish arm 等待 → 完成中断到达 → 返回。
         *
         * r3d 旧实现用 (draw_calls & 31)==0 做周期 flush：单三角形 draw_calls=1，
         * 1&31=1≠0，flush 从不触发，命令缓冲未经 flush 提交就直接 finish。
         * 现象正是 pre-finish 时 GPU 已空闲(IDLE=0x7fffffff)、FE 卡在 TS↔PE
         * semaphore 握手、完成中断不触发、finish 超时 1.6s。
         * 故改为与 rive 一致：每个 draw 后 flush(异步提交，不阻塞)。 */
        long t0 = vgl_now_ms();
        vg_lite_error_t fe = vg_lite_flush();
        long t1 = vgl_now_ms();
        VGL_LOG_RET("  per-draw vg_lite_flush", fe);
        if (trace) VGL_TRACE("  flush at draw %u took %ld ms", (unsigned)i, t1 - t0);
    }

    if (trace) VGL_TRACE("end_frame #%u: final vg_lite_finish...", (unsigned)im->frame_no);
    /* 诊断：finish 前汇总本帧三角形/draw 调用数并 dump GPU 状态(前 3 帧无条件)。 */
    if (im->frame_no < 3) {
        VGL_TRACE("end_frame #%u: pre-finish tri_count=%u draw_calls=%u",
                  (unsigned)im->frame_no, (unsigned)im->tri_count, (unsigned)im->draw_calls);
        vgl_dump_gpu_state("pre-finish");
    }
    long tf0 = vgl_now_ms();
    vg_lite_error_t fe = vg_lite_finish();
    long tf1 = vgl_now_ms();
    VGL_LOG_RET("end_frame vg_lite_finish", fe);
    /* finish 失败(超时/IO)：dump GPU 状态以区分"中断未使能"vs"中断未被接住"。
     * 成功时仅前几帧 dump。finish 耗时异常(>500ms,接近 1.5s 看门狗)也 dump。 */
    if (fe != VG_LITE_SUCCESS || im->frame_no < 3 || (tf1 - tf0) > 500)
        vgl_dump_gpu_state("post-finish");
    if (trace) VGL_TRACE("end_frame #%u: done (draw_calls=%u, finish took %ld ms)",
                         (unsigned)im->frame_no, (unsigned)im->draw_calls, tf1 - tf0);

    /* path 数据写在常驻 im->path_data，未做 GPU upload，无需逐三角形释放；
     * 下一帧直接覆写复用，缓冲在 vgl_destroy 统一释放。 */
    im->frame_no++;
}

static void vgl_present(r3d_backend_t *self)
{
    (void)self;
    /* 宿主模式：目标即宿主缓冲，flush 已在 end_frame 完成。 */
    vg_lite_error_t fe = vg_lite_finish();
    if (fe != VG_LITE_SUCCESS)
        VGL_TRACE("present vg_lite_finish ret=%d(%s)", (int)fe, vgl_err_str(fe));
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
