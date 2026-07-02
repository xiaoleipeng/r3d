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

/**
 * @file r3d_vglite_demo.c
 *
 * Standalone demo for the r3d 3D engine on NuttX, rendering B3DM models
 * directly to the framebuffer via the VGLite GPU.
 *
 * The argument may be a single .b3dm file OR a directory. When a directory
 * is given, all *.b3dm files inside are played in a loop, each shown for a
 * fixed duration (default 30s) before advancing to the next.
 *
 * Touch input (/dev/input0) drives the orbit camera: drag to rotate, pinch to
 * zoom. Double-tap the left half of the screen to zoom out (model smaller),
 * double-tap the right half to zoom in (model larger). Any touch temporarily
 * suspends auto-spin; it resumes after a short idle.
 *
 * Usage:
 *   r3d_vglite_demo <model.b3dm | dir> [options]
 *
 * Options:
 *   -d <device>   Framebuffer device (default: CONFIG_R3D_FOR_VGLITE_FB_DEV)
 *   -i <device>   Touch input device (default: /dev/input0)
 *   -f <fps>      Target frame rate (default: CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS)
 *   -t <seconds>  Seconds per model in folder mode (default: 15)
 *   -s <0|1>      Auto-spin camera (default: 1)
 *   -S <path>     Screenshot output path (default: /data/r3d_shot.ppm)
 *   -h            Show this help
 *
 * Signals:
 *   kill -USR2 <pid>   Save screenshot (PPM)
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <nuttx/input/touchscreen.h>
#include <nuttx/video/fb.h>

#include "r3d/r3d_engine.h"

#ifndef CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS
#define CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS 60
#endif

#ifndef CONFIG_R3D_FOR_VGLITE_FB_DEV
#define CONFIG_R3D_FOR_VGLITE_FB_DEV "/dev/fb0"
#endif

#define LOG_TAG               "r3d_demo"
#define DEFAULT_INPUT_DEV     "/dev/input0"
#define DEFAULT_SECS_PER_MODEL 15.0f
#define MAX_MODELS            128
#define MAX_PATH_LEN          256

/* 拖拽灵敏度：每像素旋转弧度。0.01 ≈ 拖 100px 转约 57°。 */
#define DRAG_YAW_PER_PX       0.01f
#define DRAG_PITCH_PER_PX     0.01f
/* 触摸停止后多久恢复自旋(秒) */
#define AUTOSPIN_RESUME_IDLE  3.0f

/* 双击：两次抬手间隔上限(秒) 与 位置抖动上限(像素)。
 * 屏幕左半双击缩小、右半双击放大，每次按固定倍率叠加。 */
#define DOUBLE_TAP_MAX_GAP    0.35f
#define DOUBLE_TAP_MAX_MOVE   40
#define DOUBLE_TAP_ZOOM_IN    0.7f   /* 右半：放大(距离*0.7，拉近) */
#define DOUBLE_TAP_ZOOM_OUT   1.4f   /* 左半：缩小(距离*1.4，拉远) */

static volatile bool g_running = true;
static volatile bool g_screenshot_req = false;

static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        g_running = false;
    } else if (signo == SIGUSR2) {
        g_screenshot_req = true;
    }
}

static void print_usage(const char *progname)
{
    printf("\nUsage: %s <model.b3dm | dir> [options]\n\n", progname);
    printf("Options:\n");
    printf("  -d <device>   Framebuffer device (default: %s)\n",
           CONFIG_R3D_FOR_VGLITE_FB_DEV);
    printf("  -i <device>   Touch input device (default: %s)\n", DEFAULT_INPUT_DEV);
    printf("  -f <fps>      Target frame rate (default: %d)\n",
           CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS);
    printf("  -t <seconds>  Seconds per model in folder mode (default: %.0f)\n",
           DEFAULT_SECS_PER_MODEL);
    printf("  -s <0|1>      Auto-spin camera (default: 1)\n");
    printf("  -S <path>     Screenshot output path (default: /data/r3d_shot.ppm)\n");
    printf("  -h            Show this help\n\n");
    printf("Examples:\n");
    printf("  %s /data/r3d/watch.b3dm\n", progname);
    printf("  %s /data/r3d            # loop all *.b3dm, 15s each\n", progname);
}

static float get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (float)ts.tv_sec + (float)ts.tv_nsec / 1e9f;
}

/* ---------------- 播放列表 ---------------- */

typedef struct {
    char  paths[MAX_MODELS][MAX_PATH_LEN];
    int   count;
} playlist_t;

static int has_b3dm_ext(const char *name)
{
    size_t n = strlen(name);
    return (n > 5) && (strcasecmp(name + n - 5, ".b3dm") == 0);
}

/* 把 arg 解析为播放列表：目录则枚举其中所有 *.b3dm，文件则单元素列表。 */
static int build_playlist(const char *arg, playlist_t *pl)
{
    pl->count = 0;

    struct stat st;
    if (stat(arg, &st) != 0) {
        fprintf(stderr, "[%s] cannot stat: %s (%d)\n", LOG_TAG, arg, errno);
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* 单文件 */
        snprintf(pl->paths[0], MAX_PATH_LEN, "%s", arg);
        pl->count = 1;
        return 0;
    }

    /* 目录：枚举 *.b3dm */
    DIR *dir = opendir(arg);
    if (!dir) {
        fprintf(stderr, "[%s] cannot opendir: %s (%d)\n", LOG_TAG, arg, errno);
        return -1;
    }

    size_t arglen = strlen(arg);
    int has_slash = (arglen > 0 && arg[arglen - 1] == '/');

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && pl->count < MAX_MODELS) {
        if (ent->d_name[0] == '.') continue;       /* 跳过 . / .. / 隐藏 */
        if (!has_b3dm_ext(ent->d_name)) continue;
        snprintf(pl->paths[pl->count], MAX_PATH_LEN, "%s%s%s",
                 arg, has_slash ? "" : "/", ent->d_name);
        pl->count++;
    }
    closedir(dir);

    if (pl->count == 0) {
        fprintf(stderr, "[%s] no .b3dm files in %s\n", LOG_TAG, arg);
        return -1;
    }
    return 0;
}

/* ---------------- 触摸输入 ---------------- */

typedef struct {
    int                    fd;
    uint8_t                max_points;
    size_t                 sample_size;
    struct touch_sample_s *sample;
    /* 单指拖拽(旋转)状态 */
    int                    last_x, last_y;
    int                    pressed;
    /* 双指捏合(缩放)状态 */
    int                    pinching;
    float                  pinch_dist0;   /* 进入捏合时两指间距 */
    /* 双击(缩放)状态 */
    int                    screen_w;      /* 屏幕宽度，用于判断左/右半屏 */
    int                    down_x, down_y;/* 本次按下位置(判断是否轻点) */
    int                    moved;         /* 本次按下后是否发生明显移动 */
    float                  last_tap_t;    /* 上次轻点抬手时间 */
    int                    last_tap_x;    /* 上次轻点抬手位置 */
} touch_ctx_t;

/* 读 framebuffer 分辨率，取屏宽用于双击左右判定。失败返回 0。 */
static int fb_query_width(const char *fb_dev)
{
    int fd = open(fb_dev, O_RDONLY);
    if (fd < 0) return 0;
    struct fb_videoinfo_s vinfo;
    int w = 0;
    if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) == 0)
        w = (int)vinfo.xres;
    close(fd);
    return w;
}

static int touch_open(touch_ctx_t *tc, const char *dev)
{
    memset(tc, 0, sizeof(*tc));
    tc->fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (tc->fd < 0) {
        fprintf(stderr, "[%s] touch open(%s) failed: %d (input disabled)\n",
                LOG_TAG, dev, errno);
        return -1;
    }

    uint8_t maxpoint = 0;
    if (ioctl(tc->fd, TSIOC_GETMAXPOINTS, &maxpoint) < 0 || maxpoint == 0)
        maxpoint = 1;
    tc->max_points  = maxpoint;
    tc->sample_size = SIZEOF_TOUCH_SAMPLE_S(maxpoint);
    tc->sample = (struct touch_sample_s *)malloc(tc->sample_size);
    if (!tc->sample) {
        close(tc->fd);
        tc->fd = -1;
        return -1;
    }
    printf("[%s] touch input %s enabled (maxpoints=%d)\n",
           LOG_TAG, dev, maxpoint);
    return 0;
}

static void touch_close(touch_ctx_t *tc)
{
    if (tc->sample) { free(tc->sample); tc->sample = NULL; }
    if (tc->fd >= 0) { close(tc->fd); tc->fd = -1; }
}

/* 读取触摸事件：
 *  - 单指拖拽 → 累加 yaw/pitch(旋转)
 *  - 双指捏合 → 按两指间距变化累加缩放(*zoom，倍数，>1 拉远缩小，<1 拉近放大)
 *  - 单指双击 → 屏幕左半缩小 / 右半放大(改 *zoom)；命中时 *zoom_tapped 置 1
 * 返回本次是否有触摸活动。 */
static int touch_poll(touch_ctx_t *tc, float *yaw, float *pitch, float *zoom,
                      int *zoom_tapped)
{
    if (tc->fd < 0) return 0;

    int activity = 0;
    int max_events = 32;        /* 防止事件风暴阻塞渲染 */
    while (max_events-- > 0) {
        ssize_t n = read(tc->fd, tc->sample, tc->sample_size);
        if (n < (ssize_t)sizeof(struct touch_sample_s)) break;  /* 无更多事件 */

        int np = tc->sample->npoints;
        activity = 1;

        /* ---- 双指：捏合缩放 ---- */
        if (np >= 2 && tc->max_points >= 2) {
            int x0 = tc->sample->point[0].x, y0 = tc->sample->point[0].y;
            int x1 = tc->sample->point[1].x, y1 = tc->sample->point[1].y;
            float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
            float d = sqrtf(dx*dx + dy*dy);
            if (d < 1.0f) d = 1.0f;

            if (!tc->pinching) {
                tc->pinching = 1;
                tc->pinch_dist0 = d;     /* 记录起始间距 */
            } else {
                /* 间距变大 → 放大(距离更近 → zoom 更小)。
                 * 比例 = 起始间距 / 当前间距，作用到缩放倍数上。 */
                float ratio = tc->pinch_dist0 / d;
                *zoom *= ratio;
                if (*zoom < 0.2f) *zoom = 0.2f;   /* 最近 */
                if (*zoom > 5.0f) *zoom = 5.0f;   /* 最远 */
                tc->pinch_dist0 = d;              /* 增量式：每次以当前间距为新基准 */
            }
            tc->pressed = 0;   /* 双指期间不做单指旋转 */
            tc->moved = 1;     /* 双指视为有移动，抬手不触发双击 */
            continue;
        }

        /* ---- 单指：拖拽旋转 + 双击缩放 ---- */
        tc->pinching = 0;
        uint8_t flags = tc->sample->point[0].flags;
        int x = tc->sample->point[0].x;
        int y = tc->sample->point[0].y;

        if (flags & TOUCH_DOWN) {
            tc->last_x = x;
            tc->last_y = y;
            tc->down_x = x;
            tc->down_y = y;
            tc->moved = 0;
            tc->pressed = 1;
        } else if (flags & TOUCH_MOVE) {
            if (tc->pressed) {
                int dx = x - tc->last_x;
                int dy = y - tc->last_y;
                *yaw   += (float)dx * DRAG_YAW_PER_PX;   /* 横向拖拽 → 绕 Y 转 */
                *pitch += (float)dy * DRAG_PITCH_PER_PX; /* 纵向拖拽 → 俯仰 */
                if (*pitch >  1.5f) *pitch =  1.5f;
                if (*pitch < -1.5f) *pitch = -1.5f;
                /* 累计位移超过阈值则判定为拖拽，抬手不触发双击 */
                if (abs(x - tc->down_x) > DOUBLE_TAP_MAX_MOVE ||
                    abs(y - tc->down_y) > DOUBLE_TAP_MAX_MOVE)
                    tc->moved = 1;
            }
            tc->last_x = x;
            tc->last_y = y;
        } else if (flags & TOUCH_UP) {
            /* 轻点(无明显移动)才参与双击判定 */
            if (tc->pressed && !tc->moved) {
                float t = get_time_sec();
                int w = tc->screen_w;
                /* 与上次轻点比较：时间够近且位置够近 → 双击 */
                if (t - tc->last_tap_t <= DOUBLE_TAP_MAX_GAP &&
                    abs(tc->down_x - tc->last_tap_x) <= DOUBLE_TAP_MAX_MOVE) {
                    /* 以双击点 x 判定左/右半屏；无屏宽时退化为与上次落点比较 */
                    int is_right = (w > 0) ? (tc->down_x >= w / 2)
                                           : (tc->down_x >= tc->last_tap_x);
                    if (is_right) *zoom *= DOUBLE_TAP_ZOOM_IN;   /* 右半放大 */
                    else          *zoom *= DOUBLE_TAP_ZOOM_OUT;  /* 左半缩小 */
                    if (*zoom < 0.2f) *zoom = 0.2f;
                    if (*zoom > 5.0f) *zoom = 5.0f;
                    if (zoom_tapped) *zoom_tapped = 1;
                    tc->last_tap_t = 0.0f;   /* 消费掉，避免三击连环触发 */
                } else {
                    /* 记录为本轮首次轻点 */
                    tc->last_tap_t = t;
                    tc->last_tap_x = tc->down_x;
                }
            }
            tc->pressed = 0;
        }
    }
    return activity;
}

/* ---------------- 主循环 ---------------- */

int main(int argc, FAR char *argv[])
{
    const char *fb_dev = CONFIG_R3D_FOR_VGLITE_FB_DEV;
    const char *input_dev = DEFAULT_INPUT_DEV;
    const char *arg_path = NULL;
    const char *screenshot_path = "/data/r3d_shot.ppm";
    int   target_fps = CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS;
    float secs_per_model = DEFAULT_SECS_PER_MODEL;
    int   autospin = 0;   /* 默认关闭相机自旋：只做面部 morph 动画、相机固定，
                           * 便于隔离评估合批可行性(色彩变化仅来自 morph，不含视角变化)。
                           * 需要自旋时用 -s 1 开启。 */
    int   ret;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                fb_dev = argv[++i];
            } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                input_dev = argv[++i];
            } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                target_fps = atoi(argv[++i]);
                if (target_fps <= 0 || target_fps > 120) {
                    fprintf(stderr, "Invalid fps: %s\n", argv[i]);
                    return EXIT_FAILURE;
                }
            } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
                secs_per_model = (float)atof(argv[++i]);
                if (secs_per_model <= 0.0f) secs_per_model = DEFAULT_SECS_PER_MODEL;
            } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
                autospin = atoi(argv[++i]) ? 1 : 0;
            } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
                screenshot_path = argv[++i];
            } else if (strcmp(argv[i], "-h") == 0) {
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            }
        } else {
            arg_path = argv[i];
        }
    }

    if (arg_path == NULL) {
        fprintf(stderr, "[%s] no model file or directory specified\n", LOG_TAG);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    playlist_t pl;
    if (build_playlist(arg_path, &pl) != 0) {
        return EXIT_FAILURE;
    }

    printf("[%s] source: %s (%d model%s), fb: %s, fps: %d, %.0fs each, spin: %d\n",
           LOG_TAG, arg_path, pl.count, pl.count > 1 ? "s" : "",
           fb_dev, target_fps, secs_per_model, autospin);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR2, signal_handler);

    ret = r3d_engine_init(fb_dev);
    if (ret != R3D_ENGINE_OK) {
        fprintf(stderr, "[%s] r3d_engine_init failed: %d\n", LOG_TAG, ret);
        return EXIT_FAILURE;
    }

    touch_ctx_t touch;
    touch_open(&touch, input_dev);   /* 失败不致命：仅禁用输入 */
    touch.screen_w = fb_query_width(fb_dev);  /* 双击左右半屏判定用 */

    float frame_interval = 1.0f / (float)target_fps;
    float last_time = get_time_sec();
    uint32_t frame_count = 0;
    float fps_timer = 0.0f;

    int play_idx = 0;
    int exit_code = EXIT_SUCCESS;

    /* 外层循环：依次/循环播放每个模型 */
    while (g_running) {
        const char *model_path = pl.paths[play_idx];

        r3d_engine_handle handle = r3d_engine_load_file(model_path);
        if (handle == NULL) {
            fprintf(stderr, "[%s] failed to load: %s (skip)\n", LOG_TAG, model_path);
            /* 单文件加载失败则退出；多文件则跳过该文件 */
            if (pl.count == 1) { exit_code = EXIT_FAILURE; break; }
            play_idx = (play_idx + 1) % pl.count;
            continue;
        }

        r3d_engine_set_autospin(handle, autospin);
        printf("[%s] playing [%d/%d]: %s\n", LOG_TAG,
               play_idx + 1, pl.count, model_path);

        /* 触摸 orbit 状态(每个模型重置视角) */
        float yaw = 0.0f, pitch = 0.0f;
        float zoom = 1.0f;          /* 缩放倍数：<1 放大(拉近)，>1 缩小(拉远) */
        float model_elapsed = 0.0f;
        float idle_since_touch = AUTOSPIN_RESUME_IDLE;  /* 初始即允许自旋 */

        /* 内层循环：播放当前模型 secs_per_model 秒(单文件模式则一直播) */
        while (g_running) {
            float now = get_time_sec();
            float elapsed = now - last_time;
            last_time = now;

            /* 处理触摸：单指拖拽改 yaw/pitch(暂停自旋)，双指捏合改 zoom，
             * 单指双击(左半缩小/右半放大)改 zoom。
             * 交互期间暂停切换计时：用户正在看/操作当前模型时不累计 model_elapsed，
             * 松手后才继续累计，避免操作过程中被切走。 */
            int zoom_tapped = 0;
            int interacting = touch_poll(&touch, &yaw, &pitch, &zoom, &zoom_tapped);
            if (interacting) {
                idle_since_touch = 0.0f;
                if (zoom_tapped) {
                    /* 双击缩放是离散事件：只改距离、不动 yaw/pitch，
                     * 不打断自旋(若正在自旋则继续转，只是远近变了)。 */
                    r3d_engine_set_zoom(handle, zoom);
                } else {
                    /* 拖拽/捏合：暂停自旋并按 orbit 设置相机 */
                    r3d_engine_set_autospin(handle, 0);
                    r3d_engine_set_orbit(handle, yaw, pitch, zoom);
                }
            } else {
                /* 无触摸：累计切换计时；只叠加缩放(不改 yaw/pitch，避免打断自旋)；
                 * 空闲足够久后恢复自旋。 */
                model_elapsed += elapsed;
                r3d_engine_set_zoom(handle, zoom);
                if (autospin) {
                    idle_since_touch += elapsed;
                    if (idle_since_touch >= AUTOSPIN_RESUME_IDLE)
                        r3d_engine_set_autospin(handle, 1);
                }
            }

            ret = r3d_engine_render_frame(handle, elapsed);
            if (ret != R3D_ENGINE_OK) {
                fprintf(stderr, "[%s] render_frame failed: %d\n", LOG_TAG, ret);
                g_running = false;
                exit_code = EXIT_FAILURE;
                break;
            }

            if (g_screenshot_req) {
                g_screenshot_req = false;
                int sret = r3d_engine_screenshot(screenshot_path);
                printf("[%s] screenshot %s: %s\n", LOG_TAG, screenshot_path,
                       sret == 0 ? "OK" : "FAILED");
            }

            frame_count++;
            fps_timer += elapsed;
            if (fps_timer >= 1.0f) {
                printf("[%s] fps: %.1f\n", LOG_TAG, (float)frame_count / fps_timer);
                frame_count = 0;
                fps_timer = 0.0f;
            }

            /* 多文件模式：到时切下一个 */
            if (pl.count > 1 && model_elapsed >= secs_per_model)
                break;

            float render_time = get_time_sec() - now;
            float sleep_time = frame_interval - render_time;
            if (sleep_time > 0.001f)
                usleep((useconds_t)(sleep_time * 1e6f));
        }

        r3d_engine_unload(handle);

        /* 多文件：循环到下一个；单文件：内层只在收到退出信号或出错时结束，
         * 外层 while(g_running) 随之退出，无需特殊处理。 */
        if (pl.count > 1)
            play_idx = (play_idx + 1) % pl.count;
    }

    printf("[%s] stopping...\n", LOG_TAG);
    touch_close(&touch);
    r3d_engine_deinit();
    printf("[%s] done\n", LOG_TAG);
    return exit_code;
}
