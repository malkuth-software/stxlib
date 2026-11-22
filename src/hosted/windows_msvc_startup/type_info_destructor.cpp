// type_info's virtual destructor, for Stage 1 only.
//
// Every RTTI type descriptor the compiler emits points at type_info's
// virtual function table (the Microsoft ABI's ??_7type_info@@6B@;
// MicrosoftCXXABI.cpp, getTypeInfoVTable). The vcruntime DLL does not
// export that table: Microsoft's startup library supplies it statically,
// and a libc++ built in vcruntime mode leaves type_info to the platform.
// Defining the destructor, the class's only virtual function, emits the
// table here. A libc++ built with _LIBCPP_NO_VCRUNTIME (Stage 2) defines
// type_info and its destructor itself, so this file belongs to the
// vcruntime-mode archive only, as does new_delete.cpp.
//
// The destructor releases nothing: the descriptors are constant data, and
// the name cache the platform's type_info::name keeps is the vcruntime's
// to release at its own unload.

#include <typeinfo>

#if defined(STXLIB_CENSUS) && !defined(_MSC_VER)
// The census (proofs/b1/tools/census.py) parses this file on Linux, where
// type_info is declared only in std; the Microsoft ABI declares it global.
using std::type_info;
#endif

type_info::~type_info() noexcept {}
