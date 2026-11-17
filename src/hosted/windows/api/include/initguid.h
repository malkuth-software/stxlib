/* initguid.h: makes later GUID declarations definitions.
 *
 * Hand-written from Microsoft Learn: DEFINE_GUID macro ("include Initguid.h
 * before the header that declares the GUID to allocate storage for it").
 * Headers included after this one define their GUID constants, selectany, so
 * the definitions merge with uuid.lib's. */
#ifndef STXWIN_INITGUID_H
#define STXWIN_INITGUID_H

#include "stxwin_supplement.h"

#ifndef INITGUID
#define INITGUID
#endif
#undef DEFINE_GUID
#define DEFINE_GUID STXWIN_DEFINE_GUID_DEFINITION

#endif /* STXWIN_INITGUID_H */
