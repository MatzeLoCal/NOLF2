// ----------------------------------------------------------------------- //
//
// MODULE  : render_particles.cpp
//
// PURPOSE : OT_PARTICLESYSTEM. A particle system is a linked list of PSParticle
//           (de_objects.h:638) that the GAME animates every frame; the renderer
//           turns each particle into a camera-facing textured quad. Every fire
//           in NOLF2 is one of these (Fire_Basic, JA_Firepit, Si_BarrelFire,
//           Death_Burning, ...), as is C01S01's JA_Waterfall_2.
//
//           Authority: d3d_DrawParticleSystem (sys/d3d/drawparticles.cpp:402)
//           and d3d_TestAndDrawPS (drawparticles_A.cpp:21).
//
// ⚠️⚠️ THE ONE LINE THAT MUST NOT BE DROPPED: `m_Flags |= FLAG_INTERNAL1`.
//      That is the RENDERER->OBJECT feedback edge meaning "I drew this".
//      CClientMgr::UpdateParticleSystems (clientmgr.cpp:2128) clears it every
//      frame and then SKIPS the whole update when it is not set:
//          m_Flags &= ~(FLAG_WASDRAWN | FLAG_INTERNAL1);
//          if (!(flags & FLAG_UPDATEUNSEEN) && !(flags & FLAG_INTERNAL1) &&
//              m_nChangedParticles == 0) continue;
//      so without it the particles never move and the sprite tracker never
//      advances -- the system freezes exactly the way the water did before §38.
//      D3D sets it in d3d_TestAndDrawPS, BEFORE the draw.
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "de_objects.h"      // LTParticleSystem / PSParticle
#include "clientmgr.h"       // g_pClientMgr (object lists)
#include "renderstruct.h"
#include "iltclient.h"       // PS_WORLDSPACE / PS_USEROTATION
#include "world_renderdata.h"
#include "model_renderdata.h"      // REmitState — particles share the model emit
#include "mtl_device.h"    // MTLDev_IsMetalBackend
#include "mtl_model.h"     // MTLModel_DrawTris
#include <vector>    // RWorld_ApplyObjectFog / RWorld_RestoreSceneFog
#include "render_particles.h"
#include <stdio.h>
#include <math.h>
#include "sys/shared/render_texture.h"  // RTex_* — neutral texture queries
#include "sys/shared/render_globals.h"  // g_pRenderStruct — the engine function table

namespace
{
	// Billboard basis for this scene (world space), set per frame.
	LTVector g_vCamRight(1, 0, 0), g_vCamUp(0, 1, 0);
	LTVector g_vCamFwd(0, 0, 1),   g_vCamPos(0, 0, 0);

	inline int rp_RoundToInt(float f)
	{
		return (int)(f + ((f < 0.0f) ? -0.5f : 0.5f));
	}

	inline uint8 rp_ClampByte(int n)
	{
		return (uint8)((n < 0) ? 0 : ((n > 255) ? 255 : n));
	}
}

void RParticle_SetCamera(const LTVector &vRight, const LTVector &vUp,
                          const LTVector &vForward, const LTVector &vPos)
{
	g_vCamRight = vRight;
	g_vCamUp    = vUp;
	g_vCamFwd   = vForward;
	g_vCamPos   = vPos;
}

// Draw one system. Assumes the shared translucent state is already set.
// bPlayerView selects the PLAYER-VIEW billboard basis (see below).
static bool rp_DrawSystem(LTParticleSystem *pSystem, bool bLog, bool bPlayerView)
{
	if (!pSystem || !(pSystem->m_Flags & FLAG_VISIBLE))
		return false;

	if (g_pRenderStruct && g_pRenderStruct->IsObjectGroupEnabled &&
	    !g_pRenderStruct->IsObjectGroupEnabled(pSystem->m_nRenderGroup))
		return false;

	// ★★ THE FEEDBACK EDGE — see the file header. Set it whether or not we end
	// up emitting geometry (D3D sets it before the draw too), otherwise a
	// system that is momentarily empty would never be updated again and could
	// never refill.
	pSystem->m_Flags |= FLAG_INTERNAL1;

	// ⚠️ An EMPTY system is the interesting case, not a boring one: it means the
	// system exists but is not emitting. Report it before bailing, or the trace
	// silently hides exactly the systems you are hunting for.
	if (pSystem->m_nParticles <= 0)
	{
		if (bLog)
		{
			SharedTexture *pT = pSystem->m_pCurTexture;
			fprintf(stderr, "[glps] %s system @(%.0f %.0f %.0f): EMPTY (0 particles) tex=%s\n",
			        bPlayerView ? "PV" : "world",
			        pSystem->m_Pos.x, pSystem->m_Pos.y, pSystem->m_Pos.z,
			        pT ? RTex_GetName(pT) : "<none>");
		}
		return false;
	}

	SharedTexture *pTex = pSystem->m_pCurTexture;
	// ⚠️ Neutral query (render_texture.h) — the GL-era GLTex_GetName reinterpreted
	// the Metal entry and returned garbage (§88's vanished sprites).
	const bool bTexOk = RTex_IsValid(pTex);
	const unsigned nName = bTexOk ? 1u : 0u;   // diagnostics only

	REmitState cPart;
	cPart.m_pTexture = bTexOk ? pTex : 0;
	cPart.m_bZWrite  = false;      // translucent: test but do not write
	cPart.m_bBlend   = true;

	// Blend mode from the object's flags2 — d3d_GetBlendStates (d3d_draw.h:128).
	// Fire is overwhelmingly FLAG2_ADDITIVE.
	// ⚠️⚠️ ADDITIVE IS **ONE/ONE**, NOT SRC_ALPHA/ONE. d3d_GetBlendStates
	// (d3d_draw.h:138) sets srcBlend = D3DBLEND_ONE for FLAG2_ADDITIVE, and the
	// distinction is not cosmetic: an additive FX texture routinely has NO
	// ALPHA AT ALL. Measured on the retail assets —
	//     FX/GUNS/WELDER/TORCH1.DTX      alpha 0 everywhere (100%)
	//     FX/GUNS/WELDER/WELDERGLOW.DTX  alpha 0 everywhere (100%)
	// so SRC_ALPHA multiplies the whole contribution by zero and the effect is
	// INVISIBLE. That was the welder's missing flame (§53/§57).
	// ⚠️ Do NOT "unify" this with the world-model path: D3D genuinely differs by
	// object type — d3d_DrawTranslucentWorldModel (drawworldmodel.cpp:88) uses
	// SRCALPHA/ONE for additive WORLD MODELS. Particles, sprites and polygrids
	// go through d3d_GetBlendStates and use ONE/ONE.
	if (pSystem->m_Flags2 & FLAG2_ADDITIVE)
	{ cPart.m_nSrcBlend = kRBlend_One;       cPart.m_nDstBlend = kRBlend_One; }
	else if (pSystem->m_Flags2 & FLAG2_MULTIPLY)
	{ cPart.m_nSrcBlend = kRBlend_Zero;      cPart.m_nDstBlend = kRBlend_SrcColor; }
	else
	{ cPart.m_nSrcBlend = kRBlend_SrcAlpha; cPart.m_nDstBlend = kRBlend_InvSrcAlpha; }

	// PS_WORLDSPACE means the particle positions are already world space;
	// otherwise they are in the system's object space and the system's
	// transform applies.
	const bool bObjectSpace = !(pSystem->m_psFlags & PS_WORLDSPACE);

	// ★★ PLAYER-VIEW SYSTEMS BILLBOARD OFF THE UNTRANSFORMED AXES.
	// A FLAG_REALLYCLOSE system is already in CAMERA space and the caller has
	// installed the player-view projection with an identity-ish view, so the
	// camera's world-space basis is meaningless here. D3D says the same thing
	// explicitly (d3d_DrawParticleSystem, drawparticles.cpp:422):
	//     vParticleUp(0,1,0); vParticleRight(1,0,0); normal(0,0,-1)
	// This is what the WELDER's flame is: Welder_PV_Muzz's first effect is a
	// ParticleSystem attached to the player-view weapon's "Flash" socket.
	LTVector vUp    = bPlayerView ? LTVector(0.0f, 1.0f, 0.0f) : g_vCamUp;
	LTVector vRight = bPlayerView ? LTVector(1.0f, 0.0f, 0.0f) : g_vCamRight;

	float aRModel[16];

	if (bObjectSpace)
	{
		// Compose the system transform and push it, then bring the billboard
		// basis INTO object space so the quads still face the camera
		// (d3d_DrawParticleSystem does exactly this with an inverted matrix).
		LTMatrix mRot;
		pSystem->m_Rotation.ConvertToMatrix(mRot);

		float *aGL = aRModel;
		for (int nRow = 0; nRow < 3; ++nRow)
			for (int nCol = 0; nCol < 3; ++nCol)
				aGL[nCol * 4 + nRow] = mRot.m[nRow][nCol] *
				                       ((nCol == 0) ? pSystem->m_Scale.x :
				                        (nCol == 1) ? pSystem->m_Scale.y
				                                    : pSystem->m_Scale.z);
		aGL[3] = aGL[7] = aGL[11] = 0.0f;
		aGL[12] = pSystem->m_Pos.x;
		aGL[13] = pSystem->m_Pos.y;
		aGL[14] = pSystem->m_Pos.z;
		aGL[15] = 1.0f;
		cPart.m_pModelMatrix = aRModel;

		// Inverse-rotate the basis into object space. The rotation part is
		// orthonormal, so its inverse is its transpose; divide out the scale
		// so a scaled system does not get scaled quads on top of the matrix.
		LTVector vU, vR;
		vU.x = mRot.m[0][0] * vUp.x + mRot.m[1][0] * vUp.y + mRot.m[2][0] * vUp.z;
		vU.y = mRot.m[0][1] * vUp.x + mRot.m[1][1] * vUp.y + mRot.m[2][1] * vUp.z;
		vU.z = mRot.m[0][2] * vUp.x + mRot.m[1][2] * vUp.y + mRot.m[2][2] * vUp.z;
		vR.x = mRot.m[0][0] * vRight.x + mRot.m[1][0] * vRight.y + mRot.m[2][0] * vRight.z;
		vR.y = mRot.m[0][1] * vRight.x + mRot.m[1][1] * vRight.y + mRot.m[2][1] * vRight.z;
		vR.z = mRot.m[0][2] * vRight.x + mRot.m[1][2] * vRight.y + mRot.m[2][2] * vRight.z;
		if (pSystem->m_Scale.x != 0.0f) { vU.x /= pSystem->m_Scale.x; vR.x /= pSystem->m_Scale.x; }
		if (pSystem->m_Scale.y != 0.0f) { vU.y /= pSystem->m_Scale.y; vR.y /= pSystem->m_Scale.y; }
		if (pSystem->m_Scale.z != 0.0f) { vU.z /= pSystem->m_Scale.z; vR.z /= pSystem->m_Scale.z; }
		vUp = vU;
		vRight = vR;
	}

	// ⚠️ ×2 — the corner offsets are (up ± right), so a particle of m_Size s
	// must span 2s. D3D scales the basis vectors here and comments it as
	// "compensate for old code"; keep the factor or every particle is half
	// size.
	vUp    *= 2.0f;
	vRight *= 2.0f;

	// Object tint: m_ColorR/G/B scale the particle colour, m_ColorA scales alpha.
	const float fR = (float)pSystem->m_ColorR * (1.0f / 255.0f);
	const float fG = (float)pSystem->m_ColorG * (1.0f / 255.0f);
	const float fB = (float)pSystem->m_ColorB * (1.0f / 255.0f);
	const float fA = (float)pSystem->m_ColorA;

	const bool bRotate = (pSystem->m_psFlags & PS_USEROTATION) != 0;

	uint32 nDrawn = 0;
	static std::vector<MTLModelVert> s_aPartVerts;   // static: no per-system alloc
	s_aPartVerts.clear();
	for (PSParticle *pCur = pSystem->m_ParticleHead.m_pNext;
	     pCur && pCur != &pSystem->m_ParticleHead;
	     pCur = pCur->m_pNext)
	{
		LTVector vU = vUp, vR = vRight;
		if (bRotate && pCur->m_fAngle != 0.0f)
		{
			// d3d precomputes a 256-entry table; at our particle counts the
			// direct form is cheaper than the table lookup it replaces.
			const float fC = cosf(pCur->m_fAngle);
			const float fS = sinf(pCur->m_fAngle);
			vU = vUp * fC + vRight * fS;
			vR = vRight * fC - vUp * fS;
		}

		const uint8 nCR = rp_ClampByte(rp_RoundToInt(pCur->m_Color.x * fR));
		const uint8 nCG = rp_ClampByte(rp_RoundToInt(pCur->m_Color.y * fG));
		const uint8 nCB = rp_ClampByte(rp_RoundToInt(pCur->m_Color.z * fB));
		const uint8 nCA = rp_ClampByte(rp_RoundToInt(pCur->m_Alpha   * fA));

		const LTVector &vP = pCur->m_Pos;
		const float fSize  = pCur->m_Size;

		// Corner order and UVs match CParticleVertex::SetupBaseVerts:
		// upper-left (0,0), upper-right (1,0), bottom-right (1,1),
		// bottom-left (0,1).
		LTVector v0 = vP + (vU - vR) * fSize;
		LTVector v1 = vP + (vU + vR) * fSize;
		LTVector v2 = vP + (-vU + vR) * fSize;
		LTVector v3 = vP + (-vU - vR) * fSize;

		{
			// Metal has no GL_QUADS: (0,1,2)(0,2,3), same winding.
			const LTVector aC[4] = { v0, v1, v2, v3 };
			const float    aUV[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
			static const int kTri[6] = { 0, 1, 2, 0, 2, 3 };
			for (int i = 0; i < 6; ++i)
			{
				const int n = kTri[i];
				MTLModelVert cV = { aC[n].x, aC[n].y, aC[n].z,
				                    aUV[n][0], aUV[n][1], nCR, nCG, nCB, nCA };
				s_aPartVerts.push_back(cV);
			}
		}
		++nDrawn;
	}

	// One draw per SYSTEM, not per particle -- the whole system shares its
	// texture and blend state.
	MTLModel_DrawTris(s_aPartVerts.empty() ? 0 : &s_aPartVerts[0],
	                  (uint32)s_aPartVerts.size(), &cPart);

	if (bLog)
		fprintf(stderr, "[glps] %s system @(%.0f %.0f %.0f): %d particles, %u drawn, "
		                "tex=%u(%s) space=%s blend=%s rot=%d\n",
		        bPlayerView ? "PV" : "world",
		        pSystem->m_Pos.x, pSystem->m_Pos.y, pSystem->m_Pos.z,
		        pSystem->m_nParticles, nDrawn, (unsigned)nName,
		        pTex ? RTex_GetName(pTex) : "<none>",
		        bObjectSpace ? "object" : "world",
		        (pSystem->m_Flags2 & FLAG2_ADDITIVE) ? "add" :
		        ((pSystem->m_Flags2 & FLAG2_MULTIPLY) ? "mul" : "alpha"),
		        (int)bRotate);

	return nDrawn > 0;
}

// Shared body: bPlayerView picks WHICH set to draw — the world-space systems or
// the FLAG_REALLYCLOSE (player-view) ones. They cannot be drawn together: the
// two live in different spaces under different projections.
static void rp_DrawPass(bool bPlayerView)
{
	if (!g_pClientMgr)
		return;

	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_PARTICLESYSTEM].m_Head;
	if (pHead->m_pNext == pHead)
		return;

	// ⚠️ A DIAGNOSTIC THAT FIRES ONCE LIES (§38). A system is created empty and
	// fills over the following frames, so a one-shot trace reports "1 particle"
	// for a waterfall. Report on CHANGE instead, so "is this system alive?" is
	// answerable — that is exactly the question FLAG_INTERNAL1 decides.
	static int s_nTrace = -1;
	if (s_nTrace < 0) s_nTrace = getenv("LT_TRACE_PARTICLES") ? 1 : 0;
	bool bLog = false;
	if (s_nTrace)
	{
		uint32 nSig = 0, nSystems = 0;
		for (LTLink *pIt = pHead->m_pNext; pIt != pHead; pIt = pIt->m_pNext)
		{
			LTParticleSystem *pS = (LTParticleSystem*)pIt->m_pData;
			if (!pS) continue;
			++nSystems;
			nSig = nSig * 131u + (uint32)pS->m_nParticles;
		}
		static uint32 s_nLastSig = 0xFFFFFFFFu;
		static uint32 s_nLastSystems = 0xFFFFFFFFu;
		if (nSig != s_nLastSig || nSystems != s_nLastSystems)
		{
			s_nLastSig = nSig;
			s_nLastSystems = nSystems;
			bLog = true;
		}
	}

	// Translucent: depth-test but do not write (particles must not occlude each
	// other or anything after them). Every one of these is a per-draw parameter
	// carried by REmitState -- there is no ambient state to set.

	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		LTParticleSystem *pS = (LTParticleSystem*)pCur->m_pData;
		if (!pS)
			continue;
		// Each pass takes only its own set.
		if (((pS->m_Flags & FLAG_REALLYCLOSE) != 0) != bPlayerView)
			continue;
		rp_DrawSystem(pS, bLog, bPlayerView);
	}

}

void RParticle_DrawSystems()
{
	rp_DrawPass(false);
}

// ⚠️ Must be called by the PLAYER-VIEW pass, while its projection and view are
// still installed — RModel_DrawPlayerView does it just before its teardown.
// Drawing these in the world pass puts them at the wrong place entirely, which
// is why the welder's flame was invisible while the tool worked.
void RParticle_DrawPlayerView()
{
	rp_DrawPass(true);
}
