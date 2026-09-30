"""Prepare the H723 inspection package, preserving existing nonidentical files."""
from pathlib import Path
import argparse
import hashlib
import struct
import zlib


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sd', required=True, type=Path)
    p.add_argument('--image', required=True, type=Path)
    p.add_argument('--loader', required=True, type=Path)
    a = p.parse_args()
    image, loader = a.image.read_bytes(), a.loader.read_bytes()
    if not 8 <= len(image) <= 0x100000:
        p.error('Expected complete H723 BIN, 8 bytes..1MiB')
    sp, reset = struct.unpack_from('<II', image)
    if not (reset & 1 and 0x08000000 <= (reset & ~1) < 0x08000000 + len(image)):
        p.error('Reset vector does not match an image at 0x08000000')
    if len(image) >= 0x448 and struct.unpack_from('<I', image, 0x400)[0] == 0x56455220:
        if struct.unpack_from('<I', image, 0x444)[0] != 0x08000000:
            p.error('Embedded firmware address is not 0x08000000')
    # Pin the installed, inspected loader build. A different loader needs inspection first.
    if hashlib.sha256(loader).hexdigest() != '639a988e15f5e1adebd2939ff79510e759389cbfe41ea4cacc5b961ce491838c':
        p.error('Loader differs from the inspected CubeCLT 1.20.0 0x483.stldr')
    manifest = (f'name=H723 GFX firmware\n'
                f'device=STM32H72x/STM32H73x\n'
                f'algo=/prog/loaders/st/0x483.stldr\n'
                f'image=app.bin @ 0x08000000\n'
                f'crc32=0x{zlib.crc32(image):08X}\n'
                f'loader_crc32=0x{zlib.crc32(loader):08X}\n').encode('ascii')
    readme = (f'H723 SD package -- file inspection phase\n\n'
              f'CLI: prog check h723\n'
              f'Target: STM32H723ZGT6; internal flash base 0x08000000, size 1MiB.\n'
              f'fw/h723/app.bin: copy of {a.image.name}; {len(image)} bytes.\n'
              f'Image SHA256: {hashlib.sha256(image).hexdigest()}\n'
              f'Loader: STM32CubeCLT 1.20.0 / STM32CubeProgrammer FlashLoader/0x483.stldr\n'
              f'Loader SHA256: {hashlib.sha256(loader).hexdigest()}\n'
              f'fw.txt crc32/loader_crc32: IEEE CRC32 of the entire files.\n'
              f'The check command reads files only. It does not execute the loader or flash the target.\n'
              f'CRC and vector checks do not prove board compatibility or firmware authenticity.\n').encode('utf-8')
    files = {'prog/loaders/st/0x483.stldr': loader, 'prog/fw/h723/app.bin': image,
             'prog/fw/h723/fw.txt': manifest, 'prog/README.txt': readme}
    root = a.sd.resolve(strict=True)
    # Preflight all destinations before writing any file. Never overwrite user data.
    for name, data in files.items():
        dest = (root / name).resolve()
        if not dest.is_relative_to(root):
            p.error(f'Destination escaped SD root: {dest}')
        if dest.exists() and (not dest.is_file() or dest.read_bytes() != data):
            p.error(f'Existing different file preserved: {dest}')
    for name, data in files.items():
        dest = root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            with dest.open('xb') as f:
                f.write(data)
                f.flush()
                import os
                os.fsync(f.fileno())
        if dest.read_bytes() != data:
            raise RuntimeError(f'Readback mismatch: {dest}')
        print(f'{dest}: {len(data)} bytes, CRC32 {zlib.crc32(data):08X}')
    print(f'Initial SP 0x{sp:08X}, Reset 0x{reset:08X}; original input preserved.')


if __name__ == '__main__':
    main()
