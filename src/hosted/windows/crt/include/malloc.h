/* malloc.h for x86_64-pc-windows-msvc over ucrtbase.dll: the Microsoft
 * allocation functions (Learn, "_aligned_malloc", "_aligned_free",
 * "_aligned_realloc", "_msize", "_callnewh", "_alloca").
 *
 * Ownership: a block from _aligned_malloc is released only by _aligned_free,
 * a block from malloc only by free; the two families never mix.
 */
#ifndef STXLIB_CRT_MALLOC_H
#define STXLIB_CRT_MALLOC_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

_STXLIB_UCRT void* __cdecl malloc(size_t size);
_STXLIB_UCRT void* __cdecl calloc(size_t count, size_t size);
_STXLIB_UCRT void* __cdecl realloc(void* block, size_t size);
_STXLIB_UCRT void __cdecl free(void* block);
_STXLIB_UCRT size_t __cdecl _msize(void* block);

_STXLIB_UCRT void* __cdecl _aligned_malloc(size_t size, size_t alignment);
_STXLIB_UCRT void* __cdecl _aligned_realloc(void* block, size_t size, size_t alignment);
_STXLIB_UCRT void __cdecl _aligned_free(void* block);
_STXLIB_UCRT size_t __cdecl _aligned_msize(void* block, size_t alignment, size_t offset);

/* Calls the installed new handler for a failed allocation of size bytes;
 * nonzero when a handler ran and the allocation should be retried. */
_STXLIB_UCRT int __cdecl _callnewh(size_t size);

_STXLIB_CRT_END

/* Stack allocation: the compiler's builtin (no library call; clang emits the
 * __chkstk probe for large frames). */
#define _alloca(size) __builtin_alloca(size)
#define alloca(size) __builtin_alloca(size)

#endif /* STXLIB_CRT_MALLOC_H */
