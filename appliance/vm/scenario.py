#!/usr/bin/env python3
"""scenario.py: boot a VM once and drive it through a list of steps.

Each --step is one of:
  expect:<seconds>:<regex>   wait until the serial console shows a match
                             (searched from the end of the previous match)
  monitor:<command>          send one command to QEMU's monitor (for
                             example system_powerdown: the ACPI power button)
  exit:<seconds>             wait until QEMU exits by itself (the guest
                             powered off)

The steps run in order; the first that fails stops the scenario, and QEMU is
stopped (SIGTERM, then SIGKILL) if it is still running. Every byte of the
console is kept in serial.raw, timestamped lines in serial.lines, QEMU's
answers to monitor commands in monitor.txt, and each step's outcome and host
time in steps.tsv, under
<storage>/appliance/logs/scenarios/<vm>/<session>-<label>/. Standard output:
one line per step, with the matched text for expect steps. Exit status 0
when every step passed, 1 otherwise, 2 on a usage error.

The QEMU command is boot_timing.py's, with the monitor socket enabled.
"""

import argparse
import datetime
import os
import re
import selectors
import shlex
import socket
import subprocess
import sys
import time
from typing import List, Optional, Tuple

# The sibling modules are imported from this script's own directory, so the
# script also runs under python3 -I.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from boot_timing import add_vm_arguments, flush_lines, monitor_socket, qemu_command, stop_process  # noqa: E402
from bootstages import ESCAPE_SEQUENCE  # noqa: E402
from vmlayout import ApplianceLayout, check_vm_name  # noqa: E402

POLL_SECONDS: float = 0.05
READ_CHUNK_BYTES: int = 65536
# The console is input: a guest that floods it stops the scenario.
TEXT_LIMIT_CHARACTERS: int = 16 * 1024 * 1024
MONITOR_CONNECT_SECONDS: float = 5.0


class Step:
    def __init__(self, kind: str, seconds: float, argument: str) -> None:
        self.kind = kind
        self.seconds = seconds
        self.argument = argument
        self.pattern: Optional['re.Pattern'] = re.compile(argument) if kind == 'expect' else None


def parse_step(text: str) -> Step:
    kind, separator, rest = text.partition(':')
    if separator != ':':
        raise ValueError(f'step without a kind: {text!r}')
    if kind == 'expect':
        seconds_text, separator, regex = rest.partition(':')
        if separator != ':' or len(regex) == 0:
            raise ValueError(f'expect takes <seconds>:<regex>: {text!r}')
        return Step('expect', float(seconds_text), regex)
    if kind == 'monitor':
        if len(rest) == 0 or '\n' in rest:
            raise ValueError(f'monitor takes one command: {text!r}')
        return Step('monitor', 0.0, rest)
    if kind == 'exit':
        return Step('exit', float(rest), '')
    if kind == 'host':
        seconds_text, separator, command = rest.partition(':')
        if separator != ':' or len(shlex.split(command)) == 0:
            raise ValueError(f'host takes <seconds>:<command>: {text!r}')
        return Step('host', float(seconds_text), command)
    raise ValueError(f'unknown step kind {kind!r}')


class Console:
    """QEMU's serial output, read without blocking, logged and kept as text
    for matching."""

    def __init__(self, process: subprocess.Popen, log_dir: str, started: float) -> None:
        self.process = process
        self.started = started
        self.selector = selectors.DefaultSelector()
        self.selector.register(process.stdout, selectors.EVENT_READ)
        self.raw_log = open(os.path.join(log_dir, 'serial.raw'), 'wb')
        self.lines_log = open(os.path.join(log_dir, 'serial.lines'), 'w', encoding='utf-8')
        self.line_buffer = ''
        self.text = ''
        self.closed = False

    def seconds(self) -> float:
        return time.monotonic() - self.started

    def pump(self) -> None:
        """Read what is available within one poll interval."""
        if self.closed:
            time.sleep(POLL_SECONDS)
            return
        ready = self.selector.select(timeout=POLL_SECONDS)
        if len(ready) == 0:
            return
        data = os.read(self.process.stdout.fileno(), READ_CHUNK_BYTES)
        if len(data) == 0:
            self.closed = True
            return
        self.raw_log.write(data)
        decoded = data.decode('utf-8', errors='replace')
        self.line_buffer = flush_lines(self.line_buffer + decoded, self.seconds(), self.lines_log)
        self.text += ESCAPE_SEQUENCE.sub('', decoded)
        if len(self.text) > TEXT_LIMIT_CHARACTERS:
            raise RuntimeError('console output beyond the limit')

    def close(self) -> None:
        if len(self.line_buffer) > 0:
            self.lines_log.write(f'{self.seconds():10.3f} {self.line_buffer}\n')
        self.selector.close()
        self.raw_log.close()
        self.lines_log.close()


MONITOR_ANSWER_SECONDS: float = 5.0
MONITOR_PROMPT: bytes = b'(qemu)'


def monitor_answer_complete(answer: bytes, command: bytes) -> bool:
    """True once QEMU has echoed the command and printed its prompt after
    it: the command has been read and run. The greeting's own prompt, which
    arrives before the echo, does not count."""
    echo_at = answer.find(command)
    if echo_at < 0:
        return False
    return answer.find(MONITOR_PROMPT, echo_at + len(command)) >= 0


def send_monitor_command(path: str, command: str) -> str:
    """Send one command and wait for QEMU to have run it (its echo, then the
    next prompt) before closing: a connection closed early can lose the
    command. Raises RuntimeError when no such answer comes in time."""
    deadline = time.monotonic() + MONITOR_CONNECT_SECONDS
    while True:
        try:
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(0.5)
            client.connect(path)
            break
        except OSError as failure:
            client.close()
            if time.monotonic() > deadline:
                raise RuntimeError(f'monitor: {failure}')
            time.sleep(0.1)
    encoded = command.encode()
    answer = b''
    try:
        client.sendall(encoded + b'\n')
        deadline = time.monotonic() + MONITOR_ANSWER_SECONDS
        while not monitor_answer_complete(answer, encoded) and time.monotonic() < deadline:
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                continue
            if len(chunk) == 0:
                break
            answer += chunk
    finally:
        client.close()
    text = answer.decode('utf-8', errors='replace')
    if not monitor_answer_complete(answer, encoded):
        raise RuntimeError(f'monitor: no answer to {command!r}: {text!r}')
    return text


def run_steps(steps: List[Step], console: Console, monitor_path: str, monitor_log: str) -> Tuple[bool, List[str]]:
    rows: List[str] = []
    search_from = 0
    for index, step in enumerate(steps, start=1):
        if step.kind == 'expect':
            deadline = console.seconds() + step.seconds
            match = None
            while match is None:
                match = step.pattern.search(console.text, search_from)
                if match is not None:
                    break
                if console.seconds() > deadline or (console.closed and console.process.poll() is not None):
                    break
                console.pump()
            if match is None:
                rows.append(f'{index}\texpect\tfailed\t{console.seconds():.3f}\t{step.argument}')
                return False, rows
            search_from = match.end()
            line_end = console.text.find('\n', match.start())
            shown = console.text[match.start():line_end if line_end >= 0 else match.end()].strip()
            rows.append(f'{index}\texpect\tok\t{console.seconds():.3f}\t{shown}')
            continue
        if step.kind == 'monitor':
            answer = send_monitor_command(monitor_path, step.argument)
            with open(monitor_log, 'a', encoding='utf-8') as stream:
                stream.write(f'--- {console.seconds():.3f} {step.argument}\n{answer}\n')
            rows.append(f'{index}\tmonitor\tsent\t{console.seconds():.3f}\t{step.argument}')
            continue
        if step.kind == 'host':
            # A command on the host (no shell), passing on exit status 0
            # within its time; the console keeps being read meanwhile.
            deadline = console.seconds() + step.seconds
            process = subprocess.Popen(shlex.split(step.argument), stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            while process.poll() is None and console.seconds() <= deadline:
                console.pump()
            if process.poll() is None:
                process.kill()
                process.wait()
                rows.append(f'{index}\thost\tfailed\t{console.seconds():.3f}\ttimed out: {step.argument}')
                return False, rows
            output = process.stdout.read().decode('utf-8', 'replace').strip().replace('\n', ' | ')[:200]
            outcome = 'ok' if process.returncode == 0 else 'failed'
            rows.append(f'{index}\thost\t{outcome}\t{console.seconds():.3f}\tstatus {process.returncode} {output}')
            if process.returncode != 0:
                return False, rows
            continue
        if step.kind == 'exit':
            deadline = console.seconds() + step.seconds
            while console.process.poll() is None and console.seconds() <= deadline:
                console.pump()
            if console.process.poll() is None:
                rows.append(f'{index}\texit\tfailed\t{console.seconds():.3f}\tQEMU still running')
                return False, rows
            rows.append(f'{index}\texit\tok\t{console.seconds():.3f}\tstatus {console.process.returncode}')
            continue
        rows.append(f'{index}\t{step.kind}\tunknown step\t{console.seconds():.3f}\t')
        return False, rows
    return True, rows


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    add_vm_arguments(parser)
    parser.add_argument('--step', action='append', required=True)
    parser.add_argument('--label', default='scenario')
    arguments = parser.parse_args(argv)
    try:
        check_vm_name(arguments.name)
        check_vm_name(arguments.label)
        steps = [parse_step(text) for text in arguments.step]
    except ValueError as failure:
        print(f'scenario: {failure}', file=sys.stderr)
        return 2
    layout = ApplianceLayout(arguments.storage_root)
    arguments.hang_dump = True
    arguments.monitor_socket = monitor_socket(layout, arguments.name)
    command = qemu_command(arguments, layout)
    for required_path in (arguments.qemu, layout.vm_disk(arguments.name), layout.vm_vars(arguments.name)):
        if not os.path.isfile(required_path):
            print(f'scenario: not found: {required_path}', file=sys.stderr)
            return 1
    session = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    log_dir = os.path.join(layout.appliance_root(), 'logs', 'scenarios', arguments.name, f'{session}-{arguments.label}')
    os.makedirs(log_dir, exist_ok=False)
    with open(os.path.join(log_dir, 'command.txt'), 'w', encoding='utf-8') as stream:
        stream.write(' '.join(command) + '\n')
    stderr_log = open(os.path.join(log_dir, 'qemu-stderr.txt'), 'wb')
    started = time.monotonic()
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr_log)
    console = Console(process, log_dir, started)
    passed = False
    rows: List[str] = []
    try:
        passed, rows = run_steps(steps, console, arguments.monitor_socket, os.path.join(log_dir, 'monitor.txt'))
    except RuntimeError as failure:
        rows.append(f'-\terror\tfailed\t{console.seconds():.3f}\t{failure}')
    finally:
        stop_process(process)
        console.close()
        stderr_log.close()
    with open(os.path.join(log_dir, 'steps.tsv'), 'w', encoding='utf-8') as stream:
        stream.write('step\tkind\toutcome\tseconds\tdetail\n')
        stream.write('\n'.join(rows) + '\n')
    for row in rows:
        print(row)
    print(f'logs: {log_dir}')
    if not passed:
        print('scenario: FAIL')
        return 1
    print('scenario: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
