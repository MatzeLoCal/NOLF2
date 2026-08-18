// ----------------------------------------------------------------------- //
//
// MODULE  : world_renderdata.cpp
//
// PURPOSE : BSP world render data -- see world_renderdata.h. The stream walk below mirrors
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
#include "world_renderdata.h"
#include "mtl_device.h"   // MTLDev_IsMetalBackend -- the backend branch
#include "mtl_world.h"    // the Metal world draw + lightmap backend
#include "render_polygrid.h"   // RPolyGrid_ArmCensus (per-world census)
#include "model_renderdata.h"
#include "setupobject.h"        // LoadSprite (.spr-textured world sections)
#include "de_sprite.h"          // Sprite/SpriteAnim frame data
#include "sys/shared/render_texture.h"  // RTex_* — neutral texture queries
#include "sys/shared/render_globals.h"  // g_pRenderStruct — the engine function table

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

// ⚠️ RWVertex / RWSection / RWPendingLM / RWSubLM / RWLightGroup /
// RWBlock / RWorld NOW LIVE IN world_renderdata.h — the Metal renderer walks the
// same parsed data rather than duplicating this loader. See the header.

static RWorld           *g_pMainWorld = 0;
static std::vector<RWorld*> g_aWorldModels;   // parsed, not drawn yet (doors etc.)
static uint32             g_nTotalVerts = 0, g_nTotalTris = 0;


// ---------------------------------------------------------------------------
// Composed VERTEX lighting (Gouraud surfaces).
//
// Final vertex color = baked m_nColor + sum over light groups of
// (group current color * per-vertex RLE intensity), clamped per channel --
// byte-for-byte CD3D_RenderBlock::UpdateLightingData. Recomputed whenever a
// group's color changes; the RLE stream spans the block's ENTIRE vertex array.
// ---------------------------------------------------------------------------
static void rw_ComposeVertexColors(RWBlock &cBlock)
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
		const RWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
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
bool RWorld_SaturateOn()
{
	static int s_nSaturate = -1;
	if (s_nSaturate < 0)
	{
		s_nSaturate = 1;   // retail config default
		if (g_pRenderStruct && g_pRenderStruct->GetParameter && g_pRenderStruct->GetParameterValueFloat)
		{
			HLTPARAM hParam = g_pRenderStruct->GetParameter((char*)"Saturate");
			if (hParam)
				s_nSaturate = (g_pRenderStruct->GetParameterValueFloat(hParam) != 0.0f) ? 1 : 0;
		}
		fprintf(stderr, "[glw] Saturate (2x world lighting): %s\n", s_nSaturate ? "ON" : "OFF");
	}
	return s_nSaturate != 0;
}

static inline bool rw_SaturateOn() { return RWorld_SaturateOn(); }

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

	float rw_GetConVar(const char *pName, float fDefault);   // defined with the fog block

	bool rw_EnvMapDisabled()
	{
		static int s_n = -1;
		if (s_n < 0) { const char *p = getenv("LT_NO_ENVMAP"); s_n = (p && p[0] && p[0] != '0') ? 1 : 0; }
		return s_n != 0;
	}

	bool rw_DetailDisabled()
	{
		static int s_n = -1;
		if (s_n < 0) { const char *p = getenv("LT_NO_DETAIL"); s_n = (p && p[0] && p[0] != '0') ? 1 : 0; }
		return s_n != 0;
	}

}

void RWorld_SetCamera(const LTVector &vRight, const LTVector &vUp,
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
	enum ERWSecondKind { kSecond_None = 0, kSecond_EnvMap, kSecond_Detail };
	enum ERWEnvMode    { kEnv_None = 0, kEnv_Modulate, kEnv_AddSigned, kEnv_AlphaAdd };

	struct RWEnvSetup
	{
		SharedTexture  *m_pTex;      // the linked reflection / detail texture
		unsigned        m_nName;     // nonzero = resolved (0 = unusable -> no layer)
		bool            m_bCube;     // DTX_CUBEMAP: 3-coord transform, cube target
		ERWSecondKind  m_eKind;
		ERWEnvMode     m_eMode;
		float           m_fScale, m_fCos, m_fSin;   // detail placement

		RWEnvSetup()
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
	RWEnvSetup rw_ResolveEnvMap(const RWSection &cSection, bool bAuthoredLightmap)
	{
		RWEnvSetup cOut;
		if (!cSection.m_pTexture)
			return cOut;

		switch (cSection.m_pTexture->m_eTexType)
		{
			case eSharedTexType_EnvMap:
			case eSharedTexType_EnvBumpMap:
			case eSharedTexType_DOT3EnvBumpMap:
			case eSharedTexType_EnvMapAlpha:
			{
				if (rw_EnvMapDisabled())
					return cOut;
				// "EnvMapEnable" -- retail's autoexec.cfg ships it as 1.
				static int s_nEnvEnable = -1;
				if (s_nEnvEnable < 0)
					s_nEnvEnable = (rw_GetConVar("EnvMapEnable", 1.0f) != 0.0f) ? 1 : 0;
				if (!s_nEnvEnable)
					return cOut;

				if (cSection.m_pTexture->m_eTexType == eSharedTexType_EnvMapAlpha)
				{
					if (bAuthoredLightmap)
						return cOut;             // see the ⚠️ above
					// ★ ALWAYS AlphaAdd. This used to ask GL for
					// GL_ATI_texture_env_combine3 -- a query that needs a GL
					// CONTEXT. On the Metal path there is none, so glGetString
					// returned NULL, the extension read as "missing", and every
					// EnvMapAlpha surface silently fell back to Modulate while the
					// GL reference (which had the extension -- verified on Apple's
					// GL 2.1 / M2) used AlphaAdd. Textbook lesson-5 bug: a GL-only
					// call reached from shared code. The fixed-function extension
					// was only ever a way to EMULATE D3DTOP_MODULATEALPHA_ADDCOLOR;
					// the Metal shader implements it directly and has no such limit.
					cOut.m_eMode = kEnv_AlphaAdd;
				}
				else
				{
					// D3DTOP_ADDSIGNED vs D3DTOP_MODULATE is the "EnvMapAdd" console
					// variable, and its RENDERER DEFAULT IS 1 (rendererconsolevars.h:68)
					// -- retail's config does not override it, so the shipping look is
					// ADDSIGNED (base + env - 0.5), not a multiply. Assuming MODULATE
					// here would have made every reflection far too dark.
					cOut.m_eMode = (rw_GetConVar("EnvMapAdd", 1.0f) != 0.0f)
					             ? kEnv_AddSigned : kEnv_Modulate;
				}
				cOut.m_eKind = kSecond_EnvMap;
				break;
			}

			case eSharedTexType_Detail:
			{
				if (rw_DetailDisabled())
					return cOut;
				// "DetailTextures" (retail config: 1). AllocShader falls back to
				// eShader_Gouraud_Texture when it is off, i.e. base texture only.
				static int s_nDetailEnable = -1;
				if (s_nDetailEnable < 0)
					s_nDetailEnable = (rw_GetConVar("DetailTextures", 1.0f) != 0.0f) ? 1 : 0;
				if (!s_nDetailEnable)
					return cOut;

				// ⚠️ Same authored default as the env map, different variable:
				// "DetailTextureAdd" also DEFAULTS TO 1 -> D3DTOP_ADDSIGNED. A
				// detail texture is authored around mid-grey so that
				// `base + detail - 0.5` perturbs the surface without tinting it;
				// MODULATE would darken every one of these surfaces instead.
				cOut.m_eMode = (rw_GetConVar("DetailTextureAdd", 1.0f) != 0.0f)
				             ? kEnv_AddSigned : kEnv_Modulate;
				cOut.m_eKind = kSecond_Detail;

				// The placement is the BASE texture's, times the global
				// "DetailTextureScale" -- which retail's autoexec.cfg overrides to
				// 1.0, NOT the renderer default of 0.2. Reading the renderer
				// default here would tile the detail five times too coarsely.
				// ⚠️ NEUTRAL QUERY. GLTex_GetDetailParams reads the GL entry off
				// SharedTexture::m_pRenderData, which under Metal holds an
				// MTLTexEntry -- so the authored scale and rotation came back as
				// GARBAGE and every detail layer was placed wrongly. Third time
				// this family has bitten (§88, §89); the neutral accessors exist
				// precisely so shared code cannot reach the GL ones.
				float fTexScale = 1.0f, fCos = 1.0f, fSin = 0.0f;
				RTex_GetDetailParams(cSection.m_pTexture, fTexScale, fCos, fSin);
				cOut.m_fScale = fTexScale * rw_GetConVar("DetailTextureScale", 1.0f);
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
		// Neutral queries again. m_nName is now purely "did this texture
		// resolve?" — it carried the GL texture name when there was a GL draw
		// to hand it to, and the `if (!cOut.m_nName)` below is what it is for.
		cOut.m_nName = RTex_IsValid(cOut.m_pTex) ? 1u : 0u;
		cOut.m_bCube = cOut.IsEnv() && RTex_IsCubeMap(cOut.m_pTex);
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
	bool rw_TraceFog()
	{
		static int s_nTrace = -1;
		if (s_nTrace < 0)
			s_nTrace = getenv("LT_TRACE_FOG") ? 1 : 0;
		return s_nTrace != 0;
	}

	float rw_GetConVar(const char *pName, float fDefault)
	{
		if (!g_pRenderStruct || !g_pRenderStruct->GetParameter || !g_pRenderStruct->GetParameterValueFloat)
			return fDefault;
		HLTPARAM hParam = g_pRenderStruct->GetParameter((char*)pName);
		if (!hParam)
		{
			if (rw_TraceFog())
				fprintf(stderr, "[glw] convar '%s' MISSING, using %.1f\n", pName, fDefault);
			return fDefault;
		}
		float fVal = g_pRenderStruct->GetParameterValueFloat(hParam);
		if (rw_TraceFog())
			fprintf(stderr, "[glw] convar '%s' = %.3f\n", pName, fVal);
		return fVal;
	}

	bool  s_bFogOn = false;
	float s_fFogR = 1.0f, s_fFogG = 1.0f, s_fFogB = 1.0f;
	// The live RANGE too. The per-object fog override (§31) has to re-state the
	// whole fog to the Metal backend, which is handed near/far explicitly rather
	// than inheriting them from GL state the way glFogf left them standing.
	float s_fFogNear = 0.0f, s_fFogFar = 2000.0f;
}

void RWorld_ApplyFog(bool bSky)
{
	// Engine default is FogEnable 0; the level turns it on.
	s_bFogOn = (rw_GetConVar("FogEnable", 0.0f) != 0.0f);

	float fNear = rw_GetConVar(bSky ? "SkyFogNearZ" : "FogNearZ", 0.0f);
	float fFar  = rw_GetConVar(bSky ? "SkyFogFarZ"  : "FogFarZ",  2000.0f);

	// d3d_ReadExtraConsoleVariables kills fog when the range is degenerate
	// ("This handles a TNT bug if the near and far Z are the same") -- and
	// GL would divide by zero here for exactly the same reason.
	if (fFar <= fNear)
		s_bFogOn = false;

	// ★ LT_NO_FOG=1 -- bisection only: "is this difference the fog term?".
	// ⚠️ IT MUST BE RESOLVED HERE, ABOVE THE BACKEND BRANCH. It used to be
	// applied inside MTLWorld_SetFog, so it turned fog off under Metal and left
	// the GL reference build fogging -- every measurement taken with it was a
	// fogged frame against an unfogged one. An isolation switch that only one
	// backend obeys is worse than no switch at all: it produces numbers.
	static int s_nNoFog = -1;
	if (s_nNoFog < 0)
	{
		const char *pNoFog = getenv("LT_NO_FOG");
		s_nNoFog = (pNoFog && pNoFog[0] && pNoFog[0] != '0') ? 1 : 0;
	}
	if (s_nNoFog)
		s_bFogOn = false;

	if (!s_bFogOn)
	{
		MTLWorld_SetFog(false, 0, 0, 0, 0, 1);
		return;
	}

	s_fFogR = rw_GetConVar("FogR", 255.0f) / 255.0f;
	s_fFogG = rw_GetConVar("FogG", 255.0f) / 255.0f;
	s_fFogB = rw_GetConVar("FogB", 255.0f) / 255.0f;
	s_fFogNear = fNear;
	s_fFogFar  = fFar;

	// The same GL_LINEAR range fog the fixed-function path asked for, evaluated
	// per fragment in the world shader (§103 -- per VERTEX was the fog bug).
	MTLWorld_SetFog(true, s_fFogR, s_fFogG, s_fFogB, fNear, fFar);

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

void RWorld_DisableFog()
{
	s_bFogOn = false;
	MTLWorld_SetFog(false, 0, 0, 0, 0, 1);
}

bool RWorld_GetFogColor(float &fR, float &fG, float &fB)
{
	fR = s_fFogR; fG = s_fFogG; fB = s_fFogB;
	return s_bFogOn;
}

void RWorld_ApplyObjectFog(uint32 nFlags, uint32 nFlags2)
{
	if (!s_bFogOn || (nFlags & FLAG_FOGDISABLE))
	{
		MTLWorld_SetFog(false, 0, 0, 0, s_fFogNear, s_fFogFar);
		return;
	}

	float fR = s_fFogR, fG = s_fFogG, fB = s_fFogB;
	if (nFlags2 & FLAG2_ADDITIVE)
		fR = fG = fB = 0.0f;
	else if (nFlags2 & FLAG2_MULTIPLY)
		fR = fG = fB = 1.0f;

	MTLWorld_SetFog(true, fR, fG, fB, s_fFogNear, s_fFogFar);
}

void RWorld_RestoreSceneFog()
{
	if (!s_bFogOn)
	{
		MTLWorld_SetFog(false, 0, 0, 0, s_fFogNear, s_fFogFar);
		return;
	}
	MTLWorld_SetFog(true, s_fFogR, s_fFogG, s_fFogB, s_fFogNear, s_fFogFar);
}

// ---------------------------------------------------------------------------
// Stream helpers (parse-and-discard for the parts the GL path doesn't keep).
// ---------------------------------------------------------------------------

static void rw_SkipBytes(ILTStream *pStream, uint32 nBytes)
{
	// ILTStream has no generic skip; seek relative to the current position.
	uint32 nPos = 0;
	pStream->GetPos(&nPos);
	pStream->SeekTo(nPos + nBytes);
}

// SRBGeometryPoly: u8 vertCount, vertCount * LTVector, plane (normal + dist).
static void rw_SkipGeometryPoly(ILTStream *pStream)
{
	uint8 nVertCount;
	*pStream >> nVertCount;
	rw_SkipBytes(pStream, (uint32)nVertCount * sizeof(LTVector) + sizeof(LTVector) + sizeof(float));
}

// (Light groups are parsed and composed in rw_LoadBlock — see the loader.)

// ---------------------------------------------------------------------------
// Lightmap upload. The stream carries the RLE-compressed 24-bit RGB map; the
// D3D renderer pads it into a pow2 texture without rescaling UVs, so UV1 in
// the file is relative to the padded size — replicate that exactly.
// ---------------------------------------------------------------------------

static uint32 rw_NextPow2(uint32 n)
{
	uint32 nPow2 = 1;
	while (nPow2 < n)
		nPow2 <<= 1;
	return nPow2;
}

// --- the backend seam (declared in world_renderdata.h) ------------------------
// The loader owns WHEN a lightmap exists; the backend owns WHAT it is. Metal
// pads to pow2 exactly as the GL path does, because UV1 in the file is already
// relative to the padded size (D3D never rescaled it).
uintptr_t RWorldLM_Create(const uint8 *pBGR, uint32 nWidth, uint32 nHeight)
{
	return MTLWorld_CreateLightmap(pBGR, nWidth, nHeight, rw_NextPow2(nWidth),
	                               rw_NextPow2(nHeight));
}

void RWorldLM_Update(uintptr_t hTex, const uint8 *pBGR, uint32 nWidth, uint32 nHeight)
{
	if (!hTex || !pBGR)
		return;
	MTLWorld_UpdateLightmap(hTex, pBGR, nWidth, nHeight);
}

void RWorldLM_Destroy(uintptr_t hTex)
{
	if (!hTex)
		return;
	MTLWorld_DestroyLightmap(hTex);
}

// Add one light group's RLE sub-lightmap rectangle into a decompressed
// lightmap: per texel, add color * intensity, clamped per channel. Byte-for-
// byte port of the loop in CD3D_RenderBlock's light-group update (0xFF in the
// intensity stream escapes a run: 0xFF, count, value).
static void rw_AddSubLM(RWPendingLM &cLM, const LTVector &vColor,
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

static bool rw_LoadBlock(ILTStream *pStream, RWBlock &cBlock)
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
	std::vector<RWPendingLM> &aPendingLMs = cBlock.m_aBaseLMs;
	uint32 nIndexOffset = 0;
	for (uint32 nSection = 0; nSection < nSectionCount; ++nSection)
	{
		char sTexName[2][MAX_PATH + 1];
		for (uint32 nTex = 0; nTex < 2; ++nTex)          // CRBSection::kNumTextures == 2
			pStream->ReadString(sTexName[nTex], sizeof(sTexName[nTex]));

		RWSection cSection;
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
		else if (pTex0[0] && g_pRenderStruct && g_pRenderStruct->GetSharedTexture)
		{
			cSection.m_pTexture = g_pRenderStruct->GetSharedTexture(pTex0);
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
		cSection.m_hLMTexture = 0;
		if (nLMSize && nLMWidth && nLMHeight &&
		    nLMWidth * nLMHeight <= (uint32)LIGHTMAP_MAX_TOTAL_PIXELS)
		{
			std::vector<uint8> aCompressed(nLMSize);
			pStream->Read(&aCompressed[0], nLMSize);

			RWPendingLM &cLM = aPendingLMs[nSection];
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
			rw_SkipBytes(pStream, nLMSize);

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
		RWVertex &cVert = cBlock.m_aVertices[nVert];
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
		rw_SkipGeometryPoly(pStream);

	// Occluders (a geometry poly + uint32 id)
	uint32 nOccluderCount;
	*pStream >> nOccluderCount;
	for (; nOccluderCount; --nOccluderCount)
	{
		rw_SkipGeometryPoly(pStream);
		rw_SkipBytes(pStream, sizeof(uint32));
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
		RWLightGroup cGroup;

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
				RWSubLM cSub;
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
	// fixed key light in rw_DrawWorld).
	rw_ComposeVertexColors(cBlock);

	// Compose (base + light groups at their current colors) and upload.
	for (uint32 nSection = 0; nSection < nSectionCount && nSection < cBlock.m_aSections.size(); ++nSection)
	{
		const RWPendingLM &cBase = aPendingLMs[nSection];
		if (cBase.m_aData.empty())
			continue;

		RWPendingLM cComposed = cBase;   // scratch copy; base stays pristine
		for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
		{
			const RWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
			for (size_t nSub = 0; nSub < cGroup.m_aSubLMs.size(); ++nSub)
			{
				const RWSubLM &cSub = cGroup.m_aSubLMs[nSub];
				if (cSub.m_nSection == nSection)
					rw_AddSubLM(cComposed, cGroup.m_vColor,
					             cSub.m_nLeft, cSub.m_nTop,
					             cSub.m_nWidth, cSub.m_nHeight,
					             &cSub.m_aData[0], (uint32)cSub.m_aData.size());
			}
		}
		cBlock.m_aSections[nSection].m_hLMTexture =
			RWorldLM_Create(&cComposed.m_aData[0], cComposed.m_nWidth, cComposed.m_nHeight);
	}

	// Children: u8 flags + k_NumChildren(2) * u32 index (tree unused in GL yet)
	uint8 nChildFlags;
	*pStream >> nChildFlags;
	rw_SkipBytes(pStream, 2 * sizeof(uint32));

	return true;
}

static bool rw_LoadWorld(ILTStream *pStream, RWorld &cWorld)
{
	uint32 nBlockCount;
	*pStream >> nBlockCount;

	cWorld.m_aBlocks.resize(nBlockCount);
	for (uint32 nBlock = 0; nBlock < nBlockCount; ++nBlock)
	{
		if (!rw_LoadBlock(pStream, cWorld.m_aBlocks[nBlock]))
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

		RWorld *pWM = new RWorld;
		LTStrCpy(pWM->m_sName, sWMName, sizeof(pWM->m_sName));
		if (!rw_LoadWorld(pStream, *pWM))
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

bool RWorld_Load(ILTStream *pStream)
{
	RModel_ArmCensus();     // report the object census for THIS world
	RPolyGrid_ArmCensus();  // ...and the polygrid inventory
	RWorld_Free();

	g_pMainWorld = new RWorld;
	g_nTotalVerts = g_nTotalTris = 0;

	if (!rw_LoadWorld(pStream, *g_pMainWorld))
	{
		fprintf(stderr, "[glw] world render-data load FAILED\n");
		RWorld_Free();
		return false;
	}

	fprintf(stderr, "[glw] world render data loaded: %u blocks, %u verts, %u tris, %u worldmodels\n",
	        (uint32)g_pMainWorld->m_aBlocks.size(), g_nTotalVerts, g_nTotalTris,
	        (uint32)g_aWorldModels.size());

	// How many sections ended the load carrying a lightmap handle. Separates
	// "the lightmaps were never created" from "the draw is not seeing them" --
	// the two look identical on screen (a flat, unlit world).
	{
		uint32 nWithLM = 0, nSections = 0;
		for (size_t nB = 0; nB < g_pMainWorld->m_aBlocks.size(); ++nB)
			for (size_t nS = 0; nS < g_pMainWorld->m_aBlocks[nB].m_aSections.size(); ++nS)
			{
				++nSections;
				if (g_pMainWorld->m_aBlocks[nB].m_aSections[nS].m_hLMTexture)
					++nWithLM;
			}
		fprintf(stderr, "[glw] main world: %u sections, %u carry a lightmap handle\n",
		        nSections, nWithLM);
	}

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
			const RWorld *pW = (w == 0) ? g_pMainWorld : g_aWorldModels[w - 1];
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
					const RWorld *pW3 = (w == 0) ? g_pMainWorld : g_aWorldModels[w - 1];
					if (!pW3) continue;
					for (size_t b = 0; b < pW3->m_aBlocks.size(); ++b)
						for (size_t n = 0; n < pW3->m_aBlocks[b].m_aSections.size(); ++n)
						{
							const RWSection &cS = pW3->m_aBlocks[b].m_aSections[n];
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
				const RWorld *pW2 = (w == 0) ? g_pMainWorld : g_aWorldModels[w - 1];
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
			RWorld *pWorld = (nWorld == 0) ? g_pMainWorld : g_aWorldModels[nWorld - 1];
			for (size_t nBlock = 0; pWorld && nBlock < pWorld->m_aBlocks.size(); ++nBlock)
			{
				RWBlock &cBlock = pWorld->m_aBlocks[nBlock];
				for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
				{
					RWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
					++nGroups;
					if (bInventory)
						fprintf(stderr, "[glw] lightgroup id=0x%08x color=(%.2f %.2f %.2f) sublms=%u "
						                "vtxRLE=%u %s\n",
						        cGroup.m_nID, cGroup.m_vColor.x, cGroup.m_vColor.y, cGroup.m_vColor.z,
						        (uint32)cGroup.m_aSubLMs.size(),
						        (uint32)cGroup.m_aVertexIntensities.size(),
						        pWorld->m_sName[0] ? pWorld->m_sName : "(main)");
					// ⚠️ A group with NO sublms but a vertex RLE stream is the
					// Gouraud-only case — precisely the one the Metal colour
					// upload used to miss, so it must NOT be filtered out here.
					if (bForceOn && cGroup.m_vColor.x < 0.02f &&
					    cGroup.m_vColor.y < 0.02f && cGroup.m_vColor.z < 0.02f &&
					    (!cGroup.m_aSubLMs.empty() || !cGroup.m_aVertexIntensities.empty()))
						aForceIDs.push_back(cGroup.m_nID);
				}
			}
		}
		fprintf(stderr, "[glw] %u light groups total\n", nGroups);
		for (size_t n = 0; n < aForceIDs.size(); ++n)
			RWorld_SetLightGroupColor(aForceIDs[n], LTVector(1.0f, 1.0f, 1.0f));
		if (bForceOn)
			fprintf(stderr, "[glw] TEST: forced %u black-authored light groups WHITE\n",
			        (uint32)aForceIDs.size());
	}
	return true;
}

// Recompose one section's lightmap (base + every group's current color) and
// re-upload it into the existing GL texture.
static void rw_RecomposeSectionLM(RWBlock &cBlock, uint32 nSection)
{
	if (nSection >= cBlock.m_aSections.size() || nSection >= cBlock.m_aBaseLMs.size())
		return;
	const RWPendingLM &cBase = cBlock.m_aBaseLMs[nSection];
	uintptr_t hTexture = cBlock.m_aSections[nSection].m_hLMTexture;
	if (cBase.m_aData.empty() || !hTexture)
		return;

	RWPendingLM cComposed = cBase;
	for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
	{
		const RWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
		for (size_t nSub = 0; nSub < cGroup.m_aSubLMs.size(); ++nSub)
		{
			const RWSubLM &cSub = cGroup.m_aSubLMs[nSub];
			if (cSub.m_nSection == nSection)
				rw_AddSubLM(cComposed, cGroup.m_vColor,
				             cSub.m_nLeft, cSub.m_nTop, cSub.m_nWidth, cSub.m_nHeight,
				             &cSub.m_aData[0], (uint32)cSub.m_aData.size());
		}
	}

	RWorldLM_Update(hTexture, &cComposed.m_aData[0], cComposed.m_nWidth, cComposed.m_nHeight);
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

struct RWDynLight
{
	LTVector m_vPos;
	float    m_fRadius;
	LTVector m_vColor;   // 0..1
};

// The LT_TEST_LIGHTS injection set, parsed once. ★ Shared with the MODEL
// lighting pass (§71) through RWorld_GetTestDynLights: both passes must see
// the same lights or an A/B of world-vs-model lighting compares two scenes.
uint32 RWorld_GetTestDynLights(const RWTestDynLight **ppOut)
{
	static bool s_bParsed = false;
	static std::vector<RWTestDynLight> s_aTestLights;

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
				RWTestDynLight cLight;
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

static void rw_CollectDynamicLights(std::vector<RWDynLight> &aLights)
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
			RWDynLight cLight;
			cLight.m_vPos    = pLight->m_Pos;
			cLight.m_fRadius = pLight->m_LightRadius;
			cLight.m_vColor.Init(pLight->m_ColorR * (1.0f / 255.0f),
			                     pLight->m_ColorG * (1.0f / 255.0f),
			                     pLight->m_ColorB * (1.0f / 255.0f));
			aLights.push_back(cLight);
		}
	}

	const RWTestDynLight *pTest = 0;
	const uint32 nTest = RWorld_GetTestDynLights(&pTest);
	for (uint32 n = 0; n < nTest; ++n)
	{
		RWDynLight cLight;
		cLight.m_vPos    = pTest[n].m_vPos;
		cLight.m_fRadius = pTest[n].m_fRadius;
		cLight.m_vColor  = pTest[n].m_vColor255 * (1.0f / 255.0f);
		aLights.push_back(cLight);
	}
}

static const RWorld *rw_FindWorldModel(const char *pName);
static bool rw_IsSkyObject(const LTObject *pObject);

void RWorld_DrawDynamicLights()
{
	if (!g_pMainWorld)
		return;

	std::vector<RWDynLight> aLights;
	rw_CollectDynamicLights(aLights);

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
	// light on visible surfaces only. Under Metal all of this is per-draw state
	// carried by RWDrawParams (blend AddOne, depth LEQUAL, no write, no cull).

	for (size_t nLight = 0; nLight < aLights.size(); ++nLight)
	{
		const RWDynLight &cLight = aLights[nLight];
		{
			RWDynLightDesc cDesc;
			cDesc.m_vPos    = cLight.m_vPos;
			cDesc.m_fRadius = cLight.m_fRadius;
			cDesc.m_vColor  = cLight.m_vColor;
			RWDrawParams cP;
			cP.m_pDynLight   = &cDesc;
			cP.m_eBlend      = kRWBlend_AddOne;
			cP.m_bDepthEqual = true;
			cP.m_bDepthWrite = false;
			MTLWorld_DrawWorld(g_pMainWorld, &cP);
		}

		// World models: light their object-space geometry too. The instance's
		// back-transform moves the light into model space; the model's own
		// transform goes on the GL stack exactly like the WM draw pass.
		if (g_pClientMgr)
		{
			LTLink *pWMHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_WORLDMODEL].m_Head;
			for (LTLink *pCur = pWMHead->m_pNext; pCur != pWMHead; pCur = pCur->m_pNext)
			{
				WorldModelInstance *pInstance = (WorldModelInstance*)pCur->m_pData;
				if (!pInstance || !(pInstance->m_Flags & FLAG_VISIBLE) || rw_IsSkyObject(pInstance))
					continue;
				const WorldBsp *pBsp = pInstance->GetOriginalBsp();
				const RWorld *pWorld = pBsp ? rw_FindWorldModel(pBsp->m_WorldName) : 0;
				if (!pWorld)
					continue;

				RWDynLight cObjLight = cLight;
				pInstance->m_BackTransform.Apply(cLight.m_vPos, cObjLight.m_vPos);

				const LTMatrix &m = pInstance->m_Transform;
				float aGL[16];
				for (int nRow = 0; nRow < 4; ++nRow)
					for (int nCol = 0; nCol < 4; ++nCol)
						aGL[nCol * 4 + nRow] = m.m[nRow][nCol];

				{
					RWDynLightDesc cDesc;
					cDesc.m_vPos    = cObjLight.m_vPos;   // already back-transformed
					cDesc.m_fRadius = cObjLight.m_fRadius;
					cDesc.m_vColor  = cObjLight.m_vColor;
					RWDrawParams cP;
					cP.m_pDynLight    = &cDesc;
					cP.m_pModelMatrix = aGL;
					cP.m_eBlend       = kRWBlend_AddOne;
					cP.m_bDepthEqual  = true;
					cP.m_bDepthWrite  = false;
					MTLWorld_DrawWorld(pWorld, &cP);
				}
			}
		}
	}

}

// Light one parsed render world's blocks with one light (light already in the
// world's own space; caller owns any instance transform).
bool RWorld_SetLightGroupColor(uint32 nID, const LTVector &vColor)
{
	if (!g_pMainWorld)
		return false;

	bool bRecomposed = false;

	// Main world + every world model (D3D walks both the same way).
	for (size_t nWorld = 0; nWorld < g_aWorldModels.size() + 1; ++nWorld)
	{
		RWorld *pWorld = (nWorld == 0) ? g_pMainWorld : g_aWorldModels[nWorld - 1];
		if (!pWorld)
			continue;
		for (size_t nBlock = 0; nBlock < pWorld->m_aBlocks.size(); ++nBlock)
		{
			RWBlock &cBlock = pWorld->m_aBlocks[nBlock];
			for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
			{
				RWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
				if (cGroup.m_nID != nID)
					continue;
				cGroup.m_vColor = vColor;

				// Recompose the block's VERTEX lighting (Gouraud surfaces) --
				// this is what makes a light switch visibly change the world.
				rw_ComposeVertexColors(cBlock);
				bRecomposed = true;

				// Recompose each section this group touches (once each).
				for (size_t nSub = 0; nSub < cGroup.m_aSubLMs.size(); ++nSub)
				{
					uint32 nSection = cGroup.m_aSubLMs[nSub].m_nSection;
					bool bAlreadyDone = false;
					for (size_t nPrev = 0; nPrev < nSub; ++nPrev)
						if (cGroup.m_aSubLMs[nPrev].m_nSection == nSection)
							{ bAlreadyDone = true; break; }
					if (!bAlreadyDone)
						rw_RecomposeSectionLM(cBlock, nSection);
				}
				// A name appears at most once per block; keep scanning other
				// blocks/worlds — the same group can span several.
				break;
			}
		}
	}

	// ⚠️ GL needs no notification -- rw_ComposeVertexColors writes the array
	// the draw hands to glColor3f every frame. Metal keeps the colours in a
	// per-block buffer, so the recompose has to be published. Doing it HERE and
	// not from RWorldLM_Update is the whole point: a light group that touches
	// only Gouraud surfaces has no lightmapped section, so the lightmap path
	// never runs for it and its colours were never re-uploaded.
	if (bRecomposed && MTLDev_IsMetalBackend())
		MTLWorld_InvalidateVertexColors();

	return true;
}

// ⚠️ TEST HOOK for the light-group path, driven from nr_RenderScene by
// LT_TEST_LIGHTGROUPS_AT=<scene frame>. It exists because NOTHING else can
// exercise the Metal vertex-colour upload headlessly:
//   * the load-time LT_TEST_LIGHTGROUPS_ON runs before a single GPU buffer
//     exists, so the very first fill already carries the new colours;
//   * the null shell drops the server's LightGroup messages, so the real switch
//     never fires here.
// It deliberately targets GROUPS WITH NO SUB-LIGHTMAPS — the pure-Gouraud class
// that has no lightmap upload to piggyback on, which is exactly the class the
// Metal colour buffers used to miss — and switches them OFF rather than on. Off
// is the loud direction: no retail world sampled here ships a black-authored
// group, so forcing them white barely moves a pixel (max 5/255 on c08s03), while
// removing their contribution is the in-game "someone hit the light switch".
uint32 RWorld_TestSwitchGouraudLightGroupsOff(void)
{
	std::vector<uint32> aIDs;
	for (size_t nWorld = 0; nWorld < g_aWorldModels.size() + 1; ++nWorld)
	{
		RWorld *pWorld = (nWorld == 0) ? g_pMainWorld : g_aWorldModels[nWorld - 1];
		for (size_t nBlock = 0; pWorld && nBlock < pWorld->m_aBlocks.size(); ++nBlock)
		{
			RWBlock &cBlock = pWorld->m_aBlocks[nBlock];
			for (size_t nGroup = 0; nGroup < cBlock.m_aLightGroups.size(); ++nGroup)
			{
				const RWLightGroup &cGroup = cBlock.m_aLightGroups[nGroup];
				if (cGroup.m_aSubLMs.empty() && !cGroup.m_aVertexIntensities.empty())
					aIDs.push_back(cGroup.m_nID);
			}
		}
	}
	for (size_t n = 0; n < aIDs.size(); ++n)
		RWorld_SetLightGroupColor(aIDs[n], LTVector(0.0f, 0.0f, 0.0f));
	return (uint32)aIDs.size();
}

static void rw_ReleaseTextures(RWorld *pWorld)
{
	if (!pWorld)
		return;
	for (size_t nBlock = 0; nBlock < pWorld->m_aBlocks.size(); ++nBlock)
	{
		std::vector<RWSection> &aSections = pWorld->m_aBlocks[nBlock].m_aSections;
		for (size_t nSection = 0; nSection < aSections.size(); ++nSection)
		{
			SharedTexture *pTexture = aSections[nSection].m_pTexture;
			if (pTexture && pTexture->GetRefCount() > 0)
				pTexture->SetRefCount(pTexture->GetRefCount() - 1);

			if (aSections[nSection].m_hLMTexture)
			{
				RWorldLM_Destroy(aSections[nSection].m_hLMTexture);
				aSections[nSection].m_hLMTexture = 0;
			}
		}
	}
}

void RWorld_Free()
{
	// ⚠️ BEFORE the worlds are deleted: the per-block GPU handles live on the
	// blocks, and the lightmap handles are released through rw_ReleaseTextures
	// just below.
	rw_ReleaseTextures(g_pMainWorld);
	delete g_pMainWorld;
	g_pMainWorld = 0;
	for (size_t i = 0; i < g_aWorldModels.size(); ++i)
	{
		rw_ReleaseTextures(g_aWorldModels[i]);
		delete g_aWorldModels[i];
	}
	g_aWorldModels.clear();
	g_nTotalVerts = g_nTotalTris = 0;
	if (MTLDev_IsMetalBackend())
		MTLWorld_Free();
}

// The backend-neutral view of rw_ResolveEnvMap: same gates, same console
// variables, no GL texture name (which would be garbage under Metal, §88).
bool RWorld_ResolveSecondLayer(const RWSection &cSection, bool bAuthoredLightmap,
                                RWSecondLayer *pOut)
{
	if (!pOut)
		return false;
	*pOut = RWSecondLayer();

	RWEnvSetup cEnv = rw_ResolveEnvMap(cSection, bAuthoredLightmap);
	if (cEnv.m_eKind == kSecond_None || !cEnv.m_pTex || !RTex_IsValid(cEnv.m_pTex))
		return false;

	pOut->m_pTex  = cEnv.m_pTex;
	pOut->m_eKind = cEnv.IsEnv() ? kRWSecond_EnvMap : kRWSecond_Detail;
	pOut->m_eMode = (cEnv.m_eMode == kEnv_AddSigned) ? kRWSecondMode_AddSigned
	              : (cEnv.m_eMode == kEnv_AlphaAdd)  ? kRWSecondMode_AlphaAdd
	                                                 : kRWSecondMode_Modulate;
	pOut->m_bCube  = cEnv.m_bCube;
	pOut->m_fScale = cEnv.m_fScale;
	pOut->m_fCos   = cEnv.m_fCos;
	pOut->m_fSin   = cEnv.m_fSin;
	{
		float fScale = rw_GetConVar("EnvScale", 1.0f);
		pOut->m_fEnvScale = (fabsf(fScale) > 0.001f) ? (-0.5f / fScale) : fScale;
	}
	return true;
}

bool RWorld_GetEnvMatrix(float *pOut16)
{
	if (!pOut16)
		return false;
	const LTVector &vR = s_vEnvRight, &vU = s_vEnvUp, &vF = s_vEnvForward;
	memset(pOut16, 0, sizeof(float) * 16);
	pOut16[0] = vR.x; pOut16[4] = vU.x; pOut16[8]  = -vF.x;
	pOut16[1] = vR.y; pOut16[5] = vU.y; pOut16[9]  = -vF.y;
	pOut16[2] = vR.z; pOut16[6] = vU.z; pOut16[10] = -vF.z;
	pOut16[15] = 1.0f;
	return true;
}

const RWorld *RWorld_GetMainWorld()
{
	return g_pMainWorld;
}

bool RWorld_IsLoaded()
{
	return g_pMainWorld != 0 && !g_pMainWorld->m_aBlocks.empty();
}

bool RWorld_GetBounds(LTVector &vCenter, LTVector &vHalfDims)
{
	if (!RWorld_IsLoaded())
		return false;

	LTVector vMin(FLT_MAX, FLT_MAX, FLT_MAX), vMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (size_t i = 0; i < g_pMainWorld->m_aBlocks.size(); ++i)
	{
		const RWBlock &cBlock = g_pMainWorld->m_aBlocks[i];
		LTVector vBMin = cBlock.m_vCenter - cBlock.m_vHalfDims;
		LTVector vBMax = cBlock.m_vCenter + cBlock.m_vHalfDims;
		VEC_MIN(vMin, vMin, vBMin);
		VEC_MAX(vMax, vMax, vBMax);
	}
	vCenter   = (vMin + vMax) * 0.5f;
	vHalfDims = (vMax - vMin) * 0.5f;
	return true;
}

void RWorld_GetStats(uint32 &nBlocks, uint32 &nVerts, uint32 &nTris, uint32 &nWorldModels)
{
	nBlocks      = g_pMainWorld ? (uint32)g_pMainWorld->m_aBlocks.size() : 0;
	nVerts       = g_nTotalVerts;
	nTris        = g_nTotalTris;
	nWorldModels = (uint32)g_aWorldModels.size();
}

// Common GL state for world/world-model drawing (paired with rw_EndDraw).
// Draw one parsed render world (main world or a world model, in its own space).
// bAllowAlphaTest=false: draw textured sections without the cutout test (sky
// textures often carry zero alpha for blend layering — the test would discard
// every texel).
// nObjectAlpha: the OWNING OBJECT's alpha (LTObject::m_ColorA), multiplied into
// every emitted vertex colour. 255 for the main world and opaque world models.
// ⚠️ The colour emit below used glColor3ub, which forces alpha to 1.0 and threw
// this away — the same trap §35 found in rm_EmitMesh. Without it, a world model
// with m_ColorA < 255 (all the window/door GLASS) blends at full opacity, i.e.
// looks solid.
static uint32 g_nRWDrawnSections = 0, g_nRWDrawnTris = 0;   // per-call census

// ★ LT_DEBUG_FOGFACTOR: make the framebuffer BE the fog factor.
// GL has no shader to print it from, so the surface is forced WHITE and
// UNTEXTURED; run with black fog (+FogR 0 +FogG 0 +FogB 0) and the fixed
// function computes mix(black, white, f) == f for every pixel. The Metal side
// emits in.fog directly (mtl_world.mm), and the two images diff.
static bool rw_FogFactorDebug(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_DEBUG_FOGFACTOR") ? 1 : 0;
	return s_n != 0;
}

static bool rw_TraceWorld(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_TRACE_RWORLD") ? 1 : 0;
	return s_n != 0;
}

static void rw_DrawWorld(const RWorld &cWorld, const RWDrawParams &cParams)
{
	// ★ THE BACKEND BRANCH USED TO BE HERE. Everything that reaches this point --
	// the stream parse, the light groups, the world-model/sky/translucency walk
	// that calls us -- is SHARED and stays; only the per-section draw was
	// backend-specific, and the GL half of it (507 lines, 89 GL calls: the
	// largest single block of GL in the renderer) is gone with the GL backend.
	MTLWorld_DrawWorld(&cWorld, &cParams);
}

void RWorld_Draw()
{
	if (!RWorld_IsLoaded())
		return;

	RWDrawParams cParams;
	rw_DrawWorld(*g_pMainWorld, cParams);
}

static const RWorld *rw_FindWorldModel(const char *pName)
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

#define RW_MAX_SKY_OBJECTS 64
static LTObject *g_apSkyObjects[RW_MAX_SKY_OBJECTS];
static int       g_nSkyObjects = 0;

void RWorld_SetSkyObjects(LTObject **ppSkyObjects, int nCount)
{
	g_nSkyObjects = 0;
	if (!ppSkyObjects)
		return;
	for (int i = 0; i < nCount && g_nSkyObjects < RW_MAX_SKY_OBJECTS; ++i)
	{
		if (ppSkyObjects[i])
			g_apSkyObjects[g_nSkyObjects++] = ppSkyObjects[i];
	}
}

static bool rw_IsSkyObject(const LTObject *pObj)
{
	for (int i = 0; i < g_nSkyObjects; ++i)
	{
		if (g_apSkyObjects[i] == pObj)
			return true;
	}
	return false;
}

void RWorld_DrawSkyWorldModels()
{
	if (!RWorld_IsLoaded() || g_nSkyObjects <= 0)
		return;


	// The sky is a backdrop: no depth interaction with the world. Both of those
	// are RWDrawParams now, so the Metal path gets them too.
	RWDrawParams cSkyParams;
	cSkyParams.m_bAllowAlphaTest = false;   // sky layers often carry zero alpha
	cSkyParams.m_bDepthTest      = false;
	cSkyParams.m_bDepthWrite     = false;

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
		const RWorld *pWorld = pBsp ? rw_FindWorldModel(pBsp->m_WorldName) : 0;
		if (bLog && pWorld && !pWorld->m_aBlocks.empty())
		{
			const RWBlock &cBlock = pWorld->m_aBlocks[0];
			fprintf(stderr, "[glw] sky WM '%s': %u blocks, block0 center (%.0f %.0f %.0f) half (%.0f %.0f %.0f)\n",
			        pBsp->m_WorldName, (uint32)pWorld->m_aBlocks.size(),
			        cBlock.m_vCenter.x, cBlock.m_vCenter.y, cBlock.m_vCenter.z,
			        cBlock.m_vHalfDims.x, cBlock.m_vHalfDims.y, cBlock.m_vHalfDims.z);
		}
		else if (bLog)
			fprintf(stderr, "[glw] sky WM '%s': NO render data\n",
			        pBsp ? pBsp->m_WorldName : "<null>");
		if (pWorld)
			rw_DrawWorld(*pWorld, cSkyParams);
	}

}

void RWorld_DrawWorldModels(bool bTranslucentPass)
{
	if (!RWorld_IsLoaded() || !g_pClientMgr || g_aWorldModels.empty())
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
	// walk before RModel_DrawModels() let a pane blend over the already-drawn
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

		if (rw_IsSkyObject(pInstance))
			continue;   // drawn by the sky pass

		const WorldBsp *pBsp = pInstance->GetOriginalBsp();
		if (!pBsp)
			continue;

		// Only world models that carried render data exist in our list; the
		// rest (physics/vis-only BSPs) legitimately miss.
		const RWorld *pWorld = rw_FindWorldModel(pBsp->m_WorldName);
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

		RWDrawParams cParams;
		cParams.m_pModelMatrix = aGL;
		cParams.m_nObjectAlpha = pInstance->m_ColorA;

		if (bForcedParts)
		{
			cParams.m_eBlend            = kRWBlend_Alpha;
			cParams.m_bDiscardZeroAlpha = true;
			// ⚠️ DEPTH WRITES STAY ON here -- with them off the far side of a
			// car wheel draws through the near side and looks doubled (§15).
		}
		else if (bAlphaBlended)
		{
			// d3d_SetTranslucentObjectStates (d3d_draw.cpp:102) + the additive
			// override in d3d_DrawTranslucentWorldModel (drawworldmodel.cpp:88).
			cParams.m_eBlend = (pInstance->m_Flags2 & FLAG2_ADDITIVE)
			                 ? kRWBlend_Additive : kRWBlend_Alpha;
			cParams.m_bDepthWrite = false;   // D3DRS_ZWRITEENABLE, FALSE
		}

		rw_DrawWorld(*pWorld, cParams);
	}
	}

}
