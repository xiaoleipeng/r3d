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
| `--max-tris N` | 减面预算（0=不减）。注意：多 prim、顶点不共享的模型减面效果有限 |
| `--tex-size N` | 普通纹理降采样上限（默认 256） |
| `--detail-tex-size N` | 高细节纹理（表盘/logo）上限（默认 1024） |
| `--variant NAME` | KHR_materials_variants 变体名子串（如 `Gold`） |

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
