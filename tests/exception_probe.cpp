// The positive control of tools/check_no_exceptions.py (the ctest
// stxlib_no_exception_symbols): compiled with -fexceptions on purpose
// (CMakeLists.txt, stxlib_exception_probe) and linked into nothing. Its
// object must show references to the exception runtime (a throw, a catch,
// the personality routine), so a scan that finds none in STXlib's objects
// is known to be able to see them on this toolchain.

#include <stdexcept>

int stxlib_exception_probe(int value);

int stxlib_exception_probe(const int value) {
    try {
        if (value < 0) throw std::invalid_argument("negative");
        return value;
    } catch (const std::invalid_argument&) {
        return 0;
    }
}
