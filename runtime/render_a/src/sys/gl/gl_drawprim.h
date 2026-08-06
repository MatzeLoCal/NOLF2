// ----------------------------------------------------------------------- //
//
// MODULE  : gl_drawprim.h
//
// PURPOSE : GL ILTDrawPrim (Phase-2 A5). The implementation lives in
//           gl_drawprim.cpp and registers itself as both the Default and
//           Internal ILTDrawPrim instances (console/UI 2D + engine-internal).
//
// ----------------------------------------------------------------------- //
#ifndef __GL_DRAWPRIM_H__
#define __GL_DRAWPRIM_H__

// Called by nr_RenderScene each frame: the scene's view matrix (GL layout)
// and frustum extents, so CAMERA/WORLD-space prims can reuse them.
void GLDrawPrim_SetSceneTransform(const float *pViewMatrix16,
                                  float fFrustumRight, float fFrustumTop,
                                  float fZNear, float fZFar);

#endif // __GL_DRAWPRIM_H__
