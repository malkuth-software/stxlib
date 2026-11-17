// The string group that vcruntime140 exports and the UCRT's import library
// does not (its export table lists memchr, memcmp, memcpy, memmove, memset,
// strchr, strrchr, strstr, wcschr, wcsrchr, wcsstr; the SDK 10.0.28000.0
// ucrt.lib defines none of them, llvm-nm, 2026-11-16). The compiler calls
// memcpy, memmove and memset for its own copies, so every module links this
// file statically (the static companion library, never the shared runtime).
//
// Compiled with -ffreestanding -fno-builtin so the compiler never turns
// these loops back into calls to themselves (LLVM's loop-idiom recognition
// is disabled by "no-builtins").
//
// Contracts are the C standard's (C17 7.24, 7.29.4). Overlap: memcpy's
// operands must not overlap; memmove's may. Kernels: copies and fills use
// the string instructions (rep movsb, rep stosb; Intel SDM vol. 1, 7.3.9.3,
// "fast string operation"), one decision before the loop; a backward move
// copies 8-byte words from the end and then the tail. The searches walk
// bytes or wide characters; their speed against vcruntime's vectorised
// versions is unmeasured (timings on this machine are not evidence) and is
// recorded as an open item in the queue entry.

#include "stxlib/types.hpp"

using stxlib::Byte;
using stxlib::u64;
using stxlib::usize;

extern "C" {

void* memcpy(void* const destination, const void* const source, const usize count) {
    void* rdi = destination;
    const void* rsi = source;
    usize rcx = count;
    __asm__ volatile("rep movsb" : "+D"(rdi), "+S"(rsi), "+c"(rcx) : : "memory");
    return destination;
}

void* memmove(void* const destination, const void* const source, const usize count) {
    Byte* const to = static_cast<Byte*>(destination);
    const Byte* const from = static_cast<const Byte*>(source);
    // Forward is safe unless the destination starts inside the source.
    const bool backward = to > from && to < from + count;
    if (!backward) {
        void* rdi = destination;
        const void* rsi = source;
        usize rcx = count;
        __asm__ volatile("rep movsb" : "+D"(rdi), "+S"(rsi), "+c"(rcx) : : "memory");
        return destination;
    }
    usize left = count;
    while (left >= sizeof(u64)) {
        left = left - sizeof(u64);
        u64 word = 0;
        __builtin_memcpy(&word, from + left, sizeof(u64));
        __builtin_memcpy(to + left, &word, sizeof(u64));
    }
    while (left > 0) {
        left = left - 1;
        to[left] = from[left];
    }
    return destination;
}

void* memset(void* const destination, const int value, const usize count) {
    void* rdi = destination;
    usize rcx = count;
    const unsigned char byte = static_cast<unsigned char>(value);
    __asm__ volatile("rep stosb" : "+D"(rdi), "+c"(rcx) : "a"(byte) : "memory");
    return destination;
}

int memcmp(const void* const left, const void* const right, const usize count) {
    const Byte* const a = static_cast<const Byte*>(left);
    const Byte* const b = static_cast<const Byte*>(right);
    for (usize index = 0; index < count; ++index) {
        if (a[index] != b[index]) {
            const int result = a[index] < b[index] ? -1 : 1;
            return result;
        }
    }
    return 0;
}

void* memchr(const void* const memory, const int value, const usize count) {
    const Byte* const bytes = static_cast<const Byte*>(memory);
    const Byte wanted = static_cast<Byte>(value);
    for (usize index = 0; index < count; ++index) {
        if (bytes[index] == wanted) return const_cast<Byte*>(bytes + index);
    }
    return nullptr;
}

char* strchr(const char* const text, const int value) {
    const char wanted = static_cast<char>(value);
    const char* cursor = text;
    for (;;) {
        if (*cursor == wanted) return const_cast<char*>(cursor);
        if (*cursor == '\0') return nullptr;
        cursor = cursor + 1;
    }
}

char* strrchr(const char* const text, const int value) {
    const char wanted = static_cast<char>(value);
    const char* found = nullptr;
    const char* cursor = text;
    for (;;) {
        if (*cursor == wanted) found = cursor;
        if (*cursor == '\0') return const_cast<char*>(found);
        cursor = cursor + 1;
    }
}

char* strstr(const char* const text, const char* const pattern) {
    if (*pattern == '\0') return const_cast<char*>(text);
    for (const char* start = text; *start != '\0'; start = start + 1) {
        const char* a = start;
        const char* b = pattern;
        while (*a != '\0' && *a == *b) {
            a = a + 1;
            b = b + 1;
        }
        if (*b == '\0') return const_cast<char*>(start);
    }
    return nullptr;
}

wchar_t* wcschr(const wchar_t* const text, const wchar_t value) {
    const wchar_t* cursor = text;
    for (;;) {
        if (*cursor == value) return const_cast<wchar_t*>(cursor);
        if (*cursor == L'\0') return nullptr;
        cursor = cursor + 1;
    }
}

wchar_t* wcsrchr(const wchar_t* const text, const wchar_t value) {
    const wchar_t* found = nullptr;
    const wchar_t* cursor = text;
    for (;;) {
        if (*cursor == value) found = cursor;
        if (*cursor == L'\0') return const_cast<wchar_t*>(found);
        cursor = cursor + 1;
    }
}

wchar_t* wcsstr(const wchar_t* const text, const wchar_t* const pattern) {
    if (*pattern == L'\0') return const_cast<wchar_t*>(text);
    for (const wchar_t* start = text; *start != L'\0'; start = start + 1) {
        const wchar_t* a = start;
        const wchar_t* b = pattern;
        while (*a != L'\0' && *a == *b) {
            a = a + 1;
            b = b + 1;
        }
        if (*b == L'\0') return const_cast<wchar_t*>(start);
    }
    return nullptr;
}

} // extern "C"
