#!/usr/bin/env python3
"""extract_rpi_fat.py: the FAT partition of FreeBSD's RPI image, for the Pi 4
SD image (C1 step S4; appliance/disk/build_pi4_image.sh).

    extract_rpi_fat.py --image <FreeBSD-...-RPI.img.xz> --checksums <CHECKSUM.SHA256-...-RPI>
                       --out <file>

1. Verify the .img.xz against the release's checksum file (prepare_vm.py's
   check: a missing entry or a mismatch stops here).
2. Decompress it as a stream, read the MBR, and copy the first FAT slice
   (type 0x04, 0x06, 0x0b, 0x0c or 0x0e) to <file>; the rest of the image is
   not decompressed past that slice. The copy must end in the image and
   start with a boot sector signature (0x55 0xAA at bytes 510-511).
Prints the slice's type, offset and size and the copy's SHA-256.
Exit status 0 on success, 1 on any failure (the reason is printed).
Standard library only.
"""

import argparse
import hashlib
import lzma
import os
import sys
from typing import BinaryIO, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from prepare_vm import PrepareError, verify_download  # noqa: E402

SECTOR_BYTES: int = 512
CHUNK_BYTES: int = 4 * 1024 * 1024
FAT_TYPES = (0x04, 0x06, 0x0B, 0x0C, 0x0E)
# The RPI image's FAT slice is 50 MB (release/arm64/RPI.conf FAT_SIZE); a
# bound well above it keeps a corrupt table from copying the whole image.
FAT_BYTES_MAX: int = 512 * 1024 * 1024


def first_fat_slice(mbr: bytes) -> Tuple[int, int, int]:
    """(type, first byte, byte count) of the first FAT slice in the MBR."""
    if len(mbr) != SECTOR_BYTES or mbr[510] != 0x55 or mbr[511] != 0xAA:
        raise PrepareError('the image does not start with an MBR (no 0x55AA signature)')
    for index in range(4):
        entry = mbr[446 + 16 * index: 446 + 16 * (index + 1)]
        kind = entry[4]
        first = int.from_bytes(entry[8:12], 'little')
        count = int.from_bytes(entry[12:16], 'little')
        if kind in FAT_TYPES and first > 0 and count > 0:
            if count * SECTOR_BYTES > FAT_BYTES_MAX:
                raise PrepareError(f'FAT slice {index + 1} is {count} sectors, over the bound')
            return kind, first * SECTOR_BYTES, count * SECTOR_BYTES
    raise PrepareError('the MBR has no FAT slice')


def read_exactly(stream: BinaryIO, count: int) -> bytes:
    data = stream.read(count)
    if len(data) != count:
        raise PrepareError(f'the image ends early ({len(data)} of {count} bytes)')
    return data


def extract(image_xz: str, out_path: str) -> Tuple[int, int, int, str]:
    digest = hashlib.sha256()
    with lzma.open(image_xz, 'rb') as stream:
        mbr = read_exactly(stream, SECTOR_BYTES)
        kind, first, count = first_fat_slice(mbr)
        position = SECTOR_BYTES
        while position < first:
            step = min(CHUNK_BYTES, first - position)
            read_exactly(stream, step)
            position += step
        temporary = out_path + '.part'
        boot_sector: Optional[bytes] = None
        try:
            with open(temporary, 'wb') as out:
                remaining = count
                while remaining > 0:
                    chunk = read_exactly(stream, min(CHUNK_BYTES, remaining))
                    if boot_sector is None:
                        boot_sector = chunk[:SECTOR_BYTES]
                    out.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
        except (PrepareError, OSError, lzma.LZMAError):
            if os.path.exists(temporary):
                os.remove(temporary)
            raise
        if boot_sector is None or len(boot_sector) < SECTOR_BYTES or boot_sector[510] != 0x55 \
                or boot_sector[511] != 0xAA:
            os.remove(temporary)
            raise PrepareError('the FAT slice does not start with a boot sector')
        os.replace(temporary, out_path)
    return kind, first, count, digest.hexdigest()


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--image', required=True)
    parser.add_argument('--checksums', required=True)
    parser.add_argument('--out', required=True)
    arguments = parser.parse_args(argv)
    try:
        verify_download(arguments.image, arguments.checksums)
        kind, first, count, sha = extract(arguments.image, arguments.out)
    except (PrepareError, OSError, lzma.LZMAError) as failure:
        print(f'extract_rpi_fat: {failure}', file=sys.stderr)
        return 1
    print(f'extract_rpi_fat: slice type 0x{kind:02x} at byte {first}, {count} bytes, sha256 {sha}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
