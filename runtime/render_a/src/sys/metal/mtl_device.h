// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_device.h
//
// PURPOSE : Metal device, layer and per-frame lifecycle for the macOS port.
//           This is the bottom of the Metal renderer: everything above it
//           (2D drawprim, world, models) encodes into the render command
//           encoder this file hands out.
//
//           Replaces the OpenGL context that macos_window.mm used to own. The
//           window now hosts a CAMetalLayer and knows nothing else about the
//           renderer -- LTMacWin_SwapBuffers/MakeCurrent are gone from the
//           frame path, because Metal's frame boundary is explicit here.
//
// ----------------------------------------------------------------------- //
#ifndef __MTL_DEVICE_H__
#define __MTL_DEVICE_H__

#include <stdint.h>

// Opaque to C++ callers; the .mm side casts to the real Objective-C types.
// Keeping the header ObjC-free means the C++ render modules can include it.
typedef void* MTLDeviceRef;
typedef void* MTLEncoderRef;

// ★ WHICH BACKEND THIS RUN IS USING. The window is built Metal-backed unless
// LT_RENDER_GL=1, and LTMacWin_GetMetalLayer() returns NULL under the GL view --
// so this is a fact about the window, not a second copy of the env-var rule.
// Every renderer entry point that has both a GL and a Metal path branches on it.
bool  MTLDev_IsMetalBackend(void);

// One-time setup against the window's CAMetalLayer. Safe to call twice.
bool  MTLDev_Init(void);
void  MTLDev_Term(void);
bool  MTLDev_IsReady(void);

// Frame lifecycle. BeginFrame acquires a drawable and opens a render command
// encoder with the load action already applied; EndFrame ends encoding and
// presents. BeginFrame returns false when no drawable is available (occluded
// window, resize in flight) -- callers must skip the frame, not draw anyway.
bool  MTLDev_BeginFrame(void);
void  MTLDev_EndFrame(void);

// ★ THE ENGINE DOES NOT ANNOUNCE THE START OF A FRAME. It clears, renders a
// scene, draws 2D and presents -- any of which can be the first thing that
// touches the drawable, and in the menus the first thing is a clear while in
// gameplay it is the scene. So the frame is opened LAZILY: every draw path
// calls this first and skips its work when it returns false.
bool  MTLDev_EnsureFrame(void);
bool  MTLDev_FrameActive(void);

// glClear's equivalent. Outside a frame this just opens one (the load action
// does the clearing, which is free); inside one it ends the current encoder and
// opens a new pass with Clear load actions on the requested attachments. The
// engine clears mid-frame -- CInterfaceMgr::Update does it before every menu --
// so both cases are real.
void  MTLDev_Clear(bool bColor, bool bDepth, float r, float g, float b, float a);

// Viewport in PIXELS, top-left origin (Metal's own convention, and the
// engine's -- unlike GL, no y flip is needed).
void  MTLDev_SetViewport(int x, int y, int w, int h);

// glDepthRange's equivalent -- in Metal it is part of the VIEWPORT, so this
// re-applies the stored rect with a new near/far. The player-view pass squeezes
// the weapon into the front tenth of the depth buffer with it (D3D sets the
// viewport MinZ/MaxZ to 0..0.1 for exactly that).
void  MTLDev_SetDepthRange(float fNear, float fFar);

// The encoder for the frame in flight, or NULL outside Begin/EndFrame.
// ⚠️ Re-fetch it after every MTLDev_Clear: that opens a NEW encoder and the
// old one is finished.
MTLEncoderRef MTLDev_Encoder(void);
MTLDeviceRef  MTLDev_Device(void);

// Ring slot for per-frame CPU-written resources (the drawprim vertex ring).
// Stable for the whole frame; MTLDev_BeginFrame will not hand out a slot whose
// GPU work is still running, so a buffer indexed by it is safe to overwrite.
enum { MTLDEV_FRAMES_IN_FLIGHT = 3 };
int   MTLDev_FrameSlot(void);

// ★ LT_TRACE_MTLFRAME=<n> logs the ORDER of frame operations (begin / clear /
// draw / end) for frames n..n+2 and nothing else. A renderer that composes a
// frame out of several passes can lose work to a clear it did not expect, and
// the only way to see that is the sequence, not a per-call census.
bool  MTLDev_TraceFrame(void);


// Clear colour applied by BeginFrame's load action (0..1).
void  MTLDev_SetClearColor(float r, float g, float b, float a);

// Drawable size in PIXELS (not points) -- the viewport the engine draws into.
void  MTLDev_GetDrawableSize(int* w, int* h);

// ★ The verification instrument. Reads the frame just presented back into a
// caller-owned RGB buffer (w*h*3, top-down). The port's whole method rests on
// being able to diff a frame against a known-good one, so this exists from the
// first commit rather than being retrofitted.
bool  MTLDev_ReadbackFrame(uint8_t* pRGB, int* pW, int* pH);

// Window-space depth (0..1) of the last presented frame, for A/B against GL's
// glReadPixels(GL_DEPTH_COMPONENT). Requires LT_DUMP_DEPTH=1 at startup, which
// is what makes the depth texture CPU-readable. pOut takes w*h floats.
bool  MTLDev_ReadbackDepth(float* pOut, int* pW, int* pH);

#endif // __MTL_DEVICE_H__
