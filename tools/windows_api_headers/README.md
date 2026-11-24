# Windows API headers generator

STXlib's own Windows API headers for the no-MinGW x86_64 msvc build
(projects `desks/coordinator/NO_MINGW_SWITCH.md`): SDK-named headers with one
export list per system DLL. Since STXlib 1.0.1 they declare the whole API of
every system DLL STXlib ships a `.def` for (`COVERED_LIBRARIES` in
`generate.py`): every function, and every type, constant, callback and
interface of the metadata files that declare them, so third-party code (the
image codecs) compiles as well as our own. `roots.txt`, what gui_forms, stx,
STXlib, miniaudio and libc++ use, is checked to be covered; it no longer
limits what is generated. Python 3, standard library only; output is
deterministic.

| File | Role |
|---|---|
| `PIN.json` | the metadata pin: windows-rs commit and `metadata` tree, local checkout, arch |
| `sources.json` | what `scan.py` reads (repositories at pinned revisions), exclusions, extra roots |
| `rdl.py` | reader for the RDL text form of the metadata (`metadata/metadata.rdl` is the vocabulary) |
| `scan.py` | writes `roots.txt`: included Windows header names and used metadata names |
| `layout.py` | x86_64 msvc record layout from the metadata, for the layout check |
| `generate.py` | writes `src/hosted/windows/api/{include,lib,check}` and `generated_report.txt`; `--check` compares instead |
| `probe.sh` | compile probes: every header alone (C++20, C17; plain, UNICODE, LEAN), layout check, uuid.c, RC_INVOKED |
| `REFERENCES.md` | Microsoft Learn pages behind the hand-written parts |

## Use

```text
python3 -I tools/windows_api_headers/scan.py        # needs the repositories in sources.json
python3 -I tools/windows_api_headers/generate.py    # needs the metadata checkout in PIN.json
python3 -I tools/windows_api_headers/generate.py --check   # reproduces the committed files byte for byte
STXWIN_LIBCXX=<libc++ include dir> sh tools/windows_api_headers/probe.sh <clang bin> <log dir> [crt include dir ...]
```

## Rules the generator applies

- Types live in two headers. `stxwin_types.h` holds what windows.h's
  member headers and our sources need; `stxwin_types_more.h` holds the rest
  and is included only by the headers outside windows.h, so a program that
  includes windows.h alone sees no more names than the SDK's windows.h
  gives it (appmodel.h's `PACKAGE_VERSION`, which libwebp defines as a
  macro, stays out). Each is in dependency order (records
  forward-declared, then typedefs, enums, records, callbacks; then COM
  interfaces as C++ structs with `__declspec(uuid)`, methods in metadata
  order, `#[override_slot]` re-declarations skipped). Constants (as macros,
  like the SDK), functions and GUID declarations live in the header named
  after their metadata file. Umbrella names (windows.h, uiautomation.h, ...)
  include their members; windows.h honours `WIN32_LEAN_AND_MEAN`; every
  header is macro-only under `RC_INVOKED`.
- The base typedefs the metadata flattens (`DWORD` as `u32`) take their C
  definitions from Learn's "Windows Data Types" (`unsigned long`, `wchar_t`).
- Neutral names (`CreateSemaphore`) alias the W or A form by `UNICODE`.
  A neutral function name that is also a COM method or record field name
  (`DeleteFile`, `FindText`, `GetObject`) is defined at the top of
  `stxwin_types.h`, before any record, so the member is renamed together
  with every use of it.
- `i32` is `long` in COM methods (IDL `long`), except in the UI Automation
  files (`INT_IDL_STEMS`), whose IDL says `int`.
- A `#[base]` first field (`MONITORINFOEXW`) is a base class in C++ and a
  first member in C; the layout check covers its size and alignment only.
- The GUID record's tag is `_GUID`, the type clang's `__uuidof` yields.
- Names the C runtime owns (`CRT_OWNED`: `va_list`, `size_t`, `intN_t`,
  `FILE`, `time_t`, ...) are not defined here; `stxwin_types.h` includes the
  C header that defines the ones it uses. The C runtime include directory
  comes before this one.
- COM headers include windows.h and ole2.h first, as MIDL output does.
- GUIDs are declared `extern` and defined in `lib/uuid.c`; with `INITGUID`
  (or `initguid.h`) they are `__declspec(selectany)` definitions instead.
- The scan follows first-party includes (relative ones from the includer's
  directory): a file that reaches a Windows header through our own headers
  contributes its identifiers. A token that is one of our names and not the
  Windows one is listed in `sources.json` under `excluded_tokens`.
- A used metadata file no included header reaches is added to windows.h and
  listed in `generated_report.txt` under `[gaps]`. A metadata file that only
  the complete coverage reaches is not; it is listed under `[standalone]`.
- A scoped enum (`#[scoped]`) is an `enum class` in C++; C sees its
  underlying type, since its enumerators would collide (several are named
  `None`).
- A record nested in another (`Outer::Inner` field types) is defined inline
  at its first field.
- Without `STRICT_TYPED_ITEMIDS`, the PIDL aliases (`PIDLIST_ABSOLUTE` and
  the rest), which the metadata spells as `LPITEMIDLIST`, are macros in
  `stxwin_supplement.h`.
