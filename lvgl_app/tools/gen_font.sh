#!/bin/sh
# ============================================================
# gen_font.sh - 生成 LVGL 用中文字模（D7-1）
#
# 为什么需要这个脚本：
#   LVGL 的字体不是"运行时加载 TTF"，而是编译期把所有字形烤成 C 数组。
#   所以字模是一个「生成物」——它不应该被手改，必须能一键重现。
#
# 用法：
#   ./tools/gen_font.sh            # 默认：16px / 2bpp / 全部常用汉字
#   ./tools/gen_font.sh 20 4       # 20px / 4bpp
#   ./tools/gen_font.sh 16 2 quick # 只烤界面用到的百来个字（约 30KB）
#
# 参数：
#   $1 字号（像素，默认 16）
#   $2 bpp（1/2/4，默认 2）
#   $3 模式：full（默认，全部）/ quick（只有常用字）
#
# 依赖：
#   - lv_font_conv（npm 包，已装在 ~/.workbuddy/binaries/node/workspace）
#   - fonts/NotoSansSC-Regular-extracted.ttf（从 ttc 拆出来的单个 OTF）
#
# 关于那个 ttf：
#   系统里只有 NotoSansCJK-Regular.ttc（10 个字体打包的合集），
#   lv_font_conv 不认 ttcf 签名，所以必须先拆出 index=2（简体 SC）
#   成为独立文件。拆分逻辑见本脚本末尾的说明。
# ============================================================
set -e

SIZE=${1:-16}
BPP=${2:-2}
MODE=${3:-full}

HERE=$(cd "$(dirname "$0")/.." && pwd)
FONT_IN="$HERE/fonts/NotoSansSC-Regular-extracted.ttf"
FONT_OUT="$HERE/src/fonts/lv_font_cn_${SIZE}.c"

NODE_BIN="$HOME/.workbuddy/binaries/node/workspace/node_modules/.bin/lv_font_conv"

[ -x "$NODE_BIN" ] || { echo "错误: 找不到 lv_font_conv ($NODE_BIN)" >&2; exit 1; }
[ -f "$FONT_IN" ]  || { echo "错误: 找不到源字体 $FONT_IN" >&2; exit 1; }

mkdir -p "$HERE/src/fonts"

case "$MODE" in
quick)
	# 界面文案 + 常用标点。改界面后忘了重跑，新字就会变空白，
	# 所以这个模式只适合实验，正式版本用 full。
	SYMS="回声伙伴红绿色蓝按钮温度电量点击次数最近一次上面的试退出当前时间设置音量网络暂无内容确定取消返回左右开关中文显示测试"
	RANGES="-r 0x20-0x7F -r 0x3000-0x301F"
	ARGS="--symbols $SYMS $RANGES"
	;;
full)
	# ASCII + 中文标点 + CJK 基本区全部 20902 字
	ARGS="-r 0x20-0x7F -r 0x3000-0x301F -r 0x4E00-0x9FA5 \
	      -r 0xFF01 -r 0xFF08 -r 0xFF09 -r 0xFF0C -r 0xFF1A -r 0xFF1B \
	      -r 0xFF1F -r 0xFFE5 \
	      -r 0x2026 -r 0x00B7 -r 0x2014 -r 0x2018 -r 0x2019 \
	      -r 0x201C -r 0x201D -r 0x2103 -r 0x00B0"
	;;
*)
	echo "错误: 模式只能是 full 或 quick" >&2
	exit 1
	;;
esac

echo "生成中: ${SIZE}px / ${BPP}bpp / ${MODE}"
echo "  输入: $FONT_IN"
echo "  输出: $FONT_OUT"

# shellcheck disable=SC2086
# --no-compress 必须和 lv_conf.h 的 LV_USE_FONT_COMPRESSED 配对：
#   不加这个参数 → 字模是 RLE 压缩的 → LV_USE_FONT_COMPRESSED 必须为 1
#   加了这个参数 → LV_USE_FONT_COMPRESSED 必须为 0
# 配对错了屏上全空白。实测压缩只省 9%，这里选不压缩（省 CPU、少一个坑）。
"$NODE_BIN" --size "$SIZE" --bpp "$BPP" --format lvgl \
	--font "$FONT_IN" \
	$ARGS \
	--no-compress --no-prefilter \
	--lv-font-name "lv_font_cn_${SIZE}" \
	-o "$FONT_OUT"

# ---- 体积报告 ----
SRC_SZ=$(wc -c < "$FONT_OUT")
GLYPHS=$(grep -c 'U+' "$FONT_OUT" 2>/dev/null || echo "?")
echo ""
echo "完成: $FONT_OUT"
printf "  源码体积: %.2f MB\n" "$(echo "$SRC_SZ" | awk '{print $1/1048576}')"
echo "  字形数  : $GLYPHS（含注释行，近似值）"
echo ""
echo "下一步: cd build && make -j4"
echo "（这个 .c 编译较慢，1-2 分钟属正常）"
