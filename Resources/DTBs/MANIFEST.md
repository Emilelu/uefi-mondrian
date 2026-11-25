# Device trees shipped with the firmware

`build_uefi.py` appends `Resources/DTBs/<board>.dtb` to the compressed FD when
`Resources/Configs/<board>.toml` sets `[boot_image_kernel] append_dtb = true`.
The file therefore has to exist in the checkout; it is not produced by the EDK2
build.

## mondrian.dtb

| Field | Value |
| --- | --- |
| board | `mondrian` (Redmi K60) |
| size | 4,024 bytes |
| sha256 | `2d044eaa08b7e58c0510a33da795c960085e13d6943cd4c25e0e050e0bfa33cd` |
| source | `UniXNU-mondrian` `dts/mondrian.dts`, with `-i UniXNU-sm8475-common/dts` for `sm8475.dtsi` |
| generator | `dtc -I dts -O dtb` (DTC 1.7.0) |

Reproduce with:

```sh
dtc -I dts -O dtb \
    -i ../UniXNU-sm8475-common/dts \
    -o mondrian.dtb \
    ../UniXNU-mondrian/dts/mondrian.dts
```

The framework compiles the same source to the same bytes: the `hardware.dtb`
the payload build leaves in `out/<board>/<os>/<board>/device-tree/` is
byte-identical to this file (same sha256), which is why the boot image size is
reproducible across the two build paths.

Do not substitute the full `Platforms/Xiaomi/mondrianPkg/FdtBlob/` blob: it is
480 KiB of vendor DTBO material, it is not the board description the loader
parses, and an image built with it does not boot (see the note in
`Platforms/Xiaomi/mondrianPkg/FdtBlob/README.md`).

## pong.dtb

Vendor blob used by the `pong` configuration only; unrelated to `mondrian`.
