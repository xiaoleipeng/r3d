# gltf2b3dm — glTF/glb → B3DM 离线转换工具

把 glTF 2.0（`.gltf` / `.glb`）编译为引擎内部的 B3DM 二进制：顶点定点化、材质分组、
纹理降采样烘焙、动画/骨骼/morph 提取，可选减面。

## 用法

```
gltf2b3dm input.gltf output.b3dm [选项]
```

常用选项：

| 选项 | 说明 |
|------|------|
| `--max-tris N` | 减面预算（0=不减）。按"三角形数 × 包围盒尺寸"加权分配到各 prim：大件保面、碎小件砍狠；蒙皮/morph prim 额外加权并禁用 sloppy 以保形 |
| `--tex-size N` | 普通纹理降采样上限（默认 256） |
| `--detail-tex-size N` | 高细节纹理（表盘/logo）上限（默认 1024） |
| `--variant NAME` | KHR_materials_variants 变体名子串（如 `Gold`） |
| `--material-mode M` | 材质/纹理输出模式（见下表），默认 `full` |

### 减面的属性/拓扑保护（行业惯例）

减面按网格类型分级，约束强度不同：

| 网格类型 | 算法 | 误差阈值 | 保护属性 |
|---|---|---|---|
| 纯色静态道具 | `meshopt_simplify`，超预算可 `sloppy` | 宽松(1.0) | 位置 |
| 带纹理静态 | `simplifyWithAttributes`，禁 sloppy | 中(0.1) | 位置+UV |
| **蒙皮 / morph** | `simplifyWithAttributes`，**禁 sloppy** | **收紧(0.01)** | 位置+UV+法线+**骨骼权重/morph delta** |

蒙皮/morph 网格绝不使用 sloppy（不保拓扑/属性会导致形变撕裂）。对蒙皮网格把
**骨骼权重**作为属性（保骨骼影响边界），对 morph 网格把**最活跃的若干
blendshape 的逐顶点 delta**作为属性（meshopt 属性上限 32 个 float，按能量
`Σ|delta|²` 选 topK target）纳入误差度量。

> **关于 morph 模型的减面上限**：把 blendshape delta 纳入误差后，meshopt 会拒绝
> 合并"在动画下运动差异很大"的相邻顶点（如 jawOpen 张嘴一侧 vs 另一侧），因此
> 带表情的面部 prim 往往**减不到目标预算就停下**——这是为保住表情质量的正确取舍。
> 例：facecap（52 套 ARKit blendshape）请求减到 1500 三角形时，无动画的眼睛/牙齿
> prim 被砍到极简（960→30），而面部 prim 保留在 ~3700，整体停在 ~3900 而非 1500。
> 若需更激进，应降低 morph target 数或接受表情失真。


### 材质模式 `--material-mode`

去纹理的材质降级谱系（嵌入式省纹理内存）：

| 模式 | 说明 |
|---|---|
| `full` | 保留纹理烘焙（默认，现状行为） |
| `baked-vertex` | 按 UV 把纹理采样到逐顶点色，丢弃贴图。运行时只做顶点色插值，省纹理内存。生成 `VTXCOLOR` 段(B3DM v4) |
| `solid` | 每 prim 用其贴图平均色作单色，丢弃贴图；无贴图保留 baseColor |
| `none` | 统一中灰，丢弃所有纹理/颜色（调试/线框） |

> B3DM 升到 v4（新增可选 `VTXCOLOR` 段）。运行时向后兼容 v3 资产，无需重转。


## 支持的网格压缩扩展

很多 glb（尤其从 Sketchfab/gltf-transform 导出的）带网格压缩。cgltf 只解析元数据、
不解压，若不处理顶点会塌缩成极少数、索引越界。本工具已内建两种解压：

| 扩展 | 解压方式 | 是否默认可用 |
|------|----------|--------------|
| `EXT_meshopt_compression` | 内建 meshoptimizer 解码器 | ✅ 始终可用 |
| `KHR_draco_mesh_compression` | Google Draco 解码库 | ⚠️ 需先拉取 draco 源码 |

### 启用 Draco 支持

Draco 源码体积较大（~140MB），未纳入仓库。需要解码 Draco 压缩模型时先拉取：

```bash
git clone --depth 1 -b 1.5.7 https://github.com/google/draco.git \
    tools/gltf2b3dm/third_party/draco_src
```

CMake 会自动探测 `third_party/draco_src`，存在则编入 Draco 解码（decoder-only 静态库）
并定义 `G2B_HAVE_DRACO`。配置时会打印：

```
-- gltf2b3dm: Draco 解码已启用
```

未拉取时工具仍可正常构建，但遇到 Draco 压缩 prim 会告警并跳过。

## 主机构建

```bash
cmake -S . -B /tmp/r3d_build -DR3D_BUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/r3d_build --target gltf2b3dm
cmake --build /tmp/r3d_build --target b3dm2gltf   # 反向工具，用于验证
```

## 验证产物

`b3dm2gltf` 把 B3DM 反解回 glTF，并打印顶点/索引/submesh/纹理计数，可快速核对
顶点数与索引数是否匹配（索引数应为三角形数 × 3）：

```bash
b3dm2gltf output.b3dm /tmp/check.gltf
#   顶点337834 索引1076364 submesh51 纹理0
```
