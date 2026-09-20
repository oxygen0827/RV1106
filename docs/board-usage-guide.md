# Echo-Mate RV1106 板子使用说明（算法工程师版）

> 面向对象：要在板载 Linux 系统里部署、验证自己算法的工程师。
> 本文档描述当前这块板子的实机状态（2026-08-15 验收），直接照着做即可。

---

## 1. 板子是什么

**Echo-Mate 桌面机器人**，主控 Rockchip **RV1106**，跑 Linux。

| 项目 | 实测值 |
|---|---|
| CPU | 单核 32-bit Cortex-A7，带 NEON/FPU（armv7l） |
| 内存 | 内核映射约 55 MB（`MemTotal 55728 kB`，另有 66 MB CMA 给摄像头/DMA） |
| 系统 | Buildroot 2023.02.6，内核 Linux 5.10.110 |
| 屏幕 | 浦洋 P024C128-CTP，240x320，SPI（ST7789V3），当前 fb 模式 320x240 @ RGB565 |
| 触摸 | FT6336U，I2C，`/dev/input/event0`（fts_ts） |
| 按键 | `/dev/input/event1`（adc-keys） |
| 音频 | RV1106 内置 ACodec，`card 0: rv-acodec`，playback+capture 都在 device 0 |
| Wi-Fi | RTL8723BS（SDIO），已配好连 LDKJ，IP 由 DHCP 分配 |
| 存储 | SPI NAND，rootfs 为 UBIFS（可用约 86 MB）；`/oem` 3.4 MB、`/userdata` 2.2 MB、`/tmp` 27 MB(tmpfs) |
| NPU | RKNPU（`/dev/rknpu`，驱动 v0.8.2，可跑 RKNN 模型） |
| 摄像头 | 已枚举 `/dev/video0`（CIF）+ `/dev/video11/12`（ISP main/self path），**当前未接传感器**（见 §7） |

---

## 2. 怎么登录板子

### 方式 A：ADB（推荐，USB 直连，最稳）

板子 Type-C 直连 Mac，枚举为 Rockchip USB gadget（`rk3xxx`）。需要 adb：

```sh
# macOS 安装
brew install android-platform-tools

adb devices              # 应看到 5aedb378991eebd6  device
adb shell                # 进 root shell（无密码）
```

常用命令：

```sh
adb shell 'ls /root'          # 在板上执行命令
adb push <本地文件> <板路径>   # 传文件到板子
adb pull <板路径> <本地文件>   # 从板子拉文件
adb reboot                   # 重启
```

### 方式 B：Wi-Fi SSH

板子已连 Wi-Fi（SSID: LDKJ）。先查板子当前 IP（DHCP 会变）：

```sh
adb shell ip addr show wlan0 | grep inet   # 例如 192.168.31.254
ssh root@192.168.31.254                     # 密码 root
```

### 方式 C：串口（备用）

USB-TTL 115200，登录 root/root。未验证，需要时再确认。

---

## 3. 板子当前状态（验收基线）

- 系统正常启动，无 panic/重启循环（uptime 稳定，dmesg 无 oops）。
- **DeskBot 桌面应用已部署并开机自启**：`/root/bin/main` 随开机自动运行，屏幕显示操作界面。
- Wi-Fi 自启：`/etc/init.d/S99wifi` 开机自动连 LDKJ。
- 时间：无 RTC 电池，联网后由 NTP 自动同步。
- 板子 IP：`192.168.31.254`（DHCP，重启后可能变）。

---

## 4. 部署你自己的算法（核心流程）

### 4.1 交叉编译（在 Mac 上用 Docker）

板子是 **32 位 ARM + uClibc**，不能直接在板子上编译，必须在 Ubuntu 22.04 amd64 容器里交叉编译：

```sh
cd /Users/hushaohong/vibe-coding/RV1106

# 构建 SDK 镜像（首次）
./scripts/echo-sdk build-image

# 初始化 SDK 源码到 docker volume（首次，拉取较大）
./scripts/echo-sdk init

# 进入 SDK shell（工具链在 /workspace/Echo-Mate/SDK/rv1106-sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/）
./scripts/echo-sdk shell
```

工具链前缀：`arm-rockchip830-linux-uclibcgnueabihf-gcc` / `-g++`。

**注意**：
- 本机 SDK volume 目前是空的，首次需 `init`（会拉完整仓库）。
- 若已有自己的工具链，可直接用 cmake 交叉编译，参考 `upstream/Demo4Echo/yolov5_demo/cpp/toolchain.cmake` 的写法（SDK_PATH 替换成本机路径）。
- 板上内存只有 55 MB，**别开大内存的程序**；输出文件优先放 `/tmp`（27 MB tmpfs）或 rootfs。

### 4.2 传到板子并运行

```sh
# 假设编出可执行文件 my_alg
adb push ./my_alg /root/my_alg
adb shell 'chmod +x /root/my_alg && /root/my_alg'

# 后台运行 + 看日志
adb shell 'cd /root && setsid ./my_alg </dev/null >/tmp/my_alg.log 2>&1 &'
adb shell 'tail -f /tmp/my_alg.log'
```

### 4.3 想开机自启？

编辑板子的 `/etc/init.d/S99wifi`（已存在，负责 Wi-Fi + DeskBot），在 `start)` 段加一行即可：

```sh
if [ -x /root/my_alg ]; then setsid /root/my_alg </dev/null >/tmp/my_alg.log 2>&1 & fi
```

> 也可自己新建 `/etc/init.d/S9x_xxx` 脚本，注意执行顺序（字母序）和 `start|stop` 的 case 写法。

### 4.4 调试技巧

```sh
adb shell 'cat /proc/meminfo | head -3'        # 看内存余量
adb shell 'top -b -n 1 | head -15'             # 看 CPU/进程
adb shell 'dmesg | tail -20'                   # 内核日志
```

---

## 5. 屏幕 / 触摸怎么用

### 屏幕（framebuffer）

- 设备：`/dev/fb0`，320x240 @ RGB565（即 240x320 面板旋转 270 度）。
- 整帧大小：`320 * 240 * 2 = 153600` 字节。
- 画一帧（只写一次，**不要** `cat /dev/urandom > /dev/fb0` 无限写）：

```sh
# 清屏（黑色）
dd if=/dev/zero of=/dev/fb0 bs=153600 count=1
# 写一张自己生成的 RGB565 图
cat /tmp/myframe.bin > /dev/fb0
```

- 背光：`/sys/class/backlight/backlight/brightness`（0-99，当前 50）。
- 常见 UI 方案：LVGL 9.x + fbdev 驱动（DeskBot 即用 LVGL）。
- **线程安全**：LVGL 对象只能由 UI 线程创建/修改；后台线程结果通过消息/事件交给 UI 线程，禁止后台线程直接改 LVGL 对象。

### 触摸

- 设备：`/dev/input/event0`（fts_ts）。
- 测试：`cat /proc/bus/input/devices` 看节点；有 `evtest` 就用 evtest，没有就 `dd if=/dev/input/event0 of=/tmp/ev bs=24 count=5` 然后 `od -A d -t d4 /tmp/ev`。
- 触摸坐标与屏幕旋转的对应关系需自行确认（当前面板旋转 270 度）。

---

## 6. 音频怎么用

声卡：`card 0: rv-acodec`，playback 和 capture 都是 device 0。

```sh
aplay -l            # 列出 playback
arecord -l          # 列出 capture
```

播放 / 录音（16 kHz 单声道 16-bit）：

```sh
aplay -D plughw:0,0 /tmp/tone.wav
arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/rec.wav
```

音量（Rockchip ASoC 用 numid，不用名字）：

```sh
amixer -c 0 cset numid=24 20     # 喇叭音量 DAC LINEOUT Volume (0-30)
```

常用控件：`DAC LINEOUT Volume`(numid 24)、`ADC Mode`(19, 单端选 `SingadcL`)、`ADC MIC Left/Right Gain`(2/3)、`ADC Digital Left/Right Volume`(6/7)。详见 `docs/board-bringup.md` §6。

---

## 7. 摄像头 / NPU（算法常用）

### 摄像头（当前未接传感器）

- `/dev/video0` = `stream_cif_mipi_id0`（CIF 直连 raw 流）
- `/dev/video11` = `rkisp_mainpath`，`/dev/video12` = `rkisp_selfpath`（ISP 路径）
- **注意**：这块板当前 I2C4 上没有可用的传感器（探测过 GC1084/SC3336/SC4336/SC530AI 均未绑定）。要做视觉算法，需要：
  1. 接上支持 MIPI CSI 的传感器，并核对板级支持列表（固件内置 GC1084 的 IQ 文件，`/oem/usr/share/iqfiles/`）；
  2. 或用 USB 摄像头（`gspca_main.ko` 模块在 `/oem/usr/ko/`）。
- 现有调试脚本（板端）：`scripts/board/gc1084-mipi-diag.sh`、`gc1084-isp-test.sh`（需要 `/root/gc1084-isp-kit`）。

### NPU（RKNN）

- 设备：`/dev/rknpu`，驱动 v0.8.2。
- 板上有 `librknnmrt.so`（/oem/usr/lib），DeskBot 用的 yolov5 模型在 `/root/bin/model/`。
- RKNN 模型编译（rknn-toolkit）在 PC 上做，板端用 `librknnmrt` 推理。

---

## 8. 注意事项 / 红线

1. **不要随便烧录/擦除**：写 NAND、擦 NAND、刷镜像、改分区表前必须明确介质和恢复路径。日常只往 rootfs 拷文件即可。
2. **不要乱动 GPIO**：未确认引脚的用途（背光/电源/复位）前，不主动写电平。
3. **内存很小**：55 MB 总内存，DeskBot 已在跑（约占 19 MB）。部署算法前先 `cat /proc/meminfo` 看余量；`/tmp` 只有 27 MB。
4. **存储很小**：rootfs 可用约 86 MB，别把模型/日志堆在板子 rootfs；日志写 `/tmp`，重要结果及时 pull 回 Mac。
5. **API key / token**：不要把真实 API key、Wi-Fi 密码写进 Git、日志或命令行。示例 token `123456` 不得用于跨可信局域网部署。
6. **DeskBot 正在占用屏幕/声卡**：如果算法要独占屏幕或声卡，先停掉 DeskBot：`adb shell 'killall main'`（不会影响系统，重启会自动回来）。
7. **固件版本**：当前内核为 2025-04-09 构建（#45），与开发基线（2026-03-29）不同；如果算法依赖特定驱动行为，先确认固件差异（记录在 `docs/dev-experience.md`）。
8. **Wi-Fi 密码**：`/etc/wpa_supplicant.conf` 里的密码是私密信息，不要截图外传。

---

## 9. 板子被带走的场景建议

算法工程师会单独带走板子，可能不在原有 Wi-Fi 环境。届时：

1. **登录**：优先 USB 直连用 ADB（无需网络）；没有电脑时用串口 115200。
2. **连 Wi-Fi**：改 `/etc/wpa_supplicant.conf` 里的 SSID/密码（`chmod 600`），然后：
   ```sh
   killall wpa_supplicant
   wpa_supplicant -B -c /etc/wpa_supplicant.conf -i wlan0
   udhcpc -i wlan0
   ip addr show wlan0        # 查新 IP
   ```
3. **DeskBot 不想要**：`killall main`；想关掉开机自启，把 `/etc/init.d/S99wifi` 里启动 main 的那段注释掉即可。

---

## 10. 相关资源

- 板端验收与恢复流程：`docs/board-bringup.md`
- 实机调试经验（Wi-Fi/音频/AIChat）：`docs/dev-experience.md`
- 硬件参考（原理图/接口）：`docs/hardware-reference.md`
- 开发环境 Docker：`compose.yaml` + `scripts/echo-sdk`
- 源码（只读）：`upstream/Echo-Mate/`、`upstream/Demo4Echo/`（yolov5_demo 有 cmake 交叉编译示例）
- 本板验收证据：`captures/bringup-20260815/`

---

*文档生成时间：2026-08-15。板子序列号（ADB）：5aedb378991eebd6。*
