# uefi-mondrian

UEFI firmware for the Redmi K60 (codename **mondrian**, Qualcomm **SM8475**), used by the
UniXNU port. Derived from Project-Silicium / Mu-Silicium; the platform package lives at
`Platforms/Xiaomi/mondrianPkg`.

[中文版](README.md)

The K60 port spans three repositories: this one (UEFI firmware),
[UniXNU-mondrian](https://github.com/Emilelu/UniXNU-mondrian) (device support and porting
records), and [Emilelu/UniXNU](https://github.com/Emilelu/UniXNU) (core framework and the `unixnu` CLI; official upstream [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU)).

> Last updated: 2026-11-12. Building and bring-up use `fastboot boot` only; no partition is
> written.

## Status (2026-11-12)

| Area | State | Notes |
| --- | --- | --- |
| UEFI boot chain | ✅ | ABL → `fastboot boot` → BootShim → SEC/PEI/DXE/BDS → UniXNU menu |
| Display | ✅ | ABL splash framebuffer + `SimpleFbDxe` GOP; no GPU driver |
| Buttons | ✅ | official `KeypadDeviceDxe`/`KeypadDxe` + `K60KeypadDeviceLib` (SPMI PON/GPIO) |
| Tools → Advanced Logger | ✅ | `AdvLogDxe` producer + `AdvancedLogDxe` QR plugin; 4 MB buffer |
| Auto-reset | ✅ | 60 s no-key cold reset to fastboot; if the reset returns, the menu stays alive |
| XNU payload | ⬜ | this repo carries no `kernel.macho`; bootable payloads come from the device repo's `unixnu build payload` |
| Vendor Android stack | removed | SMEM/clock/PMIC/UFS/charging/etc. are not in the minimal DXE |

Full verification on 2026-11-12 at `bd5cfa6`: `./unixnu build firmware` completed in about
2m30s and produced `Build/mondrianPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` at
**3,145,728 bytes (fixed 3 MiB)**. `scripts/unixnu/firmware.py` checks the artifact size and fails the build otherwise.

## Build

Use the UniXNU build driver; do not call `build_uefi.py` directly (it lacks the UniXNU package
paths):

```sh
git clone --recurse-submodules https://github.com/Emilelu/uefi-mondrian
git clone https://github.com/Emilelu/UniXNU-mondrian
git clone https://github.com/Emilelu/UniXNU
cd UniXNU
../UniXNU-mondrian/scripts/apply-core-patches.sh .
./unixnu defconfig mondrian_defconfig -D ../UniXNU-mondrian
./unixnu build firmware --uefi ../uefi-mondrian -D ../UniXNU-mondrian
```

Do not hardcode a framework commit: the firmware build only depends on `scripts/unixnu/` and
`patches/unixnu-core/k60-unixnu-core.patch`. Record the revision actually used in the build log:

```sh
git -C ../UniXNU rev-parse HEAD
git -C ../UniXNU log -1 --format='%h %s'
```

Output: `Build/mondrianPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`.

### Self-contained build

`UniXNUPkg` and `DarwinBootPkg` are vendored under `Platforms/Xiaomi`, so the firmware also
builds from a clean clone with no UniXNU framework checkout and no device-repository
`DarwinBoot22` copy on `PACKAGES_PATH`:

```sh
stuart_build -c Platforms/Xiaomi/mondrianPkg/DeviceBuild.py \
  TARGET=DEBUG FD_BASE=0xA7400000 FD_SIZE=0x300000 FD_BLOCKS=0x300 \
  DEVICE_MODEL=0 ENABLE_SECUREBOOT=0
```

Rebuilds are not byte-identical: the PE headers carry the build timestamp, so two builds of the
same source produce different `.fd` hashes. Identify an image by the source commit, not by a
hash carried over from another build.

The UniXNU driver above is still the supported path when a bootable payload is needed, because
it also builds the payload FAT and the Android boot image. Its `apply-core-patches.sh` remains
necessary for the `scripts/unixnu/bootimg.py` half of the patch; the firmware half is already
applied inside the vendored `UniXNUPkg` (see `Platforms/Xiaomi/UniXNUPkg/VENDORED.md`).

## Booting an image

Bring-up boots an image, it does not flash one:

```powershell
D:\Android\platform-tools\fastboot.exe boot <payload.img>
```

The firmware image inside the payload is `SILICIUM_UEFI.fd-bootshim` (`BootShim.bin` + the FD).
**Never `fastboot flash boot` and never write an Android partition.**

⚠️ Do not pass `Platforms/Xiaomi/mondrianPkg/FdtBlob/sm8475-xiaomi-mondrian.dtb` (the full
480 KB DTB) to `unixnu bootimg --dtb`: the resulting image will not boot. The DTB `bootimg`
uses is the one in the kernel-area tail, produced by `unixnu build payload`.

The procedure for reading the panic log off the phone after a failed hand-off is in
[`docs/QR-ADVLOGGER.md`](docs/QR-ADVLOGGER.md).

## Submodules and pins

`git submodule status` is the authoritative query (the table below is a 2026-11-12 snapshot):

| Submodule | Commit |
| --- | --- |
| `Binaries` | `9f4dbfd` |
| `Common/Mu` | `b8d46c7` |
| `Common/Mu_OEM_Sample` | `b70b4b7` |
| `Mu_Basecore` | `6f805f2` (`release/202608`) |
| `Silicium-ACPI` | `f50bc11` |
| `Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl` | `67b5686` |

To check whether a pin still exists upstream (a missing one breaks a clean clone):

```sh
git ls-remote https://github.com/microsoft/mu_basecore <sha>
```

The previous `Mu_Basecore` pin `3d2a9fa` no longer exists upstream, so a clean clone failed
outright; `bd5cfa6` repinned it to `6f805f2` (a commit on `release/202608`), see issue #4.
Submodule clones need full history — do not add `--depth`.

## Changes from upstream

See [`docs/K60-CHANGES.md`](docs/K60-CHANGES.md). Short version: K60 platform package and memory
map, minimal open-source DXE/BDS, official KeypadDevice chain with an SPMI K60 backend, an
Advanced Logger producer/plugin pair, and the UniXNU/DarwinBoot integration. Vendor `Binaries`
are unmodified.

## Documentation index

| Document | Contents |
| --- | --- |
| [`Status.md`](Status.md) | firmware status snapshot |
| [`Building.md`](Building.md) | generic upstream build notes (Windows/Linux dependencies, `build_uefi.py`) |
| [`docs/K60-CHANGES.md`](docs/K60-CHANGES.md) | platform changes vs upstream and known limitations |
| [`docs/QR-ADVLOGGER.md`](docs/QR-ADVLOGGER.md) | Advanced Logger / QR log retrieval and failed-hand-off triage |
| [`AGENTS.md`](AGENTS.md) | repository working rules (branches, commit style, build preconditions) |

CI note: the UEFI image workflow (`active_devices.yml`) runs on manual `workflow_dispatch` only;
a push does not trigger GitHub Actions.

## Data safety

Bring-up uses `fastboot boot` only; no partition is written. Never flash `persist` or run
`flash all`. Root/Data storage will use a dedicated `unixnu` partition (dual-system); see the
device repo `docs/DUAL-SYSTEM.md`.

## License

BSD-2-Clause-Patent for the platform code. Upstream components keep their own licenses;
`Binaries` remains the official Project-Silicium submodule.
