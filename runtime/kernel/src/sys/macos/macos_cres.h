// ----------------------------------------------------------------------- //
//
// MODULE  : macos_cres.h
//
// PURPOSE : Win32 resource-DLL string tables -- and the mouse pointer -- on
//           macOS.
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

// ★ THE GAME'S MOUSE POINTER LIVES IN THE SAME DLL AS ITS TEXT.
//
// CCursorMgr::Init asks the engine for cursor resource IDC_POINTER (630), and on
// Windows ILTCursor::LoadCursorBitmapResource LoadImage()s it out of cres.dll
// and hands it to the OS -- that is the fat pale-blue arrow every NOLF2 menu is
// remembered with. This port answered that call with an EMPTY handle (it only
// needed CCursorMgr::Init not to fail), so with the game's default setting,
// "Hardware Cursor: On", the player got the plain macOS arrow instead
// (GitHub issue #6).
//
// The artwork was never missing: the retail cres.dll carries RT_GROUP_CURSOR 630
// with two RT_CURSOR images (8 bpp and 1 bpp, both 32x32), byte-identical to
// NOLF2/ClientRes/Shared/arrow.cur in the source tree. So read it from where
// Windows reads it, and a mod that ships its own cres.dll gets its own pointer.
//
// The image comes back as straight-alpha RGBA8, top row first.
#include <vector>
struct LTMacCursorImage
{
	int m_nWidth, m_nHeight;
	int m_nHotX, m_nHotY;       // pixels from the top-left corner
	int m_nBitCount;            // of the DIB it was decoded from (for the log)
	std::vector<unsigned char> m_aRGBA;
};

// Decode cursor resource nID from the default client-resource module. Of the
// images in the group the deepest one is used, which is what LoadImage with
// LR_DEFAULTCOLOR picks on any display made this century. False if the module,
// the group or a decodable image is missing; cOut is then untouched.
bool LTMacCRes_LoadCursor(unsigned int nID, LTMacCursorImage &cOut);

#endif // __MACOS_CRES_H__
