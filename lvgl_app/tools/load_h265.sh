#!/bin/sh
# 解开并装载 RV1106 硬件 H.265 编码运行环境。
# 大文件以压缩包留在 rootfs，启动时展开到 tmpfs，节省约 1.3MB 闪存。

PACK=/root/bin/cam_codec.tar.gz
OUT=/tmp/cam_codec

[ -f "$PACK" ] || {
	echo "缺少 $PACK"
	exit 1
}

mkdir -p "$OUT" || exit 1
if [ ! -f "$OUT/librockit.so" ]; then
	tar -xzf "$PACK" -C "$OUT" || exit 1
fi

# librockit 的 DT_NEEDED 使用库名查找；固定链接让 dlopen 不依赖调用进程
# 启动时的 LD_LIBRARY_PATH。
ln -sf "$OUT/librockchip_mpp.so.0" /usr/lib/librockchip_mpp.so.1
ln -sf "$OUT/librga.so" /usr/lib/librga.so

[ -d /sys/module/rga3 ] || insmod "$OUT/rga3.ko" || exit 1
[ -d /sys/module/mpp_vcodec ] || insmod "$OUT/mpp_vcodec.ko" || exit 1
[ -c /dev/mpi/valloc ] ||
	insmod "$OUT/rockit.ko" mcu_fw_path="$OUT/hpmcu_wrap.bin" \
		mcu_fw_addr=0xff6ff000 isp_max_h=240 || exit 1

[ -c /dev/mpp_service ] && [ -c /dev/mpi/valloc ]
