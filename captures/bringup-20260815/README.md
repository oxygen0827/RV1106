# 新板子验收（2026-08-15）

2026-09-20 仓库整理时，文本证据统一为 LF 换行并去掉终端颜色转义和行尾空格；设备输出数值未修改。

- 板子：第二块 Echo-Mate RV1106（ADB 序列号 `5aedb378991eebd6`，USB 枚举名 rk3xxx）。
- 固件：与基线板（2026-03-29 #1 构建）**不同**——本板出厂镜像为内核 `5.10.110 #45 Wed Apr 9 17:33:31 CST 2025`，Buildroot 2023.02.6。
- 启动介质：SPI NAND（UBIFS，rootfs 181.3M / 可用 99.9M），`/proc/cmdline` 与基线一致。
- 验收结果：11 项通过，1 项失败（camera 无传感器，符合"暂不接摄像头"策略），3 警告。
- 实测通过项：显示（fb_st7789v 320x240 RGB565，写红/绿整帧无 SPI 错误，背光 50/99）、触摸（ft6336 event0，抓到 ABS_X/ABS_Y/BTN_TOUCH 坐标事件）、音频播放（aplay exit 0，880Hz 提示音）、音频录音（3 秒 16kHz 波形有效 peak=0.50 rms=0.017）、Wi-Fi 驱动（r8723bs 已加载，wlan0 未配网）、NPU/RGA/vcodec/蓝牙模块齐全。
- 已知差异 / 待办：
  1. 固件版本是 2025-04-09 出厂镜像，非项目基线 2026-03-29 镜像；如需一致需重刷 `nand_buildroot_img_260329.zip`。
  2. Wi-Fi 未配置（出厂只有开放网络配置），需按 docs/dev-experience.md 配置 LDKJ 才能 SSH。
  3. `/root` 为空，未部署 DeskBot 应用（`bin/` 尚未上传）。
  4. 板上时间 2021-01-01（无 RTC 电池，未联网 NTP），正常。
  5. 会话期间 USB 连接曾掉线一次，重插后恢复；连接稳定性待观察。
- 证据文件：`board-baseline.txt`（uname/cmdline/mem/mtd/mount/设备节点/声卡/模块）。

## Wi-Fi 天线验证（2026-08-15，追加）

- 安装天线模块后实测：wlan0 射频正常（txpower 12 dBm），扫描到 LDKJ 等 AP（-45 dBm 强信号）。
- 配置 LDKJ 成功：WPA2-PSK/CCMP，wpa_state=COMPLETED，DHCP 拿到 192.168.31.254（网关 192.168.31.1）。
- 验证：网关 ping 0% 丢包、外网 223.5.5.5 0% 丢包、DNS 解析正常、Mac→板子 SSH 登录成功（密钥测试后已清理）。
- 配置方式：`scripts/board-wifi-config [ADB_SERIAL]`，密码从 `~/.echo-mate-env`（600）读取，经 adb push 写入 `/etc/wpa_supplicant.conf`（600），不回显。
- 开机自启：出厂 `S99start_echo_defconfig` 中 `start_desk_bot` 被注释（Wi-Fi 不会自动连）。新增 `/etc/init.d/S99wifi`（在 S99start_echo_defconfig 之后执行，等待 wlan0 出现后起 wpa_supplicant + udhcpc）。重启实测自动连上 LDKJ。
- 注意：板子 IP 为 DHCP 分配，可能变化；本次为 192.168.31.254。SSH：`ssh root@<ip>`（密码 root）。

## DeskBot 部署与开机自启（2026-08-15，追加）

- 部署预编译包 `reference/echo-mate/extracted/bin/bin_250627/bin/` → 板子 `/root/bin/`（20.6MB，adb push 4s）。
- 屏幕从 ECHO-MATE 启动 logo 切换为 DeskBot 全屏界面（fb 内容：227 色 / 99% 非黑，对比 logo 仅中央一条）。
- 开机自启：出厂 `S99start_echo_defconfig` 中 `start_desk_bot` 被注释。已扩展 `/etc/init.d/S99wifi`（原 Wi-Fi 自启）在 Wi-Fi 连接后自动启动 `/root/bin/main`（setsid 后台，检测避免重复启动）。
- 注意：出厂脚本备份移出 init.d（放 /root/S99start_echo_defconfig.bak），避免被 init 重复执行。
- 重启实测：驱动 → Wi-Fi（LDKJ COMPLETED）→ DeskBot（./main 运行）全自动，屏幕显示 DeskBot UI。
- 待办：AIChat server_url=172.32.0.100（USB 网卡，macOS 不可用）与 token=123456（示例值）需在联调语音时按 docs/dev-experience.md 配置。
