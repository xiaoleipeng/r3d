/*
 * vgl_text.h — VGLite 后端的极简矢量字形，供表盘类文字覆盖层使用
 *
 * 为什么用矢量路径而不是位图字体图集：
 *   1) 后端画纯色三角形走的就是 vg_lite_draw + VG_LITE_HIGH(开 AA)，文字沿用同一
 *      条路径即可免费拿到抗锯齿；位图图集放大会发虚、缩小会糊。
 *   2) 设备 PSRAM 已接近用满(光晕/遮罩两张 128x128 各 64KB 已在挤)，位图图集要再
 *      占一块 GPU 内存；矢量路径只占普通 RAM 里一段 float，GPU 内存零占用。
 *
 * 字形来源：
 *   数字/冒号/连字符 —— 7 段数码管，每段一个四边形，10 个字形只需 10 个 bit mask。
 *   大写字母         —— 5x7 点阵，每个点一个小方块；26 个字母共 130 字节。
 *   两者都是纯多边形，无需任何外部字体资源。
 *
 * 关键约束：同一颜色的所有字符合成一条 path(多个 MOVE/LINE/CLOSE 子路径)，
 * 因此整块文字只花 1 次 draw call。对比地球每帧 880 次，成本可忽略。
 */

#ifndef WF_FONT_H
#define WF_FONT_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "vg_lite.h"   /* VLC_OP_* 常量 */

/* ---- 7 段数码管 ---------------------------------------------------------
 * 段编号(与工业惯例一致)：
 *      aaa
 *     f   b
 *     f   b
 *      ggg
 *     e   c
 *     e   c
 *      ddd
 * bit0=a bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g
 */
static const uint8_t WF7_DIGIT[10] = {
    /* 0 */ 0x3F, /* abcdef  */
    /* 1 */ 0x06, /* bc      */
    /* 2 */ 0x5B, /* abdeg   */
    /* 3 */ 0x4F, /* abcdg   */
    /* 4 */ 0x66, /* bcfg    */
    /* 5 */ 0x6D, /* acdfg   */
    /* 6 */ 0x7D, /* acdefg  */
    /* 7 */ 0x07, /* abc     */
    /* 8 */ 0x7F, /* abcdefg */
    /* 9 */ 0x6F, /* abcdfg  */
};

/* path 构造器：往 float 缓冲里追加子路径。
 * 与后端三角形一致 —— opcode 槽按 uint32 位模式写、坐标槽按 IEEE754 float 写
 * (backend 的 extract_tri 修复过这个坑：FP32 path 的 opcode 是整数位模式)。 */
typedef struct {
    float   *buf;
    uint32_t cap;      /* buf 可容纳的 float 个数 */
    uint32_t n;        /* 已用 float 个数 */
    int      overflow;
    /* 逐点仿射：sx = m[0]*lx + m[1]*ly + m[2] ; sy = m[3]*lx + m[4]*ly + m[5]
     * 弧形排版时逐字形设置(整字形旋转)，直排时为单位变换走快路径。 */
    float    m[6];
    int      has_xform;
} wf_pathw_t;

static inline void wf_pw_init(wf_pathw_t *w, float *buf, uint32_t cap)
{
    w->buf = buf; w->cap = cap; w->n = 0; w->overflow = 0;
    w->has_xform = 0;
    w->m[0] = 1.0f; w->m[1] = 0.0f; w->m[2] = 0.0f;
    w->m[3] = 0.0f; w->m[4] = 1.0f; w->m[5] = 0.0f;
}

static inline void wf_pw_xform(wf_pathw_t *w, float a, float b, float c,
                                float d, float e, float f)
{
    w->m[0]=a; w->m[1]=b; w->m[2]=c; w->m[3]=d; w->m[4]=e; w->m[5]=f;
    w->has_xform = 1;
}

static inline void wf_pw_noxform(wf_pathw_t *w) { w->has_xform = 0; }

/* 追加一个 4 点子路径：MOVE+3*LINE+CLOSE = 13 个 float。
 * 若设了仿射变换则逐点变换(弧形排版时矩形会变成旋转后的四边形)。 */
static inline void wf_pw_rect(wf_pathw_t *w, float x, float y, float ww, float hh)
{
    if (w->n + 13 > w->cap) { w->overflow = 1; return; }
    float lx[4] = { x, x + ww, x + ww, x };
    float ly[4] = { y, y,      y + hh, y + hh };
    float *p = &w->buf[w->n];
    uint32_t *op = (uint32_t *)p;
    for (int k = 0; k < 4; k++) {
        float sx, sy;
        if (w->has_xform) {
            sx = w->m[0]*lx[k] + w->m[1]*ly[k] + w->m[2];
            sy = w->m[3]*lx[k] + w->m[4]*ly[k] + w->m[5];
        } else { sx = lx[k]; sy = ly[k]; }
        op[k*3] = (uint32_t)(k == 0 ? VLC_OP_MOVE : VLC_OP_LINE);
        p[k*3 + 1] = sx; p[k*3 + 2] = sy;
    }
    op[12] = (uint32_t)VLC_OP_CLOSE;
    w->n += 13;
}

/* 追加一个四点多边形(数码管的斜切段用) */
static inline void wf_pw_quad(wf_pathw_t *w, const float px[4], const float py[4])
{
    if (w->n + 13 > w->cap) { w->overflow = 1; return; }
    float *p = &w->buf[w->n];
    uint32_t *op = (uint32_t *)p;
    op[0] = (uint32_t)VLC_OP_MOVE; p[1]  = px[0]; p[2]  = py[0];
    op[3] = (uint32_t)VLC_OP_LINE; p[4]  = px[1]; p[5]  = py[1];
    op[6] = (uint32_t)VLC_OP_LINE; p[7]  = px[2]; p[8]  = py[2];
    op[9] = (uint32_t)VLC_OP_LINE; p[10] = px[3]; p[11] = py[3];
    op[12] = (uint32_t)VLC_OP_CLOSE;
    w->n += 13;
}

static inline void wf_pw_end(wf_pathw_t *w)
{
    if (w->n + 1 > w->cap) { w->overflow = 1; return; }
    ((uint32_t *)&w->buf[w->n])[0] = (uint32_t)VLC_OP_END;
    w->n += 1;
}

/* ---- 数码管字形几何 ----
 * 以字符左上角为原点，h = 字高，w = 字宽(取 0.58h，接近数字表比例)。
 * t = 段粗(0.13h)，段做成六边形斜切端会更像真数码管，但四边形已足够且点数减半。 */
#define WF7_ASPECT   0.58f
#define WF7_THICK    0.13f
#define WF7_GAP      0.02f    /* 段与段之间的留白，占字高比例 */

/* 画一个数码管段。seg: 0=a 1=b 2=c 3=d 4=e 5=f 6=g */
static inline void wf7_seg(wf_pathw_t *w, float x, float y, float h, int seg)
{
    float ww = h * WF7_ASPECT;
    float t  = h * WF7_THICK;
    float g  = h * WF7_GAP;
    float mid = y + (h - t) * 0.5f;          /* 中横段的 y */
    switch (seg) {
    case 0: wf_pw_rect(w, x + t*0.5f + g,        y,                   ww - t - 2*g, t); break; /* a 上横 */
    case 1: wf_pw_rect(w, x + ww - t,            y + t*0.5f + g,      t, (h - t)*0.5f - t*0.5f - 2*g); break; /* b 右上 */
    case 2: wf_pw_rect(w, x + ww - t,            mid + t*0.5f + g,    t, (h - t)*0.5f - t*0.5f - 2*g); break; /* c 右下 */
    case 3: wf_pw_rect(w, x + t*0.5f + g,        y + h - t,           ww - t - 2*g, t); break; /* d 下横 */
    case 4: wf_pw_rect(w, x,                     mid + t*0.5f + g,    t, (h - t)*0.5f - t*0.5f - 2*g); break; /* e 左下 */
    case 5: wf_pw_rect(w, x,                     y + t*0.5f + g,      t, (h - t)*0.5f - t*0.5f - 2*g); break; /* f 左上 */
    case 6: wf_pw_rect(w, x + t*0.5f + g,        mid,                 ww - t - 2*g, t); break; /* g 中横 */
    default: break;
    }
}

/* 字符宽度(不含字距) */
static inline float wf7_char_w(char c, float h)
{
    if (c == ':' || c == '.') return h * 0.22f;
    if (c == '-')             return h * 0.36f;
    if (c == ' ')             return h * 0.30f;
    return h * WF7_ASPECT;
}

/* 把一个 7 段字符追加进 path。返回该字符占用的宽度。 */
static inline float wf7_char(wf_pathw_t *w, char c, float x, float y, float h)
{
    float t = h * WF7_THICK;
    if (c >= '0' && c <= '9') {
        uint8_t m = WF7_DIGIT[c - '0'];
        for (int s = 0; s < 7; s++)
            if (m & (1u << s)) wf7_seg(w, x, y, h, s);
    } else if (c == ':') {
        float d = t * 1.0f;
        wf_pw_rect(w, x, y + h * 0.28f - d*0.5f, d, d);
        wf_pw_rect(w, x, y + h * 0.72f - d*0.5f, d, d);
    } else if (c == '.') {
        wf_pw_rect(w, x, y + h - t, t, t);
    } else if (c == '-') {
        wf_pw_rect(w, x, y + (h - t) * 0.5f, h * 0.36f, t);
    }
    /* 空格与未知字符只占位 */
    return wf7_char_w(c, h);
}

/* ==== 几何单线字体(monoline) =============================================
 * 7 段数码管的"旧电子表感"来自段与段之间的断缝和缺角。单线字体笔画等宽、
 * 首尾相连、转角做切角(chamfer)，读起来是航天仪表/Eurostile 那种科技感，
 * 与太空题材更搭。
 *
 * 实现：每个字形是若干条折线；每段折线渲染成一个等宽四边形，折点处补一个
 * 边长等于笔宽的小方块填补拼角。所有四边形绕向一致(见下方 orientation 注释)，
 * 配合覆盖层的 NON_ZERO 填充得到并集 —— 若绕向不一致，重叠处会因缠绕数
 * 相消而出现空洞。
 *
 * 字形数据：每字形一个扁平 int8 数组，块结构 [closed, npts, x0,y0, x1,y1, ...]，
 * 坐标 0..100(x 按字宽、y 按字高归一)，以 closed==127 作结束标记。 */

#define WFM_THICK  0.115f     /* 笔宽占字高比例 */

/* 沿线段发一个等宽四边形。
 * 发点顺序取 p0-n → p1-n → p1+n → p0+n(n 为左法线)，与 wf_pw_rect 的
 * 顺时针一致 —— 该顺序的有向面积符号不随线段方向改变(旋转不改变绕向)，
 * 故所有笔画与拼角块绕向统一，NON_ZERO 下重叠即并集。 */
static inline void wf_pw_stroke(wf_pathw_t *w, float x0, float y0,
                                 float x1, float y1, float t)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * t * 0.5f;
    float ny =  dx / len * t * 0.5f;
    if (w->n + 13 > w->cap) { w->overflow = 1; return; }
    float lx[4] = { x0 - nx, x1 - nx, x1 + nx, x0 + nx };
    float ly[4] = { y0 - ny, y1 - ny, y1 + ny, y0 + ny };
    float *p = &w->buf[w->n];
    uint32_t *op = (uint32_t *)p;
    for (int k = 0; k < 4; k++) {
        float sx, sy;
        if (w->has_xform) {
            sx = w->m[0]*lx[k] + w->m[1]*ly[k] + w->m[2];
            sy = w->m[3]*lx[k] + w->m[4]*ly[k] + w->m[5];
        } else { sx = lx[k]; sy = ly[k]; }
        op[k*3] = (uint32_t)(k == 0 ? VLC_OP_MOVE : VLC_OP_LINE);
        p[k*3 + 1] = sx; p[k*3 + 2] = sy;
    }
    op[12] = (uint32_t)VLC_OP_CLOSE;
    w->n += 13;
}

#define WFM_END 127

/* 数字：切角几何风格，笔画连续 */
static const int8_t WFM_D0[] = { 1, 8,  0,22, 22,0, 78,0, 100,22, 100,78, 78,100, 22,100, 0,78, WFM_END };
static const int8_t WFM_D1[] = { 0, 3, 14,24, 56,0, 56,100, WFM_END };
static const int8_t WFM_D2[] = { 0, 7,  0,24, 24,0, 76,0, 100,24, 100,40, 0,100, 100,100, WFM_END };
static const int8_t WFM_D3[] = { 0, 9,  0,0, 76,0, 100,24, 100,38, 58,50, 100,62, 100,76, 76,100, 0,100, WFM_END };
static const int8_t WFM_D4[] = { 0, 3, 72,0, 0,70, 100,70,   0, 2, 72,0, 72,100, WFM_END };
static const int8_t WFM_D5[] = { 0, 8, 100,0, 0,0, 0,44, 76,44, 100,66, 100,76, 76,100, 0,100, WFM_END };
static const int8_t WFM_D6[] = { 0,11, 100,22, 76,0, 24,0, 0,24, 0,76, 24,100, 76,100, 100,76, 100,58, 76,46, 0,46, WFM_END };
static const int8_t WFM_D7[] = { 0, 3,  0,0, 100,0, 36,100, WFM_END };
static const int8_t WFM_D8[] = { 1, 8,  0,22, 22,0, 78,0, 100,22, 100,78, 78,100, 22,100, 0,78,   0, 2, 4,50, 96,50, WFM_END };
static const int8_t WFM_D9[] = { 0,11,  0,78, 24,100, 76,100, 100,76, 100,24, 76,0, 24,0, 0,24, 0,42, 24,54, 100,54, WFM_END };
/* 字母(仅星期/月份用到的) */
static const int8_t WFM_T[]  = { 0, 2,  0,0, 100,0,   0, 2, 50,0, 50,100, WFM_END };
static const int8_t WFM_H[]  = { 0, 2,  0,0, 0,100,   0, 2, 100,0, 100,100,   0, 2, 0,50, 100,50, WFM_END };
static const int8_t WFM_U[]  = { 0, 6,  0,0, 0,76, 24,100, 76,100, 100,76, 100,0, WFM_END };
static const int8_t WFM_M[]  = { 0, 5,  0,100, 0,0, 50,56, 100,0, 100,100, WFM_END };
static const int8_t WFM_O[]  = { 1, 8,  0,22, 22,0, 78,0, 100,22, 100,78, 78,100, 22,100, 0,78, WFM_END };
static const int8_t WFM_N[]  = { 0, 4,  0,100, 0,0, 100,100, 100,0, WFM_END };
static const int8_t WFM_E[]  = { 0, 4, 100,0, 0,0, 0,100, 100,100,   0, 2, 0,50, 72,50, WFM_END };
static const int8_t WFM_D[]  = { 1, 6,  0,0, 70,0, 100,30, 100,70, 70,100, 0,100, WFM_END };
static const int8_t WFM_S[]  = { 0,12, 100,22, 76,0, 24,0, 0,22, 0,38, 24,50, 76,50, 100,62, 100,78, 76,100, 24,100, 0,78, WFM_END };
static const int8_t WFM_W[]  = { 0, 5,  0,0, 20,100, 50,42, 80,100, 100,0, WFM_END };
static const int8_t WFM_F[]  = { 0, 3, 100,0, 0,0, 0,100,   0, 2, 0,50, 72,50, WFM_END };
static const int8_t WFM_R[]  = { 0, 6,  0,100, 0,0, 70,0, 100,26, 70,50, 0,50,   0, 2, 52,50, 100,100, WFM_END };
static const int8_t WFM_I[]  = { 0, 2, 50,0, 50,100, WFM_END };
static const int8_t WFM_A[]  = { 0, 3,  0,100, 50,0, 100,100,   0, 2, 20,62, 80,62, WFM_END };

typedef struct { char ch; uint8_t adv; const int8_t *d; } wfm_glyph_t;

/* adv = 字宽占字高的百分比。数字统一宽度以便时间不抖动。 */
static const wfm_glyph_t WFM[] = {
    { '0', 58, WFM_D0 }, { '1', 58, WFM_D1 }, { '2', 58, WFM_D2 },
    { '3', 58, WFM_D3 }, { '4', 58, WFM_D4 }, { '5', 58, WFM_D5 },
    { '6', 58, WFM_D6 }, { '7', 58, WFM_D7 }, { '8', 58, WFM_D8 },
    { '9', 58, WFM_D9 },
    { 'T', 56, WFM_T }, { 'H', 56, WFM_H }, { 'U', 56, WFM_U },
    { 'M', 64, WFM_M }, { 'O', 58, WFM_O }, { 'N', 56, WFM_N },
    { 'E', 52, WFM_E }, { 'D', 58, WFM_D }, { 'S', 54, WFM_S },
    { 'W', 68, WFM_W }, { 'F', 52, WFM_F }, { 'R', 56, WFM_R },
    { 'I', 26, WFM_I }, { 'A', 58, WFM_A },
};

static inline const wfm_glyph_t *wfm_find(char c)
{
    for (unsigned i = 0; i < sizeof(WFM)/sizeof(WFM[0]); i++)
        if (WFM[i].ch == c) return &WFM[i];
    return NULL;
}

static inline float wfm_char_w(char c, float h)
{
    if (c == ':') return h * 0.24f;
    if (c == '-') return h * 0.40f;
    if (c == ' ') return h * 0.30f;
    const wfm_glyph_t *g = wfm_find(c);
    return g ? h * (float)g->adv / 100.0f : h * 0.30f;
}

static inline float wfm_char(wf_pathw_t *w, char c, float x, float y, float h)
{
    float t = h * WFM_THICK;
    if (c == ':') {
        float d = t * 1.05f;
        float cx = x + h * 0.12f - d * 0.5f;
        wf_pw_rect(w, cx, y + h * 0.30f - d*0.5f, d, d);
        wf_pw_rect(w, cx, y + h * 0.70f - d*0.5f, d, d);
        return wfm_char_w(c, h);
    }
    if (c == '-') {
        wf_pw_stroke(w, x + t*0.5f, y + h*0.5f, x + h*0.40f - t*0.5f, y + h*0.5f, t);
        return wfm_char_w(c, h);
    }
    const wfm_glyph_t *g = wfm_find(c);
    if (g == NULL) return wfm_char_w(c, h);

    float ww = h * (float)g->adv / 100.0f;
    const int8_t *d = g->d;
    while (*d != WFM_END) {
        int closed = *d++;
        int npts   = *d++;
        float px = 0.0f, py = 0.0f, fx = 0.0f, fy = 0.0f;
        for (int i = 0; i < npts; i++) {
            float gx = x + (float)d[0] / 100.0f * ww;
            float gy = y + (float)d[1] / 100.0f * h;
            d += 2;
            if (i == 0) { fx = gx; fy = gy; }
            else {
                wf_pw_stroke(w, px, py, gx, gy, t);
                /* 折点补方块填拼角 */
                wf_pw_rect(w, px - t*0.5f, py - t*0.5f, t, t);
            }
            px = gx; py = gy;
        }
        if (closed && npts > 2) {
            wf_pw_stroke(w, px, py, fx, fy, t);
            wf_pw_rect(w, px - t*0.5f, py - t*0.5f, t, t);
            wf_pw_rect(w, fx - t*0.5f, fy - t*0.5f, t, t);
        }
    }
    return ww;
}

/* ---- 整串排版 ----
 * mode: 0 = 7 段数码管，1 = 几何单线。
 * 返回串宽，用于居中。传 w=NULL 时只测宽不产生几何。 */
static inline float wf_text_run(wf_pathw_t *w, const char *s, float x, float y,
                                 float h, float track, int mode)
{
    float cx = x, total = 0.0f;
    for (const char *p = s; *p; p++) {
        char c = *p;
        float cw;
        if (mode == 1)
            cw = w ? wfm_char(w, c, cx, y, h) : wfm_char_w(c, h);
        else
            cw = w ? wf7_char(w, c, cx, y, h) : wf7_char_w(c, h);
        cx    += cw + track;
        total += cw + track;
    }
    if (total > 0.0f) total -= track;   /* 末字符后不留字距 */
    return total;
}

/* ---- 弧形排版 ----------------------------------------------------------
 * 圆屏表盘常见需求：文字沿屏幕边缘弯曲。做法是逐字形设置一个"旋转+平移"仿射，
 * 字形自身在局部坐标里仍是直排的矩形，整体被旋转到弧上(glyph-level rotation)。
 * 只要字宽远小于弧半径，观感就是平滑的弧，不需要把字形本身做形变。
 *
 * 屏幕坐标约定：x 向右，y 向下。角度 phi 从 +x 轴起、顺时针为正，
 * 故 phi = -90° 是 12 点方向(正上)，phi = +90° 是 6 点方向(正下)。
 * 弧上一点 P(phi) = (cx + R*cos phi, cy + R*sin phi)。
 *
 * 局部坐标基向量(dir = +1)：
 *   x̂ = (-sin phi,  cos phi)     沿弧、阅读方向
 *   ŷ = (-cos phi, -sin phi)     指向圆心(内侧)
 * 校验：phi=-90°(正上) → x̂=(1,0) 屏幕向右 ✓，ŷ=(0,1) 屏幕向下且朝圆心 ✓
 *
 * dir = -1 时两个基向量同时取反，用于底部文字：
 *   phi=+90°(正下) → x̂=(1,0) 仍向右 ✓，ŷ=(0,1) 仍向下(此时是背离圆心) ✓
 * 若底部沿用 dir=+1，ŷ 会指向圆心即屏幕向上，字就上下颠倒了。
 *
 * R 对应字形顶边所在半径，与直排版 y=顶部 的语义一致。
 * angle_c = 文字中心所在角度(弧度)。传 w=NULL 时只测宽不产生几何。 */
static inline float wf_text_arc(wf_pathw_t *w, const char *s,
                                 float cx, float cy, float R, float angle_c,
                                 float h, float track, int mode, int dir)
{
    if (R <= 1.0f) return 0.0f;

    /* 先量总弧长，用于把文字整体居中到 angle_c */
    float total = 0.0f;
    int   n = 0;
    for (const char *p = s; *p; p++) {
        total += (mode == 1) ? wfm_char_w(*p, h) : wf7_char_w(*p, h);
        n++;
    }
    if (n > 1) total += track * (float)(n - 1);
    if (w == NULL) return total;

    float d = (dir >= 0) ? 1.0f : -1.0f;

    /* arc_r 统一约定为"字形靠表圈那一侧(外缘)的半径"，与 dir 无关，便于上下对称布局。
     * dir=+1 时 ŷ 朝圆心，局部 y=0 就是外缘，故 R_local = R；
     * dir=-1 时 ŷ 背离圆心，局部 y=0 是内缘，故 R_local = R - h。 */
    float Rl   = (dir >= 0) ? R : (R - h);
    /* 内缘半径。角步进必须按内缘算：字宽恒定，而同一角度槽对应的弧长随半径线性
     * 缩短，若按外缘步进，内缘处相邻字形会重叠(h/R 越大越严重)。按内缘步进则
     * 内缘刚好不重叠，外缘间隙略宽 —— 这是弧形排版的标准做法。 */
    float Rin  = (dir >= 0) ? (Rl - h) : Rl;
    if (Rin < 1.0f) Rin = 1.0f;

    float phi0 = angle_c - d * (total / Rin) * 0.5f;   /* 阅读方向起点角 */
    float acc = 0.0f;

    for (const char *p = s; *p; p++) {
        char c = *p;
        float cw = (mode == 1) ? wfm_char_w(c, h) : wf7_char_w(c, h);

        float phi = phi0 + d * ((acc + cw * 0.5f) / Rin);  /* 字形中心对应角 */
        float cph = cosf(phi), sph = sinf(phi);
        float px = cx + Rl * cph, py = cy + Rl * sph;

        /* 局部 → 屏幕：screen = P + lx*x̂ + ly*ŷ */
        wf_pw_xform(w,
                     d * (-sph), d * (-cph), px,
                     d * ( cph), d * (-sph), py);

        /* 局部坐标里字形横向以 0 为中心、纵向自 0 向内(顶边贴在半径 R 上) */
        if (mode == 1) wfm_char(w, c, -cw * 0.5f, 0.0f, h);
        else           wf7_char(w, c, -cw * 0.5f, 0.0f, h);

        acc += cw + track;
    }
    wf_pw_noxform(w);
    return total;
}

#endif /* WF_FONT_H */
