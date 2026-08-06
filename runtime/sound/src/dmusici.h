// dmusici.h - DirectMusic Interactive compatibility stub
// IDirectMusicPerformance8 and related interfaces were removed from the
// Windows SDK after DirectX 9.  This stub provides forward declarations
// sufficient for compilation; the actual calls are disabled at runtime.
#pragma once
#ifndef _DMUSICI_H_
#define _DMUSICI_H_

#include <unknwn.h>
#include <dmusicc.h>

struct IDirectMusicPerformance;
typedef IDirectMusicPerformance *LPDIRECTMUSICPERFORMANCE;

struct IDirectMusicPerformance8;
typedef IDirectMusicPerformance8 *LPDIRECTMUSICPERFORMANCE8;

struct IDirectMusicSegment8;
typedef IDirectMusicSegment8 *LPDIRECTMUSICSEGMENT8;

struct IDirectMusicAudioPath;
typedef IDirectMusicAudioPath *LPDIRECTMUSICAUDIOPATH;

#endif // _DMUSICI_H_
