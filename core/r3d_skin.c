/*
 * r3d_skin.c — 骨骼蒙皮（架构文档 §11.6）
 */
#include "r3d/r3d_skin.h"
#include "r3d/r3d_math.h"
#include "r3d/r3d_b3dm.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* node TRS（运行时，动画可覆盖）*/
typedef struct { r3d_vec3_t t,s; r3d_quat_t r; int16_t parent; } node_trs_t;

int r3d_skin_init(r3d_skin_t *sk, const r3d_model_t *m){
    memset(sk,0,sizeof(*sk));
    sk->node_count=m->node_count; sk->joint_count=m->joint_count; sk->vertex_count=m->vertex_count;
    if(sk->node_count==0||sk->joint_count==0) return -1;
    sk->node_local=(r3d_mat4_t*)malloc(sizeof(r3d_mat4_t)*sk->node_count);
    sk->node_world=(r3d_mat4_t*)malloc(sizeof(r3d_mat4_t)*sk->node_count);
    sk->palette=(r3d_mat4_t*)malloc(sizeof(r3d_mat4_t)*sk->joint_count);
    sk->out=(r3d_vertex_t*)malloc(sizeof(r3d_vertex_t)*sk->vertex_count);
    if(!sk->node_local||!sk->node_world||!sk->palette||!sk->out){ r3d_skin_free(sk); return -1; }
    memcpy(sk->out, m->vertices, sizeof(r3d_vertex_t)*sk->vertex_count);
    return 0;
}

void r3d_skin_free(r3d_skin_t *sk){
    free(sk->node_local);free(sk->node_world);free(sk->palette);free(sk->out);
    memset(sk,0,sizeof(*sk));
}

/* 内联通道采样（定步长，旋转 slerp）*/
static void skin_sample(const r3d_anim_chan_t *c, float fps, float t, float *out){
    if(c->frame_count==0) return;
    float fp=t*fps; uint32_t f0=(uint32_t)fp;
    if(f0>=c->frame_count-1){ memcpy(out,&c->frames[(c->frame_count-1)*c->comp],c->comp*sizeof(float)); return; }
    float u=fp-f0; const float *a=&c->frames[f0*c->comp], *b=&c->frames[(f0+1)*c->comp];
    if(c->path==R3D_ANIM_PATH_R){
        r3d_quat_t qa={a[0],a[1],a[2],a[3]},qb={b[0],b[1],b[2],b[3]},q=r3d_quat_slerp(qa,qb,u);
        out[0]=q.x;out[1]=q.y;out[2]=q.z;out[3]=q.w;
    } else for(int i=0;i<c->comp;i++) out[i]=a[i]+(b[i]-a[i])*u;
}

void r3d_skin_update(r3d_skin_t *sk, const r3d_model_t *m, r3d_anim_state_t *st){
    const r3d_b3dm_node_t *bn=(const r3d_b3dm_node_t*)m->nodes;
    /* 1. node 局部 TRS：默认值 */
    node_trs_t *trs=(node_trs_t*)malloc(sizeof(node_trs_t)*sk->node_count);
    for(uint32_t i=0;i<sk->node_count;i++){
        trs[i].t=(r3d_vec3_t){bn[i].translation[0],bn[i].translation[1],bn[i].translation[2]};
        trs[i].r=(r3d_quat_t){bn[i].rotation[0],bn[i].rotation[1],bn[i].rotation[2],bn[i].rotation[3]};
        trs[i].s=(r3d_vec3_t){bn[i].scale[0],bn[i].scale[1],bn[i].scale[2]};
        trs[i].parent=bn[i].parent;
    }
    /* 2. 动画覆盖：遍历 active player 的通道，按 target_node 写 TRS */
    for(int pi=0;pi<R3D_MAX_PLAYERS;pi++){
        r3d_anim_player_t *p=&st->players[pi];
        if(!p->active) continue;
        for(uint32_t c=0;c<p->clip->channel_count;c++){
            const r3d_anim_chan_t *ch=&p->clip->channels[c];
            if(ch->target_node>=sk->node_count) continue;
            /* 只处理 T/R/S；weights 通道(comp=morph 数，可能 >4)对节点 TRS 无意义，
             * 误传给 skin_sample(v[4]) 会按 comp 写满 → 栈溢出。 */
            if(ch->path!=R3D_ANIM_PATH_T && ch->path!=R3D_ANIM_PATH_R && ch->path!=R3D_ANIM_PATH_S)
                continue;
            float v[4]={0}; skin_sample(ch,p->clip->fps,p->time,v);
            node_trs_t *n=&trs[ch->target_node];
            if(ch->path==R3D_ANIM_PATH_T) n->t=(r3d_vec3_t){v[0],v[1],v[2]};
            else if(ch->path==R3D_ANIM_PATH_S) n->s=(r3d_vec3_t){v[0],v[1],v[2]};
            else if(ch->path==R3D_ANIM_PATH_R) n->r=(r3d_quat_t){v[0],v[1],v[2],v[3]};
        }
    }
    /* 3. 局部矩阵 */
    for(uint32_t i=0;i<sk->node_count;i++)
        r3d_mat4_from_trs(&sk->node_local[i], trs[i].t, trs[i].r, trs[i].s);
    /* 4. 世界矩阵累乘（假设 node 按父先于子排序；否则需拓扑序）*/
    for(uint32_t i=0;i<sk->node_count;i++){
        if(trs[i].parent<0) sk->node_world[i]=sk->node_local[i];
        else r3d_mat4_mul(&sk->node_world[i], &sk->node_world[trs[i].parent], &sk->node_local[i]);
    }
    /* 5. 调色板 palette[j] = world[joint_node[j]] × inv_bind[j] */
    for(uint32_t j=0;j<sk->joint_count;j++){
        r3d_mat4_t ib; memcpy(ib.m, &m->inv_bind[j*16], 16*sizeof(float));
        uint16_t nn=m->joint_nodes[j];
        if(nn<sk->node_count) r3d_mat4_mul(&sk->palette[j], &sk->node_world[nn], &ib);
        else sk->palette[j]=ib;
    }
    /* 6. 蒙皮顶点 */
    const r3d_b3dm_skinvtx_t *sv=(const r3d_b3dm_skinvtx_t*)m->skinvtx;
    for(uint32_t i=0;i<sk->vertex_count;i++){
        r3d_vec3_t bp=m->vertices[i].pos;
        r3d_vec4_t acc={0,0,0,0};
        float wsum=0;
        for(int k=0;k<4;k++){
            float w=sv[i].weights[k]/255.0f;
            if(w<=0) continue;
            wsum+=w;
            uint8_t jj=sv[i].joints[k];
            if(jj>=sk->joint_count) continue;
            r3d_vec4_t p={bp.x,bp.y,bp.z,1.0f};
            r3d_vec4_t tp=r3d_mat4_mul_vec4(&sk->palette[jj], p);
            acc.x+=tp.x*w; acc.y+=tp.y*w; acc.z+=tp.z*w;
        }
        sk->out[i]=m->vertices[i];
        if(wsum>0.001f){ sk->out[i].pos=(r3d_vec3_t){acc.x,acc.y,acc.z}; }
    }
    free(trs);
}
