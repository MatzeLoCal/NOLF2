// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_drawprim.h
//
// PURPOSE : Metal ILTDrawPrim -- the 2D/immediate path (engine console, HUD,
//           the whole interface). The first pass of the GL -> Metal conversion.
//
//           CMTLDrawPrim is the single registered ILTDrawPrim (Default +
//           Internal). It derives from CRenderDrawPrim (shared/render_drawprim.h)
//           for the neutral pieces CGenDrawPrim leaves pure -- it used to derive
//           from CGLDrawPrim and forward to it under LT_RENDER_GL=1, which is
//           what the base class existed for.
//
// ----------------------------------------------------------------------- //
#ifndef __MTL_DRAWPRIM_H__
#define __MTL_DRAWPRIM_H__

#include <stdint.h>

// The optimized-2D SURFACE path behind nr_BlitToScreen / nr_WarpToScreen: draw
// one software interface surface (already expanded to RGBA8, transparency
// already keyed into the alpha) as a screen-space quad.
//
// dst = the 4 screen-space corners in ENGINE screen coordinates; src = the
// matching source pixel coordinates. bBlend is resolved by the caller — the GL
// path decides it from the transparency flag AND the alpha, and both backends
// must read the same decision rather than each deriving it.
void MTLDrawPrim_PresentSurface(const uint8_t *pRGBA, uint32_t nWidth, uint32_t nHeight,
                                const float dst[4][2], const float src[4][2],
                                float fAlpha, bool bBlend);

// Releases the pipeline-state cache, the shader library and the vertex ring.
void MTLDrawPrim_Term(void);

#endif // __MTL_DRAWPRIM_H__
