// ----------------------------------------------------------------------- //
//
// MODULE  : gl_drawprim.cpp
//
// PURPOSE : GL ILTDrawPrim — the 2D/immediate drawing interface used by the
//           engine console and (later) the game HUD. Derives from
//           CGenDrawPrim, which stores all the Set* state and implements the
//           SetXY/UV/RGB helper family; this file supplies the actual GL
//           drawing.
//
//           Replaces macos_nulldrawprim.cpp (the no-op bring-up stub).
//           Registered as BOTH the Default instance (console/UI) and the
//           Internal instance (CClientMgr::Render hard-calls SetCamera).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "gendrawprim.h"      // CGenDrawPrim (state + helpers)
#include "renderstruct.h"
#include "dtxmgr.h"           // TextureData (SetUVWH texture dims)
#include "de_world.h"         // SharedTexture
#include "ltmacwindow.h"
#include "gl_texture.h"       // GLTex_GetName / g_pGLStruct
#include "gl_drawprim.h"

#include <OpenGL/gl.h>

// Scene transforms captured by nr_RenderScene (for CAMERA/WORLD-space prims).
static float g_aSceneView[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static float g_fSceneRight = 1.0f, g_fSceneTop = 0.75f;
static float g_fSceneZNear = 5.0f, g_fSceneZFar = 100000.0f;
static bool  g_bSceneValid = false;

void GLDrawPrim_SetSceneTransform(const float *pViewMatrix16,
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

// ---------------------------------------------------------------------------

class CGLDrawPrim : public CGenDrawPrim
{
public:
	declare_interface(CGLDrawPrim);

	virtual LTRESULT BeginDrawPrim() { return LT_OK; }
	virtual LTRESULT EndDrawPrim()   { return LT_OK; }
	virtual void SaveViewport()    {}
	virtual void RestoreViewport() {}

	// Pixel-space UVWH against a texture's dimensions (the one overload
	// CGenDrawPrim leaves pure; mirrors CD3DDrawPrim's version).
	virtual void SetUVWH(LT_POLYGT4 *pPrim, HTEXTURE pTex,
	                     float u, float v, float w, float h)
	{
		if (!pPrim) return;

		float tw = 0.0f, th = 0.0f;
		if (pTex && g_pGLStruct && g_pGLStruct->GetTexture)
		{
			TextureData *pData = g_pGLStruct->GetTexture((SharedTexture*)pTex);
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

	// Re-expose the base's non-texture overloads hidden by the override above.
	using CGenDrawPrim::SetUVWH;

	// ---- drawing --------------------------------------------------------

	virtual LTRESULT DrawPrim(LT_POLYGT3 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYFT3 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYG3 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYF3 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYGT4 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYGT4 **ppPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYFT4 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYG4 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYF4 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEGT *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEFT *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEG *pPrim,   const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEF *pPrim,   const uint32 nCount = 1);
	virtual LTRESULT DrawPrimPoint(LT_VERTGT *pVerts, const uint32 nCount = 1);
	virtual LTRESULT DrawPrimPoint(LT_VERTG *pVerts,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrimFan(LT_VERTGT *pVerts, const uint32 nCount);
	virtual LTRESULT DrawPrimFan(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba);
	virtual LTRESULT DrawPrimFan(LT_VERTG *pVerts,  const uint32 nCount);
	virtual LTRESULT DrawPrimFan(LT_VERTF *pVerts,  const uint32 nCount, LT_VERTRGBA rgba);
	virtual LTRESULT DrawPrimStrip(LT_VERTGT *pVerts, const uint32 nCount);
	virtual LTRESULT DrawPrimStrip(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba);
	virtual LTRESULT DrawPrimStrip(LT_VERTG *pVerts,  const uint32 nCount);
	virtual LTRESULT DrawPrimStrip(LT_VERTF *pVerts,  const uint32 nCount, LT_VERTRGBA rgba);

private:
	// Applies the current CGenDrawPrim state to GL. bTextured = the primitive
	// type carries UVs (only then does the bound texture matter).
	void SetupState(bool bTextured);
	void CleanupState();
};

define_interface(CGLDrawPrim, ILTDrawPrim);
instantiate_interface(CGLDrawPrim, ILTDrawPrim, Internal);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void CGLDrawPrim::SetupState(bool bTextured)
{
	LTMacWin_MakeCurrent();

	// DrawPrim is the 2D/UI path (console, HUD, menus). D3D forces
	// D3DRS_FOGENABLE FALSE around every one of these (d3d_draw.cpp:199/253,
	// d3d_optimizedsurface.cpp:381) — a fogged HUD would wash out with distance
	// it has no business having.
	glDisable(GL_FOG);

	int nDrawW = 640, nDrawH = 480;
	LTMacWin_GetSize(&nDrawW, &nDrawH);

	// Transform
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	switch (m_eTransType)
	{
		case DRAWPRIM_TRANSFORM_SCREEN:
		{
			// Engine screen coordinates (top-left origin) over the whole
			// drawable, whatever resolution the engine believes it runs at.
			float fScreenW = (g_pGLStruct && g_pGLStruct->m_Width)  ? (float)g_pGLStruct->m_Width  : (float)nDrawW;
			float fScreenH = (g_pGLStruct && g_pGLStruct->m_Height) ? (float)g_pGLStruct->m_Height : (float)nDrawH;
			glViewport(0, 0, nDrawW, nDrawH);
			glOrtho(0.0, fScreenW, fScreenH, 0.0, -1.0, 1.0);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
			break;
		}
		case DRAWPRIM_TRANSFORM_CAMERA:
		case DRAWPRIM_TRANSFORM_WORLD:
		{
			glFrustum(-g_fSceneRight, g_fSceneRight, -g_fSceneTop, g_fSceneTop,
			          g_fSceneZNear, g_fSceneZFar);
			glMatrixMode(GL_MODELVIEW);
			if (m_eTransType == DRAWPRIM_TRANSFORM_WORLD && g_bSceneValid)
			{
				glLoadMatrixf(g_aSceneView);
			}
			else
			{
				// Camera space: Lithtech looks down +Z, GL down -Z.
				glLoadIdentity();
				glScalef(1.0f, 1.0f, -1.0f);
			}
			break;
		}
	}

	// Z buffer
	switch (m_eZBufferMode)
	{
		case DRAWPRIM_ZRW: glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);  break;
		case DRAWPRIM_ZRO: glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); break;
		case DRAWPRIM_NOZ: glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); break;
	}

	// Alpha blend (the exotic modes map to their closest GL equivalent)
	if (m_BlendMode == DRAWPRIM_NOBLEND)
		glDisable(GL_BLEND);
	else
	{
		glEnable(GL_BLEND);
		switch (m_BlendMode)
		{
			case DRAWPRIM_BLEND_ADD:                glBlendFunc(GL_ONE, GL_ONE); break;
			case DRAWPRIM_BLEND_SATURATE:           glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE); break;
			case DRAWPRIM_BLEND_MOD_SRCALPHA:       glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
			case DRAWPRIM_BLEND_MOD_SRCCOLOR:       glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR); break;
			case DRAWPRIM_BLEND_MOD_DSTCOLOR:       glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR); break;
			case DRAWPRIM_BLEND_MUL_SRCCOL_DSTCOL:  glBlendFunc(GL_SRC_COLOR, GL_DST_COLOR); break;
			case DRAWPRIM_BLEND_MUL_SRCALPHA_ONE:   glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
			case DRAWPRIM_BLEND_MUL_SRCALPHA:       glBlendFunc(GL_SRC_ALPHA, GL_ZERO); break;
			case DRAWPRIM_BLEND_MUL_SRCCOL_ONE:     glBlendFunc(GL_SRC_COLOR, GL_ONE); break;
			case DRAWPRIM_BLEND_MUL_DSTCOL_ZERO:    glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
			default:                                glBlendFunc(GL_ONE, GL_ZERO); break;
		}
	}

	// Alpha test (reference value 0.5, per the interface contract)
	if (m_eTestMode == DRAWPRIM_NOALPHATEST)
		glDisable(GL_ALPHA_TEST);
	else
	{
		glEnable(GL_ALPHA_TEST);
		switch (m_eTestMode)
		{
			case DRAWPRIM_ALPHATEST_LESS:         glAlphaFunc(GL_LESS, 0.5f); break;
			case DRAWPRIM_ALPHATEST_LESSEQUAL:    glAlphaFunc(GL_LEQUAL, 0.5f); break;
			case DRAWPRIM_ALPHATEST_GREATER:      glAlphaFunc(GL_GREATER, 0.5f); break;
			case DRAWPRIM_ALPHATEST_GREATEREQUAL: glAlphaFunc(GL_GEQUAL, 0.5f); break;
			case DRAWPRIM_ALPHATEST_EQUAL:        glAlphaFunc(GL_EQUAL, 0.5f); break;
			case DRAWPRIM_ALPHATEST_NOTEQUAL:     glAlphaFunc(GL_NOTEQUAL, 0.5f); break;
			default: break;
		}
	}

	// Texture + color op
	GLuint nTexName = 0;
	if (bTextured && m_pTexture && m_ColorOp != DRAWPRIM_NOCOLOROP)
		nTexName = GLTex_GetName((SharedTexture*)m_pTexture);
	if (nTexName)
	{
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, nTexName);
		switch (m_ColorOp)
		{
			case DRAWPRIM_ADD:   glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD); break;
			case DRAWPRIM_DECAL: glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); break;
			default:             glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); break;
		}
	}
	else
		glDisable(GL_TEXTURE_2D);

	// Cull / fill
	if (m_eCullMode == DRAWPRIM_CULL_NONE)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		// The engine enumerates by winding to REJECT; GL by face to cull.
		glFrontFace(m_eCullMode == DRAWPRIM_CULL_CCW ? GL_CW : GL_CCW);
		glCullFace(GL_BACK);
	}
	glPolygonMode(GL_FRONT_AND_BACK, m_eFillMode == DRAWPRIM_WIRE ? GL_LINE : GL_FILL);
}

void CGLDrawPrim::CleanupState()
{
	// Leave GL in the state the 3D paths expect (they set their own state,
	// but keep the sticky odd bits from leaking).
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_2D);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glDepthMask(GL_TRUE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

// ---------------------------------------------------------------------------
// Vertex emitters
// ---------------------------------------------------------------------------

static inline void glp_EmitGT(const LT_VERTGT &v)
{
	glColor4ub(v.rgba.r, v.rgba.g, v.rgba.b, v.rgba.a);
	glTexCoord2f(v.u, v.v);
	glVertex3f(v.x, v.y, v.z);
}

static inline void glp_EmitG(const LT_VERTG &v)
{
	glColor4ub(v.rgba.r, v.rgba.g, v.rgba.b, v.rgba.a);
	glVertex3f(v.x, v.y, v.z);
}

static inline void glp_EmitFT(const LT_VERTFT &v, const LT_VERTRGBA &c)
{
	glColor4ub(c.r, c.g, c.b, c.a);
	glTexCoord2f(v.u, v.v);
	glVertex3f(v.x, v.y, v.z);
}

static inline void glp_EmitF(const LT_VERTF &v, const LT_VERTRGBA &c)
{
	glColor4ub(c.r, c.g, c.b, c.a);
	glVertex3f(v.x, v.y, v.z);
}

// ---------------------------------------------------------------------------
// DrawPrim family
// ---------------------------------------------------------------------------

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYGT3 *pPrim, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_TRIANGLES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			glp_EmitGT(pPrim[n].verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYFT3 *pPrim, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_TRIANGLES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			glp_EmitFT(pPrim[n].verts[v], pPrim[n].rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYG3 *pPrim, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_TRIANGLES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			glp_EmitG(pPrim[n].verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYF3 *pPrim, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_TRIANGLES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			glp_EmitF(pPrim[n].verts[v], pPrim[n].rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYGT4 *pPrim, const uint32 nCount)
{
	if (getenv("LT_TRACE_DRAWPRIM") && nCount > 0)
		fprintf(stderr, "[dp] GT4 n=%u trans=%d tex=%p v0=(%.1f,%.1f) v2=(%.1f,%.1f)\n",
			nCount, (int)m_eTransType, m_pTexture,
			pPrim[0].verts[0].x, pPrim[0].verts[0].y,
			pPrim[0].verts[2].x, pPrim[0].verts[2].y);
	SetupState(true);
	glBegin(GL_QUADS);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 4; ++v)
			glp_EmitGT(pPrim[n].verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYGT4 **ppPrim, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_QUADS);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 4; ++v)
			glp_EmitGT(ppPrim[n]->verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYFT4 *pPrim, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_QUADS);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 4; ++v)
			glp_EmitFT(pPrim[n].verts[v], pPrim[n].rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYG4 *pPrim, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_QUADS);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 4; ++v)
			glp_EmitG(pPrim[n].verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_POLYF4 *pPrim, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_QUADS);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 4; ++v)
			glp_EmitF(pPrim[n].verts[v], pPrim[n].rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_LINEGT *pPrim, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_LINES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			glp_EmitGT(pPrim[n].verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_LINEFT *pPrim, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_LINES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			glp_EmitFT(pPrim[n].verts[v], pPrim[n].rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_LINEG *pPrim, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_LINES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			glp_EmitG(pPrim[n].verts[v]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrim(LT_LINEF *pPrim, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_LINES);
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			glp_EmitF(pPrim[n].verts[v], pPrim[n].rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimPoint(LT_VERTGT *pVerts, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_POINTS);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitGT(pVerts[n]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimPoint(LT_VERTG *pVerts, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_POINTS);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitG(pVerts[n]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimFan(LT_VERTGT *pVerts, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_TRIANGLE_FAN);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitGT(pVerts[n]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimFan(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	SetupState(true);
	glBegin(GL_TRIANGLE_FAN);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitFT(pVerts[n], rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimFan(LT_VERTG *pVerts, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_TRIANGLE_FAN);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitG(pVerts[n]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimFan(LT_VERTF *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	SetupState(false);
	glBegin(GL_TRIANGLE_FAN);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitF(pVerts[n], rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimStrip(LT_VERTGT *pVerts, const uint32 nCount)
{
	SetupState(true);
	glBegin(GL_TRIANGLE_STRIP);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitGT(pVerts[n]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimStrip(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	SetupState(true);
	glBegin(GL_TRIANGLE_STRIP);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitFT(pVerts[n], rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimStrip(LT_VERTG *pVerts, const uint32 nCount)
{
	SetupState(false);
	glBegin(GL_TRIANGLE_STRIP);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitG(pVerts[n]);
	glEnd();
	CleanupState();
	return LT_OK;
}

LTRESULT CGLDrawPrim::DrawPrimStrip(LT_VERTF *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	SetupState(false);
	glBegin(GL_TRIANGLE_STRIP);
	for (uint32 n = 0; n < nCount; ++n)
		glp_EmitF(pVerts[n], rgba);
	glEnd();
	CleanupState();
	return LT_OK;
}
