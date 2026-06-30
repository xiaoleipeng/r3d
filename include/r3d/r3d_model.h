/*
 * r3d_model.h — B3DM 模型资产（加载后持有几何/纹理/材质）
 * 对应架构文档 §4.3/4.4。
 */
#ifndef R3D_MODEL_H
#define R3D_MODEL_H

#include "r3d_types.h"
#include "r3d_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    r3d_texture_handle_t base_color;
    r3d_texture_handle_t matcap;
    r3d_blend_t          blend;
    uint32_t             mat_flags;
    uint32_t             base_color_factor;
    uint16_t             node_id;
    uint32_t             index_offset;
    uint32_t             index_count;
} r3d_submesh_t;

typedef struct {
    r3d_vertex_t  *vertices;
    uint32_t       vertex_count;
    uint16_t      *indices;
    uint32_t       index_count;

    r3d_submesh_t *submeshes;
    uint32_t       submesh_count;

    r3d_texture_handle_t *textures;
    uint32_t              texture_count;

    r3d_aabb_t     bounds;

    /* morph：base 顶点即 vertices；deltas 指向 raw。
     * deltas 仅覆盖 morph_vertex_count 个顶点(某 submesh)，按 [target][vertex][xyz]
     * 排布；第 i 个 delta 作用到全局顶点 (morph_vertex_base + i)。 */
    const float   *morph_deltas;
    uint32_t       morph_target_count;
    uint32_t       morph_vertex_count;
    uint32_t       morph_vertex_base;

    /* skin（均指向 raw）*/
    const void    *nodes;          /* r3d_b3dm_node_t[node_count] */
    uint32_t       node_count;
    const uint16_t*joint_nodes;    /* [joint_count] */
    const float   *inv_bind;       /* [joint_count*16] */
    uint32_t       joint_count;
    const void    *skinvtx;        /* r3d_b3dm_skinvtx_t[vertex_count] */

    void          *raw;       /* 持有底层 buffer */
    r3d_backend_t *backend;   /* 用于释放纹理 */
} r3d_model_t;

r3d_model_t *r3d_model_load(r3d_backend_t *backend, const char *path);
r3d_model_t *r3d_model_load_mem(r3d_backend_t *backend, void *buf, uint32_t size);
void         r3d_model_free(r3d_model_t *m);

#ifdef __cplusplus
}
#endif
#endif /* R3D_MODEL_H */
