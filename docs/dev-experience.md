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

## 2026-08-08：AIChat 连接成功但 ASR/LLM 持续返回 401

- 现象：板端能够进入 AIChat，Server 日志已有 `Client connected`、鉴权成功和 `hello`，说话后却没有回复；ASR 和 LLM 均返回 HTTP 401。
- 原因：Client 把板端 `aliyun_api_key` 放进 `hello.api_key`，Server 又用它覆盖电脑端的 Zhipu key。当前 `/root/bin/system_para.conf` 中该字段为空，因此所有 Zhipu 请求都使用空凭据。Server 同时缺少 DashScope TTS key，旧实现会静默停在 thinking 状态。
- 修复：
  1. Server 仅从电脑端环境变量 `ZHIPU_API_KEY` 读取 ASR/LLM 凭据，忽略 Client 提供的 key。
  2. TTS 仅从电脑端 `DASHSCOPE_API_KEY` 读取凭据。
  3. ASR、LLM 或 TTS 不可用时向 Client 返回 protocol `error`，由现有状态机退出卡住状态。
  4. Client 后续构建不再发送 `hello.api_key`；当前协议仍为端口 8000、版本 2。
  5. WebSocket 鉴权 token 改由电脑端 `AICHAT_ACCESS_TOKEN` 注入，并与板端配置同步；不再使用示例 token。
- 验证：27 个 `unittest` 覆盖配置边界、模型代码、PCM 完整性、缺凭据不发云请求、ASR/TTS 失败、会话隔离、队列清理、异步 ASR 和真实 loopback WebSocket 鉴权/`hello`；RV1106 交叉编译通过。板端旧 `main` 备份为 `/root/bin/main.before-aichat-fix`。
- 外部依赖：Zhipu ASR/LLM 凭据已从板端和源码移到电脑端私密运行环境；DashScope TTS 凭据仍未配置。不得复用仓库历史中的示例或已泄露 key。

## 2026-08-09：智谱模型实测与长鉴权令牌修复

- 智谱接口实测：产品名 ASR-2512 的 API 模型代码是 `glm-asr-2512`；直接传 `ASR-2512` 会返回 `400 / 模型不存在`。同一份 16 kHz、单声道、16-bit WAV 使用 `glm-asr-2512` 识别成功；`GLM-5.1` 最小对话请求也成功。
- 完整 ASR 服务链曾把板端 `int16` PCM 转为未归一化 `float32`，随后再次乘 32767，导致上传 WAV 溢出失真。现保持 `int16` 样本原值，并用逐样本比较回归锁定。
- 随机鉴权 token 初次使用 48 个字符，但 `AIChatAppInfo_t.token[20]` 只能保留 19 个字符；DeskBot 启动后还会把截断值写回配置。缓冲区已扩为 65 字节，支持 32-64 字符 token，并增加编译期容量断言。
- 最终板端 `main` SHA-256 为 `7eb1f60433e6971329d6fd892ff985fb9d11ce3b54b741a4b55091f7658d531f`；部署前的 `c6438e5b78915e6545367e35edd59c184c12c3dec6801a9c21d7ad71d9007792` 保存在 `/root/bin/main.before-aichat-final`，更早的回滚文件保持不变。最终程序启动后，token 仍保持 48 个字符。
- Server 由用户级 LaunchAgent 常驻，端口 `8000`；`scripts/run-aichat-server` 从权限为 `0600` 的私密环境文件读取凭据，`scripts/sync-aichat-token` 只通过临时文件同步 32-64 字符鉴权 token，并要求 DeskBot 停止后再改配置。
- 最终 ARM Client 实机验证通过 WebSocket 鉴权，并完成 protocol v2 `hello`、函数注册和 idle 状态上报；测试退出后没有残留 Client 进程，DeskBot 已恢复运行。智谱实网回归中，`glm-asr-2512` 将 16 kHz 样本识别为 `Hey, Echo.`，`GLM-5.1` 返回预期短响应；ASR 首次请求曾收到一次供应商 `500`，相同请求重试成功。
- 当前没有 `DASHSCOPE_API_KEY`。ASR 和 LLM 已验证，但语音合成仍不可用；Server 会返回 `tts_unavailable`，Client 回到空闲态，不再停在 thinking。板载喇叭回放唤醒样本未触发 Snowboy，不能替代真人近场唤醒验收；获得完整播报回复仍需配置 TTS provider/key，并再执行真人唤醒、说话和播报回归。

## 2026-08-08：实机内核只映射 128 MB RAM

- 证据：`/proc/iomem` 的 System RAM 为 `0x00000000-0x07ffffff`，设备树 `memory/reg` 同样为 128 MB；启动参数另设 `rk_dma_heap_cma=66M`，运行时 `MemTotal` 约 55 MB、`MemAvailable` 约 30 MB。
- 判断：当前固件只向 Linux 映射 128 MB，和资料标注的 256 MB 不一致。可能是板卡 DDR 版本、bootloader 初始化或设备树内存描述差异，尚不能只凭 Linux 映射断言物理 DDR 容量。
- 与 AIChat 的关系：DeskBot RSS 约 9 MB，未发现 OOM；AIChat 的实际失败证据是 Server 返回 401，因此本次故障不是未插 SD 卡或内存不足。

## 2026-08-09：重插后服务与板端进程复测

- 板子重新插入后 ADB 设备 `f95be6ec9d1c67fa`、Wi-Fi DHCP `192.168.31.240` 和 `/root/bin/system_para.conf`（端口 8000、协议 2、令牌 48 字符）均恢复。
- macOS 上的 AIChat LaunchAgent 重启后继续监听 `0.0.0.0:8000`；私密令牌握手、protocol v2 `hello` 和函数注册回归通过。
- 直接从 ADB/SSH shell 使用 `nohup ./main &` 会在会话退出时被板端清理；使用 `cd /root/bin && start-stop-daemon -S -b -x ./main` 后 DeskBot 可稳定常驻。此前 `/etc/init.d/S99start_echo_defconfig` 的 `start_desk_bot` 调用被注释，因此重启后不会自动启动应用。
- 使用已有 16-bit 音频样本实网调用 `glm-asr-2512` 返回识别结果；`GLM-5.1` 最小对话请求返回 2 字符响应。当前仍未配置 TTS provider/key，进入 ChatBotPage 后可完成 ASR/LLM，但不会播报语音。
- 本轮修复后的 Server 回归为 `32/32` 通过，包含本地 WebSocket 鉴权、缺凭据失败路径、跨会话丢弃、TTS 异步错误和令牌策略；未生成或提交任何云端密钥。

## 2026-08-10：恢复 DeskBot 开机自启动

- 板端 `/etc/init.d/S99start_echo_defconfig` 已恢复调用 `start_desk_bot`；启动函数使用 `start-stop-daemon -S -b -x ./main` 后台拉起程序，并跳过已存在的 `main` 进程，避免 init 阻塞或重复启动。
- 原脚本保留在 `/etc/init.d/S99start_echo_defconfig.pre-autostart-20260810`。重启实测 `main` 自动启动，PID `482`，`fb_st7789v` 和背光正常，framebuffer 显示 DeskBot 主界面。

## 2026-08-09：AIChat 切换 GLM-4-Voice 端到端语音对话

- 服务流程已改为：板端 16 kHz Opus 上行 → Server 解码并由 VAD 划分语音段 → 封装 WAV 调用智谱 `glm-4-voice` → 解析云端 WAV → 重采样为 16 kHz 单声道 PCM → Server 按 40 ms 编码 Opus 下行。主流程不再加载 ASR、独立 GLM-5.1 对话或 DashScope TTS。
- 实测智谱返回为单声道 16-bit、22050 Hz WAV；转换器已覆盖 WAV 校验、单声道归一化和 22050→16000 重采样。音频结束标记改为 `{"type":"voice","state":"end"}`，由音频发送线程在全部 Opus 帧入队后串行发送，避免 Client 提前结束播放。
- Client 状态机改用 `voice processing` 进入 thinking，首个下行 Opus 包进入 speaking，`voice end` 后回到 listening；板端协议仍为端口 `8000`、protocol v2、16 kHz/单声道/40 ms Opus。
- 新增 GLM-4-Voice 请求、WAV/PCM 转换、VAD 分段、会话隔离和 Opus 结束顺序回归；本机 Server 回归为 `13/13` 通过。真实云端调用已确认返回音频格式，但尚未完成本轮板端真人语音完整回归。

## 2026-08-10：ChatBot 显示 App Not exist 的启动覆盖修复

- 现象：板端点击 ChatBot 后立即显示 `AIChat App Not exist.`，看起来像应用文件缺失。
- 根因：`gui_app/ui.c` 的 1 秒维护定时器把 `time_count2` 初始为 `299`，首次回调就执行整份 `system_para.conf` 保存。外部同步的 48 字符 AIChat 鉴权令牌会在启动后被旧内存值覆盖，随后 WebSocket 鉴权失败并进入停止状态；UI 将该状态误报为 App 不存在。
- 修复：计数器初始值改为 `0`，首次配置持久化延后至完整的 5 分钟周期；仅替换板端 `/root/bin/main`，原二进制保留为 `/root/bin/main.before-timer-fix`。
- 后续排查先做三项脱敏检查：`pidof main` 确认进程存在；读取 `AIChat_server_url`、端口、协议和令牌长度；比较电脑端与板端令牌哈希。若启动前后哈希变化，优先检查配置保存时机，而不是先怀疑内存或云端模型。
- 恢复步骤：停止 `main` 后再运行 `scripts/sync-aichat-token` 同步令牌，使用 `start-stop-daemon -S -b -x ./main` 从 `/root/bin` 启动；启动后等待数秒复查配置，确认令牌长度和哈希未变化，再进入 ChatBot 页面。
- 验证：修复版 ARM `main` SHA-256 为 `f60e2d905010cf24388bc30b8bf20ba28c6b1e56fc4ba65fa6c60ef9158aae73`；板端启动后令牌长度仍为 48，电脑端/板端令牌哈希一致；真实 WebSocket `Authorization`、`Device-Id`、`Protocol-Version` 握手及 `hello`/idle 消息通过；Server 回归 `8/8` 通过。

## 2026-08-10：GLM-4-Voice 第二轮对话卡住

- 现象：首句需要等待唤醒提示音和云端处理，能够回复；继续问第二句后无回复，客户端看起来卡住。
- 排查：Server 日志显示首轮 `GLM-4-Voice response queued` 正常；第二轮返回 HTTP 400，错误为 `assistant 对话中 audio.id 不能为空`，随后客户端回到 idle。唤醒时的多次短提示音来自板端固定播放资源 `AIChat_demo/Client/third_party/audio/waked.pcm`，不是多次云端请求。
- 原因：`VoiceModel` 只保存 assistant 文本，丢弃了 GLM-4-Voice 返回的 `audio.id`；该字段是下一轮多模态历史的必需字段。
- 解决：每轮只保存 assistant 音频的 `id`，不重复发送历史音频的 base64 数据；响应缺少 `audio.id` 时返回明确的无效音频错误。
- 验证：新增双轮历史回归测试；Server 测试共 `16/16` 通过。重启电脑端 LaunchAgent 后监听 `0.0.0.0:8000`，板端 `main` 已重新启动，等待进入 ChatBot 页面执行真人双轮语音验收。

## 2026-08-13：摄像头未响应与 RGA 裸地址崩溃的隔离

- 现象：Echo-Mate 固件列出 `/dev/video*`、CIF/ISP 和多个摄像头模块，但 `v4l2-ctl --list-devices` 没有 sensor entity；加载 `sc3336.ko` 后芯片 ID 为 `0x000000`、I2C 返回 `-EIO`，`/dev/video0` 无法打开。
- 排查：普通重启、I2C4 全地址扫描、三个 SC 驱动的运行时绑定检查均失败；`/dev/video11` 直接取流返回 `VIDIOC_STREAMON ... Invalid argument`，内核提示 `check rkisp_mainpath link or isp input`。桌面上的 IMX415 包只含用户态录像脚本，明确不含目标板 DTS/IQ 文件；Echo SDK 虽含通用 `imx415.c`，当前 Echo-Mate DTS 没有 IMX415 节点，不能直接套 Luckfox 配置。
- 原因：当前摄像头在 I2C/供电/排线/实际型号确认之前没有响应；不是 YOLO 模型或 NPU 缺少运行库。三个候选 SC 节点共用 `0x30` 只是设备树声明，启动脚本会卸载未探测到的驱动。
- 解决：新增 `scripts/board-driver-test`，把屏幕、触摸、音频、Wi-Fi、蓝牙、NPU、RGA、视频编解码、NAND 和 camera media graph 变成可重复审计。新增独立 `apps/yolo-smoke` 和构建/部署脚本，把静态图推理与摄像头解耦；预处理固定使用 CPU 双线性缩放，因为上游 image_utils 将普通虚拟地址交给 RGA3 会触发 `rga_mm_map_buffer` 内核空指针异常。
- 验证：驱动审计 `11 passed, 1 failed, 1 warning`（唯一失败 camera）；静态 YOLOv5 RKNN 三次推理成功，平均 `81.558 ms`、`12.261 FPS`，`bus.jpg` 检出 5 个目标，DeskBot 测试后恢复；没有刷写 NAND 或修改设备树。
- 下一步：断电后核对摄像头丝印、MIPI FPC 方向和供电；重新上电后运行 `scripts/board-driver-test`。只有 sensor entity、`/dev/video0` 和 30 帧取流通过，才进入实时 YOLO/DeskBot 页面；若实物确为 IMX415，再单独建立 Echo-Mate 的 IMX415 DTS/IQ 适配并在 SD 恢复介质上验证。
