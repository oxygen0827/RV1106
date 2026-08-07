# 软件架构与 AI 开发方法

## 系统分层

```text
Echo-Mate hardware
  -> U-Boot / Linux 5.10 / device tree / drivers
  -> Buildroot rootfs + Rockchip media/RKNN libraries
  -> DeskBot native process
       -> LVGL 9.2 + framebuffer/touch
       -> common managers (system/event/GPIO)
       -> pages/apps
       -> AIChat C interface -> C++ client -> WebSocket server
       -> YOLO camera -> opencv-mobile/RKNN
```

RV1106 只有单核 A7 和 256 MB 内存。板端适合显示、触摸、唤醒词、Opus、简单业务逻辑和量化视觉模型；ASR、LLM 和 TTS 默认放在电脑/服务器。后续接入新的大模型时，优先替换 Server 服务，不要先把模型塞进板端。

## 源码导航

`upstream/Demo4Echo/DeskBot_demo/`：

- `conf/dev_conf.h`：`LV_USE_SIMULATOR` 选择仿真/真机。
- `gui_app/pages/`：每个页面近似一个 App；`ui_template` 是最小模板。
- `gui_app/common/`：PageManager 和通用 UI。
- `common/sys_manager/`：亮度、音量、网络、时间、配置。
- `common/event_manager/`：线程间事件队列。
- `utils/system_para.conf`：运行参数模板。
- `CMakeLists.txt`：组合 LVGL、AIChat、YOLO 和运行资源。

`upstream/Demo4Echo/AIChat_demo/`：

- `Client/`：状态机、音频、WebSocket、意图注册和 C 接口。
- `Server/`：鉴权、VAD、ASR、LLM、TTS、任务和 WebSocket 服务。

`upstream/Demo4Echo/yolov5_demo/`：RKNN 模型、RGA、opencv-mobile 采集和后处理。

## 仿真与交叉编译

Ubuntu 22.04 是作者验证环境。纯 UI 先用 SDL 仿真：

1. 将 `conf/dev_conf.h` 中 `LV_USE_SIMULATOR` 设为 `1`。
2. 安装 SDL2、jsoncpp/json-c、Opus、ALSA/PortAudio、Boost/websocketpp。
3. 在 `DeskBot_demo/build` 执行 `cmake .. && make`。
4. 必须进入 `bin/` 运行 `./main`，保持相对资源路径正确。

真机编译：

1. 将 `LV_USE_SIMULATOR` 设为 `0`。
2. 修改 `toolchain.cmake`；当前文件硬编码作者的 `/home/kingham/.../rv1106-sdk`，不可直接复用。
3. 确认 sysroot 已包含 jsoncpp、Opus、ALSA、websocketpp 等目标库。
4. 执行 `cmake .. -DTARGET_ARM=ON && make`。
5. 复制整个 `bin/`，不是只复制 `main`。

SDK 全量构建使用：

```sh
./build.sh lunch
./build.sh
```

板级配置应选择 Echo Mate 的 SD 或 NAND Buildroot 配置。只有改设备树、驱动、Buildroot 包或分区时才需要全量 SDK 构建；普通 UI/业务迭代只交叉编译 DeskBot。

Echo 专用 SDK 入口：

- SD：`project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1106_Echo_Mate-DeskMate.mk`
- NAND：`project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Buildroot-RV1106_Echo_Mate-DeskMate.mk`
- DTS：`sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-echo-mate.dts`
- 共用 dtsi：`sysdrv/source/kernel/arch/arm/boot/dts/rv1106-echo-mate-ipc.dtsi`
- Kernel defconfig：`echo_rv1106_linux_defconfig`
- Buildroot defconfig：`echo_mate_defconfig`

不要在 macOS 默认卷直接构建这套 SDK。Linux kernel 源码有只按大小写区分的路径，macOS 检出已经会出现 Git 伪修改；应在 Ubuntu 22.04 的 ext4/区分大小写卷重新 clone 固定 commit。

## UI 开发约束

- PageManager 使用页面栈；新页面从 `ui_template` 复制最小结构并注册名称。
- 页面进入时启动的 worker，必须在离开/销毁时通知、join 并释放。
- LVGL 对象只能由 UI 线程创建、修改和删除；后台线程发送数据/事件，不直接操作对象生命周期。
- 用固定 240x320 画布和真机字体做布局，不按桌面窗口效果判断最终可读性。
- 先处理无网络、Server 不可达、API 限流、模型缺失等失败态，再做动画和装饰。
- UI 资源、模型和配置使用相对路径，因此运行目录是程序契约的一部分。

## AIChat 协议

客户端连接 Server 时带 Bearer token、Device-Id 和 Protocol-Version；随后发送 `hello`，声明 Opus、16 kHz、单声道和 40 ms 帧。音频二进制头包含 version、type 和 payload size；状态、ASR、VAD、TTS、chat 和 function call 使用 JSON。

推荐把意图能力做成小而明确的函数：

- 名称和参数使用稳定 schema。
- 服务端只决定意图和参数，板端执行前再次校验范围。
- 马达、背光、GPIO 等物理动作设置限幅、超时和取消路径。
- 网络断开时恢复到安全状态，不能让旧命令在重连后补执行。

## 版本与配置风险

当前资料存在明确版本差异：

| 组件 | 预编译 bin 250627 | 当前 Demo4Echo 源码 |
| --- | --- | --- |
| AIChat 端口 | 8765 | 8000 |
| 协议版本 | 1 | 2 |
| 配置文件 | 包内示例 key | 源码模板含看起来可用的 key |

开发时选择一套 client/server commit，并把协议版本、端口、模型版本和配置 schema 一起记录。出现“连不上”时先验证这一矩阵，再排查 Wi-Fi。

上游配置中存在硬编码 API key。它们应视为已泄露：

- 不运行、不复用、不写进新文档。
- 新 Server 从环境变量读取 key。
- 板端只保存必要的短期凭证；更好的方案是天气和 LLM 都由受控 Server 代理。
- access token 使用随机值并限制到可信网络。

## 摄像头与 RKNN

第一阶段保持官方 SC3336 + opencv-mobile + YOLOv5 路径，只验证端到端：打开、抓帧、推理、显示、退出。第二阶段再分别优化：

1. 固定输入分辨率和模型版本，测推理耗时、帧率、CPU 和内存。
2. 将采集从 opencv-mobile 迁到 VI/VPSS，保持模型不变。
3. 再调整 RKNN 模型、量化或摄像头。

当前 dtsi 同时启用了 SC3336、SC4336、SC530AI 三个 I2C4 `0x30` 节点。建立摄像头基线时先按实物只保留一个节点及对应 IQ 文件，再做性能优化。

每次只改一个变量。旧版和 2026 版 Datasheet 对 NPU TOPS 的标注不同，测试报告必须写明 SoC 顶标、规格书版本和实测结果。

## 推荐 vibe-coding 循环

借鉴附件黄山派工程中真正有用的方法，但针对 Linux 重写：

1. **事实层**：保存原理图、设备树、启动日志、设备节点和当前镜像哈希。
2. **最小复现**：先在 SDL 或独立命令验证一个能力，不直接塞进完整 DeskBot。
3. **薄适配层**：把亮度、音频、网络、摄像头封装在 manager，页面不直接写 sysfs/ALSA。
4. **小步部署**：优先 SCP 新 `bin/`；保留上一版可执行目录，失败可立即回退。
5. **证据验收**：记录串口/应用日志、退出状态、内存和重复进入结果。
6. **自动回归**：逐步增加 SSH 脚本，检查进程、设备节点、Server 端口和关键日志。
7. **最后改系统**：只有应用层证据证明需要时，才改 Buildroot、内核或设备树。

## 优先开发路线

### 阶段 A：稳定基座

- 完成 bring-up 表。
- 建立一键交叉编译、SCP、启动和拉日志脚本。
- 把 API key 和网络配置移出源码。
- 给页面生命周期和后台线程增加最小回归。

### 阶段 B：可扩展 App

- 从 `ui_template` 做一个离线状态页。
- 把页面注册、资源和 worker 生命周期形成模板。
- 增加设备状态 JSON/诊断页：温度、内存、存储、网络、Server 可达性。

### 阶段 C：AI 桥接

- 固定 AIChat protocol v2。
- Server 增加健康检查、超时、日志 correlation id 和 mock 模式。
- 先做无物理动作的 function call，再开放背光/马达等受限能力。

### 阶段 D：视觉

- 跑通 SC3336 + 官方 RKNN 模型。
- 建立帧率、延迟、内存基准。
- 再评估 VI/VPSS、OV2685 或自定义模型。

### 阶段 E：产品化

- 看门狗、进程自启动、崩溃恢复、只读根文件系统/数据分区策略。
- 配置迁移、版本显示、离线升级和 SD/NAND 双恢复方案。
- 长稳测试：网络断开、Server 重启、反复页面切换、摄像头/音频重复打开关闭。
