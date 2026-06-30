/*
 * r3d_anim.h — 运行时动画（架构文档 §11）
 * M5 第一版：TRS 节点动画 + 播放器池 + 混合。单根节点驱动 root_matrix。
 */
#ifndef R3D_ANIM_H
#define R3D_ANIM_H

#include "r3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R3D_MAX_PLAYERS 4
#define R3D_MAX_MORPH   64   /* 支持 ARKit 52 套 blendshape 等(facecap)，留余量 */

typedef struct {
    uint16_t      target_node;
    uint8_t       path;        /* R3D_ANIM_PATH_* */
    uint8_t       comp;
    uint32_t      frame_count;
    const float  *frames;      /* [frame_count*comp]，指向 ANIM 段 */
} r3d_anim_chan_t;

typedef struct {
    char                   name[32];
    float                  duration, fps;
    uint32_t               channel_count;
    const r3d_anim_chan_t *channels;
} r3d_anim_clip_t;

typedef struct {
    r3d_anim_clip_t  *clips;
    r3d_anim_chan_t  *chan_pool;
    uint32_t          clip_count;
} r3d_anim_set_t;

typedef struct {
    const r3d_anim_clip_t *clip;
    float time, weight, speed;
    bool  loop, active;
} r3d_anim_player_t;

typedef struct {
    r3d_anim_player_t players[R3D_MAX_PLAYERS];
    r3d_mat4_t        root_matrix;
    float             morph_weights[R3D_MAX_MORPH];
    uint32_t          morph_weight_count;
} r3d_anim_state_t;

void r3d_anim_set_parse(r3d_anim_set_t *set, const void *raw);
void r3d_anim_set_free(r3d_anim_set_t *set);

void r3d_anim_state_init(r3d_anim_state_t *st);
int  r3d_anim_play(r3d_anim_state_t *st, const r3d_anim_set_t *set, const char *name, bool loop);
void r3d_anim_stop(r3d_anim_state_t *st, const char *name);
void r3d_anim_set_weight(r3d_anim_state_t *st, const char *name, float w);

void r3d_anim_update(r3d_anim_state_t *st, float dt);

/* 算指定 node 的动画世界矩阵（NODE 段默认 TRS + anim 采样覆盖 + 父链累乘）。
 * model 透传为 void* 避免头依赖循环。out 写列主序 mat4。*/
void r3d_anim_node_matrix(const void *model, r3d_anim_state_t *st,
                          uint16_t node_id, r3d_mat4_t *out);

#ifdef __cplusplus
}
#endif
#endif
