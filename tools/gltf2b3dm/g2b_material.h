/* g2b_material.h — 材质/纹理输出模式处理（被 main.c include，在 simplify 之后调用）
 *
 * 行业惯例的"去纹理"材质降级谱系：
 *   full          保留纹理烘焙（不处理）
 *   baked-vertex  按 UV 把纹理采样到逐顶点色，丢弃贴图（去纹理主方案，省纹理内存）
 *   solid         每 prim 用其贴图的平均色作单色，丢弃贴图
 *   none          统一中灰，丢弃贴图（调试/线框）
 *
 * 放在减面之后执行：减面阶段仍把带纹理 prim 的 UV 作为属性保护，顶点色采样
 * 因此更准确；之后再丢弃纹理。 */
#ifndef G2B_MATERIAL_H
#define G2B_MATERIAL_H

/* 最近邻采样一张 g2b_tex_t（rgba 顺序），UV∈[0,1] → ARGB(a=255) */
static uint32_t g2b_tex_sample(const g2b_tex_t *t, float u, float v) {
    if (!t || !t->rgba || t->w<=0 || t->h<=0) return 0xFFFFFFFFu;
    if (u<0)u=0; if(u>1)u=1; if(v<0)v=0; if(v>1)v=1;
    int x=(int)(u*(t->w-1)+0.5f), y=(int)(v*(t->h-1)+0.5f);
    if(x<0)x=0; if(x>=t->w)x=t->w-1; if(y<0)y=0; if(y>=t->h)y=t->h-1;
    const uint8_t *p=&t->rgba[((size_t)y*t->w+x)*4];
    return (0xFFu<<24)|((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
}

/* 计算一张纹理的平均色（忽略全透明像素）→ ARGB(a=255) */
static uint32_t g2b_tex_average(const g2b_tex_t *t) {
    if (!t || !t->rgba || t->w<=0 || t->h<=0) return 0xFFFFFFFFu;
    uint64_t r=0,g=0,b=0,n=0; uint32_t total=(uint32_t)t->w*t->h;
    for(uint32_t i=0;i<total;i++){
        const uint8_t *p=&t->rgba[(size_t)i*4];
        if(p[3]<8) continue; /* 跳过透明 */
        r+=p[0]; g+=p[1]; b+=p[2]; n++;
    }
    if(!n) return 0xFFFFFFFFu;
    return (0xFFu<<24)|((uint32_t)(r/n)<<16)|((uint32_t)(g/n)<<8)|(uint32_t)(b/n);
}

/* 释放场景所有纹理并清零计数（去纹理模式用）*/
static void g2b_drop_textures(g2b_scene_t *sc) {
    for(uint32_t i=0;i<sc->tex_count;i++){ free(sc->texs[i].rgba); sc->texs[i].rgba=NULL; }
    sc->tex_count=0;
}

static void g2b_apply_material_mode(g2b_scene_t *sc, const opts_t *o) {
    if (o->mat_mode==G2B_MAT_FULL) return;

    if (o->mat_mode==G2B_MAT_NONE) {
        /* 统一中灰，丢弃纹理与顶点色 */
        for(uint32_t i=0;i<sc->prim_count;i++){
            wprim_t *wp=&sc->prims[i];
            wp->tex_id=-1; wp->mat_flags &= ~4u; /* 清 matcap */
            wp->base_color_factor=0xFFB0B0B0u;
            free(wp->vcolor); wp->vcolor=NULL;
        }
        g2b_drop_textures(sc);
        return;
    }

    if (o->mat_mode==G2B_MAT_SOLID) {
        /* 每 prim：有贴图取平均色，无贴图保留 base_color_factor(其颜色更真) */
        for(uint32_t i=0;i<sc->prim_count;i++){
            wprim_t *wp=&sc->prims[i];
            if(wp->tex_id>=0 && wp->tex_id<(int)sc->tex_count && !(wp->mat_flags&4))
                wp->base_color_factor=g2b_tex_average(&sc->texs[wp->tex_id]);
            wp->tex_id=-1; wp->mat_flags &= ~4u;
            free(wp->vcolor); wp->vcolor=NULL;
        }
        g2b_drop_textures(sc);
        return;
    }

    /* G2B_MAT_BAKED_VERTEX：按 UV 把贴图采样到逐顶点色，无贴图用 base_color_factor */
    for(uint32_t i=0;i<sc->prim_count;i++){
        wprim_t *wp=&sc->prims[i];
        wp->vcolor=(uint32_t*)malloc(sizeof(uint32_t)*(wp->vcount?wp->vcount:1));
        int textured = (wp->tex_id>=0 && wp->tex_id<(int)sc->tex_count && !(wp->mat_flags&4));
        const g2b_tex_t *t = textured ? &sc->texs[wp->tex_id] : NULL;
        for(uint32_t v=0;v<wp->vcount;v++){
            if(textured) wp->vcolor[v]=g2b_tex_sample(t, wp->verts[v].u, wp->verts[v].v);
            else         wp->vcolor[v]=wp->base_color_factor;
        }
        wp->tex_id=-1; wp->mat_flags &= ~4u;
        wp->base_color_factor=0xFFFFFFFFu; /* 颜色已进顶点色，base 设白避免二次调制 */
    }
    g2b_drop_textures(sc);
}
#endif
