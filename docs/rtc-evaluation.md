# RTC 路线评估（火山引擎 ByteRTCLite）

> 2026-08-16 初步调研。背景：主持人问答长期走火山 RTC（全双工、低延迟），
> 当前 demo 走 WebSocket（API_DOC 第 5 节）。本文件回答"RTC 在 RV1106
> (armv7 Linux) 上是否可行、卡点在哪"。

## SDK 架构（来源：volcengine/ByteRTCLiteHal，MIT）

ByteRTCLite 嵌入式 SDK = 两部分：

1. **HAL 层 `libVolcEngineRTCHal.a`（我们编译）**
   - MIT 开源：https://github.com/volcengine/ByteRTCLiteHal
   - 纯 C 抽象：内存/文件/线程/互斥/网络/时间/TLS(mbedtls) 等 ~20 个头文件
   - 官方参考实现：`esp32`、`macos`（POSIX）两个平台目录
   - **RV1106 落地路径明确**：以 macos（POSIX pthread/socket/stdio）实现
     为蓝本适配 armv7 Linux/uClibc，常规 C 移植工作量（mbedtls 已在
     Buildroot 可用）
2. **核心库 `libVolcEngineRTC.a`（向火山获取）**
   - 专有，README 明确"联系火山商务或研发获取相应平台 libVolcEngineRTC.a"
   - **这是唯一硬卡点**：需要 32-bit ARM Linux (armv7) 版本，且工具链 ABI
     需对齐本板 Buildroot（GCC 8.3.0 + uClibc；火山若只有 glibc 版需评估
     兼容或双 rootfs 方案）

## 结论

- RTC 路线**可行**：HAL 开源、POSIX 参考现成，核心库需要走商务渠道申请
  armv7 版本。
- 行动项：
  1. 请软件方/商务向火山确认 `libVolcEngineRTC.a` 是否有 armv7 Linux
     （32-bit）版本及其工具链要求（glibc/uClibc、GCC 版本）；
  2. 拿到 .a 后：克隆 ByteRTCLiteHal → 新增 `platform/linux_armv7` 目录
     （参考 macos 实现）→ 交叉编译 HAL → 链接验证 demo 入房；
  3. 期间 WebSocket 路线保持 demo 主路径（传输层已抽象为 IWsTransport，
     切换零上层改动）。
