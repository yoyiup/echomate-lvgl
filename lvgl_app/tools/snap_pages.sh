#!/bin/sh
# snap_pages.sh —— 把 4 个页面各截一张图，一次验证完所有 UI
#
# 在【宿主】上跑：
#   cd lvgl_app && sh tools/snap_pages.sh
#
# 原理：利用 lvgl_app 的 LVGL_START_PAGE 环境变量，
# 每次启动直接显示指定页面 → 抓 /dev/fb0 → 转 PNG → 换下一页。
# 这样不用在板子上用手指一页页点，改完 UI 跑一条命令就能看全貌。

set -e

BOARD=root@172.32.0.93
KEY=$HOME/.ssh/id_ed25519
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -i $KEY"
PY=${PYTHON:-python3}
OUT=shots

mkdir -p "$OUT"

for page in home net volume about; do
	echo "===== $page ====="

	# 停旧实例
	ssh $SSHOPT $BOARD 'sh /etc/init.d/S99start_lvgl stop >/dev/null 2>&1' || true

	# 启动到指定页面，跑 30 秒足够截图
	ssh $SSHOPT $BOARD "rm -f /tmp/lvgl_boot.log; \
		LVGL_START_PAGE=$page LVGL_RUN_SEC=30 sh /etc/init.d/S99start_lvgl start >/dev/null 2>&1"

	# 等界面画完。LVGL 首帧 + 字体渲染在 200MHz 的小核上要几百毫秒，
	# 给 3 秒足够。短了可能抓到黑屏或半成品。
	sleep 3

	# 抓显存
	ssh $SSHOPT $BOARD 'dd if=/dev/fb0 of=/tmp/fb.bin bs=153600 count=1 2>/dev/null'

	# 拉回本地再转 PNG（宿主上有 python，板上不一定有）
	scp $SSHOPT $BOARD:/tmp/fb.bin "$OUT/fb_$page.bin" >/dev/null 2>&1
	$PY tools/fb2png.py "$OUT/fb_$page.bin" "$OUT/$page.png" 2

	# 顺便把该页的启动日志片段打出来，页面里如果有 printf 能看到
	ssh $SSHOPT $BOARD 'grep -E "\[关于\]|\[网络\]|\[布局\]|警告" /tmp/lvgl_boot.log 2>/dev/null' || true

	rm -f "$OUT/fb_$page.bin"
	echo
done

echo "全部完成，图片在 $OUT/"
