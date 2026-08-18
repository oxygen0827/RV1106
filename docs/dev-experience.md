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
  2. 数字音量 235 左右：`amixer -c 0 cset numid=6 235`、`numid=7 235`。板载麦灵敏度低，185 时 1 米外语音峰值仅 -19 dBFS，ASR 无法识别；235（约 +20dB）时峰值 -6 dBFS 不削波。
  3. 音量步进是 0.5dB/步：185→235 约 +25dB；若环境嘈杂或出现削波可回调，但不要低于 220，否则远距离语音会低于识别门限。
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

## 2026-08-16：板端 wss 客户端联调（websocketpp 四个坑）

- 现象：meeting_demo 的 WS 上行帧时断时续、mock 侧收到帧呈指数延迟；退出时
  `terminate called without an active exception`（rc=134）。
- 排查：
  1. sysroot 的 websocketpp 0.8.2 中 `config/asio_client.hpp` 定义的是
     **永远走 TLS** 的 `asio_tls_client`；连 ws:// 都会做 TLS 握手
     （对明文端口报 "TLS handshake failed"）。明文必须用
     `config/asio_no_tls_client.hpp` 里的 `asio_client`。
     AIChat 客户端就是这样用的（include no_tls 头、用 asio_client）。
  2. TLS 配置要求 endpoint 级 `set_tls_init_handler` 在 `get_connection`
     **之前**调用，否则报 "Required tls_init handler not present"。
  3. 帧指数延迟：非 io 线程直接 `endpoint::send` 与 asio epoll reactor
     存在竞态，异步写注册丢失（板端 Send-Q 积压 62KB、mock 侧收帧间隔
     0.6s→3.6s→13s）。修复：发送统一入队 + `io_service.post` 编组到
     io 线程串行发送（io_service::post 线程安全）。
  4. `client_.run()` 在无任务时立即返回：connect 在 start 之后排队会
     永远不被处理（无 connected 也无 fail 日志）。修复：io_service
     work guard 保活。
  5. 退出 abort：未 join 的 std::thread 析构触发 terminate。所有线程
     （io/capture/playback/auto）退出前必须 join；stdin 用 poll 轮询而
     非阻塞 getline，保证 SIGTERM 能及时响应。
- 相关文件/命令：`apps/meeting-demo/wss_transport.h`、`main.cc`。

## 2026-08-16：RTL8723BS 上行吞吐停滞 + 路由器 deauth（未完全解决）

- 现象：
  1. 板端 TCP 上行（Python 裸 socket 复现）前 ~7.5KB 正常，之后停滞
     （Send-Q 积压、FIN_WAIT1），下行同样 ~1-3KB/s；纯 Python 测试排除
     应用因素。
  2. 尝试 `rmmod r8723bs` 后用 `rtw_power_mgnt=0 rtw_ips_mode=0
     rtw_smart_ps=0` 重载后，路由器反复 `deauth reason 34`，板子短暂
     关联后被踢；恢复原参数（1/1/2）后仍长时间 ASSOCIATING。
- 排查：
  - SDIO 总线健康（4-bit、49.5MHz high-speed），无线统计无重传，
    link quality 100%——不是总线/射频问题。
  - 板子 ARP 能解析路由器（L2 通），但 ICMP/TCP 不通；Mac 中途从有线
    en0 掉线切到 Wi-Fi en1（同 IP），板子↔Mac 互访随即全断，疑似路由器
    客户端表状态错乱（deauth 34 = "unable to handle all currently
    associated STAs"）。
  - `iw set power_save off` 只作用于 mac80211 层，管不到 vendor 驱动
    自己的 PS 状态机（rtw_power_mgnt/rtw_ips_mode/rtw_smart_ps）。
- 待办：
  1. 重启路由器（清客户端表）后复测关联与吞吐；
  2. 若上行仍停滞：再试 `rtw_power_mgnt=0 rtw_ampdu_enable=0` 等参数组合；
  3. 板子目前"associating 不 completed"，用户重启路由器后按
     dev-experience「板子 Wi-Fi 联网配置」重新关联。
- 缓解：demo 开发期间用板端本地 mock（回环 8700），完全绕开 Wi-Fi；
  见 `scripts/deploy-meeting-demo --local-mock`。

## 2026-08-16：板端本地 mock 后端（纯 stdlib WS 服务器）

- 现象：板子没有 aiohttp/websockets，也无外网装包；需要后端协议供
  板端 demo 联调。
- 解决：`mock_server_stdlib.py`（confer-sum/mock-server/），纯标准库
  RFC6455 实现：http.server 单端口承载 HTTP API + WS 升级，手动
  帧编解码（读掩码帧/发未掩码帧）。要点：
  - WS 读写必须经 handler 的 rfile/wfile（raw socket 会丢
    BaseHTTPRequestHandler 缓冲的字节），写完要 flush；
  - WS 结束必须发 close 帧（否则客户端收 1006）并置
    `close_connection = True`；
  - 板端常驻启动：`start-stop-daemon -S -b -x /usr/bin/python3 -- mock_server_stdlib.py ...`。
- 相关文件：`confer-sum/mock-server/mock_server_stdlib.py`、
  `confer-sum/mock-server/client_selftest.py`（全协议自测，Mac 上
  用 .venv 的 websockets 客户端跑，注意 `--noproxy`/ProxyHandler 绕
  本地 Clash 代理）。

## 2026-08-16：Wi-Fi 吞吐参数矩阵与持久化（续）

- 现象：路由器恢复后板子重新关联成功；复测上行不再停滞（此前 deauth 循环
  与停滞均随路由器状态消失），但吞吐被压在 ~6KB/s。
- 参数矩阵实测（板端 TCP 上行 3200B×60 帧，PS=power_mgnt/ips/smart_ps）：
  | 参数 | 上行速率 |
  |---|---|
  | 默认（PS=1/1/2, ampdu=1, wmm=1） | ~6KB/s |
  | PS=0/0/0 | ~6KB/s（仍卡） |
  | PS=0 + ampdu=0, wmm=1 | ~13-14KB/s |
  | PS=0 + ampdu=1, wmm=0 | ~16-20KB/s |
  | PS=0 + ampdu=0, wmm=0 | ~18-23KB/s（最优） |
  | PS=0 + ampdu=0, wmm=0 + ht=0(11g 54M) | ~22-24KB/s |
  | mac80211 `iw set power_save off`（重载后需重设） | 无显著变化 |
  下行 ~77KB/s；上行远低于下行，且上下行不对称，指向驱动 TX 路径。
- 结论与持久化：**WMM 关闭是最大增益，AMPDU 关闭次之**；`iw power_save off`
  只作用于 mac80211 层，管不到 vendor 驱动自己的 PS。
  持久化（2026-08-16 已应用，重载后验证连通）：
  1. `/oem/usr/ko/insmod_wifi.sh`：r8723bs.ko 加载行加
     `rtw_power_mgnt=0 rtw_ips_mode=0 rtw_smart_ps=0 rtw_ampdu_enable=0 rtw_wmm_enable=0`
     （原文件备份 .bak-20260816）；
  2. `/etc/init.d/S99start_echo_defconfig`：wpa_supplicant 后加
     `iw dev wlan0 set power_save off`（原文件备份 .bak-20260816）。
- 应用级实测：meeting_demo listen 模式走 Wi-Fi 上行，有效吞吐 ~6-8KB/s、
  帧积压成批到达（20s 音频需 ~50s 送达）。32KB/s PCM 上行在现有 Wi-Fi
  环境不够；**Opus 16kbps（~2KB/s）压缩上行可留 3 倍余量**，板端 libopus
  已有、AIChat 链路已验证，需软件方在 clare-voice-api 协议上支持 Opus
  解码（新增讨论议题）。

## 2026-08-16：meeting_demo 桌面图标与启动页（DeskBot 集成）

- 现象：meeting_demo 是控制台程序，测试必须走 ADB 前台 shell，屏幕上没有入口。
- 解决：给 DeskBot 桌面加「会议」图标 + MeetingDemoPage：
  1. 新页面 `apps/meeting-demo/deskbot-ui/ui_MeetingDemoPage.{c,h}`（不改 upstream）：
     页面初始「未开始」，点击全宽「开启会议」按钮后 fork/exec
     `/root/meeting_demo/meeting-demo-run.sh`（stdin/stdout 管道），
     读线程收行进环形缓冲，300ms LVGL 定时器刷新转写文本区（流式显示）；
     触屏按钮写 stdin：按住提问=Enter×2、打断=s、退出=q；进程退出后
     按钮重新出现可再开新一轮。
  2. `ui.c.diff`/`ui_HomePage.c.diff` 幂等注册页面、把死掉的 Memo 占位图标
     换成会议图标（MemoPage 从未注册，点它本来就无反应）。
  3. `scripts/build-deskbot-meeting`（容器内交叉编译，导出 tar.gz 排除
     system_para.conf）、`scripts/deploy-deskbot-meeting`（备份/解包/重启/校验）。
- 踩坑：
  1. GNU patch 对 hunk 计数严格（busybox patch 宽松），手写 diff 的
     +行数、后续 hunk 的 +行号都要精确，否则 "malformed patch"。
  2. 板端 busybox tar 无 `-z` 且 `-a` 不解压，解包用
     `gzip -dc file.tar.gz | tar x`。
  3. 板端 rootfs 只剩 ~2MB：旧 main 备份改为 `adb pull` 到 Mac
     （`out/deskbot-meeting/rollback/main.prev-20260816`，SHA
     f60e2d90…，回滚时 push 回去即可）。
  4. 包装脚本没有 exec，进程树是 sh → meeting_demo：只 kill sh 会把
     meeting_demo 变孤儿。子进程必须 `setpgid(0,0)` 自成进程组，
     兜底用 `kill(-pid, SIGTERM/SIGKILL)`。
  5. 退出尾部 `/api/session/end` HTTP 对真实服务端要 5~15s（板端 Wi-Fi
     上行慢 + 服务端收尾转写），页面等待预算只给 4s，超时按组 TERM——
     服务端已收到请求会自行收尾，无残留进程（冒烟测试验证
     group_gone=1 orphan=0）。
- 验证：`apps/meeting-demo/tests/deskbot-launcher-smoke.c`（复刻页面
  启动器链路，板端实测 PASS：会话创建、转写推流、Enter/s/q 控制、
  无孤儿）；桌面绿色「会议」图标经 fb 像素校验（RGB565 大端序，
  0x2BEB=0x2E7D5B）1428 绿像素渲染确认；main SHA 08e55996…。
- 待办：图标点击进入页面的触屏链路需实机点按验收（触摸节点不接受
  userspace 注入，I2C 模拟又依赖 INT 引脚不可行，不做）。
- 相关文件/命令：`scripts/build-deskbot-meeting`、
  `scripts/deploy-deskbot-meeting`、`apps/meeting-demo/deskbot-ui/`。

## 2026-08-16：meeting_demo 页面满屏「口」——DeskBot 字库是子集字库

- 现象：MeetingDemoPage 上线后文案与转写文本大量显示「口」（缺字形方块）。
- 排查：
  1. DeskBot 自带的 heiti14/heiti22 是 SquareLine 按上游页面文本裁剪的
     **子集字库**：CJK 区间（0x4E00-0x9ED2）用的是 SPARSE_TINY 稀疏映射，
     heiti14 实际只有 104 个汉字（日期/节日/天气词），我的「会议纪要/
     按住提问/打断」和转写文本绝大多数字符都不在子集里。全角标点
     （：（）【】）也不在。LVGL 9 对缺字形渲染成「□」。
  2. 生成新字库：lv_font_conv 1.5.2 + Source Han Sans SC Medium
     （与上游黑体同源），字符集 = ASCII + GB2312 区1-9 符号 + 区16-55
     一级汉字 3755 个 + 常用标点，共 4535 字。
     - `ui_font_meeting14.c`（4bpp 14px，~470KB 二进制，转写文本用）
     - `ui_font_meeting22.c`（4bpp 22px，仅 UI 文案，标题/图标用）
  3. lv_font_conv 1.5.2 按 LVGL8 生成：`LV_VERSION_CHECK` 宏 LVGL9 已无，
     且 LVGL9 移除了 dsc 的 `cache` 成员。必须把
     `#if LV_VERSION_CHECK(8,0,0)` 按上下文改写：cache 相关 →
     `LVGL_VERSION_MAJOR == 8`，const 字体定义 → `>= 8`（构建脚本内
     已用 python 归一化，幂等）。
  4. GNU patch 2.7.6 对 `gui_app/ui.h` 的匹配异常（目标行与 hunk 字节级
     一致仍报 Hunk FAILED，原因未明）；ui.h 的字体声明改用 sed 注入。
- 解决后的图标「会议」两字经 fb 像素校验渲染为真实笔画（白色字形像素
  出现在绿色按钮内）；main 从 10.04MB 增至 10.51MB（+470KB 字库）。
- 相关文件/命令：`apps/meeting-demo/deskbot-ui/fonts/`（OTF + 生成脚本
  `gen_font_chars.py`）、`scripts/build-deskbot-meeting`（同步+归一化）。

## 2026-08-16：会议基础链路收口（listen 模式）

- P0 链路固定为：DeskBot 开启会议 → POST Session → transcribe WS →
  ALSA 16kHz PCM → VAD → binary PCM 上行 → partial/final 转写上屏 →
  `q` → `/end=200` → `rc=0` 且无孤儿进程。
- DeskBot 页面默认使用 `MODE=listen`；该模式不再打开播放设备，
  Host WS、MP3 播放和 understanding 均不是开始会议的前置条件。
- VAD 800ms 前视环改为真正的滚动缓存：未满时从尾部追加，满后覆盖
  最旧帧；语音开始时按时间顺序冲刷，避免短停顿后发送空帧或陈旧帧。
- `deskbot-launcher-smoke` 已纳入 CMake/部署包；本地 mock 连续两轮均检查
  Session、WS、partial/final、`/end=200`、`rc=0`、进程组清理和无孤儿。
- 启动时先等待 transcribe WS 打开，再给服务端 2 秒 ASR 预热并发送一帧静音，
  随后启动采集；WS 未连接前上行会丢帧，连续发送 2 秒静音又会在弱网下把
  真实语音挤到 64KB 积压之后，因此不要调整为提前采集或持续静音。
- 页面启动新会议前会清理残留的 `meeting_demo`，避免旧进程持续占用声卡；
  正常退出仍以 `q`、`/end` 和进程组收尾为主。
- 板子重启后约 1 分钟内 DNS 可能尚未就绪，Session 创建会直接失败；当前
  实测 Session 约 2 秒、WS 握手约 3–5 秒。该板 BusyBox `nslookup` 可能恒报
  `No answer`，应以应用使用的 `getaddrinfo` 链路为准。
- Host 问答、流式文本分帧与 Opus 上行全部延后。

## 2026-08-16：移除采集电平可视化，聚焦会议基础链路

- 按当前阶段范围，移除采集线程的 RMS 计算、stderr 旁路、页面柱状显示与
  冒烟诊断；子进程 stderr 合并回 stdout，会议日志仍完整进入页面和测试。
- VAD 保留并继续控制语音 PCM 上行；页面只保留开启会议、实时转写和退出，
  冒烟测试继续覆盖 Session、转写 WS、partial/final、`/end=200` 和无孤儿。

## 2026-08-16：板端 wss/TLS 链路验证矩阵 + 喇叭声学回环

- wss 验证（`apps/meeting-demo/tls_probe`，板端实测）：
  1. `wss://echo.websocket.org` + CA bundle（150 张根证书）→ 握手 + JSON
     echo 往返 PASS；
  2. 自签证书 + CA bundle → 正确拒绝（verify_peer 生效）；
  3. 自签证书作 cafile（自定义 CA 场景，对应公司内网 CA）→ TLS 握手 PASS。
  结论：无论后端用公共 CA 还是内网自签 CA（把证书给到 cafile 即可），
  板端 wss 链路都已就绪。
- 喇叭声学回环：板端 `arecord`(16k) 同时 `aplay` 24kHz PCM，录音与所播
  音频互相关峰值/噪声基线 = **194×**（lag 0.9s 处），证明 MP3 解码→
  24kHz ALSA→喇叭的物理出声链路完整。

## 2026-08-16：真实生产后端（clare.vinex.top/voice-api）联调验证

- 生产地址：`https://clare.vinex.top/voice-api`（Cloudflare + Let's Encrypt
  公共证书，CA bundle 直接可用）；协议为 API_DOC v2.0。
- 板端全链路验证（wss 直连生产）：
  1. session 创建/状态/结束：全部 200，含 analysis_final；
  2. 转写通道：二进制 PCM + VAD 上行，生产火山 ASR 转写回传
     transcript 事件，板端实时显示（"三季度同比增长23%。"逐字中间态
     + 最终态）；understanding 快照轮询正常；
  3. 服务端转写通道有 keepalive ping（超时以 1011 关闭），websocketpp
     会自动回 pong，但上行拥堵时 pong 会延迟 → 已加断线自动重连（2s）；
  4. host 问答：JSON base64 帧在 12-24KB/s 上行下送达过慢，服务器
     关闭握手超时（1006）——生产问答不稳定，**根因仍是上行带宽**。
- 结论：demo 客观目标（与真实后端打通）达成；会议问答的稳定运行需要
  Opus 压缩上行（16kbps ≈ 2KB/s，10 倍余量）——需软件方在协议支持，
  证据链已齐（本文件前述 Wi-Fi 实测 + 本轮生产行为）。
- 板端 DNS 偶发失败（Host not found）——understanding 轮询 5s 周期
  天然重试，可接受。
