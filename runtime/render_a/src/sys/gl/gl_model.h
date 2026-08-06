// ----------------------------------------------------------------------- //
//
// MODULE  : gl_model.h
//
// PURPOSE : GL model rendering (Phase-2 A3). Provides the render objects the
//           engine's ModelPiece::Load creates through
//           RenderStruct::CreateRenderObject (rigid + skeletal LTB meshes,
//           parsed into CPU arrays), and a per-frame draw that walks the
//           client's OT_MODEL instances, CPU-skins them with the engine's
//           animated node transforms, and draws them with their skins.
//
// ----------------------------------------------------------------------- //
#ifndef __GL_MODEL_H__
#define __GL_MODEL_H__

#include "renderobject.h"

// RenderStruct::CreateRenderObject / DestroyRenderObject implementations.
CRenderObject *GLModel_CreateRenderObject(CRenderObject::RENDER_OBJECT_TYPES eType);
bool GLModel_DestroyRenderObject(CRenderObject *pObject);

// Draw every visible client model instance (call from the scene render with
// the view matrix already loaded; transforms are model-space -> world-space).
// fAspect = viewport width/height; needed for the player-view (FLAG_REALLYCLOSE)
// pass, which builds its own projection instead of using the scene one.
void GLModel_DrawModels(float fAspect);
// The player-view (FLAG_REALLYCLOSE) weapon/hands pass. ⚠️ It CLEARS THE DEPTH
// BUFFER, so it must be the LAST scene draw — see the comment in gl_model.cpp.
void GLModel_DrawPlayerView(float fAspect);

// Re-arm the one-shot in-world object census (called on each world load, so the
// numbers describe the level you are actually standing in -- otherwise the very
// first world, the intro cinematic, is the only one ever reported).
void GLModel_ArmCensus(void);

// Sprite pass (OT_SPRITE billboards). Set the camera basis each frame from
// nr_RenderScene, then draw after the opaque passes.
void GLSprite_SetCamera(const LTVector &vRight, const LTVector &vUp,
                        const LTVector &vForward, const LTVector &vPos);
void GLSprite_DrawSprites();

// Player-view (FLAG_REALLYCLOSE) sprites — first-person muzzle/tool effects.
// ⚠️ Call from inside the player-view pass; camera space, fixed billboard basis.
void GLSprite_DrawPlayerView();

// DRAWMODE_OBJECTLIST: draw ONLY these objects, with no world. This is the
// interface pass (CInterfaceMgr -> ILTClient::RenderObjects).
struct LTObject;
void GLObjectList_Draw(LTObject **ppObjects, int nCount);

// Per-scene bookkeeping (the LT_TRACE_UI frame counter). Call once per scene.
void GLModel_BeginSceneFrame(void);

// ★ Recompute every ATTACHED object's transform from its parent, for EVERY
// object type — not just models. Call once per scene, BEFORE any draw pass.
//
// D3D does this inside its object walk: d3d_ReallyProcessObject ends with
// `if(pObject->m_Attachments) d3d_ProcessAttachments(pObject, 0)`
// (tagnodes.cpp:104), which runs for every object type it processes, world
// models included. Our renderer only did it for OT_MODEL parents (inside the
// model draw), so anything attached to a WORLD MODEL — door handles, window
// panes — never had its client-side rotation recomputed.
void GLModel_ProcessAttachments(void);

#endif // __GL_MODEL_H__
