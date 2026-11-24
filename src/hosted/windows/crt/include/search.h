/* search.h for x86_64-pc-windows-msvc over ucrtbase.dll: the linear searches
 * (Learn, "_lfind", "_lfind_s", "_lsearch", "_lsearch_s") and their POSIX
 * names; bsearch, qsort, bsearch_s and qsort_s are stdlib.h's.
 */
#ifndef STXLIB_CRT_SEARCH_H
#define STXLIB_CRT_SEARCH_H

#include "stxlib_crt_common.h"
#include "stdlib.h"

_STXLIB_CRT_BEGIN

_STXLIB_UCRT void* __cdecl _lfind(const void* key, const void* base, unsigned* count, unsigned size,
                                  int(__cdecl* compare)(const void* key, const void* element));
_STXLIB_UCRT void* __cdecl _lfind_s(const void* key, const void* base, unsigned* count, size_t size,
                                    int(__cdecl* compare)(void* context, const void* key, const void* element),
                                    void* context);
_STXLIB_UCRT void* __cdecl _lsearch(const void* key, void* base, unsigned* count, unsigned size,
                                    int(__cdecl* compare)(const void* key, const void* element));
_STXLIB_UCRT void* __cdecl _lsearch_s(const void* key, void* base, unsigned* count, size_t size,
                                      int(__cdecl* compare)(void* context, const void* key, const void* element),
                                      void* context);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX void* __cdecl lfind(const void* key, const void* base, unsigned* count, unsigned size,
                                       int(__cdecl* compare)(const void* key, const void* element));
_STXLIB_UCRT_POSIX void* __cdecl lsearch(const void* key, void* base, unsigned* count, unsigned size,
                                         int(__cdecl* compare)(const void* key, const void* element));
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_SEARCH_H */
