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

/* Learn: "_creat, _wcreat", "_chsize", "_eof", "_filelength", "_locking",
 * "_sopen, _wsopen", "_tell, _telli64", "_umask", "_mktemp, _wmktemp",
 * "_mktemp_s, _wmktemp_s", "_pipe", "_findfirst, ...", "_findnext, ...",
 * "_findclose" and "Filename search functions" (the _finddata structures and
 * the _A_ attributes). The *64 forms are the ones a 64-bit time_t names. */
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20

struct _finddata64_t {
    unsigned attrib;
    __time64_t time_create;
    __time64_t time_access;
    __time64_t time_write;
    long long size;
    char name[260];
};
struct _wfinddata64_t {
    unsigned attrib;
    __time64_t time_create;
    __time64_t time_access;
    __time64_t time_write;
    long long size;
    wchar_t name[260];
};
struct _finddata64i32_t {
    unsigned attrib;
    __time64_t time_create;
    __time64_t time_access;
    __time64_t time_write;
    unsigned long size;
    char name[260];
};
struct _wfinddata64i32_t {
    unsigned attrib;
    __time64_t time_create;
    __time64_t time_access;
    __time64_t time_write;
    unsigned long size;
    wchar_t name[260];
};
#define _finddata_t _finddata64i32_t
#define _wfinddata_t _wfinddata64i32_t
#define _finddatai64_t _finddata64_t
#define _wfinddatai64_t _wfinddata64_t
#define _findfirst _findfirst64i32
#define _findnext _findnext64i32
#define _wfindfirst _wfindfirst64i32
#define _wfindnext _wfindnext64i32
#define _findfirsti64 _findfirst64
#define _findnexti64 _findnext64
#define _wfindfirsti64 _wfindfirst64
#define _wfindnexti64 _wfindnext64

_STXLIB_UCRT int __cdecl _creat(const char* path, int permission);
_STXLIB_UCRT int __cdecl _wcreat(const wchar_t* path, int permission);
_STXLIB_UCRT int __cdecl _chsize(int descriptor, long size);
_STXLIB_UCRT int __cdecl _eof(int descriptor);
_STXLIB_UCRT long __cdecl _filelength(int descriptor);
_STXLIB_UCRT int __cdecl _locking(int descriptor, int mode, long count);
_STXLIB_UCRT int __cdecl _sopen(const char* path, int flags, int share, ...);
_STXLIB_UCRT int __cdecl _wsopen(const wchar_t* path, int flags, int share, ...);
_STXLIB_UCRT long __cdecl _tell(int descriptor);
_STXLIB_UCRT long long __cdecl _telli64(int descriptor);
_STXLIB_UCRT int __cdecl _umask(int mask);
_STXLIB_UCRT errno_t __cdecl _umask_s(int mask, int* previous);
_STXLIB_UCRT char* __cdecl _mktemp(char* name_template);
_STXLIB_UCRT wchar_t* __cdecl _wmktemp(wchar_t* name_template);
_STXLIB_UCRT errno_t __cdecl _mktemp_s(char* name_template, size_t size);
_STXLIB_UCRT errno_t __cdecl _wmktemp_s(wchar_t* name_template, size_t size);
_STXLIB_UCRT errno_t __cdecl _access_s(const char* path, int mode);
_STXLIB_UCRT errno_t __cdecl _waccess_s(const wchar_t* path, int mode);
_STXLIB_UCRT int __cdecl _pipe(int* descriptors, unsigned size, int text_mode);
_STXLIB_UCRT intptr_t __cdecl _findfirst64(const char* pattern, struct _finddata64_t* found);
_STXLIB_UCRT intptr_t __cdecl _wfindfirst64(const wchar_t* pattern, struct _wfinddata64_t* found);
_STXLIB_UCRT intptr_t __cdecl _findfirst64i32(const char* pattern, struct _finddata64i32_t* found);
_STXLIB_UCRT intptr_t __cdecl _wfindfirst64i32(const wchar_t* pattern, struct _wfinddata64i32_t* found);
_STXLIB_UCRT int __cdecl _findnext64(intptr_t handle, struct _finddata64_t* found);
_STXLIB_UCRT int __cdecl _wfindnext64(intptr_t handle, struct _wfinddata64_t* found);
_STXLIB_UCRT int __cdecl _findnext64i32(intptr_t handle, struct _finddata64i32_t* found);
_STXLIB_UCRT int __cdecl _wfindnext64i32(intptr_t handle, struct _wfinddata64i32_t* found);
_STXLIB_UCRT int __cdecl _findclose(intptr_t handle);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX int __cdecl access(const char* path, int mode);
_STXLIB_UCRT_POSIX int __cdecl chmod(const char* path, int permission);
_STXLIB_UCRT_POSIX int __cdecl chsize(int descriptor, long size);
_STXLIB_UCRT_POSIX int __cdecl close(int descriptor);
_STXLIB_UCRT_POSIX int __cdecl creat(const char* path, int permission);
_STXLIB_UCRT_POSIX int __cdecl dup(int descriptor);
_STXLIB_UCRT_POSIX int __cdecl dup2(int descriptor, int target);
_STXLIB_UCRT_POSIX int __cdecl eof(int descriptor);
_STXLIB_UCRT_POSIX long __cdecl filelength(int descriptor);
_STXLIB_UCRT_POSIX int __cdecl isatty(int descriptor);
_STXLIB_UCRT_POSIX int __cdecl locking(int descriptor, int mode, long count);
_STXLIB_UCRT_POSIX long __cdecl lseek(int descriptor, long offset, int origin);
_STXLIB_UCRT_POSIX char* __cdecl mktemp(char* name_template);
_STXLIB_UCRT_POSIX int __cdecl open(const char* path, int flags, ...);
_STXLIB_UCRT_POSIX int __cdecl read(int descriptor, void* buffer, unsigned count);
_STXLIB_UCRT_POSIX int __cdecl setmode(int descriptor, int mode);
_STXLIB_UCRT_POSIX int __cdecl sopen(const char* path, int flags, int share, ...);
_STXLIB_UCRT_POSIX long __cdecl tell(int descriptor);
_STXLIB_UCRT_POSIX int __cdecl umask(int mask);
_STXLIB_UCRT_POSIX int __cdecl unlink(const char* path);
_STXLIB_UCRT_POSIX int __cdecl write(int descriptor, const void* buffer, unsigned count);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_IO_H */
