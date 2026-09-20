# 摄像头与 YOLO 上板验收

本流程针对 Echo-Mate RV1106、GC1084 摄像头和仓库内的 YOLOv5 RKNN
模型。模型/NPU 验证与摄像头验证分开进行，避免把 I2C/CSI 故障误判为
RKNN 故障。

## 摄像头型号确认（2026-08-14）

实际安装的摄像头模组为 **GC1084-C31YA**（GalaxyCore），不是之前误判的
SC3336 或 OV2685。依据：

- 原厂 datasheet（SHSAE-1M-3040 GC1084-C31YA CSP Datasheet V1.1）封面、
  正文和寄存器列表均标注 GC1084。PDF 元数据标题误写为 `GC8054 Datasheet`，
  属于制作疏忽，不代表实际型号。
- 芯片 ID 寄存器 `0x03f0/0x03f1` 期望值 `0x1084`，实机读取成功。
- 寄存器初始化列表 `release..v1.txt` 与 SDK `gc1084.c` 驱动 mode table 一致
  （113 项，仅 `0x03f8` 和 `0x0d06` 两项微调）。

### GC1084 关键参数

| 参数 | 值 |
| --- | --- |
| 分辨率 | 1280×720 |
| 输出格式 | GRBG RAW10 |
| I2C 7-bit 地址 | 0x37（写 0x6e / 读 0x6f） |
| ID 寄存器 | 0x03f0/0x03f1 = 0x1084 |
| MCLK | 27 MHz |
| MIPI | 1-lane，396 Mbps（驱动配置 400 Mbps） |
| XSHUTDOWN | 低电平待机，高电平工作 |
| 电源 | AVDD 2.8V，DVDD 1.8V，VDDIO 1.8V |

### FPC 接口定义（22-pin）

**权威来源已核对（2026-08-15）**：摄像头 FPC 接口位于 **Core 核心板**（不是
Drive 驱动板），权威原理图 `reference/echo-mate/Core-sch.pdf`。原理图 FPC
连接器标注为 `CAMERA`（兼容标注 `SC3336 & OV2685`，即此前文档所述"接口
兼容性标注，不代表实际安装型号"），信号名与下表完全一致。

| Pin | Signal | Pin | Signal |
| --- | --- | --- | --- |
| 1 | VCC_3V3 | 12 | GND |
| 2 | VCC_3V3 | 13 | MIPI_CLKN |
| 3 | GND | 14 | MIPI_CLKP |
| 4 | GND | 15 | GND |
| 5 | MIPI_RST | 16 | MIPI_D0N |
| 6 | GND | 17 | MIPI_D0P |
| 7 | MIPI_IIC_SDA | 18 | GND |
| 8 | MIPI_IIC_SCL | 19 | MIPI_CLK0 |
| 9 | GND | 20 | GND |
| 10 | MIPI_D1N | 21 | GND |
| 11 | MIPI_D1P | 22 | connector shield/GND |

SDA/SCL 由 R47/R46 各通过 4.7 kΩ 上拉到 VCC_1V8。GC1084 使用 1-lane MIPI，
Pin 10/11（D1）实际未用。FPC Pin 5 `MIPI_RST` 接 GC1084 的 XSHUTDOWN，
`GPIO3_PC5` active-high pwdn 方向正确。

**时钟澄清**：原理图标注 24MHz 是 RV1106 的 24MHz 主晶振；gc1084 驱动
`clk_set_rate(xvclk, 27000000)` 经 CRU `MCLK_REF_MIPI0` fracmux 倍频输出
27MHz MCLK 给传感器。两者不矛盾，此前文档"27 MHz"正确。

**黑白根因（2026-08-15 原理图核对后确认）**：Core 板上**没有 IR-CUT
滤镜控制电路**，摄像头接口是按彩色传感器（SC3336/OV2685 均为彩色）设计的，
硬件支持彩色。屏幕/ISP 输出黑白纯粹是软件问题——Luckfox 的 IQ 文件
`gc1084_SV-SMSM50S_80IR-F20` 是 80IR 红外镜头标定，`colorAsGrey.enable=1`
强制灰度输出。DeskBot 的 AI 相机画面（摄像头→ISP→LCD）同样经过该 IQ 文件，
所以屏幕显示黑白。修复需换可见光标定的 IQ 文件，或换非 IR 版摄像头模组。

## 错误原因与解决方案

### 错误 1：传感器型号误判

- **现象**：原厂固件在 DTS 中同时声明 SC3336、SC4336、SC530AI 三个 I2C4
  地址 `0x30` 的节点，三个驱动均未绑定，media topology 没有 sensor entity。
- **原因**：实际摄像头是 GC1084（I2C 地址 0x37），不是 SC 系列（0x30）。
  原理图标注的 `SC3336 & OV2685` 是接口兼容性标注，不代表实际安装型号。
- **解决**：DTS 替换为 `gc1084@37` 单节点，`compatible = "galaxycore,gc1084"`，
  `data-lanes = <1>`，`GPIO3_PC5 GPIO_ACTIVE_HIGH` pwdn。

### 错误 2：缺少内核配置

- **现象**：启用 `CONFIG_VIDEO_GC1084=m` 后首次构建未生成 `gc1084.ko`。
- **原因**：GC1084 Kconfig 依赖 `VIDEO_V4L2_SUBDEV_API`（`depends on`），
  而 SC3336 用 `select` 自动启用该选项。仅添加 `CONFIG_VIDEO_GC1084=m`
  不会自动满足依赖。
- **解决**：在 defconfig 中显式添加 `CONFIG_VIDEO_V4L2_SUBDEV_API=y`。

### 错误 3：缺少 RKAIQ IQ 文件

- **现象**：SDK 不含 GC1084 的 RKAIQ IQ 标定文件。
- **解决**：从 Luckfox RV1106 公开仓库获取 `gc1084_SV-SMSM50S_80IR-F20.json`
  和 `.bin`，同芯片 GC1084 标定，模组/镜头不同但可用于 bring-up。
  正式产品需对实际模组单独标定。

## 实机验证结果（2026-08-14）

### 传感器探测

- `gc1084.ko` 加载后，I2C4 地址 0x37 成功读取芯片 ID `0x1084`。
- media graph 出现 `m00_b_gc1084 4-0037` sensor entity。
- DPHY 数据率 800 Mbps，CSI2 stream ON/OFF 正常。
- `gc1084_s_stream: on: 1, 1280x720@30` 确认传感器进入流式传输。

### RAW10 帧捕获

- 通过 CIF `/dev/video0`（BA10 格式）捕获 RAW10，每帧 1290240 字节
  （stride=1792 × 720 行），10-bit 值范围 0-1023，bright 场景 mean≈241、
  高光饱和到 1023。
- 帧间差异 mean=125.95，确认是真实场景的实时拍摄，非静态噪声。
- **更正（2026-08-14 复查）**：之前"21/30 帧后断流"是 **tmpfs 写满假象**。
  板端 `/tmp` 是 27MB tmpfs，30 帧 RAW=38.7MB 会在 ~22 帧处写满，v4l2-ctl
  后续帧写盘失败。改用 `--stream-to=/dev/null` 抓 60 帧零错误、33.75 FPS
  连续稳定，传感器/MIPI/CIF DMA 实际完好。

### ISP 路径

- `rkaiq_3A_server` 成功初始化 ISP 引擎（media1 = rkisp，主路径
  `/dev/video11` = mainpath，Multiplanar），加载 GC1084 IQ 文件。
- 数据通路正常：dmesg 显示 sensor stream on、DPHY 800Mbps、CIF
  `link_mode 1`（桥接到 ISP）、rkisp 输入 `SGRBG10_1X10 1280x720@30`。
- **更正**：`dma_en 0x0` 是 ISP 桥接模式的正常状态（CIF 不写内存直接喂
  ISP）；"输出全零"是 **AE 未收敛**——3A 从黑帧起，约 15 帧（0.5s）后
  收敛到可见图像（Y 从 0 升到 ~22）。之前只抓 10 帧，没等到收敛。
- `rkisp_demo`（进程内 rkaiq）会段错误（rc=139），但 3A server +
  v4l2-ctl 路径已足够，无需 rkisp_demo。

### 实时 YOLO（2026-08-14 验收）

- `apps/camera-yolo` 端到端跑通：V4L2 抓 `/dev/video11` NV12 →
  RGA letterbox 转 640×640 RGB888 → RKNN yolov5 推理 → 后处理，
  连续 100 帧 **10.9 FPS**（≈92ms/帧），退出后资源正常释放。
- 当前输出偏暗（Y≈22）且灰度（U=V=128，AWB/CCM 未生效），检测到 0 目标；
  属 IQ 标定问题（Luckfox 通用 IQ 与本模组/镜头不匹配），非管线故障。
  正式产品需对实际模组单独标定 RKAIQ IQ 文件。

### 灰度根因（已查证）

画面黑白不是管线故障，而是 Luckfox 的 IQ 文件本身就是给**红外/黑白监控
摄像头**标定的（文件名 `80IR` = 8mm 红外镜头）。其 `scene_isp32` 段内：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `colorAsGrey.param.enable` | **1** | 强制"彩色转灰度"（每 10 帧） |
| `wb_v32.control.byPass` | **1** | AWB 白平衡被旁路 |
| `ccm_calib_v2.control.enable` | **0** | CCM 色彩矩阵关闭 |
| `cproc.param.enable` | **0** | 色彩处理（饱和度）关闭 |

实测：把上述开关改回彩色模式（`colorAsGrey=0`、`byPass=0`、`ccm=1`、
`cproc=1`）后输出变成**全黑**——因为该文件的 AWB/CCM 数据是按红外光照
标定的，对可见光是退化的（增益/矩阵把图像算成 0）。所以**不能靠改开关
恢复彩色**，必须用本模组在可见光下的标定文件替换。原始灰度配置反而是
这份 IR 标定能稳定出图的"安全"状态。

## SDK 变更（分支 codex/gc1084-camera）

所有修改在独立工作树 `/workspace/Echo-Mate-gc1084` 的 `codex/gc1084-camera`
分支上，不修改上游 pinned commit。

### DTS（rv1106-echo-mate-ipc.dtsi）

- 删除 SC3336、SC4336、SC530AI 三个节点和对应的 DPHY input1/input2。
- 新增 `gc1084@37` 节点：`compatible = "galaxycore,gc1084"`，`reg = <0x37>`，
  `data-lanes = <1>`，`pwdn-gpios = <&gpio3 RK_PC5 GPIO_ACTIVE_HIGH>`。
- DPHY input0 的 `data-lanes` 从 `<1 2>` 改为 `<1>`。

### defconfig（echo_rv1106_linux_defconfig）

- `CONFIG_VIDEO_SC3336/SC4336/SC530AI=m` 替换为 `CONFIG_VIDEO_GC1084=m`。
- 新增 `CONFIG_VIDEO_V4L2_SUBDEV_API=y`（GC1084 Kconfig 依赖）。

### BoardConfig（SD_CARD 和 SPI_NAND）

- `RK_CAMERA_SENSOR_IQFILES` 改为 `gc1084_SV-SMSM50S_80IR-F20.json`。
- `RK_CAMERA_SENSOR_CAC_BIN` 注释掉（GC1084 无 CAC 标定）。

### insmod_ko.sh

- `sc4336.ko`、`sc3336.ko`、`sc530ai.ko`、`ov2685.ko` 替换为 `gc1084.ko`。
- `sensor_height` 改为 720。

## 部署与回滚

### 部署内容

| 文件 | 板端路径 | 说明 |
| --- | --- | --- |
| boot.img | mtd3（flashcp 刷写） | 含新内核 + GC1084 DTB |
| gc1084.ko | /oem/usr/ko/ | 内核模块 |
| insmod_ko.sh | /oem/usr/ko/ | 替换 SC 加载为 GC1084 |
| gc1084 IQ .json/.bin | /oem/usr/share/iqfiles/ | RKAIQ 标定文件 |

### 回滚

原始 boot 分区已备份：

- 宿主：`out/gc1084-build/boot-backup-original.img`（4MB，md5: ed3e53...）
- 回滚命令：`adb push out/gc1084-build/boot-backup-original.img /tmp/ && adb shell 'flashcp /tmp/boot-backup-original.img /dev/mtd3'`
- 原始 `insmod_ko.sh` 备份在板端 `/oem/usr/ko/insmod_ko.sh.bak`。

## 构建命令

```sh
# 在宿主执行，使用 Docker SDK 容器
./scripts/echo-sdk exec bash -lc 'cd /workspace/Echo-Mate-gc1084/SDK/rv1106-sdk && ./build.sh kernel'
```

构建产物在 `out/gc1084-build/`：`gc1084.ko`、`rv1106g-echo-mate.dtb`、
`boot.img`、`boot-backup-original.img`、`gc1084-30frames-raw10.raw`。

## 下一步

1. ~~排查 MIPI 链路稳定性~~ 已证伪（tmpfs 写满假象，链路实际稳定）。
2. ~~ISP 通路稳定后获取高质量 YUV 帧~~ 已打通（AE 收敛后出真实帧）。
3. 对实际模组单独标定 RKAIQ IQ 文件：当前输出偏暗（Y≈22）且灰度
   （AWB/CCM 未生效），需用本模组的标定替换 Luckfox 通用文件。
4. 实时 YOLO 验收（已跑通 10.9 FPS）：把摄像头对准可识别目标（人/常见
   物体）复测检测结果，验证标定后的彩色图像检测精度。
5. 内存预算：板端仅 54MB，YOLO 运行期可用内存 ~10-14MB，若叠加其它
   功能需裁剪模型或释放 DeskBot 等常驻进程。

## 板端环境陷阱（实测确认）

| 陷阱 | 结论 |
| --- | --- |
| `/data`(=userdata) 仅 2.2MB | 工具包/大文件放 `/root`（rootfs 28MB）或 `/tmp`（27MB tmpfs） |
| `/tmp` 是 27MB tmpfs | 大帧文件会写满导致"断流"假象；连续抓帧用 `--stream-to=/dev/null` |
| BusyBox 无 `timeout` | 脚本用后台 `&` + `kill -0` 轮询实现超时 |
| BusyBox 无 `stat` | 取文件大小用 `wc -c < file` |
| BusyBox `i2cget` 仅 8 位寄存器 | GC1084 是 16 位寄存器，无法直接读 ID，改查 `/sys/bus/i2c/devices/4-0037/name` |
| `/dev/video11` 是 Multiplanar | V4L2 应用须用 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` |
| AE 需 ~15 帧收敛 | 短抓帧（<0.5s）会误判"ISP 输出全零" |

## 诊断工具（2026-08-14 补充）

### ISP 通路测试 `scripts/gc1084-isp-test [demo|v4l2] [count]`

之前 `rk_mpi_vi_test` 报的 `dma_en 0x0` 是**误判**：ISP 模式下 CIF
不写内存（直接内联喂 ISP），DMA 关闭是正常行为；真正的"输出全零"是
**AE 未收敛**（短抓帧）。推荐 `v4l2` 模式：

- `v4l2`：后台 `rkaiq_3A_server`（默认读 `/etc/iqfiles/`）+ `v4l2-ctl`
  直接抓 `/dev/video11` NV12。抓帧前 `--stream-skip 90` 跳过 AE 热身后
  再落盘，即可拿到收敛后的真实帧。
- `demo`：`rkisp_demo`（进程内 rkaiq）当前会段错误（rc=139），暂不可用。

工具包在 `out/gc1084-build/isp-kit/`（rkisp_demo、rkaiq_3A_server、
librkaiq.so、libsmartIr.so、librga.so、gc1084 IQ 文件、rockit.ko）。

### MIPI 稳定性诊断 `scripts/gc1084-mipi-diag [frames] [rounds]`

rkcif 驱动在 `/proc/rkcif-mipi-lvds` 暴露中断统计（frame dma end、
csi overflow、bandwidth lack、size err、all err）。抓帧对比统计可区分
"CIF 收不到数据（error 计数增长）" vs "传感器停流"。**注意**：脚本
务必 `--stream-to=/dev/null` 或控制帧数，否则 tmpfs 写满会制造假断流。

### 实时 YOLO `apps/camera-yolo` + `scripts/deploy-camera-yolo`

`camera_yolo <model.rknn> <video-device> [--count N] [--dump-prefix P]`：
V4L2（Multiplanar）MMAP 抓 ISP mainpath NV12 → RGA `convert_image_with_letterbox`
转 640×640 RGB888 → RKNN yolov5 推理 → 打印检测框与 FPS；
`--dump-prefix` 落盘 NV12/RGB 帧供宿主侧 `scripts/nv12-to-png` 目检。
deploy 脚本通过板端 `scripts/board/gc1084-yolo-run.sh` 在**同一 adb shell**
里启动 `rkaiq_3A_server` + camera_yolo（跨 shell 的 `nohup` 会被杀掉）。
厂商 Demo 的 OpenCV 路径走 `/dev/video0` 裸 RAW，GC1084 BA10 下不可用，
故实时检测必须挂 ISP 路径。

### 板端状态（2026-08-14 复盘）

板子 USB 枚举但 ADB `offline` 时，恢复步骤：重新上电 →
`adb kill-server && adb start-server` → `adb devices` 应出现 `device`。
本轮已完整跑通：RAW 抓帧稳定、ISP 出真实帧、实时 YOLO 10.9 FPS。
