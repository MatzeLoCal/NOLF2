// macOS stub for the DirectX error-string helper header.
#ifndef __LT_COMPAT_DXERR_H__
#define __LT_COMPAT_DXERR_H__
#include <windows.h>
static inline const char* DXGetErrorStringA(HRESULT)      { return ""; }
static inline const char* DXGetErrorDescriptionA(HRESULT) { return ""; }
#define DXGetErrorString      DXGetErrorStringA
#define DXGetErrorDescription DXGetErrorDescriptionA
#define DXGetErrorString9     DXGetErrorStringA
#endif
