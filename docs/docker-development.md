# Docker 开发环境

完整 Echo-Mate SDK 保存在 Docker named volume `echo-mate-sdk`，不会检出到 macOS 默认的大小写不敏感文件系统。构建容器是 Ubuntu 22.04 x86_64，Apple Silicon Mac 通过 Docker Desktop 的 amd64 仿真运行。

## 布局

| 路径 | 用途 |
|---|---|
| Docker volume `/workspace/Echo-Mate` | 可编译的完整上游仓库、Demo 子模块及 Git LFS 文件 |
| 容器 `/host-project` | 本资料库的只读挂载 |
| 容器 `/workspace/Echo-Mate/SDK/rv1106-sdk` | SDK 构建目录 |
| 容器 `/workspace/Echo-Mate/Demo` | 与主仓库固定版本一致的 Demo 子模块 |

不要把 macOS 中 `upstream/Echo-Mate` 的检出结果复制进 volume。该目录只用于查阅，内核源码在默认 APFS 上已有大小写冲突。

## 常用命令

从本资料库根目录执行：

```bash
./scripts/echo-sdk status
./scripts/echo-sdk check
./scripts/echo-sdk shell
```

首次初始化默认选择 Echo-Mate 的 SD Card + Buildroot 基线。切换存储介质时使用：

```bash
./scripts/echo-sdk configure-sd
./scripts/echo-sdk configure-nand
```

SDK 选板和构建：

```bash
./scripts/echo-sdk lunch
./scripts/echo-sdk sdk
./scripts/echo-sdk sdk kernel
./scripts/echo-sdk sdk rootfs
```

`lunch` 时选择以下板级配置之一：

- SD：`BoardConfig-SD_CARD-Buildroot-RV1106_Echo_Mate-DeskMate.mk`
- NAND：`BoardConfig-SPI_NAND-Buildroot-RV1106_Echo_Mate-DeskMate.mk`

构建输出保留在同一个 named volume 中。进入容器后可在 `SDK/rv1106-sdk/output` 查看。需要复制到 macOS 时，从另一个终端执行：

```bash
mkdir -p artifacts
docker compose run --rm -v "$PWD/artifacts:/export" sdk \
  bash -lc 'cp -a SDK/rv1106-sdk/output/. /export/'
```

## 版本和维护

初始化脚本默认固定 Echo-Mate 到提交 `b7a9f31e2d1e4407b89e0bfe5db0ad78678b96a9`。不要在没有核对硬件、设备树和 AIChat 协议变化时直接更新主分支。

查看 volume：

```bash
docker volume inspect echo-mate-sdk
```

volume 是唯一的可编译 SDK 副本。删除 `echo-mate-sdk` 会同时删除源码、构建缓存和输出，因此不要把 `docker volume rm` 或 `docker compose down -v` 当作普通清理命令。

## 已验证基线

2026-08-07 在 Apple Silicon Mac + Docker Desktop 上完成以下验证：

- 容器：Ubuntu 22.04.3 LTS、`x86_64`。
- Echo-Mate：`b7a9f31e2d1e4407b89e0bfe5db0ad78678b96a9`。
- Demo 及全部递归子模块已初始化，`git lfs fsck` 通过。
- RV1106 交叉编译器：GCC 8.3.0，SDK 官方 `build.sh check` 全部通过。
- 构建配置：Echo-Mate、SD Card、Buildroot、`rv1106g-echo-mate.dts`。
- `./scripts/echo-sdk sdk kernel` 构建成功；`output/image/boot.img` 已生成。

本次只用内核构建验证开发环境，没有预先执行 U-Boot、Buildroot rootfs 和完整固件的一键构建。需要完整重编镜像时执行 `./scripts/echo-sdk sdk`。
