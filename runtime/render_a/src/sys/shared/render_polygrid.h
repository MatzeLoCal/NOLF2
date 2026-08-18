// ----------------------------------------------------------------------- //
//
// MODULE  : render_polygrid.h
//
// PURPOSE : OT_POLYGRID rendering — this is WATER (and the Siberia cutscene
//           ice, and every other animated height-field surface in the game).
//           Port of the fixed-function half of d3d_DrawPolyGrid
//           (sys/d3d/drawpolygrid.cpp).
//
// ----------------------------------------------------------------------- //
#ifndef __RENDER_POLYGRID_H__
#define __RENDER_POLYGRID_H__

// Draw every visible client OT_POLYGRID. Call with the scene matrices current.
//
// bTranslucent selects the pass, mirroring d3d_ProcessPolyGrid's split
// (drawpolygrid.cpp:1441): grids whose LTObject::IsTranslucent() is false are
// drawn with the opaque world (depth writes on), the rest go in the
// translucent pass after it (depth test on, writes off). Water is normally
// translucent; the split matters because an opaque grid must occlude.
void RPolyGrid_Draw(bool bTranslucent);

// Re-arm the one-shot census so it reports again for the NEXT world.
// RWorld_Load calls this (as it does RModel_ArmCensus): without it the
// inventory only ever described the FIRST level loaded — for the retail
// campaign that is the c01 intro, never the level you are actually looking at.
void RPolyGrid_ArmCensus(void);

// The scene camera, pushed once per scene by nr_RenderScene (as
// RSprite_SetCamera is). The env-map path needs BOTH halves of it:
//   * the basis, to turn GL's EYE-space reflection vector back into world space
//     for the texture matrix (d3d_SetEnvMapTextureStates' mCamToWorld);
//   * the position, because the Fresnel term is a per-vertex dot against the
//     vector from the camera to that vertex (GeneratePolyGridFresnelAlpha).
void RPolyGrid_SetCamera(const LTVector &vRight, const LTVector &vUp,
                          const LTVector &vForward, const LTVector &vPos);

#endif // __RENDER_POLYGRID_H__
