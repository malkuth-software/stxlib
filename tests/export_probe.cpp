// The positive control of tools/check_no_export_directives.py (the ctest
// stxlib_state_static_no_exports): compiled on Windows only
// (CMakeLists.txt, stxlib_export_probe) and linked into nothing. Its
// dllexport must leave an export directive in the object's .drectve
// section, so a scan that finds none in stxlib_state's objects is known to
// be able to see them on this toolchain.

__declspec(dllexport) int stxlib_export_probe(int value);

__declspec(dllexport) int stxlib_export_probe(const int value) {
    return value + 1;
}
