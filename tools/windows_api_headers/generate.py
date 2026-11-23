#!/usr/bin/env python3
"""Emits STXlib's own Windows API headers from windows-rs's metadata.

Usage:
  python3 -I tools/windows_api_headers/generate.py [--root <stxlib checkout>]

Inputs (all in this directory): `PIN.json` (the metadata pin and the local
checkout), `roots.txt` (scan.py's output: the header names our sources
include and the names they use) and `layout.py`'s rules. Outputs, under
`src/hosted/windows/api/`:

- `include/stxwin_types.h`: every used type, in dependency order (records
  forward-declared first, then typedefs, enums, records and callbacks, then
  COM interfaces as C++ structs with their vtables in metadata order);
- `include/<stem>.h`: per metadata file (one per SDK header), the used
  constants (as macros, like the SDK), functions and GUID declarations;
- `include/<name>.h` for the umbrella names our sources include (windows.h,
  uiautomation.h, ...), which include their member headers;
- `lib/<dll>.def`: one export list per system DLL, used functions only;
- `lib/uuid.c`: the GUIDs (IIDs, CLSIDs, other GUID constants) used;
- `check/layout_check.cpp`: static_asserts of sizeof, alignof and offsetof
  for every emitted record against `layout.py`'s computation from the
  metadata.

`stxwin_supplement.h` (macros and inline helpers that are not metadata) is
hand-written and not touched here. Output is deterministic: names sorted or
in dependency order with sorted tie-breaks, LF line ends, no timestamps.
Target: x86_64 msvc (`PIN.json` arch X64); the arch attributes are kept by
the reader, so an Arm64 run selects its own declarations.
"""

import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import layout  # noqa: E402
import rdl  # noqa: E402

PRIMITIVES = {
    'void': 'void', 'bool': 'bool', 'i8': 'char', 'u8': 'unsigned char', 'i16': 'short',
    'u16': 'unsigned short', 'i32': 'int', 'u32': 'unsigned int', 'i64': 'long long',
    'u64': 'unsigned long long', 'isize': 'long long', 'usize': 'unsigned long long',
    'f32': 'float', 'f64': 'double',
}

# The metadata flattens the SDK's base typedefs to fixed-width integers.
# These are their C definitions from Microsoft Learn, "Windows Data Types"
# (learn.microsoft.com/windows/win32/winprog/windows-data-types, read
# 2026-11-17), where the long/int and wchar_t/unsigned short distinction
# matters to C++ overloading and pointer conversion.
NATIVE_TYPEDEFS = {
    'CHAR': 'char', 'WCHAR': 'wchar_t', 'LONG': 'long', 'ULONG': 'unsigned long',
    'DWORD': 'unsigned long', 'BOOL': 'int', 'HRESULT': 'long', 'SCODE': 'long',
    'NTSTATUS': 'long', 'LCID': 'DWORD', 'LCTYPE': 'DWORD', 'LGRPID': 'DWORD',
    'COLORREF': 'DWORD', 'ACCESS_MASK': 'DWORD', 'OLECHAR': 'WCHAR',
    'PSTR': 'CHAR *', 'LPSTR': 'CHAR *', 'PCSTR': 'const CHAR *', 'LPCSTR': 'const CHAR *',
    'PWSTR': 'WCHAR *', 'LPWSTR': 'WCHAR *', 'PCWSTR': 'const WCHAR *', 'LPCWSTR': 'const WCHAR *',
    'LPOLESTR': 'OLECHAR *', 'LPCOLESTR': 'const OLECHAR *', 'BSTR': 'OLECHAR *',
    'PCHAR': 'CHAR *', 'PWCHAR': 'WCHAR *', 'PDWORD': 'DWORD *', 'LPDWORD': 'DWORD *',
    'PULONG': 'ULONG *', 'PLONG': 'LONG *', 'LPLONG': 'LONG *',
}
# Each override names the typedefs its definition uses, so they are emitted first.
NATIVE_TYPEDEF_DEPENDENCIES = {
    'LCID': ['DWORD'], 'LCTYPE': ['DWORD'], 'LGRPID': ['DWORD'], 'COLORREF': ['DWORD'],
    'ACCESS_MASK': ['DWORD'], 'OLECHAR': ['WCHAR'], 'PSTR': ['CHAR'], 'LPSTR': ['CHAR'],
    'PCSTR': ['CHAR'], 'LPCSTR': ['CHAR'], 'PWSTR': ['WCHAR'], 'LPWSTR': ['WCHAR'],
    'PCWSTR': ['WCHAR'], 'LPCWSTR': ['WCHAR'], 'LPOLESTR': ['OLECHAR'], 'LPCOLESTR': ['OLECHAR'],
    'BSTR': ['OLECHAR'], 'PCHAR': ['CHAR'], 'PWCHAR': ['WCHAR'], 'PDWORD': ['DWORD'],
    'LPDWORD': ['DWORD'], 'PULONG': ['ULONG'], 'PLONG': ['LONG'], 'LPLONG': ['LONG'],
}
# C library names the metadata also declares. The crt stream owns them: they
# are spelled by name, never defined here, and the types header includes the
# C header (clang's resource header or the crt's) that defines them.
CRT_OWNED = {
    'va_list': 'stdarg.h', 'size_t': 'stddef.h', 'ptrdiff_t': 'stddef.h', 'intptr_t': 'stdint.h',
    'uintptr_t': 'stdint.h', 'int8_t': 'stdint.h', 'int16_t': 'stdint.h', 'int32_t': 'stdint.h',
    'int64_t': 'stdint.h', 'uint8_t': 'stdint.h', 'uint16_t': 'stdint.h', 'uint32_t': 'stdint.h',
    'uint64_t': 'stdint.h', 'time_t': 'time.h', 'FILE': 'stdio.h', 'wint_t': 'wchar.h', 'errno_t': 'errno.h',
}
# Always emitted: the spelling rules below use them.
BASE_TYPES = ['WCHAR', 'CHAR', 'LPWSTR', 'LPSTR', 'GUID', 'IID', 'CLSID', 'LONG_PTR', 'HRESULT', 'ULONG',
              'IUnknown', 'PROPERTYKEY', 'WORD', 'BYTE', 'DWORD', 'LONG', 'DWORD_PTR', 'ULONG_PTR', 'COLORREF',
              'WPARAM', 'LPARAM', 'LRESULT', 'HANDLE']
# Hand-written headers a generated one includes after the types (stxwin_*.h).
HAND_TAILS = {'versionhelpers': 'stxwin_versionhelpers.h'}
HAND_WRITTEN = ['stxwin_supplement.h', 'stxwin_versionhelpers.h', 'initguid.h']

CXX_KEYWORDS = {
    'alignas', 'alignof', 'and', 'asm', 'auto', 'bool', 'break', 'case', 'catch', 'char', 'class', 'const',
    'constexpr', 'continue', 'default', 'delete', 'do', 'double', 'else', 'enum', 'explicit', 'export',
    'extern', 'false', 'float', 'for', 'friend', 'goto', 'if', 'inline', 'int', 'long', 'mutable',
    'namespace', 'new', 'noexcept', 'not', 'nullptr', 'operator', 'or', 'private', 'protected', 'public',
    'register', 'return', 'short', 'signed', 'sizeof', 'static', 'struct', 'switch', 'template', 'this',
    'throw', 'true', 'try', 'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using', 'virtual',
    'void', 'volatile', 'while', 'xor', 'char8_t', 'char16_t', 'char32_t', 'concept', 'requires',
    'interface',
}

# The metadata writes IDL `long` and `int` alike as i32. COM interfaces come
# from MIDL headers, whose IDL uses `long` (Learn's method syntax, e.g.
# IAccessible::get_accChildCount(long *pcountChildren)), except the UI
# Automation IDL, which uses `int` (e.g. ITextRangeProvider::Move(TextUnit
# unit, int count, int *pRetVal)). Method signatures matter to C++ overriding
# and pointer conversion, so i32 in an interface method is spelled `long`
# unless the interface comes from one of these files.
INT_IDL_STEMS = {'uiautomationcore', 'uiautomationclient', 'uiautomationcoreapi'}

# Umbrella headers: what an SDK-named header that has no metadata file of
# its own (or that pulls in others) includes. From Microsoft Learn's
# per-function "Header" lines, "X.h (include Y.h)", read 2026-11-17, and
# "Using the Windows Headers" for WIN32_LEAN_AND_MEAN.
WINDOWS_H_MEMBERS = [
    'sdkddkver', 'winapifamily', 'shared', 'minwindef', 'windef', 'winnt', 'winerror', 'minwinbase',
    'winbase', 'wingdi', 'winuser', 'winnls', 'winreg', 'wincon', 'consoleapi', 'consoleapi2',
    'consoleapi3', 'processthreadsapi', 'processenv', 'synchapi', 'fileapi', 'handleapi', 'memoryapi',
    'libloaderapi', 'errhandlingapi', 'sysinfoapi', 'heapapi', 'debugapi', 'profileapi', 'ioapiset',
    'jobapi', 'jobapi2', 'namedpipeapi', 'threadpoolapiset', 'threadpoollegacyapiset', 'timezoneapi',
    'stringapiset', 'datetimeapi', 'securitybaseapi', 'interlockedapi', 'realtimeapiset', 'fibersapi',
    'wow64apiset', 'utilapiset', 'winver', 'imm', 'winsvc', 'apiquery2', 'enclaveapi', 'namespaceapi',
    'systemtopologyapi', 'processtopologyapi', 'psapi', 'winioctl', 'excpt',
]
# Included by windows.h only without WIN32_LEAN_AND_MEAN.
WINDOWS_H_NOT_LEAN = ['shellapi', 'commdlg', 'ole2', 'objbase', 'combaseapi', 'oleauto', 'oaidl', 'objidl',
                      'objidlbase', 'unknwnbase', 'wtypes', 'wtypesbase', 'oleidl', 'propidlbase',
                      'wincrypt', 'mmsystem', 'mmsyscom', 'timeapi', 'rpc', 'rpcdce', 'dde', 'ddeml', 'dlgs',
                      'cderr', 'winspool', 'winscard', 'winefs', 'nb30', 'winperf', 'lzexpand']
# Record tags that differ from the metadata name: clang's __uuidof yields
# `const _GUID` (Learn: __uuidof operator; GUID structure "typedef struct _GUID").
TAG_NAMES = {'GUID': '_GUID'}
# COM base headers: they hold interfaces but do not include ole2.h.
COM_BASE_STEMS = {'unknwnbase', 'objidlbase', 'objidl', 'wtypes', 'wtypesbase', 'oaidl', 'oleidl', 'ocidl',
                  'propidlbase', 'combaseapi', 'objbase', 'ole2', 'oleauto'}
UMBRELLA_MEMBERS = {
    'uiautomation': ['uiautomationcore', 'uiautomationclient', 'uiautomationcoreapi'],
    'objbase': ['combaseapi', 'objidl', 'objidlbase', 'unknwnbase', 'wtypes', 'wtypesbase', 'propidlbase'],
    'ole2': ['objbase', 'oleauto', 'oleidl', 'oaidl'],
    'oleauto': ['oaidl'],
    'shobjidl': ['shobjidl_core'],
    'shlobj': ['shlobj_core', 'shobjidl', 'knownfolders'],
    'shlobj_core': ['shobjidl_core'],
    'oleacc': ['ole2'],
    'commdlg': ['prsht'],
    'roapi': ['activation'],
    'versionhelpers': ['winbase', 'winnt'],
    'windowsx': [],
    'inspectable': ['unknwnbase', 'hstring'],
    'winstring': ['hstring'],
}


class GenerationError(Exception):
    pass


def is_anonymous_member(field: rdl.Field) -> bool:
    """`Anonymous`, `Anonymous2`, ... with an inline record type: an unnamed member."""
    name = field.name
    if not name.startswith('Anonymous') or field.field_type.kind != 'record':
        return False
    return name[len('Anonymous'):] == '' or name[len('Anonymous'):].isdigit()


def safe_name(name: str) -> str:
    if name in CXX_KEYWORDS:
        return name + '_'
    return name


def guid_parts(text: str) -> tuple[int, int, int, list[int]]:
    """0xd3ee12ad_3865_4362_9746_b75a682df0e6 -> (Data1, Data2, Data3, Data4)."""
    digits = text.lower().replace('0x', '').replace('_', '')
    if len(digits) != 32:
        raise GenerationError(f'bad GUID {text}')
    data4: list[int] = []
    for offset in range(16, 32, 2):
        data4.append(int(digits[offset:offset + 2], 16))
    return (int(digits[0:8], 16), int(digits[8:12], 16), int(digits[12:16], 16), data4)


def guid_initializer(text: str) -> str:
    data1, data2, data3, data4 = guid_parts(text)
    tail = ', '.join(f'0x{byte:02x}' for byte in data4)
    return f'{{0x{data1:08x}, 0x{data2:04x}, 0x{data3:04x}, {{{tail}}}}}'


def guid_string(text: str) -> str:
    data1, data2, data3, data4 = guid_parts(text)
    head = ''.join(f'{byte:02x}' for byte in data4[0:2])
    tail = ''.join(f'{byte:02x}' for byte in data4[2:8])
    return f'{data1:08x}-{data2:04x}-{data3:04x}-{head}-{tail}'


def c_string(text: str, wide: bool) -> str:
    """An RDL string body (Rust escapes) as a C literal."""
    pieces: list[str] = []
    index = 0
    while index < len(text):
        character = text[index]
        if character == '\\' and text.startswith('\\u{', index):
            end = text.index('}', index)
            code = int(text[index + 3:end], 16)
            pieces.append(f'\\x{code:04x}' if wide else ''.join(f'\\x{b:02x}' for b in chr(code).encode('utf-8')))
            index = end + 1
            continue
        if character == '\\':
            pieces.append(text[index:index + 2])
            index += 2
            continue
        pieces.append(character)
        index += 1
    return ('L"' if wide else '"') + ''.join(pieces) + '"'


class Model:
    """The metadata for one architecture and the closure of what is used."""

    def __init__(self, declarations: list[rdl.Decl], arch: str) -> None:
        self.declarations: dict[str, rdl.Decl] = {}
        self.enumerators: dict[str, str] = {}
        for declaration in declarations:
            if rdl.arch_allows(declaration.attributes, arch):
                self.declarations[declaration.name] = declaration
                if declaration.kind == 'enum':
                    for member, _ in declaration.members:
                        self.enumerators.setdefault(member, declaration.name)
        self.used: set[str] = set()
        self.neutral: set[str] = set()
        self.iids: set[str] = set()
        self.clsids: set[str] = set()
        self.winrt: dict[str, str] = {}
        self.gaps: list[str] = []
        self.methods: set[str] = set()
        self.crt_headers: set[str] = set()

    def is_interface(self, name: str) -> bool:
        declaration = self.declarations.get(name)
        return declaration is not None and declaration.kind == 'interface'

    def resolve_named(self, type_ref: rdl.TypeRef) -> 'rdl.Decl | None':
        if type_ref.namespace:
            return None
        return self.declarations.get(type_ref.name)

    def type_names(self, type_ref: 'rdl.TypeRef | None', names: list[str]) -> None:
        """Appends every named type that type_ref mentions."""
        if type_ref is None:
            return
        if type_ref.kind == 'named':
            if type_ref.namespace:
                self.winrt[type_ref.name] = type_ref.namespace
            elif type_ref.name not in PRIMITIVES and type_ref.name != 'String':
                names.append(type_ref.name)
            return
        if type_ref.kind == 'record':
            self.record_type_names(type_ref.record, names)
            return
        if type_ref.kind == 'function':
            for parameter in type_ref.parameter_types:
                self.type_names(parameter, names)
            self.type_names(type_ref.target, names)
            return
        if type_ref.kind == 'pointer' and type_ref.target.kind == 'named' and \
                type_ref.target.name in ('u16', 'i8'):
            names.append('WCHAR' if type_ref.target.name == 'u16' else 'CHAR')
        self.type_names(type_ref.target, names)

    def record_type_names(self, record: rdl.Decl, names: list[str]) -> None:
        for field in record.fields:
            self.type_names(field.field_type, names)
            for bit in field.bits:
                self.type_names(bit.member_type, names)
        for nested in record.nested:
            if nested.kind in ('struct', 'union'):
                self.record_type_names(nested, names)

    def dependencies(self, declaration: rdl.Decl) -> list[str]:
        names: list[str] = []
        kind = declaration.kind
        if kind in ('struct', 'union'):
            self.record_type_names(declaration, names)
            base = declaration.attribute('base')
            if base is not None:
                raise GenerationError(f'{declaration.name}: #[base] records are not supported')
        elif kind == 'type':
            if declaration.name in NATIVE_TYPEDEFS:
                names.extend(NATIVE_TYPEDEF_DEPENDENCIES.get(declaration.name, []))
            else:
                self.type_names(declaration.aliased, names)
        elif kind == 'const':
            self.type_names(declaration.value_type, names)
        elif kind in ('function', 'callback'):
            for parameter in declaration.params:
                self.type_names(parameter.param_type, names)
            self.type_names(declaration.result, names)
        elif kind == 'interface':
            self.type_names(declaration.base, names)
            for method in declaration.methods:
                self.methods.add(method.name)
                for parameter in method.params:
                    self.type_names(parameter.param_type, names)
                self.type_names(method.result, names)
        return names

    def add_root(self, name: str) -> None:
        declarations = self.declarations
        if name.startswith('IID_') and self.is_interface(name[4:]):
            self.iids.add(name[4:])
            self.close(name[4:])
            return
        if name.startswith('CLSID_') and name[6:] in declarations:
            self.clsids.add(name[6:])
            self.close('CLSID')
            return
        if name in declarations:
            declaration = declarations[name]
            ansi = declarations.get(name + 'A')
            wide = declarations.get(name + 'W')
            if ansi is not None and wide is not None:
                # The neutral name: an alias chosen by UNICODE.
                self.neutral.add(name)
                self.close(name + 'A')
                self.close(name + 'W')
                return
            if declaration.kind == 'const' and wide is not None:
                self.neutral.add(name)
                self.close(name)
                self.close(name + 'W')
                return
            self.close(name)
            return
        if name + 'W' in declarations:
            self.neutral.add(name)
            self.close(name + 'W')
            if name + 'A' in declarations:
                self.close(name + 'A')
            return
        if name in self.enumerators:
            self.close(self.enumerators[name])
            return
        self.gaps.append(f'root {name}: not in the metadata')

    def close(self, name: str) -> None:
        pending = [name]
        while pending:
            current = pending.pop()
            if current in self.used:
                continue
            if current in CRT_OWNED:
                self.crt_headers.add(CRT_OWNED[current])
                continue
            declaration = self.declarations.get(current)
            if declaration is None:
                self.gaps.append(f'type {current}: referenced but not in the metadata')
                self.used.add(current)
                continue
            self.used.add(current)
            for dependency in self.dependencies(declaration):
                if dependency not in self.used:
                    pending.append(dependency)


class Speller:
    """C spelling of metadata types."""

    def __init__(self, model: Model) -> None:
        self.model = model
        # Set while spelling a COM method of a MIDL header whose IDL says
        # `long` (see INT_IDL_STEMS).
        self.long_i32 = False

    def base_name(self, type_ref: rdl.TypeRef) -> tuple[str, bool]:
        """(spelling, is_interface) of a named type."""
        if type_ref.namespace:
            path = type_ref.namespace.replace('super::', 'ABI::Windows::')
            return (path + type_ref.name, True)
        if type_ref.name == 'i32' and self.long_i32:
            return ('long', False)
        if type_ref.name in PRIMITIVES:
            return (PRIMITIVES[type_ref.name], False)
        if type_ref.name == 'String':
            raise GenerationError('String used as a type')
        return (type_ref.name, self.model.is_interface(type_ref.name))

    def spell(self, type_ref: rdl.TypeRef, declarator: str, const_object: bool = False,
              in_cxx: bool = True) -> str:
        kind = type_ref.kind
        if kind == 'named':
            name, interface = self.base_name(type_ref)
            if interface:
                # An interface named as a value is a pointer to it.
                inner = '*' + (' const ' if const_object else '') + declarator
                return f'{name} {inner}'.rstrip()
            prefix = 'const ' if const_object else ''
            return f'{prefix}{name} {declarator}'.rstrip()
        if kind == 'pointer' or kind == 'ptr64':
            target = type_ref.target
            if kind == 'pointer' and target.kind == 'named' and target.name in ('u16', 'i8'):
                text = 'WCHAR' if target.name == 'u16' else 'CHAR'
                prefix = '' if type_ref.mutable else 'const '
                inner = '*' + (' const ' if const_object else '') + declarator
                return f'{prefix}{text} {inner}'.rstrip()
            if kind == 'ptr64':
                return self.spell(target, declarator, const_object, in_cxx)
            inner = '*' + (' const ' if const_object else '') + declarator
            if target.kind == 'array':
                inner = '(' + inner + ')'
            return self.spell(target, inner, not type_ref.mutable, in_cxx)
        if kind == 'reference':
            marker = '&' if in_cxx else '*'
            return self.spell(type_ref.target, marker + declarator, not type_ref.mutable, in_cxx)
        if kind == 'array':
            inner = declarator
            if inner.startswith('*') or inner.startswith('&'):
                inner = '(' + inner + ')'
            return self.spell(type_ref.target, f'{inner}[{type_ref.count}]', const_object, in_cxx)
        if kind == 'function':
            parameters: list[str] = []
            for parameter in type_ref.parameter_types:
                parameters.append('...' if parameter is None else self.spell(parameter, '', False, in_cxx))
            convention = '__cdecl' if type_ref.abi == 'C' else '__stdcall'
            result = rdl.TypeRef('named')
            result.name = 'void'
            target = type_ref.target if type_ref.target is not None else result
            # The metadata's function type is a function pointer, as a callback name is.
            pointer = '*' + (' const ' if const_object else '') + declarator
            text = ', '.join(parameters) if parameters else 'void'
            return self.spell(target, f'({convention} {pointer})({text})', False, in_cxx)
        raise GenerationError(f'cannot spell {type_ref.describe()}')

    def parameters(self, params: list[rdl.Param], in_cxx: bool, reserved: set[str]) -> str:
        pieces: list[str] = []
        for parameter in params:
            if parameter.name == '...':
                pieces.append('...')
                continue
            name = safe_name(parameter.name)
            if name in reserved:
                name = ''
            pieces.append(self.spell(parameter.param_type, name, False, in_cxx))
        if not pieces:
            return 'void'
        return ', '.join(pieces)


def has_reference(params: list[rdl.Param], result: 'rdl.TypeRef | None') -> bool:
    pending: list[rdl.TypeRef] = []
    for parameter in params:
        if parameter.param_type is not None:
            pending.append(parameter.param_type)
    if result is not None:
        pending.append(result)
    while pending:
        current = pending.pop()
        if current.kind == 'reference':
            return True
        if current.target is not None:
            pending.append(current.target)
    return False


class Emitter:
    def __init__(self, model: Model, roots: list[str], includes: list[str]) -> None:
        self.model = model
        self.speller = Speller(model)
        self.roots = roots
        self.includes = includes
        self.macro_names: set[str] = set()
        self.layout = layout.Layout(model.declarations, PRIMITIVES)
        # Record field names: a macro of the same name would rename the field.
        self.field_names: set[str] = set()
        for name in model.used:
            declaration = model.declarations.get(name)
            if declaration is not None and declaration.kind in ('struct', 'union'):
                self.collect_field_names(declaration)

    def collect_field_names(self, record: rdl.Decl) -> None:
        pending: list[rdl.Decl] = [record]
        while pending:
            current = pending.pop()
            for field in current.fields:
                self.field_names.add(field.name)
                for bit in field.bits:
                    self.field_names.add(bit.name)
                if field.field_type.kind == 'record':
                    pending.append(field.field_type.record)
            for nested in current.nested:
                if nested.kind in ('struct', 'union'):
                    pending.append(nested)

    # --- ordering -----------------------------------------------------------

    def declared_dependencies(self, declaration: rdl.Decl) -> list[str]:
        """Names that must be declared before declaration (records are forward-declared)."""
        result: list[str] = []
        for name in self.model.dependencies(declaration):
            dependency = self.model.declarations.get(name)
            if dependency is None or dependency.kind in ('struct', 'union', 'interface', 'const', 'function'):
                continue
            result.append(name)
        if declaration.kind in ('struct', 'union'):
            for name in self.layout.complete_dependencies(declaration):
                result.append(name)
        return result

    def type_order(self) -> list[rdl.Decl]:
        """Typedefs, enums, records and callbacks in an order that declares before use."""
        candidates: list[str] = []
        for name in sorted(self.model.used):
            declaration = self.model.declarations.get(name)
            if declaration is not None and declaration.kind in ('type', 'enum', 'struct', 'union', 'callback'):
                candidates.append(name)
        order: list[rdl.Decl] = []
        state: dict[str, int] = {}
        for start in candidates:
            if state.get(start, 0) != 0:
                continue
            # Iterative depth-first search: (name, dependencies, next index).
            stack: list[tuple[str, list[str], int]] = []
            state[start] = 1
            stack.append((start, sorted(set(self.declared_dependencies(self.model.declarations[start]))), 0))
            while stack:
                name, dependencies, position = stack.pop()
                if position < len(dependencies):
                    stack.append((name, dependencies, position + 1))
                    dependency = dependencies[position]
                    declaration = self.model.declarations.get(dependency)
                    if declaration is None or declaration.kind not in ('type', 'enum', 'struct', 'union', 'callback'):
                        continue
                    status = state.get(dependency, 0)
                    if status == 1:
                        raise GenerationError(f'dependency cycle through {name} and {dependency}')
                    if status == 0:
                        state[dependency] = 1
                        stack.append((dependency, sorted(set(self.declared_dependencies(declaration))), 0))
                    continue
                state[name] = 2
                order.append(self.model.declarations[name])
        return order

    def interface_order(self) -> list[rdl.Decl]:
        names: list[str] = []
        for name in sorted(self.model.used):
            if self.model.is_interface(name):
                names.append(name)
        order: list[rdl.Decl] = []
        done: set[str] = set()
        for start in names:
            chain: list[str] = []
            current = start
            while current not in done and self.model.is_interface(current):
                chain.append(current)
                base = self.model.declarations[current].base
                if base is None or base.namespace:
                    break
                current = base.name
            for name in reversed(chain):
                if name not in done:
                    done.add(name)
                    order.append(self.model.declarations[name])
        return order

    # --- records ------------------------------------------------------------

    def record_lines(self, record: rdl.Decl, indent: str, inherited_pack: int) -> list[str]:
        lines: list[str] = []
        for nested in record.nested:
            if nested.kind in ('struct', 'union'):
                lines.append(f'{indent}{nested.kind} {nested.name} {{')
                lines.extend(self.record_lines(nested, indent + '    ', inherited_pack))
                lines.append(f'{indent}}};')
            elif nested.kind == 'const':
                self.model.gaps.append(f'{record.name}.{nested.name}: record-scoped constant not emitted')
        for field in record.fields:
            alignment = rdl.find_attribute(field.attributes, 'align')
            prefix = f'__declspec(align({alignment.arguments[0]})) ' if alignment is not None else ''
            field_type = field.field_type
            if field_type.kind == 'record':
                inner = field_type.record
                packed = inner.attribute('packed')
                if packed is not None and int(packed.arguments[0]) != inherited_pack:
                    raise GenerationError(f'{record.name}.{field.name}: inline record packing differs')
                lines.append(f'{indent}{prefix}{inner.kind} {{')
                lines.extend(self.record_lines(inner, indent + '    ', inherited_pack))
                if is_anonymous_member(field):
                    lines.append(f'{indent}}};')
                else:
                    lines.append(f'{indent}}} {safe_name(field.name)};')
                continue
            if field.bits:
                backing = self.speller.spell(field_type, '')
                for bit in field.bits:
                    if bit.name == '_':
                        lines.append(f'{indent}{backing} : {bit.width};')
                    else:
                        lines.append(f'{indent}{backing} {safe_name(bit.name)} : {bit.width};')
                continue
            lines.append(f'{indent}{prefix}{self.speller.spell(field_type, safe_name(field.name))};')
        return lines

    def record_definition(self, record: rdl.Decl) -> list[str]:
        lines: list[str] = []
        packed = record.attribute('packed')
        pack = int(packed.arguments[0]) if packed is not None else 0
        alignment = record.attribute('align')
        prefix = f'__declspec(align({alignment.arguments[0]})) ' if alignment is not None else ''
        if pack:
            lines.append(f'#pragma pack(push, {pack})')
        tag = TAG_NAMES.get(record.name, record.name)
        base = based_field(record)
        if base is None:
            lines.append(f'{record.kind} {prefix}{tag} {{')
            lines.extend(self.record_lines(record, '    ', pack))
        else:
            if record.nested:
                raise GenerationError(f'{record.name}: #[base] with nested records is not supported')
            # #[base] (MONITORINFOEXW): C++ derives, so the base's members
            # are the record's own; C holds the base as its first member.
            # The x64 layout is the same either way.
            base_type = self.speller.spell(base.field_type, '')
            lines.append('#ifdef __cplusplus')
            lines.append(f'{record.kind} {prefix}{tag} : {base_type} {{')
            lines.append('#else')
            lines.append(f'{record.kind} {prefix}{tag} {{')
            lines.append(f'    {self.speller.spell(base.field_type, safe_name(base.name))};')
            lines.append('#endif')
            lines.extend(self.record_lines(record, '    ', pack)[1:])
        lines.append('};')
        if pack:
            lines.append('#pragma pack(pop)')
        return lines

    # --- types file -----------------------------------------------------------

    def typedef_line(self, declaration: rdl.Decl) -> list[str]:
        name = declaration.name
        if name in NATIVE_TYPEDEFS:
            text = NATIVE_TYPEDEFS[name]
            if name == 'WCHAR':
                return ['#ifdef __cplusplus', 'typedef wchar_t WCHAR;', '#else',
                        'typedef unsigned short WCHAR;', '#endif']
            if text.endswith('*'):
                return [f'typedef {text}{name};']
            return [f'typedef {text} {name};']
        aliased = declaration.aliased
        if aliased.kind == 'named' and aliased.name.endswith('A') and aliased.name[:-1] == name and \
                name + 'W' in self.model.declarations:
            return ['#ifdef UNICODE', f'typedef {name}W {name};', '#else', f'typedef {name}A {name};', '#endif']
        return [f'typedef {self.speller.spell(aliased, name)};']

    def enum_lines(self, declaration: rdl.Decl) -> list[str]:
        representation = declaration.attribute('repr')
        base = representation.arguments[0] if representation is not None else 'i32'
        scoped = declaration.attribute('scoped') is not None
        lines: list[str] = []
        if scoped:
            lines.append('#ifdef __cplusplus')
            lines.append(f'enum class {declaration.name} : {PRIMITIVES[base]} {{')
        elif base != 'i32':
            lines.append('#ifdef __cplusplus')
            lines.append(f'typedef enum {declaration.name} : {PRIMITIVES[base]} {{')
            lines.append('#else')
            lines.append(f'typedef enum {declaration.name} {{')
            lines.append('#endif')
        else:
            lines.append(f'typedef enum {declaration.name} {{')
        for member, value in declaration.members:
            lines.append(f'    {member} = {value},')
        if scoped:
            lines.append('};')
            lines.append('#endif')
        else:
            lines.append(f'}} {declaration.name};')
            if declaration.attribute('flags') is not None:
                lines.append(f'STXWIN_ENUM_FLAG_OPERATORS({declaration.name})')
        return lines

    def callback_line(self, declaration: rdl.Decl) -> list[str]:
        convention = '__cdecl' if declaration.abi == 'C' else '__stdcall'
        result = declaration.result
        if has_reference(declaration.params, declaration.result):
            raise GenerationError(f'{declaration.name}: reference in a callback')
        parameters = self.speller.parameters(declaration.params, True, self.macro_names)
        inner = f'({convention} *{declaration.name})({parameters})'
        if result is None:
            return [f'typedef void {inner};']
        return [f'typedef {self.speller.spell(result, inner)};']

    def interface_lines(self, declaration: rdl.Decl) -> list[str]:
        guid = declaration.attribute('guid')
        uuid = f'__declspec(uuid("{guid_string(guid.arguments[0])}")) ' if guid is not None else ''
        base = ''
        if declaration.base is not None:
            base_name, _ = self.speller.base_name(declaration.base)
            base = f' : public {base_name}'
        lines = [f'struct {uuid}__declspec(novtable) {declaration.name}{base} {{']
        seen: set[str] = set()
        self.speller.long_i32 = declaration.source not in INT_IDL_STEMS
        for method in declaration.methods:
            if rdl.find_attribute(method.attributes, 'override_slot') is not None:
                continue
            if method.name in seen:
                self.model.gaps.append(f'{declaration.name}::{method.name}: overloaded COM method '
                                       '(MSVC vtable order for overloads differs); emitted in metadata order')
            seen.add(method.name)
            parameters = self.speller.parameters(method.params, True, self.macro_names)
            result = 'void' if method.result is None else self.speller.spell(method.result, '')
            lines.append(f'    virtual {result} __stdcall {method.name}({parameters}) = 0;')
        self.speller.long_i32 = False
        lines.append('};')
        return lines

    def types_header(self) -> str:
        lines = header_preamble('stxwin_types.h', 'Every Windows API type our sources use, in dependency order.')
        lines.append('#ifndef RC_INVOKED')
        lines.append('')
        for header in sorted(self.model.crt_headers):
            lines.append(f'#include <{header}>')
        if self.model.crt_headers:
            lines.append('')
        early = self.early_neutral_lines()
        if early:
            lines.append('/* Neutral aliases that are also member names: before any record. */')
            lines.extend(early)
            lines.append('')
        lines.append('#ifdef __cplusplus')
        # Bitwise operators for a flags enum (Learn: DEFINE_ENUM_FLAG_OPERATORS).
        lines.append('#define STXWIN_ENUM_BINARY(E, op, assign) \\')
        lines.append('    inline constexpr E operator op(E a, E b) { return E(((unsigned long long)a) op '
                     '((unsigned long long)b)); } \\')
        lines.append('    inline E &operator assign(E &a, E b) { a = a op b; return a; }')
        lines.append('#define STXWIN_ENUM_FLAG_OPERATORS(E) \\')
        lines.append('    STXWIN_ENUM_BINARY(E, |, |=) STXWIN_ENUM_BINARY(E, &, &=) STXWIN_ENUM_BINARY(E, ^, ^=) \\')
        lines.append('    inline constexpr E operator~(E a) { return E(~((unsigned long long)a)); }')
        lines.append('#else')
        lines.append('#define STXWIN_ENUM_FLAG_OPERATORS(E)')
        lines.append('#endif')
        lines.append('')
        # WinRT interfaces reached through the Win32 metadata stay opaque.
        if self.model.winrt:
            lines.append('#ifdef __cplusplus')
            for name in sorted(self.model.winrt):
                namespace = self.model.winrt[name].replace('super::', 'Windows::').rstrip(':').rstrip(':')
                parts = ['ABI'] + namespace.split('::')
                opening = ' '.join(f'namespace {part} {{' for part in parts if part)
                closing = ' '.join('}' for part in parts if part)
                lines.append(f'{opening} struct {name}; {closing}')
            lines.append('#endif')
            lines.append('')
        lines.append('/* Records and interfaces, declared first so any order of use is possible. */')
        for name in sorted(self.model.used):
            declaration = self.model.declarations.get(name)
            if declaration is not None and declaration.kind in ('struct', 'union'):
                lines.append(f'typedef {declaration.kind} {TAG_NAMES.get(name, name)} {name};')
            elif declaration is not None and declaration.kind == 'interface':
                lines.append(f'typedef struct {name} {name};')
        lines.append('')
        for declaration in self.type_order():
            kind = declaration.kind
            lines.append(f'/* {declaration.source}.h */')
            if kind == 'type':
                lines.extend(self.typedef_line(declaration))
            elif kind == 'enum':
                lines.extend(self.enum_lines(declaration))
            elif kind in ('struct', 'union'):
                if not declaration.fields and not declaration.nested:
                    lines.append(f'/* {declaration.name} is opaque: declared, never defined. */')
                else:
                    lines.extend(self.record_definition(declaration))
            elif kind == 'callback':
                lines.extend(self.callback_line(declaration))
            lines.append('')
        interfaces = self.interface_order()
        if interfaces:
            lines.append('#ifdef __cplusplus')
            lines.append('')
            for declaration in interfaces:
                lines.append(f'/* {declaration.source}.h */')
                lines.extend(self.interface_lines(declaration))
                lines.append('')
            lines.append('#endif /* __cplusplus */')
            lines.append('')
        lines.append('#endif /* RC_INVOKED */')
        lines.append('')
        lines.append('#endif /* STXWIN_TYPES_H */')
        return '\n'.join(lines) + '\n'

    # --- constants and functions ----------------------------------------------

    def constant_value(self, declaration: rdl.Decl) -> 'str | None':
        """The macro body, or None for a GUID-shaped constant (declared extern)."""
        value_type = declaration.value_type
        kind = declaration.value_kind
        if kind == 'string':
            encoding = declaration.attribute('encoding')
            wide = encoding is not None and encoding.arguments[0] == '"utf-16"'
            return c_string(declaration.value, wide)
        if kind == 'bool':
            return '1' if declaration.value == 'true' else '0'
        if kind != 'number':
            raise GenerationError(f'{declaration.name}: constant value {declaration.value}')
        text = declaration.value.replace('_', '')
        if value_type.kind == 'named' and value_type.name in ('GUID', 'PROPERTYKEY', 'DEVPROPKEY'):
            return None
        if value_type.kind == 'named' and value_type.name in PRIMITIVES:
            primitive = value_type.name
            if primitive in ('f32', 'f64'):
                return text + ('f' if primitive == 'f32' and '.' in text else '')
            number = int(text, 0)
            if primitive in ('i64', 'isize'):
                return f'({number}LL)' if number < 0 else f'{number}LL'
            if primitive in ('u64', 'usize'):
                return f'0x{number:X}ULL' if number > 0xFFFF else f'{number}ULL'
            if primitive == 'u32':
                return f'0x{number:X}' if number > 0x7FFFFFFF else f'{number}'
            if number < 0:
                return f'({number})'
            return f'{number}'
        number = int(text, 0)
        target = self.layout.resolve(value_type)
        name, interface = self.speller.base_name(value_type) if value_type.kind == 'named' else ('', False)
        if target.kind in ('pointer', 'ptr64') or interface:
            spelled = self.speller.spell(value_type, '')
            return f'(({spelled})(long long)({number}))'
        spelled = self.speller.spell(value_type, '')
        if number < 0:
            return f'(({spelled})({number}))'
        if number > 0x7FFFFFFF:
            return f'(({spelled})0x{number:X})'
        return f'(({spelled}){number})'

    def resource_constant(self, declaration: rdl.Decl) -> 'list[str] | None':
        """A numeric LPSTR/LPCSTR constant is MAKEINTRESOURCE(n): wide under UNICODE."""
        value_type = declaration.value_type
        if declaration.value_kind != 'number' or value_type.kind != 'named' or \
                value_type.name not in ('LPSTR', 'LPCSTR', 'PSTR', 'PCSTR'):
            return None
        number = int(declaration.value.replace('_', ''), 0)
        if number < 0:
            return None
        name = declaration.name
        return ['#ifdef UNICODE',
                f'#define {name} ((LPWSTR)(unsigned long long)(unsigned short)({number}))',
                '#else',
                f'#define {name} ((LPSTR)(unsigned long long)(unsigned short)({number}))',
                '#endif']

    def function_lines(self, declaration: rdl.Decl) -> list[str]:
        library = declaration.attribute('library')
        for argument in library.arguments:
            if argument == 'import':
                self.model.gaps.append(f'{declaration.name}: imported under another name; not emitted')
                return []
        convention = '__cdecl' if declaration.abi == 'C' else '__stdcall'
        noreturn = '__declspec(noreturn) ' if declaration.attribute('noreturn') is not None else ''
        prefix = f'__declspec(dllimport) {noreturn}'
        if has_reference(declaration.params, declaration.result):
            return ['#ifdef __cplusplus', prefix + self.signature(declaration, convention, True), '#else',
                    prefix + self.signature(declaration, convention, False), '#endif']
        return [prefix + self.signature(declaration, convention, True)]

    def signature(self, declaration: rdl.Decl, convention: str, in_cxx: bool) -> str:
        parameters = self.speller.parameters(declaration.params, in_cxx, self.macro_names)
        inner = f'{convention} {declaration.name}({parameters})'
        if declaration.result is None:
            return f'void {inner};'
        return f'{self.speller.spell(declaration.result, inner, False, in_cxx)};'

    def stems(self) -> dict[str, list[rdl.Decl]]:
        grouped: dict[str, list[rdl.Decl]] = {}
        for name in sorted(self.model.used):
            declaration = self.model.declarations.get(name)
            if declaration is None or declaration.kind not in ('const', 'function'):
                continue
            grouped.setdefault(declaration.source, []).append(declaration)
        for name in sorted(self.model.iids):
            grouped.setdefault(self.model.declarations[name].source, [])
        for name in sorted(self.model.clsids):
            grouped.setdefault(self.model.declarations[name].source, [])
        for name in sorted(self.model.neutral):
            target = self.model.declarations.get(name + 'W')
            if target is not None:
                grouped.setdefault(target.source, [])
        return grouped

    def is_early_neutral(self, name: str) -> bool:
        """A neutral function alias that is also a COM method or record field
        name (DeleteFile, FindText, GetObject) is defined in stxwin_types.h
        before any record, so the member is renamed with every use of it and
        the alias works for the function too; other aliases stay with their
        functions."""
        if name not in self.model.methods and name not in self.field_names:
            return False
        wide = self.model.declarations.get(name + 'W')
        return wide is not None and wide.kind == 'function'

    def early_neutral_lines(self) -> list[str]:
        lines: list[str] = []
        for name in sorted(self.model.neutral):
            if not self.is_early_neutral(name):
                continue
            ansi = name + 'A' if name + 'A' in self.model.declarations else name
            lines.extend(['#ifdef UNICODE', f'#define {name} {name}W', '#else', f'#define {name} {ansi}', '#endif'])
        return lines

    def stem_header(self, stem: str, declarations: list[rdl.Decl], members: list[str]) -> str:
        lines = header_preamble(f'{stem}.h', f'What our sources use of the SDK header {stem}.h.')
        lines.append('#include "stxwin_supplement.h"')
        for member in members:
            lines.append(f'#include "{member}.h"')
        lines.append('')
        macros: list[str] = []
        for declaration in declarations:
            if declaration.kind != 'const':
                continue
            if declaration.name in self.model.neutral:
                continue
            resource = self.resource_constant(declaration)
            if resource is not None:
                macros.extend(resource)
                continue
            body = self.constant_value(declaration)
            if body is None:
                continue
            if declaration.name in self.field_names or declaration.name in self.model.methods:
                self.model.gaps.append(f'{declaration.name}: constant macro shares a record field or method name')
            if declaration.source in ('sdkddkver', 'winapifamily'):
                macros.extend([f'#ifndef {declaration.name}', f'#define {declaration.name} {body}', '#endif'])
            else:
                macros.append(f'#define {declaration.name} {body}')
        for name in sorted(self.model.neutral):
            wide = self.model.declarations.get(name + 'W')
            if wide is None or wide.source != stem:
                continue
            if self.is_early_neutral(name):
                continue
            ansi = name + 'A' if name + 'A' in self.model.declarations else name
            if self.model.declarations[ansi].kind == 'type' or wide.kind in ('struct', 'union', 'type', 'callback'):
                continue
            macros.extend(['#ifdef UNICODE', f'#define {name} {name}W', '#else', f'#define {name} {ansi}', '#endif'])
        lines.extend(macros)
        lines.append('')
        lines.append('#ifndef RC_INVOKED')
        lines.append('#include "stxwin_types.h"')
        if stem in HAND_TAILS:
            lines.append(f'#include "{HAND_TAILS[stem]}"')
        lines.append('')
        externs: list[str] = []
        for guid_stem, declaration_text, initializer in self.guid_definitions():
            if guid_stem != stem:
                continue
            # Defined here only after initguid.h (Learn: DEFINE_GUID); otherwise uuid.lib holds it.
            externs.extend(['#ifdef INITGUID',
                            f'STXWIN_GUID_DEFINITION {selectany(declaration_text)} = {initializer};',
                            '#else', f'STXWIN_EXTERN_C {declaration_text};', '#endif'])
        lines.extend(externs)
        functions: list[str] = []
        for declaration in declarations:
            if declaration.kind == 'function':
                functions.extend(self.function_lines(declaration))
        if functions:
            lines.append('')
            lines.append('#ifdef __cplusplus')
            lines.append('extern "C" {')
            lines.append('#endif')
            lines.extend(functions)
            lines.append('#ifdef __cplusplus')
            lines.append('}')
            lines.append('#endif')
        lines.append('')
        lines.append('#endif /* RC_INVOKED */')
        lines.append('')
        lines.append(f'#endif /* {guard_name(stem + ".h")} */')
        return '\n'.join(lines) + '\n'

    def umbrella_header(self, name: str, members: list[str], lean_members: list[str]) -> str:
        lines = header_preamble(f'{name}.h', f'The SDK header {name}.h: includes its member headers.')
        lines.append('#include "stxwin_supplement.h"')
        for member in members:
            lines.append(f'#include "{member}.h"')
        if lean_members:
            lines.append('#ifndef WIN32_LEAN_AND_MEAN')
            for member in lean_members:
                lines.append(f'#include "{member}.h"')
            lines.append('#endif')
        lines.append('#ifndef RC_INVOKED')
        lines.append('#include "stxwin_types.h"')
        lines.append('#endif')
        lines.append('')
        lines.append(f'#endif /* {guard_name(name + ".h")} */')
        return '\n'.join(lines) + '\n'

    # --- uuid.c, export lists, layout check -------------------------------------

    def uuid_source(self) -> str:
        lines = ['/* Generated by tools/windows_api_headers/generate.py from windows-rs metadata; do not edit.',
                 ' * The GUIDs our sources use: interface IIDs, class CLSIDs and other GUID constants.',
                 ' * Built into uuid.lib by the own-runtime assembly project. */',
                 '#include "stxwin_types.h"', '#include "stxwin_supplement.h"', '']
        for _, declaration_text, initializer in self.guid_definitions():
            lines.append(f'STXWIN_GUID_DEFINITION {selectany(declaration_text)} = {initializer};')
        return '\n'.join(lines) + '\n'

    def guid_definitions(self) -> list[tuple[str, str, str]]:
        """(stem, declaration, initializer) of every used GUID-shaped constant."""
        definitions: list[tuple[str, str, str]] = []
        for name in sorted(self.model.iids):
            declaration = self.model.declarations[name]
            guid = declaration.attribute('guid')
            definitions.append((declaration.source, f'const IID IID_{name}', guid_initializer(guid.arguments[0])))
        for name in sorted(self.model.clsids):
            declaration = self.model.declarations[name]
            definitions.append((declaration.source, f'const CLSID CLSID_{name}', guid_initializer(declaration.value)))
        for name in sorted(self.model.used):
            declaration = self.model.declarations.get(name)
            if declaration is None or declaration.kind != 'const' or name in self.model.neutral:
                continue
            value_type = declaration.value_type
            if value_type.kind != 'named' or value_type.name not in ('GUID', 'PROPERTYKEY', 'DEVPROPKEY'):
                continue
            if value_type.name == 'GUID':
                definitions.append((declaration.source, f'const GUID {name}', guid_initializer(declaration.value)))
            else:
                guid = declaration.attribute('guid')
                initializer = f'{{{guid_initializer(guid.arguments[0])}, {int(declaration.value, 0)}}}'
                definitions.append((declaration.source, f'const {value_type.name} {name}', initializer))
        return definitions

    def export_lists(self) -> dict[str, str]:
        grouped: dict[str, list[str]] = {}
        for name in sorted(self.model.used):
            declaration = self.model.declarations.get(name)
            if declaration is None or declaration.kind != 'function':
                continue
            library = declaration.attribute('library')
            if 'import' in library.arguments:
                continue
            dll = library.arguments[0].strip('"').lower()
            grouped.setdefault(dll, []).append(name)
        for dll in REQUIRED_LIBRARIES:
            grouped.setdefault(dll, [])
        result: dict[str, str] = {}
        for dll in sorted(grouped):
            stem = dll[:-4] if dll.endswith('.dll') else dll
            file_name = DEF_FILE_NAMES.get(stem, stem)
            lines = [f'; Generated by tools/windows_api_headers/generate.py from windows-rs metadata; do not edit.',
                     f'; The {dll} functions our sources use.', f'LIBRARY {dll}', 'EXPORTS']
            for name in grouped[dll]:
                lines.append(f'    {name}')
            result[file_name] = '\n'.join(lines) + '\n'
        return result

    def layout_check(self) -> str:
        lines = ['// Generated by tools/windows_api_headers/generate.py; do not edit.',
                 '// The emitted records against the layout computed from windows-rs metadata',
                 '// (layout.py: x86_64 msvc rules, #[packed], #[align]). The metadata states',
                 '// no sizes itself; this proves the C spelling matches the metadata\'s fields.',
                 '#include "windows.h"']
        for stem in sorted(self.stems()):
            lines.append(f'#include "{stem}.h"')
        lines.append('')
        lines.append('#define STXWIN_OFFSET(type, member) __builtin_offsetof(type, member)')
        lines.append('')
        for name in sorted(self.model.used):
            declaration = self.model.declarations.get(name)
            if declaration is None or declaration.kind not in ('struct', 'union') or not declaration.fields:
                continue
            record_layout = self.layout.record(declaration)
            lines.append(f'static_assert(sizeof({name}) == {record_layout.size}, "{name} size");')
            lines.append(f'static_assert(alignof({name}) == {record_layout.alignment}, "{name} alignment");')
            if based_field(declaration) is not None:
                # A derived C++ record is not standard-layout: offsetof is
                # not admitted, so only its size and alignment are checked.
                continue
            for member, offset in record_layout.offsets:
                lines.append(f'static_assert(STXWIN_OFFSET({name}, {member}) == {offset}, "{name}.{member}");')
        return '\n'.join(lines) + '\n'


# Export lists are named for the import library the build links.
DEF_FILE_NAMES = {
    'api-ms-win-core-realtime-l1-1-1': 'mincore',
    'api-ms-win-core-synch-l1-2-0': 'synchronization',
    'api-ms-win-core-winrt-l1-1-0': 'runtimeobject',
    'api-ms-win-core-winrt-string-l1-1-0': 'runtimeobject_string',
    'api-ms-win-shcore-scaling-l1-1-1': 'shcore',
}
# Linked by name across gui_forms, stx and STXlib (queue/claude__windows-msvc-target.md):
# each gets an export list even when no used function comes from it.
REQUIRED_LIBRARIES = ['advapi32.dll', 'bcrypt.dll', 'dwmapi.dll', 'gdi32.dll', 'kernel32.dll', 'ole32.dll',
                      'oleacc.dll', 'oleaut32.dll', 'shell32.dll', 'uiautomationcore.dll', 'user32.dll',
                      'uxtheme.dll', 'api-ms-win-core-realtime-l1-1-1.dll']


def based_field(record: rdl.Decl) -> rdl.Field | None:
    """The record's #[base] field; the metadata puts it first."""
    for index, field in enumerate(record.fields):
        if rdl.find_attribute(field.attributes, 'base') is not None:
            if index != 0:
                raise GenerationError(f'{record.name}.{field.name}: #[base] is not the first field')
            return field
    return None


def selectany(declaration_text: str) -> str:
    """'const IID IID_X' -> 'const IID __declspec(selectany) IID_X'."""
    head, name = declaration_text.rsplit(' ', 1)
    return f'{head} __declspec(selectany) {name}'


def members_of(name: str, stems: dict[str, list[rdl.Decl]], com_stems: set[str]) -> list[str]:
    """The headers an emitted header includes after the supplement."""
    members: list[str] = []
    for member in UMBRELLA_MEMBERS.get(name, []):
        if member in stems or member in UMBRELLA_MEMBERS:
            members.append(member)
    if name in com_stems and name not in COM_BASE_STEMS:
        members.extend(['windows', 'ole2'])
    return members


def guard_name(file_name: str) -> str:
    return 'STXWIN_' + file_name.upper().replace('.', '_').replace('-', '_')


def header_preamble(file_name: str, purpose: str) -> list[str]:
    guard = guard_name(file_name)
    return [f'/* {file_name}: {purpose}',
            ' * Generated by tools/windows_api_headers/generate.py from windows-rs metadata',
            ' * (MIT OR Apache-2.0, see NOTICE); do not edit. */',
            f'#ifndef {guard}', f'#define {guard}', '']


def read_roots(path: pathlib.Path) -> tuple[list[str], list[str]]:
    includes: list[str] = []
    roots: list[str] = []
    section = ''
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        if line.startswith('['):
            section = line
            continue
        if section == '[includes]':
            includes.append(line.split()[0])
        elif section == '[roots]':
            roots.append(line)
    return includes, roots


def generate(root: pathlib.Path) -> dict[str, str]:
    """Returns {relative output path: content}."""
    pin = json.loads((HERE / 'PIN.json').read_text(encoding='utf-8'))
    includes, roots = read_roots(HERE / 'roots.txt')
    metadata = pathlib.Path(pin['local_checkout']) / 'metadata' / 'win32'
    model = Model(rdl.load_directory(metadata), pin['arch'])
    for name in BASE_TYPES:
        model.close(name)
    for name in roots:
        model.add_root(name)
    emitter = Emitter(model, roots, includes)
    stems = emitter.stems()

    # Supplement and generated names must not collide with parameter names.
    for declarations in stems.values():
        for declaration in declarations:
            if declaration.kind == 'const':
                emitter.macro_names.add(declaration.name)
    emitter.macro_names.update(model.neutral)

    outputs: dict[str, str] = {}
    base = 'src/hosted/windows/api/'
    outputs[base + 'include/stxwin_types.h'] = emitter.types_header()

    # COM interface headers are MIDL output, which brings in windows.h and the
    # COM base (Learn: CoTaskMemFree, "combaseapi.h (include Objbase.h)";
    # IUnknown, "unknwn.h"); the base headers themselves take nothing more.
    com_stems: set[str] = set()
    for declaration in model.declarations.values():
        if declaration.kind == 'interface':
            com_stems.add(declaration.source)

    # Which stems each included name reaches; the rest go into windows.h.
    reachable: set[str] = set()
    pending = list(includes) + list(WINDOWS_H_MEMBERS) + list(WINDOWS_H_NOT_LEAN)
    while pending:
        current = pending.pop()
        if current in reachable:
            continue
        reachable.add(current)
        pending.extend(UMBRELLA_MEMBERS.get(current, []))
        if current in com_stems and current not in COM_BASE_STEMS:
            pending.append('ole2')
    fallback: list[str] = []
    for stem in sorted(stems):
        if stem not in reachable:
            fallback.append(stem)
            model.gaps.append(f'{stem}.h: used but not reachable from an included header; added to windows.h')

    written: set[str] = set()
    queue = sorted(set(stems) | set(includes))
    while queue:
        name = queue.pop(0)
        if name == 'windows' or name in written:
            continue
        members = members_of(name, stems, com_stems)
        outputs[base + f'include/{name}.h'] = emitter.stem_header(name, stems.get(name, []), members)
        written.add(name)
        for member in members:
            if member not in written and member != 'windows':
                queue.append(member)
    windows_members: list[str] = []
    for stem in WINDOWS_H_MEMBERS:
        if stem in written:
            windows_members.append(stem)
    windows_members.extend(fallback)
    lean_members: list[str] = []
    for stem in WINDOWS_H_NOT_LEAN:
        if stem in written:
            lean_members.append(stem)
    outputs[base + 'include/windows.h'] = emitter.umbrella_header('windows', windows_members, lean_members)
    outputs[base + 'lib/uuid.c'] = emitter.uuid_source()
    for file_name, text in emitter.export_lists().items():
        outputs[base + f'lib/{file_name}.def'] = text
    outputs[base + 'check/layout_check.cpp'] = emitter.layout_check()

    report = ['# Generated by tools/windows_api_headers/generate.py; do not edit.',
              f'# metadata: windows-rs {pin["commit"]} (metadata tree {pin["metadata_tree"]}), arch {pin["arch"]}']
    counts: dict[str, int] = {}
    for name in model.used:
        declaration = model.declarations.get(name)
        if declaration is not None:
            counts[declaration.kind] = counts.get(declaration.kind, 0) + 1
    for kind in sorted(counts):
        report.append(f'{kind}: {counts[kind]}')
    report.append(f'iid: {len(model.iids)}')
    report.append(f'clsid: {len(model.clsids)}')
    report.append(f'neutral alias: {len(model.neutral)}')
    report.append(f'headers: {len(written) + 1}')
    report.append('[gaps]')
    for gap in sorted(set(model.gaps)):
        report.append(gap)
    outputs['tools/windows_api_headers/generated_report.txt'] = '\n'.join(report) + '\n'
    return outputs


def main() -> int:
    arguments = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    arguments.add_argument('--root', default=str(HERE.parent.parent))
    arguments.add_argument('--check', action='store_true',
                           help='compare with the committed files instead of writing them')
    options = arguments.parse_args()
    root = pathlib.Path(options.root)
    outputs = generate(root)
    generated_dirs = ['src/hosted/windows/api/include', 'src/hosted/windows/api/lib',
                      'src/hosted/windows/api/check']
    hand_written: set[str] = set()
    for name in HAND_WRITTEN:
        hand_written.add('src/hosted/windows/api/include/' + name)
    existing: set[str] = set()
    for directory in generated_dirs:
        folder = root / directory
        if folder.is_dir():
            for path in folder.iterdir():
                relative = path.relative_to(root).as_posix()
                if relative not in hand_written:
                    existing.add(relative)
    if options.check:
        differences: list[str] = []
        for relative in sorted(outputs):
            path = root / relative
            if not path.is_file() or path.read_bytes() != outputs[relative].encode('utf-8'):
                differences.append(f'differs: {relative}')
        for relative in sorted(existing - set(outputs)):
            differences.append(f'stale: {relative}')
        for line in differences:
            print(line)
        print(f'generate --check: {len(outputs)} files, {len(differences)} differences')
        return 1 if differences else 0
    for relative in sorted(existing - set(outputs)):
        (root / relative).unlink()
    for relative in sorted(outputs):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(outputs[relative].encode('utf-8'))
    print(f'generate: {len(outputs)} files written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
