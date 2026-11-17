#pragma once

// Shared by cross_dll.cpp (the program) and cross_dll_library.cpp (its
// DLL): a polymorphic exception type each image emits its own copy of
// (inline virtual functions, so its type descriptor and vtable are
// per-image), matched across the two by decorated name, and the library's
// functions.

#include <exception>

#ifdef CROSS_DLL_LIBRARY
#define CROSS_DLL_API __declspec(dllexport)
#else
#define CROSS_DLL_API __declspec(dllimport)
#endif

struct Payload {
    int value;
    explicit Payload(const int payload_value) : value(payload_value) {}
    virtual ~Payload() {}
    virtual int doubled() const {
        return value * 2;
    }
};

struct LibraryOnly {
    int code;
};

using Callback = void (*)(int);

CROSS_DLL_API void library_throw(int value);
CROSS_DLL_API void library_catch(Callback callback, int value);
CROSS_DLL_API void library_rethrow(int value);
CROSS_DLL_API int library_uncaught();
CROSS_DLL_API std::exception_ptr library_capture(int value);
CROSS_DLL_API void library_rethrow_pointer(std::exception_ptr pointer);
