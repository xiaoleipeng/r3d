#!/usr/bin/env bash
#
# start.sh — 一键启动 gltf2b3dm 在线转换/预览工具
#
# 自动完成：
#   1. 定位 r3d 源码根目录
#   2. 若命令行工具(gltf2b3dm / b3dm2gltf)缺失则用 CMake 构建
#   3. 启动 Node 本地服务
#   4. 打开默认浏览器
#
# 用法:
#   ./start.sh [--port N] [--build-dir DIR] [--no-open] [--rebuild]
#
set -euo pipefail

# ---- 默认参数 ----
PORT=8787
BUILD_DIR="/tmp/r3d_build2"
OPEN_BROWSER=1
REBUILD=0

# ---- 解析参数 ----
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)      PORT="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --no-open)   OPEN_BROWSER=0; shift;;
    --rebuild)   REBUILD=1; shift;;
    -h|--help)
      echo "用法: $0 [--port N] [--build-dir DIR] [--no-open] [--rebuild]"
      echo "  --port N        服务端口 (默认 8787)"
      echo "  --build-dir DIR CMake 构建目录 (默认 /tmp/r3d_build2)"
      echo "  --no-open       不自动打开浏览器"
      echo "  --rebuild       强制重新构建命令行工具"
      exit 0;;
    *) echo "未知参数: $1"; exit 2;;
  esac
done

# ---- 路径定位 ----
# 脚本位于 <r3d>/tools/gltf2b3dm/web/start.sh，向上三级即 r3d 根
WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R3D_ROOT="$(cd "$WEB_DIR/../../.." && pwd)"

echo "[start] r3d 源码根: $R3D_ROOT"
echo "[start] web 目录:   $WEB_DIR"
echo "[start] 构建目录:   $BUILD_DIR"

TOOL="$BUILD_DIR/gltf2b3dm"
VERIFY="$BUILD_DIR/b3dm2gltf"

# ---- 依赖检查 ----
command -v node >/dev/null 2>&1 || { echo "[错误] 未找到 node，请先安装 Node.js"; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "[错误] 未找到 cmake"; exit 1; }

# ---- 构建命令行工具(按需) ----
need_build=0
if [[ "$REBUILD" -eq 1 ]]; then
  need_build=1
elif [[ ! -x "$TOOL" || ! -x "$VERIFY" ]]; then
  need_build=1
fi

if [[ "$need_build" -eq 1 ]]; then
  echo "[start] 构建命令行工具 (gltf2b3dm / b3dm2gltf)…"
  cmake -S "$R3D_ROOT" -B "$BUILD_DIR" -DR3D_BUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" --target gltf2b3dm -j"$(nproc)"
  cmake --build "$BUILD_DIR" --target b3dm2gltf -j"$(nproc)"
else
  echo "[start] 命令行工具已存在，跳过构建 (--rebuild 可强制重建)"
fi

[[ -x "$TOOL" ]]   || { echo "[错误] 构建后仍找不到 $TOOL"; exit 1; }
[[ -x "$VERIFY" ]] || { echo "[警告] 找不到 $VERIFY，b3dm 预览将不可用"; }

# ---- 若端口被占用，提示并尝试复用/退出 ----
if command -v lsof >/dev/null 2>&1 && lsof -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "[警告] 端口 $PORT 已被占用，可能服务已在运行。"
  echo "        直接打开 http://localhost:$PORT 或用 --port 换端口。"
  if [[ "$OPEN_BROWSER" -eq 1 ]]; then
    URL="http://localhost:$PORT"
    (xdg-open "$URL" >/dev/null 2>&1 || open "$URL" >/dev/null 2>&1 || true) &
  fi
  exit 0
fi

# ---- 启动服务(后台) ----
URL="http://localhost:$PORT"
echo "[start] 启动服务: $URL"

node "$WEB_DIR/server.js" --port "$PORT" --tool "$TOOL" --verify "$VERIFY" &
SERVER_PID=$!

# 退出时清理服务
cleanup() {
  echo ""
  echo "[start] 停止服务 (pid=$SERVER_PID)…"
  kill "$SERVER_PID" >/dev/null 2>&1 || true
}
trap cleanup INT TERM EXIT

# ---- 等待服务就绪 ----
echo -n "[start] 等待服务就绪"
for i in $(seq 1 40); do
  if command -v curl >/dev/null 2>&1; then
    if curl -s -o /dev/null "$URL/"; then echo " ✓"; break; fi
  else
    # 无 curl：用 node 探测
    if node -e "require('http').get('$URL/',r=>process.exit(0)).on('error',()=>process.exit(1))" 2>/dev/null; then
      echo " ✓"; break
    fi
  fi
  echo -n "."
  sleep 0.25
  if [[ "$i" -eq 40 ]]; then echo " (超时，但服务可能仍在启动)"; fi
done

# ---- 打开浏览器 ----
if [[ "$OPEN_BROWSER" -eq 1 ]]; then
  echo "[start] 打开浏览器: $URL"
  (xdg-open "$URL" >/dev/null 2>&1 \
    || open "$URL" >/dev/null 2>&1 \
    || sensible-browser "$URL" >/dev/null 2>&1 \
    || echo "[提示] 无法自动打开浏览器，请手动访问 $URL") &
fi

echo ""
echo "============================================================"
echo "  gltf2b3dm 在线工具运行中:  $URL"
echo "  按 Ctrl+C 停止服务"
echo "============================================================"

# ---- 前台等待服务进程 ----
wait "$SERVER_PID"
