// ----------------------------------------------------------------------- //
//
// MODULE  : gl_polygrid.cpp
//
// PURPOSE : OT_POLYGRID = WATER. A polygrid is an animated height field: the
//           game (CPolyGridFX, ClientShellShared/PolyGridFX.cpp) owns a
//           Width x Height array of signed bytes and animates it every frame;
//           the renderer turns that array into a lit, textured, usually
//           translucent triangle mesh. Water surfaces, the Siberia cutscene
//           ice and assorted goo volumes are all polygrids.
//
//           Authority: d3d_DrawPolyGrid (sys/d3d/drawpolygrid.cpp:635). This
//           ports its FIXED-FUNCTION paths: the plain textured path AND the
//           ENVIRONMENT-MAP path with the Fresnel alpha term. The pixel-shader
//           branches (ps\envbumpmap.psh bump mapping, D3DX effect shaders) are
//           not implemented; a grid that asks for bump mapping falls back to the
//           env-map path, which is exactly what D3D does when the shader fails
//           to load (drawpolygrid.cpp:780-784).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "de_objects.h"      // LTPolyGrid
#include "clientmgr.h"       // g_pClientMgr (object lists)
#include "renderstruct.h"
#include "iltclient.h"       // PG_NOBACKFACECULL / PG_FRESNEL
#include "gl_texture.h"
#include "gl_worlddata.h"    // GLWorld_ApplyObjectFog / GLWorld_RestoreSceneFog
#include "gl_polygrid.h"
#include <OpenGL/gl.h>
#include <stdio.h>
#include <vector>

// nullrender.cpp's presented-frame counter — the number LT_DUMP_SWAP matches.
// The census reports it so that "which LT_DUMP_SWAP frame is this spot?" stops
// being guesswork: the menu alone burns several thousand frames before the
// first world ever loads.
extern int g_nSwapCount;

namespace
{
// Env-gated debug switches (read once):
//   LT_PG_OFF=1   -- draw no polygrids at all (A/B against the previous build)
//   LT_PG_WIRE=1  -- draw them as opaque bright magenta WIREFRAME: no texture,
//                    no blend, no fog. Removes every shading variable so a single
//                    screenshot answers the only questions that matter -- is the
//                    surface the right SHAPE, at the right HEIGHT, in the right
//                    PLACE? A translucent dark sheet cannot answer those.
int glpg_DebugMode()
{
	static int s_nMode = -1;
	if (s_nMode < 0)
	{
		s_nMode = 0;
		if (getenv("LT_PG_OFF"))  s_nMode = 1;
		if (getenv("LT_PG_WIRE")) s_nMode = 2;
	}
	return s_nMode;
}

// LT_PG_NOENV=1 -- ignore m_pEnvMap and draw the old base-texture-only way.
// This is the A/B for the whole env-map change, equivalent to the retail
// console var `EnvMapPolyGrids 0` (rendererconsolevars.h:245, default 1).
bool glpg_NoEnvMap()
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_PG_NOENV") ? 1 : 0;
	return s_n != 0;
}

// ⚠️ LT_PG_ENVBASE=1 -- ALSO modulate the base texture into the env-mapped
// surface (two texture units: unit0 = base with UV0, unit1 = reflection).
//
// This exists because of a genuine oddity in the authority, which is worth
// writing down rather than silently picking a side. d3d_SetEnvMapTextureStates
// (drawpolygrid.cpp:340) authors:
//     stage0  COLOROP=MODULATE2X  ARG1=TEXTURE  ARG2=DIFFUSE
//     stage1  COLOROP=MODULATE    ARG1=DIFFUSE  ARG2=TEXTURE
// In D3D fixed function, D3DTA_DIFFUSE at stage 1 is the interpolated vertex
// colour, NOT the previous stage's result (that is D3DTA_CURRENT). So stage 1
// throws stage 0 away and the final colour is `vertexColour * envMap` — the
// base texture contributes NOTHING on the env-map path. Odd, but corroborated
// by PG_NORMALMAPSPRITE, whose documented meaning is literally "the sprite is a
// normal map; if not bump mapping, do not use any texture" (iltclient.h:104):
// the engine already expects env-mapped water to draw with no base texture.
// The faithful reading is therefore the DEFAULT here. This switch gives the
// other reading (stage 1 arg1 = CURRENT) for side-by-side comparison against a
// Windows capture, since that is the only thing that can settle it.
bool glpg_EnvBase()
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_PG_ENVBASE") ? 1 : 0;
	return s_n != 0;
}

// LT_PG_HEIGHTS=1 -- trace the height field's min/max on change (see the use).
bool glpg_TraceHeights()
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_PG_HEIGHTS") ? 1 : 0;
	return s_n != 0;
}

// --------------------------------------------------------------------
// The scene camera (GLPolyGrid_SetCamera).
// --------------------------------------------------------------------
LTVector s_vCamRight(1, 0, 0), s_vCamUp(0, 1, 0), s_vCamForward(0, 0, 1);
LTVector s_vCamPos(0, 0, 0);

// --------------------------------------------------------------------
// CFresnelTable (drawpolygrid.cpp:138) — the Schlick-free, full Fresnel
// reflectance for a dielectric, tabulated against |N.V| and delivered as an
// alpha byte. Byte-for-byte the same maths; only the storage differs (we keep
// the byte, D3D pre-shifted it into an ARGB's alpha slot).
//
// ⚠️ This is what makes retail water read as water: the surface is nearly
// transparent where you look straight down into it and nearly mirror-like at
// grazing angles. Substituting a flat alpha (which is what we did until now)
// removes the single most recognisable cue the surface has.
// --------------------------------------------------------------------
class CGLFresnelTable
{
public:
	enum { TABLE_SIZE = 1024 };

	CGLFresnelTable() : m_fVolumeIOR(-1.0f), m_fBaseReflection(-1.0f) {}

	// Regenerate only when the authored parameters actually change. A polygrid
	// keeps them for its lifetime, so in practice this runs once per grid.
	void Ensure(float fVolumeIOR, float fBaseReflection)
	{
		if (fabsf(m_fVolumeIOR - fVolumeIOR) < 0.01f &&
		    fabsf(m_fBaseReflection - fBaseReflection) < 0.01f)
			return;

		static const float kfViewerIOR = 1.0003f;
		const float fIORRatioSqr = (fVolumeIOR / kfViewerIOR) * (fVolumeIOR / kfViewerIOR);

		float fCos = 0.0f;
		const float fCosInc = 1.0f / TABLE_SIZE;

		for (uint32 i = 0; i < TABLE_SIZE; ++i)
		{
			const float fG = fIORRatioSqr + fCos * fCos - 1.0f;

			// Note fG can go negative for a grazing angle with a low IOR, and
			// the denominators can approach zero; D3D asserted 0..1 here (and
			// asserts are compiled out at retail parity, §5) so a NaN would
			// have gone straight into the table. Guard, then clamp.
			const float fNum   = (fG - fCos) * (fG - fCos);
			const float fDen   = 2.0f * (fG + fCos) * (fG + fCos);
			float fVal = 1.0f;
			if (fDen > 1e-8f)
			{
				const float fA = fCos * (fG + fCos) - 1.0f;
				const float fB = fCos * (fG - fCos) + 1.0f;
				if (fabsf(fB) > 1e-8f)
					fVal = (fNum / fDen) * (1.0f + (fA * fA) / (fB * fB));
			}

			fVal += fBaseReflection;
			fVal = LTCLAMP(fVal, 0.0f, 1.0f);

			m_aTable[i] = (unsigned char)(fVal * 255.0f);
			fCos += fCosInc;
		}

		m_fVolumeIOR      = fVolumeIOR;
		m_fBaseReflection = fBaseReflection;
	}

	unsigned char GetValue(float fDot) const
	{
		int32 nIdx = (int32)(fabsf(fDot) * (TABLE_SIZE - 1));
		if (nIdx < 0) nIdx = 0;
		if (nIdx >= TABLE_SIZE) nIdx = TABLE_SIZE - 1;
		return m_aTable[nIdx];
	}

private:
	float         m_fVolumeIOR;
	float         m_fBaseReflection;
	unsigned char m_aTable[TABLE_SIZE];
};

CGLFresnelTable s_FresnelTable;

	// Scratch vertex arrays, reused across grids and frames (a polygrid can be
	// 64x64 = 4096 verts and is rebuilt every frame because the data animates).
	std::vector<float> s_vPos;      // xyz
	std::vector<float> s_vUV;       // uv
	std::vector<unsigned char> s_vColor;   // rgba
	std::vector<float> s_vNormal;   // xyz, env-map path only (texgen needs it)

	// One census per PASS. A single shared flag reported only the opaque pass and
	// left the translucent one — the pass water actually uses — permanently
	// silent: the §28 "a diagnostic that only fires once" trap again.
	bool s_bCensus[2] = { false, false };

	// --------------------------------------------------------------------
	// Build the modelview for this grid: the object's position and rotation,
	// NO scale -- d3d_DrawPolyGrid deliberately passes a unit scale ("we don't
	// do the scale as that tends to mess up normals"); the dimensions are
	// already baked into the vertex spacing below.
	// --------------------------------------------------------------------
	void glpg_PushGridTransform(const LTPolyGrid *pGrid)
	{
		const LTVector &vPos = pGrid->GetPos();
		LTVector vR = pGrid->m_Rotation.Right();
		LTVector vU = pGrid->m_Rotation.Up();
		LTVector vF = pGrid->m_Rotation.Forward();

		// Column-major GL matrix: basis vectors in the columns, translation in
		// the last column. Same convention as the world-model draw.
		float aM[16];
		aM[0] = vR.x; aM[4] = vU.x; aM[8]  = vF.x; aM[12] = vPos.x;
		aM[1] = vR.y; aM[5] = vU.y; aM[9]  = vF.y; aM[13] = vPos.y;
		aM[2] = vR.z; aM[6] = vU.z; aM[10] = vF.z; aM[14] = vPos.z;
		aM[3] = 0.0f; aM[7] = 0.0f; aM[11] = 0.0f; aM[15] = 1.0f;

		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glMultMatrixf(aM);
	}

	// --------------------------------------------------------------------
	// One grid. Returns false if it had nothing drawable.
	// --------------------------------------------------------------------
	bool glpg_DrawGrid(LTPolyGrid *pGrid, bool bLog)
	{
		// d3d_DrawPolyGrid's own guards, in the same order. Each one is logged:
		// "not drawn" with no reason is a dead end for whoever picks this up.
		if (!pGrid->m_Data)
		{
			if (bLog) fprintf(stderr, "[glpg] grid skipped: no m_Data (game never filled it)\n");
			return false;
		}
		if (!pGrid->m_Indices || pGrid->m_nIndices == 0)
		{
			if (bLog) fprintf(stderr, "[glpg] grid skipped: no indices (%p, %u)\n",
			                  (void*)pGrid->m_Indices, pGrid->m_nIndices);
			return false;
		}
		if (pGrid->m_Width < 2 || pGrid->m_Height < 2)
		{
			if (bLog) fprintf(stderr, "[glpg] grid skipped: degenerate %ux%u\n",
			                  pGrid->m_Width, pGrid->m_Height);
			return false;
		}

		// ★★★ "SPECIFY THAT WE WERE VISIBLE" (drawpolygrid.cpp:834). THE RENDERER
		// OWNS THE FEEDBACK EDGE THAT DRIVES THE WATER'S ANIMATION.
		//
		// CClientMgr::UpdatePolyGrids (clientmgr.cpp:2209-2216) runs every frame
		// and does:
		//     m_Flags &= ~(FLAG_WASDRAWN | FLAG_INTERNAL1);
		//     if (!FLAG_UPDATEUNSEEN && !FLAG_INTERNAL1) continue;   // skip it
		//     m_Flags |= FLAG_WASDRAWN;
		//     spr_UpdateTracker(...);                                 // sprite frames
		// so FLAG_INTERNAL1 is a flag the RENDERER sets to say "I drew this",
		// and everything downstream keys off it. Without it:
		//   * CPolyGridFX::Update returns early at PolyGridFX.cpp:382
		//     (`if (!(dwFlags & FLAG_WASDRAWN) && !m_bAlwaysUpdate)`), so
		//     UpdateWaveProp/UpdatePlasma NEVER RUN and the height field stays
		//     at the all-zero state it was created with;
		//   * spr_UpdateTracker is never called, so the surface sprite never
		//     advances a frame either.
		// Measured before this line existed: the height buffer's checksum changed
		// ONCE in 4800 draws, min==max==0, so every vertex indexed the middle of
		// the colour ramp and emitted a single flat colour (188,190,188) — which
		// is precisely the uniform pale-grey slab the water was rendering as. The
		// "flat grey sheet" was never a shading bug.
		//
		// ⚠️ This is the §5 "unfilled RenderStruct seam" family again: the engine
		// assumes the renderer writes back into object state, and a renderer that
		// only reads looks correct until something downstream quietly stops.
		pGrid->m_Flags |= FLAG_INTERNAL1;

		// --- Which fixed-function path? Exactly d3d_DrawPolyGrid's two tests:
		// an env map is used iff m_pEnvMap is set (and `EnvMapPolyGrids`, which
		// LT_PG_NOENV stands in for), and the Fresnel alpha iff PG_FRESNEL.
		GLuint nEnvName = 0;
		if (pGrid->m_pEnvMap && !glpg_NoEnvMap())
			nEnvName = GLTex_GetName(pGrid->m_pEnvMap);
		const bool bEnvMap  = (nEnvName != 0) && glpg_DebugMode() != 2;
		// ★ CUBIC WATER REFLECTIONS. d3d_DrawPolyGrid asks the env map's RTexture
		// IsCubeMap() (drawpolygrid.cpp:732) and passes it to
		// d3d_SetEnvMapTextureStates, which then uses D3DTTFF_COUNT3 with the raw
		// camera->world matrix instead of COUNT2 with the XZ projection.
		// ⚠️ §38 ported only the NON-cubic branch, because at the time cube DTXs
		// could not load at all (see §59) -- so the water has always taken the
		// wrong branch. C01S01's river authors TexFX\Cubic\JP_BRIDGE1.DTX, which
		// IS a cube map, and once §59 made cube maps real the old
		// `glBindTexture(GL_TEXTURE_2D, ...)` here became an INVALID OPERATION:
		// the unit kept whatever 2D texture happened to be bound and the river
		// turned into a flat pale slab.
		const bool bEnvCube = bEnvMap && GLTex_IsCubeMap(pGrid->m_pEnvMap);
		const bool bFresnel = bEnvMap && (pGrid->m_nPGFlags & PG_FRESNEL) != 0;

		// --- Colour table: the authored 256-entry ramp modulated by the
		// object's own colour, exactly as d3d builds nColorTable. The table is
		// indexed by the SIGNED height byte, hence the +128 bias.
		const float fScaledR = pGrid->m_ColorR * (1.0f / 255.0f);
		const float fScaledG = pGrid->m_ColorG * (1.0f / 255.0f);
		const float fScaledB = pGrid->m_ColorB * (1.0f / 255.0f);

		// No env map in this path, so d3d's nColorAlpha is 255 and the object's
		// own m_ColorA carries the translucency (applied via glColor4f below --
		// D3D gets it from the material/diffuse the same way).
		unsigned char aTable[256][3];
		for (uint32 i = 0; i < 256; ++i)
		{
			float r = pGrid->m_ColorTable[i].x * fScaledR;
			float g = pGrid->m_ColorTable[i].y * fScaledG;
			float b = pGrid->m_ColorTable[i].z * fScaledB;
			aTable[i][0] = (unsigned char)LTCLAMP(r, 0.0f, 255.0f);
			aTable[i][1] = (unsigned char)LTCLAMP(g, 0.0f, 255.0f);
			aTable[i][2] = (unsigned char)LTCLAMP(b, 0.0f, 255.0f);
		}

		// --- Vertex generation (drawpolygrid.cpp:887-905 + the plain loop at
		// :1224). Height bytes are SIGNED; y = data * (dims.y / 127).
		const float fHalfGridWidth  = ((float)pGrid->m_Width  - 1) * 0.5f;
		const float fHalfGridHeight = ((float)pGrid->m_Height - 1) * 0.5f;

		const LTVector &vDims = pGrid->GetDims();
		const float fXInc   = vDims.x * 2.0f / (pGrid->m_Width  - 1);
		const float fZInc   = vDims.z * 2.0f / (pGrid->m_Height - 1);
		const float fYScale = vDims.y / 127.0f;

		const float fXStart = -fHalfGridWidth * fXInc;
		const float fZStart = -fHalfGridHeight * fZInc;

		const float fXScale = pGrid->m_xScale / ((pGrid->m_Width  - 1) * fXInc);
		const float fZScale = pGrid->m_yScale / ((pGrid->m_Height - 1) * fZInc);
		const float fStartU = (float)fmod(pGrid->m_xPan, 1.0f);
		const float fStartV = (float)fmod(pGrid->m_yPan, 1.0f);
		const float fUInc   = fXInc * fXScale;
		const float fVInc   = fZInc * fZScale;

		const uint32 nMaxVerts = pGrid->m_Width * pGrid->m_Height;

		s_vPos.resize(nMaxVerts * 3);
		s_vUV.resize(nMaxVerts * 2);
		s_vColor.resize(nMaxVerts * 4);
		if (bEnvMap)
			s_vNormal.resize(nMaxVerts * 3);

		const int8 *pData = (const int8*)pGrid->m_Data;

		// ★ nColorAlpha, drawpolygrid.cpp:857-862, verbatim: 255 with no env map,
		// 128 with an env map but no Fresnel, and 0 with Fresnel — because in that
		// last case GeneratePolyGridFresnelAlpha ORs the real per-vertex term in
		// afterwards. (Using m_ColorA here instead of 255 on the plain path made
		// the river 75% transparent over near-black rock, which was a large part
		// of why it read as a flat dark sheet.)
		//
		// The env-map path then multiplies by m_ColorA in stage 1
		// (ALPHAOP=MODULATE(TFACTOR, DIFFUSE), TFACTOR.a = m_ColorA). GL's fixed
		// function has no TEXTUREFACTOR, so fold that constant into the vertex
		// alpha on the CPU — identical result, one less state.
		const float fObjAlphaScale = bEnvMap ? (pGrid->m_ColorA * (1.0f / 255.0f)) : 1.0f;
		const unsigned char nAlpha = bEnvMap
			? (unsigned char)((bFresnel ? 0.0f : 128.0f) * fObjAlphaScale)
			: 255;

		// Fresnel needs the camera in the grid's LOCAL space (the space the
		// vertices are generated in) and the table for this grid's authored IOR.
		// glpg_PushGridTransform builds local->world with R/U/F as the columns,
		// so world->local is the transpose: dot the delta against each axis.
		//
		// ⚠️ D3D's GeneratePolyGridFresnelAlpha (drawpolygrid.cpp:550) builds this
		// as `mOrientation * translate(-pos)` using the rotation matrix itself,
		// NOT its inverse — which is only correct for an unrotated grid. Water
		// grids are authored axis-aligned, so the two agree in practice; we use
		// the mathematically correct inverse.
		LTVector vCamLocal(0, 0, 0);
		if (bFresnel)
		{
			const LTVector vDelta = s_vCamPos - pGrid->GetPos();
			vCamLocal.x = vDelta.Dot(pGrid->m_Rotation.Right());
			vCamLocal.y = vDelta.Dot(pGrid->m_Rotation.Up());
			vCamLocal.z = vDelta.Dot(pGrid->m_Rotation.Forward());
			s_FresnelTable.Ensure(LTMAX(1.0003f, pGrid->m_fFresnelVolumeIOR),
			                      pGrid->m_fBaseReflection);
		}

		// Normal generation constants (GenerateNormal, drawpolygrid.cpp:407, as
		// called from the MASKED loop at :1183 — that call site passes the real
		// world spacings, where the unmasked one at :1148 passes the UV tiling
		// scales m_xScale/m_yScale instead. Normals are normalised, so only the
		// ratio matters, and the spacings are the geometrically meaningful pair.)
		const float fSpacingX = fXInc * 2.0f;
		const float fSpacingZ = fZInc * 2.0f;
		const float fNormalY  = fSpacingX * fSpacingZ;
		const int32 nGridW    = (int32)pGrid->m_Width;
		const int32 nGridH    = (int32)pGrid->m_Height;

		if (bLog)
		{
			// Where does this surface actually SIT? "Floating above the world" vs
			// "sunk in the riverbed" is answered by comparing the grid's world-Y
			// span against the world's own bounds — no screenshot needed.
			LTVector vWC, vWH;
			if (GLWorld_GetBounds(vWC, vWH))
				fprintf(stderr, "[glpg]   grid worldY %.0f..%.0f (pos.y %.0f +/- dims.y %.0f) | "
				                "world Y bounds %.0f..%.0f, XZ %.0f..%.0f / %.0f..%.0f\n",
				        pGrid->GetPos().y - vDims.y, pGrid->GetPos().y + vDims.y,
				        pGrid->GetPos().y, vDims.y,
				        vWC.y - vWH.y, vWC.y + vWH.y,
				        vWC.x - vWH.x, vWC.x + vWH.x, vWC.z - vWH.z, vWC.z + vWH.z);
			fprintf(stderr, "[glpg]   grid worldXZ %.0f..%.0f / %.0f..%.0f\n",
			        pGrid->GetPos().x - vDims.x, pGrid->GetPos().x + vDims.x,
			        pGrid->GetPos().z - vDims.z, pGrid->GetPos().z + vDims.z);
		}

		// ★ The env-map/Fresnel authored data. d3d_DrawPolyGrid's env-map path is
		// selected purely by pGrid->m_pEnvMap being non-NULL, and the Fresnel term
		// by PG_FRESNEL — both console vars (EnvMapPolyGrids / FresnelPolyGrids)
		// default to 1. Everything the reflection needs is here; read it before
		// writing any of it.
		if (bLog)
		{
			uint32 nEW = 0, nEH = 0;
			GLuint nEnvName = 0;
			if (pGrid->m_pEnvMap)
			{
				nEnvName = GLTex_GetName(pGrid->m_pEnvMap);
				GLTex_GetDims(pGrid->m_pEnvMap, nEW, nEH);
			}
			fprintf(stderr, "[glpg]   envmap=%s glname=%u %ux%u texType=%d | "
			                "PG_FRESNEL=%d PG_NORMALMAPSPRITE=%d PG_NOBACKFACECULL=%d | "
			                "IOR=%.4f baseRefl=%.4f objAlpha=%u\n",
			        pGrid->m_pEnvMap ? "YES" : "no", (unsigned)nEnvName, nEW, nEH,
			        pGrid->m_pEnvMap ? (int)pGrid->m_pEnvMap->m_eTexType : -1,
			        (pGrid->m_nPGFlags & PG_FRESNEL) ? 1 : 0,
			        (pGrid->m_nPGFlags & PG_NORMALMAPSPRITE) ? 1 : 0,
			        (pGrid->m_nPGFlags & PG_NOBACKFACECULL) ? 1 : 0,
			        pGrid->m_fFresnelVolumeIOR, pGrid->m_fBaseReflection,
			        pGrid->m_ColorA);
		}

		if (bLog)
			fprintf(stderr, "[glpg]   color=(%u %u %u) objAlpha=%u envmap=%s pgflags=0x%x "
			                "table[0]=(%.0f %.0f %.0f) table[128]=(%.0f %.0f %.0f) table[255]=(%.0f %.0f %.0f)\n",
			        pGrid->m_ColorR, pGrid->m_ColorG, pGrid->m_ColorB, pGrid->m_ColorA,
			        pGrid->m_pEnvMap ? "YES" : "no", pGrid->m_nPGFlags,
			        pGrid->m_ColorTable[0].x, pGrid->m_ColorTable[0].y, pGrid->m_ColorTable[0].z,
			        pGrid->m_ColorTable[128].x, pGrid->m_ColorTable[128].y, pGrid->m_ColorTable[128].z,
			        pGrid->m_ColorTable[255].x, pGrid->m_ColorTable[255].y, pGrid->m_ColorTable[255].z);

		// ★ THE VALID MASK. A grid may be a SPARSE height field: a 1-bit-per-cell
		// mask says which cells exist, cells without a bit emit NO vertex, and
		// m_Indices is built against that COMPACTED numbering. This is how a
		// level author carves a water surface to the shape of a riverbed — and
		// it is not an edge case: C01S01's stream under the red bridge is a
		// 150x70 masked grid, i.e. the first water the player ever sees.
		//
		// Bit order, exactly as drawpolygrid.cpp walks it: bit 0 of the first
		// uint32 is the first cell of a row; the word advances after bit 31; at
		// the end of a row a partially-consumed word is finished off, so each row
		// costs ceil(Width/32) words.
		const uint32 *pMask = pGrid->m_pValidMask;
		const uint32 nMaskLineAdjust = (pGrid->m_Width % 32) ? 1 : 0;

		const int8 *pDataBase = (const int8*)pGrid->m_Data;

		uint32 nOut = 0;
		float fCurrZ = fZStart, fCurrV = fStartV;
		for (uint32 y = 0; y < pGrid->m_Height; ++y)
		{
			float fCurrX = fXStart, fCurrU = fStartU;
			uint32 nShift = 0x1;

			for (uint32 x = 0; x < pGrid->m_Width; ++x)
			{
				const int8 nHeight = *pData++;

				if (!pMask || (*pMask & nShift))
				{
					const uint32 nTableIdx = (uint32)(nHeight + 128);

					s_vPos[nOut * 3 + 0] = fCurrX;
					s_vPos[nOut * 3 + 1] = (float)nHeight * fYScale;
					s_vPos[nOut * 3 + 2] = fCurrZ;

					s_vUV[nOut * 2 + 0] = fCurrU;
					s_vUV[nOut * 2 + 1] = fCurrV;

					s_vColor[nOut * 4 + 0] = aTable[nTableIdx][0];
					s_vColor[nOut * 4 + 1] = aTable[nTableIdx][1];
					s_vColor[nOut * 4 + 2] = aTable[nTableIdx][2];
					s_vColor[nOut * 4 + 3] = nAlpha;

					if (bEnvMap)
					{
						// GenerateNormal: a central difference over the height
						// field, in GRID-LOCAL space (which is where these
						// vertices live, so GL's texgen sees it correctly once
						// the modelview carries the grid transform).
						//
						// D3D indexes +/-1 and +/-Width unconditionally, which
						// reads across row boundaries (and one byte before
						// m_Data at the very first vertex). Clamp instead — it
						// only differs on the edge row/column and it cannot
						// read out of the array.
						const int32 x0 = (x > 0)                    ? (int32)x - 1 : (int32)x;
						const int32 x1 = ((int32)x < nGridW - 1)    ? (int32)x + 1 : (int32)x;
						const int32 y0 = (y > 0)                    ? (int32)y - 1 : (int32)y;
						const int32 y1 = ((int32)y < nGridH - 1)    ? (int32)y + 1 : (int32)y;

						const int32 nRow = (int32)y * nGridW;
						LTVector vN(
							((int32)pDataBase[nRow + x0] - (int32)pDataBase[nRow + x1]) * fYScale * fSpacingZ,
							fNormalY,
							((int32)pDataBase[y0 * nGridW + (int32)x] - (int32)pDataBase[y1 * nGridW + (int32)x]) * fYScale * fSpacingX);
						vN.Normalize();

						s_vNormal[nOut * 3 + 0] = vN.x;
						s_vNormal[nOut * 3 + 1] = vN.y;
						s_vNormal[nOut * 3 + 2] = vN.z;

						if (bFresnel)
						{
							// GeneratePolyGridFresnelAlpha (:576): the term is
							// |N . normalize(camera - vertex)|, both in grid
							// space. D3D ORs the table byte into the alpha slot
							// of a colour whose alpha is 0, i.e. it ASSIGNS;
							// then stage 1 scales it by m_ColorA.
							const LTVector vToCam(vCamLocal.x - fCurrX,
							                      vCamLocal.y - (float)nHeight * fYScale,
							                      vCamLocal.z - fCurrZ);
							const float fMag = vToCam.Mag();
							const float fDot = (fMag > 0.0001f) ? (vToCam.Dot(vN) / fMag) : 1.0f;
							s_vColor[nOut * 4 + 3] = (unsigned char)
								(s_FresnelTable.GetValue(fDot) * fObjAlphaScale);
						}
					}

					++nOut;
				}

				fCurrX += fXInc;
				fCurrU += fUInc;

				if (pMask)
				{
					if (nShift == 0x80000000) { ++pMask; nShift = 1; }
					else                      { nShift <<= 1; }
				}
			}

			if (pMask)
				pMask += nMaskLineAdjust;
			fCurrZ += fZInc;
			fCurrV += fVInc;
		}

		// ★★ IS THE HEIGHT FIELD ACTUALLY A HEIGHT FIELD?
		//
		// LT_PG_HEIGHTS=1. Every shading theory about "the water is a flat grey
		// sheet" presupposes the geometry undulates. It is cheaper to check that
		// than to argue about it: report the min/max of the signed height bytes
		// and how often that pair CHANGES. min==max means the surface is a plane
		// and no amount of colour-table work will ever make it read as water,
		// because the ramp (white at -128, black at +127) is being sampled at a
		// single point. A changing pair means CPolyGridFX is animating it.
		//
		// ⚠️ Reports on CHANGE plus a heartbeat, never once: a one-shot here
		// would print the first frame's values and could not distinguish
		// "static" from "animating" at all (§34/§36's rule).
		if (glpg_TraceHeights())
		{
			// ⚠️ CHECKSUM, not min/max. min/max cannot tell a static surface from
			// an animated one: a travelling wave keeps its extremes exactly
			// constant while every individual cell changes. (Measured before this
			// was fixed: min=-37 max=70 for 3700 consecutive draws, which looked
			// like "frozen" and proved nothing.) A checksum changes iff the DATA
			// changes, which is the actual question.
			uint32 nSum = 2166136261u;
			int nMin = 127, nMax = -128;
			for (uint32 i = 0; i < nMaxVerts; ++i)
			{
				const int v = ((const int8*)pGrid->m_Data)[i];
				if (v < nMin) nMin = v;
				if (v > nMax) nMax = v;
				nSum = (nSum ^ (uint32)(uint8)v) * 16777619u;
			}

			// And the OTHER half of "why does it look uniform?": the range of
			// vertex colour actually emitted. The ramp runs white->black over the
			// height byte, so a surface with relief must emit a SPREAD. If this
			// reports a spread and the screen shows a uniform sheet, the vertex
			// colour is being discarded downstream, and that is a renderer bug
			// rather than a data one.
			int nCMin = 255, nCMax = 0;
			for (uint32 i = 0; i < nOut; ++i)
			{
				const int c = s_vColor[i * 4 + 0];
				if (c < nCMin) nCMin = c;
				if (c > nCMax) nCMax = c;
			}
			int nAMin = 255, nAMax = 0;
			for (uint32 i = 0; i < nOut; ++i)
			{
				const int a = s_vColor[i * 4 + 3];
				if (a < nAMin) nAMin = a;
				if (a > nAMax) nAMax = a;
			}

			// ⚠️ PER-GRID state. A level can hold several polygrids (the retail
			// campaign entry has two: c01's cinematic pool and C01S01's river),
			// and one shared set of statics makes them indistinguishable — every
			// draw looks like a "change" as the counters ping-pong between grids,
			// or worse, only the first grid is ever described. Key on the object.
			struct GridStat { const void *pGrid; uint32 nSum, nChanges, nCalls; };
			static GridStat s_aStats[8] = {};
			GridStat *pStat = NULL;
			for (uint32 i = 0; i < 8; ++i)
			{
				if (s_aStats[i].pGrid == pGrid || s_aStats[i].pGrid == NULL)
				{
					s_aStats[i].pGrid = pGrid;
					pStat = &s_aStats[i];
					break;
				}
			}
			if (pStat)
			{
				++pStat->nCalls;
				const bool bChanged = (pStat->nCalls > 1) && (nSum != pStat->nSum);
				if (bChanged)
					++pStat->nChanges;
				if (bChanged ? (pStat->nChanges <= 3 || (pStat->nChanges % 200) == 0)
				             : ((pStat->nCalls % 120) == 0))
				{
					fprintf(stderr, "[glpg] heights %ux%u %s: sum=%08x min=%d max=%d span=%d | "
					                "vtxRGB %d..%d  vtxA %d..%d | %u data changes / %u draws%s\n",
					        pGrid->m_Width, pGrid->m_Height,
					        bChanged ? "CHANGED " : "SAME    ",
					        nSum, nMin, nMax, nMax - nMin, nCMin, nCMax, nAMin, nAMax,
					        pStat->nChanges, pStat->nCalls,
					        (nMin == nMax) ? "  <-- ⚠️ FLAT" : "");
				}
				pStat->nSum = nSum;
			}
		}

		// Mask-walk sanity, on the census frame only (the mask and index list are
		// fixed for the grid's lifetime — only the heights animate). If our bit
		// walk disagreed with the one that produced m_Indices, the indices would
		// reach past the compacted vertex count; say so instead of drawing a
		// scrambled surface. (Not a memory-safety issue: the arrays are always
		// sized for the FULL Width*Height, and nOut <= that, so a desync stays
		// in-bounds — it would just look wrong.)
		if (bLog)
		{
			uint32 nMaxIndex = 0;
			for (uint32 i = 0; i < pGrid->m_nIndices; ++i)
				if (pGrid->m_Indices[i] > nMaxIndex)
					nMaxIndex = pGrid->m_Indices[i];
			if (nMaxIndex >= nOut)
				fprintf(stderr, "[glpg] ⚠️ mask desync: max index %u >= %u emitted verts\n",
				        nMaxIndex, nOut);
		}

		// --- Texture: the grid animates its texture through a sprite, exactly
		// like an OT_SPRITE (LTPolyGrid::m_pSprite / m_SpriteTracker).
		GLuint nTexName = 0;
		if (pGrid->m_pSprite && pGrid->m_SpriteTracker.m_pCurFrame)
		{
			SharedTexture *pTex = pGrid->m_SpriteTracker.m_pCurFrame->m_pTex;
			if (pTex)
				nTexName = GLTex_GetName(pTex);
		}

		// ★ PG_NORMALMAPSPRITE: "the sprite surface of the polygrid is entirely a
		// normal map, and if not rendering with bump mapping, to NOT USE ANY
		// TEXTURE" (iltclient.h:104). We never render with bump mapping (no pixel
		// shaders), so this sprite must be dropped — otherwise the water is
		// textured with a raw normal map, i.e. a lilac bumpy sheet. c01's
		// cinematic grid sets this flag; C01S01's river does not.
		const bool bNormalMapSprite = (pGrid->m_nPGFlags & PG_NORMALMAPSPRITE) != 0;
		if (bNormalMapSprite)
			nTexName = 0;

		// On the env-map path the base texture is not used at all (see
		// glpg_EnvBase's note) unless the LT_PG_ENVBASE A/B asks for it.
		const bool bBaseTex = nTexName != 0 && (!bEnvMap || glpg_EnvBase());

		// --- State. Blend mode from the object's flags2, the same rule the
		// sprite path uses (d3d_GetBlendStates, d3d_draw.h:128).
		const bool bAdditive = (pGrid->m_Flags2 & FLAG2_ADDITIVE) != 0;
		const bool bMultiply = (pGrid->m_Flags2 & FLAG2_MULTIPLY) != 0;
		if (bAdditive)
			glBlendFunc(GL_ONE, GL_ONE);
		else if (bMultiply)
			glBlendFunc(GL_ZERO, GL_SRC_COLOR);
		else
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_BLEND);

		GLWorld_ApplyObjectFog(pGrid->m_Flags, pGrid->m_Flags2);

		if (glpg_DebugMode() == 2)
		{
			// LT_PG_WIRE: strip every shading variable.
			nTexName = 0;
			glDisable(GL_BLEND);
			glDisable(GL_FOG);
			glDisable(GL_TEXTURE_2D);
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			for (uint32 i = 0; i < nOut; ++i)
			{
				s_vColor[i * 4 + 0] = 255;
				s_vColor[i * 4 + 1] = 0;
				s_vColor[i * 4 + 2] = 255;
				s_vColor[i * 4 + 3] = 255;
			}
		}

		// The reflection lives on whichever unit is free: unit 0 on its own, or
		// unit 1 when LT_PG_ENVBASE also wants the base texture underneath.
		const GLenum eEnvUnit = (bEnvMap && bBaseTex) ? GL_TEXTURE1 : GL_TEXTURE0;

		if (bBaseTex)
		{
			glActiveTexture(GL_TEXTURE0);
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, nTexName);
			// texture x diffuse. On the plain path this is stage 0's authored
			// MODULATE (drawpolygrid.cpp:322); under LT_PG_ENVBASE it stands in
			// for MODULATE2X's first half, and unit 1 finishes the job.
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			// The surface pans; the UVs run well outside 0..1.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		}
		else if (eEnvUnit != GL_TEXTURE0)
		{
			glActiveTexture(GL_TEXTURE0);
			glDisable(GL_TEXTURE_2D);
		}

		if (bEnvMap)
		{
			// ================= THE REFLECTION =================
			// d3d_SetEnvMapTextureStates (drawpolygrid.cpp:340) generates
			// D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR and then transforms it by
			// mCamToWorld * mTex1Trans. GL's equivalent of the first half is
			// GL_REFLECTION_MAP texgen, which emits the EYE-space reflection
			// vector into (s,t,r) — so all three coords must be generated, even
			// for a 2D texture, because the texture matrix mixes them.
			glActiveTexture(eEnvUnit);
			if (bEnvCube)
			{
				glDisable(GL_TEXTURE_2D);
				glEnable(GL_TEXTURE_CUBE_MAP);
				glBindTexture(GL_TEXTURE_CUBE_MAP, nEnvName);
			}
			else
			{
				glEnable(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, nEnvName);
				// A reflection lookup must not wrap: the coords leave 0..1 wherever
				// the reflection points away from the mapped hemisphere.
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
			glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
			glEnable(GL_TEXTURE_GEN_S);
			glEnable(GL_TEXTURE_GEN_T);
			glEnable(GL_TEXTURE_GEN_R);

			// The texture matrix: eye-space reflection -> world space -> the XZ
			// projection D3D's mTex1Trans performs (u = 0.5*Rw.x + 0.5,
			// v = 0.5*Rw.z + 0.5).
			//
			// mCamToWorld's rows are the camera's world-space axes, so
			// Rw = Reye.x*right + Reye.y*up + Reye.z*forward. ⚠️ GL eye space is
			// RIGHT-handed and our view matrix negates the forward row
			// (nullrender.cpp:402), so GL's +Z eye axis is -forward: the third
			// column here must be NEGATED or the reflection is mirrored
			// front-to-back and slides the wrong way as the player turns.
			const LTVector &vR2 = s_vCamRight;
			const LTVector &vU2 = s_vCamUp;
			const LTVector &vF2 = s_vCamForward;
			float aTex[16];
			memset(aTex, 0, sizeof(aTex));
			if (bEnvCube)
			{
				// D3DTTFF_COUNT3: mTex1Trans is SKIPPED entirely and the raw
				// camera->world rotation is the whole transform, feeding all three
				// coordinates to the cube lookup. Same shape as the world-surface
				// cubic branch in gl_worlddata.cpp (§59).
				aTex[0] = vR2.x; aTex[4] = vU2.x; aTex[8]  = -vF2.x;
				aTex[1] = vR2.y; aTex[5] = vU2.y; aTex[9]  = -vF2.y;
				aTex[2] = vR2.z; aTex[6] = vU2.z; aTex[10] = -vF2.z;
				aTex[15] = 1.0f;
			}
			else
			{
			// row 0 -> s' = 0.5*Rw.x + 0.5
			aTex[0]  = 0.5f * vR2.x;  aTex[4]  = 0.5f * vU2.x;  aTex[8]  = -0.5f * vF2.x;  aTex[12] = 0.5f;
			// row 1 -> t' = 0.5*Rw.z + 0.5
			aTex[1]  = 0.5f * vR2.z;  aTex[5]  = 0.5f * vU2.z;  aTex[9]  = -0.5f * vF2.z;  aTex[13] = 0.5f;
			aTex[15] = 1.0f;
			}

			glMatrixMode(GL_TEXTURE);
			glPushMatrix();
			glLoadMatrixf(aTex);
			glMatrixMode(GL_MODELVIEW);

			// stage-1 COLOROP = MODULATE(DIFFUSE, TEXTURE); ALPHAOP is
			// MODULATE(TFACTOR, DIFFUSE), which we pre-folded into the vertex
			// alpha — so alpha here is REPLACE(PRIMARY_COLOR). It must NOT come
			// from the env map (GL_MODULATE would multiply the reflection's own
			// alpha in and, for an opaque env map, that happens to be harmless,
			// but for one with an alpha channel it would silently dim the water).
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
			glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB,
			          (eEnvUnit == GL_TEXTURE0) ? GL_PRIMARY_COLOR : GL_PREVIOUS);
			glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
			// Stage 1's authored op is a PLAIN modulate, so no scale on the
			// faithful path. The 2x belongs to stage 0's MODULATE2X, which only
			// exists in the LT_PG_ENVBASE reading — and since the scale is
			// applied at the end of the last unit either way, 2*(base*diffuse)
			// *env is the same number, so it can live here.
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, bBaseTex ? 2.0f : 1.0f);

			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
			glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		}
		else if (!bBaseTex)
		{
			glDisable(GL_TEXTURE_2D);
		}

		glpg_PushGridTransform(pGrid);

		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		glVertexPointer(3, GL_FLOAT, 0, &s_vPos[0]);
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, &s_vColor[0]);
		if (bBaseTex)
		{
			glClientActiveTexture(GL_TEXTURE0);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glTexCoordPointer(2, GL_FLOAT, 0, &s_vUV[0]);
		}
		if (bEnvMap)
		{
			// texgen supplies the coords, but the NORMAL is the input it derives
			// them from — without this array every reflection coord is identical.
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_FLOAT, 0, &s_vNormal[0]);
		}

		glDrawElements(GL_TRIANGLES, (GLsizei)pGrid->m_nIndices,
		               GL_UNSIGNED_SHORT, pGrid->m_Indices);

		glDisableClientState(GL_NORMAL_ARRAY);
		if (bBaseTex)
		{
			glClientActiveTexture(GL_TEXTURE0);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);

		glPopMatrix();

		if (bEnvMap)
		{
			// Undo everything the env-map path touched, on its own unit, and
			// leave unit 0 selected — d3d_UnsetEnvMapTextureStates' job. A
			// leaked texture matrix or texgen enable would corrupt every later
			// pass in the frame (the sprites and the player-view weapon).
			glActiveTexture(eEnvUnit);
			glDisable(GL_TEXTURE_GEN_S);
			glDisable(GL_TEXTURE_GEN_T);
			glDisable(GL_TEXTURE_GEN_R);
			glMatrixMode(GL_TEXTURE);
			glPopMatrix();
			glMatrixMode(GL_MODELVIEW);
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			if (bEnvCube)
				glDisable(GL_TEXTURE_CUBE_MAP);
			glDisable(GL_TEXTURE_2D);
			if (eEnvUnit != GL_TEXTURE0)
			{
				glActiveTexture(GL_TEXTURE0);
				glDisable(GL_TEXTURE_2D);
			}
		}

		if (glpg_DebugMode() == 2)
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		if (bLog)
			fprintf(stderr, "[glpg] grid %ux%u @(%.0f %.0f %.0f) dims=(%.0f %.0f %.0f) "
			                "%u verts%s %u tris basetex=%u envtex=%u alpha=%s %s\n",
			        pGrid->m_Width, pGrid->m_Height,
			        pGrid->GetPos().x, pGrid->GetPos().y, pGrid->GetPos().z,
			        vDims.x, vDims.y, vDims.z,
			        nOut, pGrid->m_pValidMask ? " (masked)" : "",
			        pGrid->m_nIndices / 3, bBaseTex ? nTexName : 0,
			        bEnvMap ? (unsigned)nEnvName : 0,
			        bFresnel ? "fresnel" : (bEnvMap ? "envmap-flat" : "opaque-255"),
			        bAdditive ? "additive" : (bMultiply ? "multiply" : "alpha"));

		return true;
	}
}

void GLPolyGrid_ArmCensus(void)
{
	s_bCensus[0] = s_bCensus[1] = false;
}

void GLPolyGrid_SetCamera(const LTVector &vRight, const LTVector &vUp,
                          const LTVector &vForward, const LTVector &vPos)
{
	s_vCamRight   = vRight;
	s_vCamUp      = vUp;
	s_vCamForward = vForward;
	s_vCamPos     = vPos;
}


void GLPolyGrid_Draw(bool bTranslucent)
{
	if (!g_pClientMgr || glpg_DebugMode() == 1)
		return;

	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_POLYGRID].m_Head;
	if (pHead->m_pNext == pHead)
		return;

	// One-shot inventory per pass, like the [glm] model census.
	bool bLog = !s_bCensus[bTranslucent ? 1 : 0];

	// Save the states we touch so the rest of the frame is unaffected.
	glEnable(GL_DEPTH_TEST);
	glDepthMask(bTranslucent ? GL_FALSE : GL_TRUE);
	// The models and sprite passes both draw unculled; polygrids follow, which
	// also sidesteps the winding question our LH->RH view flip introduces.
	// (D3D culls CCW unless PG_NOBACKFACECULL -- water is a single sheet, so
	// the practical difference is only seen from underneath.)
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);

	uint32 nDrawn = 0, nTotal = 0, nInvisible = 0, nGroupOff = 0, nOtherPass = 0;
	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		LTPolyGrid *pGrid = (LTPolyGrid*)pCur->m_pData;
		if (!pGrid)
			continue;
		++nTotal;
		if (!(pGrid->m_Flags & FLAG_VISIBLE)) { ++nInvisible; continue; }
		if (g_pGLStruct && g_pGLStruct->IsObjectGroupEnabled &&
		    !g_pGLStruct->IsObjectGroupEnabled(pGrid->m_nRenderGroup)) { ++nGroupOff; continue; }
		if (pGrid->IsTranslucent() != bTranslucent) { ++nOtherPass; continue; }

		if (glpg_DrawGrid(pGrid, bLog))
			++nDrawn;
	}

	if (bLog)
	{
		s_bCensus[bTranslucent ? 1 : 0] = true;
		fprintf(stderr, "[glpg] %s pass @swapframe %d: %u polygrids, %u drawn "
		                "(%u !visible, %u group off, %u other pass)\n",
		        bTranslucent ? "translucent" : "opaque", g_nSwapCount,
		        nTotal, nDrawn, nInvisible, nGroupOff, nOtherPass);
	}

	GLWorld_RestoreSceneFog();
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glDisable(GL_TEXTURE_2D);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}
