// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_world.h
//
// PURPOSE : The Metal BSP-world draw — pass 2 of the GL → Metal conversion.
//
//           ⚠️ THIS FILE DOES NOT PARSE ANYTHING. The world loader, the
//           lightmap RLE decompression, the light-group composition and the
//           world-model / sky / translucency walk all stay in world_renderdata.cpp
//           and are shared: `mtl_world.mm` only turns one parsed RWorld into
//           Metal draw calls. That logic is years of hard-won correctness
//           (§4, §36, §43, §49) and duplicating it would be the fastest way to
//           lose it.
//
// ----------------------------------------------------------------------- //
#ifndef __MTL_WORLD_H__
#define __MTL_WORLD_H__

#include <stdint.h>

struct RWorld;
struct RWDrawParams;

// --- the lightmap backend behind RWorldLM_* (world_renderdata.h) -------------
// nPadW/nPadH are the pow2 padded dimensions. ⚠️ The padding is NOT cosmetic:
// UV1 in the world file is already relative to the padded size because D3D
// never rescaled it, so a tightly-sized texture shifts every lightmap.
uintptr_t MTLWorld_CreateLightmap(const uint8_t *pBGR, uint32_t nWidth, uint32_t nHeight,
                                  uint32_t nPadW, uint32_t nPadH);
void      MTLWorld_UpdateLightmap(uintptr_t hTex, const uint8_t *pBGR,
                                  uint32_t nWidth, uint32_t nHeight);
void      MTLWorld_DestroyLightmap(uintptr_t hTex);

// ⚠️ Call this whenever the CPU recomposes a block's per-vertex colours --
// i.e. from RWorld_SetLightGroupColor, NOT from the lightmap upload. Under GL
// the recomposed colours are handed to glColor3f every frame, so nothing has to
// be told; Metal holds them in a per-block buffer that must be re-uploaded. A
// light group with no lightmapped section never reaches MTLWorld_UpdateLightmap
// at all, which is exactly how the Gouraud half went stale.
void      MTLWorld_InvalidateVertexColors(void);

// --- the draw -------------------------------------------------------------
// The view/projection for this frame, published by nr_RenderScene. Both are
// column-major; the projection must already be Metal's 0..1 depth range.
void MTLWorld_SetSceneTransform(const float *pView16, const float *pProj16);

// Sets the fog the shader applies. bEnable false leaves geometry unfogged.
void MTLWorld_SetFog(bool bEnable, float fR, float fG, float fB,
                     float fNearZ, float fFarZ);

// ⚠️ Read by the MODEL pass too. Under GL, glEnable(GL_FOG) is global state:
// it fogs models, sprites and particles exactly as it fogs the world, until
// RWorld_DisableFog(). A Metal shader has to be told, so every pass that draws
// inside the scene reads the same numbers from here rather than keeping its own.
// pColor4 receives rgb + enable.
void MTLWorld_GetFog(float *pColor4, float *pNearZ, float *pFarZ);

// Draws one parsed world with the given state. Vertex/index buffers and
// lightmap textures are built on first use and cached on the block.
void MTLWorld_DrawWorld(const RWorld *pWorld, const RWDrawParams *pParams);

// Frees every GPU resource built for the loaded world (called from RWorld_Free
// and nr_Term). Safe to call twice.
void MTLWorld_Free(void);

#endif // __MTL_WORLD_H__
