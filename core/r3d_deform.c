/*
 * r3d_deform.c — morph 顶点变形（架构文档 §11.4 步骤3.1）
 */
#include "r3d/r3d_deform.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 单调微秒(诊断分段计时用) */
static long r3d_deform_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec*1000000L + ts.tv_nsec/1000);
}

/* 连续 axpy：s[k] += w*d[k]，k in [0,n)。
 * 用 restrict 显式声明 s 与 d 不重叠(实际就是两块独立 malloc/raw 内存)，
 * 消除编译器的别名顾虑，使 -O3 在 Cortex-M55 上自动生成 MVE(Helium)向量循环
 * (一次 4 个 float 的 vldrw/vfma/vstrw)，而非退化为逐个 float 的标量 vfma。
 * 单独成函数并加 restrict 是让自动向量化稳定触发的关键(内联在大函数里、
 * 指针来自结构体解引用时，GCC 无法证明不别名，会保守放弃向量化)。 */
static void r3d_deform_axpy(float *restrict s, const float *restrict d,
                            float w, uint32_t n){
    for (uint32_t k=0;k<n;k++) s[k] += w * d[k];
}

int r3d_deform_init(r3d_deform_t *df, const r3d_model_t *m){
    df->count = m->vertex_count;
    df->out = (r3d_vertex_t*)malloc((size_t)m->vertex_count*sizeof(r3d_vertex_t));
    if(!df->out) return -1;
    memcpy(df->out, m->vertices, (size_t)m->vertex_count*sizeof(r3d_vertex_t));

    /* 连续位置累加缓冲：仅覆盖 morph 影响的 morph_vertex_count 个顶点。
     * 累加在此连续 float 数组上做，cache 友好且 M55 Helium/MVE 可向量化。 */
    df->scratch_verts = m->morph_vertex_count;
    df->scratch = NULL;
    if (df->scratch_verts) {
        df->scratch = (float*)malloc((size_t)df->scratch_verts * 3 * sizeof(float));
        if (!df->scratch) { free(df->out); df->out = NULL; return -1; }
    }

    df->stat_targets = df->stat_active = df->stat_morph_verts = 0;
    df->stat_reset_us = df->stat_accum_us = 0;
    return 0;
}

void r3d_deform_free(r3d_deform_t *df){
    free(df->out); df->out=NULL; df->count=0;
    free(df->scratch); df->scratch=NULL; df->scratch_verts=0;
}

void r3d_deform_apply(r3d_deform_t *df, const r3d_model_t *m,
                      const float *weights, uint32_t nweights){
    uint32_t vc = m->vertex_count;
    uint32_t nt = m->morph_target_count;
    if (nweights < nt) nt = nweights;

    df->stat_targets = m->morph_target_count;
    df->stat_morph_verts = m->morph_vertex_count;
    df->stat_active = 0;

    uint32_t mvc  = m->morph_vertex_count;
    uint32_t mbase= m->morph_vertex_base;

    /* 无 morph 数据 / 无 scratch：退化为仅复位 base(与旧行为一致)。 */
    if (!m->morph_deltas || nt==0 || mvc==0 || !df->scratch) {
        long t0 = r3d_deform_now_us();
        for (uint32_t i=0;i<vc;i++) df->out[i].pos = m->vertices[i].pos;
        df->stat_reset_us = r3d_deform_now_us() - t0;
        df->stat_accum_us = 0;
        return;
    }

    /* mbase+mvc 已在加载期校验不越界(见 r3d_model.c)，此处可安全按 mvc 处理。 */

    /* ---- reset：非 morph 顶点直接复位；morph 顶点的 base 位置装入连续 scratch ---- */
    long t0 = r3d_deform_now_us();
    for (uint32_t i=0;i<vc;i++) df->out[i].pos = m->vertices[i].pos;
    /* 把 morph 区顶点的 base 位置搬进连续缓冲(1 遍跨步读) */
    for (uint32_t i=0;i<mvc;i++){
        const r3d_vec3_t *p = &m->vertices[mbase+i].pos;
        df->scratch[i*3+0] = p->x;
        df->scratch[i*3+1] = p->y;
        df->scratch[i*3+2] = p->z;
    }
    df->stat_reset_us = r3d_deform_now_us() - t0;

    /* ---- accum：在连续 scratch 上累加各 target 的 delta ----
     * 内层是两个连续 float 数组的 axpy(scratch += w*d)，
     * 无大结构体跨步、无 cache 冲刷，M55 Helium/MVE 可自动向量化。 */
    long a0 = r3d_deform_now_us();
    const uint32_t n3 = mvc * 3;
    for (uint32_t t=0; t<nt; t++){
        float w = weights[t];
        if (w==0.0f) continue;
        df->stat_active++;
        const float *d = m->morph_deltas + (size_t)t*n3;
        r3d_deform_axpy(df->scratch, d, w, n3);  /* MVE 向量化的连续累加 */
    }
    df->stat_accum_us = r3d_deform_now_us() - a0;

    /* ---- 写回：连续 scratch → out 大结构体的 pos(1 遍跨步写) ---- */
    for (uint32_t i=0;i<mvc;i++){
        df->out[mbase+i].pos.x = df->scratch[i*3+0];
        df->out[mbase+i].pos.y = df->scratch[i*3+1];
        df->out[mbase+i].pos.z = df->scratch[i*3+2];
    }
}
