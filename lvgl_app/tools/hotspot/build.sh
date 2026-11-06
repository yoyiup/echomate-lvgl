#!/bin/sh
# Build board binaries from the RV1106 SDK without writing into the SDK tree.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDK_ROOT=${SDK_ROOT:-/home/tao/Echo-Mate/SDK/rv1106-sdk}
WORK=${WORK:-/tmp/echomate-hotspot-build}
OUT=${OUT:-$WORK/out}
CROSS=$SDK_ROOT/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
WIFI=$SDK_ROOT/project/app/wifi_app

mkdir -p "$WORK" "$OUT"
if [ ! -d "$WORK/libnl-3.4.0" ]; then
	tar -xzf "$WIFI/libnl-3.4.0.tar.gz" -C "$WORK"
fi
if [ ! -f "$WORK/libnl-install/lib/libnl-3.so" ]; then
	(cd "$WORK/libnl-3.4.0" && \
		./configure --host=arm-rockchip830-linux-uclibcgnueabihf \
		--prefix="$WORK/libnl-install" CC="$CROSS-gcc" \
		--disable-cli --disable-documentation && make -j4 && make install)
fi
if [ ! -d "$WORK/hostapd-2.6" ]; then
	cp -a "$WIFI/hostapd-2.6" "$WORK/hostapd-2.6"
fi
PKG_CONFIG_PATH="$WORK/libnl-install/lib/pkgconfig" \
	CFLAGS="-Os -I$WORK/libnl-install/include/libnl3" \
	LDFLAGS="-L$WORK/libnl-install/lib" CC="$CROSS-gcc" \
	make -C "$WORK/hostapd-2.6/hostapd" -j4
cp "$WORK/hostapd-2.6/hostapd/hostapd" "$OUT/echomate-hostapd-nl80211"
"$CROSS-strip" -s "$OUT/echomate-hostapd-nl80211"
cp "$WIFI/precompile/arm-rockchip830-linux-uclibcgnueabihf/bin/dnsmasq" \
	"$OUT/echomate-dnsmasq"
cp "$WIFI/precompile/arm-rockchip830-linux-uclibcgnueabihf/lib/libintl.so.8" \
	"$OUT/libintl.so.8"
"$CROSS-g++" -std=c++17 -Os -Wall -Wextra "$HERE/transfer_server.cpp" \
	-o "$OUT/echomate-transfer"
"$CROSS-strip" -s "$OUT/echomate-transfer"
cp "$HERE/hotspot.sh" "$OUT/echomate-hotspot.sh"
chmod +x "$OUT/echomate-hotspot.sh"
echo "板端文件位于 $OUT"
