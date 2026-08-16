# meeting_demo —— 会议纪要助手板端 demo（Echo-Mate RV1106）

对接 clare-voice-api（API_DOC v2.0）协议的板端应用：

```
ALSA 采集 16kHz/mono/s16（100ms 帧）
  → 二进制 PCM → /ws/transcribe 持续推流
  → Base64 + JSON → /ws/host 问答
  ← transcription / answer_text（流式）/ answer_audio（24kHz MP3 逐句）
  → minimp3 软解 → 播放队列 → ALSA 24kHz 边收边播
```

## 架构要点：传输层抽象

上层（采集上行、下行回调、状态机）只依赖 `IWsTransport` 接口。
实现按 URL scheme 选择 `asio_client`（ws:// 明文）或 `asio_tls_client`
（wss://，CA 校验）。将来 RTC 路线 = 新增 `RtcTransport` 实现同一接口，
上层零改动。

## 构建 / 部署

```sh
./scripts/build-meeting-demo          # Docker SDK 容器内交叉编译 → out/meeting-demo/
./scripts/deploy-meeting-demo --local-mock     # 部署 + 板端本地 mock + 运行
./scripts/deploy-meeting-demo --server ws://192.168.31.97:8700 --mode full
```

部署脚本会同时更新 `meeting_demo` 与 `meeting-demo-run.sh`；`full` 模式会等
transcribe/host 两条 WS 都打开，再预热 ASR 2 秒并启动采集。弱网预热只发送
一帧静音，避免连续静音 PCM 排在真实语音前面。

控制台：`Enter`=按住提问/再按结束提问  `s`=打断  `q`=退出（优雅收尾）
无人值守：`--auto-host-every N` 每 N 秒自动提问一轮。

参数：`--server ws(s)://HOST:PORT`、`--mode listen|host|full`、
`--cafile PATH`（wss 校验，默认 /root/bin/cacert.pem）、
`--duplex-upload 0|1`（默认 0：播放回答期间暂停转写上行，半双工时序防回声）。

## 屏幕入口（DeskBot 桌面图标）

meeting_demo 本身是控制台程序，不出画面。桌面上给它加图标/页面的代码在
`apps/meeting-demo/deskbot-ui/`（不修改 upstream），构建/部署流程：

```sh
./scripts/build-deskbot-meeting      # 容器内交叉编译带会议入口的 DeskBot → out/deskbot-meeting/
./scripts/deploy-deskbot-meeting     # 备份旧 main → 替换 → 重启 → 验收
```

- 桌面第一页左下角（原 Memo 占位图标位置）出现绿色「会议」图标；
  点击进入 MeetingDemoPage。
- 页面初始状态为「未开始」，显示全宽绿色「开启会议」按钮；
  点击后 fork/exec `/root/meeting_demo/meeting-demo-run.sh`（基础 Demo 默认 listen 模式），
  板端开始录音并推流，partial/final 转写经子进程 stdout 管道实时显示在
  转写文本区（按钮自动隐藏，状态变「运行中」）。
- 基础 Demo 仅保留 `开启会议` 和 `退出`；Host 提问/打断控件暂时隐藏。
  会议进程自行退出后状态变「已停止」，
  「开启会议」按钮重新出现，可再次开启新会议。
- `deskbot-launcher-smoke` 的 PASS 条件为 Session 创建、transcribe WS 打开、
  partial/final、`/end=200`、`rc=0` 且无孤儿进程。
- 服务器地址优先读 `/root/meeting_demo/server.conf`（一行 ws(s)://URL），
  缺省 `ws://192.168.31.97:8700`；板端本地 mock 时写成 `ws://127.0.0.1:8700`。
- 部署包排除 `system_para.conf`：AIChat 令牌等板端配置不被模板覆盖；
  旧 main 自动备份到 `out/deskbot-meeting/rollback/`（adb pull）。
- **字库**：DeskBot 自带 heiti 字库是上游页面裁剪的子集（heiti14 仅 104 个
  汉字），页面文案和转写文本会显示「口」。本应用自带生成字库
  `deskbot-ui/fonts/ui_font_meeting14.c`（GB2312 一级 3755 字 + 全角标点 +
  ASCII，转写文本用）和 `ui_font_meeting22.c`（UI 文案，标题/图标用）。
  重新生成：`deskbot-ui/fonts/` 下运行
  `python3 gen_font_chars.py full > chars.txt`，再
  `lv_font_conv --no-compress --no-prefilter --bpp 4 --size 14 --font SourceHanSansSC-Medium.otf --symbols "$(cat chars.txt)" --format lvgl --lv-include "../../ui.h" -o ui_font_meeting14.c`。
  构建脚本会自动把 LVGL8 风格的 `LV_VERSION_CHECK` 守卫归一化为 LVGL9。

## 板端实机验证（2026-08-16）

- full 模式：转写持续推流 + 自动问答两轮，文本流式显示、MP3 解码播放、
  退出 rc=0、无残留进程。
- 音频：16kHz 采集（板端 amixer 调教见 dev-experience.md）、24kHz 播放
  （ACodec 原生支持，免重采样）。

## 踩坑记录（详见 docs/dev-experience.md）

1. 本 sysroot 的 websocketpp：`config/asio_client.hpp` 定义的是**永远走
   TLS** 的 `asio_tls_client`；明文 ws:// 必须用
   `config/asio_no_tls_client.hpp` 里的 `asio_client`。
2. TLS 配置要求 endpoint 级 `set_tls_init_handler` 必须在 `get_connection`
   **之前**调用，否则连 ws:// 也报 "Required tls_init handler not present"。
3. 非 io 线程直接 `endpoint::send` 会触发 asio reactor 竞态（帧被无限期
   延迟）；所有发送经 `io_service.post` 编组到 io 线程。
4. `client_.run()` 无任务时立即返回（connect 在 start 之后排队会丢）：
   需 io_service work guard 保活。
5. 退出前必须 join 全部线程（含 auto 线程），否则析构未 join 的
   std::thread 会 abort。

## 后续路线（RTC 不丢）

- 当前 demo 走 WebSocket；RTC 作为 `RtcTransport` 新实现接入。
- 火山 RTC SDK 的 armv7-linux 可用性是唯一硬卡点，需单独验证。
