# K60 platform changes

This tree is a K60 port of the Project-Silicium Mu-Silicium UEFI firmware. The
platform package is `Platforms/Xiaomi/mondrianPkg`; the device configuration is
in the separate `UniXNU-mondrian` repository.

## K60 platform

- `mondrianPkg` DSC/FDF, ACPI tables, platform PCDs and the K60 memory map
  (`Library/MemoryMapLib`), including ramoops, `ABL DTB` at `0xB6760000`,
  IMEM cookie and the DXE heap.
- Minimal open-source DXE/BDS: DxeCore/Pcd/ReportStatusCode/Runtime/Cpu/Gic/
  Timer/Metronome/Watchdog/Variable/DiskIo/Partition/FAT/English/BdsDxe/
  SimpleFbDxe, plus the UniXNU integration modules.
- `BootManagerLib` auto-launches the UniXNU FV application from BDS so the
  menu appears without a boot option.
- `SimpleFbDxe` wraps the ABL splash framebuffer as GOP; the UEFI text console
  is drawn by CPU writes (no GPU/display controller driver).

## Input

- Official `KeypadDeviceDxe` + `KeypadDxe` chain, never a bare
  `SimpleTextIn` handle.
- `K60KeypadDeviceLib` implements the official `KeypadDeviceLib` interface
  with direct SPMI PMIC arbiter reads: PON PPID `0x013` (power/volume-down
  RT_STS bits 7/6), pm8350 GPIO PPID `0x18D` (volume-up, active low).
- 60 s no-key auto-reset writes IMEM `0x77665500` and calls
  `EfiResetCold` (PSCI `SYSTEM_RESET`). If the reset returns, the auto-reset
  is disabled and the menu stays alive instead of halting in `wfi`.

## Logging

- `AdvLogDxe` hosts the official AdvLoggerPkg DxeCore-instance sources and
  explicitly calls `DxeCoreAdvancedLoggerLibConstructor` (the source is in
  `[Sources]`, so EDK2 does not auto-call it).
- `AdvancedLogDxe` is placed in the APRIORI list immediately after the
  producer so it is dispatched before BDS; it publishes the Tools menu plugin
  and the QR sequence.
- App `PcdUniXNUDebugLevel` is set to ERROR in `mondrian.dsc` so the UniXNU
  menu/payload `UNIXNU:` lines reach the buffer without enabling global INFO.

## UniXNU integration

- `UniXNU`, `UniXNUDxe` and `DarwinBoot` are in the FV.
- `UniXNUPkg` and `DarwinBootPkg` are **vendored in-tree** under
  `Platforms/Xiaomi`, imported from `Chengdong-Yu/uefi-pong@87c10fd`. The
  checkout therefore builds from a clean clone without a UniXNU framework
  checkout or the device repository's `drivers/DarwinBoot22` in
  `PACKAGES_PATH`. Both packages carry a `VENDORED.md` listing every K60 delta.
- The firmware half of the K60 core patch
  (`UniXNU-mondrian/patches/unixnu-core/k60-unixnu-core.patch`) is applied to
  the vendored `UniXNUPkg` at import time instead of to an out-of-tree
  checkout: the `RAMOOPS` panic mirror dump, the mapped-range and FAT
  boot-sector guards in `UniXNURamDiskDxe`, `timeout = -1` plus volume probe
  logs in `Config.c`, and the storage counters in `Main.c`. The
  `scripts/unixnu/bootimg.py` hunk is not firmware and stays in the device
  repository.
- `DarwinBootPkg` is pong's `DarwinBoot24` copy retargeted to the **Darwin 22**
  `boot_args` ABI (0x2e0 bytes, 608-byte command line), because the K60 payload
  is `ipados-16.6.1-20G81` (XNU 8796.141.3). This replaces the device
  repository's `DarwinBoot22` fork in the firmware build; the four values to
  restore for a Darwin 24 payload are documented in
  `DarwinBootPkg/Driver/BootArgs.h`.
- `PcdUniXNUDeviceModel` is `"Redmi K60"`; `board.toml` supplies the
  `mondrian_defconfig` and the firmware DSC/FD paths.

## Panic readability

- `Tools -> Advanced Logger` renders the 4 MiB Advanced Logger buffer as
  Onesend QR frames; `docs/QR-ADVLOGGER.md` is the operator procedure,
  including how to read a failed hand-off after a reboot.
- The loader logs its kernel-window failure (`DARWINBOOT: no window ...`,
  `DARWINBOOT: free ...`) in addition to drawing it, so the numbers survive the
  screen being reused.


## Official consistency

- Vendor binaries (`Binaries` submodule) are unmodified; no `.efi` patch.
- At build time `[firmware].patches` applies two Mu source patches to the pinned
  `edk2` (Mu_Basecore) checkout: `AsmMacroLib` (normalizes `.text` segment names
  in `MdePkg/Include/AArch64/AsmMacroLib.h`) and `Boot-Manager`
  (`BootManagerMenuApp` is present in the K60 FV). Three other patches that were
  inert for K60 (`Auth-Service`, `Timer`, `Usb-Bus`) have been removed.
- The only core deviations are in the device repo patch:
  `bootimg.py` v3/v4 header verification, `Config.c` menu timeout, and the
  `UniXNURamDiskDxe` range/alignment guards. They are documented in the patch
  series and can be reverted independently.
- `MondrianBootMemoryDxe` (FDT initrd block device) is compiled in the DSC but gated out of the FV by `MONDRIAN_INITRD_DISK_ENABLED`, which is undefined in this tree; it is retained for the later XNU initrd path. Its two probes under `Test/` reference it by include/name only.
- The K60-specific code is `K60KeypadDeviceLib` and the `AdvLogDxe` shell;
  both implement official interfaces or compile official sources.
- Other K60 device-layer additions beyond those two drivers: `MondrianBootMemoryDxe`
  (FDT initrd block device), `ConfigurationMapLib`, `MemoryMapLib`, and the two
  Mu patches listed above. `K60ButtonsDxe` was removed as retired.

## Known limitations

- No `kernel.macho` yet; `UniXNU` uses its built-in 4-entry menu and reports
  `config: no boot entry found`.
- Missing vendor protocols (SCM/Clock/DDR/ChipInfo) are expected in the
  minimal stack and are not required for the XNU handoff.
- The ABL initrd staging range can be unaligned; the driver skips it with a
  warning. Payload builds must pad the FAT volume to 4 KiB.
- No touch/GPU/audio/network/camera; UFS/Data/root are not implemented yet.
