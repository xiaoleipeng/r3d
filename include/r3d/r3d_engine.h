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
#include "r3d/r3d_types.h"
#include "r3d/r3d_backend.h"   /* r3d_render_hook_t / r3d_render_stage_t */

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

/* 增量旋转(轨迹球)。d_yaw/d_pitch 为本次的角度增量(弧度)，分别绕相机当前的
 * 局部 up / right 轴。内部只做四元数后乘，无万向锁、无需夹 pitch，可连续越过
 * 极点。交互式拖拽应优先用本接口。 */
void r3d_engine_orbit_delta(r3d_engine_handle handle,
                            float d_yaw, float d_pitch, float dist_scale);
/* 仅设置缩放(相对默认距离的倍数，<1 放大、>1 缩小)，不改变 yaw/pitch，
 * 因此可在自旋进行时叠加缩放而不打断旋转。 */
void r3d_engine_set_zoom(r3d_engine_handle handle, float dist_scale);

/* 截图：把当前 framebuffer 存为 PPM。返回 0 成功。 */
int r3d_engine_screenshot(const char *path);

/* 光照参数（运行时可调，所见即所得地贴近 glTF 中性外观）。
 * 传 NULL 恢复默认。后端不支持时静默忽略。返回 0 成功。 */
int r3d_engine_set_lighting(const r3d_light_params_t *lp);
/* 取当前光照参数(便于在默认基础上微调)。返回 0 成功。 */
/* 画面垂直偏移(像素，+ 向下)。用投影的垂直镜头偏移实现，纯屏幕平移，
 * 物体轮廓半径不变。圆屏表盘想在上方腾出空间放大字号时用。 */
int r3d_engine_set_view_shift(float dy_px);

/* 上一帧的物体屏幕半径(像素)，0 = 尚未渲染过。
 * 供上层按实际尺寸排布覆盖层，避免把布局写死成像素常量。 */
float r3d_engine_get_screen_radius(void);

/* 注册渲染钩子，让上层在引擎绘制流程中插入自己的 2D 绘制(背景/覆盖层)。
 * 详见 r3d_backend.h 的 r3d_render_hook_t。fn=NULL 取消。 */
int r3d_engine_set_render_hook(r3d_render_hook_t fn, void *user);

/* 清屏颜色(ARGB8888)。默认 0xFF1F1F26，太空类场景可设 0xFF000000。 */
int r3d_engine_set_clear_color(uint32_t argb);

int r3d_engine_get_lighting(r3d_light_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __R3D_ENGINE_H__ */
