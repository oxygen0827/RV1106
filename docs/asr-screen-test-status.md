# GLM ASR 屏幕测试变更记录

最后更新：2026-08-19

## 1. 当前状态

| 项目 | 状态 | 证据 / 说明 |
| --- | --- | --- |
| 功能目标 | 已实现代码接入 | 板端录音、Opus 上行、服务端 VAD、GLM ASR、屏幕显示 |
| GLM 模型 | 已配置代码默认值 | `glm-asr-2512`，可由 `ZHIPU_ASR_MODEL` 覆盖 |
| API Key | 已在临时服务进程中配置 | 仅存在于服务端进程环境，未写入源码、配置或日志；测试结束后删除 |
| 屏幕入口 | 已接入 ChatBot 页面 | `DeskBot_demo/gui_app/pages/ui_ChatBotPage` |
| TTS / 会议总结 | 未实现 | 本次只覆盖 ASR 测试 |
| 提交分支 | `codex/fix-aichat-credentials` | 当前 Git 分支 |
| 用户远端仓库 | `https://github.com/oxygen0827/RV1106.git` | Git remote `userrepo` |

## 2. 变更内容

### 服务端

- `AICHAT_MODE=asr` 选择 ASR 流程，默认模式仍为 `voice`。
- VAD 结束后把 16 kHz、单声道 PCM 封装为 WAV，调用智谱音频转写接口。
- 返回 `{"type":"asr","state":"text","text":"..."}`，随后返回 `state=end`。
- 缺少 API Key、空音频或请求失败时返回错误并结束当前语音段。

### 板端客户端

- `Application` 增加 ASR 模式和转写文本队列。
- WebSocket 收到 ASR 文本后写入队列，状态机回到待机。
- C 接口增加 `get_aichat_asr_text()`，供 LVGL 线程安全读取。

### 屏幕 UI

- ChatBot 页面以 ASR 模式启动 AIChat。
- 页面底部新增文本面板，显示 `ASR: <转写内容>`。
- LVGL 定时器在 UI 线程刷新，网络线程不直接操作 LVGL 对象。

## 3. 配置与运行

在运行服务端的电脑上配置，不要把 API Key 写入板端配置文件：

```sh
cd /Volumes/ML/vibe-coding/RV1106/upstream/Demo4Echo/AIChat_demo/Server
export AICHAT_ACCESS_TOKEN='<32-64位随机token>'
export ZHIPU_API_KEY='<智谱API Key>'
export AICHAT_MODE=asr
python3 main.py
```

板端 `system_para.conf` 需要填写服务端地址、端口 `8000`、相同的 `AICHAT_ACCESS_TOKEN` 和协议版本 `2`。启动 `/bin/main` 后进入主界面的 `ChatBot` 页面测试。

访问令牌与 GLM API Key 是两个独立凭据：

- `ZHIPU_API_KEY` 只配置在 Mac 服务端，用于请求智谱 GLM ASR。
- `AICHAT_ACCESS_TOKEN` 同时配置在服务端和板端，用于板端 WebSocket 鉴权。
- 修改板端 `/root/bin/system_para.conf` 后，需要重启 `/root/bin/main`，因为 GUI 只在启动时加载配置。

本次板端通过 USB ADB 连接，不会出现 Windows 风格的 `COMx`。macOS 检测到 Rockchip USB 设备后，可使用 `adb devices -l` 和 `adb shell` 管理板端；只有连接独立的 3.3 V USB-TTL 模块时，才会出现 `/dev/cu.usbserial-*` 一类串口设备。

## 4. 验证记录

| 验证项 | 结果 | 限制 |
| --- | --- | --- |
| Python AST 语法检查 | 通过 | 只覆盖源码语法，不代表云端请求成功 |
| Git diff 检查 | 通过 | 当前工作区曾存在批量权限噪声，已在提交前清理 |
| 客户端 CMake 配置 | 未完成 | 本机缺少 `pkg-config` |
| DeskBot 编译 | 未完成 | `DeskBot_demo/lvgl/lvgl.h` 所在 LVGL 子模块未检出 |
| 板端 ADB | 通过 | Mac 可通过 USB ADB 进入 Echo Mate 板端 |
| 板端 Wi-Fi | 通过 | `wlan0` 已获取局域网地址，默认路由和 DNS 正常 |
| 板端到 Mac | 通过 | 板端可访问 Mac `192.168.31.97` |
| ASR 服务端口 | 通过 | 板端访问 `192.168.31.97:8000` 收到 WebSocket `426 Upgrade Required` |
| WebSocket 客户端上行 | 通过 | 服务端捕获到板端 ChatBot 连接请求 |
| WebSocket 鉴权 | 已修复待复测 | 首次实测因板端内存仍使用旧访问令牌失败；已同步配置并重启 GUI |
| GLM 实际转写 | 待完成 | 重启后需要重新进入 ChatBot，说一段真实语音并确认日志和屏幕文本 |

## 5. 风险与后续

- ASR 是“VAD 分段后提交”的转写，不是逐字流式显示。
- 主界面的 Wi-Fi 图标只在启动时和每 5 分钟检查一次外网；DHCP 晚于 GUI 启动时会暂时误报无 Wi-Fi。
- `AIChat App Not exist.` 目前也会在客户端鉴权失败、连接线程退出时出现，不代表应用文件真的缺失。诊断时应以服务端日志为准。
- `ui_font_heiti22` 只包含工程已生成的字符集合，极少数字符可能显示为空框，需要时补充字体字形。
- 构建前必须检出 LVGL 子模块，并准备 RV1106 交叉编译依赖。
- 下一阶段可在 ASR 文本队列之后接入会议摘要，再将摘要送入 TTS；本次没有改动 TTS 链路。

## 6. 回滚

回滚本次提交即可移除 ASR 模式、文本队列和屏幕转写面板；服务端默认 `voice` 模式的原有入口保留。正式回滚前应重新编译并在板端验证 ChatBot 页面和网络连接。
