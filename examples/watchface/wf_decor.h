/*
 * wf_decor.h — 表盘装饰层示例：背景星点、大气光晕、明暗遮罩、装饰弧、文字
 *
 * 这些都不是 3D 能力，所以不放进 r3d 引擎。r3d 只负责 mesh 绘制，本文件通过
 * 引擎的后绘制钩子(r3d_engine_set_post_geometry_hook)在 3D 几何完成后插入
 * 自己的 2D 绘制：星点、明暗遮罩、光晕、装饰弧和文字。
 *
 * POST 阶段的顺序有讲究：遮罩是乘法压暗，必须最先贴，否则会把光晕一起压暗；
 * 光晕是 additive；文字最后画，才不被压暗也不被冲淡。
 *
 * 全部实现只用两类 VGLite 调用：
 *   vg_lite_blit  贴一张离线烘焙的 128x128 渐变(光晕、遮罩)
 *   vg_lite_draw  画一条多子路径(星点、弧线、文字)
 * 每项各 1 次 draw call，与场景多边形数无关。
 *
 * 头文件内联实现，与 wf_font.h 一致 —— 示例代码，便于单文件取用。
 */

#ifndef WF_DECOR_H
#define WF_DECOR_H

#include "vg_lite.h"
#include "wf_font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WF_TEX          128     /* 光晕/遮罩烘焙贴图边长 */
#define WF_SLOTS        4       /* 文字槽位数 */
#define WF_TEXTLEN      24
#define WF_PATH_FLOATS  4608    /* 约 354 个四边形。单线字体每字约 20 个 */
#define WF_STARS_MAX    160

/* 与后端一致：文字用 NON_ZERO。弧形排版下相邻字形朝圆心收敛可能相互重叠，
 * EVEN_ODD 会把重叠区异或成空洞，笔画角被啃掉看着像梯形。 */
#define WF_FILL_RULE    VG_LITE_FILL_NON_ZERO
#define WF_QUALITY      VG_LITE_HIGH

typedef struct {
    /* ---- 目标几何：星球在屏幕上的位置与半径，由调用方每帧更新 ---- */
    float    cx, cy, r;

    /* ---- 大气光晕(additive 环) ---- */
    int      halo_on;
    uint32_t halo_rgb;
    float    halo_strength;     /* 0 = 关 */
    float    halo_extent;       /* 外缘 / 星球半径 */
    float    halo_inward;       /* 向内渗透占半径比例 */
    float    halo_falloff;      /* 向外衰减指数 */

    /* ---- 明暗遮罩(逐像素昼夜，替代逐三角形 flat 光照) ---- */
    int      shade_on;
    float    shade_dir[3];      /* view 空间光照方向 */
    float    shade_ambient;
    float    shade_diffuse;
    float    shade_limb;        /* 临边压暗宽度占半径比例，0 = 关 */

    /* ---- 背景星点 ---- */
    int      stars_n;           /* 0 = 关 */
    uint32_t stars_seed;
    uint32_t stars_rgb;

    /* ---- 两侧装饰弧 ---- */
    int      arcs_on;
    float    arcs_r, arcs_span_deg, arcs_thick;
    uint32_t arcs_rgb;

    /* ---- 文字风格(见 wf_preset_space) ---- */
    int      font_style;
    uint32_t text_rgb;

    /* ---- 文字槽位 ---- */
    struct {
        char     text[WF_TEXTLEN];
        float    x0, y;         /* 直排：左边缘 / 顶部 */
        float    h, track;
        uint32_t rgb;
        int      font;          /* 0=7段 1=点阵 2=几何单线 */
        int      on;
        /* 弧形排版：arc_r > 0 时 x0/y 复用为弧心 */
        float    arc_r, arc_rad;
        int      arc_dir;
    } slot[WF_SLOTS];

    /* ---- 内部状态 ---- */
    vg_lite_buffer_t halo_buf, shade_buf;
    int      halo_ready, halo_dirty, shade_ready, shade_dirty;
    float   *path;
    float   *stars_path;
    int      logged;
} wf_decor_t;

/* ------------------------------------------------------------------ 初始化 */

static inline void wf_decor_init(wf_decor_t *d)
{
    memset(d, 0, sizeof(*d));
    d->halo_rgb      = 0x5B7099u;
    d->halo_extent   = 1.12f;
    d->halo_inward   = 0.08f;
    d->halo_falloff  = 1.5f;
    d->shade_dir[0]  = 0.90f;
    d->shade_dir[1]  = 0.22f;
    d->shade_dir[2]  = 0.38f;
    d->shade_ambient = 0.04f;
    d->shade_diffuse = 1.15f;
    d->shade_limb    = 0.06f;
    d->stars_seed    = 20260820u;
    d->stars_rgb     = 0xB2B2B2u;
    d->arcs_rgb      = 0xFFFFFFu;
    d->arcs_span_deg = 34.0f;
    d->arcs_thick    = 3.5f;
    d->halo_dirty = d->shade_dirty = 1;
}

static inline void wf_decor_deinit(wf_decor_t *d)
{
    if (d->halo_ready)  vg_lite_free(&d->halo_buf);
    if (d->shade_ready) vg_lite_free(&d->shade_buf);
    free(d->path);
    free(d->stars_path);
    memset(d, 0, sizeof(*d));
}

static inline int wf_alloc_tex(wf_decor_t *d, vg_lite_buffer_t *b, const char *what)
{
    memset(b, 0, sizeof(*b));
    b->width  = WF_TEX;
    b->height = WF_TEX;
    b->format = VG_LITE_BGRA8888;
    b->tiled  = VG_LITE_LINEAR;
    b->image_mode        = VG_LITE_NORMAL_IMAGE_MODE;
    b->transparency_mode = VG_LITE_IMAGE_OPAQUE;
    if (vg_lite_allocate(b) != VG_LITE_SUCCESS) {
        if (!d->logged) {
            fprintf(stderr, "wf_decor: %s allocate %dx%d failed\n", what, WF_TEX, WF_TEX);
            d->logged = 1;
        }
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- 光晕烘焙 */

/* 贴图半边长映射到 r*extent，故星球轮廓落在归一化半径 u = 1/extent 处，
 * profile 在该处取峰值 1，向内外两侧都衰减到 0：
 *   u >= edge : ((1-u)/(1-edge))^falloff        —— edge 处 1，外缘 0
 *   u <  edge : ((u-inner)/(edge-inner))^falloff —— inner 处 0，edge 处 1
 *               inner = edge*(1-inward)
 * 两支在 u=edge 处都等于 1，故连续。这点很关键：早先向内那支写成
 * (u/edge)^2*inward，在 edge 处只有 inward(0.3)，与外支的 1.0 形成 3.3 倍硬边；
 * 那道硬边是完美的圆，而星球轮廓是多边形且无抗锯齿(纹理三角形走 VG_LITE_LOW)，
 * 两条不重合的边界交错就在衔接处呈现明显锯齿。
 * 强度烘进 RGB 而非 alpha：ADDITIVE 是 "RGB: S + D"，不消费源 alpha。 */
static inline int wf_halo_bake(wf_decor_t *d)
{
    if (d->halo_ready && !d->halo_dirty) return 1;
    if (!d->halo_ready) {
        if (!wf_alloc_tex(d, &d->halo_buf, "halo")) { d->halo_on = 0; return 0; }
        d->halo_ready = 1;
    }
    float edge  = 1.0f / d->halo_extent;
    float inner = edge * (1.0f - d->halo_inward);
    uint32_t cr = (d->halo_rgb >> 16) & 0xFF;
    uint32_t cg = (d->halo_rgb >>  8) & 0xFF;
    uint32_t cb =  d->halo_rgb        & 0xFF;
    int half = WF_TEX / 2;

    for (int y = 0; y < WF_TEX; y++) {
        uint8_t *row = (uint8_t *)d->halo_buf.memory + (size_t)y * d->halo_buf.stride;
        for (int x = 0; x < WF_TEX; x++) {
            float dx = (x + 0.5f) - half, dy = (y + 0.5f) - half;
            float u = sqrtf(dx*dx + dy*dy) / (float)half;
            float p;
            if (u >= 1.0f)                        p = 0.0f;
            else if (u >= edge)                   p = powf((1.0f - u) / (1.0f - edge), d->halo_falloff);
            else if (u > inner && edge > inner)   p = powf((u - inner) / (edge - inner), d->halo_falloff);
            else                                  p = 0.0f;
            p *= d->halo_strength;
            if (p > 1.0f) p = 1.0f;
            row[x*4 + 0] = (uint8_t)(cb * p + 0.5f);   /* BGRA */
            row[x*4 + 1] = (uint8_t)(cg * p + 0.5f);
            row[x*4 + 2] = (uint8_t)(cr * p + 0.5f);
            row[x*4 + 3] = 0xFF;
        }
    }
    d->halo_dirty = 0;
    return 1;
}

/* --------------------------------------------------------------- 遮罩烘焙 */

/* 光照方向在 view 空间给出，故相机绕轨时屏幕上的明暗图案静止：屏幕点 (dx,dy)
 * 对应的球面法线只由 dx/R、dy/R 决定，是纯屏幕空间函数，可完整烘进贴图，
 * 于是逐像素、无三角形台阶。RGB=0、alpha=1-lit 配 SRC_OVER 得 dst = D*lit。
 * 星球外 alpha=0(透明) —— 遮罩是正方形贴图,只压暗星球盘面,星球外不碰背景,
 * 故不依赖清屏色(早先写成不透明黑,会在星球周围留一圈黑方框)。
 * dy 取负是因为后端把屏幕 y 翻转过(vc->sy 用 1-ndc)。 */
#define WF_SHADE_MARGIN 1.06f

static inline int wf_shade_bake(wf_decor_t *d)
{
    if (d->shade_ready && !d->shade_dirty) return 1;
    if (!d->shade_ready) {
        if (!wf_alloc_tex(d, &d->shade_buf, "shade")) { d->shade_on = 0; return 0; }
        d->shade_buf.transparency_mode = VG_LITE_IMAGE_TRANSPARENT;
        d->shade_ready = 1;
    }
    int half = WF_TEX / 2;
    float lx = d->shade_dir[0], ly = d->shade_dir[1], lz = d->shade_dir[2];
    for (int y = 0; y < WF_TEX; y++) {
        uint8_t *row = (uint8_t *)d->shade_buf.memory + (size_t)y * d->shade_buf.stride;
        for (int x = 0; x < WF_TEX; x++) {
            /* 乘 MARGIN 把纹理坐标归一到"星球半径 = 1" */
            float ux = ((x + 0.5f) - half) / (float)half * WF_SHADE_MARGIN;
            float uy = ((y + 0.5f) - half) / (float)half * WF_SHADE_MARGIN;
            float rr = ux*ux + uy*uy;
            uint32_t a;
            if (rr >= 1.0f) {
                a = 0;                      /* 星球外: 透明,不碰背景(不依赖清屏色) */
            } else {
                float nz = sqrtf(1.0f - rr);
                float dd = ux*lx + (-uy)*ly + nz*lz;
                if (dd < 0.0f) dd = 0.0f;
                float lit = d->shade_ambient + d->shade_diffuse * dd;
                if (lit < 0.0f) lit = 0.0f;
                if (lit > 1.0f) lit = 1.0f;
                /* 临边压暗：掠射光照下最亮点被压缩到轮廓上只占 1-3 像素，
                 * 实测最外像素亮度 222 而内部仅 23，读起来像"阴影没盖住"的硬亮
                 * 描边；而带纹理三角形为避免相邻三角形间黑缝已关掉 AA，轮廓是
                 * 硬边多边形，这条亮边又把锯齿放大。把最外 shade_limb 比例的
                 * 半径线性压到 0，既消掉亮描边又把硬轮廓藏进暗部。 */
                if (d->shade_limb > 0.0f) {
                    float u = sqrtf(rr);
                    float t0 = 1.0f - d->shade_limb;
                    if (u > t0) lit *= (1.0f - u) / d->shade_limb;
                }
                a = (uint32_t)((1.0f - lit) * 255.0f + 0.5f);
            }
            row[x*4 + 0] = 0;
            row[x*4 + 1] = 0;
            row[x*4 + 2] = 0;
            row[x*4 + 3] = (uint8_t)a;
        }
    }
    d->shade_dirty = 0;
    return 1;
}

/* ------------------------------------------------------------------ 贴图层 */

static inline void wf_blit_tex(vg_lite_buffer_t *target, vg_lite_buffer_t *src,
                               float cx, float cy, float diameter,
                               vg_lite_blend_t blend)
{
    float s = diameter / (float)WF_TEX;
    vg_lite_matrix_t m;
    vg_lite_identity(&m);
    m.m[0][0] = s;  m.m[1][1] = s;
    m.m[0][2] = cx - diameter * 0.5f;
    m.m[1][2] = cy - diameter * 0.5f;
    vg_lite_blit(target, src, &m, blend, 0, VG_LITE_FILTER_BI_LINEAR);
}

/* ------------------------------------------------------------ 路径类绘制 */

/* VGLite 的 vg_lite_color_t 是 ABGR，而配置里按惯例写 ARGB(0xRRGGBB)，
 * 必须换序。不换的话红蓝互换，纯白/纯灰看不出来，一上彩色就错。 */
static inline vg_lite_color_t wf_argb_to_vgl(uint32_t argb)
{
    uint32_t a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >>  8) & 0xFF, b =  argb        & 0xFF;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static inline int wf_draw_path(vg_lite_buffer_t *target, float *buf, uint32_t n,
                               int w, int h, uint32_t rgb, vg_lite_blend_t blend)
{
    if (n <= 1) return 0;
    vg_lite_path_t pp;
    memset(&pp, 0, sizeof(pp));
    pp.format      = VG_LITE_FP32;
    pp.quality     = WF_QUALITY;
    pp.path        = buf;
    pp.path_length = (int32_t)(n * sizeof(float));
    pp.bounding_box[0] = 0.0f;
    pp.bounding_box[1] = 0.0f;
    pp.bounding_box[2] = (float)w;
    pp.bounding_box[3] = (float)h;
    vg_lite_matrix_t m;
    vg_lite_identity(&m);
    return vg_lite_draw(target, &pp, WF_FILL_RULE, &m, blend,
                        wf_argb_to_vgl(0xFF000000u | (rgb & 0x00FFFFFFu)))
           == VG_LITE_SUCCESS;
}

/* 星点。位置用固定种子的 LCG 生成 → 每帧一致，星星不会闪。
 * 明暗不靠多次 draw 换颜色，而靠改方块边长：亚像素尺寸经 AA 后覆盖率不足
 * 自然偏暗，于是同色单批也有明暗层次，只花 1 次 draw call。 */
static inline void wf_draw_stars(wf_decor_t *d, vg_lite_buffer_t *target,
                                 int w, int h)
{
    if (d->stars_n <= 0) return;
    if (!d->stars_path) {
        d->stars_path = (float *)malloc(sizeof(float) * (WF_STARS_MAX * 13 + 1));
        if (!d->stars_path) { d->stars_n = 0; return; }
    }
    wf_pathw_t pw;
    wf_pw_init(&pw, d->stars_path, (uint32_t)(WF_STARS_MAX * 13 + 1));

    uint32_t st = d->stars_seed ? d->stars_seed : 1u;
    float fw = (float)w, fh = (float)h;
    float sc = fw * 0.5f, scy = fh * 0.5f;
    float rscr = (fw < fh ? fw : fh) * 0.5f;
    int n = d->stars_n > WF_STARS_MAX ? WF_STARS_MAX : d->stars_n;

    for (int i = 0; i < n; i++) {
        st = st * 1664525u + 1013904223u;
        float u = (float)((st >> 8) & 0xFFFFu) / 65535.0f;
        st = st * 1664525u + 1013904223u;
        float v = (float)((st >> 8) & 0xFFFFu) / 65535.0f;
        st = st * 1664525u + 1013904223u;
        float b = (float)((st >> 8) & 0xFFFFu) / 65535.0f;
        float x = u * fw, y = v * fh;
        float dx = x - sc, dy = y - scy;
        if (dx*dx + dy*dy > rscr * rscr) continue;   /* 圆屏外不画 */
        float keepout = d->r;
        if (d->halo_on && d->halo_extent > 1.0f)
            keepout *= d->halo_extent;
        if (keepout > 1.0f && dx*dx + dy*dy < keepout * keepout)
            continue;                                /* POST 后不盖住星球 */
        float sz = 0.7f + b * 1.2f;
        wf_pw_rect(&pw, x - sz * 0.5f, y - sz * 0.5f, sz, sz);
    }
    wf_pw_end(&pw);
    wf_draw_path(target, d->stars_path, pw.n, w, h, d->stars_rgb,
                 VG_LITE_BLEND_SRC_OVER);
}

/* 两侧装饰弧：左右各一段，每 4 度一个折线段，按线宽描成四边形 */
static inline void wf_draw_arcs(wf_decor_t *d, vg_lite_buffer_t *target,
                                int w, int h)
{
    if (!d->arcs_on || d->arcs_r <= 1.0f || d->arcs_thick <= 0.0f) return;
    if (!d->path) {
        d->path = (float *)malloc(sizeof(float) * WF_PATH_FLOATS);
        if (!d->path) return;
    }
    wf_pathw_t pw;
    wf_pw_init(&pw, d->path, WF_PATH_FLOATS);
    float half = d->arcs_span_deg * (float)M_PI / 180.0f;
    int nseg = (int)(d->arcs_span_deg * 2.0f / 4.0f);
    if (nseg < 4)  nseg = 4;
    if (nseg > 60) nseg = 60;
    for (int side = 0; side < 2; side++) {
        float base = (side == 0) ? 0.0f : (float)M_PI;
        float px = 0.0f, py = 0.0f;
        for (int i = 0; i <= nseg; i++) {
            float a = base - half + (2.0f * half) * (float)i / (float)nseg;
            float x = d->cx + d->arcs_r * cosf(a);
            float y = d->cy + d->arcs_r * sinf(a);
            if (i > 0) wf_pw_stroke(&pw, px, py, x, y, d->arcs_thick);
            px = x; py = y;
        }
    }
    wf_pw_end(&pw);
    if (!pw.overflow)
        wf_draw_path(target, d->path, pw.n, w, h, d->arcs_rgb,
                     VG_LITE_BLEND_SRC_OVER);
}

static inline void wf_draw_text(wf_decor_t *d, vg_lite_buffer_t *target,
                                int w, int h)
{
    if (!d->path) {
        d->path = (float *)malloc(sizeof(float) * WF_PATH_FLOATS);
        if (!d->path) return;
    }
    for (int i = 0; i < WF_SLOTS; i++) {
        if (!d->slot[i].on) continue;
        wf_pathw_t pw;
        wf_pw_init(&pw, d->path, WF_PATH_FLOATS);
        if (d->slot[i].arc_r > 0.0f)
            wf_text_arc(&pw, d->slot[i].text, d->slot[i].x0, d->slot[i].y,
                        d->slot[i].arc_r, d->slot[i].arc_rad, d->slot[i].h,
                        d->slot[i].track, d->slot[i].font, d->slot[i].arc_dir);
        else
            wf_text_run(&pw, d->slot[i].text, d->slot[i].x0, d->slot[i].y,
                        d->slot[i].h, d->slot[i].track, d->slot[i].font);
        wf_pw_end(&pw);
        if (pw.overflow) {
            if (!d->logged) {
                fprintf(stderr, "wf_decor: path overflow (slot %d \"%s\")\n",
                        i, d->slot[i].text);
                d->logged = 1;
            }
            continue;
        }
        wf_draw_path(target, d->path, pw.n, w, h, d->slot[i].rgb,
                     VG_LITE_BLEND_SRC_OVER);
    }
}

/* ---------------------------------------------------------------- 排版辅助 */

/* 设直排文字。align: 0=以 x 为中心 1=x 为左边缘 2=x 为右边缘。返回串宽。 */
static inline float wf_set_text(wf_decor_t *d, int slot, const char *text,
                                float x, float y, float hgt, float track,
                                uint32_t rgb, int font, int align)
{
    if (slot < 0 || slot >= WF_SLOTS) return 0.0f;
    if (!text || !text[0] || hgt <= 1.0f) { d->slot[slot].on = 0; return 0.0f; }
    float tw = wf_text_run(NULL, text, 0.0f, 0.0f, hgt, track, font);
    strncpy(d->slot[slot].text, text, WF_TEXTLEN - 1);
    d->slot[slot].text[WF_TEXTLEN - 1] = '\0';
    d->slot[slot].x0    = (align == 1) ? x : (align == 2) ? (x - tw) : (x - tw * 0.5f);
    d->slot[slot].y     = y;
    d->slot[slot].h     = hgt;
    d->slot[slot].track = track;
    d->slot[slot].rgb   = rgb;
    d->slot[slot].font  = font;
    d->slot[slot].arc_r = 0.0f;
    d->slot[slot].on    = 1;
    return tw;
}

/* 设弧形文字。arc_deg 按屏幕系：-90 = 正上，+90 = 正下。
 * dir: +1 = 字顶朝外(上方弧)，-1 = 字顶朝内(下方弧，否则上下颠倒)。 */
static inline float wf_set_text_arc(wf_decor_t *d, int slot, const char *text,
                                    float cx, float cy, float arc_r, float arc_deg,
                                    float hgt, float track, uint32_t rgb,
                                    int font, int dir)
{
    if (slot < 0 || slot >= WF_SLOTS) return 0.0f;
    if (!text || !text[0] || hgt <= 1.0f || arc_r <= 1.0f) {
        d->slot[slot].on = 0;
        return 0.0f;
    }
    float len = wf_text_arc(NULL, text, 0.0f, 0.0f, arc_r, 0.0f, hgt, track, font, dir);
    strncpy(d->slot[slot].text, text, WF_TEXTLEN - 1);
    d->slot[slot].text[WF_TEXTLEN - 1] = '\0';
    d->slot[slot].x0      = cx;
    d->slot[slot].y       = cy;
    d->slot[slot].h       = hgt;
    d->slot[slot].track   = track;
    d->slot[slot].rgb     = rgb;
    d->slot[slot].font    = font;
    d->slot[slot].arc_r   = arc_r;
    d->slot[slot].arc_rad = arc_deg * (float)M_PI / 180.0f;
    d->slot[slot].arc_dir = dir;
    d->slot[slot].on      = 1;
    return len;
}

/* ------------------------------------------------- 预设：太空星球表盘 */

/* 所有观感参数集中在这一处。这些值是调出来的，不打算做成运行时开关 ——
 * 上层只需要 init + preset + 每帧一次 frame()，不必理解各参数含义。
 * 想换风格就复制这个函数改一份，而不是给 demo 加命令行参数。 */

#define WF_FONT_SEG      0      /* 7 段数码管：复古电子表 */
#define WF_FONT_MONO     1      /* 几何单线：航天仪表风 */

typedef struct {
    /* 屏幕尺寸与星球状态，每帧由调用方给出 */
    int   scr_w, scr_h;
    float sphere_r;     /* 星球屏幕半径，由 demo 按固定场景参数提供 */
} wf_frame_t;

static inline void wf_preset_space(wf_decor_t *d)
{
    /* 大气光晕：色调取自 Blue Marble 贴图的轮廓平均色，亮度约 1.6 倍。
     * 纯白或高饱和蓝都会显得和地表不协调。 */
    d->halo_on       = 1;
    d->halo_rgb      = 0x424170u;
    d->halo_strength = 0.75f;
    d->halo_extent   = 1.12f;    /* 外缘 / 星球半径 */
    d->halo_inward   = 0.08f;    /* 向内渗透占半径比例 */
    d->halo_falloff  = 1.5f;

    /* 明暗遮罩取代逐图元 flat 光照：逐像素、无台阶。
     * 方向的 z 分量越小 = 与视轴夹角越大 = 阴影越多。0.38 约合 68°。 */
    d->shade_on      = 1;
    d->shade_dir[0]  = 0.90f;
    d->shade_dir[1]  = 0.22f;
    d->shade_dir[2]  = 0.38f;
    d->shade_ambient = 0.04f;    /* 背光面近全黑 */
    d->shade_diffuse = 1.15f;
    d->shade_limb    = 0.06f;    /* 临边压暗宽度占半径比例 */

    /* 背景星点：不宜比文字抢眼，取文字亮度的 70% */
    d->stars_n    = 40;
    d->stars_seed = 20260820u;
    d->stars_rgb  = 0xB2B2B2u;

    d->arcs_on = 1;              /* 两侧装饰弧，几何由 wf_frame() 按星球位置算 */

    /* 文字：纯白单线体 */
    d->font_style = WF_FONT_MONO;
    d->text_rgb   = 0xFFFFFFu;
}

/* 每帧一次。time_text 形如 "10:08:30"，date_text 形如 "THU 08-20"。
 * 版式全部在这里定：时间弧在上、日期弧在下、两侧装饰弧贴着星球外圈。 */
static inline void wf_frame(wf_decor_t *d, const wf_frame_t *f,
                            const char *time_text, const char *date_text)
{
    /* 字号与留白按屏高取比例，换分辨率自动跟随。480 屏 → 40 / 20 / 8 px */
    float h_time = (float)f->scr_h * 0.0833f;
    float h_date = (float)f->scr_h * 0.0417f;
    float bezel  = (float)f->scr_h * 0.0167f;
    float gap    = 14.0f;        /* 文字内缘到光晕外缘的间隙 */

    float scx = (float)f->scr_w * 0.5f;
    float scy = (float)f->scr_h * 0.5f;

    d->cx = scx;                 /* 装饰与星球同心：相机恒定看向 center，
                                  * 故星球圆心就是视口中心 */
    d->cy = scy;
    d->r  = f->sphere_r;

    float rho = f->sphere_r * d->halo_extent;       /* 需避开的内圈 = 光晕外缘 */
    if (rho <= 1.0f) rho = (float)f->scr_h * 0.323f;    /* 首帧兜底 */

    /* 上下弧共用同一内缘半径 → 星球在两行文字之间自然居中，间隙相等。 */
    float rin  = rho + gap;
    float rmax = scy - bezel;
    float rt = rin + h_time;  if (rt > rmax) rt = rmax;
    float rd = rin + h_date;  if (rd > rmax) rd = rmax;

    if (d->arcs_on) {
        d->arcs_r        = rho + 19.0f;   /* 贴着星球外圈，填补左右空旷 */
        d->arcs_span_deg = 34.0f;
        d->arcs_thick    = 3.5f;
        d->arcs_rgb      = d->text_rgb;
    }

    /* 上方弧字顶朝外(dir=+1)，下方弧字顶朝内(dir=-1)，否则下方文字上下颠倒 */
    wf_set_text_arc(d, 0, time_text, scx, scy, rt, -90.0f,
                    h_time, h_time * 0.10f, d->text_rgb, d->font_style, +1);
    wf_set_text_arc(d, 1, date_text, scx, scy, rd, +90.0f,
                    h_date, h_date * 0.16f, d->text_rgb, d->font_style, -1);
}

/* 合成时钟：不读 RTC(未必准，且合成值让截图可复现)。从固定时刻起按累计秒推进。 */
static inline void wf_clock_text(float elapsed_total, char *hms, size_t hms_sz,
                                 const char **date_text)
{
    const int T0 = 10 * 3600 + 8 * 60 + 30;     /* 起始 10:08:30 */
    int t = (T0 + (int)elapsed_total) % 86400;
    snprintf(hms, hms_sz, "%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    *date_text = "THU 08-20";
}

/* ------------------------------------------------------------------- 钩子 */

/* 注册给 r3d_engine_set_post_geometry_hook。user 指向 wf_decor_t。 */
static inline void wf_decor_hook(void *target, int w, int h, void *user)
{
    wf_decor_t *d = (wf_decor_t *)user;
    vg_lite_buffer_t *tb = (vg_lite_buffer_t *)target;
    if (!d || !tb) return;

    /* 星点只保留在星球和光晕外侧，故在 POST 阶段绘制也不会盖住 3D 几何。
     * 遮罩(乘法压暗)必须先贴，否则会把光晕一起压暗；光晕 additive；
     * 弧线与文字最后画。 */
    wf_draw_stars(d, tb, w, h);

    if (d->shade_on && d->r > 1.0f && wf_shade_bake(d))
        wf_blit_tex(tb, &d->shade_buf, d->cx, d->cy,
                    2.0f * d->r * WF_SHADE_MARGIN, VG_LITE_BLEND_SRC_OVER);

    if (d->halo_on && d->halo_strength > 0.0f && d->r > 1.0f && wf_halo_bake(d))
        wf_blit_tex(tb, &d->halo_buf, d->cx, d->cy,
                    2.0f * d->r * d->halo_extent, VG_LITE_BLEND_ADDITIVE);

    wf_draw_arcs(d, tb, w, h);
    wf_draw_text(d, tb, w, h);
}

#endif /* WF_DECOR_H */
