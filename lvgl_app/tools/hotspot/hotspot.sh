#!/bin/sh
# EchoMate Wi-Fi AP + DHCP + phone browser file exchange.
set -eu

BIN=/root/bin
AP_IP=192.168.50.1
SSID=EchoMate-48B4
PASS_FILE=/root/.echomate-hotspot-password
AP_CONF=/tmp/echomate-hotspot.conf
AP_PID=/tmp/echomate-hostapd.pid
DHCP_PID=/tmp/echomate-dnsmasq.pid
WEB_PID=/tmp/echomate-transfer.pid

stop_process() {
	[ -f "$1" ] || return 0
	pid=$(cat "$1")
	case "$pid" in *[!0-9]*|'') ;; *) kill "$pid" 2>/dev/null || true ;; esac
	rm -f "$1"
}

stop_ap() {
	stop_process "$WEB_PID"
	stop_process "$DHCP_PID"
	stop_process "$AP_PID"
	sleep 1
	ifconfig wlan0 0.0.0.0 up
}

restore_sta() {
	if ! pidof wpa_supplicant >/dev/null 2>&1; then
		wpa_supplicant -B -c /userdata/wpa_supplicant.conf -i wlan0
	fi
	udhcpc -i wlan0 -n -t 3 -T 2 -q >/tmp/echomate-wifi-restore.log 2>&1 || true
}

start_ap() {
	for program in "$BIN/echomate-hostapd-nl80211" "$BIN/echomate-dnsmasq" \
		"$BIN/libintl.so.8" "$BIN/echomate-transfer"; do
		[ -f "$program" ] || { echo "缺少 $program"; exit 1; }
	done
	if [ -f "$AP_PID" ] && kill -0 "$(cat "$AP_PID")" 2>/dev/null; then
		echo "热点已经运行"; status; return
	fi
	umask 077
	if [ ! -s "$PASS_FILE" ]; then
		od -An -N8 -tx1 /dev/urandom | tr -d ' \n' > "$PASS_FILE"
	fi
	password=$(cat "$PASS_FILE")
	cat > "$AP_CONF" <<EOF
interface=wlan0
driver=nl80211
ssid=$SSID
hw_mode=g
channel=6
wpa=2
wpa_passphrase=$password
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOF
	# Single-radio board: use wlan0 for AP, temporarily leave the home Wi-Fi.
	wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true
	sleep 1
	ifconfig wlan0 "$AP_IP" netmask 255.255.255.0 up
	if ! "$BIN/echomate-hostapd-nl80211" -B -P "$AP_PID" \
		-f /tmp/echomate-hostapd.log "$AP_CONF"; then
		stop_ap; restore_sta; echo "热点启动失败，已恢复原 Wi-Fi"; exit 1
	fi
	sleep 1
	if ! iw dev wlan0 info | grep -q 'type AP'; then
		stop_ap; restore_sta; echo "无线网卡未进入 AP 模式"; exit 1
	fi
	if ! LD_LIBRARY_PATH="$BIN" "$BIN/echomate-dnsmasq" --port=0 \
		--interface=wlan0 --bind-interfaces \
		--dhcp-range=192.168.50.10,192.168.50.50,255.255.255.0,12h \
		--dhcp-option=3,"$AP_IP" --pid-file="$DHCP_PID" \
		--log-facility=/tmp/echomate-dnsmasq.log; then
		stop_ap; restore_sta; echo "DHCP 启动失败，已恢复原 Wi-Fi"; exit 1
	fi
	setsid "$BIN/echomate-transfer" "$AP_IP" 8080 \
		>/tmp/echomate-transfer.log 2>&1 </dev/null &
	echo $! > "$WEB_PID"
	sleep 1
	if ! kill -0 "$(cat "$WEB_PID")" 2>/dev/null; then
		stop_ap; restore_sta; echo "文件服务启动失败，已恢复原 Wi-Fi"; exit 1
	fi
	status
}

status() {
	if iw dev wlan0 info | grep -q 'type AP'; then
		echo "热点：$SSID"
		echo "密码：$(cat "$PASS_FILE")"
		echo "手机连接后打开：http://$AP_IP:8080"
		echo "上传保存在 /tmp/phone_upload；相机文件可从网页下载"
	else
		echo "热点未运行"
	fi
}

case "${1:-}" in
	start) start_ap ;;
	stop) stop_ap; restore_sta; echo "已关闭热点并恢复原 Wi-Fi" ;;
	status) status ;;
	*) echo "用法：$0 start|stop|status"; exit 2 ;;
esac
