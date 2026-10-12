"""Prepare a checked ESP32/S3/C3 multi-image project without overwriting SD files."""
from pathlib import Path
import argparse
import hashlib
import os
import re
import struct
import zlib

CHIPS = {"esp32": ("ESP32", 0, 0x1000),
         "esp32s3": ("ESP32-S3", 9, 0),
         "esp32c3": ("ESP32-C3", 5, 0)}


def inspect_image(data, chip_id):
    if len(data) < 24 or data[0] != 0xE9 or not 1 <= data[1] <= 16:
        raise ValueError("Invalid ESP image header")
    if struct.unpack_from("<H", data, 12)[0] != chip_id:
        raise ValueError("BIN chip ID differs from selected chip")
    offset, checksum = 24, 0xEF
    for _ in range(data[1]):
        if offset + 8 > len(data):
            raise ValueError("Truncated segment header")
        size = struct.unpack_from("<I", data, offset + 4)[0]
        offset += 8
        if size > len(data) - offset:
            raise ValueError("Truncated segment data")
        for byte in data[offset:offset + size]:
            checksum ^= byte
        offset += size
    end = (offset + 16) & ~15
    if end > len(data) or data[end - 1] != checksum:
        raise ValueError("ESP image checksum mismatch")
    if data[23]:
        if len(data) < end + 32 or hashlib.sha256(data[:end]).digest() != data[end:end + 32]:
            raise ValueError("ESP image SHA256 mismatch")
    size_code = data[3] >> 4
    if size_code > 4:
        raise ValueError("Initial ROM backend supports 1..16 MiB Flash")
    return 0x100000 << size_code


def inspect_partitions(data, flash_size, app_size):
    entries = []
    for offset in range(0, len(data), 32):
        record = data[offset:offset + 32]
        if len(record) != 32:
            raise ValueError("Truncated partition entry")
        magic = struct.unpack_from("<H", record)[0]
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            if record[16:] != hashlib.md5(data[:offset]).digest():
                raise ValueError("Partition MD5 mismatch")
            break
        if magic != 0x50AA:
            raise ValueError("Invalid partition entry")
        _, kind, subtype, address, size, label, flags = struct.unpack("<HBBII16sI", record)
        if not size or address & 4095 or size & 4095 or address + size > flash_size:
            raise ValueError("Invalid partition range")
        if any(address < old[2] + old[3] and old[2] < address + size for old in entries):
            raise ValueError("Overlapping partitions")
        entries.append((kind, subtype, address, size))
    apps = [entry for entry in entries if entry[0] == 0 and entry[2] == 0x10000]
    if len(apps) != 1 or app_size > apps[0][3] or apps[0][1] not in (0, 0x10):
        raise ValueError("Expected factory/ota_0 application at 0x10000")
    ota = [entry for entry in entries if entry[:2] == (1, 0)]
    if len(ota) > 1 or (ota and ota[0][3] != 0x2000):
        raise ValueError("Unexpected OTA data partition")
    if apps[0][1] == 0x10 and not ota:
        raise ValueError("OTA app requires OTA data partition")
    return ota[0] if ota else None


def prepare(source, sd_root, project, chip, name, baud="auto"):
    if str(baud) not in ("auto", "115200", "230400", "460800", "921600"):
        raise ValueError("Unsupported baud ceiling")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,24}", project):
        raise ValueError("Project ID must be 1..24 letters, digits, _ or -")
    if not 1 <= len(name) <= 47 or any(ord(c) < 32 or ord(c) > 126 for c in name):
        raise ValueError("Name must be 1..47 printable ASCII characters")
    device, chip_id, boot_address = CHIPS[chip]
    boot = (source / "bootloader.bin").read_bytes()
    app = (source / "firmware.bin").read_bytes()
    partitions = (source / "partitions.bin").read_bytes()
    flash_size = inspect_image(boot, chip_id)
    if inspect_image(app, chip_id) != flash_size:
        raise ValueError("Boot/app Flash size settings differ")
    if boot_address + len(boot) > 0x8000 or len(partitions) > 4096:
        raise ValueError("Bootloader/partition table exceeds its range")
    ota = inspect_partitions(partitions, flash_size, len(app))
    images = [(boot_address, "bootloader.bin", boot), (0x8000, "partitions.bin", partitions),
              (0x10000, "firmware.bin", app)]
    if ota:
        images.append((ota[2], "ota-reset.bin", b"\xff" * ota[3]))
    images.sort()
    previous_end = 0
    for address, filename, data in images:
        if address < previous_end:
            raise ValueError("Overlapping image erase sectors")
        previous_end = address + ((len(data) + 4095) & ~4095)
    lines = [f"name={name}", f"device={device}", f"baud={baud}", f"flash_size=0x{flash_size:08X}"]
    lines.extend(f"image={filename},0x{address:08X},0x{len(data):08X},0x{zlib.crc32(data):08X}"
                 for address, filename, data in images)
    outputs = {filename: data for _, filename, data in images}
    outputs["fw.txt"] = ("\n".join(lines) + "\n").encode("ascii")
    root = sd_root.resolve(strict=True)
    target = (root / "prog/fw" / project).resolve()
    if not target.is_relative_to(root):
        raise ValueError("Destination escaped SD root")
    for filename, data in outputs.items():
        dest = (target / filename).resolve()
        if not dest.is_relative_to(root):
            raise ValueError("File escaped SD root")
        if dest.exists() and (not dest.is_file() or dest.read_bytes() != data):
            raise ValueError(f"Existing different file preserved: {dest}")
    target.mkdir(parents=True, exist_ok=True)
    # Manifest is published last; interrupted copies are not selectable projects.
    for filename, data in outputs.items():
        dest = target / filename
        if not dest.exists():
            with dest.open("xb") as out:
                out.write(data)
                out.flush()
                os.fsync(out.fileno())
        if dest.read_bytes() != data:
            raise OSError(f"Readback failed: {dest}")
    return target, lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--sd", type=Path, required=True)
    parser.add_argument("--project", default="esp32s3-rly4-pro")
    parser.add_argument("--chip", choices=CHIPS, default="esp32s3")
    parser.add_argument("--name", default="HG-ESP32-S3-RLY4-PRO")
    parser.add_argument("--baud", choices=["auto", "115200", "230400", "460800", "921600"], default="auto")
    args = parser.parse_args()
    try:
        target, lines = prepare(args.source, args.sd, args.project, args.chip, args.name, args.baud)
    except (ValueError, OSError) as error:
        parser.exit(1, f"ERROR: {error}\n")
    print(target)
    print("\n".join(lines))
    print("Files copied and read back. Source files and other SD projects preserved.")
    print("OTA data reset image included when needed; NVS/filesystem partitions preserved.")


if __name__ == "__main__":
    main()
