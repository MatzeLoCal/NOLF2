// ----------------------------------------------------------------------- //
//
// MODULE  : macos_cres.h
//
// PURPOSE : Win32 resource-DLL string tables on macOS.
//
//           NOLF2 keeps all of its UI text in cres.dll — a Win32 resource-only
//           DLL shipped inside GAME.rez. On Windows the engine LoadLibrary's it
//           and the game calls LoadString(hModule, id, ...). macOS can't load a
//           PE, so instead we read the file through the engine's file system
//           and parse its PE resource directory for RT_STRING tables.
//
//           The handle handed back to the game (via the "cres_hinstance"
//           engine hook) is an LTMacResModule*. That struct carries a function
//           pointer, so the compat LoadString() — which is compiled *into*
//           libCShell.dylib as a static inline — can call back into the engine
//           through plain data, with no cross-module link symbol. This mirrors
//           Win32, where HINSTANCE is likewise an opaque module token.
//
// ----------------------------------------------------------------------- //
#ifndef __MACOS_CRES_H__
#define __MACOS_CRES_H__

// The shape of the "module handle". Must stay in sync with the identical
// declaration in macbuild/compat/windows.h (which is what the game sees).
#include "ltmacresmodule.h"

// Returns the default client-resource module ("cres.dll"), loading and parsing
// it on first use. NULL if it can't be found/parsed (callers then behave as
// they did before: no strings).
LTMacResModule *LTMacCRes_GetDefault();

// Release the cached module (engine shutdown).
void LTMacCRes_Term();

#endif // __MACOS_CRES_H__
