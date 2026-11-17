# Windows API headers generator

STXlib's own Windows API headers for the no-MinGW x86_64 msvc build
(projects `desks/coordinator/NO_MINGW_SWITCH.md`): SDK-named headers that
declare only what gui_forms, stx, STXlib, miniaudio and libc++ use, with one
export list per system DLL. Python 3, standard library only; output is
deterministic.

| File | Role |
|---|---|
| `PIN.json` | the metadata pin: windows-rs commit and `metadata` tree, local checkout, arch |
| `sources.json` | what `scan.py` reads (repositories at pinned revisions), exclusions, extra roots |
| `rdl.py` | reader for the RDL text form of the metadata (`metadata/metadata.rdl` is the vocabulary) |
| `scan.py` | writes `roots.txt`: included Windows header names and used metadata names |
| `layout.py` | x86_64 msvc record layout from the metadata, for the layout check |
| `generate.py` | writes `src/hosted/windows/api/{include,lib,check}` and `generated_report.txt`; `--check` compares instead |
| `probe.sh` | compile probes: every header alone (C++20, C11; plain, UNICODE, LEAN), layout check, uuid.c, RC_INVOKED |
| `REFERENCES.md` | Microsoft Learn pages behind the hand-written parts |

## Use

```text
python3 -I tools/windows_api_headers/scan.py        # needs the repositories in sources.json
python3 -I tools/windows_api_headers/generate.py    # needs the metadata checkout in PIN.json
python3 -I tools/windows_api_headers/generate.py --check   # reproduces the committed files byte for byte
sh tools/windows_api_headers/probe.sh <clang bin> <log dir> [crt include dir]
```

## Rules the generator applies

- Types live in one header, `stxwin_types.h`, in dependency order (records
  forward-declared, then typedefs, enums, records, callbacks; then COM
  interfaces as C++ structs with `__declspec(uuid)`, methods in metadata
  order, `#[override_slot]` re-declarations skipped). Constants (as macros,
  like the SDK), functions and GUID declarations live in the header named
  after their metadata file. Umbrella names (windows.h, uiautomation.h, ...)
  include their members; windows.h honours `WIN32_LEAN_AND_MEAN`; every
  header is macro-only under `RC_INVOKED`.
- The base typedefs the metadata flattens (`DWORD` as `u32`) take their C
  definitions from Learn's "Windows Data Types" (`unsigned long`, `wchar_t`).
- Neutral names (`CreateSemaphore`) alias the W or A form by `UNICODE`,
  unless the name is also a COM method or record field.
- GUIDs are declared `extern` and defined in `lib/uuid.c`.
- A used metadata file no included header reaches is added to windows.h and
  listed in `generated_report.txt` under `[gaps]`.
