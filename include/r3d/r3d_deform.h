/*
 * r3d_deform.h — 顶点变形（morph）。架构文档 §11.4 步骤3.1。
 * final_pos = base_pos + Σ weightᵢ · deltaᵢ
 */
#ifndef R3D_DEFORM_H
#define R3D_DEFORM_H

#include "r3d_types.h"
#include "r3d_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    r3d_vertex_t *out;     /* 变形后顶点（拷贝自 base，pos 被改写）*/
    uint32_t      count;

    /* 连续 morph 位置累加缓冲(优化)：morph_vertex_count*3 个 float，紧凑排布。
     * 累加在此连续缓冲上进行(cache 友好、MVE 可向量化)，仅首尾各碰一次 40 字节
     * 的大顶点结构，避免原来 39 遍跨步读改写 out 数组的 cache 灾难。 */
    float        *scratch;
    uint32_t      scratch_verts;   /* scratch 覆盖的顶点数(= morph_vertex_count) */

    /* ---- 诊断(每次 apply 更新，供上层打点定位 deform 瓶颈) ---- */
    uint32_t      stat_targets;        /* morph target 总数 */
    uint32_t      stat_active;         /* 本次权重非零(实际参与累加)的 target 数 */
    uint32_t      stat_morph_verts;    /* 每个 target 覆盖的顶点数(mvc) */
    long          stat_reset_us;       /* reset 复位 base 耗时 */
    long          stat_accum_us;       /* 累加 delta 耗时 */
} r3d_deform_t;

/* 分配输出 buffer（vcount 个顶点），拷贝 base */
int  r3d_deform_init(r3d_deform_t *df, const r3d_model_t *m);
void r3d_deform_free(r3d_deform_t *df);

/* 应用 morph 权重：out.pos = base.pos + Σ w[i]·delta[i] */
void r3d_deform_apply(r3d_deform_t *df, const r3d_model_t *m,
                      const float *weights, uint32_t nweights);

#ifdef __cplusplus
}
#endif
#endif
