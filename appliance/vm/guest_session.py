#!/usr/bin/env python3
"""guest_session.py: boot a C1 VM, run shell commands in it as root, power off.

    guest_session.py --commands FILE [--name stock] [--serve-dir DIR]
                     [--define NAME=VALUE ...] [--timeout 240]
                     [the VM options of boot_timing.py]

For changing what is inside the VM's disk from the M4 without mounting UFS
on macOS: the VM boots exactly as boot_timing.py boots it (same QEMU
command), the script logs in on the serial console as root, runs each line
of FILE as one shell command, records its output and exit status, and then
powers the VM off (QEMU runs with -no-reboot, so power-off ends it).

Login: the stock FreeBSD VM image's root account has no password. If the
console asks for one, the session stops: it never sends a password.

FILE: one command per line; blank lines and lines starting with '#' are
skipped. "{http}" is replaced by the address at which the guest reaches
--serve-dir (served read-only on the host's 127.0.0.1 for the session's
length; QEMU's user network shows the host as 10.0.2.2), and "{NAME}" by
each --define NAME=VALUE.

Output under <storage>/appliance/logs/sessions/<name>/<session-id>/:
serial.raw (every byte), commandNN.txt (each command and its output) and
summary.txt. Standard output: one line per command with its exit status.
Exit status 0 only if every command exited 0 and the VM powered off.
Standard library only; Python 3.9 (the M4's /usr/bin/python3).
"""

import argparse
import datetime
import functools
import http.server
import os
import re
import selectors
import subprocess
import sys
import threading
import time
from typing import Dict, List, Optional

# The sibling modules are imported from this script's own directory, so the
# scripts also run under python3 -I (which leaves that directory off the path).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bootstages import strip_escapes
from boot_timing import add_vm_arguments, qemu_command, stop_process
from vmlayout import ApplianceLayout, check_vm_name

READ_CHUNK_BYTES: int = 65536
POLL_SECONDS: float = 0.05
POWER_OFF_SECONDS: float = 90.0
# Text kept for matching. A command's output beyond this is still in
# serial.raw; the matching buffer keeps only its tail.
PENDING_LIMIT_CHARACTERS: int = 8 * 1024 * 1024
LOGIN_PROMPT = re.compile(r'login: $')
AFTER_LOGIN = re.compile(r'Password:|# $')
DONE_MARKER = re.compile(r'@@DONE (\d+)@@')
GUEST_HOST_ADDRESS: str = '10.0.2.2'


class SessionError(Exception):
    """The session cannot go on; the message says where it stopped."""


class SerialConsole:
    """The guest's serial console through QEMU's standard input and output."""

    def __init__(self, process: subprocess.Popen, raw_log, started: float, deadline: float) -> None:
        self.process: subprocess.Popen = process
        self.raw_log = raw_log
        self.started: float = started
        self.deadline: float = deadline
        self.pending: str = ''
        self.selector = selectors.DefaultSelector()
        self.selector.register(process.stdout, selectors.EVENT_READ)

    def close(self) -> None:
        self.selector.close()

    def send(self, text: str) -> None:
        self.process.stdin.write(text.encode('utf-8'))
        self.process.stdin.flush()

    def read_once(self) -> None:
        if time.monotonic() > self.deadline:
            raise SessionError('session timed out')
        ready = self.selector.select(timeout=POLL_SECONDS)
        if len(ready) == 0:
            if self.process.poll() is not None:
                raise SessionError(f'QEMU exited with status {self.process.returncode}')
            return
        data = os.read(self.process.stdout.fileno(), READ_CHUNK_BYTES)
        if len(data) == 0:
            self.process.wait()
            raise SessionError(f'QEMU exited with status {self.process.returncode}')
        self.raw_log.write(data)
        self.pending += strip_escapes(data.decode('utf-8', errors='replace'))
        if len(self.pending) > PENDING_LIMIT_CHARACTERS:
            self.pending = self.pending[-PENDING_LIMIT_CHARACTERS:]

    def expect(self, pattern: 're.Pattern', what: str) -> 're.Match':
        """Wait for pattern; return its match and drop the text up to its end.
        The text before the match is kept in self.consumed for the caller."""
        while True:
            match = pattern.search(self.pending)
            if match is not None:
                self.consumed = self.pending[:match.start()]
                self.pending = self.pending[match.end():]
                return match
            try:
                self.read_once()
            except SessionError as failure:
                raise SessionError(f'while waiting for {what}: {failure}')


def read_commands(path: str, replacements: Dict[str, str]) -> List[str]:
    commands: List[str] = []
    with open(path, 'r', encoding='utf-8') as stream:
        for raw_line in stream:
            line = raw_line.strip()
            if len(line) == 0 or line.startswith('#'):
                continue
            for key in replacements:
                line = line.replace('{' + key + '}', replacements[key])
            if '@@DONE' in line:
                raise SessionError(f'a command may not contain the completion marker: {line}')
            commands.append(line)
    return commands


def parse_defines(defines: Optional[List[str]]) -> Dict[str, str]:
    replacements: Dict[str, str] = {}
    if defines is None:
        return replacements
    for item in defines:
        name, separator, value = item.partition('=')
        if separator != '=' or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name) or name == 'http':
            raise SessionError(f'--define takes NAME=VALUE with a plain name other than http: {item!r}')
        replacements[name] = value
    return replacements


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """Serves one directory read-only (GET and HEAD only) without logging to stderr."""

    def log_message(self, format_string: str, *arguments) -> None:
        return


def start_server(serve_dir: str) -> http.server.ThreadingHTTPServer:
    handler = functools.partial(QuietHandler, directory=serve_dir)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    thread = threading.Thread(target=server.serve_forever, name='serve-dir', daemon=True)
    thread.start()
    return server


def run_session(arguments: argparse.Namespace, commands: List[str], session_dir: str,
                summary: List[str]) -> None:
    """Boot, log in, run the commands, power off; append what happened to
    summary as it happens, so a failure keeps the lines before it."""
    layout = ApplianceLayout(arguments.storage_root)
    command_line = qemu_command(arguments, layout)
    with open(os.path.join(session_dir, 'command.txt'), 'w', encoding='utf-8') as stream:
        stream.write(' '.join(command_line) + '\n')
    raw_log = open(os.path.join(session_dir, 'serial.raw'), 'wb')
    stderr_log = open(os.path.join(session_dir, 'qemu-stderr.txt'), 'wb')
    started = time.monotonic()
    process = subprocess.Popen(command_line, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=stderr_log)
    console = SerialConsole(process, raw_log, started, started + arguments.timeout)
    try:
        console.expect(LOGIN_PROMPT, 'the login prompt')
        summary.append(f'login prompt at {time.monotonic() - started:.3f} s')
        console.send('root\r')
        answer = console.expect(AFTER_LOGIN, 'a shell prompt after login')
        if answer.group(0) == 'Password:':
            raise SessionError('the console asked for a password; none is sent')
        for index in range(len(commands)):
            command = commands[index]
            console.send(command + '; echo "@@DONE $?@@"\r')
            done = console.expect(DONE_MARKER, f'command {index + 1} to finish')
            status = int(done.group(1))
            path = os.path.join(session_dir, f'command{index + 1:02d}.txt')
            with open(path, 'w', encoding='utf-8') as stream:
                stream.write(f'$ {command}\n{console.consumed}\nexit {status}\n')
            summary.append(f'command {index + 1}: exit {status}: {command}')
            if status != 0:
                raise SessionError(f'command {index + 1} exited {status}; stopping before the rest')
        console.send('shutdown -p now\r')
        power_off_deadline = time.monotonic() + POWER_OFF_SECONDS
        while process.poll() is None and time.monotonic() < power_off_deadline:
            try:
                console.read_once()
            except SessionError:
                break
        if process.poll() is None:
            raise SessionError('the VM did not power off in time')
        summary.append(f'powered off at {time.monotonic() - started:.3f} s')
    finally:
        console.close()
        stop_process(process)
        raw_log.close()
        stderr_log.close()


def parse_arguments(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Run shell commands as root inside a C1 VM.')
    parser.add_argument('--commands', required=True)
    parser.add_argument('--serve-dir', default=None)
    parser.add_argument('--define', action='append', default=None)
    parser.add_argument('--timeout', type=float, default=240.0, help='seconds for the whole session')
    add_vm_arguments(parser)
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    arguments = parse_arguments(argv)
    server: Optional[http.server.ThreadingHTTPServer] = None
    summary: List[str] = []
    session_dir = ''
    outcome = 'failed'
    try:
        check_vm_name(arguments.name)
        layout = ApplianceLayout(arguments.storage_root)
        for required_path in (arguments.qemu, layout.vm_disk(arguments.name), layout.vm_vars(arguments.name)):
            if not os.path.isfile(required_path):
                raise SessionError(f'not found: {required_path}')
        replacements = parse_defines(arguments.define)
        if arguments.serve_dir is not None:
            if not os.path.isdir(arguments.serve_dir):
                raise SessionError(f'not a directory: {arguments.serve_dir}')
            server = start_server(arguments.serve_dir)
            replacements['http'] = f'http://{GUEST_HOST_ADDRESS}:{server.server_address[1]}'
        commands = read_commands(arguments.commands, replacements)
        session_id = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        label = os.path.splitext(os.path.basename(arguments.commands))[0]
        session_dir = os.path.join(layout.appliance_root(), 'logs', 'sessions', arguments.name,
                                   f'{session_id}-{label}')
        os.makedirs(session_dir, exist_ok=False)
        run_session(arguments, commands, session_dir, summary)
        outcome = 'ok'
    except (SessionError, ValueError, OSError) as failure:
        summary.append(f'guest_session: FAIL: {failure}')
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
    for line in summary:
        print(line)
    if len(session_dir) > 0:
        with open(os.path.join(session_dir, 'summary.txt'), 'w', encoding='utf-8') as stream:
            stream.write('\n'.join(summary) + f'\noutcome {outcome}\n')
        print(f'logs: {session_dir}')
    if outcome != 'ok':
        return 1
    print('guest_session: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
