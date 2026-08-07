# Echo-Mate RV1106 开发工作区

这个目录用于后续通过 AI / vibe coding 开发 Echo-Mate RV1106 开发板。当前阶段先建立可恢复、可验证的资料与开发基线，不直接修改系统镜像或板级驱动。

## 已整理内容

- [资源索引](RESOURCES.md)：官方源码、固件、屏幕规格书、配件、3D 文件和教程目录。
- [硬件参考](docs/hardware-reference.md)：RV1106、屏幕、触摸、网络、音频、存储和摄像头边界。
- [首次上板与恢复](docs/board-bringup.md)：装屏、烧录、登录、网络和逐项验收流程。
- [软件架构与 AI 开发方法](docs/software-architecture-and-vibecoding.md)：DeskBot、LVGL、AIChat、RKNN 以及后续迭代方式。
- [Docker 开发环境](docs/docker-development.md)：Ubuntu 22.04 amd64 容器、Linux volume 和统一构建命令。
- [AI 协作约束](AGENTS.md)：后续 Codex/Agent 在这个目录工作时必须遵守的事实和安全边界。

## 目录

```text
RV1106/
├── docs/                    # 提炼后的开发文档
├── docker/                  # Ubuntu 22.04 amd64 构建镜像
├── reference/echo-mate/     # 官方附件和关键 README 快照
├── scripts/echo-sdk         # Docker SDK 管理入口
├── upstream/Echo-Mate/      # 官方主仓库浅克隆
└── upstream/Demo4Echo/      # DeskBot/AIChat/YOLO 源码浅克隆
```

`reference/` 与 `upstream/` 是上游资料，不在里面直接做产品改动。正式代码应另建应用目录或从上游建立自己的分支后再修改。

## 当前结论

- 主控为 RV1106 系列，板上为 256 MB DDR3L，资料指向 RV1106G3。2026 年 Datasheet V2.0 将 G2/G3 都列为 1 TOPS，但旧 Rev 1.1 写 0.5 TOPS；应先核对芯片丝印并以实测 RKNN 性能建立预算。
- 屏幕为浦洋 `P024C128-CTP`：2.4 英寸、240x320、4-wire SPI、ST7789V3；触摸为 FT6336U、I2C。
- LCD 座是翻盖式 FPC 端子。先打开锁扣并把排线完全插到底，再合上锁扣；锁扣盖不紧通常是排线没有进入连接器。
- 推荐先用 SD 卡镜像恢复和验证；SD 启动前必须确认 SPI NAND 为空。
- RV1106 资源有限，UI、唤醒和 RKNN 推理可在板端运行；ASR、LLM、TTS 默认放在电脑/服务器端。
