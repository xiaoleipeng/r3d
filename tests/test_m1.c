/*
 * test_m1.c — M1 骨架验证
 *  1) math 单元测试对拍
 *  2) 构造最小 B3DM + null 后端加载 + vtable 分发
 */
#include "r3d/r3d_math.h"
#include "r3d/r3d_model.h"
#include "r3d/r3d_b3dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; } \
    else         { printf("  [ok]   %s\n", msg); } \
} while (0)

static int almost(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* ---- math 测试 ---- */
static void test_math(void)
{
    printf("== math ==\n");
    r3d_mat4_t id; r3d_mat4_identity(&id);
    CHECK(almost(id.m[0],1) && almost(id.m[5],1) && almost(id.m[10],1) && almost(id.m[15],1),
          "identity diagonal");
    CHECK(almost(id.m[1],0) && almost(id.m[4],0), "identity off-diagonal");

    /* identity * v = v */
    r3d_vec4_t v = {1,2,3,1};
    r3d_vec4_t r = r3d_mat4_mul_vec4(&id, v);
    CHECK(almost(r.x,1) && almost(r.y,2) && almost(r.z,3) && almost(r.w,1), "I*v=v");

    /* A*I = A */
    r3d_mat4_t a; r3d_mat4_identity(&a); a.m[12]=5; a.m[13]=6; a.m[14]=7; /* 平移 */
    r3d_mat4_t prod; r3d_mat4_mul(&prod, &a, &id);
    CHECK(almost(prod.m[12],5) && almost(prod.m[13],6) && almost(prod.m[14],7), "A*I=A");

    /* 平移矩阵作用于点 */
    r3d_vec4_t p = {0,0,0,1};
    r3d_vec4_t tp = r3d_mat4_mul_vec4(&a, p);
    CHECK(almost(tp.x,5) && almost(tp.y,6) && almost(tp.z,7), "translate point");

    /* cross/dot */
    r3d_vec3_t x={1,0,0}, y={0,1,0};
    r3d_vec3_t z = r3d_vec3_cross(x,y);
    CHECK(almost(z.x,0) && almost(z.y,0) && almost(z.z,1), "cross x,y=z");
    CHECK(almost(r3d_vec3_dot(x,y),0), "dot x,y=0");

    /* normalize */
    r3d_vec3_t n = r3d_vec3_normalize((r3d_vec3_t){3,4,0});
    CHECK(almost(n.x,0.6f) && almost(n.y,0.8f), "normalize 3,4,0");

    /* slerp 端点 */
    r3d_quat_t q0={0,0,0,1}, q1={0,0,0,1};
    r3d_quat_t qs = r3d_quat_slerp(q0,q1,0.5f);
    CHECK(almost(qs.w,1), "slerp identical");

    /* perspective 关键项符号 */
    r3d_mat4_t pj; r3d_mat4_perspective(&pj, 1.0f, 1.0f, 0.1f, 100.0f);
    CHECK(pj.m[11] < 0, "perspective w-row = -1");
    CHECK(pj.m[0] > 0 && pj.m[5] > 0, "perspective scale > 0");
}

/* ---- 构造最小 B3DM（1 三角形，无纹理）---- */
static void *build_min_b3dm(uint32_t *out_size)
{
    /* 布局：header(64) + section_table(2*16) + vertex(3*16) + index(3*u16, 补齐) */
    uint32_t off_tbl  = sizeof(r3d_b3dm_header_t);          /* 64 */
    uint32_t off_vtx  = off_tbl + 2*sizeof(r3d_b3dm_section_t); /* 96 */
    uint32_t vtx_sz   = 3*sizeof(r3d_b3dm_vertex_t);        /* 48 */
    uint32_t off_idx  = off_vtx + vtx_sz;                   /* 144 */
    uint32_t idx_sz   = 4*sizeof(uint16_t);                 /* 3 索引 + 1 补齐 = 8 */
    uint32_t total    = off_idx + idx_sz;

    uint8_t *buf = (uint8_t *)calloc(1, total);

    r3d_b3dm_header_t *h = (r3d_b3dm_header_t *)buf;
    h->magic = R3D_B3DM_MAGIC;
    h->version = R3D_B3DM_VERSION;
    h->flags = 0;
    h->section_count = 2;
    h->bounding_sphere[3] = 1.0f;
    h->vertex_scale[0]=h->vertex_scale[1]=h->vertex_scale[2]=1.0f/32767.0f;
    h->vertex_bias[0]=h->vertex_bias[1]=h->vertex_bias[2]=0.0f;

    r3d_b3dm_section_t *tbl = (r3d_b3dm_section_t *)(buf + off_tbl);
    tbl[0].type=R3D_SEC_VERTEX; tbl[0].offset=off_vtx; tbl[0].size=vtx_sz; tbl[0].count=3;
    tbl[1].type=R3D_SEC_INDEX;  tbl[1].offset=off_idx; tbl[1].size=idx_sz; tbl[1].count=3;

    r3d_b3dm_vertex_t *v = (r3d_b3dm_vertex_t *)(buf + off_vtx);
    /* 定点 32767 → 还原为 1.0 */
    v[0].pos[0]=0;      v[0].pos[1]=0;      v[0].pos[2]=0;
    v[1].pos[0]=32767;  v[1].pos[1]=0;      v[1].pos[2]=0;
    v[2].pos[0]=0;      v[2].pos[1]=32767;  v[2].pos[2]=0;
    v[0].uv[0]=0;     v[0].uv[1]=0;
    v[1].uv[0]=65535; v[1].uv[1]=0;
    v[2].uv[0]=0;     v[2].uv[1]=65535;

    uint16_t *idx = (uint16_t *)(buf + off_idx);
    idx[0]=0; idx[1]=1; idx[2]=2;

    *out_size = total;
    return buf;
}

static void test_loader(void)
{
    printf("== loader ==\n");
    r3d_backend_t *be = r3d_backend_null_create();
    CHECK(be != NULL, "null backend create");
    be->vt->init(be, NULL);

    uint32_t sz=0;
    void *buf = build_min_b3dm(&sz);
    r3d_model_t *m = r3d_model_load_mem(be, buf, sz);
    CHECK(m != NULL, "model load");
    if (m) {
        CHECK(m->vertex_count == 3, "vertex_count == 3");
        CHECK(m->index_count == 3, "index_count == 3");
        CHECK(almost(m->vertices[1].pos.x, 1.0f), "vtx1.x decoded == 1.0");
        CHECK(almost(m->vertices[2].pos.y, 1.0f), "vtx2.y decoded == 1.0");
        CHECK(almost(m->vertices[1].uv.x, 1.0f), "vtx1.uv.x == 1.0");
        CHECK(m->indices[0]==0 && m->indices[1]==1 && m->indices[2]==2, "indices");

        /* 跑通 vtable 分发：begin/set_camera/draw/end */
        r3d_target_t tgt = {0}; tgt.w=64; tgt.h=64;
        r3d_camera_t cam; r3d_mat4_identity(&cam.view); r3d_mat4_identity(&cam.proj);
        be->vt->begin_frame(be, &tgt);
        be->vt->set_camera(be, &cam);
        r3d_mesh_t mesh = {0};
        mesh.vertices=m->vertices; mesh.vertex_count=m->vertex_count;
        mesh.indices=m->indices;   mesh.index_count=m->index_count;
        r3d_mat4_t model; r3d_mat4_identity(&model);
        r3d_material_t mat = {0};
        be->vt->draw(be, &mesh, &model, &mat);
        be->vt->end_frame(be);
        CHECK(1, "vtable dispatch ran");
        CHECK(be->vt->query_feature(be, R3D_FEATURE_ZBUFFER)==false, "null feature=false");

        r3d_model_free(m);
    }
    r3d_backend_destroy(be);
}

int main(void)
{
    test_math();
    test_loader();
    printf("\n%s (failures=%d)\n", g_fail==0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail ? 1 : 0;
}
