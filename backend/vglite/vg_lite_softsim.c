/*
 * vg_lite_softsim.c — VGLite API 的 CPU 软件实现(仅本机验证用，非硬件)
 *
 * 目的：在没有 best1600 硬件/cmodel 的本机，用纯 CPU 软件光栅化实现
 * backend_vglite.c 用到的 VGLite API 子集，使后端代码端到端跑起来出图，
 * 从而验证我的投影/仿射纹理映射/画家算法/matcap UV 逻辑是否正确。
 *
 * 注意：这不验证 VGLite 硬件光栅化器本身，只验证 r3d VGLite 后端的调用逻辑。
 * 真机/cmodel 上换成厂商 libvg_lite 即可，后端代码不变。
 *
 * 实现的 API：init/close/allocate/free/clear/identity/init_path/clear_path/
 *            finish/draw/draw_pattern
 * 光栅化：扫描线三角形填充；draw_pattern 用 pattern_matrix 逆映射采样纹理(双线性)。
 */

#include "vg_lite.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 当前 target(clear/draw 写入) */
/* 软件实现是无状态的，target 由各 API 入参带入 */

vg_lite_error_t vg_lite_init(vg_lite_uint32_t tw, vg_lite_uint32_t th)
{ (void)tw; (void)th; return VG_LITE_SUCCESS; }

vg_lite_error_t vg_lite_close(void) { return VG_LITE_SUCCESS; }

vg_lite_error_t vg_lite_finish(void) { return VG_LITE_SUCCESS; }
vg_lite_error_t vg_lite_flush(void)  { return VG_LITE_SUCCESS; }

/* softsim：无真实硬件寄存器，返回固定的"全空闲 + 中断已使能"值，
 * 使后端的诊断打印路径在本机也能编译/运行。 */
vg_lite_error_t vg_lite_get_register(vg_lite_uint32_t address, vg_lite_uint32_t *result)
{
    if (!result) return VG_LITE_INVALID_ARGUMENT;
    switch (address) {
        case 0x04: *result = 0x0B05;     break; /* HW_IDLE: 全空闲态 */
        case 0x10: *result = 0x00000000; break; /* INTR_STATUS */
        case 0x14: *result = 0xFFFFFFFF; break; /* INTR_ENABLE */
        default:   *result = 0x00000000; break;
    }
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_allocate(vg_lite_buffer_t *buf)
{
    if (!buf || buf->width <= 0 || buf->height <= 0) return VG_LITE_INVALID_ARGUMENT;
    if (buf->stride <= 0) buf->stride = buf->width * 4;  /* ARGB8888 */
    size_t sz = (size_t)buf->stride * buf->height;
    buf->memory = malloc(sz);
    if (!buf->memory) return VG_LITE_OUT_OF_MEMORY;
    buf->handle = buf->memory;
    memset(buf->memory, 0, sz);
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_free(vg_lite_buffer_t *buf)
{
    if (buf && buf->memory) { free(buf->memory); buf->memory = NULL; buf->handle = NULL; }
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_identity(vg_lite_matrix_t *m)
{
    if (!m) return VG_LITE_INVALID_ARGUMENT;
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = 1.0f;
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_clear(vg_lite_buffer_t *target, vg_lite_rectangle_t *rect, vg_lite_color_t color)
{
    (void)rect;
    if (!target || !target->memory) return VG_LITE_INVALID_ARGUMENT;
    /* VGLite color 内部序 0xAABBGGRR；目标 ARGB8888 内存按 r3d 约定存 0xAARRGGBB */
    uint32_t a=(color>>24)&0xFF, b=(color>>16)&0xFF, g=(color>>8)&0xFF, r=color&0xFF;
    uint32_t argb = (a<<24)|(r<<16)|(g<<8)|b;
    for (int y = 0; y < target->height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)target->memory + (size_t)y * target->stride);
        for (int x = 0; x < target->width; x++) row[x] = argb;
    }
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_init_path(vg_lite_path_t *path, vg_lite_format_t fmt,
                                  vg_lite_quality_t q, vg_lite_uint32_t len,
                                  vg_lite_pointer data, vg_lite_float_t minx,
                                  vg_lite_float_t miny, vg_lite_float_t maxx, vg_lite_float_t maxy)
{
    if (!path) return VG_LITE_INVALID_ARGUMENT;
    memset(path, 0, sizeof(*path));
    path->format = fmt;
    path->quality = q;
    path->path_length = len;
    path->path = data;
    path->bounding_box[0] = minx; path->bounding_box[1] = miny;
    path->bounding_box[2] = maxx; path->bounding_box[3] = maxy;
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_clear_path(vg_lite_path_t *path)
{ (void)path; return VG_LITE_SUCCESS; }

/* softsim：path 数据保持指向调用方提供的内存(本机无独立 GPU 内存)。
 * 真机由厂商库实现 vg_lite_allocate + 拷贝。这里只做最小校验，
 * 使后端的返回值检查路径(上传失败→跳过)在本机也能被走到。 */
vg_lite_error_t vg_lite_upload_path(vg_lite_path_t *path)
{
    if (!path || !path->path || path->path_length == 0)
        return VG_LITE_INVALID_ARGUMENT;
    path->path_changed = 0;
    return VG_LITE_SUCCESS;
}

/* ---- 软件光栅化辅助 ---- */

/* 从 FP32 path 数据提取三角形 3 顶点(MOVE,LINE,LINE,CLOSE,END) */
/* VG_LITE_FP32 path layout: each word is 4 bytes; opcode words hold a raw
 * uint32 (NOT a float-encoded value), coordinate words hold float. This
 * matches how backend_vglite.c emits paths, and how LVGL's production
 * vg_lite backend does it (lv_vg_lite_path_append_op: FP32/S32 -> uint32
 * opcode, points -> float). Reading the opcode as float yields a denormal
 * that casts to 0, which silently drops every triangle. */
static int extract_tri(const vg_lite_path_t *path, float vx[3], float vy[3])
{
    const uint32_t *pop = (const uint32_t *)path->path;
    const float *pdata = (const float *)path->path;
    int n = 0, i = 0, words = (int)(path->path_length / sizeof(uint32_t));
    while (i < words && n < 3) {
        uint32_t op = pop[i];
        if (op == VLC_OP_MOVE || op == VLC_OP_LINE) {
            if (i + 2 >= words) break;
            vx[n] = pdata[i+1]; vy[n] = pdata[i+2]; n++; i += 3;
        } else if (op == VLC_OP_CLOSE) { i += 1; }
        else if (op == VLC_OP_END) break;
        else i += 1;
    }
    return n == 3;
}

/* alpha over 混合：src/dst 均 0xAARRGGBB */
static uint32_t blend_over(uint32_t src, uint32_t dst)
{
    uint32_t sa = (src>>24)&0xFF;
    if (sa == 255) return src;
    if (sa == 0)   return dst;
    uint32_t sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
    uint32_t dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
    uint32_t r=(sr*sa + dr*(255-sa))/255;
    uint32_t g=(sg*sa + dg*(255-sa))/255;
    uint32_t b=(sb*sa + db*(255-sa))/255;
    return (255u<<24)|(r<<16)|(g<<8)|b;
}

/* VGLite color(0xAABBGGRR) → ARGB(0xAARRGGBB) */
static uint32_t vgl_to_argb(vg_lite_color_t c)
{
    uint32_t a=(c>>24)&0xFF, b=(c>>16)&0xFF, g=(c>>8)&0xFF, r=c&0xFF;
    return (a<<24)|(r<<16)|(g<<8)|b;
}

/* 双线性采样纹理(ARGB8888)，返回 0xAARRGGBB */
static uint32_t sample_tex(const vg_lite_buffer_t *tex, float fx, float fy)
{
    if (fx < 0) fx = 0; if (fx > tex->width-1)  fx = tex->width-1;
    if (fy < 0) fy = 0; if (fy > tex->height-1) fy = tex->height-1;
    int x0=(int)fx, y0=(int)fy, x1=x0+1, y1=y0+1;
    if (x1 >= tex->width)  x1 = tex->width-1;
    if (y1 >= tex->height) y1 = tex->height-1;
    float tx=fx-x0, ty=fy-y0;
    const uint32_t *m = (const uint32_t *)tex->memory;
    int st = tex->stride/4;
    uint32_t c00=m[y0*st+x0], c10=m[y0*st+x1], c01=m[y1*st+x0], c11=m[y1*st+x1];
    float out[4];
    for (int ch=0; ch<4; ch++) {
        float a=( (c00>>(ch*8))&0xFF ), b=( (c10>>(ch*8))&0xFF );
        float c=( (c01>>(ch*8))&0xFF ), d=( (c11>>(ch*8))&0xFF );
        float top=a+(b-a)*tx, bot=c+(d-c)*tx;
        out[ch]=top+(bot-top)*ty;
    }
    return ((uint32_t)(out[3]+0.5f)<<24)|((uint32_t)(out[2]+0.5f)<<16)
         | ((uint32_t)(out[1]+0.5f)<<8)|(uint32_t)(out[0]+0.5f);
}

/* color 调制(逐通道乘) */
static uint32_t modulate(uint32_t px, uint32_t tint)
{
    if (tint == 0xFFFFFFFFu) return px;
    uint32_t pa=(px>>24)&0xFF, pr=(px>>16)&0xFF, pg=(px>>8)&0xFF, pb=px&0xFF;
    uint32_t ta=(tint>>24)&0xFF, tr=(tint>>16)&0xFF, tg=(tint>>8)&0xFF, tb=tint&0xFF;
    return ((pa*ta/255)<<24)|((pr*tr/255)<<16)|((pg*tg/255)<<8)|(pb*tb/255);
}

/* 扫描线填充三角形，回调每像素采色 */
typedef uint32_t (*pixel_fn)(int x, int y, void *ctx);

static void raster_tri(vg_lite_buffer_t *t, float vx[3], float vy[3],
                       int do_blend, pixel_fn fn, void *ctx)
{
    float ymin=vy[0], ymax=vy[0], xmin=vx[0], xmax=vx[0];
    for (int k=1;k<3;k++){ if(vy[k]<ymin)ymin=vy[k]; if(vy[k]>ymax)ymax=vy[k];
                           if(vx[k]<xmin)xmin=vx[k]; if(vx[k]>xmax)xmax=vx[k]; }
    int y0=(int)floorf(ymin), y1=(int)ceilf(ymax);
    int x0=(int)floorf(xmin), x1=(int)ceilf(xmax);
    if (y0<0)y0=0; if (y1>t->height)y1=t->height;
    if (x0<0)x0=0; if (x1>t->width)x1=t->width;
    float d = (vy[1]-vy[2])*(vx[0]-vx[2]) + (vx[2]-vx[1])*(vy[0]-vy[2]);
    if (fabsf(d) < 1e-6f) return;
    float inv = 1.0f/d;
    for (int y=y0;y<y1;y++){
        uint32_t *row=(uint32_t*)((uint8_t*)t->memory+(size_t)y*t->stride);
        for (int x=x0;x<x1;x++){
            float px=x+0.5f, py=y+0.5f;
            float l0=((vy[1]-vy[2])*(px-vx[2])+(vx[2]-vx[1])*(py-vy[2]))*inv;
            float l1=((vy[2]-vy[0])*(px-vx[2])+(vx[0]-vx[2])*(py-vy[2]))*inv;
            float l2=1.0f-l0-l1;
            if (l0<-0.001f||l1<-0.001f||l2<-0.001f) continue;
            uint32_t src=fn(x,y,ctx);
            row[x]= do_blend ? blend_over(src,row[x]) : src;
        }
    }
}

/* draw: 纯色填充 */
typedef struct { uint32_t color; } solid_ctx_t;
static uint32_t solid_px(int x,int y,void*c){ (void)x;(void)y; return ((solid_ctx_t*)c)->color; }

vg_lite_error_t vg_lite_draw(vg_lite_buffer_t *target, vg_lite_path_t *path,
                             vg_lite_fill_t fill, vg_lite_matrix_t *mat,
                             vg_lite_blend_t blend, vg_lite_color_t color)
{
    (void)fill; (void)mat;
    if (!target||!target->memory||!path) return VG_LITE_INVALID_ARGUMENT;
    float vx[3], vy[3];
    if (!extract_tri(path, vx, vy)) return VG_LITE_SUCCESS;
    solid_ctx_t ctx = { vgl_to_argb(color) };
    raster_tri(target, vx, vy, blend != VG_LITE_BLEND_NONE, solid_px, &ctx);
    return VG_LITE_SUCCESS;
}

/* draw_pattern: 仿射纹理填充。pattern_matrix 把纹理像素→屏幕，逆映射采样。 */
typedef struct {
    const vg_lite_buffer_t *tex;
    float inv[6];      /* 屏幕→纹理像素 仿射 */
    uint32_t tint;
} pat_ctx_t;

static uint32_t pat_px(int x,int y,void*c)
{
    pat_ctx_t *p=(pat_ctx_t*)c;
    float sx=x+0.5f, sy=y+0.5f;
    float tu=p->inv[0]*sx+p->inv[1]*sy+p->inv[2];
    float tv=p->inv[3]*sx+p->inv[4]*sy+p->inv[5];
    return modulate(sample_tex(p->tex, tu, tv), p->tint);
}

vg_lite_error_t vg_lite_draw_pattern(vg_lite_buffer_t *target, vg_lite_path_t *path,
                                     vg_lite_fill_t fill, vg_lite_matrix_t *path_mat,
                                     vg_lite_buffer_t *pattern, vg_lite_matrix_t *pat_mat,
                                     vg_lite_blend_t blend, vg_lite_pattern_mode_t mode,
                                     vg_lite_color_t pat_color, vg_lite_color_t color,
                                     vg_lite_filter_t filter)
{
    (void)fill;(void)path_mat;(void)mode;(void)pat_color;(void)filter;
    if (!target||!target->memory||!path||!pattern||!pattern->memory) return VG_LITE_INVALID_ARGUMENT;
    float vx[3], vy[3];
    if (!extract_tri(path, vx, vy)) return VG_LITE_SUCCESS;

    /* pat_mat: 纹理像素→屏幕 (a b c / d e f)。求逆得屏幕→纹理 */
    float a=pat_mat->m[0][0], b=pat_mat->m[0][1], c=pat_mat->m[0][2];
    float d=pat_mat->m[1][0], e=pat_mat->m[1][1], f=pat_mat->m[1][2];
    float det=a*e-b*d;
    if (fabsf(det)<1e-9f) return VG_LITE_SUCCESS;
    float idet=1.0f/det;
    pat_ctx_t ctx;
    ctx.tex=pattern; ctx.tint=vgl_to_argb(color);
    ctx.inv[0]= e*idet; ctx.inv[1]=-b*idet; ctx.inv[2]=(b*f-c*e)*idet;
    ctx.inv[3]=-d*idet; ctx.inv[4]= a*idet; ctx.inv[5]=(c*d-a*f)*idet;
    raster_tri(target, vx, vy, blend != VG_LITE_BLEND_NONE, pat_px, &ctx);
    return VG_LITE_SUCCESS;
}
