#!/usr/bin/env python3
"""run_b1.py: run the B1 benchmark (B1.md sections 3 and 4) and record every
run as one JSON line in OUT/runs.jsonl, in the entry schema of
proofs/report.json (B1.md section 6) plus the fields row, harness, config,
variant and expected.

For each row (rows/NN-name/row.json), harness, configuration, tool and tool
configuration (solver or memory model):
  - the original harness at every size of the row's sweep (bounded);
  - each mutant and the vacuity probe at the row's headline size;
  - the original once more without a length bound (STX_UNBOUNDED) for the
    tools the row lists under "unbounded" and whose template has an
    unbounded form (k-induction, cpp-verify's unbounded mode).

Expected outcomes: the original proves; a mutant and the probe are refuted.
A row counts as proved only when all three hold (B1.md section 3.3); the
analysis, not this script, decides that.

Usage:
  run_b1.py --sources sources.json --out OUT [--rows 03,07] [--tools esbmc]
            [--variants original,mutant,probe,unbounded] [--timeout 900]
            [--smoke] [--dry-run]
  --smoke   one bounded original run per (row, harness, tool) at the smallest
            size, first configuration only: the phase A check that commands
            and flags work before the measured runs.
  --dry-run print the commands and write the lifted translation units only.
  --rows-directory D
            the rows to run (default: rows/ beside this script's parent);
            phase D's targeted harnesses are proofs/d/rows.
  --lean    the targeted configuration of phase D (coordinator's amendment
            of 2026-11-13, "stop the permutation grid"): a 120 s limit
            unless --timeout is given, the variants original, mutant and
            probe (no unbounded runs unless asked), and a tool's second
            solver configuration only where the first gave no verdict
            (timed out, unknown, out of memory, tool error): a run decided
            as proved, refuted or bound too small by the first is not
            repeated. Skipped configurations are recorded as such.

Every run's full output is kept in OUT/logs/; lifted units in OUT/units/.
The runner never deletes anything in OUT.
"""

import argparse
import datetime
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
B1 = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lift  # noqa: E402  (the lifter beside this script)


def load_json(path):
    with open(path, 'r', encoding='utf-8') as handle:
        return json.load(handle)


def evaluate_unwind(expression, size):
    allowed = {'N': size, 'max': max, 'min': min}
    value = eval(expression, {'__builtins__': {}}, allowed)  # row files are reviewed source
    return int(value)


def expand(template, tokens):
    command = []
    for part in template:
        match = re.fullmatch(r'\{(\w+)\}', part)
        if match and isinstance(tokens.get(match.group(1)), list):
            command.extend(tokens[match.group(1)])
            continue
        text = part
        for key, value in tokens.items():
            if not isinstance(value, list):
                text = text.replace('{' + key + '}', str(value))
        command.append(text)
    return command


def resolve_include(entry, sources, row_directory, out):
    kind, relative = entry.split(':', 1)
    if kind == 'harness':
        if relative == 'common':
            return os.path.join(B1, 'common')
        if relative == 'row':
            return row_directory
        if relative == 'd-common':
            return os.path.join(os.path.dirname(B1), 'd', 'common')
        return os.path.join(B1, relative)
    return sources.file(kind, relative)


def classify(text, exit_code, timed_out, tool):
    if timed_out:
        return 'timed-out'
    verdict = None
    for marker, meaning in tool['verdicts']:
        if marker in text:
            verdict = meaning
            break
    if verdict == 'failure':
        for marker in tool.get('bound_markers', []):
            if marker in text:
                return 'bound-too-small'
        return 'refuted'
    if verdict == 'success':
        return 'proved'
    if verdict == 'unknown':
        return 'unknown'
    lowered = text.lower()
    # Only the tool's own output is classified (the /usr/bin/time block is
    # cut off by run_command). 'memory limit' is not a marker: ESBMC prints
    # "Couldn't set memory limit" when setrlimit fails, which is not an
    # exhaustion.
    if 'out of memory' in lowered or 'std::bad_alloc' in lowered or 'cannot allocate memory' in lowered:
        return 'out-of-memory'
    if 'unsupported' in lowered or 'not supported' in lowered or 'not yet supported' in lowered:
        return 'unsupported'
    for pattern in tool.get('unsupported_patterns', []):
        if re.search(pattern, text):
            return 'unsupported'
    return 'tool-error'


def peak_memory_mib(stderr_text):
    match = re.search(r'(\d+)\s+maximum resident set size', stderr_text)
    if match:
        return round(int(match.group(1)) / (1024 * 1024), 1)
    return None


TIME_BLOCK = re.compile(r'^\s+[\d.]+ real\s+[\d.]+ user\s+[\d.]+ sys\s*$', re.MULTILINE)


def split_time_block(stderr_text):
    """stderr without the block /usr/bin/time -l appends, and the block."""
    match = None
    for match in TIME_BLOCK.finditer(stderr_text):
        pass
    if match is None:
        return stderr_text, ''
    return stderr_text[:match.start()], stderr_text[match.start():]


MEMORY_BUDGET_MIB = 16 * 1024   # per run, B1.md section 4.2
WATCH_INTERVAL_S = 0.25


def group_rss_mib(group):
    """Resident set size of every process in the process group, in MiB,
    from ps (macOS does not enforce RLIMIT_AS, and RLIMIT_DATA does not
    cover mmap, so the budget is a watchdog rather than a kernel limit)."""
    try:
        listing = subprocess.run(['ps', '-A', '-o', 'pgid=,rss='], capture_output=True, text=True,
                                 timeout=10, check=False).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    total_kib = 0
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == str(group) and fields[1].isdigit():
            total_kib += int(fields[1])
    return total_kib / 1024


def run_command(command, timeout, log_path, budget_mib=MEMORY_BUDGET_MIB):
    """Run one tool under the time limit and the memory budget. The budget
    is enforced by sampling the process group's RSS every WATCH_INTERVAL_S
    and killing the group when the sum exceeds it: a sampled watchdog, so a
    run can overshoot by what it allocates within one interval."""
    wrapped = command
    if sys.platform == 'darwin' and os.path.exists('/usr/bin/time'):
        wrapped = ['/usr/bin/time', '-l'] + command
    start = time.monotonic()
    process = subprocess.Popen(wrapped, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               errors='replace', start_new_session=True)
    timed_out = False
    over_budget = False
    sampled_peak = 0.0
    while True:
        try:
            stdout, stderr = process.communicate(timeout=WATCH_INTERVAL_S)
            break
        except subprocess.TimeoutExpired:
            pass
        rss = group_rss_mib(process.pid)
        if rss is not None:
            sampled_peak = max(sampled_peak, rss)
        if rss is not None and rss > budget_mib:
            over_budget = True
        elif time.monotonic() - start > timeout:
            timed_out = True
        if over_budget or timed_out:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
            break
    elapsed = time.monotonic() - start
    with open(log_path, 'w', encoding='utf-8') as handle:
        handle.write('$ ' + shlex.join(command) + '\n')
        handle.write(stdout)
        handle.write('\n--- stderr ---\n')
        handle.write(stderr)
        if over_budget:
            handle.write(f'\n--- runner: killed, process group RSS above the budget of {budget_mib} MiB '
                         f'(sampled peak {sampled_peak:.0f} MiB) ---\n')
    tool_stderr, time_block = split_time_block(stderr)
    watch = {'over_budget': over_budget, 'sampled_peak_mib': round(sampled_peak, 1), 'budget_mib': budget_mib}
    return (stdout + '\n' + tool_stderr, process.returncode, timed_out, elapsed, peak_memory_mib(time_block),
            watch)


_SDK_PATH = []


def sdk_path():
    """The macOS SDK the host's clang would use, once per run; '' elsewhere."""
    if not _SDK_PATH:
        value = ''
        if sys.platform == 'darwin':
            try:
                value = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True,
                                       timeout=60, check=False).stdout.strip()
            except (OSError, subprocess.TimeoutExpired):
                value = ''
        _SDK_PATH.append(value)
    return _SDK_PATH[0]


_RESOURCE_INCLUDE = {}


def resource_include(clang):
    """The builtin-header directory (stdarg.h, stddef.h, ...) of a clang
    installation: `<clang> -print-resource-dir`/include; '' if absent."""
    if clang not in _RESOURCE_INCLUDE:
        value = ''
        try:
            directory = subprocess.run([clang, '-print-resource-dir'], capture_output=True, text=True,
                                       timeout=60, check=False).stdout.strip()
            if directory and os.path.isdir(os.path.join(directory, 'include')):
                value = os.path.join(directory, 'include')
        except (OSError, subprocess.TimeoutExpired):
            value = ''
        _RESOURCE_INCLUDE[clang] = value
    return _RESOURCE_INCLUDE[clang]


def environment_values(tool):
    values = {'sdk_path': sdk_path()}
    if tool.get('resource_clang'):
        values['resource_include'] = resource_include(tool['resource_clang'])
    return values


def environment_flags(tool):
    """The tool's environment flags with {sdk_path} and {resource_include}
    filled; a flag pair whose value would be empty is dropped (no SDK off
    macOS, no such clang)."""
    flags = list(tool.get('environment', []))
    values = environment_values(tool)
    result = []
    index = 0
    while index < len(flags):
        following = flags[index + 1] if index + 1 < len(flags) else None
        match = re.fullmatch(r'\{(\w+)\}', following or '')
        if match and match.group(1) in values:
            if values[match.group(1)]:
                result.extend([flags[index], values[match.group(1)]])
            index += 2
            continue
        result.append(flags[index])
        index += 1
    return result


SHELL_SAFE = re.compile(r'[A-Za-z0-9_./=+:,@%^-]+')


class Remote:
    """A tool that runs in a guest reached by ssh (tools.json `remote`):
    the lifting stays on this host (the pin is checked here), the harness
    tree and the sources' working trees are copied into the guest once per
    invocation, each unit is copied before its run, and every local path
    in the command is mapped to its guest copy. The guest's own memory
    bounds the run; the time limit is enforced in the guest with timeout."""

    def __init__(self, spec, sources, out):
        self.spec = spec
        self.root = spec['root']
        self.ssh = list(spec['ssh'])
        self.mapping = [(B1, self.root + '/b1'), (out, self.root + '/out')]
        for name, entry in sources.table.items():
            self.mapping.append((entry['root'], f'{self.root}/src/{name}'))
        self.mapping.sort(key=lambda pair: -len(pair[0]))
        self.synced = False

    def map(self, token):
        text = str(token)
        for local, guest in self.mapping:
            if text == local or text.startswith(local + os.sep):
                return guest + text[len(local):]
        return text

    def shell(self, command):
        return subprocess.run(self.ssh + [command], capture_output=True, text=True, timeout=600, check=False)

    def sync(self):
        if self.synced:
            return
        self.shell(f'rm -rf {shlex.quote(self.root)}/b1 {shlex.quote(self.root)}/src && mkdir -p '
                   f'{shlex.quote(self.root)}/out/units {shlex.quote(self.root)}/src')
        for local, guest in self.mapping:
            if guest.endswith('/out'):
                continue
            packer = subprocess.Popen(['tar', '--exclude', '.git', '-C', local, '-cf', '-', '.'],
                                      stdout=subprocess.PIPE, env=dict(os.environ, COPYFILE_DISABLE='1'))
            subprocess.run(self.ssh + [f'mkdir -p {shlex.quote(guest)} && tar --warning=no-unknown-keyword '
                                       f'-C {shlex.quote(guest)} -xf -'],
                           stdin=packer.stdout, check=False, timeout=600)
            packer.wait()
        self.synced = True

    def push(self, path):
        with open(path, 'rb') as handle:
            subprocess.run(self.ssh + [f'cat > {shlex.quote(self.map(path))}'], stdin=handle, check=False,
                           timeout=120)

    def wrap(self, command, timeout):
        mapped = [self.map(token) for token in command]
        line = (f'cd {shlex.quote(self.root)} && PATH={shlex.quote(self.spec["path"])}:$PATH '
                f'timeout -s KILL {int(timeout)} {shlex.join(mapped)}')
        return self.ssh + [line], mapped


def tool_version(tool, cache, remote=None):
    name = tool['binary']
    if name not in cache:
        try:
            command = expand(tool['version'], {'binary': name})
            if remote is not None:
                command = remote.wrap(command, 60)[0]
            result = subprocess.run(command, capture_output=True,
                                    text=True, timeout=60, check=False)
            cache[name] = (result.stdout + result.stderr).strip().splitlines()[0:2]
        except (OSError, subprocess.TimeoutExpired) as error:
            cache[name] = [f'unavailable: {error}']
    return cache[name]


def plan_runs(row, harness, tools, arguments):
    sizes = harness.get('sizes')
    headline = harness.get('headline')
    if sizes and headline is None:
        headline = max(size for size in sizes if size <= 16) if min(sizes) <= 16 else min(sizes)
    variants = set(arguments.variants.split(','))
    runs = []
    for config in harness.get('configs', [{'name': 'default', 'defines': {}}]):
        for tool_name in harness['tools']:
            if arguments.tools and tool_name not in arguments.tools.split(','):
                continue
            if tool_name not in tools:
                # A harness run by its own script (phase D's enumerations,
                # row.json "runner"), not by this runner.
                continue
            tool = tools[tool_name]
            if bool(tool.get('remote')) != bool(getattr(arguments, 'remote', False)):
                continue
            for mode in ('bounded', 'unbounded'):
                template = tool.get(mode)
                if template is None:
                    continue
                if mode == 'unbounded':
                    listed = (harness.get('unbounded') or {}).get('tools', [])
                    if tool_name not in listed or 'unbounded' not in variants:
                        continue
                configurations = tool['configurations'] if '{configuration}' in template else {'default': []}
                for configuration_name, configuration_flags in configurations.items():
                    if mode == 'bounded':
                        run_sizes = sizes if sizes else [None]
                        if arguments.smoke:
                            run_sizes = [min(sizes)] if sizes else [None]
                        for size in run_sizes:
                            if 'original' in variants:
                                runs.append((config, tool_name, mode, configuration_name, configuration_flags, size, 'original', None))
                        if not arguments.smoke:
                            size = headline if sizes else None
                            if 'mutant' in variants:
                                for mutant in harness.get('mutants', []):
                                    runs.append((config, tool_name, mode, configuration_name, configuration_flags, size, 'mutant', mutant))
                            if 'probe' in variants:
                                runs.append((config, tool_name, mode, configuration_name, configuration_flags, size, 'probe', None))
                    elif not arguments.smoke:
                        size = (harness.get('unbounded') or {}).get('size')
                        runs.append((config, tool_name, mode, configuration_name, configuration_flags, size, 'original', None))
                    if arguments.smoke:
                        break
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--sources', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--rows')
    parser.add_argument('--tools')
    parser.add_argument('--variants')
    parser.add_argument('--timeout', type=int)
    parser.add_argument('--rows-directory')
    parser.add_argument('--lean', action='store_true')
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--remote', action='store_true',
                        help='run only the tools that run in a guest (tools.json remote); without it they are '
                             'skipped. Use inside vm_genmc.sh, which boots the guest.')
    arguments = parser.parse_args()
    if arguments.timeout is None:
        arguments.timeout = 120 if arguments.lean else 900
    if arguments.variants is None:
        arguments.variants = 'original,mutant,probe' if arguments.lean else 'original,mutant,probe,unbounded'
    arguments.out = os.path.abspath(arguments.out)
    repository = os.path.realpath(os.path.dirname(os.path.dirname(B1)))
    if os.path.commonpath([os.path.realpath(arguments.out), repository]) == repository:
        # The M4 copy of the repository is synced with rsync --delete, so
        # anything written inside it is lost at the next sync.
        sys.exit(f'--out must be outside the repository ({repository})')

    sources = lift.Sources(arguments.sources)
    tools = {name: value for name, value in load_json(os.path.join(HERE, 'tools.json')).items()
             if not name.startswith('_')}
    for directory in ('units', 'logs'):
        os.makedirs(os.path.join(arguments.out, directory), exist_ok=True)
    runs_path = os.path.join(arguments.out, 'smoke.jsonl' if arguments.smoke else 'runs.jsonl')
    versions = {}
    remotes = {}
    selected = set(arguments.rows.split(',')) if arguments.rows else None
    rows_directory = os.path.abspath(arguments.rows_directory) if arguments.rows_directory else os.path.join(B1, 'rows')
    decided = set()   # --lean: runs a first solver configuration decided
    for row_name in sorted(os.listdir(rows_directory)):
        row_directory = os.path.join(rows_directory, row_name)
        row_file = os.path.join(row_directory, 'row.json')
        if not os.path.isfile(row_file):
            continue
        row = load_json(row_file)
        if selected and row['row'] not in selected:
            continue
        for harness in row['harnesses']:
            harness_path = os.path.join(row_directory, harness['file'])
            for run in plan_runs(row, harness, tools, arguments):
                config, tool_name, mode, configuration_name, configuration_flags, size, variant, mutant = run
                tool = tools[tool_name]
                identifier = '-'.join(str(part) for part in (
                    row['row'], os.path.splitext(harness['file'])[0], config['name'], tool_name, mode,
                    configuration_name, 'N' + str(size) if size is not None else 'N-', variant,
                    mutant or '')).rstrip('-')
                unit_path = os.path.join(arguments.out, 'units', identifier + os.path.splitext(harness['file'])[1])
                decision_key = (row['row'], harness['file'], config['name'], tool_name, mode, size, variant, mutant)
                if arguments.lean and decision_key in decided:
                    print(f'{identifier}: skipped (lean: decided by an earlier configuration)', flush=True)
                    continue
                record = {
                    'row': row['row'], 'title': row['title'], 'harness': os.path.relpath(harness_path, B1),
                    'config': config['name'], 'tool': tool_name, 'mode': mode,
                    'configuration': configuration_name, 'size': size, 'variant': variant,
                    'mutant': mutant,
                    'expected': 'proved' if variant == 'original' else 'refuted',
                    'started': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
                }
                try:
                    text, report = lift.lift(sources, harness_path, mutant)
                except lift.LiftError as error:
                    record.update({'result': 'lift-error', 'detail': str(error)})
                    with open(runs_path, 'a', encoding='utf-8') as handle:
                        handle.write(json.dumps(record) + '\n')
                    print(f'{identifier}: lift-error: {error}', file=sys.stderr)
                    continue
                with open(unit_path, 'w', encoding='utf-8', newline='\n') as handle:
                    handle.write(text)
                defines = dict(harness.get('defines', {}))
                defines.update(config.get('defines', {}))
                defines[tool['define']] = '1'
                if size is not None:
                    defines['STX_N'] = str(size)
                if variant == 'probe':
                    defines['STX_VACUITY'] = '1'
                if mode == 'unbounded':
                    defines['STX_UNBOUNDED'] = '1'
                define_tokens = []
                for key, value in defines.items():
                    define_tokens.extend(tool['define_flag'] + [f'{key}={value}'])
                include_tokens = []
                for entry in harness.get('includes', []) + ['harness:common', 'harness:row']:
                    include_tokens.extend(tool['include_flag'] + [resolve_include(entry, sources, row_directory, arguments.out)])
                unwind = 1
                if mode == 'bounded' and harness.get('unwind'):
                    unwind = evaluate_unwind(harness['unwind'], size)
                language = list(tool['language'].get(harness['language'], []))
                language.extend(harness.get('flags', {}).get(tool_name, []))
                command = expand(tool[mode], {
                    'binary': tool['binary'], 'file': unit_path,
                    'language': language,
                    'includes': include_tokens, 'defines': define_tokens,
                    'unwind': unwind, 'configuration': configuration_flags,
                    'timeout_ms': arguments.timeout * 1000,
                    'environment': environment_flags(tool),
                })
                record.update({'unwind': unwind if mode == 'bounded' else None, 'command': shlex.join(command),
                               'lift': report})
                if arguments.dry_run:
                    print(shlex.join(command))
                    continue
                remote = None
                if tool.get('remote'):
                    if tool_name not in remotes:
                        remotes[tool_name] = Remote(tool['remote'], sources, arguments.out)
                    remote = remotes[tool_name]
                    remote.sync()
                record['tool_version'] = tool_version(tool, versions, remote)
                if '{environment}' in tool[mode]:
                    record['environment'] = environment_values(tool)
                if tool.get('joins_through_shell'):
                    # GenMC passes its clang arguments to sh -c unquoted:
                    # a token with shell syntax would be re-parsed.
                    unsafe = [token for token in command if not SHELL_SAFE.fullmatch(str(token))]
                    if unsafe:
                        record.update({'result': 'harness-error',
                                       'detail': f'tokens unsafe for {tool_name}\'s sh -c: {unsafe}'})
                        with open(runs_path, 'a', encoding='utf-8') as handle:
                            handle.write(json.dumps(record) + '\n')
                        print(f'{identifier}: harness-error: unsafe tokens {unsafe}', flush=True)
                        continue
                log_path = os.path.join(arguments.out, 'logs', identifier + '.txt')
                executed = command
                if remote is not None:
                    remote.push(unit_path)
                    executed, mapped = remote.wrap(command, arguments.timeout)
                    record['guest_command'] = shlex.join(mapped)
                    record['guest'] = tool['remote'].get('description', '')
                output, exit_code, timed_out, elapsed, peak, watch = run_command(
                    executed, arguments.timeout + (30 if remote is not None else 0), log_path)
                guest_killed = remote is not None and exit_code == 137
                if guest_killed and elapsed >= arguments.timeout - 1:
                    timed_out = True  # killed by timeout -s KILL in the guest
                record.update({
                    'result': 'out-of-memory' if watch['over_budget'] or (guest_killed and not timed_out)
                              else classify(output, exit_code, timed_out, tool),
                    'memory_watch': watch,
                    'exit_code': exit_code, 'time_s': round(elapsed, 2), 'peak_memory_mib': peak,
                    'log': os.path.relpath(log_path, arguments.out),
                    'output_head': output.strip().splitlines()[:15],
                    'output_tail': output.strip().splitlines()[-25:],
                })
                if exit_code is not None and exit_code < 0 and record['result'] == 'tool-error':
                    record['signal'] = -exit_code
                record['lean'] = bool(arguments.lean)
                if record['result'] in ('proved', 'refuted', 'bound-too-small'):
                    decided.add(decision_key)
                with open(runs_path, 'a', encoding='utf-8') as handle:
                    handle.write(json.dumps(record) + '\n')
                print(f'{identifier}: {record["result"]} ({record["time_s"]} s)', flush=True)
                if arguments.smoke and record['result'] not in ('proved', 'refuted'):
                    # The M4's logs stay on the M4; the excerpt reaches the
                    # seat through m4sbrun's copy of this output.
                    for text in record['output_head'] + ['...'] + record['output_tail'][-10:]:
                        print('    | ' + text[:300], flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
