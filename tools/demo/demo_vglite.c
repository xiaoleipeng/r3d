/*
 * demo_vglite.c — 无头(headless)VGLite 后端渲染验证。
 * 加载 B3DM，用 VGLite 后端(本机搭配 vg_lite_softsim CPU 实现)渲染一帧到内存缓冲，
 * 输出 PPM。验证 r3d VGLite 后端的投影/纹理映射/画家算法/matcap 逻辑。
 * 用法: demo_vglite model.b3dm [out.ppm]
 * 相机环境变量: R3D_YAW R3D_PITCH R3D_DIST(与 OpenGL demo 一致)
 */
#include "r3d/r3d_backend.h"
#include "r3d/r3d_model.h"
#include "r3d/r3d_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 800
#define H 600

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s model.b3dm [out.ppm]\n", argv[0]); return 2; }
    const char *out = (argc >= 3) ? argv[2] : "/tmp/r3d_vglite.ppm";

    r3d_backend_t *be = r3d_backend_create();   /* 编译期 = vglite */
    if (!be || be->vt->init(be, NULL) != R3D_OK) { fprintf(stderr, "VGLite init 失败\n"); return 1; }

    r3d_model_t *m = r3d_model_load(be, argv[1]);
    if (!m) { fprintf(stderr, "加载失败: %s\n", argv[1]); return 1; }
    printf("模型: v=%u i=%u submesh=%u tex=%u\n",
           m->vertex_count, m->index_count, m->submesh_count, m->texture_count);

    /* 目标缓冲(ARGB8888) */
    uint32_t *pixels = (uint32_t *)malloc((size_t)W * H * 4);
    r3d_target_t target = { pixels, W, H, W * 4, R3D_FMT_ARGB8888, NULL };

    /* 包围球 */
    float r = m->bounds.max.x - m->bounds.min.x;
    float ry = m->bounds.max.y - m->bounds.min.y;
    if (ry > r) r = ry; if (r < 1e-3f) r = 1.0f;
    r3d_vec3_t center = { (m->bounds.min.x + m->bounds.max.x) * 0.5f,
                          (m->bounds.min.y + m->bounds.max.y) * 0.5f,
                          (m->bounds.min.z + m->bounds.max.z) * 0.5f };

    float yaw = getenv("R3D_YAW") ? atof(getenv("R3D_YAW")) : -0.5f;
    float pitch = getenv("R3D_PITCH") ? atof(getenv("R3D_PITCH")) : 0.3f;
    float dist0 = r * 1.6f + 0.5f;
    float dist = getenv("R3D_DIST") ? dist0 * atof(getenv("R3D_DIST")) : dist0;

    r3d_vec3_t eye = { center.x + dist * cosf(pitch) * sinf(yaw),
                       center.y + dist * sinf(pitch),
                       center.z + dist * cosf(pitch) * cosf(yaw) };
    r3d_camera_t cam;
    r3d_mat4_look_at(&cam.view, eye, center, (r3d_vec3_t){0,1,0});
    r3d_mat4_perspective(&cam.proj, 1.0f, (float)W / H, 0.05f, 100.0f);
    cam.viewport = (r3d_viewport_t){ 0, 0, W, H };

    /* 渲染一帧 */
    be->vt->begin_frame(be, &target);
    be->vt->set_camera(be, &cam);

    r3d_mat4_t model; r3d_mat4_identity(&model);
    /* 两趟：先不透明后半透明 */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t s = 0; s < m->submesh_count; s++) {
            r3d_submesh_t *sm = &m->submeshes[s];
            int tl = (sm->mat_flags & R3D_MAT_TRANSLUCENT) ? 1 : 0;
            if (tl != pass) continue;
            r3d_mesh_t mesh = {0};
            mesh.vertices = m->vertices; mesh.vertex_count = m->vertex_count;
            mesh.indices = m->indices + sm->index_offset; mesh.index_count = sm->index_count;
            r3d_material_t mat = {0};
            mat.base_color = sm->base_color; mat.matcap = sm->matcap;
            mat.blend = sm->blend; mat.flags = sm->mat_flags;
            mat.base_color_factor = sm->base_color_factor;
            be->vt->draw(be, &mesh, &model, &mat);
        }
    }
    be->vt->end_frame(be);

    /* 写 PPM(ARGB8888 → RGB) */
    FILE *fp = fopen(out, "wb");
    if (!fp) { fprintf(stderr, "无法写 %s\n", out); return 1; }
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint32_t p = pixels[i];
        uint8_t rgb[3] = { (p>>16)&0xFF, (p>>8)&0xFF, p&0xFF };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    printf("已渲染 → %s\n", out);

    free(pixels);
    be->vt->destroy(be);
    return 0;
}
