// ----------------------------------------------------------------------- //
//
// MODULE  : model_renderdata.h
//
// PURPOSE : MODEL RENDER DATA, BACKEND-NEUTRAL. Provides the render objects the
//           engine's ModelPiece::Load creates through
//           RenderStruct::CreateRenderObject (rigid + skeletal LTB meshes,
//           parsed into CPU arrays), and a per-frame draw that walks the
//           client's OT_MODEL instances, CPU-skins them with the engine's
//           animated node transforms, and draws them with their skins.
//
// ----------------------------------------------------------------------- //
#ifndef __MODEL_RENDERDATA_H__
#define __MODEL_RENDERDATA_H__

#include "sys/shared/render_state.h"   // kRBlend_* / kRAlpha_* (was GLenum)
#include "renderobject.h"

class SharedTexture;

// ---------------------------------------------------------------------------
// ★ THE AUTHORED RENDER STATE FOR ONE EMITTED MESH, resolved by the shared
// walk in model_renderdata.cpp and applied by whichever backend is live.
//
// Same split as the world pass (§85): the piece ordering, the CPU skinning, the
// four lighting terms (§68–§71) and the render-style resolution (§18/§35/§47)
// are renderer independent and stay in model_renderdata.cpp; only the emit is
// backend-specific. Under GL these were ambient state set around the emit; a
// Metal encoder has none, so they became an explicit description of the draw
// that BOTH backends read.
//
// ⚠️ The enums are GL's (GL_SRC_ALPHA, GL_GEQUAL, …) because that is what
// CRenderStyle already resolves to; mtl_model.mm maps them. Do not "modernise"
// them here — render_style.cpp is the authority and is shared.
// ---------------------------------------------------------------------------
struct REmitState
{
	SharedTexture *m_pTexture;      // NULL = untextured
	bool    m_bAlphaTest;
	uint32  m_nAlphaFunc;           // ERAlphaFunc (kRAlpha_*)
	float   m_fAlphaRef;            // 0..1
	bool    m_bBlend;
	uint32  m_nSrcBlend, m_nDstBlend;   // ERBlendFactor (kRBlend_*)
	bool    m_bZTest;
	bool    m_bZWrite;
	// The authored COLOUR pipeline (RRenderStyleState): does the vertex colour
	// touch the texture at all, and at what scale (MODULATE2X = 2).
	bool    m_bIgnoreDiffuse;
	float   m_fColorScale;
	// The authored ALPHA pipeline, which is a SEPARATE question: does the
	// fragment's alpha come from the TEXTURE or from the object (vertex) alpha?
	// Alpha in a NOLF2 skin is frequently an environment mask, not opacity —
	// letting that reach a SRC_ALPHA blend deletes the surface.
	bool    m_bTextureAlpha;

	// ★★ NoZ SPRITES (lamp halos) — HOW METAL REPRODUCES THE DEPTH PROBE.
	//
	// The GL path glReadPixels()es the DEPTH BUFFER at the sprite's centre and
	// skips the sprite when the world occludes that point, then draws it with
	// depth OFF so the halo still glows over its own lamp. That readback is not
	// available mid-pass in Metal (the depth attachment is private and still
	// being written), and doing it per sprite would mean a GPU sync each time.
	//
	// Instead the SAME TEST is done by the depth unit: the quad is drawn with
	// depth testing ON but with every vertex FLATTENED to the centre's NDC depth
	// minus the same bias the GL probe uses. A wall in front then fails the test
	// for the whole quad (what the probe did), while the lamp the halo sits on
	// is behind the bias and still passes (what depth-off did). No readback, and
	// it degrades per pixel rather than all-or-nothing.
	bool    m_bFlattenDepth;
	float   m_fFlatNDCz;     // centre depth in Metal NDC (0..1), bias applied

	// Optional model matrix, column-major, NULL = identity. Under GL this was
	// a glPushMatrix/glMultMatrixf around the emit; particle systems that are
	// NOT PS_WORLDSPACE keep their particles in object space and need it.
	const float *m_pModelMatrix;

	// The POLYGRID's reflection layer (§38/§59/§61). NULL = no reflection.
	// ⚠️ Its 2D projection is XZ, not the world's XY — same shader, different
	// matrix, and §59 says explicitly not to copy one to the other.
	SharedTexture *m_pEnvMap;

	REmitState()
		: m_pTexture(0), m_bAlphaTest(false), m_nAlphaFunc(0), m_fAlphaRef(0.0f),
		  m_bBlend(false), m_nSrcBlend(0), m_nDstBlend(0),
		  m_bZTest(true), m_bZWrite(true),
		  m_bIgnoreDiffuse(false), m_fColorScale(1.0f), m_bTextureAlpha(true),
		  m_bFlattenDepth(false), m_fFlatNDCz(0.0f), m_pModelMatrix(0),
		  m_pEnvMap(0) {}
};

// RenderStruct::CreateRenderObject / DestroyRenderObject implementations.
CRenderObject *RModel_CreateRenderObject(CRenderObject::RENDER_OBJECT_TYPES eType);
bool RModel_DestroyRenderObject(CRenderObject *pObject);

// Draw every visible client model instance (call from the scene render with
// the view matrix already loaded; transforms are model-space -> world-space).
// fAspect = viewport width/height; needed for the player-view (FLAG_REALLYCLOSE)
// pass, which builds its own projection instead of using the scene one.
void RModel_DrawModels(float fAspect);
// The player-view (FLAG_REALLYCLOSE) weapon/hands pass. ⚠️ It CLEARS THE DEPTH
// BUFFER, so it must be the LAST scene draw — see the comment in model_renderdata.cpp.
void RModel_DrawPlayerView(float fAspect);

// Re-arm the one-shot in-world object census (called on each world load, so the
// numbers describe the level you are actually standing in -- otherwise the very
// first world, the intro cinematic, is the only one ever reported).
void RModel_ArmCensus(void);

// Sprite pass (OT_SPRITE billboards). Set the camera basis each frame from
// nr_RenderScene, then draw after the opaque passes.
void RSprite_SetCamera(const LTVector &vRight, const LTVector &vUp,
                        const LTVector &vForward, const LTVector &vPos);
void RSprite_DrawSprites();

// Player-view (FLAG_REALLYCLOSE) sprites — first-person muzzle/tool effects.
// ⚠️ Call from inside the player-view pass; camera space, fixed billboard basis.
void RSprite_DrawPlayerView();

// DRAWMODE_OBJECTLIST: draw ONLY these objects, with no world. This is the
// interface pass (CInterfaceMgr -> ILTClient::RenderObjects).
struct LTObject;
void RObjectList_Draw(LTObject **ppObjects, int nCount);

// Per-scene bookkeeping (the LT_TRACE_UI frame counter). Call once per scene.
void RModel_BeginSceneFrame(void);

// ★ Recompute every ATTACHED object's transform from its parent, for EVERY
// object type — not just models. Call once per scene, BEFORE any draw pass.
//
// D3D does this inside its object walk: d3d_ReallyProcessObject ends with
// `if(pObject->m_Attachments) d3d_ProcessAttachments(pObject, 0)`
// (tagnodes.cpp:104), which runs for every object type it processes, world
// models included. Our renderer only did it for OT_MODEL parents (inside the
// model draw), so anything attached to a WORLD MODEL — door handles, window
// panes — never had its client-side rotation recomputed.
void RModel_ProcessAttachments(void);

#endif // __MODEL_RENDERDATA_H__
