// ----------------------------------------------------------------------- //
//
// MODULE  : render_texture.cpp
//
// PURPOSE : The backend-neutral texture queries -- see render_texture.h.
//           Lifted verbatim out of gl_texture.cpp, which has since been
//           deleted; with Metal the only backend these now forward straight
//           to MTLTex_*.
//
//           ⚠️ THE FORWARDING LAYER IS NOT REDUNDANT. It is the vocabulary the
//           shared parse/walk/scene code (world_renderdata, gl_model,
//           gl_polygrid, gl_particles) asks its texture questions in. Calling
//           MTLTex_* from those files instead would put a backend name back
//           into shared code -- which is exactly the coupling that produced
//           five separate bugs while GL was still alive (§107/§108).
//
// ----------------------------------------------------------------------- //

#include <windows.h>
#include "ltbasedefs.h"
#include "render_texture.h"
#include "renderstruct.h"
#include "render_globals.h"
#include "sys/metal/mtl_texture.h"

// ---------------------------------------------------------------------------
// The backend-neutral queries (see the header for why they exist).
// ---------------------------------------------------------------------------
bool RTex_IsValid(SharedTexture *pTexture)
{
	if (!pTexture)
		return false;
	return MTLTex_Get(pTexture) != 0;
}

bool RTex_IsCubeMap(SharedTexture *pTexture)
{
	if (!pTexture)
		return false;
	return MTLTex_IsCubeMap(pTexture);
}

bool RTex_GetDetailParams(SharedTexture *pTexture, float &fScale,
                          float &fCos, float &fSin)
{
	fScale = 1.0f; fCos = 1.0f; fSin = 0.0f;
	if (!pTexture)
		return false;
	return MTLTex_GetDetailParams(pTexture, fScale, fCos, fSin);
}

bool RTex_GetDims(SharedTexture *pTexture, uint32 &nWidth, uint32 &nHeight)
{
	nWidth = nHeight = 0;
	if (!pTexture)
		return false;
	return MTLTex_GetDims(pTexture, nWidth, nHeight);
}

unsigned int RTex_GetAlphaRef(SharedTexture *pTexture)
{
	if (!pTexture)
		return 0;
	return MTLTex_GetAlphaRef(pTexture);
}

bool RTex_IsFullbrite(SharedTexture *pTexture)
{
	if (!pTexture)
		return false;
	return MTLTex_IsFullbrite(pTexture);
}

// The texture's FILE name, for diagnostics. ★ This one was always
// backend-neutral despite living in gl_texture.cpp as gltex_Name: it asks the
// ENGINE through the RenderStruct and never looks at m_pRenderData, so it was
// the one function in that file worth keeping.
const char *RTex_GetName(SharedTexture *pTexture)
{
	if (g_pRenderStruct && g_pRenderStruct->GetTextureName)
	{
		const char *pName = g_pRenderStruct->GetTextureName(pTexture);
		if (pName)
			return pName;
	}
	return "<unknown>";
}
