/* g2b_write.h — 顶点定点化 + material 分组 + B3DM 序列化（被 main.c include）*/
#ifndef G2B_WRITE_H
#define G2B_WRITE_H

static uint32_t align16(uint32_t x){ return (x + 15u) & ~15u; }

/* 构建 ANIM 段 blob（段内相对偏移）。返回 malloc 的 blob，*len 写长度。无动画返回 NULL */
/* 构建 NODE 段：r3d_b3dm_node_t[] */
static uint8_t* g2b_build_node(g2b_scene_t *sc, uint32_t *out_len){
    if(sc->node_count==0){ *out_len=0; return NULL; }
    uint32_t len=sc->node_count*sizeof(r3d_b3dm_node_t);
    uint8_t *b=(uint8_t*)malloc(len);
    memcpy(b,sc->nodes,len); *out_len=len; return b;
}
/* 构建 SKELETON 段：header + invbind + joint_nodes */
static uint8_t* g2b_build_skeleton(g2b_scene_t *sc, uint32_t *out_len){
    if(sc->joint_count==0){ *out_len=0; return NULL; }
    uint32_t off_ib=align16(sizeof(r3d_b3dm_skeleton_t));
    uint32_t ib_bytes=sc->joint_count*16*4;
    uint32_t off_jn=align16(off_ib+ib_bytes);
    uint32_t jn_bytes=sc->joint_count*2;
    uint32_t total=align16(off_jn+jn_bytes);
    uint8_t *b=(uint8_t*)calloc(1,total);
    r3d_b3dm_skeleton_t *sh=(r3d_b3dm_skeleton_t*)b;
    sh->joint_count=sc->joint_count; sh->inverse_bind_offset=off_ib; sh->joint_node_offset=off_jn;
    memcpy(b+off_ib,sc->inv_bind,ib_bytes);
    memcpy(b+off_jn,sc->joint_nodes,jn_bytes);
    *out_len=total; return b;
}
/* 构建 SKINVTX 段：每顶点 joints[4]+weights[4]（取 prims[0]）*/
static uint8_t* g2b_build_skinvtx(g2b_scene_t *sc, uint32_t *out_len){
    if(sc->prim_count==0 || !sc->prims[0].joints){ *out_len=0; return NULL; }
    wprim_t *wp=&sc->prims[0];
    uint32_t len=wp->vcount*sizeof(r3d_b3dm_skinvtx_t);
    uint8_t *b=(uint8_t*)malloc(len);
    r3d_b3dm_skinvtx_t *sv=(r3d_b3dm_skinvtx_t*)b;
    for(uint32_t i=0;i<wp->vcount;i++){
        for(int k=0;k<4;k++){ sv[i].joints[k]=wp->joints[i*4+k]; sv[i].weights[k]=wp->weights[i*4+k]; }
    }
    *out_len=len; return b;
}

/* 构建 MORPH 段 blob（单 prim 假设，取 prims[0]）。无 morph 返回 NULL */
static uint8_t* g2b_build_morph(g2b_scene_t *sc, uint32_t *out_len){
    if (sc->prim_count==0 || sc->prims[0].morph_count==0){ *out_len=0; return NULL; }
    wprim_t *wp=&sc->prims[0];
    uint32_t off_hdr=0;
    uint32_t off_deltas=align16(sizeof(r3d_b3dm_morph_t));
    uint32_t dbytes=(uint32_t)wp->morph_count*wp->vcount*3*4;
    uint32_t total=align16(off_deltas+dbytes);
    uint8_t *blob=(uint8_t*)calloc(1,total);
    r3d_b3dm_morph_t *mh=(r3d_b3dm_morph_t*)(blob+off_hdr);
    mh->target_count=wp->morph_count; mh->vertex_count=wp->vcount;
    mh->deltas_offset=off_deltas;
    memcpy(blob+off_deltas, wp->morph_delta, dbytes);
    *out_len=total;
    return blob;
}

static uint8_t* g2b_build_anim(g2b_scene_t *sc, uint32_t *out_len){
    if (sc->clip_count==0) { *out_len=0; return NULL; }
    /* 统计 channel 总数与 framedata 总字节 */
    uint32_t total_ch=0; uint32_t fdata_bytes=0;
    for(uint32_t c=0;c<sc->clip_count;c++){
        total_ch += sc->clips[c].nchan;
        for(uint32_t k=0;k<sc->clips[c].nchan;k++)
            fdata_bytes += sc->clips[c].chans[k].frames * sc->clips[c].chans[k].comp * 4;
    }
    uint32_t off_hdr = 0;
    uint32_t off_clips = align16(sizeof(r3d_b3dm_anim_header_t));
    uint32_t off_chans = align16(off_clips + sc->clip_count*sizeof(r3d_b3dm_clip_t));
    uint32_t off_fdata = align16(off_chans + total_ch*sizeof(r3d_b3dm_channel_t));
    uint32_t total = align16(off_fdata + fdata_bytes);

    uint8_t *blob=(uint8_t*)calloc(1,total);
    r3d_b3dm_anim_header_t *ah=(r3d_b3dm_anim_header_t*)(blob+off_hdr);
    ah->clip_count=sc->clip_count; ah->clips_offset=off_clips;

    r3d_b3dm_clip_t *clips=(r3d_b3dm_clip_t*)(blob+off_clips);
    r3d_b3dm_channel_t *chans=(r3d_b3dm_channel_t*)(blob+off_chans);
    uint32_t ch_i=0, fdata_cur=off_fdata;

    for(uint32_t c=0;c<sc->clip_count;c++){
        wclip_t *wc=&sc->clips[c];
        memcpy(clips[c].name, wc->name, 32);
        clips[c].duration=wc->dur; clips[c].fps=wc->fps;
        clips[c].channel_count=wc->nchan;
        clips[c].channels_offset=off_chans + ch_i*sizeof(r3d_b3dm_channel_t);
        for(uint32_t k=0;k<wc->nchan;k++){
            wchan_t *wch=&wc->chans[k];
            chans[ch_i].target_node=wch->node;
            chans[ch_i].path=wch->path; chans[ch_i].comp=wch->comp;
            chans[ch_i].frame_count=wch->frames;
            chans[ch_i].data_offset=fdata_cur;
            uint32_t bytes=wch->frames*wch->comp*4;
            memcpy(blob+fdata_cur, wch->data, bytes);
            fdata_cur += bytes;
            ch_i++;
        }
    }
    *out_len=total;
    return blob;
}


/* 简单动态 buffer */
typedef struct { uint8_t *p; uint32_t len, cap; } buf_t;
static void buf_ensure(buf_t *b, uint32_t add){
    if (b->len+add > b->cap){ b->cap = (b->len+add)*2 + 64; b->p=(uint8_t*)realloc(b->p,b->cap); }
}
static void buf_put(buf_t *b, const void *d, uint32_t n){ buf_ensure(b,n); memcpy(b->p+b->len,d,n); b->len+=n; }
static void buf_pad16(buf_t *b){ uint32_t a=align16(b->len); buf_ensure(b,a-b->len); while(b->len<a) b->p[b->len++]=0; }

static int g2b_write(g2b_scene_t *sc, const opts_t *o) {
    /* ---- 1. 全局 AABB → scale/bias（定点 s16 [-32767,32767]）---- */
    float bmin[3]={1e30f,1e30f,1e30f}, bmax[3]={-1e30f,-1e30f,-1e30f};
    for (uint32_t pi=0;pi<sc->prim_count;pi++){
        wprim_t *wp=&sc->prims[pi];
        for (uint32_t i=0;i<wp->vcount;i++){
            float p[3]={wp->verts[i].px,wp->verts[i].py,wp->verts[i].pz};
            for(int k=0;k<3;k++){ if(p[k]<bmin[k])bmin[k]=p[k]; if(p[k]>bmax[k])bmax[k]=p[k]; }
        }
    }
    float scale[3],bias[3];
    for(int k=0;k<3;k++){
        bias[k]=(bmin[k]+bmax[k])*0.5f;
        float half=(bmax[k]-bmin[k])*0.5f; if(half<1e-6f)half=1e-6f;
        scale[k]=half/32767.0f;          /* q = (p-bias)/scale */
    }

    /* ---- 2. 合并顶点 + 分组排序（按 tex_id,blend）---- */
    /* 先给每个 prim 一个排序键，按键排序 prim 顺序 */
    uint32_t *order=(uint32_t*)malloc(sizeof(uint32_t)*sc->prim_count);
    for(uint32_t i=0;i<sc->prim_count;i++) order[i]=i;
    /* 插入排序(prim 数少) */
    for(uint32_t i=1;i<sc->prim_count;i++){
        uint32_t v=order[i]; uint32_t j=i;
        while(j>0){
            wprim_t*a=&sc->prims[order[j-1]],*b=&sc->prims[v];
            long ka=((long)(a->tex_id+1)<<8)|a->blend, kb=((long)(b->tex_id+1)<<8)|b->blend;
            if(ka<=kb)break; order[j]=order[j-1]; j--;
        }
        order[j]=v;
    }

    buf_t vtx={0}, idx={0};
    /* submesh 记录 */
    typedef struct { int tex,blend,flags; uint32_t ioff,icount,bcf; int nid; } sm_t;
    sm_t *sms=(sm_t*)calloc(sc->prim_count,sizeof(sm_t)); uint32_t sm_n=0;
    uint32_t vbase=0, ioff=0;

    for(uint32_t oi=0; oi<sc->prim_count; oi++){
        wprim_t *wp=&sc->prims[order[oi]];
        /* 顶点定点化写入 */
        for(uint32_t i=0;i<wp->vcount;i++){
            r3d_b3dm_vertex_t qv; memset(&qv,0,sizeof qv);
            float p[3]={wp->verts[i].px,wp->verts[i].py,wp->verts[i].pz};
            for(int k=0;k<3;k++){
                int q=(int)lroundf((p[k]-bias[k])/scale[k]);
                if(q>32767)q=32767; if(q<-32767)q=-32767; qv.pos[k]=(int16_t)q;
            }
            float uu=wp->verts[i].u, vv=wp->verts[i].v;
            if(uu<0)uu=0; if(uu>1)uu=1; if(vv<0)vv=0; if(vv>1)vv=1;
            qv.uv[0]=(uint16_t)lroundf(uu*65535.0f);
            qv.uv[1]=(uint16_t)lroundf(vv*65535.0f);
            oct_encode(wp->verts[i].nx,wp->verts[i].ny,wp->verts[i].nz,&qv.normal_oct[0],&qv.normal_oct[1]);
            { int ao=(int)(wp->verts[i].ao*255.0f+0.5f); if(ao>255)ao=255; if(ao<0)ao=0; qv.pad[0]=(uint8_t)ao; }
            buf_put(&vtx,&qv,sizeof qv);
        }
        /* 索引(加 vbase 偏移) */
        for(uint32_t i=0;i<wp->icount;i++){
            uint16_t v=(uint16_t)(wp->idx[i]+vbase);
            buf_put(&idx,&v,sizeof v);
        }
        sms[sm_n].tex=wp->tex_id; sms[sm_n].blend=wp->blend;
        sms[sm_n].flags=wp->mat_flags | (wp->is_dynamic? 16:0); /* bit4=dynamic */
        sms[sm_n].nid=wp->node_id;
        sms[sm_n].ioff=ioff; sms[sm_n].icount=wp->icount; sms[sm_n].bcf=wp->base_color_factor; sm_n++;
        vbase += wp->vcount; ioff += wp->icount;
    }
    free(order);
    uint32_t total_v = vbase, total_i = ioff;

    /* 索引段补齐到偶数个 u16 */
    if ((total_i & 1)) { uint16_t z=0; buf_put(&idx,&z,sizeof z); }

    /* ---- 3. submesh 段 ---- */
    buf_t smbuf={0};
    for(uint32_t i=0;i<sm_n;i++){
        r3d_b3dm_submesh_t s; memset(&s,0,sizeof s);
        s.tex_id    = sms[i].tex<0 ? 0xFFFF : (uint16_t)sms[i].tex;
        s.matcap_id = 0xFFFF;
        s.blend     = (uint8_t)sms[i].blend;
        s.mat_flags = (uint8_t)sms[i].flags;
        s.node_id   = (uint16_t)(sms[i].nid<0?0:sms[i].nid);
        s.index_offset = sms[i].ioff;
        s.index_count  = sms[i].icount;
        s.base_color_factor = sms[i].bcf;
        buf_put(&smbuf,&s,sizeof s);
    }

    /* ---- 4. texture 段：合成单 blob（表头[count] + 像素），data_offset 段内相对 ---- */
    buf_t texblob={0};
    uint32_t tex_hdr_sz=sc->tex_count*sizeof(r3d_b3dm_texture_t);
    uint32_t tex_pix_start=align16(tex_hdr_sz);
    {
        /* 先算每张像素段内偏移并拼像素到临时 */
        buf_t pix={0}; uint32_t *poff=(uint32_t*)malloc(sizeof(uint32_t)*(sc->tex_count?sc->tex_count:1));
        for(uint32_t i=0;i<sc->tex_count;i++){
            poff[i]=tex_pix_start+pix.len;
            g2b_tex_t *t=&sc->texs[i]; uint32_t n=(uint32_t)t->w*t->h;
            for(uint32_t k=0;k<n;k++){
                uint8_t r=t->rgba[k*4+0],g=t->rgba[k*4+1],b=t->rgba[k*4+2],a=t->rgba[k*4+3];
                r=(uint8_t)(r*a/255);g=(uint8_t)(g*a/255);b=(uint8_t)(b*a/255);
                uint8_t bgra[4]={b,g,r,a}; buf_put(&pix,bgra,4);
            }
            buf_pad16(&pix);
        }
        /* 写表头 */
        for(uint32_t i=0;i<sc->tex_count;i++){
            r3d_b3dm_texture_t th; memset(&th,0,sizeof th);
            th.width=(uint16_t)sc->texs[i].w; th.height=(uint16_t)sc->texs[i].h; th.format=0;
            th.data_offset=poff[i]; th.data_size=(uint32_t)sc->texs[i].w*sc->texs[i].h*4;
            buf_put(&texblob,&th,sizeof th);
        }
        while(texblob.len<tex_pix_start){ uint8_t z=0; buf_put(&texblob,&z,1); }
        buf_put(&texblob,pix.p,pix.len);
        free(pix.p); free(poff);
    }

    /* ---- 5. 各段 blob 统一组装（游标 16 对齐推进）---- */
    uint32_t anim_len=0; uint8_t *anim_blob=g2b_build_anim(sc,&anim_len);
    uint32_t morph_len=0; uint8_t *morph_blob=g2b_build_morph(sc,&morph_len);
    uint32_t node_len=0; uint8_t *node_blob=g2b_build_node(sc,&node_len);
    uint32_t skel_len=0; uint8_t *skel_blob=g2b_build_skeleton(sc,&skel_len);
    uint32_t skv_len=0;  uint8_t *skv_blob=g2b_build_skinvtx(sc,&skv_len);

    /* 段描述表 */
    struct { uint32_t type; const void* data; uint32_t len, count; } segs[10]; int ns=0;
    segs[ns++]=(typeof(segs[0])){R3D_SEC_VERTEX, vtx.p, vtx.len, total_v};
    segs[ns++]=(typeof(segs[0])){R3D_SEC_INDEX,  idx.p, idx.len, total_i};
    if(sm_n)        segs[ns++]=(typeof(segs[0])){R3D_SEC_SUBMESH, smbuf.p, smbuf.len, sm_n};
    if(sc->tex_count)segs[ns++]=(typeof(segs[0])){R3D_SEC_TEXTURE, texblob.p, texblob.len, sc->tex_count};
    if(anim_len)    segs[ns++]=(typeof(segs[0])){R3D_SEC_ANIM, anim_blob, anim_len, sc->clip_count};
    if(morph_len)   segs[ns++]=(typeof(segs[0])){R3D_SEC_MORPH, morph_blob, morph_len, sc->prims[0].morph_count};
    if(node_len)    segs[ns++]=(typeof(segs[0])){R3D_SEC_NODE, node_blob, node_len, sc->node_count};
    if(skel_len)    segs[ns++]=(typeof(segs[0])){R3D_SEC_SKELETON, skel_blob, skel_len, sc->joint_count};
    if(skv_len)     segs[ns++]=(typeof(segs[0])){R3D_SEC_SKINVTX, skv_blob, skv_len, total_v};

    uint32_t off_tbl=sizeof(r3d_b3dm_header_t);
    uint32_t cursor=align16(off_tbl + ns*sizeof(r3d_b3dm_section_t));
    uint32_t seg_off[10];
    for(int i=0;i<ns;i++){ seg_off[i]=cursor; cursor=align16(cursor+segs[i].len); }
    uint32_t total=cursor;

    uint8_t *out=(uint8_t*)calloc(1,total);
    r3d_b3dm_header_t *h=(r3d_b3dm_header_t*)out;
    h->magic=R3D_B3DM_MAGIC; h->version=R3D_B3DM_VERSION;
    h->flags=(anim_len?R3D_B3DM_FLAG_ANIM:0)|(morph_len?R3D_B3DM_FLAG_MORPH:0)|(skv_len?R3D_B3DM_FLAG_SKIN:0);
    h->section_count=ns;
    h->bounding_sphere[0]=sc->bs[0];h->bounding_sphere[1]=sc->bs[1];
    h->bounding_sphere[2]=sc->bs[2];h->bounding_sphere[3]=sc->bs[3];
    for(int k=0;k<3;k++){h->vertex_scale[k]=scale[k];h->vertex_bias[k]=bias[k];}

    r3d_b3dm_section_t *tbl=(r3d_b3dm_section_t*)(out+off_tbl);
    for(int i=0;i<ns;i++){
        tbl[i]=(r3d_b3dm_section_t){segs[i].type,seg_off[i],segs[i].len,segs[i].count};
        memcpy(out+seg_off[i], segs[i].data, segs[i].len);
    }
    free(anim_blob);free(morph_blob);free(node_blob);free(skel_blob);free(skv_blob);free(texblob.p);

    FILE *fp=fopen(o->output,"wb");
    int rc = (fp && fwrite(out,1,total,fp)==total) ? 0 : -1;
    if(fp)fclose(fp);

    free(out); free(sms);
    free(vtx.p);free(idx.p);free(smbuf.p);
    return rc;
}
#endif
