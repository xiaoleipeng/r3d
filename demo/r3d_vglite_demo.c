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
 * Standalone demo for the r3d 3D engine on NuttX, rendering a B3DM model
 * directly to the framebuffer via the VGLite GPU. The orbit camera
 * auto-rotates (same behaviour as tools/demo/demo_viewer.c).
 *
 * Usage:
 *   r3d_vglite_demo <model.b3dm> [-d <fb_dev>] [-f <fps>] [-S <png>]
 *
 * Examples:
 *   r3d_vglite_demo /data/r3d/watch.b3dm
 *   r3d_vglite_demo /data/r3d/watch.b3dm -d /dev/fb0 -f 60
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

#include "r3d/r3d_engine.h"

#ifndef CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS
#define CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS 60
#endif

#ifndef CONFIG_R3D_FOR_VGLITE_FB_DEV
#define CONFIG_R3D_FOR_VGLITE_FB_DEV "/dev/fb0"
#endif

#define LOG_TAG "r3d_demo"

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
    printf("\nUsage: %s <model.b3dm> [options]\n\n", progname);
    printf("Options:\n");
    printf("  -d <device>   Framebuffer device (default: %s)\n",
           CONFIG_R3D_FOR_VGLITE_FB_DEV);
    printf("  -f <fps>      Target frame rate (default: %d)\n",
           CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS);
    printf("  -s <0|1>      Auto-spin camera (default: 1)\n");
    printf("  -S <path>     Screenshot output path (default: /data/r3d_shot.ppm)\n");
    printf("  -h            Show this help\n\n");
    printf("Examples:\n");
    printf("  %s /data/r3d/watch.b3dm\n", progname);
    printf("  %s /data/r3d/watch.b3dm -f 30 -s 0\n", progname);
}

static float get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (float)ts.tv_sec + (float)ts.tv_nsec / 1e9f;
}

int main(int argc, FAR char *argv[])
{
    const char *fb_dev = CONFIG_R3D_FOR_VGLITE_FB_DEV;
    const char *model_path = NULL;
    const char *screenshot_path = "/data/r3d_shot.ppm";
    int target_fps = CONFIG_R3D_VGLITE_DEMO_DEFAULT_FPS;
    int autospin = 1;
    int ret;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                fb_dev = argv[++i];
            } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                target_fps = atoi(argv[++i]);
                if (target_fps <= 0 || target_fps > 120) {
                    fprintf(stderr, "Invalid fps: %s\n", argv[i]);
                    return EXIT_FAILURE;
                }
            } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
                autospin = atoi(argv[++i]) ? 1 : 0;
            } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
                screenshot_path = argv[++i];
            } else if (strcmp(argv[i], "-h") == 0) {
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            }
        } else {
            model_path = argv[i];
        }
    }

    if (model_path == NULL) {
        fprintf(stderr, "[%s] no model file specified\n", LOG_TAG);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    printf("[%s] model: %s, fb: %s, fps: %d, spin: %d\n",
           LOG_TAG, model_path, fb_dev, target_fps, autospin);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR2, signal_handler);

    ret = r3d_engine_init(fb_dev);
    if (ret != R3D_ENGINE_OK) {
        fprintf(stderr, "[%s] r3d_engine_init failed: %d\n", LOG_TAG, ret);
        return EXIT_FAILURE;
    }

    r3d_engine_handle handle = r3d_engine_load_file(model_path);
    if (handle == NULL) {
        fprintf(stderr, "[%s] failed to load: %s\n", LOG_TAG, model_path);
        r3d_engine_deinit();
        return EXIT_FAILURE;
    }
    r3d_engine_set_autospin(handle, autospin);

    printf("[%s] playing: %s\n", LOG_TAG, model_path);

    float frame_interval = 1.0f / (float)target_fps;
    float last_time = get_time_sec();
    uint32_t frame_count = 0;
    float fps_timer = 0.0f;

    while (g_running) {
        float now = get_time_sec();
        float elapsed = now - last_time;
        last_time = now;

        ret = r3d_engine_render_frame(handle, elapsed);
        if (ret != R3D_ENGINE_OK) {
            fprintf(stderr, "[%s] render_frame failed: %d\n", LOG_TAG, ret);
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

        float render_time = get_time_sec() - now;
        float sleep_time = frame_interval - render_time;
        if (sleep_time > 0.001f) {
            usleep((useconds_t)(sleep_time * 1e6f));
        }
    }

    printf("[%s] stopping...\n", LOG_TAG);
    r3d_engine_unload(handle);
    r3d_engine_deinit();
    printf("[%s] done\n", LOG_TAG);
    return EXIT_SUCCESS;
}
