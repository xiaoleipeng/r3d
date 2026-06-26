/*
 * r3d_backend.c — 后端工厂。编译期宏选择具体后端。
 * R3D_BACKEND_VGLITE / 默认 null。
 */
#include "r3d/r3d_backend.h"

r3d_backend_t *r3d_backend_create(void)
{
#if defined(R3D_BACKEND_VGLITE)
    return r3d_backend_vglite_create();
#elif defined(R3D_BACKEND_OPENGL)
    return r3d_backend_opengl_create();
#else
    return r3d_backend_null_create();
#endif
}

void r3d_backend_destroy(r3d_backend_t *be)
{
    if (be && be->vt && be->vt->destroy)
        be->vt->destroy(be);
}
