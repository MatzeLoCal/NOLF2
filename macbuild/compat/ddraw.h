// Minimal stub of <ddraw.h> (DirectDraw) for the macOS build.
// DirectDraw is Windows-only; the DirectShow video manager header
// (dshowvideomgrimpl.h) includes it and references a couple of surface pointer
// types as opaque handles. Video playback is a later (Phase 4) concern, so this
// just provides the named types so the shared headers parse.
#ifndef __LT_COMPAT_DDRAW_H__
#define __LT_COMPAT_DDRAW_H__

#include <windows.h>
#include <unknwn.h>

struct IDirectDraw;
struct IDirectDraw7;
struct IDirectDrawSurface;
struct IDirectDrawSurface7;
struct IDirectDrawPalette;
struct IDirectDrawClipper;

typedef IDirectDraw*         LPDIRECTDRAW;
typedef IDirectDraw7*        LPDIRECTDRAW7;
typedef IDirectDrawSurface*  LPDIRECTDRAWSURFACE;
typedef IDirectDrawSurface7* LPDIRECTDRAWSURFACE7;
typedef IDirectDrawPalette*  LPDIRECTDRAWPALETTE;
typedef IDirectDrawClipper*  LPDIRECTDRAWCLIPPER;

#endif // __LT_COMPAT_DDRAW_H__
