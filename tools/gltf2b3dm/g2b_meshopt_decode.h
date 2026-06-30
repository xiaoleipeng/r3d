/* g2b_meshopt_decode.h — 解压 EXT_meshopt_compression 的 buffer_view（被 main.c include）
 *
 * cgltf 能解析 EXT_meshopt_compression 扩展字段，但不会解压压缩后的 buffer。
 * 若不解压，cgltf_accessor_read_* 读到的是压缩字节（垃圾），顶点会塌缩成极少数。
 * 这里在 cgltf_load_buffers 之后、提取之前，遍历所有压缩 buffer_view，
 * 用 meshoptimizer 的解码 API 解压到新内存，并写入 view->data（cgltf 读取时优先用它）。
 */
#ifndef G2B_MESHOPT_DECODE_H
#define G2B_MESHOPT_DECODE_H

#include "meshoptimizer.h"

/* 返回 0 成功；-1 失败。失败表示存在无法解码的压缩视图。 */
static int g2b_decode_meshopt(cgltf_data *d) {
    int decoded = 0, failed = 0;
    for (cgltf_size i = 0; i < d->buffer_views_count; i++) {
        cgltf_buffer_view *view = &d->buffer_views[i];
        if (!view->has_meshopt_compression) continue;

        cgltf_meshopt_compression *mc = &view->meshopt_compression;
        if (!mc->buffer || !mc->buffer->data) { failed++; continue; }

        const unsigned char *src = (const unsigned char*)mc->buffer->data + mc->offset;
        size_t out_bytes = (size_t)mc->count * mc->stride;
        unsigned char *dst = (unsigned char*)malloc(out_bytes ? out_bytes : 1);
        if (!dst) { failed++; continue; }

        int rc = -1;
        switch (mc->mode) {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(dst, mc->count, mc->stride, src, mc->size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(dst, mc->count, mc->stride, src, mc->size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(dst, mc->count, mc->stride, src, mc->size);
            break;
        default:
            rc = -1;
            break;
        }

        if (rc != 0) { free(dst); failed++; continue; }

        /* 解码后再按 filter 还原（就地） */
        switch (mc->filter) {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(dst, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(dst, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(dst, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_color:
            /* color filter 在本 meshopt 版本未提供解码器；标记失败而非崩溃 */
            fprintf(stderr, "meshopt: 不支持 color filter，跳过该 view\n");
            failed++;
            break;
        case cgltf_meshopt_compression_filter_none:
        default:
            break;
        }

        view->data = dst;   /* cgltf_buffer_view_data 优先返回此指针 */
        decoded++;
    }

    if (decoded)
        fprintf(stderr, "meshopt: 解压 %d 个压缩 buffer_view%s\n",
                decoded, failed ? "（部分失败）" : "");
    return failed ? -1 : 0;
}

#endif
