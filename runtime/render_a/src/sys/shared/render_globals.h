// ----------------------------------------------------------------------- //
//
// MODULE  : render_globals.h
//
// PURPOSE : Backend-neutral renderer globals — the handful of things BOTH the
//           GL and Metal paths need, owned by neither.
//
//           ★ WHY THIS EXISTS: `g_pGLStruct` was DEFINED in gl_texture.cpp but
//           ASSIGNED in nullrender.cpp (the shared dispatcher, from
//           rdll_RenderDLLSetup) and read by the Metal modules, which each
//           carried their OWN `extern` for it — three declarations of one
//           symbol, and the shared layer depending on a GL translation unit for
//           storage. That is exactly backwards, and it is why mtl_world.mm,
//           mtl_model.mm and mtl_drawprim.mm all had to include a GL header.
//
//           Renamed `g_pRenderStruct` because it never had anything to do with
//           GL: it is the ENGINE's function table (GetSharedTexture, GetTexture,
//           …), the renderer's handle on engine-side services.
//
// ----------------------------------------------------------------------- //
#ifndef __RENDER_GLOBALS_H__
#define __RENDER_GLOBALS_H__

struct RenderStruct;

// The engine-side RenderStruct (function table for GetSharedTexture/GetTexture
// etc.). Set by rdll_RenderDLLSetup before anything can bind or draw.
extern RenderStruct *g_pRenderStruct;

#endif  // __RENDER_GLOBALS_H__
