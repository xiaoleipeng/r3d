# glTF 3D 渲染引擎技术架构设计

配套文档：可行性评估见 `3d_gltf_rendering_evaluation.md`。本文档承接其结论，给出可落地的技术架构。

技术路线：**离线把 glTF 编译为二进制 B3DM，运行时极速加载 + 渲染**；架构上**渲染后端可替换**——当前用 VGLite，未来切到 OpenGL 时后端能快速适配，上层零改动。

---

## 1. 设计目标与范围

### 1.1 目标

| 目标 | 说明 |
|------|------|
| 二进制资产 | glTF 离线编译为 B3DM，运行时免 JSON 解析、免纹理解码 |
| 后端可替换 | 渲染后端隔离在抽象接口后，VGLite ↔ OpenGL 切换不影响上层 |
| 性能优先 | 凡与"当前帧/视角"无关的计算全部离线；运行时只剩投影/剔除/排序/提交 |
| 视觉最优 | 烘焙资产（albedo/matcap/AO）内嵌，加载即最佳画质 |
| 资源可控 | 面数、纹理尺寸、内存在编译期可约束（适配 best1600） |

### 1.2 范围

**包含**：资产格式（B3DM）、离线转换工具（gltf2b3dm）、运行时加载器、场景/动画驱动、渲染后端抽象接口（RHI）、VGLite 后端实现、OpenGL 后端适配预案、跨后端兼容性策略、LVGL 渲染管线融入。

**不包含**：编辑器/DCC 插件、物理、音频、网络。光照/材质的近似手段（matcap/烘焙/平面阴影）沿用评估文档第 7、8 节，本文只描述其在架构中的接入点。

### 1.3 核心设计原则

1. **后端可替换是第一约束**——所有渲染调用必须经过抽象接口 RHI，上层（场景、动画、资产）不得直接调用 `vg_lite_*` 或 `gl*`。
2. **抽象边界画在"可渲染对象"层级**——上层向后端提交的是"网格 + 变换 + 材质"这类语义对象，**不是**已投影的 2D 三角形。原因：投影/剔除/排序/深度处理在不同后端差异极大（VGLite 走 CPU + Painter's，OpenGL 走 GPU + z-buffer），必须由后端各自决定，否则抽象会泄漏。
3. **离线优先**——能在 gltf2b3dm 阶段算的，绝不放运行时。
4. **数据与逻辑分离**——B3DM 是纯数据（POD），加载后即用；渲染逻辑全在后端。
5. **C 语言 + vtable 多态**——面向嵌入式，用函数指针表实现后端多态，零 C++ 依赖、零虚函数开销顾虑、可静态裁剪。

---

## 2. 整体分层架构

### 2.1 分层图

```
┌──────────────────────────────────────────────────────────┐
│  应用层 (App)                                              │
│   创建 viewer、加载模型、设置相机、驱动动画                  │
└───────────────────────────┬──────────────────────────────┘
                            │ 只依赖 r3d 公共 API
┌───────────────────────────▼──────────────────────────────┐
│  引擎层 (r3d core)                                         │
│  ┌────────────┐ ┌────────────┐ ┌────────────────────┐    │
│  │ 资产层      │ │ 场景层      │ │ 动画/蒙皮驱动        │    │
│  │ B3DM loader│ │ node 树     │ │ TRS/morph/skin 更新 │    │
│  │ 纹理/网格   │ │ 相机/裁剪   │ │                    │    │
│  └─────┬──────┘ └─────┬──────┘ └─────────┬──────────┘    │
│        └──────────────┴──────────────────┘                │
│                       │ 提交可渲染对象 (mesh+matrix+material)│
│        ┌──────────────▼───────────────┐                   │
│        │  渲染后端抽象接口 RHI          │  ← 架构的核心边界  │
│        │  (r3d_backend vtable)         │                   │
│        └──────────────┬───────────────┘                   │
└───────────────────────┼───────────────────────────────────┘
                        │ 函数指针分发
        ┌───────────────┴────────────────┐
        ▼                                 ▼
┌──────────────────┐            ┌──────────────────┐
│ VGLite 后端       │            │ OpenGL 后端 (未来) │
│ CPU 投影+Painter  │            │ GPU 投影+z-buffer │
│ draw_pattern      │            │ VBO/shader/FBO    │
└──────────────────┘            └──────────────────┘
```

### 2.2 各层职责

| 层 | 职责 | 关键约束 |
|----|------|---------|
| **应用层** | 业务逻辑：加载哪个模型、相机怎么动、播放哪段动画 | 只用 r3d 公共 API，不碰后端 |
| **资产层** | 加载/持有 B3DM：网格、纹理、材质表、动画数据 | 纯数据；纹理上传委托给后端 |
| **场景层** | node 树、世界矩阵累乘、相机、视锥/包围球剔除的输入 | 不做投影（投影是后端的事） |
| **动画/蒙皮** | 每帧更新 node TRS、morph 权重、骨骼矩阵 | 输出更新后的矩阵/顶点，交后端渲染 |
| **RHI 抽象** | 定义后端能力的统一契约（vtable） | 唯一允许跨越到后端的边界 |
| **后端实现** | 把"可渲染对象"变成屏幕像素 | 投影/剔除/排序/深度策略由后端自定 |

### 2.3 数据流总览

```
[离线] glTF ──gltf2b3dm──→ model.b3dm
                                │
[运行时] 加载: b3dm → r3d_model (mesh/tex/material/anim)
                                │
         每帧:
           1. 动画更新 node TRS / morph / skin   (引擎层)
           2. 场景累乘世界矩阵                     (引擎层)
           3. begin_frame + set_camera（相机每帧设一次）
           4. 对每个可见 submesh:
              backend->draw(mesh, world_matrix, material)
                                │
           5. backend->end_frame() ← 后端内部完成投影/剔除/排序/绘制
                                │
                            屏幕
```

关键：第 4 步上层提交的是**语义对象**（哪个网格、在哪、什么材质），相机已在第 3 步通过 `set_camera` 设好；第 5 步后端才决定**怎么画**。VGLite 后端在 end_frame 里做 CPU 投影 + Painter's 排序 + 逐三角形 `draw_pattern`；OpenGL 后端在 draw 里灌 VBO + 设 shader uniform + `glDrawElements`，靠 z-buffer 解决深度。上层完全不感知差异。

---

## 3. 渲染后端抽象接口 (RHI) 设计

这是整个架构的核心。RHI 定义了"上层能让后端做什么"的统一契约，后端的差异全部封死在接口之下。

> 命名说明：**RHI（Render Hardware Interface，渲染硬件接口）** 是行业通用术语——Unreal Engine、Qt、NVIDIA(NVRHI) 均用此名指代"上层渲染代码与具体图形 API 之间的抽象层"。本文 RHI 的方法命名也对齐主流约定：`create_texture`/`destroy_texture`（资源创建动词在前，同 Vulkan `vkCreate*` / WebGPU `createTexture`）、`draw`（提交绘制，同 sokol_gfx/WebGPU/Metal）、`begin_frame`/`end_frame`（帧边界，同 Unreal RHI）、`present`（呈现，同 WebGPU/Metal/Vulkan）、`query_feature`（能力查询，同时对齐平台 VGLite 自身的 `vg_lite_query_feature`）。

### 3.1 抽象边界放在哪里——关键决策

候选了三种边界，最终选 **C**：

| 方案 | 抽象边界 | 问题 / 优势 |
|------|---------|------------|
| A | 后端直接收"已投影 2D 三角形" | ❌ 抽象泄漏：投影/剔除/排序被迫放上层，OpenGL 后端拿到 2D 三角形反而没法用 z-buffer |
| B | 后端收"原始 glTF 节点" | ❌ 太重：后端要懂 glTF 语义，且每个后端重复实现资产解析 |
| **C** | 后端收**可渲染对象**（mesh + 世界矩阵 + 材质；相机经 set_camera 每帧设一次） | ✅ 上层只描述"画什么、在哪、什么材质"；后端自决"怎么画" |

选 C 的根本原因：**投影、背面剔除、深度排序、深度处理这四件事，在 VGLite 和 OpenGL 上的做法完全不同**——

| 步骤 | VGLite 后端 | OpenGL 后端 |
|------|------------|------------|
| 投影 | CPU 算 MVP，逐顶点透视除法 | 顶点着色器在 GPU 算 |
| 背面剔除 | CPU 投影后叉积判断 | `glEnable(GL_CULL_FACE)` |
| 深度排序 | CPU Painter's 算法排序三角形 | 不需要，z-buffer 硬件解决 |
| 深度处理 | 无 z-buffer，靠排序 | `glEnable(GL_DEPTH_TEST)` |

如果把这些放在上层（方案 A），等于强迫 OpenGL 后端接受一套为 CPU 渲染设计的流程，z-buffer 等硬件能力全废。放在边界之下（方案 C），各后端用最优策略，上层无感。

### 3.2 接口契约（C + vtable）

```c
/* ---- 传给后端的语义数据（后端无关） ---- */

typedef struct {
    const r3d_vertex_t *vertices;   /* 顶点：3D pos + uv (+ 量化 normal) */
    uint32_t            vertex_count;
    const uint16_t     *indices;    /* 已按 material 分组排序 */
    uint32_t            index_count;
    r3d_aabb_t          bounds;     /* 包围盒，供剔除 */
} r3d_mesh_t;

typedef struct {
    r3d_texture_handle_t base_color; /* 后端纹理句柄（上传后获得） */
    r3d_texture_handle_t matcap;     /* 可选，0 表示无 */
    r3d_blend_t          blend;      /* SRC_OVER / ADDITIVE / ... */
    r3d_material_flags_t flags;      /* double_sided / flat_shading / use_matcap ... */
    uint32_t             base_color_factor; /* ARGB 调制 */
} r3d_material_t;

typedef struct {
    r3d_mat4_t view;
    r3d_mat4_t proj;
    r3d_viewport_t viewport;
} r3d_camera_t;

/* ---- 渲染目标：由宿主提供，r3d 不分配其内存 ---- */
typedef struct {
    void     *pixels;   /* 像素内存，宿主拥有（LVGL draw_buf / canvas / fb 驱动） */
    uint32_t  w, h;
    uint32_t  stride;   /* 行字节数 */
    r3d_pixel_format_t format;  /* ARGB8888 等 */
    void     *native_surface;   /* 可选：原生表面句柄，独立模式下宿主提供
                                 * EGL=EGLNativeWindow，Vulkan=VkSurfaceKHR；
                                 * 宿主模式(画进 LVGL buffer)下为 NULL */
} r3d_target_t;

/* ---- 后端 vtable：所有后端必须实现 ---- */

typedef struct r3d_backend_vtable {
    /* 生命周期 */
    r3d_result_t (*init)(r3d_backend_t *self, const r3d_backend_cfg_t *cfg);
    void         (*destroy)(r3d_backend_t *self);

    /* 资源：纹理上传/释放（后端把数据搬进自己的 GPU 可访问内存） */
    r3d_texture_handle_t (*create_texture)(r3d_backend_t *self,
                                           const r3d_image_t *img);
    void                 (*destroy_texture)(r3d_backend_t *self,
                                            r3d_texture_handle_t h);

    /* 帧 */
    void (*begin_frame)(r3d_backend_t *self, const r3d_target_t *target);
    void (*set_camera)(r3d_backend_t *self, const r3d_camera_t *cam);

    /* 绘制一个可渲染对象（draw call）——核心调用 */
    void (*draw)(r3d_backend_t *self,
                 const r3d_mesh_t     *mesh,
                 const r3d_mat4_t     *model,     /* 世界矩阵 */
                 const r3d_material_t *material);

    /* 结束帧：完成绘制（投影/剔除/排序/flush），**不含上屏** */
    void (*end_frame)(r3d_backend_t *self);

    /* 呈现（可选，仅"自持 surface 的独立模式"实现）：
     * 把 target 显示到物理屏。EGL 后端=eglSwapBuffers，Vulkan=vkQueuePresentKHR。
     * VGLite/宿主模式下为 NULL——上屏由宿主(LVGL flush / fb 驱动)负责。 */
    void (*present)(r3d_backend_t *self);

    /* 能力查询：上层据此决定是否启用某特性 */
    bool (*query_feature)(r3d_backend_t *self, r3d_feature_t cap);
} r3d_backend_vtable_t;

struct r3d_backend {
    const r3d_backend_vtable_t *vt;   /* 指向具体后端的函数表 */
    void                       *impl; /* 后端私有数据 */
};
```

调用方（引擎层）永远只通过 `backend->vt->draw(...)` 这类间接调用，编译期不依赖任何具体后端。

### 3.3 能力查询机制

不同后端能力不同（评估文档已确认 best1600 VGLite 的诸多限制）。上层用 `query_feature` 在运行时探测，而非硬编码：

```c
typedef enum {
    R3D_FEATURE_PERSPECTIVE_TEXTURE,  /* 透视校正贴图 */
    R3D_FEATURE_ZBUFFER,              /* 硬件深度缓冲 */
    R3D_FEATURE_PER_PIXEL_LIGHT,      /* 逐像素光照(shader) */
    R3D_FEATURE_BLEND_MULTIPLY,       /* MULTIPLY 混合(需读dst) */
    R3D_FEATURE_MSAA,
    R3D_FEATURE_SHADOW_MAP,
} r3d_feature_t;
```

| 能力 | VGLite(best1600) | OpenGL |
|------|:---:|:---:|
| `PERSPECTIVE_TEXTURE` | ✅（需填透视行） | ✅ |
| `ZBUFFER` | ❌（靠 Painter's） | ✅ |
| `PER_PIXEL_LIGHT` | ❌ | ✅ |
| `BLEND_MULTIPLY` | ⚠️（USE_DST=0 需实测） | ✅ |
| `SHADOW_MAP` | ❌（用平面投影阴影） | ✅ |

上层据此分支：例如 `query_feature(ZBUFFER)` 为假时，引擎层知道半透明物体需要额外注意提交顺序；为真时则放心交给后端。但**注意**：剔除/排序本身仍由后端做，能力查询只用于上层的高层决策（如是否启用某视觉特性），不把渲染逻辑拉回上层。

### 3.4 为什么用 C + vtable 而非 C++ 虚函数

| 维度 | C + vtable | C++ 虚函数 |
|------|-----------|-----------|
| 工具链 | 嵌入式 C 编译器全支持 | 部分 RTOS 工具链 C++ 支持弱 |
| 二进制体积 | 可只链接选中的后端 | RTTI/异常可能引入额外开销 |
| 可控性 | 函数表显式可见，易调试/打桩 | 编译器隐式生成 |
| 静态裁剪 | 未用的后端不编译进固件 | 需依赖 LTO |

编译期通过宏选择后端，未启用的后端代码完全不进固件：

```c
r3d_backend_t *r3d_backend_create(void) {
#if defined(R3D_BACKEND_VGLITE)
    return r3d_backend_vglite_create();
#elif defined(R3D_BACKEND_OPENGL)
    return r3d_backend_opengl_create();
#endif
}
```

---

## 4. B3DM 资产格式与加载器

B3DM（Binary 3D Model）是后端无关的二进制资产格式，承接评估文档附录 D 的设计。它是纯数据容器，加载后映射为结构体即用。

### 4.1 设计要点

- **后端无关**：B3DM 只描述几何/纹理/材质/动画的数据，不含任何 VGLite 或 OpenGL 概念。纹理以"待上传的位图"形式存在，由后端的 `create_texture` 决定怎么进显存。
- **零解析加载**：固定布局 + 4/16 字节对齐，load 到 RAM 后直接 `reinterpret` 为结构体指针；支持 XIP 的只读段（顶点/索引）可免拷贝。
- **小端、版本化**：Header 带 magic + version，向后兼容靠 flags 位。

### 4.2 文件布局

```
┌──────────────────────────────────────────┐
│ Header (固定 64B)                          │
│  magic="B3DM" u32 / version u16 / flags u16│
│  bounding_sphere (cx,cy,cz,r) f32×4        │
│  section_table[N]: {type,offset,size}      │
├──────────────────────────────────────────┤
│ Section: VERTEX                            │
│  format flags(定点位宽/有无normal/uv)       │
│  scale/bias (定点还原用) f32×6              │
│  interleaved vertex data                   │
├──────────────────────────────────────────┤
│ Section: INDEX                             │
│  已按 material 分组排序的 uint16 索引        │
├──────────────────────────────────────────┤
│ Section: SUBMESH (material 分组表)          │
│  每条: {tex_id, blend, image_mode, flags,  │
│         index_offset, index_count}         │
├──────────────────────────────────────────┤
│ Section: TEXTURE                           │
│  每张: {w,h,format,offset,size}            │
│  数据已是直采格式(ARGB8888预乘/对齐)         │
│  含 baseColor / matcap / 烘焙图             │
├──────────────────────────────────────────┤
│ Section: ANIMATION (可选)                   │
│  定步长重采样的关键帧: node TRS / morph 权重 │
├──────────────────────────────────────────┤
│ Section: SKELETON (可选)                    │
│  展平的骨骼层级 + 逆绑定矩阵                  │
└──────────────────────────────────────────┘
```

顶点 Z 坐标的取舍（自由视角必存、固定视角可省）见评估文档附录 D.3。

### 4.3 运行时数据结构

加载后映射成引擎层使用的 `r3d_model`，注意它与 RHI 的 `r3d_mesh_t`/`r3d_material_t` 的关系——`r3d_model` 持有数据，每帧从中取出 submesh 拼成 `r3d_mesh_t` 提交：

```c
typedef struct {
    /* 直接指向 mmap/RAM 中的 B3DM 段，零拷贝 */
    const r3d_vertex_t *vertices;
    uint32_t            vertex_count;
    const uint16_t     *indices;

    r3d_submesh_t      *submeshes;    /* material 分组 */
    uint32_t            submesh_count;

    r3d_texture_handle_t *textures;   /* create_texture 后填入的后端句柄 */
    uint32_t              texture_count;

    r3d_anim_t         *animations;   /* 可选 */
    r3d_skeleton_t     *skeleton;     /* 可选 */

    r3d_aabb_t          bounds;
    void               *raw;          /* 持有底层 buffer，用于释放 */
} r3d_model_t;
```

### 4.4 加载流程

```c
r3d_model_t *r3d_model_load(r3d_backend_t *backend, const char *path) {
    /* 1. 读文件到 RAM（或 XIP 映射只读段） */
    void *raw = load_file(path);

    /* 2. 校验 Header magic/version */
    r3d_b3dm_header_t *hdr = raw;
    if (hdr->magic != R3D_B3DM_MAGIC) return NULL;

    /* 3. 段指针直接映射（零解析） */
    r3d_model_t *m = alloc_model();
    map_sections(m, raw, hdr);   /* vertices/indices/submeshes 直接指过去 */

    /* 4. 纹理：唯一需要"上传"的资源，委托后端 */
    for (uint32_t i = 0; i < m->texture_count; i++) {
        r3d_image_t img = read_texture_section(raw, hdr, i);
        m->textures[i] = backend->vt->create_texture(backend, &img);
    }

    m->raw = raw;
    return m;
}
```

关键：**加载器只跟 RHI 的 `create_texture` 打交道**，不含任何后端细节。VGLite 后端把位图搬进 `vg_lite_buffer`，OpenGL 后端调 `glTexImage2D`——加载器都不关心，只拿回一个 `r3d_texture_handle_t`。

### 4.5 与可行性评估的衔接

| 评估文档结论 | 在 B3DM 的体现 |
|-------------|---------------|
| 附录 D：离线编译为二进制 | B3DM 即该格式的具体定义 |
| 附录 D：顶点定点化/法线量化 | VERTEX 段的 format flags + scale/bias |
| 附录 C：烘焙资产内嵌 | TEXTURE 段含 albedo/matcap/AO |
| 附录 A：material 分组提交 | SUBMESH 段离线分好组 |
| 附录 A：动画定步长重采样 | ANIMATION 段 |

### 4.6 精确字节布局

实现 gltf2b3dm 序列化器与运行时加载器的共享契约（`r3d_b3dm.h`）。**小端、struct 不含隐式 padding（显式对齐）、所有 section 起始 16 字节对齐。**

#### Header（64 字节，固定）

| 偏移 | 字段 | 类型 | 说明 |
|:---:|------|------|------|
| 0 | magic | u32 | `'B','3','D','M'` = 0x4D443342（小端） |
| 4 | version | u16 | 格式版本，当前 1 |
| 6 | flags | u16 | bit0=有动画 bit1=有骨架 bit2=已烘焙光照 bit3=drop_z |
| 8 | section_count | u32 | section_table 条目数 |
| 12 | bounding_sphere | f32×4 | cx,cy,cz,r（剔除/排序） |
| 28 | vertex_scale | f32×3 | 定点还原：pos = q·scale + bias |
| 40 | vertex_bias | f32×3 | |
| 52 | reserved | u8×12 | 补齐到 64，未来扩展 |

#### Section Table（紧随 Header，每条 16 字节）

| 偏移 | 字段 | 类型 | 说明 |
|:---:|------|------|------|
| 0 | type | u32 | 1=VERTEX 2=INDEX 3=SUBMESH 4=TEXTURE 5=ANIM 6=SKELETON |
| 4 | offset | u32 | 相对文件起始的字节偏移（16 对齐） |
| 8 | size | u32 | 段字节数 |
| 12 | count | u32 | 段内元素个数（顶点数/索引数/submesh 数…） |

> 加载器遍历 section_table，按 type 把段指针映射到 `r3d_model_t`；**遇到不认识的 type 直接跳过**（前向兼容，§8.1 增强段靠此实现）。

#### VERTEX 段（每顶点，默认 16 字节 / 定点+量化法线）

| 偏移 | 字段 | 类型 | 说明 |
|:---:|------|------|------|
| 0 | pos_x/y/z | s16×3 | 定点坐标，经 Header scale/bias 还原 |
| 6 | uv_u/v | u16×2 | 归一化 UV（/65535） |
| 10 | normal_oct | s16×2 | oct 编码法线（烘焙光照时此段可省，见 flags bit2） |
| 14 | pad | u8×2 | 补齐到 16，保证 4 字节对齐 |

> drop_z（flags bit3）时 pos 退化为 s16×2，每顶点 12 字节——固定视角省 25%。

#### INDEX 段

`uint16 × count`，已按 material 分组排序。count 为奇数时尾部补 1 个 u16 对齐。

#### SUBMESH 段（每条 16 字节）

| 偏移 | 字段 | 类型 | 说明 |
|:---:|------|------|------|
| 0 | tex_id | u16 | TEXTURE 段内索引；0xFFFF=无纹理 |
| 2 | matcap_id | u16 | matcap 纹理索引；0xFFFF=无 |
| 4 | blend | u8 | 0=SRC_OVER 1=ADDITIVE 2=MULTIPLY… |
| 5 | mat_flags | u8 | bit0=double_sided bit1=flat_shading bit2=use_matcap |
| 6 | pad | u8×2 | |
| 8 | index_offset | u32 | 在 INDEX 段内的起始索引 |
| 12 | index_count | u32 | 本组索引数 |

#### TEXTURE 段（表头每条 16 字节 + 紧随的像素数据）

| 偏移 | 字段 | 类型 | 说明 |
|:---:|------|------|------|
| 0 | width | u16 | |
| 2 | height | u16 | |
| 4 | format | u8 | 0=ARGB8888 预乘 1=RGB565… |
| 5 | pad | u8×3 | |
| 8 | data_offset | u32 | 像素数据相对文件起始偏移（16 对齐） |
| 12 | data_size | u32 | 字节数（含行对齐 stride） |

#### ANIMATION / SKELETON 段

可选，由 flags 控制。ANIMATION：定步长重采样的关键帧数组（每 channel: target_node u16 + path u8 + 帧值数组）；SKELETON：joint_count u32 + 逆绑定矩阵 f32×16×count + 父索引 u16×count。详细布局在 M5 里程碑前细化（M1-M4 不依赖）。

> 设计约束：所有多字节字段自然对齐，section 16 对齐——保证 `r3d_model_t` 的指针可直接指向段内、无需拷贝重排（§4.3 零拷贝映射的前提）。

---

## 5. 离线工具链 gltf2b3dm

把 glTF 编译成 B3DM 的离线工具。运行在开发机（有充足算力），是"离线优先"原则的执行者。

### 5.1 职责

把评估文档附录 D.4 中"离线列"的所有工作一次性做完：解析、减面、纹理转换、顶点定点化、material 分组、动画重采样、（可选）静态光照烘焙、多视角预剔除。

### 5.2 命令行

```
gltf2b3dm  input.gltf  output.b3dm  [选项]
  --max-tris 400          # 减面预算（meshoptimizer）
  --tex-size 256          # 纹理降采样上限
  --tex-format argb8888   # GPU 直采格式（预乘）
  --quantize-pos 16       # 顶点定点位宽
  --drop-z                # 固定视角：丢弃 z，只存屏幕 xy（需配合 --views）
  --bake-light <x,y,z>    # 静态光照：预算逐面亮度
  --matcap matcap.png     # 内嵌 matcap
  --anim-fps 30           # 动画重采样步长
  --views 16              # 多视角预剔除+预排序
```

### 5.3 处理流水线

```
input.gltf
   │
   ▼  cgltf 解析 → 内存模型 (node树/mesh/material/animation)
   │
   ▼  几何处理 (meshoptimizer)
   │    ├─ 减面到 --max-tris (meshopt_simplify)
   │    ├─ 顶点 cache 优化 (meshopt_optimizeVertexCache)
   │    └─ 顶点去重/重排 (meshopt_optimizeVertexFetch)
   │
   ▼  纹理处理 (stb_image + stb_image_resize)
   │    ├─ 解码 PNG/JPG
   │    ├─ 降采样到 --tex-size
   │    └─ 转 ARGB8888 预乘 + 对齐
   │
   ▼  顶点编码
   │    ├─ 计算 AABB → scale/bias
   │    ├─ pos 定点化 (--quantize-pos)
   │    ├─ normal oct 编码 (或 --bake-light 时预算亮度后丢弃 normal)
   │    └─ 交错打包 (pos+uv+normal)
   │
   ▼  material 分组
   │    └─ 索引按 (texture, blend, image_mode) 排序分组 → SUBMESH 表
   │
   ▼  动画重采样 (可选)
   │    └─ 关键帧按 --anim-fps 定步长重采样 (slerp 旋转)
   │
   ▼  多视角预处理 (可选, --views)
   │    └─ 每视角: 投影→背面剔除→Painter排序→存一份索引
   │
   ▼  序列化 → 写对齐的二进制
output.b3dm
```

### 5.4 关键实现点

**减面与预算对齐**——`meshopt_simplify` 的目标三角数直接来自 `--max-tris`，确保产物落在评估文档第 4 节的性能甜区（300-500 面）：

```c
size_t target = max_tris * 3;
float  error = 0.f;
size_t got = meshopt_simplify(out_idx, in_idx, in_idx_count,
                              &verts[0].x, vert_count, sizeof(r3d_vertex_t),
                              target, /*target_error*/ 0.01f, /*options*/ 0, &error);
```

**material 分组排序**——离线把同纹理/同混合模式的三角形排到一起，运行时后端直接按 SUBMESH 分批提交，省去每帧分组：

```c
/* 按 (tex_id, blend, image_mode) 作为排序键 */
qsort(tris, tri_count, sizeof(tri_t), cmp_by_material_key);
/* 扫描生成 submesh 区间表 */
build_submesh_ranges(tris, tri_count, &submeshes);
```

**静态光照烘焙**——若指定 `--bake-light`，离线逐面算 flat shading 亮度写入顶点色或调制表，运行时连法线点积都省（评估文档第 7 节方案 B' 的离线版）：

```c
for each triangle:
    vec3 n = face_normal(v0, v1, v2);
    float b = max(dot(n, -light_dir), 0) * 0.7f + 0.3f;
    store_face_brightness(tri, b);   /* 运行时直接用，免算 */
```

### 5.5 工具复用的第三方库

| 库 | 用途 | 形态 |
|----|------|------|
| **cgltf** | glTF 解析 | 单头文件 C |
| **meshoptimizer** | 减面 / cache 优化 / 量化 | C++ 库 |
| **stb_image** | 纹理解码 | 单头文件 C |
| **stb_image_resize** | 纹理降采样 | 单头文件 C |

工具运行在开发机，不受嵌入式约束，可放心用 C++ 的 meshoptimizer。产物 B3DM 才是嵌入式要消费的东西。

---

## 6. 运行时数据流与 VGLite 后端详细设计

本章把第 3 章的抽象接口落到 VGLite 的具体实现，展示"提交语义对象 → 屏幕像素"的完整路径。

### 6.1 每帧主循环（引擎层，后端无关）

```c
void r3d_render_frame(r3d_scene_t *scene, r3d_backend_t *be) {
    /* 1. 动画驱动：推进时间、采样通道 → 更新 node 局部 TRS + morph 权重 */
    r3d_anim_update(scene, dt);

    /* 2. 场景：递归累乘世界矩阵（局部 TRS → world_matrix） */
    r3d_scene_update_world_matrices(scene);

    /* 3. 顶点变形（依赖世界矩阵）：morph 叠加 + 骨骼蒙皮，产出更新后的顶点 */
    r3d_deform_update(scene);

    /* 4. 开帧 + 设相机 */
    be->vt->begin_frame(be, &scene->target);
    be->vt->set_camera(be, &scene->camera);

    /* 5. 遍历可见 node 的每个 submesh，提交语义对象 */
    for each visible node:
        for each submesh in node->model:
            r3d_mesh_t     mesh = make_mesh_view(node->model, submesh);
            r3d_material_t mat  = make_material(node->model, submesh);
            be->vt->draw(be, &mesh, &node->world_matrix, &mat);

    /* 6. 收帧：后端在此完成投影/剔除/排序/绘制（不含上屏） */
    be->vt->end_frame(be);
}
/* 注：present(上屏) 不在此循环内——它是顶层显示循环的事。
 * 宿主模式(画进 LVGL buffer)由宿主 flush 上屏；独立模式由 app 调 be->vt->present。
 * 详见 §6.5。动画三步(1-3)的详细逻辑见第 11 章。 */
```

这段代码对 VGLite 和 OpenGL **完全一致**。差异全在 `draw`/`end_frame` 的后端实现里。

### 6.2 VGLite 后端：draw 阶段

VGLite 后端在 impl 私有数据里维护两块**帧内缓冲**（begin_frame 时复位，end_frame 后回收）：
- **顶点缓冲**：存当前 submesh 投影后的屏幕空间顶点（每次 draw 投影后写入，绘制时读取）
- **三角形队列**：跨所有 draw 累积，存"三角形 + 顶点引用 + material 引用 + 平均深度"，供 end_frame 全局排序

`draw` 不立即绘制，而是把可渲染对象**记录进帧内队列**（因为要先收集全部三角形才能做 Painter's 排序）：

```c
static void vglite_draw(r3d_backend_t *self,
                          const r3d_mesh_t *mesh,
                          const r3d_mat4_t *model,
                          const r3d_material_t *mat) {
    vglite_impl_t *impl = self->impl;

    /* 帧内顶点缓冲：本 submesh 投影结果（容量不足则扩容） */
    screen_vertex_t *screen_v = frame_alloc_vertices(impl, mesh->vertex_count);

    /* MVP = proj × view × model */
    r3d_mat4_t mvp;
    mat4_mul(&mvp, &impl->view_proj, model);

    /* 逐顶点投影到屏幕空间（CPU） */
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        vec4 clip = mat4_mul_vec4(&mvp, mesh->vertices[i].pos);
        float inv_w = 1.0f / clip.w;
        screen_v[i].x = (clip.x*inv_w*0.5f + 0.5f) * vp.w;
        screen_v[i].y = (1.0f - (clip.y*inv_w*0.5f + 0.5f)) * vp.h;
        screen_v[i].z = clip.z * inv_w;         /* 排序用 */
        screen_v[i].inv_w = inv_w;              /* 透视校正贴图用 */
        screen_v[i].uv = mesh->vertices[i].uv;
    }

    /* 逐三角形：背面剔除 + 入队（队列存顶点+material 引用，跨 draw 累积） */
    for each triangle (i0,i1,i2):
        if (!mat->double_sided && backface(screen_v[i0],i1,i2)) continue;
        enqueue_tri(impl, screen_v, i0,i1,i2, mat);  /* 记录平均深度 */
}
```

### 6.3 VGLite 后端：end_frame 阶段

收集完所有三角形后，统一排序并绘制：

```c
static void vglite_end_frame(r3d_backend_t *self) {
    vglite_impl_t *impl = self->impl;

    /* 1. Painter's：全部三角形按平均深度从远到近排序 */
    qsort(impl->tris, impl->tri_count, sizeof(tri_t), cmp_depth_desc);

    /* 2. 逐三角形绘制 */
    for (uint32_t t = 0; t < impl->tri_count; t++) {
        tri_t *tri = &impl->tris[t];

        /* 构建三角形 path */
        build_triangle_path(&impl->path, tri);

        /* UV→屏幕 仿射/透视矩阵 */
        vg_lite_matrix_t pat_mtx;
        if (impl->feat_perspective)
            solve_perspective(&pat_mtx, tri);   /* 填透视行(inv_w) */
        else
            solve_affine(&pat_mtx, tri);

        /* flat shading：MULTIPLY_IMAGE_MODE + 逐面亮度调制色 */
        vg_lite_color_t color = modulate_color(tri->mat, tri->brightness);

        vg_lite_draw_pattern(&impl->target, &impl->path, VG_LITE_FILL_NON_ZERO,
                             &impl->path_mtx, tex_of(tri->mat), &pat_mtx,
                             blend_of(tri->mat), VG_LITE_PATTERN_COLOR,
                             0, color, VG_LITE_FILTER_LINEAR);
    }

    /* 3. 统一 flush */
    vg_lite_flush(&impl->target);
    reset_frame_queue(impl);
}
```

这正是评估文档里确认的 VGLite 能力的集中落地：透视校正贴图（填透视行）、flat shading（MULTIPLY 调制）、material 分组（提交顺序）。

> **透视校正原理**（评估文档第 6 节）：`solve_perspective` 把三角形 3 个顶点的 `inv_w`（1/w，draw 阶段已存）编入 UV→屏幕映射矩阵的底行 `m[2][*]`，VGLite 硬件逐像素插值 w 并做 `x/w` 除法，从而正确透视采样。这与 `solve_affine`（底行写死 0/0/1）的唯一区别就是底行是否携带 inv_w。`impl->feat_perspective` 由 `query_feature(R3D_FEATURE_PERSPECTIVE_TEXTURE)` 初始化时确定。
>
> **帧内缓冲约束**：三角形队列存的是 draw 阶段顶点缓冲的引用，因此顶点缓冲必须存活到 end_frame 绘制完成，begin_frame 时才整体复位——不可在单个 draw 结束后释放。

### 6.4 VGLite 后端：create_texture

```c
static r3d_texture_handle_t vglite_create_texture(r3d_backend_t *self,
                                                  const r3d_image_t *img) {
    vg_lite_buffer_t *buf = alloc_vg_buffer();
    buf->width  = img->w;  buf->height = img->h;
    buf->format = VG_LITE_BGRA8888;   /* 预乘直采 */
    vg_lite_allocate(buf);            /* GPU 可访问内存 */
    copy_aligned(buf->memory, img->data, img->size);  /* B3DM 已是直采格式，仅搬运 */
    return (r3d_texture_handle_t)buf;
}
```

注意：B3DM 的纹理已是直采格式，这里**只搬运不解码**（呼应评估文档附录 D 的修正）。

### 6.5 framebuffer 来源与呈现

**r3d 不分配 framebuffer，渲染目标由宿主提供。** `begin_frame(target)` 收到的 `r3d_target_t` 只是对一块外部像素内存的描述（指针+宽高+stride+格式），其内存生命周期由宿主管理。三种来源：

| 来源 | 分配者 | 对应场景 |
|------|--------|---------|
| LVGL layer buffer (`layer->draw_buf`) | LVGL | 紧耦合融入（§8.5 方式 B） |
| LVGL canvas/image buffer | 应用创建 canvas | 松耦合融入（§8.5 方式 A） |
| 裸 framebuffer | fb 驱动 / 应用 | 不用 LVGL，直接对接显示驱动 |

VGLite 后端在 `begin_frame` 里把 `r3d_target_t` **包成** `vg_lite_buffer`，只设指针不分配内存——与 LVGL 现有的 `lv_vg_lite_buffer_from_draw_buf()` 同理（`buffer->memory = target->pixels`）：

```c
static void vglite_begin_frame(r3d_backend_t *self, const r3d_target_t *t) {
    vglite_impl_t *impl = self->impl;
    /* 包装外部 framebuffer，不分配内存 */
    impl->target.memory = t->pixels;     /* 指向宿主的 buffer */
    impl->target.width  = t->w;
    impl->target.height = t->h;
    impl->target.stride = t->stride;
    impl->target.format = to_vg_format(t->format);
    reset_frame_queue(impl);
}
```

> 后端无关性：OpenGL 后端的 `begin_frame` 则把 target 对应到一个 FBO 或默认帧缓冲（若 target 是屏幕）。无论哪种后端，**framebuffer 的分配权都在宿主，不在 r3d**——这避免了引擎与显示子系统的内存管理冲突，也是融入 LVGL 的前提。

#### 渲染 vs 呈现：两个分开的职责

EGL/Vulkan 的引入暴露出一个需厘清的点——**"画进 target"和"把 target 显示到物理屏"是两件事**：

| 动作 | 职责 | 谁做 |
|------|------|------|
| **render**（draw/end_frame） | 把像素画进 target | r3d 后端，**总是** |
| **present**（上屏/swap） | 把 target 显示到物理屏 | **宿主**；仅独立模式由后端 `present` 做 |

关键推敲：**宿主模式（§8.5 方式 B，r3d 画进 LVGL layer buffer）下 r3d 绝不能 present。** 因为同一 buffer 上还有别的 LVGL widget 在画，上屏必须等 LVGL 把整帧画完后由其 display flush 统一做一次——r3d 画完自己那部分就 swap 会导致过早上屏/撕裂。

因此 `present` 设计为 **vtable 可选方法**：

| 模式 | `end_frame` | `present` |
|------|------------|-----------|
| VGLite + LVGL 宿主 | flush 自己的绘制 | **NULL**——上屏由 LVGL flush 负责 |
| OpenGL + EGL 独立 | glFlush/绘制完成 | `eglSwapBuffers`（用 target.native_surface） |
| Vulkan 独立 | queue submit | `vkQueuePresentKHR` |

并且 **`present` 不进 `r3d_render_frame`（§6.1 每帧主循环）**——它是顶层显示循环的事：独立 app 自己调 `backend->vt->present`，宿主模式下没人调（宿主负责刷新）。EGL 的 context/surface 创建则封装在 OpenGL 后端的 `init`/`begin_frame` 内部，`target.native_surface` 由宿主提供——RHI 核心契约"render into target"不受影响。

### 6.6 关键设计点小结

| 设计点 | 做法 | 为何这样 |
|--------|------|---------|
| draw 不立即画 | 入队，end_frame 统一排序 | Painter's 需全局三角形 |
| 投影在 draw | CPU 逐顶点 MVP | VGLite 无顶点着色器 |
| 排序在 end_frame | 全帧三角形 qsort | 无 z-buffer |
| 透视校正 | solve_perspective 填透视行 | 评估文档确认硬件支持 |
| flat shading | MULTIPLY + 逐面调制色 | 复用 mesh_draw_cb 现有通路 |

---

## 7. OpenGL 后端适配预案

本章证明：同一套 RHI 接口下，换 OpenGL 后端时**上层（应用/资产/场景/动画）零改动**，只需新增一个后端实现文件。这是第 3 章抽象边界设计正确性的验证。

### 7.1 上层完全不变

第 6.1 节的每帧主循环、第 4 章的加载器、第 5 章的 B3DM 资产——全部不动。切换只发生在编译期选择：

```c
/* 编译时 -DR3D_BACKEND_OPENGL，其余代码一行不改 */
r3d_backend_t *be = r3d_backend_create();
r3d_model_t   *m  = r3d_model_load(be, "pet.b3dm");  /* 同一份 B3DM */
```

B3DM 资产也无需重新导出——OpenGL 后端消费同样的顶点/索引/纹理数据，只是处理方式不同。

### 7.2 OpenGL 后端的 vtable 实现

逐个对照 VGLite 实现，体现"同接口、异实现"：

| vtable 方法 | VGLite 实现 | OpenGL 实现 |
|------------|------------|------------|
| `init` | 初始化 vg_lite 上下文 | 创建 GL context，编译 shader 程序 |
| `create_texture` | `vg_lite_allocate` + 搬运 | `glGenTextures`+`glTexImage2D` |
| `begin_frame` | 设 target buffer | 绑 FBO，`glClear` 颜色+深度 |
| `set_camera` | 存 view/proj 矩阵备 CPU 投影 | 存矩阵，待 draw 时设 uniform |
| `draw` | CPU 投影+剔除+入队 | 直接灌 VBO + 设 uniform + `glDrawElements` |
| `end_frame` | qsort 排序 + 逐三角 draw_pattern | `glFlush`（z-buffer 已处理深度） |
| `present` | NULL（宿主 flush 上屏） | `eglSwapBuffers`（独立模式）/ NULL（画进 FBO 给宿主） |
| `query_feature` | ZBUFFER=否, PER_PIXEL_LIGHT=否 | ZBUFFER=是, PER_PIXEL_LIGHT=是 |

### 7.3 draw：OpenGL 的极简实现

OpenGL 后端的 draw 比 VGLite 简单得多——不投影、不剔除、不入队，直接提交给 GPU：

```c
static void gl_draw(r3d_backend_t *self,
                      const r3d_mesh_t *mesh,
                      const r3d_mat4_t *model,
                      const r3d_material_t *mat) {
    gl_impl_t *impl = self->impl;

    /* MVP 交给顶点着色器算，这里只传矩阵 */
    glUniformMatrix4fv(impl->u_mvp, 1, GL_FALSE,
                       mat4_mul_tmp(&impl->view_proj, model));

    /* 上传/绑定该 mesh 的 VBO（首次上传后缓存） */
    bind_or_upload_vbo(impl, mesh);

    /* 材质 → uniform / 纹理绑定 */
    glBindTexture(GL_TEXTURE_2D, (GLuint)mat->base_color);
    set_blend_state(mat->blend);
    if (mat->flags & R3D_MAT_DOUBLE_SIDED) glDisable(GL_CULL_FACE);
    else                                   glEnable(GL_CULL_FACE);

    /* 直接绘制——深度由 z-buffer 解决，无需排序 */
    glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_SHORT, 0);
}
```

`end_frame` 几乎是空的——因为没有"收集后统一排序"的需求：

```c
static void gl_end_frame(r3d_backend_t *self) {
    glFlush();   /* 半透明物体若需排序，可在此对透明队列处理，但不强制 */
}
```

### 7.4 顶点着色器（投影 + flat 光照）

VGLite 在 CPU 做的投影和光照，OpenGL 移到 GPU（下例为 flat shading；matcap 则在片元着色器用法线 xy 采样 matcap 纹理）：

```glsl
/* vertex shader */
uniform mat4 u_mvp;
uniform vec3 u_light_dir;
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec3 a_normal;
varying vec2 v_uv;
varying float v_brightness;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);   /* GPU 投影，透视自动 */
    v_uv = a_uv;
    v_brightness = max(dot(normalize(a_normal), -u_light_dir), 0.0)*0.7 + 0.3;
}
```

OpenGL 还能做 VGLite 做不到的：法线贴图、逐像素 PBR、阴影——`query_feature` 返回 true 后，上层可启用这些特性。但即便不启用，同一份 B3DM 也能正确渲染。

### 7.5 适配工作量评估

| 工作 | 量 | 说明 |
|------|----|----|
| 新增 `backend_opengl.c` | 中 | 实现 9 个 vtable 方法（present 可选） |
| shader 程序 | 小 | 1 个顶点 + 1 个片元 shader |
| VBO/纹理缓存管理 | 小 | 标准 GL 资源管理 |
| 上层改动 | **0** | 一行不改 |
| B3DM 重新导出 | **0** | 资产复用 |

> 结论：抽象边界画在"可渲染对象"层级（第 3.1 节方案 C）使得后端替换是**纯增量**——加一个文件、改一个编译开关。投影/剔除/排序/深度的策略差异被 vtable 完全吸收，这正是架构可扩展性的证明。

### 7.6 迈向完全 3D 的扩展点

第 7.1-7.5 证明了**渲染管线层**换 OpenGL 零障碍。但要发挥 OpenGL 的完整 3D 能力（真 PBR、法线贴图、多光源、高精度大模型），还需补齐**数据层**和**接口层**——因为当前 B3DM 和材质接口是为 best1600 特化裁剪的。

#### 三层评估

| 层 | 换 OpenGL 后 | 说明 |
|----|------------|------|
| **渲染管线** | ✅ 完全支持 | z-buffer / shader / 逐像素光照 / 透视，RHI 接口已足够 |
| **数据(B3DM)** | ⚠️ 需扩展 | 当前为省而砍掉的信息不可逆，OpenGL 有能力也无米下锅 |
| **接口(材质/光照)** | ⚠️ 需扩展 | r3d_material_t / 光照抽象为单后端简化过 |

#### 当前为 best1600 做的取舍，及对完全 3D 的影响

| B3DM 现状 | best1600 取舍 | 对 OpenGL 完全 3D 的影响 |
|----------|--------------|------------------------|
| 法线 oct 16-bit / 静态光时丢弃 | flat shading 只需粗法线 | 逐像素光照需高精度法线 |
| 无 TANGENT | 评估文档 A.1 标"用不上" | 法线贴图必须有切线 |
| 无 metallic/roughness 贴图 | 无 shader 读不了 | PBR 必须有这些贴图 |
| pos 定点 16-bit | 屏幕精度够用 | 大模型/近距精度不足 |
| 减面 300-500 | 性能甜区 | OpenGL 可跑上万面 |
| 纹理 256 / 无 mipmap | 显存/带宽 | OpenGL 支持 mipmap/大图 |

> 核心：**B3DM 是烘焙过、压缩过、减面过的成品，丢失的信息不可逆**。OpenGL 后端能正确显示它，但显示的是"为低端硬件优化的资产"，非完整 3D 信息。

#### 补齐方案——全部是增量，不动核心架构

1. **B3DM 加 profile 档位**：新增 `--profile=opengl` 导出，保留高精度法线、切线、metallic/roughness 贴图、不减面、mipmap。**依赖现有机制**：Header 的 flags 位 + section_table 本就为版本化/可选段设计（§4.1）。VGLite 用 `--profile=best1600`，OpenGL 用 `--profile=opengl`，同一工具不同档位。

2. **扩 `r3d_material_t`**：增加 `normal` / `metallic_roughness` / `emissive` 纹理句柄。**依赖现有机制**：VGLite 后端忽略这些（`query_feature` 返回 false），OpenGL 后端使用——这正是 §3.3 能力查询的用途。结构体扩字段不破坏现有后端（旧后端不读新字段）。

3. **加光照抽象**：RHI 增 `set_lights(lights[], count)`，传场景光源（方向/点/聚光）。VGLite 后端只取主方向光驱动 flat shading，OpenGL 后端全用。**依赖现有机制**：与 `set_camera` 同级的每帧设置，后端自决怎么用。

#### 结论

| 维度 | 能否完全 3D |
|------|:---:|
| 架构能力（RHI 抽象） | ✅ 能，边界画对了 |
| 当前数据/接口实例 | ⚠️ 不能开箱即用，为 best1600 特化 |
| 补齐代价 | 增量（profile 档位 + 扩材质 + 加光照），不重构 |

> 准确表述：**当前架构为完全 3D 预留了通路，但当前实例化是 best1600 特化的。** 换 OpenGL 时渲染代码可复用，配套升级资产管线（B3DM profile）和材质/光照接口后，即可吃满 OpenGL 的完整 3D 能力。这些扩展点（flags 版本化、query_feature、后端自决、与 set_camera 同级的每帧设置）架构中均已预留。

---

## 8. 跨后端兼容性与 LVGL 融入

扩展能力后，必须解决"同一套上层代码 + 同源资产，如何在能力悬殊的 VGLite 和 OpenGL 上都正确绘制"。前提先说清楚：**视觉不可能完全一致，目标是"可预期的优雅降级"**——强求像素级一致只会把 OpenGL 拉低到 VGLite 水平，失去换后端的意义。

兼容性分三层解决，**全部依赖架构已有机制**。

### 8.1 资产兼容：公共子集 + 可选增强段

核心矛盾：VGLite 要"减面+压缩+烘焙"的瘦资产，OpenGL 要"高精度+PBR"的全资产。两种策略：

| 策略 | 做法 | 权衡 |
|------|------|------|
| **A 单文件多段** | 基础段(pos/uv/index/baseColor)两端共用；增强段(高精度法线/切线/metallic-roughness/mipmap)标记为可选，VGLite 加载时**跳过**、OpenGL **读取** | 单一分发省管理；但增强段占空间，best1600 平台白存 |
| **B 同源多产物** | `--profile=best1600` 出瘦版，`--profile=opengl` 出全版 | 各自最优、瘦设备不浪费 Flash；但维护两份产物 |

依赖机制：策略 A 靠 Header flags + section_table 的版本化设计（§4.1）——VGLite 后端遇到不认识的段 type 直接跳过。**Flash 紧张选 B，统一分发选 A。**

### 8.2 接口兼容：能力协商 + 优雅降级

**原则：后端声明能力，降级逻辑写在后端，不在上层。** 上层永远提交"完整材质"，后端按 `query_feature` 自己取用——保证 `r3d_render_frame` 两端完全一致。

```
扩展字段(如 normal 贴图) 进入 r3d_material_t
        │
        ├─ OpenGL 后端: query_feature(PER_PIXEL_LIGHT)=true  → 用法线贴图
        └─ VGLite 后端: query_feature(PER_PIXEL_LIGHT)=false → 忽略，退回 flat shading
```

降级链（离线就要备好降级目标）：

| 材质特性 | OpenGL | VGLite 降级到 |
|---------|--------|--------------|
| PBR metallic/roughness | 真 PBR | matcap 近似 → 再退 baseColor |
| 法线贴图 | 逐像素 | 忽略（细节已烘进 albedo） |
| 多光源 | 全部 | 只取主方向光做 flat |
| 实时阴影 | shadow map | 平面投影阴影 |
| 透视贴图 | 自动 | 填透视行（已支持） |

> 关键：VGLite 退回的 baseColor 必须是**已烘焙光照的 albedo**，否则退下去是一片死光。即使 opengl profile 也要保留一份烘焙 baseColor 作降级底——这是 gltf2b3dm 的硬约束。

### 8.3 视觉一致性：锚定基线，接受细节差异

诚实地讲，OpenGL 的真 PBR+多光源+阴影，和 VGLite 的 matcap+flat+平面阴影看起来不会一样。务实目标分三档：

1. **几何/构图一致（必须）**：同模型、同相机、同姿态，轮廓和布局完全一致——B3DM 共享基础段保证
2. **色调基线一致（应当）**：baseColor、主光方向、整体明暗倾向一致，"一眼是同一个东西"——共享 albedo + 统一主光参数
3. **材质细节差异（接受）**：OpenGL 有高光/反射/阴影细节，VGLite 没有——这是能力差，不是 bug

> 实用技巧：OpenGL 后端留一个**兼容模式开关**——调试或需严格一致时，OpenGL 也走 flat + 同一 matcap、关掉 PBR，用于验证资产正确性；正式渲染再开 PBR 发挥优势。

### 8.4 一致性测试

兼容性靠测试钉死，不靠口头保证：

- **黄金图比对**：同一 B3DM 两后端各渲一张，几何/构图必须吻合（材质细节允许阈值内差异）
- **能力矩阵测试**：每个 query_feature 组合下后端都跑通不崩（重点测 VGLite 忽略增强字段的路径）
- **降级路径测试**：故意给 VGLite 喂带 PBR 贴图的 opengl profile 资产，验证正确降级而非崩溃/乱画

### 8.5 融入 LVGL 渲染管线

LVGL 已有 mesh 渲染路径（`lv_vector_mesh_t` → `mesh_draw_cb`），所以 r3d 融入不是从零接管，而是选**在哪一层接进去**。两种方式，且**融入方式被封装在 VGLite 后端内部，上层与 OpenGL 后端完全不受影响**：

```
r3d 引擎层 draw(mesh, model, material)   ← 不变
        │
   VGLite 后端 end_frame 的两种实现：
     ├─ 独立模式: 直接 vg_lite_draw_pattern(...)
     └─ LVGL 模式: 填 lv_vector_mesh_t → 走 LVGL 的 mesh_draw_cb
```

| 方式 | 做法 | 优点 | 缺点 |
|------|------|------|------|
| **A 松耦合** | r3d 独立渲染，结果画到 LVGL canvas/image；LVGL 管 UI，r3d 管 3D 区域 | r3d 不依赖 LVGL 内部，后端可换性完整 | 两套 VGLite 上下文需协调 |
| **B 紧耦合** | r3d VGLite 后端不直接调 draw_pattern，而是填 `lv_vector_mesh_t` 走 LVGL 现成通路 | 复用 LVGL 的 path 复用/scissor/flush/image_recolor；与 LVGL 任务调度天然融合 | VGLite 后端绑定 LVGL mesh 接口 |

**坐标与裁剪对齐**：融入时 r3d 渲染区域要和 LVGL 的 widget 坐标系、scissor、`global_matrix` 对齐。LVGL 的 `mesh_draw_cb` 已处理 global_matrix 和 scissor_area——方式 B 天然继承；方式 A 需 r3d 把视口映射到 LVGL 给定区域。

> 建议：**起步用方式 A**（松耦合，r3d 独立性最强、最易验证），**性能调优期可切方式 B**（复用 LVGL 已调优的 VGLite 通路）。两者都是 VGLite 后端 `end_frame` 的实现选择，切换不影响上层和资产——再次体现 RHI 抽象边界的价值。

#### 与 LVGL refresh 机制结合

LVGL 的刷新链路是：`lv_timer_handler` → `lv_refr`（刷新定时器）→ 收集**脏区域（invalid areas）** → 逐区域调各 widget 的 draw → `flush_cb` 送屏。3D 内容融入的核心原则是：**让 3D 区域参与 LVGL 的失效/刷新机制，而不是绕过它自刷**——否则会和 LVGL 的双缓冲、脏区域合并冲突，导致撕裂或重绘错乱。

两种方式各自如何挂进这条链路：

**方式 A（canvas，松耦合）—— r3d 离屏渲染 + invalidate 触发合成**

```
1. 创建 lv_canvas，lv_canvas_set_buffer 指定一块 buffer 作为 r3d 的 target
2. 动画/交互时，应用侧：
     r3d_render_frame(scene, be)        // r3d 画进 canvas buffer
     lv_obj_invalidate(canvas)          // 告诉 LVGL：这块脏了
3. 下一次 lv_timer_handler 触发 lv_refr：
     LVGL 把 canvas 当普通图像，合成进脏区域 → flush_cb 送屏
```

要点：
- **r3d 不调 present**——canvas 只是 LVGL 的一个 image 源，上屏由 LVGL 的 flush_cb 统一做（呼应 §6.5：宿主模式 present 为 NULL）
- **刷新节流**：动画帧率由"何时调 `r3d_render_frame` + `lv_obj_invalidate`"控制。3D 区域常需比 UI 高的刷新率，可用独立 `lv_timer` 驱动 r3d 重绘，只 invalidate canvas，不强制全屏刷新
- **脏区域最小化**：只 invalidate canvas 的区域，LVGL 自动把刷新限制在这块矩形，不重绘整屏

**方式 B（DRAW_MAIN 事件，紧耦合）—— r3d 嵌进 widget 的绘制回调**

```
1. 自定义 widget（或空 obj）注册 LV_EVENT_DRAW_MAIN 事件回调
2. LVGL 刷新到该 widget 时触发回调，回调内拿到 layer/draw_buf：
     r3d 后端 begin_frame 把 target 指向该 layer->draw_buf
     r3d 提交 + end_frame  // 直接画进 LVGL 当前 layer
3. r3d 不 flush 不 present——继续走 LVGL 后续 widget 绘制 + 统一 flush_cb
```

要点：
- **天然同步**：r3d 绘制就是 LVGL 绘制阶段的一部分，脏区域/送屏完全由 LVGL 管，零额外协调
- **复用 VGLite 上下文**：直接用 LVGL draw unit 的 vg_lite 上下文和 scissor，避免两套上下文冲突
- **触发重绘**：动画时调 `lv_obj_invalidate(widget)` 让 LVGL 下一帧重绘该 widget

**两种方式的 refresh 对接对比**：

| 维度 | 方式 A（canvas） | 方式 B（DRAW_MAIN） |
|------|-----------------|---------------------|
| r3d 渲染时机 | 应用主动调，独立于 LVGL draw | LVGL 绘制阶段回调内 |
| target 来源 | canvas buffer | LVGL 当前 layer->draw_buf |
| 触发刷新 | `lv_obj_invalidate(canvas)` | `lv_obj_invalidate(widget)` |
| 上屏 | LVGL flush_cb | LVGL flush_cb |
| 双缓冲/脏区域 | LVGL 管 canvas 矩形 | LVGL 全权管 |
| 独立刷新率 | ✅ 易（独立 timer 驱动 r3d） | ⚠️ 跟随 LVGL 刷新节奏 |
| 上下文冲突 | 两套 vg_lite 上下文需注意 | 复用 LVGL 上下文，无冲突 |

**动画驱动与刷新节流（两方式通用）**：

```c
/* 用独立 timer 驱动 3D 动画，与 UI 刷新解耦 */
static void anim_timer_cb(lv_timer_t *t) {
    r3d_scene_advance(scene, t->period);   /* 推进动画时间 */
    /* 方式A: 立即渲染并标脏；方式B: 仅标脏，渲染在 DRAW_MAIN 回调 */
    lv_obj_invalidate(view_obj);           /* 请求 LVGL 下帧刷新该区域 */
}
lv_timer_create(anim_timer_cb, 33, NULL);  /* ~30fps，独立于 UI 刷新周期 */
```

要点：**渲染节流靠"何时 invalidate"控制，不靠 r3d 自己定时送屏**。这样 3D 区域和 UI 共享 LVGL 的单一刷新出口（flush_cb），避免双源送屏撕裂；同时 3D 可有自己的动画 timer，做到"UI 静止时 3D 仍能动"。

> 关键结论：无论方式 A/B，**送屏的唯一出口都是 LVGL 的 flush_cb，r3d 只负责"画进 buffer + 标脏(invalidate)"**。这与 §6.5 "宿主模式 present 为 NULL"完全一致——r3d 把"何时上屏"的控制权交给 LVGL，自己只管渲染和申报失效区域。

#### 与 LVGL refresh 机制结合

LVGL 的刷新链路是：`lv_timer_handler` → `lv_refr`（刷新定时器）→ 收集**脏区域（invalid areas）** → 逐区域调各 widget 的 draw → `flush_cb` 送屏。3D 内容融入的核心原则是：**让 3D 区域参与 LVGL 的失效/刷新机制，而不是绕过它自刷**——否则会和 LVGL 的双缓冲、脏区域合并冲突，导致撕裂或重绘错乱。

两种方式各自如何挂进这条链路：

**方式 A（canvas，松耦合）—— r3d 离屏渲染 + invalidate 触发合成**

```
1. 创建 lv_canvas，lv_canvas_set_buffer 指定一块 buffer 作为 r3d 的 target
2. 动画/交互时，应用侧：
     r3d_render_frame(scene, be)        // r3d 画进 canvas buffer
     lv_obj_invalidate(canvas)          // 告诉 LVGL：这块脏了
3. 下一次 lv_timer_handler 触发 lv_refr：
     LVGL 把 canvas 当普通图像，合成进脏区域 → flush_cb 送屏
```

要点：
- **r3d 不调 present**——canvas 只是 LVGL 的一个 image 源，上屏由 LVGL 的 flush_cb 统一做（呼应 §6.5：宿主模式 present 为 NULL）
- **刷新节流**：动画帧率由"何时调 `r3d_render_frame` + `lv_obj_invalidate`"控制。3D 区域常需要比 UI 高的刷新率，可用独立的 `lv_timer` 驱动 r3d 重绘，只 invalidate canvas，不强制全屏刷新
- **脏区域最小化**：只 invalidate canvas 的区域，LVGL 自动把刷新限制在这块矩形，不重绘整屏

**方式 B（DRAW_MAIN 事件，紧耦合）—— r3d 嵌进 widget 的绘制回调**

```
1. 自定义 widget（或空 obj）注册 LV_EVENT_DRAW_MAIN 事件回调
2. LVGL 刷新到该 widget 时触发回调，回调内拿到 layer/draw_buf：
     在 r3d 后端 begin_frame 时把 target 指向该 layer->draw_buf
     r3d 提交 + end_frame  // 直接画进 LVGL 当前 layer
3. r3d 不 flush 不 present——继续走 LVGL 后续 widget 绘制 + 统一 flush_cb
```

要点：
- **天然同步**：r3d 绘制就是 LVGL 绘制阶段的一部分，脏区域/送屏完全由 LVGL 管，零额外协调
- **复用 VGLite 上下文**：直接用 LVGL draw unit 的 vg_lite 上下文和 scissor，避免两套上下文冲突
- **触发重绘**：动画时仍调 `lv_obj_invalidate(widget)` 让 LVGL 下一帧重绘该 widget

**两种方式的 refresh 对接对比**：

| 维度 | 方式 A（canvas） | 方式 B（DRAW_MAIN） |
|------|-----------------|---------------------|
| r3d 渲染时机 | 应用主动调，独立于 LVGL draw | LVGL 绘制阶段回调内 |
| target 来源 | canvas buffer | LVGL 当前 layer->draw_buf |
| 触发刷新 | `lv_obj_invalidate(canvas)` | `lv_obj_invalidate(widget)` |
| 上屏 | LVGL flush_cb | LVGL flush_cb |
| 双缓冲/脏区域 | LVGL 管 canvas 矩形 | LVGL 全权管 |
| 独立刷新率 | ✅ 易（独立 timer 驱动 r3d） | ⚠️ 跟随 LVGL 刷新节奏 |
| 上下文冲突 | 两套 vg_lite 上下文需注意 | 复用 LVGL 上下文，无冲突 |

**动画驱动与刷新节流（两方式通用）**：

```c
/* 用独立 timer 驱动 3D 动画，与 UI 刷新解耦 */
static void anim_timer_cb(lv_timer_t *t) {
    r3d_scene_advance(scene, t->period);   /* 推进动画时间 */
    /* 方式A: 立即渲染并标脏；方式B: 仅标脏，渲染在 DRAW_MAIN 回调 */
    lv_obj_invalidate(view_obj);           /* 请求 LVGL 下帧刷新该区域 */
}
lv_timer_create(anim_timer_cb, 33, NULL);  /* ~30fps，独立于 UI 的刷新周期 */
```

要点：**渲染节流靠"何时 invalidate"控制，不靠 r3d 自己定时送屏**。这样 3D 区域和 UI 共享 LVGL 的单一刷新出口（flush_cb），避免双源送屏的撕裂；同时 3D 可有自己的动画 timer，做到"UI 静止时 3D 仍能动"。

> 关键结论：无论方式 A/B，**送屏的唯一出口都是 LVGL 的 flush_cb，r3d 只负责"画进 buffer + 标脏(invalidate)"**。这与 §6.5 "宿主模式 present 为 NULL"完全一致——r3d 把"何时上屏"的控制权交给 LVGL，自己只管渲染和申报失效区域。

### 8.6 后端梯队与 Vulkan/D3D 适配性

RHI 抽象边界不止支持 OpenGL，对 Vulkan/D3D 同样成立。但要分清**接口能适配**与**实际工程量/平台前提**。

#### 后端梯队（按平台能力）

```
VGLite    →  MCU/受限平台 (best1600)        ← 当前
OpenGL ES →  中端嵌入式/手机 (有 GPU)         ← 第一扩展
Vulkan    →  高端手机/车机/PC (现代 GPU)      ← 进一步
D3D12     →  Windows / Xbox                  ← 特定平台
```

同一份上层 + 同源 B3DM（opengl 或更高 profile），沿梯队层层适配，每加一个后端 = 加一个 `backend_xxx.c` + 编译开关。

#### 接口层：✅ 无需改动

Vulkan/D3D 后端同样实现 RHI vtable，投影/剔除/排序/深度全后端自决（GPU 投影 + 硬件 z-buffer），上层零改动：

| vtable 方法 | Vulkan/D3D 后端 |
|------------|----------------|
| `init` | instance/device/swapchain/command pool/pipeline(PSO) |
| `create_texture` | device-local image + staging 上传 |
| `begin_frame` | 绑 render target view，begin command buffer |
| `draw` | 绑 pipeline/descriptor/VBO，record draw command |
| `end_frame` | 结束 command buffer、submit queue（不含上屏） |
| `present` | `vkQueuePresentKHR`（独立模式） |
| `query_feature` | ZBUFFER/PER_PIXEL_LIGHT/SHADOW_MAP 全 = 是 |

RHI 是语义级契约，不关心后端是 immediate mode(OpenGL) 还是显式 command buffer(Vulkan/D3D)——抽象边界画在"可渲染对象"层级的价值再次兑现。

#### 后端内部：⚠️ 工程量远大于 OpenGL

Vulkan/D3D12 是显式底层 API，后端要手动管理 OpenGL/VGLite 隐藏的东西：

| 维度 | OpenGL 后端 | Vulkan/D3D12 后端 |
|------|------------|-------------------|
| 资源内存 | 驱动管理 | 手动分配/对齐/释放 |
| 同步 | 驱动隐式 | 显式 fence/semaphore/barrier |
| command | 即时调用 | 录制 command buffer + 提交队列 |
| pipeline | 设状态即可 | 预编译 PSO |
| 多帧并行 | 不用管 | frames-in-flight / 资源 double buffer |

"能适配"为真，但 Vulkan 后端的初始化与资源管理代码量可能是 OpenGL 后端的数倍——这是 **API 复杂度**，不是架构问题。

#### 平台前提

- **Vulkan**：需支持 Vulkan 的 GPU + 驱动。best1600 的 VGLite 是 2.5D 矢量加速器，**无 Vulkan 驱动**——Vulkan 后端只在有桌面/移动级 GPU 的平台才有意义。
- **D3D**：Windows/Xbox 专属，嵌入式 Linux/RTOS 无。

#### 一个接口演进点：present 与异步同步

呈现职责已通过可选的 `present` 方法厘清（§6.5）：`end_frame` 只完成绘制，`present` 才上屏，宿主模式下 `present` 为 NULL。这套划分对 VGLite/OpenGL+EGL 已足够。

Vulkan/D3D 还多一层**异步**问题：submit 后 GPU 才执行，present 有 timeline，跨帧需 fence/semaphore 同步、资源 frames-in-flight。覆盖它需要 `end_frame`/`present` 之外再加一个**可选的 `wait`/fence 回调**语义（让上层或宿主知道"这帧 GPU 真正画完了"）。这同样是**小的接口演进，非重构**：VGLite/OpenGL 后端的 `wait` 同步返回即可，Vulkan 后端用 fence 实现。

> 小结：`render`(end_frame) / `present` / `wait` 三者解耦后，从 VGLite 宿主模式（只 render，宿主上屏）到 Vulkan 独立模式（render + present + fence 同步）全覆盖，RHI 核心契约不变。

> 结论：**接口能适配 Vulkan/D3D（架构正确），但它们是高端平台选项（best1600 跑不了），且后端内部工程量大（显式 API 复杂度）。** 这套架构天然支持"按平台梯队选后端"，需要的接口演进仅是 `render`/`present`/`wait` 三者的解耦（已在 §6.5 厘清），核心契约不变。

---

## 9. 第三方库、代码组织与实现里程碑

### 9.1 第三方库清单

区分**离线工具**（开发机，不受嵌入式约束）和**运行时**（best1600 固件）：

#### 离线工具 gltf2b3dm（开发机）

| 库 | 用途 | 形态 | 协议 |
|----|------|------|------|
| cgltf | glTF 解析 | 单头文件 C | MIT |
| meshoptimizer | 减面 / cache 优化 / 量化 | C++ 静态库 | MIT |
| stb_image | 纹理解码 PNG/JPG | 单头文件 C | public domain |
| stb_image_resize | 纹理降采样 | 单头文件 C | public domain |

#### 运行时（固件）

| 依赖 | 用途 | 说明 |
|------|------|------|
| VGLite 驱动 | VGLite 后端 | 平台已有（评估文档分析对象） |
| LVGL（可选） | 若复用 mesh_draw_cb 路径 | 平台已有 |
| OpenGL ES（未来） | OpenGL 后端 | 切换后端时引入 |
| **无 cgltf/stb** | — | **运行时不解析 glTF、不解码图片**，这些已在离线完成 |

> 关键：运行时**没有 glTF 解析库、没有图片解码库**——这正是二进制路线的收益。固件只需一个矩阵库（手写 ~200 行或 linmath.h）+ 选定的后端。

#### 矩阵库选型（手写 vs linmath.h vs cglm）

运行时唯一的数学依赖是 4×4 矩阵/向量。三个候选：

| 维度 | 手写 ~200 行 | linmath.h | cglm |
|------|:---:|:---:|:---:|
| 形态 | 自有 .c/.h | 单头文件 | 头文件库（量大） |
| 协议 | 自有 | MIT | MIT |
| 体积 | 极小，只留用到的 | 小 | 大（全功能） |
| SIMD | 无（标量） | 无 | 有（NEON/SSE，可关） |
| 功能 | 仅 MVP/look_at/perspective + 四元数 slerp | 基础齐全 | 非常全（含四元数/投影/工具） |
| 可控/可调试 | ✅ 完全可控 | ✅ 单文件易读 | ⚠️ 宏多，调试稍重 |
| 嵌入式适配 | ✅ 最佳 | ✅ 好 | ⚠️ 需裁剪 |

**决策：M1-M4 用手写**，理由——
- 本平台只用到很小子集：`mat4_mul` / `mat4_mul_vec4`（投影）、`look_at` / `perspective`（相机）、四元数 `slerp`（动画旋转插值）、骨骼矩阵调色板。手写 200 行覆盖足够，且**与顶点定点化、CPU 投影热路径深度耦合**，自有实现便于按 best1600 优化（如定点化运算、避免不必要的 double）。
- linmath.h 作为**对照/兜底**：若手写出现数值问题，可临时替换验证。
- cglm 留给 **OpenGL/高端后端**：那时有 SIMD 收益且不在乎体积，可在 opengl 后端单独引入，不影响 VGLite 固件。

> 矩阵库是 §6.2 CPU 投影和交互层 orbit 相机（见第 10 章）的共同底座，归在 `core/r3d_math.c`，后端无关。


### 9.2 代码目录组织

```
r3d/
├── include/r3d/
│   ├── r3d.h                 # 公共 API（应用层唯一入口）
│   ├── r3d_backend.h         # RHI 抽象接口 + vtable 定义
│   ├── r3d_types.h           # mesh/material/camera/mat4 等 POD
│   └── r3d_b3dm.h            # B3DM 格式定义（离线工具与运行时共享）
│
├── core/                     # 后端无关引擎层
│   ├── r3d_model.c           # B3DM 加载器
│   ├── r3d_scene.c           # node 树 / 世界矩阵累乘（11.4 步骤2）
│   ├── r3d_anim.c            # 动画驱动（步骤1：时间推进/采样/局部矩阵）
│   ├── r3d_deform.c          # 顶点变形 morph+skin 调度（步骤3）
│   ├── r3d_skin.c            # 骨骼蒙皮（被 deform 调用）
│   ├── r3d_math.c            # 矩阵/向量库
│   └── r3d_backend.c         # r3d_backend_create 工厂（编译期选后端）
│
├── backend/
│   ├── vglite/
│   │   └── r3d_backend_vglite.c   # VGLite 后端（CPU 投影+Painter）
│   └── opengl/
│       ├── r3d_backend_opengl.c   # OpenGL 后端（GPU 投影+z-buffer）
│       └── shaders/               # 顶点/片元 shader
│
└── CMakeLists.txt            # -DR3D_BACKEND_VGLITE / _OPENGL

tools/
└── gltf2b3dm/                # 离线工具（开发机构建，不进固件）
    ├── main.c                # 命令行
    ├── pipeline.c            # 处理流水线
    ├── encode_b3dm.c         # 序列化（共享 r3d_b3dm.h）
    └── third_party/          # cgltf / meshoptimizer / stb
```

**依赖方向**（单向，禁止反向）：

```
app → r3d.h → core → r3d_backend.h(RHI) → backend/{vglite|opengl}
                                              ↑ core 不依赖任何具体后端
tools/gltf2b3dm → r3d_b3dm.h（仅共享格式定义）
```

- `core/` 只 include `r3d_backend.h`（抽象），绝不 include 具体后端头
- 具体后端只在 `r3d_backend.c` 工厂里被编译期选中
- 离线工具与运行时**只共享 `r3d_b3dm.h`** 一个格式定义文件，保证编解码一致

### 9.3 实现里程碑与任务拆解

每个里程碑拆为可执行任务项，标注依赖、对应章节、验收标准。M1/M2 可并行（接口与工具独立）。

#### M1 骨架（RHI + 加载器 + 空后端）

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 1.1 | 定义 `r3d_types.h`：mesh/material/camera/target/mat4 等 POD | §3.2 | 编译通过 |
| 1.2 | 定义 `r3d_backend.h`：vtable（9 方法）+ feature 枚举 | §3.2/3.3 | 接口冻结 |
| 1.3 | 定义 `r3d_b3dm.h`：Header/Section 结构（与工具共享） | §4.6 | 字段对齐校验 |
| 1.4 | 实现 `r3d_model_load`：文件→段指针映射 | §4.4 | 加载不崩、段指针正确 |
| 1.5 | 空后端 `backend_null.c`：vtable 全 stub | §3.4 | vtable 分发跑通 |
| 1.6 | `r3d_math.c`：mat4 乘/mul_vec4/look_at/perspective/slerp | §9.1 | 单元测试对拍 |

> 出口：能 load 一个手工构造的 B3DM，调用 vtable 不崩，`query_feature` 可查。

#### M2 离线工具 gltf2b3dm

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 2.1 | 集成 cgltf，解析 glTF → 内存模型 | §5.3 | 顶点/索引/材质读出 |
| 2.2 | 集成 meshoptimizer：减面到 `--max-tris` | §5.4 | 面数达标 |
| 2.3 | 集成 stb_image：纹理解码→降采样→ARGB8888 预乘 | §5.3 | 纹理段正确 |
| 2.4 | 顶点定点化 + AABB→scale/bias + 交错打包 | §4.6/5.3 | 还原误差在阈值内 |
| 2.5 | material 分组排序 → SUBMESH 段 | §5.4 | 分组数/区间正确 |
| 2.6 | B3DM 序列化器（对齐、section_table） | §4.6 | M1 加载器能读 |

> 出口：`gltf2b3dm in.gltf out.b3dm` 产物能被 M1 加载器零拷贝映射。

#### M3 VGLite MVP（静态模型上屏）

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 3.1 | `backend_vglite.c`：init/destroy + create_texture（搬进 vg_lite_buffer） | §6.4 | 纹理上传成功 |
| 3.2 | begin_frame：包装 target 为 vg_lite_buffer（不分配） | §6.5 | 渲染目标正确 |
| 3.3 | draw：CPU MVP 投影 + 背面剔除 + 入队 | §6.2 | 顶点屏幕坐标正确 |
| 3.4 | end_frame：Painter's 排序 + 逐三角 draw_pattern（仿射） + flush | §6.3 | 单模型正确显示 |
| 3.5 | 接入 LVGL（方式 A：canvas + invalidate） | §8.5 | 在 LVGL 界面内显示 |

> 出口：一个静态带纹理模型在 LVGL 界面内正确显示（仿射贴图，大三角形可有畸变）。

#### M4 视觉增强（VGLite 完整后端）

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 4.1 | 透视校正贴图：solve_perspective 填透视行（inv_w） | §6.3/评估§6 | 大三角形纹理不畸变 |
| 4.2 | flat shading：MULTIPLY 调制 + 逐面亮度 | §6.3/评估§7 | 模型有明暗体积感 |
| 4.3 | 平面投影阴影：投影矩阵压扁 + 实心填充 | 评估§8.4 | 地面有跟随阴影 |
| 4.4 | gltf2b3dm 加 `--bake-light`：离线烘焙亮度 | §5.4 | 静态光零运行时成本 |
| 4.5 | matcap：法线 xy 采样 matcap 图（可选） | 评估§8.1 | 伪 PBR 材质感 |

> 出口：对照评估文档的视觉效果，完整 VGLite 后端可用。

M1-M2 可并行（接口与工具独立）。M7 是可选的未来项，前 6 个里程碑完成即是可用的 VGLite 渲染引擎。

#### M5 动画

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 5.1 | gltf2b3dm：动画关键帧定步长重采样 → ANIMATION 段 | §5.3/11.7 | 段可加载 |
| 5.2 | gltf2b3dm：骨骼逆绑定矩阵 → SKELETON 段 | §4.6/11.6 | 段可加载 |
| 5.3 | `r3d_anim.c`：播放器池 + 控制 API（play/stop/weight/speed） | §11.3 | 播放状态正确 |
| 5.4 | `r3d_anim_update`：时间推进 + 通道采样 + 局部矩阵合成 | §11.4 步骤1 | TRS 动画播放 |
| 5.5 | `r3d_deform.c`：morph 叠加 | §11.4 步骤3.1 | 表情/变形正确 |
| 5.6 | `r3d_skin.c`：调色板 + 逐顶点蒙皮 | §11.6 | 骨骼动画正确 |
| 5.7 | 动画混合：多 player 加权（TRS/morph） | §11.5 | idle→happy 平滑过渡 |

> 出口：骨骼动画 + 表情动画播放，支持混合过渡。

#### M6 优化

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 6.1 | 按需刷新：脏标记驱动，静止零计算 | §10.4/11.8 | 静止时 CPU 空闲 |
| 6.2 | gltf2b3dm 多视角预剔除 `--views`（固定视角场景） | §5.2/评估 B.2 | 视角切换流畅 |
| 6.3 | 顶点 set_positions 增量更新（动画路径） | §11.6 | 避免重传索引 |
| 6.4 | 真机性能 profiling + 调优到甜区 | 评估第 4 节 | 帧率达标 |
| 6.5 | 内存预算核对（帧内队列/顶点缓冲上限） | §6.2 | 无 OOM |

> 出口：目标模型在 best1600 真机达性能甜区（300-500 面，流畅）。

#### M7 OpenGL 适配（按需）

| # | 任务 | 章节 | 验收 |
|---|------|------|------|
| 7.1 | `backend_opengl.c`：实现 9 个 vtable 方法 | §7.2 | vtable 跑通 |
| 7.2 | shader：顶点(投影+flat) + 片元(纹理+matcap) | §7.4 | 渲染正确 |
| 7.3 | VBO/纹理缓存 + EGL surface/present | §7.3/6.5 | 上屏正确 |
| 7.4 | B3DM opengl profile（高精度法线/切线/PBR 贴图） | §7.6 | 完全 3D 资产 |
| 7.5 | 跨后端一致性测试（黄金图/降级路径） | §8.4 | 几何构图吻合 |

> 出口：同一份 B3DM 在 OpenGL 后端正确渲染，上层零改动。

### 9.4 架构关键决策回顾

| 决策 | 选择 | 理由（章节） |
|------|------|------------|
| 资产路线 | 二进制 B3DM | 运行时免解析免解码（§4） |
| 抽象边界 | 可渲染对象层级 | 吸收投影/排序/深度的后端差异（§3.1） |
| 多态机制 | C + vtable | 嵌入式友好、可静态裁剪（§3.4） |
| 投影位置 | 后端自决 | VGLite=CPU，OpenGL=GPU（§6/§7） |
| 深度处理 | 后端自决 | VGLite=Painter's，OpenGL=z-buffer（§6/§7） |
| 离线/运行时切分 | 视角无关全离线 | 性能最优（§5、评估文档附录 D） |
| 跨后端兼容 | 能力协商 + 后端降级 | 降级逻辑在后端不在上层，上层零分支（§8.2） |
| LVGL 融入 | VGLite 后端内部选择 | 松耦合(canvas)/紧耦合(复用 mesh_draw_cb)，不影响上层（§8.5） |

> 一句话总结：**B3DM 解决"加载快"，RHI 抽象解决"后端可换"。** 抽象边界画在"可渲染对象"层级是关键——它让 VGLite 的 CPU+Painter's 和 OpenGL 的 GPU+z-buffer 两套截然不同的渲染策略，都能干净地藏在同一接口之后，上层与资产零感知。

---

## 10. 交互支持

前 9 章的 RHI/B3DM 设计覆盖了"渲染"，但**交互的输入侧（相机控制、拾取、按需刷新）是独立的一层**，需补充。交互拆成三类，难度递增：相机交互、触发式交互、拾取（picking）。

> 定位：交互层在**引擎层**（core），后端无关——它产出的是相机矩阵、动画指令、失效请求，最终仍通过既有的 `set_camera` / 动画系统 / `lv_obj_invalidate` 作用。**RHI 不需要为相机交互改动。**

### 10.1 相机交互：orbit 控制器

渲染侧零障碍——每帧 `set_camera(view, proj)` 本就可变，旋转/缩放 = 改 view 矩阵。缺的是"输入→相机矩阵"的转换，补一个 orbit camera 控制器（`core/r3d_orbit.c`）：

```c
typedef struct {
    float yaw, pitch;     /* 球面角（绕目标旋转） */
    float distance;       /* 相机到目标距离（缩放） */
    r3d_vec3_t target;    /* 注视点 */
} r3d_orbit_t;

/* 输入事件 → 更新 orbit 参数 */
void r3d_orbit_drag(r3d_orbit_t *o, float dx, float dy) {
    o->yaw   += dx * ORBIT_SPEED;
    o->pitch += dy * ORBIT_SPEED;
    o->pitch = clampf(o->pitch, -1.5f, 1.5f);  /* 限制翻转 */
}
void r3d_orbit_zoom(r3d_orbit_t *o, float factor) {
    o->distance = clampf(o->distance * factor, MIN_D, MAX_D);
}

/* orbit 参数 → view 矩阵（喂 set_camera） */
void r3d_orbit_to_view(const r3d_orbit_t *o, r3d_mat4_t *view) {
    r3d_vec3_t eye = orbit_eye_pos(o);          /* 由 yaw/pitch/distance 算 */
    mat4_look_at(view, eye, o->target, UP);     /* 复用 r3d_math */
}
```

输入来源对接 LVGL 的手势事件：

```c
/* LVGL widget 的事件回调里 */
case LV_EVENT_PRESSING: {
    lv_point_t d; lv_indev_get_vect(indev, &d);
    r3d_orbit_drag(&orbit, d.x, d.y);
    lv_obj_invalidate(view_obj);   /* 标脏，触发重绘 */
}
```

### 10.2 触发式交互：动画控制 API

点按播放动画、切表情——动画系统（§6.1 `r3d_anim_update` + 附录 A.10）已支撑，补显式控制 API：

```c
void r3d_anim_play(r3d_scene_t *s, const char *name, bool loop);
void r3d_anim_stop(r3d_scene_t *s, const char *name);
void r3d_anim_set_weight(r3d_scene_t *s, const char *name, float w); /* 表情混合 */
```

应用层："摸头"手势 → `r3d_anim_play(scene, "happy", false)`。这是上层逻辑，不涉及 RHI。

### 10.3 拾取（picking）：两后端策略不同

点中屏幕某点，反算中了哪个部位（戳肚子/摸头触发不同反应）。和投影/排序一样，**后端策略不同**，两种设计：

| 方案 | 做法 | 适用 |
|------|------|------|
| **引擎层 CPU 射线（推荐起步）** | 屏幕点→世界射线，与各 submesh 包围盒/三角形求交，取最近 | 后端无关，VGLite/OpenGL 通用；面数少时够快 |
| **后端 color picking** | RHI 加 `pick(x,y)→id`，离屏渲染 submesh ID 到 buffer，读该点像素 | OpenGL 高效；VGLite 需额外一遍绘制 |

best1600 面数少（300-500），**CPU 射线足够**且后端无关，作为默认：

```c
/* 引擎层，后端无关 */
uint32_t r3d_pick(r3d_scene_t *s, float screen_x, float screen_y) {
    r3d_ray_t ray = screen_to_ray(screen_x, screen_y, &s->camera); /* 反投影 */
    return raycast_nearest_submesh(s, &ray);   /* 返回 submesh/部位 id */
}
```

> 若未来 OpenGL 后端要 GPU picking，再给 RHI 加可选 `pick()` 方法——和 `present` 一样作为可选 vtable 项，CPU 射线作为默认 fallback。

### 10.4 按需刷新：交互场景的刷新模式

§8.5 的动画 timer 是固定 ~30fps 推进，但**交互/待机时不必恒定重绘**——best1600 尤其在意功耗。改为**按需刷新**：

| 状态 | 刷新策略 |
|------|---------|
| 静止（无操作、无动画） | 不重绘，0 功耗 |
| 交互中（拖拽/缩放） | 每个输入事件后 `lv_obj_invalidate` |
| 播放动画 | 动画 timer 驱动，播完即停 |

```c
/* 事件驱动：仅在状态变化时标脏 */
static bool dirty = false;
void on_interaction(...) { update_state(); dirty = true; }

static void tick_cb(lv_timer_t *t) {
    if (scene_has_active_anim(scene)) { r3d_scene_advance(scene, t->period); dirty = true; }
    if (dirty) { lv_obj_invalidate(view_obj); dirty = false; }
}
```

要点：**渲染由"脏标记"驱动，而非无条件每帧重绘**。静止时 CPU/GPU 完全空闲——与 §8.5"送屏唯一出口是 LVGL flush_cb，r3d 只负责画+标脏"一致，按需刷新只是把"标脏"从定时改为事件触发。

### 10.5 交互层与架构的关系

| 交互能力 | 归属 | 是否改 RHI |
|---------|------|:---:|
| orbit 相机 | 引擎层 `r3d_orbit.c` → set_camera | 否 |
| 动画控制 | 引擎层动画 API | 否 |
| CPU 射线拾取 | 引擎层 `r3d_pick` | 否 |
| GPU 拾取（未来） | 可选 RHI `pick()` | 可选新增 |
| 按需刷新 | 应用层 + LVGL invalidate | 否 |

> 结论：**交互是引擎层 + 应用层的增量，几乎不动 RHI。** 这再次印证 RHI 抽象边界画在"可渲染对象"层级的合理性——相机、输入、刷新这些"渲染之外"的关注点，自然落在边界之上，不污染后端契约。唯一可能的 RHI 演进是未来的 GPU 拾取，且作为可选方法（与 `present` 同款模式）。

---

## 11. 动画系统

动画逻辑此前分散在 §6.1（`r3d_anim_update`）、§10.2（控制 API）、评估文档附录 A.8-A.10。本章统一为完整设计。

> 定位：动画系统在**引擎层**（`core/r3d_anim.c` + `core/r3d_deform.c` + `core/r3d_skin.c`），**后端无关**——它每帧产出"更新后的 node 矩阵 / 顶点位置"，再走既有 `draw` 提交。动画本身不进 RHI。

### 11.1 四类动画驱动

glTF 动画最终都归结为四种作用方式，本平台支持度不同（承接评估文档附录 A）：

| 驱动 | 改变什么 | 本平台 | 每帧成本 |
|------|---------|--------|---------|
| **TRS 动画** | node 的 translation/rotation/scale | ✅ 主力 | 低（每 node 一次矩阵重算） |
| **Skin 蒙皮** | 顶点跟骨骼变形 | ✅ 角色动画 | 中（顶点数 × 影响骨骼数） |
| **Morph 变形** | 顶点位置 = base + Σ权重·delta | ⚠️ 限 2-4 组 | 中（顶点数 × target 数） |
| **UV 换脸** | 切换 UV 偏移（离散表情） | ✅ 最省 | 极低（只改 UV） |

> 选型见评估文档附录 A.9：脸用 Morph/骨骼，离散表情用 UV 换脸，身体用骨骼。

### 11.2 数据结构

B3DM 的 ANIMATION 段加载后映射为运行时结构（定步长重采样，免运行时查找关键帧）：

```c
typedef enum { R3D_PATH_TRANSLATION, R3D_PATH_ROTATION,
               R3D_PATH_SCALE, R3D_PATH_WEIGHTS } r3d_anim_path_t;

typedef struct {
    uint16_t          target_node;   /* 作用的 node 索引 */
    r3d_anim_path_t   path;          /* 改 T/R/S 还是 morph 权重 */
    uint16_t          frame_count;
    const void       *frames;        /* 定步长帧值数组（指向 B3DM 段，零拷贝） */
} r3d_anim_channel_t;

typedef struct {
    const char         *name;        /* "idle"/"happy"/... */
    float               duration;    /* 秒 */
    float               fps;         /* 重采样步长，离线固定 */
    uint16_t            channel_count;
    r3d_anim_channel_t *channels;
} r3d_anim_clip_t;

/* 运行时播放实例（一个 clip 可被多次播放/混合） */
typedef struct {
    const r3d_anim_clip_t *clip;
    float    time;        /* 当前播放时间 */
    float    weight;      /* 混合权重 0-1 */
    float    speed;       /* 播放速率 */
    bool     loop;
    bool     active;
} r3d_anim_player_t;
```

> **定步长的好处**：`time` 直接除以 `1/fps` 取整得帧索引，免二分查找关键帧（评估文档附录 D.4）。代价是离线重采样后帧数可能比原始关键帧多，用存储换运行时简单。

### 11.3 播放状态与控制 API

§10.2 的控制 API 落到状态机：

```c
void r3d_anim_play(r3d_scene_t *s, const char *name, bool loop);
void r3d_anim_stop(r3d_scene_t *s, const char *name);
void r3d_anim_set_weight(r3d_scene_t *s, const char *name, float w);
void r3d_anim_set_speed(r3d_scene_t *s, const char *name, float spd);
```

- `play`：在播放器池找空位，绑定 clip，`active=true`，`time=0`
- `stop`：`active=false`，从池移除
- 多个 player 可同时 active → 形成混合（§11.5）
- 播放器池大小固定（如 4），避免动态分配——嵌入式友好

### 11.4 每帧更新流水线

`r3d_anim_update(scene, dt)`（§6.1 主循环第 1 步）内部按序做：

`r3d_anim_update` 与后续两步构成 §6.1 主循环的动画三步，**职责严格分离**（顺序关键：蒙皮依赖世界矩阵，必须在累乘之后）：

```
【步骤1】r3d_anim_update(scene, dt)  —— 只产出局部变换 + morph 权重
  1.1 推进时间：每个 active player  time += dt × speed
                loop 则 time = fmod(time, duration)，否则到末尾 active=false
  1.2 采样通道：对每个 player 的每个 channel，按 time 取帧值
        TRS     → 累加到目标 node 的 T/R/S 累加器（带 weight，混合见 11.5）
        WEIGHTS → 累加到目标 node 的 morph 权重
  1.3 合成 node 局部变换：T/R/S 累加器 → 局部矩阵（旋转用四元数 slerp）

【步骤2】r3d_scene_update_world_matrices(scene)  —— 局部 → 世界
  递归累乘：world = parent.world × local

【步骤3】r3d_deform_update(scene)  —— 顶点变形（依赖世界矩阵）
  3.1 Morph：对有 morph 的 mesh，CPU 算 final_pos = base + Σ wᵢ·deltaᵢ
  3.2 Skin ：对蒙皮 mesh，先算骨骼调色板(需 world_matrix)，再逐顶点蒙皮
```

产出交给后端 `draw`：步骤 1-2 产出更新后的 node 世界矩阵，步骤 3 产出更新后的顶点。VGLite 后端据此 CPU 投影；OpenGL 后端可把世界矩阵/骨骼调色板传 shader，把步骤 3 下放 GPU（此时引擎层跳过 3.2，只传 palette）。

> 为何 skin 必须在步骤 2 之后：骨骼调色板 `palette[j] = world_matrix[joint_node[j]] × inverse_bind[j]` 依赖世界矩阵（§11.6），故顶点变形不能并进 `anim_update`，而是独立的步骤 3。

### 11.5 动画混合

多个 active player 同时作用同一 node 时，按权重混合（表情过渡、idle→happy 平滑切换的关键）：

```c
/* TRS 混合：加权累加后归一 */
node.translation = Σ (playerᵢ.weight × sampleᵢ.translation) / Σweight
node.rotation    = nlerp/slerp 加权混合各 player 的四元数
node.scale       = Σ (playerᵢ.weight × sampleᵢ.scale) / Σweight

/* Morph 权重混合：直接加权叠加 */
morph_weight[j] = Σ (playerᵢ.weight × sampleᵢ.morph[j])
```

典型用法：idle 常驻 weight=1，触发 happy 时用 ~150ms 把 happy 的 weight 从 0 渐升到 1、idle 渐降，实现平滑过渡而非突变。

### 11.6 骨骼蒙皮细节

`core/r3d_skin.c`，承接评估文档附录 A.8：

```c
/* 每帧：算骨骼矩阵调色板 */
for each joint j:
    palette[j] = world_matrix[joint_node[j]] × inverse_bind[j]

/* CPU 蒙皮：每顶点 */
for each vertex v:
    skinned = Σ_{k=0..3} weight[v][k] × palette[joint[v][k]] × pos[v]
```

- 调色板每帧算一次（joint 数次矩阵乘），顶点蒙皮是 `顶点数 × 4` 次矩阵·向量
- 拓扑不变 → 蒙皮后顶点用 `set_positions` 增量更新，不重传索引
- best1600 预算内（300-500 顶点）可行；OpenGL 后端可改 GPU 蒙皮（palette 传 uniform，顶点着色器算）

### 11.7 与 B3DM / 渲染的衔接

| 环节 | 离线（gltf2b3dm） | 运行时 |
|------|------------------|--------|
| 关键帧 | 按 `--anim-fps` 定步长重采样 | 取整索引直接采样 |
| 旋转插值 | 重采样时 slerp | 帧间 nlerp（省）或 slerp |
| morph delta | 存进 ANIMATION/VERTEX 段 | CPU 叠加 |
| 骨骼 | 逆绑定矩阵存 SKELETON 段 | 每帧算调色板 |

### 11.8 性能预算与按需更新

动画本身廉价，**瓶颈在重投影的顶点数**（评估文档附录 A.10）。两条优化：

- **脏判定**：无 active player 且无交互 → 跳过整个 `anim_update`，配合 §10.4 按需刷新，静止时零计算
- **分层成本**：TRS 动画极廉价（只重算少量 node 矩阵）；morph/skin 才触及逐顶点。优先用 TRS + UV 换脸，顶点级变形（morph/skin）按预算克制使用

> 小结：动画系统全部在引擎层 CPU 完成（best1600），产出"新矩阵 + 新顶点"交后端绘制。后端无关性保持——OpenGL 后端可把蒙皮/morph 下放 GPU，但接口（每帧 `draw` 提交更新后的网格）不变。
