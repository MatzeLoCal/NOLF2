// ----------------------------------------------------------------------- //
//
// MODULE  : macos_input.h
//
// PURPOSE : Cocoa (NSEvent) keyboard + mouse capture for the macOS port —
//           the replacement for the Win32 DirectInput layer.
//
//           Two consumers, two representations of the same keystrokes:
//
//           1. The UI/menu path. The engine drains a key-event QUEUE
//              (dsi_NumKeyDowns/GetKeyDown/...) and hands it to the client
//              shell as OnKeyDown/OnKeyUp. Those want **Windows VK_* codes**.
//
//           2. The gameplay path. InputMgr::ReadInput samples persistent key
//              STATE and turns it into engine "actions" through the bindings
//              in autoexec.cfg, which name their triggers "##<n>" where n is
//              a **PC DIK scancode**.
//
//           So every key is translated to both, once, here.
//
// ----------------------------------------------------------------------- //
#ifndef __MACOS_INPUT_H__
#define __MACOS_INPUT_H__

#ifdef __cplusplus
extern "C" {
#endif

// Called by LTMacWin_PumpEvents for every NSEvent (opaque here to keep this
// header C-callable). Returns true if the event was consumed as input.
bool LTMacInput_HandleEvent(void *pNSEvent);

// --- UI/menu key queue (VK codes) ---------------------------------------
unsigned int LTMacInput_NumKeyDowns(void);
unsigned int LTMacInput_GetKeyDown(unsigned int i);
unsigned int LTMacInput_GetKeyDownRep(unsigned int i);
unsigned int LTMacInput_NumKeyUps(void);
unsigned int LTMacInput_GetKeyUp(unsigned int i);
void         LTMacInput_ClearKeyDowns(void);
void         LTMacInput_ClearKeyUps(void);

// --- Gameplay state (DIK scancodes) -------------------------------------
// Persistent "is it held right now" state, sampled by InputMgr::ReadInput.
bool LTMacInput_IsDIKDown(unsigned int nDIK);

// Mouse motion accumulated since the last call (consuming read), in pixels.
void LTMacInput_ConsumeMouseDelta(float *pDX, float *pDY);
// Drop any accumulated motion (used when the cursor is warped on capture --
// the warp itself shows up as one huge bogus delta).
void LTMacInput_ClearMouseDelta(void);
// Mouse buttons: 0 = left, 1 = right, 2 = middle.
bool LTMacInput_IsMouseButtonDown(unsigned int nButton);

// Env-gated synthetic keystrokes for headless testing (LT_TEST_KEYS).
// Call once per frame from the event pump.
void LTMacInput_TickInjection(void);
void LTMacInput_TickHold(void);   // LT_TEST_HOLD_DIK

// Drop all held keys/buttons (focus loss, ClearInput).
void LTMacInput_ClearAll(void);

#ifdef __cplusplus
}
#endif

#endif // __MACOS_INPUT_H__
