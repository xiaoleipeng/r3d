/*
 * r3d_math.h — 4x4 矩阵/向量/四元数（列主序）
 * 对应架构文档 §9.1。仅实现渲染所需子集。
 */
#ifndef R3D_MATH_H
#define R3D_MATH_H

#include "r3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 矩阵 */
void       r3d_mat4_identity(r3d_mat4_t *out);
void       r3d_mat4_mul(r3d_mat4_t *out, const r3d_mat4_t *a, const r3d_mat4_t *b); /* out = a*b */
r3d_vec4_t r3d_mat4_mul_vec4(const r3d_mat4_t *m, r3d_vec4_t v);
void       r3d_mat4_look_at(r3d_mat4_t *out, r3d_vec3_t eye, r3d_vec3_t center, r3d_vec3_t up);
void       r3d_mat4_perspective(r3d_mat4_t *out, float fovy_rad, float aspect, float znear, float zfar);
void       r3d_mat4_from_trs(r3d_mat4_t *out, r3d_vec3_t t, r3d_quat_t r, r3d_vec3_t s);

/* 向量 */
r3d_vec3_t r3d_vec3_sub(r3d_vec3_t a, r3d_vec3_t b);
r3d_vec3_t r3d_vec3_cross(r3d_vec3_t a, r3d_vec3_t b);
float      r3d_vec3_dot(r3d_vec3_t a, r3d_vec3_t b);
r3d_vec3_t r3d_vec3_normalize(r3d_vec3_t v);

/* 四元数 */
r3d_quat_t r3d_quat_identity(void);
r3d_quat_t r3d_quat_from_axis_angle(r3d_vec3_t axis, float angle);
/* r = a*b：先施加 b 再施加 a。相机增量旋转时后乘 = 绕自身局部轴，前乘 = 绕世界轴。 */
r3d_quat_t r3d_quat_mul(r3d_quat_t a, r3d_quat_t b);
r3d_quat_t r3d_quat_normalize(r3d_quat_t q);
r3d_vec3_t r3d_quat_rotate_vec3(r3d_quat_t q, r3d_vec3_t v);
r3d_quat_t r3d_quat_slerp(r3d_quat_t a, r3d_quat_t b, float t);
void       r3d_quat_to_mat4(r3d_mat4_t *out, r3d_quat_t q);

#ifdef __cplusplus
}
#endif
#endif /* R3D_MATH_H */
