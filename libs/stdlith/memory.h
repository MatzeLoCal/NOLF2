#ifndef __MEMORY_H__
#define __MEMORY_H__

    #ifndef __NEW_H__
    // clang has no legacy <new.h> (uses <new>). Note -fms-compatibility (used by
    // the game DLLs) makes clang stop defining __GNUC__, so also test __clang__.
    #if !defined(__GNUC__) && !defined(__clang__)
    #include <new.h>
    #define __NEW_H__
    #endif
    #endif

    #ifndef __MALLOC_H__
    #if defined(__APPLE__)
    #include <stdlib.h>
    #else
    #include <malloc.h>
    #endif
    #define __MALLOC_H__
    #endif

    #ifndef __LITHEXCEPTION_H__
    #include "lithexception.h"
    #endif


#endif //__MEMORY_H__

