/* g2b_skin.h — 提取 node 树 + skin（被 main.c include，在 extract 之后）*/
#ifndef G2B_SKIN_H
#define G2B_SKIN_H

#include "r3d/r3d_b3dm.h"

static int g2b_node_idx2(cgltf_data *d, cgltf_node *n){
    for(size_t i=0;i<d->nodes_count;i++) if(&d->nodes[i]==n) return (int)i;
    return -1;
}

/* 提取 node 树（所有 node 的 parent+TRS）+ skin[0] 的 joints/invbind */
static void g2b_extract_skin(cgltf_data *d, g2b_scene_t *sc){
    /* node 树 */
    if(d->nodes_count>0){
        r3d_b3dm_node_t *nodes=(r3d_b3dm_node_t*)calloc(d->nodes_count,sizeof(r3d_b3dm_node_t));
        for(size_t i=0;i<d->nodes_count;i++){
            cgltf_node *n=&d->nodes[i];
            nodes[i].parent=(int16_t)(n->parent?g2b_node_idx2(d,n->parent):-1);
            /* TRS：cgltf 提供分量或 matrix；优先分量 */
            float t[3]={0,0,0},r[4]={0,0,0,1},s[3]={1,1,1};
            if(n->has_translation){t[0]=n->translation[0];t[1]=n->translation[1];t[2]=n->translation[2];}
            if(n->has_rotation){r[0]=n->rotation[0];r[1]=n->rotation[1];r[2]=n->rotation[2];r[3]=n->rotation[3];}
            if(n->has_scale){s[0]=n->scale[0];s[1]=n->scale[1];s[2]=n->scale[2];}
            for(int k=0;k<3;k++){nodes[i].translation[k]=t[k];nodes[i].scale[k]=s[k];}
            for(int k=0;k<4;k++)nodes[i].rotation[k]=r[k];
        }
        sc->nodes=nodes; sc->node_count=(uint32_t)d->nodes_count;
    }
    /* skin[0] */
    if(d->skins_count>0){
        cgltf_skin *sk=&d->skins[0];
        sc->joint_count=(uint32_t)sk->joints_count;
        sc->joint_nodes=(uint16_t*)calloc(sk->joints_count,sizeof(uint16_t));
        sc->inv_bind=(float*)calloc(sk->joints_count*16,sizeof(float));
        for(size_t j=0;j<sk->joints_count;j++){
            int ni=g2b_node_idx2(d,sk->joints[j]);
            sc->joint_nodes[j]=(uint16_t)(ni<0?0:ni);
            if(sk->inverse_bind_matrices)
                cgltf_accessor_read_float(sk->inverse_bind_matrices,j,&sc->inv_bind[j*16],16);
            else { /* 单位矩阵 */ sc->inv_bind[j*16+0]=sc->inv_bind[j*16+5]=sc->inv_bind[j*16+10]=sc->inv_bind[j*16+15]=1.0f; }
        }
    }
}
#endif
