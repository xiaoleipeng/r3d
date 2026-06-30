/* g2b_draco.cpp — Draco 网格解码桥接实现 */
#include "g2b_draco.h"

#ifdef G2B_HAVE_DRACO

#include <cstdlib>
#include <cstring>
#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"

/* 把 draco 某 attribute（按 unique_id）解成连续 float[vcount*ncomp] */
static float* g2b_draco_read_attr(const draco::Mesh &mesh, int unique_id,
                                  int ncomp, uint32_t vcount) {
    if (unique_id < 0) return nullptr;
    const draco::PointAttribute *attr =
        mesh.GetAttributeByUniqueId((uint32_t)unique_id);
    if (!attr) return nullptr;

    float *out = (float*)malloc((size_t)vcount * ncomp * sizeof(float));
    if (!out) return nullptr;

    /* draco 按 PointIndex 取值；mesh 顶点序与 PointIndex 一致（0..vcount-1）。
       用 mapped value 接口逐点读 float。 */
    float tmp[4];
    for (uint32_t i = 0; i < vcount; i++) {
        draco::PointIndex pi(i);
        draco::AttributeValueIndex avi = attr->mapped_index(pi);
        if (!attr->ConvertValue<float>(avi, ncomp, tmp)) {
            std::memset(tmp, 0, sizeof(tmp));
        }
        for (int k = 0; k < ncomp; k++) out[i * ncomp + k] = tmp[k];
    }
    return out;
}

extern "C" int g2b_draco_decode(const uint8_t *data, size_t size,
                                int pos_id, int nrm_id, int uv_id,
                                g2b_draco_mesh_t *out) {
    std::memset(out, 0, sizeof(*out));

    draco::DecoderBuffer buf;
    buf.Init(reinterpret_cast<const char*>(data), size);

    auto type_st = draco::Decoder::GetEncodedGeometryType(&buf);
    if (!type_st.ok()) return -1;
    if (type_st.value() != draco::TRIANGULAR_MESH) return -2;

    draco::Decoder decoder;
    auto mesh_st = decoder.DecodeMeshFromBuffer(&buf);
    if (!mesh_st.ok()) return -3;
    std::unique_ptr<draco::Mesh> mesh = std::move(mesh_st).value();

    uint32_t vcount = (uint32_t)mesh->num_points();
    uint32_t fcount = (uint32_t)mesh->num_faces();
    uint32_t icount = fcount * 3;

    uint32_t *idx = (uint32_t*)malloc((size_t)icount * sizeof(uint32_t));
    if (!idx) return -4;
    for (uint32_t f = 0; f < fcount; f++) {
        const draco::Mesh::Face &face = mesh->face(draco::FaceIndex(f));
        idx[f * 3 + 0] = face[0].value();
        idx[f * 3 + 1] = face[1].value();
        idx[f * 3 + 2] = face[2].value();
    }

    out->vertex_count = vcount;
    out->index_count  = icount;
    out->indices      = idx;
    out->position     = g2b_draco_read_attr(*mesh, pos_id, 3, vcount);
    out->normal       = g2b_draco_read_attr(*mesh, nrm_id, 3, vcount);
    out->texcoord     = g2b_draco_read_attr(*mesh, uv_id,  2, vcount);
    return 0;
}

extern "C" void g2b_draco_free(g2b_draco_mesh_t *m) {
    if (!m) return;
    free(m->indices);
    free(m->position);
    free(m->normal);
    free(m->texcoord);
    std::memset(m, 0, sizeof(*m));
}

extern "C" int g2b_draco_available(void) { return 1; }

#else  /* 未编入 draco */

#include <string.h>
extern "C" int g2b_draco_decode(const uint8_t *data, size_t size,
                                int pos_id, int nrm_id, int uv_id,
                                g2b_draco_mesh_t *out) {
    (void)data; (void)size; (void)pos_id; (void)nrm_id; (void)uv_id;
    if (out) memset(out, 0, sizeof(*out));
    return -100;
}
extern "C" void g2b_draco_free(g2b_draco_mesh_t *m) { (void)m; }
extern "C" int g2b_draco_available(void) { return 0; }

#endif
