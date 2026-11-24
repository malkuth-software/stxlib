/* eventtoken.h: the token a Windows Runtime event registration returns.
 *
 * Hand-written from Microsoft Learn ("EventRegistrationToken struct",
 * Windows.Foundation: one Int64 member, Value; eventtoken.h spells it
 * `__int64 value`); the type is Windows Runtime metadata, not Win32, so the
 * generator does not emit it (tools/windows_api_headers/REFERENCES.md). */
#ifndef STXWIN_EVENTTOKEN_H
#define STXWIN_EVENTTOKEN_H

#ifndef RC_INVOKED
typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;
#endif

#endif /* STXWIN_EVENTTOKEN_H */
