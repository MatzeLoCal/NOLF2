// dxerr9.h - compatibility shim
// The DirectX 9 SDK header "dxerr9.h" was renamed to "DxErr.h" in the
// June 2010 DirectX SDK.  Map the old name and the "9"-suffixed function
// aliases to their modern equivalents.
#pragma once
#ifndef _DXERR9_H_
#define _DXERR9_H_

#include <DxErr.h>

#ifdef UNICODE
#  define DXGetErrorString9      DXGetErrorStringW
#  define DXGetErrorDescription9 DXGetErrorDescriptionW
#  define DXTrace9               DXTraceW
#else
#  define DXGetErrorString9      DXGetErrorStringA
#  define DXGetErrorDescription9 DXGetErrorDescriptionA
#  define DXTrace9               DXTraceA
#endif

#endif // _DXERR9_H_
