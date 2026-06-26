/* g2b_simplify.h — meshoptimizer 减面 + 顶点 cache 优化（被 main.c include）*/
#ifndef G2B_SIMPLIFY_H
#define G2B_SIMPLIFY_H

#include "meshoptimizer.h"

/* 对单个 prim 减面到 target_tris；cache 优化 */
static void g2b_simplify_prim(wprim_t *wp, uint32_t target_tris, int has_tex) {
    if (wp->icount < 3) return;
    uint32_t tri = wp->icount / 3;

    /* uint16 → uint32 索引（meshopt 用 32 位）*/
    unsigned int *i32 = (unsigned int*)malloc(wp->icount*sizeof(unsigned int));
    for (uint32_t i=0;i<wp->icount;i++) i32[i]=wp->idx[i];

    if (target_tris>0 && tri>target_tris) {
        size_t target_idx = (size_t)target_tris*3;
        unsigned int *out = (unsigned int*)malloc(wp->icount*sizeof(unsigned int));
        float err=0.0f;
        if (has_tex) {
            /* 带纹理 prim(表盘/logo/编织)：保 UV 属性，误差受控，绝不 sloppy
               (sloppy 不保 UV/拓扑，会毁掉纹理与平面) */
            const float attr_w[2] = { 1.0f, 1.0f };   /* u,v 权重 */
            size_t got = meshopt_simplifyWithAttributes(
                out, i32, wp->icount,
                &wp->verts[0].px, wp->vcount, sizeof(wvert_t),
                &wp->verts[0].u,  sizeof(wvert_t),     /* 属性=UV(offset 3) */
                attr_w, 2, NULL,
                target_idx, 0.1f, 0, &err);
            free(i32); i32 = out; wp->icount = (uint32_t)got;
        } else {
            /* 纯色 prim(螺丝/塑料块)：可激进，先保拓扑简化，不足再 sloppy 强制 */
            size_t got = meshopt_simplify(out, i32, wp->icount,
                                      &wp->verts[0].px, wp->vcount, sizeof(wvert_t),
                                      target_idx, 1.0f, 0, &err);
            if (got > target_idx * 3 / 2) {
                size_t got2 = meshopt_simplifySloppy(out, i32, wp->icount,
                                      &wp->verts[0].px, wp->vcount, sizeof(wvert_t),
                                      NULL, target_idx, 1.0f, &err);
                if (got2 >= 3) got = got2;
            }
            free(i32); i32 = out; wp->icount = (uint32_t)got;
        }
    }

    /* 顶点 cache 优化（不改顶点，只重排索引）*/
    meshopt_optimizeVertexCache(i32, i32, wp->icount, wp->vcount);

    /* 顶点 fetch 优化 + 压缩：去掉减面后未引用的顶点，降低顶点数 */
    {
        unsigned int *remap = (unsigned int*)malloc(wp->vcount*sizeof(unsigned int));
        size_t newvc = meshopt_optimizeVertexFetchRemap(remap, i32, wp->icount, wp->vcount);
        /* 重排索引 */
        meshopt_remapIndexBuffer(i32, i32, wp->icount, remap);
        /* 重排顶点及所有 per-vertex 数据 */
        wvert_t *nv = (wvert_t*)malloc(newvc*sizeof(wvert_t));
        meshopt_remapVertexBuffer(nv, wp->verts, wp->vcount, sizeof(wvert_t), remap);
        free(wp->verts); wp->verts = nv;
        if (wp->morph_delta && wp->morph_count){
            float *nm = (float*)malloc((size_t)wp->morph_count*newvc*3*sizeof(float));
            for (uint32_t t=0;t<wp->morph_count;t++)
                for (uint32_t old=0; old<wp->vcount; old++)
                    if (remap[old]!=~0u){
                        const float *s=&wp->morph_delta[((size_t)t*wp->vcount+old)*3];
                        float *d=&nm[((size_t)t*newvc+remap[old])*3];
                        d[0]=s[0];d[1]=s[1];d[2]=s[2];
                    }
            free(wp->morph_delta); wp->morph_delta=nm;
        }
        if (wp->joints){
            uint8_t *nj=(uint8_t*)malloc(newvc*4), *nw=(uint8_t*)malloc(newvc*4);
            for (uint32_t old=0; old<wp->vcount; old++)
                if (remap[old]!=~0u){
                    memcpy(&nj[remap[old]*4], &wp->joints[old*4], 4);
                    memcpy(&nw[remap[old]*4], &wp->weights[old*4], 4);
                }
            free(wp->joints); free(wp->weights); wp->joints=nj; wp->weights=nw;
        }
        wp->vcount = (uint32_t)newvc;
        free(remap);
    }

    /* 回写 uint16 */
    free(wp->idx);
    wp->idx = (uint16_t*)malloc(wp->icount*sizeof(uint16_t));
    for (uint32_t i=0;i<wp->icount;i++) wp->idx[i]=(uint16_t)i32[i];
    free(i32);
}

/* 全场景按比例分配预算减面 */
static void g2b_simplify(g2b_scene_t *sc, uint32_t max_tris) {
    if (max_tris==0 || sc->total_tris<=max_tris) {
        /* 仍做 cache 优化 */
        for (uint32_t i=0;i<sc->prim_count;i++){
            wprim_t *p=&sc->prims[i];
            int keepuv = (p->tex_id>=0) && !(p->mat_flags & 4); /* matcap(bit2) UV 运行时算，不需保 */
            g2b_simplify_prim(p, 0, keepuv);
        }
        return;
    }
    uint32_t total = sc->total_tris;
    uint32_t new_total = 0;
    for (uint32_t i=0;i<sc->prim_count;i++) {
        wprim_t *wp=&sc->prims[i];
        uint32_t tri = wp->icount/3;
        uint32_t budget = (uint32_t)((uint64_t)tri*max_tris/total);
        if (budget<1) budget=1;
        int keepuv = (wp->tex_id>=0) && !(wp->mat_flags & 4); /* matcap UV 运行时算 */
        g2b_simplify_prim(wp, budget, keepuv);
        new_total += wp->icount/3;
    }
    sc->total_tris = new_total;
    fprintf(stderr,"减面: %u → %u 三角形 (预算 %u)\n", total, new_total, max_tris);
}
#endif
