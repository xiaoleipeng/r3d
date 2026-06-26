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
