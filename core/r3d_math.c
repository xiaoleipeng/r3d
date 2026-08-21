/*
 * r3d_math.c — 列主序 4x4 矩阵 / 向量 / 四元数实现
 * 列主序：m[col*4 + row]，与 glTF/OpenGL 一致。
 */
#include "r3d/r3d_math.h"
#include <math.h>

#define M(mat, c, r) ((mat)->m[(c)*4 + (r)])

void r3d_mat4_identity(r3d_mat4_t *o)
{
    for (int i = 0; i < 16; i++) o->m[i] = 0.0f;
    o->m[0] = o->m[5] = o->m[10] = o->m[15] = 1.0f;
}

/* out = a * b（列主序，先应用 b 再应用 a）*/
void r3d_mat4_mul(r3d_mat4_t *out, const r3d_mat4_t *a, const r3d_mat4_t *b)
{
    r3d_mat4_t r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a->m[k*4 + row] * b->m[c*4 + k];
            r.m[c*4 + row] = s;
        }
    }
    *out = r;
}

r3d_vec4_t r3d_mat4_mul_vec4(const r3d_mat4_t *m, r3d_vec4_t v)
{
    r3d_vec4_t o;
    o.x = m->m[0]*v.x + m->m[4]*v.y + m->m[8]*v.z  + m->m[12]*v.w;
    o.y = m->m[1]*v.x + m->m[5]*v.y + m->m[9]*v.z  + m->m[13]*v.w;
    o.z = m->m[2]*v.x + m->m[6]*v.y + m->m[10]*v.z + m->m[14]*v.w;
    o.w = m->m[3]*v.x + m->m[7]*v.y + m->m[11]*v.z + m->m[15]*v.w;
    return o;
}

r3d_vec3_t r3d_vec3_sub(r3d_vec3_t a, r3d_vec3_t b)
{ r3d_vec3_t r = { a.x-b.x, a.y-b.y, a.z-b.z }; return r; }

r3d_vec3_t r3d_vec3_cross(r3d_vec3_t a, r3d_vec3_t b)
{
    r3d_vec3_t r = { a.y*b.z - a.z*b.y,
                     a.z*b.x - a.x*b.z,
                     a.x*b.y - a.y*b.x };
    return r;
}

float r3d_vec3_dot(r3d_vec3_t a, r3d_vec3_t b)
{ return a.x*b.x + a.y*b.y + a.z*b.z; }

r3d_vec3_t r3d_vec3_normalize(r3d_vec3_t v)
{
    float len = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len > 1e-8f) { float inv = 1.0f/len; v.x*=inv; v.y*=inv; v.z*=inv; }
    return v;
}

/* 右手系 look_at，列主序 */
void r3d_mat4_look_at(r3d_mat4_t *out, r3d_vec3_t eye, r3d_vec3_t center, r3d_vec3_t up)
{
    r3d_vec3_t f = r3d_vec3_normalize(r3d_vec3_sub(center, eye));
    r3d_vec3_t s = r3d_vec3_normalize(r3d_vec3_cross(f, up));
    r3d_vec3_t u = r3d_vec3_cross(s, f);

    r3d_mat4_identity(out);
    M(out,0,0)= s.x; M(out,1,0)= s.y; M(out,2,0)= s.z;
    M(out,0,1)= u.x; M(out,1,1)= u.y; M(out,2,1)= u.z;
    M(out,0,2)=-f.x; M(out,1,2)=-f.y; M(out,2,2)=-f.z;
    M(out,3,0)= -r3d_vec3_dot(s, eye);
    M(out,3,1)= -r3d_vec3_dot(u, eye);
    M(out,3,2)=  r3d_vec3_dot(f, eye);
}

/* 透视投影，右手系，深度 [-1,1]（GL 风格），列主序 */
void r3d_mat4_perspective(r3d_mat4_t *out, float fovy, float aspect, float zn, float zf)
{
    float t = tanf(fovy * 0.5f);
    for (int i = 0; i < 16; i++) out->m[i] = 0.0f;
    out->m[0]  = 1.0f / (aspect * t);
    out->m[5]  = 1.0f / t;
    out->m[10] = -(zf + zn) / (zf - zn);
    out->m[11] = -1.0f;
    out->m[14] = -(2.0f * zf * zn) / (zf - zn);
}

void r3d_quat_to_mat4(r3d_mat4_t *out, r3d_quat_t q)
{
    float x=q.x, y=q.y, z=q.z, w=q.w;
    float xx=x*x, yy=y*y, zz=z*z;
    float xy=x*y, xz=x*z, yz=y*z;
    float wx=w*x, wy=w*y, wz=w*z;
    r3d_mat4_identity(out);
    M(out,0,0)=1-2*(yy+zz); M(out,1,0)=2*(xy-wz);   M(out,2,0)=2*(xz+wy);
    M(out,0,1)=2*(xy+wz);   M(out,1,1)=1-2*(xx+zz); M(out,2,1)=2*(yz-wx);
    M(out,0,2)=2*(xz-wy);   M(out,1,2)=2*(yz+wx);   M(out,2,2)=1-2*(xx+yy);
}

void r3d_mat4_from_trs(r3d_mat4_t *out, r3d_vec3_t t, r3d_quat_t r, r3d_vec3_t s)
{
    r3d_mat4_t rot;
    r3d_quat_to_mat4(&rot, r);
    /* 缩放（列缩放）*/
    M(&rot,0,0)*=s.x; M(&rot,0,1)*=s.x; M(&rot,0,2)*=s.x;
    M(&rot,1,0)*=s.y; M(&rot,1,1)*=s.y; M(&rot,1,2)*=s.y;
    M(&rot,2,0)*=s.z; M(&rot,2,1)*=s.z; M(&rot,2,2)*=s.z;
    /* 平移 */
    M(&rot,3,0)=t.x; M(&rot,3,1)=t.y; M(&rot,3,2)=t.z;
    *out = rot;
}

r3d_quat_t r3d_quat_identity(void)
{
    r3d_quat_t q = { 0.0f, 0.0f, 0.0f, 1.0f };
    return q;
}

/* 轴角 → 四元数。axis 不需预先归一化。 */
r3d_quat_t r3d_quat_from_axis_angle(r3d_vec3_t axis, float angle)
{
    float len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (len < 1e-8f) return r3d_quat_identity();
    float s = sinf(angle * 0.5f) / len;
    r3d_quat_t q = { axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f) };
    return q;
}

/* 哈密顿积。r = a*b 表示"先施加 b 再施加 a"(与矩阵乘法同序)。
 * 相机做增量旋转时：后乘 = 绕自身局部轴转，前乘 = 绕世界轴转。 */
r3d_quat_t r3d_quat_mul(r3d_quat_t a, r3d_quat_t b)
{
    r3d_quat_t r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return r;
}

/* 归一化。增量旋转会长期累积，必须周期性归一化以免漂移。 */
r3d_quat_t r3d_quat_normalize(r3d_quat_t q)
{
    float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (n < 1e-8f) return r3d_quat_identity();
    float inv = 1.0f / n;
    r3d_quat_t r = { q.x*inv, q.y*inv, q.z*inv, q.w*inv };
    return r;
}

/* v' = q * v * q^-1，展开为不含三角函数的形式 */
r3d_vec3_t r3d_quat_rotate_vec3(r3d_quat_t q, r3d_vec3_t v)
{
    /* t = 2 * (q_vec × v) ; v' = v + q_w * t + q_vec × t */
    float tx = 2.0f * (q.y*v.z - q.z*v.y);
    float ty = 2.0f * (q.z*v.x - q.x*v.z);
    float tz = 2.0f * (q.x*v.y - q.y*v.x);
    r3d_vec3_t r;
    r.x = v.x + q.w*tx + (q.y*tz - q.z*ty);
    r.y = v.y + q.w*ty + (q.z*tx - q.x*tz);
    r.z = v.z + q.w*tz + (q.x*ty - q.y*tx);
    return r;
}

r3d_quat_t r3d_quat_slerp(r3d_quat_t a, r3d_quat_t b, float t)
{
    float cosom = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (cosom < 0.0f) { cosom = -cosom; b.x=-b.x; b.y=-b.y; b.z=-b.z; b.w=-b.w; }
    float sa, sb;
    if (1.0f - cosom > 1e-6f) {
        float om = acosf(cosom), sinom = sinf(om);
        sa = sinf((1.0f - t) * om) / sinom;
        sb = sinf(t * om) / sinom;
    } else { sa = 1.0f - t; sb = t; }  /* 近似线性 */
    r3d_quat_t r = { sa*a.x + sb*b.x, sa*a.y + sb*b.y,
                     sa*a.z + sb*b.z, sa*a.w + sb*b.w };
    return r;
}
