# echo-mate-lvgl

RV1106 桌面机器人「回声伙伴」的 LVGL 应用，以及自写的 SC3336 图像传感器驱动。

板子是一块 320×240 的小屏，跑的不是 Android/Linux 发行版，而是 Rockchip RV1106 上
一个精简的 Buildroot + uClibc 环境（内存 54MB，rootfs 只剩几 MB）。所以这里的代码
基本都是「在资源极紧的前提下把功能做实」的产物，注释里记录了大量踩过的坑。

## 硬件

| 项 | 型号 / 参数 |
|---|---|
| SoC | Rockchip RV1106G3（单核 Cortex-A7 + 0.5T NPU） |
| 屏幕 | 320×240 SPI LCD（ST7789V），RGB565 |
| 摄像头 | SC3336（自写 V4L2 sensor 驱动，走 rkisp） |
| WiFi | RTL8723BS（2.4G 单频） |
| 电机 | 两路 H 桥，GPIO 直驱 |
| 音频 | 片内 acodec，16kHz 单声道 |

## 功能

- **相机**：V4L2 采集 + rkaiq 3A（自动曝光/白平衡），支持拍照（BMP）与录像（AVI）
- **YOLOv5**：RKNN 零拷贝推理，640×640 单帧约 80ms，界面 10fps
- **语音对话**：WebSocket + Opus + ASR + LLM + TTS，唤醒词或按钮触发
- **桌面形象**：4 个程序化绘制的角色，随语音状态变表情（聆听/思考/说话），可点击互动
- **电机测试**：左右两路方向控制，离开页面自动停止
- **系统页**：WiFi 扫描/连接、天气、NTP 校时、背光调节

## 目录

```
echomate-lvgl/
├── driver/
│   └── sc3336.c              # 自写 SC3336 sensor 驱动（V4L2 subdev）
└── lvgl_app/
    ├── CMakeLists.txt
    ├── lv_conf.h
    ├── conf/                 # 板端配置文件
    ├── tools/                # 抓屏、调试脚本
    └── src/
        ├── main.c            # 入口：LVGL 初始化、页面装配、主循环
        ├── ui/               # 界面层（纯 C，只做接线，不碰文件/网络/线程）
        ├── cam/              # 采集（C++ 类）+ 图像格式转换（纯 C）
        ├── yolo/             # NPU 推理
        ├── aichat/           # 语音链路接线
        ├── motor/            # 电机控制
        ├── avatar/           # 桌面形象：角色表 + 选择持久化
        ├── bright/           # 背光（sysfs）
        ├── wifi/  net/       # WiFi 扫描/连接、天气、校时
        └── fonts/            # 生成的中文字形（16px，见下）
```

约 1.3 万行手写代码。

## 设计上比较在意的几点

**1. 界面层不碰系统资源**

`src/ui/*.c` 里没有 `fopen`、没有 socket、没有 `pthread_create`。所有 IO 和并发都在
C++ 模块里，UI 只负责把结果画出来。这样换界面不会碰到资源管理，换实现也不会碰界面。

**2. C++ 模块只暴露一张函数表**

```c
struct motor_ops {
    int (*open)(void);
    int (*set)(int id, int dir);
    ...
};
const struct motor_ops *motor_api(void);   // 全模块唯一的 C 符号
```

这样 C 和 C++ 的边界只有一个符号，不用给每个函数写 `extern "C"` 包装。

**3. LVGL 只在主线程碰**

后台线程只写 `std::atomic` 快照，UI 用 `lv_timer` 定时拉。跨线程直接操作 `lv_obj_*`
在这块板子上的表现是随机花屏或崩溃，很难查。

**4. 状态用快照，不用回调**

相机、语音、天气都是「业务线程写快照 → UI 定时读」。比回调简单，也不会有跨线程回调
碰 UI 的问题。

## 编译

需要 Rockchip 的 ARM 工具链：

```
arm-rockchip830-linux-uclibcgnueabihf-gcc 8.3.0
```

依赖：

- **LVGL 9.2.3**（本仓库未包含，需自行放到 `lvgl_app/lvgl/`）
- rkaiq（ISP 3A，来自 Rockchip SDK）
- librknnmrt（NPU 运行时，来自 RKNN Toolkit2）

```sh
cd lvgl_app
mkdir -p build && cd build
export TMPDIR=$PWD/.tmp_build        # 板子/容器 /tmp 小，编译中间文件放这里
cmake .. -DCMAKE_TOOLCHAIN_FILE=<你的工具链文件>
make -j12
```

## 关于 `driver/sc3336.c`

Rockchip SDK 里没有这颗 sensor 的驱动，这个文件是照着 SC3336 datasheet 手写的
V4L2 subdev 驱动，包含：

- I2C 寄存器读写、上电时序、MIPI 时钟配置
- 曝光/增益控制（`V4L2_CID_EXPOSURE`、`V4L2_CID_ANALOGUE_GAIN`）
- 与 rkisp 对接的 media pipeline 绑定

编译成 `sc3336.ko` 后 `insmod`，会注册成 `/dev/video*` 的一个 subdev 节点。

注意：驱动**不会开机自启**，需要应用侧或脚本手动加载。

## 说明

- 仓库里没有放编译产物（`build/`）、截图存档（`shots/`）和 LVGL 源码（`lvgl/`）
- `src/fonts/lv_font_cn_16.c` 是生成文件（9.8MB），内容是中文字形点阵数组，
  用 LVGL 的字体转换工具从 Noto Sans SC 提取，保留它是为了让仓库 clone 下来能直接编
