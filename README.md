# uefi-mondrian

Redmi K60（代号 **mondrian**，高通 **SM8475**）的 UEFI 固件，用于 UniXNU 移植。
派生自 Project-Silicium / Mu-Silicium；平台包位于 `Platforms/Xiaomi/mondrianPkg`。

[English](README_EN.md)

K60 移植由三个仓库组成：本仓库（UEFI 固件）、
[UniXNU-mondrian](https://github.com/Emilelu/UniXNU-mondrian)（设备支持与移植记录）、
[Emilelu/UniXNU](https://github.com/Emilelu/UniXNU)（核心框架与 `unixnu` CLI；官方上游 [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU)）。

> 最后更新：2026-11-12。构建与烧录只走 `fastboot boot`，不写任何分区。

## 状态（2026-11-12）

| 区域 | 状态 | 说明 |
| --- | --- | --- |
| UEFI 启动链 | ✅ | ABL → `fastboot boot` → BootShim → SEC/PEI/DXE/BDS → UniXNU 菜单 |
| 显示 | ✅ | ABL splash framebuffer + `SimpleFbDxe` GOP；无 GPU 驱动 |
| 按键 | ✅ | 官方 `KeypadDeviceDxe`/`KeypadDxe` + `K60KeypadDeviceLib`（SPMI PON/GPIO） |
| 工具 → Advanced Logger | ✅ | `AdvLogDxe` 生产者 + `AdvancedLogDxe` QR 插件；4 MB 缓冲区 |
| 自动复位 | ✅ | 60 s 无按键冷复位回 fastboot；复位返回则菜单继续存活 |
| XNU payload | ⬜ | 本仓库不携带 `kernel.macho`；可引导 payload 由设备仓库的 `unixnu build payload` 生成 |
| 厂商 Android 栈 | 已移除 | SMEM/时钟/PMIC/UFS/充电等不在最小 DXE 中 |

2026-11-12 在 `bd5cfa6` 上做过一次全量验证：`./unixnu build firmware` 约 2 分 30 秒完成，
产物 `Build/mondrianPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` 为 **3,145,728 字节（固定 3 MiB）**。
`scripts/unixnu/firmware.py` 会校验产物字节数，不符即构建失败。

## 构建

用 UniXNU 的构建驱动，不要直接调 `build_uefi.py`（它缺少 UniXNU 的包路径）：

```sh
git clone --recurse-submodules https://github.com/Emilelu/uefi-mondrian
git clone https://github.com/Emilelu/UniXNU-mondrian
git clone https://github.com/Emilelu/UniXNU
cd UniXNU
../UniXNU-mondrian/scripts/apply-core-patches.sh .
./unixnu defconfig mondrian_defconfig -D ../UniXNU-mondrian
./unixnu build firmware --uefi ../uefi-mondrian -D ../UniXNU-mondrian
```

不要写死框架 commit：固件构建只依赖 `scripts/unixnu/` 与
`patches/unixnu-core/k60-unixnu-core.patch`。构建记录里保存实际使用的版本：

```sh
git -C ../UniXNU rev-parse HEAD
git -C ../UniXNU log -1 --format='%h %s'
```

产物：`Build/mondrianPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`。

### 自包含构建

`UniXNUPkg` 与 `DarwinBootPkg` 已 vendor 到 `Platforms/Xiaomi` 下，因此固件也能在没有
UniXNU 框架检出、且 `PACKAGES_PATH` 上没有设备仓库 `DarwinBoot22` 副本的情况下，从干净 clone 构建：

```sh
stuart_build -c Platforms/Xiaomi/mondrianPkg/DeviceBuild.py \
  TARGET=DEBUG FD_BASE=0xA7400000 FD_SIZE=0x300000 FD_BLOCKS=0x300 \
  DEVICE_MODEL=0 ENABLE_SECUREBOOT=0
```

重复构建不是逐字节一致：PE 头带构建时间戳，同一份源码两次构建的 `.fd` 哈希不同。
识别镜像请用源码 commit，不要沿用别的构建带过来的哈希。

需要可引导 payload 时仍走上面的 UniXNU 驱动，因为它同时生成 payload FAT 与 Android boot image。
`apply-core-patches.sh` 对 `scripts/unixnu/bootimg.py` 那半边补丁仍是必需的；固件那半边已内联进
vendored 的 `UniXNUPkg`（见 `Platforms/Xiaomi/UniXNUPkg/VENDORED.md`）。

## 启动镜像

bring-up 阶段是 boot 一个镜像，不是 flash：

```powershell
D:\Android\platform-tools\fastboot.exe boot <payload.img>
```

payload 里的固件镜像是 `SILICIUM_UEFI.fd-bootshim`（`BootShim.bin` + FD）。
**永远不要 `fastboot flash boot`，永远不要写 Android 分区。**

⚠️ 不要拿 `Platforms/Xiaomi/mondrianPkg/FdtBlob/sm8475-xiaomi-mondrian.dtb`（全量 DTB，480 KB）
去 `unixnu bootimg --dtb`：那会让镜像起不来。`bootimg` 用的 DTB 是 kernel 区尾部那一个
（由 `unixnu build payload` 生成）。

失败交接后排障读日志的流程见 [`docs/QR-ADVLOGGER.md`](docs/QR-ADVLOGGER.md)。

## 子模块与版本固定

`git submodule status` 是唯一权威查询入口（下表为 2026-11-12 的快照）：

| 子模块 | commit |
| --- | --- |
| `Binaries` | `9f4dbfd` |
| `Common/Mu` | `b8d46c7` |
| `Common/Mu_OEM_Sample` | `b70b4b7` |
| `Mu_Basecore` | `6f805f2`（`release/202608`） |
| `Silicium-ACPI` | `f50bc11` |
| `Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl` | `67b5686` |

查询某个 pin 在上游是否还存在（不存在则干净 clone 会失败）：

```sh
git ls-remote https://github.com/microsoft/mu_basecore <sha>
```

`Mu_Basecore` 原 pin `3d2a9fa` 已从上游消失，clean clone 会直接失败；`bd5cfa6` 重签到
`6f805f2`（`release/202608` 上的提交），见 issue #4。子模块克隆必须用完整历史，不要加 `--depth`。

## 相对上游的改动

见 [`docs/K60-CHANGES.md`](docs/K60-CHANGES.md)。简版：K60 平台包与内存映射、最小开源
DXE/BDS、官方 KeypadDevice 链加 SPMI K60 后端、Advanced Logger 生产者/插件对，以及
UniXNU/DarwinBoot 集成。`Binaries` 为未改动的官方 Project-Silicium 子模块。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [`Status.md`](Status.md) | 固件状态快照 |
| [`Building.md`](Building.md) | 上游通用构建说明（Windows/Linux 依赖、`build_uefi.py`） |
| [`docs/K60-CHANGES.md`](docs/K60-CHANGES.md) | 相对上游的平台改动与已知限制 |
| [`docs/QR-ADVLOGGER.md`](docs/QR-ADVLOGGER.md) | Advanced Logger / QR 取日志与失败交接排障 |
| [`AGENTS.md`](AGENTS.md) | 仓库工作规则（分支、提交规范、构建前置条件） |

CI 说明：UEFI 镜像工作流（`active_devices.yml`）只在手动 `workflow_dispatch` 触发，
push 不会自动跑 GitHub Actions。

## 数据安全

bring-up 只用 `fastboot boot`，不写任何分区。不要 flash `persist`，不要 `flash all`。
Root/Data 会使用独立的 `unixnu` 分区（双系统），见设备仓库 `docs/DUAL-SYSTEM.md`。

## 许可

平台代码为 BSD-2-Clause-Patent。上游组件保留各自许可；`Binaries` 仍是官方
Project-Silicium 子模块。
