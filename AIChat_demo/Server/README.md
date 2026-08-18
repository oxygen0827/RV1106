## AI语言助手demo(Server端)

### 环境搭建

这里默认大家都是用的自己的电脑搭建服务，默认同学们都没有GPU（有就更好）

首先创建虚拟环境, 不然容易污染你的系统环境, 作者使用的python3.10。环境名字就起名`AIChatServerEnv`好了，环境名可自定义。

``` sh
cd ./your-path
conda create --prefix ./AIChatServerEnv python=3.10
```

然后启动虚拟环境，并安装所需要的包，如果下载不了需要科学上网

``` sh
conda activate ./AIChatServerEnv
pip install -r ./requirements.txt
```

当前服务端使用智谱 `GLM-4-Voice` 端到端语音模型。服务端只负责 VAD 分段、WAV 封装、云端调用以及将返回音频重采样为板端格式；不再经过独立 ASR、LLM、TTS 三段链路。云端凭据只允许在运行 Server 的电脑上注入，Client 的 `hello` 消息不能设置或覆盖 Server 凭据。

ASR 测试模式：设置 `AICHAT_MODE=asr` 和 `ZHIPU_API_KEY` 后启动服务端，客户端命令末尾追加 `asr`。服务端用 `glm-asr-2512` 转写每个 VAD 语音段，只向板端返回 JSON 文本，不播放 TTS。

ASR 成功回包示例：`{"type":"asr","state":"text","text":"..."}`，随后发送 `{"type":"asr","state":"end"}`。

启动前通过私密环境配置提供：

- `AICHAT_ACCESS_TOKEN`：Client/Server 共享的 32-64 字符随机访问令牌，必需；不得使用示例值。
- `ZHIPU_API_KEY`：ASR 或 GLM-4-Voice 云请求必需。
- `ZHIPU_ASR_MODEL`：ASR 模式可选，默认 `glm-asr-2512`。
- `AICHAT_MODE`：可选，`voice`（默认）或 `asr`。
- `ZHIPU_VOICE_MODEL`：可选，默认 `glm-4-voice`。
- `ZHIPU_VOICE_HISTORY_TURNS`：可选，保留的多模态对话轮数，默认 `3`。

不要把实际 key 写入源码、`system_para.conf`、日志或启动命令。使用本机的私密环境配置、进程管理器或 secret store 注入，然后运行：

``` sh
python ./main.py
```

板端配置必须与 Server 成套：当前源码使用端口 `8000`、协议版本 `2`。旧预编译包使用端口 `8765`、协议版本 `1`，不能混用。

回归测试：

```sh
python -m unittest discover -s test -p 'test_*.py' -v
```

### 文件目录介绍

```sh
Server/
├── config/                # 全局设置
├── handle/                # ws接收内容的处理
|   ├── audio_handle.py    # 音频数据处理
|   ├── auth_handle.py     # 鉴权
│   └── text_handle.py     # 文本数据处理
├── models/                # 
├── services/              # 
├── test/                  # 单功能测试
├── threads/               # 多线程相关
├── tools/                 # 工具
|   ├── audio_processor.py # 音频处理
|   ├── logger.py          # log
│   └── registry.py        # 意图注册
├── ws_server.py           # websocket server 业务
├── service_manager.py     # services 全局管理
└── main.py
```

### WebSockets协议说明

以下是Server端会向Client端发送的信息:

1. 鉴权信息：

   ```json
   {
      "type": "auth",
      "message": "Authentication failed" 
   }
   ```
   "message"还包括: "Client authenticated"

2. VAD检测到说话的活跃状态

   ```json
   {
      "type": "vad",
      "state": "no_speech" 
   }
   ```
   "state"还包括: "end", "too_long"

3. GLM-4-Voice 会话状态

   ```json
   {
       "type": "voice",
       "state": "processing"
   }
   ```

   `state` 还包括 `no_speech`、`text` 和 `end`。`text` 仅用于日志/调试，语音回复通过后续二进制帧发送。

4. 语音回复完成

   ```json
   {
      "type": "voice",
      "state": "end"
   }
   ```
   该消息在所有 Opus 音频帧入队后发送，Client 可据此结束播放并进入下一轮 listening。

5. 对话结束

   ```json
   {
      "type": "chat",
      "dialogue": "end"
   }
   ```
   "state"还包括: "continue"


6. 打包发送的音频数据

   ```python
    version: 协议版本 (2 字节)
    type: 消息类型 (2 字节)
    payload: 16 kHz、单声道、16-bit PCM 按 40 ms 切片后的 Opus 负载 (字节)
   ```
