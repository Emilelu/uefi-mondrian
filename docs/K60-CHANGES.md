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
- The shared `UniXNURamDiskDxe` publishes the FDT initrd as a block device.
  The K60 tree uses the core patch in
  `UniXNU-mondrian/patches/unixnu-core/k60-unixnu-core.patch` for the mapped
  range check and the 4 KiB alignment guard.
- `PcdUniXNUDeviceModel` is `"Redmi K60"`; `board.toml` supplies the
  `mondrian_defconfig` and the firmware DSC/FD paths.

## Official consistency

- Vendor binaries (`Binaries` submodule) are unmodified; no `.efi` patch.
- No Mu/DxeCore source patch is used at build time; the build uses the pinned
  feed (`out/feeds/mu_basecore-*`).
- The only core deviations are in the device repo patch:
  `bootimg.py` v3/v4 header verification, `Config.c` menu timeout, and the
  `UniXNURamDiskDxe` range/alignment guards. They are documented in the patch
  series and can be reverted independently.
- The K60-specific code is `K60KeypadDeviceLib` and the `AdvLogDxe` shell;
  both implement official interfaces or compile official sources.

## Known limitations

- No `kernel.macho` yet; `UniXNU` uses its built-in 4-entry menu and reports
  `config: no boot entry found`.
- Missing vendor protocols (SCM/Clock/DDR/ChipInfo) are expected in the
  minimal stack and are not required for the XNU handoff.
- The ABL initrd staging range can be unaligned; the driver skips it with a
  warning. Payload builds must pad the FAT volume to 4 KiB.
- No touch/GPU/audio/network/camera; UFS/Data/root are not implemented yet.
