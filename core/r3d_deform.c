/*
 * r3d_deform.c — morph 顶点变形（架构文档 §11.4 步骤3.1）
 */
#include "r3d/r3d_deform.h"
#include <stdlib.h>
#include <string.h>

int r3d_deform_init(r3d_deform_t *df, const r3d_model_t *m){
    df->count = m->vertex_count;
    df->out = (r3d_vertex_t*)malloc((size_t)m->vertex_count*sizeof(r3d_vertex_t));
    if(!df->out) return -1;
    memcpy(df->out, m->vertices, (size_t)m->vertex_count*sizeof(r3d_vertex_t));
    return 0;
}

void r3d_deform_free(r3d_deform_t *df){
    free(df->out); df->out=NULL; df->count=0;
}

void r3d_deform_apply(r3d_deform_t *df, const r3d_model_t *m,
                      const float *weights, uint32_t nweights){
    uint32_t vc = m->vertex_count;
    uint32_t nt = m->morph_target_count;
    if (nweights < nt) nt = nweights;

    /* 先复位为 base */
    for (uint32_t i=0;i<vc;i++) df->out[i].pos = m->vertices[i].pos;

    if (!m->morph_deltas || nt==0) return;

    /* 叠加每个 target 的加权 delta */
    for (uint32_t t=0; t<nt; t++){
        float w = weights[t];
        if (w==0.0f) continue;
        const float *d = m->morph_deltas + (size_t)t*vc*3;
        for (uint32_t i=0;i<vc;i++){
            df->out[i].pos.x += w * d[i*3+0];
            df->out[i].pos.y += w * d[i*3+1];
            df->out[i].pos.z += w * d[i*3+2];
        }
    }
}
