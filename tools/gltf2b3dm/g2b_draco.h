/* g2b_draco.h — Draco 网格解码桥接（C 接口，实现在 g2b_draco.cpp）
 *
 * cgltf 能解析 KHR_draco_mesh_compression 的元数据（压缩 buffer_view、draco 内
 * attribute id），但不解码。这里用 Google Draco 解码库把单个 prim 的压缩数据
 * 解成 float 顶点属性 + uint32 索引，供 g2b_extract 回填。
 *
 * 编译开关 G2B_HAVE_DRACO：未定义时为空桩，draco 模型会被跳过并告警。
 */
#ifndef G2B_DRACO_H
#define G2B_DRACO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 解码结果：调用方负责 free 各数组。属性指针为 NULL 表示该属性不存在。 */
typedef struct {
    uint32_t  vertex_count;
    uint32_t  index_count;
    uint32_t *indices;     /* [index_count]               */
    float    *position;    /* [vertex_count*3] 或 NULL    */
    float    *normal;      /* [vertex_count*3] 或 NULL    */
    float    *texcoord;    /* [vertex_count*2] 或 NULL    */
} g2b_draco_mesh_t;

/* 用 draco 内部 attribute unique_id 解码。
 * data/size: 压缩 buffer_view 字节。
 * pos_id/nrm_id/uv_id: draco attribute unique id，-1 表示无该属性。
 * 返回 0 成功（out 填充），非 0 失败。 */
int g2b_draco_decode(const uint8_t *data, size_t size,
                     int pos_id, int nrm_id, int uv_id,
                     g2b_draco_mesh_t *out);

void g2b_draco_free(g2b_draco_mesh_t *m);

/* 运行期查询是否编入了 draco 支持 */
int g2b_draco_available(void);

#ifdef __cplusplus
}
#endif

#endif
