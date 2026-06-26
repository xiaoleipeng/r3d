#!/usr/bin/env bash
# run_vglite.sh — 本机运行 VGLite 后端(softsim) demo，渲染并自动转 PNG。
#
# 用法:
#   ./run_vglite.sh <model.b3dm> [输出前缀]
#   ./run_vglite.sh tests/assets/watch.b3dm /tmp/watch
#
# 相机环境变量(可选)：
#   R3D_YAW=-0.5 R3D_PITCH=0.3 R3D_DIST=0.9 ./run_vglite.sh ...
#
# 多角度(设 MULTI=1 渲染 4 个角度)：
#   MULTI=1 ./run_vglite.sh tests/assets/watch.b3dm /tmp/watch
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

MODEL="${1:-tests/assets/watch.b3dm}"
OUT="${2:-/tmp/r3d_vglite}"
DEMO="build-vg/demo_vglite"

# ---- 确保已构建 ----
if [ ! -x "$DEMO" ]; then
    echo "[构建] VGLite demo 未找到，开始构建..."
    cmake -S . -B build-vg -DR3D_BACKEND_VGLITE=ON -DR3D_BUILD_TOOLS=OFF >/dev/null
    cmake --build build-vg --target demo_vglite >/dev/null
    echo "[构建] 完成"
fi

# ---- PPM → PNG 辅助 ----
ppm2png() {
    python3 -c "
import struct,zlib,sys
ppm,png=sys.argv[1],sys.argv[2]
f=open(ppm,'rb');f.readline();w,h=map(int,f.readline().split());f.readline();d=f.read()
raw=bytearray()
for y in range(h): raw.append(0);raw.extend(d[y*w*3:(y+1)*w*3])
def ch(t,b): return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
out=b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(bytes(raw),9))+ch(b'IEND',b'')
open(png,'wb').write(out)
" "$1" "$2"
}

render() {
    local yaw="$1" pitch="$2" dist="$3" tag="$4"
    R3D_YAW="$yaw" R3D_PITCH="$pitch" R3D_DIST="$dist" \
        "$DEMO" "$MODEL" "${OUT}${tag}.ppm" >/dev/null
    ppm2png "${OUT}${tag}.ppm" "${OUT}${tag}.png"
    echo "  → ${OUT}${tag}.png"
}

echo "[渲染] 模型: $MODEL"
if [ "${MULTI:-0}" = "1" ]; then
    echo "[渲染] 多角度(正面/侧面/背面/俯视):"
    render -0.5  0.3  0.9 _front
    render -1.4  0.2  0.9 _side
    render  3.0  0.15 0.95 _back
    render -0.5  1.0  0.9 _top
else
    # 单帧：用环境变量(未设则用默认正面视角)
    render "${R3D_YAW:--0.5}" "${R3D_PITCH:-0.3}" "${R3D_DIST:-0.9}" ""
fi
echo "[完成]"
