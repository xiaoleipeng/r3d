/*
 * backend_vglite.c — VGLite 后端(best1600，无可编程 shader)
 *
 * 设计要点(对应评估文档附录 C + VGLite 源码分析)：
 *  - VGLite 是 2.5D 矢量/位图加速器，无顶点/片元 shader、无 z-buffer。
 *  - 3D 网格渲染策略：CPU 端做 MVP 投影 + 透视除法，得到屏幕空间三角形；
 *    每个三角形用 vg_lite_draw_pattern 把烘焙好的纹理按 2D 仿射映射贴上去。
 *  - 纹理坐标→屏幕的映射：由三角形 3 个顶点的 (screen_xy, uv) 解出 2x3 仿射矩阵，
 *    设为 pattern_matrix。纹理本身已离线烘焙好(PBR/AO/matcap/编织)，无需 shader。
 *  - 无 z-buffer：收集整帧三角形，end_frame 时按平均深度排序(画家算法)后绘制，
 *    半透明三角形最后画。
 *  - matcap：CPU 按 view 空间法线算 UV(nv.xy*0.5+0.5)，与普通纹理同样走仿射映射。
 *  - baseColorFactor：用 draw_pattern 的 color 参数整体乘色染色。
 *  - 顶点光照(可选)：CPU 逐顶点算亮度，best1600 支持 LINEAR_GRADIENT，
 *    可用线性渐变近似三角形内亮度过渡；当前先用顶点平均亮度做整体调制(flat)。
 *
 * VGLite 关键约束(best1600_vg_lite_options.h 实测)：
 *  - 无 IM_REPEAT_REFLECT → 纹理不能 GPU 端 REPEAT；我们烘焙时已把平铺烘进纹理 +
 *    UV 归一化到[0,1]，故只需 PATTERN_PAD，契合此限制。
 *  - 无 COLOR_TRANSFORMATION → 不能逐像素颜色矩阵；染色用单一 color 参数(够用)。
 *  - 支持 3x3 矩阵透视项(transform 函数算 pt_w 除法)，但本后端在 CPU 投影后只用
 *    仿射映射贴图，更可控。
 */

#include "r3d/r3d_backend.h"
#include "r3d/r3d_math.h"
#include "vg_lite.h"

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* 日志：仅用于错误与每秒一次的性能统计。
 * 用 syslog；本机(softsim)无 syslog 时退化为 printf。 */
#if defined(__NuttX__) || defined(CONFIG_ARCH)
#include <syslog.h>
#define VGL_LOG(fmt, ...) syslog(LOG_INFO,  "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#define VGL_ERR(fmt, ...) syslog(LOG_ERR,   "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define VGL_LOG(fmt, ...) fprintf(stderr, "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#define VGL_ERR(fmt, ...) fprintf(stderr, "r3d_vgl: " fmt "\n", ##__VA_ARGS__)
#endif

/* 返回当前单调毫秒，用于每秒统计窗口计时 */
static long vgl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* 返回当前单调微秒，用于帧内各阶段细粒度计时(性能剖析)。
 * clock_gettime 单次开销 ~亚微秒，每帧仅 ~6 次，对 60fps 预算可忽略。 */
static long vgl_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000);
}

/* 单个耗时指标的窗口聚合(微秒)：求和(算平均) + 最小/最大。 */
typedef struct {
    uint64_t sum;
    uint32_t min, max;
} vgl_timestat_t;

static void vgl_ts_reset(vgl_timestat_t *s)
{
    s->sum = 0;
    s->min = 0xFFFFFFFFu;
    s->max = 0;
}

static void vgl_ts_add(vgl_timestat_t *s, long us)
{
    if (us < 0) us = 0;
    s->sum += (uint64_t)us;
    if ((uint32_t)us < s->min) s->min = (uint32_t)us;
    if ((uint32_t)us > s->max) s->max = (uint32_t)us;
}

/* 原始逐帧记录：单帧各阶段测量值(微秒)与三角形计数。定长、可平凡拷贝，
 * 存入环形缓冲，每 N 秒批量转储到串口供离线工具分析。字段顺序与转储
 * 行的字段顺序、离线工具解析顺序三者必须保持一致。 */
typedef struct {
    uint32_t frame;        /* 全局帧序号(单调递增) */
    uint32_t t_frame;      /* 整帧 CPU 墙钟(begin_frame→end_frame) */
    uint32_t wait;         /* 引擎侧：双缓冲 poll 等空闲缓冲(vsync 空闲，非工作) */
    uint32_t anim;         /* 引擎侧：r3d_anim_update 关键帧采样+混合 */
    uint32_t node;         /* 引擎侧：逐 submesh r3d_anim_node_matrix 累计 */
    uint32_t deform;       /* 引擎侧：CPU 变形(morph/skin) */
    uint32_t collect;      /* vgl_draw 收集累计(投影+剔除+光照) */
    uint32_t sort;         /* 画家算法排序 */
    uint32_t submit;       /* 命令缓冲构建(= sbuild + sdraw + sflush) */
    uint32_t sbuild;       /* submit 细分：CPU 建 path(顶点/bbox/仿射) */
    uint32_t sdraw;        /* submit 细分：vg_lite_draw/draw_pattern 调用 */
    uint32_t sflush;       /* submit 细分：周期 vg_lite_flush */
    uint32_t gpu;          /* vg_lite_finish 残余等待(非 GPU 利用率，见注释) */
    uint32_t pan;          /* 引擎侧：上一帧 FBIOPAN_DISPLAY 翻页 */
    uint32_t tris;         /* 提交绘制三角形数(kept) */
    uint32_t drawcalls;    /* draw call 次数 */
    uint32_t tex;          /* 纹理绑定次数(draw_pattern) */
} vgl_raw_rec_t;

/* ---------------- 内部数据 ---------------- */

#define VGL_MAX_TRIS_DEFAULT  20000

/* 投影安全阈值：
 *  - VGL_W_EPSILON：裁剪空间 w 小于此值视为在近平面前/相机后，整片丢弃，
 *    避免 1/w 爆炸产生超大屏幕坐标。
 *  - VGL_COORD_LIMIT：屏幕坐标绝对值上限。超大坐标会让 VGLite tessellation
 *    遍历范围爆炸/定点溢出，是已知的 GPU hang 来源；超限三角形直接丢弃。
 *    32768 远超任何真实视口，正常 on/部分 off-screen 三角形不受影响。 */
#define VGL_W_EPSILON   1e-4f
#define VGL_COORD_LIMIT 32768.0f

/* 三角形填充规则。
 * 逐三角形单独绘制，单个三角形 NON_ZERO 与 EVEN_ODD 结果一致；沿用 EVEN_ODD。 */
#define VGL_FILL_RULE  VG_LITE_FILL_EVEN_ODD

/* 周期 flush 阈值：每提交这么多次 draw 调用 flush 一次命令缓冲，
 * 防止超大模型(上万三角形)命令缓冲溢出；帧末再统一 finish 一次。
 * 不再每个 draw 都 flush(那是早期定位 hang 时的临时做法，开销巨大)。 */
#define VGL_FLUSH_BATCH  64

/* path 细分质量。VG_LITE_HIGH：保留三角形边缘抗锯齿，配合逐面 flat 光照
 * 呈现曲面的明暗层次(立体感来源)。 */
#define VGL_FILL_QUALITY  VG_LITE_HIGH

/* ---- 原始逐帧性能采集(Raw_Frame_Record + 环形缓冲 + 定期串口转储) ----
 * 架构：固件只做"低开销采集 + 每 N 秒批量转储原始逐帧数据到串口"，
 * 所有重统计(p50/p95/p99、1%/0.1% low、标准差、抖动、直方图)交由主机侧
 * 离线工具(tools/perf/r3d_perf_analyze.py)在长采集上以全保真度计算。
 * 默认启用、始终生效，参数为下列固定编译期常量，不经 Kconfig 配置。 */
#define VGL_PERF_DUMP_PERIOD_SEC  5           /* 转储周期 N(秒) */
#define VGL_PERF_RING_FRAMES      512         /* 环形缓冲帧数(>= N*60fps=300，留余量) */
#define VGL_PERF_BUDGET_US        16667       /* 60fps 单帧预算(微秒) */
/* 转储日志前缀：离线工具据此从混合串口流中 grep 原始行。
 *   头部行： r3d_perfraw: HDR ...
 *   逐帧行： r3d_perfraw: F <字段...> */
#define VGL_PERF_RAW_TAG          "r3d_perfraw"

/* submit 阶段细分(path 构建 / vg_lite_draw 调用 / flush)。submit 通常是本后端
 * 最大头，细分能区分“成本在我们建 path 还是 driver 的 draw 调用”。代价是每三角形
 * 多几次 clock_gettime(每帧 ~2×三角形数)，故用编译开关控制；置 0 可完全编出、
 * 回到单一 submit 计时。默认开启。 */
#ifndef R3D_PERF_SUBMIT_DETAIL
#define R3D_PERF_SUBMIT_DETAIL 1
#endif

/* submit 细分计时的取时/累加宏。关闭时不产生任何 clock_gettime 调用，
 * 三个细分值恒为 0(submit 总数仍准确)。 */
#if R3D_PERF_SUBMIT_DETAIL
#define VGL_SUBMIT_T0()         vgl_now_us()
#define VGL_SUBMIT_ADD(acc, t0) do { (acc) += vgl_now_us() - (t0); } while (0)
#else
#define VGL_SUBMIT_T0()         (0L)
#define VGL_SUBMIT_ADD(acc, t0) do { (void)(t0); } while (0)
#endif

/* 合批潜力分析：对若干感知量化档(每通道右移位数)，统计"若把画家序中相邻、
 * 量化后同色的 tex=0 三角形合并为一次 draw，则本帧实际 draw call 数"。
 * 只统计不改绘制，用于评估合批收益上限。shift 越大量化越粗、合批越狠。
 * 每通道 step = 1<<shift（shift=1→128级/通道，4→16级/通道）。 */
#define VGL_BATCH_NLEVELS  4
static const uint8_t VGL_BATCH_SHIFT[VGL_BATCH_NLEVELS] = { 1, 2, 3, 4 };

/* 一个待绘制的屏幕空间三角形(投影后收集，end_frame 排序绘制) */
typedef struct {
    float sx[3], sy[3];        /* 屏幕坐标 */
    float uv[3][2];            /* 对应纹理 UV(已归一化或 matcap UV) */
    float depth;               /* 平均视深度，用于画家算法排序 */
    vg_lite_buffer_t *tex;     /* 贴图(NULL=纯色) */
    vg_lite_color_t color;     /* 染色/纯色(ABGR8888，VGLite 内部序) */
    int translucent;           /* 半透明：最后绘制 */
    int blend;                 /* r3d_blend_t */
    uint32_t seq;              /* 收集顺序：深度相等时的稳定 tie-breaker(防止帧间排序抖动闪烁) */
} vgl_tri_t;

/* 纹理句柄实体：包一个 vg_lite_buffer */
typedef struct {
    vg_lite_buffer_t buf;
    int valid;
} vgl_tex_t;

/* 逐顶点变换缓存项(优化 #4)：同一顶点在一个 submesh 内被多个三角形共享，
 * 原实现每个面都重算投影/法线变换。改为每个 vgl_draw 内每顶点只算一次，
 * 面阶段直接查表。用 epoch 戳(vcache_gen)标记有效性，避免每次清空整表，
 * 也只计算被本 submesh 引用到的顶点。 */
typedef struct {
    float   sx, sy;        /* 屏幕坐标 */
    float   vz;            /* view 空间深度 */
    float   mvn[3];        /* MV 变换后的法线(光照用) */
    float   vn[3];         /* VIEW 变换后的法线(matcap 用；非 matcap 不填) */
    uint8_t behind;        /* 裁剪空间 w <= 阈值(近平面前/相机后) */
    uint8_t bad;           /* 屏幕坐标 NaN/Inf/超限 */
} vgl_vcache_t;

typedef struct {
    /* 帧目标 */
    vg_lite_buffer_t target;
    int target_valid;

    /* GPU 所有权标记(信息用)：GPU 统一由外部 gpu_init 初始化/拥有，
     * 本后端 init/destroy 始终不调用 vg_lite_init/vg_lite_close。
     * 保留此字段仅为兼容 create/create_hosted 两个工厂入口。 */
    int hosted;

    /* 相机 */
    r3d_mat4_t view, proj, view_proj;
    int vp_w, vp_h;

    /* 光照参数(运行时可调，init 时填默认值) */
    r3d_light_params_t light;

    /* 帧三角形队列 */
    vgl_tri_t *tris;
    uint32_t   tri_count, tri_cap;

    /* 逐顶点变换缓存(优化 #4)：按 mesh->vertex_count 分配，跨 submesh/帧复用。
     * vcache_gen 为 epoch 戳，vcache_stamp[v]==vcache_gen 表示该顶点本轮已算。 */
    vgl_vcache_t *vcache;
    uint32_t     *vcache_stamp;
    uint32_t      vcache_cap;    /* 已分配容量(顶点数) */
    uint32_t      vcache_gen;    /* 当前 epoch，每次 vgl_draw 递增 */

    /* 排序索引(优化 #3)：qsort 排序索引而非 80 字节的 vgl_tri_t，
     * 交换只动 4 字节，避免大结构体拷贝。 */
    uint32_t *sort_idx;
    uint32_t  sort_idx_cap;

    /* 帧内 vg_lite_path_t 数组：每三角形一个，存活到帧末 finish。
     * 对齐 rive_for_vglite 做法：不调用 vg_lite_upload_path，path 顶点数据
     * 由 vg_lite_draw 编入主命令缓冲提交，GPU 随命令流读取。
     * 关键：path 数据缓冲不能放栈上(draw 是异步提交，finish 前 GPU 才真正
     * 读取)，故用一块常驻缓冲 path_data，每三角形 11 个 float。 */
    vg_lite_path_t *vgpaths;
    uint32_t        vgpaths_cap;
    float          *path_data;      /* [vgpaths_cap * 11]，每三角形一段，帧内存活 */

    /* 纹理表(句柄=指针) */
    /* 直接用 vgl_tex_t* 作句柄，无需表 */

    /* 上层注入的渲染钩子与清屏色。引擎本身不提供文字/背景/后处理能力，
     * 由上层通过钩子自行绘制(见 demo 的表盘示例)。 */
    r3d_render_hook_t hook;
    void             *hook_user;
    uint32_t          clear_argb;

    /* ---- 每帧计数(end_frame 末尾汇总进秒窗口) ---- */
    uint32_t draw_calls;       /* 本帧 vg_lite_draw / draw_pattern 调用次数 */
    uint32_t tex_binds;        /* 本帧纹理绑定次数(draw_pattern) */
    uint32_t frame_no;         /* 帧计数 */

    /* ---- collect 阶段每帧剔除计数(begin_frame 清零，vgl_draw 累加) ----
     * 用于分析 morph 姿态变化下三角形的输入/剔除分布：
     *   input     = 送入 collect 的候选三角形总数(index_count/3 之和)
     *   c_oob     = 索引越界丢弃(损坏模型)
     *   c_behind  = 任一顶点在近平面前/相机后丢弃(w 阈值)
     *   c_bad     = 透视除法后 NaN/Inf 或坐标超限丢弃
     *   c_back    = 背面剔除(有向面积，非双面)
     *   c_degen   = 退化三角形(面积近 0)
     *   c_capped  = 三角形队列已满被丢弃(tri_cap 不足)
     * kept(实际入队) = tri_count，已由 tri/f 反映。 */
    uint32_t cull_input;
    uint32_t cull_oob, cull_behind, cull_bad, cull_back, cull_degen, cull_capped;

    /* ---- 每秒性能统计窗口(行业标准 3D 指标) ---- */
    long     stat_window_ms;   /* 当前统计窗口起始时刻 */
    uint32_t stat_frames;      /* 窗口内帧数 */
    /* 三角面(提交绘制的三角形数) */
    uint64_t stat_tris_sum;
    uint32_t stat_tris_min, stat_tris_max;
    /* draw call 次数 */
    uint64_t stat_dc_sum;
    uint32_t stat_dc_min, stat_dc_max;
    /* 纹理绑定次数 */
    uint64_t stat_tex_sum;
    uint32_t stat_tex_min, stat_tex_max;

    /* ---- 帧内各阶段耗时(微秒，end_frame 汇总进秒窗口) ----
     * 用于定位 VGLite 后端瓶颈：CPU 收集/排序/命令构建 vs GPU 实际填充。
     * t_collect : begin_frame→end_frame 间 vgl_draw 累计(投影+光照+收集三角形)
     * t_sort    : 画家算法 qsort
     * t_submit  : 逐三角形构建 path + vg_lite_draw/draw_pattern 入命令缓冲(不含 GPU 等待)
     * t_gpu     : vg_lite_finish 阻塞等待 GPU 完成填充(真正的 GPU 墙钟)
     * t_frame   : begin_frame→end_frame 整帧 CPU 墙钟 */
    long          collect_us_accum;  /* 本帧 vgl_draw 累计(begin_frame 清零) */
    long          frame_begin_us;    /* begin_frame 时刻(算整帧墙钟) */
    vgl_timestat_t st_collect, st_sort, st_submit, st_gpu, st_frame;

    /* ---- collect 剔除计数的秒窗口累加(算每帧平均) ---- */
    uint64_t stat_cull_input_sum, stat_cull_oob_sum, stat_cull_behind_sum,
             stat_cull_bad_sum, stat_cull_back_sum, stat_cull_degen_sum,
             stat_cull_capped_sum;

    /* ---- Deadline-Miss 秒窗口统计(基于 t_frame vs 预算) ---- */
    uint32_t stat_miss_count;   /* 窗口内超预算帧数 */
    uint32_t stat_worst_over;   /* 窗口内最坏超出量(微秒) */

    /* ---- 合批潜力秒窗口统计：各量化档下"合批后 draw call"的累加(算每帧均值) ---- */
    uint64_t stat_batch_sum[VGL_BATCH_NLEVELS];

    /* ---- 引擎经 perf_frame_mark 传入的当帧各阶段耗时(end_frame 消费后清零) ---- */
    r3d_engine_perf_t eng_perf_pending;
    int               eng_perf_valid;   /* 本帧引擎是否已上报(未报则记 0) */

    /* ---- 新增阶段的秒窗口累加(简单求和算平均；稳健分位数交给离线工具) ---- */
    uint64_t stat_wait_sum, stat_anim_sum, stat_node_sum, stat_pan_sum;
    uint64_t stat_sbuild_sum, stat_sdraw_sum, stat_sflush_sum;

    /* ---- 原始逐帧采集：环形缓冲 + 定期串口转储 ---- */
    vgl_raw_rec_t *raw_ring;        /* 定长环形缓冲(init 一次性分配) */
    uint32_t       raw_head;        /* 下一写入位置 */
    uint32_t       raw_filled;      /* 已写入帧数(封顶 VGL_PERF_RING_FRAMES) */
    long           raw_dump_ms;     /* 上次转储时刻(毫秒窗口计时) */
    uint32_t       global_frame;    /* 全局帧序号 */
} vgl_impl_t;

/* ---------------- 工具 ---------------- */

/* vg_lite 错误码 → 可读名字，用于日志 */
static const char *vgl_err_str(vg_lite_error_t e)
{
    switch (e) {
        case VG_LITE_SUCCESS:             return "SUCCESS";
        case VG_LITE_INVALID_ARGUMENT:    return "INVALID_ARGUMENT";
        case VG_LITE_OUT_OF_MEMORY:       return "OUT_OF_MEMORY";
        case VG_LITE_NO_CONTEXT:          return "NO_CONTEXT";
        case VG_LITE_TIMEOUT:             return "TIMEOUT";
        case VG_LITE_OUT_OF_RESOURCES:    return "OUT_OF_RESOURCES";
        case VG_LITE_GENERIC_IO:          return "GENERIC_IO";
        case VG_LITE_NOT_SUPPORT:         return "NOT_SUPPORT";
        case VG_LITE_ALREADY_EXISTS:      return "ALREADY_EXISTS";
        case VG_LITE_NOT_ALIGNED:         return "NOT_ALIGNED";
        case VG_LITE_FLEXA_TIME_OUT:      return "FLEXA_TIME_OUT";
        case VG_LITE_FLEXA_HANDSHAKE_FAIL:return "FLEXA_HANDSHAKE_FAIL";
        default:                          return "UNKNOWN";
    }
}

/* r3d ARGB8888 → VGLite ABGR8888(VGLite color 内部为 0xAABBGGRR) */
static vg_lite_color_t argb_to_vgl(uint32_t argb)
{
    uint32_t a=(argb>>24)&0xFF, r=(argb>>16)&0xFF, g=(argb>>8)&0xFF, b=argb&0xFF;
    return (a<<24)|(b<<16)|(g<<8)|r;
}

/* 列主序 mat4 × vec4 */
static r3d_vec4_t mul_mv(const r3d_mat4_t *m, float x, float y, float z, float w)
{
    r3d_vec4_t o;
    o.x = m->m[0]*x + m->m[4]*y + m->m[8]*z  + m->m[12]*w;
    o.y = m->m[1]*x + m->m[5]*y + m->m[9]*z  + m->m[13]*w;
    o.z = m->m[2]*x + m->m[6]*y + m->m[10]*z + m->m[14]*w;
    o.w = m->m[3]*x + m->m[7]*y + m->m[11]*z + m->m[15]*w;
    return o;
}

/* r3d 像素格式 → VGLite buffer 格式 */
static vg_lite_buffer_format_t to_vgl_format(r3d_pixel_format_t f)
{
    switch (f) {
        case R3D_FMT_BGRA8888: return VG_LITE_BGRA8888;
        case R3D_FMT_BGR888:   return VG_LITE_BGR888;   /* 24bpp 打包 BGR(3 字节)，
                                                         * 与 LVGL RGB888→BGR888 一致 */
        case R3D_FMT_RGB565:   return VG_LITE_BGR565;
        case R3D_FMT_ARGB8888:
        default:               return VG_LITE_ARGB8888;
    }
}

/* ---------------- 生命周期 ---------------- */

static r3d_result_t vgl_init(r3d_backend_t *self, const r3d_backend_cfg_t *cfg)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    uint32_t cap = (cfg && cfg->max_triangles) ? cfg->max_triangles : VGL_MAX_TRIS_DEFAULT;

    /* GPU 初始化由外部(gpu_init)统一负责，本后端不自持 GPU。
     *
     * 原因(对照 rive_for_vglite 与 vendor/bes gpu_port.c)：
     *  - 真机上把 GPU 跑起来需要 gpu_hw_reset() + gpu_memory_setup() +
     *    vg_lite_init(480,480) + set_gpu_done_interrupt_handler() 一整套，
     *    单独调 vg_lite_init 既不够(缺中断 handler，finish 必超时)，
     *    tessellation buffer 尺寸也无从正确设定(目标尺寸要到 begin_frame 才知道)。
     *  - 因此真机路径只能由 gpu_init 拥有 GPU(r3d_engine.c 已用 static-bool 守卫调用)，
     *    backend 不再调用 vg_lite_init/vg_lite_close，避免重复初始化或埋下
     *    16x16 tess buffer 这类隐患。
     *  - host/softsim 测试构建中 vg_lite 由软件桩实现，同样无需本后端初始化。
     * 这与 rive 一致：rive 渲染器也从不自己 vg_lite_init，只调 gpu_init。 */

    im->tri_cap = cap;
    im->tris = (vgl_tri_t *)malloc(sizeof(vgl_tri_t) * cap);
    if (!im->tris) { return R3D_ERR_NO_MEM; }
    im->tri_count = 0;

    /* 排序索引(优化 #3)：与三角形队列同容量，一次性分配。 */
    im->sort_idx = (uint32_t *)malloc(sizeof(uint32_t) * cap);
    if (!im->sort_idx) { free(im->tris); im->tris = NULL; return R3D_ERR_NO_MEM; }
    im->sort_idx_cap = cap;

    /* 逐顶点变换缓存(优化 #4)：容量在首次 vgl_draw 按 mesh 顶点数惰性分配。 */
    im->vcache = NULL;
    im->vcache_stamp = NULL;
    im->vcache_cap = 0;
    im->vcache_gen = 0;

    r3d_light_params_default(&im->light);   /* 光照默认参数(中性外观) */
    im->clear_argb = 0xFF1F1F26u;          /* 与原行为一致；上层可改 */

    /* 每三角形一个 vg_lite_path_t + 一段 11 float 的 path 数据(存活到帧末 finish) */
    im->vgpaths_cap = cap;
    im->vgpaths = (vg_lite_path_t *)malloc(sizeof(vg_lite_path_t) * cap);
    if (!im->vgpaths) {
        free(im->tris); im->tris = NULL;
        return R3D_ERR_NO_MEM;
    }

    /* 常驻 path 数据缓冲：每三角形 11 个 float(MOVE/LINE/LINE/CLOSE/END)。
     * vg_lite_draw 异步提交，GPU 直到 finish 才读取，故 path 数据必须存活整帧、
     * 不能放栈上。对齐 rive：数据随主命令缓冲提交，无需 vg_lite_upload_path。 */
    im->path_data = (float *)malloc(sizeof(float) * 11 * cap);
    if (!im->path_data) {
        free(im->vgpaths); im->vgpaths = NULL;
        free(im->tris);    im->tris = NULL;
        return R3D_ERR_NO_MEM;
    }

    /* 性能统计窗口初始化 */
    im->stat_window_ms = vgl_now_ms();
    im->stat_tris_min = im->stat_dc_min = im->stat_tex_min = 0xFFFFFFFFu;
    vgl_ts_reset(&im->st_collect);
    vgl_ts_reset(&im->st_sort);
    vgl_ts_reset(&im->st_submit);
    vgl_ts_reset(&im->st_gpu);
    vgl_ts_reset(&im->st_frame);

    /* 原始逐帧采集环形缓冲：init 一次性分配，逐帧 O(1) 覆盖写入，
     * destroy 统一释放，逐帧路径不再分配。 */
    im->raw_ring = (vgl_raw_rec_t *)calloc(VGL_PERF_RING_FRAMES, sizeof(vgl_raw_rec_t));
    /* 分配失败不致命：仅禁用原始采集，其余统计照常。 */
    im->raw_head = im->raw_filled = 0;
    im->raw_dump_ms = vgl_now_ms();
    im->global_frame = 0;
    memset(&im->eng_perf_pending, 0, sizeof(im->eng_perf_pending));
    im->eng_perf_valid = 0;

    return R3D_OK;
}

static void vgl_destroy(r3d_backend_t *self)
{
    if (!self) return;
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (im) {
        free(im->tris);
        free(im->vgpaths);
        free(im->path_data);
        free(im->raw_ring);
        free(im->sort_idx);
        free(im->vcache);
        free(im->vcache_stamp);
        /* GPU 由 gpu_init 拥有，本后端不调用 vg_lite_close。 */
        free(im);
    }
    free(self);
}

/* ---------------- 纹理 ---------------- */

static r3d_texture_handle_t vgl_create_texture(r3d_backend_t *self, const r3d_image_t *img)
{
    (void)self;
    if (!img || !img->data || !img->w || !img->h) return R3D_TEXTURE_NONE;

    vgl_tex_t *t = (vgl_tex_t *)calloc(1, sizeof(vgl_tex_t));
    if (!t) return R3D_TEXTURE_NONE;

    /* 纹理内存用系统堆(memalign)分配，而非 vg_lite_allocate 的 GPU 私有堆。
     * 对照 rive_for_vglite 的 allocBufferMalloc / LVGL 的 lv_vg_lite_buffer_init：
     * 本 SoC 是统一寻址(flat-map)，GPU 可直接 DMA 访问系统内存，故无需占用
     * 容量有限、易 OOM 的 GPU 私有堆(256x256 纹理在私有堆常分配失败)。
     * 关键约束：
     *   - stride 必须 64 字节对齐(VGLite 硬件要求，同 LV_VG_LITE_BUF_ALIGN)
     *   - 起始地址 64 字节对齐(用 memalign)
     *   - address 直接用虚拟地址(统一寻址下 == GPU 可用地址)，handle=NULL */
    t->buf.width            = (vg_lite_int32_t)img->w;
    t->buf.height           = (vg_lite_int32_t)img->h;
    t->buf.format           = VG_LITE_BGRA8888;   /* 纹理像素在 b3dm 中按 BGRA 字节序存储(见 gltf2b3dm)，与 framebuffer 同序，避免 R/B 反 */
    t->buf.tiled            = VG_LITE_LINEAR;
    /* MULTIPLY：让 pattern 像素与 paint color 相乘，从而把 CPU 逐三角形算出的
     * 光照 tint(vgl_draw 里的 T->color)真正作用到纹理上。
     * 不能靠 vg_lite_draw_pattern 的 color 参数——那个参数仅在
     * pattern_mode == VG_LITE_PATTERN_COLOR 时用于填充 pattern 边界之外的像素
     * (见 vg_lite.h 的 vg_lite_pattern_mode 注释)，而这里用的是 PATTERN_PAD，
     * 故在 NORMAL_IMAGE_MODE 下 tint 会被硬件完全忽略，带纹理的模型看不出昼夜明暗。
     * (softsim 无条件调制，比硬件宽松，曾掩盖此问题。) */
    t->buf.image_mode       = VG_LITE_MULTIPLY_IMAGE_MODE;
    t->buf.transparency_mode= VG_LITE_IMAGE_OPAQUE;
    t->buf.stride           = ((vg_lite_int32_t)img->w * 4 + 63) & ~63;  /* 64 对齐 */
    t->buf.handle           = NULL;

    size_t size = (size_t)t->buf.stride * img->h;
    /* 多分配一行 stride 作为安全余量，防止 GPU 写越界(对照 rive) */
    void *mem = memalign(64, size + t->buf.stride);
    if (!mem) {
        VGL_ERR("create_texture: memalign(%dx%d, %zu bytes) failed",
                (int)img->w, (int)img->h, size + t->buf.stride);
        free(t);
        return R3D_TEXTURE_NONE;
    }
    t->buf.memory  = mem;
    t->buf.address = (vg_lite_uint32_t)(uintptr_t)mem;

    /* 拷贝像素(按行，处理 stride 差异) */
    uint32_t src_stride = img->stride ? img->stride : img->w * 4;
    uint8_t *dst = (uint8_t *)t->buf.memory;
    const uint8_t *src = (const uint8_t *)img->data;
    for (uint32_t y = 0; y < img->h; y++)
        memcpy(dst + (size_t)y * t->buf.stride, src + (size_t)y * src_stride, img->w * 4);

    t->valid = 1;
    return (r3d_texture_handle_t)t;
}

static void vgl_destroy_texture(r3d_backend_t *self, r3d_texture_handle_t h)
{
    (void)self;
    vgl_tex_t *t = (vgl_tex_t *)h;
    if (t && t->valid) {
        /* memalign 分配，用 free 释放(非 vg_lite_free) */
        if (t->buf.memory) free(t->buf.memory);
        free(t);
    }
}

/* ---------------- 帧 ---------------- */

static void vgl_begin_frame(r3d_backend_t *self, const r3d_target_t *target)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->tri_count = 0;
    im->draw_calls = 0;
    im->tex_binds = 0;
    im->collect_us_accum = 0;          /* 本帧 vgl_draw 累计耗时清零 */
    im->frame_begin_us = vgl_now_us(); /* 整帧 CPU 墙钟起点 */
    im->cull_input = 0;
    im->cull_oob = im->cull_behind = im->cull_bad = 0;
    im->cull_back = im->cull_degen = im->cull_capped = 0;

    if (!target) return;

    /* 包装宿主提供的目标内存为 vg_lite_buffer（字段对照 rive initRenderTargetBuffer）*/
    memset(&im->target, 0, sizeof(im->target));
    im->target.width  = (vg_lite_int32_t)target->w;
    im->target.height = (vg_lite_int32_t)target->h;
    im->target.stride = (vg_lite_int32_t)(target->stride ? target->stride : target->w * 4);
    im->target.format = to_vgl_format(target->format);
    im->target.tiled  = VG_LITE_LINEAR;
    im->target.memory = target->pixels;
    /* GPU 是独立硬件，DMA 访问 framebuffer 必须用物理地址。
     * 对齐 rive_for_vglite(initRenderTargetBuffer)：address 用 pinfo.fbmem 物理地址，
     * 而非 mmap 虚拟地址。本 SoC 上 fb 的虚拟地址≠物理地址时，若把 mmap 虚拟地址
     * 当 GPU 地址下发，GPU 会写到错误地址、命令跑完却不产生 END event，
     * 表现为 IDLE 但 INTR_STATUS=0、finish 超时挂死。
     * 故优先用 phys_addr；仅当其为 0(软件/宿主等价)时回退到虚拟地址。 */
    im->target.address = target->phys_addr
                         ? (vg_lite_uint32_t)target->phys_addr
                         : (vg_lite_uint32_t)(uintptr_t)target->pixels;
    im->target.handle = NULL;   /* 外部 framebuffer：无 vg_lite 分配句柄 */
    im->target.image_mode    = VG_LITE_NORMAL_IMAGE_MODE;
    im->target.transparency_mode  = VG_LITE_IMAGE_OPAQUE;
    im->target.compress_mode = VG_LITE_DEC_DISABLE;
    im->target.fc_enable     = 0;
    im->target.premultiplied = 0;
    im->target_valid  = (target->pixels != NULL);
    im->vp_w = (int)target->w;
    im->vp_h = (int)target->h;

    /* 清屏(深色背景)。clear 命令与后续 draw 同批，帧末 end_frame 统一 finish。 */
    if (im->target_valid) {
        vg_lite_color_t bg = argb_to_vgl(im->clear_argb);
        vg_lite_error_t e = vg_lite_clear(&im->target, NULL, bg);
        if (e != VG_LITE_SUCCESS)
            VGL_ERR("vg_lite_clear ret=%d(%s)", (int)e, vgl_err_str(e));
        /* 背景层：紧跟清屏，随后的几何会覆盖它，故天然有遮挡关系 */
        if (im->hook)
            im->hook(R3D_RENDER_PRE_GEOMETRY, &im->target,
                     im->vp_w, im->vp_h, im->hook_user);
    }
}

static void vgl_set_render_hook(r3d_backend_t *self, r3d_render_hook_t fn,
                                void *user)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->hook = fn;
    im->hook_user = user;
}

static void vgl_set_clear_color(r3d_backend_t *self, uint32_t argb)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->clear_argb = argb;
}

static void vgl_set_camera(r3d_backend_t *self, const r3d_camera_t *cam)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    im->view = cam->view;
    im->proj = cam->proj;
    r3d_mat4_mul(&im->view_proj, &im->proj, &im->view);  /* VP = P * V */
}

static void vgl_set_lighting(r3d_backend_t *self, const r3d_light_params_t *lp)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (lp) im->light = *lp;            /* 应用调用方参数 */
    else    r3d_light_params_default(&im->light); /* NULL = 恢复默认 */
}

/* ---------------- 绘制(收集三角形) ---------------- */

/* 计算/获取一个顶点的变换缓存(优化 #4)。首次(本 epoch)计算并写入缓存，
 * 后续三角形共享同一顶点时直接命中，避免重复的投影/法线矩阵乘法。
 * need_view_normal：matcap 需要 VIEW 空间法线，非 matcap 跳过以省一次矩阵乘。 */
static vgl_vcache_t *vgl_vtx(vgl_impl_t *im, const r3d_mesh_t *mesh, uint32_t vi,
                             const r3d_mat4_t *mvp, const r3d_mat4_t *mv,
                             int need_view_normal)
{
    vgl_vcache_t *vc = &im->vcache[vi];
    if (im->vcache_stamp[vi] == im->vcache_gen)
        return vc;   /* 本 epoch 已算，命中 */
    im->vcache_stamp[vi] = im->vcache_gen;

    const r3d_vertex_t *v = &mesh->vertices[vi];

    /* 裁剪空间 + 透视除法 → 屏幕 */
    r3d_vec4_t c = mul_mv(mvp, v->pos.x, v->pos.y, v->pos.z, 1.0f);
    if (c.w <= VGL_W_EPSILON) {
        vc->behind = 1; vc->bad = 0;
        return vc;
    }
    vc->behind = 0;
    float inv = 1.0f / c.w;
    float ndc_x = c.x * inv, ndc_y = c.y * inv;
    vc->sx = (ndc_x * 0.5f + 0.5f) * im->vp_w;
    vc->sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * im->vp_h;   /* Y 翻转 */
    vc->bad = (!isfinite(vc->sx) || !isfinite(vc->sy) ||
               fabsf(vc->sx) > VGL_COORD_LIMIT || fabsf(vc->sy) > VGL_COORD_LIMIT);

    /* view 空间深度(排序用) */
    r3d_vec4_t p = mul_mv(mv, v->pos.x, v->pos.y, v->pos.z, 1.0f);
    vc->vz = p.z;

    /* MV 法线(光照用) */
    r3d_vec4_t n = mul_mv(mv, v->normal.x, v->normal.y, v->normal.z, 0.0f);
    vc->mvn[0] = n.x; vc->mvn[1] = n.y; vc->mvn[2] = n.z;

    /* VIEW 法线(仅 matcap) */
    if (need_view_normal) {
        r3d_vec4_t nv = mul_mv(&im->view, v->normal.x, v->normal.y, v->normal.z, 0.0f);
        vc->vn[0] = nv.x; vc->vn[1] = nv.y; vc->vn[2] = nv.z;
    }
    return vc;
}

static void vgl_draw(r3d_backend_t *self, const r3d_mesh_t *mesh,
                     const r3d_mat4_t *model, const r3d_material_t *mat)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (!mesh || !mesh->vertices || !mesh->indices) return;

    long draw_t0 = vgl_now_us();   /* 本次 draw 收集耗时计时起点 */

    r3d_mat4_t mvp;
    r3d_mat4_mul(&mvp, &im->view_proj, model);  /* MVP = VP * M */
    r3d_mat4_t mv;
    r3d_mat4_mul(&mv, &im->view, model);         /* MV = V * M(法线变到 view 空间) */

    int use_matcap   = (mat->flags & R3D_MAT_USE_MATCAP) ? 1 : 0;
    int double_sided = (mat->flags & R3D_MAT_DOUBLE_SIDED) ? 1 : 0;
    int translucent  = (mat->flags & R3D_MAT_TRANSLUCENT) ? 1 : 0;
    vgl_tex_t *tex   = use_matcap ? (vgl_tex_t *)mat->matcap
                                  : (vgl_tex_t *)mat->base_color;
    uint32_t base_argb = mat->base_color_factor ? mat->base_color_factor : 0xFFFFFFFFu;

    /* 逐顶点缓存(优化 #4)：确保容量覆盖本 mesh 顶点数，递增 epoch 使旧标记失效。
     * 分配失败则回退(vcache=NULL)，下方按无缓存路径逐面直算，保证功能不受影响。 */
    if (im->vcache_cap < mesh->vertex_count) {
        free(im->vcache); free(im->vcache_stamp);
        im->vcache = (vgl_vcache_t *)malloc(sizeof(vgl_vcache_t) * mesh->vertex_count);
        im->vcache_stamp = (uint32_t *)calloc(mesh->vertex_count, sizeof(uint32_t));
        im->vcache_cap = (im->vcache && im->vcache_stamp) ? mesh->vertex_count : 0;
        if (!im->vcache_cap) { free(im->vcache); free(im->vcache_stamp);
                               im->vcache = NULL; im->vcache_stamp = NULL; }
    }
    int use_cache = (im->vcache_cap >= mesh->vertex_count);
    if (use_cache) {
        im->vcache_gen++;
        if (im->vcache_gen == 0) {  /* epoch 回绕：清 stamp 重来(极罕见) */
            memset(im->vcache_stamp, 0, sizeof(uint32_t) * im->vcache_cap);
            im->vcache_gen = 1;
        }
    }

    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        im->cull_input++;   /* 候选三角形计数(剔除前) */
        uint32_t idx3[3] = {
            r3d_index_at(mesh->indices, mesh->index_size, i),
            r3d_index_at(mesh->indices, mesh->index_size, i+1),
            r3d_index_at(mesh->indices, mesh->index_size, i+2)
        };
        /* 防御：索引越界(损坏/不匹配的模型)会越界读垃圾内存当顶点坐标。 */
        if (idx3[0] >= mesh->vertex_count || idx3[1] >= mesh->vertex_count ||
            idx3[2] >= mesh->vertex_count)
            { im->cull_oob++; continue; }
        const r3d_vertex_t *v0 = &mesh->vertices[idx3[0]];
        const r3d_vertex_t *v1 = &mesh->vertices[idx3[1]];
        const r3d_vertex_t *v2 = &mesh->vertices[idx3[2]];
        const r3d_vertex_t *vv[3] = { v0, v1, v2 };

        /* 取三个顶点的变换缓存(命中则不重算)。 */
        vgl_vcache_t *vc[3];
        int behind = 0;
        for (int k = 0; k < 3; k++) {
            vc[k] = vgl_vtx(im, mesh, idx3[k], &mvp, &mv, use_matcap);
            if (vc[k]->behind) behind = 1;
        }
        if (behind) { im->cull_behind++; continue; }

        float sx[3], sy[3], vz[3];
        int bad = 0;
        for (int k = 0; k < 3; k++) {
            sx[k] = vc[k]->sx; sy[k] = vc[k]->sy; vz[k] = vc[k]->vz;
            if (vc[k]->bad) { bad = 1; }
        }
        if (bad) { im->cull_bad++; continue; }

        /* 背面剔除(屏幕空间有向面积)；双面材质跳过 */
        float area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sx[2]-sx[0])*(sy[1]-sy[0]);
        if (!double_sided && area >= 0.0f) { im->cull_back++; continue; } /* 顺时针为正面(Y已翻转) */
        if (fabsf(area) < 0.01f) { im->cull_degen++; continue; }          /* 退化三角形 */

        if (im->tri_count >= im->tri_cap) { im->cull_capped++; break; }
        vgl_tri_t *T = &im->tris[im->tri_count];
        T->seq = im->tri_count;   /* 稳定排序序号 */
        im->tri_count++;

        for (int k = 0; k < 3; k++) {
            T->sx[k] = sx[k];
            T->sy[k] = sy[k];
            if (use_matcap) {
                /* matcap：view 空间法线 xy → UV(用缓存的 VIEW 法线) */
                float nx = vc[k]->vn[0], ny = vc[k]->vn[1], nz = vc[k]->vn[2];
                float l = sqrtf(nx*nx + ny*ny + nz*nz);
                if (l > 1e-6f) { nx /= l; ny /= l; }
                T->uv[k][0] = nx * 0.5f + 0.5f;
                T->uv[k][1] = -ny * 0.5f + 0.5f;
            } else {
                T->uv[k][0] = vv[k]->uv.x;
                T->uv[k][1] = vv[k]->uv.y;
            }
        }
        T->depth = (vz[0] + vz[1] + vz[2]) * (1.0f/3.0f);  /* view 空间线性深度，越负越远 */
        T->tex = (tex && tex->valid) ? &tex->buf : NULL;

        /* CPU 光照。对齐 OpenGL：view 空间法线，光方向 view 空间。
           法线用缓存的 MV 法线三顶点求和(与原逐面重算数值等价)。 */
        uint32_t draw_argb = base_argb;
        {
            float nx=0, ny=0, nz=0, ao=0;
            for (int k = 0; k < 3; k++) {
                nx += vc[k]->mvn[0]; ny += vc[k]->mvn[1]; nz += vc[k]->mvn[2];
                ao += vv[k]->ao;
            }
            float l = sqrtf(nx*nx + ny*ny + nz*nz);
            if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
            ao *= (1.0f/3.0f);
            float lx=im->light.light_dir[0], ly=im->light.light_dir[1], lz=im->light.light_dir[2];
            float ll = sqrtf(lx*lx+ly*ly+lz*lz); if(ll<1e-6f)ll=1.0f; lx/=ll; ly/=ll; lz/=ll;
            float d = nx*lx + ny*ly + nz*lz; if (d < 0) d = 0;
            float hemi = 0.5f + 0.5f*ny;
            float lit;
            if (use_matcap)
                lit = (0.78f + 0.10f*d + 0.12f*hemi) * (0.6f + 0.4f*ao); /* 柔和：保金属亮度 */
            else
                /* 中性漫反射: ambient + diffuse*d + hemi*hemiTerm，乘 AO，封顶防过曝。 */
                lit = (im->light.ambient + im->light.diffuse*d + im->light.hemi*hemi) * ao;
            float lit_cap = use_matcap ? 1.0f : im->light.lit_max;
            if (lit > lit_cap) lit = lit_cap;
            /* 无纹理时优先用逐顶点烘焙色(去纹理材质模式)，否则用材质 base_color_factor。 */
            uint32_t shade_argb = base_argb;
            if (!tex && (v0->color | v1->color | v2->color)) {
                uint32_t ar=0, rr=0, gr=0, br=0; int n=0;
                const r3d_vertex_t *cvs[3] = { v0, v1, v2 };
                for (int k = 0; k < 3; k++) {
                    uint32_t cc = cvs[k]->color;
                    if (!cc) continue;
                    ar += (cc>>24)&0xFF; rr += (cc>>16)&0xFF;
                    gr += (cc>>8)&0xFF;  br += cc&0xFF; n++;
                }
                if (n) shade_argb = ((ar/n)<<24)|((rr/n)<<16)|((gr/n)<<8)|(br/n);
            }
            uint32_t a=(shade_argb>>24)&0xFF, r=(shade_argb>>16)&0xFF,
                     g=(shade_argb>>8)&0xFF, b=shade_argb&0xFF;
            r=(uint32_t)(r*lit); g=(uint32_t)(g*lit); b=(uint32_t)(b*lit);
            draw_argb = (a<<24)|(r<<16)|(g<<8)|b;
        }
        T->color = argb_to_vgl(draw_argb);
        T->translucent = translucent;
        T->blend = mat->blend;
    }

    /* 累计本次 draw 的收集耗时(投影+透视除法+背面剔除+CPU 光照+写三角形队列)。
     * 一帧可能多次 draw(多 submesh)，故累加，end_frame 统一并入窗口统计。 */
    im->collect_us_accum += vgl_now_us() - draw_t0;
}

/* ---------------- 排序 + flush ---------------- */

/* 画家算法：远的先画。depth=view空间z，越负越远。半透明排在所有不透明之后。
 * 排序通过索引比较器 tri_idx_cmp 完成(优化 #3)，避免拷贝 80 字节结构体。 */
static const vgl_tri_t *g_tri_base;
static int tri_idx_cmp(const void *a, const void *b)
{
    const vgl_tri_t *ta = &g_tri_base[*(const uint32_t *)a];
    const vgl_tri_t *tb = &g_tri_base[*(const uint32_t *)b];
    if (ta->translucent != tb->translucent)
        return ta->translucent - tb->translucent;       /* 不透明(0)在前 */
    if (ta->depth < tb->depth) return -1;                /* 更负=更远，先画 */
    if (ta->depth > tb->depth) return 1;
    /* 深度相等：用收集顺序做稳定 tie-breaker，防止帧间排序抖动闪烁。 */
    if (ta->seq < tb->seq) return -1;
    if (ta->seq > tb->seq) return 1;
    return 0;
}

/* 解纹理 UV → 屏幕的 2x3 仿射矩阵：
 * 已知 3 个对应点 (u,v)->(sx,sy)，求 M 使得 [sx;sy] = M*[u;v;1]。
 * pattern_matrix 把 pattern 图像坐标(像素)映射到屏幕。VGLite pattern_matrix
 * 作用：屏幕点 → (经逆矩阵) → 采样 pattern 像素。这里我们给的是
 * pattern(纹理像素)→屏幕 的正向矩阵，驱动内部会取逆。 */
static int solve_affine(const vgl_tri_t *T, int tex_w, int tex_h, vg_lite_matrix_t *m)
{
    /* 纹理像素坐标 */
    float px[3], py[3];
    for (int k = 0; k < 3; k++) {
        px[k] = T->uv[k][0] * tex_w;
        py[k] = T->uv[k][1] * tex_h;
    }
    /* 求仿射: sx = a*px + b*py + c ; sy = d*px + e*py + f
     * 解 2 个 3x3 线性方程组(共享系数矩阵 [px py 1]) */
    float det = px[0]*(py[1]-py[2]) - py[0]*(px[1]-px[2]) + (px[1]*py[2]-px[2]*py[1]);
    if (fabsf(det) < 1e-6f) return 0;
    float inv = 1.0f / det;

    /* 系数矩阵 A=[px py 1] 的逆 × 屏幕坐标 */
    float i00 = (py[1]-py[2]) * inv;
    float i01 = (py[2]-py[0]) * inv;
    float i02 = (py[0]-py[1]) * inv;
    float i10 = (px[2]-px[1]) * inv;
    float i11 = (px[0]-px[2]) * inv;
    float i12 = (px[1]-px[0]) * inv;
    float i20 = (px[1]*py[2]-px[2]*py[1]) * inv;
    float i21 = (px[2]*py[0]-px[0]*py[2]) * inv;
    float i22 = (px[0]*py[1]-px[1]*py[0]) * inv;

    float a = i00*T->sx[0] + i01*T->sx[1] + i02*T->sx[2];
    float b = i10*T->sx[0] + i11*T->sx[1] + i12*T->sx[2];
    float c = i20*T->sx[0] + i21*T->sx[1] + i22*T->sx[2];
    float d = i00*T->sy[0] + i01*T->sy[1] + i02*T->sy[2];
    float e = i10*T->sy[0] + i11*T->sy[1] + i12*T->sy[2];
    float f = i20*T->sy[0] + i21*T->sy[1] + i22*T->sy[2];

    /* vg_lite_matrix m[row][col]，仿射(第三行 0 0 1) */
    m->m[0][0] = a; m->m[0][1] = b; m->m[0][2] = c;
    m->m[1][0] = d; m->m[1][1] = e; m->m[1][2] = f;
    m->m[2][0] = 0; m->m[2][1] = 0; m->m[2][2] = 1.0f;
    return 1;
}

static vg_lite_blend_t to_vgl_blend(int blend, int translucent)
{
    if (translucent) return VG_LITE_BLEND_SRC_OVER;
    switch (blend) {
        case R3D_BLEND_ADDITIVE: return VG_LITE_BLEND_ADDITIVE;
        case R3D_BLEND_MULTIPLY: return VG_LITE_BLEND_MULTIPLY;
        case R3D_BLEND_SCREEN:   return VG_LITE_BLEND_SCREEN;
        default:                 return VG_LITE_BLEND_NONE;
    }
}

/* 引擎每帧绘制提交前调用：把引擎侧测得的各阶段耗时暂存，
 * end_frame 时写入当帧原始记录后清零。 */
static void vgl_perf_frame_mark(r3d_backend_t *self, const r3d_engine_perf_t *ep)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (ep) {
        im->eng_perf_pending = *ep;
        im->eng_perf_valid = 1;
    }
}

/* 非负截断为 uint32(微秒)。 */
static inline uint32_t vgl_u32(long v) { return (uint32_t)(v < 0 ? 0 : v); }

/* 把一帧的原始测量值写入环形缓冲(O(1)，覆盖最旧)。
 * 后端侧耗时(t_frame/gpu/collect/sort/submit + submit 细分)由调用方传入；
 * 引擎侧阶段(wait/anim/node/deform/pan)从 eng_perf_pending 取；计数从 im 取。 */
static void vgl_raw_record(vgl_impl_t *im, uint32_t t_frame, uint32_t gpu,
                           uint32_t collect, uint32_t sort, uint32_t submit,
                           uint32_t sbuild, uint32_t sdraw, uint32_t sflush,
                           uint32_t tris, uint32_t drawcalls)
{
    if (!im->raw_ring) return;
    const r3d_engine_perf_t *ep = &im->eng_perf_pending;
    vgl_raw_rec_t *r = &im->raw_ring[im->raw_head];
    r->frame     = im->global_frame;
    r->t_frame   = t_frame;
    r->wait      = vgl_u32(ep->wait_us);
    r->anim      = vgl_u32(ep->anim_us);
    r->node      = vgl_u32(ep->node_us);
    r->deform    = vgl_u32(ep->deform_us);
    r->collect   = collect;
    r->sort      = sort;
    r->submit    = submit;
    r->sbuild    = sbuild;
    r->sdraw     = sdraw;
    r->sflush    = sflush;
    r->gpu       = gpu;
    r->pan       = vgl_u32(ep->pan_us);
    r->tris      = tris;
    r->drawcalls = drawcalls;
    r->tex       = im->tex_binds;

    im->raw_head = (im->raw_head + 1) % VGL_PERF_RING_FRAMES;
    if (im->raw_filled < VGL_PERF_RING_FRAMES) im->raw_filled++;
    im->global_frame++;
    /* 消费掉引擎侧数据，避免下一帧引擎未调用时误用旧值。 */
    memset(&im->eng_perf_pending, 0, sizeof(im->eng_perf_pending));
    im->eng_perf_valid = 0;
}

/* 每 N 秒把环形缓冲里的原始逐帧数据批量转储到串口/syslog。
 * 一次性批量输出(非逐帧内联)，转储后清空缓冲以复用；调用时机在 end_frame 末尾
 * (帧与帧之间)，把批量 I/O 突发局限在窗口边界。
 * 格式(离线工具 tools/perf/r3d_perf_analyze.py 解析；列由 HDR 的 fields= 动态决定)：
 *   头部： r3d_perfraw: HDR n=<帧数> budget_us=16667 fields=frame,t_frame,wait,anim,node,deform,collect,sort,submit,sbuild,sdraw,sflush,gpu,pan,tris,drawcalls,tex
 *   逐帧： r3d_perfraw: F <上述字段顺序的 17 个值>
 *   结尾： r3d_perfraw: END
 * 注：gpu 为 vg_lite_finish 残余等待，非 GPU 利用率(flush 使 GPU 与 submit 并行)。 */
static void vgl_raw_dump(vgl_impl_t *im)
{
    if (!im->raw_ring || im->raw_filled == 0) return;

    uint32_t n = im->raw_filled;
    /* 最旧样本的起点：缓冲未满时为 0，满时为 head(即将被覆盖的最旧位置)。 */
    uint32_t start = (im->raw_filled < VGL_PERF_RING_FRAMES) ? 0 : im->raw_head;

    VGL_LOG("%s: HDR n=%u budget_us=%d fields=frame,t_frame,wait,anim,node,"
            "deform,collect,sort,submit,sbuild,sdraw,sflush,gpu,pan,"
            "tris,drawcalls,tex",
            VGL_PERF_RAW_TAG, (unsigned)n, VGL_PERF_BUDGET_US);

    for (uint32_t k = 0; k < n; k++) {
        const vgl_raw_rec_t *r = &im->raw_ring[(start + k) % VGL_PERF_RING_FRAMES];
        VGL_LOG("%s: F %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                VGL_PERF_RAW_TAG,
                (unsigned)r->frame, (unsigned)r->t_frame, (unsigned)r->wait,
                (unsigned)r->anim, (unsigned)r->node, (unsigned)r->deform,
                (unsigned)r->collect, (unsigned)r->sort, (unsigned)r->submit,
                (unsigned)r->sbuild, (unsigned)r->sdraw, (unsigned)r->sflush,
                (unsigned)r->gpu, (unsigned)r->pan,
                (unsigned)r->tris, (unsigned)r->drawcalls, (unsigned)r->tex);
    }
    VGL_LOG("%s: END", VGL_PERF_RAW_TAG);

    /* 转储后清空，下个窗口重新累积(避免相邻窗口重复输出同批帧)。 */
    im->raw_head = im->raw_filled = 0;
    im->raw_dump_ms = vgl_now_ms();
}

/* 重置每秒统计窗口(汇总输出后、或切换模型时调用)。 */
static void vgl_window_reset(vgl_impl_t *im)
{
    im->stat_window_ms = vgl_now_ms();
    im->stat_frames = 0;
    im->stat_tris_sum = im->stat_dc_sum = im->stat_tex_sum = 0;
    im->stat_tris_min = im->stat_dc_min = im->stat_tex_min = 0xFFFFFFFFu;
    im->stat_tris_max = im->stat_dc_max = im->stat_tex_max = 0;
    vgl_ts_reset(&im->st_collect);
    vgl_ts_reset(&im->st_sort);
    vgl_ts_reset(&im->st_submit);
    vgl_ts_reset(&im->st_gpu);
    vgl_ts_reset(&im->st_frame);
    im->stat_cull_input_sum = im->stat_cull_oob_sum = im->stat_cull_behind_sum = 0;
    im->stat_cull_bad_sum = im->stat_cull_back_sum = 0;
    im->stat_cull_degen_sum = im->stat_cull_capped_sum = 0;
    im->stat_miss_count = 0;
    im->stat_worst_over = 0;
    for (int L = 0; L < VGL_BATCH_NLEVELS; L++) im->stat_batch_sum[L] = 0;
    im->stat_wait_sum = im->stat_anim_sum = im->stat_node_sum = im->stat_pan_sum = 0;
    im->stat_sbuild_sum = im->stat_sdraw_sum = im->stat_sflush_sum = 0;
}

/* 每秒一次输出标准 3D 性能统计：FPS + 三角面/draw call/纹理绑定的
 * 平均、最小、最大，以及帧内各阶段耗时(微秒)。把本帧计数并入窗口，
 * 跨过 1 秒边界时汇总并重置。
 * 阶段耗时(微秒)：collect=CPU 投影+光照+收集，sort=画家算法排序，
 * submit=构建 path+入命令缓冲，gpu=vg_lite_finish 等待 GPU，frame=整帧 CPU 墙钟。 */
static void vgl_stats_tick(vgl_impl_t *im, uint32_t tris, uint32_t dc, uint32_t tex,
                           long collect_us, long sort_us, long submit_us,
                           long gpu_us, long frame_us,
                           long sbuild_us, long sdraw_us, long sflush_us)
{
    im->stat_frames++;
    im->stat_tris_sum += tris;
    im->stat_dc_sum   += dc;
    im->stat_tex_sum  += tex;
    if (tris < im->stat_tris_min) im->stat_tris_min = tris;
    if (tris > im->stat_tris_max) im->stat_tris_max = tris;
    if (dc   < im->stat_dc_min)   im->stat_dc_min   = dc;
    if (dc   > im->stat_dc_max)   im->stat_dc_max   = dc;
    if (tex  < im->stat_tex_min)  im->stat_tex_min  = tex;
    if (tex  > im->stat_tex_max)  im->stat_tex_max  = tex;

    vgl_ts_add(&im->st_collect, collect_us);
    vgl_ts_add(&im->st_sort,    sort_us);
    vgl_ts_add(&im->st_submit,  submit_us);
    vgl_ts_add(&im->st_gpu,     gpu_us);
    vgl_ts_add(&im->st_frame,   frame_us);

    /* 新增阶段秒窗口累加(简单求和算平均)。引擎侧阶段取自本帧待写入的
     * eng_perf_pending(stats_tick 在 raw_record 之前调用，此时仍有效)。 */
    im->stat_sbuild_sum += (sbuild_us < 0 ? 0 : (uint64_t)sbuild_us);
    im->stat_sdraw_sum  += (sdraw_us  < 0 ? 0 : (uint64_t)sdraw_us);
    im->stat_sflush_sum += (sflush_us < 0 ? 0 : (uint64_t)sflush_us);
    {
        const r3d_engine_perf_t *ep = &im->eng_perf_pending;
        im->stat_wait_sum += (ep->wait_us < 0 ? 0 : (uint64_t)ep->wait_us);
        im->stat_anim_sum += (ep->anim_us < 0 ? 0 : (uint64_t)ep->anim_us);
        im->stat_node_sum += (ep->node_us < 0 ? 0 : (uint64_t)ep->node_us);
        im->stat_pan_sum  += (ep->pan_us  < 0 ? 0 : (uint64_t)ep->pan_us);
    }

    /* Deadline-Miss：基于 t_frame(引擎工作墙钟) vs 预算，非受限交付 fps。 */
    if (frame_us > VGL_PERF_BUDGET_US) {
        im->stat_miss_count++;
        uint32_t over = (uint32_t)(frame_us - VGL_PERF_BUDGET_US);
        if (over > im->stat_worst_over) im->stat_worst_over = over;
    }

    /* collect 阶段剔除计数并入窗口(算每帧平均) */
    im->stat_cull_input_sum  += im->cull_input;
    im->stat_cull_oob_sum    += im->cull_oob;
    im->stat_cull_behind_sum += im->cull_behind;
    im->stat_cull_bad_sum    += im->cull_bad;
    im->stat_cull_back_sum   += im->cull_back;
    im->stat_cull_degen_sum  += im->cull_degen;
    im->stat_cull_capped_sum += im->cull_capped;

    long now = vgl_now_ms();
    long dt  = now - im->stat_window_ms;
    if (dt < 1000 || im->stat_frames == 0) return;

    uint32_t nf = im->stat_frames;
    float fps = nf * 1000.0f / (float)dt;
    VGL_LOG("perf: fps=%.1f frames=%u | tri/f avg=%u min=%u max=%u | "
            "drawcall/f avg=%u min=%u max=%u | tex/f avg=%u min=%u max=%u",
            fps, (unsigned)nf,
            (unsigned)(im->stat_tris_sum / nf),
            (unsigned)im->stat_tris_min, (unsigned)im->stat_tris_max,
            (unsigned)(im->stat_dc_sum / nf),
            (unsigned)im->stat_dc_min, (unsigned)im->stat_dc_max,
            (unsigned)(im->stat_tex_sum / nf),
            (unsigned)im->stat_tex_min, (unsigned)im->stat_tex_max);

    /* 阶段耗时(微秒)：avg/min/max。这是定位瓶颈的关键——
     * 若 gpu 远大于 collect+submit，则瓶颈在 GPU 填充(减少覆盖面积/像素/AA)；
     * 若 collect 占大头，则瓶颈在 CPU 顶点处理(morph/投影/逐面光照)。
     * 16667us = 60fps 单帧预算，可直接对照各阶段占比。
     * 注：min/max 为单样本极值(p0/p100)，仅作粗略参考；稳健的 p50/p95/p99
     * 由离线工具在原始逐帧数据(r3d_perfraw)上计算。 */
    VGL_LOG("perf-time(us): frame avg=%u p0=%u p100=%u | budget60=16667 | "
            "collect avg=%u max=%u | sort avg=%u max=%u | "
            "submit avg=%u max=%u | gpu(finish) avg=%u max=%u",
            (unsigned)(im->st_frame.sum / nf), (unsigned)im->st_frame.min, (unsigned)im->st_frame.max,
            (unsigned)(im->st_collect.sum / nf), (unsigned)im->st_collect.max,
            (unsigned)(im->st_sort.sum / nf), (unsigned)im->st_sort.max,
            (unsigned)(im->st_submit.sum / nf), (unsigned)im->st_submit.max,
            (unsigned)(im->st_gpu.sum / nf), (unsigned)im->st_gpu.max);

    /* 补充阶段(avg，微秒)：submit 细分(建 path/draw 调用/flush) + 引擎侧
     * (wait/anim/node/pan)。submit 细分可判断 submit 成本在我们建 path 还是
     * driver 的 draw 调用；wait 为 vsync 空闲等待(非工作)。注：gpu 为
     * vg_lite_finish 残余等待，非 GPU 利用率(flush 使其与 submit 并行)。 */
    VGL_LOG("perf-time2(us): submit[build avg=%u draw avg=%u flush avg=%u] | "
            "wait avg=%u anim avg=%u node avg=%u pan avg=%u",
            (unsigned)(im->stat_sbuild_sum / nf),
            (unsigned)(im->stat_sdraw_sum / nf),
            (unsigned)(im->stat_sflush_sum / nf),
            (unsigned)(im->stat_wait_sum / nf),
            (unsigned)(im->stat_anim_sum / nf),
            (unsigned)(im->stat_node_sum / nf),
            (unsigned)(im->stat_pan_sum / nf));

    /* Deadline-Miss(截止期错失)：对硬性 60fps 目标最可执行的指标——
     * 本秒有多少帧的 t_frame 超过 16667us 预算，及最坏超出量。 */
    VGL_LOG("perf-miss: budget60=16667 miss=%u/%u (%u%%) worst_over=%uus",
            (unsigned)im->stat_miss_count, (unsigned)nf,
            (unsigned)(nf ? im->stat_miss_count * 100 / nf : 0),
            (unsigned)im->stat_worst_over);

    /* collect 阶段三角形输入/剔除分布(每帧平均)。kept=入队(=tri/f)。
     * input = kept + 各类剔除之和。可据此判断:
     *   - back 通常≈input 一半(背面剔除)，占比异常说明法线/朝向有问题
     *   - behind/bad 偏高说明相机近平面或坏顶点(morph 变形后越界)
     *   - capped>0 说明 tri_cap 不足，三角形被丢，需调大 max_triangles */
    VGL_LOG("perf-cull/f: input avg=%u kept avg=%u | back avg=%u degen avg=%u "
            "behind avg=%u bad avg=%u oob avg=%u capped avg=%u",
            (unsigned)(im->stat_cull_input_sum / nf),
            (unsigned)(im->stat_tris_sum / nf),
            (unsigned)(im->stat_cull_back_sum / nf),
            (unsigned)(im->stat_cull_degen_sum / nf),
            (unsigned)(im->stat_cull_behind_sum / nf),
            (unsigned)(im->stat_cull_bad_sum / nf),
            (unsigned)(im->stat_cull_oob_sum / nf),
            (unsigned)(im->stat_cull_capped_sum / nf));

    /* 合批潜力(tex=0，画家序相邻同色合并)：各感知量化档下合批后每帧 draw call 均值。
     * 对照 drawcall/f(当前=每三角形一次)看能降到多少。shift 越大合批越狠、
     * 画质损失越大。用于决定是否值得实现合批及选哪个档。 */
    VGL_LOG("perf-batch/f: drawcall_now=%u | s1(128lv)=%u s2(64lv)=%u "
            "s3(32lv)=%u s4(16lv)=%u",
            (unsigned)(im->stat_dc_sum / nf),
            (unsigned)(im->stat_batch_sum[0] / nf),
            (unsigned)(im->stat_batch_sum[1] / nf),
            (unsigned)(im->stat_batch_sum[2] / nf),
            (unsigned)(im->stat_batch_sum[3] / nf));

    /* 重置窗口 */
    vgl_window_reset(im);
}

/* 切换/载入新模型：先转储上一个模型残留帧(归属上一个模型)，打印模型标记，
 * 再把帧号清零、清空环形缓冲与统计窗口，使每个 b3dm 逐帧数据自成一段。 */
static void vgl_perf_model_begin(r3d_backend_t *self, const char *name)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    if (!im) return;
    /* 上一个模型还没到 5s 转储点的尾帧：先转出来(出现在新标记之前 → 归上一个模型)。
     * vgl_raw_dump 内部会清空 ring 并复位 raw_dump_ms。 */
    if (im->raw_filled > 0) vgl_raw_dump(im);
    /* syslog 同流打印模型标记(与逐帧转储同一路，顺序可靠，离线工具据此精确分段)。 */
    VGL_LOG("perf model=%s", name ? name : "?");
    /* 新模型从头计数、清空缓冲与统计窗口。 */
    im->global_frame = 0;
    im->raw_head = im->raw_filled = 0;
    im->raw_dump_ms = vgl_now_ms();
    vgl_window_reset(im);
    im->eng_perf_valid = 0;
    memset(&im->eng_perf_pending, 0, sizeof(im->eng_perf_pending));
}

/* 合批潜力估算：给定画家序(排序索引)，对某量化档(shift)统计合批后 draw call 数。
 * 规则贴合真实可实现的合批：只合并"画家序中相邻、量化后同色、同 blend、均为
 * tex=0"的三角形为一次 draw；有纹理三角形各自独立(draw_pattern 无法合批)。
 * 保持画家序=不改变遮挡与半透明顺序，故这是无正确性风险的合批上限估计。 */
static uint32_t vgl_batch_count(vgl_impl_t *im, uint8_t shift)
{
    uint32_t batches = 0;
    int have_prev = 0;
    uint32_t prev_key = 0; int prev_blend = -1, prev_tex = 0;
    for (uint32_t i = 0; i < im->tri_count; i++) {
        const vgl_tri_t *T = &im->tris[im->sort_idx[i]];
        int is_tex = (T->tex != NULL);
        if (is_tex) {
            /* 有纹理：必然自成一次 draw，且打断合批链。 */
            batches++;
            have_prev = 0;
            continue;
        }
        /* 量化颜色(ABGR8888)：每通道右移 shift 位做感知分档。 */
        uint32_t c = T->color;
        uint32_t a = ((c >> 24) & 0xFF) >> shift;
        uint32_t b = ((c >> 16) & 0xFF) >> shift;
        uint32_t g = ((c >> 8)  & 0xFF) >> shift;
        uint32_t r = ( c        & 0xFF) >> shift;
        uint32_t key = (a << 24) | (b << 16) | (g << 8) | r;
        if (have_prev && key == prev_key && T->blend == prev_blend && prev_tex == 0) {
            /* 与前一个同批，不新增 draw。 */
        } else {
            batches++;
            have_prev = 1;
            prev_key = key; prev_blend = T->blend; prev_tex = 0;
        }
    }
    return batches;
}

static void vgl_end_frame(r3d_backend_t *self)
{
    vgl_impl_t *im = (vgl_impl_t *)self->impl;
    uint32_t submit_tris = im->tri_count;  /* 本帧提交绘制的三角面数 */

    if (!im->target_valid || im->tri_count == 0) {
        /* 即使没有三角形，begin_frame 里的 vg_lite_clear 命令仍在命令缓冲中，
         * 必须 finish 让其落地，否则随后翻页会显示一块未清屏的缓冲。 */
        long gpu_us = 0;
        if (im->target_valid) {
            long g0 = vgl_now_us();
            vg_lite_error_t fe = vg_lite_finish();
            gpu_us = vgl_now_us() - g0;
            if (fe != VG_LITE_SUCCESS)
                VGL_ERR("end_frame(empty) vg_lite_finish ret=%d(%s)",
                        (int)fe, vgl_err_str(fe));
        }
        vgl_stats_tick(im, 0, 0, 0,
                       im->collect_us_accum, 0, 0, gpu_us,
                       vgl_now_us() - im->frame_begin_us, 0, 0, 0);
        vgl_raw_record(im, (uint32_t)(vgl_now_us() - im->frame_begin_us),
                       (uint32_t)gpu_us, (uint32_t)im->collect_us_accum,
                       0, 0, 0, 0, 0, 0, 0);
        if (vgl_now_ms() - im->raw_dump_ms >= VGL_PERF_DUMP_PERIOD_SEC * 1000)
            vgl_raw_dump(im);
        im->frame_no++;
        return;
    }

    /* 画家算法排序(优化 #3)：排序索引而非 80 字节结构体，交换只动 4 字节。 */
    long sort_t0 = vgl_now_us();
    for (uint32_t i = 0; i < im->tri_count; i++) im->sort_idx[i] = i;
    g_tri_base = im->tris;
    qsort(im->sort_idx, im->tri_count, sizeof(uint32_t), tri_idx_cmp);
    long sort_us = vgl_now_us() - sort_t0;

    /* 合批潜力分析(只统计不改绘制)：各量化档下合批后 draw call 数并入窗口。 */
    for (int L = 0; L < VGL_BATCH_NLEVELS; L++)
        im->stat_batch_sum[L] += vgl_batch_count(im, VGL_BATCH_SHIFT[L]);

    /* 逐三角形绘制(保持画家算法的逐面遮挡 + 每面 flat 光照的明暗层次，
     * 这是立体感的来源；不做"同色合批"——合批会把曲面上相邻、量化后同色的
     * 面合成一片纯色，丢失曲率的明暗渐变，看起来变平)。
     * 性能优化只保留"批量 flush"：不再每个 draw 都 flush，而是每
     * VGL_FLUSH_BATCH 次 flush 一次 + 帧末统一 finish。 */
    int pending_draws = 0;

    /* submit 细分累加(微秒)：sbuild=CPU 建 path/bbox/仿射，sdraw=vg_lite_draw
     * 调用，sflush=周期 flush。R3D_PERF_SUBMIT_DETAIL=0 时恒为 0。 */
    long sbuild_us = 0, sdraw_us = 0, sflush_us = 0;

    long submit_t0 = vgl_now_us();   /* 命令缓冲构建耗时起点(不含末尾 GPU 等待) */
    for (uint32_t i = 0; i < im->tri_count; i++) {
        vgl_tri_t *T = &im->tris[im->sort_idx[i]];   /* 按排序后的索引取三角形 */

        vg_lite_path_t *pp = (i < im->vgpaths_cap) ? &im->vgpaths[i] : NULL;
        if (!pp) break;

        long build_t0 = VGL_SUBMIT_T0();   /* build 段起点 */

        /* 三角形 path 顶点数据(FP32)：MOVE/LINE/LINE/CLOSE/END = 11 个 4 字节 slot。
         * opcode slot 按 uint32 整数位模式写、坐标 slot 按 IEEE754 float 写。 */
        float *pdata = &im->path_data[(size_t)i * 11];
        uint32_t *pop = (uint32_t *)pdata;
        pop[0] = (uint32_t)VLC_OP_MOVE;  pdata[1] = T->sx[0]; pdata[2] = T->sy[0];
        pop[3] = (uint32_t)VLC_OP_LINE;  pdata[4] = T->sx[1]; pdata[5] = T->sy[1];
        pop[6] = (uint32_t)VLC_OP_LINE;  pdata[7] = T->sx[2]; pdata[8] = T->sy[2];
        pop[9]  = (uint32_t)VLC_OP_CLOSE;
        pop[10] = (uint32_t)VLC_OP_END;

        float minx = T->sx[0], maxx = T->sx[0], miny = T->sy[0], maxy = T->sy[0];
        for (int k = 1; k < 3; k++) {
            if (T->sx[k] < minx) minx = T->sx[k];
            if (T->sx[k] > maxx) maxx = T->sx[k];
            if (T->sy[k] < miny) miny = T->sy[k];
            if (T->sy[k] > maxy) maxy = T->sy[k];
        }

        memset(pp, 0, sizeof(*pp));
        pp->format       = VG_LITE_FP32;
        /* 纹理(pattern)三角形用 LOW(关 AA)：GPU 抗锯齿产生分数边缘覆盖率，
         * 相邻三角形共享边各贡献部分覆盖，经 src-over 混合后不足 100%，缝隙
         * 像素透出底色形成可见黑线(对照 rive drawImageMeshPatternFill 的结论)。
         * 硬边(100%/0%)保证相邻三角形无缝拼接，仅外轮廓略锯齿。
         * 纯色三角形无此问题，保留 HIGH 以获得更平滑边缘/立体感。 */
        pp->quality      = T->tex ? VG_LITE_LOW : VGL_FILL_QUALITY;
        pp->path         = pdata;
        pp->path_length  = (vg_lite_uint32_t)(11 * sizeof(uint32_t));
        pp->path_changed = 1;
        pp->bounding_box[0] = minx;
        pp->bounding_box[1] = miny;
        pp->bounding_box[2] = maxx;
        pp->bounding_box[3] = maxy;

        vg_lite_matrix_t path_mat;
        vg_lite_identity(&path_mat);

        vg_lite_blend_t blend = to_vgl_blend(T->blend, T->translucent);

        /* pattern 仿射预解算属于 build 段。 */
        vg_lite_matrix_t pat_mat;
        int use_pattern = (T->tex &&
                           solve_affine(T, T->tex->width, T->tex->height, &pat_mat));

        VGL_SUBMIT_ADD(sbuild_us, build_t0);   /* build 段结束 */

        /* 实际下发 draw 命令(driver 侧成本)。 */
        long draw_t0 = VGL_SUBMIT_T0();
        vg_lite_error_t derr;
        if (use_pattern) {
            derr = vg_lite_draw_pattern(&im->target, pp, VGL_FILL_RULE,
                         &path_mat, T->tex, &pat_mat, blend,
                         VG_LITE_PATTERN_PAD, 0, T->color, VG_LITE_FILTER_BI_LINEAR);
            im->tex_binds++;
        } else {
            derr = vg_lite_draw(&im->target, pp, VGL_FILL_RULE,
                         &path_mat, blend, T->color);
        }
        VGL_SUBMIT_ADD(sdraw_us, draw_t0);

        if (derr != VG_LITE_SUCCESS) {
            VGL_ERR("tri[%u] %s ret=%d(%s), abort frame", (unsigned)i,
                    use_pattern ? "vg_lite_draw_pattern" : "vg_lite_draw",
                    (int)derr, vgl_err_str(derr));
            break;
        }
        im->draw_calls++;

        /* 批量 flush：每 VGL_FLUSH_BATCH 次 draw flush 一次(不再每个 draw 都 flush)。 */
        if (++pending_draws >= VGL_FLUSH_BATCH) {
            long flush_t0 = VGL_SUBMIT_T0();
            vg_lite_flush();
            VGL_SUBMIT_ADD(sflush_us, flush_t0);
            pending_draws = 0;
        }
    }
    long submit_us = vgl_now_us() - submit_t0;  /* 命令缓冲构建总耗时(CPU 侧) */

    long gpu_t0 = vgl_now_us();
    vg_lite_error_t fe = vg_lite_finish();
    long gpu_us = vgl_now_us() - gpu_t0;        /* 真正的 GPU 填充墙钟(阻塞等待) */
    if (fe != VG_LITE_SUCCESS)
        VGL_ERR("end_frame vg_lite_finish ret=%d(%s)", (int)fe, vgl_err_str(fe));

    /* 覆盖层：必须在上面的 finish 之后 —— 几何是在 finish 处才真正落地的，
     * 早于它绘制会被随后落地的不透明几何盖掉。钩子内若自行提交命令，需自己
     * finish(或依赖下面 present 的 finish)。 */
    if (im->hook)
        im->hook(R3D_RENDER_POST_GEOMETRY, &im->target,
                 im->vp_w, im->vp_h, im->hook_user);

    /* path 数据写在常驻 im->path_data，下一帧直接覆写复用，destroy 时统一释放。 */
    long frame_us = vgl_now_us() - im->frame_begin_us;
    vgl_stats_tick(im, submit_tris, im->draw_calls, im->tex_binds,
                   im->collect_us_accum, sort_us, submit_us, gpu_us,
                   frame_us, sbuild_us, sdraw_us, sflush_us);
    /* 原始逐帧记录 + 每 N 秒批量转储(在帧末、帧间发出，把 I/O 突发局限在窗口边界)。 */
    vgl_raw_record(im, (uint32_t)frame_us, (uint32_t)gpu_us,
                   (uint32_t)im->collect_us_accum, (uint32_t)sort_us,
                   (uint32_t)submit_us,
                   (uint32_t)sbuild_us, (uint32_t)sdraw_us, (uint32_t)sflush_us,
                   submit_tris, im->draw_calls);
    if (vgl_now_ms() - im->raw_dump_ms >= VGL_PERF_DUMP_PERIOD_SEC * 1000)
        vgl_raw_dump(im);
    im->frame_no++;
}

static void vgl_present(r3d_backend_t *self)
{
    (void)self;
    /* 宿主模式：目标即宿主缓冲，flush 已在 end_frame 完成。 */
    vg_lite_error_t fe = vg_lite_finish();
    if (fe != VG_LITE_SUCCESS)
        VGL_ERR("present vg_lite_finish ret=%d(%s)", (int)fe, vgl_err_str(fe));
}

static bool vgl_query_feature(r3d_backend_t *self, r3d_feature_t f)
{
    (void)self;
    switch (f) {
        case R3D_FEATURE_PERSPECTIVE_TEXTURE: return false; /* 仿射近似(CPU 投影) */
        case R3D_FEATURE_ZBUFFER:             return false; /* 画家算法 */
        case R3D_FEATURE_PER_PIXEL_LIGHT:     return false; /* 烘焙光照 */
        case R3D_FEATURE_BLEND_MULTIPLY:      return true;
        default:                              return false;
    }
}

static const r3d_backend_vtable_t VGL_VT = {
    .init            = vgl_init,
    .destroy         = vgl_destroy,
    .create_texture  = vgl_create_texture,
    .destroy_texture = vgl_destroy_texture,
    .begin_frame     = vgl_begin_frame,
    .set_render_hook = vgl_set_render_hook,
    .set_clear_color = vgl_set_clear_color,
    .set_camera      = vgl_set_camera,
    .set_lighting    = vgl_set_lighting,
    .draw            = vgl_draw,
    .end_frame       = vgl_end_frame,
    .present         = vgl_present,
    .query_feature   = vgl_query_feature,
    .perf_frame_mark = vgl_perf_frame_mark,
    .perf_model_begin = vgl_perf_model_begin,
};

r3d_backend_t *r3d_backend_vglite_create(void)
{
    r3d_backend_t *be = (r3d_backend_t *)calloc(1, sizeof(r3d_backend_t));
    if (!be) return NULL;
    be->impl = calloc(1, sizeof(vgl_impl_t));
    if (!be->impl) { free(be); return NULL; }
    be->vt = &VGL_VT;
    return be;
}

/* 宿主模式：vg_lite 已由外部(如 NuttX gpu_init / LVGL)初始化，
 * 本后端不再调用 vg_lite_init / vg_lite_close。 */
r3d_backend_t *r3d_backend_vglite_create_hosted(void)
{
    r3d_backend_t *be = r3d_backend_vglite_create();
    if (!be) return NULL;
    ((vgl_impl_t *)be->impl)->hosted = 1;
    return be;
}
