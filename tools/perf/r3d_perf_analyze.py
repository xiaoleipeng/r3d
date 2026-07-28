#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
r3d_perf_analyze.py — r3d VGLite 后端原始逐帧性能数据离线分析工具。

配套固件侧的 Firmware_Perf 原始采集(backend_vglite.c)：固件每 N 秒把环形缓冲
里的原始逐帧数据以 `r3d_perfraw:` 前缀批量转储到串口/syslog。本工具从捕获的
串口日志(可含其它混杂日志行)中 grep 出这些原始行，重建逐帧序列，并以全保真度
计算固件内不便计算的稳健统计量：

  - 百分位 p50/p95/p99（顺序统计量，抗离群，刻画卡顿尾部）
  - 1% low / 0.1% low（最差 1%/0.1% 帧的均值，游戏业界低帧指标）
  - 标准差（样本标准差，离散度）
  - 帧间抖动 Jitter = mean(|t_i - t_{i-1}|)（平滑度）
  - 直方图（围绕 60fps 预算的耗时分布）

聚焦 morph 工作负载的三项核心指标：t_frame、gpu、deform。
collect/submit 作为可选分析。

数据格式（固件转储，见 backend_vglite.c）：
  r3d_perfraw: HDR n=<帧数> budget_us=16667 fields=frame,t_frame,gpu,deform,collect,sort,submit,tris,drawcalls
  r3d_perfraw: F <frame> <t_frame> <gpu> <deform> <collect> <sort> <submit> <tris> <drawcalls>
  r3d_perfraw: END

用法：
  # 从串口日志文件分析
  python3 r3d_perf_analyze.py capture.log

  # 从串口直接管道输入
  cat /dev/ttyUSB0 | python3 r3d_perf_analyze.py -

  # 指定预算(默认 16667us=60fps)，输出直方图
  python3 r3d_perf_analyze.py capture.log --budget 16667 --hist

  # 导出重建后的逐帧数据为 CSV，供其它工具进一步分析
  python3 r3d_perf_analyze.py capture.log --csv frames.csv
"""

import sys
import argparse
import math

RAW_TAG = "r3d_perfraw:"

# 字段名 → 在 F 行中的列索引（与固件 fields= 头部一致，作为默认/回退）。
DEFAULT_FIELDS = ["frame", "t_frame", "wait", "anim", "node", "deform",
                  "collect", "sort", "submit", "sbuild", "sdraw", "sflush",
                  "gpu", "pan", "tris", "drawcalls", "tex"]

# 参与统计分析的耗时指标（morph 聚焦：核心三项 + 可选阶段）。
CORE_METRICS = ["t_frame", "submit", "gpu", "deform"]
OPTIONAL_METRICS = ["collect", "sort", "sbuild", "sdraw", "sflush",
                    "anim", "node", "wait", "pan"]


def parse_log(lines):
    """从混合日志流中解析出所有 r3d_perfraw 批次，返回逐帧记录列表。

    合并所有批次（跨多次 N 秒转储）的帧为一个连续序列。以每行携带的
    fields 头部为准解析列，未见头部时回退到 DEFAULT_FIELDS。
    返回: (records, fields) —— records 为 dict 列表，fields 为字段名列表。
    """
    fields = DEFAULT_FIELDS
    records = []
    seen_frames = set()  # 按全局 frame 去重（相邻窗口理论上不重叠，防御性去重）

    for line in lines:
        idx = line.find(RAW_TAG)
        if idx < 0:
            continue
        payload = line[idx + len(RAW_TAG):].strip()

        if payload.startswith("HDR"):
            # 解析 fields=a,b,c 更新列布局
            for tok in payload.split():
                if tok.startswith("fields="):
                    fields = tok[len("fields="):].split(",")
            continue
        if payload.startswith("END"):
            continue
        if payload.startswith("F "):
            parts = payload[2:].split()
            if len(parts) < len(fields):
                continue  # 截断/损坏行，跳过
            try:
                vals = [int(x) for x in parts[:len(fields)]]
            except ValueError:
                continue
            rec = dict(zip(fields, vals))
            fno = rec.get("frame")
            if fno is not None and fno in seen_frames:
                continue
            if fno is not None:
                seen_frames.add(fno)
            records.append(rec)

    # 按 frame 序号排序，保证逐帧序列时间有序（抖动计算依赖顺序）
    if records and "frame" in records[0]:
        records.sort(key=lambda r: r["frame"])
    return records, fields


def percentile(sorted_vals, p):
    """线性插值百分位。sorted_vals 已升序，p in [0,100]。"""
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return float(sorted_vals[0])
    rank = (p / 100.0) * (len(sorted_vals) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(sorted_vals[lo])
    frac = rank - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def low_mean(sorted_vals, fraction):
    """最差 fraction 帧（最大值一侧）的均值，如 1% low / 0.1% low。"""
    if not sorted_vals:
        return 0.0
    k = max(1, int(len(sorted_vals) * fraction))
    worst = sorted_vals[-k:]
    return sum(worst) / len(worst)


def stddev(vals):
    """样本标准差（n-1）。"""
    n = len(vals)
    if n < 2:
        return 0.0
    mean = sum(vals) / n
    var = sum((x - mean) ** 2 for x in vals) / (n - 1)
    return math.sqrt(var)


def jitter(vals):
    """帧间抖动 = mean(|t_i - t_{i-1}|)，需按时间顺序传入。"""
    if len(vals) < 2:
        return 0.0
    diffs = [abs(vals[i] - vals[i - 1]) for i in range(1, len(vals))]
    return sum(diffs) / len(diffs)


def analyze_metric(name, vals_in_order, budget):
    """对单个指标计算全套统计量。vals_in_order 为按帧序的原始值。"""
    n = len(vals_in_order)
    s = sorted(vals_in_order)
    mean = sum(s) / n
    over = sum(1 for v in vals_in_order if v > budget)
    return {
        "name": name,
        "n": n,
        "avg": mean,
        "min": s[0],
        "max": s[-1],
        "p50": percentile(s, 50),
        "p95": percentile(s, 95),
        "p99": percentile(s, 99),
        "low1": low_mean(s, 0.01),
        "low01": low_mean(s, 0.001),
        "std": stddev(vals_in_order),
        "jitter": jitter(vals_in_order),
        "over_budget": over,
        "over_pct": (100.0 * over / n) if n else 0.0,
    }


def fmt_report(stats, budget, is_frame):
    """格式化单指标报告行。"""
    lines = []
    lines.append("  %-8s  n=%d" % (stats["name"], stats["n"]))
    lines.append("    avg=%.0f  p50=%.0f  p95=%.0f  p99=%.0f  (min=%.0f max=%.0f)"
                 % (stats["avg"], stats["p50"], stats["p95"], stats["p99"],
                    stats["min"], stats["max"]))
    lines.append("    std=%.0f  jitter=%.0f  1%%low=%.0f  0.1%%low=%.0f"
                 % (stats["std"], stats["jitter"], stats["low1"], stats["low01"]))
    if is_frame:
        # 只有整帧 t_frame 与预算比较才有 deadline 意义
        verdict = "OK" if stats["over_pct"] < 1.0 else "MISS"
        lines.append("    budget=%d  over=%d/%d (%.1f%%)  [%s]"
                     % (budget, stats["over_budget"], stats["n"],
                        stats["over_pct"], verdict))
    return "\n".join(lines)


def print_histogram(name, vals, budget, nbuckets=12):
    """围绕预算打印耗时分布直方图（等宽桶，末桶为 >budget 溢出）。"""
    if not vals:
        return
    # 桶宽：把 [0, budget] 均分为 nbuckets-1 个桶，最后一个桶收纳 > budget。
    width = budget / (nbuckets - 1)
    counts = [0] * nbuckets
    for v in vals:
        if v >= budget:
            counts[-1] += 1
        else:
            counts[int(v / width)] += 1
    peak = max(counts) or 1
    total = len(vals)
    print("  直方图 %s (桶宽=%.0fus, 末桶=>预算):" % (name, width))
    for i, c in enumerate(counts):
        if i == nbuckets - 1:
            label = ">%d" % budget
        else:
            label = "%d-%d" % (int(i * width), int((i + 1) * width))
        bar = "#" * int(40 * c / peak)
        print("    %-14s %6d (%4.1f%%) %s"
              % (label, c, 100.0 * c / total, bar))


def export_csv(records, fields, path):
    with open(path, "w") as f:
        f.write(",".join(fields) + "\n")
        for r in records:
            f.write(",".join(str(r.get(k, "")) for k in fields) + "\n")


# 各阶段中文标签与说明，用于 HTML 报告的瓶颈分解。
STAGE_LABELS = {
    "t_frame": ("整帧", "begin_frame→end_frame 整帧 CPU 墙钟(不含 wait/pan)"),
    "wait":    ("等待", "双缓冲 poll 等空闲缓冲(vsync 空闲，非 CPU 工作)"),
    "anim":    ("动画", "r3d_anim_update 关键帧采样+混合"),
    "node":    ("节点矩阵", "逐 DYNAMIC_NODE submesh 的 r3d_anim_node_matrix 累计"),
    "deform":  ("变形", "CPU 顶点变形(morph/skin)"),
    "collect": ("收集", "投影+透视除法+背面剔除+逐面光照"),
    "sort":    ("排序", "画家算法 qsort"),
    "submit":  ("提交", "建 path + vg_lite_draw 入命令缓冲(= build+draw+flush)"),
    "sbuild":  ("submit·建path", "CPU 构建 path 顶点/bbox/仿射"),
    "sdraw":   ("submit·draw调用", "vg_lite_draw/draw_pattern 调用(driver 侧)"),
    "sflush":  ("submit·flush", "周期 vg_lite_flush"),
    "gpu":     ("GPU(finish残余)", "vg_lite_finish 残余等待，非 GPU 利用率(与 submit 并行)"),
    "pan":     ("翻页", "FBIOPAN_DISPLAY(上一帧值)"),
}
# 构成整帧的子阶段（用于占比分解）。submit 用其三个细分(sbuild/sdraw/sflush)代替，
# 避免与 submit 重复计数；wait/pan 在 t_frame 之外，不计入分解。相加≈t_frame。
BREAKDOWN_STAGES = ["anim", "node", "deform", "collect", "sort",
                    "sbuild", "sdraw", "sflush", "gpu"]


def build_html(records, fields, budget):
    """生成自包含 HTML 报告字符串：摘要卡片 + 统计表 + 瓶颈分解 +
    逐帧曲线图(canvas) + 逐帧详细列表。无外部依赖。"""
    n = len(records)

    # ---- 统计各指标 ----
    metrics = [m for m in (CORE_METRICS + OPTIONAL_METRICS + ["sort"]) if m in fields]
    # 去重保序
    seen = set()
    metrics = [m for m in metrics if not (m in seen or seen.add(m))]
    stats = {}
    for m in metrics:
        vals = [r[m] for r in records if m in r]
        if vals:
            stats[m] = analyze_metric(m, vals, budget)

    # ---- 瓶颈分解（基于 avg）----
    frame_avg = stats["t_frame"]["avg"] if "t_frame" in stats else 0
    breakdown = []
    for s in BREAKDOWN_STAGES:
        if s in stats:
            avg = stats[s]["avg"]
            pct = (100.0 * avg / frame_avg) if frame_avg else 0
            breakdown.append((s, avg, pct))
    breakdown.sort(key=lambda x: -x[1])

    # ---- 达标情况 ----
    fps_est = 1e6 / frame_avg if frame_avg else 0
    miss = stats["t_frame"]["over_budget"] if "t_frame" in stats else 0
    miss_pct = stats["t_frame"]["over_pct"] if "t_frame" in stats else 0

    def us(v):
        return "%.0f" % v

    # ---- HTML 拼装 ----
    h = []
    h.append("""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>r3d 性能分析报告</title>
<style>
  :root{--bg:#0f1117;--card:#1a1d27;--fg:#e6e8ee;--mut:#9aa0ae;--acc:#4ea1ff;
        --ok:#3ecf8e;--warn:#ffb454;--bad:#ff5c5c;--line:#2a2e3a;}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
       font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",sans-serif;}
  .wrap{max-width:1180px;margin:0 auto;padding:24px}
  h1{font-size:22px;margin:0 0 4px} h2{font-size:16px;margin:28px 0 12px;color:var(--acc)}
  .sub{color:var(--mut);margin-bottom:20px}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
  .card .k{color:var(--mut);font-size:12px} .card .v{font-size:24px;font-weight:600;margin-top:4px}
  .card .u{font-size:12px;color:var(--mut);font-weight:400}
  table{width:100%;border-collapse:collapse;background:var(--card);
        border:1px solid var(--line);border-radius:10px;overflow:hidden}
  th,td{padding:8px 10px;text-align:right;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}
  th:first-child,td:first-child{text-align:left}
  th{background:#20242f;color:var(--mut);font-weight:600;position:sticky;top:0}
  tr:hover td{background:#20242f}
  .bad{color:var(--bad)} .ok{color:var(--ok)} .warn{color:var(--warn)} .mut{color:var(--mut)}
  .bar{height:10px;background:#20242f;border-radius:5px;overflow:hidden;min-width:80px;display:inline-block;vertical-align:middle;width:100%}
  .bar>i{display:block;height:100%;background:linear-gradient(90deg,var(--acc),#7b5cff)}
  .barcell{width:38%}
  .detail{max-height:520px;overflow:auto;border:1px solid var(--line);border-radius:10px}
  .detail table{border:0;border-radius:0}
  .over td{color:var(--bad)}
  .note{color:var(--mut);font-size:12px;margin-top:8px}
  canvas{background:var(--card);border:1px solid var(--line);border-radius:10px;width:100%;height:260px}
  .legend span{display:inline-block;margin-right:14px;font-size:12px;color:var(--mut)}
  .legend i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:middle}
</style></head><body><div class="wrap">""")

    h.append("<h1>r3d 性能分析报告</h1>")
    h.append('<div class="sub">采集帧数 %d · 预算 %d us (60fps) · 字段: %s</div>'
             % (n, budget, ",".join(fields)))

    # 摘要卡片
    verdict_cls = "ok" if miss_pct < 1 else ("warn" if miss_pct < 50 else "bad")
    h.append('<div class="cards">')
    h.append('<div class="card"><div class="k">实测帧率(基于t_frame)</div>'
             '<div class="v %s">%.1f<span class="u"> fps</span></div></div>'
             % (verdict_cls, fps_est))
    h.append('<div class="card"><div class="k">整帧耗时 avg</div>'
             '<div class="v">%s<span class="u"> us</span></div></div>' % us(frame_avg))
    h.append('<div class="card"><div class="k">预算 16667us</div>'
             '<div class="v %s">%.1f×<span class="u"> 超支</span></div></div>'
             % (verdict_cls, frame_avg / budget if budget else 0))
    h.append('<div class="card"><div class="k">超预算帧(Deadline-Miss)</div>'
             '<div class="v %s">%d<span class="u">/%d (%.0f%%)</span></div></div>'
             % (verdict_cls, miss, n, miss_pct))
    if "tris" in fields:
        tv = [r["tris"] for r in records if "tris" in r]
        h.append('<div class="card"><div class="k">三角形/帧</div>'
                 '<div class="v">%.0f</div></div>' % (sum(tv) / len(tv)))
    h.append('</div>')

    # 瓶颈分解
    h.append("<h2>瓶颈分解（占整帧 avg 比例）</h2>")
    h.append('<table><tr><th>阶段</th><th>说明</th><th>avg (us)</th>'
             '<th>占比</th><th class="barcell">占比图</th></tr>')
    for s, avg, pct in breakdown:
        label, desc = STAGE_LABELS.get(s, (s, ""))
        h.append('<tr><td><b>%s</b></td><td class="mut">%s</td><td>%s</td>'
                 '<td>%.1f%%</td><td class="barcell"><span class="bar">'
                 '<i style="width:%.1f%%"></i></span></td></tr>'
                 % (label, desc, us(avg), pct, min(pct, 100)))
    h.append('</table>')
    h.append('<div class="note">GPU 占比极低说明瓶颈在 CPU；占比最高的阶段是首要优化目标。</div>')

    # 统计表（百分位/离散度）
    h.append("<h2>各指标统计（微秒）</h2>")
    h.append('<table><tr><th>指标</th><th>avg</th><th>p50</th><th>p95</th>'
             '<th>p99</th><th>min</th><th>max</th><th>std</th>'
             '<th>jitter</th><th>1% low</th><th>0.1% low</th></tr>')
    for m in metrics:
        if m not in stats:
            continue
        st = stats[m]
        label = STAGE_LABELS.get(m, (m, ""))[0]
        h.append('<tr><td><b>%s</b> <span class="mut">%s</span></td>'
                 '<td>%s</td><td>%s</td><td>%s</td><td class="warn">%s</td>'
                 '<td>%s</td><td class="bad">%s</td><td>%s</td><td>%s</td>'
                 '<td>%s</td><td>%s</td></tr>'
                 % (label, m, us(st["avg"]), us(st["p50"]), us(st["p95"]),
                    us(st["p99"]), us(st["min"]), us(st["max"]), us(st["std"]),
                    us(st["jitter"]), us(st["low1"]), us(st["low01"])))
    h.append('</table>')
    h.append('<div class="note">p50 稳健(抗离群)，p99/max 反映卡顿尖峰，'
             'jitter 反映平滑度，1%/0.1% low 为最差帧均值。</div>')

    # 逐帧曲线图（canvas）
    h.append("<h2>逐帧曲线</h2>")
    h.append('<div class="legend"><span><i style="background:#4ea1ff"></i>t_frame</span>'
             '<span><i style="background:#ffb454"></i>submit</span>'
             '<span><i style="background:#3ecf8e"></i>deform</span>'
             '<span><i style="background:#ff5c5c"></i>预算线</span></div>')
    h.append('<canvas id="chart"></canvas>')

    # 逐帧详细列表
    h.append("<h2>逐帧详细列表</h2>")
    h.append('<div class="detail"><table><tr>')
    for f in fields:
        h.append("<th>%s</th>" % f)
    h.append("</tr>")
    for r in records:
        over = r.get("t_frame", 0) > budget
        h.append('<tr class="%s">' % ("over" if over else ""))
        for f in fields:
            h.append("<td>%s</td>" % r.get(f, ""))
        h.append("</tr>")
    h.append("</table></div>")
    h.append('<div class="note">红色行 = 该帧 t_frame 超过预算。</div>')

    # 图表数据 + 绘制脚本
    tf = [r.get("t_frame", 0) for r in records]
    sub = [r.get("submit", 0) for r in records]
    dfm = [r.get("deform", 0) for r in records]
    import json as _json
    h.append("<script>")
    h.append("var TF=%s,SUB=%s,DFM=%s,BUD=%d;" %
             (_json.dumps(tf), _json.dumps(sub), _json.dumps(dfm), budget))
    h.append(r"""
var cv=document.getElementById('chart');
function draw(){
  var dpr=window.devicePixelRatio||1, W=cv.clientWidth, H=cv.clientHeight;
  cv.width=W*dpr; cv.height=H*dpr; var g=cv.getContext('2d'); g.scale(dpr,dpr);
  g.clearRect(0,0,W,H);
  var pad=36, n=TF.length;
  var mx=Math.max.apply(null,TF.concat([BUD])); mx*=1.08;
  function X(i){return pad+(W-pad-8)*(n<2?0:i/(n-1));}
  function Y(v){return H-pad-(H-pad-8)*(v/mx);}
  // grid + budget line
  g.strokeStyle='#2a2e3a'; g.lineWidth=1;
  g.beginPath();g.moveTo(pad,H-pad);g.lineTo(W-8,H-pad);g.moveTo(pad,8);g.lineTo(pad,H-pad);g.stroke();
  g.strokeStyle='#ff5c5c';g.setLineDash([5,4]);g.beginPath();
  g.moveTo(pad,Y(BUD));g.lineTo(W-8,Y(BUD));g.stroke();g.setLineDash([]);
  g.fillStyle='#ff5c5c';g.font='11px sans-serif';g.fillText('预算 '+BUD+'us',pad+4,Y(BUD)-4);
  // y labels
  g.fillStyle='#9aa0ae';
  g.fillText((mx/1000).toFixed(0)+'ms',2,14); g.fillText('0',2,H-pad+4);
  function line(arr,col){g.strokeStyle=col;g.lineWidth=1.5;g.beginPath();
    for(var i=0;i<arr.length;i++){var x=X(i),y=Y(arr[i]);i?g.lineTo(x,y):g.moveTo(x,y);}g.stroke();}
  line(SUB,'#ffb454'); line(DFM,'#3ecf8e'); line(TF,'#4ea1ff');
}
draw(); window.addEventListener('resize',draw);
""")
    h.append("</script>")

    h.append("</div></body></html>")
    return "".join(h)


def main():
    ap = argparse.ArgumentParser(
        description="r3d VGLite 原始逐帧性能数据离线分析（p50/p95/p99、"
                    "1%/0.1% low、标准差、抖动、直方图；聚焦 morph）")
    ap.add_argument("input", help="串口日志文件路径，或 '-' 从标准输入读取")
    ap.add_argument("--budget", type=int, default=16667,
                    help="单帧预算(微秒)，默认 16667=60fps")
    ap.add_argument("--hist", action="store_true", help="输出耗时分布直方图")
    ap.add_argument("--optional", action="store_true",
                    help="额外分析 collect/submit 阶段")
    ap.add_argument("--csv", metavar="PATH",
                    help="把重建的逐帧数据导出为 CSV")
    ap.add_argument("--html", metavar="PATH", nargs="?", const="r3d_perf_report.html",
                    help="生成自包含 HTML 报告(含摘要/统计表/瓶颈分解/逐帧曲线/逐帧详细列表)，"
                         "默认输出 r3d_perf_report.html")
    args = ap.parse_args()

    if args.input == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.input, "r", errors="replace") as f:
            lines = f.readlines()

    records, fields = parse_log(lines)
    if not records:
        print("未找到任何 r3d_perfraw 原始逐帧数据。"
              "请确认串口日志中包含 'r3d_perfraw: F ...' 行。", file=sys.stderr)
        return 1

    print("=" * 64)
    print("r3d 性能分析 — 采集帧数: %d" % len(records))
    print("字段: %s" % ",".join(fields))
    print("预算: %d us (60fps)" % args.budget)
    print("=" * 64)

    metrics = list(CORE_METRICS)
    if args.optional:
        metrics += OPTIONAL_METRICS

    for m in metrics:
        if m not in fields:
            continue
        vals = [r[m] for r in records if m in r]
        if not vals:
            continue
        st = analyze_metric(m, vals, args.budget)
        print(fmt_report(st, args.budget, is_frame=(m == "t_frame")))
        print()

    if args.hist:
        for m in metrics:
            if m in fields:
                vals = [r[m] for r in records if m in r]
                print_histogram(m, vals, args.budget)
                print()

    # 三角形/drawcall 概览（若有）
    if "tris" in fields:
        tris = [r["tris"] for r in records if "tris" in r]
        dcs = [r.get("drawcalls", 0) for r in records]
        if tris:
            print("  几何: tris avg=%.0f max=%d | drawcalls avg=%.0f"
                  % (sum(tris) / len(tris), max(tris),
                     sum(dcs) / len(dcs) if dcs else 0))

    if args.csv:
        export_csv(records, fields, args.csv)
        print("\n已导出逐帧数据到: %s" % args.csv)

    if args.html:
        html = build_html(records, fields, args.budget)
        with open(args.html, "w", errors="replace") as f:
            f.write(html)
        print("\n已生成 HTML 报告: %s（浏览器打开查看图表与逐帧详细列表）" % args.html)

    return 0


if __name__ == "__main__":
    sys.exit(main())
