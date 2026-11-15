// ----------------------------------------------------------------------- //
//
// MODULE  : render_volumeeffect.cpp
//
// PURPOSE : OT_VOLUMEEFFECT (VolumeEffectInfo::kDynamicParticles) — snowfall,
//           and the scattered grass/debris of ScatterFX.
//
//           A SnowVolume brush in the world becomes a client CSnowFX, which
//           splits itself into SNOWFX_AIRSPACEDIMS-sized airspaces and creates
//           ONE OT_VOLUMEEFFECT object per active airspace. The object carries
//           no geometry of its own: the GAME fills a vertex buffer we lend it,
//           through m_DPUpdateFn, every frame. Our whole job is to lend the
//           buffer, call back until the object says it is done, and draw.
//
//           Authority: DrawDynamicParticles (sys/d3d/drawvolumeeffects.cpp:107).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "de_objects.h"            // LTVolumeEffect
#include "clientmgr.h"             // g_pClientMgr — the per-type object lists
#include "renderstruct.h"
#include "iltclient.h"
#include "model_renderdata.h"      // REmitState / MTLModelVert — shared emit
#include "mtl_device.h"            // MTLDev_IsMetalBackend
#include "mtl_model.h"             // MTLModel_DrawTris
#include "sys/shared/render_state.h"    // kRBlend_* / kRAlpha_*
#include "sys/shared/render_texture.h"  // RTex_GetName
#include "render_volumeeffect.h"
#include <vector>
#include <stdio.h>
#include <stdlib.h>

namespace
{
	// ⚠️ MUST MATCH THE GAME, BYTE FOR BYTE. See the header: the game declares
	// its own private copy of this and writes through it. 24 bytes.
	struct DynamicParticleVertex
	{
		float  x, y, z;
		uint32 color;          // 0xAARRGGBB (D3DFVF_DIFFUSE order)
		float  u, v;
	};

	// The lighting sample array the callback may write when m_DPLighting is
	// kSinglePointNonDirectional. Layout from drawvolumeeffects.cpp:68.
	struct DynamicParticleLightingData
	{
		LTVector pos;
		uint32   alpha;
		LTVector acc;
	};

	// Donor's buffer size (drawvolumeeffects.cpp:76). The callback is invoked
	// repeatedly until it reports done, so this caps work per call, not the
	// particle count: an airspace with more snow than fits simply takes more
	// turns. Keep it divisible by 3 (trilist) and 4 (quadlist).
	const uint32 kVBNumVerts = 6000;

	// ⚠️ UVs LEAVE 0..1 BY DESIGN, SO THE SAMPLER MUST CLAMP.
	//
	// A snow particle is ONE TRIANGLE, not a quad: the game emits 3 verts whose
	// UVs are (0,-1), (0,1), (2,1) — a triangle that circumscribes the flake's
	// square, which is cheaper than 6 verts for a round sprite that only
	// occupies the inscribed circle. Outside 0..1 the texture is transparent,
	// and the alpha test discards it. Under a REPEATING sampler those same
	// coordinates tile the flake 3x3 instead, i.e. snow would render as a
	// lattice of flakes. The donor sets D3DTADDRESS_CLAMP on both axes
	// explicitly (drawvolumeeffects.cpp:182) for exactly this reason.

	std::vector<DynamicParticleVertex> g_aFill;     // what the GAME writes
	std::vector<MTLModelVert>          g_aEmit;     // what Metal draws
	std::vector<DynamicParticleLightingData> g_aLight;

	int   g_nTrace    = -1;
	int   g_nDisabled = -1;
	uint32 g_nDrawnLastFrame = 0;

	// ⚠️ AN A/B SWITCH MUST ANNOUNCE ITSELF. Twice in this port a patch script
	// failed silently and both arms of a comparison ran the same binary, which
	// reads as "the change makes no difference" -- the most expensive possible
	// wrong answer. So say so, once, on stderr.
	inline bool VolFXDisabled()
	{
		if (g_nDisabled < 0)
		{
			g_nDisabled = getenv("LT_NO_VOLFX") ? 1 : 0;
			if (g_nDisabled)
				fprintf(stderr, "[volfx] LT_NO_VOLFX=1 -- volume effects (snowfall) NOT drawn\n");
		}
		return g_nDisabled != 0;
	}

	inline bool VolFXTracing()
	{
		if (g_nTrace < 0)
			g_nTrace = getenv("LT_TRACE_VOLFX") ? 1 : 0;
		return g_nTrace != 0;
	}
}

// Draw one effect. Returns the number of TRIANGLES drawn.
static uint32 rve_DrawDynamicParticles(LTVolumeEffect *pEffect)
{
	if (!pEffect->m_DPUpdateFn)
		return 0;

	const bool bQuads = (pEffect->m_DPPrimitive == VolumeEffectInfo::kQuadlist);
	const bool bLighting = (pEffect->m_DPLighting != VolumeEffectInfo::kNone);

	if (g_aFill.size() < kVBNumVerts)
		g_aFill.resize(kVBNumVerts);
	if (bLighting && g_aLight.size() < (kVBNumVerts / 3))
		g_aLight.resize(kVBNumVerts / 3);

	// The authored state, straight off the donor (drawvolumeeffects.cpp:174-193
	// plus VerifyTranslucentObjectStates, d3d_draw.cpp:92).
	REmitState cState;
	cState.m_pTexture      = pEffect->m_DPTexture;
	cState.m_bAlphaTest    = true;
	cState.m_nAlphaFunc    = kRAlpha_Greater;   // D3DCMP_GREATER
	cState.m_fAlphaRef     = 0.0f;              // D3DRS_ALPHAREF 0 — ">50% fill
	                                            // (and read) rate" per the donor
	cState.m_bBlend        = true;
	cState.m_nSrcBlend     = kRBlend_SrcAlpha;
	cState.m_nDstBlend     = kRBlend_InvSrcAlpha;
	cState.m_bZTest        = true;
	cState.m_bZWrite       = false;             // flakes must not occlude each
	                                            // other, nor anything after
	cState.m_bIgnoreDiffuse = false;            // COLORARG2 = DIFFUSE
	cState.m_bTextureAlpha = true;              // ALPHAOP = TEXTURE x DIFFUSE
	// MODULATE2X when the volume asks to saturate — SnowVolume's UseSaturate
	// defaults TRUE, and every Siberia volume uses the default, so dropping
	// this makes all snow half as bright as Windows.
	cState.m_fColorScale   = pEffect->m_DPSaturate ? 2.0f : 1.0f;
	cState.m_bClampTexture = true;              // see the UV note above
	cState.m_pModelMatrix  = 0;                 // the game emits WORLD space

	uint32 nTotalFilled = 0;
	uint32 nTrisDrawn   = 0;
	bool   bDone        = false;

	while (!bDone)
	{
		uint32 nCurFilled = 0;

		// The callback fills our buffer and tells us how many verts it wrote.
		// It returns TRUE when it has nothing left — note the inverted sense
		// against the usual convention, and that it is called at least once.
		bDone = pEffect->m_DPUpdateFn(pEffect->m_DPUserData,
		                              &g_aFill[0],
		                              bLighting && !g_aLight.empty() ? (void*)&g_aLight[0] : (void*)0,
		                              kVBNumVerts,
		                              nTotalFilled,
		                              nCurFilled);

		if (nCurFilled == 0)
			break;
		if (nCurFilled > kVBNumVerts)      // defensive: a bad fill would walk
			nCurFilled = kVBNumVerts;      // off the end of the vector

		// ⚠️ BOTH PRIMITIVE KINDS SHIP, measured -- do not drop either.
		//   SnowFX  (snowfall)        = kTrilist,  3 verts = 1 flake
		//   ScatterFX (grass/debris)  = kQuadlist, 4 verts = 1 blade
		// C01S01 alone sets up 11 quadlist volumes of TexFX\Grass\grass2.dtx.
		// D3D draws quads INDEXED off a static IB of (0,1,2)(2,1,3) per quad
		// (drawvolumeeffects.cpp:144); expanded to 6 verts here instead, since
		// nothing else in this renderer takes an index buffer.
		g_aEmit.clear();
		if (bQuads)
		{
			const uint32 nQuads = nCurFilled / 4;
			g_aEmit.reserve(nQuads * 6);
			static const int kIdx[6] = { 0, 1, 2, 2, 1, 3 };
			for (uint32 q = 0; q < nQuads; ++q)
			{
				for (int i = 0; i < 6; ++i)
				{
					const DynamicParticleVertex &cV = g_aFill[q * 4 + kIdx[i]];
					MTLModelVert cO = { cV.x, cV.y, cV.z, cV.u, cV.v,
					                    (uint8)((cV.color >> 16) & 0xFF),
					                    (uint8)((cV.color >>  8) & 0xFF),
					                    (uint8)( cV.color        & 0xFF),
					                    (uint8)((cV.color >> 24) & 0xFF) };
					g_aEmit.push_back(cO);
				}
			}
			nTrisDrawn += nQuads * 2;
		}
		else
		{
			const uint32 nVerts = (nCurFilled / 3) * 3;
			g_aEmit.reserve(nVerts);
			for (uint32 i = 0; i < nVerts; ++i)
			{
				const DynamicParticleVertex &cV = g_aFill[i];
				MTLModelVert cO = { cV.x, cV.y, cV.z, cV.u, cV.v,
				                    (uint8)((cV.color >> 16) & 0xFF),   // R
				                    (uint8)((cV.color >>  8) & 0xFF),   // G
				                    (uint8)( cV.color        & 0xFF),   // B
				                    (uint8)((cV.color >> 24) & 0xFF) }; // A
				g_aEmit.push_back(cO);
			}
			nTrisDrawn += nVerts / 3;
		}

		if (!g_aEmit.empty())
			MTLModel_DrawTris(&g_aEmit[0], (uint32)g_aEmit.size(), &cState);

		nTotalFilled += nCurFilled;

		// Belt and braces: a callback that never reports done would spin here
		// forever. The donor has the same shape and the same exposure.
		if (nTotalFilled > kVBNumVerts * 64)
			break;
	}

	return nTrisDrawn;
}

void RVolumeEffect_Draw()
{
	if (!g_pClientMgr)
		return;
	if (!MTLDev_IsMetalBackend())
		return;
	if (VolFXDisabled())
		return;

	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_VOLUMEEFFECT].m_Head;
	if (pHead->m_pNext == pHead)
		return;

	uint32 nEffects = 0, nDrawn = 0, nTris = 0;

	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		LTVolumeEffect *pEffect = (LTVolumeEffect*)pCur->m_pData;
		if (!pEffect)
			continue;
		++nEffects;

		// CSnowFX parks an airspace's effect object by CLEARING FLAG_VISIBLE and
		// keeping it on a free list (SnowFX.cpp:700) rather than destroying it,
		// so the list holds far more objects than are live. Honour the flag or
		// retired airspaces keep snowing.
		if (!(pEffect->m_Flags & FLAG_VISIBLE))
			continue;

		if (pEffect->m_EffectType != VolumeEffectInfo::kDynamicParticles)
			continue;

		const uint32 n = rve_DrawDynamicParticles(pEffect);
		if (n)
		{
			++nDrawn;
			nTris += n;
		}
	}

	// ⚠️ REPORT ON CHANGE, NOT EVERY FRAME (§38's lesson): snow is continuous,
	// so a per-frame line buries the log in 60 identical rows a second and the
	// interesting transition -- an airspace waking or retiring -- scrolls past.
	if (VolFXTracing() && nTris != g_nDrawnLastFrame)
	{
		g_nDrawnLastFrame = nTris;
		fprintf(stderr, "[volfx] %u effect(s) in list, %u drawn, %u tris\n",
		        nEffects, nDrawn, nTris);
	}
}
