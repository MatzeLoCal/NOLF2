// ----------------------------------------------------------------------- //
//
// MODULE  : render_drawprim.cpp
//
// PURPOSE : The neutral half of the old gl_drawprim.cpp -- see the header.
//
// ----------------------------------------------------------------------- //

#include <string.h>
#include "bdefs.h"
#include "renderstruct.h"
#include "dtxmgr.h"           // TextureData (SetUVWH texture dims)
#include "de_world.h"         // SharedTexture
#include "sys/shared/render_drawprim.h"
#include "sys/shared/render_globals.h"

static float g_aSceneView[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static float g_fSceneRight = 1.0f, g_fSceneTop = 0.75f;
static float g_fSceneZNear = 5.0f, g_fSceneZFar = 100000.0f;
static bool  g_bSceneValid = false;

void RenderDrawPrim_SetSceneTransform(const float *pViewMatrix16,
                                  float fFrustumRight, float fFrustumTop,
                                  float fZNear, float fZFar)
{
	memcpy(g_aSceneView, pViewMatrix16, sizeof(g_aSceneView));
	g_fSceneRight = fFrustumRight;
	g_fSceneTop   = fFrustumTop;
	g_fSceneZNear = fZNear;
	g_fSceneZFar  = fZFar;
	g_bSceneValid = true;
}

bool RenderDrawPrim_GetSceneTransform(float *pViewMatrix16,
                                  float &fFrustumRight, float &fFrustumTop,
                                  float &fZNear, float &fZFar)
{
	if (pViewMatrix16)
		memcpy(pViewMatrix16, g_aSceneView, sizeof(g_aSceneView));
	fFrustumRight = g_fSceneRight;
	fFrustumTop   = g_fSceneTop;
	fZNear        = g_fSceneZNear;
	fZFar         = g_fSceneZFar;
	return g_bSceneValid;
}

void CRenderDrawPrim::SetUVWH(LT_POLYGT4 *pPrim, HTEXTURE pTex,
                          float u, float v, float w, float h)
{
	if (!pPrim) return;

	float tw = 0.0f, th = 0.0f;
	if (pTex && g_pRenderStruct && g_pRenderStruct->GetTexture)
	{
		TextureData *pData = g_pRenderStruct->GetTexture((SharedTexture*)pTex);
		if (pData)
		{
			tw = (float)pData->m_Header.m_BaseWidth;
			th = (float)pData->m_Header.m_BaseHeight;
		}
	}
	if (tw <= 0.0f || th <= 0.0f)
	{
		pPrim->verts[0].u = pPrim->verts[1].u = pPrim->verts[2].u = pPrim->verts[3].u = 0.0f;
		pPrim->verts[0].v = pPrim->verts[1].v = pPrim->verts[2].v = pPrim->verts[3].v = 0.0f;
		return;
	}

	const float fFactor = 1.0f;
	float fCenterU = 0.05f / tw;
	float fCenterV = 0.05f / th;
	pPrim->verts[0].u = u / tw                 + fCenterU;
	pPrim->verts[0].v = v / th                 + fCenterV;
	pPrim->verts[1].u = (u + w + fFactor) / tw + fCenterU;
	pPrim->verts[1].v = v / th                 + fCenterV;
	pPrim->verts[2].u = (u + w + fFactor) / tw + fCenterU;
	pPrim->verts[2].v = (v + h + fFactor) / th + fCenterV;
	pPrim->verts[3].u = u / tw                 + fCenterU;
	pPrim->verts[3].v = (v + h + fFactor) / th + fCenterV;
}
