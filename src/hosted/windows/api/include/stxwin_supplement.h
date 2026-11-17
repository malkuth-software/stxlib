/* stxwin_supplement.h: the Windows API macros and inline helpers that are not
 * metadata (calling-convention names, HRESULT tests, word packing, GUID
 * comparison, interlocked names, resource ordinals).
 *
 * Hand-written from Microsoft Learn documentation (references in
 * tools/windows_api_headers/REFERENCES.md); no SDK, VC, mingw-w64, Wine or
 * ReactOS header was read. Every generated header includes this file first.
 * The macro part is usable from resource scripts (RC_INVOKED); the part that
 * needs types follows the types header. Target: x86_64 msvc. */
#ifndef STXWIN_SUPPLEMENT_H
#define STXWIN_SUPPLEMENT_H

#if !defined(_WIN64) && !defined(RC_INVOKED)
#error "STXlib's Windows API headers cover x86_64 msvc only (arm64 later)"
#endif

/* Linkage. */
#ifdef __cplusplus
#define STXWIN_EXTERN_C extern "C"
#else
#define STXWIN_EXTERN_C extern
#endif
#ifndef EXTERN_C
#define EXTERN_C STXWIN_EXTERN_C
#endif

/* Calling conventions (Learn: "Windows Data Types", "Argument Passing and
 * Naming Conventions"). On x64 every one is the single x64 convention; the
 * names stay for source compatibility and for x86 later. */
#define WINAPI __stdcall
#define WINAPIV __cdecl
#define APIENTRY WINAPI
#define CALLBACK __stdcall
#define NTAPI __stdcall
#define STDMETHODCALLTYPE __stdcall
#define STDMETHODVCALLTYPE __cdecl
#define STDAPICALLTYPE __stdcall
#define STDAPIVCALLTYPE __cdecl
#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE
#define STDAPI_(type) EXTERN_C type STDAPICALLTYPE
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(type) type STDMETHODCALLTYPE
#define IFACEMETHODIMP STDMETHODIMP
#define IFACEMETHODIMP_(type) STDMETHODIMP_(type)
#define DECLSPEC_IMPORT __declspec(dllimport)
#define DECLSPEC_NORETURN __declspec(noreturn)
#define DECLSPEC_NOVTABLE __declspec(novtable)
#define DECLSPEC_UUID(x) __declspec(uuid(x))
#define DECLSPEC_SELECTANY __declspec(selectany)

/* HRESULT tests and construction (Learn: "Using Macros for Error Handling",
 * SUCCEEDED, FAILED, HRESULT_FROM_WIN32, MAKE_HRESULT). */
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define IS_ERROR(status) (((unsigned long)(status)) >> 31 == 1)
#define HRESULT_CODE(hr) ((hr) & 0xFFFF)
#define HRESULT_FACILITY(hr) (((hr) >> 16) & 0x1fff)
#define HRESULT_SEVERITY(hr) (((hr) >> 31) & 0x1)
#define MAKE_HRESULT(severity, facility, code) \
    ((HRESULT)(((unsigned long)(severity) << 31) | ((unsigned long)(facility) << 16) | ((unsigned long)(code))))
#define STXWIN_FACILITY_WIN32 7
#define HRESULT_FROM_WIN32(x)                                                                       \
    ((HRESULT)(x) <= 0 ? ((HRESULT)(x))                                                             \
                       : ((HRESULT)(((x) & 0x0000FFFF) | (STXWIN_FACILITY_WIN32 << 16) | 0x80000000)))

/* Word and byte packing (Learn: LOWORD, HIWORD, LOBYTE, HIBYTE, MAKEWORD,
 * MAKELONG, MAKEWPARAM, MAKELPARAM, MAKELRESULT). */
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)(DWORD)MAKELONG(l, h))

/* Message parameter unpacking (Learn: GET_X_LPARAM, GET_Y_LPARAM in
 * windowsx.h; GET_WHEEL_DELTA_WPARAM, GET_KEYSTATE_WPARAM,
 * GET_XBUTTON_WPARAM, GET_APPCOMMAND_LPARAM in winuser.h). */
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_KEYSTATE_WPARAM(wParam) (LOWORD(wParam))
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#define GET_KEYSTATE_LPARAM(lParam) (LOWORD(lParam))
#define GET_APPCOMMAND_LPARAM(lParam) ((short)(HIWORD(lParam) & ~0xF000))

/* Colours (Learn: RGB, GetRValue, GetGValue, GetBValue). */
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) (LOBYTE(rgb))
#define GetGValue(rgb) (LOBYTE(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) (LOBYTE((rgb) >> 16))

/* Resource ordinals (Learn: MAKEINTRESOURCEW, IS_INTRESOURCE, MAKEINTATOM). */
#define IS_INTRESOURCE(r) ((((ULONG_PTR)(r)) >> 16) == 0)
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTATOM(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#ifdef UNICODE
#define MAKEINTRESOURCE MAKEINTRESOURCEW
#define TEXT(quote) L##quote
#else
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define TEXT(quote) quote
#endif
#define __TEXT(quote) TEXT(quote)

/* Structured exception filter results (Learn: "try-except statement"). */
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

/* Static initializers (Learn: InitializeSRWLock, InitializeConditionVariable,
 * InitOnceInitialize: "SRWLOCK_INIT", "CONDITION_VARIABLE_INIT",
 * "INIT_ONCE_STATIC_INIT"). */
#define SRWLOCK_INIT {0}
#define CONDITION_VARIABLE_INIT {0}
#define INIT_ONCE_STATIC_INIT {0}

/* GDI's failure handle (Learn: SelectObject, "HGDI_ERROR"). */
#define HGDI_ERROR ((HANDLE)(LONG_PTR)(LONG)0xFFFFFFFF)

/* Interlocked operations and fences (Learn: InterlockedIncrement and the
 * family; "Compiler intrinsics": the _Interlocked* intrinsics, which clang
 * provides as builtins for the msvc target). */
#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchange _InterlockedExchange
#define InterlockedExchangeAdd _InterlockedExchangeAdd
#define InterlockedCompareExchange _InterlockedCompareExchange
#define InterlockedIncrement64 _InterlockedIncrement64
#define InterlockedDecrement64 _InterlockedDecrement64
#define InterlockedExchange64 _InterlockedExchange64
#define InterlockedExchangeAdd64 _InterlockedExchangeAdd64
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#define InterlockedExchangePointer _InterlockedExchangePointer
#define InterlockedCompareExchangePointer _InterlockedCompareExchangePointer
#define YieldProcessor __builtin_ia32_pause
#define MemoryBarrier __faststorefence

/* Memory helpers (Learn: ZeroMemory, CopyMemory, MoveMemory, FillMemory,
 * SecureZeroMemory). The builtins need no C library header. */
#define ZeroMemory(destination, length) __builtin_memset((destination), 0, (length))
#define FillMemory(destination, length, fill) __builtin_memset((destination), (fill), (length))
#define CopyMemory(destination, source, length) __builtin_memcpy((destination), (source), (length))
#define MoveMemory(destination, source, length) __builtin_memmove((destination), (source), (length))

#ifndef RC_INVOKED
#include "stxwin_types.h"

/* GUID references and comparison (Learn: REFIID, REFCLSID, REFGUID,
 * IsEqualGUID, IsEqualIID, IsEqualCLSID; in C++ the references are C++
 * references and GUIDs compare with == and !=). */
#ifdef __cplusplus
typedef const GUID &REFGUID;
typedef const IID &REFIID;
typedef const CLSID &REFCLSID;
typedef const GUID &REFFMTID;
typedef const PROPERTYKEY &REFPROPERTYKEY;
inline int IsEqualGUID(REFGUID first, REFGUID second)
{
    return __builtin_memcmp(&first, &second, sizeof(GUID)) == 0;
}
inline bool operator==(REFGUID first, REFGUID second)
{
    return IsEqualGUID(first, second) != 0;
}
inline bool operator!=(REFGUID first, REFGUID second)
{
    return IsEqualGUID(first, second) == 0;
}
#define IsEqualIID(first, second) IsEqualGUID(first, second)
#define IsEqualCLSID(first, second) IsEqualGUID(first, second)
#define InlineIsEqualGUID(first, second) IsEqualGUID(first, second)

/* IID_PPV_ARGS (Learn): the interface's IID from its __declspec(uuid) and
 * the pointer as void **. The pointer must name a COM interface. */
template <typename Interface>
void **STXWIN_IID_PPV_ARGS_Helper(Interface **pointer)
{
    return reinterpret_cast<void **>(pointer);
}
#define IID_PPV_ARGS(pointer) __uuidof(**(pointer)), STXWIN_IID_PPV_ARGS_Helper(pointer)
#else
typedef const GUID *REFGUID;
typedef const IID *REFIID;
typedef const CLSID *REFCLSID;
typedef const GUID *REFFMTID;
typedef const PROPERTYKEY *REFPROPERTYKEY;
#define IsEqualGUID(first, second) (__builtin_memcmp((first), (second), sizeof(GUID)) == 0)
#define IsEqualIID(first, second) IsEqualGUID(first, second)
#define IsEqualCLSID(first, second) IsEqualGUID(first, second)
#endif

/* SecureZeroMemory (Learn): a fill the compiler may not remove. */
static __inline void *STXWIN_SecureZeroMemory(void *destination, unsigned long long length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    while (length != 0) {
        *bytes = 0;
        bytes = bytes + 1;
        length = length - 1;
    }
    return destination;
}
#define SecureZeroMemory(destination, length) STXWIN_SecureZeroMemory((destination), (length))
#endif /* RC_INVOKED */

#endif /* STXWIN_SUPPLEMENT_H */
