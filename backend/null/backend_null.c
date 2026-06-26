/*
 * backend_null.c — 空后端：vtable 全 stub，用于 M1 验证分发链路。
 */
#include "r3d/r3d_backend.h"
#include <stdlib.h>

typedef struct {
    uintptr_t next_handle;
    uint32_t  draw_calls;
} null_impl_t;

static r3d_result_t null_init(r3d_backend_t *self, const r3d_backend_cfg_t *cfg)
{ (void)self; (void)cfg; return R3D_OK; }

static void null_destroy(r3d_backend_t *self)
{
    if (!self) return;
    free(self->impl);
    free(self);
}

static r3d_texture_handle_t null_create_texture(r3d_backend_t *self, const r3d_image_t *img)
{
    (void)img;
    null_impl_t *im = (null_impl_t *)self->impl;
    return (r3d_texture_handle_t)(++im->next_handle); /* 非 0 假句柄 */
}

static void null_destroy_texture(r3d_backend_t *self, r3d_texture_handle_t h)
{ (void)self; (void)h; }

static void null_begin_frame(r3d_backend_t *self, const r3d_target_t *t)
{ (void)self; (void)t; }

static void null_set_camera(r3d_backend_t *self, const r3d_camera_t *c)
{ (void)self; (void)c; }

static void null_draw(r3d_backend_t *self, const r3d_mesh_t *mesh,
                      const r3d_mat4_t *model, const r3d_material_t *mat)
{
    (void)mesh; (void)model; (void)mat;
    ((null_impl_t *)self->impl)->draw_calls++;
}

static void null_end_frame(r3d_backend_t *self) { (void)self; }

static bool null_query_feature(r3d_backend_t *self, r3d_feature_t f)
{ (void)self; (void)f; return false; }

static const r3d_backend_vtable_t NULL_VT = {
    .init            = null_init,
    .destroy         = null_destroy,
    .create_texture  = null_create_texture,
    .destroy_texture = null_destroy_texture,
    .begin_frame     = null_begin_frame,
    .set_camera      = null_set_camera,
    .draw            = null_draw,
    .end_frame       = null_end_frame,
    .present         = NULL,           /* 空后端不呈现 */
    .query_feature   = null_query_feature,
};

r3d_backend_t *r3d_backend_null_create(void)
{
    r3d_backend_t *be = (r3d_backend_t *)calloc(1, sizeof(r3d_backend_t));
    if (!be) return NULL;
    be->impl = calloc(1, sizeof(null_impl_t));
    if (!be->impl) { free(be); return NULL; }
    be->vt = &NULL_VT;
    return be;
}
