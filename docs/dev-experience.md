# 开发经验记录

记录开发与调试过程中遇到并解决的问题。每新增一条请按下面的模板追加：

```markdown
## YYYY-MM-DD：一句话标题

- 现象：…
- 排查：…
- 原因：…
- 解决：…
- 相关文件/命令：…
```

规则：

- 每条只记录已确认解决的问题，临时猜测不写入。
- 记录排查路径比记录最终答案更重要，避免下次重复踩坑。
- 涉及硬件事实、配置协议、恢复步骤的，同时更新 [AGENTS.md](../AGENTS.md) 对应章节。

---

## 2026-08-07：macOS 通过 USB-C 直连无法进板子

- 现象：板子 Type-C 直连 Mac，`172.32.0.93` ping 不通，`ssh root@172.32.0.93` 无响应，系统没有识别到 Rockchip USB 设备。
- 排查：`ioreg`/`system_profiler` 看 USB 树、`log show` 查插拔事件、`ifconfig -a` 查网卡。板子拔插后 USB 树能枚举出 `rk3xxx`（VID 8711），但没有任何网卡接口出现。
- 原因：板子 USB gadget 暴露的是 **RNDIS 网卡 + ADB** 两个接口；macOS 26 原生不支持 RNDIS 驱动，所以不会自动创建虚拟网卡，`172.32.0.93` 只对 Linux/Windows 生效。
- 解决：改用 **ADB** 进板子：
  ```sh
  brew install android-platform-tools
  adb devices          # 序列号即设备号，如 f95be6ec9d1c67fa
  adb shell            # root 身份
  adb push/pull        # 传文件
  ```
- 相关文件/命令：`AGENTS.md`「板端访问（macOS 实机）」。

## 2026-08-07：amixer `sget` 找不到声卡控件，但 `contents` 能列出

- 现象：`amixer -c 0 sget 'DAC LINEOUT Volume'` 报 `Unable to find simple control`。
- 排查：`amixer -c 0 contents` 能列出 `numid=24 DAC LINEOUT Volume`、`numid=25 DAC HPMIX Volume`。
- 原因：Rockchip ASoC 驱动的控件没有注册为 alsa-lib 的 simple control，`sget`/`sset` 按名字查不到。
- 解决：用 `cset` 加 numid 控制：
  ```sh
  amixer -c 0 cset numid=24 10     # DAC LINEOUT Volume（0-30）
  ```
- 相关文件/命令：`docs/board-bringup.md` 音频章节。

## 2026-08-07：屏幕写帧大小与 framebuffer 模式

- 现象：不确定该往 `/dev/fb0` 写多少字节才能显示整帧。
- 排查：`fbset` 显示 mode `320x240` @ 16bpp（RGB565），即物理 240x320 已旋转 270 度。
- 原因：帧大小 = 宽 x 高 x 2 = 320 x 240 x 2 = 153600 字节，与资料中的 240x320 RGB565 估算值恰好相同，但方向已旋转。
- 解决：写整帧用 `cat file > /dev/fb0` 一次完成，不要用 `cat /dev/urandom > /dev/fb0` 之类无限写入命令。
- 相关文件/命令：`docs/board-bringup.md` 屏幕章节；`AGENTS.md`「实机确认」。

## 2026-08-07：麦克风录音信号弱、增益调教

- 现象：`arecord` 能录到文件，但语音段能量极弱（rms 几十），只有上电瞬间的 pop 尖峰明显。
- 排查：`amixer -c 0 contents` 检查 ADC 相关控件。默认 `ADC Mode=DiffadcL`（差分模式）、`ADC Digital Volume=211`（0.5dB/步，该值已接近顶格）。
- 原因：板载 MIC 是单端接法，差分模式浪费一半信号；且数字音量过高会在大声时削波（峰值打到 16384）。
- 解决：
  1. 切单端：`amixer -c 0 cset numid=19 1`（ADC Mode → `SingadcL`），语音强度提升约 3-5 倍。
  2. 数字音量降到 185 左右：`amixer -c 0 cset numid=6 185`、`numid=7 185`，正常说话约 -12 dBFS、大声不削波。
  3. 音量步进是 0.5dB/步：从 211 降到 160 是约 -25dB，会连底噪一起压没；要按需微调。
- 注意：提示音要用喇叭先响、停顿 1-2 秒再开始录音，否则测试者来不及开口；DAC LINEOUT 音量太低时提示音听不见。
- 相关文件/命令：`arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -d N /data/x.wav`；`AGENTS.md`「实机确认」音频节。

## 2026-08-07：buildroot 源码下载失败（代理与镜像）

- 现象：`./build.sh rootfs` 在 `>>> xxx Downloading` 阶段反复失败，日志只显示 `.stamp_downloaded Error 1`，无具体网络错误。
- 排查：
  1. 容器内 `curl` 不存在导致 `buildroot_mirror_select.sh` 失败（`timeout: failed to run command 'curl'`）。
  2. 手动 wget 官方源（alsa-project.org、ftp.gnu.org）**走代理**能下载，`sources.buildroot.net` 即使走代理也不可用。
  3. buildroot 的下载命令是 `wget --passive-ftp -nd -t 3`，但它**不继承传入的环境代理变量**（`http_proxy` 等被子 make 清掉）。
  4. 曾误判：autoconf 手头有 .tar.gz 但 buildroot 要的是 **.tar.xz**（`AUTOCONF_SOURCE = autoconf-2.71.tar.xz`），文件名不一致导致反复重下。
- 解决：
  1. 代理写进 `/etc/wgetrc`（`use_proxy = yes` + http/https 指向 `http://host.docker.internal:7897`），所有 wget 自动走代理。宿主机 Clash 类代理端口用 `scutil --proxy` 查（本例 7897）。
  2. 用 `make source` 一次性预下载全部源码包（本地 dl/ 有缓存后构建不再依赖网络）。
  3. 注意 Docker Desktop 里容器访问宿主用 `host.docker.internal`。
- 相关文件/命令：`scripts/docker-wget-proxy.sh`（运行时注入 wgetrc 的辅助脚本）；`docker compose -f compose.yaml run --rm -e http_proxy=http://host.docker.internal:7897 -e https_proxy=http://host.docker.internal:7897 sdk bash -lc 'bash /host-project/scripts/docker-wget-proxy.sh "..."'`。

## 2026-08-07：busybox 构建报 `write jobserver: Bad file descriptor`

- 现象：buildroot 构建到 busybox 时，`make[2]: *** write jobserver: Bad file descriptor. Stop.`，`.stamp_dotconfig` 失败；`BR2_JLEVEL=0` 时尤其明显。
- 排查：GNU Make 4.3 的嵌套 jobserver fd 传递 bug 与 busybox Kbuild 的 `$(MAKE)` 递归冲突；改 `BR2_JLEVEL=1` 无效（build.sh 每次重写 .config，且 jobserver 错误依旧）。
- 解决：**绕过 `./build.sh rootfs`，直接在 buildroot 目录执行 `make -j1`**。手动 `make -j1 oldconfig` 验证 busybox 本身没问题；顶层 `make -j1` 走完整构建无 jobserver 错误。
- 注意：`./build.sh rootfs` 每次会重跑 `make echo_mate_defconfig` 覆盖 .config 修改（如 BR2_JLEVEL），直接改 buildroot 里的 defconfig 文件才是持久修改。
- 相关文件/命令：`/workspace/Echo-Mate/SDK/rv1106-sdk/sysdrv/source/buildroot/buildroot-2023.02.6` 下 `make -j1`。

## 2026-08-07：DeskBot 交叉编译依赖（jsoncpp/opus/websocketpp）

- 现象：`cmake .. -DTARGET_ARM=ON` 报缺 `opus`、`jsoncpp`（pkg-config）、`WEBSOCKETPP`（find_package）——官方 `echo_mate_defconfig` 没有这些包。
- 排查：`upstream` 的 CMakeLists.txt 确认 AIChat Client 依赖 opus/jsoncpp/websocketpp；YOLO 用自带 opencv-mobile + librknnmrt。
- 解决：
  1. 在 `echo_mate_defconfig` 追加三行：`BR2_PACKAGE_JSONCPP=y`、`BR2_PACKAGE_OPUS=y`、`BR2_PACKAGE_WEBSOCKETPP=y`。
  2. buildroot tarball 自带这三包定义（`package/{jsoncpp,opus,websocketpp}/`），无需新增 recipe。
  3. 重新构建后 sysroot 提供头文件（opus.h、json/json.h、websocketpp/）+ pkgconfig（opus.pc、jsoncpp.pc）。
- 注意：jsoncpp 头文件装在 `usr/include/json/`（不带 jsoncpp 前缀），pkgconfig 的 `-I/usr/include` 正确。
- 相关文件/命令：`sysdrv/tools/board/buildroot/echo_mate_defconfig`；`Demo/DeskBot_demo/toolchain.cmake`（SDK 路径要改成容器内 `/workspace/Echo-Mate/SDK/rv1106-sdk`）。

## 2026-08-07：板端部署自编译 DeskBot 的完整流程

- 现象：如何把交叉编译的 DeskBot 部署到板子并验收。
- 解决（已验证）：
  1. Docker 内编译：`cmake .. -DTARGET_ARM=ON && make -j4`，产物在 `Demo/DeskBot_demo/bin/`（main + model/ + lib/ + 资源文件，19.6MB）。
  2. 从 volume 导出：`docker run --rm -v echo-mate-sdk:/w busybox sh -c 'cd /w/Echo-Mate/Demo/DeskBot_demo/bin && tar czf - .'` → 宿主解包。
  3. 部署：`adb shell "rm -rf /root/bin_old && mv /root/bin /root/bin_old && mkdir -p /root/bin"`，再 `adb push ./ /root/bin/`（先停旧进程）。
  4. 启动：`cd /root/bin && nohup ./main > /tmp/deskbot.log 2>&1 &`，必须从 bin 目录运行（相对路径契约）。
  5. 验收：进程存活、屏幕 UI 正常、触摸可操作；板端自带 libopus/libjsoncpp 运行时库与自编译版本兼容（ldd 无 missing）。
- 注意：自编译版本默认 `AIChat_protocol_version=2`、端口 8000（`system_para.conf`），预编译 250627 是协议 1/端口 8765；连 Server 前核对矩阵。
- 相关文件/命令：`docs/board-bringup.md`；`AGENTS.md`「版本陷阱」。

## 2026-08-07：板子 Wi-Fi 联网配置

- 现象：板子只有 USB 虚拟网卡（usb0=172.32.0.93，macOS 无 RNDIS 驱动用不了），需要走 wlan0 联网。
- 排查：`iw dev wlan0 scan` 能扫到 AP（驱动 r8723bs 正常）；官方 DeskBot UI 的 Wi-Fi 图标只是状态显示，**没有选 SSID/输密码的界面**（源码 `ui_HomePage.c` 中 `ui_WifiLabel`/`ui_NoWifiLabel` 无点击事件，`sys_get_wifi_status()` 仅探测网络可达性）。
- 解决（已验证，2026-08-07，连 LDKJ 成功）：
  1. 写配置（密码不回显，权限 600）：
     ```sh
     adb shell 'printf "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\ncountry=CN\n\nnetwork={\n    ssid=\"LDKJ\"\n    psk=\"密码\"\n    key_mgmt=WPA-PSK\n}\n" > /etc/wpa_supplicant.conf && chmod 600 /etc/wpa_supplicant.conf'
     ```
  2. 连接：
     ```sh
     adb shell 'ip link set wlan0 up; mkdir -p /var/run/wpa_supplicant; wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf; wpa_cli -i wlan0 status'  # 看 wpa_state=COMPLETED
     ```
  3. 获取 IP：`udhcpc -i wlan0`。
  4. 时间同步：`killall ntpd; ntpd -q -p ntp.aliyun.com`（若 `ntpd -q` 报端口被占用，先停已有 ntpd）。
  5. 重启 DeskBot 让 NTP 生效（日志出现 `System time has been successfully updated`）。
- 注意：
  - 板子 IP 是 DHCP 分配的，可能变化；2026-08-07 实测为 `192.168.31.240`，与 Mac 同网段后可 `ssh root@<板子IP>`（密码 root）。
  - wpa_supplicant.conf 开机自动生效（系统有自启脚本）；本次手动启动未加自启，重启后需确认。
  - DNS 实测 `nslookup baidu.com` 可能报 No answer，但 `www.baidu.com` 正常；`/etc/resolv.conf` 指向路由器即正常。
- 相关文件/命令：`docs/board-bringup.md` Wi-Fi 章节；`AGENTS.md`「板端访问」。
