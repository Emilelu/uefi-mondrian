# Vendored UniXNUPkg (K60 / mondrian)

Base copy: `Platforms/Nothing/UniXNUPkg` from **Chengdong-Yu/uefi-pong @ 87c10fd**,
which is itself a vendored copy of `package/UniXNUPkg` from the UniXNU framework
at commit `40d21cc65171c3a24faa130a2a8e899f85790485`
(<https://github.com/UniXNU/UniXNU>).

Vendored into `Platforms/Xiaomi/UniXNUPkg` so that `uefi-mondrian` builds from a
clean clone: `mondrian.dsc`, `mondrian.fdf` and `Include/APRIORI.inc` reference
this package by name, and it is not one of this repository's submodules.

`Platforms/Xiaomi` is the first entry after the workspace root in
`mondrianPkg/DeviceBuild.py::CommonPlatform.PackagesPath`, so an in-tree copy
resolves ahead of any `UNIXNU_PACKAGES_PATH` entry the UniXNU build driver adds.
No change is needed in the device repository's `board.toml`.

## Contents

Device-agnostic UniXNU boot manager, unchanged in structure:

- `Library/UniXNULib` — Config / Toml / Menu / Kext / Volume / Slice / Inject /
  Boot / Ui / Font / Logo
- `Application/UniXNU`, `Driver/UniXNUDxe` — menu application and BDS registrar
- `Driver/AdvancedLogDxe` — Tools -> Advanced Logger, renders the Advanced
  Logger buffer as Onesend-framed QR pages (uses `QrEncoderLib` from
  MsGraphicsPkg and `AdvancedLoggerAccessLib` from AdvLoggerPkg)
- `Driver/UniXNURamDiskDxe` — publishes the FDT initrd as a block device
- `Test/` — reference fixtures, not referenced by `mondrian.dsc`

## K60 deltas applied on top of the base copy

Pong's copy is pristine upstream. The K60 tree additionally needs the firmware
half of `UniXNU-mondrian/patches/unixnu-core/k60-unixnu-core.patch`, which until
now was applied to an out-of-tree UniXNU checkout. It is applied here instead so
the vendored copy *is* the K60 behaviour:

| File | Delta | Why K60 needs it |
| --- | --- | --- |
| `Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.c` | `DumpRamoops()` | Reads the XNU console mirror at `0xA7000000` (magic `CGBD`, 12-byte header, 2 MiB ring) and prints the tail as `DEBUG_ERROR` before any allocation. This is the panic text A1 reads out of the QR pages. |
| same | `IsRangeMapped()` | ABL points `/chosen` initrd at staging areas the K60 MMU does not map (`0xB5760000`); publishing them aborts on the first block read. |
| same | FAT boot-sector scan | Android 12+ ABL concatenates the `boot.img` ramdisk (our payload FAT) with a ~22 MiB `vendor_boot` ramdisk, so sector 0 is a cpio header. Walk byte-wise for a real `0x55AA` + `FAT` boot sector. |
| same | connect / `selfread` / volume counters | A raw `BlockIo` nobody binds is a dead end; the log shows whether DiskIo and FAT claimed the device. |
| `Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf` | `SiliciumPkg.dec`, `MemoryMapHelperLib`, DiskIo/SimpleFileSystem | Dependencies of the guards above. |
| `Library/UniXNULib/Config.c` | built-in `timeout = -1`, `fallback_timeout = 300`, per-volume probe logs | A countdown silently boots the first entry ("Return to firmware") and drops the machine back into Android. |
| `Library/UniXNULib/Main.c` | unconditional `UniXNUConnectAll()` twice, `blockio=/connected=/volumes=` on the menu | An application launched from the boot menu path arrives with every volume unbound, so the payload ramdisk carries no file system and the config stays invisible. |

The `scripts/unixnu/bootimg.py` hunk of that patch is not firmware and is out of
scope for this repository; it stays in the device repository.

Line endings were normalised CRLF -> LF and file modes 0755 -> 0644 on import.

## Keeping it in sync

Re-import from `uefi-pong/Platforms/Nothing/UniXNUPkg`, normalise line endings,
then re-apply the table above. Do not edit the upstream files in place for
reasons unrelated to K60; record any further K60 delta in this file.

License: BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors.
