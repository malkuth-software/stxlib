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

#endif /* STXLIB_CRT_SYS_STAT_H */
