// ----------------------------------------------------------------------- //
// macos_window.mm C ABI — the engine's render/display hookup talks to the
// Cocoa+OpenGL window only through these functions (no Objective-C leaks out).
// ----------------------------------------------------------------------- //
#ifndef __LTMACWINDOW_H__
#define __LTMACWINDOW_H__

#ifdef __cplusplus
extern "C" {
#endif

// Per-frame draw callback the GL view invokes (the engine's render entry).
typedef void (*LTMacWin_DrawFn)(void* user);

bool  LTMacWin_Create(const char* pTitle, int width, int height, bool fullscreen);
void  LTMacWin_SetDrawCallback(LTMacWin_DrawFn fn, void* user);
void  LTMacWin_PumpEvents(void);      // non-blocking Cocoa event pump
void  LTMacWin_SwapBuffers(void);
void  LTMacWin_MakeCurrent(void);
bool  LTMacWin_ShouldClose(void);
// Ask the main loop to exit, exactly as the red close button does. This is the
// macOS stand-in for Win32's PostQuitMessage(0): the game's menu "Quit" reaches
// the engine as g_pLTClient->Shutdown() -> ci_Shutdown -> dsi_OnClientShutdown,
// and on Win32 that PostQuitMessage is the only thing that breaks the message
// loop. Without this the shutdown request was simply swallowed.
void  LTMacWin_RequestClose(void);
void  LTMacWin_GetSize(int* w, int* h);
void* LTMacWin_GetNSWindow(void);     // opaque NSWindow* (for dsi_GetMainWindow)
void  LTMacWin_SetTitle(const char* pTitle);   // window title (FPS counter etc.)

// Mouse capture ("pointer lock"). While captured the hardware cursor is hidden
// and decoupled from the mouse, so the pointer can never leave the window, hit a
// screen edge (which would silently stop the look deltas) or click another app.
// Driven by the engine's cursor mode: CM_None (gameplay) captures, CM_Hardware
// (menus) releases -- see CLTCursor::PreSetMode. Capture is dropped
// automatically while the app is not frontmost so Cmd-Tab always frees the
// mouse, and restored on reactivation.
void  LTMacWin_SetMouseCapture(bool bCapture);
// Hardware-cursor visibility, driven by the engine's cursor MODE. Separate
// from capture: the game hides the pointer in menus too (it draws its own
// cursor sprite there) but only LOCKS it during gameplay.
void  LTMacWin_SetCursorVisible(bool bVisible);

// The game's hooked window procedure. NOLF2 gets ALL its mouse input through a
// WNDPROC it installs itself (CGameClientShell::HookWindow) -- IClientShell has
// no mouse callbacks. With no Win32 window there is nothing to hook, so the game
// stores its proc in this slot (reached via the "MacWndProcSlot" engine hook)
// and the Cocoa pump synthesizes WM_MOUSEMOVE / WM_?BUTTON* into it.
typedef long (*LTMacWndProc)(void* hWnd, unsigned int uMsg,
                             unsigned long wParam, long lParam);
LTMacWndProc* LTMacWin_GetWndProcSlot(void);
// Implemented engine-side (macos_client.cpp): the g_CV_CursorCenter console
// variable, which is how the game asks for a locked mouse.
bool  LTMacWin_EngineWantsCursorLock(void);
bool  LTMacWin_IsMouseCaptured(void);

void  LTMacWin_Destroy(void);

#ifdef __cplusplus
}
#endif

#endif // __LTMACWINDOW_H__
