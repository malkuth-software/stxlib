/* Windows API names libc++ 22.1.8 uses that STXlib's Windows API headers
 * (src/hosted/windows/api/include, at fc43fb6) do not yet declare.
 * toolchain.cmake force-includes this file into every libc++ source, so
 * libc++ builds now; each entry goes once the API headers carry it, and the
 * file goes with the last one.
 *
 * Every entry is guarded or a compatible redeclaration, so it is harmless
 * after the API headers gain the name.
 *
 *   WINAPI_FAMILY_PARTITION, WINAPI_PARTITION_DESKTOP (src/chrono.cpp):
 *     a desktop build; libc++ then calls GetSystemTimePreciseAsFileTime,
 *     which the API headers declare.
 *   AreFileApisANSI, CP_ACP, CP_OEMCP (src/filesystem/path.cpp): Learn
 *     ("AreFileApisANSI", fileapi.h, BOOL WINAPI, no parameters; "Code Page
 *     Identifiers"). CP_ACP 0 and CP_OEMCP 1 are the traditional values,
 *     unconfirmed here; the API headers' generator should supply them.
 */
#ifndef STXLIB_LIBCXX_API_GAPS_H
#define STXLIB_LIBCXX_API_GAPS_H

#ifndef WINAPI_PARTITION_DESKTOP
#define WINAPI_PARTITION_DESKTOP 1
#endif
#ifndef WINAPI_FAMILY_PARTITION
#define WINAPI_FAMILY_PARTITION(partition) (partition)
#endif
#ifndef CP_ACP
#define CP_ACP 0
#endif
#ifndef CP_OEMCP
#define CP_OEMCP 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) int __stdcall AreFileApisANSI(void);
#ifdef __cplusplus
}
#endif

#endif /* STXLIB_LIBCXX_API_GAPS_H */
