# r3d — 基于 VGLite 的轻量 3D 引擎

r3d 是一个面向嵌入式 GPU（Vivante VGLite，如 best1600）的轻量级 3D 渲染引擎。
它在没有可编程 shader、没有硬件 z-buffer 的 2.5D 矢量加速器上，通过 **CPU 端
MVP 投影 + 三角形光栅化（vg_lite path fill）+ 画家算法排序** 实现 glTF/B3DM
模型的渲染，并作为 NuttX `ANIMATION_ENGINE` 的子组件与 LVGL 共享同一块 GPU。

## 特性

- **后端无关的核心引擎**：模型加载、骨骼/变形动画、数学库均与渲染后端解耦。
- **多后端**：VGLite（设备生产路径）、Null（无显示，用于 bring-up/CI）、
  OpenGL（主机开发对照，非设备构建）。
- **B3DM 资产格式**：紧凑的二进制模型容器（小端、16 字节对齐），由离线工具
  从 glTF 转换而来。
- **与 LVGL 共存**：GPU 由共享的 `gpu_init()` 统一初始化，r3d 以宿主模式接入，
  运行期静态守卫确保 GPU 不被重复初始化或提前关闭。

## 目录结构

| 路径 | 说明 |
|------|------|
| `include/r3d/` | 公共头文件（引擎、后端、模型、动画、数学、B3DM 格式） |
| `core/` | 后端无关核心：模型加载、动画、蒙皮、变形、数学 |
| `engine/` | NuttX 运行时封装（`r3d_engine.c`）：framebuffer + GPU 粘合层 |
| `backend/vglite/` | VGLite GPU 后端（设备生产路径） |
| `backend/null/` | Null 后端（无显示，bring-up/CI） |
| `backend/opengl/` | OpenGL 后端（主机开发对照） |
| `demo/` | `r3d_vglite_demo` 设备端 framebuffer demo |
| `tools/` | 离线工具：`gltf2b3dm`、`b3dm2gltf`、`demo`（主机查看器） |
| `tests/` | 主机单元测试（`test_m1`、`test_m2`） |
| `docs/` | 架构与评估文档 |

## 架构分层

```
应用 / demo
      │
      ▼
r3d_engine  (engine/)        ← NuttX 设备运行时：fb/gpu_init/mmap/翻页上屏
      │
      ▼
r3d core    (core/)          ← 后端无关：模型、动画、蒙皮、变形、数学
      │
      ▼
r3d backend (backend/*/)     ← 渲染抽象 vtable：VGLite / Null / OpenGL
      │
      ▼
VGLite GPU  (vg_lite + gpu_port，与 LVGL 共享)
```

## 渲染策略

VGLite 是 2.5D 矢量加速器，没有顶点/片元 shader、没有 z-buffer。r3d 的做法：

1. CPU 端做 MVP 投影 + 透视除法，得到屏幕空间三角形；
2. 每个三角形转成一条 vg_lite path（FP32），用 `vg_lite_draw` /
   `vg_lite_draw_pattern` 填充（纯色或仿射贴图）；
3. 无 z-buffer：整帧收集三角形后按平均视深度排序（画家算法），远的先画，
   半透明最后画；
4. 光照在 CPU 端逐顶点计算（flat），纹理离线烘焙。

## 配置（Kconfig）

启用 `CONFIG_R3D`（依赖 `ANIMATION_ENGINE`）后可配置：

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `R3D_BACKEND_VGLITE` / `R3D_BACKEND_NULL` | VGLite | 渲染后端选择 |
| `R3D_FOR_VGLITE_FB_DEV` | `/dev/fb0` | framebuffer 设备 |
| `R3D_VGLITE_DEMO` | y | 是否构建 `r3d_vglite_demo` |
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

## 设备端 demo

```
r3d_vglite_demo /data/r3d/tri.b3dm
```

从 framebuffer 打开 GPU，加载并自旋渲染指定 B3DM 模型。

## 资产流程

离线用 `tools/gltf2b3dm` 把 glTF 转成 B3DM，推送到设备 `/data/r3d/` 即可。
B3DM 格式定义见 `include/r3d/r3d_b3dm.h`。

## 主机构建与测试

主机侧用 CMake 构建核心库与单元测试（Null/OpenGL 后端），用于在无设备环境下
开发与回归：

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build        # test_m1 / test_m2
```

> 设备（NuttX）构建走上层 `frameworks/graphics/animengine/CMakeLists.txt`，
> 由 `CONFIG_R3D` 门控接入，不使用本目录的 host-only `CMakeLists.txt`。

## VGLite path 数据编码注意事项

向 `vg_lite_draw` 提交 FP32 格式的 path 时，path 缓冲里每个 4 字节 slot 的解释
**取决于它是 opcode 还是坐标**：

- **opcode**（`VLC_OP_MOVE/LINE/CLOSE/END`）必须按 **uint32 整数位模式** 写入
  （例如 MOVE = 字节 `02 00 00 00`）；
- **坐标** 才按 IEEE-754 **float** 写入。

若误把 opcode 写成浮点（如 `(float)2.0f` → 位模式 `0x40000000`），GPU 命令
解析器会读到非法 opcode 流，在浮点坐标下触发 tessellation 握手卡死
（`vg_lite_finish` 超时、GPU 空闲但完成中断不触达）。此约定与 LVGL、rive 的
VGLite path 实现一致，详见 `backend/vglite/backend_vglite.c` 中的注释。
