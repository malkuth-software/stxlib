#!/usr/bin/env python3
"""prepare_vm.py: make a FreeBSD arm64 VM for C1 from a downloaded image.

    prepare_vm.py --image <FreeBSD-...-ufs.qcow2.xz> [--name stock]
                  [--checksums <CHECKSUM.SHA256>] [--storage-root DIR]
                  [--uefi-code FILE] [--qemu-img FILE] [--reset]

Steps, each checked before the next:
1. Verify the downloaded .qcow2.xz against the release's CHECKSUM.SHA256
   (FreeBSD's format: "SHA256 (<file>) = <hex>"). A missing entry or a
   mismatch stops here.
2. Decompress it once into <storage>/appliance/images/<name>.qcow2 and make
   that file read-only: the pristine image. If it is already there, reuse it.
3. Create the VM directory <storage>/appliance/vm/<name>/ with a
   copy-on-write overlay disk.qcow2 backed by the pristine image, and a
   zero-filled UEFI variable store vars.fd. QEMU's virt machine maps each
   pflash device as 64 MiB; if the firmware file is not exactly that size a
   zero-padded copy, code.fd, is made in the VM directory.

An existing VM directory is left alone unless --reset is given; --reset
removes only disk.qcow2, vars.fd and code.fd there and makes them again, so
the VM boots as if new. Nothing outside <storage>/appliance is written.

Exit status 0 when the VM is ready, 1 on any failure (the reason is printed).
Standard library only; needs qemu-img from the installed QEMU.
"""

import argparse
import hashlib
import lzma
import os
import re
import subprocess
import sys


# The sibling modules are imported from this script's own directory, so the
# scripts also run under python3 -I (which leaves that directory off the path).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vmlayout import (ApplianceLayout, QEMU_IMG_DEFAULT, STORAGE_ROOT_DEFAULT,
                      UEFI_CODE_DEFAULT, check_vm_name)

PFLASH_BYTES: int = 64 * 1024 * 1024
READ_CHUNK_BYTES: int = 4 * 1024 * 1024
CHECKSUM_LINE = re.compile(r'^SHA256 \((?P<file>[^)]+)\) = (?P<hex>[0-9a-f]{64})$')


class PrepareError(Exception):
    """A step failed; the message says which and why."""


def sha256_of_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while True:
            chunk = stream.read(READ_CHUNK_BYTES)
            if len(chunk) == 0:
                break
            digest.update(chunk)
    return digest.hexdigest()


def expected_sha256(checksums_path: str, file_name: str) -> str:
    """Return the hex digest listed for file_name, or fail if it is absent."""
    with open(checksums_path, 'r', encoding='ascii') as stream:
        for raw_line in stream:
            match = CHECKSUM_LINE.match(raw_line.strip())
            if match is not None and match.group('file') == file_name:
                return match.group('hex')
    raise PrepareError(f'{checksums_path} has no SHA256 entry for {file_name}')


def verify_download(image_xz: str, checksums_path: str) -> None:
    file_name = os.path.basename(image_xz)
    wanted = expected_sha256(checksums_path, file_name)
    actual = sha256_of_file(image_xz)
    if actual != wanted:
        raise PrepareError(f'checksum mismatch for {file_name}: listed {wanted}, file {actual}')
    print(f'verified: {file_name} sha256 {actual}')


def pristine_name(image_xz: str) -> str:
    file_name = os.path.basename(image_xz)
    if not file_name.endswith('.qcow2.xz'):
        raise PrepareError(f'expected a .qcow2.xz image, got {file_name}')
    return file_name[:-len('.xz')]


def decompress_once(image_xz: str, pristine_path: str) -> None:
    """Write the pristine image through a temporary name, then rename it.

    A failed or interrupted decompression leaves no file under the final
    name, so a later run never mistakes a partial image for a pristine one.
    """
    if os.path.exists(pristine_path):
        print(f'pristine image present, reused: {pristine_path}')
        return
    temporary_path = pristine_path + '.partial'
    if os.path.exists(temporary_path):
        os.remove(temporary_path)
    with lzma.open(image_xz, 'rb') as source, open(temporary_path, 'wb') as target:
        while True:
            chunk = source.read(READ_CHUNK_BYTES)
            if len(chunk) == 0:
                break
            target.write(chunk)
        target.flush()
        os.fsync(target.fileno())
    os.rename(temporary_path, pristine_path)
    os.chmod(pristine_path, 0o444)
    print(f'decompressed: {pristine_path}')


def remove_if_present(path: str) -> None:
    if os.path.exists(path):
        os.remove(path)


def make_overlay(qemu_img: str, pristine_path: str, disk_path: str) -> None:
    command = [qemu_img, 'create', '-q', '-f', 'qcow2', '-F', 'qcow2',
               '-b', os.path.abspath(pristine_path), disk_path]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise PrepareError(f'qemu-img failed ({completed.returncode}): {completed.stderr.strip()}')
    print(f'overlay: {disk_path} (backing {pristine_path})')


def write_zero_file(path: str, size_bytes: int) -> None:
    with open(path, 'wb') as stream:
        stream.truncate(size_bytes)


def firmware_for_vm(uefi_code: str, vm_dir: str) -> str:
    """Return the firmware file the VM boots: the installed one if it is
    exactly one pflash device in size, else a zero-padded copy in vm_dir."""
    size_bytes = os.path.getsize(uefi_code)
    if size_bytes == PFLASH_BYTES:
        return uefi_code
    if size_bytes > PFLASH_BYTES:
        raise PrepareError(f'{uefi_code} is {size_bytes} bytes, larger than a pflash device')
    padded_path = os.path.join(vm_dir, 'code.fd')
    with open(uefi_code, 'rb') as source, open(padded_path, 'wb') as target:
        target.write(source.read())
        target.truncate(PFLASH_BYTES)
    print(f'firmware padded to {PFLASH_BYTES} bytes: {padded_path}')
    return padded_path


def prepare(arguments: argparse.Namespace) -> None:
    check_vm_name(arguments.name)
    layout = ApplianceLayout(arguments.storage_root)
    if not os.path.isdir(arguments.storage_root):
        raise PrepareError(f'storage root {arguments.storage_root} does not exist')
    for tool_path in (arguments.qemu_img, arguments.uefi_code, arguments.image):
        if not os.path.isfile(tool_path):
            raise PrepareError(f'not found: {tool_path}')
    checksums_path = arguments.checksums
    if checksums_path is None:
        checksums_path = os.path.join(os.path.dirname(arguments.image), 'CHECKSUM.SHA256')

    verify_download(arguments.image, checksums_path)

    os.makedirs(layout.images_dir(), exist_ok=True)
    pristine_path = os.path.join(layout.images_dir(), pristine_name(arguments.image))
    decompress_once(arguments.image, pristine_path)

    vm_dir = layout.vm_dir(arguments.name)
    disk_path = layout.vm_disk(arguments.name)
    vars_path = layout.vm_vars(arguments.name)
    if os.path.exists(disk_path) and not arguments.reset:
        print(f'VM {arguments.name} already prepared; --reset to make it new: {vm_dir}')
        return
    os.makedirs(vm_dir, exist_ok=True)
    remove_if_present(disk_path)
    remove_if_present(vars_path)
    remove_if_present(os.path.join(vm_dir, 'code.fd'))

    make_overlay(arguments.qemu_img, pristine_path, disk_path)
    firmware_path = firmware_for_vm(arguments.uefi_code, vm_dir)
    write_zero_file(vars_path, PFLASH_BYTES)
    with open(layout.vm_backing_record(arguments.name), 'w', encoding='utf-8') as record:
        record.write(f'backing {pristine_path}\nfirmware {firmware_path}\n')
    print(f'ready: VM {arguments.name} in {vm_dir}')


def parse_arguments(argv: list) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Prepare a FreeBSD arm64 VM for C1.')
    parser.add_argument('--image', required=True, help='downloaded FreeBSD-...-ufs.qcow2.xz')
    parser.add_argument('--checksums', default=None, help='CHECKSUM.SHA256 (default: beside the image)')
    parser.add_argument('--name', default='stock', help='VM name (default: stock)')
    parser.add_argument('--storage-root', default=STORAGE_ROOT_DEFAULT)
    parser.add_argument('--uefi-code', default=UEFI_CODE_DEFAULT)
    parser.add_argument('--qemu-img', default=QEMU_IMG_DEFAULT)
    parser.add_argument('--reset', action='store_true', help='remake disk, variables and firmware copy')
    return parser.parse_args(argv)


def main(argv: list) -> int:
    arguments = parse_arguments(argv)
    try:
        prepare(arguments)
    except (PrepareError, ValueError, OSError) as failure:
        print(f'prepare_vm: {failure}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
