// macOS compat: windowsx.h
//
// The real windowsx.h is a bag of GDI/USER convenience macros (Edit_*,
// ListBox_*, HANDLE_MSG, GET_X/Y_LPARAM, and a DeleteFont() macro that the
// game code actively works around). Most of it doesn't apply to the Cocoa/GL
// client — but HANDLE_MSG does, and it has to be REAL.
//
// ⚠️ WHY THIS IS NOT A NO-OP ANY MORE:
// NOLF2 receives ALL mouse input through a window procedure it installs itself
// (CGameClientShell's HookedWindowProc, installed by HookWindow()). IClientShell
// has no mouse callbacks at all — WM_MOUSEMOVE is what drives
// CInterfaceMgr::m_CursorPos (the game's OWN cursor sprite) and WM_LBUTTONDOWN
// is what clicks menu items. While HANDLE_MSG expanded to `((void)0)` the
// game's cursor sat pinned at (0,0) and the menus could not be clicked at all.
// The macOS event pump now synthesizes these messages into the game's hooked
// proc (see LTMacWin_PumpEvents and the "MacWndProcSlot" engine hook), so the
// cracking macros below have to dispatch for real.
#ifndef __WINDOWSX_H_COMPAT__
#define __WINDOWSX_H_COMPAT__

// LPARAM packs the cursor position as two signed 16-bit halves.
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp)  ((int)(short)((unsigned long)(lp) & 0xFFFF))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp)  ((int)(short)(((unsigned long)(lp) >> 16) & 0xFFFF))
#endif

// Message crackers — signatures match the Win32 originals, because the game's
// handlers are declared against them, e.g.
//   void OnMouseMove  (HWND, int x, int y, UINT keyFlags)
//   void OnLButtonDown(HWND, BOOL fDoubleClick, int x, int y, UINT keyFlags)
#define HANDLE_WM_MOUSEMOVE(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)

#define HANDLE_WM_LBUTTONDOWN(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), FALSE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_LBUTTONDBLCLK(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), TRUE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_LBUTTONUP(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)

#define HANDLE_WM_RBUTTONDOWN(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), FALSE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_RBUTTONDBLCLK(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), TRUE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)
#define HANDLE_WM_RBUTTONUP(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), (UINT)(wParam)), 0L)

#define HANDLE_WM_CHAR(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), (TCHAR)(wParam), (int)(short)((unsigned long)(lParam) & 0xFFFF)), 0L)

#define HANDLE_WM_SETCURSOR(hwnd, wParam, lParam, fn) \
    (LRESULT)(BOOL)(fn)((hwnd), (HWND)(wParam), \
        (UINT)((unsigned long)(lParam) & 0xFFFF), \
        (UINT)(((unsigned long)(lParam) >> 16) & 0xFFFF))

#ifndef HANDLE_MSG
#define HANDLE_MSG(hwnd, message, fn) \
    case (message): return HANDLE_##message((hwnd), wParam, lParam, (fn));
#endif

#endif
