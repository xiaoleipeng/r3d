# gltf2b3dm 在线工具

浏览器里预览 glTF/glb、转换为 B3DM、预览转换结果，并实时调整转换参数。

```
┌─────────────────┬─────────────────┬──────────┐
│  原始 glTF 预览  │  转换后 b3dm 预览 │ 参数面板 │
│  (three.js)     │  (b3dm2gltf反解)  │ (lil-gui)│
└─────────────────┴─────────────────┴──────────┘
```

## 三个能力

1. **预览 glTF/glb** — 浏览器用 three.js 直接渲染，内置 Draco / meshopt 解压，可看动画、轨道控制旋转缩放。
2. **预览 b3dm** — b3dm 是引擎私有格式。服务端调用命令行 `b3dm2gltf` 把它反解成 glTF 再渲染，所见即引擎实际拿到的数据。
3. **参数实时调整** — 右侧面板调 `--max-tris` / `--tex-size` / `--detail-tex-size` / `--variant`，改动后自动重新调用 `gltf2b3dm` 转换并刷新右侧预览，与左侧原始模型实时对比。

> 转换由真实的命令行工具完成，结果与离线 `gltf2b3dm` 完全一致——网页不重复实现转换逻辑，只做编排与可视化。

## 前置条件

先构建命令行工具（服务端会调用）：

```bash
cmake -S frameworks/graphics/animengine/r3d -B /tmp/r3d_build2 -DR3D_BUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/r3d_build2 --target gltf2b3dm
cmake --build /tmp/r3d_build2 --target b3dm2gltf
```

需要 Node.js（任意 16+，仅用内置模块，无需 `npm install`）。

## 启动

```bash
cd tools/gltf2b3dm/web
node server.js                       # 默认端口 8787，自动找 /tmp/r3d_build2 下的工具
# 或显式指定工具路径与端口：
node server.js --port 9000 --tool /path/to/gltf2b3dm --verify /path/to/b3dm2gltf
```

浏览器打开 `http://localhost:8787`，拖入或选择一个 `.gltf` / `.glb` 即可。

## 工作目录

上传文件、转换产物、反解预览都放在系统临时目录 `${TMPDIR}/gltf2b3dm_web/`，
重启系统后自动清理。下载按钮可保存生成的 `.b3dm`。

## 关于 vendor/three

`public/vendor/three/` 自带 three.js（r160）的运行时与 GLTFLoader / DRACOLoader /
OrbitControls / meshopt 解码器 / lil-gui（约 3.7MB）。因为目标环境通常离线（CDN/npm
不可达），故随仓库提供，开箱即用。来源：https://github.com/mrdoob/three.js (r160)。
