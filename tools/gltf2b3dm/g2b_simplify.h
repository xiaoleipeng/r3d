/* g2b_simplify.h — meshoptimizer 减面 + 顶点 cache 优化（被 main.c include）*/
#ifndef G2B_SIMPLIFY_H
#define G2B_SIMPLIFY_H

#include "meshoptimizer.h"
#include <math.h>

/* 对单个 prim 减面到 target_tris；cache 优化。
 * has_tex : 该 prim 带纹理(UV 需保护)。
 * is_anim : 该 prim 被骨骼蒙皮或 morph 驱动(顶点会形变)。
 * o       : 命令行选项(morph 锁定阈值/误差等)。
 *
 * 行业惯例分级(对应 meshopt / Simplygon 的属性/拓扑约束)：
 *   - 蒙皮/morph 网格：绝不用 sloppy(不保拓扑/属性 → 形变撕裂)，收紧误差，
 *     法线(+UV)作为属性纳入误差度量；并对"形变活跃"顶点用 vertex_lock 锁定，
 *     保住 blendshape/骨骼形变的关键区域(如嘴唇)，不受属性槽数量限制。
 *   - 带纹理静态网格：simplifyWithAttributes 保 UV，误差受控，不 sloppy。
 *   - 纯色静态道具：可激进，先保拓扑简化，不足再 sloppy 强制压到预算。 */
static void g2b_simplify_prim(wprim_t *wp, uint32_t target_tris, int has_tex,
                              int is_anim, const opts_t *o, float bbox_diag) {
    if (wp->icount < 3) return;
    uint32_t tri = wp->icount / 3;

    /* uint16 → uint32 索引（meshopt 用 32 位）*/
    unsigned int *i32 = (unsigned int*)malloc(wp->icount*sizeof(unsigned int));
    for (uint32_t i=0;i<wp->icount;i++) i32[i]=wp->idx[i];

    if (target_tris>0 && tri>target_tris) {
        size_t target_idx = (size_t)target_tris*3;
        unsigned int *out = (unsigned int*)malloc(wp->icount*sizeof(unsigned int));
        float err=0.0f;
        if (is_anim) {
            /* 蒙皮/morph：保守减面。关键在于保护"在动画下显著运动"的顶点不被
               无差别合并(如嘴唇：基础姿势上下唇重合，一播口型就糊在一起)。

               策略 = 属性度量 + 顶点锁定双保险：
                 1) 属性：UV(有纹理)+法线+骨骼权重 纳入误差，保曲率/拓扑/骨骼边界。
                 2) 顶点锁定：逐顶点取其在所有 morph target 下的最大位移 activity，
                    activity > ratio×包围盒 的顶点标记 Lock 禁止移动。这覆盖全部
                    blendshape(不受 meshopt 32 属性槽限制)，解决"少数槽装不下 52
                    套口型"导致的嘴部合并问题。锁定占比设上限，防过锁减不动。
               绝不 sloppy。*/
            const int has_uv = has_tex ? 1 : 0;
            const int n_uv   = has_uv ? 2 : 0;
            const int n_nrm  = 3;
            const int n_skin = wp->joints ? 4 : 0;
            int attr_n = n_uv + n_nrm + n_skin;
            float *attrs = (float*)malloc((size_t)wp->vcount*attr_n*sizeof(float));
            float *aw    = (float*)malloc((size_t)attr_n*sizeof(float));
            int c=0;
            for (int i=0;i<n_uv;i++)  aw[c++]=1.0f;
            for (int i=0;i<n_nrm;i++) aw[c++]=0.5f;
            for (int i=0;i<n_skin;i++)aw[c++]=2.0f;
            for (uint32_t v=0;v<wp->vcount;v++){
                float *a=&attrs[(size_t)v*attr_n]; int j=0;
                if (has_uv){ a[j++]=wp->verts[v].u; a[j++]=wp->verts[v].v; }
                a[j++]=wp->verts[v].nx; a[j++]=wp->verts[v].ny; a[j++]=wp->verts[v].nz;
                for (int s=0;s<n_skin;s++) a[j++]=wp->weights[v*4+s]/255.0f;
            }

            /* 顶点锁定：morph 活跃度超阈值则锁。 */
            unsigned char *lock = NULL;
            if (wp->morph_count>0 && o && o->morph_lock_ratio>0.0f && bbox_diag>1e-6f) {
                float thr = o->morph_lock_ratio * bbox_diag;
                float thr2 = thr*thr;
                float *act = (float*)malloc(sizeof(float)*wp->vcount); /* activity^2 */
                for (uint32_t v=0;v<wp->vcount;v++){
                    float mx=0.0f;
                    for (uint32_t t=0;t<wp->morph_count;t++){
                        const float *dd=&wp->morph_delta[((size_t)t*wp->vcount+v)*3];
                        float m=dd[0]*dd[0]+dd[1]*dd[1]+dd[2]*dd[2];
                        if (m>mx) mx=m;
                    }
                    act[v]=mx;
                }
                lock = (unsigned char*)calloc(wp->vcount,1);
                uint32_t nlock=0;
                for (uint32_t v=0;v<wp->vcount;v++)
                    if (act[v]>thr2){ lock[v]=meshopt_SimplifyVertex_Lock; nlock++; }
                /* 过锁保护：超过上限则只锁活跃度最高的那部分 */
                uint32_t cap = (uint32_t)(o->morph_lock_max_pct * wp->vcount);
                if (cap>0 && nlock>cap){
                    /* 找第 cap 大的活跃度作动态阈值(简单选择：排序拷贝) */
                    float *tmp=(float*)malloc(sizeof(float)*wp->vcount);
                    memcpy(tmp,act,sizeof(float)*wp->vcount);
                    /* 部分排序：nth_element 替代，用简单快速选择 */
                    /* 退化为 qsort 降序后取阈值(顶点数不大) */
                    for(uint32_t a2=0;a2<wp->vcount;a2++)
                        for(uint32_t b2=a2+1;b2<wp->vcount;b2++)
                            if(tmp[b2]>tmp[a2]){ float s=tmp[a2];tmp[a2]=tmp[b2];tmp[b2]=s; }
                    float dyn_thr=tmp[cap-1];
                    memset(lock,0,wp->vcount);
                    nlock=0;
                    for (uint32_t v=0;v<wp->vcount;v++)
                        if (act[v]>=dyn_thr && nlock<cap){ lock[v]=meshopt_SimplifyVertex_Lock; nlock++; }
                    free(tmp);
                }
                free(act);
                fprintf(stderr,"  morph 锁定顶点 %u/%u (%.1f%%, ratio=%.4f)\n",
                        nlock, wp->vcount, 100.0f*nlock/wp->vcount, o->morph_lock_ratio);
            }

            float aerr = (o && o->anim_error>0.0f) ? o->anim_error : 0.01f;
            size_t got = meshopt_simplifyWithAttributes(
                out, i32, wp->icount,
                &wp->verts[0].px, wp->vcount, sizeof(wvert_t),
                attrs, (size_t)attr_n*sizeof(float),
                aw, (size_t)attr_n, lock,
                target_idx, aerr, 0, &err);
            free(attrs); free(aw); free(lock);
            free(i32); i32 = out; wp->icount = (uint32_t)got;
        } else if (has_tex) {
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

/* prim 包围盒对角线长度(屏幕尺寸/预算加权用) */
static float g2b_prim_diag(const wprim_t *wp){
    if (wp->vcount==0) return 0.0f;
    float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
    for (uint32_t i=0;i<wp->vcount;i++){
        const float p[3]={wp->verts[i].px,wp->verts[i].py,wp->verts[i].pz};
        for(int k=0;k<3;k++){ if(p[k]<mn[k])mn[k]=p[k]; if(p[k]>mx[k])mx[k]=p[k]; }
    }
    float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

/* 全场景按"三角形数 × 包围盒尺寸"分配预算减面。
 * 纯按三角形数分配会让又碎又小的零件(螺丝/logo 背板)占用过多预算；
 * 行业做法按屏幕覆盖(≈投影面积≈尺寸)加权，大件保面、小件砍狠。 */
static void g2b_simplify(g2b_scene_t *sc, uint32_t max_tris, const opts_t *o) {
    if (max_tris==0 || sc->total_tris<=max_tris) {
        /* 仍做 cache 优化 */
        for (uint32_t i=0;i<sc->prim_count;i++){
            wprim_t *p=&sc->prims[i];
            int keepuv = (p->tex_id>=0) && !(p->mat_flags & 4); /* matcap(bit2) UV 运行时算，不需保 */
            int is_anim = (p->joints!=NULL) || (p->morph_count>0);
            g2b_simplify_prim(p, 0, keepuv, is_anim, o, g2b_prim_diag(p));
        }
        return;
    }
    uint32_t total = sc->total_tris;

    /* 先算每个 prim 的权重 = 三角形数 × 包围盒对角线。蒙皮/morph prim 额外
       上调权重(×1.5)，避免动画网格被过度削减导致形变质量下降。 */
    double *w = (double*)malloc(sizeof(double)*(sc->prim_count?sc->prim_count:1));
    double wsum = 0.0;
    for (uint32_t i=0;i<sc->prim_count;i++){
        wprim_t *wp=&sc->prims[i];
        uint32_t tri = wp->icount/3;
        float diag = g2b_prim_diag(wp);
        if (diag < 1e-6f) diag = 1e-6f;
        double wi = (double)tri * (double)diag;
        if ((wp->joints!=NULL) || (wp->morph_count>0)) wi *= 1.5;
        w[i] = wi; wsum += wi;
    }
    if (wsum < 1e-12) wsum = 1.0;

    uint32_t new_total = 0;
    for (uint32_t i=0;i<sc->prim_count;i++) {
        wprim_t *wp=&sc->prims[i];
        uint32_t tri = wp->icount/3;
        uint32_t budget = (uint32_t)((double)max_tris * (w[i]/wsum));
        if (budget<1) budget=1;
        if (budget>tri) budget=tri;   /* 不为小件分配超过自身的预算 */
        int keepuv = (wp->tex_id>=0) && !(wp->mat_flags & 4); /* matcap UV 运行时算 */
        int is_anim = (wp->joints!=NULL) || (wp->morph_count>0);
        g2b_simplify_prim(wp, budget, keepuv, is_anim, o, g2b_prim_diag(wp));
        new_total += wp->icount/3;
    }
    free(w);
    sc->total_tris = new_total;
    fprintf(stderr,"减面: %u → %u 三角形 (预算 %u，按尺寸加权)\n", total, new_total, max_tris);
}
#endif
