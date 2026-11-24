/* direct.h for x86_64-pc-windows-msvc over ucrtbase.dll: the directory
 * functions (Learn, "_getcwd, _wgetcwd", "_getdcwd, _wgetdcwd",
 * "_chdir, _wchdir", "_mkdir, _wmkdir", "_rmdir, _wrmdir", "_getdrive",
 * "_chdrive", "_getdiskfree") and their POSIX names.
 */
#ifndef STXLIB_CRT_DIRECT_H
#define STXLIB_CRT_DIRECT_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

struct _diskfree_t {
    unsigned total_clusters;
    unsigned avail_clusters;
    unsigned sectors_per_cluster;
    unsigned bytes_per_sector;
};

_STXLIB_UCRT char* __cdecl _getcwd(char* buffer, int size);
_STXLIB_UCRT wchar_t* __cdecl _wgetcwd(wchar_t* buffer, int size);
_STXLIB_UCRT char* __cdecl _getdcwd(int drive, char* buffer, int size);
_STXLIB_UCRT wchar_t* __cdecl _wgetdcwd(int drive, wchar_t* buffer, int size);
_STXLIB_UCRT int __cdecl _chdir(const char* path);
_STXLIB_UCRT int __cdecl _wchdir(const wchar_t* path);
_STXLIB_UCRT int __cdecl _mkdir(const char* path);
_STXLIB_UCRT int __cdecl _wmkdir(const wchar_t* path);
_STXLIB_UCRT int __cdecl _rmdir(const char* path);
_STXLIB_UCRT int __cdecl _wrmdir(const wchar_t* path);
_STXLIB_UCRT int __cdecl _getdrive(void);
_STXLIB_UCRT int __cdecl _chdrive(int drive);
_STXLIB_UCRT unsigned __cdecl _getdiskfree(unsigned drive, struct _diskfree_t* space);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX char* __cdecl getcwd(char* buffer, int size);
_STXLIB_UCRT_POSIX int __cdecl chdir(const char* path);
_STXLIB_UCRT_POSIX int __cdecl mkdir(const char* path);
_STXLIB_UCRT_POSIX int __cdecl rmdir(const char* path);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_DIRECT_H */
