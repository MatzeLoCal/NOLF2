// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_texture.h
//
// PURPOSE : DTX -> MTLTexture. The Metal counterpart of gl_texture.{h,cpp},
//           cached on SharedTexture::m_pRenderData exactly as the GL one is.
//
//           ⚠️ THE TWO SHARE THAT ONE POINTER SLOT, so exactly one of them may
//           be live in a run. That is enforced upstream: nr_BindTexture and
//           nr_UnbindTexture dispatch on MTLDev_IsMetalBackend(), and every
//           other GLTex_* caller is a gl_* draw module which does not run under
//           Metal. Do not add a GLTex_* call to a path that runs on both.
//
// ----------------------------------------------------------------------- //
#ifndef __MTL_TEXTURE_H__
#define __MTL_TEXTURE_H__

class SharedTexture;
struct RenderStruct;

// The engine-side RenderStruct, from the backend-neutral header. Metal used to
// carry its own extern for a symbol gl_texture.cpp defined -- one pointer with
// three declarations and its storage in a GL translation unit.
#include "sys/shared/render_globals.h"

// RenderStruct::BindTexture / UnbindTexture, Metal side.
void MTLTex_Bind(SharedTexture *pTexture, bool bTextureChanged);
void MTLTex_Unbind(SharedTexture *pTexture);

// The MTLTexture for drawing, as an UNRETAINED id<MTLTexture> in a void*
// (the header stays Objective-C-free so the C++ render modules can include it).
// NULL = none/failed. Creates it on first use if the eager bind did not.
void *MTLTex_Get(SharedTexture *pTexture);

// Sampler state for this texture: cube maps clamp on all axes, 2D repeats.
// Also an unretained id<MTLSamplerState>.
void *MTLTex_Sampler(SharedTexture *pTexture);

// Base (mip 0) dimensions, like GLTex_GetDims.
bool MTLTex_GetDims(SharedTexture *pTexture, uint32 &nWidth, uint32 &nHeight);

// Authored alpha-test reference from the DTX command string's "AlphaRef <n>".
// 0 = ALPHAREF_NONE = this surface must NOT be alpha-tested. ⚠️ Metal has no
// alpha test at all, so this is what the fragment shader's discard is driven
// by -- see §80 for what a wrong reference does to foliage.
unsigned int MTLTex_GetAlphaRef(SharedTexture *pTexture);

// DTX_CUBEMAP -- decides the env-map texture transform (see GLTex_IsCubeMap).
bool MTLTex_IsCubeMap(SharedTexture *pTexture);

// DTX_FULLBRITE. Read ONLY for a dual-texture section's slot-1 texture, where
// it selects ADD over CROSS-FADE (d3d_rendershader_gouraud.cpp).
bool MTLTex_IsFullbrite(SharedTexture *pTexture);

// Authored DETAIL-texture placement from this BASE texture's DTX header Extra
// bytes (scale + rotation as cos/sin). Mirrors GLTex_GetDetailParams.
bool MTLTex_GetDetailParams(SharedTexture *pTexture, float &fScale,
                            float &fCos, float &fSin);

// The texture's FILE name, for diagnostics.
const char *MTLTex_GetTexName(SharedTexture *pTexture);

#endif // __MTL_TEXTURE_H__
