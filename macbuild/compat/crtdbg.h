// macOS stub for MSVC <crtdbg.h> — debug-heap/assert macros become no-ops.
#ifndef __LT_COMPAT_CRTDBG_H__
#define __LT_COMPAT_CRTDBG_H__
#include <assert.h>
#ifndef _ASSERT
#define _ASSERT(expr)        ((void)0)
#endif
#ifndef _ASSERTE
#define _ASSERTE(expr)       ((void)0)
#endif
#ifndef _CrtCheckMemory
#define _CrtCheckMemory()    (1)
#endif
#ifndef _CrtSetDbgFlag
#define _CrtSetDbgFlag(f)    (0)
#endif
#endif
