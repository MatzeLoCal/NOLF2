// ----------------------------------------------------------------------- //
//
// MODULE  : render_texture.h
//
// PURPOSE : The BACKEND-NEUTRAL texture queries. They used to live in
//           gl_texture.h -- a shared path including a GL header to ask a
//           neutral question is how the confusion below started -- and then
//           dispatched on whichever renderer was live. GL is gone, so they now
//           forward to Metal; the NAMES are the point (see below).
//
// ----------------------------------------------------------------------- //
#ifndef __RENDER_TEXTURE_H__
#define __RENDER_TEXTURE_H__

#include "ltbasedefs.h"

class SharedTexture;

// ★★ BACKEND-NEUTRAL TEXTURE QUERIES — USE THESE FROM ANY SHARED PATH.
//
// ⚠️ THE HISTORY IS THE REASON THESE NAMES ARE WORTH KEEPING. While both
// backends were alive, SharedTexture::m_pRenderData held a GLTexEntry under GL
// and an MTLTexEntry under Metal, so a GLTex_* call from shared code
// reinterpreted one struct as the other and returned GARBAGE — a non-zero
// "texture name" that passed an `if (nName)` test, and dimensions of 0 because
// the GL query needed a context that did not exist. That is exactly how the
// sprite pass silently drew nothing (its guard was `if (!nName ||
// !GLTex_GetDims(...)) return false;`), and it happened FIVE times in all.
//
// GL is deleted, so the trap itself is gone — but the shared parse/walk/scene
// modules still ask their texture questions here rather than naming a backend,
// which is what keeps them portable if a second backend ever returns.
// ---------------------------------------------------------------------------
bool         RTex_IsValid(SharedTexture *pTexture);
bool         RTex_IsCubeMap(SharedTexture *pTexture);
bool         RTex_GetDetailParams(SharedTexture *pTexture, float &fScale,
                                  float &fCos, float &fSin);
bool         RTex_GetDims(SharedTexture *pTexture, uint32 &nWidth, uint32 &nHeight);
unsigned int RTex_GetAlphaRef(SharedTexture *pTexture);

// The texture's FILE name, for diagnostics — identifying which texture an
// effect uses is otherwise guesswork from world positions. Asks the engine
// through the RenderStruct, so it never touched m_pRenderData and was neutral
// all along (it lived in gl_texture.cpp as gltex_Name).
const char  *RTex_GetName(SharedTexture *pTexture);

#endif  // __RENDER_TEXTURE_H__
