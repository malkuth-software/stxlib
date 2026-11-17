/* io.h for x86_64-pc-windows-msvc over ucrtbase.dll: the low-level file
 * descriptor functions (Learn, "_open, _wopen", "_close", "_read", "_write",
 * "_lseek, _lseeki64", "_get_osfhandle", "_open_osfhandle", "_setmode",
 * "_isatty", "_dup, _dup2", "_commit", "_filelength, _filelengthi64",
 * "_chsize_s", "_access, _waccess", "_chmod, _wchmod", "_unlink, _wunlink",
 * "_sopen_s, _wsopen_s").
 *
 * The flag values are fcntl.h's and the permission values sys/stat.h's.
 * _get_osfhandle returns the operating-system handle as an intptr_t; -1 for
 * a descriptor that is not open. The descriptor keeps owning the handle.
 */
#ifndef STXLIB_CRT_IO_H
#define STXLIB_CRT_IO_H

#include "stxlib_crt_common.h"
/* off_t, the offset type libc++'s filesystem expects beside io.h. */
#include "sys/types.h"

_STXLIB_CRT_BEGIN

_STXLIB_UCRT int __cdecl _open(const char* path, int flags, ...);
_STXLIB_UCRT int __cdecl _wopen(const wchar_t* path, int flags, ...);
_STXLIB_UCRT errno_t __cdecl _sopen_s(int* descriptor, const char* path, int flags, int share, int permission);
_STXLIB_UCRT errno_t __cdecl _wsopen_s(int* descriptor, const wchar_t* path, int flags, int share, int permission);
_STXLIB_UCRT int __cdecl _close(int descriptor);
_STXLIB_UCRT int __cdecl _read(int descriptor, void* buffer, unsigned count);
_STXLIB_UCRT int __cdecl _write(int descriptor, const void* buffer, unsigned count);
_STXLIB_UCRT long __cdecl _lseek(int descriptor, long offset, int origin);
_STXLIB_UCRT long long __cdecl _lseeki64(int descriptor, long long offset, int origin);
_STXLIB_UCRT long long __cdecl _filelengthi64(int descriptor);
_STXLIB_UCRT errno_t __cdecl _chsize_s(int descriptor, long long size);
_STXLIB_UCRT int __cdecl _commit(int descriptor);
_STXLIB_UCRT int __cdecl _dup(int descriptor);
_STXLIB_UCRT int __cdecl _dup2(int descriptor, int target);
_STXLIB_UCRT int __cdecl _isatty(int descriptor);
_STXLIB_UCRT int __cdecl _setmode(int descriptor, int mode);
_STXLIB_UCRT intptr_t __cdecl _get_osfhandle(int descriptor);
_STXLIB_UCRT int __cdecl _open_osfhandle(intptr_t handle, int flags);
_STXLIB_UCRT int __cdecl _access(const char* path, int mode);
_STXLIB_UCRT int __cdecl _waccess(const wchar_t* path, int mode);
_STXLIB_UCRT int __cdecl _chmod(const char* path, int permission);
_STXLIB_UCRT int __cdecl _wchmod(const wchar_t* path, int permission);
_STXLIB_UCRT int __cdecl _unlink(const char* path);
_STXLIB_UCRT int __cdecl _wunlink(const wchar_t* path);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_IO_H */
