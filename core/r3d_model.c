/*
 * r3d_model.c — B3DM 加载器
 * 对应架构文档 §4.4。读文件→校验→段映射→顶点解码→纹理上传。
 */
#include "r3d/r3d_model.h"
#include "r3d/r3d_b3dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const r3d_b3dm_section_t *
find_section(const r3d_b3dm_header_t *hdr, const r3d_b3dm_section_t *tbl,
             uint32_t type)
{
    for (uint32_t i = 0; i < hdr->section_count; i++)
        if (tbl[i].type == type) return &tbl[i];
    return NULL;
}

r3d_model_t *r3d_model_load_mem(r3d_backend_t *backend, void *buf, uint32_t size)
{
    if (!buf || size < sizeof(r3d_b3dm_header_t)) return NULL;

    const r3d_b3dm_header_t *hdr = (const r3d_b3dm_header_t *)buf;
    if (hdr->magic != R3D_B3DM_MAGIC) {
        fprintf(stderr, "[r3d] 非 B3DM 文件 (magic 不匹配)\n");
        return NULL;
    }
    if (hdr->version != R3D_B3DM_VERSION) {
        fprintf(stderr, "[r3d] B3DM 版本不匹配: 文件 v%u, 期望 v%u —— 请用当前 gltf2b3dm 重新转换\n",
                hdr->version, R3D_B3DM_VERSION);
        return NULL;
    }

    const uint8_t *base = (const uint8_t *)buf;
    const r3d_b3dm_section_t *tbl =
        (const r3d_b3dm_section_t *)(base + sizeof(r3d_b3dm_header_t));

    /* 校验 section_table 在范围内 */
    if (sizeof(r3d_b3dm_header_t) + (size_t)hdr->section_count * sizeof(r3d_b3dm_section_t) > size)
        return NULL;

    r3d_model_t *m = (r3d_model_t *)calloc(1, sizeof(r3d_model_t));
    if (!m) return NULL;
    m->raw = buf;
    m->backend = backend;

    /* ---- VERTEX ---- */
    const r3d_b3dm_section_t *sv = find_section(hdr, tbl, R3D_SEC_VERTEX);
    if (!sv) goto fail;
    {
        const r3d_b3dm_vertex_t *qv =
            (const r3d_b3dm_vertex_t *)(base + sv->offset);
        m->vertex_count = sv->count;
        m->vertices = (r3d_vertex_t *)calloc(sv->count, sizeof(r3d_vertex_t));
        if (!m->vertices) goto fail;

        const float *sc = hdr->vertex_scale;
        const float *bi = hdr->vertex_bias;
        for (uint32_t i = 0; i < sv->count; i++) {
            m->vertices[i].pos.x = qv[i].pos[0] * sc[0] + bi[0];
            m->vertices[i].pos.y = qv[i].pos[1] * sc[1] + bi[1];
            m->vertices[i].pos.z = qv[i].pos[2] * sc[2] + bi[2];
            m->vertices[i].uv.x  = qv[i].uv[0] / 65535.0f;
            m->vertices[i].uv.y  = qv[i].uv[1] / 65535.0f;
            /* oct 法线解码（还原 z 并归一化）*/
            float ex = qv[i].normal_oct[0] / 32767.0f;
            float ey = qv[i].normal_oct[1] / 32767.0f;
            float nz = 1.0f - (ex<0?-ex:ex) - (ey<0?-ey:ey);
            float nx = ex, ny = ey;
            if (nz < 0.0f) {
                float ax = nx, ay = ny;
                nx = (1.0f - (ay<0?-ay:ay)) * (ax>=0?1.0f:-1.0f);
                ny = (1.0f - (ax<0?-ax:ax)) * (ay>=0?1.0f:-1.0f);
            }
            float inv = 1.0f / sqrtf(nx*nx + ny*ny + nz*nz);
            m->vertices[i].normal.x = nx*inv;
            m->vertices[i].normal.y = ny*inv;
            m->vertices[i].normal.z = nz*inv;
            m->vertices[i].ao = qv[i].pad[0] / 255.0f;
        }
    }

    /* ---- INDEX ---- */
    const r3d_b3dm_section_t *si = find_section(hdr, tbl, R3D_SEC_INDEX);
    if (!si) goto fail;
    m->index_count = si->count;
    m->indices = (uint16_t *)(base + si->offset); /* 零拷贝指向 buf */

    /* ---- SUBMESH ---- */
    const r3d_b3dm_section_t *ss = find_section(hdr, tbl, R3D_SEC_SUBMESH);
    if (ss) {
        const r3d_b3dm_submesh_t *qs =
            (const r3d_b3dm_submesh_t *)(base + ss->offset);
        m->submesh_count = ss->count;
        m->submeshes = (r3d_submesh_t *)calloc(ss->count, sizeof(r3d_submesh_t));
        if (!m->submeshes) goto fail;
        for (uint32_t i = 0; i < ss->count; i++) {
            m->submeshes[i].blend        = (r3d_blend_t)qs[i].blend;
            m->submeshes[i].mat_flags    = qs[i].mat_flags;
            m->submeshes[i].index_offset = qs[i].index_offset;
            m->submeshes[i].index_count  = qs[i].index_count;
            m->submeshes[i].base_color_factor = qs[i].base_color_factor;
            m->submeshes[i].node_id = qs[i].node_id;
            m->submeshes[i].base_color   = R3D_TEXTURE_NONE; /* 填表后绑定 */
            m->submeshes[i].matcap       = R3D_TEXTURE_NONE;
        }
        /* 记录 tex_id 待绑定 */
        /* ---- TEXTURE ---- */
        const r3d_b3dm_section_t *st = find_section(hdr, tbl, R3D_SEC_TEXTURE);
        if (st && backend && backend->vt->create_texture) {
            const r3d_b3dm_texture_t *qt =
                (const r3d_b3dm_texture_t *)(base + st->offset);
            m->texture_count = st->count;
            m->textures = (r3d_texture_handle_t *)calloc(st->count, sizeof(r3d_texture_handle_t));
            if (!m->textures) goto fail;
            for (uint32_t i = 0; i < st->count; i++) {
                r3d_image_t img;
                img.w      = qt[i].width;
                img.h      = qt[i].height;
                img.format = (r3d_pixel_format_t)qt[i].format;
                img.data   = base + st->offset + qt[i].data_offset;
                img.size   = qt[i].data_size;
                img.stride = qt[i].width * 4; /* ARGB8888 */
                m->textures[i] = backend->vt->create_texture(backend, &img);
            }
            /* 绑定 submesh → texture */
            for (uint32_t i = 0; i < ss->count; i++) {
                if (qs[i].tex_id != 0xFFFF && qs[i].tex_id < m->texture_count)
                    m->submeshes[i].base_color = m->textures[qs[i].tex_id];
                if (qs[i].matcap_id != 0xFFFF && qs[i].matcap_id < m->texture_count)
                    m->submeshes[i].matcap = m->textures[qs[i].matcap_id];
            }
        }
    }

    m->bounds.min.x = hdr->bounding_sphere[0] - hdr->bounding_sphere[3];
    m->bounds.min.y = hdr->bounding_sphere[1] - hdr->bounding_sphere[3];
    m->bounds.min.z = hdr->bounding_sphere[2] - hdr->bounding_sphere[3];
    m->bounds.max.x = hdr->bounding_sphere[0] + hdr->bounding_sphere[3];
    m->bounds.max.y = hdr->bounding_sphere[1] + hdr->bounding_sphere[3];
    m->bounds.max.z = hdr->bounding_sphere[2] + hdr->bounding_sphere[3];

    /* ---- MORPH（可选）---- */
    const r3d_b3dm_section_t *smo = find_section(hdr, tbl, R3D_SEC_MORPH);
    if (smo) {
        const r3d_b3dm_morph_t *mh = (const r3d_b3dm_morph_t *)(base + smo->offset);
        m->morph_target_count = mh->target_count;
        m->morph_deltas = (const float *)(base + smo->offset + mh->deltas_offset);
    }

    /* ---- NODE / SKELETON / SKINVTX（可选，skin）---- */
    const r3d_b3dm_section_t *snd = find_section(hdr, tbl, R3D_SEC_NODE);
    if (snd) { m->nodes = base + snd->offset; m->node_count = snd->count; }
    const r3d_b3dm_section_t *ssk = find_section(hdr, tbl, R3D_SEC_SKELETON);
    if (ssk) {
        const r3d_b3dm_skeleton_t *sh = (const r3d_b3dm_skeleton_t *)(base + ssk->offset);
        m->joint_count = sh->joint_count;
        m->inv_bind    = (const float *)(base + ssk->offset + sh->inverse_bind_offset);
        m->joint_nodes = (const uint16_t *)(base + ssk->offset + sh->joint_node_offset);
    }
    const r3d_b3dm_section_t *ssv = find_section(hdr, tbl, R3D_SEC_SKINVTX);
    if (ssv) m->skinvtx = base + ssv->offset;

    return m;

fail:
    r3d_model_free(m);
    return NULL;
}

r3d_model_t *r3d_model_load(r3d_backend_t *backend, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return NULL; }

    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);

    r3d_model_t *m = r3d_model_load_mem(backend, buf, (uint32_t)sz);
    if (!m) free(buf);
    return m;
}

void r3d_model_free(r3d_model_t *m)
{
    if (!m) return;
    if (m->backend && m->backend->vt->destroy_texture && m->textures)
        for (uint32_t i = 0; i < m->texture_count; i++)
            if (m->textures[i]) m->backend->vt->destroy_texture(m->backend, m->textures[i]);
    free(m->textures);
    free(m->submeshes);
    free(m->vertices);    /* indices 指向 raw，不单独 free */
    free(m->raw);
    free(m);
}
