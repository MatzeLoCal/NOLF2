#ifndef __TIMEMGR_H__
#define __TIMEMGR_H__

#ifndef __LTINTEGER_H__
#include "ltinteger.h"
#endif

// Note: we're assuming the timer doesn't need to be initialized .. if it does,
// it should be initialized in system-dependent code.
float	time_GetTime();		// seconds since engine startup
uint32  timeGetTime();		// milliseconds since boot (Win32 GetTickCount semantics)
uint32	time_GetMSTime();	// milliseconds since engine startup

// NOTE: time_GetMSTime was a `#define time_GetMSTime timeGetTime`, which
// dropped the base subtraction that sys/win/timemgr.cpp does deliberately.
// The engine treats time_GetMSTime's result as a SMALL number: it divides it
// into a float (CClientMgr::m_CurTime) and casts it to int32
// (CServerMgr::Update). Do not collapse these two functions again.

#endif  // __TIMEMGR_H__

