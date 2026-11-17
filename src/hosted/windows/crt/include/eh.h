/* eh.h for x86_64-pc-windows-msvc: the terminate and unexpected hooks.
 *
 * terminate, set_terminate and _get_terminate are ucrtbase.dll's (Learn,
 * "terminate (CRT)", "set_terminate (CRT)", "_get_terminate"); libc++ routes
 * std::terminate and std::set_terminate there. set_unexpected,
 * _get_unexpected and unexpected are supplied by STXlib's C++ runtime in
 * each image (src/hosted/windows/msvc_runtime/terminate.cpp), so they carry
 * no import. The declarations match the ones libc++ itself writes in
 * src/support/runtime/exception_msvc.ipp (set_terminate and set_unexpected
 * non-throwing), so the two never disagree in one translation unit.
 */
#ifndef STXLIB_CRT_EH_H
#define STXLIB_CRT_EH_H

#include "stxlib_crt_common.h"

#ifndef __cplusplus
#error "eh.h is a C++ header"
#endif

extern "C" {

typedef void(__cdecl* terminate_handler)();
typedef void(__cdecl* terminate_function)();
typedef void(__cdecl* unexpected_handler)();
typedef void(__cdecl* unexpected_function)();

_STXLIB_UCRT _STXLIB_NORETURN void __cdecl terminate() noexcept;
_STXLIB_UCRT terminate_handler __cdecl set_terminate(terminate_handler handler) noexcept;
_STXLIB_UCRT terminate_handler __cdecl _get_terminate();

_STXLIB_NORETURN void __cdecl unexpected();
unexpected_handler __cdecl set_unexpected(unexpected_handler handler) noexcept;
unexpected_handler __cdecl _get_unexpected();

}

#endif /* STXLIB_CRT_EH_H */
