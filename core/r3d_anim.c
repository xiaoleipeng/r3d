/*
 * r3d_anim.c — 运行时动画（架构文档 §11.3-11.5）
 * 解析 ANIM 段、播放器池、定步长采样、加权混合 → 根节点局部矩阵。
 */
#include "r3d/r3d_anim.h"
#include "r3d/r3d_b3dm.h"
#include "r3d/r3d_math.h"
#include "r3d/r3d_model.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 在 raw buffer 里找 ANIM 段 */
static const uint8_t* find_anim(const void *raw, uint32_t *seg_size){
    const r3d_b3dm_header_t *h=(const r3d_b3dm_header_t*)raw;
    if(h->magic!=R3D_B3DM_MAGIC) return NULL;
    const r3d_b3dm_section_t *tbl=(const r3d_b3dm_section_t*)((const uint8_t*)raw+sizeof(*h));
    for(uint32_t i=0;i<h->section_count;i++)
        if(tbl[i].type==R3D_SEC_ANIM){ if(seg_size)*seg_size=tbl[i].size; return (const uint8_t*)raw+tbl[i].offset; }
    return NULL;
}

void r3d_anim_set_parse(r3d_anim_set_t *set, const void *raw){
    memset(set,0,sizeof(*set));
    const uint8_t *seg=find_anim(raw,NULL);
    if(!seg) return;

    const r3d_b3dm_anim_header_t *ah=(const r3d_b3dm_anim_header_t*)seg;
    const r3d_b3dm_clip_t *bclips=(const r3d_b3dm_clip_t*)(seg+ah->clips_offset);

    /* 统计 channel 总数 */
    uint32_t total_ch=0;
    for(uint32_t c=0;c<ah->clip_count;c++) total_ch+=bclips[c].channel_count;

    set->clip_count=ah->clip_count;
    set->clips=(r3d_anim_clip_t*)calloc(ah->clip_count,sizeof(r3d_anim_clip_t));
    set->chan_pool=(r3d_anim_chan_t*)calloc(total_ch?total_ch:1,sizeof(r3d_anim_chan_t));

    uint32_t ch_i=0;
    for(uint32_t c=0;c<ah->clip_count;c++){
        memcpy(set->clips[c].name, bclips[c].name, 32);
        set->clips[c].duration=bclips[c].duration;
        set->clips[c].fps=bclips[c].fps;
        set->clips[c].channel_count=bclips[c].channel_count;
        set->clips[c].channels=&set->chan_pool[ch_i];
        const r3d_b3dm_channel_t *bch=(const r3d_b3dm_channel_t*)(seg+bclips[c].channels_offset);
        for(uint32_t k=0;k<bclips[c].channel_count;k++){
            r3d_anim_chan_t *rc=&set->chan_pool[ch_i++];
            rc->target_node=bch[k].target_node;
            rc->path=bch[k].path; rc->comp=bch[k].comp;
            rc->frame_count=bch[k].frame_count;
            rc->frames=(const float*)(seg+bch[k].data_offset);
        }
    }
}

void r3d_anim_set_free(r3d_anim_set_t *set){
    free(set->clips); free(set->chan_pool); memset(set,0,sizeof(*set));
}

void r3d_anim_state_init(r3d_anim_state_t *st){
    memset(st,0,sizeof(*st));
    r3d_mat4_identity(&st->root_matrix);
}

int r3d_anim_play(r3d_anim_state_t *st, const r3d_anim_set_t *set, const char *name, bool loop){
    const r3d_anim_clip_t *clip=NULL;
    for(uint32_t i=0;i<set->clip_count;i++)
        if(!name || strcmp(set->clips[i].name,name)==0){ clip=&set->clips[i]; break; }
    if(!clip && set->clip_count>0 && !name) clip=&set->clips[0];
    if(!clip) return -1;
    for(int i=0;i<R3D_MAX_PLAYERS;i++){
        if(!st->players[i].active){
            st->players[i].clip=clip; st->players[i].time=0; st->players[i].weight=1.0f;
            st->players[i].speed=1.0f; st->players[i].loop=loop; st->players[i].active=true;
            return i;
        }
    }
    return -1;
}

void r3d_anim_stop(r3d_anim_state_t *st, const char *name){
    for(int i=0;i<R3D_MAX_PLAYERS;i++)
        if(st->players[i].active && (!name || strcmp(st->players[i].clip->name,name)==0))
            st->players[i].active=false;
}

void r3d_anim_set_weight(r3d_anim_state_t *st, const char *name, float w){
    for(int i=0;i<R3D_MAX_PLAYERS;i++)
        if(st->players[i].active && (!name || strcmp(st->players[i].clip->name,name)==0))
            st->players[i].weight=w;
}

/* 在通道上按 time 采样（定步长，取整 + 帧间线性/slerp）*/
static void sample_chan(const r3d_anim_chan_t *c, float fps, float t, float *out){
    if(c->frame_count==0) return;
    float fpos=t*fps; uint32_t f0=(uint32_t)fpos;
    if(f0>=c->frame_count-1){ memcpy(out,&c->frames[(c->frame_count-1)*c->comp],c->comp*sizeof(float)); return; }
    float u=fpos-f0;
    const float *a=&c->frames[f0*c->comp], *b=&c->frames[(f0+1)*c->comp];
    if(c->path==R3D_ANIM_PATH_R && c->comp==4){
        r3d_quat_t qa={a[0],a[1],a[2],a[3]}, qb={b[0],b[1],b[2],b[3]};
        r3d_quat_t q=r3d_quat_slerp(qa,qb,u);
        out[0]=q.x;out[1]=q.y;out[2]=q.z;out[3]=q.w;
    } else for(int i=0;i<c->comp;i++) out[i]=a[i]+(b[i]-a[i])*u;
}

void r3d_anim_update(r3d_anim_state_t *st, float dt){
    /* 累加器：根节点 TRS（带权重）*/
    float wsum=0;
    r3d_vec3_t T={0,0,0}, S={0,0,0};
    r3d_quat_t R={0,0,0,0};
    int have_T=0,have_R=0,have_S=0;
    float morph_acc[R3D_MAX_MORPH]={0}; uint32_t morph_n=0;

    for(int i=0;i<R3D_MAX_PLAYERS;i++){
        r3d_anim_player_t *p=&st->players[i];
        if(!p->active) continue;
        p->time += dt*p->speed;
        if(p->loop && p->clip->duration>0){
            while(p->time>p->clip->duration) p->time-=p->clip->duration;
        } else if(p->time>p->clip->duration){ p->time=p->clip->duration; }

        float w=p->weight; wsum+=w;
        for(uint32_t c=0;c<p->clip->channel_count;c++){
            const r3d_anim_chan_t *ch=&p->clip->channels[c];
            if(ch->path==R3D_ANIM_PATH_W){
                /* morph 权重通道：comp = target 数 */
                float mw[R3D_MAX_MORPH]={0};
                uint32_t cn = ch->comp<R3D_MAX_MORPH?ch->comp:R3D_MAX_MORPH;
                sample_chan(ch,p->clip->fps,p->time,mw);
                for(uint32_t k=0;k<cn;k++) morph_acc[k]+=mw[k]*w;
                if(cn>morph_n) morph_n=cn;
                continue;
            }
            float v[4]={0};
            sample_chan(ch,p->clip->fps,p->time,v);
            if(ch->path==R3D_ANIM_PATH_T){ T.x+=v[0]*w;T.y+=v[1]*w;T.z+=v[2]*w; have_T=1; }
            else if(ch->path==R3D_ANIM_PATH_S){ S.x+=v[0]*w;S.y+=v[1]*w;S.z+=v[2]*w; have_S=1; }
            else if(ch->path==R3D_ANIM_PATH_R){
                R.x+=v[0]*w;R.y+=v[1]*w;R.z+=v[2]*w;R.w+=v[3]*w; have_R=1;
            }
        }
    }

    /* 输出 morph 权重（按 wsum 归一）*/
    st->morph_weight_count=morph_n;
    if(morph_n>0 && wsum>0.0f)
        for(uint32_t k=0;k<morph_n;k++) st->morph_weights[k]=morph_acc[k]/wsum;

    if(wsum<=0.0f){ r3d_mat4_identity(&st->root_matrix); return; }
    float inv=1.0f/wsum;
    r3d_vec3_t t = have_T ? (r3d_vec3_t){T.x*inv,T.y*inv,T.z*inv} : (r3d_vec3_t){0,0,0};
    r3d_vec3_t s = have_S ? (r3d_vec3_t){S.x*inv,S.y*inv,S.z*inv} : (r3d_vec3_t){1,1,1};
    r3d_quat_t r;
    if(have_R){
        r.x=R.x;r.y=R.y;r.z=R.z;r.w=R.w;
        float n=sqrtf(r.x*r.x+r.y*r.y+r.z*r.z+r.w*r.w);
        if(n>1e-8f){r.x/=n;r.y/=n;r.z/=n;r.w/=n;} else {r=(r3d_quat_t){0,0,0,1};}
    } else r=(r3d_quat_t){0,0,0,1};

    r3d_mat4_from_trs(&st->root_matrix, t, r, s);
}

/* 算单个 node 的局部矩阵（NODE 默认 TRS + active player 通道覆盖）*/
static void node_local_matrix(const r3d_b3dm_node_t *bn, r3d_anim_state_t *st,
                              uint16_t nid, r3d_mat4_t *out){
    r3d_vec3_t t={bn[nid].translation[0],bn[nid].translation[1],bn[nid].translation[2]};
    r3d_quat_t r={bn[nid].rotation[0],bn[nid].rotation[1],bn[nid].rotation[2],bn[nid].rotation[3]};
    r3d_vec3_t s={bn[nid].scale[0],bn[nid].scale[1],bn[nid].scale[2]};
    for(int i=0;i<R3D_MAX_PLAYERS;i++){
        r3d_anim_player_t *p=&st->players[i];
        if(!p->active) continue;
        for(uint32_t c=0;c<p->clip->channel_count;c++){
            const r3d_anim_chan_t *ch=&p->clip->channels[c];
            if(ch->target_node!=nid) continue;
            float v[4]={0}; sample_chan(ch,p->clip->fps,p->time,v);
            if(ch->path==R3D_ANIM_PATH_T) t=(r3d_vec3_t){v[0],v[1],v[2]};
            else if(ch->path==R3D_ANIM_PATH_S) s=(r3d_vec3_t){v[0],v[1],v[2]};
            else if(ch->path==R3D_ANIM_PATH_R) r=(r3d_quat_t){v[0],v[1],v[2],v[3]};
        }
    }
    r3d_mat4_from_trs(out,t,r,s);
}

void r3d_anim_node_matrix(const void *model, r3d_anim_state_t *st,
                          uint16_t node_id, r3d_mat4_t *out){
    const r3d_model_t *m=(const r3d_model_t*)model;
    const r3d_b3dm_node_t *bn=(const r3d_b3dm_node_t*)m->nodes;
    if(!bn || node_id>=m->node_count){ r3d_mat4_identity(out); return; }
    /* 收集 node→root 链 */
    uint16_t chain[64]; int n=0; int cur=node_id;
    while(cur>=0 && n<64){ chain[n++]=(uint16_t)cur; cur=bn[cur].parent; }
    /* 从根到该 node 累乘：world = root_local × ... × node_local */
    r3d_mat4_t world; r3d_mat4_identity(&world);
    for(int i=n-1;i>=0;i--){
        r3d_mat4_t local; node_local_matrix(bn,st,chain[i],&local);
        r3d_mat4_t tmp; r3d_mat4_mul(&tmp,&world,&local);
        world=tmp;
    }
    *out=world;
}
