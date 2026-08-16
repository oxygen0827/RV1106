# meeting_demo —— 会议纪要助手板端 demo（Echo-Mate RV1106）

对接 clare-voice-api（API_DOC v1.0）协议的板端应用：

```
ALSA 采集 16kHz/mono/s16（100ms 帧）
  → Base64 + JSON → WS 上行（/ws/transcribe 持续推流；/ws/host 问答）
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

控制台：`Enter`=按住提问/再按结束提问  `s`=打断  `q`=退出（优雅收尾）
无人值守：`--auto-host-every N` 每 N 秒自动提问一轮。

参数：`--server ws(s)://HOST:PORT`、`--mode listen|host|full`、
`--cafile PATH`（wss 校验，默认 /root/bin/cacert.pem）、
`--duplex-upload 0|1`（默认 0：播放回答期间暂停转写上行，半双工时序防回声）。

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
