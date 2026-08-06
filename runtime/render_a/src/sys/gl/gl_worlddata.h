// ----------------------------------------------------------------------- //
//
// MODULE  : gl_worlddata.h
//
// PURPOSE : GL bring-up world renderer — loads the world file's render
//           section (the same binary blob CD3D_RenderBlock consumes) into
//           CPU-side arrays and draws it with fixed-function GL.
//
//           Phase-2 scope: main-world geometry, vertex colors, no textures/
//           lightmaps yet (both are parsed and skipped, stream-exact).
//
// ----------------------------------------------------------------------- //
#ifndef __GL_WORLDDATA_H__
#define __GL_WORLDDATA_H__

#include "ltbasedefs.h"

class ILTStream;

// Parse the full render section (render blocks + world models) from pStream.
// Returns false on a malformed stream (the caller treats that as a failed
// world load).
bool GLWorld_Load(ILTStream *pStream);

void GLWorld_Free();

bool GLWorld_IsLoaded();

// Combined bounds of the main world's render blocks (false until loaded).
bool GLWorld_GetBounds(LTVector &vCenter, LTVector &vHalfDims);

// Draw the main world's geometry (assumes the caller set up matrices).
void GLWorld_Draw();

// Draw the visible world-model instances (doors etc.) with their engine
// transform (walks the client's OT_WORLDMODEL list). Sky objects (see below)
// are skipped — they belong to the sky pass.
//
// ⚠️ CALL THIS TWICE PER SCENE, WITH THE MODELS DRAWN IN BETWEEN — false for
// the solid set, then GLModel_DrawModels(), then true for the translucent set.
// That is D3D's order (drawobjects.cpp:218); collapsing it lets a glass pane's
// depth write hide every character behind it.
void GLWorld_DrawWorldModels(bool bTranslucentPass);

class LTObject;

// Sky pass: hand over this frame's sky-object list (SceneDesc::m_SkyObjects),
// then draw the sky world models (untransformed, depth disabled) with the sky
// camera matrices already set by the caller.
void GLWorld_SetSkyObjects(LTObject **ppSkyObjects, int nCount);
void GLWorld_DrawSkyWorldModels();

// --- World-surface ENVIRONMENT MAPPING ---
//
// The reflection is generated from the camera->world rotation, exactly as
// d3d_SetEnvMapTransform (3d_ops.cpp:103) builds it from
// ViewParams::m_mWorldEnvMap (= SetBasisVectors(Right, Up, Forward)). Push this
// frame's camera basis before drawing the world, or every reflected surface
// keeps the previous frame's reflection.
void GLWorld_SetCamera(const LTVector &vRight, const LTVector &vUp,
                       const LTVector &vForward);

// One-line stats for logging.
void GLWorld_GetStats(uint32 &nBlocks, uint32 &nVerts, uint32 &nTris, uint32 &nWorldModels);

// Runtime switchable light groups (RenderStruct::SetLightGroupColor): nID is
// the 31-polynomial hash of the group name (SRBLightGroup convention). Updates
// the group's color in every render block / world model that carries it and
// recomposes + re-uploads the affected lightmaps.
bool GLWorld_SetLightGroupColor(uint32 nID, const LTVector &vColor);

// Additive dynamic-light pass over the main world (client OT_LIGHT objects +
// LT_TEST_LIGHTS env injections). Call after the opaque world/model draws with
// the scene transforms still current.
void GLWorld_DrawDynamicLights();

// The LT_TEST_LIGHTS injection set, shared with the MODEL lighting pass (§71).
// The null shell drops the SFX messages that create real dynamic lights, so
// this is the only way to exercise either pass headlessly — and the model pass
// must see exactly the same lights the world pass does, or an A/B of the two is
// comparing different scenes. Colour is 0..255, the scale model lighting works
// in. Returns the count; *ppOut is valid until process exit.
struct GLWTestDynLight
{
	LTVector m_vPos;
	float    m_fRadius;
	LTVector m_vColor255;
};
uint32 GLWorld_GetTestDynLights(const GLWTestDynLight **ppOut);

// --- Distance fog (fixed-function GL_FOG, mirrors the D3D render states) ---
//
// The authored source is the level's WorldProperties object, which pushes its
// FogEnable / FogColor / FogNearZ / FogFarZ (and the SkyFog* pair) into console
// variables at load; VolumeBrushes override them while the player is inside
// one. So these must be re-read every frame, NOT cached like Saturate.
//
// Call GLWorld_ApplyFog(true) before the sky pass (uses SkyFogNearZ/FarZ, per
// d3d_drawsky.cpp) and GLWorld_ApplyFog(false) before the world/model passes.
// Always GLWorld_DisableFog() before any 2D/console drawing — D3D does the
// same in d3d_optimizedsurface.cpp.
void GLWorld_ApplyFog(bool bSky);
void GLWorld_DisableFog();

// True while fog is enabled this frame; the caller clears to the fog color so
// unreached distance blends into the haze instead of the void.
bool GLWorld_GetFogColor(float &fR, float &fG, float &fB);

// Per-object fog for TRANSLUCENT draws (sprites, particles, polygrids) — the
// fog half of d3d_GetBlendStates (d3d_draw.h:128), which is the authority:
//   FLAG_FOGDISABLE  -> fog off for this object
//   FLAG2_ADDITIVE   -> fog colour BLACK (distance fades the addition out)
//   FLAG2_MULTIPLY   -> fog colour WHITE (distance fades the multiply to a no-op)
// Using the scene's fog colour for an additive sprite instead adds haze on top
// of the glow. Call GLWorld_RestoreSceneFog() when the translucent pass ends.
void GLWorld_ApplyObjectFog(uint32 nFlags, uint32 nFlags2);
void GLWorld_RestoreSceneFog();

#endif // __GL_WORLDDATA_H__
