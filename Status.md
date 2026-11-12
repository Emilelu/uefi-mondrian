# Pong UEFI status

K60 (mondrian) build status for this fork. The upstream file of this name described the
Nothing Phone (2) "pong" board; none of those values apply here.

| Item | Value |
| --- | --- |
| Device | Redmi K60 |
| Codename | mondrian |
| SoC | Qualcomm SM8475 |
| Architecture | AArch64 |
| Device tree | `Platforms/Xiaomi/mondrianPkg/FdtBlob/sm8475-xiaomi-mondrian.dtb` |
| Firmware volume base | `0xA7400000` (moved off `0xA7000000` because ramoops lives there) |
| Firmware volume size | `0x300000` (3 MiB, fixed) |
| Framebuffer | 1440 × 3200, 32-bit |

Source of the values: `Resources/Configs/mondrian.toml` (`[uefi_fd]`) and
`Platforms/Xiaomi/mondrianPkg/mondrian.dsc` (`PcdFrameBufferWidth`/`Height`).

The DTB above is the full 480 KB firmware device tree. It is **not** the DTB to pass to
`unixnu bootimg --dtb`; boot-image assembly uses the DTB in the kernel-area tail.

The Pong firmware builds from the board package in this repository. UniXNU menu and DarwinBoot
integration are present. Development runs have reached the UniXNU menu and transferred control
to DarwinBoot; this is not a claim that every kernel or operating system boots successfully.

Last verified build: 2026-11-12 at `bd5cfa6` — `Build/mondrianPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`,
3,145,728 bytes, build time 00:02:30.
