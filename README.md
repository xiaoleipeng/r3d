# r3d — 轻量级嵌入式 3D 引擎

r3d 是一个面向嵌入式平台的轻量级 3D 渲染引擎。核心引擎与渲染后端解耦，
通过 **CPU 端 MVP 投影 + 三角形光栅化 + 画家算法排序** 实现 glTF/B3DM
模型的渲染，可在不同图形硬件/API 上运行：既能跑在带可编程管线的 OpenGL 上，
也能跑在无 shader、无 z-buffer 的 2.5D 矢量加速器上。

## 特性

- **后端无关的核心引擎**：模型加载、骨骼/变形动画、数学库均与渲染后端解耦，
  通过统一的后端抽象（vtable）对接不同硬件。
- **多后端**：
  - **OpenGL** — 带可编程管线的标准 3D 渲染路径，也用于主机开发对照；
  - **VGLite** — 面向嵌入式 2.5D 矢量 GPU（如 Vivante VGLite）的后端，
    生产设备路径；
  - **Null** — 无显示后端，用于 bring-up / CI。
- **B3DM 资产格式**：紧凑的二进制模型容器（小端、16 字节对齐），由离线工具
  从 glTF 转换而来。
- **嵌入式运行时**：在 NuttX 上以 `ANIMATION_ENGINE` 子组件接入，封装
  framebuffer 与 GPU 初始化，逐帧推进动画并上屏。

## 目录结构

| 路径 | 说明 |
|------|------|
| `include/r3d/` | 公共头文件（引擎、后端、模型、动画、数学、B3DM 格式） |
| `core/` | 后端无关核心：模型加载、动画、蒙皮、变形、数学 |
| `engine/` | 运行时封装（`r3d_engine.c`）：framebuffer + GPU 粘合层 |
| `backend/opengl/` | OpenGL 后端 |
| `backend/vglite/` | VGLite 后端（嵌入式 2.5D GPU） |
| `backend/null/` | Null 后端（无显示，bring-up/CI） |
| `demo/` | 设备端 framebuffer demo |
| `tools/` | 离线工具：`gltf2b3dm`、`b3dm2gltf`、`demo`（主机查看器） |
| `tests/` | 主机单元测试（`test_m1`、`test_m2`） |
| `docs/` | 架构与评估文档 |

## 架构分层

```
应用 / demo
      │
      ▼
r3d_engine  (engine/)        ← 运行时：fb / GPU 初始化 / mmap / 翻页上屏
      │
      ▼
r3d core    (core/)          ← 后端无关：模型、动画、蒙皮、变形、数学
      │
      ▼
r3d backend (backend/*/)     ← 渲染抽象 vtable：OpenGL / VGLite / Null
      │
      ▼
图形硬件 / API
```

核心引擎只与 `backend/` 暴露的抽象接口交互，新增一个后端只需实现该 vtable，
核心层无需改动。

## 渲染策略

为了在能力差异很大的硬件上统一工作，r3d 采用与具体管线无关的渲染策略：

1. CPU 端做 MVP 投影 + 透视除法，得到屏幕空间三角形；
2. 由后端把三角形提交给底层硬件/API 绘制（OpenGL 走标准管线，2.5D
   矢量后端则转成路径填充）；
3. 无统一 z-buffer 依赖：整帧收集三角形后按平均视深度排序（画家算法），
   远的先画，半透明最后画；
4. 光照在 CPU 端逐顶点计算（flat），纹理离线烘焙，从而不依赖可编程 shader。

> 这种设计让同一份模型与动画逻辑既能在 OpenGL 上渲染，也能在无 shader 的
> 2.5D 矢量加速器上渲染。

## 配置（嵌入式 / Kconfig）

在 NuttX 上启用 `CONFIG_R3D`（依赖 `ANIMATION_ENGINE`）后可配置：

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `R3D_BACKEND_VGLITE` / `R3D_BACKEND_NULL` | VGLite | 渲染后端选择 |
| `R3D_FOR_VGLITE_FB_DEV` | `/dev/fb0` | framebuffer 设备 |
| `R3D_VGLITE_DEMO` | y | 是否构建设备端 demo |
| `R3D_VGLITE_DEMO_DEFAULT_FPS` | 60 | demo 默认帧率（1–120） |
| `R3D_VGLITE_DEMO_PRIORITY` | 100 | demo 任务优先级 |
| `R3D_VGLITE_DEMO_STACKSIZE` | 32768 | demo 任务栈大小 |
| `R3D_LOG_LEVEL` | 3 | 日志级别（3=ERROR …255=OFF） |

## 运行时 API

```c
#include "r3d/r3d_engine.h"

r3d_engine_init("/dev/fb0");                                  /* 打开 fb + GPU */
r3d_engine_handle h = r3d_engine_load_file("/data/r3d/watch.b3dm");

while (running)
    r3d_engine_render_frame(h, elapsed_sec);                 /* 推进并渲染一帧 */

r3d_engine_unload(h);
r3d_engine_deinit();
```

相机为轨道（orbit）相机，默认自动旋转；可用 `r3d_engine_set_autospin()` /
`r3d_engine_set_orbit()` 控制，`r3d_engine_screenshot()` 可截屏为 PPM。

## 资产流程

离线用 `tools/gltf2b3dm` 把 glTF 转成 B3DM，推送到设备 `/data/r3d/` 即可。
B3DM 格式定义见 `include/r3d/r3d_b3dm.h`。

## 主机构建与测试

主机侧用 CMake 构建核心库与单元测试（OpenGL / Null 后端），用于在无设备环境下
开发与回归：

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build        # test_m1 / test_m2
```

> 设备（NuttX）构建由上层 animengine 工程通过 `CONFIG_R3D` 门控接入，
> 不使用本目录的 host-only `CMakeLists.txt`。
