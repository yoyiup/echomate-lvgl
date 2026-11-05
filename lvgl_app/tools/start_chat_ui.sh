#!/bin/sh
# =====================================================================
#  start_chat_ui.sh —— 启动带真语音对话链路的 LVGL 界面
#
#  板上路径：/root/start_chat_ui.sh
#  用法：  /root/start_chat_ui.sh            （前台跑，能看日志）
#          /root/start_chat_ui.sh -d         （后台跑，日志 /tmp/lvgl_chat.log）
#          LVGL_RUN_SEC=60 /root/start_chat_ui.sh   （跑 60 秒自退）
#
#  【为什么必须有这个脚本，而不是直接 ./lvgl_app】
#  有三个前置条件，少一个就"喊不醒"，而且全都不报错：
#
#   1. 麦克风增益。开机脚本 S99start_echo_defconfig 会配，但手动重启
#      程序时不会跑那段 —— 增益掉回默认，底噪从 80 掉到 20，喊死都不醒。
#
#   2. ★ 下混丢 6dB 这件事，决定了增益该怎么配。
#      实测：本板 codec 只有【左声道】有信号，右声道恒为 0：
#          arecord -c 2  →  L rms=666   R rms=0.0
#      而 /etc/asound.conf 把 default 定义成 `type plug / slave.pcm hw:0,0`，
#      PortAudio 请求 16k/1ch 时，plug 的 2ch→1ch 是 (L+R)/2，
#      右声道为 0 就等于【幅度直接砍半，白丢 6dB】。
#      实测验证：L=57.3 时下混恰好 28.7，就是精确的一半。
#
#      这个 6dB 要不要补，**取决于用哪种唤醒方式**：
#        · 能量兜底（SNOWBOY_WAKE_RMS>0）：比的是【绝对幅度】，
#          要过 400 的门槛就必须补 → ADC Digital Volume 用 223(+14dB)。
#        · 只用唤醒词（RMS=0，当前默认）：snowboy 是【波形模式匹配】，
#          看的是相对形状，不吃绝对幅度 → 补了没用，只会放大噪声。
#          实测 223 + gain2.0 + sens0.7 时【一分钟误唤醒一次】，
#          回 211 + gain1.0 + sens0.4 后才正常。
#      现在脚本用 211（出厂值）。若改回能量兜底，记得同时把这里调回 223。
#
#   3. cwd 必须是 /root/bin。AIChat Demo 里 snowboy 的模型路径是【相对路径】
#      (third_party/snowboy/resources/common.res)，cwd 不对就永远检测不到，
#      而且不报任何错 —— 只是安静地失效。
#
#  【唤醒阈值 —— 为什么最终用"能量"而不是"唤醒词"】
#  原设计是 snowboy 认 "echo" 唤醒词 + 能量兜底双保险。实测下来
#  **snowboy 这条路在本板上不可用**，证据链：
#      rms 586  sens0.7 gain2.0  → 命中过 1 次
#      rms 363  sens0.7 gain2.0  → 0 次
#      rms 739  sens0.7 gain3.0  → 0 次  ← 比成功那次还响，仍认不出
#  而它又会把环境噪音误判成 echo（实测静默时每分钟误醒 1 次）。
#  即"该醒不醒、不该醒乱醒"。原因：echo.pmdl 是英文母语者训练的模型，
#  中文口音特征对不上；且 snowboy 官方训练服务 2020-12 已关，训练不出新词。
#  板上也只有这一个 pmdl，没有备选。
#
#  所以现在**以能量兜底为主**，snowboy 保留但灵敏度压到 0.3（基本不干扰）。
#
#    SNOWBOY_WAKE_RMS   能量阈值。**默认 600**，按实测底噪/说话/喊叫三档定的：
#                         底噪 50~60   普通说话 100~300   大声喊 700+
#                       600 卡在中间 → 大声喊才醒，说话和噪音不醒。
#                       调低更灵敏但更容易被噪音误触；设 0 = 完全关闭能量兜底。
#    SNOWBOY_WAKE_FRAMES 连续几帧超阈值才算（默认 2，抗突发磕碰声）
#    SNOWBOY_SENSITIVITY 唤醒词灵敏度 0~1。默认压到 0.3：
#                       调高（0.6~0.7）会频繁误唤醒，调低（0.4~0.55）则完全认不出
#                       —— 中间没有可用区间，所以不用它做主判据。
#    SNOWBOY_AUDIO_GAIN  snowboy 内部信号增益（原厂建议 2~4）。这是【灵敏度】类参数，
#                       不是"放大噪声"：降到 1.0 后任何 sensitivity 都达不到检测门槛。
#
#  【依赖】网关必须先起来（VM 侧）：tools/aichat_cloud/gateway.py --port 8000
#          连不上会进 fault 态并重试 3 次，之后要重启本程序。
# =====================================================================

BIN=/root/lvgl_app
LOG=/tmp/lvgl_chat.log
WORKDIR=/root/bin

set_audio_config() {
	amixer -c 0 cset name='ADC HPF Cut-off' 1            >/dev/null 2>&1
	amixer -c 0 cset name='ADC MIC Right Switch' 1       >/dev/null 2>&1
	# ★ 用出厂值 211(+8dB)。曾经为了能量兜底抬到 223 补 6dB 下混损失，
	#   但能量兜底关闭后那 6dB 只剩副作用：snowboy 是【波形模式匹配】，
	#   只看相对形状、不需要绝对幅度，额外增益只会放大噪声、
	#   让它更早误触发（实测 223 + gain2.0 + sens0.7 时一分钟误醒一次）。
	amixer -c 0 cset name='ADC Digital Left Volume' 223  >/dev/null 2>&1
	amixer -c 0 cset name='ADC Digital Right Volume' 223 >/dev/null 2>&1
	amixer -c 0 cset name='ADC MIC Left Gain' 3          >/dev/null 2>&1
	amixer -c 0 cset name='ADC ALC Left Volume' 16       >/dev/null 2>&1
	amixer -c 0 cset name='DAC LINEOUT Volume' 25        >/dev/null 2>&1
}

# 停掉正在跑的和抢 fb0 的
stop_all() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		case "$(cat $p/comm 2>/dev/null)" in
		lvgl_app|main) kill -9 "$pid" 2>/dev/null ;;
		esac
	done
	sleep 1
}

stop_all
set_audio_config

# 摘 fbcon，否则内核 console 会盖在界面上面
for v in /sys/class/vtconsole/vtcon*; do
	[ -e "$v/name" ] || continue
	case "$(cat $v/name 2>/dev/null)" in
	*frame\ buffer*) [ "$(cat $v/bind 2>/dev/null)" = "1" ] && echo 0 > "$v/bind" 2>/dev/null ;;
	esac
done

cd "$WORKDIR" || { echo "进不去 $WORKDIR"; exit 1; }

# 能量兜底默认关闭（0）：只认唤醒词 echo.pmdl，不会被环境噪音误唤醒。
# 现在默认开（600），所以 `idle mic rms=` 日志会正常打印，可随时看底噪。

export SNOWBOY_WAKE_RMS=${SNOWBOY_WAKE_RMS:-600}
export SNOWBOY_WAKE_FRAMES=${SNOWBOY_WAKE_FRAMES:-2}
export SNOWBOY_SENSITIVITY=${SNOWBOY_SENSITIVITY:-0.3}
export SNOWBOY_AUDIO_GAIN=${SNOWBOY_AUDIO_GAIN:-2.0}

if [ "$1" = "-d" ]; then
	rm -f "$LOG"
	LVGL_RUN_SEC=${LVGL_RUN_SEC:-0} setsid "$BIN" > "$LOG" 2>&1 < /dev/null &
	echo "已后台启动，日志：$LOG"
	if [ "$SNOWBOY_WAKE_RMS" = "0" ]; then
		echo "唤醒方式：只认唤醒词（能量兜底已关闭）  灵敏度=${SNOWBOY_SENSITIVITY}"
	else
		echo "唤醒方式：唤醒词 + 能量兜底（RMS>=${SNOWBOY_WAKE_RMS} 连续${SNOWBOY_WAKE_FRAMES}帧）  灵敏度=${SNOWBOY_SENSITIVITY}"
	fi
else
	exec "$BIN"
fi
