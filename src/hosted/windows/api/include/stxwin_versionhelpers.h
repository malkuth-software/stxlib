/* stxwin_versionhelpers.h: the version helper functions of versionhelpers.h.
 *
 * Hand-written from Microsoft Learn: "Version Helper functions",
 * IsWindowsVersionOrGreater, and "Verifying the System Version" (the
 * VerifyVersionInfoW and VerSetConditionMask pattern). Included by the
 * generated versionhelpers.h after winbase.h and winnt.h. */
#ifndef STXWIN_VERSIONHELPERS_SUPPLEMENT_H
#define STXWIN_VERSIONHELPERS_SUPPLEMENT_H

#include "winbase.h"
#include "winnt.h"
/* The metadata files VerSetConditionMask under shared.h (the SDK: winnt.h). */
#include "shared.h"

#ifndef RC_INVOKED

/* True when the running system's version is at least major.minor with
 * service pack service_pack_major. */
static __inline int IsWindowsVersionOrGreater(WORD major, WORD minor, WORD service_pack_major)
{
    OSVERSIONINFOEXW wanted;
    __builtin_memset(&wanted, 0, sizeof(wanted));
    wanted.dwOSVersionInfoSize = sizeof(wanted);
    wanted.dwMajorVersion = major;
    wanted.dwMinorVersion = minor;
    wanted.wServicePackMajor = service_pack_major;
    DWORDLONG mask = 0;
    mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    const DWORD fields = VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR;
    return VerifyVersionInfoW(&wanted, fields, mask) != 0;
}

/* Learn, "Version Helper functions": Windows 7 is 6.1, Windows 8 is 6.2,
 * Windows 8.1 is 6.3, Windows 10 is 10.0. Without a manifest declaring
 * support, the system reports 6.2 to the 8.1 and 10 tests. */
static __inline int IsWindows7OrGreater(void)
{
    return IsWindowsVersionOrGreater(6, 1, 0);
}
static __inline int IsWindows8OrGreater(void)
{
    return IsWindowsVersionOrGreater(6, 2, 0);
}
static __inline int IsWindows8Point1OrGreater(void)
{
    return IsWindowsVersionOrGreater(6, 3, 0);
}
static __inline int IsWindows10OrGreater(void)
{
    return IsWindowsVersionOrGreater(10, 0, 0);
}

#endif /* RC_INVOKED */

#endif /* STXWIN_VERSIONHELPERS_SUPPLEMENT_H */
