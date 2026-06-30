/*
 * test_m2.c — 验证 gltf2b3dm 产物能被 M1 加载器读取（M2 出口）。
 * 用法: test_m2 <path-to.b3dm>
 */
#include "r3d/r3d_model.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s\n",m);g_fail++;} else printf("  [ok]   %s\n",m);}while(0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"用法: %s file.b3dm\n",argv[0]); return 2; }

    r3d_backend_t *be = r3d_backend_null_create();
    be->vt->init(be, NULL);

    r3d_model_t *m = r3d_model_load(be, argv[1]);
    CHECK(m != NULL, "load b3dm 产物");
    if (m) {
        printf("  v=%u i=%u sm=%u tex=%u\n",
               m->vertex_count, m->index_count, m->submesh_count, m->texture_count);
        CHECK(m->vertex_count == 3, "vertex_count == 3");
        CHECK(m->index_count == 3, "index_count == 3");
        CHECK(m->submesh_count == 1, "submesh_count == 1");
        CHECK(r3d_index_at(m->indices,m->index_size,0)==0 && r3d_index_at(m->indices,m->index_size,1)==1 && r3d_index_at(m->indices,m->index_size,2)==2, "indices 0,1,2");
        /* double_sided 在 gltf 设了 → mat_flags bit0 */
        CHECK((m->submeshes[0].mat_flags & 1) != 0, "double_sided flag");
        r3d_model_free(m);
    }
    r3d_backend_destroy(be);

    printf("\n%s (failures=%d)\n", g_fail?"SOME FAILED":"ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
