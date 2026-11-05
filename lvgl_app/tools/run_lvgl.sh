#!/bin/sh
# run_lvgl.sh —— 用自己写的 lcd.ko + touch.ko 跑 LVGL
#
# 前置（一次装好）：
#   make load            # 编 + 传 + 装 lcd.ko 和 touch.ko
#   make deploy          # 只传不装
#
# 用法：
#   ./run_lvgl.sh            启动（停掉占 fb0 的进程，退出后【不】恢复 main）
#   ./run_lvgl.sh --check    只检查环境，不启动
#   ./run_lvgl.sh --restore  启动，退出后把 main 拉回来
#   ./run_lvgl.sh --start-main  只启动 main，不跑 LVGL
#
# 【这个脚本相对旧版改了什么】
#   旧版认 "fts" 开头的触摸设备名（原厂驱动），现在换成了自己写的
#   echo_my_touch，所以判断条件改成「看有没有 ABS_MT_POSITION_X」——
#   这是触摸设备的本质特征，不依赖某个具体驱动的名字。
#
#   旧版只找 /root/lvgl_full，现在优先找 /root/lvgl_app。
#
# 【为什么启动前要摘 fbcon】
#   fbcon 会持续往 /dev/fb0 上重绘内核 console 的文字，和 LVGL 抢同一块显存。
#   结果就是界面上一片乱码在闪。摘掉它（vtcon1，名字是
#   "(M) frame buffer device"）之后 fb0 才完全归 LVGL。
#   重启后 fbcon 会自动绑回来，所以每次进来都要重新摘。

BIN=""
for b in /root/lvgl_app /tmp/lvgl_app /root/lvgl_full /tmp/lvgl_full; do
	[ -x "$b" ] && { BIN=$b; break; }
done

CHECKONLY=0
KEEP=1			# 默认不恢复 main
START_MAIN_ONLY=0
for a in "$@"; do
	case "$a" in
	--check) CHECKONLY=1 ;;
	--restore) KEEP=0 ;;
	--start-main) START_MAIN_ONLY=1 ;;
	-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
	esac
done

say() { echo "[run_lvgl] $*"; }
die() { echo "[run_lvgl] 错误: $*" >&2; exit 1; }

# ---------- 0. 只启动 main ----------
if [ "$START_MAIN_ONLY" = "1" ]; then
	if [ -x /root/bin/start_main.sh ]; then
		say "启动 main（setsid 脱离本会话）"
		cd /root/bin && setsid sh ./start_main.sh </dev/null >>/root/bin/main.log 2>&1 &
		sleep 4
		for p in /proc/[0-9]*; do
			ls -l "$p/fd" 2>/dev/null | grep -q '/dev/fb0' && \
				say "已在运行 pid=${p#/proc/}"
		done
	else
		die "/root/bin/start_main.sh 不存在"
	fi
	exit 0
fi

# ---------- 1. framebuffer：必须是 lcd.ko 提供的 ----------
if [ ! -e /dev/fb0 ]; then
	die "/dev/fb0 不存在。先跑：cd kernel/my && make load"
fi

FBNAME=$(cat /sys/class/graphics/fb0/name 2>/dev/null)
FBSIZE=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)
say "fb0: name=$FBNAME  size=$FBSIZE"

# name 是驱动注册的 fix.id。lcd.c 没显式设 fix.id 时会是 fbtft 的样子，
# 所以这里不硬卡名字，只要有 320x240 就说明是我们的屏。
case "$FBSIZE" in
320,240) ;;
*) say "警告: fb0 尺寸是 $FBSIZE，不是 320,240 —— 可能不是我们的屏驱动" ;;
esac

# ---------- 2. 触摸：必须有 ABS_MT_POSITION_X ----------
# 不靠驱动名，靠能力位。ABS_MT_POSITION_X = 0x35 = 53，
# ARM32 上 unsigned long 是 32 位，所以落在第 1 个 long 的第 21 位。
# 这里用 evtest 式的笨办法：直接读 /sys 的 capabilities/abs 更简单。
TOUCH=""
TOUCH_NAME=""
for d in /dev/input/event*; do
	[ -e "$d" ] || continue
	nm=$(basename "$d")
	caps=$(cat /sys/class/input/$nm/device/capabilities/abs 2>/dev/null)
	[ -z "$caps" ] && continue
	# capabilities/abs 是一串十六进制，按 64 位一组从高到低排。
	# ABS_MT_POSITION_X(53) 在低 64 位的第 53-32=21 个十六进制位。
	# 与其猜位偏移，不如用 ioctl 让内核告诉我们——这里借助 python 更好。
	if command -v python3 >/dev/null 2>&1; then
		ok=$(python3 - "$d" <<'PY'
import sys, fcntl, struct
# EVIOCGBIT(0, len) = _IOC(_IOC_READ, 'E', 0x20 + 0, len)
# 但我们要的是 EV_ABS 的位图：EVIOCGBIT(EV_ABS=3, len)
def ioc(dir_, type_, nr, size):
    return (dir_ << 30) | (size << 16) | (ord(type_) << 8) | nr
EV_ABS = 3
try:
    fd = open(sys.argv[1], 'rb')
except OSError:
    print(0); sys.exit()
buf = bytearray(64)
req = ioc(2, 'E', 0x20 + EV_ABS, len(buf))
try:
    fcntl.ioctl(fd, req, buf, True)
except OSError:
    print(0); sys.exit()
def bit(code):
    return buf[code // 8] & (1 << (code % 8))
print(1 if bit(53) or bit(0) else 0)
PY
)
		[ "$ok" = "1" ] || continue
	fi
	TOUCH=$d
	TOUCH_NAME=$(cat /sys/class/input/$nm/device/name 2>/dev/null)
	break
done
[ -n "$TOUCH" ] || die "没找到带 ABS 轴的触摸设备。检查 touch.ko 是否 loaded（make status）"
say "触摸设备: $TOUCH ($TOUCH_NAME)"

# ---------- 3. 摘掉 fbcon ----------
for v in /sys/class/vtconsole/vtcon*; do
	[ -e "$v/bind" ] || continue
	case "$(cat $v/name 2>/dev/null)" in
	*frame\ buffer*)
		if [ "$(cat $v/bind)" = "1" ]; then
			echo 0 > "$v/bind" && say "已摘掉 fbcon ($v)"
		fi
		;;
	esac
done

# ---------- 4. 找占着 fb0 的进程 ----------
PIDS=""
VICTIM_CMD=""
for p in /proc/[0-9]*; do
	pid=${p#/proc/}
	ls -l "$p/fd" 2>/dev/null | grep -q '/dev/fb0' || continue
	cmd=$(cat "$p/cmdline" 2>/dev/null | tr '\0' ' ')
	[ -z "$cmd" ] && continue
	PIDS="$PIDS $pid"
	VICTIM_CMD=$cmd
	say "占用 fb0: pid=$pid  cmd=$cmd"
done

if [ "$CHECKONLY" = "1" ]; then
	say "（--check 模式，不启动）"
	exit 0
fi

# ---------- 5. 停掉占用者 ----------
if [ -n "$PIDS" ]; then
	for pid in $PIDS; do
		kill -9 "$pid" 2>/dev/null && say "已停止 pid=$pid"
	done
	sleep 1
fi

# ---------- 6. 恢复函数 ----------
restore() {
	[ "$KEEP" = "1" ] && return
	case "$VICTIM_CMD" in
	*main*)
		if [ -x /root/bin/start_main.sh ]; then
			say "恢复 main..."
			# busybox 的 setsid 没有 -f；裸 & 会被本脚本 exit 收掉。
			# 写个一次性脚本再 setsid 拉起它，独占新会话。
			printf '#!/bin/sh\nsleep 1\ncd /root/bin\nexec sh ./start_main.sh >>/root/bin/main.log 2>&1\n' \
				> /tmp/_restore_main.sh
			chmod +x /tmp/_restore_main.sh
			setsid /tmp/_restore_main.sh </dev/null &
			sleep 4
		fi
		;;
	esac
}
trap 'restore; exit 0' INT TERM

# ---------- 7. 启动 ----------
[ -n "$BIN" ] || die "找不到 lvgl_app，请先 scp 到 /root/（lvgl_app/build/lvgl_app）"
say "启动 $BIN"
echo "----------------------------------------"
"$BIN"
rc=$?
echo "----------------------------------------"
say "LVGL 退出，返回码=$rc"
restore
exit $rc
