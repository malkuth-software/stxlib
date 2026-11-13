"""Boot stages recognised on the serial console, and the tracker that times them.

Pure: no process, file or clock. The runner (boot_timing.py) feeds it bytes
with host times; tests feed it recorded text.

Each stage is recognised by text that FreeBSD (or the firmware) prints, cited
at freebsd-src fb9033d2876d (releng/15.2, BETA2), the pin recorded in notes
planning/stages/A0-freebsd-pin.md. A stage is recorded at the host time of
the chunk that completed its first match, and only once. Times are therefore
host-observed: when the bytes reached QEMU's serial pipe, not when the guest
produced them. In-guest times come from TSLOG once our kernel has it
(notes planning/stages/A0-boot-path.md section 1.7).
"""

import codecs
import re
from dataclasses import dataclass
from typing import Dict, List, Optional


@dataclass(frozen=True)
class Stage:
    name: str
    pattern: 're.Pattern'
    # Required stages must appear for a run to count as complete; optional
    # ones are recorded when seen (they depend on firmware build or menu path).
    required: bool
    # A prompt has no line end after it; such a stage is also matched against
    # the unfinished last line.
    matches_partial_line: bool
    source: str


STAGES: List[Stage] = [
    Stage('firmware_boot_option', re.compile(r'BdsDxe: (loading|starting) Boot'), False, False,
          'edk2 BdsDxe console message; present in QEMU\'s edk2 build is unconfirmed'),
    Stage('loader_banner', re.compile(r'FreeBSD/arm64 EFI loader'), True, False,
          'stand/common/newvers.sh:57 with stand/efi/loader/Makefile:89; printed by '
          'stand/efi/loader/main.c (bootprog_info)'),
    Stage('loader_countdown', re.compile(r'Autoboot in \d+ seconds|Hit \[Enter\] to boot immediately'),
          False, True, 'stand/lua/menu.lua:549-550; stand/common/boot.c:213-216'),
    Stage('loader_boot', re.compile(r'Booting \[[^\]]*\]\.\.\.'), False, True,
          'stand/common/boot.c:254 (autoboot path only)'),
    Stage('kernel_first', re.compile(r'---<<BOOT>>---|Copyright \(c\) 1992-\d+ The FreeBSD Project'),
          True, False, 'sys/kern/subr_prf.c:129; sys/kern/init_main.c:371 (announce)'),
    Stage('root_mount', re.compile(r'Trying to mount root from'), True, False,
          'sys/kern/vfs_mountroot.c:770'),
    Stage('userland_first', re.compile(r'Setting hostuuid'), True, False,
          'libexec/rc/rc.d/hostid:52 (first rc message of the stock image)'),
    Stage('login_prompt', re.compile(r'login: ?$'), True, True,
          'getty prompt on the serial console (stock image only)'),
]

# Escape sequences the loader menu and terminal code emit (CSI and the
# two-byte charset selections); removed before matching, kept in serial.raw.
ESCAPE_SEQUENCE = re.compile(r'\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][0-9A-Za-z]|\x1b[=>78]')

# The longest unfinished line kept for partial matching. The serial console
# is input: a guest that never ends a line must not grow this without bound.
PARTIAL_LINE_LIMIT_CHARACTERS: int = 4096

# The intervals reported, each from one stage to a later one.
INTERVALS: List[tuple] = [
    ('firmware', None, 'loader_banner'),
    ('loader', 'loader_banner', 'kernel_first'),
    ('kernel_to_root', 'kernel_first', 'root_mount'),
    ('root_to_userland', 'root_mount', 'userland_first'),
    ('userland_to_login', 'userland_first', 'login_prompt'),
    ('total', None, 'login_prompt'),
]


def strip_escapes(text: str) -> str:
    return ESCAPE_SEQUENCE.sub('', text)


class StageTracker:
    """Feeds serial text in arrival order and records the first time of each stage."""

    def __init__(self, stages: List[Stage]) -> None:
        self.stages: List[Stage] = stages
        self.seen_seconds: Dict[str, float] = {}
        self.unfinished_line: str = ''
        self.decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')

    def feed(self, data: bytes, seconds: float) -> List[str]:
        """Take a chunk received at `seconds`; return the stages it completed."""
        newly_seen: List[str] = []
        text = self.unfinished_line + self.decoder.decode(data)
        line_start = 0
        index = 0
        while index < len(text):
            character = text[index]
            if character == '\n' or character == '\r':
                self.match_line(text[line_start:index], seconds, False, newly_seen)
                line_start = index + 1
            index += 1
        self.unfinished_line = text[line_start:]
        if len(self.unfinished_line) > PARTIAL_LINE_LIMIT_CHARACTERS:
            self.unfinished_line = self.unfinished_line[-PARTIAL_LINE_LIMIT_CHARACTERS:]
        self.match_line(self.unfinished_line, seconds, True, newly_seen)
        return newly_seen

    def match_line(self, raw_line: str, seconds: float, partial: bool, newly_seen: List[str]) -> None:
        line = strip_escapes(raw_line)
        for stage in self.stages:
            if stage.name in self.seen_seconds:
                continue
            if partial and not stage.matches_partial_line:
                continue
            if stage.pattern.search(line) is not None:
                self.seen_seconds[stage.name] = seconds
                newly_seen.append(stage.name)

    def missing_required(self) -> List[str]:
        missing: List[str] = []
        for stage in self.stages:
            if stage.required and stage.name not in self.seen_seconds:
                missing.append(stage.name)
        return missing

    def interval_seconds(self, start_stage: Optional[str], end_stage: str) -> Optional[float]:
        """Seconds from start_stage (None: QEMU start, time 0) to end_stage,
        or None when either stage was not seen."""
        if end_stage not in self.seen_seconds:
            return None
        start = 0.0
        if start_stage is not None:
            if start_stage not in self.seen_seconds:
                return None
            start = self.seen_seconds[start_stage]
        return self.seen_seconds[end_stage] - start


# The appliance's boot ends at our PID 1's marker, not at a login prompt
# (owner, 2026-11-13: no login). The marker is one line written by
# runtime/init/stxinit.cpp as soon as it starts.
PID1_MARKER = re.compile(r'STXINIT uptime_ns=(\d+) cntvct=(\d+) cntfrq=(\d+)')

# FreeBSD's uptime starts at 1 s, not 0 (sys/kern/kern_tc.c:91, th_offset).
UPTIME_ORIGIN_SECONDS: float = 1.0

APPLIANCE_STAGE_NAMES: List[str] = ['firmware_boot_option', 'loader_banner', 'loader_countdown',
                                    'loader_boot', 'kernel_first', 'root_mount']

APPLIANCE_INTERVALS: List[tuple] = [
    ('firmware', None, 'loader_banner'),
    ('loader', 'loader_banner', 'kernel_first'),
    ('kernel_to_root', 'kernel_first', 'root_mount'),
    ('root_to_pid1', 'root_mount', 'pid1_marker'),
    ('total', None, 'pid1_marker'),
]


def appliance_stage_list() -> List[Stage]:
    """The stock stages up to the root mount, then our PID 1's marker."""
    copied: List[Stage] = []
    for stage in STAGES:
        if stage.name in APPLIANCE_STAGE_NAMES:
            copied.append(stage)
    copied.append(Stage('pid1_marker', PID1_MARKER, True, False,
                        'runtime/init/stxinit.cpp boot marker (our PID 1)'))
    return copied


def pid1_marker_values(text: str) -> Optional[Dict[str, float]]:
    """The in-guest times in the first PID 1 marker of a serial log, in
    seconds: pid1_uptime, the kernel clock's run time when PID 1 began (the
    marker's CLOCK_UPTIME less the 1 s FreeBSD's time keeping starts from,
    sys/kern/kern_tc.c:91), and pid1_counter, the generic timer's virtual
    count over its frequency (QEMU starts it at zero with the VM: assumed,
    not yet checked). None when there is no complete marker or the
    frequency is 0."""
    match = PID1_MARKER.search(text)
    if match is None:
        return None
    uptime_ns = int(match.group(1))
    count = int(match.group(2))
    frequency = int(match.group(3))
    if frequency == 0:
        return None
    return {'pid1_uptime': uptime_ns / 1e9 - UPTIME_ORIGIN_SECONDS, 'pid1_counter': count / frequency}


def default_stage_list() -> List[Stage]:
    """A fresh list of the stages above, for one tracker."""
    copied: List[Stage] = []
    for stage in STAGES:
        copied.append(stage)
    return copied
