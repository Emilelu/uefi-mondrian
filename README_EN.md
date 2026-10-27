# uefi-mondrian

K60 (Redmi K60, **mondrian**, Qualcomm **SM8475**) UEFI firmware for the UniXNU
port, derived from Project-Silicium / Mu-Silicium.

## Status (2026-10-27)

| Area | State |
| --- | --- |
| UEFI boot chain | OK |
| Display (ABL splash + SimpleFbDxe) | OK |
| Buttons (official KeypadDevice chain + SPMI backend) | OK |
| Tools -> Advanced Logger QR | OK |
| XNU payload | TODO (no `kernel.macho`) |
| Vendor Android stack | removed (minimal DXE) |

## Build

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

See `docs/K60-CHANGES.md` for the platform changes and known limitations.

## Data safety

`fastboot boot` only during bring-up. Never flash `persist` or `flash all`.
Root/Data will use a dedicated `unixnu` partition; Android `userdata` stays
untouched.

## License

BSD-2-Clause-Patent; upstream components and the `Binaries` submodule keep
their own licenses.
