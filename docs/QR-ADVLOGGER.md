# Panic readability on K60: Advanced Logger + QR

This board has no serial console, no network and no writable firmware volume. A
panic that only lands in a debug log costs a reboot and is never read. The
Advanced Logger buffer plus the QR plugin are the only way to get firmware and
loader diagnostics off the phone, so treat them as a first-class boot feature,
not a debugging extra.

## 1. What is wired

| Piece | Module | Role |
| --- | --- | --- |
| Producer | `mondrianPkg/Drivers/AdvLogDxe/AdvLogDxe.inf` | Hosts the AdvLoggerPkg DxeCore-instance sources and explicitly calls `DxeCoreAdvancedLoggerLibConstructor`, so `gAdvancedLoggerProtocolGuid` exists without patching the pinned `Mu_Basecore` DxeCore. Buffer is `PcdAdvancedLoggerPages = 1024` (4 MiB). |
| Sink | `UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf` | Reads the buffer through `AdvancedLoggerAccessLib`, splits it into 720-byte blocks, wraps them in the Onesend envelope (`LMS1` magic, CRC32 per block) and renders one QR code per block in a rotating carousel. Publishes the `Tools -> Advanced Logger` menu entry through `gUniXNUMenuPluginProtocolGuid`. |
| Ordering | `mondrianPkg/Include/APRIORI.inc` | `AdvLogDxe` is the first platform driver after DxeCore/Pcd, immediately followed by `AdvancedLogDxe`. The plugin therefore has a log to read before BDS starts. |
| Encoder | `MsGraphicsPkg/Library/QrEncoderLib` | `QrEncodeData (QrAutoVersion, QrECLevel_L, QrByteMode, ...)`, sized to the menu content rect. |

Both modules are in the FV. Verify in a build with:

```sh
grep -E 'AdvLogDxe|AdvancedLogDxe' Build/mondrianPkg/DEBUG_CLANGPDB/FV/FVMAIN.Fv.txt
# AdvLogDxe        FILE_GUID 4F7A9C21-6B3E-4D58-9A10-2C5D8E71F3A4
# AdvancedLogDxe   FILE_GUID 8D4E7F50-3A2C-4C9A-9E1B-50C4C8472D41
```

## 2. What reaches the log

`PcdDebugPrintErrorLevel` is `0x80000000` (ERROR only) and
`PcdUniXNUDebugLevel` is `0x80000000`, so anything that must survive to the QR
pages is emitted at `DEBUG_ERROR`. The K60-relevant producers are:

| Source | Line prefix | Meaning |
| --- | --- | --- |
| `UniXNURamDiskDxe` | `RAMOOPS: ...` / `RAMOOPS| ...` | XNU console mirror at `0xA7000000`, dumped before any allocation. This is the previous boot's kernel panic text. |
| `UniXNURamDiskDxe` | `UNIXNURAMDISK: initrd ...`, `... FAT boot sector at start+0x...`, `... selfread=... volumes=...` | Whether the payload FAT volume was found and whether DiskIo/FAT claimed it. |
| `DarwinBoot` | `DARWINBOOT: no window 0x... biggest run ...`, `DARWINBOOT: free 0x... + 0x...` | The kernel window allocation failed. The same numbers are drawn on the panel, but only the log copy survives the screen being reused. |
| `UniXNULib` | `config: N file system volume(s)`, `storage: blockio=... connected=... volumes=...` | Storage enumeration result, also shown as a notice on the menu. |
| UniXNU app | `UNIXNU: ...` | Menu/payload decisions. |

## 3. Reading the log off the phone

1. Boot the firmware image (see `../README.md` for the artifact and
   `fastboot boot` procedure). Stay in the UniXNU menu.
2. **Tools -> Advanced Logger**. The status line shows
   `Advanced Logger <page>/<pages>  left <n>`.
3. On a second phone or a laptop with a camera, open
   **<https://onesend.01mvp.com>** ("扫传") and start the receive scan.
4. Hold the full QR code in frame and keep scanning while the carousel rotates.
   The receiver reassembles the frames into one file named
   `<model>-YYYYMMDD-HHMMSS.log`, where `<model>` is `PcdUniXNUDeviceModel`
   (`Redmi K60` in `mondrian.dsc`).
5. Rotation speed: **Down / `j`** slower, **Up / `k`** faster (five gears,
   5 s down to 250 ms). **Enter** or **Esc** returns to the menu.

A block is 720 payload bytes plus a 28-byte header and a 4-byte CRC, i.e. one
752-byte frame per QR page, so a full 4 MiB buffer needs several hundred pages.
That is expected: read the *tail*, which is where the newest lines are.

## 4. Reading a failed hand-off (the A1 case)

The point of this path is the boot that dies between the menu and the kernel.
The recipe is deliberately "boot, fail, reboot, read":

1. Boot, let the hand-off fail (black screen, reset, or return to the menu).
2. Reboot the same firmware image **without** reflashing anything.
3. Tools -> Advanced Logger, scan.

The new log opens with the `RAMOOPS` block from the *previous* boot, because
`UniXNURamDiskDxe` dumps the reserved mirror at `0xA7000000` (magic `CGBD`,
12-byte header, 2 MiB ring) before it touches the heap. How to read what you get:

| What the log shows | What it means |
| --- | --- |
| `RAMOOPS| ` lines with kernel text | XNU reached console init and panicked; the text after the marker is the panic. |
| `RAMOOPS: sig=0x... size=0x... (no xnu log)` | The kernel never wrote the mirror: the fault is before kernel console init, or the hand-off never happened. |
| `DARWINBOOT: no window ...` | The loader could not find a contiguous kernel window; the `DARWINBOOT: free` lines list the runs it did see. Compare against `Library/MemoryMapLib`. |
| `UNIXNURAMDISK: initrd ... not inside mapped memory, skipping` | ABL pointed `/chosen` at a staging range the K60 MMU does not map. |
| `UNIXNURAMDISK: no FAT boot sector in 0x... bytes` | The payload ramdisk is not a FAT volume (missing or unpadded payload). |
| No `UNIXNURAMDISK` lines at all | The driver never ran: check the FV, or that the payload was built against this FD. |

Because the loader now logs its window failure as well as drawing it, a failure
that previously left only a transient panel line is recoverable after a reboot.

## 5. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `Tools` has no `Advanced Logger` entry | Plugin not dispatched: depex unmet or module missing from FV | Check `gAdvancedLoggerProtocolGuid` is produced (`AdvLogDxe` in APRIORI), and both INFs are in `mondrian.fdf` |
| `Advanced Logger: no log` | Producer installed but nothing was written at ERROR level | Check `PcdDebugPrintErrorLevel`, and that the driver you expect logs at `DEBUG_ERROR` |
| Scan progress stalls | Carousel faster than the decoder | Press Down / `j` to slow down, keep the camera steady, scan a full cycle |
| QR too small to focus | Content rect smaller than the frame | The plugin caps the code at `min(width, height)`, minus 1/8, max 1200 px; on a 1440x3200 panel this is not the limiting factor |
| Log ends mid-line | Buffer wrapped, or you scanned an older page | Scan again and keep the carousel running until the receiver reports completion |

## 6. Where this lives in the tree

- Producer: `Platforms/Xiaomi/mondrianPkg/Drivers/AdvLogDxe/`
- QR plugin: `Platforms/Xiaomi/UniXNUPkg/Driver/AdvancedLogDxe/`
- Vendoring and K60 deltas of the plugin's package: `Platforms/Xiaomi/UniXNUPkg/VENDORED.md`
- APRIORI order: `Platforms/Xiaomi/mondrianPkg/Include/APRIORI.inc`
