#!/usr/bin/env python3
"""Pack the Pong firmware volume and device tree in an Android boot image v4 container."""
import argparse
import gzip
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu-root', type=Path, default=root.parent / 'UniXNU')
    parser.add_argument('--ramdisk', type=Path)
    parser.add_argument('--fd', type=Path)
    parser.add_argument('--output-dir', type=Path, default=root / 'out/pong/boot')
    args = parser.parse_args()

    sys.path.insert(0, str(args.unixnu_root / 'scripts'))
    from unixnu.bootimg import compile_shim

    config = tomllib.loads((root / 'Resources/Configs/pong.toml').read_text())
    base = config['uefi_fd']['base']
    size = config['uefi_fd']['size']
    fd = args.fd or root / 'out/Build/pongPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd'
    dtb = root / 'Resources/DTBs/pong.dtb'
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    image = output / 'pong-unixnu-uefi.img'
    ramdisk = args.ramdisk.read_bytes() if args.ramdisk else b''
    firmware = fd.read_bytes()

    with tempfile.TemporaryDirectory(prefix='pong-bootimg-') as temporary:
        scratch = Path(temporary)
        shim = compile_shim(root, base, size, True, scratch)
        (scratch / 'kernel').write_bytes(gzip.compress(shim + firmware, mtime=0) + dtb.read_bytes())
        (scratch / 'ramdisk').write_bytes(ramdisk)
        subprocess.run([
            sys.executable, str(root / 'Resources/Scripts/mkbootimg.py'),
            '--header_version', '4', '--kernel', str(scratch / 'kernel'),
            '--ramdisk', str(scratch / 'ramdisk'), '-o', str(image),
        ], check=True)

    print(image)


if __name__ == '__main__':
    main()
