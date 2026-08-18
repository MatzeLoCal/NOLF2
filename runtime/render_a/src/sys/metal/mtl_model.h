// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_model.h
//
// PURPOSE : The Metal model draw — pass 3 of the GL → Metal conversion.
//           Characters, props, attached weapons and the player-view weapon.
//
//           ⚠️ THIS FILE DOES NO SKINNING AND NO LIGHTING. model_renderdata.cpp CPU-
//           skins every mesh and resolves all four D3D lighting terms to a
//           vertex colour (§68–§71) before anything gets here; it hands over a
//           finished triangle list. Duplicating either would fork years of
//           verified maths — the same reason mtl_world.mm does not parse.
//
// ----------------------------------------------------------------------- //
#ifndef __MTL_MODEL_H__
#define __MTL_MODEL_H__

#include <stdint.h>

struct REmitState;

// The vertex model_renderdata.cpp builds: position (already skinned, in whatever space
// the current pass draws in), UV, and the fully resolved lit colour + object
// alpha. Deliberately the same 24-byte layout the drawprim pass uses.
struct MTLModelVert
{
	float   x, y, z;
	float   u, v;
	uint8_t r, g, b, a;
	// ⚠️ APPENDED, so every existing aggregate initialiser still compiles and
	// simply zero-fills these. Only the POLYGRID's env-map path needs a normal
	// (a reflection vector cannot be faked on a height field); models, sprites
	// and particles leave it zero and their pipelines never read it.
	float   nx, ny, nz;
};

// The transform for the models drawn next. Both column-major; the projection
// must already be Metal's 0..1 depth range (use mtl_Frustum).
void MTLModel_SetTransform(const float *pView16, const float *pProj16);

// Project a world (or camera-space) point with the current transform and return
// its Metal NDC depth, 0..1. Returns false when it is behind the near plane --
// the caller should then skip the sprite, exactly as the GL probe did.
bool MTLModel_ProjectDepth(float x, float y, float z, float *pOutNDCz);

// Draw one mesh's triangle list with the authored state.
void MTLModel_DrawTris(const MTLModelVert *pVerts, uint32_t nCount,
                       const REmitState *pState);

// Release the shader library, pipeline cache and vertex ring.
void MTLModel_Term(void);

#endif // __MTL_MODEL_H__
