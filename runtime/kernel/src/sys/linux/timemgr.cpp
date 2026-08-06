
#include "timemgr.h"
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// The engine's millisecond clock. This mirrors sys/win/timemgr.cpp, including
// the reason it subtracts a base:
//
//   "It does this so the timer won't start out at really large numbers
//    (otherwise, the numbers will be less accurate and the timer will recycle
//    faster). At the windows timer rate, it would take 49.71 days for the
//    timer to recycle itself."
//
// That base subtraction is not a nicety. CClientMgr::Update does
//     m_CurTime = m_nCurUpdateTimeMS / 1000.0f;   // float!
// and the game hands that value back to the server as
//     Writeint32( (int)(GetTime() * 1000.0f) )    // ClientWeapon.cpp
// so a large starting value costs mantissa bits first and then overflows int32
// outright. This file used to `#define time_GetMSTime timeGetTime` with
// timeGetTime() returning UNIX EPOCH milliseconds — ~1.78e12, truncated to
// 2.3e9 as a uint32 — which made every weapon-fire timestamp saturate to
// INT32_MAX and jammed CWeapon::Fire's rate limiter. See PHASE2_HANDOFF §16.

static uint32 g_TimeBase = 0;
static bool   g_bTimeBaseSet = false;

// Milliseconds since boot — the Win32 GetTickCount/timeGetTime semantic.
// CLOCK_MONOTONIC rather than gettimeofday so the clock cannot be stepped
// backwards by NTP; the compat windows.h GetTickCount is implemented
// identically, so it does not matter which of the two a translation unit
// happens to bind to.
uint32 timeGetTime()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

// Milliseconds since the first call — i.e. since engine startup. Based lazily
// rather than from a static constructor so it cannot depend on static-init
// order.
uint32 time_GetMSTime()
{
	uint32 nNow = timeGetTime();

	if (!g_bTimeBaseSet)
	{
		g_bTimeBaseSet = true;
		g_TimeBase = nNow;
	}

	return nNow - g_TimeBase;
}

float time_GetTime()
{
	return (float)time_GetMSTime() * (1.0f / 1000.0f);
}
