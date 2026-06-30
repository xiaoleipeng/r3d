#!/usr/bin/env bash
#
# batch_convert.sh — 批量把文件夹内的 glTF/glb 转换为 B3DM
#
# 用法:
#   ./batch_convert.sh <输入文件夹> [输出文件夹] [选项]
#
#   输入文件夹   必填。递归扫描其中所有 *.gltf / *.glb
#   输出文件夹   选填，默认 = <输入文件夹>/b3dm
#
# 选项(透传给 gltf2b3dm):
#   --max-tris N         减面预算(默认 0=不减)
#   --tex-size N         普通纹理上限(默认 256)
#   --detail-tex-size N  高细节纹理上限(默认 1024)
#   --variant NAME       材质变体子串
#   --no-suffix          输出文件名不追加三角形数量(默认追加 _<tris>)
#   --flat               不保留输入子目录结构(全部平铺到输出根)
#   --tool PATH          指定 gltf2b3dm 路径
#   --verify PATH        指定 b3dm2gltf 路径(用于读取真实三角形数)
#
# 示例:
#   ./batch_convert.sh /path/to/models
#   ./batch_convert.sh /path/to/models /path/to/out --max-tris 50000
#
set -euo pipefail

if [[ $# -lt 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi

IN_DIR="$1"; shift
OUT_DIR=""
if [[ $# -gt 0 && "${1:0:2}" != "--" ]]; then OUT_DIR="$1"; shift; fi

ADD_SUFFIX=1
FLATTEN=0
TOOL=""
VERIFY=""
PASS_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-suffix) ADD_SUFFIX=0; shift;;
    --flat)      FLATTEN=1; shift;;
    --tool)      TOOL="$2"; shift 2;;
    --verify)    VERIFY="$2"; shift 2;;
    --max-tris|--tex-size|--detail-tex-size|--variant)
                 PASS_ARGS+=("$1" "$2"); shift 2;;
    *) echo "未知选项: $1"; exit 2;;
  esac
done

if [[ ! -d "$IN_DIR" ]]; then echo "[错误] 输入文件夹不存在: $IN_DIR"; exit 1; fi
IN_DIR="$(cd "$IN_DIR" && pwd)"
[[ -z "$OUT_DIR" ]] && OUT_DIR="$IN_DIR/b3dm"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R3D_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
find_tool() {
  local name="$1" override="$2"
  if [[ -n "$override" && -x "$override" ]]; then echo "$override"; return; fi
  for c in "/tmp/r3d_build2/$name" "$R3D_ROOT/build/$name" "$R3D_ROOT/build-gl/$name"; do
    [[ -x "$c" ]] && { echo "$c"; return; }
  done
}
TOOL="$(find_tool gltf2b3dm "$TOOL")"
VERIFY="$(find_tool b3dm2gltf "${VERIFY:-}")"

if [[ -z "$TOOL" ]]; then
  echo "[错误] 找不到 gltf2b3dm。请先构建，或用 --tool 指定。"
  echo "  构建: cmake -S $R3D_ROOT -B /tmp/r3d_build2 -DR3D_BUILD_TOOLS=ON && cmake --build /tmp/r3d_build2 --target gltf2b3dm"
  exit 1
fi

echo "[batch] 工具:   $TOOL"
echo "[batch] 验证:   ${VERIFY:-（无，文件名不带三角形数）}"
echo "[batch] 输入:   $IN_DIR"
echo "[batch] 输出:   $OUT_DIR"
[[ ${#PASS_ARGS[@]} -gt 0 ]] && echo "[batch] 转换选项: ${PASS_ARGS[*]}"
echo "----------------------------------------------------------------"

[[ -z "$VERIFY" ]] && ADD_SUFFIX=0

mapfile -t FILES < <(find "$IN_DIR" -type f \( -iname '*.gltf' -o -iname '*.glb' \) | sort)
if [[ ${#FILES[@]} -eq 0 ]]; then echo "[batch] 未找到 .gltf/.glb 文件"; exit 0; fi

TMP_VERIFY_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_VERIFY_DIR"' EXIT

ok=0; fail=0
for f in "${FILES[@]}"; do
  base="$(basename "$f")"
  stem="${base%.*}"
  rel="${f#$IN_DIR/}"
  reldir="$(dirname "$rel")"

  if [[ "$FLATTEN" -eq 1 || "$reldir" == "." ]]; then
    dest_dir="$OUT_DIR"
  else
    dest_dir="$OUT_DIR/$reldir"
    mkdir -p "$dest_dir"
  fi

  tmp_out="$TMP_VERIFY_DIR/$stem.b3dm"
  echo "[转换] $rel"
  if ! "$TOOL" "$f" "$tmp_out" "${PASS_ARGS[@]}" 2>&1 | sed 's/^/    /'; then
    echo "    ✗ 转换失败"; fail=$((fail+1)); continue
  fi
  if [[ ! -f "$tmp_out" ]]; then echo "    ✗ 未产出文件"; fail=$((fail+1)); continue; fi

  final_name="$stem.b3dm"
  if [[ "$ADD_SUFFIX" -eq 1 ]]; then
    info="$("$VERIFY" "$tmp_out" "$TMP_VERIFY_DIR/$stem.gltf" 2>/dev/null || true)"
    tris="$(echo "$info" | grep -oE '索引[0-9]+' | head -1 | grep -oE '[0-9]+' || true)"
    if [[ -n "$tris" ]]; then tris=$((tris/3)); final_name="${stem}_${tris}.b3dm"; fi
  fi

  cp "$tmp_out" "$dest_dir/$final_name"
  echo "    ✓ → $dest_dir/$final_name"
  ok=$((ok+1))
done

echo "----------------------------------------------------------------"
echo "[batch] 完成: 成功 $ok，失败 $fail，共 ${#FILES[@]}"
