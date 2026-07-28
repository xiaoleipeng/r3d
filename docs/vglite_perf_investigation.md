# r3d VGLite 后端性能优化调研报告

> 工作负载：facecap 面部 morph 动画，约 2844 三角形，466×466，目标 60fps（单帧预算 16667µs）
> 平台：BES best1600 / Cortex-M55 @ 192MHz，硬件 FPU + MVE(Helium)，D-cache/I-cache 开
> 渲染：VGLite 2.5D GPU，CPU 端投影 + 逐面 flat 光照 + 逐三角形 draw call

## 一、结论速览

当前实测约 **8.6fps**（整帧 CPU 墙钟 ~116ms），距 60fps 差约 7 倍。

**瓶颈 100% 在 CPU，不在 GPU。** GPU 实际填充（`vg_lite_finish`）每帧仅 ~0.4ms（占 0.3%）。时间几乎全花在 CPU 侧的命令构建和顶点处理上。

在**不改 VGLite 驱动、不减面、不降逐面光照画质**三个约束下，主瓶颈 submit（占整帧 62%）**没有可行的优化路径**——所有常规手段均已查证并被硬件/驱动约束堵死（见第四节）。

已落地的 CPU 侧无损优化把整帧从最初 ~142ms 降到 ~116ms（7→8.6fps），但 submit 是天花板。

## 二、整帧耗时分解（实测，固定相机 morph）

| 阶段 | 耗时 | 占比 | 性质 |
|------|------|------|------|
| **submit** | ~71,000µs | **62%** | CPU 逐三角形构建 vg_lite 命令缓冲 |
| deform | ~14,500µs | 12% | CPU morph 顶点变形 |
| sort | ~11,900µs | 10% | CPU 画家算法排序 |
| collect | ~10,000µs | 9% | CPU 投影+透视除法+逐面光照 |
| gpu(finish) | ~400µs | 0.3% | GPU 实际填充（阻塞等待） |

关键数字：submit 71ms / 2844 draw call ≈ **每次 draw call ~25µs 纯 CPU 开销**，而 GPU 画完全部三角形仅 0.4ms。典型的 **draw-call bound**。

## 三、已落地的优化（无画质损失）

| 优化 | 阶段 | 手段 | 效果 |
|------|------|------|------|
| 顶点变换缓存 | collect | 同顶点被多面共享，每 epoch 只算一次投影/法线 | collect 15→10ms |
| 排序改索引 | sort | qsort 排 4 字节索引而非 80 字节结构体 | sort 16→12ms |
| 连续 scratch 累加 | deform | morph 累加改到连续 float 缓冲，消除大结构体跨步的 cache 灾难 | deform 37→14ms |
| axpy + restrict | deform | 累加循环加 restrict，触发 -O3 下 MVE 自动向量化 | 待真机确认 |

morph 变形的根因已定位：原实现 target 外层、逐顶点跨步读改写 40 字节大结构体的 `out` 数组，39 个 target 反复冲刷 61KB 数组 → cache miss。改为连续 scratch 缓冲后大幅改善。（注：不是软浮点问题——`.config` 确认 `CONFIG_ARCH_FPU=y` + `mve.fp`，反汇编确认硬浮点。）

## 四、submit 优化路径调研（全部查证，均不可行）

submit 的本质：每次 `vg_lite_draw` 内部无条件 push ~15 条 state 命令 + path 数据 ≈ 160 字节命令缓冲。2844 次 = 每帧 ~600KB 命令。命令缓冲 64KB 双缓冲，每帧溢出中途 submit ~9 次，每次触发全 D-cache 冲刷 + GPU stall。

| 方案 | 设想 | 查证结论 | 为什么不可行 |
|------|------|---------|-------------|
| **path upload** | `vg_lite_upload_path` 预上传 path 到 GPU 内存，draw 时只 push_call | ❌ | path 每帧变（morph+投影），每帧重上传 = 2844 次 `vg_lite_allocate`，比 push_data 更慢 |
| **加大命令缓冲** | 调大到能装整帧，消除中途 submit+stall | ❌ | GPU 连续内存池仅 448KB（framebuffer+tess+双缓冲），装不下 600KB×2 |
| **命令缓冲搬 SRAM** | cached PSRAM → 更快的片内 SRAM | ❌ | 该布局下 GPU 可用 SRAM 段仅 56KB，装不下 128KB 命令缓冲，且 SRAM 被音频等模块共享 |
| **定向 cache flush** | 避免每次 submit 全 D-cache 冲刷 | ⚠️ | 驱动 `gpu_cache_invalid()` 已有定向分支，但命令量 >32KB 时走全冲刷（合理权衡）；需命令量降到 32KB 以下才触发，又回到减命令量 |
| **合批**（同色三角形合并 draw） | 减少 draw call 数 | ❌ | 实测：facecap 整脸 morph + 逐面光照，颜色高度分散。即使量化到 16 级/通道（明显 banding），draw call 仅从 2844 降到 ~2350（省 17%）。固定相机下比自旋更差。收益不足且掉画质 |
| **blit 替代 draw** | 预制单位三角形纹理，矩阵变换 blit | ❌ | 精确对比命令数：blit 每次 ~20+ 条 state（draw ~15 条），CPU 预处理更重（4 角变换+逆矩阵+gamma+premul），且矩形包围盒 2× overdraw + 透明边缝隙。命令量翻倍 → submit 更慢，画质更差 |
| **跳过恒定 state**（方案 E） | 手工构命令缓冲，只推变化的 state | ❌ | 公开 API 无手工命令控制入口（push_state/push_data 是驱动内部函数）；且可省的仅 path_matrix(identity 6条)+SCALE+BIAS+tess-control ~8-9 条恒定 state，而 morph 驱动的 color 和 path 坐标（submit 大头）省不掉，收益仅 20-30%；须改驱动，风险高 |

### 一个驱动层的已知问题（非本仓库可改）

VGLite 的 `push_state()` 内部有状态去重逻辑 `if (hw_states[addr].state != data)`，但被 `/* TODO wait for hw */` **注释禁用**。因此每次 draw 无条件重写所有 state（包括对全部 2844 三角形恒定的 path_matrix=identity、SCALE、BIAS、tess-control）。若 BES 实现该去重，submit 可省约 20-30%，但这是全系统共用 GPU 驱动的改动。

## 五、submit 瓶颈机理（供决策参考）

```
每帧 2844 次 vg_lite_draw
  每次 → push ~15 条 state(8字节/条) + push_data(path ~44字节)
       → ~160 字节命令 写入 cached PSRAM 命令缓冲
  累计 ~600KB 命令/帧
    → 64KB 命令缓冲每帧溢出 ~9 次
      → 每次溢出: submit() + stall()  [GPU 同步阻塞]
        + gpu_cache_invalid(): 命令>32KB → hal_cache_sync_all() [全 D-cache 冲刷]
```

三个叠加的 CPU 成本：①逐条命令构建（含溢出边界检查）②反复全 D-cache 冲刷（连累其它阶段 cache）③反复 GPU stall。合批本可同时压下这三者，但因颜色分散省不动 draw 数。

## 六、建议（按现实性排序）

1. **调整目标帧率**：当前架构下 2844 面逐面光照 morph 的现实上限约 10-15fps。若产品可接受，是最省事的路径。
2. **改 VGLite 驱动实现 state 去重**：需 BES 配合，风险中等，submit 预计省 20-30%（整帧 ~100ms，~10fps）。收益有限。
3. **改渲染架构**：放弃 VGLite 逐三角形 draw，改用 CPU+MVE 软件光栅化直接填 framebuffer，绕开整个 submit 瓶颈。工作量大、高风险，但可能是唯一能显著突破的方向（很多无 GPU 低端设备即如此）。
4. **降画质换合批**：放弃逐面 flat 光照（改为整体单色或大块量化），使颜色可合批。违背当前视觉目标，不推荐。

## 七、性能观测工具（本次已交付）

- 固件每秒摘要日志：`perf`（fps/tri/drawcall）、`perf-time`（各阶段 avg+p0/p100+budget）、`perf-miss`（deadline 错失）、`perf-cull`（剔除分布）、`perf-batch`（合批潜力）、`perf-deform`（morph 细分）
- 原始逐帧数据：每 N 秒批量转储到串口（`r3d_perfraw`），供离线全保真分析
- 离线工具 `tools/perf/`：
  - `r3d_perf_analyze.py`：算 p50/p95/p99、1%/0.1% low、标准差、抖动、直方图
  - `r3d_perf.html`：纯前端拖拽日志即分析

采集方法：串口抓日志 → 喂离线工具。这些指标遵循实时渲染业界惯例（百分位而非单样本极值、deadline-miss 率、区分交付 fps 与引擎工作墙钟）。
