/****************************************************************************
 *
 * Copyright (C) 2025 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

/*
 * r3d_engine.h — r3d 在 NuttX 设备上的运行时封装层（对照 rive_engine.h）。
 *
 * 负责把 r3d 核心引擎(后端无关) + VGLite 后端 与 NuttX framebuffer / vg_lite
 * GPU 粘合起来：打开 /dev/fbN、gpu_init、mmap、把 fb 包成 r3d 渲染目标，
 * 每帧推进动画并渲染、翻页上屏。相机为轨道(orbit)相机，默认自动旋转
 * (与 tools/demo/demo_viewer.c 行为一致)。
 *
 * 用法:
 *   r3d_engine_init("/dev/fb0");
 *   r3d_engine_handle h = r3d_engine_load_file("/data/r3d/watch.b3dm");
 *   while (running) r3d_engine_render_frame(h, elapsed_sec);
 *   r3d_engine_unload(h);
 *   r3d_engine_deinit();
 */
#ifndef __R3D_ENGINE_H__
#define __R3D_ENGINE_H__

#include <stdint.h>

typedef void *r3d_engine_handle;

#ifdef __cplusplus
extern "C" {
#endif

/* 结果码 */
#define R3D_ENGINE_OK          0
#define R3D_ENGINE_ERR_INIT   -1
#define R3D_ENGINE_ERR_PARAM  -2
#define R3D_ENGINE_ERR_NOMEM  -3
#define R3D_ENGINE_ERR_FB     -4
#define R3D_ENGINE_ERR_LOAD   -5

/* 引擎生命周期 */
int r3d_engine_init(const char *fb_dev);
int r3d_engine_deinit(void);

/* 模型资源 */
r3d_engine_handle r3d_engine_load(const void *data, uint32_t size);
r3d_engine_handle r3d_engine_load_file(const char *path);
void              r3d_engine_unload(r3d_engine_handle handle);

/* 渲染：推进 elapsed 秒并渲染一帧到 framebuffer */
int r3d_engine_render_frame(r3d_engine_handle handle, float elapsed);

/* 相机控制 */
int  r3d_engine_set_autospin(r3d_engine_handle handle, int enable);
void r3d_engine_set_orbit(r3d_engine_handle handle,
                          float yaw, float pitch, float dist_scale);

/* 截图：把当前 framebuffer 存为 PPM。返回 0 成功。 */
int r3d_engine_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* __R3D_ENGINE_H__ */
