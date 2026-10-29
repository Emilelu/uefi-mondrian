# uefi-mondrian

K60 (Redmi K60, **mondrian**, Qualcomm **SM8475**) UEFI firmware for the UniXNU
port. Derived from Project-Silicium / Mu-Silicium; the platform package is
`Platforms/Xiaomi/mondrianPkg`.

Part of the K60 port together with
[UniXNU-mondrian](https://github.com/Emilelu/UniXNU-mondrian) and the
[UniXNU](https://github.com/UniXNU/UniXNU) core.

## Status (2026-10-27)

| Area | State | Notes |
| --- | --- | --- |
| UEFI boot chain | OK | ABL -> `fastboot boot` -> BootShim -> SEC/PEI/DXE/BDS -> UniXNU menu |
| Display | OK | ABL splash framebuffer + `SimpleFbDxe` GOP; no GPU driver |
| Buttons | OK | official `KeypadDeviceDxe`/`KeypadDxe` + `K60KeypadDeviceLib` (SPMI PON/GPIO) |
| Tools -> Advanced Logger | OK | `AdvLogDxe` producer + `AdvancedLogDxe` QR plugin; 4 MB buffer |
| Auto-reset | OK | 60 s no-key cold reset to fastboot; if the reset returns, the menu stays alive |
| XNU payload | TODO | no `kernel.macho`; see the device repo `docs/NEXT-STEPS.md` |
| Vendor Android stack | removed | SMEM/clock/PMIC/UFS/charging/etc. are not in the minimal DXE |

## Build

Use the UniXNU build driver; do not call `build_uefi.py` directly (it lacks the
UniXNU package paths):

```sh
git clone --recurse-submodules https://github.com/Emilelu/uefi-mondrian
git clone https://github.com/Emilelu/UniXNU-mondrian
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
cd UniXNU
git checkout ae68700
../UniXNU-mondrian/scripts/apply-core-patches.sh .
./unixnu defconfig mondrian_defconfig -D ../UniXNU-mondrian
./unixnu build firmware --uefi ../uefi-mondrian -D ../UniXNU-mondrian
```

Output: `Build/mondrianPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`.
The firmware DTB is tracked at
`Platforms/Xiaomi/mondrianPkg/FdtBlob/sm8475-xiaomi-mondrian.dtb`.

### Self-contained build

`UniXNUPkg` and `DarwinBootPkg` are vendored under `Platforms/Xiaomi`, so the
firmware also builds from a clean clone with no UniXNU framework checkout and no
device-repository `DarwinBoot22` copy on `PACKAGES_PATH`:

```sh
stuart_build -c Platforms/Xiaomi/mondrianPkg/DeviceBuild.py \
  TARGET=DEBUG FD_BASE=0xA7400000 FD_SIZE=0x300000 FD_BLOCKS=0x300 \
  DEVICE_MODEL=0 ENABLE_SECUREBOOT=0
```

The UniXNU driver above is still the supported path when a bootable payload is
needed, because it also builds the payload FAT and the Android boot image. Its
`apply-core-patches.sh` remains necessary for the `scripts/unixnu/bootimg.py`
half of the patch; the firmware half is already applied inside the vendored
`UniXNUPkg` (see `Platforms/Xiaomi/UniXNUPkg/VENDORED.md`).

### Booting an image

Bring-up boots an image, it does not flash one:

```powershell
D:\Android\platform-tools\fastboot.exe boot <payload.img>
```

The firmware image inside the payload is `SILICIUM_UEFI.fd-bootshim`
(`BootShim.bin` + the FD). Never `fastboot flash boot` and never write an
Android partition. `docs/QR-ADVLOGGER.md` is the procedure for reading the
panic log off the phone after a failed hand-off.

## Changes from upstream

See `docs/K60-CHANGES.md`. Short version: K60 platform package and memory map,
minimal open-source DXE/BDS, official KeypadDevice chain with an SPMI K60
backend, an Advanced Logger producer/plugin pair, and the UniXNU/DarwinBoot
integration. Vendor `Binaries` are unmodified.

## Data safety

Bring-up uses `fastboot boot` only; no partition is written. Never flash
`persist` or run `flash all`. Root/Data storage will use a dedicated `unixnu`
partition (dual-system); see the device repo `docs/DUAL-SYSTEM.md`.

## License

BSD-2-Clause-Patent for the platform code. Upstream components keep their own
licenses; `Binaries` remains the official Project-Silicium submodule.
