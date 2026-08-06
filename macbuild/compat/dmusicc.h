// Minimal stub of <dmusicc.h> (DirectMusic Core) for the macOS build.
// DirectMusic is Windows-only and the engine's music path is disabled on macOS
// (see musicdriver.cpp). The engine's dmusici.h compat stub #includes this and
// only forward-declares the Performance/Segment interfaces it actually names, so
// this header just needs to satisfy the include and the few referenced types.
#ifndef __LT_COMPAT_DMUSICC_H__
#define __LT_COMPAT_DMUSICC_H__

#include <windows.h>
#include <unknwn.h>

// Forward declarations of the DirectMusic Core interfaces that leak into the
// shared sound headers as opaque pointers. No methods are ever invoked.
struct IDirectMusic;
struct IDirectMusic8;
struct IDirectMusicPort;
struct IDirectMusicBuffer;
struct IDirectMusicCollection;

typedef IDirectMusic*           LPDIRECTMUSIC;
typedef IDirectMusic8*          LPDIRECTMUSIC8;
typedef IDirectMusicPort*       LPDIRECTMUSICPORT;
typedef IDirectMusicBuffer*     LPDIRECTMUSICBUFFER;
typedef IDirectMusicCollection* LPDIRECTMUSICCOLLECTION;

#endif // __LT_COMPAT_DMUSICC_H__
