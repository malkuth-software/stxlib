#!/usr/bin/env python3
"""Unit tests of the generator's rules on small synthetic metadata.

Usage:
  python3 -I tools/windows_api_headers/test_tools.py

Needs no metadata checkout and no repository: each test writes its own RDL
file. The whole generator is checked by `generate.py --check` against the
pinned metadata; these tests pin the rules that check relies on.
"""

import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
# Run with -I (isolated): the modules under test are imported from this directory explicitly.
sys.path.insert(0, str(HERE))
import generate  # noqa: E402
import rdl  # noqa: E402
import scan  # noqa: E402

# One file per stem; `extern fn` without #[library] is a callback.
STEMS = {
    'winuser': '''
        struct WNDCLASSA { style: u32, }
        struct WNDCLASSW { style: u32, }
        type WNDCLASS = WNDCLASSA;
        type LPWNDCLASSA = *mut WNDCLASSA;
        type LPWNDCLASSW = *mut WNDCLASSW;
        type LPWNDCLASS = *mut WNDCLASSA;
        extern fn ENUMPROCA(value: u32) -> i32;
        extern fn ENUMPROCW(value: u32) -> i32;
        extern fn ENUMPROC(value: u32) -> i32;
        #[library("USER32.dll")]
        extern fn RegisterClassW(lpWndClass: *const WNDCLASSW) -> u16;
        #[library("USER32.dll")]
        extern fn RegisterClassA(lpWndClass: *const WNDCLASSA) -> u16;
    ''',
    'psapi': '''
        const PSAPI_VERSION: i32 = 2;
        #[library("KERNEL32.dll", import = "K32EnumProcesses")]
        extern fn EnumProcesses(lpidProcess: *mut u32, cb: u32, lpcbNeeded: *mut u32) -> i32;
        #[library("KERNEL32.dll")]
        extern fn K32EnumProcesses(lpidProcess: *mut u32, cb: u32, lpcbNeeded: *mut u32) -> i32;
        #[library("KERNEL32.dll", import = "K32GetModuleBaseNameW")]
        extern fn GetModuleBaseNameW(cb: u32) -> u32;
        #[library("KERNEL32.dll", import = "K32GetModuleBaseNameA")]
        extern fn GetModuleBaseNameA(cb: u32) -> u32;
        #[library("KERNEL32.dll")]
        extern fn K32GetModuleBaseNameW(cb: u32) -> u32;
        #[library("KERNEL32.dll")]
        extern fn K32GetModuleBaseNameA(cb: u32) -> u32;
    ''',
    'winnetwk': '''
        const WN_SUCCESS: u32 = 0;
        #[library("MPR.dll")]
        extern fn WNetCancelConnection(lpName: *const u8, fForce: i32) -> u32;
    ''',
    'verrsrc': '''
        const VS_FFI_FILEFLAGSMASK: i32 = 63;
        const VFT_APP: i32 = 1;
    ''',
}


def load_model(directory: pathlib.Path) -> generate.Model:
    for stem, body in STEMS.items():
        (directory / f'{stem}.rdl').write_text(f'#[win32]\nmod Windows {{\n    mod Win32 {{\n{body}\n    }}\n}}\n',
                                               encoding='utf-8')
    return generate.Model(rdl.load_directory(directory), 'X64')


class ScanTests(unittest.TestCase):
    def test_resource_scripts_are_windows_facing(self) -> None:
        for path in ('packaging/windows.rc', 'packaging/windows_version.rc.in', 'res/app.rc2'):
            self.assertTrue(path.endswith(scan.EXTENSIONS), path)
            self.assertTrue(scan.windows_facing_path(path), path)
        self.assertFalse(scan.windows_facing_path('src/document.cpp'))

    def test_decode_source_reads_utf16_resource_scripts(self) -> None:
        text = '#include <winver.h>\nFILEOS VOS_NT_WINDOWS32\n'
        self.assertEqual(scan.decode_source(b'\xff\xfe' + text.encode('utf-16-le')), text)
        self.assertEqual(scan.decode_source(b'\xef\xbb\xbf' + text.encode('utf-8')), text)
        self.assertEqual(scan.decode_source(text.encode('utf-8')), text)


class GeneratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.model = load_model(pathlib.Path(self.directory.name))
        self.saved = (generate.COVERED_LIBRARIES, generate.WHOLE_STEMS, generate.SDK_WINDOWS_H,
                      generate.WINDOWS_H_UNCOVERED)
        generate.COVERED_LIBRARIES = ['kernel32.dll', 'user32.dll']
        generate.WHOLE_STEMS = ['verrsrc']
        generate.SDK_WINDOWS_H = ['winuser', 'verrsrc', 'winnetwk']
        generate.WINDOWS_H_UNCOVERED = {}

    def tearDown(self) -> None:
        (generate.COVERED_LIBRARIES, generate.WHOLE_STEMS, generate.SDK_WINDOWS_H,
         generate.WINDOWS_H_UNCOVERED) = self.saved
        self.directory.cleanup()

    def complete(self) -> generate.Emitter:
        generate.add_complete_libraries(self.model)
        generate.add_import_aliases(self.model)
        return generate.Emitter(self.model, [], [])

    def test_neutral_typedefs_and_callbacks_choose_by_unicode(self) -> None:
        emitter = self.complete()
        for name in ('WNDCLASS', 'LPWNDCLASS', 'ENUMPROC'):
            self.assertIn(name, self.model.used, name)
            lines = emitter.typedef_line(self.model.declarations[name])
            self.assertEqual(lines, ['#ifdef UNICODE', f'typedef {name}W {name};', '#else',
                                     f'typedef {name}A {name};', '#endif'])

    def test_whole_stems_are_emitted_without_functions(self) -> None:
        self.complete()
        self.assertIn('VS_FFI_FILEFLAGSMASK', self.model.used)
        self.assertIn('VFT_APP', self.model.used)

    def test_import_aliases_follow_psapi_version(self) -> None:
        emitter = self.complete()
        self.assertEqual(generate.import_target(self.model.declarations['EnumProcesses']), 'K32EnumProcesses')
        self.assertEqual(self.model.import_aliases['EnumProcesses'], 'K32EnumProcesses')
        self.assertIn('GetModuleBaseName', self.model.neutral)
        lines = emitter.import_alias_lines('psapi')
        self.assertEqual(lines[0], '#if PSAPI_VERSION > 1')
        self.assertIn('#define EnumProcesses K32EnumProcesses', lines)
        self.assertIn('#define GetModuleBaseNameW K32GetModuleBaseNameW', lines)
        self.assertEqual(lines[-1], '#endif')
        header = emitter.stem_header('psapi', emitter.stems()['psapi'], [])
        self.assertIn('#ifndef PSAPI_VERSION\n#define PSAPI_VERSION 2\n#endif', header)

    def test_coverage_fails_on_an_uncovered_dll_without_a_reason(self) -> None:
        self.complete()
        report, failures = generate.windows_h_coverage(self.model)
        self.assertIn('winuser.h: complete', report)
        self.assertIn('verrsrc.h: complete', report)
        self.assertEqual(failures, ['winnetwk.h: 1 function from mpr.dll missing',
                                    'winnetwk.h: 1 declaration missing: WN_SUCCESS'])

    def test_coverage_accepts_a_named_dll_and_rejects_a_stale_entry(self) -> None:
        self.complete()
        generate.WINDOWS_H_UNCOVERED = {'winnetwk': {'mpr.dll': 'not linked'}}
        _, failures = generate.windows_h_coverage(self.model)
        self.assertEqual(failures, [])
        generate.WINDOWS_H_UNCOVERED = {'winnetwk': {'mpr.dll': 'not linked'}, 'winuser': {'user32.dll': 'stale'}}
        _, failures = generate.windows_h_coverage(self.model)
        self.assertEqual(failures, ['winuser.h: complete, but WINDOWS_H_UNCOVERED still names user32.dll'])


if __name__ == '__main__':
    unittest.main()
