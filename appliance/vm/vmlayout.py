"""Where the appliance VM's files live on the M4, in one place.

Owner rule (2026-11-13, notes decisions/2026-11-13-m4-storage.md): bulky
material (downloads, source trees, VM images and disks, long logs) goes on
the M4's /Volumes/Storage, under /Volumes/Storage/stxlib_workers/. Scripts
and their small outputs stay in the repository.

Layout under STORAGE_ROOT/appliance/:

    images/                 pristine downloads, never modified
        <image>.qcow2.xz    as downloaded
        CHECKSUM.SHA256     the release's checksum file, as downloaded
        <image>.qcow2       decompressed once by prepare_vm.py, made read-only
    vm/<name>/              one directory per VM
        disk.qcow2          copy-on-write overlay whose backing file is the
                            pristine image; deleting it resets the VM
        vars.fd             the VM's own UEFI variable store
        backing.txt         the pristine image the overlay was made from
    logs/boot-timing/<name>/<run-id>/
        serial.raw          every byte from the serial console, unchanged
        serial.lines        the same text split into lines, each prefixed by
                            seconds since QEMU was started
        stages.tsv          the stages found, with times and intervals
        command.txt         the exact QEMU command line
    src/                    source trees (the integrator's shallow clone of
                            freebsd-src at the pinned commit goes here or is
                            named by its own path)
    obj/                    FreeBSD build object trees (MAKEOBJDIRPREFIX)

The integrator chooses the download paths; scripts take them as arguments,
and these defaults only name where things go when nothing else is said.
"""

import os
from dataclasses import dataclass

STORAGE_ROOT_DEFAULT: str = '/Volumes/Storage/stxlib_workers'

# Homebrew's QEMU installs its firmware here on an Apple Silicon Mac. The
# integrator reported edk2-aarch64-code.fd present (QEMU 10.1.3) and no
# aarch64 variable store, so prepare_vm.py makes one per VM.
QEMU_BINARY_DEFAULT: str = '/opt/homebrew/bin/qemu-system-aarch64'
QEMU_IMG_DEFAULT: str = '/opt/homebrew/bin/qemu-img'
UEFI_CODE_DEFAULT: str = '/opt/homebrew/share/qemu/edk2-aarch64-code.fd'


@dataclass(frozen=True)
class ApplianceLayout:
    """Paths derived from one storage root. Plain data: no file is touched."""

    storage_root: str

    def appliance_root(self) -> str:
        return os.path.join(self.storage_root, 'appliance')

    def images_dir(self) -> str:
        return os.path.join(self.appliance_root(), 'images')

    def vm_dir(self, vm_name: str) -> str:
        return os.path.join(self.appliance_root(), 'vm', vm_name)

    def vm_disk(self, vm_name: str) -> str:
        return os.path.join(self.vm_dir(vm_name), 'disk.qcow2')

    def vm_vars(self, vm_name: str) -> str:
        return os.path.join(self.vm_dir(vm_name), 'vars.fd')

    def vm_backing_record(self, vm_name: str) -> str:
        return os.path.join(self.vm_dir(vm_name), 'backing.txt')

    def timing_logs_dir(self, vm_name: str) -> str:
        return os.path.join(self.appliance_root(), 'logs', 'boot-timing', vm_name)

    def src_dir(self) -> str:
        return os.path.join(self.appliance_root(), 'src')

    def obj_dir(self) -> str:
        return os.path.join(self.appliance_root(), 'obj')


def check_vm_name(vm_name: str) -> None:
    """A VM name becomes a directory name: letters, digits, '-' and '_' only."""
    if len(vm_name) == 0 or len(vm_name) > 64:
        raise ValueError(f'VM name must be 1 to 64 characters: {vm_name!r}')
    for character in vm_name:
        if not (character.isascii() and (character.isalnum() or character in '-_')):
            raise ValueError(f'VM name may hold only letters, digits, - and _: {vm_name!r}')
