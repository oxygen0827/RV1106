# 首次上板与恢复

目标是先建立“能恢复、能登录、能显示、能传文件”的基线，再开始改代码。

## 1. 上电前

- 断电安装 P024C128-CTP，按 [硬件参考](hardware-reference.md) 的翻盖 FPC 步骤操作。
- 检查 Type-C 线具备数据能力。
- 暂不连接马达和摄像头，减少变量。
- 准备 USB-TTL，串口电平按板上标识，不接错 TX/RX/GND，也不要误接 5 V。
- 记录板上 SoC、NAND、Wi-Fi 模组丝印和 PCB 版本照片。

## 2. 选择恢复介质

初次建议 SD 路线：

1. 使用官方 `sd_buildroot_img_260329.zip`。
2. 选择 8-16 GB SD 卡并先格式化。
3. 确认板载 NAND 已擦空；有有效 NAND 系统时，板子可能不会从 SD 启动。
4. 用瑞芯微 SocToolKit 选择 RV1106，按官方分区配置烧到 SD；不要把 `update.img` 当成 SD 卡目标分区文件。
5. 插卡、上电，保留完整串口启动日志。

NAND 路线：按住 BOOT 后插 USB，识别 Maskrom，再选 `nand_buildroot_img_260329.zip` 中对应镜像。NAND 擦除和烧写是破坏性操作，先确认 SD 恢复包可用。

## 3. 登录

官方默认：

```text
user: root
password: root
serial: 115200 baud
board USB IP: 172.32.0.93
host USB IP: 172.32.0.100
```

首次登录后立即采集，而不是先安装软件：

```sh
uname -a
cat /proc/cpuinfo
cat /proc/meminfo
cat /proc/cmdline
cat /proc/mtd
mount
df -h
ip addr
ls -l /dev/fb0 /dev/input /dev/video* /dev/snd 2>/dev/null
dmesg
```

把输出连同镜像文件名、PCB 版本、启动介质和日期保存到 `captures/bringup-YYYYMMDD/`。这将成为后续 AI 判断回归的基准。

## 4. 屏幕与触摸

先检查背光节点：

```sh
cat /sys/class/backlight/backlight/max_brightness
cat /sys/class/backlight/backlight/brightness
echo 49 > /sys/class/backlight/backlight/brightness
```

只向 framebuffer 写一帧，避免官方 `cat /dev/urandom > /dev/fb0` 无限写入：

```sh
fbset -i 2>/dev/null || true
dd if=/dev/zero of=/dev/fb0 bs=153600 count=1
dd if=/dev/urandom of=/dev/fb0 bs=153600 count=1
```

`153600` 仅按 240x320 RGB565 估算；先用 `fbset` 或 sysfs 确认真实 stride/bpp，再调整。屏幕不亮时按顺序排查：FPC 插入、背光、`/dev/fb0`、内核日志、设备树，不能直接判定 LCD 损坏。

触摸：

```sh
cat /proc/bus/input/devices
ls -l /dev/input/event*
```

若镜像带 `evtest`，选择 FT6336 对应 event 节点测试四角、滑动和坐标方向。记录显示旋转与触摸坐标是否一致。

## 5. Wi-Fi 和 SSH

配置 `/etc/wpa_supplicant.conf`，文件权限设为 600，不把密码提交到工作区：

```sh
ifconfig wlan0 up
mkdir -p /var/run/wpa_supplicant
wpa_supplicant -B -c /etc/wpa_supplicant.conf -i wlan0
udhcpc -i wlan0
ip addr show wlan0
```

切换网络前优先正常终止进程；仅在确认卡死时使用 `killall -9 wpa_supplicant`。完成后验证板子能访问网关、电脑能 SSH 到板子、板子能访问 AIChat Server 端口。

USB 虚拟网卡登录：电脑端设 `172.32.0.100`，然后：

```sh
ssh root@172.32.0.93
scp -r ./bin root@172.32.0.93:/root/
```

## 6. 音频

先枚举实际声卡和 controls：

```sh
aplay -l
arecord -l
amixer -c 0 contents
```

官方常用扬声器 control：

```sh
amixer -c 0 cset name='DAC LINEOUT Volume' 18
```

RK MPI 参考命令使用 16 kHz、16-bit PCM。测试前确认喇叭/MIC 接口和 PA 状态，音量从低值开始。录音后把 PCM 拉回电脑做波形、峰值、直流偏置和底噪检查，不能只凭“文件存在”判定成功。

## 7. 运行预编译 DeskBot

将 `reference/echo-mate/extracted/bin/bin_250627/bin/` 完整复制到板子。必须从 `bin` 目录启动，因为模型、证书、音频和共享库使用相对路径：

```sh
cd /root/bin
chmod +x ./main
./main
```

此预编译包是 AIChat 协议 1/端口 8765；不要和当前源码默认协议 2/端口 8000 的 Server 混用。

## 8. 基线验收表

| 项目 | 通过条件 |
| --- | --- |
| 启动 | 串口完整启动，无持续重启/内核 panic |
| 存储 | 实际启动介质与 `/proc/cmdline` 一致，空间可写 |
| 显示 | 背光、清屏、随机帧正常，无持续 SPI/fb 错误 |
| 触摸 | 四角和滑动有事件，方向与显示一致 |
| USB 网络 | `172.32.0.93` 可 SSH/SCP |
| Wi-Fi | RTL8723BS 获得地址，可达网关和电脑 |
| 音频 | 可控音量播放，MIC 录音有有效波形 |
| DeskBot | 首页、设置、页面切换和退出稳定 |
| 重复性 | 冷启动 3 次、进入退出主要页面 10 次仍正常 |

