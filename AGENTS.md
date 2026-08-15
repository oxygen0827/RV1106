# Echo-Mate Agent Guide

本文件约束后续 AI/Codex 在此工作区内的开发行为。

## 项目身份

- 目标硬件：Echo-Mate RV1106 Linux 开发板/桌面机器人。
- 不是黄山派 SF32，也不是 SiFli/RT-Thread 工程。
- 黄山派附件只能借鉴“资料索引、板级 bring-up、能力边界、自动回归”的方法，不能复用其引脚、SDK、驱动、烧录命令或 Runtime API。

## 已确认硬件事实

- CPU：单核 32-bit Cortex-A7，带 NEON/FPU。
- 内存：项目资料标注 256 MB DDR3L。
- 屏幕：浦洋 P024C128-CTP，240x320，4-wire SPI，ST7789V3。
- 触摸：FT6336U，I2C。
- Wi-Fi/蓝牙：RTL8723BS；Wi-Fi 走 SDIO，蓝牙当前官方手册仍标为测试阶段。
- 存储：SD 卡或 SPI NAND；SD 启动要求 NAND 无有效固件。
- 应用基线：Buildroot + Linux 5.10 系列 + LVGL 9.2 + framebuffer。

不确定项必须从实机、原理图、设备树或当前 SDK 配置确认，不能从类似开发板猜测。

### 实机确认（2026-08-07，SPI NAND 启动）

- 内核：Linux 5.10.110，2026-03-29 构建，armv7l。
- 启动介质：SPI NAND（`androidboot.storagemedia=mtd`，UBIFS；rootfs 约 181 MB，可用约 102 MB）。
- 屏幕：`/dev/fb0` = `fb_st7789v`，当前 fb 模式 320x240 @ RGB565（240x320 旋转 270 度）；背光 `/sys/class/backlight/backlight`，`brightness=50 / max=99`。
- 触摸：`fts_ts`（FT6336U，I2C3 @ 0x38）为 `event0`；`adc-keys`（板上按键）为 `event1`。
- 音频：`card 0: rv-acodec`（RV1106 内置 ACodec，`ffae0000.i2s` + `ff480000.acodec`），playback + capture 均已枚举。
- 喇叭验证通过：`aplay` 播放 16 kHz 单声道 WAV 正常（exit 0）；音量控制在 `DAC LINEOUT Volume`。
- 屏幕验证通过：向 `/dev/fb0` 写 153600 字节整帧可显示纯色；写入方式为一次 `cat file > /dev/fb0`，不要用无限写入命令。
- 当前固件的 `/proc/iomem` 与设备树只向 Linux 映射 128 MB RAM，启动参数另预留 66 MB CMA，故 `MemTotal` 约 55 MB；这与资料标注的 256 MB 不一致，物理 DDR 容量需结合芯片/DDR 丝印、bootloader 和设备树继续确认。

## 权威顺序

遇到冲突时按以下顺序处理并记录差异：

1. 实机证据：芯片丝印、启动日志、`/proc/device-tree`、sysfs、设备节点。
2. 立创开源平台 PCB/原理图：硬件事实。
3. 当前官方 Git 仓库和目标 commit：软件事实。
4. no-chicken 手册：操作流程和最新下载入口。
5. B 站教程：理解思路，不作为精确版本契约。
6. Luckfox/Rockchip 相邻板资料：仅作参考，必须重新验证。

## 资料与源码边界

- `reference/echo-mate/` 保存下载附件和只读快照。
- `upstream/` 保存上游浅克隆，不在其中直接写产品功能。
- 可编译源码以 Docker volume `echo-mate-sdk` 中的 `/workspace/Echo-Mate` 为准；从宿主项目使用 `./scripts/echo-sdk` 管理。
- 新功能代码应放入单独工作树/分支；开始前记录上游 commit、SDK board config 和镜像版本。
- 当前 macOS SDK 检出仅用于阅读。Linux kernel 源码含仅大小写不同的文件，大小写不敏感文件系统会让 Git 显示伪修改/覆盖；正式构建必须在 Ubuntu 22.04 的区分大小写文件系统重新克隆。
- 完整 SDK 和目标程序交叉编译必须在 `linux/amd64` Ubuntu 22.04 容器内执行，不直接使用 macOS 检出目录构建。
- 不提交镜像、模型、构建输出或第三方大文件，除非仓库策略明确允许。

## 板端访问（macOS 实机）

- 板子 Type-C 直连 Mac 后枚举为 Rockchip USB gadget：`rk3xxx`，带 RNDIS 网卡 + ADB 接口，序列号即 ADB 设备号（如 `f95be6ec9d1c67fa`）。
- **macOS 26 不支持 RNDIS 驱动**，所以官方文档的 USB 虚拟网卡 `172.32.0.93` SSH/SCP 在 macOS 上不可用。
- 在 macOS 上进板子的可用通道是 **ADB**：`brew install android-platform-tools`，然后：
  ```sh
  adb shell            # 进板子 shell
  adb push <local> <remote>   # 传文件到板子
  adb pull <remote> <local>   # 拉取文件
  ```
- `adb shell` 登录身份为 root，可执行 `aplay`、`amixer`、`cat /sys/...` 等。
- **Wi-Fi 已验证（2026-08-07）**：板子已配好 LDKJ（WPA2），DHCP IP 为 `192.168.31.240`，与 Mac 同网段；可用 `ssh root@192.168.31.240`（密码 root）。DHCP 分配，IP 可能变化，用 `adb shell ip addr show wlan0` 现查。配置见 `docs/dev-experience.md`「板子 Wi-Fi 联网配置」。
- 其它登录途径（USB-TTL 串口 115200、Wi-Fi SSH root/root）中 Wi-Fi SSH 已随上一条实测可用；USB-TTL 未验证，需要时再单独确认。

## 安全约束

- 上游源码中存在看起来可用的高德/阿里云 API key。视为已经泄露的示例，不得使用、传播或复制到新配置；新 key 必须从环境变量或板外私密配置注入。
- `123456` 只是示例 access token，不得用于跨可信局域网部署。
- 不把 Wi-Fi 密码、API key、token 写进 Git、截图、日志或命令行参数回显。
- 写 NAND、擦 NAND、烧整镜像、修改分区表或设备树前，必须明确目标介质和恢复路径。
- 未确认 GPIO、背光、电源和复位用途前，不主动写电平。

## 开发顺序

1. 先完成 [board-bringup.md](docs/board-bringup.md) 并保存基线证据。
2. 优先用 SDL 仿真开发纯 UI/状态逻辑。
3. 板端应用优先只替换 `bin/`，不要每次都重烧系统镜像。
4. 涉及驱动/内核时再进入 SDK、设备树和 Buildroot 层。
5. 每个功能都要有最小真机验收：启动、正常路径、失败路径、退出后资源释放、重复进入。
6. 涉及线程时，后台线程不得直接销毁或并发修改 LVGL 对象；通过消息/事件交给 UI 线程。

## 版本陷阱

- 2025-06-27 预编译 `bin` 使用 AIChat 端口 `8765`、协议 `1`。
- 当前 Demo4Echo 源码配置使用端口 `8000`、协议 `2`。
- 客户端、服务端、`system_para.conf` 必须成套，禁止混用后只排查网络。
- 当前 AIChat 修复分支使用端口 8000、协议 2；`GLM-4-Voice` 只从电脑端 `ZHIPU_API_KEY` 读取，鉴权使用随机 `AICHAT_ACCESS_TOKEN`。Client 不得发送或覆盖云端 key，缺配置或云端音频非法时必须返回明确 protocol error。
- 文档写 `conf/dev_conf`，当前源码实际是 `conf/dev_conf.h`。
- `toolchain.cmake` 含作者本机绝对 SDK 路径，构建前必须改为本机路径或改造成可配置变量。
- Echo Buildroot 板级配置只有两条正式基线：`BoardConfig-SD_CARD-Buildroot-RV1106_Echo_Mate-DeskMate.mk` 和 `BoardConfig-SPI_NAND-Buildroot-RV1106_Echo_Mate-DeskMate.mk`；不要误选 Luckfox Pico 配置。

## 完成标准

- 构建命令和依赖可复现。
- 仿真或目标板测试结果有日志证据。
- 失败不会留下后台线程、摄像头、声卡、GPIO 或 LVGL 对象处于未知状态。
- 文档同步更新硬件事实、配置协议和恢复步骤。
