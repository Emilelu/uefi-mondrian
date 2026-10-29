# Vendored DarwinBootPkg (K60 / mondrian)

Base copy: `Platforms/Nothing/DarwinBootPkg` from **Chengdong-Yu/uefi-pong @
87c10fd**, which is itself a copy of `drivers/DarwinBoot24/DarwinBootPkg` from
the UniXNU-pong device repository at commit
`876bb91e55edbbd7a95c0359f7419261a33aa285`
(<https://github.com/Chengdong-Yu/UniXNU-pong>).

`mondrian.dsc` and `mondrian.fdf` reference `DarwinBootPkg/Driver/DarwinBoot.inf`
to provide the XNU loader protocol that `UniXNULib` consumes. The package is not
distributed on its own and is not one of this repository's submodules, so it is
vendored here to make a clean clone build on its own, and to retire the
device-repository `drivers/DarwinBoot22` fork from the firmware build.

The `Test/` harness and `README.md` of the upstream `drivers/DarwinBoot24` are
not needed for the firmware build and are not included.

## K60 deltas on top of the base copy

| File | Delta | Why |
| --- | --- | --- |
| `Driver/BootArgs.h` | boot_args ABI retargeted Darwin 24 -> **Darwin 22** (`DARWIN_BOOT_LINE_LENGTH` 1024 -> 608, `BootFlags` 0x470 -> 0x2d0, `MemorySizeActual` 0x478 -> 0x2d8, `sizeof` 0x480 -> 0x2e0) | The K60 payload is `ipados-16.6.1-20G81`, i.e. XNU 8796.141.3 / Darwin 22. A Darwin 24 layout puts `BootFlags` and `MemorySizeActual` 0x1a0 bytes past where that kernel reads them, which faults immediately on handoff. Every field this loader *writes* exists at the same offset in both layouts, so the retarget is confined to this header. The header documents the four values to restore for a Darwin 24 payload. |
| `Driver/DarwinBoot.c` | two `DEBUG_ERROR` lines in the "no window" path (`DARWINBOOT: no window ...`, `DARWINBOOT: free ...`) | Pong drew these numbers on the panel only. On K60 the panel text is lost as soon as the screen is reused, and the board has no serial, no network and no writable volume. Mirroring them into the Advanced Logger puts the window failure into the Tools -> Advanced Logger QR pages. |
| `Driver/DarwinBoot.c` | `DARWINBOOT24` -> `DARWINBOOT`, "Darwin 24 ABI" -> "Darwin 22 ABI" in the status lines | Those strings are read off the QR pages; a loader that reports a Darwin 24 ABI while handing over a Darwin 22 struct is actively misleading during bring-up. |
| `Driver/DarwinBoot.inf` | `BASE_NAME` `DarwinBoot24` -> `DarwinBoot` | The output is not the Darwin 24 build any more. Nothing looks the driver up by name; the DSC/FDF reference the `.inf` path. |

Line endings were normalised CRLF -> LF and file modes 0755 -> 0644 on import.

## Pong-only code retained on purpose

`ParseTree` / `ClaimPongRam` implement three device-tree contracts
(`darwinboot-memory-ranges`, `pong-allocatable-ram`, `pong-audio-dma`). They are
name-gated: `ClaimPongRam` returns `EFI_SUCCESS` before touching the memory map
when those nodes are absent, which is the case for `sm8475-xiaomi-mondrian.dtb`.
They are kept so this copy stays a faithful import and can be diffed against
upstream; do not delete them without re-checking the pong side.

## Keeping it in sync

Re-import from `uefi-pong/Platforms/Nothing/DarwinBootPkg`, normalise line
endings, then re-apply the table above.

License: BSD-2-Clause-Patent, Copyright (c) 2026, DarwinBoot contributors.
