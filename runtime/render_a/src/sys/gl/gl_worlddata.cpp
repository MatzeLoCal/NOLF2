// ----------------------------------------------------------------------- //
//
// MODULE  : gl_worlddata.cpp
//
// PURPOSE : GL bring-up world renderer. The stream walk below mirrors
//           CD3D_RenderBlock::Load / CD3D_RenderWorld::Load byte for byte
//           (see d3d_renderblock.cpp) — the world file is a shared format, so
//           any deviation desyncs everything after it. Retail vertices are
//           44 bytes: pos(12) uv0(8) uv1(8) color(4) normal(12).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "iltstream.h"
#include "de_world.h"           // SharedTexture / WorldBsp
#include "de_objects.h"         // WorldModelInstance (per-instance transforms)
#include "clientmgr.h"          // g_pClientMgr (OT_WORLDMODEL object list)
#include "renderstruct.h"       // engine services (GetSharedTexture)
#include "lightmapdefs.h"       // LIGHTMAP_MAX_TOTAL_PIXELS
#include "lightmap_compress.h"  // DecompressLMData (RLE 24-bit)
#include "gl_worlddata.h"
#include "gl_polygrid.h"   // GLPolyGrid_ArmCensus (per-world census)
#include "gl_model.h"
#include "gl_texture.h"
#include "ltmacwindow.h"        // LTMacWin_MakeCurrent (runtime lightmap re-upload)
#include "setupobject.h"        // LoadSprite (.spr-textured world sections)
#include "de_sprite.h"          // Sprite/SpriteAnim frame data

#include <OpenGL/gl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>       // fabsf (env-map scale)
#include <vector>
#include <set>          // LT_TRACE_SECTIONFX distinct-value set
#include <string>

// PC shader codes from the world packer (EPCShaderType in d3d_renderblock.cpp).
enum
{
	kPCShader_None             = 0,
	kPCShader_Gouraud          = 1,
	kPCShader_Lightmap         = 2,
	kPCShader_Lightmap_Texture = 4,
	kPCShader_Skypan           = 5,
	kPCShader_SkyPortal        = 6,
	kPCShader_Occluder         = 7,
	kPCShader_DualTexture      = 8,
	kPCShader_Lightmap_Dual    = 9
};

struct GLWVertex
{
	LTVector m_vPos;
	float    m_fU0, m_fV0;
	float    m_fU1, m_fV1;
	uint32   m_nColor;      // D3D ARGB
	LTVector m_vNormal;
};

struct GLWSection
{
	uint8  m_nShaderCode;
	uint32 m_nTriCount;
	uint32 m_nStartIndex;   // first index (== triangle start * 3)
	SharedTexture *m_pTexture;   // base texture (slot 0); NULL = untextured
	GLuint m_nLMTexture;    // static lightmap (0 = none); UV1, clamped
	std::string m_sTexName; // slot-0 name, kept for the LT_TRACE_NOTEX census
};

// A decompressed 24-bit lightmap kept on the CPU: used both as the parse-time
// scratch and as each section's retained BASE (pre-light-group) lightmap so
// switchable light groups can be recomposed at runtime.
struct GLWPendingLM
{
	uint32 m_nWidth, m_nHeight;
	std::vector<uint8> m_aData;   // empty = section has no lightmap

	GLWPendingLM() : m_nWidth(0), m_nHeight(0) {}
};

// One light group's RLE contribution to one section's lightmap.
struct GLWSubLM
{
	uint32 m_nSection;
	uint32 m_nLeft, m_nTop, m_nWidth, m_nHeight;
	std::vector<uint8> m_aData;   // RLE intensity stream (0xFF = run escape)
};

// A named light group in a block: current color + its sub-lightmaps.
// m_nID is the 31-polynomial hash of the group name — identical to
// SRBLightGroup's operator>> so the engine's SetLightGroupColor IDs match.
struct GLWLightGroup
{
	uint32   m_nID;
	LTVector m_vColor;    // CURRENT color; starts at the authored default
	std::vector<GLWSubLM> m_aSubLMs;
	// RLE per-vertex intensity stream over the WHOLE block's vertex array
	// (CD3D_RenderBlock format: byte != 0 -> intensity for this vertex;
	// byte == 0 -> next byte is a skip count). This is how a light group
	// reaches VERTEX-LIT (Gouraud) surfaces, i.e. how a light switch can
	// affect the majority of the world that carries no lightmap.
	std::vector<uint8> m_aVertexIntensities;
};

struct GLWBlock
{
	LTVector m_vCenter, m_vHalfDims;
	std::vector<GLWSection> m_aSections;
	std::vector<GLWVertex>  m_aVertices;
	std::vector<uint32>     m_aIndices;
	// Retained for runtime light-group recomposition (switchable lights).
	std::vector<GLWPendingLM>  m_aBaseLMs;      // per section; may be empty
	std::vector<GLWLightGroup> m_aLightGroups;
	// Composed per-vertex color = baked m_nColor + sum over light groups of
	// (group color x RLE intensity), clamped -- CD3D_RenderBlock::
	// UpdateLightingData's exact math. RGB triplets, ready for glColor3ub.
	std::vector<uint8>         m_aComposedColor;
};

struct GLWorld
{
	std::vector<GLWBlock> m_aBlocks;
	char m_sName[64 + 1];   // world-model name (MAX_WORLDNAME_LEN); "" = main world

	GLWorld() { m_sName[0] = 0; }
};

static GLWorld           *g_pMainWorld = 0;
static std::vector<GLWorld*> g_aWorldModels;   // parsed, not drawn yet (doors etc.)
static uint32             g_nTotalVerts = 0, g_nTotalTris = 0;


// ---------------------------------------------------------------------------
// Composed VERTEX lighting (Gouraud surfaces).
//
// Final vertex color = baked m_nColor + sum over light groups of
// (group current color * per-vertex RLE intensity), clamped per channel --
// byte-for-byte CD3D_RenderBlock::UpdateLightingData. Recomputed whenever a
// group's color changes; the RLE stream spans the block's ENTIRE vertex array.
// ---------------------------------------------------------------------------
static void glw_ComposeVertexColors(GLWBlock &cBlock)
{
	const size_t nVerts = cBlock.m_aVertices.size();
	cBlock.m_aComposedColor.resize(nVerts * 3);

	// Base: the baked vertex lighting (D3D ARGB -> RGB bytes).
	for (size_t nVert = 0; nVert < nVerts; ++nVert)
	{
		uint32 nColor = cBlock.m_aVertices[nVert].m_nColor;
		cBlock.m_aComposedColor[nVert * 3 + 0] = (uint8)((nColor >> 16) & 0xFF);
		cBlock.m_aComposedColor[nVert * 3 + 1] = (uint8)((nColor >>  8) & 0xFF);
		cBlock.m_aComposedColor[nVert * 3 + 2] = (uint8)( nColor        & 0xFF);
	}

	for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
	{
		const GLWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
		if (cGroup.m_aVertexIntensities.empty())
			continue;

		// Groups that are switched off contribute nothing (D3D's early-out).
		LTVector vMax = cGroup.m_vColor * 255.0f;
		if ((uint32)vMax.x == 0 && (uint32)vMax.y == 0 && (uint32)vMax.z == 0)
			continue;

		const uint8 *pRLE = &cGroup.m_aVertexIntensities[0];
		const uint8 *pEnd = pRLE + cGroup.m_aVertexIntensities.size();

		// Mirrors the D3D walk exactly, including the skip semantics: a zero
		// byte is followed by a skip count, and the loop's own increment then
		// advances one more vertex.
		for (size_t nVert = 0; nVert < nVerts; ++nVert)
		{
			if (pRLE >= pEnd)
				break;
			uint8 nValue = *pRLE++;
			if (!nValue)
			{
				if (pRLE >= pEnd)
					break;
				nVert += *pRLE++;
				continue;
			}

			uint32 nR = (uint32)cBlock.m_aComposedColor[nVert * 3 + 0] + (uint32)(cGroup.m_vColor.x * (float)nValue);
			uint32 nG = (uint32)cBlock.m_aComposedColor[nVert * 3 + 1] + (uint32)(cGroup.m_vColor.y * (float)nValue);
			uint32 nB = (uint32)cBlock.m_aComposedColor[nVert * 3 + 2] + (uint32)(cGroup.m_vColor.z * (float)nValue);
			cBlock.m_aComposedColor[nVert * 3 + 0] = (uint8)LTMIN(nR, 0xFFu);
			cBlock.m_aComposedColor[nVert * 3 + 1] = (uint8)LTMIN(nG, 0xFFu);
			cBlock.m_aComposedColor[nVert * 3 + 2] = (uint8)LTMIN(nB, 0xFFu);
		}
	}
}


// ---------------------------------------------------------------------------
// ★ RETAIL RENDERS THE WORLD AT 2x ("Saturate"). Both D3D world paths
// overbrighten when g_CV_Saturate is set -- Gouraud via D3DTOP_MODULATE2X
// (d3d_rendershader_gouraud.cpp:195), lightmaps via the
// SRCBLEND=DESTCOLOR/DESTBLEND=SRCCOLOR trick (dest*src + src*dest) -- and the
// retail autoexec.cfg ships "Saturate" "1", so every retail install ran 2x.
// Rendering at plain MODULATE made our ENTIRE world half as bright as retail
// (spotted only when the user provided a Windows reference screenshot of the
// same Siberia spot: bright overcast there, near-night here). GL equivalent:
// GL_COMBINE with GL_RGB_SCALE 2 on the relevant stage.
// ---------------------------------------------------------------------------
static bool glw_SaturateOn()
{
	static int s_nSaturate = -1;
	if (s_nSaturate < 0)
	{
		s_nSaturate = 1;   // retail config default
		if (g_pGLStruct && g_pGLStruct->GetParameter && g_pGLStruct->GetParameterValueFloat)
		{
			HLTPARAM hParam = g_pGLStruct->GetParameter((char*)"Saturate");
			if (hParam)
				s_nSaturate = (g_pGLStruct->GetParameterValueFloat(hParam) != 0.0f) ? 1 : 0;
		}
		fprintf(stderr, "[glw] Saturate (2x world lighting): %s\n", s_nSaturate ? "ON" : "OFF");
	}
	return s_nSaturate != 0;
}

// ---------------------------------------------------------------------------
// ★★ WORLD-SURFACE ENVIRONMENT MAPPING (§40c)
//
// Reflective world geometry -- glass, polished metal, the whole submarine
// interior -- is NOT alpha-blended in this engine. erendershader.h has no
// alpha-blended world shader at all; the reflection IS the effect, and the
// signal is AUTHORED in the DTX command string:
//
//     GlUW001.DTX    envmap       TexFX\Cubic\glue1.dtx
//     sarc4.dtx      envmapalpha  texfx\envmapalpha\Global008.dtx
//
// r_LoadSystemTexture (render.cpp:142) parses those into
// SharedTexture::m_eTexType + the eLinkedTex_EnvMap link, and
// CD3D_RenderWorld::AllocShader (d3d_renderworld.cpp:861) maps the type onto
// eShader_Gouraud_EnvMap / _Alpha_EnvMap / eShader_Lightmap_Texture_EnvMap.
//
// ⚠️ Read §40's THREE DEAD ENDS before touching this: the texture alpha CLASS
// (550 of 903 textures "translucent"), the per-section textureEffect string
// (empty in every level), and SURF_TRANSPARENT (not in the render data). The
// command string was the signal all along.
// ---------------------------------------------------------------------------
namespace
{
	// This frame's camera basis -- d3d's ViewParams::m_mWorldEnvMap, which
	// SetBasisVectors(Right, Up, Forward) writes as COLUMNS, i.e. the
	// camera->world rotation.
	LTVector s_vEnvRight(1, 0, 0), s_vEnvUp(0, 1, 0), s_vEnvForward(0, 0, 1);

	float glw_GetConVar(const char *pName, float fDefault);   // defined with the fog block

	bool glw_EnvMapDisabled()
	{
		static int s_n = -1;
		if (s_n < 0) s_n = getenv("LT_NO_ENVMAP") ? 1 : 0;
		return s_n != 0;
	}

	bool glw_DetailDisabled()
	{
		static int s_n = -1;
		if (s_n < 0) s_n = getenv("LT_NO_DETAIL") ? 1 : 0;
		return s_n != 0;
	}

	// GL_ATI_texture_env_combine3 gives GL_MODULATE_ADD_ATI (arg0*arg2 + arg1),
	// which is the only fixed-function way to reproduce EnvMapAlpha's
	// D3DTOP_MODULATEALPHA_ADDCOLOR (base.rgb + base.a * env.rgb) in one pass.
	// Verified present on Apple's GL 2.1 (M2, "2.1 Metal - 90.5"); if it ever is
	// not, EnvMapAlpha falls back to a plain modulate rather than drawing wrong.
	bool glw_HaveCombine3()
	{
		static int s_n = -1;
		if (s_n < 0)
		{
			const char *pExt = (const char*)glGetString(GL_EXTENSIONS);
			s_n = (pExt && strstr(pExt, "GL_ATI_texture_env_combine3")) ? 1 : 0;
			if (!s_n)
				fprintf(stderr, "[glenv] GL_ATI_texture_env_combine3 missing — "
				                "EnvMapAlpha will modulate instead of add\n");
		}
		return s_n != 0;
	}
}

void GLWorld_SetCamera(const LTVector &vRight, const LTVector &vUp,
                       const LTVector &vForward)
{
	s_vEnvRight   = vRight;
	s_vEnvUp      = vUp;
	s_vEnvForward = vForward;
}

namespace
{
	// The SECOND texture layer this section's base texture authors, if any.
	// EnvMap and Detail are MUTUALLY EXCLUSIVE by construction -- they are two
	// values of the same `m_eTexType`, and they even share the one linked-texture
	// slot -- so they can share a texture unit and a code path. Only the
	// coordinate source differs: a reflection is generated (texgen), a detail
	// layer is the base UV run through a scale/rotate.
	enum EGLWSecondKind { kSecond_None = 0, kSecond_EnvMap, kSecond_Detail };
	enum EGLWEnvMode    { kEnv_None = 0, kEnv_Modulate, kEnv_AddSigned, kEnv_AlphaAdd };

	struct GLWEnvSetup
	{
		SharedTexture  *m_pTex;      // the linked reflection / detail texture
		GLuint          m_nName;     // its GL name (0 = unusable -> no layer)
		bool            m_bCube;     // DTX_CUBEMAP: 3-coord transform, cube target
		EGLWSecondKind  m_eKind;
		EGLWEnvMode     m_eMode;
		float           m_fScale, m_fCos, m_fSin;   // detail placement

		GLWEnvSetup()
			: m_pTex(0), m_nName(0), m_bCube(false), m_eKind(kSecond_None),
			  m_eMode(kEnv_None), m_fScale(1.0f), m_fCos(1.0f), m_fSin(0.0f) {}
		bool Active() const { return m_eKind != kSecond_None && m_nName != 0; }
		bool IsEnv()  const { return m_eKind == kSecond_EnvMap; }
	};

	// Mirrors CD3D_RenderWorld::AllocShader's texture-type switch, including its
	// FALLBACKS: with the bump-map paths unported we take the same branch D3D
	// takes when EnvBumpMap/DOT3EnvBumpMap are disabled, which is a plain env
	// map -- not "no reflection".
	//
	// ⚠️ eSharedTexType_EnvBumpMap_NoFallback is deliberately NOT in the list.
	// Its whole point is the name: AllocShader's disabled branch for it is
	// eShader_Gouraud_Texture / eShader_Lightmap_Texture, i.e. plain texturing
	// with NO reflection, unlike every other bump variant. Adding it "for
	// symmetry" would put a reflection on surfaces retail leaves matte.
	//
	// ⚠️ EnvMapAlpha exists ONLY on the Gouraud path. AllocShader's lightmapped
	// branch has no eSharedTexType_EnvMapAlpha case at all, so a lightmapped
	// surface with an envmapalpha command string gets eShader_Lightmap_Texture
	// and NO reflection. That is retail behaviour, not an oversight to "fix".
	//
	// ★★ DETAIL TEXTURING lives here too (eSharedTexType_Detail ->
	// eShader_Gouraud_Detail / eShader_Lightmap_Texture_Detail). It is by far the
	// most COMMON of these types -- 125 of C01S01's 127 authored linked textures,
	// i.e. essentially every stone, wood, stucco and fabric surface in Japan.
	// Its stage layout is identical to the env map's; only the coordinates differ.
	GLWEnvSetup glw_ResolveEnvMap(const GLWSection &cSection, bool bAuthoredLightmap)
	{
		GLWEnvSetup cOut;
		if (!cSection.m_pTexture)
			return cOut;

		switch (cSection.m_pTexture->m_eTexType)
		{
			case eSharedTexType_EnvMap:
			case eSharedTexType_EnvBumpMap:
			case eSharedTexType_DOT3EnvBumpMap:
			case eSharedTexType_EnvMapAlpha:
			{
				if (glw_EnvMapDisabled())
					return cOut;
				// "EnvMapEnable" -- retail's autoexec.cfg ships it as 1.
				static int s_nEnvEnable = -1;
				if (s_nEnvEnable < 0)
					s_nEnvEnable = (glw_GetConVar("EnvMapEnable", 1.0f) != 0.0f) ? 1 : 0;
				if (!s_nEnvEnable)
					return cOut;

				if (cSection.m_pTexture->m_eTexType == eSharedTexType_EnvMapAlpha)
				{
					if (bAuthoredLightmap)
						return cOut;             // see the ⚠️ above
					cOut.m_eMode = glw_HaveCombine3() ? kEnv_AlphaAdd : kEnv_Modulate;
				}
				else
				{
					// D3DTOP_ADDSIGNED vs D3DTOP_MODULATE is the "EnvMapAdd" console
					// variable, and its RENDERER DEFAULT IS 1 (rendererconsolevars.h:68)
					// -- retail's config does not override it, so the shipping look is
					// ADDSIGNED (base + env - 0.5), not a multiply. Assuming MODULATE
					// here would have made every reflection far too dark.
					cOut.m_eMode = (glw_GetConVar("EnvMapAdd", 1.0f) != 0.0f)
					             ? kEnv_AddSigned : kEnv_Modulate;
				}
				cOut.m_eKind = kSecond_EnvMap;
				break;
			}

			case eSharedTexType_Detail:
			{
				if (glw_DetailDisabled())
					return cOut;
				// "DetailTextures" (retail config: 1). AllocShader falls back to
				// eShader_Gouraud_Texture when it is off, i.e. base texture only.
				static int s_nDetailEnable = -1;
				if (s_nDetailEnable < 0)
					s_nDetailEnable = (glw_GetConVar("DetailTextures", 1.0f) != 0.0f) ? 1 : 0;
				if (!s_nDetailEnable)
					return cOut;

				// ⚠️ Same authored default as the env map, different variable:
				// "DetailTextureAdd" also DEFAULTS TO 1 -> D3DTOP_ADDSIGNED. A
				// detail texture is authored around mid-grey so that
				// `base + detail - 0.5` perturbs the surface without tinting it;
				// MODULATE would darken every one of these surfaces instead.
				cOut.m_eMode = (glw_GetConVar("DetailTextureAdd", 1.0f) != 0.0f)
				             ? kEnv_AddSigned : kEnv_Modulate;
				cOut.m_eKind = kSecond_Detail;

				// The placement is the BASE texture's, times the global
				// "DetailTextureScale" -- which retail's autoexec.cfg overrides to
				// 1.0, NOT the renderer default of 0.2. Reading the renderer
				// default here would tile the detail five times too coarsely.
				float fTexScale = 1.0f, fCos = 1.0f, fSin = 0.0f;
				GLTex_GetDetailParams(cSection.m_pTexture, fTexScale, fCos, fSin);
				cOut.m_fScale = fTexScale * glw_GetConVar("DetailTextureScale", 1.0f);
				cOut.m_fCos   = fCos;
				cOut.m_fSin   = fSin;
				break;
			}

			default:
				return cOut;
		}

		// ⚠️ eLinkedTex_EnvMap and eLinkedTex_Detail are THE SAME SLOT
		// (SharedTexture::GetTextureID, de_world.h:175) -- the link alone cannot
		// tell you which kind it is, only m_eTexType can. Either name works.
		cOut.m_pTex = cSection.m_pTexture->GetLinkedTexture(
			cOut.IsEnv() ? eLinkedTex_EnvMap : eLinkedTex_Detail);
		if (!cOut.m_pTex)
		{
			// An authored second texture whose file is missing from the install
			// (Tex\Cinematics\pub_glass.dtx is one). Retail tolerates it; draw the
			// base texture alone rather than crashing or drawing black. Same
			// family as §39/§41's empty authored strings.
			cOut.m_eKind = kSecond_None;
			return cOut;
		}
		cOut.m_nName = GLTex_GetName(cOut.m_pTex);
		cOut.m_bCube = cOut.IsEnv() && GLTex_IsCubeMap(cOut.m_pTex);
		if (!cOut.m_nName)
			cOut.m_eKind = kSecond_None;
		return cOut;
	}

	// Install the reflection on nUnit: GL_REFLECTION_MAP texgen plus the texture
	// matrix d3d_SetEnvMapTransform builds.
	//
	// ⚠️⚠️ THE NEGATED FORWARD COLUMN. GL_REFLECTION_MAP emits the reflection
	// vector in GL EYE space, which is RIGHT-handed (+Z points BACKWARD), while
	// D3D's CAMERASPACEREFLECTIONVECTOR is left-handed (+Z forward) -- and our
	// view matrix negates the forward row to bridge them (nullrender.cpp:402).
	// Without the negation the reflection is mirrored front-to-back and slides
	// the wrong way as the player turns. Same trap as the water in §38.
	void glw_BeginEnvUnit(GLenum eUnit, const GLWEnvSetup &cEnv, bool bLastUnitSaturate)
	{
		glActiveTexture(eUnit);
		if (cEnv.m_bCube)
		{
			glDisable(GL_TEXTURE_2D);
			glEnable(GL_TEXTURE_CUBE_MAP);
			glBindTexture(GL_TEXTURE_CUBE_MAP, cEnv.m_nName);
		}
		else
		{
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, cEnv.m_nName);
			// A reflection lookup must never wrap: the coordinates leave 0..1
			// wherever the reflection points away from the mapped hemisphere.
			// A DETAIL layer is the exact opposite -- its whole purpose is to tile
			// many times across the surface, so it must REPEAT.
			const GLint eWrap = cEnv.IsEnv() ? GL_CLAMP_TO_EDGE : GL_REPEAT;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, eWrap);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, eWrap);
		}

		float aTex[16];
		memset(aTex, 0, sizeof(aTex));

		if (!cEnv.IsEnv())
		{
			// ── DETAIL ──────────────────────────────────────────────────────
			// The coordinates are the BASE UVs put through the authored
			// scale+rotation; CRenderShader_*_Detail::TranslateVertices bakes
			// this per vertex, but it is a plain 2x2 so a texture matrix does it
			// for free and leaves the vertex loop alone:
			//     u1 = u0*S*cos + v0*S*(-sin)
			//     v1 = u0*S*sin + v0*S*cos
			// The caller feeds UV0 to this unit via glMultiTexCoord2f.
			const float S = cEnv.m_fScale;
			aTex[0] =  S * cEnv.m_fCos;  aTex[4] = S * -cEnv.m_fSin;
			aTex[1] =  S * cEnv.m_fSin;  aTex[5] = S *  cEnv.m_fCos;
			aTex[10] = 1.0f;
			aTex[15] = 1.0f;
			glMatrixMode(GL_TEXTURE);
			glPushMatrix();
			glLoadMatrixf(aTex);
			glMatrixMode(GL_MODELVIEW);
		}
		else
		{

		glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
		glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
		glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
		glEnable(GL_TEXTURE_GEN_S);
		glEnable(GL_TEXTURE_GEN_T);
		glEnable(GL_TEXTURE_GEN_R);

		const LTVector &vR = s_vEnvRight, &vU = s_vEnvUp, &vF = s_vEnvForward;
		if (cEnv.m_bCube)
		{
			// D3DTTFF_COUNT3 with mScale = m_mWorldEnvMap: the raw camera->world
			// rotation, all three coordinates, no scale or bias.
			aTex[0] = vR.x; aTex[4] = vU.x; aTex[8]  = -vF.x;
			aTex[1] = vR.y; aTex[5] = vU.y; aTex[9]  = -vF.y;
			aTex[2] = vR.z; aTex[6] = vU.z; aTex[10] = -vF.z;
			aTex[15] = 1.0f;
		}
		else
		{
			// D3DTTFF_COUNT2 with mScale = [fScale 0 0 .5; 0 fScale 0 .5; ...]
			// * m_mWorldEnvMap, i.e. u = fScale*Rworld.x + 0.5,
			// v = fScale*Rworld.y + 0.5. ⚠️ x and Y, not x and z -- the polygrid's
			// own transform (§38) projects XZ, because drawpolygrid.cpp builds a
			// different matrix. Do not copy that one here.
			float fScale = glw_GetConVar("EnvScale", 1.0f);
			fScale = (fabsf(fScale) > 0.001f) ? (-0.5f / fScale) : fScale;
			aTex[0] = fScale * vR.x; aTex[4] = fScale * vU.x; aTex[8] = -fScale * vF.x; aTex[12] = 0.5f;
			aTex[1] = fScale * vR.y; aTex[5] = fScale * vU.y; aTex[9] = -fScale * vF.y; aTex[13] = 0.5f;
			aTex[15] = 1.0f;
		}
		glMatrixMode(GL_TEXTURE);
		glPushMatrix();
		glLoadMatrixf(aTex);
		glMatrixMode(GL_MODELVIEW);

		}   // env-map coordinate setup

		// The combiner: D3D stage 1's COLOROP against CURRENT (the base texture)
		// and TEXTURE (the reflection or detail layer). ALPHA is passed through
		// untouched (D3D: ALPHAOP = SELECTARG2(CURRENT)) -- the second layer's own
		// alpha must never reach the fragment, or a detail texture or an env map
		// with an alpha channel would silently dim or discard the surface.
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		switch (cEnv.m_eMode)
		{
			case kEnv_AlphaAdd:
				// D3DTOP_MODULATEALPHA_ADDCOLOR = arg1.rgb + arg1.a * arg2.rgb.
				// GL_MODULATE_ADD_ATI = arg0*arg2 + arg1.
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE_ADD_ATI);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
				break;
			case kEnv_AddSigned:
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD_SIGNED);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
				break;
			default:
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
				break;
		}
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, bLastUnitSaturate ? 2.0f : 1.0f);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

		// ⚠️ ALWAYS LEAVE UNIT 0 SELECTED. glTexEnv/glTexParameter act on the
		// ACTIVE unit, and the stage-0 combiner is configured after this call --
		// leaving another unit active silently redirected the base-texture setup
		// onto this one, which then sampled at the default (0,0) texcoord and
		// painted flat white and black patches over half the room.
		glActiveTexture(GL_TEXTURE0);
	}

	// d3d_UnsetEnvMapTransform's job. A leaked texture matrix or texgen enable
	// corrupts every later pass in the frame -- the models, the sprites and the
	// player-view weapon all run through units 0/1 afterwards.
	void glw_EndEnvUnit(GLenum eUnit, const GLWEnvSetup &cEnv)
	{
		glActiveTexture(eUnit);
		if (cEnv.IsEnv())
		{
			glDisable(GL_TEXTURE_GEN_S);
			glDisable(GL_TEXTURE_GEN_T);
			glDisable(GL_TEXTURE_GEN_R);
		}
		glMatrixMode(GL_TEXTURE);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		if (cEnv.m_bCube)
			glDisable(GL_TEXTURE_CUBE_MAP);
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
	}

	// The final stage: D3D's stage 2, MODULATE(CURRENT, DIFFUSE) -- MODULATE2X
	// under Saturate. It exists as its own unit because the EnvMapAlpha ADD and
	// the ADDSIGNED op are NOT commutative with the diffuse modulate: retail
	// computes (base ⊕ env) * diffuse, and folding diffuse into unit 0 instead
	// would give base*diffuse ⊕ env, which is a different picture.
	//
	// A fixed-function unit is bypassed entirely when it has no enabled texture,
	// so the base texture is bound here again purely to keep the unit live; the
	// combiner never references GL_TEXTURE.
	void glw_BeginDiffuseUnit(GLenum eUnit, GLuint nAnyTexName, bool bSaturate)
	{
		glActiveTexture(eUnit);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, nAnyTexName);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, bSaturate ? 2.0f : 1.0f);
		// The object alpha rides the vertex colour (translucent world models),
		// and the alpha test reads the base texture's -- so both must survive.
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
		glActiveTexture(GL_TEXTURE0);   // see the warning in glw_BeginEnvUnit
	}

	void glw_EndDiffuseUnit(GLenum eUnit)
	{
		glActiveTexture(eUnit);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
	}
}

// ---------------------------------------------------------------------------
// Distance fog. Fixed-function GL_FOG against the D3D render states set in
// d3d_ReadExtraConsoleVariables (d3d_init.cpp:52-55): LINEAR mode, colour from
// FogR/G/B, range from FogNearZ..FogFarZ, gated on FogEnable.
//
// ★ AUTHORED DATA, not a heuristic: the level's WorldProperties object writes
// these console vars at load (WorldProperties.cpp:421-451) and a VolumeBrush
// overrides them while the player is inside it. Both change at runtime, so —
// unlike Saturate — these are re-read EVERY frame.
//
// Note FogR/G/B are declared `int` console vars but WorldProperties writes them
// with "%f", so read them as float and scale from 0..255.
// ---------------------------------------------------------------------------
namespace
{
	// LT_TRACE_FOG=1 -> log every console-var lookup behind the fog state.
	// This is how the "vars do not exist at all" root (see gl_convar.cpp) was
	// found: FogEnable resolved (autoexec.cfg sets it) while FogR/G/B/NearZ/FarZ
	// all reported MISSING.
	bool glw_TraceFog()
	{
		static int s_nTrace = -1;
		if (s_nTrace < 0)
			s_nTrace = getenv("LT_TRACE_FOG") ? 1 : 0;
		return s_nTrace != 0;
	}

	float glw_GetConVar(const char *pName, float fDefault)
	{
		if (!g_pGLStruct || !g_pGLStruct->GetParameter || !g_pGLStruct->GetParameterValueFloat)
			return fDefault;
		HLTPARAM hParam = g_pGLStruct->GetParameter((char*)pName);
		if (!hParam)
		{
			if (glw_TraceFog())
				fprintf(stderr, "[glw] convar '%s' MISSING, using %.1f\n", pName, fDefault);
			return fDefault;
		}
		float fVal = g_pGLStruct->GetParameterValueFloat(hParam);
		if (glw_TraceFog())
			fprintf(stderr, "[glw] convar '%s' = %.3f\n", pName, fVal);
		return fVal;
	}

	bool  s_bFogOn = false;
	float s_fFogR = 1.0f, s_fFogG = 1.0f, s_fFogB = 1.0f;
}

void GLWorld_ApplyFog(bool bSky)
{
	// Engine default is FogEnable 0; the level turns it on.
	s_bFogOn = (glw_GetConVar("FogEnable", 0.0f) != 0.0f);

	float fNear = glw_GetConVar(bSky ? "SkyFogNearZ" : "FogNearZ", 0.0f);
	float fFar  = glw_GetConVar(bSky ? "SkyFogFarZ"  : "FogFarZ",  2000.0f);

	// d3d_ReadExtraConsoleVariables kills fog when the range is degenerate
	// ("This handles a TNT bug if the near and far Z are the same") -- and
	// GL would divide by zero here for exactly the same reason.
	if (fFar <= fNear)
		s_bFogOn = false;

	if (!s_bFogOn)
	{
		glDisable(GL_FOG);
		return;
	}

	s_fFogR = glw_GetConVar("FogR", 255.0f) / 255.0f;
	s_fFogG = glw_GetConVar("FogG", 255.0f) / 255.0f;
	s_fFogB = glw_GetConVar("FogB", 255.0f) / 255.0f;

	const GLfloat aColor[4] = { s_fFogR, s_fFogG, s_fFogB, 1.0f };
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogfv(GL_FOG_COLOR, aColor);
	glFogf(GL_FOG_START, fNear);
	glFogf(GL_FOG_END, fFar);
	// D3D's D3DRS_FOGTABLEMODE is per-pixel; GL_FOG_HINT is the closest ask.
	glHint(GL_FOG_HINT, GL_NICEST);
	glEnable(GL_FOG);

	// ★ Report every CHANGE, not just the first call: these vars arrive late
	// (WorldProperties' InitialUpdate) and change again per volume brush, so a
	// once-only line would forever show the pre-level renderer defaults --
	// exactly the §28 "a diagnostic that can only fire once" trap.
	if (!bSky)
	{
		static float s_fLast[5] = { -1, -1, -1, -1, -1 };
		if (s_fLast[0] != s_fFogR || s_fLast[1] != s_fFogG || s_fLast[2] != s_fFogB ||
		    s_fLast[3] != fNear   || s_fLast[4] != fFar)
		{
			s_fLast[0] = s_fFogR; s_fLast[1] = s_fFogG; s_fLast[2] = s_fFogB;
			s_fLast[3] = fNear;   s_fLast[4] = fFar;
			fprintf(stderr, "[glw] fog ON: color=(%.0f %.0f %.0f) range=%.0f..%.0f\n",
			        s_fFogR * 255.0f, s_fFogG * 255.0f, s_fFogB * 255.0f, fNear, fFar);
		}
	}
}

void GLWorld_DisableFog()
{
	glDisable(GL_FOG);
}

bool GLWorld_GetFogColor(float &fR, float &fG, float &fB)
{
	fR = s_fFogR; fG = s_fFogG; fB = s_fFogB;
	return s_bFogOn;
}

void GLWorld_ApplyObjectFog(uint32 nFlags, uint32 nFlags2)
{
	if (!s_bFogOn || (nFlags & FLAG_FOGDISABLE))
	{
		glDisable(GL_FOG);
		return;
	}

	GLfloat aColor[4] = { s_fFogR, s_fFogG, s_fFogB, 1.0f };
	if (nFlags2 & FLAG2_ADDITIVE)
	{
		aColor[0] = aColor[1] = aColor[2] = 0.0f;
	}
	else if (nFlags2 & FLAG2_MULTIPLY)
	{
		aColor[0] = aColor[1] = aColor[2] = 1.0f;
	}
	glFogfv(GL_FOG_COLOR, aColor);
	glEnable(GL_FOG);
}

void GLWorld_RestoreSceneFog()
{
	if (!s_bFogOn)
	{
		glDisable(GL_FOG);
		return;
	}
	const GLfloat aColor[4] = { s_fFogR, s_fFogG, s_fFogB, 1.0f };
	glFogfv(GL_FOG_COLOR, aColor);
	glEnable(GL_FOG);
}

// ---------------------------------------------------------------------------
// Stream helpers (parse-and-discard for the parts the GL path doesn't keep).
// ---------------------------------------------------------------------------

static void glw_SkipBytes(ILTStream *pStream, uint32 nBytes)
{
	// ILTStream has no generic skip; seek relative to the current position.
	uint32 nPos = 0;
	pStream->GetPos(&nPos);
	pStream->SeekTo(nPos + nBytes);
}

// SRBGeometryPoly: u8 vertCount, vertCount * LTVector, plane (normal + dist).
static void glw_SkipGeometryPoly(ILTStream *pStream)
{
	uint8 nVertCount;
	*pStream >> nVertCount;
	glw_SkipBytes(pStream, (uint32)nVertCount * sizeof(LTVector) + sizeof(LTVector) + sizeof(float));
}

// (Light groups are parsed and composed in glw_LoadBlock — see the loader.)

// ---------------------------------------------------------------------------
// Lightmap upload. The stream carries the RLE-compressed 24-bit RGB map; the
// D3D renderer pads it into a pow2 texture without rescaling UVs, so UV1 in
// the file is relative to the padded size — replicate that exactly.
// ---------------------------------------------------------------------------

static uint32 glw_NextPow2(uint32 n)
{
	uint32 nPow2 = 1;
	while (nPow2 < n)
		nPow2 <<= 1;
	return nPow2;
}

static GLuint glw_CreateLightmapFromRGB(const uint8 *pData, uint32 nWidth, uint32 nHeight)
{
	if (!pData || !nWidth || !nHeight)
		return 0;

	uint32 nRealWidth  = glw_NextPow2(nWidth);
	uint32 nRealHeight = glw_NextPow2(nHeight);

	GLuint nName = 0;
	glGenTextures(1, &nName);
	if (!nName)
		return 0;
	glBindTexture(GL_TEXTURE_2D, nName);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	// Allocate the padded pow2 surface (contents undefined, like D3D), then
	// fill the used region. 24-bit rows are the D3D byte order (B,G,R).
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, nRealWidth, nRealHeight, 0,
	             GL_BGR, GL_UNSIGNED_BYTE, 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, nWidth, nHeight,
	                GL_BGR, GL_UNSIGNED_BYTE, pData);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return nName;
}

// Add one light group's RLE sub-lightmap rectangle into a decompressed
// lightmap: per texel, add color * intensity, clamped per channel. Byte-for-
// byte port of the loop in CD3D_RenderBlock's light-group update (0xFF in the
// intensity stream escapes a run: 0xFF, count, value).
static void glw_AddSubLM(GLWPendingLM &cLM, const LTVector &vColor,
                         uint32 nLeft, uint32 nTop, uint32 nWidth, uint32 nHeight,
                         const uint8 *pData, uint32 nDataSize)
{
	if (cLM.m_aData.empty() ||
	    nLeft + nWidth > cLM.m_nWidth || nTop + nHeight > cLM.m_nHeight)
		return;

	uint32 nStride = cLM.m_nWidth * 3;
	uint8 *pCurTexel = &cLM.m_aData[nTop * nStride + nLeft * 3];
	uint8 *pEndTexel = pCurTexel + nHeight * nStride;
	uint32 nWidthRemaining = nWidth;
	uint32 nStrideSkip = nStride - nWidth * 3;

	const uint8 *pCurInput = pData;
	const uint8 *pEndInput = pData + nDataSize;
	uint8 nRunLength = 0;
	uint8 nRunValue0 = 0, nRunValue1 = 0, nRunValue2 = 0;

	for (; pCurTexel != pEndTexel; pCurTexel += 3)
	{
		if (nRunLength)
		{
			uint32 n0 = pCurTexel[0] + (uint32)nRunValue0;
			pCurTexel[0] = (uint8)LTMIN(n0, 0xFF);
			uint32 n1 = pCurTexel[1] + (uint32)nRunValue1;
			pCurTexel[1] = (uint8)LTMIN(n1, 0xFF);
			uint32 n2 = pCurTexel[2] + (uint32)nRunValue2;
			pCurTexel[2] = (uint8)LTMIN(n2, 0xFF);
			--nRunLength;
		}
		else
		{
			if (pCurInput >= pEndInput)
				break;
			if (*pCurInput == 0xFF)
			{
				++pCurInput;
				if (pCurInput >= pEndInput)
					break;
				nRunLength = *pCurInput;
				++pCurInput;
				if (pCurInput >= pEndInput)
					break;
			}

			LTVector vLightAdd = vColor * (float)*pCurInput;
			uint32 n0 = pCurTexel[0] + (uint32)vLightAdd.x;
			pCurTexel[0] = (uint8)LTMIN(n0, 0xFF);
			uint32 n1 = pCurTexel[1] + (uint32)vLightAdd.y;
			pCurTexel[1] = (uint8)LTMIN(n1, 0xFF);
			uint32 n2 = pCurTexel[2] + (uint32)vLightAdd.z;
			pCurTexel[2] = (uint8)LTMIN(n2, 0xFF);

			if (nRunLength)
			{
				nRunValue0 = (uint8)LTMIN((uint32)vLightAdd.x, 0xFF);
				nRunValue1 = (uint8)LTMIN((uint32)vLightAdd.y, 0xFF);
				nRunValue2 = (uint8)LTMIN((uint32)vLightAdd.z, 0xFF);
			}

			++pCurInput;
		}

		--nWidthRemaining;
		if (!nWidthRemaining)
		{
			pCurTexel += nStrideSkip;
			nWidthRemaining = nWidth;
		}
	}
}

// ---------------------------------------------------------------------------
// Block / world loading (must consume exactly what the D3D loader consumes).
// ---------------------------------------------------------------------------

static int g_nDebugBlocks = 0;   // TEMP: verbose dump of the first blocks

static bool glw_LoadBlock(ILTStream *pStream, GLWBlock &cBlock)
{
	*pStream >> cBlock.m_vCenter;
	*pStream >> cBlock.m_vHalfDims;

	// Sections
	uint32 nSectionCount;
	*pStream >> nSectionCount;

	bool bDebug = (g_nDebugBlocks++ < 2);
	if (bDebug)
		fprintf(stderr, "[glw]   block: center=(%.0f %.0f %.0f) half=(%.0f %.0f %.0f) sections=%u\n",
		        cBlock.m_vCenter.x, cBlock.m_vCenter.y, cBlock.m_vCenter.z,
		        cBlock.m_vHalfDims.x, cBlock.m_vHalfDims.y, cBlock.m_vHalfDims.z,
		        nSectionCount);
	cBlock.m_aSections.reserve(nSectionCount);
	// Base lightmaps stay CPU-side (retained on the block) so light groups can
	// be composed in now AND recomposed at runtime (switchable lights).
	cBlock.m_aBaseLMs.clear();
	cBlock.m_aBaseLMs.resize(nSectionCount);
	std::vector<GLWPendingLM> &aPendingLMs = cBlock.m_aBaseLMs;
	uint32 nIndexOffset = 0;
	for (uint32 nSection = 0; nSection < nSectionCount; ++nSection)
	{
		char sTexName[2][MAX_PATH + 1];
		for (uint32 nTex = 0; nTex < 2; ++nTex)          // CRBSection::kNumTextures == 2
			pStream->ReadString(sTexName[nTex], sizeof(sTexName[nTex]));

		GLWSection cSection;
		*pStream >> cSection.m_nShaderCode;
		*pStream >> cSection.m_nTriCount;
		cSection.m_nStartIndex = nIndexOffset;
		nIndexOffset += cSection.m_nTriCount * 3;

		// Resolve the base texture (slot 0) through the engine. Sprite-textured
		// sections (*.spr) need the sprite system — not ported yet; draw those
		// untextured. Slot 1 (dual-texture/lightmap-dual) is a later pass.
		cSection.m_pTexture = 0;
		const char *pTex0 = sTexName[0];
		cSection.m_sTexName = pTex0 ? pTex0 : "";
		size_t nTexNameLen = strlen(pTex0);
		bool bSprite = (nTexNameLen >= 4) && (stricmp(pTex0 + nTexNameLen - 4, ".spr") == 0);
		if (pTex0[0] && bSprite)
		{
			// Sprite-textured surface (lamp glass, animated signs): load the
			// .spr through the engine and use its first frame's texture. (The
			// full animation is a later pass; retail-visible surfaces stop
			// being untextured holes.)
			FileRef cRef;
			cRef.m_FileType = FILE_ANYFILE;
			cRef.m_pFilename = pTex0;
			Sprite *pSprite = 0;
			if (LoadSprite(&cRef, &pSprite) == LT_OK && pSprite &&
			    pSprite->m_nAnims > 0 && pSprite->m_Anims[0].m_nFrames > 0)
				cSection.m_pTexture = pSprite->m_Anims[0].m_Frames[0].m_pTex;
			if (cSection.m_pTexture)
				cSection.m_pTexture->SetRefCount(cSection.m_pTexture->GetRefCount() + 1);
			else
				fprintf(stderr, "[glw] sprite texture not resolved: %s\n", pTex0);
		}
		else if (pTex0[0] && g_pGLStruct && g_pGLStruct->GetSharedTexture)
		{
			cSection.m_pTexture = g_pGLStruct->GetSharedTexture(pTex0);
			if (cSection.m_pTexture)
				cSection.m_pTexture->SetRefCount(cSection.m_pTexture->GetRefCount() + 1);
			else
				fprintf(stderr, "[glw] texture not found: %s\n", pTex0);
		}

		// ⚠️ The per-section AUTHORED "texture effect" string. Parsed and
		// discarded until now. LT_TRACE_SECTIONFX=1 reports every distinct
		// (shaderCode, effect, tex0, tex1) combination once, because "which
		// authored field says this surface is glass" is the whole question
		// behind the opaque-glass bug — and guessing from texture CONTENT is
		// what §13/§14 ruled out.
		char sName[MAX_PATH + 1];
		pStream->ReadString(sName, sizeof(sName));
		{
			static int s_nTraceFX = -1;
			if (s_nTraceFX < 0) s_nTraceFX = getenv("LT_TRACE_SECTIONFX") ? 1 : 0;
			if (s_nTraceFX && sName[0])
			{
				static std::set<std::string> s_seen;
				char szKey[600];
				snprintf(szKey, sizeof(szKey), "shader=%u effect='%s' tex0='%s' tex1='%s'",
				         (unsigned)cSection.m_nShaderCode, sName, pTex0,
				         sTexName[1][0] ? sTexName[1] : "");
				if (s_seen.insert(szKey).second)
					fprintf(stderr, "[sectionfx] %s\n", szKey);
			}
		}

		// Static lightmap: decompress now; the GL texture is created after
		// this block's light groups are composed in (below).
		uint32 nLMWidth, nLMHeight, nLMSize;
		*pStream >> nLMWidth >> nLMHeight >> nLMSize;
		cSection.m_nLMTexture = 0;
		if (nLMSize && nLMWidth && nLMHeight &&
		    nLMWidth * nLMHeight <= (uint32)LIGHTMAP_MAX_TOTAL_PIXELS)
		{
			std::vector<uint8> aCompressed(nLMSize);
			pStream->Read(&aCompressed[0], nLMSize);

			GLWPendingLM &cLM = aPendingLMs[nSection];
			std::vector<uint8> aDecompressed(LIGHTMAP_MAX_TOTAL_PIXELS * 3);
			if (DecompressLMData(&aCompressed[0], nLMSize, &aDecompressed[0]))
			{
				cLM.m_nWidth  = nLMWidth;
				cLM.m_nHeight = nLMHeight;
				cLM.m_aData.assign(aDecompressed.begin(),
				                   aDecompressed.begin() + (size_t)nLMWidth * nLMHeight * 3);
			}
			else
				fprintf(stderr, "[glw] lightmap decompress failed (%ux%u, %u bytes)\n",
				        nLMWidth, nLMHeight, nLMSize);
		}
		else if (nLMSize)
			glw_SkipBytes(pStream, nLMSize);

		if (bDebug)
			fprintf(stderr, "[glw]     sec %u: shader=%u tris=%u lm=%ux%u/%u\n",
			        nSection, (uint32)cSection.m_nShaderCode, cSection.m_nTriCount,
			        nLMWidth, nLMHeight, nLMSize);

		cBlock.m_aSections.push_back(cSection);
	}

	// Vertices (retail 44-byte layout)
	uint32 nVertexCount;
	*pStream >> nVertexCount;
	if (bDebug)
		fprintf(stderr, "[glw]     verts=%u\n", nVertexCount);
	cBlock.m_aVertices.resize(nVertexCount);
	for (uint32 nVert = 0; nVert < nVertexCount; ++nVert)
	{
		GLWVertex &cVert = cBlock.m_aVertices[nVert];
		*pStream >> cVert.m_vPos;
		*pStream >> cVert.m_fU0 >> cVert.m_fV0;
		*pStream >> cVert.m_fU1 >> cVert.m_fV1;
		*pStream >> cVert.m_nColor;
		*pStream >> cVert.m_vNormal;
	}

	// Triangles: i0 i1 i2 polyIndex (all uint32)
	uint32 nTriCount;
	*pStream >> nTriCount;
	cBlock.m_aIndices.resize(nTriCount * 3);
	for (uint32 nTri = 0; nTri < nTriCount; ++nTri)
	{
		uint32 nPolyIndex;
		*pStream >> cBlock.m_aIndices[nTri * 3 + 0];
		*pStream >> cBlock.m_aIndices[nTri * 3 + 1];
		*pStream >> cBlock.m_aIndices[nTri * 3 + 2];
		*pStream >> nPolyIndex;

		if (cBlock.m_aIndices[nTri * 3 + 0] >= nVertexCount ||
		    cBlock.m_aIndices[nTri * 3 + 1] >= nVertexCount ||
		    cBlock.m_aIndices[nTri * 3 + 2] >= nVertexCount)
		{
			fprintf(stderr, "[glw] corrupt index in block (tri %u) — stream desync?\n", nTri);
			return false;
		}
	}

	g_nTotalVerts += nVertexCount;
	g_nTotalTris  += nTriCount;

	// Sky portals
	uint32 nSkyPortalCount;
	*pStream >> nSkyPortalCount;
	for (; nSkyPortalCount; --nSkyPortalCount)
		glw_SkipGeometryPoly(pStream);

	// Occluders (a geometry poly + uint32 id)
	uint32 nOccluderCount;
	*pStream >> nOccluderCount;
	for (; nOccluderCount; --nOccluderCount)
	{
		glw_SkipGeometryPoly(pStream);
		glw_SkipBytes(pStream, sizeof(uint32));
	}

	// Light groups: RETAIN each group (name-hash id, authored color, RLE
	// sub-lightmaps) on the block so runtime SetLightGroupColor can recompose,
	// then compose base + Σ color×intensity into a scratch and upload. The
	// authored color is the default "on" state; switchable lights author BLACK
	// and are turned on by the game at runtime.
	uint32 nLightGroupCount;
	*pStream >> nLightGroupCount;
	cBlock.m_aLightGroups.reserve(nLightGroupCount);
	for (; nLightGroupCount; --nLightGroupCount)
	{
		GLWLightGroup cGroup;

		// Name → 31-polynomial hash, byte-for-byte the SRBLightGroup id.
		uint16 nNameLen;
		*pStream >> nNameLen;
		cGroup.m_nID = 0;
		for (uint16 nChar = 0; nChar < nNameLen; ++nChar)
		{
			uint8 nNextChar;
			*pStream >> nNextChar;
			cGroup.m_nID = cGroup.m_nID * 31 + (uint32)nNextChar;
		}

		*pStream >> cGroup.m_vColor;

		uint32 nIntensityDataLen;
		*pStream >> nIntensityDataLen;
		cGroup.m_aVertexIntensities.resize(nIntensityDataLen);
		if (nIntensityDataLen)
			pStream->Read(&cGroup.m_aVertexIntensities[0], nIntensityDataLen);

		uint32 nSectionLMCount;
		*pStream >> nSectionLMCount;
		for (uint32 nSectionLM = 0; nSectionLM < nSectionLMCount; ++nSectionLM)
		{
			uint32 nSubLMCount;
			*pStream >> nSubLMCount;
			for (; nSubLMCount; --nSubLMCount)
			{
				GLWSubLM cSub;
				cSub.m_nSection = nSectionLM;
				*pStream >> cSub.m_nLeft >> cSub.m_nTop
				         >> cSub.m_nWidth >> cSub.m_nHeight;
				uint32 nDataSize;
				*pStream >> nDataSize;
				cSub.m_aData.resize(nDataSize);
				if (nDataSize)
					pStream->Read(&cSub.m_aData[0], nDataSize);

				if (nSectionLM < aPendingLMs.size() && nDataSize)
					cGroup.m_aSubLMs.push_back(cSub);
			}
		}

		cBlock.m_aLightGroups.push_back(cGroup);
	}

	// Composed VERTEX lighting: baked color + the groups' authored default
	// colors. Without this, night levels rendered as if lit (see the removed
	// fixed key light in glw_DrawWorld).
	glw_ComposeVertexColors(cBlock);

	// Compose (base + light groups at their current colors) and upload.
	for (uint32 nSection = 0; nSection < nSectionCount && nSection < cBlock.m_aSections.size(); ++nSection)
	{
		const GLWPendingLM &cBase = aPendingLMs[nSection];
		if (cBase.m_aData.empty())
			continue;

		GLWPendingLM cComposed = cBase;   // scratch copy; base stays pristine
		for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
		{
			const GLWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
			for (size_t nSub = 0; nSub < cGroup.m_aSubLMs.size(); ++nSub)
			{
				const GLWSubLM &cSub = cGroup.m_aSubLMs[nSub];
				if (cSub.m_nSection == nSection)
					glw_AddSubLM(cComposed, cGroup.m_vColor,
					             cSub.m_nLeft, cSub.m_nTop,
					             cSub.m_nWidth, cSub.m_nHeight,
					             &cSub.m_aData[0], (uint32)cSub.m_aData.size());
			}
		}
		cBlock.m_aSections[nSection].m_nLMTexture =
			glw_CreateLightmapFromRGB(&cComposed.m_aData[0], cComposed.m_nWidth, cComposed.m_nHeight);
	}

	// Children: u8 flags + k_NumChildren(2) * u32 index (tree unused in GL yet)
	uint8 nChildFlags;
	*pStream >> nChildFlags;
	glw_SkipBytes(pStream, 2 * sizeof(uint32));

	return true;
}

static bool glw_LoadWorld(ILTStream *pStream, GLWorld &cWorld)
{
	uint32 nBlockCount;
	*pStream >> nBlockCount;

	cWorld.m_aBlocks.resize(nBlockCount);
	for (uint32 nBlock = 0; nBlock < nBlockCount; ++nBlock)
	{
		if (!glw_LoadBlock(pStream, cWorld.m_aBlocks[nBlock]))
			return false;
	}

	// EVERY world reads a trailing world-model count — nested world models
	// included (CD3D_RenderWorld::Load is fully recursive; nested counts are
	// simply 0). Skipping this read at nested levels desyncs the stream.
	uint32 nNumWorldModels;
	*pStream >> nNumWorldModels;
	for (uint32 nWM = 0; nWM < nNumWorldModels; ++nWM)
	{
		char sWMName[64 + 1];
		pStream->ReadString(sWMName, sizeof(sWMName));

		GLWorld *pWM = new GLWorld;
		LTStrCpy(pWM->m_sName, sWMName, sizeof(pWM->m_sName));
		if (!glw_LoadWorld(pStream, *pWM))
		{
			delete pWM;
			return false;
		}
		g_aWorldModels.push_back(pWM);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GLWorld_Load(ILTStream *pStream)
{
	GLModel_ArmCensus();     // report the object census for THIS world
	GLPolyGrid_ArmCensus();  // ...and the polygrid inventory
	GLWorld_Free();

	g_pMainWorld = new GLWorld;
	g_nTotalVerts = g_nTotalTris = 0;

	if (!glw_LoadWorld(pStream, *g_pMainWorld))
	{
		fprintf(stderr, "[glw] world render-data load FAILED\n");
		GLWorld_Free();
		return false;
	}

	fprintf(stderr, "[glw] world render data loaded: %u blocks, %u verts, %u tris, %u worldmodels\n",
	        (uint32)g_pMainWorld->m_aBlocks.size(), g_nTotalVerts, g_nTotalTris,
	        (uint32)g_aWorldModels.size());

	// Section census by shader code, main world + world models. Shader 0/6/7
	// (None/SkyPortal/Occluder) are deliberately skipped at draw time; anything
	// ELSE with a large triangle share that we mishandle would show up here as
	// geometry the player simply never sees.
	{
		static const char *s_apShader[] = {
			"0 None(SKIPPED)", "1 Gouraud", "2 Lightmap", "3 ?", "4 Lightmap_Texture",
			"5 Skypan", "6 SkyPortal(SKIPPED)", "7 Occluder(SKIPPED)",
			"8 DualTexture", "9 Lightmap_Dual" };
		uint32 aTris[16] = { 0 }, aSecs[16] = { 0 };
		for (size_t w = 0; w < 1 + g_aWorldModels.size(); ++w)
		{
			const GLWorld *pW = (w == 0) ? g_pMainWorld : g_aWorldModels[w - 1];
			if (!pW) continue;
			for (size_t b = 0; b < pW->m_aBlocks.size(); ++b)
				for (size_t n = 0; n < pW->m_aBlocks[b].m_aSections.size(); ++n)
				{
					uint8 c = pW->m_aBlocks[b].m_aSections[n].m_nShaderCode;
					if (c < 16) { aTris[c] += pW->m_aBlocks[b].m_aSections[n].m_nTriCount; ++aSecs[c]; }
				}
		}
		// LT_TRACE_WORLDTEX=1 lists every distinct section texture; setting it to
		// a substring instead lists the matching sections WITH their block centre,
		// so a suspect surface can be found and aimed at with LT_CAM_TARGET.
		if (const char *pFilter = getenv("LT_TRACE_WORLDTEX"))
		{
			if (pFilter[0] && pFilter[1])   // a real substring, not just "1"
			{
				for (size_t w = 0; w < 1 + g_aWorldModels.size(); ++w)
				{
					const GLWorld *pW3 = (w == 0) ? g_pMainWorld : g_aWorldModels[w - 1];
					if (!pW3) continue;
					for (size_t b = 0; b < pW3->m_aBlocks.size(); ++b)
						for (size_t n = 0; n < pW3->m_aBlocks[b].m_aSections.size(); ++n)
						{
							const GLWSection &cS = pW3->m_aBlocks[b].m_aSections[n];
							if (!strcasestr(cS.m_sTexName.c_str(), pFilter)) continue;
							fprintf(stderr, "[worldtex] %s%s tris=%u shader=%u block@(%.0f %.0f %.0f)\n",
							        (w == 0) ? "" : "[WM] ", cS.m_sTexName.c_str(),
							        (unsigned)cS.m_nTriCount, (unsigned)cS.m_nShaderCode,
							        pW3->m_aBlocks[b].m_vCenter.x, pW3->m_aBlocks[b].m_vCenter.y,
							        pW3->m_aBlocks[b].m_vCenter.z);
						}
				}
			}
			std::vector<std::string> aNames;
			for (size_t w = 0; w < 1 + g_aWorldModels.size(); ++w)
			{
				const GLWorld *pW2 = (w == 0) ? g_pMainWorld : g_aWorldModels[w - 1];
				if (!pW2) continue;
				for (size_t b = 0; b < pW2->m_aBlocks.size(); ++b)
					for (size_t n = 0; n < pW2->m_aBlocks[b].m_aSections.size(); ++n)
					{
						const std::string &sN = pW2->m_aBlocks[b].m_aSections[n].m_sTexName;
						bool bSeen = false;
						for (size_t i = 0; i < aNames.size(); ++i)
							if (aNames[i] == sN) { bSeen = true; break; }
						if (!bSeen) aNames.push_back(sN);
					}
			}
			for (size_t i = 0; i < aNames.size(); ++i)
				fprintf(stderr, "[worldtex] %s\n", aNames[i].c_str());
		}

		for (uint32 c = 0; c < 16; ++c)
			if (aSecs[c])
				fprintf(stderr, "[glw]   shader %-22s %6u sections %8u tris\n",
				        (c < 10) ? s_apShader[c] : "?", aSecs[c], aTris[c]);
	}

	// Diagnostics for switchable light groups (both env-gated, free):
	//   LT_LIGHTGROUP_INVENTORY=1  print every group (id hash, authored color)
	//   LT_TEST_LIGHTGROUPS_ON=1   turn every black-authored group WHITE — the
	//                              null shell drops the server's LightGroup
	//                              messages, so this simulates the game
	//                              switching the lights on (lamp posts etc.)
	if (getenv("LT_LIGHTGROUP_INVENTORY") || getenv("LT_TEST_LIGHTGROUPS_ON"))
	{
		bool bInventory = getenv("LT_LIGHTGROUP_INVENTORY") != 0;
		bool bForceOn   = getenv("LT_TEST_LIGHTGROUPS_ON") != 0;
		std::vector<uint32> aForceIDs;
		uint32 nGroups = 0;
		for (size_t nWorld = 0; nWorld < g_aWorldModels.size() + 1; ++nWorld)
		{
			GLWorld *pWorld = (nWorld == 0) ? g_pMainWorld : g_aWorldModels[nWorld - 1];
			for (size_t nBlock = 0; pWorld && nBlock < pWorld->m_aBlocks.size(); ++nBlock)
			{
				GLWBlock &cBlock = pWorld->m_aBlocks[nBlock];
				for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
				{
					GLWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
					++nGroups;
					if (bInventory)
						fprintf(stderr, "[glw] lightgroup id=0x%08x color=(%.2f %.2f %.2f) sublms=%u %s\n",
						        cGroup.m_nID, cGroup.m_vColor.x, cGroup.m_vColor.y, cGroup.m_vColor.z,
						        (uint32)cGroup.m_aSubLMs.size(),
						        pWorld->m_sName[0] ? pWorld->m_sName : "(main)");
					if (bForceOn && cGroup.m_vColor.x < 0.02f &&
					    cGroup.m_vColor.y < 0.02f && cGroup.m_vColor.z < 0.02f &&
					    !cGroup.m_aSubLMs.empty())
						aForceIDs.push_back(cGroup.m_nID);
				}
			}
		}
		fprintf(stderr, "[glw] %u light groups total\n", nGroups);
		for (size_t n = 0; n < aForceIDs.size(); ++n)
			GLWorld_SetLightGroupColor(aForceIDs[n], LTVector(1.0f, 1.0f, 1.0f));
		if (bForceOn)
			fprintf(stderr, "[glw] TEST: forced %u black-authored light groups WHITE\n",
			        (uint32)aForceIDs.size());
	}
	return true;
}

// Recompose one section's lightmap (base + every group's current color) and
// re-upload it into the existing GL texture.
static void glw_RecomposeSectionLM(GLWBlock &cBlock, uint32 nSection)
{
	if (nSection >= cBlock.m_aSections.size() || nSection >= cBlock.m_aBaseLMs.size())
		return;
	const GLWPendingLM &cBase = cBlock.m_aBaseLMs[nSection];
	GLuint nTexture = cBlock.m_aSections[nSection].m_nLMTexture;
	if (cBase.m_aData.empty() || !nTexture)
		return;

	GLWPendingLM cComposed = cBase;
	for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
	{
		const GLWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
		for (size_t nSub = 0; nSub < cGroup.m_aSubLMs.size(); ++nSub)
		{
			const GLWSubLM &cSub = cGroup.m_aSubLMs[nSub];
			if (cSub.m_nSection == nSection)
				glw_AddSubLM(cComposed, cGroup.m_vColor,
				             cSub.m_nLeft, cSub.m_nTop, cSub.m_nWidth, cSub.m_nHeight,
				             &cSub.m_aData[0], (uint32)cSub.m_aData.size());
		}
	}

	glBindTexture(GL_TEXTURE_2D, nTexture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cComposed.m_nWidth, cComposed.m_nHeight,
	                GL_BGR, GL_UNSIGNED_BYTE, &cComposed.m_aData[0]);
}

// ---------------------------------------------------------------------------
// Dynamic lights (lamp posts, muzzle flashes…): an additive per-vertex pass
// over the world geometry inside each light's radius — the fixed-function
// equivalent of D3D's dynamic-light shader. Lights come from the client's
// OT_LIGHT list (created by the game shell's SpecialFX) plus an env-injected
// test set:  LT_TEST_LIGHTS="x y z radius r g b[; x y z radius r g b]…"
// (r g b in 0-255) — the null shell drops the SFX messages that would create
// real lights, so this is how the pass is exercised without the game shell.
// ---------------------------------------------------------------------------

struct GLWDynLight
{
	LTVector m_vPos;
	float    m_fRadius;
	LTVector m_vColor;   // 0..1
};

// The LT_TEST_LIGHTS injection set, parsed once. ★ Shared with the MODEL
// lighting pass (§71) through GLWorld_GetTestDynLights: both passes must see
// the same lights or an A/B of world-vs-model lighting compares two scenes.
uint32 GLWorld_GetTestDynLights(const GLWTestDynLight **ppOut)
{
	static bool s_bParsed = false;
	static std::vector<GLWTestDynLight> s_aTestLights;

	if (!s_bParsed)
	{
		s_bParsed = true;
		const char *pEnv = getenv("LT_TEST_LIGHTS");
		if (pEnv)
		{
			char sBuf[1024];
			strncpy(sBuf, pEnv, sizeof(sBuf) - 1);
			sBuf[sizeof(sBuf) - 1] = 0;
			for (char *pTok = strtok(sBuf, ";"); pTok; pTok = strtok(NULL, ";"))
			{
				GLWTestDynLight cLight;
				if (sscanf(pTok, "%f %f %f %f %f %f %f",
				           &cLight.m_vPos.x, &cLight.m_vPos.y, &cLight.m_vPos.z,
				           &cLight.m_fRadius, &cLight.m_vColor255.x,
				           &cLight.m_vColor255.y, &cLight.m_vColor255.z) == 7)
					s_aTestLights.push_back(cLight);
			}
			fprintf(stderr, "[glw] %u test dynamic lights from LT_TEST_LIGHTS\n",
			        (uint32)s_aTestLights.size());
		}
	}

	if (ppOut)
		*ppOut = s_aTestLights.empty() ? 0 : &s_aTestLights[0];
	return (uint32)s_aTestLights.size();
}

static void glw_CollectDynamicLights(std::vector<GLWDynLight> &aLights)
{
	if (g_pClientMgr)
	{
		LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_LIGHT].m_Head;
		for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
		{
			DynamicLight *pLight = (DynamicLight*)pCur->m_pData;
			if (!pLight || !(pLight->m_Flags & FLAG_VISIBLE))
				continue;
			if (pLight->m_LightRadius <= 1.0f)
				continue;
			GLWDynLight cLight;
			cLight.m_vPos    = pLight->m_Pos;
			cLight.m_fRadius = pLight->m_LightRadius;
			cLight.m_vColor.Init(pLight->m_ColorR * (1.0f / 255.0f),
			                     pLight->m_ColorG * (1.0f / 255.0f),
			                     pLight->m_ColorB * (1.0f / 255.0f));
			aLights.push_back(cLight);
		}
	}

	const GLWTestDynLight *pTest = 0;
	const uint32 nTest = GLWorld_GetTestDynLights(&pTest);
	for (uint32 n = 0; n < nTest; ++n)
	{
		GLWDynLight cLight;
		cLight.m_vPos    = pTest[n].m_vPos;
		cLight.m_fRadius = pTest[n].m_fRadius;
		cLight.m_vColor  = pTest[n].m_vColor255 * (1.0f / 255.0f);
		aLights.push_back(cLight);
	}
}

static void glw_LightWorldBlocks(const GLWorld &cWorldData, const GLWDynLight &cLight);
static const GLWorld *glw_FindWorldModel(const char *pName);
static bool glw_IsSkyObject(const LTObject *pObject);

void GLWorld_DrawDynamicLights()
{
	if (!g_pMainWorld)
		return;

	std::vector<GLWDynLight> aLights;
	glw_CollectDynamicLights(aLights);

	// LT_TRACE_LIGHTFX=1 -- how many dynamic lights this pass actually gets,
	// once a second. The one-shot OT_LIGHT census at world load only sees
	// LEVEL-placed lights (of which the retail campaign has NONE, see §19);
	// the lights that matter are created during play -- the flashlight,
	// projectiles, explosions -- so they need a running count, not a census.
	if (getenv("LT_TRACE_LIGHTFX"))
	{
		static size_t s_nLastReported = (size_t)-1;
		if (aLights.size() != s_nLastReported)
		{
			s_nLastReported = aLights.size();
			fprintf(stderr, "[lfx-gl] dynamic light count now %u\n", (unsigned)aLights.size());
		}
	}

	if (aLights.empty())
		return;

	// Additive over the already-lit world; depth-test (no write) keeps the
	// light on visible surfaces only.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	for (size_t nLight = 0; nLight < aLights.size(); ++nLight)
	{
		const GLWDynLight &cLight = aLights[nLight];
		glw_LightWorldBlocks(*g_pMainWorld, cLight);

		// World models: light their object-space geometry too. The instance's
		// back-transform moves the light into model space; the model's own
		// transform goes on the GL stack exactly like the WM draw pass.
		if (g_pClientMgr)
		{
			LTLink *pWMHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_WORLDMODEL].m_Head;
			for (LTLink *pCur = pWMHead->m_pNext; pCur != pWMHead; pCur = pCur->m_pNext)
			{
				WorldModelInstance *pInstance = (WorldModelInstance*)pCur->m_pData;
				if (!pInstance || !(pInstance->m_Flags & FLAG_VISIBLE) || glw_IsSkyObject(pInstance))
					continue;
				const WorldBsp *pBsp = pInstance->GetOriginalBsp();
				const GLWorld *pWorld = pBsp ? glw_FindWorldModel(pBsp->m_WorldName) : 0;
				if (!pWorld)
					continue;

				GLWDynLight cObjLight = cLight;
				pInstance->m_BackTransform.Apply(cLight.m_vPos, cObjLight.m_vPos);

				const LTMatrix &m = pInstance->m_Transform;
				float aGL[16];
				for (int nRow = 0; nRow < 4; ++nRow)
					for (int nCol = 0; nCol < 4; ++nCol)
						aGL[nCol * 4 + nRow] = m.m[nRow][nCol];
				glMatrixMode(GL_MODELVIEW);
				glPushMatrix();
				glMultMatrixf(aGL);
				glw_LightWorldBlocks(*pWorld, cObjLight);
				glPopMatrix();
			}
		}
	}

	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glDisable(GL_TEXTURE_2D);
}

// Light one parsed render world's blocks with one light (light already in the
// world's own space; caller owns GL state + any instance transform).
static void glw_LightWorldBlocks(const GLWorld &cWorldData, const GLWDynLight &cLight)
{
	{
		const float fRadiusSq = cLight.m_fRadius * cLight.m_fRadius;

		for (size_t nBlock = 0; nBlock < cWorldData.m_aBlocks.size(); ++nBlock)
		{
			const GLWBlock &cBlock = cWorldData.m_aBlocks[nBlock];

			// Sphere-vs-AABB reject.
			LTVector vMin = cBlock.m_vCenter - cBlock.m_vHalfDims;
			LTVector vMax = cBlock.m_vCenter + cBlock.m_vHalfDims;
			LTVector vClamped(
				LTCLAMP(cLight.m_vPos.x, vMin.x, vMax.x),
				LTCLAMP(cLight.m_vPos.y, vMin.y, vMax.y),
				LTCLAMP(cLight.m_vPos.z, vMin.z, vMax.z));
			if ((vClamped - cLight.m_vPos).MagSqr() > fRadiusSq)
				continue;

			for (size_t nSection = 0; nSection < cBlock.m_aSections.size(); ++nSection)
			{
				const GLWSection &cSection = cBlock.m_aSections[nSection];
				if (cSection.m_nShaderCode == kPCShader_None ||
				    cSection.m_nShaderCode == kPCShader_SkyPortal ||
				    cSection.m_nShaderCode == kPCShader_Occluder)
					continue;

				GLuint nTexName = cSection.m_pTexture ? GLTex_GetName(cSection.m_pTexture) : 0;
				if (nTexName)
				{
					glEnable(GL_TEXTURE_2D);
					glBindTexture(GL_TEXTURE_2D, nTexName);
				}
				else
					glDisable(GL_TEXTURE_2D);

				glBegin(GL_TRIANGLES);
				const uint32 *pIndices = &cBlock.m_aIndices[cSection.m_nStartIndex];
				for (uint32 nIndex = 0; nIndex < cSection.m_nTriCount * 3; ++nIndex)
				{
					const GLWVertex &cVert = cBlock.m_aVertices[pIndices[nIndex]];
					LTVector vToLight = cLight.m_vPos - cVert.m_vPos;
					float fDist = vToLight.Mag();
					float fAtten = 1.0f - fDist / cLight.m_fRadius;
					if (fAtten > 0.0f && fDist > 0.1f)
					{
						// Lambert falloff (floor under a lamp gets the pool,
						// walls fall off with angle).
						float fNDotL = cVert.m_vNormal.Dot(vToLight / fDist);
						if (fNDotL < 0.0f) fNDotL = 0.0f;
						fAtten *= fNDotL;
					}
					else
						fAtten = 0.0f;
					glColor3f(cLight.m_vColor.x * fAtten,
					          cLight.m_vColor.y * fAtten,
					          cLight.m_vColor.z * fAtten);
					glTexCoord2f(cVert.m_fU0, cVert.m_fV0);
					glVertex3f(cVert.m_vPos.x, cVert.m_vPos.y, cVert.m_vPos.z);
				}
				glEnd();
			}
		}
	}
}

bool GLWorld_SetLightGroupColor(uint32 nID, const LTVector &vColor)
{
	if (!g_pMainWorld)
		return false;

	LTMacWin_MakeCurrent();

	// Main world + every world model (D3D walks both the same way).
	for (size_t nWorld = 0; nWorld < g_aWorldModels.size() + 1; ++nWorld)
	{
		GLWorld *pWorld = (nWorld == 0) ? g_pMainWorld : g_aWorldModels[nWorld - 1];
		if (!pWorld)
			continue;
		for (size_t nBlock = 0; nBlock < pWorld->m_aBlocks.size(); ++nBlock)
		{
			GLWBlock &cBlock = pWorld->m_aBlocks[nBlock];
			for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
			{
				GLWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
				if (cGroup.m_nID != nID)
					continue;
				cGroup.m_vColor = vColor;

				// Recompose the block's VERTEX lighting (Gouraud surfaces) --
				// this is what makes a light switch visibly change the world.
				glw_ComposeVertexColors(cBlock);

				// Recompose each section this group touches (once each).
				for (size_t nSub = 0; nSub < cGroup.m_aSubLMs.size(); ++nSub)
				{
					uint32 nSection = cGroup.m_aSubLMs[nSub].m_nSection;
					bool bAlreadyDone = false;
					for (size_t nPrev = 0; nPrev < nSub; ++nPrev)
						if (cGroup.m_aSubLMs[nPrev].m_nSection == nSection)
							{ bAlreadyDone = true; break; }
					if (!bAlreadyDone)
						glw_RecomposeSectionLM(cBlock, nSection);
				}
				// A name appears at most once per block; keep scanning other
				// blocks/worlds — the same group can span several.
				break;
			}
		}
	}
	return true;
}

static void glw_ReleaseTextures(GLWorld *pWorld)
{
	if (!pWorld)
		return;
	for (size_t nBlock = 0; nBlock < pWorld->m_aBlocks.size(); ++nBlock)
	{
		std::vector<GLWSection> &aSections = pWorld->m_aBlocks[nBlock].m_aSections;
		for (size_t nSection = 0; nSection < aSections.size(); ++nSection)
		{
			SharedTexture *pTexture = aSections[nSection].m_pTexture;
			if (pTexture && pTexture->GetRefCount() > 0)
				pTexture->SetRefCount(pTexture->GetRefCount() - 1);

			if (aSections[nSection].m_nLMTexture)
			{
				glDeleteTextures(1, &aSections[nSection].m_nLMTexture);
				aSections[nSection].m_nLMTexture = 0;
			}
		}
	}
}

void GLWorld_Free()
{
	glw_ReleaseTextures(g_pMainWorld);
	delete g_pMainWorld;
	g_pMainWorld = 0;
	for (size_t i = 0; i < g_aWorldModels.size(); ++i)
	{
		glw_ReleaseTextures(g_aWorldModels[i]);
		delete g_aWorldModels[i];
	}
	g_aWorldModels.clear();
	g_nTotalVerts = g_nTotalTris = 0;
}

bool GLWorld_IsLoaded()
{
	return g_pMainWorld != 0 && !g_pMainWorld->m_aBlocks.empty();
}

bool GLWorld_GetBounds(LTVector &vCenter, LTVector &vHalfDims)
{
	if (!GLWorld_IsLoaded())
		return false;

	LTVector vMin(FLT_MAX, FLT_MAX, FLT_MAX), vMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (size_t i = 0; i < g_pMainWorld->m_aBlocks.size(); ++i)
	{
		const GLWBlock &cBlock = g_pMainWorld->m_aBlocks[i];
		LTVector vBMin = cBlock.m_vCenter - cBlock.m_vHalfDims;
		LTVector vBMax = cBlock.m_vCenter + cBlock.m_vHalfDims;
		VEC_MIN(vMin, vMin, vBMin);
		VEC_MAX(vMax, vMax, vBMax);
	}
	vCenter   = (vMin + vMax) * 0.5f;
	vHalfDims = (vMax - vMin) * 0.5f;
	return true;
}

void GLWorld_GetStats(uint32 &nBlocks, uint32 &nVerts, uint32 &nTris, uint32 &nWorldModels)
{
	nBlocks      = g_pMainWorld ? (uint32)g_pMainWorld->m_aBlocks.size() : 0;
	nVerts       = g_nTotalVerts;
	nTris        = g_nTotalTris;
	nWorldModels = (uint32)g_aWorldModels.size();
}

// Common GL state for world/world-model drawing (paired with glw_EndDraw).
static void glw_BeginDraw()
{
	glEnable(GL_DEPTH_TEST);

	// ★★★ BACKFACE CULLING — AND THE WINDING IS A SINGLE GLOBAL FLIP, NOT
	// "INCONSISTENT". D3D's main world draw sets D3DRS_CULLMODE = D3DCULL_CCW
	// (d3d_renderworld.cpp:668): it DOES cull world backfaces. We drew
	// two-sided for the whole bring-up, and §15a concluded the D3D-era winding
	// "does not survive our transform chain". That conclusion was wrong — it
	// was reached by trying GL_BACK only. Our view matrix negates the forward
	// row to go LH->RH (nullrender.cpp), and a negated row is a REFLECTION:
	// it flips triangle winding globally and uniformly. So D3D's front faces
	// arrive here wound CW, and GL's default GL_CCW front face calls them
	// back-facing. Declaring GL_CW as front makes GL_BACK cull exactly the set
	// D3D culls.
	//
	// ⚠️ WHY IT MATTERS BEYOND "extra triangles": a DOUBLE-SIDED surface has two
	// COINCIDENT faces. Drawing both rasterizes them at the same depth, and
	// which one wins is decided per-pixel by depth-test ties — that is the
	// view-angle-dependent banding on C08S03's noren curtain, and it is the
	// "texture flickering" of §42(b)/§45/§48. Exactly coplanar surfaces
	// z-fight no matter how good the depth buffer is, which is why §45's
	// near/far precision arithmetic correctly found nothing to fix.
	//
	// Verified: with this on, the curtain is clean AND the room is complete.
	// (GL_BACK culling — the §15a experiment — removes the floor and walls,
	// which is what "inconsistent winding" looked like.)
	// LT_WORLD_NOCULL=1 restores the old two-sided draw for A/B.
	static int s_nNoCull = -1;
	if (s_nNoCull < 0) s_nNoCull = getenv("LT_WORLD_NOCULL") ? 1 : 0;
	if (s_nNoCull)
	{
		glDisable(GL_CULL_FACE);
	}
	else
	{
		glFrontFace(GL_CW);          // our LH->RH view matrix flips winding
		glCullFace(GL_BACK);
		glEnable(GL_CULL_FACE);
	}
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	// Cut out masked texels (fences, grates, foliage). Real translucency
	// (glass) becomes a cutout too — acceptable until blended passes exist.
	glAlphaFunc(GL_GREATER, 0.5f);
}

static void glw_EndDraw()
{
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_ALPHA_TEST);
	// ⚠️ Hand back the GL defaults. Every other pass (models, sprites,
	// polygrids) was written when the world left culling OFF, and model
	// winding really IS inconsistent — so leaking GL_CULL_FACE/GL_CW out of
	// here would drop model triangles.
	glDisable(GL_CULL_FACE);
	glFrontFace(GL_CCW);
}

// Draw one parsed render world (main world or a world model, in its own space).
// bAllowAlphaTest=false: draw textured sections without the cutout test (sky
// textures often carry zero alpha for blend layering — the test would discard
// every texel).
// nObjectAlpha: the OWNING OBJECT's alpha (LTObject::m_ColorA), multiplied into
// every emitted vertex colour. 255 for the main world and opaque world models.
// ⚠️ The colour emit below used glColor3ub, which forces alpha to 1.0 and threw
// this away — the same trap §35 found in glm_EmitMesh. Without it, a world model
// with m_ColorA < 255 (all the window/door GLASS) blends at full opacity, i.e.
// looks solid.
static void glw_DrawWorld(const GLWorld &cWorld, bool bAllowAlphaTest = true,
                          uint8 nObjectAlpha = 255)
{
	for (size_t nBlock = 0; nBlock < cWorld.m_aBlocks.size(); ++nBlock)
	{
		const GLWBlock &cBlock = cWorld.m_aBlocks[nBlock];
		for (size_t nSection = 0; nSection < cBlock.m_aSections.size(); ++nSection)
		{
			const GLWSection &cSection = cBlock.m_aSections[nSection];

			// Invisible / helper geometry
			if (cSection.m_nShaderCode == kPCShader_None ||
			    cSection.m_nShaderCode == kPCShader_SkyPortal ||
			    cSection.m_nShaderCode == kPCShader_Occluder)
				continue;

			GLuint nTexName = cSection.m_pTexture ? GLTex_GetName(cSection.m_pTexture) : 0;

			// LT_TRACE_NOTEX=1: one-shot census of every section that ends up
			// WITHOUT a base texture. Such a section falls into the bLMOnly path
			// below and draws its bare LIGHTMAP -- which reads as a black hole in
			// any shadowed area, so "missing texture" and "missing light" look
			// identical in a screenshot. This says which it actually is, and
			// where. LT_DEBUG_NOTEX=1 additionally tints them magenta on screen.
			if (nTexName == 0 && getenv("LT_TRACE_NOTEX"))
			{
				static std::vector<std::string> s_aReported;
				bool bSeen = false;
				for (size_t i = 0; i < s_aReported.size(); ++i)
					if (s_aReported[i] == cSection.m_sTexName) { bSeen = true; break; }
				if (!bSeen)
				{
					s_aReported.push_back(cSection.m_sTexName);
					fprintf(stderr, "[notex] '%s' shader=%u tris=%u block@(%.0f %.0f %.0f) lm=%s\n",
					        cSection.m_sTexName.c_str(), (unsigned)cSection.m_nShaderCode,
					        (unsigned)cSection.m_nTriCount,
					        cBlock.m_vCenter.x, cBlock.m_vCenter.y, cBlock.m_vCenter.z,
					        cSection.m_nLMTexture ? "yes" : "no");
				}
			}
			GLuint nLMName  = cSection.m_nLMTexture;

			// LT_TRACE_ENVMAP=1 — the AUTHORED texture-type census. r_LoadSystemTexture
			// parses each DTX command string into SharedTexture::m_eTexType and, for the
			// env-map family, links the reflection texture under eLinkedTex_EnvMap. This
			// reports every distinct base texture that is NOT plain Single, with the
			// shader code it is drawn under -- i.e. exactly the authored signal §40(c)
			// established is the real one, after the alpha-class / textureEffect /
			// SURF_TRANSPARENT dead ends.
			if (getenv("LT_TRACE_ENVMAP") && cSection.m_pTexture &&
			    cSection.m_pTexture->m_eTexType != eSharedTexType_Single)
			{
				static std::set<std::string> s_seen;
				const SharedTexture *pLinked =
					cSection.m_pTexture->GetLinkedTexture(eLinkedTex_EnvMap);
				char szKey[700];
				uint32 nEW = 0, nEH = 0;
				if (pLinked) GLTex_GetDims((SharedTexture*)pLinked, nEW, nEH);
				snprintf(szKey, sizeof(szKey), "type=%d shader=%u lm=%s tex='%s' env='%s' %ux%u cube=%d "
				                               "tris=%u block@(%.0f %.0f %.0f)",
				         (int)cSection.m_pTexture->m_eTexType,
				         (unsigned)cSection.m_nShaderCode, nLMName ? "yes" : "no",
				         cSection.m_sTexName.c_str(),
				         pLinked ? GLTex_GetTexName((SharedTexture*)pLinked) : "<none>",
				         nEW, nEH,
				         pLinked ? (int)GLTex_IsCubeMap((SharedTexture*)pLinked) : -1,
				         (unsigned)cSection.m_nTriCount,
				         cBlock.m_vCenter.x, cBlock.m_vCenter.y, cBlock.m_vCenter.z);
				if (s_seen.insert(szKey).second)
					fprintf(stderr, "[envmap] %s\n", szKey);
			}

			// Pure-lightmap sections (no base texture): the lightmap IS the
			// surface color — put it on unit 0 with its own UV set.
			// A section whose texture never resolved falls into the bLMOnly path
			// below and draws its bare LIGHTMAP, which reads as a flat glowing
			// panel -- the white/cyan patches on the village buildings. Those are
			// the LightAnim_BASE (light-animation) surfaces; until that system is
			// understood, drawing nothing is much closer to retail than drawing a
			// glowing panel. LT_DRAW_NOTEX=1 restores the old behaviour for
			// diagnosis (LT_TRACE_NOTEX=1 lists them).
			static int s_nDrawNoTex = -1;
			if (s_nDrawNoTex < 0) s_nDrawNoTex = getenv("LT_DRAW_NOTEX") ? 1 : 0;
			// ⚠️ Refinement of the §14 skip: it must only apply to LIGHTMAPPED
			// sections (whose fallback would be the glowing bare-lightmap
			// panels). A GOURAUD section with a failed texture is drawn by
			// retail D3D as solid VERTEX COLOR -- and the Siberia sky dome
			// depends on it: its authored texture (Tex/Siberia/SkySib01.dtx)
			// does not exist in the retail install AT ALL, so retail always
			// drew that dome as vertex-colored storm grey. Skipping it gave a
			// pure black Siberian sky. (Found via the §3 rez harness: the six
			// GAME2.REZ string hits are world-data references, not the file.)
			if (nTexName == 0 && nLMName != 0 && !s_nDrawNoTex)
				continue;

			bool bLMOnly = (nTexName == 0 && nLMName != 0);

			glActiveTexture(GL_TEXTURE0);
			if (nTexName || bLMOnly)
			{
				glEnable(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, bLMOnly ? nLMName : nTexName);
			}
			else
				glDisable(GL_TEXTURE_2D);

			// Alpha test only for real textures (cutout masks); lightmaps have
			// no alpha.
			// How this surface uses its alpha. An unconditional 0.5 cutout on
			// every textured surface discards EVERY texel of a uniformly
			// semi-transparent texture -- shoji paper screens, lamp-post glass
			// and railings all vanished that way, looking exactly like missing
			// geometry. Only genuine 0/255 masks (fences, foliage) get the test;
			// smoothly translucent surfaces are blended instead.
			// LT_NO_ALPHATEST=1 disables the test entirely (bisecting aid).
			static int s_nNoAlphaTest = -1;
			if (s_nNoAlphaTest < 0) s_nNoAlphaTest = getenv("LT_NO_ALPHATEST") ? 1 : 0;

			// The AUTHORED alpha-test reference (DTX "AlphaRef <n>"), exactly as
			// the D3D renderer does it: 0 == ALPHAREF_NONE == do not alpha-test
			// this surface. Content-based guessing is gone -- see §13.
			unsigned int nAlphaRef = cSection.m_pTexture
			                       ? GLTex_GetAlphaRef(cSection.m_pTexture)
			                       : 0;
			bool bBlend = false;

			if (nTexName && bAllowAlphaTest && !s_nNoAlphaTest && nAlphaRef != 0)
			{
				// Use the AUTHORED reference, not a hardcoded 0.5. D3D sets
				// D3DRS_ALPHAREF to this value with a GREATEREQUAL func.
				glAlphaFunc(GL_GEQUAL, (float)nAlphaRef / 255.0f);
				glEnable(GL_ALPHA_TEST);
			}
			else
			{
				glDisable(GL_ALPHA_TEST);

				// ★★ SMOOTHLY TRANSLUCENT SURFACES MUST BE BLENDED — THIS IS
				// WHAT MAKES WINDOW AND DOOR GLASS SEE-THROUGH.
				//
				// ⚠️ `bBlend` existed here with its teardown already written,
				// but NOTHING EVER SET IT: it was declared false and never
				// assigned, so the comment above promising "smoothly translucent
				// surfaces are blended instead" was simply not implemented. A
				// glass section therefore got NO alpha test (correct — its
				// authored AlphaRef is 0) and NO blend either, i.e. it drew
				// fully OPAQUE. Dead flag, silent wrong result.
				//
				// ⚠️⚠️ GATED OFF BY DEFAULT — `LT_WORLDBLEND=1` TO TRY IT.
				//
				// The obvious implementation is "blend when the texture's alpha
				// CLASS is TRANSLUCENT", and that is WRONG for the same reason
				// §13/§14 already established: the class is a CONTENT HEURISTIC,
				// and the authored AlphaRef is the real signal. The trace line in
				// gl_texture.cpp even says so — "class is diagnostic only;
				// AlphaRef decides".
				//
				// Measured over one C01/C01S01 run with LT_TRACE_ALPHACLASS=1:
				//     550 TRANSLUCENT / 175 MASKED / 178 OPAQUE
				// i.e. the classifier calls MOST of the game translucent,
				// including CHARS/SKINS/CATECASUALHEAD.DTX and
				// Guns/Skins_PV/Katana.dtx. Turning that into blending would
				// smear a large fraction of the world.
				//
				// The AUTHORED signal is SURF_TRANSPARENT (de_world.h:62,
				// "(1<<3) // Translucent"), but it lives in the BSP SURFACE data,
				// not in the render data this file parses (§4: a section carries
				// texture names, a shader code and a texture-effect string — no
				// surface flags). Each triangle does carry a `polyIndex` into the
				// world's polygon list, so the real fix is to resolve
				// polyIndex -> surface flags and split sections on
				// SURF_TRANSPARENT. That is the next step, not this.
				static int s_nWorldBlend = -1;
				if (s_nWorldBlend < 0)
					s_nWorldBlend = getenv("LT_WORLDBLEND") ? 1 : 0;

				if (nTexName && s_nWorldBlend && cSection.m_pTexture &&
				    GLTex_GetAlphaClass(cSection.m_pTexture) == GLTEX_ALPHA_TRANSLUCENT)
				{
					// d3d_SetTranslucentObjectStates (d3d_draw.cpp:113-124):
					// blend SRC_ALPHA/INV_SRC_ALPHA with DEPTH WRITES OFF, so a
					// pane never occludes what is behind it.
					bBlend = true;
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					glDepthMask(GL_FALSE);
				}
			}

			bool bMultitex = (nTexName != 0 && nLMName != 0);
			bool bSaturate = glw_SaturateOn();

			// ★★ ENVIRONMENT MAPPING. Resolved from the AUTHORED texture type;
			// eligibility for the EnvMapAlpha variant follows the section's
			// authored SHADER CODE (4 = Lightmap_Texture), not whether a lightmap
			// happened to resolve -- that is the discriminator AllocShader uses.
			GLWEnvSetup cEnv;
			if (nTexName && !bLMOnly)
				cEnv = glw_ResolveEnvMap(cSection,
				                         cSection.m_nShaderCode == kPCShader_Lightmap_Texture ||
				                         cSection.m_nShaderCode == kPCShader_Lightmap ||
				                         cSection.m_nShaderCode == kPCShader_Lightmap_Dual);
			const bool bEnv = cEnv.Active();
			// Units in D3D's stage order: 0 base, 1 reflection, 2 lightmap,
			// 3 diffuse. Without a reflection the layout is the original 0/1.
			const GLenum eUnitLM      = bEnv ? GL_TEXTURE2 : GL_TEXTURE1;
			const GLenum eUnitEnv     = GL_TEXTURE1;
			const GLenum eUnitDiffuse = bMultitex ? GL_TEXTURE3 : GL_TEXTURE2;

			// ⚠️ ORDER MATTERS. The lightmap block below must run FIRST: without a
			// lightmap eUnitLM and eUnitDiffuse are the SAME unit (2), and its
			// `else glDisable(GL_TEXTURE_2D)` branch would switch the diffuse unit
			// straight back off -- a Gouraud reflected surface would then lose its
			// lighting entirely and render fullbright.
			glActiveTexture(eUnitLM);
			if (bMultitex)
			{
				glEnable(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, nLMName);
				if (bSaturate)
				{
					// texture x lightmap x 2 -- the D3D Saturate lightmap
					// blend (SRCBLEND=DESTCOLOR, DESTBLEND=SRCCOLOR).
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
					glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
					glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
					glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
					glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 2);
					glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
					glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
				}
				else
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			}
			else if (!bEnv)
				glDisable(GL_TEXTURE_2D);
			glActiveTexture(GL_TEXTURE0);

			if (bEnv)
			{
				// The reflection sits between base and lighting, and its op may be
				// an ADD -- so the 2x can no longer live on unit 0 or on the
				// reflection itself for a Gouraud surface. It moves to the diffuse
				// unit, which is where D3D's MODULATE2X actually is (stage 2). For a
				// lightmapped surface the 2x stays on the lightmap unit, exactly as
				// on the non-reflective path.
				glw_BeginEnvUnit(eUnitEnv, cEnv, false);
				glw_BeginDiffuseUnit(eUnitDiffuse, nTexName, bSaturate && !bMultitex);
			}

			bool bLit = (nLMName != 0);   // real (baked) lighting present

			// Stage 0: Gouraud sections get texture x vertexcolor x 2 under
			// Saturate (D3DTOP_MODULATE2X, d3d_rendershader_gouraud.cpp:195).
			// Lightmapped sections keep 1x here -- their 2x lives on stage 1;
			// doubling both would render at 4x.
			//
			// ⚠️ With a reflection in the chain unit 0 must emit the BARE base
			// texture (D3DTSS_COLOROP = SELECTARG1(TEXTURE), gouraud.cpp:1067):
			// the diffuse modulate has moved downstream past the env op, which
			// for EnvMapAlpha and ADDSIGNED is not commutative with it.
			if (bEnv)
			{
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
			}
			else if (bSaturate && !bLit && nTexName)
			{
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 2);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
			}
			else
			{
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
			}

			const uint32 *pIndices = &cBlock.m_aIndices[cSection.m_nStartIndex];
			glBegin(GL_TRIANGLES);
			for (uint32 nIdx = 0; nIdx < cSection.m_nTriCount * 3; ++nIdx)
			{
				const uint32 nVertIdx = pIndices[nIdx];
				const GLWVertex &cVert = cBlock.m_aVertices[nVertIdx];

				// ★ AUTHORED vertex lighting, not a heuristic. The old code
				// here shaded non-lightmapped sections with a fixed key light
				// floored at 140/255 -- so every Gouraud surface (most of the
				// world) rendered at >=55% brightness and "night looked like
				// day". The baked answer was in m_nColor all along; the
				// composed array adds the light groups' current colors on top,
				// which is what lets a light switch change these surfaces
				// (same ★ rule as AlphaRef/§14 and render styles).
				if (bLit)
				{
					// Lightmapped: the lightmap carries the light; D3D's
					// lightmap shader does not modulate vertex diffuse.
					glColor4ub(255, 255, 255, nObjectAlpha);
				}
				else if (nVertIdx * 3 + 2 < cBlock.m_aComposedColor.size())
				{
					glColor4ub(cBlock.m_aComposedColor[nVertIdx * 3 + 0],
					           cBlock.m_aComposedColor[nVertIdx * 3 + 1],
					           cBlock.m_aComposedColor[nVertIdx * 3 + 2],
					           nObjectAlpha);
				}
				else
				{
					glColor4ub(255, 255, 255, nObjectAlpha);
				}

				if (bLMOnly)
					glTexCoord2f(cVert.m_fU1, cVert.m_fV1);
				else if (nTexName)
					glTexCoord2f(cVert.m_fU0, cVert.m_fV0);
				if (bMultitex)
					glMultiTexCoord2f(eUnitLM, cVert.m_fU1, cVert.m_fV1);
				if (bEnv)
				{
					if (cEnv.IsEnv())
					{
						// ⚠️ GL_REFLECTION_MAP derives its coordinates from the
						// NORMAL. Without this call every vertex reflects the same
						// direction and the surface shows one flat colour -- which
						// reads as "the env map did not load", not as a missing
						// normal. The 44-byte world vertex carries it already (§4).
						glNormal3f(cVert.m_vNormal.x, cVert.m_vNormal.y, cVert.m_vNormal.z);
					}
					else
					{
						// A DETAIL layer takes the BASE UVs; the unit's texture
						// matrix applies the authored scale and rotation.
						glMultiTexCoord2f(eUnitEnv, cVert.m_fU0, cVert.m_fV0);
					}
				}

				glVertex3f(cVert.m_vPos.x, cVert.m_vPos.y, cVert.m_vPos.z);
			}
			glEnd();

			if (bEnv)
			{
				glw_EndDiffuseUnit(eUnitDiffuse);
				glw_EndEnvUnit(eUnitEnv, cEnv);
				if (bMultitex)
				{
					// The lightmap moved to unit 2 for this section; put unit 2
					// back the way the non-env path leaves unit 1, or the next
					// section inherits a live texture on a unit it never touches.
					glActiveTexture(eUnitLM);
					glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					glDisable(GL_TEXTURE_2D);
					glActiveTexture(GL_TEXTURE0);
				}
			}

			if (bBlend)
			{
				glDisable(GL_BLEND);
				glDepthMask(GL_TRUE);
			}
		}
	}
}

void GLWorld_Draw()
{
	if (!GLWorld_IsLoaded())
		return;

	glw_BeginDraw();
	glw_DrawWorld(*g_pMainWorld);
	glw_EndDraw();
}

static const GLWorld *glw_FindWorldModel(const char *pName)
{
	if (!pName || !pName[0])
		return 0;
	for (size_t i = 0; i < g_aWorldModels.size(); ++i)
	{
		if (stricmp(g_aWorldModels[i]->m_sName, pName) == 0)
			return g_aWorldModels[i];
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Sky (the sky-object list is per frame, from SceneDesc::m_SkyObjects)
// ---------------------------------------------------------------------------

#define GLW_MAX_SKY_OBJECTS 64
static LTObject *g_apSkyObjects[GLW_MAX_SKY_OBJECTS];
static int       g_nSkyObjects = 0;

void GLWorld_SetSkyObjects(LTObject **ppSkyObjects, int nCount)
{
	g_nSkyObjects = 0;
	if (!ppSkyObjects)
		return;
	for (int i = 0; i < nCount && g_nSkyObjects < GLW_MAX_SKY_OBJECTS; ++i)
	{
		if (ppSkyObjects[i])
			g_apSkyObjects[g_nSkyObjects++] = ppSkyObjects[i];
	}
}

static bool glw_IsSkyObject(const LTObject *pObj)
{
	for (int i = 0; i < g_nSkyObjects; ++i)
	{
		if (g_apSkyObjects[i] == pObj)
			return true;
	}
	return false;
}

void GLWorld_DrawSkyWorldModels()
{
	if (!GLWorld_IsLoaded() || g_nSkyObjects <= 0)
		return;

	glw_BeginDraw();
	// The sky is a backdrop: no depth interaction with the world.
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	static bool s_bLogged = false;
	bool bLog = !s_bLogged;
	s_bLogged = true;

	for (int i = 0; i < g_nSkyObjects; ++i)
	{
		LTObject *pObj = g_apSkyObjects[i];
		if (pObj->m_ObjectType != OT_WORLDMODEL || !(pObj->m_Flags & FLAG_VISIBLE))
		{
			if (bLog)
				fprintf(stderr, "[glw] sky object %d: type=%d flags=0x%x — skipped\n",
				        i, (int)pObj->m_ObjectType, pObj->m_Flags);
			continue;
		}

		// Sky world models draw UNTRANSFORMED (their geometry is authored in
		// sky space; the caller's sky camera provides the view) — D3D parity.
		const WorldBsp *pBsp = ((WorldModelInstance*)pObj)->GetOriginalBsp();
		const GLWorld *pWorld = pBsp ? glw_FindWorldModel(pBsp->m_WorldName) : 0;
		if (bLog && pWorld && !pWorld->m_aBlocks.empty())
		{
			const GLWBlock &cBlock = pWorld->m_aBlocks[0];
			fprintf(stderr, "[glw] sky WM '%s': %u blocks, block0 center (%.0f %.0f %.0f) half (%.0f %.0f %.0f)\n",
			        pBsp->m_WorldName, (uint32)pWorld->m_aBlocks.size(),
			        cBlock.m_vCenter.x, cBlock.m_vCenter.y, cBlock.m_vCenter.z,
			        cBlock.m_vHalfDims.x, cBlock.m_vHalfDims.y, cBlock.m_vHalfDims.z);
		}
		else if (bLog)
			fprintf(stderr, "[glw] sky WM '%s': NO render data\n",
			        pBsp ? pBsp->m_WorldName : "<null>");
		if (pWorld)
			glw_DrawWorld(*pWorld, false);   // no cutout test on sky layers
	}

	glDepthMask(GL_TRUE);
	glw_EndDraw();
}

void GLWorld_DrawWorldModels(bool bTranslucentPass)
{
	if (!GLWorld_IsLoaded() || !g_pClientMgr || g_aWorldModels.empty())
		return;

	// One-shot inventory (bring-up diagnostics). Only the SOLID call logs — it
	// still walks the entire OT_WORLDMODEL list, so the census stays complete,
	// and the translucent call would otherwise print every world model twice.
	static bool s_bLogged = false;
	bool bLog = !s_bLogged && !bTranslucentPass;
	if (!bTranslucentPass)
		s_bLogged = true;

	// One-shot census of client dynamic lights (OT_LIGHT): these are the lamp
	// posts' light pools — the render side for them is the Track-A dynamic-
	// light pass (not implemented yet).
	if (bLog)
	{
		uint32 nLights = 0;
		LTLink *pLightHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_LIGHT].m_Head;
		for (LTLink *pCur = pLightHead->m_pNext; pCur != pLightHead; pCur = pCur->m_pNext)
		{
			DynamicLight *pLight = (DynamicLight*)pCur->m_pData;
			if (!pLight)
				continue;
			++nLights;
			if (nLights <= 24)
				fprintf(stderr, "[glw] light @(%.0f %.0f %.0f) r=%.0f color=(%u %u %u) flags=0x%x\n",
				        pLight->m_Pos.x, pLight->m_Pos.y, pLight->m_Pos.z,
				        pLight->m_LightRadius,
				        (uint32)pLight->m_ColorR, (uint32)pLight->m_ColorG, (uint32)pLight->m_ColorB,
				        pLight->m_Flags);
		}
		fprintf(stderr, "[glw] %u dynamic lights (OT_LIGHT) in world\n", nLights);
	}

	glw_BeginDraw();

	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_WORLDMODEL].m_Head;
	// ⚠️ TWO PASSES, AND THEY ARE TWO SEPARATE CALLS WITH MODELS DRAWN BETWEEN
	// THEM. D3D gets this from its visible-set split (d3d_ProcessWorldModel ->
	// m_SolidWorldModels vs m_TranslucentWorldModels, drawworldmodel.cpp:20)
	// and d3d_FlushObjectQueues' order (drawobjects.cpp:218): solid world
	// models -> solid polygrids -> SOLID MODELS -> the whole translucent set.
	//
	// ★ The models-in-between part is not cosmetic. Everything IsTranslucent()
	// — including FLAG2_FORCETRANSLUCENT — is a translucent world model, and
	// C08S03's glass cage ('WallGlass01'..'04', 'TopGlassWM', 'FloorGlass01/02',
	// all rgba a=255 flags2=0x40) is exactly that. Running the whole world-model
	// walk before GLModel_DrawModels() let a pane blend over the already-drawn
	// BSP room (so the room still showed through) while writing depth, which
	// depth-rejected every CHARACTER behind it — symmetrically, from either side
	// of the cage. That was the reported "characters do not render through the
	// glass" bug.
	//
	// NOT YET DONE: D3D also SORTS the translucent set back-to-front
	// (ObjectDrawList). Overlapping panes can therefore composite in the wrong
	// order; single panes — the common case — are correct.
	{
	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		WorldModelInstance *pInstance = (WorldModelInstance*)pCur->m_pData;
		if (!pInstance || !(pInstance->m_Flags & FLAG_VISIBLE))
			continue;

		if (glw_IsSkyObject(pInstance))
			continue;   // drawn by the sky pass

		const WorldBsp *pBsp = pInstance->GetOriginalBsp();
		if (!pBsp)
			continue;

		// Only world models that carried render data exist in our list; the
		// rest (physics/vis-only BSPs) legitimately miss.
		const GLWorld *pWorld = glw_FindWorldModel(pBsp->m_WorldName);
		if (bLog)
			fprintf(stderr, "[glw] worldmodel '%s' @(%.0f %.0f %.0f): %s  rgba=(%u %u %u %u) flags=0x%x flags2=0x%x\n",
			        pBsp->m_WorldName, pInstance->m_Pos.x, pInstance->m_Pos.y,
			        pInstance->m_Pos.z, pWorld ? "drawing" : "no render data",
			        (unsigned)pInstance->m_ColorR, (unsigned)pInstance->m_ColorG,
			        (unsigned)pInstance->m_ColorB, (unsigned)pInstance->m_ColorA,
			        (unsigned)pInstance->m_Flags, (unsigned)pInstance->m_Flags2);
		if (!pWorld)
			continue;

		// Instance transform (model space -> world space). LTMatrix is
		// row-major; GL wants column-major — transpose into the GL stack.
		const LTMatrix &m = pInstance->m_Transform;
		float aGL[16];
		for (int nRow = 0; nRow < 4; ++nRow)
			for (int nCol = 0; nCol < 4; ++nCol)
				aGL[nCol * 4 + nRow] = m.m[nRow][nCol];

		// ★★ TWO KINDS OF "TRANSLUCENT", AND THEY NEED DIFFERENT STATE.
		//
		// LTObject::IsTranslucent() (de_objects.h:190) is
		//     m_ColorA < 255 || FLAG2_ADDITIVE || FLAG2_FORCETRANSLUCENT
		// and D3D routes ALL of those into its sorted translucent set. But the
		// three do NOT want the same depth behaviour, and conflating them
		// regresses previously-fixed content:
		//
		//  * m_ColorA < 255 / FLAG2_ADDITIVE = the object as a whole is see
		//    through. This is WINDOW AND DOOR GLASS. It needs real alpha
		//    blending with DEPTH WRITES OFF, drawn after the opaque world, or
		//    it hides whatever is behind it. This is what was missing: the
		//    object's alpha never even reached the vertex colour (glColor3ub),
		//    so glass composited at full opacity — i.e. looked solid.
		//
		//  * FLAG2_FORCETRANSLUCENT = "overall opaque, but has translucent
		//    PARTS" (the car's wheels, §15: props/skins/eurocar2.dtx is DXT5
		//    with transparent regions but no AlphaRef). ⚠️ DEPTH WRITES MUST
		//    STAY ON here — with them off the far side of a wheel drew straight
		//    through the near side and looked doubled. Keep discarding only the
		//    fully transparent texels so they neither blend nor write depth.
		//    ⚠️ This is a DELIBERATE deviation: D3D turns ZWRITE off for these
		//    too (d3d_SetTranslucentObjectStates, d3d_draw.cpp:114). Keeping it
		//    on is safe now only because the whole set draws AFTER the models —
		//    the depth it writes can no longer hide a character.
		const bool bAlphaBlended = (pInstance->m_ColorA < 255) ||
		                           (pInstance->m_Flags2 & FLAG2_ADDITIVE) != 0;
		const bool bForcedParts  = !bAlphaBlended &&
		                           (pInstance->m_Flags2 & FLAG2_FORCETRANSLUCENT) != 0;

		// LTObject::IsTranslucent() (de_objects.h:190) — the same predicate
		// d3d_ProcessWorldModel splits its visible set on. Solid models are
		// drawn by the caller between the two calls.
		if ((bAlphaBlended || bForcedParts) != bTranslucentPass)
			continue;

		if (bForcedParts)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glAlphaFunc(GL_GREATER, 0.0f);
			glEnable(GL_ALPHA_TEST);
		}
		else if (bAlphaBlended)
		{
			// d3d_SetTranslucentObjectStates (d3d_draw.cpp:102) + the additive
			// override in d3d_DrawTranslucentWorldModel (drawworldmodel.cpp:88).
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA,
			            (pInstance->m_Flags2 & FLAG2_ADDITIVE) ? GL_ONE
			                                                   : GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);          // D3DRS_ZWRITEENABLE, FALSE
		}

		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glMultMatrixf(aGL);
		glw_DrawWorld(*pWorld, true, pInstance->m_ColorA);
		glPopMatrix();

		if (bForcedParts)
		{
			glDisable(GL_BLEND);
			glDisable(GL_ALPHA_TEST);
		}
		else if (bAlphaBlended)
		{
			glDisable(GL_BLEND);
			glDepthMask(GL_TRUE);
		}
	}
	}

	glw_EndDraw();
}
