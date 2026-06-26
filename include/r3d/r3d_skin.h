/*
 * r3d_skin.h — 骨骼蒙皮（架构文档 §11.6）
 * node 树世界矩阵累乘 + 调色板 + 逐顶点蒙皮。
 */
#ifndef R3D_SKIN_H
#define R3D_SKIN_H

#include "r3d_types.h"
#include "r3d_model.h"
#include "r3d_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    r3d_mat4_t   *node_local;   /* [node_count] 局部矩阵（动画覆盖）*/
    r3d_mat4_t   *node_world;   /* [node_count] 世界矩阵 */
    r3d_mat4_t   *palette;      /* [joint_count] 蒙皮调色板 */
    r3d_vertex_t *out;          /* [vertex_count] 蒙皮后顶点 */
    uint32_t      node_count, joint_count, vertex_count;
} r3d_skin_t;

int  r3d_skin_init(r3d_skin_t *sk, const r3d_model_t *m);
void r3d_skin_free(r3d_skin_t *sk);

/* 用动画状态采样 node TRS → 世界矩阵 → 调色板 → 蒙皮顶点 */
void r3d_skin_update(r3d_skin_t *sk, const r3d_model_t *m,
                     r3d_anim_state_t *st);

#ifdef __cplusplus
}
#endif
#endif
