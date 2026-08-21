/*
 * test_quat_orbit.c — 验证四元数轨道相机替代欧拉角后确实消除了万向锁
 *
 * 原实现用球坐标 + 固定世界 up：
 *     eye = center + dist*(cos(p)*sin(y), sin(p), cos(p)*cos(y))
 *     look_at(eye, center, (0,1,0))
 * 在 p → ±90° 处有两重退化：
 *   (1) cos(p) → 0，eye 的水平分量趋 0，yaw 完全失效(万向锁)
 *   (2) 视线与 up 平行，look_at 的 cross(dir, up) 退化
 * 原代码靠把 pitch 夹到 ±1.5 rad 躲开，代价是接近极点时横向拖拽几乎不动。
 *
 * 新实现：姿态存为四元数 q，eye = center + q*(0,0,dist)，up = q*(0,1,0)。
 * 拖拽按"后乘局部轴"施加增量 → 不存在被偏爱的世界轴，因此无锁、无需夹角。
 */

#include "r3d/r3d_math.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0;

static void ck(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static float v3len(r3d_vec3_t v)
{
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
}

static r3d_vec3_t v3sub(r3d_vec3_t a, r3d_vec3_t b)
{
    r3d_vec3_t r = { a.x-b.x, a.y-b.y, a.z-b.z };
    return r;
}

/* ---- 旧实现(球坐标)，用于对照 ---- */
static r3d_vec3_t old_eye(float yaw, float pitch, float dist)
{
    r3d_vec3_t e = { dist * cosf(pitch) * sinf(yaw),
                     dist * sinf(pitch),
                     dist * cosf(pitch) * cosf(yaw) };
    return e;
}

/* ---- 新实现 ---- */
static r3d_vec3_t new_eye(r3d_quat_t q, float dist)
{
    r3d_vec3_t base = { 0.0f, 0.0f, dist };
    return r3d_quat_rotate_vec3(q, base);
}

static r3d_vec3_t new_up(r3d_quat_t q)
{
    r3d_vec3_t base = { 0.0f, 1.0f, 0.0f };
    return r3d_quat_rotate_vec3(q, base);
}

/* 与引擎 set_orbit 相同的构造：q = Ry(yaw) * Rx(-pitch) */
static r3d_quat_t quat_from_yaw_pitch(float yaw, float pitch)
{
    r3d_vec3_t Y = { 0.0f, 1.0f, 0.0f };
    r3d_vec3_t X = { 1.0f, 0.0f, 0.0f };
    return r3d_quat_mul(r3d_quat_from_axis_angle(Y, yaw),
                        r3d_quat_from_axis_angle(X, -pitch));
}

int main(void)
{
    const float D = 3.7f;

    printf("== 1. 基本代数 ==\n");
    {
        r3d_quat_t i = r3d_quat_identity();
        r3d_vec3_t v = { 1.0f, 2.0f, 3.0f };
        r3d_vec3_t r = r3d_quat_rotate_vec3(i, v);
        ck(fabsf(r.x-1)<1e-5f && fabsf(r.y-2)<1e-5f && fabsf(r.z-3)<1e-5f,
           "单位四元数不改变向量");

        r3d_vec3_t Y = { 0, 1, 0 };
        r3d_quat_t q90 = r3d_quat_from_axis_angle(Y, (float)M_PI/2);
        r3d_vec3_t z = { 0, 0, 1 };
        r3d_vec3_t rz = r3d_quat_rotate_vec3(q90, z);
        /* 右手系绕 +Y 转 90°：+Z → +X */
        ck(fabsf(rz.x-1)<1e-5f && fabsf(rz.y)<1e-5f && fabsf(rz.z)<1e-5f,
           "绕 Y 转 90 度把 +Z 变成 +X");

        r3d_quat_t q45 = r3d_quat_from_axis_angle(Y, (float)M_PI/4);
        r3d_quat_t qq  = r3d_quat_mul(q45, q45);
        r3d_vec3_t rz2 = r3d_quat_rotate_vec3(qq, z);
        ck(fabsf(rz2.x-1)<1e-4f, "两次 45 度复合等于一次 90 度");

        ck(fabsf(v3len(r3d_quat_rotate_vec3(q90, v)) - v3len(v)) < 1e-4f,
           "旋转保持向量长度");
    }

    printf("\n== 2. 与旧球坐标实现在非退化区一致 ==\n");
    {
        float worst = 0.0f;
        for (int iy = 0; iy < 12; iy++) {
            for (int ip = -4; ip <= 4; ip++) {
                float yaw = (float)iy * 0.5f;
                float pitch = (float)ip * 0.3f;     /* |pitch| <= 1.2 rad */
                r3d_vec3_t eo = old_eye(yaw, pitch, D);
                r3d_vec3_t en = new_eye(quat_from_yaw_pitch(yaw, pitch), D);
                float d = v3len(v3sub(eo, en));
                if (d > worst) worst = d;
            }
        }
        printf("     最大 eye 偏差 = %.6f\n", worst);
        ck(worst < 1e-3f, "四元数构造复现旧视角(默认视图不变)");
    }

    printf("\n== 3. 万向锁：极点附近横向拖拽是否仍然有效 ==\n");
    {
        /* 把相机抬到接近极点，再施加一次横向拖拽，比较 eye 的移动量。
         * 旧实现：水平半径 = dist*cos(pitch)，pitch→90° 时趋 0 → 拖拽无效。
         * 新实现：绕局部 up 轴旋转，任何姿态下位移都是 dist*|sin(dθ)|。 */
        const float dth = 0.10f;            /* 一次横向拖拽的角度 */
        printf("     %-14s %-16s %-16s\n", "pitch(度)", "旧实现位移", "新实现位移");
        float old_at_pole = -1.0f, new_at_pole = -1.0f;
        for (int k = 0; k < 5; k++) {
            float pitch = (float)M_PI/2 * (0.5f + 0.125f*(float)k); /* 45..90 度 */
            /* 旧：yaw 增量 */
            r3d_vec3_t o1 = old_eye(0.0f, pitch, D);
            r3d_vec3_t o2 = old_eye(dth,  pitch, D);
            float dold = v3len(v3sub(o1, o2));
            /* 新：绕当前局部 up 后乘 */
            r3d_quat_t q = quat_from_yaw_pitch(0.0f, pitch);
            r3d_vec3_t n1 = new_eye(q, D);
            r3d_vec3_t Y = { 0, 1, 0 };
            r3d_quat_t q2 = r3d_quat_mul(q, r3d_quat_from_axis_angle(Y, dth));
            r3d_vec3_t n2 = new_eye(q2, D);
            float dnew = v3len(v3sub(n1, n2));
            printf("     %-14.1f %-16.4f %-16.4f\n",
                   pitch * 180.0f / (float)M_PI, dold, dnew);
            if (k == 4) { old_at_pole = dold; new_at_pole = dnew; }
        }
        float expect = D * 2.0f * sinf(dth * 0.5f);   /* 弦长 */
        ck(old_at_pole < 1e-4f, "旧实现在极点处横向拖拽位移退化到 0(万向锁)");
        ck(fabsf(new_at_pole - expect) < 1e-3f,
           "新实现在极点处位移仍为满量(无万向锁)");
    }

    printf("\n== 4. 连续越过极点不产生退化 ==\n");
    {
        /* 沿局部 X 轴连续转 720 度，检查 eye/up 始终有限、模长恒定、
         * 且 up 与视线不会平行(look_at 的 cross 不退化)。 */
        r3d_quat_t q = r3d_quat_identity();
        r3d_vec3_t X = { 1, 0, 0 };
        r3d_quat_t step = r3d_quat_from_axis_angle(X, 0.05f);
        int bad = 0, near_parallel = 0;
        float min_sin = 1.0f;
        for (int i = 0; i < 720/3; i++) {          /* 0.05 rad * 240 ≈ 687 度 */
            q = r3d_quat_normalize(r3d_quat_mul(q, step));
            r3d_vec3_t e = new_eye(q, D);
            r3d_vec3_t u = new_up(q);
            if (!isfinite(e.x) || !isfinite(e.y) || !isfinite(e.z) ||
                !isfinite(u.x) || !isfinite(u.y) || !isfinite(u.z)) bad++;
            if (fabsf(v3len(e) - D) > 1e-3f) bad++;
            /* dir = center - eye = -e(归一化后)；|dir × up| = sin(夹角) */
            r3d_vec3_t d = { -e.x/D, -e.y/D, -e.z/D };
            r3d_vec3_t c = { d.y*u.z - d.z*u.y, d.z*u.x - d.x*u.z, d.x*u.y - d.y*u.x };
            float s = v3len(c);
            if (s < min_sin) min_sin = s;
            if (s < 1e-3f) near_parallel++;
        }
        printf("     异常样本=%d  视线与 up 夹角 sin 最小值=%.4f  近平行样本=%d\n",
               bad, min_sin, near_parallel);
        ck(bad == 0, "越过极点 240 步无 NaN、eye 模长恒定");
        ck(near_parallel == 0, "视线与 up 始终不平行(look_at 不退化)");
    }

    printf("\n== 5. 长期累积后仍是单位四元数(无漂移) ==\n");
    {
        r3d_quat_t q = r3d_quat_identity();
        r3d_vec3_t A = { 0.3f, 1.0f, -0.2f };
        r3d_quat_t s = r3d_quat_from_axis_angle(A, 0.017f);
        for (int i = 0; i < 20000; i++)
            q = r3d_quat_normalize(r3d_quat_mul(q, s));
        float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        printf("     20000 步后模长 = %.7f\n", n);
        ck(fabsf(n - 1.0f) < 1e-4f, "归一化抑制了累积漂移");
    }

    printf("\n%s (失败 %d 项)\n", g_fail ? "存在失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
