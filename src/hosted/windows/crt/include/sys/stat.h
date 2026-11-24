/* sys/stat.h for x86_64-pc-windows-msvc: the permission values _open,
 * _wopen and _chmod take (Learn, "_open, _wopen", names only).
 *
 * Probe: of the sixteen single bits as the creation permission, only 0x80
 * made the new file writable; every other bit made it read-only. So
 * _S_IWRITE is 0x80. _S_IREAD has no observable effect (every file is
 * readable on Windows, Learn) and is the traditional 0x100, which ucrtbase
 * accepts (unconfirmed beyond that).
 *
 * struct stat and the stat functions are not provided: no source in the
 * build uses them (libc++'s filesystem uses its own on Windows).
 */
#ifndef STXLIB_CRT_SYS_STAT_H
#define STXLIB_CRT_SYS_STAT_H

#include "../stxlib_crt_common.h"
#include "types.h"

#define _S_IWRITE 0x0080
#define _S_IREAD 0x0100
#define S_IWRITE _S_IWRITE
#define S_IREAD _S_IREAD

/* The stat family (Learn, "_stat, _stat32, _stat64, _stati64, _stat32i64,
 * _stat64i32, _wstat, ..." and "_fstat, _fstat32, _fstat64, _fstati64,
 * _fstat32i64, _fstat64i32"): four structures by the width of the size and
 * the times; with a 64-bit time_t, _stat is _stat64i32 and _stati64 is
 * _stat64. The st_mode bits are Learn's (_S_IFMT and the rest, _S_IEXEC). */
#define _S_IFMT 0xF000
#define _S_IFDIR 0x4000
#define _S_IFCHR 0x2000
#define _S_IFIFO 0x1000
#define _S_IFREG 0x8000
#define _S_IEXEC 0x0040
#define S_IFMT _S_IFMT
#define S_IFDIR _S_IFDIR
#define S_IFCHR _S_IFCHR
#define S_IFREG _S_IFREG
#define S_IEXEC _S_IEXEC

typedef unsigned short _ino_t;
typedef unsigned int _dev_t;
typedef _ino_t ino_t;
typedef _dev_t dev_t;

_STXLIB_CRT_BEGIN

struct _stat32 {
    _dev_t st_dev;
    _ino_t st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    _dev_t st_rdev;
    _off_t st_size;
    __time32_t st_atime;
    __time32_t st_mtime;
    __time32_t st_ctime;
};
struct _stat32i64 {
    _dev_t st_dev;
    _ino_t st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    _dev_t st_rdev;
    long long st_size;
    __time32_t st_atime;
    __time32_t st_mtime;
    __time32_t st_ctime;
};
struct _stat64i32 {
    _dev_t st_dev;
    _ino_t st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    _dev_t st_rdev;
    _off_t st_size;
    __time64_t st_atime;
    __time64_t st_mtime;
    __time64_t st_ctime;
};
struct _stat64 {
    _dev_t st_dev;
    _ino_t st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    _dev_t st_rdev;
    long long st_size;
    __time64_t st_atime;
    __time64_t st_mtime;
    __time64_t st_ctime;
};
/* struct stat, the POSIX name's structure: _stat64i32's layout. */
struct stat {
    _dev_t st_dev;
    _ino_t st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    _dev_t st_rdev;
    _off_t st_size;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};
#define _stat _stat64i32
#define _stati64 _stat64
#define _fstat _fstat64i32
#define _fstati64 _fstat64
#define _wstat _wstat64i32
#define _wstati64 _wstat64

_STXLIB_UCRT int __cdecl _stat32(const char* path, struct _stat32* status);
_STXLIB_UCRT int __cdecl _stat32i64(const char* path, struct _stat32i64* status);
_STXLIB_UCRT int __cdecl _stat64i32(const char* path, struct _stat64i32* status);
_STXLIB_UCRT int __cdecl _stat64(const char* path, struct _stat64* status);
_STXLIB_UCRT int __cdecl _wstat32(const wchar_t* path, struct _stat32* status);
_STXLIB_UCRT int __cdecl _wstat32i64(const wchar_t* path, struct _stat32i64* status);
_STXLIB_UCRT int __cdecl _wstat64i32(const wchar_t* path, struct _stat64i32* status);
_STXLIB_UCRT int __cdecl _wstat64(const wchar_t* path, struct _stat64* status);
_STXLIB_UCRT int __cdecl _fstat32(int descriptor, struct _stat32* status);
_STXLIB_UCRT int __cdecl _fstat32i64(int descriptor, struct _stat32i64* status);
_STXLIB_UCRT int __cdecl _fstat64i32(int descriptor, struct _stat64i32* status);
_STXLIB_UCRT int __cdecl _fstat64(int descriptor, struct _stat64* status);

/* The POSIX names (Learn: "The POSIX names ... stat, fstat"): struct stat has
 * _stat64i32's layout, so they import _stat64i32 and _fstat64i32. */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX int __cdecl stat(const char* path, struct stat* status);
_STXLIB_UCRT_POSIX int __cdecl fstat(int descriptor, struct stat* status);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_SYS_STAT_H */
