// ----------------------------------------------------------------------- //
//
// MODULE  : ltmacresmodule.h
//
// PURPOSE : The "module handle" the macOS build hands out in place of a Win32
//           HINSTANCE for resource-only DLLs (cres.dll).
//
//           This header is deliberately dependency-free and lives in compat/
//           because BOTH sides need it:
//             * the engine (runtime/kernel/src/sys/macos/macos_cres.cpp)
//               builds one of these, backed by a parsed PE resource table, and
//               returns it from the "cres_hinstance" engine hook;
//             * the game (libCShell.dylib) receives it as an HINSTANCE and
//               passes it to LoadString(), whose compat implementation in
//               windows.h calls straight through m_pfnLoadString.
//
//           Dispatching through a function pointer in DATA (rather than an
//           extern) is what keeps libCShell free of cross-module link symbols
//           — it is self-contained today and we want to keep it that way.
//
// ----------------------------------------------------------------------- //
#ifndef __LTMACRESMODULE_H__
#define __LTMACRESMODULE_H__

// 'LTMR' — guards against a non-resource HINSTANCE reaching LoadString.
#define LTMACRESMODULE_MAGIC 0x4C544D52

struct LTMacResModule
{
    unsigned int m_nMagic;

    // Copies string nID into pBuf (always NUL-terminated). Returns the number
    // of characters written, 0 if the id isn't present — matching Win32
    // LoadString's contract closely enough for the game's callers.
    int (*m_pfnLoadString)(struct LTMacResModule *pSelf,
                           unsigned int nID, char *pBuf, int nBufLen);

    void *m_pImpl;   // engine-side parsed resource data
};

#endif // __LTMACRESMODULE_H__
