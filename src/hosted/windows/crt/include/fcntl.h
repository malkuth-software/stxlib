/* fcntl.h for x86_64-pc-windows-msvc: the _open flags (Learn, "_open,
 * _wopen", names only; values observed from ucrtbase.dll):
 * - access: a file opened with 0 reads and refuses writes, with 1 writes and
 *   refuses reads, with 2 does both;
 * - 0x8 moves every write to the end (the position after writing one byte
 *   to a 3-byte file is 4);
 * - 0x100 creates a missing file (the only single bit that opens one);
 * - 0x200 alone is refused with EINVAL, as Learn says _O_TRUNC is with
 *   read-only access;
 * - 0x40 deletes the file when the last descriptor closes (the file was gone
 *   after the next open);
 * - 0x400 makes creating an existing file fail with EEXIST (probe3);
 * - _setmode accepts 0x4000, 0x8000, 0x10000, 0x20000 and 0x40000 and
 *   reports 0x4000 as the default mode of a newly opened file.
 * The POSIX names libc++ uses (src/filesystem) are the same values.
 */
#ifndef STXLIB_CRT_FCNTL_H
#define STXLIB_CRT_FCNTL_H

#define _O_RDONLY 0x0000
#define _O_WRONLY 0x0001
#define _O_RDWR 0x0002
#define _O_APPEND 0x0008
#define _O_TEMPORARY 0x0040
#define _O_CREAT 0x0100
#define _O_TRUNC 0x0200
#define _O_EXCL 0x0400
#define _O_TEXT 0x4000
#define _O_BINARY 0x8000
#define _O_WTEXT 0x10000
#define _O_U16TEXT 0x20000
#define _O_U8TEXT 0x40000
#define _O_RAW _O_BINARY

#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_APPEND _O_APPEND
#define O_TEMPORARY _O_TEMPORARY
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_EXCL _O_EXCL
#define O_TEXT _O_TEXT
#define O_BINARY _O_BINARY
#define O_RAW _O_BINARY

#endif /* STXLIB_CRT_FCNTL_H */
