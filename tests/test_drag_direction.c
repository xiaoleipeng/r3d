/*
 * test_drag_direction.c — 验证拖拽方向：模型表面必须跟随手指("抓取式"手感)
 *
 * 判据不靠推理符号，而是端到端投影：取球面上正对相机的那个点，
 * 用与后端一致的 view/proj/viewport 把它投到屏幕，比较拖拽前后的屏幕位移
 * 是否与手指位移同向。
 *
 * 屏幕坐标与后端一致：sx = (ndc_x*0.5+0.5)*W，sy = (1-(ndc_y*0.5+0.5))*H
 * 即 y 向下。手指右移 dx>0、下移 dy>0。
 */

#include "r3d/r3d_math.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;
static void ck(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

#define W 480
#define H 480
#define DIST 3.33f
#define SPHERE_R 1.0f

/* demo 的拖拽系数与符号(见 touch_poll)：
 *   d_yaw   = -dx * K_YAW      横向取负,使表面跟随手指
 *   d_pitch = +dy * K_PITCH
 * 这两个符号是既有约定，本测试验证引擎侧如何解释它们才不会反向。 */
#define K_YAW    0.010f
#define K_PITCH  0.010f

static r3d_quat_t quat_from_yaw_pitch(float yaw, float pitch)
{
    r3d_vec3_t Y = { 0, 1, 0 }, X = { 1, 0, 0 };
    return r3d_quat_mul(r3d_quat_from_axis_angle(Y, yaw),
                        r3d_quat_from_axis_angle(X, -pitch));
}

/* pitch_sign: 引擎 orbit_delta 里 Rx 的符号。+1 = 当前实现，-1 = 与 set_orbit 一致 */
static r3d_quat_t apply_drag(r3d_quat_t q, float dx, float dy, float pitch_sign)
{
    r3d_vec3_t Y = { 0, 1, 0 }, X = { 1, 0, 0 };
    float d_yaw   = -dx * K_YAW;
    float d_pitch =  dy * K_PITCH;
    q = r3d_quat_mul(q, r3d_quat_from_axis_angle(Y, d_yaw));
    q = r3d_quat_mul(q, r3d_quat_from_axis_angle(X, pitch_sign * d_pitch));
    return r3d_quat_normalize(q);
}

/* 把世界点投到屏幕。返回 0 表示在相机后方(不可用)。 */
static int project(r3d_quat_t q, r3d_vec3_t p, float *sx, float *sy)
{
    r3d_vec3_t center = { 0, 0, 0 };
    r3d_vec3_t off = r3d_quat_rotate_vec3(q, (r3d_vec3_t){ 0, 0, DIST });
    r3d_vec3_t eye = { center.x + off.x, center.y + off.y, center.z + off.z };
    r3d_vec3_t up  = r3d_quat_rotate_vec3(q, (r3d_vec3_t){ 0, 1, 0 });

    r3d_mat4_t view, proj, vp;
    r3d_mat4_look_at(&view, eye, center, up);
    r3d_mat4_perspective(&proj, 1.0f, (float)W / (float)H, 0.05f, 100.0f);
    r3d_mat4_mul(&vp, &proj, &view);

    /* 列主序：clip = VP * (p,1) */
    float cx = vp.m[0]*p.x + vp.m[4]*p.y + vp.m[8]*p.z  + vp.m[12];
    float cy = vp.m[1]*p.x + vp.m[5]*p.y + vp.m[9]*p.z  + vp.m[13];
    float cw = vp.m[3]*p.x + vp.m[7]*p.y + vp.m[11]*p.z + vp.m[15];
    if (cw <= 1e-6f) return 0;
    float nx = cx / cw, ny = cy / cw;
    *sx = (nx * 0.5f + 0.5f) * (float)W;
    *sy = (1.0f - (ny * 0.5f + 0.5f)) * (float)H;
    return 1;
}

/* 取当前正对相机的球面点 */
static r3d_vec3_t front_point(r3d_quat_t q)
{
    r3d_vec3_t off = r3d_quat_rotate_vec3(q, (r3d_vec3_t){ 0, 0, DIST });
    float len = sqrtf(off.x*off.x + off.y*off.y + off.z*off.z);
    r3d_vec3_t p = { off.x/len*SPHERE_R, off.y/len*SPHERE_R, off.z/len*SPHERE_R };
    return p;
}

/* 施加一次拖拽，返回"正对点"在屏幕上的位移 */
static int surface_shift(r3d_quat_t q0, float dx, float dy, float pitch_sign,
                         float *out_dx, float *out_dy)
{
    r3d_vec3_t p = front_point(q0);          /* 手指按住的那个表面点 */
    float ax, ay, bx, by;
    if (!project(q0, p, &ax, &ay)) return 0;
    r3d_quat_t q1 = apply_drag(q0, dx, dy, pitch_sign);
    if (!project(q1, p, &bx, &by)) return 0;
    *out_dx = bx - ax;
    *out_dy = by - ay;
    return 1;
}

int main(void)
{
    /* 初始姿态与 make_instance 一致：yaw=0, pitch=0.4 */
    r3d_quat_t q0 = quat_from_yaw_pitch(0.0f, 0.4f);
    const float DRAG = 40.0f;      /* 手指移动 40 px */

    for (int variant = 0; variant < 2; variant++) {
        float sign = (variant == 0) ? +1.0f : -1.0f;
        printf("== orbit_delta 中 Rx 符号 = %+.0f %s ==\n", sign,
               variant == 0 ? "(当前实现)" : "(与 set_orbit 一致)");

        float mx, my;
        /* 横向：手指右移 → 表面点应右移 */
        surface_shift(q0, DRAG, 0.0f, sign, &mx, &my);
        printf("     手指右移 %.0f px → 表面位移 (%+.1f, %+.1f)\n", DRAG, mx, my);
        int h_ok = (mx > 1.0f);
        /* 纵向：手指下移 → 表面点应下移 */
        surface_shift(q0, 0.0f, DRAG, sign, &mx, &my);
        printf("     手指下移 %.0f px → 表面位移 (%+.1f, %+.1f)\n", DRAG, mx, my);
        int v_ok = (my > 1.0f);

        if (variant == 1) {
            ck(h_ok, "横向：表面跟随手指");
            ck(v_ok, "纵向：表面跟随手指");
        } else {
            printf("     → 横向 %s, 纵向 %s\n",
                   h_ok ? "同向" : "反向", v_ok ? "同向" : "反向");
        }
        printf("\n");
    }

    /* 多个姿态下都必须同向(含接近极点、越过极点后) */
    printf("== 各姿态下方向一致性(Rx 符号 = -1) ==\n");
    {
        int bad = 0;
        float pitches[] = { 0.0f, 0.4f, 1.2f, 1.55f, 2.4f, 3.0f };
        for (unsigned i = 0; i < sizeof(pitches)/sizeof(pitches[0]); i++) {
            r3d_quat_t q = quat_from_yaw_pitch(0.7f, pitches[i]);
            float hx, hy, vx, vy;
            surface_shift(q, DRAG, 0.0f, -1.0f, &hx, &hy);
            surface_shift(q, 0.0f, DRAG, -1.0f, &vx, &vy);
            int ok = (hx > 1.0f) && (vy > 1.0f);
            printf("     pitch=%5.2f rad: 横向(%+7.1f,%+6.1f) 纵向(%+6.1f,%+7.1f) %s\n",
                   pitches[i], hx, hy, vx, vy, ok ? "" : "  ← 反向");
            if (!ok) bad++;
        }
        ck(bad == 0, "所有姿态(含越过极点)方向都跟随手指");
    }

    printf("\n%s (失败 %d 项)\n", g_fail ? "存在失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
