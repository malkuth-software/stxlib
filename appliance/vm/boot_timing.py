#!/usr/bin/env python3
"""boot_timing.py: boot a C1 VM under QEMU and time each boot stage.

    boot_timing.py [--name stock] [--runs 3] [--timeout 180]
                   [--skip-countdown] [--storage-root DIR] [--qemu FILE]
                   [--uefi-code FILE] [--memory-mib 2048] [--cpus 2]
                   [--accel hvf] [--machine virt] [--dry-run]

Boots the VM made by prepare_vm.py with UEFI firmware, a serial console on
QEMU's standard input and output, no display and no monitor. Every byte of
the serial console is kept with the host time it arrived, measured from just
before QEMU was started. The stages in bootstages.py are recognised in the
text; a run ends when the last required stage (the login prompt) appears,
when QEMU exits, or at --timeout, and QEMU is then stopped.

--skip-countdown sends one Enter when the loader's countdown or menu
appears, so one stock image gives two measurements: as shipped (the 10 s
autoboot default, notes planning/stages/A0-boot-path.md 1.3.6) and with the
countdown skipped.

The first run after prepare_vm.py starts from an empty UEFI variable store,
where the firmware creates its boot entries; the summary marks it, and the
median is taken over the later runs when there are any.

Output per run, under <storage>/appliance/logs/boot-timing/<name>/<run-id>/:
serial.raw, serial.lines, stages.tsv, command.txt, qemu-stderr.txt; and a
summary on standard output: one line per run, then minimum, median and
maximum of each interval.

Exit status 0 when every run reached every required stage, 1 otherwise (a
failed run is a failed check, whatever the log says), 2 for bad arguments.
Standard library only.
"""

import argparse
import datetime
import os
import selectors
import socket
import signal
import statistics
import subprocess
import sys
import time
from typing import Dict, List, Optional


# The sibling modules are imported from this script's own directory, so the
# scripts also run under python3 -I (which leaves that directory off the path).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import re

from bootstages import (APPLIANCE_INTERVALS, INTERVALS, Stage, StageTracker, appliance_stage_list,
                        default_stage_list, pid1_marker_values)
from vmlayout import (ApplianceLayout, QEMU_BINARY_DEFAULT, STORAGE_ROOT_DEFAULT,
                      UEFI_CODE_DEFAULT, check_vm_name)

READ_CHUNK_BYTES: int = 65536
STOP_GRACE_SECONDS: float = 5.0
POLL_SECONDS: float = 0.05
LINE_BUFFER_LIMIT_CHARACTERS: int = 65536
FINAL_STAGE: str = 'login_prompt'


def firmware_path(layout: ApplianceLayout, vm_name: str, requested: Optional[str]) -> str:
    """The padded copy made by prepare_vm.py wins over the installed file."""
    if requested is not None:
        return requested
    padded_path = os.path.join(layout.vm_dir(vm_name), 'code.fd')
    if os.path.isfile(padded_path):
        return padded_path
    return UEFI_CODE_DEFAULT


def qemu_command(arguments: argparse.Namespace, layout: ApplianceLayout) -> List[str]:
    """The exact command line, built in one place so command.txt records it."""
    cpu_model = 'host'
    if arguments.accel != 'hvf':
        cpu_model = 'max'
    code_path = firmware_path(layout, arguments.name, arguments.uefi_code)
    monitor = 'none'
    if getattr(arguments, 'hang_dump', False):
        # A monitor socket, used only when a boot times out (hang_dump()).
        monitor = f'unix:{monitor_socket(layout, arguments.name)},server=on,wait=off'
    # A host-only port forward to the guest's SSH (C1 S4's login check):
    # bound to 127.0.0.1, so nothing outside the M4 reaches the guest.
    netdev = 'user,id=net0'
    ssh_port = getattr(arguments, 'ssh_forward_port', None)
    if ssh_port is not None:
        netdev += f',hostfwd=tcp:127.0.0.1:{int(ssh_port)}-:22'
    command: List[str] = [
        arguments.qemu,
        '-machine', arguments.machine,
        '-accel', arguments.accel,
        '-cpu', cpu_model,
        '-m', str(arguments.memory_mib),
        '-smp', str(arguments.cpus),
        '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={code_path}',
        '-drive', f'if=pflash,format=raw,unit=1,file={layout.vm_vars(arguments.name)}',
    ]
    # The appliance disk (C1 S4): a raw image built inside the VM. Attached
    # as a second disk while it is built; alone, so that the firmware boots
    # it and the loader sees it as disk0, when the appliance runs.
    appliance_disk = getattr(arguments, 'appliance_disk', None)
    if appliance_disk is not None and getattr(arguments, 'appliance_only', False):
        command += ['-drive', f'if=none,id=disk0,format=raw,file={appliance_disk}',
                    '-device', 'virtio-blk-pci,drive=disk0']
    else:
        command += ['-drive', f'if=none,id=disk0,format=qcow2,file={layout.vm_disk(arguments.name)}',
                    '-device', 'virtio-blk-pci,drive=disk0']
        if appliance_disk is not None:
            command += ['-drive', f'if=none,id=disk1,format=raw,file={appliance_disk}',
                        '-device', 'virtio-blk-pci,drive=disk1']
    command += [
        '-netdev', netdev,
        '-device', 'virtio-net-pci,netdev=net0',
        '-display', 'none',
        '-monitor', monitor,
        '-chardev', 'stdio,id=serial0,signal=off',
        '-serial', 'chardev:serial0',
        '-no-reboot',
    ]
    if arguments.firmware_timeout_ms is not None:
        # edk2's ArmVirtQemu takes its boot-menu timeout from QEMU when
        # "-boot menu=on" is given: splash-time N ms, rounded up to whole
        # seconds, becomes PcdPlatformBootTimeOut and the UEFI "Timeout"
        # variable (edk2-stable202408 OvmfPkg/Library/QemuBootOrderLib/
        # QemuBootOrderLib.c:2373-2420, ArmVirtPkg/Library/
        # PlatformBootManagerLib/PlatformBm.c:505-517). Without it the
        # platform default applies: 5 s (ArmVirtPkg/ArmVirtQemu.dsc:330).
        command.extend(['-boot', f'menu=on,splash-time={arguments.firmware_timeout_ms}'])
    return command


def monitor_socket(layout: ApplianceLayout, vm_name: str) -> str:
    return os.path.join(layout.vm_dir(vm_name), 'monitor.sock')


def hang_dump(path: str, log_dir: str) -> None:
    """Ask a hung guest's QEMU monitor for the CPU registers (every vCPU)
    and save the answer as hang-registers.txt, for locating the PC."""
    answer = b''
    try:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(3.0)
        client.connect(path)
        client.sendall(b'info registers -a\n')
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                break
            if len(chunk) == 0:
                break
            answer += chunk
        client.close()
    except OSError as failure:
        answer += f'\nboot_timing: monitor failed: {failure}\n'.encode()
    with open(os.path.join(log_dir, 'hang-registers.txt'), 'wb') as stream:
        stream.write(answer)


class RunRecord:
    """One boot: its tracker, its outcome and where its logs are."""

    def __init__(self, run_id: str, log_dir: str, stages: List[Stage], intervals: List[tuple],
                 final_stage: str) -> None:
        self.run_id: str = run_id
        self.log_dir: str = log_dir
        self.tracker: StageTracker = StageTracker(stages)
        self.intervals: List[tuple] = intervals
        self.final_stage: str = final_stage
        # In-guest times read from our PID 1's marker, when the run printed one.
        self.guest_values: Dict[str, float] = {}
        self.outcome: str = 'not started'
        self.elapsed_seconds: float = 0.0

    def complete(self) -> bool:
        return self.outcome == 'reached final stage' and len(self.tracker.missing_required()) == 0


def stop_process(process: subprocess.Popen) -> None:
    """SIGTERM, a short grace, then SIGKILL; always reaps the process."""
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=STOP_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def write_stages(record: RunRecord) -> None:
    path = os.path.join(record.log_dir, 'stages.tsv')
    with open(path, 'w', encoding='utf-8') as stream:
        stream.write('stage\tseconds\tsource\n')
        for stage in record.tracker.stages:
            seconds = record.tracker.seen_seconds.get(stage.name)
            shown = 'not seen' if seconds is None else f'{seconds:.3f}'
            stream.write(f'{stage.name}\t{shown}\t{stage.source}\n')
        stream.write('\ninterval\tseconds\n')
        for interval_name, start_stage, end_stage in record.intervals:
            value = record.tracker.interval_seconds(start_stage, end_stage)
            shown = 'n/a' if value is None else f'{value:.3f}'
            stream.write(f'{interval_name}\t{shown}\n')
        for name, value in sorted(record.guest_values.items()):
            stream.write(f'{name}\t{value:.6f}\n')
        stream.write(f'\noutcome\t{record.outcome}\nelapsed\t{record.elapsed_seconds:.3f}\n')


def boot_once(arguments: argparse.Namespace, command: List[str], record: RunRecord) -> None:
    os.makedirs(record.log_dir, exist_ok=False)
    with open(os.path.join(record.log_dir, 'command.txt'), 'w', encoding='utf-8') as stream:
        stream.write(' '.join(command) + '\n')
    raw_log = open(os.path.join(record.log_dir, 'serial.raw'), 'wb')
    lines_log = open(os.path.join(record.log_dir, 'serial.lines'), 'w', encoding='utf-8')
    stderr_log = open(os.path.join(record.log_dir, 'qemu-stderr.txt'), 'wb')
    countdown_answered = False
    line_buffer = ''
    started = time.monotonic()
    try:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=stderr_log)
    except OSError as failure:
        record.outcome = f'QEMU did not start: {failure}'
        raw_log.close()
        lines_log.close()
        stderr_log.close()
        write_stages(record)
        return
    selector = selectors.DefaultSelector()
    try:
        selector.register(process.stdout, selectors.EVENT_READ)
        record.outcome = 'timed out'
        while True:
            seconds = time.monotonic() - started
            if seconds > arguments.timeout:
                break
            ready = selector.select(timeout=POLL_SECONDS)
            if len(ready) == 0:
                if process.poll() is not None:
                    record.outcome = f'QEMU exited with status {process.returncode}'
                    break
                continue
            data = os.read(process.stdout.fileno(), READ_CHUNK_BYTES)
            seconds = time.monotonic() - started
            if len(data) == 0:
                process.wait()
                record.outcome = f'QEMU exited with status {process.returncode}'
                break
            raw_log.write(data)
            line_buffer += data.decode('utf-8', errors='replace')
            line_buffer = flush_lines(line_buffer, seconds, lines_log)
            if len(line_buffer) > LINE_BUFFER_LIMIT_CHARACTERS:
                # A guest that never ends a line: log what there is and go on.
                lines_log.write(f'{seconds:10.3f} {line_buffer}\n')
                line_buffer = ''
            newly_seen = record.tracker.feed(data, seconds)
            if arguments.skip_countdown and not countdown_answered and 'loader_countdown' in newly_seen:
                process.stdin.write(b'\r')
                process.stdin.flush()
                countdown_answered = True
                lines_log.write(f'{seconds:10.3f} [boot_timing: sent Enter to skip the countdown]\n')
            if record.final_stage in record.tracker.seen_seconds:
                record.outcome = 'reached final stage'
                break
    finally:
        record.elapsed_seconds = time.monotonic() - started
        selector.close()
        if record.outcome == 'timed out' and getattr(arguments, 'hang_dump', False):
            hang_dump(arguments.monitor_socket, record.log_dir)
        stop_process(process)
        if len(line_buffer) > 0:
            lines_log.write(f'{record.elapsed_seconds:10.3f} {line_buffer}\n')
        raw_log.close()
        lines_log.close()
        stderr_log.close()
    write_stages(record)


def flush_lines(buffer: str, seconds: float, lines_log) -> str:
    """Write each finished line with its arrival time; return the unfinished rest."""
    line_start = 0
    index = 0
    while index < len(buffer):
        if buffer[index] == '\n' or buffer[index] == '\r':
            line = buffer[line_start:index]
            if len(line) > 0:
                lines_log.write(f'{seconds:10.3f} {line}\n')
            line_start = index + 1
        index += 1
    return buffer[line_start:]


def read_guest_values(record: RunRecord) -> None:
    """Adopt the in-guest times of our PID 1's marker from serial.raw, and
    add them to stages.tsv."""
    path = os.path.join(record.log_dir, 'serial.raw')
    if not os.path.isfile(path):
        return
    with open(path, 'rb') as stream:
        text = stream.read().decode('utf-8', errors='replace')
    values = pid1_marker_values(text)
    if values is None:
        return
    record.guest_values = values
    write_stages(record)


def summary_line(record: RunRecord, first_after_prepare: bool) -> str:
    parts: List[str] = [record.run_id]
    for interval_name, start_stage, end_stage in record.intervals:
        value = record.tracker.interval_seconds(start_stage, end_stage)
        shown = 'n/a' if value is None else f'{value:.3f}'
        parts.append(f'{interval_name}={shown}')
    for name, value in sorted(record.guest_values.items()):
        parts.append(f'{name}={value:.4f}')
    parts.append(record.outcome)
    missing = record.tracker.missing_required()
    if len(missing) > 0:
        parts.append('missing: ' + ','.join(missing))
    if first_after_prepare:
        parts.append('(first boot on a fresh variable store)')
    return '  '.join(parts)


def interval_statistics(records: List[RunRecord]) -> List[str]:
    lines: List[str] = []
    rows: List[tuple] = list(records[0].intervals) if len(records) > 0 else []
    for name in ('pid1_uptime', 'pid1_counter'):
        rows.append((name, None, None))
    for interval_name, start_stage, end_stage in rows:
        values: List[float] = []
        for record in records:
            if end_stage is None and start_stage is None and interval_name in record.guest_values:
                values.append(record.guest_values[interval_name])
                continue
            if end_stage is None:
                continue
            value = record.tracker.interval_seconds(start_stage, end_stage)
            if value is not None:
                values.append(value)
        if len(values) == 0 and interval_name in ('pid1_uptime', 'pid1_counter'):
            continue
        if len(values) == 0:
            lines.append(f'{interval_name}: no values')
            continue
        lines.append(f'{interval_name}: min {min(values):.3f}  median {statistics.median(values):.3f}  '
                     f'max {max(values):.3f}  (n={len(values)})')
    return lines


def vars_store_is_fresh(path: str) -> bool:
    """True when the UEFI variable store is still all zero bytes (never booted)."""
    with open(path, 'rb') as stream:
        while True:
            chunk = stream.read(READ_CHUNK_BYTES)
            if len(chunk) == 0:
                return True
            if chunk.count(0) != len(chunk):
                return False


def parse_arguments(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Time the boot stages of a C1 VM.')
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=180.0, help='seconds per run')
    parser.add_argument('--skip-countdown', action='store_true')
    add_vm_arguments(parser)
    parser.add_argument('--label', default=None, help='run-id label (default: asis or skip)')
    parser.add_argument('--require', default=None,
                        help='a regular expression the console must show for a run to count '
                             '(for example the kernel ident)')
    parser.add_argument('--end', choices=('login', 'pid1'), default='login',
                        help='last stage: the stock login prompt, or our PID 1 marker (appliance)')
    parser.add_argument('--stop-after-failure', action='store_true',
                        help='stop after the first run that misses a required stage')
    parser.add_argument('--hang-dump', action='store_true',
                        help='on a timed-out boot, save every vCPU register (QEMU monitor) first')
    parser.add_argument('--dry-run', action='store_true', help='print the QEMU command and stop')
    return parser.parse_args(argv)


def add_vm_arguments(parser: argparse.ArgumentParser) -> None:
    """The options that shape the VM, shared with guest_session.py so both
    boot exactly the same machine."""
    parser.add_argument('--name', default='stock')
    parser.add_argument('--storage-root', default=STORAGE_ROOT_DEFAULT)
    parser.add_argument('--qemu', default=QEMU_BINARY_DEFAULT)
    parser.add_argument('--uefi-code', default=None)
    parser.add_argument('--memory-mib', type=int, default=2048)
    parser.add_argument('--cpus', type=int, default=2)
    parser.add_argument('--accel', default='hvf')
    parser.add_argument('--machine', default='virt')
    parser.add_argument('--firmware-timeout-ms', type=int, default=None,
                        help='pass -boot menu=on,splash-time=N (edk2 boot-menu timeout)')
    parser.add_argument('--appliance-disk', default=None,
                        help='raw appliance disk image: a second disk, or the only one with --appliance-only')
    parser.add_argument('--appliance-only', action='store_true',
                        help='boot the appliance disk alone (C1 S4)')
    parser.add_argument('--ssh-forward-port', type=int, default=None,
                        help='forward 127.0.0.1:PORT on the host to the guest port 22')


def main(argv: List[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        check_vm_name(arguments.name)
    except ValueError as failure:
        print(f'boot_timing: {failure}', file=sys.stderr)
        return 2
    if arguments.runs < 1 or arguments.runs > 50 or arguments.timeout <= 0:
        print('boot_timing: --runs must be 1 to 50 and --timeout positive', file=sys.stderr)
        return 2
    layout = ApplianceLayout(arguments.storage_root)
    arguments.monitor_socket = monitor_socket(layout, arguments.name)
    command = qemu_command(arguments, layout)
    if arguments.dry_run:
        print(' '.join(command))
        return 0
    for required_path in (arguments.qemu, layout.vm_disk(arguments.name), layout.vm_vars(arguments.name)):
        if not os.path.isfile(required_path):
            print(f'boot_timing: not found: {required_path} (run prepare_vm.py first)', file=sys.stderr)
            return 1

    session = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    variant = 'skip' if arguments.skip_countdown else 'asis'
    if arguments.label is not None:
        check_vm_name(arguments.label)
        variant = arguments.label
    records: List[RunRecord] = []
    first_flags: List[bool] = []
    for run_index in range(arguments.runs):
        run_id = f'{session}-{variant}-{run_index + 1}'
        if arguments.end == 'pid1':
            stages = appliance_stage_list()
            intervals = APPLIANCE_INTERVALS
            final_stage = 'pid1_marker'
        else:
            stages = default_stage_list()
            intervals = INTERVALS
            final_stage = FINAL_STAGE
        if arguments.require is not None:
            stages.append(Stage('required_text', re.compile(arguments.require), True, False,
                                'boot_timing --require'))
        record = RunRecord(run_id, os.path.join(layout.timing_logs_dir(arguments.name), run_id), stages,
                           intervals, final_stage)
        first_flags.append(vars_store_is_fresh(layout.vm_vars(arguments.name)))
        boot_once(arguments, command, record)
        read_guest_values(record)
        records.append(record)
        print(summary_line(record, first_flags[-1]))
        if arguments.stop_after_failure and not record.complete():
            print(f'boot_timing: run {run_index + 1} failed; --stop-after-failure: no further runs')
            break

    later_records: List[RunRecord] = []
    for index in range(len(records)):
        if not first_flags[index]:
            later_records.append(records[index])
    if len(later_records) == 0:
        later_records = records
    for line in interval_statistics(later_records):
        print(line)
    print(f'logs: {layout.timing_logs_dir(arguments.name)}/{session}-{variant}-*')

    all_complete = True
    for record in records:
        if not record.complete():
            all_complete = False
    if not all_complete:
        print('boot_timing: FAIL: at least one run did not reach every required stage')
        return 1
    print('boot_timing: PASS: every run reached every required stage')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
