# glTF 3D 模型渲染技术评估

基于 LVGL mesh 接口 + VGLite 硬件加速，在 ARM Cortex-M55 平台上渲染 glTF 3D 模型的可行性评估。

## 1. glTF 文件结构详解

仅提取 glTF 中的 mesh 顶点/索引数据，**不能**还原 glTF 的视觉效果。glTF 是一个完整的 3D 场景描述格式，mesh 只是其中的几何骨架。

### glTF 包含的完整数据

| 数据类型 | 内容 | 渲染中的作用 |
|----------|------|-------------|
| **Mesh** | POSITION(vec3), NORMAL(vec3), TEXCOORD_0(vec2), indices | 几何形状 + UV 映射 |
| **Node hierarchy** | 树状结构，每个 node 有 TRS (translation/rotation/scale) | 模型的空间位置和父子层级关系 |
| **Camera** | perspective/orthographic, FOV, near/far plane | 决定观察视角和投影方式 |
| **Materials (PBR)** | baseColorFactor, metallicFactor, roughnessFactor, emissiveFactor, alphaMode | 表面材质属性 |
| **Textures** | baseColorTexture, normalMap, metallicRoughnessTexture, occlusionTexture | 表面细节贴图 |
| **Samplers** | magFilter, minFilter, wrapS, wrapT | 纹理采样方式 |
| **Lights** (KHR_lights_punctual) | directional/point/spot, color, intensity | 光源照明 |
| **Skins** | joints[], inverseBindMatrices | 骨骼蒙皮（角色动画） |
| **Morph targets** | 多组顶点位置偏移 | 表情/变形动画 |
| **Animations** | 关键帧 channel → node 的 TRS / weights | 驱动以上所有属性随时间变化 |

### 我们能用的 vs 不能用的

```
glTF 完整数据
├── ✅ Mesh positions → MVP投影后喂入 mesh 接口
├── ✅ Mesh indices → 直接透传
├── ✅ Mesh TEXCOORD_0 → UV 直接透传
├── ✅ Node transforms → 合并到 Model 矩阵
├── ✅ baseColorTexture → 解码后作为 pattern fill 纹理
├── ✅ Animations (TRS) → 每帧更新 Model 矩阵
├── ✅ Animations (vertex) → 每帧更新 positions
├── 🔧 NORMAL → flat shading：mesh 已设 MULTIPLY 模式，移动 recolor 计算入循环即可（无法逐像素）
├── ❌ PBR metallic/roughness → 无 shader 实时不可；matcap/烘焙近似（见第 8 节）
├── ❌ normalMap → 逐像素光照不可；细节烘进 albedo 替代（见第 8.2 节）
├── ❌ Environment map / IBL → 实时不可；matcap 近似（见第 8.3 节）
├── 🔧 Morph targets → CPU 算偏移可做，限 2-4 组（见附录 A.9）
└── ❌ 实时阴影(shadow map) → 无深度缓冲；但平面投影阴影可实时（见第 8.4 节）
```

> 说明：上面 ❌ 指**无法实时/物理正确实现**，多数有业界通用的近似路径（第 8 节）。

## 2. 现有接口能力

接口位置：`apps/graphics/lvgl/lvgl/src/draw/vg_lite_vector_optimize/lv_draw_vg_lite_vector_opt.c`（mesh_draw_cb）

| 能力 | 支持 | 说明 |
|------|------|------|
| 2D 顶点 (lv_fpoint_t) | ✅ | float x, y |
| 顶点索引 (uint16_t) | ✅ | 最多 65535 顶点（性能瓶颈远早于此上限） |
| UV 纹理坐标 | ✅ | [0.0, 1.0] 范围 |
| 纹理贴图 (Pattern fill) | ✅ | ARGB8888 |
| 纯色填充 (Solid fill) | ✅ | |
| 逐顶点颜色 | ❌ | LVGL 高层接口不暴露 |
| 变换矩阵 | ✅ | LVGL 层 2D 仿射；底层 VGLite 矩阵为完整 3×3 投影矩阵 |
| 透明度 | ✅ | 全局 opa |
| 混合模式 | ✅ | SRC_OVER 等 |

### VGLite 底层硬件实际能力（best1600）

LVGL 的 `lv_draw_vector` 是高层封装，底层 `vg_lite_*` 暴露的硬件能力更多。下表是直接调用 VGLite 时可用的能力（依据 `vg_lite.h` + `best1600_vg_lite_options.h`）：

| 硬件能力 | best1600 | 对 3D 渲染的意义 |
|---------|:--------:|-----------------|
| 投影矩阵 (3×3 含透视行) | ✅ | 硬件 `transform()` 做 `x/w`，但 mesh 代码用仿射(见下) |
| 透视插值 (x_step/y_step/c_step 含 `[2]`) | ✅ | GPU 逐像素插值 w，可做透视校正贴图 |
| MULTIPLY_IMAGE_MODE (texel×color) | ✅ | flat shading 通路，mesh 已接通(`image_recolor`) |
| blend: SRC_OVER/DST_OVER/ADDITIVE/SUBTRACT | ✅ | 不依赖读 dst，`convert_blend` 映射且无 gating |
| blend: MULTIPLY/SCREEN (读 dst) | ⚠️ | 代码允许，但 `USE_DST=0`(读 dst 关闭)，需实测确认 |
| 双线性过滤 (FILTER_LINEAR/BI_LINEAR) | ✅ | mesh 已用 FILTER_LINEAR |
| 硬件 scissor (仅矩形) | ✅ | 仅矩形 AABB，但够用：模型轮廓靠逐三角形 path 表达，scissor 只裁控件矩形 |
| blend: DARKEN/LIGHTEN | ❌ | `NEW_BLEND_MODE=0`，`vg_lite_draw_pattern` 返回 NOT_SUPPORT(vg_lite.c:3905) |
| 基础线性渐变 (init_grad + draw_grad) | ✅ | 无 feature gating，本质是渐变 buffer + draw_pattern(path.c:6255) |
| 增强线性渐变 (LINEAR_GRADIENT_EXT) | ❌ (=0) | 硬件加速版禁用，仅 best1700 |
| 径向渐变 (RADIAL_GRADIENT) | ❌ (=0) | 仅 best1700 启用 |
| 颜色变换 (COLOR_TRANSFORMATION, scale+bias) | ❌ (=0) | 仅 best1700；best1600 用 MULTIPLY 模式替代 |
| 像素矩阵 (PIXEL_MATRIX, 4×5) | ❌ (=0) | 仅 best1700 启用 |
| 高斯模糊 (GAUSSIAN_BLUR) | ❌ (=0) | 仅 best1700 启用 |
| MASK | ❌ (=0) | 仅 best1700 启用 |
| 可编程 fragment shader | ❌ | 架构上不存在 → PBR/法线贴图/IBL 无法实现 |

> 注意：上表"✅"指**硬件支持**，不等于 mesh 当前已启用。
> 透视贴图：硬件支持，但 `lv_matrix_solve_affine()` 把矩阵底行写死为仿射，需改代码才能用。
> flat shading：`mesh_draw_cb` 已设 MULTIPLY_IMAGE_MODE，仅需把调制色计算移进三角形循环。

### VGLite 硬件加速路径

`lv_draw_vg_lite_vector_opt.c` 中已实现 `mesh_draw_cb`，渲染方式：
- 逐三角形调用 `vg_lite_draw_pattern()`
- GPU 硬件光栅化 + 纹理采样
- 统一 flush，批量执行 command buffer

#### VGLite 硬件渲染管线详解

每个三角形的完整渲染路径：

```
CPU 侧（mesh_draw_cb）:
  1. 构建三角形 path: move_to → line_to → line_to → close → end
     (5个命令，每个命令写入 cmd buffer，CPU 开销极小)

  2. lv_matrix_solve_affine(): 根据3个顶点的2D坐标和UV坐标
     解出仿射变换矩阵，建立 屏幕坐标→纹理坐标 的映射
     (解2×2线性方程组，~20次浮点运算)
     注：当前 LVGL 路径用仿射映射；硬件矩阵支持透视行，
         若把顶点 1/w 编入矩阵底行即可启用透视校正贴图(见第5节)

  3. 组合 pattern_matrix = global_matrix × affine_matrix
     (一次3×3矩阵乘法)

  4. 设置 bounding box (min/max of 3 vertices)

  5. vg_lite_draw_pattern() → 写入 GPU command buffer
     (不立即执行，只写命令)

GPU 侧（flush 时批量执行）:
  6. 硬件路径光栅化: 三角形边 → 内部像素 span
  7. 对每个像素: 用 pattern_matrix 反变换得到 UV
  8. 从纹理 buffer 双线性采样
  9. Alpha blend 到目标 buffer
```

#### 关键优化点（已实现）

- **路径复用**: `lv_vg_lite_path_reset()` 在循环中复用同一个 path 对象，避免反复 alloc/free
- **批量 flush**: 所有三角形写入 cmd buffer 后统一提交 GPU 执行
- **硬件 scissor**: 如果 GPU 支持 `gcFEATURE_BIT_VG_SCISSOR`，裁剪在硬件完成
- **linear filter**: 纹理采样使用 `VG_LITE_FILTER_LINEAR`（GPU 硬件双线性插值）

#### 瓶颈分析

```
每三角形时间分解（基于实测 0.164ms/三角形）：
├── CPU: path构建+矩阵求解      ~0.03ms  (18%)
├── CPU: cmd buffer 写入        ~0.02ms  (12%)
├── GPU: 光栅化                 ~0.02ms  (12%) ← 与面积正相关
├── GPU: 纹理采样+blend         ~0.06ms  (37%) ← 主要瓶颈，与面积正相关
└── 固定开销(调度等)            ~0.03ms  (18%)
```

对于 3D 模型的小三角形，GPU 光栅化和纹理采样的时间会大幅降低（面积小），但 CPU 侧和固定开销不变。因此：
- **大三角形场景**：GPU bound（纹理带宽受限）
- **小三角形场景**：CPU bound（命令提交成为瓶颈）

## 3. 渲染 glTF 需要补齐的工作

> 注：下表是"运行时直解 glTF"所需的工作。其中解析、纹理解码、顶点转换等**与视角无关的部分可全部移到离线预处理**，设备只保留投影/剔除/排序等视角相关计算——详见附录 D 的自定义运行时格式设计。

### 必须做

| # | 工作 | 复杂度 | 说明 |
|---|------|--------|------|
| 1 | glTF 解析 | 低 | cgltf（单头文件 C 库）+ stb_image 解码纹理 |
| 2 | 4×4 矩阵库 | 低 | linmath.h 或手写 ~200 行，MVP 投影需要 |
| 3 | MVP 投影 | 中 | 每顶点：clip = P×V×M×pos → 透视除法 → 屏幕坐标 |
| 4 | 背面剔除 | 低 | 投影后叉积判断，丢弃背面三角形（省 ~50% 绘制量）|
| 5 | 深度排序 | 低 | Painter's algorithm，按三角形平均 Z 从远到近 |
| 6 | 纹理分组提交 | 低 | 按 material 分批，每批绑定一张纹理 |
| 7 | UV 透传 | 低 | glTF UV 直接给 mesh 接口 |

### 可选增强

| # | 工作 | 效果 |
|---|------|------|
| 8 | 烘焙光照到纹理 | 零运行时成本，视觉提升巨大 |
| 9 | flat shading | 逐面片明暗，增加体积感（硬件 MULTIPLY 调制，约 5 行，见第 7 节） |
| 10 | 视锥裁剪 | 减少屏幕外三角形提交 |
| 11 | LOD 简化 | 远处用低面数版本 |

### 数据流

```
glTF file
  ├─ vertices (vec3) ──→ MVP投影 ──→ 2D顶点 (x,y) ──→ mesh接口
  ├─ UV (vec2) ─────────────────────→ 直接透传 ──────→ mesh接口
  ├─ indices ───────────────────────→ 背面剔除+排序 ─→ mesh接口
  └─ texture (png) ─→ 解码 ────────→ 图片数据 ──────→ mesh接口
```

### MVP 投影实现细节

LVGL 的 `lv_matrix_t` 虽是 3×3，但高层 API 只按 2D 仿射使用（透视行常写死）。**3D 的 MVP 投影需要 4×4 矩阵，必须在 CPU 侧自行实现**；投影到屏幕后，2D 三角形再喂给 mesh 接口。
（注：VGLite 底层矩阵支持透视行，可用于贴图透视校正，见第 6 节；但 MVP 顶点投影这一步仍在 CPU 用 4×4 完成。）

#### 投影流程（每顶点）

```c
// 1. Model矩阵：glTF node 的 TRS 组合
mat4 model = node->global_transform;  // 从 glTF node hierarchy 递归计算

// 2. View矩阵：摄像机位置和朝向
mat4 view = mat4_look_at(eye, center, up);

// 3. Projection矩阵：透视投影
mat4 proj = mat4_perspective(fov_y, aspect, z_near, z_far);

// 4. 合并 MVP
mat4 mvp = proj * view * model;

// 5. 对每个顶点做变换
for (int i = 0; i < vertex_count; i++) {
    vec4 clip = mat4_mul_vec4(mvp, (vec4){pos[i].x, pos[i].y, pos[i].z, 1.0f});

    // 6. 透视除法（clip space → NDC）
    float inv_w = 1.0f / clip.w;
    float ndc_x = clip.x * inv_w;
    float ndc_y = clip.y * inv_w;
    float ndc_z = clip.z * inv_w;  // 保留用于深度排序

    // 7. 视口变换（NDC → 屏幕像素坐标）
    screen[i].x = (ndc_x * 0.5f + 0.5f) * screen_width;
    screen[i].y = (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_height;  // Y轴翻转
    depth[i] = ndc_z;
}
```

#### 背面剔除

```c
// 投影到2D后，计算三角形有符号面积（叉积z分量）
for (int t = 0; t < tri_count; t++) {
    lv_fpoint_t v0 = screen[indices[t*3+0]];
    lv_fpoint_t v1 = screen[indices[t*3+1]];
    lv_fpoint_t v2 = screen[indices[t*3+2]];

    float cross_z = (v1.x - v0.x) * (v2.y - v0.y)
                  - (v1.y - v0.y) * (v2.x - v0.x);

    if (cross_z <= 0) {
        // 背面朝向相机，跳过此三角形
        tri_visible[t] = false;
    }
}
```

#### 深度排序（Painter's Algorithm）

```c
// 每个可见三角形计算平均深度
for (int t = 0; t < visible_count; t++) {
    float z0 = depth[indices[t*3+0]];
    float z1 = depth[indices[t*3+1]];
    float z2 = depth[indices[t*3+2]];
    tri_depth[t] = (z0 + z1 + z2) / 3.0f;
}

// 从远到近排序（depth大的先画）
qsort(tri_order, visible_count, sizeof(int), compare_depth_desc);

// 按排序顺序重新组织索引，提交给 mesh 接口
```

#### 为什么需要自己写而不能用 lv_matrix_t

| | lv_matrix_t (3×3) | 需要的 (4×4) |
|---|---|---|
| 平移 | ✅ 2D | 需要 3D (x,y,z) |
| 旋转 | ✅ 绕Z轴 | 需要绕任意轴 |
| 缩放 | ✅ 2D | 需要 3D |
| 透视 | ❌ 无法表达 | ✅ w 分量实现近大远小 |
| 投影 | ❌ | ✅ 3D→2D 降维 |

> 注意：此处"❌透视"指 **3D→2D 的 MVP 投影**（需 4×4，3×3 无法表达 3 维深度降维）。
> 与第 6 节"3×3 矩阵做 2D 贴图透视校正"不矛盾——后者是平面到平面的单应变换，前者是 3 维到 2 维的投影，两者维度不同。

推荐使用 `linmath.h`（单头文件，MIT 协议）或 `cglm`（也是头文件库）。

## 4. 性能评估

### 基准实测数据

| 场景 | 纹理 | 三角形数 | 帧时间 |
|------|------|---------|--------|
| A | 1张 256×256 | 33 | 13ms |
| B | 10张 | 133 | ~29.4ms (34FPS) |

### 成本模型

```
帧时间 = 7.6ms(固定开销) + 三角形数 × 0.164ms
```

- 固定开销：image decode + flush + GPU sync
- 边际成本：path 构建 + 矩阵求解 + GPU 光栅化

### 3D 模型帧率预估

3D 模型投影后三角形面积远小于 benchmark（256×256/33≈2000px/三角形），GPU 填充成本更低。

| 三角形数 | 保守估计 | 实际预期 | 帧率 |
|----------|---------|---------|------|
| 200 | 40ms | 20-30ms | **33-50 FPS** |
| 500 | 90ms | 40-55ms | **18-25 FPS** |
| 1000 | 172ms | 70-100ms | **10-14 FPS** |
| 2000 | 336ms | 140-200ms | **5-7 FPS** |

### 性能甜区

| 目标 | 三角形数 |
|------|---------|
| 流畅交互 (30FPS) | 200-300 |
| 可用 (15-20FPS) | 400-600 |
| 能看 (10FPS) | 800-1000 |

## 5. 视觉效果还原度

| 维度 | 还原度 | 说明 |
|------|--------|------|
| 几何形状 | 95% | 投影正确即可还原 |
| 纹理贴图 | 85% (改后 95%) | 当前 mesh 用仿射，大三角形有畸变；改 solve_affine 启用透视后达 95% |
| 光照 | 50-65% | 无逐像素光照，但 MULTIPLY 模式可做 flat shading（mesh 需改约 5 行）+ 烘焙 |
| PBR 材质 | ~0% | 无可编程 shader |
| 透明/半透明 | 70% | Painter's algorithm 无法处理交叉面 |
| 动画 | 可做 | 每帧更新 positions（topology 不变用 set_positions）|

### 能做到的 3D 效果

**现成可用（mesh_draw_cb 已支持）：**
- ✅ 透视投影（近大远小，顶点 CPU 投影）
- ✅ 纹理贴图（仿射映射）
- ✅ 实时旋转/缩放
- ✅ 前后遮挡
- ✅ 骨骼动画（每帧更新顶点）

**硬件支持，需改 mesh 代码启用：**
- 🔧 透视校正贴图（改 `lv_matrix_solve_affine` 填透视行）
- 🔧 flat shading 明暗（把 recolor 计算移进三角形循环，约 5 行）

### 逐项重评估（基于 best1600 驱动 + mesh_draw_cb 实测）

> 目标芯片确认为 **best1600**（`out/bes_o62_ap/defconfig` → `best1600_ep`，CHIPID 0x265）。
> 依据：`vg_lite.h` / `vg_lite.c` / `vg_lite_matrix.c` / `best1600_vg_lite_options.h`，
> 以及 mesh 实现 `lv_draw_vg_lite_vector_opt.c` 的 `mesh_draw_cb()`、`lv_matrix.c` 的 `lv_matrix_solve_affine()`。
> **区分三类**：✅ 现成可用 / 🔧 硬件支持但需改 mesh 代码 / ❌ 硬件无此能力。

| 效果 | 早期结论 | 重评估 | 为什么 / 代码依据 | 替代方案 |
|------|---------|--------|------------------|----------|
| **透视校正贴图** | ❌ 仿射限制 | 🔧 **硬件支持，当前代码未用** | 硬件能做：`vg_lite_matrix_t` 完整 3×3，`transform()` 做 `x/w`，blit 上传 `x_step/y_step/c_step` 含透视行。但 `lv_matrix_solve_affine()` 把 `m[2][0]=m[2][1]=0, m[2][2]=1` **写死为仿射**(lv_matrix.c:305)，pattern_matrix 因此退化 | 绕过 solve_affine，自己把顶点 `1/w` 填入矩阵底行后传 `vg_lite_draw_pattern` |
| **flat shading 明暗** | ❌ 无逐顶点色/shader | 🔧 **通路已接通，约 5 行启用** | `mesh_draw_cb` 已调 `lv_vg_lite_image_recolor()`→设 `MULTIPLY_IMAGE_MODE`，并把 `color` 传给 `vg_lite_draw_pattern`(opt.c:619,673)。但 `color` 在循环**外**只算一次，全模型同一 tint | 把 color 计算移进三角形循环，乘逐面亮度（见第 7 节方案 B'）；GPU 做 texel×color，零像素成本 |
| **高光 / 叠加光** | ❌ | ⚠️ **可近似** | `ADDITIVE` 不读 dst，确定可用；`SCREEN`/`MULTIPLY` 读 dst，`USE_DST=0` 需实测（`lv_blend_to_vg` opt.c:150+ 已映射） | 高光区叠一层 ADDITIVE 三角形；非物理但有提升 |
| **DARKEN / LIGHTEN 混合** | — | ❌ best1600 禁用 | `gcFEATURE_VG_NEW_BLEND_MODE=0`，驱动对这两个 blend 返回 `VG_LITE_NOT_SUPPORT`(vg_lite.c) | 用 MULTIPLY/SCREEN 替代 |
| **逐通道调色 (scale+bias)** | — | ❌ best1600 禁用 | `COLOR_TRANSFORMATION=0`、`PIXEL_MATRIX=0`（仅 best1700） | 用 MULTIPLY_IMAGE_MODE 调亮度替代 |
| **PBR metallic/roughness** | ❌ | ❌ 实时不可，有近似 | 需逐像素 BRDF，无可编程 shader | matcap 近似 / 烘焙到 baseColor（第 8.1 节） |
| **法线贴图 / 逐像素光照** | ❌ | ❌ 实时不可，有近似 | 同上，无 fragment shader | 法线细节烘进 diffuse / 提高网格密度（第 8.2 节） |
| **环境反射 / IBL** | ❌ | ❌ 实时不可，有近似 | 无 cubemap 采样 | matcap / 球面环境映射（第 8.3 节） |
| **实时阴影** | ❌ | ⚠️ 平面投影可实时 | 无 shadow map、无深度缓冲 | 平面投影阴影（复用透视矩阵）/ blob / 烘焙 AO（第 8.4 节） |
| **径向渐变 / 高斯模糊 / 增强渐变** | — | ❌ best1600 禁用 | `RADIAL_GRADIENT=0`、`GAUSSIAN_BLUR=0`、`MASK=0`、`LINEAR_GRADIENT_EXT=0`（仅 best1700）；但**基础线性渐变可用**(draw_grad 转 pattern) | 渐变用基础 `init_grad`+`draw_grad`；径向/模糊无替代 |
| **半透明交叉面排序** | ❌ | ⚠️ 部分 | 无深度缓冲，Painter's 无法处理穿插 | 模型避免大面积穿插；按三角形深度排序 |

**结论修正**：
- **实时无法实现，但有近似路径**：PBR、法线贴图、IBL 实时不可（无可编程 fragment shader），但可用 matcap/烘焙近似；实时阴影可用平面投影阴影实现（详见第 8 节）。
- **彻底做不到（best1600 关闭特性）**：径向渐变、增强渐变(EXT)、高斯模糊、MASK、DARKEN/LIGHTEN 混合。
- **此前误判**：透视贴图和 flat shading 不是做不到，而是**硬件支持但当前 mesh 代码没用到**。透视贴图需改 `solve_affine` 路径；flat shading 只需把 recolor 计算移进循环（约 5 行）。两者都不增加新硬件依赖。

## 6. 透视矩阵的应用场景

`vg_lite_matrix_t` 是完整 3×3 矩阵，blit/draw_pattern 上传给 GPU 的 `x_step/y_step/c_step` 均含透视行 `m[2][*]`，GPU 逐像素插值齐次坐标 w 并做 `x/w` 除法（vg_lite.c `transform()`）。这是**真正的逐像素单应变换（homography）采样**，不是设一次透视后退化成仿射。基于此可实现两类效果。

### 应用 A：3D mesh 透视校正贴图（主用例）

当前 mesh 走仿射（`lv_matrix_solve_affine` 把透视行写死 0/0/1），大三角形纹理会出现 PS1 式"游泳/扭曲"。启用透视行后：

- 把每个顶点投影时的 `1/w` 编入三角形 UV 矩阵底行
- GPU 逐像素透视校正插值，**大三角形纹理不再畸变**（纹路近疏远密正确）
- 代价：CPU 每三角形多算 3 个 `1/w` + 一次解算；GPU 侧零额外成本

收益最明显的是地面、墙面、大斜面这类伸向远方的三角形。

### 应用 B：2D / 伪 3D（单张图，无需完整 3D 管线）

单应变换能把一张矩形图映射到**任意四边形**，开启一批轻量效果：

| 效果 | 说明 |
|------|------|
| 四角自由变形 (quad warp) | 把图钉在 4 个任意屏幕点，自动透视收敛 |
| 倾斜平面 / 广告牌 | 图片立在 3D 空间向后倾，近大远小 |
| Mode-7 地面 | SNES《马里奥赛车》式地面向地平线收敛，一张贴图 + 一个透视矩阵 |
| 翻牌 / 翻页 / Cover Flow | 卡片绕 Y 轴旋转的透视外观 |
| 梯形校正 (keystone) | 投影画面校正 |

### 能力边界

- **只能平面映射平面**：一个四边形对应一个平面，曲面（圆柱/球）须切成多片逼近
- **无 z-buffer**：透视是 2D 单应的视觉效果，深度遮挡仍靠 Painter's 排序
- **不是 3D 光照**：只解决形状/贴图的透视，明暗仍需第 7 节方案

## 7. 光照补偿方案

没有光照的模型看起来"平"，这是最大的视觉短板。

### 方案 A：烘焙光照到纹理（推荐）

- 在 Blender 中离线烘焙 lightmap 到 diffuse texture
- 运行时零成本，视觉效果提升巨大
- 缺点：光照方向固定

### 方案 B'：硬件调制 flat shading（best1600 推荐，约 5 行改动）

`VG_LITE_MULTIPLY_IMAGE_MODE` 让 GPU 对每个采样像素做 `out = texel × 调制色`，像素级乘法全部由 GPU 完成，**无逐像素 CPU 开销**。

**关键：`mesh_draw_cb` 已经接通这条通路**——它调用 `lv_vg_lite_image_recolor()`（内部设 `MULTIPLY_IMAGE_MODE`）并把返回的 `color` 传给 `vg_lite_draw_pattern`。唯一问题是 `color` 在三角形循环**外**只算一次（opt.c:619），全模型同一 tint。把它移进循环并乘上逐面亮度即可：

```c
// 移进 for(tri) 循环内：每三角形算一次亮度（~5 次浮点运算）
vec3 n = normalize(cross(p1 - p0, p2 - p0));   // 用 3D 顶点法线
float b = fmaxf(dot(n, light_dir), 0.0f) * 0.7f + 0.3f;  // 0.3~1.0
uint8_t L = (uint8_t)(b * 255);
// 在原 recolor 返回的 color 基础上再乘亮度（保留 opa/alpha）
vg_lite_color_t modulate = (color & 0xFF000000)
                         | (mul8(L, color>>16 & 0xFF) << 16)
                         | (mul8(L, color>>8  & 0xFF) << 8)
                         |  mul8(L, color     & 0xFF);
// 其余不变：image_mode 已是 MULTIPLY，draw_pattern 传 modulate
```


- 与方案 B 同样是 faceted 风格，但**像素混合在 GPU**，比纯 CPU flat shading 快得多
- 不依赖 `COLOR_TRANSFORMATION`/`PIXEL_MATRIX`（best1600 这两者禁用），仅用 image_mode，全系可用
- 配合 `VG_LITE_BLEND_ADDITIVE` 叠高光层可进一步增强体积感

### 方案 B：纯 CPU flat shading（无硬件调制时的退路）

```c
vec3 normal = cross(v1 - v0, v2 - v0);
normalize(normal);
float brightness = max(dot(normal, light_dir), 0.0) * 0.7 + 0.3;
```

- ~50 行代码，逐面片计算后改写纹理或顶点色
- 缺点：若要逐像素生效需 CPU 改写采样，开销大；best1600 优先用方案 B'

### 方案 C：matcap 纹理

- 按法线方向映射球形环境贴图
- 效果好，但需要运行时重算 UV
- 详见第 8 节（matcap 是 PBR/IBL 的核心近似手段）

## 8. 竞争力效果的实现路径（PBR / 法线贴图 / IBL / 阴影）

这四项是设计师做"高级感"动画的核心要素，但都需要逐像素光照计算，而本硬件**无可编程 fragment shader**，无法实时实现。业界在受限平台（早期手游、低端 Switch、ZBrush 预览器、Apple Watch 表盘）的通用对策只有一句：**把逐像素实时计算搬到离线烘焙或每帧的廉价近似上**。

### 8.1 PBR（金属度/粗糙度）

实时逐像素 BRDF 做不到。可做的工作：

| 方案 | 业界出处 | best1600 落地 | 代价 |
|------|---------|--------------|------|
| 烘焙 lit texture | 移动端低端机标准流程 | 固定光照下把 PBR 结果烘进 albedo，运行时只是贴图 | 零运行时；光照方向固定 |
| **Matcap（材质捕获）** | ZBrush / three.js MeshMatcapMaterial | 预渲染一颗带材质+环境的球→一张图，运行时按**视空间法线** `uv = normal.xy*0.5+0.5` 采样 | **几乎零成本，复用现有 mesh pattern 路径** |

**Matcap 是性价比最高的"伪 PBR"**：一次纹理采样同时编码金属度、粗糙度、环境反射的观感。当前 `mesh_draw_cb` 已是"逐顶点 UV + pattern 贴图"，matcap 只需把 UV 从 glTF texcoord 换成视空间法线的 `xy`，再绑一张 matcap 图——**不改硬件路径，改动量极小**。

### 8.2 法线贴图

逐像素扰动法线做光照做不到。可做的工作：

- **法线细节烘进 albedo**：用高模烘出带明暗细节的 diffuse，低模贴上。丢动态响应，保留视觉细节（N64/PS2 至今的常规手段）
- **几何替代**：在 300-500 三角形预算内把关键细节直接做进网格
- **matcap + 烘焙叠加**：matcap 给整体材质感，法线细节烘进纹理

### 8.3 IBL / 环境反射

cubemap 采样做不到。可做的工作：

- **matcap 一并解决**：matcap 那颗球本就在环境里渲染，反射观感已被编码——这是它能以假乱真的原因
- **球面环境映射**：按反射向量算 UV 采样全景反射图，每顶点重算 UV，适合少量高光金属物体
- **静态反射烘焙**：固定视角产品展示，反射直接烘进纹理

### 8.4 实时阴影

shadow map 做不到（无深度缓冲），但有一项**真能实时**：

| 方案 | 业界出处 | 落地 | 代价 |
|------|---------|------|------|
| **平面投影阴影 (planar shadow)** | PS1/PS2 至今标准廉价阴影 | 用投影矩阵把 mesh 压扁到地面平面，半透明黑色实心填充再画一遍 | **可实时、跟随光向**；三角形量翻倍 |
| Blob shadow | 几乎所有移动游戏 | 物体下方贴一张软边深色椭圆 | 极廉价；不贴合形状 |
| 烘焙 AO/阴影到 lightmap | 静态场景标准 | 离线烘 AO + 接触阴影进纹理 | 零运行时；不随物体移动 |

**平面投影阴影正好复用第 6 节确认的透视矩阵**：投影到地面是一次矩阵变换，再走实心填充（`LV_VECTOR_DRAW_STYLE_SOLID`）画暗色。这是本硬件上唯一的"动态贴合阴影"，建议作为标配。

### 8.5 整体建议：走"风格化 PBR"而非写实

小屏 + 受限 GPU 上，**低多边形 + 精心烘焙纹理 + matcap + 平面阴影 + 边缘光(rim)** 的组合，视觉竞争力往往高于勉强模拟写实 PBR（参考 Monument Valley、Alto's Odyssey、Apple Watch 表盘）。边缘光也廉价：逐三角形判断法线是否接近垂直视线，用 `ADDITIVE` 叠一层亮色，模拟菲涅尔边缘高光。

**落地优先级（按性价比）：**

1. **Matcap** —— 改动最小（复用现有 UV+pattern），一次拿下伪 PBR + 伪 IBL
2. **平面投影阴影** —— 唯一的实时贴合阴影，复用透视矩阵
3. **烘焙 albedo/AO** —— 零运行时成本的底子
4. **flat shading + rim light** —— 补动态明暗和边缘高光

> 这套组合已在 best1600 VGLite 后端落地并验证，实现细节见**附录 E**。

## 9. 优化建议

1. **模型控制在 300-500 三角形** —— 这是当前硬件甜区
2. **离线减面** —— meshoptimizer 简化到目标面数
3. **合并纹理为 atlas** —— 减少 image decode 和 flush 次数
4. **烘焙光照到纹理** —— 零成本获得体积感
5. **低多边形 + 手绘纹理风格** —— 比试图做写实效果好看得多
6. **屏幕面积 < 2px 的三角形跳过** —— 减少无效绘制

## 10. 结论

在 M55 + VGLite (best1600) 平台上：

- **技术上可行**：能渲染带纹理的 3D glTF 模型，支持旋转/缩放交互（mesh_draw_cb 现成）
- **能力边界修正**：透视校正贴图和 flat shading 明暗此前被误判为"做不到"——实测**硬件支持，但当前 mesh 代码未启用**。透视贴图需改 `lv_matrix_solve_affine` 填透视行；flat shading 只需把 recolor 计算移进三角形循环（约 5 行）。均不增加新硬件依赖
- **无实时实现，但有近似路径**：PBR、法线贴图、IBL 无法实时计算（无可编程 fragment shader），但业界通用对策可落地——matcap 近似 PBR+IBL、烘焙解决材质细节；实时阴影虽无 shadow map，但平面投影阴影可做实时贴合阴影（详见第 8 节）
- **best1600 特有限制**：径向渐变、增强线性渐变(EXT)、颜色变换、像素矩阵、高斯模糊、MASK、DARKEN/LIGHTEN 混合均禁用（部分仅 best1700 启用）；**基础线性渐变可用**
- **性能约束**：流畅交互需控制在 300-500 三角形
- **视觉效果**：加透视贴图 + flat shading + 烘焙光照后，可达 2010 年前后手游水平
- **最佳使用场景**：低多边形风格的 3D 模型预览器、产品展示、简单动画播放
- **不适合**：实时写实渲染、复杂场景、大量半透明物体（若需写实呈现，见附录 B 的离线渲染+播放方案）

## 附录 A：glTF 数据类型详解

第 1 节表格里列出的 10 种数据类型，逐个展开其 glTF 结构、关键字段，以及在 best1600 + mesh 接口上的处理方式。
（标注：✅ 直接可用 / 🔧 需 CPU 预处理 / ⚠️ 部分支持 / ❌ 实时不可，多有近似路径见第 8 节）

### A.1 Mesh ✅🔧

几何数据本体，存于 `meshes[].primitives[]`。每个 primitive 是一组可独立绘制的三角形。

| 属性 | 类型 | 说明 | 本平台处理 |
|------|------|------|-----------|
| `POSITION` | vec3 | 顶点局部坐标 | 🔧 经 MVP 投影成 2D 屏幕坐标喂 mesh 接口 |
| `NORMAL` | vec3 | 顶点法线 | 🔧 CPU 算 flat shading 亮度 / matcap UV |
| `TEXCOORD_0` | vec2 | UV 坐标 [0,1] | ✅ 直接透传 |
| `TANGENT` | vec4 | 切线（法线贴图用） | ❌ 无逐像素光照，用不上 |
| `COLOR_0` | vec3/4 | 顶点色 | ⚠️ mesh 接口不支持逐顶点色，只能取均值做 tint |
| `indices` | uint16/32 | 三角形索引 | ✅ 透传（注意接口上限 uint16） |

- `primitives[].mode` 必须是 4（TRIANGLES）；其余（点/线/strip）当前 mesh 路径不处理
- 多 primitive = 多材质，需按 material 分批提交

### A.2 Node hierarchy 🔧

`nodes[]` 构成场景树，每个 node 可有子节点和局部变换。

- 变换两种写法：直接给 `matrix`(mat4)，或拆成 `translation`(vec3)/`rotation`(quat)/`scale`(vec3) 即 **TRS**
- 渲染前需**递归累乘**得到每个 node 的世界矩阵：`world = parent.world × local`
- 本平台：在 CPU 侧把 world 矩阵并入 Model 矩阵，再做 MVP。glTF 用**右手系、列主序、Y-up**，注意与屏幕坐标的转换

### A.3 Camera ⚠️

`cameras[]` 定义观察方式，由某个 node 引用以获得位置/朝向。

| 类型 | 字段 | 说明 |
|------|------|------|
| `perspective` | `yfov`, `aspectRatio`, `znear`, `zfar` | 透视投影，近大远小 |
| `orthographic` | `xmag`, `ymag`, `znear`, `zfar` | 正交投影，无近大远小 |

- 本平台需用这些参数自己构建 4×4 投影矩阵（见第 3 节 MVP）
- 多数预览器**忽略 glTF 内置相机**，改用可交互的轨道相机（用户旋转/缩放）

### A.4 Materials (PBR) ⚠️❌

`materials[]` 描述表面响应，glTF 用 **metallic-roughness** 模型。

| 字段 | 含义 | 本平台 |
|------|------|--------|
| `baseColorFactor` | RGBA 基础色乘子 | ✅ 作为 pattern 调制色 |
| `baseColorTexture` | 基础色贴图 | ✅ 解码后作纹理 |
| `metallicFactor` | 金属度 [0,1] | ❌ 实时不可；matcap 近似 |
| `roughnessFactor` | 粗糙度 [0,1] | ❌ 同上 |
| `emissiveFactor` | 自发光 RGB | ⚠️ 可烘进 albedo 或用 ADDITIVE 叠加 |
| `alphaMode` | OPAQUE/MASK/BLEND | ⚠️ BLEND 走 SRC_OVER；MASK 需 CPU 阈值裁剪 |
| `doubleSided` | 是否双面 | 🔧 影响背面剔除（双面不剔除） |

### A.5 Textures 🔧

`textures[]` 关联 `images[]`(图源) 与 `samplers[]`(采样方式)。

| 贴图槽 | 用途 | 本平台 |
|--------|------|--------|
| `baseColorTexture` | 反照率 | ✅ 主纹理 |
| `metallicRoughnessTexture` | 金属/粗糙(打包在 GB 通道) | ❌ 无 shader 解读 |
| `normalTexture` | 法线贴图 | ❌ 烘进 albedo 替代 |
| `occlusionTexture` | AO(R 通道) | 🔧 可离线乘进 albedo |
| `emissiveTexture` | 自发光 | ⚠️ 烘焙或 ADDITIVE |

- 图源多为 PNG/JPG，需 stb_image 等解码成 ARGB8888 喂 GPU
- 嵌入方式：外部文件 / data URI(base64) / GLB 二进制块

### A.6 Samplers 🔧⚠️

`samplers[]` 定义纹理采样行为。

| 字段 | 取值 | 本平台 |
|------|------|--------|
| `magFilter` / `minFilter` | NEAREST / LINEAR / mipmap 系列 | ⚠️ 硬件支持 POINT/LINEAR/BI_LINEAR；**无 mipmap** |
| `wrapS` / `wrapT` | REPEAT / CLAMP / MIRROR | ⚠️ best1600 `IM_REPEAT_REFLECT=0`，REPEAT/MIRROR 受限，优先 CLAMP/PAD |

- 无 mipmap 意味着模型缩小时纹理会闪烁（aliasing），需离线降采样或控制显示尺寸

### A.7 Lights (KHR_lights_punctual) 🔧❌

扩展 `KHR_lights_punctual`，存于 `extensions`，由 node 引用。

| 类型 | 字段 | 本平台 |
|------|------|--------|
| `directional` | `color`, `intensity` | 🔧 取方向做 flat shading 的 light_dir |
| `point` | + `range` | ⚠️ 逐顶点距离衰减(CPU)，成本高 |
| `spot` | + `innerConeAngle`/`outerConeAngle` | ❌ 锥形衰减需逐像素，做不到 |

- 实用做法：只取主方向光的方向和颜色，驱动第 7 节的 flat shading；其余光照烘焙

### A.8 Skins（骨骼蒙皮）🔧

`skins[]` 实现角色骨骼动画。

- `joints[]`：作为骨骼的 node 索引数组
- `inverseBindMatrices`：每根骨骼的逆绑定矩阵(mat4)
- 顶点属性 `JOINTS_0`(每顶点 4 个骨骼索引) + `WEIGHTS_0`(对应权重)
- **CPU 蒙皮**：每帧对每顶点算 `skinned = Σ weight_i × (jointMatrix_i × inverseBind_i) × pos`，再投影
- 代价高（顶点数 × 4 次矩阵乘），300-500 顶点预算内可行；拓扑不变，用 `set_positions` 增量更新

### A.9 Morph targets（变形动画）🔧⚠️

存于 `meshes[].primitives[].targets[]`，多组顶点偏移。

- 每个 target 是一组 `POSITION`/`NORMAL` 的 delta
- 运行时：`final_pos = base_pos + Σ weight_i × target_i.delta`，weight 由动画驱动
- 常用于表情/口型。本平台 CPU 算偏移后更新 positions
- ⚠️ target 数量多时内存和计算开销大，建议限制在 2-4 组

#### 萌宠表情的技术选型（Morph vs 骨骼 vs UV 换脸）

萌宠动画通常是 **Morph targets + 骨骼(Skin) + UV 换脸** 三者分工协作，Morph 是脸部表情的核心之一，但不是全部：

| 动作 | 主力技术 | 原因 |
|------|---------|------|
| 眨眼、口型、笑、鼓脸、squash&stretch | **Morph targets** | 软组织连续形变，无明确关节 |
| 转头、摆耳、甩尾、肢体 | **骨骼 Skin** | 刚性/半刚性，绕关节旋转 |
| 整体蹦跳/缩放 | Node TRS 动画 | 整体刚体变换 |
| 离散表情切换（😊😮😴） | **UV 换脸** | 表情画进 atlas，只改 UV，顶点不动 |

一句话：**脸靠 Morph，身体靠骨骼，离散表情靠贴图**。纯 Morph 做转头要存大量目标，纯骨骼做笑容很僵硬。

**本平台(best1600，300-500 三角形预算)建议：**

1. **关键表情用 Morph，但只对脸部局部顶点、限 2-4 组** —— 眼、嘴的平滑形变
2. **能贴图换脸的优先 UV atlas 切换** —— 离散表情零顶点计算，在 mesh 接口上几乎免费，省下预算（参考《动物森友会》早期、大量手游萌宠）
3. **身体动作用骨骼蒙皮**（见 A.8）—— 转头、耳朵、尾巴
4. **眨眼两者皆可** —— 要平滑闭合用 Morph；要 Q 版"啪嗒"眨眼，贴图换帧更萌更省

> 性价比排序：UV 换脸（最省）> 局部 Morph > 全脸 Morph（最贵）。受限硬件下尽量把离散表情交给贴图，把宝贵的顶点变形预算留给真正需要平滑过渡的部位。

#### 用 mesh + 骨骼实现表情（受限平台的务实选择）

骨骼完全能做表情——在脸上埋小骨头（眼皮骨、嘴角骨、下巴骨、脸颊骨），靠旋转/平移带动周围顶点。Live2D、Spine、DragonBones 做萌宠表情正是骨骼+网格变形，在 2D/低多边形领域比 Morph 更常见。

| 维度 | 骨骼 Skin | Morph targets |
|------|----------|---------------|
| 形变方式 | 顶点绕关节旋转/平移 | 顶点在目标位置间插值 |
| 擅长 | 张嘴、眼皮开合、耳朵——**有转轴**的动作 | 微笑弧、鼓脸、squash&stretch——**无转轴**的曲面膨胀 |
| 内存 | 省（骨骼矩阵 + 权重） | 费（每表情一组顶点 delta） |
| 美术成本 | 绑骨骼 + 刷权重 | 逐个雕表情目标 |

关键看形变有无"自然轴"：张嘴/眨眼绕铰链转 → 骨骼自然；嘴角上扬成弧、整脸鼓起无明确轴 → 骨骼要堆骨头+细刷权重才逼近，Morph 一组目标搞定。

**best1600 上骨骼方案的优势：**

1. **内存省** —— 表情多时尤其明显，不像 Morph 每表情存一份顶点
2. **复用蒙皮管线** —— 身体动作已用骨骼(A.8)，表情骨骼**共用同一套 CPU 蒙皮代码**，无需另写 Morph 插值
3. **每帧成本低** —— 脸部顶点少，蒙皮的 `顶点数 × 影响骨骼数` 矩阵乘很轻

**代价**：刷权重比雕 Morph 更需经验（刷不好会拉扯穿插）；纯软形变（鼓脸/squash）骨骼略假，可叠 1-2 组 Morph 补足。

> 推荐组合：**主体表情用骨骼**（眼皮/嘴/下巴/耳，覆盖 ~80% 动作）→ **离散表情用 UV 换脸** → **纯软形变叠少量 Morph**。骨骼挑大梁，Morph 降为补充，比纯 Morph 更省、更适合本平台。

#### 卡通萌宠 vs 写实萌宠：骨骼优势会翻转

骨骼挑大梁的前提是**卡通/低多边形**风格。一旦目标变写实，分工会反转：

| | 卡通萌宠 | 写实萌宠 |
|---|---------|---------|
| 脸部表情主力 | **骨骼可挑大梁** | **Morph(blendshape)挑大梁** |
| 原因 | 夸张、有转轴的大动作 | 皱纹/眼周挤压/嘴唇翻卷等细微软组织滑动，无转轴 |
| 业界标准 | Live2D/Spine 骨骼为主 | FACS blendshape（几十~上百组动作单元） |
| 骨骼角色 | 表情+身体全包 | 退回到下巴铰链、眼球、耳朵、身体 |

写实人脸/动物脸的工业标准是 **FACS（面部动作编码系统）**——把脸拆成几十个动作单元各做一组 blendshape，影视/3A 的脸表情层几乎全是 blendshape，骨骼只管有明确转轴的部分。

**但写实萌宠本身就超出 best1600 能力圈**（前述硬墙）：

1. **面数不够** —— 写实脸需上千~上万面承载细微表情，本平台甜区仅 300-500 面
2. **无 PBR/shader** —— 写实皮毛、湿润眼睛、次表面散射全靠逐像素光照，本平台只能 matcap/烘焙近似，在"写实"标准下极易露馅
3. **Morph 内存爆预算** —— 几十组 blendshape，每组一份顶点 delta，内存与每帧 CPU 插值都超限

> **结论**：骨骼的优势是"卡通 + 受限硬件"的产物。目标变写实时，脸部优势让位给 Morph，且整个写实方向超出 best1600 能力圈。务实做法是**在本平台坚持精致卡通/低多边形风格**（此时骨骼重新有优势且省）；要写实，需换有 GPU shader、能上高面数的平台。

### A.10 Animations 🔧

`animations[]` 驱动以上所有属性随时间变化。

- `samplers`：关键帧数据，`input`(时间轴) + `output`(值) + `interpolation`(LINEAR/STEP/CUBICSPLINE)
- `channels`：把 sampler 绑到目标 `node` 的某个 `path`（translation/rotation/scale/weights）
- 运行时：按当前时间在关键帧间插值（四元数旋转用 slerp），更新对应 TRS 或 morph weight
- 本平台：每帧重算受影响 node 的矩阵或顶点，重新投影提交。**动画本身廉价，瓶颈在重投影的顶点数**

### 处理优先级小结

| 类型 | 落地优先级 | 理由 |
|------|-----------|------|
| Mesh / Node / TEXCOORD / baseColorTexture | P0 必做 | 基础渲染 |
| Camera(轨道相机) / Animations(TRS) | P0 必做 | 交互与动画核心 |
| NORMAL(flat shading) / Skins | P1 增强 | 体积感与角色动画 |
| Morph / point light | P2 可选 | 按需，注意成本 |
| metallic/roughness / normalMap / spot light | — 不直接做 | 走第 8 节 matcap/烘焙近似 |

## 附录 B：写实萌宠的可行方案

best1600 做不了**实时 3D 写实渲染**（面数、PBR、全局光照都超能力圈），但若目标是"用户看到的画面是写实的"，可绕开实时渲染。核心转变：**别在设备上算写实，把写实"搬运"到设备上播放**——写实交给离线渲染，设备降级为聪明的播放器。

### B.1 预渲染序列帧（最写实，零交互）

在 Blender/Maya 用 Cycles 离线渲染成逐帧图片，设备按帧播放。

- **写实度 100%**：屏幕即离线画质，PBR/皮毛/次表面散射全有
- **设备成本低**：仅贴图解码 + 显示，复用 image decode 路径
- **致命限制**：零交互，只能播固定动画；视角/动作/表情全是录好的
- **存储大**：每帧一图，几秒动画数百帧，Flash 吃紧；可用 H.264 压缩（需确认 best1600 硬解能力）
- **适合**：开机动画、待机循环、固定剧情演出

### B.2 多视角预渲染 + 离散切换（伪 3D，有限交互）

从 N 个角度各预渲染一套（如水平 16 方向 × 几个表情/动作），用户"转"时切到最近角度。

- **写实度接近 100%**：每张都是离线画质
- **有限交互**：能转视角（离散档位）、切表情/动作，非连续自由
- **业界先例**：电商 360° 商品展示、2.5D 游戏、宝可梦部分精灵图
- **存储**：视角数 × 表情数 × 帧数，组合易爆炸，需做减法
- **适合**：能转着看、能逗一下的桌宠（接受视角是档位而非丝滑旋转）

### B.3 Impostor / Billboard（预渲染贴图贴面片）

预渲染的写实图当纹理，贴到简单面片上，面片用第 6 节透视矩阵做朝向变换。

- **写实度高**（贴图本身写实），但边缘平，近看穿帮
- **能实时转一点**：billboard 跟随视角 + 多视角图切换
- 比序列帧省存储；本质是 B.2 的轻量版（游戏远景树木/人群常用）

### B.4 风格化"伪写实"（唯一全交互）

放弃物理写实，用烘焙 + matcap + 精细贴图在低面数模型上**画出**写实感（而非算出来）。

- **写实度中**：静态看挺真，动起来/换光会露馅
- **真实时 3D**：可自由交互——唯一既写实向又全交互的方案
- 依赖美术：高模细节/光影**烘进 albedo**，matcap 给材质反射感
- 即第 8 节"风格化 PBR"推到极限的版本

### B.5 决策表与选型

| 方案 | 写实度 | 交互性 | 存储 | 适合场景 |
|------|-------|--------|------|---------|
| B.1 预渲染序列帧 | ★★★★★ | ❌ 无 | 大 | 开机/待机/演出 |
| B.2 多视角切换 | ★★★★☆ | ⚠️ 离散 | 很大 | 可转可逗的桌宠 |
| B.3 Impostor | ★★★☆ | ⚠️ 有限 | 中 | 轻量伪 3D |
| B.4 风格化伪写实 | ★★★ | ✅ 全 | 小 | 需自由交互又要质感 |

**选型先问：用户需要实时自由交互吗？**

- **不需要（看就行）** → B.1 序列帧，直接拿离线画质，最省心最写实（先确认视频硬解，有则用视频，无则压缩图序列）
- **需要有限交互（转/切表情）** → B.2 多视角，平衡写实与交互，严控视角×表情组合数
- **必须全自由交互** → B.4 风格化伪写实，接受"非真写实"，靠美术烘焙做足质感

> 一句话：**best1600 做不了"实时写实"，但能做"播放写实"。** 把写实交给离线渲染、设备做聪明的播放/切换，是受限硬件呈现高质量画面的标准工业做法（电商 360° 展示、早期主机过场、街机预渲染精灵皆如此）。

## 附录 C：烘焙纹理技术与设计师交付规范

文档多处提到"烘焙"（lightmap、AO、albedo、matcap）。本附录说明其原理、各类烘焙的用途，以及**设计师如何把烘焙资产交付给研发**。

### C.1 为什么要烘焙

本平台无可编程 shader，无法实时算光照/材质。烘焙的本质是：**把离线渲染器（Blender/Substance）算好的光照、阴影、材质效果，"拍扁"成一张静态贴图**，运行时只做最廉价的纹理采样。一句话——**用存储和离线算力，换运行时的零成本**。

```
离线（设计师，有 GPU）           运行时（best1600，无 shader）
高模 + PBR + 灯光 + GI    ──烘焙──→   一张 albedo 贴图   ──采样──→  屏幕
（几分钟渲染一帧）                     （几 KB 纹理）        （免费）
```

### C.2 几种烘焙类型与用途

| 烘焙类型 | 把什么"拍扁"进纹理 | 用途 | 文档位置 |
|---------|------------------|------|---------|
| **Lit / Diffuse 烘焙** | 光照 + 材质 + 颜色 全部合一 | 模型直接贴这张，看起来"有光" | 第 7 节方案 A |
| **AO（环境光遮蔽）** | 缝隙/凹陷的变暗 | 乘进 albedo，增加体积层次 | 第 8.4 节 |
| **Lightmap** | 仅光照（不含 baseColor） | 与 albedo 相乘，光照可单独控制 | 第 7 节 |
| **法线细节烘焙** | 高模的凹凸明暗 | 烘进 albedo，低模显细节 | 第 8.2 节 |
| **Matcap** | 材质 + 环境反射（编码在球面） | 按视空间法线采样，伪 PBR/IBL | 第 8.1/8.3 节 |

> 关键区别：**Lit 烘焙**把所有东西合成一张（最省、最不灵活）；**Lightmap/AO 分离**让运行时还能调颜色或亮度（稍灵活，需多一次相乘）。本平台优先 Lit 烘焙。

### C.3 UV 展开——烘焙的前提

烘焙的本质是"把 3D 表面的每个点，对应到 2D 纹理上的一个像素"，这个对应关系就是 **UV**。

- 每个顶点带一组 UV 坐标 [0,1]，把模型表面"剪开摊平"到一张方形纹理上
- **Lit/AO/Lightmap 烘焙要求 UV 不重叠**（每块表面占纹理独立区域），否则光照会串色
- baseColor 贴图的 UV 可以重叠（复用纹理省空间），但烘焙光照的 UV 必须唯一——这通常是**第二套 UV（UV1）**

设计师交付前必须确保 UV 展开合理（无重叠、无拉伸、利用率高），这是烘焙质量的地基。

### C.4 设计师 → 研发 的交付规范

这是本附录的核心。设计师需交付**自包含、参数明确、研发拿来即用**的资产包。

#### 交付物清单

| 交付物 | 格式 | 要求 |
|--------|------|------|
| 模型 | `.glb`（推荐）或 `.gltf+bin` | 三角面 ≤ 预算（见 C.5）；含 UV |
| 烘焙纹理 | `.png`（带 alpha）/ `.jpg`（无 alpha） | 2 的幂尺寸（256/512）；sRGB |
| Matcap 图（若用） | `.png` | 正方形，球形材质捕获 |
| 贴图清单 | 文本/表格 | 标明每张图的类型、对应模型部位、UV 套 |
| 预览参考图 | `.png` | 设计师在 DCC 里的最终效果截图，作为研发验收基准 |

#### 关键参数对齐（必须双方确认）

1. **三角形预算** —— 研发给出上限（如 400），设计师在此之内建模
2. **纹理尺寸** —— 受 Flash/RAM 限制，研发给出（如单图 ≤ 256×256）
3. **颜色空间** —— albedo 用 sRGB；统一约定，避免运行时偏色
4. **光照方向** —— Lit 烘焙时光照已固定，设计师需告知研发"光从哪来"，使动态元素（如 flat shading 的补光、平面阴影方向）与烘焙一致
5. **坐标系** —— glTF 右手系/Y-up；设计师导出时确认朝向，研发不必再翻轴

#### 烘焙参数建议（设计师在 DCC 侧）

- **烘焙器**：Blender Cycles（免费）或 Substance Painter
- **采样**：AO/GI 用高采样消噪（128+），烘完降噪
- **留边（padding/dilation）**：UV 岛屿外扩 2-4 px，防止采样到接缝黑边
- **不要烘高光（specular）**：高光随视角变，烘死会假；交给 matcap 或省略

### C.5 不同方案的交付差异

| 渲染方案 | 设计师额外交付 | 研发处理 |
|---------|--------------|---------|
| Lit 烘焙（第 7 节 A） | 一张烘好光的 albedo | 当普通纹理贴 |
| flat shading（第 7 节 B'） | 纯 albedo + 光照方向 | 运行时按法线算亮度调制 |
| matcap（第 8 节） | albedo + 一张 matcap 球图 | 运行时用法线 xy 采样 matcap |
| 平面阴影（第 8.4 节） | 地面位置 + 光向 | 运行时投影矩阵压扁 mesh |
| 预渲染序列帧（附录 B.1） | 整套 PNG/视频序列 | 按帧播放 |
| 多视角（附录 B.2） | 视角×表情×帧 矩阵命名的图集 | 按视角索引切换 |

### C.6 协作流程（推荐）

```
1. 研发先给约束：三角形预算 + 纹理尺寸 + 渲染方案
2. 设计师按约束建模、UV 展开、烘焙
3. 设计师交付资产包 + 预览参考图
4. 研发导入，在真机比对参考图
5. 偏差大时回退：常见是纹理被压缩偏色、UV 接缝、面数超标
   → 双方按 C.4 参数清单逐项核对
```

> 核心原则：**设计师在离线环境把"好看"固化进贴图，研发在运行时只负责廉价采样和摆放**。两边在 C.4 的参数清单上对齐，是避免"设计稿很美、上设备就崩"的关键。

## 附录 D：自定义运行时格式设计（离线预处理 + 极速加载）

运行时直接解析 glTF 是浪费：JSON 解析、通用 accessor 寻址、每帧重做投影/剔除/排序，全是设备上不该付的成本。本附录设计一个**对 best1600 量身定制的二进制格式**，配一个离线转换工具，把"能离线算的全算掉"，设备只负责加载即用。

核心原则：**凡是与"当前帧、当前视角"无关的计算，全部移到离线。**

### D.1 运行时直解 glTF 的成本

| 环节 | glTF 直解的开销 | 能否离线消除 |
|------|----------------|-------------|
| JSON 解析 | 字符串解析、建对象树 | ✅ 离线转二进制 |
| accessor/bufferView 寻址 | 多级间接、stride 计算 | ✅ 离线摊平成数组 |
| 顶点格式转换 | float/归一化/交错解包 | ✅ 离线转定点/紧凑布局 |
| 纹理解码 (PNG/JPG) | 运行时 stb_image 解码 | ✅ 离线转 GPU 直采格式 |
| 三角形数据组织 | 每帧从索引拼三角形 | ✅ 离线预拼 |
| 背面剔除 / 深度排序 | ⚠️ 依赖视角，每帧重算 | ⚠️ 部分（见 D.4） |

前 5 项与视角无关，可 100% 离线消除。只有投影、剔除、排序依赖当前相机，必须运行时做——而这正是格式要重点优化的部分。

### D.2 设计目标

1. **零解析加载**：直接 load 到 RAM 后指针映射为结构体即用，无 JSON、无对象构建；若平台支持 XIP/mmap，只读段（顶点/索引）可直接寻址免拷贝
2. **GPU 友好**：纹理已是 best1600 直采格式（预乘 ARGB8888 / 对齐布局），加载时一次性搬入 GPU 可访问 RAM，**省解码**（搬运仍需，但免 stb_image）
3. **运行时最省**：预计算所有静态数据，每帧只剩"投影 + 剔除 + 排序 + 提交"
4. **视觉最优**：烘焙资产（albedo/matcap/AO）直接内嵌，加载即最佳画质
5. **可流式/分块**：大模型可按需加载，省 RAM

### D.3 格式结构（建议）

二进制布局，小端，4/16 字节对齐，方便直接映射为结构体指针。

```
┌─────────────────────────────────────────┐
│ Header                                   │
│  magic "B3DM" / version / flags          │
│  bounding sphere (剔除/排序用)            │
│  各段 offset + size 表                   │
├─────────────────────────────────────────┤
│ Vertex Block（紧凑、可定点化）            │
│  pos[xyz] + uv[uv] (+ normal 量化)       │
│  （固定视角时退化为屏幕 xy，免存 z）      │
│  预转交错布局，cache 友好                 │
├─────────────────────────────────────────┤
│ Index Block                              │
│  已按 material 分组排序的三角形索引       │
├─────────────────────────────────────────┤
│ Submesh / Material Table                 │
│  每组：纹理 id、blend、image_mode、       │
│  顶点/索引范围                           │
├─────────────────────────────────────────┤
│ Texture Block                            │
│  已解码为 GPU 直采格式（ARGB8888 预乘/   │
│  对齐）；含 matcap / 烘焙图               │
├─────────────────────────────────────────┤
│ Animation Block（可选）                  │
│  预采样关键帧（定步长，免运行时插值查找） │
│  骨骼矩阵调色板 / morph delta             │
├─────────────────────────────────────────┤
│ Skin / Skeleton（可选）                  │
│  逆绑定矩阵、骨骼层级（已展平）           │
└─────────────────────────────────────────┘
```

关键设计点：

- **是否存 Z 坐标**：取决于视角模式——
  - **自由视角（运行时投影）**：`pos.xyz` 三个分量**必须全存**。Z 是 MVP 投影（透视近大远小）和深度排序（Painter's 算法）的必需输入，省了就无法做 3D。优化点：Z 精度可低于 XY（XY 决定像素位置需高精度，Z 只用于投影和排序比较），如 XY 用 16-bit、Z 用 12-bit 即够
  - **固定/离散视角（预投影）**：投影和排序已离线做完，Vertex Block 只需存**屏幕 xy**，Z 可省略（见 D.4 进阶）。代价是每个视角存一份
- **顶点定点化**：屏幕空间精度有限，pos 可用 16-bit 定点 + 整体 scale/bias，省一半内存、加快 CPU 投影
- **法线量化**：flat shading 只需法线算亮度，可用 oct 编码压到 16-bit，或**离线预算逐三角形亮度**（静态光照时连法线都不用存）
- **material 预分组**：索引离线按纹理排好序，运行时直接分批提交，省去运行时分组
- **纹理内嵌且对齐**：免运行时解码（省 stb_image），加载时按对齐要求搬入 GPU 可访问 RAM 即可直接绑定

### D.4 哪些计算移到离线（关键收益）

| 计算 | 离线（工具做） | 运行时（设备做） |
|------|--------------|-----------------|
| JSON/accessor 解析 | ✅ 全部 | — |
| 顶点格式转换、定点化 | ✅ | — |
| 纹理解码 + 格式转换 | ✅ | — |
| material 分组排序 | ✅ | — |
| 法线 → 静态光照亮度 | ✅（静态光时） | — |
| 动画关键帧重采样 | ✅ 定步长 | 取整索引即可，免二分查找 |
| **MVP 投影** | ❌ 依赖视角 | ✅ 每帧（4×4 × 顶点） |
| **背面剔除** | ⚠️ 静态视角可预；自由视角运行时 | ✅ 投影后叉积 |
| **深度排序** | ⚠️ 同上 | ✅ 每帧（或固定视角时离线定序） |

> 进阶：若是**固定/离散视角**（如附录 B.2 的档位旋转），连剔除和排序都能离线，每个视角预存一份"已剔除+已排序"的索引，运行时连排序都省了——退化成"选一份索引直接画"。

### D.5 离线转换工具（gltf2b3dm）

```
gltf2b3dm  input.gltf  output.b3dm  [选项]
  --max-tris 400          # 减面到预算（调 meshoptimizer）
  --tex-size 256          # 纹理降采样上限
  --tex-format argb8888   # 转 GPU 直采格式（预乘）
  --quantize-pos 16       # 顶点定点位宽
  --bake-light <dir>      # 静态光照：预算逐面亮度，免运行时算
  --matcap matcap.png     # 内嵌 matcap
  --anim-fps 30           # 动画重采样步长
  --views 16              # （可选）多视角预剔除+预排序
```

工具内部流水线（复用业界库）：

```
glTF ──cgltf 解析──→ 内存模型
   ├─ meshoptimizer：减面 / 顶点 cache 优化 / 重排
   ├─ stb_image：解码纹理 → 降采样 → 转 ARGB8888 预乘
   ├─ 顶点定点化 + 交错打包
   ├─ 按 material 分组排序索引
   ├─ （静态光）逐面烘焙亮度
   ├─ （动画）关键帧定步长重采样
   └─ 写出对齐的二进制 B3DM
```

### D.6 运行时加载路径（对比）

```
glTF 直解（慢）：
  读文件 → JSON 解析 → 建 accessor 树 → 解码纹理 →
  每帧：拼三角形 → 投影 → 剔除 → 排序 → 提交

B3DM（快）：
  load 到 RAM（或 XIP 映射只读段）→ 指针映射结构体 →
  纹理一次性搬入 GPU RAM（已是直采格式，免解码）
  每帧：投影 → 剔除 → 排序 → 提交
        （固定视角时：选预存索引 → 提交）
```

加载阶段从"解析+解码数百 ms"降到"近乎零"；每帧从"拼接+投影+剔除+排序"减到"只剩投影及必要的剔除/排序"。

### D.7 收益小结

| 维度 | glTF 直解 | B3DM 自定义格式 |
|------|----------|----------------|
| 加载时间 | 慢（JSON + 解码） | 极快（load 即用，免解析免解码） |
| RAM 占用 | 高（对象树 + 解码缓冲） | 低（定点 + 紧凑 + 可流式） |
| 每帧 CPU | 拼接+投影+剔除+排序 | 投影 +（必要时剔除/排序） |
| 纹理 | 运行时解码 | 离线转直采格式 |
| 视觉 | 取决于运行时 | 烘焙资产内嵌，加载即最佳 |
| Flash 占用 | glTF + 原图 | 可控（减面 + 纹理压缩） |

> 一句话：**glTF 是"交换格式"，不是"运行时格式"。** 在 best1600 上，用离线工具把 glTF 编译成量身定制的 B3DM——就像源码编译成机器码——设备加载即用、每帧只算躲不掉的视角相关部分，在视觉最优前提下达到性能最高。这是受限嵌入式平台渲染资产的标准工业做法（游戏引擎的 cooked asset、GPU 纹理的 KTX/Basis 都是同一思路）。


---

## 附录 E：VGLite 后端设计与验证（best1600，无 shader / 无 z-buffer）

本附录记录在 best1600 平台（GCNanoLiteV，CHIPID 0x265）上实现 3D 网格渲染后端的设计、关键约束与验证结果。结论：**通过"离线烘焙 + CPU 投影 + 逐三角形纹理填充"，在无可编程管线的 2.5D 矢量加速器上可还原 glTF 的工业 PBR 观感。**

### E.1 VGLite 能力核对（基于实际驱动源码）

核对了 best1600 的 `vg_lite.c` 与 `best1600_vg_lite_options.h`，关键事实：

| 能力 | best1600 | 说明 |
|------|----------|------|
| 矩阵变换 | `vg_lite_matrix_t m[3][3]` | **硬件支持透视项**：`transform()` 计算 `pt_w = x·m[2][0]+y·m[2][1]+m[2][2]` 并做投影除法 |
| 路径填充贴图 | `vg_lite_draw_pattern` | 用 path 轮廓填充经矩阵变换的纹理图案——3D 贴图的核心 API |
| 混合 | `VG_LITE_BLEND_SRC_OVER` 等 | 半透明表镜原生支持；`SRC_PREMULTIPLIED=1` |
| 双线性采样 | `VG_LITE_FILTER_BI_LINEAR` | 纹理放大平滑（表盘清晰度） |
| 图像 REPEAT | `IM_REPEAT_REFLECT=0` ❌ | **不支持图像 REPEAT 采样** |
| 颜色变换 | `COLOR_TRANSFORMATION=0` ❌ | **不能逐像素颜色矩阵**；染色只能用单一 `color` 参数 |
| 线性渐变 | 支持 | 可用于三角形内亮度过渡（Gouraud 近似） |
| 径向渐变 | `RADIAL_GRADIENT=0` ❌ | 不支持 |
| z-buffer | 无 | 需 CPU 排序（画家算法） |
| 可编程 shader | 无 | 所有着色靠离线烘焙 + CPU 计算 |

### E.2 渲染管线设计

VGLite 是 2.5D 加速器，无顶点/片元 shader、无深度缓冲。3D 网格渲染策略：

```
每个 draw call(submesh)：
  1. CPU 计算 MVP，逐三角形投影顶点 → 裁剪空间
  2. 透视除法 → NDC → 屏幕坐标(Y 翻转)
  3. 屏幕空间有向面积做背面剔除
  4. CPU flat 光照：view 空间法线 → 亮度，调制 baseColorFactor
  5. matcap 材质：CPU 按 view 法线算 UV = nv.xy·0.5+0.5
  6. 三角形(屏幕坐标 + UV + 深度 + 染色)入帧队列
end_frame：
  7. 画家算法排序(view 空间线性深度，远先画；半透明最后)
  8. 逐三角形：构造 path 轮廓 + 解 UV→屏幕仿射矩阵
  9. vg_lite_draw_pattern 贴烘焙纹理(PATTERN_PAD + BI_LINEAR)
```

### E.3 设计如何契合 VGLite 的限制

关键在于**离线烘焙的设计天然规避了 best1600 的两大硬限制**：

1. **无图像 REPEAT（`IM_REPEAT_REFLECT=0`）**：碳纤维等平铺纹理在离线工具中已"平铺烘焙进纹理 + 顶点 UV 归一化到 [0,1]"，运行时只需 `PATTERN_PAD`，不依赖硬件 REPEAT。若当初采用 GL 的 REPEAT wrap，在 best1600 上会直接失效。
2. **无颜色变换（`COLOR_TRANSFORMATION=0`）**：matcap 染色、baseColorFactor 都是 submesh 级整体 tint，用 `draw_pattern` 的单一 `color` 参数即可，不需要逐像素颜色矩阵。

其余 PBR 效果（金属/粗糙分区、AO、法线编织明暗、logo 定位）全部在离线阶段烘进 baseColor 纹理，运行时是普通图片采样。**这印证了附录 C 的烘焙方向是正确的工程路径。**

### E.4 无 shader 的两个 CPU 替代

| GPU(OpenGL) | VGLite(无 shader)替代 |
|-------------|----------------------|
| 顶点 shader 算 matcap UV | CPU 逐顶点算 `uv = nv.xy·0.5+0.5`，设为 pattern 矩阵 |
| 片元 shader 逐像素 Phong | CPU 逐三角形 flat 光照(3 顶点平均法线)，`lit=(0.50+0.32·d+0.18·hemi)·ao`，乘进 `color` |

flat shading 与 OpenGL 的逐像素着色有差异（三角形内无渐变），因 `draw_pattern` 对整个三角形只接受一个 `color`。若需三角形内渐变，可用 best1600 支持的 linear gradient 近似 Gouraud；对 3.3 万三角的手表，flat 已足够且更快。

### E.5 画家算法与深度排序

无 z-buffer，用画家算法（远先画）。**排序键必须用 view 空间线性深度，而非 NDC z**：NDC z 经透视投影后非线性，远处精度差，深度接近的三角形（表盘面 vs 刻度/指针）排序抖动，产生穿插黑块。改用 view 空间 z（线性、区分度均匀）后穿插消除。

固有局限：对互相穿插的三角形，画家算法理论上无法完美排序。手表模型实测无明显问题；极端情况可用三角形分割（BSP）或分层 scissor。

### E.6 验证方法与结果

- **环境限制**：本机无 best1600 硬件；VGLite 官方 cmodel 为 Windows-only（依赖私有 GPU 模型）。
- **验证手段**：实现 `vg_lite_softsim.c`——VGLite API 子集的 CPU 软件实现（扫描线光栅化 + 仿射纹理采样），完全遵循真实 API 签名与语义。后端代码 `backend_vglite.c` 一行不改，真机换厂商 `libvg_lite` 即可。
- **验证范围**：这验证 r3d VGLite 后端的**调用逻辑**（投影/仿射映射/画家算法/matcap/光照），**不验证 VGLite 硬件光栅化器本身**。
- **结果**：手表正确渲染——表壳金属、表盘（数字/日期窗/指针/刻度）、金色编织表带、黑塑料件、银按钮、logo 均正确；CPU flat 光照赋予表壳明暗立体感；view 深度排序消除穿插。

### E.7 构建配置

```bash
# 本机验证(自动编入 softsim CPU 实现)
cmake -S . -B build-vg -DR3D_BACKEND_VGLITE=ON
# 目标平台(链接厂商库)
cmake -S . -B build-vg -DR3D_BACKEND_VGLITE=ON \
      -DR3D_VGLITE_INC=<vg_lite.h 路径> \
      -DR3D_VGLITE_LIB=<libvg_lite 路径>
```

> 一句话：**VGLite 无 shader / 无 z-buffer 不是 3D 渲染的障碍——只要把着色离线烘进纹理、把投影与排序放到 CPU、用 `draw_pattern` 做仿射贴图，就能在 2.5D 矢量加速器上还原工业 PBR 观感。** 离线烘焙的设计（附录 C/D）正是为这类受限平台量身定制的。
