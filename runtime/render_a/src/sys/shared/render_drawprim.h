// ----------------------------------------------------------------------- //
//
// MODULE  : render_drawprim.h
//
// PURPOSE : The BACKEND-NEUTRAL ILTDrawPrim base. CMTLDrawPrim derives from
//           this; it used to derive from CGLDrawPrim.
//
//           ★ WHY: gl_drawprim.h explained the arrangement and predicted this
//           step -- "when OpenGL is deleted the base class goes with it and the
//           overrides just lose one line each". Two implementations cannot both
//           register as ILTDrawPrim.Default (the interface manager's rule
//           without a chooser is "latest added wins", i.e. static-init order),
//           so inheritance carried the backend switch instead. With GL gone the
//           switch is gone, but the NEUTRAL parts of the old base are still
//           needed: the four trivial overrides, and SetUVWH -- the one overload
//           CGenDrawPrim leaves pure, which reads the DTX header through the
//           engine's RenderStruct and never touched a GL object.
//
// ----------------------------------------------------------------------- //
#ifndef __RENDER_DRAWPRIM_H__
#define __RENDER_DRAWPRIM_H__

#include "gendrawprim.h"      // CGenDrawPrim (state + helpers)

// Published by nr_RenderScene each frame: the scene's view matrix and frustum
// extents, so CAMERA/WORLD-space prims can reuse them.
void RenderDrawPrim_SetSceneTransform(const float *pViewMatrix16,
                                      float fFrustumRight, float fFrustumTop,
                                      float fZNear, float fZFar);

// Returns false before the first scene of the run, when the view matrix is
// still the identity.
bool RenderDrawPrim_GetSceneTransform(float *pViewMatrix16,
                                      float &fFrustumRight, float &fFrustumTop,
                                      float &fZNear, float &fZFar);

class CRenderDrawPrim : public CGenDrawPrim
{
public:
	declare_interface(CRenderDrawPrim);

	virtual LTRESULT BeginDrawPrim() { return LT_OK; }
	virtual LTRESULT EndDrawPrim()   { return LT_OK; }
	virtual void SaveViewport()    {}
	virtual void RestoreViewport() {}

	// Pixel-space UVWH against a texture's dimensions (mirrors CD3DDrawPrim).
	// ⚠️ Backend-neutral: reads the DTX header through the engine's
	// RenderStruct, not through any renderer object.
	virtual void SetUVWH(LT_POLYGT4 *pPrim, HTEXTURE pTex,
	                     float u, float v, float w, float h);

	// Re-expose the base's non-texture overloads hidden by the override above.
	using CGenDrawPrim::SetUVWH;
};

#endif  // __RENDER_DRAWPRIM_H__
