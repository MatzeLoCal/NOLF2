// macOS stub for MSVC <mmsystem.h> — the multimedia subset the engine references.
// WAVEFORMAT(EX)/timeGetTime/MMRESULT come from the windows.h shim.
#ifndef __LT_COMPAT_MMSYSTEM_H__
#define __LT_COMPAT_MMSYSTEM_H__
#include <windows.h>

#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0
#endif
static inline MMRESULT timeBeginPeriod(UINT) { return 0; }
static inline MMRESULT timeEndPeriod(UINT)   { return 0; }

typedef struct mmtime_tag {
    UINT wType;
    union { DWORD ms; DWORD sample; DWORD cb; DWORD ticks; } u;
} MMTIME, *LPMMTIME;

#endif
