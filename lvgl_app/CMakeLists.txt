# ============================================================
# lvgl_app - 自建 LVGL 9 应用（RV1106）
#
# 用法：
#   mkdir build && cd build
#   cmake .. -DTARGET_ARM=ON
#   make -j4
#
# 产物：build/lvgl_app
# ============================================================
cmake_minimum_required(VERSION 3.10)

option(TARGET_ARM "Build for ARM (RV1106)" ON)

if(TARGET_ARM)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.cmake"
            CACHE FILEPATH "Toolchain" FORCE)
    endif()
endif()

project(lvgl_app C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ------------------------------------------------------------
# LVGL 9 库
# ------------------------------------------------------------
# 我们把 lvgl/demos 目录删掉了（省 46MB），
# 必须告诉 LVGL 的 cmake 不要再建 lvgl_demos 目标，
# 否则 add_library(lvgl_demos) 会因为「No SOURCES given」失败。
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)

# examples 也用不上（编译要 2 分半，几百个文件），一起关掉
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)

# ThorVG 保持开启：lv_lottie.h 硬性要求它，关掉会导致 vg_lite 系列编译报错。
# 所以这里不 disable，而是在下面把 lvgl_thorvg 链接进 lvgl_app。

add_subdirectory(lvgl)

# lv_conf.h 放在项目根目录，通过 LV_CONF_INCLUDE_SIMPLE 让它被找到
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ------------------------------------------------------------
# 应用
# ------------------------------------------------------------
# 中文字模是生成出来的 .c 文件，不能放进 lvgl/ 目录（那是上游源码，
# 升级会被覆盖）。它作为应用自己的源文件一起编译。
#
# 注意：这个文件 9.4MB 源码、2 万个字形，-O2 编译要 1-2 分钟，-j4 时会
# 明显拖慢整个构建。改它之后单独 make 就行，不会重复触发。
add_executable(lvgl_app
    src/main.c
    src/ui/ui_theme.c
    src/ui/ui_home.c
    src/ui/ui_pages.c
    src/ui/ui_selftest.c
    src/wifi/wifi_scan.cpp
    src/wifi/wifi_conn.cpp
    src/ui/ui_wifidlg.c
    src/ui/ui_netcheck.c
    src/ui/ui_weathercard.c
    src/net/netcheck.cpp
    src/net/weather.cpp
    src/net/timesync.cpp
    src/fonts/lv_font_cn_16.c
    # 唤醒框架 + LVGL 桥接层（只加 .cpp，不能加 demo 的 main.cpp）
    ../interview/wake_sdk_demo/src/wake_runtime.cpp
    ../interview/wake_sdk_demo/src/supervisor.cpp
    ../interview/wake_sdk_demo/src/mock_detector.cpp
    # 语音链路的所有"接线"代码，已从 interview/wake_sdk_demo/integration/
    # 搬进本工程：不再跨目录桥接，改页面入口方式（见 src/ui/ui_aichat.c）
    src/aichat/lv_wake_bridge.cpp
    src/aichat/snowboy_wake.cpp
    src/aichat/voice_chat.cpp
    # snowboy 的 C wrapper 是源码（.a 里只有核心库）
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/third_party/snowboy/snowboy-detect-c-wrapper.cc

    # ===== 真语音对话链路（AIChat Demo 的 Client 核心）=====
    # 只要"能真的对话"就必须带上这一整套：WS 收发、Opus 编解码、
    # 状态机、VAD/ASR/TTS 的协议处理。缺一个都会在链接期报 undefined。
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Audio/AudioProcess.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/WebSocket/WebsocketClient.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Intent/IntentHandler.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/StateMachine/StateMachine.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/Application.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/WS_Handler.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/StateConfig.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/IntentsRegistry.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Fault.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Startup.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Stop.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Idle.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Listening.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Thinking.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserStates/Speaking.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/Application/UserIntents/RobotMove.cc
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/c_interface/AIchat_c_interface.cc
    # 我们自己的封装层（独立线程 + 线程安全快照）
    # ★ 已随 lv_wake_bridge 一起搬进 src/aichat/，见上面那组
    # ===== 摄像头采集 + 界面 =====
    # cam_capture.cpp  V4L2 采集 + rkaiq 3A + 采集线程（C++ 类，RAII 管资源）
    #                  ★ 取代了原 cam.c：那边用全局结构体 + 手工清理，
    #                    失败路径漏 free 过一次，直接把 54MB 的板子 OOM 掉。
    #                    现在资源全是成员对象，构造函数失败自动析构，不会再漏。
    # cam_image.cpp    NV12→RGB565/BGR24、BMP 拍照、AVI 录像（零依赖手写）
    # ui_camera.c      预览页 + 拍照/录像按钮（C，调 extern "C" 那层）
    src/cam/cam_capture.cpp
    src/cam/cam_image.cpp
    src/ui/ui_camera.c
    # ===== 电机（D1：/dev/echo_motor，两路 GPIO H 桥）=====
    # motor_ctl.cpp  用户态唯一入口：open/ioctl 封装 + RAII + 中文错误
    #                ★ 枚举和 ioctl 号直接来自 kernel/my/motor_uapi.h，
    #                  不在应用里另抄一份（抄了必错位）。
    # ui_motortest.c 手动点按测试页（C，调 extern "C" 那层）
    src/motor/motor_ctl.cpp
    src/ui/ui_motortest.c
    # ===== YOLO 目标检测（RV1106 NPU，零拷贝）=====
    # yolo_detect.cpp  rknn 初始化 + letterbox + 解码/NMS + 推理线程
    # ui_yolo.c        预览 + 检测框叠加（C，调 extern "C" 那层）
    src/yolo/yolo_detect.cpp
    src/ui/ui_yolo.c
    # AI 对话页（语音链路入口：进页面拉起、退页面停）
    src/ui/ui_aichat.c
    # 屏幕亮度（背光 sysfs）+ 页面
    src/bright/brightness.cpp
    src/ui/ui_brightness.c
    # 桌面形象（角色表 + 持久化）+ 渲染/选择页
    src/avatar/avatar.cpp
    src/ui/ui_avatar.c
)

# rknn 的头文件只给 yolo_detect.cpp 用。
# ★ 不要写成全局 include_directories：一改全部编译命令都变，
#   整个工程（含 AIChat 那批 20 秒一个的 .cc）会全部重编。
set_source_files_properties(src/yolo/yolo_detect.cpp PROPERTIES
    INCLUDE_DIRECTORIES
    "/home/tao/Echo-Mate/Demo/yolov5_demo/cpp/3rdparty/rknpu2/include")

# 字模文件全是静态常量数组，优化不了什么，用 -O0 编能让编译时间
# 从分钟级降到十几秒，产物大小几乎不变（常量数据不会被优化掉）。
set_source_files_properties(src/fonts/lv_font_cn_16.c
    PROPERTIES COMPILE_OPTIONS "-O0")

# 让 main.c 里的 #include "ui/ui.h" 能被解析到 src/ui/ui.h
# rkaiq 的头文件层次很深（include/rkaiq/xcore/base、algos/*/... 互相 include），
# 手动列目录必然漏，直接把它 include 下【所有】子目录都加进来。
# 这是库作者的目录组织问题，不是我们的设计 —— 注释在这里免得以后被当成"乱"。
set(RKAIQ_ROOT "/home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf")
# GLOB_RECURSE 会把文件也收集进来（LIST_DIRECTORIES 只影响是否包含目录本身），
# 所以还得自己过滤一遍：只要目录，否则 gcc 会对每个 .h 报 "not a directory"。
file(GLOB_RECURSE RKAIQ_ALL ${RKAIQ_ROOT}/include/*)
set(RKAIQ_INCLUDE_DIRS "")
foreach(_d ${RKAIQ_ALL})
    if(IS_DIRECTORY ${_d})
        list(APPEND RKAIQ_INCLUDE_DIRS ${_d})
    endif()
endforeach()
list(APPEND RKAIQ_INCLUDE_DIRS ${RKAIQ_ROOT}/include)

target_include_directories(lvgl_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 唤醒框架 + 桥接层
target_include_directories(lvgl_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../interview/wake_sdk_demo/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aichat
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/third_party/snowboy/include
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/c_interface
    /home/tao/echo-sysroot/usr/include/jsoncpp
    # 摄像头：自己的头 + rkaiq 的 uAPI2 头
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cam
    # 通用 RAII 资源封装（resources.hpp：Fd/File/Pipe/Mmap/run_argv）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    # rkaiq 的 include 目录（生成时用 find 展开，共 64 个）
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/a3dlut
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/abayer2dnr2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/abayer2dnrV23
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/abayertnr2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/abayertnrV23
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/ablc
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/ablcV32
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acac
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/accm
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acgc
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acnr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acnr2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acnrV30
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acp
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/acsm
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/adebayer
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/adegamma
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/adehaze
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/adpcc
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/adrc
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/ae
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aeis
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/af
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/afec
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/again
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/again2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/agamma
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/agic
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aie
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aldch
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/alsc
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/amd
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/amerge
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/amfnr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/anr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/anr/tnr_md
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aorb
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/arawnr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/arawnr2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/asd
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/asharp
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/asharp3
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/asharp4
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/asharpV33
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/atmo
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/auvnr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/awb
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aynr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aynr2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aynr3
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/algos/aynrV22
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/common
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/common/mediactl
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/ipc_server
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/iq_parser
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/iq_parser_v2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/iq_parser_v2/j2s
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/smartIr
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/uAPI2
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/xcore
    /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rkaiq/xcore/base)
# std::thread 需要 -pthread 参与编译（不只是链接）
target_compile_options(lvgl_app PRIVATE -pthread)
# 【ABI 必须分文件设置，不能全局设】
#   libsnowboy-detect.a 是旧 ABI 编的，SnowboyDetect 的构造函数签名是
#   std::string（非 __cxx11），所以【编译 snowboy 的那个 wrapper】必须用
#   _GLIBCXX_USE_CXX11_ABI=0，否则符号对不上。
#   但 sysroot 的 libjsoncpp.so.25 是【新 ABI】（符号带 B5cxx11），
#   全局设成 0 会让 Json::Value::asString() 等全部 undefined reference。
#   两边都要，所以只能按文件区分：
#     - 只有 snowboy-detect-c-wrapper.cc 用旧 ABI
#     - 其余（jsoncpp 用户、我们自己的代码）用默认新 ABI
#   这样混用安全，因为两者之间只隔着 C 接口（SnowboyDetectConstructor
#   收 const char*，不传 std::string），没有跨 ABI 的对象。
set_source_files_properties(
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/third_party/snowboy/snowboy-detect-c-wrapper.cc
    PROPERTIES COMPILE_DEFINITIONS "_GLIBCXX_USE_CXX11_ABI=0")
set_target_properties(lvgl_app PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# 打开 rkaiq（3A）。关掉它图像会偏暗 —— 见 cam.c 里 aiq_start 的注释。
# 调试时可以 -DCAM_NO_AIQ 快速对比"有没有 3A"的差别。
option(CAM_NO_AIQ "不启用 rkaiq 3A（调试用）" OFF)
if(NOT CAM_NO_AIQ)
    target_compile_definitions(lvgl_app PRIVATE CAM_USE_RKAIQ)
    target_link_libraries(lvgl_app PRIVATE /home/tao/Echo-Mate/SDK/rv1106-sdk/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/lib/librkaiq.so)
endif()

target_link_libraries(lvgl_app PRIVATE lvgl)
# 字体声明在 lv_conf.h 的 LV_FONT_CUSTOM_DECLARE 里，需要能找到 lv_font_cn_16.c 的头声明。
# lv_font_conv 生成的文件开头会 include "lvgl.h"，靠 lvgl 的 PUBLIC include 传递即可。
target_link_libraries(lvgl_app PRIVATE lvgl_thorvg)
target_link_libraries(lvgl_app PRIVATE m pthread
    # 真 snowboy（树莓派 ARM 静态库）+ PortAudio 录音
    /home/tao/Echo-Mate/Demo/AIChat_demo/Client/third_party/snowboy/lib/rpi/libsnowboy-detect.a
    portaudio asound dl openblas
    # 对话链路：json 协议解析 + Opus 编解码
    jsoncpp opus
    # NPU 推理运行时（RV1106 的迷你 runtime，和板上 /root/bin/lib 那份同一个版本）
    # 运行时靠 rpath 找：$ORIGIN/lib 和 $ORIGIN/bin/lib 都试
    /home/tao/Echo-Mate/Demo/yolov5_demo/cpp/3rdparty/rknpu2/Linux/armhf-uclibc/librknnmrt.so)

# 让 lvgl_app 启动时能找到 librknnmrt.so：
#   部署位置是 /root/lvgl_app，所以 $ORIGIN = /root
#   板上已有的那份在 /root/bin/lib（原厂 demo 装的），不用再拷一份占 rootfs
set_target_properties(lvgl_app PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN/lib:\$ORIGIN/bin/lib")

# 输出到 build/ 下，方便 scp
set_target_properties(lvgl_app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "==========================================")
message(STATUS " lvgl_app 配置")
message(STATUS "   TARGET_ARM = ${TARGET_ARM}")
message(STATUS "   SYSROOT    = ${CMAKE_SYSROOT}")
message(STATUS "==========================================")
