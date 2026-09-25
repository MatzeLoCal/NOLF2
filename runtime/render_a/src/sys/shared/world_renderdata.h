// ----------------------------------------------------------------------- //
//
// MODULE  : world_renderdata.h
//
// PURPOSE : THE BSP WORLD'S RENDER DATA, BACKEND-NEUTRAL. Loads the world
//           file's render section (the same binary blob CD3D_RenderBlock
//           consumes) into CPU-side arrays, owns the lightmaps, the dynamic
//           lights, the sky list, the fog state and the second-layer
//           (env-map/detail) resolution, and walks the world to produce draw
//           parameters. Metal consumes what this produces; it contains no
//           backend calls of its own.
//
//           ⚠️ This was gl_worlddata.{h,cpp} and is where the GL renderer's
//           parse/walk code survived its deletion (§108) -- the GL emit came
//           out, everything that decided WHAT to draw stayed.
//
// ----------------------------------------------------------------------- //
#ifndef __WORLD_RENDERDATA_H__
#define __WORLD_RENDERDATA_H__

#include "ltbasedefs.h"
#include <vector>
#include <string>

class ILTStream;
class SharedTexture;

// ---------------------------------------------------------------------------
// ★ THE PARSED WORLD IS BACKEND-NEUTRAL AND IS SHARED WITH THE METAL RENDERER.
//
// Everything below the parse — the stream walk, lightmap RLE decompression,
// light-group composition, the world-model/sky/translucency walk — is renderer
// independent and took a long time to get exactly right (§4, §36, §43, §49).
// The GL→Metal conversion therefore does NOT duplicate it: `mtl_world.mm`
// walks these same structures and only supplies the per-section draw.
//
// ⚠️ When OpenGL is finally deleted, this file and its .cpp become the
// backend-neutral `world_renderdata.{h,cpp}` — the loader stays, the GL draw
// goes. Do not "tidy" the structs into the .cpp again.
// ---------------------------------------------------------------------------

// The retail 44-byte world vertex, exactly as the file stores it (§4).
struct RWVertex
{
	LTVector m_vPos;
	float    m_fU0, m_fV0;
	float    m_fU1, m_fV1;
	uint32   m_nColor;      // D3D ARGB
	LTVector m_vNormal;
};

struct RWSection
{
	uint8  m_nShaderCode;
	uint32 m_nTriCount;
	uint32 m_nStartIndex;   // first index (== triangle start * 3)
	SharedTexture *m_pTexture;   // base texture (slot 0); NULL = untextured
	// Static lightmap, 0 = none; UV1, clamped. ⚠️ An OPAQUE BACKEND HANDLE:
	// an index into the Metal lightmap table (it was a GLuint texture name
	// back when GL owned this). Created/updated/destroyed only through RWorldLM_*.
	uintptr_t m_hLMTexture;
	std::string m_sTexName; // slot-0 name, kept for the LT_TRACE_NOTEX census

	// ★★ SLOT 1 — THE DUAL-TEXTURE LAYER (shader codes 8 and 9).
	//
	// A second BASE texture, sampled with the vertex's OWN SECOND UV SET, and
	// cross-faded against slot 0 by the PER-VERTEX ALPHA. This is how the game
	// paints a dirt path fading into snow, or moss onto stone: one section,
	// two textures, a soft authored transition. Without it the section draws
	// slot 0 alone at full opacity and the transition becomes a hard polygon
	// edge -- which is exactly how Siberia's paths looked before this landed.
	//
	// ⚠️ NOT the same thing as the §59/§60 "second layer" (env map / detail),
	// which is resolved through RWorld_ResolveSecondLayer from the texture's
	// authored LINKS and takes DERIVED coordinates. These are different
	// features and D3D implements them as different shader classes; a dual
	// section never carries env/detail as well.
	SharedTexture *m_pTexture1;
	std::string m_sTexName1;   // slot-1 name, for the LT_TRACE_DUAL census
};

// A decompressed 24-bit lightmap kept on the CPU: used both as the parse-time
// scratch and as each section's retained BASE (pre-light-group) lightmap so
// switchable light groups can be recomposed at runtime.
struct RWPendingLM
{
	uint32 m_nWidth, m_nHeight;
	std::vector<uint8> m_aData;   // empty = section has no lightmap

	RWPendingLM() : m_nWidth(0), m_nHeight(0) {}
};

// One light group's RLE contribution to one section's lightmap.
struct RWSubLM
{
	uint32 m_nSection;
	uint32 m_nLeft, m_nTop, m_nWidth, m_nHeight;
	std::vector<uint8> m_aData;   // RLE intensity stream (0xFF = run escape)
};

// A named light group in a block: current color + its sub-lightmaps.
// m_nID is the 31-polynomial hash of the group name — identical to
// SRBLightGroup's operator>> so the engine's SetLightGroupColor IDs match.
struct RWLightGroup
{
	uint32   m_nID;
	LTVector m_vColor;    // CURRENT color; starts at the authored default
	std::vector<RWSubLM> m_aSubLMs;
	// RLE per-vertex intensity stream over the WHOLE block's vertex array.
	std::vector<uint8> m_aVertexIntensities;
};

struct RWBlock
{
	LTVector m_vCenter, m_vHalfDims;
	std::vector<RWSection> m_aSections;
	std::vector<RWVertex>  m_aVertices;
	std::vector<uint32>     m_aIndices;
	// Retained for runtime light-group recomposition (switchable lights).
	std::vector<RWPendingLM>  m_aBaseLMs;      // per section; may be empty
	std::vector<RWLightGroup> m_aLightGroups;
	// Composed per-vertex color = baked m_nColor + sum over light groups of
	// (group color x RLE intensity), clamped -- CD3D_RenderBlock::
	// UpdateLightingData's exact math. RGB triplets.
	std::vector<uint8>         m_aComposedColor;
	// ★★ THE PER-VERTEX DIFFUSE ALPHA, i.e. the TOP byte of m_nColor.
	//
	// This is where a WorldModel's authored `Alpha` property ends up: the level
	// PRE-PROCESSOR bakes it into the vertex colours (WorldModel.cpp:70 — "DO
	// NOT REMOVE THIS!!!! Pre-Processor looks at this value" — nothing reads
	// that property at runtime). D3D's textured-gouraud shader consumes it as
	// D3DTA_DIFFUSE under ALPHAOP=MODULATE(TEXTURE, DIFFUSE), which is how
	// Siberia's window glass is see-through even though GlUW002.dtx is a fully
	// opaque DXT1 with no alpha channel at all.
	//
	// Kept as its OWN array rather than widening m_aComposedColor to RGBA: the
	// light groups below accumulate into RGB only, and alpha is never touched
	// after load, so a parallel array keeps that math untouched.
	// Empty == every vertex is 255 (the common case, no allocation).
	std::vector<uint8>         m_aComposedAlpha;
	// Opaque per-block handle for a backend that keeps GPU buffers per block
	// (Metal: index into its vertex/index buffer table). 0 = not built yet.
	uintptr_t                  m_hGPU;

	RWBlock() : m_hGPU(0) {}
};

struct RWorld
{
	std::vector<RWBlock> m_aBlocks;
	char m_sName[64 + 1];   // world-model name (MAX_WORLDNAME_LEN); "" = main world
	// True if ANY block authored a vertex alpha below 255. Drives the depth
	// decision for this world model — see RWorld_DrawWorldModels.
	bool m_bHasVertexAlpha;

	RWorld() : m_bHasVertexAlpha(false) { m_sName[0] = 0; }
};

// ---------------------------------------------------------------------------
// The lightmap-texture seam. The loader owns WHEN a lightmap exists; the
// backend owns WHAT it is. Implemented in world_renderdata.cpp, which dispatches to
// GL or to mtl_world.mm.
// ---------------------------------------------------------------------------
uintptr_t RWorldLM_Create(const uint8 *pBGR, uint32 nWidth, uint32 nHeight);
void      RWorldLM_Update(uintptr_t hTex, const uint8 *pBGR, uint32 nWidth, uint32 nHeight);
void      RWorldLM_Destroy(uintptr_t hTex);

// ---------------------------------------------------------------------------
// Per-draw state the shared walker resolves and the backend applies. Under GL
// these were ambient state set around the call (glEnable(GL_BLEND),
// glPushMatrix…); a Metal encoder has no such ambient state, so they became
// explicit parameters — which is also why the GL path now reads them from here
// rather than setting them at the call site.
// ---------------------------------------------------------------------------
enum ERWBlendMode
{
	kRWBlend_None = 0,
	kRWBlend_Alpha,        // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
	kRWBlend_Additive,     // SRC_ALPHA / ONE   (translucent world models)
	kRWBlend_AddOne        // ONE / ONE         (the dynamic-light pass)
};

// One dynamic light for the additive world pass. The position is in the space
// of the world being drawn (a world model gets it back-transformed first).
struct RWDynLightDesc
{
	LTVector m_vPos;
	float    m_fRadius;
	LTVector m_vColor;      // 0..1
};

struct RWDrawParams
{
	const float *m_pModelMatrix;   // column-major 4x4, NULL = identity
	uint8        m_nObjectAlpha;   // LTObject::m_ColorA, into every vertex colour
	bool         m_bAllowAlphaTest;
	bool         m_bDepthTest;
	bool         m_bDepthWrite;
	ERWBlendMode m_eBlend;
	// FLAG2_FORCETRANSLUCENT: discard only fully transparent texels, keep
	// depth writes on (§15 — the car wheels).
	bool         m_bDiscardZeroAlpha;
	// LEQUAL instead of LESS: the additive light pass draws over geometry
	// already in the depth buffer at exactly the same depth.
	bool         m_bDepthEqual;
	// Non-NULL = the ADDITIVE DYNAMIC-LIGHT pass. The shader then produces
	// `texture x (lightColour * attenuation)` per vertex instead of the usual
	// lighting, and blocks outside the light's radius are rejected.
	// ⚠️ It also alpha-tests with the AUTHORED AlphaRef regardless of
	// m_bAllowAlphaTest or LT_NO_ALPHATEST — §80: without the cutout this pass
	// lights the WHOLE foliage quad, which is the glowing-white-rectangles bug.
	const RWDynLightDesc *m_pDynLight;

	RWDrawParams()
		: m_pModelMatrix(0), m_nObjectAlpha(255), m_bAllowAlphaTest(true),
		  m_bDepthTest(true), m_bDepthWrite(true), m_eBlend(kRWBlend_None),
		  m_bDiscardZeroAlpha(false), m_bDepthEqual(false), m_pDynLight(0) {}
};

// Parse the full render section (render blocks + world models) from pStream.
// Returns false on a malformed stream (the caller treats that as a failed
// world load).
bool RWorld_Load(ILTStream *pStream);

void RWorld_Free();

bool RWorld_IsLoaded();

// Combined bounds of the main world's render blocks (false until loaded).
bool RWorld_GetBounds(LTVector &vCenter, LTVector &vHalfDims);

// Draw the main world's geometry (assumes the caller set up matrices).
void RWorld_Draw();

// Draw the visible world-model instances (doors etc.) with their engine
// transform (walks the client's OT_WORLDMODEL list). Sky objects (see below)
// are skipped — they belong to the sky pass.
//
// ⚠️ CALL THIS TWICE PER SCENE, WITH THE MODELS DRAWN IN BETWEEN — false for
// the solid set, then RModel_DrawModels(), then true for the translucent set.
// That is D3D's order (drawobjects.cpp:218); collapsing it lets a glass pane's
// depth write hide every character behind it.
void RWorld_DrawWorldModels(bool bTranslucentPass);

class LTObject;

// Sky pass: hand over this frame's sky-object list (SceneDesc::m_SkyObjects),
// then draw the sky world models (untransformed, depth disabled) with the sky
// camera matrices already set by the caller.
void RWorld_SetSkyObjects(LTObject **ppSkyObjects, int nCount);
void RWorld_DrawSkyWorldModels();

// --- World-surface ENVIRONMENT MAPPING ---
//
// The reflection is generated from the camera->world rotation, exactly as
// d3d_SetEnvMapTransform (3d_ops.cpp:103) builds it from
// ViewParams::m_mWorldEnvMap (= SetBasisVectors(Right, Up, Forward)). Push this
// frame's camera basis before drawing the world, or every reflected surface
// keeps the previous frame's reflection.
void RWorld_SetCamera(const LTVector &vRight, const LTVector &vUp,
                       const LTVector &vForward);

// One-line stats for logging.
void RWorld_GetStats(uint32 &nBlocks, uint32 &nVerts, uint32 &nTris, uint32 &nWorldModels);

// The parsed main world, for a backend draw module. NULL until loaded.
const RWorld *RWorld_GetMainWorld();

// ---------------------------------------------------------------------------
// ★★ THE AUTHORED SECOND TEXTURE LAYER — environment map (§59) or detail (§60).
//
// ⚠️ RESOLVE IT ONLY THROUGH RWorld_ResolveSecondLayer(). The authored texture
// type alone is NOT the answer: `rw_ResolveEnvMap` also applies the console
// variables (EnvMapEnable, EnvMapAdd, DetailTextures, DetailTextureAdd,
// DetailTextureScale, EnvScale) AND the env gates LT_NO_ENVMAP / LT_NO_DETAIL.
// Reading `m_eTexType` + `GetLinkedTexture()` directly skips all of that and
// re-creates §89's bug — a backend rendering something the GL path has switched
// off. §90 flagged these two gates as the live traps; this is the seam that
// makes them unreachable by accident.
// ---------------------------------------------------------------------------
enum ERWSecondLayerKind { kRWSecond_None = 0, kRWSecond_EnvMap, kRWSecond_Detail };
enum ERWSecondLayerMode { kRWSecondMode_Modulate = 0, kRWSecondMode_AddSigned,
                           kRWSecondMode_AlphaAdd };

struct RWSecondLayer
{
	SharedTexture      *m_pTex;      // the linked reflection / detail texture
	ERWSecondLayerKind m_eKind;
	ERWSecondLayerMode m_eMode;
	bool  m_bCube;                   // DTX_CUBEMAP: 3-coord reflection, cube target
	float m_fScale, m_fCos, m_fSin;  // detail placement
	float m_fEnvScale;               // env 2D: the -0.5/EnvScale factor

	RWSecondLayer()
		: m_pTex(0), m_eKind(kRWSecond_None), m_eMode(kRWSecondMode_Modulate),
		  m_bCube(false), m_fScale(1.0f), m_fCos(1.0f), m_fSin(0.0f),
		  m_fEnvScale(-0.5f) {}
};

bool RWorld_ResolveSecondLayer(const RWSection &cSection, bool bAuthoredLightmap,
                                RWSecondLayer *pOut);

// The camera->world rotation the reflection is generated with (columns R, U,
// -F), column-major 4x4. False until RWorld_SetCamera has run.
bool RWorld_GetEnvMatrix(float *pOut16);

// ★ RETAIL RENDERS THE WORLD AT 2x ("Saturate", §27) — read from the engine's
// own parameter, cached. Both backends need the same answer.
bool RWorld_SaturateOn();

// Runtime switchable light groups (RenderStruct::SetLightGroupColor): nID is
// the 31-polynomial hash of the group name (SRBLightGroup convention). Updates
// the group's color in every render block / world model that carries it and
// recomposes + re-uploads the affected lightmaps.
bool RWorld_SetLightGroupColor(uint32 nID, const LTVector &vColor);

// Test hook behind LT_TEST_LIGHTGROUPS_AT=<scene frame> (see the definition):
// switches every SUB-LIGHTMAP-FREE (pure Gouraud) group OFF MID-RUN, which is
// the only headless way to exercise the vertex-colour re-upload. Returns how many.
uint32 RWorld_TestSwitchGouraudLightGroupsOff(void);

// Additive dynamic-light pass over the main world (client OT_LIGHT objects +
// LT_TEST_LIGHTS env injections). Call after the opaque world/model draws with
// the scene transforms still current.
void RWorld_DrawDynamicLights();

// The LT_TEST_LIGHTS injection set, shared with the MODEL lighting pass (§71).
// The null shell drops the SFX messages that create real dynamic lights, so
// this is the only way to exercise either pass headlessly — and the model pass
// must see exactly the same lights the world pass does, or an A/B of the two is
// comparing different scenes. Colour is 0..255, the scale model lighting works
// in. Returns the count; *ppOut is valid until process exit.
struct RWTestDynLight
{
	LTVector m_vPos;
	float    m_fRadius;
	LTVector m_vColor255;
};
uint32 RWorld_GetTestDynLights(const RWTestDynLight **ppOut);

// --- Distance fog (fixed-function GL_FOG, mirrors the D3D render states) ---
//
// The authored source is the level's WorldProperties object, which pushes its
// FogEnable / FogColor / FogNearZ / FogFarZ (and the SkyFog* pair) into console
// variables at load; VolumeBrushes override them while the player is inside
// one. So these must be re-read every frame, NOT cached like Saturate.
//
// Call RWorld_ApplyFog(true) before the sky pass (uses SkyFogNearZ/FarZ, per
// d3d_drawsky.cpp) and RWorld_ApplyFog(false) before the world/model passes.
// Always RWorld_DisableFog() before any 2D/console drawing — D3D does the
// same in d3d_optimizedsurface.cpp.
void RWorld_ApplyFog(bool bSky);
void RWorld_DisableFog();

// True while fog is enabled this frame; the caller clears to the fog color so
// unreached distance blends into the haze instead of the void.
bool RWorld_GetFogColor(float &fR, float &fG, float &fB);

// Per-object fog for TRANSLUCENT draws (sprites, particles, polygrids) — the
// fog half of d3d_GetBlendStates (d3d_draw.h:128), which is the authority:
//   FLAG_FOGDISABLE  -> fog off for this object
//   FLAG2_ADDITIVE   -> fog colour BLACK (distance fades the addition out)
//   FLAG2_MULTIPLY   -> fog colour WHITE (distance fades the multiply to a no-op)
// Using the scene's fog colour for an additive sprite instead adds haze on top
// of the glow. Call RWorld_RestoreSceneFog() when the translucent pass ends.
void RWorld_ApplyObjectFog(uint32 nFlags, uint32 nFlags2);
void RWorld_RestoreSceneFog();

#endif // __WORLD_RENDERDATA_H__
