"""Local paths for the generator's inputs, portable across machines.

PIN.json's `local_checkout` and each sources.json `path` are the defaults
(the Windows build machine's D: paths). Environment variables override them:

- STXWIN_METADATA: the windows-rs checkout holding `metadata/` at the pin;
- STXWIN_SOURCE_<NAME>: the path of the source named <NAME> in sources.json,
  upper-cased with every character other than A-Z and 0-9 turned into `_`
  (gui_forms -> STXWIN_SOURCE_GUI_FORMS, libcxx-22.1.8 ->
  STXWIN_SOURCE_LIBCXX_22_1_8).
"""
import os
import pathlib
import re


def metadata_checkout(pin: dict) -> pathlib.Path:
    return pathlib.Path(os.environ.get('STXWIN_METADATA', pin['local_checkout']))


def source_path(source: dict) -> str:
    variable = 'STXWIN_SOURCE_' + re.sub('[^A-Z0-9]', '_', source['name'].upper())
    return os.environ.get(variable, source['path'])
