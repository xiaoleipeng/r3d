/* g2b_extract.h — 从 cgltf 数据提取 primitives 与纹理（被 main.c include）*/
#ifndef G2B_EXTRACT_H
#define G2B_EXTRACT_H

typedef struct {
    int      w, h;
    uint8_t *rgba;   /* 解码+降采样后 ARGB8888(实际 RGBA 顺序，写出时处理) */
} g2b_tex_t;

typedef struct {
    wprim_t  *prims;     uint32_t prim_count;
    g2b_tex_t*texs;      uint32_t tex_count;
    uint32_t  total_tris;
    float     bs[4];     /* bounding sphere cx,cy,cz,r */
    wclip_t  *clips;     uint32_t clip_count;  /* 动画 */
    /* skin */
    void     *nodes;     uint32_t node_count;  /* r3d_b3dm_node_t[] */
    uint16_t *joint_nodes; uint32_t joint_count;
    float    *inv_bind;  /* [joint_count*16] */
} g2b_scene_t;

/* 找 image 在 data->images 的索引 */
static int g2b_image_index(cgltf_data *d, cgltf_image *img) {
    for (size_t i = 0; i < d->images_count; i++)
        if (&d->images[i] == img) return (int)i;
    return -1;
}

/* 解码并降采样一张纹理 */
static int g2b_load_tex(cgltf_data *d, cgltf_image *img, const char *gltf_path,
                        int max_size, g2b_tex_t *out) {
    int w=0,h=0,n=0;
    uint8_t *pix = NULL;

    if (img->buffer_view) {  /* 内嵌 */
        const uint8_t *p = (const uint8_t*)img->buffer_view->buffer->data
                         + img->buffer_view->offset;
        pix = stbi_load_from_memory(p, (int)img->buffer_view->size, &w,&h,&n, 4);
    } else if (img->uri) {   /* 外部文件：相对 gltf 路径 */
        char dir[1024]; snprintf(dir, sizeof dir, "%s", gltf_path);
        char *slash = strrchr(dir, '/'); if (slash) slash[1]=0; else dir[0]=0;
        char full[2048]; snprintf(full, sizeof full, "%s%s", dir, img->uri);
        pix = stbi_load(full, &w,&h,&n, 4);
    }
    if (!pix) return -1;

    int tw = w, th = h;
    if (tw > max_size || th > max_size) {
        float s = (float)max_size / (tw > th ? tw : th);
        tw = (int)(tw*s); th = (int)(th*s);
        if (tw<1)tw=1; if(th<1)th=1;
        uint8_t *rs = (uint8_t*)malloc((size_t)tw*th*4);
        stbir_resize_uint8_linear(pix, w,h,0, rs, tw,th,0, STBIR_RGBA);
        stbi_image_free(pix);
        out->rgba = rs;
    } else {
        out->rgba = pix; /* 复用，free 用 free()——见下 */
        /* 统一用 malloc 释放：把 stbi 的拷到 malloc */
        size_t sz=(size_t)w*h*4; uint8_t*c=(uint8_t*)malloc(sz);
        memcpy(c,pix,sz); stbi_image_free(pix); out->rgba=c;
    }
    out->w = tw; out->h = th;
    return 0;
}

/* 选变体材质：若指定 variant 名子串且 prim 有 mappings，返回匹配变体的 material */
static cgltf_material* g2b_pick_material(cgltf_data*d, cgltf_primitive*pr, const char*variant){
    if(variant && pr->mappings_count){
        for(size_t i=0;i<pr->mappings_count;i++){
            cgltf_size vi=pr->mappings[i].variant;
            if(vi<d->variants_count && d->variants[vi].name && strstr(d->variants[vi].name,variant))
                return pr->mappings[i].material;
        }
    }
    return pr->material;
}

/* 加载外部图片文件 → g2b_tex_t（降采样到 max_size）*/
static int g2b_load_file(const char*path,int max_size,g2b_tex_t*out){
    int bw,bh,bn; unsigned char*bp=stbi_load(path,&bw,&bh,&bn,4);
    if(!bp) return -1;
    int tw=bw,th=bh;
    if(tw>max_size||th>max_size){float s=(float)max_size/(tw>th?tw:th);tw=(int)(tw*s);th=(int)(th*s);if(tw<1)tw=1;if(th<1)th=1;}
    out->w=tw;out->h=th;out->rgba=(unsigned char*)malloc((size_t)tw*th*4);
    if(tw!=bw||th!=bh) stbir_resize_uint8_linear(bp,bw,bh,0,out->rgba,tw,th,0,STBIR_RGBA);
    else memcpy(out->rgba,bp,(size_t)tw*th*4);
    stbi_image_free(bp);
    return 0;
}

/* 把 baseColor 贴图按 UV 范围[u0,u1]×[v0,v1]重采样烘进新纹理(支持平铺，
   wrap 用 REPEAT 取模)。用于 KHR_texture_transform：变换后 UV 超出[0,1]时，
   离线烘焙 + UV 归一化，CLAMP_TO_EDGE 平台也能正确显示。*/
static int g2b_bake_uv_region(cgltf_data *d, cgltf_image *img, const char *gltf_path,
                              int max_size, float u0,float u1,float v0,float v1,
                              int wrap_repeat, g2b_tex_t *out){
    (void)d;
    int w=0,h=0,n=0; unsigned char *pix=NULL;
    if (img->buffer_view){
        const unsigned char *p=(const unsigned char*)img->buffer_view->buffer->data+img->buffer_view->offset;
        pix=stbi_load_from_memory(p,(int)img->buffer_view->size,&w,&h,&n,4);
    } else if (img->uri){
        char dir[1024]; snprintf(dir,sizeof dir,"%s",gltf_path);
        char *sl=strrchr(dir,'/'); if(sl)sl[1]=0; else dir[0]=0;
        char full[2048]; snprintf(full,sizeof full,"%s%s",dir,img->uri);
        pix=stbi_load(full,&w,&h,&n,4);
    }
    if(!pix) return -1;
    int tw=w,th=h;
    if(tw>max_size||th>max_size){ float s=(float)max_size/(tw>th?tw:th); tw=(int)(tw*s);th=(int)(th*s); if(tw<1)tw=1;if(th<1)th=1; }
    out->w=tw; out->h=th; out->rgba=(uint8_t*)malloc((size_t)tw*th*4);
    for(int y=0;y<th;y++)for(int x=0;x<tw;x++){
        /* 输出[0,1] 映射回变换后 UV 区间，按 sampler wrap 模式采样源图 */
        float fu=u0+(u1-u0)*((x+0.5f)/tw);
        float fv=v0+(v1-v0)*((y+0.5f)/th);
        if(wrap_repeat){ fu-=floorf(fu); fv-=floorf(fv); }       /* REPEAT：平铺 */
        else { if(fu<0)fu=0; if(fu>1)fu=1; if(fv<0)fv=0; if(fv>1)fv=1; } /* CLAMP：夹到边缘(logo 只出现一次) */
        int sx=(int)(fu*w); if(sx>=w)sx=w-1; if(sx<0)sx=0;
        int sy=(int)(fv*h); if(sy>=h)sy=h-1; if(sy<0)sy=0;
        const unsigned char *c=&pix[(sy*w+sx)*4];
        uint8_t *o2=&out->rgba[(y*tw+x)*4];
        o2[0]=c[0];o2[1]=c[1];o2[2]=c[2];o2[3]=c[3];
    }
    stbi_image_free(pix);
    return 0;
}

/* 把 baseColor + metallicRoughness 贴图烘成单张近似 PBR 颜色纹理(无 shader 平台)。
   MR: G=粗糙度, B=金属度。金属+低粗糙 → 提亮 + 加镜面高光感；粗糙/非金属 → 哑光。
   两张图尺寸不同时各自按比例采样。*/
static int g2b_bake_mr(cgltf_data *d, cgltf_image *bimg, cgltf_image *mrimg,
                       const char *gltf_path, int max_size, g2b_tex_t *out){
    (void)d;
    int bw,bh,bn,mw,mh,mn; unsigned char *bp=NULL,*mp=NULL;
    /* baseColor */
    if (bimg->buffer_view){
        const unsigned char *p=(const unsigned char*)bimg->buffer_view->buffer->data+bimg->buffer_view->offset;
        bp=stbi_load_from_memory(p,(int)bimg->buffer_view->size,&bw,&bh,&bn,4);
    } else if (bimg->uri){
        char dir[1024]; snprintf(dir,sizeof dir,"%s",gltf_path);
        char *sl=strrchr(dir,'/'); if(sl)sl[1]=0; else dir[0]=0;
        char full[2048]; snprintf(full,sizeof full,"%s%s",dir,bimg->uri); bp=stbi_load(full,&bw,&bh,&bn,4);
    }
    if(!bp) return -1;
    /* MR */
    if (mrimg->buffer_view){
        const unsigned char *p=(const unsigned char*)mrimg->buffer_view->buffer->data+mrimg->buffer_view->offset;
        mp=stbi_load_from_memory(p,(int)mrimg->buffer_view->size,&mw,&mh,&mn,4);
    } else if (mrimg->uri){
        char dir[1024]; snprintf(dir,sizeof dir,"%s",gltf_path);
        char *sl=strrchr(dir,'/'); if(sl)sl[1]=0; else dir[0]=0;
        char full[2048]; snprintf(full,sizeof full,"%s%s",dir,mrimg->uri); mp=stbi_load(full,&mw,&mh,&mn,4);
    }
    if(!mp){ stbi_image_free(bp); return -1; }
    int tw=bw,th=bh;
    if(tw>max_size||th>max_size){ float s=(float)max_size/(tw>th?tw:th); tw=(int)(tw*s);th=(int)(th*s); if(tw<1)tw=1;if(th<1)th=1; }
    out->w=tw; out->h=th; out->rgba=(uint8_t*)malloc((size_t)tw*th*4);
    for(int y=0;y<th;y++)for(int x=0;x<tw;x++){
        float fu=(x+0.5f)/tw, fv=(y+0.5f)/th;
        int bx=(int)(fu*bw); if(bx>=bw)bx=bw-1;
        int by=(int)(fv*bh); if(by>=bh)by=bh-1;
        int mx=(int)(fu*mw); if(mx>=mw)mx=mw-1;
        int my=(int)(fv*mh); if(my>=mh)my=mh-1;
        const unsigned char *bc=&bp[(by*bw+bx)*4];
        const unsigned char *mc=&mp[(my*mw+mx)*4];
        float rough=mc[1]/255.0f, metal=mc[2]/255.0f;
        float r=bc[0]/255.0f,g=bc[1]/255.0f,b=bc[2]/255.0f;
        /* 金属低粗糙：提亮(镜面感)；高粗糙/非金属：略压暗(哑光) */
        float gloss=1.0f-rough;
        float spec=metal*gloss;          /* 镜面强度 0..1 */
        float shade=0.82f + 0.30f*spec - 0.12f*rough*(1.0f-metal);
        /* 金属高光偏白 */
        float wmix=spec*0.35f;
        r=r*shade+wmix; g=g*shade+wmix; b=b*shade+wmix;
        int R=(int)(r*255+0.5f),G=(int)(g*255+0.5f),B=(int)(b*255+0.5f);
        if(R>255)R=255;if(G>255)G=255;if(B>255)B=255;
        if(R<0)R=0;if(G<0)G=0;if(B<0)B=0;
        uint8_t *o2=&out->rgba[(y*tw+x)*4];
        o2[0]=(uint8_t)R;o2[1]=(uint8_t)G;o2[2]=(uint8_t)B;o2[3]=bc[3];
    }
    stbi_image_free(bp); stbi_image_free(mp);
    return 0;
}

/* 法线贴图 + baseColor → 烘焙明暗颜色纹理（固定光照，无 shader 也能显示条纹）*/
static int g2b_bake_normal(cgltf_data *d, cgltf_image *nimg, const char *gltf_path,
                           int max_size, uint32_t bcf, int tu, int tv, g2b_tex_t *out) {
    int w=0,h=0,n=0; unsigned char *pix=NULL;
    if (nimg->buffer_view) {
        const unsigned char *p=(const unsigned char*)nimg->buffer_view->buffer->data+nimg->buffer_view->offset;
        pix=stbi_load_from_memory(p,(int)nimg->buffer_view->size,&w,&h,&n,3);
    } else if (nimg->uri) {
        char dir[1024]; snprintf(dir,sizeof dir,"%s",gltf_path);
        char *sl=strrchr(dir,'/'); if(sl)sl[1]=0; else dir[0]=0;
        char full[2048]; snprintf(full,sizeof full,"%s%s",dir,nimg->uri);
        pix=stbi_load(full,&w,&h,&n,3);
    }
    if(!pix) return -1;
    int tw=w,th=h;
    if(tw>max_size||th>max_size){ float s=(float)max_size/(tw>th?tw:th); tw=(int)(tw*s);th=(int)(th*s); if(tw<1)tw=1;if(th<1)th=1; }
    out->w=tw; out->h=th; out->rgba=(uint8_t*)malloc((size_t)tw*th*4);
    /* baseColor 分量 */
    float br=((bcf>>16)&0xFF)/255.0f, bg=((bcf>>8)&0xFF)/255.0f, bb=(bcf&0xFF)/255.0f;
    /* 固定光方向 + 视线（切线空间）。金属编织靠强方向性高光区分横/竖纤维块 */
    float lx=0.35f,ly=0.45f,lz=0.82f;
    { float l=sqrtf(lx*lx+ly*ly+lz*lz); lx/=l;ly/=l;lz/=l; }
    float vx=0.0f,vy=0.0f,vz=1.0f;
    float hx=lx+vx,hy=ly+vy,hz=lz+vz;
    { float l=sqrtf(hx*hx+hy*hy+hz*hz); hx/=l;hy/=l;hz/=l; }
    for(int y=0;y<th;y++)for(int x=0;x<tw;x++){
        /* 平铺：输出 [0,1] 映射到 normal 的 tu×tv 次重复 */
        int sx=(int)((long)x*tu*w/tw)%w; if(sx<0)sx+=w;
        int sy=(int)((long)y*tv*h/th)%h; if(sy<0)sy+=h;
        const unsigned char *c=&pix[(sy*w+sx)*3];
        float nx=c[0]/127.5f-1.0f, ny=c[1]/127.5f-1.0f, nz=c[2]/127.5f-1.0f;
        float l=sqrtf(nx*nx+ny*ny+nz*nz); if(l>1e-6f){nx/=l;ny/=l;nz/=l;}
        float dif=nx*lx+ny*ly+nz*lz; if(dif<0)dif=0;
        /* 方向性高光：横/竖纤维块法线不同 → 高光强弱不同，凸显编织光泽 */
        float ndh=nx*hx+ny*hy+nz*hz; if(ndh<0)ndh=0;
        float spec=ndh*ndh; spec*=spec; spec*=ndh;  /* pow(ndh,5)：稍宽柔和 */
        float sh=dif*0.50f+0.52f;       /* 漫反射明暗 + 环境 */
        uint8_t *o2=&out->rgba[(y*tw+x)*4];
        /* 金属高光染基色(暖金)而非纯白，光泽更接近金属拉丝；偏白量少许 */
        float hi=spec*0.6f;
        int R=(int)((br*(sh+hi)+hi*0.25f)*255);
        int G=(int)((bg*(sh+hi)+hi*0.25f)*255);
        int B=(int)((bb*(sh+hi)+hi*0.25f)*255);
        if(R>255)R=255;if(G>255)G=255;if(B>255)B=255;
        o2[0]=(uint8_t)R;o2[1]=(uint8_t)G;o2[2]=(uint8_t)B;o2[3]=255;
    }
    stbi_image_free(pix);
    return 0;
}

static const float* g2b_read_accessor_vec(cgltf_accessor *acc, size_t *count, int *comp) {
    /* 仅支持 float 访问；用 cgltf_accessor_read_float 逐元素更稳妥，这里返回需调用方处理 */
    *count = acc ? acc->count : 0;
    *comp  = acc ? (int)cgltf_num_components(acc->type) : 0;
    return NULL;
}

static int g2b_extract(cgltf_data *d, const char *gltf_path,
                       const opts_t *o, g2b_scene_t *sc) {
    /* 纹理：预分配 images + materials（含法线烘焙纹理）*/
    uint32_t texcap = (uint32_t)(d->images_count + d->materials_count + 1);
    sc->texs = (g2b_tex_t*)calloc(texcap, sizeof(g2b_tex_t));
    sc->tex_count = 0;
    int *img_remap = (int*)malloc(sizeof(int)*(d->images_count?d->images_count:1));
    for (size_t i=0;i<d->images_count;i++) {
        if (g2b_load_tex(d, &d->images[i], gltf_path, o->tex_size, &sc->texs[sc->tex_count])==0)
            img_remap[i] = (int)sc->tex_count++;
        else img_remap[i] = -1;
    }

    /* primitives */
    uint32_t cap = 16; sc->prims = (wprim_t*)calloc(cap, sizeof(wprim_t));
    float bmin[3]={1e30f,1e30f,1e30f}, bmax[3]={-1e30f,-1e30f,-1e30f};

    /* 动态 node 集合：被 T/R/S 动画 channel 驱动的 node 及其子孙(这些不烘焙节点
     * 变换，留给运行时蒙皮/节点动画)。注意：仅被 weights(morph 权重)驱动的 node
     * 其 TRS 仍是静态的，应照常烘焙节点变换，否则量化顶点会停留在错误尺寸/位置。 */
    unsigned char *dyn=(unsigned char*)calloc(d->nodes_count?d->nodes_count:1,1);
    int matcap_tid=-1;  /* 共享 matcap 纹理 id(去重) */
    for(size_t ai=0;ai<d->animations_count;ai++)
        for(size_t ci=0;ci<d->animations[ai].channels_count;ci++){
            /* weights 通道不让 node 变"几何动态" */
            if(d->animations[ai].channels[ci].target_path==cgltf_animation_path_type_weights)
                continue;
            cgltf_node *tn=d->animations[ai].channels[ci].target_node;
            int ti=g2b_node_index(d,tn);
            if(ti>=0 && ti<(int)d->nodes_count){
                /* 标记该 node 及所有子孙 */
                /* 简化：用多轮传播(node 少) */
                dyn[ti]=1;
            }
        }
    /* 子孙传播：parent 动态则子动态(多轮直到稳定) */
    for(int iter=0;iter<(int)d->nodes_count;iter++){
        int changed=0;
        for(size_t i=0;i<d->nodes_count;i++){
            if(dyn[i] && d->nodes[i].children)
                for(size_t c=0;c<d->nodes[i].children_count;c++){
                    int ci=g2b_node_index(d,d->nodes[i].children[c]);
                    if(ci>=0 && !dyn[ci]){ dyn[ci]=1; changed=1; }
                }
        }
        if(!changed) break;
    }

    for (size_t ni=0; ni<d->nodes_count; ni++) {
        cgltf_node *nd=&d->nodes[ni];
        if (!nd->mesh) continue;
        cgltf_mesh *mesh=nd->mesh;
        /* node 世界矩阵（列主序）；蒙皮 mesh 或动态 node 不 bake */
        float wm[16]; cgltf_node_transform_world(nd, wm);
        int is_dyn = dyn[ni];
        int skinned = (nd->skin!=NULL) || is_dyn;  /* 不 bake 顶点 */
        for (size_t pi=0; pi<mesh->primitives_count; pi++) {
            cgltf_primitive *pr=&mesh->primitives[pi];
            if (pr->type != cgltf_primitive_type_triangles) continue;

            cgltf_accessor *apos=NULL,*auv=NULL,*anrm=NULL,*ajoint=NULL,*aweight=NULL;
            for (size_t a=0;a<pr->attributes_count;a++){
                cgltf_attribute *at=&pr->attributes[a];
                if (at->type==cgltf_attribute_type_position) apos=at->data;
                else if (at->type==cgltf_attribute_type_texcoord && !auv) auv=at->data;
                else if (at->type==cgltf_attribute_type_normal) anrm=at->data;
                else if (at->type==cgltf_attribute_type_joints && !ajoint) ajoint=at->data;
                else if (at->type==cgltf_attribute_type_weights && !aweight) aweight=at->data;
            }
            if (!apos) continue;

            /* Draco 压缩 prim：cgltf 不解码，accessor 指向压缩字节(读出是垃圾)。
               用 Draco 桥接解出 float 顶点属性 + 索引，覆盖后续读取。 */
            g2b_draco_mesh_t dm; memset(&dm,0,sizeof dm);
            int draco_ok=0;
            if (pr->has_draco_mesh_compression){
                cgltf_draco_mesh_compression *dc=&pr->draco_mesh_compression;
                int pos_id=-1,nrm_id=-1,uv_id=-1;
                for(size_t a=0;a<dc->attributes_count;a++){
                    /* draco unique_id 被 cgltf 存为 attributes[a].data 的 accessor 下标 */
                    int id=(int)(dc->attributes[a].data - d->accessors);
                    switch(dc->attributes[a].type){
                        case cgltf_attribute_type_position: pos_id=id; break;
                        case cgltf_attribute_type_normal:   nrm_id=id; break;
                        case cgltf_attribute_type_texcoord: if(uv_id<0)uv_id=id; break;
                        default: break;
                    }
                }
                const uint8_t *cbuf=(const uint8_t*)cgltf_buffer_view_data(dc->buffer_view);
                if (cbuf && g2b_draco_decode(cbuf, dc->buffer_view->size,
                                             pos_id,nrm_id,uv_id,&dm)==0){
                    draco_ok=1;
                } else {
                    if(!g2b_draco_available())
                        fprintf(stderr,"跳过 Draco 压缩 prim(未编入 Draco 支持)\n");
                    else
                        fprintf(stderr,"Draco 解码失败，跳过该 prim\n");
                    sc->prim_count--; /* 回退本 prim */
                    continue;
                }
            }

            if (sc->prim_count==cap){cap*=2;sc->prims=(wprim_t*)realloc(sc->prims,cap*sizeof(wprim_t));}
            wprim_t *wp=&sc->prims[sc->prim_count++];
            memset(wp,0,sizeof(*wp));
            wp->node_id=(int)ni; wp->is_dynamic=is_dyn;
            wp->vcount = draco_ok ? dm.vertex_count : (uint32_t)apos->count;
            wp->verts=(wvert_t*)calloc(wp->vcount,sizeof(wvert_t));
            for (uint32_t i=0;i<wp->vcount;i++){
                float p[3]={0};
                if (draco_ok && dm.position){ p[0]=dm.position[i*3];p[1]=dm.position[i*3+1];p[2]=dm.position[i*3+2]; }
                else cgltf_accessor_read_float(apos,i,p,3);
                if (!skinned){
                    /* bake: p' = wm(列主序) × p */
                    float x=p[0],y=p[1],z=p[2];
                    p[0]=wm[0]*x+wm[4]*y+wm[8]*z+wm[12];
                    p[1]=wm[1]*x+wm[5]*y+wm[9]*z+wm[13];
                    p[2]=wm[2]*x+wm[6]*y+wm[10]*z+wm[14];
                }
                wp->verts[i].px=p[0]; wp->verts[i].py=p[1]; wp->verts[i].pz=p[2];
                for(int k=0;k<3;k++){ if(p[k]<bmin[k])bmin[k]=p[k]; if(p[k]>bmax[k])bmax[k]=p[k]; }
                if (draco_ok){
                    if (dm.texcoord){ wp->verts[i].u=dm.texcoord[i*2]; wp->verts[i].v=dm.texcoord[i*2+1]; }
                } else if (auv){ float t[2]={0}; cgltf_accessor_read_float(auv,i,t,2); wp->verts[i].u=t[0]; wp->verts[i].v=t[1]; }
                if (draco_ok ? (dm.normal!=NULL) : (anrm!=NULL)){
                    float nn[3]={0};
                    if (draco_ok){ nn[0]=dm.normal[i*3];nn[1]=dm.normal[i*3+1];nn[2]=dm.normal[i*3+2]; }
                    else cgltf_accessor_read_float(anrm,i,nn,3);
                    if (!skinned){
                        float x=nn[0],y=nn[1],z=nn[2];
                        nn[0]=wm[0]*x+wm[4]*y+wm[8]*z;
                        nn[1]=wm[1]*x+wm[5]*y+wm[9]*z;
                        nn[2]=wm[2]*x+wm[6]*y+wm[10]*z;
                        float l=sqrtf(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]);
                        if(l>1e-8f){nn[0]/=l;nn[1]/=l;nn[2]/=l;}
                    }
                    wp->verts[i].nx=nn[0]; wp->verts[i].ny=nn[1]; wp->verts[i].nz=nn[2];
                }
                else { wp->verts[i].nx=0.0f; wp->verts[i].ny=0.0f; wp->verts[i].nz=0.0f; }
            }
            /* 世界质心(框选区域减面用)：静态顶点已烘焙到世界(!skinned 时乘过 wm)，
             * 动态顶点为局部空间，需再乘 wm 得到世界坐标。 */
            {
                double cx=0,cy=0,cz=0;
                for (uint32_t i=0;i<wp->vcount;i++){
                    cx+=wp->verts[i].px; cy+=wp->verts[i].py; cz+=wp->verts[i].pz;
                }
                if (wp->vcount){ cx/=wp->vcount; cy/=wp->vcount; cz/=wp->vcount; }
                if (skinned){
                    float x=(float)cx,y=(float)cy,z=(float)cz;
                    wp->wcenter[0]=wm[0]*x+wm[4]*y+wm[8]*z+wm[12];
                    wp->wcenter[1]=wm[1]*x+wm[5]*y+wm[9]*z+wm[13];
                    wp->wcenter[2]=wm[2]*x+wm[6]*y+wm[10]*z+wm[14];
                } else {
                    wp->wcenter[0]=(float)cx; wp->wcenter[1]=(float)cy; wp->wcenter[2]=(float)cz;
                }
            }

            /* 源是否提供法线(否则下面按面计算 flat 法线) */
            int had_normals = (draco_ok ? (dm.normal!=NULL) : (anrm!=NULL));
            /* 索引 */
            if (draco_ok){
                wp->icount=dm.index_count;
                wp->idx=(uint16_t*)malloc(wp->icount*sizeof(uint16_t));
                for(uint32_t i=0;i<wp->icount;i++) wp->idx[i]=(uint16_t)dm.indices[i];
                g2b_draco_free(&dm);
            } else if (pr->indices){
                wp->icount=(uint32_t)pr->indices->count;
                wp->idx=(uint16_t*)malloc(wp->icount*sizeof(uint16_t));
                for(uint32_t i=0;i<wp->icount;i++) wp->idx[i]=(uint16_t)cgltf_accessor_read_index(pr->indices,i);
            } else {
                wp->icount=wp->vcount;
                wp->idx=(uint16_t*)malloc(wp->icount*sizeof(uint16_t));
                for(uint32_t i=0;i<wp->icount;i++) wp->idx[i]=(uint16_t)i;
            }
            sc->total_tris += wp->icount/3;

            /* 源无法线：按面计算 flat 法线(行业惯例兜底)。用三角形两边叉乘，
             * 累加到三个顶点后归一化。顶点位置此时已在与法线一致的空间(静态=世界、
             * 蒙皮=局部)，故法线空间正确。低模(如 Fox)由此恢复硬边分面着色，
             * 而非之前恒定 +Z 法线导致的平淡均匀着色。 */
            if (!had_normals && wp->icount>=3) {
                for(uint32_t i=0;i<wp->icount;i+=3){
                    uint32_t a=wp->idx[i], b=wp->idx[i+1], c=wp->idx[i+2];
                    if(a>=wp->vcount||b>=wp->vcount||c>=wp->vcount) continue;
                    float ux=wp->verts[b].px-wp->verts[a].px;
                    float uy=wp->verts[b].py-wp->verts[a].py;
                    float uz=wp->verts[b].pz-wp->verts[a].pz;
                    float vx=wp->verts[c].px-wp->verts[a].px;
                    float vy=wp->verts[c].py-wp->verts[a].py;
                    float vz=wp->verts[c].pz-wp->verts[a].pz;
                    float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
                    /* 累加(共享顶点时近似 area-weighted)；非索引网格则每面独立=精确 flat */
                    wp->verts[a].nx+=nx; wp->verts[a].ny+=ny; wp->verts[a].nz+=nz;
                    wp->verts[b].nx+=nx; wp->verts[b].ny+=ny; wp->verts[b].nz+=nz;
                    wp->verts[c].nx+=nx; wp->verts[c].ny+=ny; wp->verts[c].nz+=nz;
                }
                for(uint32_t i=0;i<wp->vcount;i++){
                    float l=sqrtf(wp->verts[i].nx*wp->verts[i].nx
                                + wp->verts[i].ny*wp->verts[i].ny
                                + wp->verts[i].nz*wp->verts[i].nz);
                    if(l>1e-8f){ wp->verts[i].nx/=l; wp->verts[i].ny/=l; wp->verts[i].nz/=l; }
                    else { wp->verts[i].nz=1.0f; } /* 退化三角形兜底 */
                }
            }

            /* skin 顶点属性: JOINTS_0(u8x4) + WEIGHTS_0(归一) */
            wp->joints=NULL; wp->weights=NULL;
            if(ajoint && aweight){
                wp->joints=(uint8_t*)calloc((size_t)wp->vcount*4,1);
                wp->weights=(uint8_t*)calloc((size_t)wp->vcount*4,1);
                for(uint32_t i=0;i<wp->vcount;i++){
                    cgltf_uint ji[4]={0,0,0,0}; cgltf_accessor_read_uint(ajoint,i,ji,4);
                    float wf[4]={0,0,0,0}; cgltf_accessor_read_float(aweight,i,wf,4);
                    for(int k=0;k<4;k++){
                        wp->joints[i*4+k]=(uint8_t)(ji[k]&0xFF);
                        int wq=(int)(wf[k]*255.0f+0.5f); if(wq>255)wq=255; if(wq<0)wq=0;
                        wp->weights[i*4+k]=(uint8_t)wq;
                    }
                }
            }

            /* morph targets: 提取每个 target 的 POSITION delta。
             * delta 是位置偏移，烘焙节点变换时需乘 wm 的线性部分(旋转+缩放，
             * 不含平移)，与顶点保持同一空间，否则 morph 形变尺寸/方向错乱。 */
            wp->morph_count=0; wp->morph_delta=NULL;
            if (pr->targets_count>0){
                wp->morph_count=(uint32_t)pr->targets_count;
                wp->morph_delta=(float*)calloc((size_t)wp->morph_count*wp->vcount*3,sizeof(float));
                for(uint32_t t=0;t<wp->morph_count;t++){
                    cgltf_accessor *pa=NULL;
                    for(size_t a=0;a<pr->targets[t].attributes_count;a++)
                        if(pr->targets[t].attributes[a].type==cgltf_attribute_type_position)
                            pa=pr->targets[t].attributes[a].data;
                    if(!pa) continue;
                    for(uint32_t i=0;i<wp->vcount;i++){
                        float dd[3]={0}; cgltf_accessor_read_float(pa,i,dd,3);
                        if(!skinned){
                            float x=dd[0],y=dd[1],z=dd[2];
                            dd[0]=wm[0]*x+wm[4]*y+wm[8]*z;   /* 线性部分，无平移 */
                            dd[1]=wm[1]*x+wm[5]*y+wm[9]*z;
                            dd[2]=wm[2]*x+wm[6]*y+wm[10]*z;
                        }
                        float *dst=&wp->morph_delta[((size_t)t*wp->vcount+i)*3];
                        dst[0]=dd[0];dst[1]=dd[1];dst[2]=dd[2];
                    }
                }
            }

            /* 材质 → 纹理/blend/flags/颜色 */
            wp->tex_id=-1; wp->blend=0; wp->mat_flags=0; wp->base_color_factor=0xFFFFFFFFu;
            int uv_normalized=0;  /* UV 被烘焙归一化后，occlusion 不能再用同一 UV 采样 */
            cgltf_material *m = g2b_pick_material(d, pr, o->variant);
            if (m){
                if (m->double_sided) wp->mat_flags|=1; /* R3D_MAT_DOUBLE_SIDED */
                int a=255;
                if (m->has_pbr_metallic_roughness){
                    cgltf_texture *t=m->pbr_metallic_roughness.base_color_texture.texture;
                    int tbaked=0;
                    if (t && t->image){ int gi=g2b_image_index(d,t->image); if(gi>=0&&gi<(int)d->images_count) wp->tex_id=img_remap[gi]; }
                    /* KHR_texture_transform: 变换后 UV 超 [0,1] → 离线烘焙变换区域 + UV 归一化
                       (CLAMP_TO_EDGE / VGLite 无 REPEAT 也能正确显示 logo 平铺/定位) */
                    if (t && t->image && m->pbr_metallic_roughness.base_color_texture.has_transform){
                        const cgltf_texture_transform *tt=&m->pbr_metallic_roughness.base_color_texture.transform;
                        float sx=tt->scale[0], sy=tt->scale[1], ox=tt->offset[0], oy=tt->offset[1];
                        /* 该 prim 顶点 UV 经 transform 后的范围 */
                        float u0=1e30f,u1=-1e30f,v0=1e30f,v1=-1e30f;
                        for(uint32_t i=0;i<wp->vcount;i++){
                            float u=wp->verts[i].u*sx+ox, v=wp->verts[i].v*sy+oy;
                            if(u<u0)u0=u;if(u>u1)u1=u;if(v<v0)v0=v;if(v>v1)v1=v;
                        }
                        if(u1>u0 && v1>v0 &&
                           g2b_bake_uv_region(d,t->image,gltf_path,o->detail_tex_size,u0,u1,v0,v1,
                                              (t->sampler && t->sampler->wrap_s==10497),
                                              &sc->texs[sc->tex_count])==0){
                            wp->tex_id=(int)sc->tex_count++;
                            float ur=u1-u0,vr=v1-v0;
                            for(uint32_t i=0;i<wp->vcount;i++){
                                float u=wp->verts[i].u*sx+ox, v=wp->verts[i].v*sy+oy;
                                wp->verts[i].u=(u-u0)/ur;
                                wp->verts[i].v=(v-v0)/vr;
                            }
                            tbaked=1; uv_normalized=1;
                        }
                    }
                    /* metallicRoughness 烘焙：有 baseColor+MR 贴图且未经 transform 烘焙
                       → 把逐像素金属/粗糙烘进 baseColor(金属光滑提亮，粗糙哑光) */
                    if (!tbaked && t && t->image
                        && m->pbr_metallic_roughness.metallic_roughness_texture.texture
                        && m->pbr_metallic_roughness.metallic_roughness_texture.texture->image){
                        if (g2b_bake_mr(d, t->image,
                                        m->pbr_metallic_roughness.metallic_roughness_texture.texture->image,
                                        gltf_path, o->detail_tex_size, &sc->texs[sc->tex_count])==0){
                            wp->tex_id=(int)sc->tex_count++;
                        }
                    }
                    const cgltf_float *bf=m->pbr_metallic_roughness.base_color_factor;
                    a=(int)(bf[3]*255+0.5f);
                    int r=(int)(bf[0]*255+0.5f),g=(int)(bf[1]*255+0.5f),b=(int)(bf[2]*255+0.5f);
                    if(a>255)a=255;if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;
                    /* 极暗纯色塑料(无纹理)抬下限：flat 光照是乘法，纯黑(0.03)无论怎么打光都死黑、
                       丢失形状。抬到 ~0.16 让明暗梯度可见(深灰塑料)，近似 IBL 下黑塑料的观感。*/
                    if (wp->tex_id<0){
                        int mx=r>g?(r>b?r:b):(g>b?g:b);
                        if (mx<40){ int fl=42; r+=fl;g+=fl;b+=fl; if(r>255)r=255;if(g>255)g=255;if(b>255)b=255; }
                    }
                    wp->base_color_factor=((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
                }
                /* 透明识别：alphaMode=BLEND / KHR_transmission / 材质名含 Glass */
                int translucent = (m->alpha_mode==cgltf_alpha_mode_blend) || m->has_transmission;
                if (m->name && (strstr(m->name,"Glass")||strstr(m->name,"glass"))) translucent=1;
                if (translucent){
                    wp->blend=0; /* SRC_OVER */
                    wp->mat_flags|=8; /* R3D_MAT_TRANSLUCENT */
                    /* 玻璃用较低 alpha，便于透出下方表盘 */
                    int ta = (m->has_transmission)? (int)((1.0f-m->transmission.transmission_factor)*255) : a;
                    if (ta>120) ta=60;  /* 强制足够透明 */
                    wp->base_color_factor = (wp->base_color_factor & 0x00FFFFFFu) | ((uint32_t)ta<<24);
                }
                /* 法线贴图烘焙：有 normal 但无 baseColor 贴图 → 平铺烘成明暗纹理(显示条纹) */
                if (wp->tex_id<0 && m->normal_texture.texture && m->normal_texture.texture->image){
                    /* 算该 prim 的 UV 范围(此时顶点 UV 还是原始平铺值) */
                    float umin=1e30f,umax=-1e30f,vmin=1e30f,vmax=-1e30f;
                    for(uint32_t i=0;i<wp->vcount;i++){
                        float u=wp->verts[i].u,v=wp->verts[i].v;
                        if(u<umin)umin=u;if(u>umax)umax=u;if(v<vmin)vmin=v;if(v>vmax)vmax=v;
                    }
                    float ur=umax-umin, vr=vmax-vmin;
                    int tu=(int)(ur+0.5f); if(tu<1)tu=1; if(tu>8)tu=8;
                    int tv=(int)(vr+0.5f); if(tv<1)tv=1; if(tv>8)tv=8;
                    if (g2b_bake_normal(d, m->normal_texture.texture->image, gltf_path,
                                        o->detail_tex_size, wp->base_color_factor, tu, tv,
                                        &sc->texs[sc->tex_count])==0){
                        wp->tex_id=(int)sc->tex_count++;
                        wp->base_color_factor=0xFFFFFFFFu;
                        /* 归一化该 prim 顶点 UV 到 [0,1]（平铺已烘进纹理）*/
                        for(uint32_t i=0;i<wp->vcount;i++){
                            wp->verts[i].u = (ur>1e-6f)?(wp->verts[i].u-umin)/ur:0.0f;
                            wp->verts[i].v = (vr>1e-6f)?(wp->verts[i].v-vmin)/vr:0.0f;
                        }
                        uv_normalized=1;  /* UV 已被改写，occlusion 不能再用此 UV 采样 */
                    }
                }
                /* 外挂按钮拉丝纹理：Button Metal 材质 + --button-tex */
                if (wp->tex_id<0 && o->button_tex && m->name && strstr(m->name,"Button Metal")){
                    int bw,bh,bn; unsigned char*bp=stbi_load(o->button_tex,&bw,&bh,&bn,4);
                    if(bp){
                        int tw=bw,th=bh;
                        if(tw>o->tex_size||th>o->tex_size){float s=(float)o->tex_size/(tw>th?tw:th);tw=(int)(tw*s);th=(int)(th*s);if(tw<1)tw=1;if(th<1)th=1;}
                        g2b_tex_t*ot=&sc->texs[sc->tex_count];
                        ot->w=tw;ot->h=th;ot->rgba=(unsigned char*)malloc((size_t)tw*th*4);
                        if(tw!=bw||th!=bh) stbir_resize_uint8_linear(bp,bw,bh,0,ot->rgba,tw,th,0,STBIR_RGBA);
                        else memcpy(ot->rgba,bp,(size_t)tw*th*4);
                        stbi_image_free(bp);
                        wp->tex_id=(int)sc->tex_count++;
                        wp->base_color_factor=0xFFFFFFFFu;
                    }
                }
                /* 金属材质 → matcap(银球 + baseColorFactor 染色)，所有金属共享一张 matcap */
                if (wp->tex_id<0 && o->matcap_silver
                    && m->has_pbr_metallic_roughness
                    && m->pbr_metallic_roughness.metallic_factor > 0.5f){
                    if (matcap_tid<0){  /* 首次加载，之后复用 */
                        if (g2b_load_file(o->matcap_silver, o->tex_size, &sc->texs[sc->tex_count])==0)
                            matcap_tid=(int)sc->tex_count++;
                    }
                    if (matcap_tid>=0){
                        wp->tex_id=matcap_tid;
                        wp->mat_flags |= 4;  /* R3D_MAT_USE_MATCAP */
                    }
                }
            }

            /* AO: 逐顶点采样 occlusion 贴图 R 通道（默认 1.0）*/
            for(uint32_t i=0;i<wp->vcount;i++) wp->verts[i].ao=1.0f;
            if(!uv_normalized && pr->material && pr->material->occlusion_texture.texture
               && pr->material->occlusion_texture.texture->image && auv){
                cgltf_image *oi=pr->material->occlusion_texture.texture->image;
                int ow=0,oh=0,on=0; unsigned char *opix=NULL;
                if(oi->buffer_view){
                    const unsigned char *p=(const unsigned char*)oi->buffer_view->buffer->data+oi->buffer_view->offset;
                    opix=stbi_load_from_memory(p,(int)oi->buffer_view->size,&ow,&oh,&on,1);
                } else if(oi->uri){
                    char dir[1024]; snprintf(dir,sizeof dir,"%s",gltf_path);
                    char *sl=strrchr(dir,'/'); if(sl)sl[1]=0; else dir[0]=0;
                    char full[2048]; snprintf(full,sizeof full,"%s%s",dir,oi->uri);
                    opix=stbi_load(full,&ow,&oh,&on,1);
                }
                if(opix){
                    for(uint32_t i=0;i<wp->vcount;i++){
                        float u=wp->verts[i].u, v=wp->verts[i].v;
                        u-=floorf(u); v-=floorf(v);
                        int px=(int)(u*(ow-1)+0.5f), py=(int)(v*(oh-1)+0.5f);
                        if(px<0)px=0;if(px>=ow)px=ow-1;if(py<0)py=0;if(py>=oh)py=oh-1;
                        wp->verts[i].ao = opix[py*ow+px]/255.0f;
                    }
                    stbi_image_free(opix);
                }
            }
        }
    }
    free(img_remap);
    free(dyn);

    /* bounding sphere（用 AABB 中心+半对角）*/
    if (sc->prim_count){
        float cx=(bmin[0]+bmax[0])*0.5f, cy=(bmin[1]+bmax[1])*0.5f, cz=(bmin[2]+bmax[2])*0.5f;
        float dx=bmax[0]-cx,dy=bmax[1]-cy,dz=bmax[2]-cz;
        sc->bs[0]=cx; sc->bs[1]=cy; sc->bs[2]=cz; sc->bs[3]=sqrtf(dx*dx+dy*dy+dz*dz);
        /* 暂存 AABB 供 write 定点化（用 bs 范围近似 scale/bias）*/
    }
    return sc->prim_count?0:-1;
}

static void g2b_scene_free(g2b_scene_t *sc){
    for(uint32_t i=0;i<sc->prim_count;i++){ free(sc->prims[i].verts); free(sc->prims[i].idx); free(sc->prims[i].morph_delta); free(sc->prims[i].joints); free(sc->prims[i].weights); free(sc->prims[i].vcolor); }
    free(sc->prims);
    for(uint32_t i=0;i<sc->tex_count;i++) free(sc->texs[i].rgba);
    free(sc->texs);
    if(sc->clips) g2b_free_anim(sc->clips, sc->clip_count);
    free(sc->nodes); free(sc->joint_nodes); free(sc->inv_bind);
}
#endif
