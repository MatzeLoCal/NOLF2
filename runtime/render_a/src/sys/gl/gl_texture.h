// ----------------------------------------------------------------------- //
//
// MODULE  : gl_texture.h
//
// PURPOSE : GL texture manager for the Phase-2 renderer. Turns the engine's
//           TextureData (raw DTX: 32-bit ARGB or DXT1/3/5) into GL texture
//           objects, cached on SharedTexture::m_pRenderData. Implements the
//           RenderStruct BindTexture/UnbindTexture seam.
//
// ----------------------------------------------------------------------- //
#ifndef __GL_TEXTURE_H__
#define __GL_TEXTURE_H__

class SharedTexture;
struct RenderStruct;

// The engine-side RenderStruct (function table for GetSharedTexture/GetTexture
// etc.). Set by rdll_RenderDLLSetup before anything can bind.
extern RenderStruct *g_pGLStruct;

// RenderStruct::BindTexture / UnbindTexture implementations. Bind creates the
// GL texture immediately (the engine loads the DTX on demand via GetTexture);
// a texture that fails to convert is cached as "failed" and drawn untextured.
void GLTex_Bind(SharedTexture *pTexture, bool bTextureChanged);
void GLTex_Unbind(SharedTexture *pTexture);

// GL texture name for drawing (0 = none/failed). Creates the GL texture on
// first use if the eager bind didn't already (needs a current GL context).
// How a texture's alpha channel must be used when drawing. Applying the wrong
// one makes surfaces disappear: a 0.5 alpha TEST discards every texel of a
// uniformly semi-transparent texture (paper screens, lamp glass, railings).
#define GLTEX_ALPHA_OPAQUE       0   // no alpha at all -> no test, no blend
#define GLTEX_ALPHA_MASKED       1   // hard 0/255 cutout (fences, foliage) -> alpha test
#define GLTEX_ALPHA_TRANSLUCENT  2   // smooth partial alpha -> must be BLENDED

unsigned int GLTex_GetName(SharedTexture *pTexture);

// The texture's FILE name, for diagnostics — identifying which texture an
// effect uses is otherwise guesswork from world positions.
const char *GLTex_GetTexName(SharedTexture *pTexture);
// Alpha class of an already-bound texture (GLTEX_ALPHA_*).
unsigned int GLTex_GetAlphaClass(SharedTexture *pTexture);
// Authored alpha-test reference from the DTX command string's "AlphaRef <n>".
// 0 = ALPHAREF_NONE = this surface must NOT be alpha-tested. This is the same
// rule the D3D renderer uses and is the real source of truth.
unsigned int GLTex_GetAlphaRef(SharedTexture *pTexture);

// True when the DTX carries DTX_CUBEMAP. d3d_SetEnvMapTransform branches on
// exactly this: a cube env map takes the raw 3-component camera->world
// reflection vector (D3DTTFF_COUNT3), a 2D one takes the scaled-and-biased
// x/y pair (D3DTTFF_COUNT2). Getting it backwards puts the reflection in the
// wrong place, not merely at the wrong scale.
bool GLTex_IsCubeMap(SharedTexture *pTexture);

// Authored DETAIL-texture placement for a BASE texture: the scale and the
// rotation (as cos/sin) that map its UV0 onto its linked detail texture. Both
// come from the DTX header's Extra bytes, not the command string. False when
// the texture is not bound; the outputs are still set to the identity.
bool GLTex_GetDetailParams(SharedTexture *pTexture, float &fScale, float &fCos, float &fSin);

// Base (mip 0) dimensions of the uploaded texture — sprite quads are sized
// texture-pixels × instance scale, like D3D's GetBaseWidth/Height.
bool GLTex_GetDims(SharedTexture *pTexture, uint32 &nWidth, uint32 &nHeight);

#endif // __GL_TEXTURE_H__
