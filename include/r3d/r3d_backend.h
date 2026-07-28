/*
 * r3d_backend.h — 渲染后端抽象接口 RHI（Render Hardware Interface）
 * 对应架构文档 §3。所有渲染调用经此分发，上层不直接调 vg_lite_ 或 gl 接口。
 */
#ifndef R3D_BACKEND_H
#define R3D_BACKEND_H

#include "r3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 能力查询 ---- */
typedef enum {
    R3D_FEATURE_PERSPECTIVE_TEXTURE = 0, /* 透视校正贴图 */
    R3D_FEATURE_ZBUFFER,                 /* 硬件深度缓冲 */
    R3D_FEATURE_PER_PIXEL_LIGHT,         /* 逐像素光照(shader) */
    R3D_FEATURE_BLEND_MULTIPLY,          /* MULTIPLY 混合(读 dst) */
    R3D_FEATURE_MSAA,
    R3D_FEATURE_SHADOW_MAP,
    R3D_FEATURE_COUNT
} r3d_feature_t;

/* ---- 后端配置 ---- */
typedef struct {
    uint32_t max_triangles;  /* 帧内三角形队列上限，0=默认 */
    uint32_t max_vertices;   /* 帧内顶点缓冲上限，0=默认 */
} r3d_backend_cfg_t;

/* ---- 引擎侧逐帧各阶段 CPU 耗时(微秒)，经 perf_frame_mark 传给后端 ----
 * 这些阶段发生在引擎层(后端看不到)，补齐后端已有的 collect/sort/submit/gpu，
 * 使一帧的 CPU 链路可完整量化。语义组合：
 *   t_loop = wait + t_frame + pan
 *   t_frame ⊇ anim + node + deform + collect + sort + submit + gpu (+杂项)
 * 即 anim/node/deform 是 t_frame 的子区间；wait 在 t_frame 前，pan 在其后。 */
typedef struct {
    long wait_us;    /* 双缓冲 poll(POLLOUT) 等空闲缓冲(vsync 相关空闲等待) */
    long anim_us;    /* r3d_anim_update：关键帧采样+混合 */
    long node_us;    /* 逐 DYNAMIC_NODE submesh 的 r3d_anim_node_matrix 累计 */
    long deform_us;  /* CPU 顶点变形(morph/skin) */
    long pan_us;     /* FBIOPAN_DISPLAY 翻页(上一帧值，见 perf_frame_mark 说明) */
} r3d_engine_perf_t;

typedef struct r3d_backend r3d_backend_t;

/* ---- 后端 vtable（9 方法）---- */
typedef struct r3d_backend_vtable {
    /* 生命周期 */
    r3d_result_t (*init)(r3d_backend_t *self, const r3d_backend_cfg_t *cfg);
    void         (*destroy)(r3d_backend_t *self);

    /* 资源 */
    r3d_texture_handle_t (*create_texture)(r3d_backend_t *self, const r3d_image_t *img);
    void                 (*destroy_texture)(r3d_backend_t *self, r3d_texture_handle_t h);

    /* 帧 */
    void (*begin_frame)(r3d_backend_t *self, const r3d_target_t *target);
    void (*set_camera)(r3d_backend_t *self, const r3d_camera_t *cam);

    /* 光照参数（运行时可调，NULL=恢复默认）。可选，不支持的后端置 NULL。 */
    void (*set_lighting)(r3d_backend_t *self, const r3d_light_params_t *lp);

    /* 绘制一个可渲染对象（draw call）——核心调用 */
    void (*draw)(r3d_backend_t *self,
                 const r3d_mesh_t     *mesh,
                 const r3d_mat4_t     *model,
                 const r3d_material_t *material);

    /* 结束帧：完成绘制（投影/剔除/排序/flush），不含上屏 */
    void (*end_frame)(r3d_backend_t *self);

    /* 呈现（可选，宿主模式为 NULL）*/
    void (*present)(r3d_backend_t *self);

    /* 能力查询 */
    bool (*query_feature)(r3d_backend_t *self, r3d_feature_t feat);

    /* 性能：由引擎在每帧绘制提交前调用，把引擎侧测得的各阶段 CPU 耗时
     * (微秒)喂给后端，使其并入后端统一的逐帧原始性能记录与串口转储。
     * 可选，后端不支持则置 NULL；引擎调用前需判空。
     * 约定：应在 begin_frame 之后、end_frame 之前调用(此时 wait/anim/node/
     * deform 均已知)；end_frame 消费该值写入当前帧记录，随后清零。
     * pan_us 为“上一帧”的翻页耗时(翻页发生在 end_frame 之后，本帧的还未知)，
     * 故按 1 帧延迟携带，离线分析时注意此语义。 */
    void (*perf_frame_mark)(r3d_backend_t *self, const r3d_engine_perf_t *ep);

    /* 性能：切换/载入新模型时调用。后端会先把上一个模型残留(未到转储周期)的
     * 逐帧数据转储出来(归属上一个模型)，再打印一行 syslog 模型标记
     * "r3d_vgl: perf model=<name>"，并把帧号清零、清空环形缓冲与统计窗口。
     * 这样每个 b3dm 的逐帧数据自成一段(帧号从 0 起、互不串扰)，离线工具可精确
     * 按模型分类。可选，后端不支持则置 NULL；引擎调用前需判空。 */
    void (*perf_model_begin)(r3d_backend_t *self, const char *name);
} r3d_backend_vtable_t;

struct r3d_backend {
    const r3d_backend_vtable_t *vt;
    void                       *impl;
};

/* 工厂：编译期选后端 */
r3d_backend_t *r3d_backend_create(void);
void           r3d_backend_destroy(r3d_backend_t *be);

/* 各后端构造函数 */
r3d_backend_t *r3d_backend_null_create(void);
#if defined(R3D_BACKEND_VGLITE)
r3d_backend_t *r3d_backend_vglite_create(void);
/* 宿主模式构造：vg_lite 由外部(gpu_init/LVGL)初始化，后端不 init/close GPU。 */
r3d_backend_t *r3d_backend_vglite_create_hosted(void);
#endif
#if defined(R3D_BACKEND_OPENGL)
r3d_backend_t *r3d_backend_opengl_create(void);
bool           r3d_opengl_should_close(r3d_backend_t *be);
typedef struct {
    double mouse_x, mouse_y;
    int    left, right, middle;
    double scroll;
} r3d_opengl_input_t;
void           r3d_opengl_poll_input(r3d_backend_t *be, r3d_opengl_input_t *in);
#endif

#ifdef __cplusplus
}
#endif
#endif /* R3D_BACKEND_H */
