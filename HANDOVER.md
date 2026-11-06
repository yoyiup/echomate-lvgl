# Echo-Mate 项目交接文档

> 最后更新：2026-11-06 · 代码仓库：https://github.com/yoyiup/echomate-lvgl
>
> 本文面向**接手这块板子的人**：先讲清楚东西在哪、怎么跑起来，再讲架构约定和踩过的坑。
> 只想要最小路径的，直接看第 0 节；要排查问题的，跳到第 7 节。

---

## 0. 五分钟上手清单

1. 板子通过 USB RNDIS 连上宿主，宿主侧 `usb0` 应自动拿到 `172.32.0.1/24`，**先 `ping 172.32.0.93`**。
2. SSH 登录（命令见 2.2）。
3. `pidof lvgl_app` + `/root/bin/lvgl_daemon.sh status` —— **屏幕看着"死了"先做这一步，别急着重启**。
4. 看日志：`/root/bin/lvgl_daemon.sh log`，开机日志在 `/tmp/lvgl_boot.log`。
5. 想重新部署：按 4.2 的顺序做，一步都不能省。
6. 想看界面：`tools/fb2png.py` 抓屏；相机帧必须带 `--wh 640 480`。

---

## 1. 项目是什么 / 东西都在哪

RV1106 上一台 320×240 小屏的桌面机器人，跑精简 Buildroot + uClibc（**内存 54MB**），
不是 Android 也不是桌面发行版。所有代码都是"在资源极紧的前提下把功能做实"的产物。

### ★ 两个工作副本，别搞混

| 路径 | 内容 | 用途 |
|---|---|---|
| `/home/tao/Echo-Mate-from0/` | `kernel/`、`lvgl_app/`、`app/`、`docs/`、`tools/`、`interview/`、`board_backup/`、`out/` | **实际教学/开发工程**，本文件相对路径默认以它为根 |
| `/home/tao/Echo-Mate/`（IDE workspace） | `SDK/`、`Demo/`、`tools/`、`kernel/usb_acm` | 芯片原厂 SDK 与 Demo，以及 `.workbuddy/memory/` 专题笔记 |

### Git 仓库

- 本地：`/home/tao/Echo-Mate-from0/echomate-lvgl/`，提交 `c38e417`（62 → 60 个文件，约 10.8MB）。
- 远端：`yoyiup/echomate-lvgl`（public，默认分支 `main`），推送后为 `709188c3`（初始化）+ `76c871e2`（同步源码）。
  ⚠️ 远端历史是在空仓库上独立播种出来的，**与本地 `c38e417` 不同源但文件内容一致**；
  继续用本地仓库往远端推之前，先做一次内容比对，别直接 `git push`（会 non-fast-forward）。
- 仓库里只有 **`driver/sc3336.c`** 和 **`lvgl_app/`**；已排除 `build/`、截图、LVGL 源码、
  以及两个误入的 x86 可执行测试程序（`block_test`、`wpa_psk_test`，源码 `.c` 仍在）。
- **未入库、需要自己准备**：LVGL 9.2.3 源码（放 `lvgl_app/lvgl/`）、rkaiq / librknnmrt、
  Rockchip ARM 工具链、中文字体源 TTF。
- `lvgl_app/src/fonts/lv_font_cn_16.c` 是 10.2MB 的**生成文件**（LVGL 字体转换工具从
  Noto Sans SC 提取），保留它是为了 clone 下来能直接编；要重新生成用 `lvgl_app/tools/gen_font.sh`
  与 `extract_ttc.py`。

---

## 2. 硬件与板子环境

### 2.1 硬件

| 项 | 型号 / 参数 |
|---|---|
| SoC | Rockchip RV1106G3（单核 Cortex-A7 + 0.5T NPU） |
| 屏幕 | 320×240 SPI LCD（ST7789V），RGB565 |
| 摄像头 | SC3336（自写 V4L2 subdev 驱动，走 rkisp / rkaiq） |
| WiFi | RTL8723BS，2.4G 单频 |
| 电机 | 两路 H 桥 GPIO 直驱 |
| 音频 | 片内 acodec，16kHz，只有左声道有麦 |

### 2.2 连接与环境约束

```sh
ssh -i /home/tao/.ssh/id_ed25519 -o UserKnownHostsFile=/dev/null \
    -o StrictHostKeyChecking=no root@172.32.0.93
```

- **reboot / 宿主重建后 USB RNDIS 必掉**，需要人工插拔；插拔后 VM 侧 `usb0` 自动配 `172.32.0.1/24`。
- busybox 是精简版：**没有 pkill / timeout / nc / stat / getent / curl**。
  ⚠️ `timeout <cmd>` 会静默不执行，**绝对不要用它包命令**（会误判成程序没反应）。
- 内存 54MB，rootfs 30.2M（ubifs），`/tmp` 27MB（tmpfs），uClibc 1.0.31。
- 无 `/proc/config.gz`，`ldd --version` 不可用；**`/proc/kallsyms` 不含数据符号**（未开 `CONFIG_KALLSYMS_ALL`）。
- SPI 有效速率约为名义值的 **57%**（实测 4.28MB/s），整屏 150KB 约 28.1ms；
  想量带宽就读两次 `/sys/module/lcd/parameters/stats` 做差。
- 语音网关在宿主后台跑 `tools/aichat_cloud/gateway.py`（端口 8024），
  阿里云 key **只从 `.workbuddy/tmp/aliyun.key` 注入环境变量**，不要写进代码或仓库。

---

## 3. 代码结构与架构约定

```
lvgl_app/
├── CMakeLists.txt / lv_conf.h / conf/dev_conf.h
├── tools/                 # 抓屏、字体生成、板端运行脚本
└── src/
    ├── main.c             # 入口：LVGL 初始化、页面装配、主循环、flock 单实例
    ├── ui/                # 界面层（纯 C，8+ 页面，只做接线）
    ├── cam/               # V4L2 采集（C++ 类）+ 图像格式转换
    ├── yolo/              # RKNN 推理
    ├── aichat/            # 语音链路接线（snowboy 唤醒 + 对话）
    ├── motor/  bright/  avatar/
    ├── wifi/  net/       # WiFi、天气、NTP、连通性检查
    ├── common/resources.hpp  # RAII：Fd/File/Pipe/Mmap/AddrInfo
    └── fonts/             # 生成的中文字形
```

### 二次开发必须遵守的约定

1. **语言分工**：Linux 应用层尽量 C++；`src/main.c` + `src/ui/*.c` 留 C（LVGL 接线层）。
   UI 层里零 `fopen` / socket / `pthread_create`，所有 IO 与并发在 C++ 模块。
2. **C++ 模块只暴露一张函数表**：`.h` 里定义 `struct xxx_ops`（全是函数指针）+
   `const struct xxx_ops *xxx_api(void);`，整个模块只有一个 C 符号（`motor`、`brightness` 已按此改造）。
   `extern "C"` **只在头文件写一次**，定义处再写是冗余。
3. **不抛异常**（uClibc + C 混编，异常穿越 C 边界是 UB），用返回码 + `last_error()`；资源用 RAII。
4. **状态用快照，不用回调**：业务线程只写 `std::atomic` 快照，UI 用 `lv_timer` 定时拉。
   跨线程直接调 `lv_obj_*` 会随机花屏/崩溃。
5. **单实例**：`main.c` 用 `flock("/var/run/lvgl_app.lock")`。两个实例同时开 `/dev/fb0`
   会轮流覆盖显存，画面在两个页面间跳，看起来像 UI 乱刷。
6. **同一个概念只允许一个定义**（预览尺寸曾 `PV_H=160` vs `CAM_PV_H=240`，每帧越界写 50KB）。
   给 `memcpy` 类接口加 `dst_bytes` 校验。
7. **LVGL 事件 `user_data` 的生命周期必须比控件长**；`textarea_get_text()` 指针在 delete 后失效，先拷贝。

### C++ 改造时最容易踩的 6 个坑

`goto` 不能跨过带非平凡析构的变量初始化（声明提到函数最前）；头文件要自己 `#include` 用到的东西；
`const` 成员函数锁 mutex 时成员必须 `mutable`；`std::string` 不能当 `const char*`（用
`xxx_holder.c_str()`，`empty()` 给 nullptr）；列表初始化 `uint8_t b[4]={v&0xff,...}` 有窄化要显式
`static_cast`；**`Pipe` 必须 `pclose`**（`fclose` 会让子进程变僵尸），`Mmap` 长度要和指针一起存。

---

## 4. 构建与部署 SOP

### 4.1 编译

工具链在 `SDK/rv1106-sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin`
（在 workspace 副本下），需要 `arm-rockchip830-linux-uclibcgnueabihf-gcc 8.3.0`。

```sh
cd lvgl_app
mkdir -p build && cd build
export TMPDIR=$PWD/.tmp_build        # 容器/板子 /tmp 小，中间文件放这里
cmake .. -DCMAKE_TOOLCHAIN_FILE=<你的工具链文件>
make -j12                            # 宿主；在板上必须 -j1，否则 OOM 被 SIGKILL(137)
```

- **不要加全局 `include_directories`**（会让 123 个目标全部重编），用
  `set_source_files_properties(... INCLUDE_DIRECTORIES ...)` 精确加。
- 内核编译要**沙箱外 + 干净 PATH**；**别整树刷固件**，只换 fdt：`kernel/my/dtb/flash_dt.sh`；
  看设备树一律用 `tools/dt2dts.py`。
- 后台编译命令**不要接 `tail`/`grep`**（等 EOF 会误判卡住），判断进度看 `.o` 的时间戳。

### 4.2 部署顺序（顺序错了会静默失败）

```sh
# 板上
/root/bin/lvgl_daemon.sh stop
pidof lvgl_app                 # 确认没有残留进程
: > /root/lvgl_app             # ★ 先截断：rootfs 满时 mv 会失败，但程序照旧跑旧二进制
# 宿主
scp lvgl_app/build/lvgl_app root@172.32.0.93:/root/lvgl_app
# 板上，必须核对字节数
wc -c /root/lvgl_app           # 与宿主 `wc -c` 对比，不一致就是没覆盖成功
/root/bin/lvgl_daemon.sh start
```

> ⚠️ 不先 stop 会 `Text file busy`；不先 truncate + 不核对字节，会出现"日志一切正常但跑的是旧二进制"。

守护脚本 `/root/bin/lvgl_daemon.sh`：`start | stop | restart | status | uptime | log`。
`setsid` 脱离 ssh，进程退出后 sleep 2 拉起，日志自轮转；启动前会摘 fbcon、归零 ctrl、
`load_camera()`。`LVGL_RUN_SEC` 默认 0（常驻），限时调试必须显式传 N。

### 4.3 开机自启

- `main` 应用**不会**自己开机自启；靠 `/etc/init.d/S99start_lvgl`。
- busybox init 按**文件名字典序**执行 `/etc/init.d/S*`，所以必须叫 `S99...`
  （`start_l` > `start_e`）；**任何 `S*` 都会被当脚本执行**，备份放 `/root/init.d-backup/`。
- `S99start_echo_defconfig`：优先 `lcd.ko` + `touch.ko`，fb0 出不来回退 `echo_lcd.ko`。
- `S99start_lvgl`：摘 fbcon → kill 占 fb0 的进程 → 起 daemon，日志 `/tmp/lvgl_boot.log`。
- 调试开关：`touch /root/.no_lvgl` 可跳过自启。
- 实测重启零介入：`lsmod` = touch+lcd、`event1` = `echo_my_touch`、开机 16.6s。

---

## 5. 模块现状与开关

| 模块 | 入口 / 设备 | 关键开关与注意 |
|---|---|---|
| 相机 | `/dev/video*`、`rkaiq` 3A | 驱动**不开机自启**，靠 `load_camera()`；界面报"打开失败"先 `ls /dev/video*`。页面提供 AVI 与 H265 两种录像：H265 使用 RV1106 VENC 硬编，停止后由 ffmpeg 无重编码封装 MP4；首次使用会运行 `/root/bin/load_h265.sh`。实测 3A 是 2A（AE+AWB，定焦无 AF），9 线程；`CAM_AIQ=off` 对照：开亮度 113.6 正常色、关 41.7 发绿 |
| YOLO | `rknpu.ko` + `librknnmrt.so` + `yolov5.rknn` | **板上原本没有 NPU 驱动**，SDK 里 `CONFIG_ROCKCHIP_RKNPU=m` 编出 `.ko` 直接 insmod；**没加自启，重启后要手动 insmod**。只能零拷贝，输入 UINT8，量化公式 `q = f32/scale + zp`（`zp=-128, scale=1/255`），搞反一个框都检不出 |
| 电机 | `/dev/echo_motor`，GPIO 32/33(左)、35/36(右) | ioctl 只有 SET/GET/STOP_ALL，语义是"设方向，写一次一直转"，**离开页面必须停**。命令行 `tools/motor_test` 比点屏可靠 |
| 音频 | 片内 acodec (`card 0: rvacodec`) | 不用自己写驱动；硬件只支持 **2/4 声道**，`-c 1` 报错，**右声道恒为 0**；功放 `GPIO0_A2 ACTIVE_LOW` 驱动自动管 |
| 天气 | `t.weather.itboy.net` | 源站会长时间停更但会自恢复，`max-age=3000`（CDN 缓存 50 分钟）；已实现 `update_epoch` + `WX_STALE_SEC=3h` 变橙加 `!`。**判新旧用 `mktime()`，不要手动 +8**（进程内 `TZ=CST-8` 已生效） |
| WiFi | `wlan0` / `wpa_supplicant` | 三层分别验：关联 / IP / 路由+ARP（`default` 指向 wlan0 **且**网关在 `/proc/net/arp` flags 含 0x2）。`udhcpc -R` 是"退出时释放 IP"**不是**清旧地址，清旧地址只能 `ifconfig wlan0 0.0.0.0`。配置路径统一 `/userdata/wpa_supplicant.conf`；扫频必须异步（`iw scan` 1.7~2.1s），传 psk 绕开 shell（fork+execvp） |
| 语音 | snowboy + PortAudio + WebSocket 网关 | 灵敏度 `SNOWBOY_SENSITIVITY=0.65 / GAIN=1.2 / RMS=0`（关掉能量兜底，否则不喊也响）；`VAD_SILENCE_MS=1200`；有「唤醒」按钮兜底 `force_aichat_wake()`；调试开关 `AICHAT_AUTOWAKE`、`LVGL_DEBUG_LABEL=1`；调试直达 `LVGL_START_PAGE=xxx` |
| 背光 | `/sys/class/backlight/backlight` | `BRIGHT_MIN=10`，`bright_set()` 钳位，进页面自愈 |
| 桌面形象 | `/userdata/avatar.id` | 4 个程序化绘制角色，按语音状态变表情（空闲浮动/聆听/思考/说话），点击有眯眼+张嘴+蹦的反馈；眨眼用 130ms 一次性定时器（曾误用 2.2s 看着像没睡醒） |

### 高频重试函数必须在失败路径还资源（OOM 级教训）

相机页曾"打开失败"后几十秒整个 app 被 OOM 杀：20ms 一次的 `poll_cb` 无条件 `cam_start()`，
开头 malloc 2×200KB，`v4l2_init()` 失败直接 return，指针又被下次 `memset(&g,0)` 覆盖 →
每次漏 200KB ≈ 10MB/s。修法是统一 `cam_start_cleanup()` 接住全部失败路径、streamon 失败额外
`aiq_stop()`、重试限速 `RETRY_MS=2000`。**这类 bug 必须量化**：
`tools/cam_test/cam_leak_test.c` 量 `/proc/self/statm`，1000 次增量 196KB（修复前每次 200KB，差 1000 倍）。

---

## 6. LVGL 9 注意事项

- **惰性布局**：`lv_obj_get_width/height()` 首次绘制前返回 0，先 `lv_obj_update_layout()`。
- 符号字体显示空白 → 用 `lv_font_t.fallback` 补 `lv_font_montserrat_14`；`lv_layer_top()` 无 display 时返回 NULL。
- 切页动画在 SPI 带宽下跑不动 → `NAV_ANIM_MS=0`；`LV_OBJ_FLAG_SCROLLABLE` 是能滚的总开关。
- `translate_y` 不带动子对象 → 改用内层 container + `lv_obj_set_y`。
- 抓屏 `tools/fb2png.py`，**相机帧必须 `--wh 640 480`**，否则出来的图是错的。

---

## 7. 排查手册：先看"假象"再看代码

| 现象 | 真正原因 / 第一步 |
|---|---|
| 屏幕冻住、以为板子卡死 | **app 退出后 fbcon 已被解绑，没人重绘，画面停在最后一帧**；板子其实活着（SSH 通、load 0.02）。先 `pidof lvgl_app` + `lvgl_daemon.sh status`，别重启 |
| 画面在两个页面间来回跳 | 两个 `lvgl_app` 实例抢 fb0。查 `pidof` 是否多于一个，或 `for p in /proc/[0-9]*; do ls -l $p/fd\|grep fb0; done` |
| 改了代码但行为没变 | rootfs 满时 `mv`/scp 覆盖失败，程序跑的还是旧二进制，**日志完全正常**。核对字节数 |
| 抓屏 0 字节 / v4l2 `bytesused=0` | `/tmp` 满了。**测大文件前先 `df -h /tmp`**；`/tmp/cam/` 是 tmpfs，重启即丢，照片视频要及时拉回宿主 |
| 相机卡顿、帧率跳动、应用反复被杀 | 先 `df -h /tmp` 和 `dmesg | tail`。2026-11-06 实测多个未压缩 AVI 占满 27MB tmpfs，内核 OOM 杀 `lvgl_app`（rc=137）；现有 AVI 写入留 4MB 安全余量。备份媒体并核对校验和后再清理板上副本。 |
| 摄像头"打开失败" | 先看 `ls /dev/video*`，驱动没自启；再看 `docs/D10-3摄像头问题排查记录.md` 第 7 节的 6 个"代码对、环境错"假象 |
| 连着 WiFi 却上不了网 | 关联 ≠ 能上网。三层分别验；最常见是旧网段 IP 残留导致 DHCP 被跳过 |
| 喊不醒 / 没喊也响 | 灵敏度与能量兜底；用「唤醒」按钮和 `AICHAT_AUTOWAKE` 定位，量化 rms 判断是不是电平问题 |
| `timeout xxx` 没输出 | busybox 的 `timeout` 会静默不执行，禁用 |

---

## 8. 已知问题 / 待办

1. `rknpu.ko` 与 camera sensor 驱动**都没有开机自启**，重启后需手动加载。
2. 语音网关仍依赖宿主后台进程 + 外部注入的 key，不是独立可交付服务。
3. 10.2MB 中文字库是生成文件，体积大且源字体有授权问题；长期应改成运行时生成或子集化。
4. 天气数据源单一且会停更，备选源目前全废。
5. GitHub **连接器只有读权限**（写操作一律 `403 Resource not accessible by integration`），
   同步代码需要 PAT；另注意 GitHub 空仓库连 `git/blobs` 都返回 409，必须先用
   `contents` 接口播种一个初始提交，之后才能用 Git Data API 批量推文件。
6. H.265 运行组件在板上以 `/root/bin/cam_codec.tar.gz` 压缩保存（约 884KB），首次录像
   展开到 `/tmp/cam_codec` 并加载 `rga3/mpp_vcodec/rockit`。重新刷 rootfs 后需同步部署该压缩包；
   原始文件来自 SDK 的 `media/out/lib` 与 `sysdrv/out/kernel_drv_ko`。板载 BusyBox `tar`
   不支持 `-z`，装载脚本使用 `gzip -dc | tar -xf -`；更新脚本后也要同步部署到板上。

---

## 9. 资料索引

工程内 `docs/`：

- `D10-3摄像头问题排查记录.md`（1145 行 / 30 个问题，五层分类 + 决策树，**动摄像头前先读**）
- `D10-1` / `D10-2` 摄像头与 V4L2 subdev 实验操作单、`D4` 中断、`D6` 设备树、`D9-1` poll
- `板上速查.md`、`Linux命令行速查.md`、`集成到LVGL.md`、`应用层七课.md`
- `唤醒引擎选型评估.md`、`面试-唤醒SDK软件设计.md`、`面试代码导航.md`、`全链路思维导图.md`

**专题笔记不在仓库里**，在 workspace 的 `.workbuddy/memory/TOPIC-*.md`：
`camera` / `device` / `kernel` / `lvgl` / `motor` / `net` / `voicechat` / `yolo`，
以及每日工作日志 `YYYY-MM-DD.md`。这些是踩坑的第一手记录，比代码注释更全。

板端调试工具在 `tools/`：`cam_test`(含 `cam_leak_test.c`)、`motor_test`、`yolo_test`、
`aiq_test`、`audio_test`、`wifi_test`(含 `wifi_dns_check.sh`)、`dt2dts.py`、`fb2png.py`、
`capture_ev.py`、`echo_dbg.py` 等。

---

## 10. 命令速查

```sh
# 判活
ping 172.32.0.93
pidof lvgl_app
/root/bin/lvgl_daemon.sh status
/root/bin/lvgl_daemon.sh log

# 页面 / 调试
LVGL_START_PAGE=motor /root/lvgl_app        # 直达页面
LVGL_RUN_SEC=30 /root/lvgl_app              # 限时运行
touch /root/.no_lvgl                        # 禁用开机自启

# 设备
ls /dev/video*                              # 相机驱动是否加载
insmod /root/bin/rknpu.ko                   # YOLO 页起不来时
cat /sys/class/backlight/backlight/brightness
cat /sys/kernel/debug/gpio                  # 验电机电平
df -h /tmp                                  # 抓屏/录像前必看

# 网络
iw dev wlan0 scan                           # 注意是异步，1.7~2.1s
ifconfig wlan0 0.0.0.0 && udhcpc -i wlan0 -n -t 5 -T 3 -q
tools/wifi_test/wifi_dns_check.sh

# 宿主侧
python3 tools/fb2png.py --wh 640 480        # 抓屏（相机帧必带 --wh）
python3 tools/dt2dts.py                     # 看设备树
```
