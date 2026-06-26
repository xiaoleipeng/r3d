/* g2b_anim.h — cgltf 动画提取 + 定步长重采样（被 main.c include，在 extract 之后）*/
#ifndef G2B_ANIM_H
#define G2B_ANIM_H

typedef struct { uint16_t node; uint8_t path, comp; uint32_t frames; float *data; } wchan_t;
typedef struct { char name[32]; float dur, fps; wchan_t *chans; uint32_t nchan; } wclip_t;

/* node 指针 → 索引 */
static int g2b_node_index(cgltf_data *d, cgltf_node *n){
    for(size_t i=0;i<d->nodes_count;i++) if(&d->nodes[i]==n) return (int)i;
    return 0;
}

/* 在采样器上按时间 t 采样（线性/slerp）。out 写 comp 个 float */
static void g2b_sample(cgltf_animation_sampler *s, int comp, float t, float *out){
    size_t n = s->input->count;
    float t0, t1; cgltf_accessor_read_float(s->input,0,&t0,1);
    cgltf_accessor_read_float(s->input,n-1,&t1,1);
    if(t<=t0){ cgltf_accessor_read_float(s->output,0,out,comp); return; }
    if(t>=t1){ cgltf_accessor_read_float(s->output,n-1,out,comp); return; }
    /* 找区间 */
    size_t k=0; float a=t0,b=t0;
    for(k=0;k+1<n;k++){
        cgltf_accessor_read_float(s->input,k,&a,1);
        cgltf_accessor_read_float(s->input,k+1,&b,1);
        if(t>=a && t<=b) break;
    }
    float u = (b>a)?(t-a)/(b-a):0.0f;
    float va[4]={0}, vb[4]={0};
    cgltf_accessor_read_float(s->output,k,va,comp);
    cgltf_accessor_read_float(s->output,k+1,vb,comp);
    if(comp==4){ /* 四元数 slerp */
        float dot=va[0]*vb[0]+va[1]*vb[1]+va[2]*vb[2]+va[3]*vb[3];
        if(dot<0){for(int i=0;i<4;i++)vb[i]=-vb[i];dot=-dot;}
        float sa,sb;
        if(1.0f-dot>1e-6f){float om=acosf(dot),so=sinf(om);sa=sinf((1-u)*om)/so;sb=sinf(u*om)/so;}
        else{sa=1-u;sb=u;}
        for(int i=0;i<4;i++) out[i]=sa*va[i]+sb*vb[i];
    } else {
        for(int i=0;i<comp;i++) out[i]=va[i]+(vb[i]-va[i])*u;
    }
}

/* 提取所有动画 clip，定步长重采样。返回 clip 数组 */
static wclip_t* g2b_extract_anim(cgltf_data *d, uint32_t *out_n, float fps){
    if(d->animations_count==0){ *out_n=0; return NULL; }
    wclip_t *clips=(wclip_t*)calloc(d->animations_count,sizeof(wclip_t));
    for(size_t ai=0;ai<d->animations_count;ai++){
        cgltf_animation *an=&d->animations[ai];
        wclip_t *wc=&clips[ai];
        snprintf(wc->name,32,"%s", an->name?an->name:"clip");
        wc->fps=fps;
        /* duration = 最大 input 时间 */
        float dur=0;
        for(size_t c=0;c<an->channels_count;c++){
            cgltf_accessor *in=an->channels[c].sampler->input;
            float last; cgltf_accessor_read_float(in,in->count-1,&last,1);
            if(last>dur)dur=last;
        }
        wc->dur=dur;
        uint32_t frames=(uint32_t)(dur*fps)+1; if(frames<1)frames=1;
        wc->nchan=(uint32_t)an->channels_count;
        wc->chans=(wchan_t*)calloc(wc->nchan,sizeof(wchan_t));
        for(size_t c=0;c<an->channels_count;c++){
            cgltf_animation_channel *ch=&an->channels[c];
            wchan_t *wch=&wc->chans[c];
            wch->node=(uint16_t)g2b_node_index(d,ch->target_node);
            int comp=3, path=0;
            if(ch->target_path==cgltf_animation_path_type_translation){path=0;comp=3;}
            else if(ch->target_path==cgltf_animation_path_type_rotation){path=1;comp=4;}
            else if(ch->target_path==cgltf_animation_path_type_scale){path=2;comp=3;}
            else {path=3;comp=1;}
            wch->path=(uint8_t)path; wch->comp=(uint8_t)comp; wch->frames=frames;
            wch->data=(float*)malloc((size_t)frames*comp*sizeof(float));
            for(uint32_t f=0;f<frames;f++){
                float t=(float)f/fps;
                g2b_sample(ch->sampler,comp,t,&wch->data[f*comp]);
            }
        }
    }
    *out_n=(uint32_t)d->animations_count;
    return clips;
}

static void g2b_free_anim(wclip_t *clips, uint32_t n){
    for(uint32_t i=0;i<n;i++){ for(uint32_t c=0;c<clips[i].nchan;c++) free(clips[i].chans[c].data); free(clips[i].chans); }
    free(clips);
}
#endif
