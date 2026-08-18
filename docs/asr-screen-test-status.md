# GLM ASR 屏幕测试变更记录

## 1. 当前状态

| 项目 | 状态 | 证据 / 说明 |
| --- | --- | --- |
| 功能目标 | 已实现代码接入 | 板端录音、Opus 上行、服务端 VAD、GLM ASR、屏幕显示 |
| GLM 模型 | 已配置代码默认值 | `glm-asr-2512`，可由 `ZHIPU_ASR_MODEL` 覆盖 |
| API Key | 待运行环境配置 | 仅从服务端 `ZHIPU_API_KEY` 环境变量读取，不写入板端 |
| 屏幕入口 | 已接入 ChatBot 页面 | `DeskBot_demo/gui_app/pages/ui_ChatBotPage` |
| TTS / 会议总结 | 未实现 | 本次只覆盖 ASR 测试 |
| 提交分支 | `codex/fix-aichat-credentials` | 当前 Git 分支 |
| 远端仓库 | `https://github.com/No-Chicken/Demo4Echo.git` | Git remote `origin` |

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

## 4. 验证记录

| 验证项 | 结果 | 限制 |
| --- | --- | --- |
| Python AST 语法检查 | 通过 | 只覆盖源码语法，不代表云端请求成功 |
| Git diff 检查 | 通过 | 当前工作区曾存在批量权限噪声，已在提交前清理 |
| 客户端 CMake 配置 | 未完成 | 本机缺少 `pkg-config` |
| DeskBot 编译 | 未完成 | `DeskBot_demo/lvgl/lvgl.h` 所在 LVGL 子模块未检出 |
| GLM 实际请求 | 未验证 | 当前运行环境是否有有效 `ZHIPU_API_KEY` 待确认 |
| RV1106 上板验证 | 未验证 | 未提供本次测试的板端日志或硬件验证记录 |

## 5. 风险与后续

- ASR 是“VAD 分段后提交”的转写，不是逐字流式显示。
- `ui_font_heiti22` 只包含工程已生成的字符集合，极少数字符可能显示为空框，需要时补充字体字形。
- 构建前必须检出 LVGL 子模块，并准备 RV1106 交叉编译依赖。
- 下一阶段可在 ASR 文本队列之后接入会议摘要，再将摘要送入 TTS；本次没有改动 TTS 链路。

## 6. 回滚

回滚本次提交即可移除 ASR 模式、文本队列和屏幕转写面板；服务端默认 `voice` 模式的原有入口保留。正式回滚前应重新编译并在板端验证 ChatBot 页面和网络连接。
