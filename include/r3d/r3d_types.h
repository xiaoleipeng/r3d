/*
 * r3d_types.h — 核心 POD 类型定义（后端无关）
 * 对应架构文档 §3.2。
 */
#ifndef R3D_TYPES_H
#define R3D_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 结果码 ---- */
typedef enum {
    R3D_OK = 0,
    R3D_ERR_INVALID_ARG = -1,
    R3D_ERR_BAD_FORMAT  = -2,
    R3D_ERR_IO          = -3,
    R3D_ERR_NO_MEM      = -4,
    R3D_ERR_UNSUPPORTED = -5,
} r3d_result_t;

/* ---- 数学类型 ---- */
typedef struct { float x, y; }       r3d_vec2_t;
typedef struct { float x, y, z; }    r3d_vec3_t;
typedef struct { float x, y, z, w; } r3d_vec4_t;
typedef struct { float x, y, z, w; } r3d_quat_t;

/* 4x4 矩阵，列主序（m[col][row]），与 glTF/OpenGL 一致 */
typedef struct { float m[16]; } r3d_mat4_t;

typedef struct { r3d_vec3_t min, max; } r3d_aabb_t;

typedef struct { int32_t x, y, w, h; } r3d_viewport_t;

/* ---- 像素格式 ---- */
typedef enum {
    R3D_FMT_ARGB8888 = 0,   /* 预乘，主格式 */
    R3D_FMT_RGB565   = 1,
    R3D_FMT_BGRA8888 = 2,   /* NuttX FB_FMT_RGB32 真机帧缓冲常见序 (B,G,R,A) */
} r3d_pixel_format_t;

/* ---- 顶点（运行时，解码后）---- */
typedef struct {
    r3d_vec3_t pos;
    r3d_vec2_t uv;
    r3d_vec3_t normal;
    float      ao;       /* 环境光遮蔽 [0,1]，1=无遮蔽 */
} r3d_vertex_t;

/* ---- 待上传的位图（create_texture 入参）---- */
typedef struct {
    const void        *data;
    uint32_t           w, h;
    uint32_t           stride;
    r3d_pixel_format_t format;
    uint32_t           size;
} r3d_image_t;

/* 后端纹理句柄（不透明） */
typedef uintptr_t r3d_texture_handle_t;
#define R3D_TEXTURE_NONE ((r3d_texture_handle_t)0)

/* ---- 混合模式 ---- */
typedef enum {
    R3D_BLEND_SRC_OVER = 0,
    R3D_BLEND_ADDITIVE = 1,
    R3D_BLEND_MULTIPLY = 2,
    R3D_BLEND_SCREEN   = 3,
} r3d_blend_t;

/* ---- 材质 ---- */
typedef enum {
    R3D_MAT_DOUBLE_SIDED = 1u << 0,
    R3D_MAT_FLAT_SHADING = 1u << 1,
    R3D_MAT_USE_MATCAP   = 1u << 2,
    R3D_MAT_TRANSLUCENT  = 1u << 3,
    R3D_MAT_DYNAMIC_NODE = 1u << 4,
} r3d_material_flags_t;

typedef struct {
    r3d_texture_handle_t base_color;
    r3d_texture_handle_t matcap;
    r3d_blend_t          blend;
    uint32_t             flags;            /* r3d_material_flags_t 位组合 */
    uint32_t             base_color_factor; /* ARGB 调制 */
} r3d_material_t;

/* ---- 网格视图（draw 入参，指向 model 内的数据）---- */
typedef struct {
    const r3d_vertex_t *vertices;
    uint32_t            vertex_count;
    const uint16_t     *indices;
    uint32_t            index_count;
    r3d_aabb_t          bounds;
    int                 dynamic;  /* 1=顶点每帧变化(morph/skin)，后端总是重传 */
} r3d_mesh_t;

/* ---- 相机 ---- */
typedef struct {
    r3d_mat4_t     view;
    r3d_mat4_t     proj;
    r3d_viewport_t viewport;
} r3d_camera_t;

/* ---- 渲染目标（由宿主提供，r3d 不分配）---- */
typedef struct {
    void              *pixels;
    uint32_t           w, h;
    uint32_t           stride;
    r3d_pixel_format_t format;
    uintptr_t          phys_addr;       /* 硬件 GPU 用的物理地址(framebuffer)；
                                         * 0 表示与 pixels 相同/软件实现不需要 */
    void              *native_surface;  /* 可选：EGL/Vulkan surface，宿主模式为 NULL */
} r3d_target_t;

#ifdef __cplusplus
}
#endif
#endif /* R3D_TYPES_H */
