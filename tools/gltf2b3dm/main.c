/*
 * gltf2b3dm — 离线把 glTF 编译为 B3DM（架构文档 §5）
 * M2 第一版：cgltf 解析 + stb 纹理 + 顶点定点化 + material 分组 + 序列化。
 * meshoptimizer 减面作为可选增强（本版超预算仅警告）。
 */
#include "cgltf.h"
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "r3d/r3d_b3dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ---- 命令行选项 ---- */
typedef struct {
    const char *input;
    const char *output;
    int   max_tris;
    int   tex_size;
    int   detail_tex_size; /* 高细节纹理(表盘/logo)上限，默认 1024 */
    int   quantize_pos; /* 固定 16 */
    float anim_fps;
    const char *button_tex; /* 外挂按钮拉丝纹理 */
    const char *matcap_gold;
    const char *matcap_silver;
    const char *variant; /* KHR_materials_variants 变体名子串(如 Gold) */
} opts_t;

static void usage(const char *p) {
    fprintf(stderr,
      "用法: %s input.gltf output.b3dm [选项]\n"
      "  --max-tris N      减面预算(默认 0=不减)\n"
      "  --tex-size N      纹理降采样上限(默认 256)\n", p);
}

static int parse_opts(int argc, char **argv, opts_t *o) {
    memset(o, 0, sizeof(*o));
    o->tex_size = 256;
    o->detail_tex_size = 1024;
    o->quantize_pos = 16;
    o->anim_fps = 30.0f;
    if (argc < 3) return -1;
    o->input = argv[1];
    o->output = argv[2];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--max-tris") && i+1 < argc) o->max_tris = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tex-size") && i+1 < argc) o->tex_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--detail-tex-size") && i+1 < argc) o->detail_tex_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--anim-fps") && i+1 < argc) o->anim_fps = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--button-tex") && i+1 < argc) o->button_tex = argv[++i];
        else if (!strcmp(argv[i], "--matcap-gold") && i+1 < argc) o->matcap_gold = argv[++i];
        else if (!strcmp(argv[i], "--matcap-silver") && i+1 < argc) o->matcap_silver = argv[++i];
        else if (!strcmp(argv[i], "--variant") && i+1 < argc) o->variant = argv[++i];
        else { fprintf(stderr, "未知选项: %s\n", argv[i]); return -1; }
    }
    return 0;
}

/* ---- 中间数据 ---- */
typedef struct { float px,py,pz, u,v, nx,ny,nz, ao; } wvert_t;  /* 工作顶点(float) */
typedef struct {
    wvert_t  *verts; uint32_t vcount;
    uint16_t *idx;   uint32_t icount;
    int       tex_id;     /* 该 primitive 用的纹理(-1 无) */
    int       blend;
    int       mat_flags;
    uint32_t  base_color_factor; /* ARGB，默认白 */
    uint32_t  morph_count;   /* morph target 数 */
    float    *morph_delta;   /* [morph_count*vcount*3] POSITION delta */
    uint8_t  *joints;        /* [vcount*4] 骨骼索引，NULL=无 skin */
    uint8_t  *weights;       /* [vcount*4] 权重 /255 */
    int       node_id;       /* 所属 node 索引 */
    int       is_dynamic;    /* 被动画驱动(不烘焙节点变换) */
} wprim_t;

/* ---- oct 法线编码 ---- */
static void oct_encode(float nx, float ny, float nz, int16_t *ox, int16_t *oy) {
    float s = fabsf(nx) + fabsf(ny) + fabsf(nz);
    if (s < 1e-8f) s = 1.0f;
    float x = nx / s, y = ny / s;
    if (nz < 0.0f) { float ax=x,ay=y; x=(1-fabsf(ay))*(ax>=0?1:-1); y=(1-fabsf(ax))*(ay>=0?1:-1); }
    int xi = (int)lroundf(x * 32767.0f); if (xi>32767)xi=32767; if (xi<-32767)xi=-32767;
    int yi = (int)lroundf(y * 32767.0f); if (yi>32767)yi=32767; if (yi<-32767)yi=-32767;
    *ox = (int16_t)xi; *oy = (int16_t)yi;
}

#include "g2b_meshopt_decode.h" /* 解压 EXT_meshopt_compression buffer_view */
#include "g2b_draco.h"    /* Draco 解码桥接 */
#include "g2b_anim.h"     /* 动画提取(定义 wclip_t) */
#include "g2b_extract.h"  /* cgltf 提取 → wprim_t 列表 */
#include "g2b_skin.h"     /* node 树 + skin 提取 */
#include "g2b_simplify.h" /* meshoptimizer 减面 */
#include "g2b_write.h"    /* wprim_t 列表 + 纹理 + 动画 → B3DM 序列化 */

int main(int argc, char **argv) {
    opts_t o;
    if (parse_opts(argc, argv, &o) != 0) { usage(argv[0]); return 2; }

    /* 1. cgltf 解析 */
    cgltf_options gopt = {0};
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&gopt, o.input, &data) != cgltf_result_success) {
        fprintf(stderr, "解析失败: %s\n", o.input); return 1;
    }
    if (cgltf_load_buffers(&gopt, data, o.input) != cgltf_result_success) {
        fprintf(stderr, "加载 buffer 失败\n"); cgltf_free(data); return 1;
    }

    /* 1.5 解压 EXT_meshopt_compression（cgltf 不自动解压）*/
    if (g2b_decode_meshopt(data) != 0) {
        fprintf(stderr, "警告: 存在无法解压的 meshopt buffer_view，结果可能不完整\n");
    }

    /* 2. 提取 primitives + 纹理 */
    g2b_scene_t scene = {0};
    if (g2b_extract(data, o.input, &o, &scene) != 0) {
        fprintf(stderr, "提取失败\n"); cgltf_free(data); return 1;
    }
    scene.clips = g2b_extract_anim(data, &scene.clip_count, o.anim_fps);
    g2b_extract_skin(data, &scene);
    cgltf_free(data);

    /* 2.5 减面 + cache 优化 */
    g2b_simplify(&scene, (uint32_t)o.max_tris);

    /* 3. 序列化 B3DM */
    if (g2b_write(&scene, &o) != 0) {
        fprintf(stderr, "写出失败\n"); g2b_scene_free(&scene); return 1;
    }

    printf("OK: %s → %s  (prims=%u, tris=%u, texs=%u, clips=%u)\n",
           o.input, o.output, scene.prim_count, scene.total_tris, scene.tex_count, scene.clip_count);
    g2b_scene_free(&scene);
    return 0;
}
