# Pong UEFI status

| Item | Value |
| --- | --- |
| Device | Nothing Phone (2) |
| Codename | pong |
| SoC | Qualcomm SM8475 |
| Architecture | AArch64 |
| Device tree | `Resources/DTBs/pong.dtb` |
| Firmware volume base | `0xA7100000` |
| Firmware volume size | `0x300000` |
| Framebuffer | 1080 × 2412, 32-bit |

The Pong firmware builds from the board package in this repository. UniXNU menu and DarwinBoot integration are present. Development runs have reached the UniXNU menu and transferred control to DarwinBoot; this is not a claim that every kernel or operating system boots successfully.
