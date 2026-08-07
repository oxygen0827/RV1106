# Echo-Mate 资料索引

本索引记录 2026-08-07 已下载和核对的资料。外部链接可能更新；本地文件提供可复现快照，哈希用于校验。

## 官方入口

- 立创开源硬件：https://oshwhub.com/no_chicken/ai-desktop-robot-echo
- 作者手册：https://no-chicken.com/content/Echo-Mate/intro.html
- 主仓库：https://github.com/No-Chicken/Echo-Mate
- Demo 仓库：https://github.com/No-Chicken/Demo4Echo
- 教程视频：https://www.bilibili.com/video/BV1685qztEec
- Luckfox RKMPI 指南：https://wiki.luckfox.com/zh/Luckfox-Pico/RKMPI-example

硬件项目页面标注 `CC BY-NC-SA 4.0`，代码仓库标注 `GPL-3.0`。引用、分发或做产品前分别检查硬件资料和软件代码的许可边界。

## 本地源码

- `upstream/Echo-Mate/`：官方主仓库浅克隆，commit `b7a9f31e2d1e4407b89e0bfe5db0ad78678b96a9`；Echo SDK 已检出，约 3 GB。
- `upstream/Demo4Echo/`：官方 Demo 浅克隆，commit `97973f751df531bf3e8a008bd65e40530275669b`；已检出 DeskBot、AIChat 和 YOLO 目录。
- `rkmpi_demos`、LVGL、SenseVoice/FSMN 模型是独立子模块，当前未递归拉取；需要时按任务单独初始化，避免一次下载全部模型。

当前 SDK 位于 macOS 默认大小写不敏感卷。Linux kernel 树存在如 `xt_MARK`/`xt_mark` 这类仅大小写不同的文件，检出后会出现 Git 伪修改，因此这个副本只用于索引和阅读。编译时应在 Ubuntu 22.04/ext4 重新 clone 同一 commit，不能从这里打包 SDK 传入 Linux。

## 本地附件

| 文件 | 用途 | SHA-256 |
| --- | --- | --- |
| `reference/echo-mate/sd_buildroot_img_260329.zip` | SD 启动/恢复镜像，ZIP 已完整校验 | `39b1db7e6685886c57b53072fa96ba1cf23ccf1ce2d9ce4de0954709dba09f1a` |
| `reference/echo-mate/nand_buildroot_img_260329.zip` | SPI NAND 启动/恢复镜像，ZIP 已完整校验 | `adf37017b48f954d25e5433ca5668de53d5b7924dee38a987cc7f873ff444fdc` |
| `reference/echo-mate/bin_250627.zip` | 预编译 DeskBot、RKNN 模型和运行库 | `4b6648558112ddbea2e8ae43049507f6a397bdfec0ac00160bfcd7964b5e24cc` |
| `reference/echo-mate/P024C128-CTP-datasheet.pdf` | 屏幕规格、16-pin 定义和电气参数 | `7a2b934d3b490557fccbc7f0b2730229ab410730006e371768d10d38db4f224d` |
| `reference/echo-mate/RV1106-datasheet-v2.0.pdf` | 2026 年 RV1106/G2/G3 最新规格快照 | `53476c5ad66bc0cb8da5d7c5c4e72cd6f3365c9ad3209a36855a523c7b453284` |
| `reference/echo-mate/Rockchip-reference.rar` | RV1106、ACodec、RTL8723BS 参考 PDF | `c137fb3e317d375cb24624fad81fd1c35cb8453c5728bde7f23cb16653c51666` |
| `reference/echo-mate/Echo-parts.pdf` | 机械件、排线、屏幕和摄像头清单 | `8bb2731154498d52f141f3430fcc4e1fb6cde23dbcc7eea773f909b7fe0f5b1b` |
| `reference/echo-mate/AMap_adcode_citycode.xlsx` | 3241 行高德城市 `adcode/citycode` 映射 | `ec3eac01dd92e67a27e34fcd65f3b60b04ab1233757c7c17494b8a9f187c0330` |
| `reference/echo-mate/Echo-shell-3d.rar` | 上盖、底座、挡板、主动轮和从动轮 STL | `1f81bc7ef197c55184c0c12ec697e5e04557574fb1d34897d199bdc96665477b` |
| `reference/echo-mate/Echo-shell-install.mp4` | 官方外壳安装视频 | `33d58ae500fe9941d8a71cdcaecc53c95725c9bc59642c721ef6b9eef2bea0bd` |

原压缩包已经解到 `reference/echo-mate/extracted/`。预编译 `bin` 也已解开，方便核对模型、运行库与配置。

## Rockchip 压缩包内容判断

直接相关：

- `RV1106 highly integrated vision processor SoC Datasheet.pdf`
- `Rockchip_Developer_Guide_Linux_RV1106_ACodec_CN.pdf`
- `RK3228平台 RTL8723BS模块应用参考原理图...pdf`，仅参考 RTL8723BS 连接方式，最终以 Echo 原理图为准。

邻近型号参考：

- `Rockchip_RV1126_RV1109_Hardware_Design_Guide...pdf` 可参考电源、CSI、DDR/高速信号原则，但不是 RV1106/Echo 原理图。

与本板无直接关系：

- `RL-UM02WBS-8723BU...pdf` 是 8723BU，不是板载 8723BS。
- 三份 RK3588 核心板/主板图纸不用于 Echo 开发。

## 教程目录

视频总长约 2 小时 14 分，共 14 节：

1. 前言
2. 需要的技术栈
3. 硬件复刻
4. 代码烧录
5. 3D 外壳安装
6. SDL 仿真运行
7. AI 聊天运行
8. 开发板使用
9. 拉取仓库和编译
10. 看懂原理图
11. CMake 快速构建
12. LVGL UI 设计
13. 整体软件框架
14. AI Chat 代码讲解

建议先看 3、4、8、9 完成上板，再看 6、11、12、13 做应用开发，最后看 7、14 接 AIChat。

## 已知缺口

- 手册中的核心板原理图链接 `Core_2025-03-23.pdf` 当前返回 404。硬件修改前必须从立创工程设计图重新导出或向作者获取最新版。
- 尚未在本机编译 SDK，也未连接实板验证串口、设备树、触摸坐标或摄像头。
- 旧 Rev 1.1 写 NPU 0.5 TOPS，2026 Rev 2.0 把 RV1106、G2、G3 都列为 1 TOPS。后续报告必须写明芯片顶标、数据手册版本和实测结果。
- 上游源码含看起来可用的 API key；只作安全问题证据，严禁复用。
