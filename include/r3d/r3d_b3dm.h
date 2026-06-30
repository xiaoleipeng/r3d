/*
 * r3d_b3dm.h — B3DM 二进制资产格式定义（离线工具与运行时共享）
 * 对应架构文档 §4.6。小端、显式对齐、section 16 字节对齐。
 */
#ifndef R3D_B3DM_H
#define R3D_B3DM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R3D_B3DM_MAGIC   0x4D443342u  /* 'B','3','D','M' 小端 */
#define R3D_B3DM_VERSION 5   /* v5: 支持 32 位索引(flag IDX32，顶点数>65535)；v4: VTXCOLOR 段；v3: MORPH vertex_base */
#define R3D_B3DM_VERSION_MIN 3   /* 运行时可加载的最低版本(v3 无 VTXCOLOR 段，向后兼容) */

/* Header flags 位 */
enum {
    R3D_B3DM_FLAG_ANIM       = 1u << 0,
    R3D_B3DM_FLAG_SKELETON   = 1u << 1,
    R3D_B3DM_FLAG_BAKED_LIGHT= 1u << 2,
    R3D_B3DM_FLAG_DROP_Z     = 1u << 3,
    R3D_B3DM_FLAG_MORPH      = 1u << 4,
    R3D_B3DM_FLAG_SKIN       = 1u << 5,
    R3D_B3DM_FLAG_VTXCOLOR   = 1u << 6,  /* 存在 VTXCOLOR 段：逐顶点烘焙色 */
    R3D_B3DM_FLAG_IDX32      = 1u << 7,  /* INDEX 段为 32 位索引(顶点数>65535)；否则 16 位 */
};

/* Section 类型 */
typedef enum {
    R3D_SEC_VERTEX   = 1,
    R3D_SEC_INDEX    = 2,   /* 默认 uint16；header flag IDX32 置位时为 uint32(顶点数>65535) */
    R3D_SEC_SUBMESH  = 3,
    R3D_SEC_TEXTURE  = 4,
    R3D_SEC_ANIM     = 5,
    R3D_SEC_SKELETON = 6,
    R3D_SEC_MORPH    = 7,
    R3D_SEC_NODE     = 8,   /* node 树（骨骼动画用）*/
    R3D_SEC_SKINVTX  = 9,   /* 每顶点 joints[4]+weights[4] */
    R3D_SEC_VTXCOLOR = 10,  /* 每顶点烘焙色 BGRA8888（去纹理材质模式用）*/
} r3d_b3dm_sec_type_t;

#pragma pack(push, 1)

/* Header（64 字节固定）*/
typedef struct {
    uint32_t magic;            /* 0  */
    uint16_t version;          /* 4  */
    uint16_t flags;            /* 6  */
    uint32_t section_count;    /* 8  */
    float    bounding_sphere[4];/* 12: cx,cy,cz,r */
    float    vertex_scale[3];  /* 28 */
    float    vertex_bias[3];   /* 40 */
    uint8_t  reserved[12];     /* 52..63 */
} r3d_b3dm_header_t;           /* = 64 */

/* Section Table 条目（16 字节）*/
typedef struct {
    uint32_t type;     /* r3d_b3dm_sec_type_t */
    uint32_t offset;   /* 相对文件起始，16 对齐 */
    uint32_t size;     /* 段字节数 */
    uint32_t count;    /* 元素个数 */
} r3d_b3dm_section_t;          /* = 16 */

/* VERTEX 段每顶点（默认 16 字节：定点 pos + uv + oct 法线）*/
typedef struct {
    int16_t  pos[3];   /* 定点，经 header scale/bias 还原 */
    uint16_t uv[2];    /* 归一化 /65535 */
    int16_t  normal_oct[2];
    uint8_t  pad[2];
} r3d_b3dm_vertex_t;           /* = 16 */

/* SUBMESH 段（20 字节）*/
typedef struct {
    uint16_t tex_id;       /* 0xFFFF=无 */
    uint16_t matcap_id;    /* 0xFFFF=无 */
    uint8_t  blend;
    uint8_t  mat_flags;
    uint16_t node_id;      /* 动态 submesh 所属 node(mat_flags bit4=dynamic) */
    uint32_t index_offset; /* 在 INDEX 段内起始索引(元素下标，非字节) */
    uint32_t index_count;
    uint32_t base_color_factor; /* ARGB，纯色材质的颜色 */
} r3d_b3dm_submesh_t;          /* = 20 */

/* TEXTURE 段表头（16 字节）+ 紧随像素数据 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  format;       /* r3d_pixel_format_t */
    uint8_t  pad[3];
    uint32_t data_offset;  /* 像素数据相对文件起始偏移 */
    uint32_t data_size;
} r3d_b3dm_texture_t;          /* = 16 */

/* ---- ANIMATION 段 ---- */
/* 通道 path 类型 */
enum { R3D_ANIM_PATH_T=0, R3D_ANIM_PATH_R=1, R3D_ANIM_PATH_S=2, R3D_ANIM_PATH_W=3 };

/* ANIM 段开头：anim 段头（段内偏移均相对 ANIM 段起始）*/
typedef struct {
    uint32_t clip_count;
    uint32_t clips_offset;   /* r3d_b3dm_clip_t[clip_count] */
} r3d_b3dm_anim_header_t;

typedef struct {
    char     name[32];
    float    duration;       /* 秒 */
    float    fps;            /* 重采样步长 */
    uint32_t channel_count;
    uint32_t channels_offset;/* r3d_b3dm_channel_t[channel_count]，段内相对 */
} r3d_b3dm_clip_t;

typedef struct {
    uint16_t target_node;    /* 作用 node（M5 第一版固定 0=根）*/
    uint8_t  path;           /* R3D_ANIM_PATH_* */
    uint8_t  comp;           /* 每帧分量数：T/S=3，R=4，W=morph 数 */
    uint32_t frame_count;
    uint32_t data_offset;    /* float[frame_count*comp]，段内相对 */
} r3d_b3dm_channel_t;

/* ---- SKELETON 段 ---- */
typedef struct {
    uint32_t joint_count;
    uint32_t inverse_bind_offset; /* float[joint_count*16]，段内相对 */
    uint32_t joint_node_offset;   /* uint16[joint_count]，段内相对 */
} r3d_b3dm_skeleton_t;

/* ---- MORPH 段 ---- */
/* 段开头 morph 头 + 紧随 target_count 组 delta（每组 vertex_count*3 float）。
 * delta 仅覆盖 morph 所属 submesh 的顶点(vertex_count 个)，这些顶点在合并顶点
 * 缓冲中从 vertex_base 开始连续排布。运行时把第 i 个 delta 加到全局顶点
 * (vertex_base + i) 上。 */
typedef struct {
    uint32_t target_count;
    uint32_t vertex_count;    /* morph 覆盖的顶点数(= 所属 submesh 顶点数) */
    uint32_t deltas_offset;   /* float[target_count*vertex_count*3]，段内相对 */
    uint32_t vertex_base;     /* 这些顶点在合并顶点缓冲中的起始全局下标 */
} r3d_b3dm_morph_t;

/* ---- NODE 段 ---- 每 node 一条，默认局部 TRS + 父索引 */
typedef struct {
    int16_t  parent;       /* -1 = 根 */
    uint16_t pad;
    float    translation[3];
    float    rotation[4];  /* 四元数 */
    float    scale[3];
} r3d_b3dm_node_t;          /* = 44 */

/* SKINVTX 段：每顶点 4 骨骼索引 + 4 权重(归一 u8) */
typedef struct {
    uint8_t joints[4];
    uint8_t weights[4];    /* /255 */
} r3d_b3dm_skinvtx_t;       /* = 8 */

/* VTXCOLOR 段：每顶点 1 个 BGRA8888 烘焙色（与 VERTEX 段顺序一一对应）。
 * 去纹理材质模式(baked-vertex)下，离线把纹理/baseColor 采样到顶点色，
 * 运行时不再需要纹理，只做顶点色插值。BGRA 顺序与 framebuffer 一致。 */
typedef struct {
    uint8_t bgra[4];
} r3d_b3dm_vtxcolor_t;       /* = 4 */

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif /* R3D_B3DM_H */
