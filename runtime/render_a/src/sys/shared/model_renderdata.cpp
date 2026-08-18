// ----------------------------------------------------------------------- //
//
// MODULE  : model_renderdata.cpp
//
// PURPOSE : GL model rendering. The LTB mesh loaders below mirror
//           CD3DRigidMesh::Load / CD3DSkelMesh::Load{_RD,_MP}
//           (d3dmeshrendobj_rigid.cpp / d3dmeshrendobj_skel.cpp) byte for
//           byte, but keep the data in CPU arrays and CPU-skin at draw time
//           (a few thousand verts per model — cheap at this scale).
//
//           Transforms come from ModelInstance::GetRenderingTransforms():
//           per-node DDMatrix (bind-pose model space -> world space, scale
//           included). DDMatrix memory order is the transpose of LTMatrix,
//           i.e. p' = (_11*x + _21*y + _31*z + _41, ...).
//
//           Every Load is bracketed by the size prefix: read u32 objSize,
//           parse, then SeekTo(start + objSize) — so a partial/unknown parse
//           can never desync the model file.
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "de_objects.h"      // ModelInstance / Attachment / DDMatrix
#include "model.h"           // Model / ModelPiece / CDIModelDrawable
#include "ltb.h"             // LTB_Header
#include "iltstream.h"
#include "clientmgr.h"       // g_pClientMgr (object lists)
#include "renderstruct.h"
#include "model_renderdata.h"
#include "mtl_device.h"   // MTLDev_IsMetalBackend -- the backend branch
#include "mtl_model.h"    // the Metal model emit
#include "mtl_matrix.h"
#include "render_particles.h"   // player-view particle systems draw inside the PV pass
#include "world_renderdata.h"   // RWorld_IsLoaded (census timing)
#include "render_style.h" // authored per-piece render state
#include "world_shared_bsp.h"  // IWorldSharedBSP::LightTable (model ambient, §68)
#include "world_client_bsp.h"  // IWorldClientBSP::ClientTree (static-light query)
#include "world_tree.h"        // FindObjInfo / NOA_Lights / StaticLight
#include "de_mainworld.h"      // w_DoLightLookup
#include "fullintersectline.h" // i_IntersectSegment (sun sky-visibility ray)
// ★ The renderer's own console variables (§71). gl_convar.cpp instantiates them
// and nr_ReadConsoleVariables refreshes them whenever the console changes, so
// reading `g_CV_DynamicLight.m_Val` here is the same live value D3D reads —
// and it is a real player-facing option ("Dynamic lights" in Display), not an
// internal default. ⚠️ Read the VARIABLE, never the RCONVAR default (§60).
#include "rendererconsolevars.h"
#include "sys/shared/render_texture.h"  // RTex_* — neutral texture queries
#include "sys/shared/render_globals.h"  // g_pRenderStruct — the engine function table

#include <string>          // LT_TRACE_PV set signature
#include <stdio.h>
#include <math.h>       // tanf (really-close projection)
#include <vector>
#include <algorithm>   // std::fill (the per-vertex colour cache stamp wrap)
#include <set>          // LT_TRACE_MODELLIGHT distinct-value set
#include <algorithm>         // stable_sort for the authored piece render priority

// ★ THE WORLD'S PRECOMPUTED LIGHT GRID (§68). The same holder the D3D renderer
// uses (setuptouchinglights.cpp:26). The grid is already loaded on our path —
// CWorldSharedBSP::LoadLightGrid runs during the normal shared-BSP world load —
// so this is a read of data the port has always had and never used.
static IWorldSharedBSP *g_pModelWorldBSP;
define_holder(IWorldSharedBSP, g_pModelWorldBSP);
static IWorldClientBSP *g_pModelWorldClient;
define_holder(IWorldClientBSP, g_pModelWorldClient);

// The scene camera basis (RSprite_SetCamera, below). Declared here because the
// model LIGHTING needs it as well as the sprite billboards: player-view models
// are in camera space and must be round-tripped through world space to be lit
// (§69).
static LTVector g_vSprCamRight(1, 0, 0), g_vSprCamUp(0, 1, 0);
static LTVector g_vSprCamFwd(0, 0, 1), g_vSprCamPos(0, 0, 0);

// LT_TRACE_UI state (defined further down, next to the census flags).
extern int  g_nRSceneFrame;
extern bool g_bRTraceUIFrame;
extern bool g_bRInterfacePass;

// Vertex data type flags (match the LTB packer; see d3d_utils.h).
#define RM_VERTDATATYPE_POSITION      0x0001
#define RM_VERTDATATYPE_NORMAL        0x0002
#define RM_VERTDATATYPE_UVSETS_1      0x0010
#define RM_VERTDATATYPE_UVSETS_2      0x0020
#define RM_VERTDATATYPE_UVSETS_3      0x0040
#define RM_VERTDATATYPE_UVSETS_4      0x0080
#define RM_VERTDATATYPE_BASISVECTORS  0x0100

// Vertex blend types (VERTEX_BLEND_TYPE order in d3d_utils.h).
enum
{
	kRMBlend_None         = 0,   // 1 bone, no weights in the vertex
	kRMBlend_NonIndexed_B1 = 1,  // 2 bones, 1 weight
	kRMBlend_NonIndexed_B2 = 2,
	kRMBlend_NonIndexed_B3 = 3,
	kRMBlend_Indexed_B1   = 4,   // 2 bones, 1 weight + uint8 index[4]
	kRMBlend_Indexed_B2   = 5,
	kRMBlend_Indexed_B3   = 6
};

// Field offsets within one packed LTB vertex (-1 = absent). Field order is
// the D3D FVF order: position, blend weights, [beta indices], normal, UVs,
// [tangent+binormal appended last].
struct RMLayout
{
	uint32 m_nStride;
	int    m_nPos, m_nBlend, m_nIndex, m_nNormal, m_nUV;
	uint32 m_nBlendCount;
};

static void rm_ComputeLayout(int nBlendType, uint32 nFlags, RMLayout &cLayout)
{
	cLayout.m_nStride = 0;
	cLayout.m_nPos = cLayout.m_nBlend = cLayout.m_nIndex = cLayout.m_nNormal = cLayout.m_nUV = -1;
	cLayout.m_nBlendCount = 0;

	uint32 nOff = 0;
	if ((nFlags & RM_VERTDATATYPE_POSITION) && (nFlags & RM_VERTDATATYPE_NORMAL))
	{
		cLayout.m_nPos = (int)nOff;
		nOff += 3 * sizeof(float);

		uint32 nBlends = 0;
		bool bIndexed = false;
		switch (nBlendType)
		{
			case kRMBlend_NonIndexed_B1: nBlends = 1; break;
			case kRMBlend_NonIndexed_B2: nBlends = 2; break;
			case kRMBlend_NonIndexed_B3: nBlends = 3; break;
			case kRMBlend_Indexed_B1:    nBlends = 1; bIndexed = true; break;
			case kRMBlend_Indexed_B2:    nBlends = 2; bIndexed = true; break;
			case kRMBlend_Indexed_B3:    nBlends = 3; bIndexed = true; break;
			default: break;
		}
		if (nBlends)
		{
			cLayout.m_nBlend = (int)nOff;
			cLayout.m_nBlendCount = nBlends;
			nOff += nBlends * sizeof(float);
		}
		if (bIndexed)
		{
			cLayout.m_nIndex = (int)nOff;
			nOff += 4;
		}

		cLayout.m_nNormal = (int)nOff;
		nOff += 3 * sizeof(float);
	}

	uint32 nUVSets = 0;
	if      (nFlags & RM_VERTDATATYPE_UVSETS_1) nUVSets = 1;
	else if (nFlags & RM_VERTDATATYPE_UVSETS_2) nUVSets = 2;
	else if (nFlags & RM_VERTDATATYPE_UVSETS_3) nUVSets = 3;
	else if (nFlags & RM_VERTDATATYPE_UVSETS_4) nUVSets = 4;
	if (nUVSets)
	{
		cLayout.m_nUV = (int)nOff;                 // first set is the base UV
		nOff += nUVSets * 2 * sizeof(float);
	}

	if (nFlags & RM_VERTDATATYPE_BASISVECTORS)
		nOff += 6 * sizeof(float);                 // tangent+binormal, unused

	cLayout.m_nStride = nOff;
}

// DDMatrix (transposed-LTMatrix memory order) transform helpers.
static inline void rm_TransformPoint(const DDMatrix &m, const float *pIn, LTVector &vOut)
{
	vOut.x = m._11 * pIn[0] + m._21 * pIn[1] + m._31 * pIn[2] + m._41;
	vOut.y = m._12 * pIn[0] + m._22 * pIn[1] + m._32 * pIn[2] + m._42;
	vOut.z = m._13 * pIn[0] + m._23 * pIn[1] + m._33 * pIn[2] + m._43;
}

static inline void rm_RotateVector(const DDMatrix &m, const float *pIn, LTVector &vOut)
{
	vOut.x = m._11 * pIn[0] + m._21 * pIn[1] + m._31 * pIn[2];
	vOut.y = m._12 * pIn[0] + m._22 * pIn[1] + m._32 * pIn[2];
	vOut.z = m._13 * pIn[0] + m._23 * pIn[1] + m._33 * pIn[2];
}

// ---------------------------------------------------------------------------
// Mesh classes (returned through CreateRenderObject; the engine treats them
// as CDIModelDrawable and calls Load during model load).
// ---------------------------------------------------------------------------

class RModelMesh : public CDIModelDrawable
{
public:
	uint32 m_nVertCount, m_nPolyCount;

	std::vector<float>  m_aPos;      // 3 per vert
	std::vector<float>  m_aNormal;   // 3 per vert
	std::vector<float>  m_aUV;       // 2 per vert
	std::vector<float>  m_aBlend;    // m_nBlendPerVert per vert (may be 0)
	std::vector<uint8>  m_aBoneIdx;  // 4 per vert (indexed/MP meshes only)
	std::vector<uint16> m_aIndices;  // 3 per tri
	uint32 m_nBlendPerVert;

	RModelMesh() : m_nVertCount(0), m_nPolyCount(0), m_nBlendPerVert(0) {}

	virtual uint32 GetVertexCount() { return m_nVertCount; }
	virtual uint32 GetPolyCount()   { return m_nPolyCount; }

	// Reads the (up to 4) vertex streams + the index list.
	bool ReadStreams(ILTStream &File, const uint32 *pStreamFlags, int nBlendType)
	{
		m_aPos.resize((size_t)m_nVertCount * 3, 0.0f);
		m_aNormal.resize((size_t)m_nVertCount * 3, 0.0f);
		m_aUV.resize((size_t)m_nVertCount * 2, 0.0f);

		std::vector<uint8> aStream;
		for (uint32 nStream = 0; nStream < 4; ++nStream)
		{
			if (!pStreamFlags[nStream])
				continue;

			RMLayout cLayout;
			rm_ComputeLayout(nBlendType, pStreamFlags[nStream], cLayout);
			if (!cLayout.m_nStride)
				return false;

			aStream.resize((size_t)cLayout.m_nStride * m_nVertCount);
			File.Read(&aStream[0], (uint32)aStream.size());

			if (cLayout.m_nBlendCount && m_aBlend.empty())
			{
				m_nBlendPerVert = cLayout.m_nBlendCount;
				m_aBlend.resize((size_t)m_nVertCount * m_nBlendPerVert, 0.0f);
			}
			if (cLayout.m_nIndex >= 0 && m_aBoneIdx.empty())
				m_aBoneIdx.resize((size_t)m_nVertCount * 4, 0);

			for (uint32 nVert = 0; nVert < m_nVertCount; ++nVert)
			{
				const uint8 *pVert = &aStream[(size_t)nVert * cLayout.m_nStride];
				if (cLayout.m_nPos >= 0)
					memcpy(&m_aPos[(size_t)nVert * 3], pVert + cLayout.m_nPos, 3 * sizeof(float));
				if (cLayout.m_nNormal >= 0)
					memcpy(&m_aNormal[(size_t)nVert * 3], pVert + cLayout.m_nNormal, 3 * sizeof(float));
				if (cLayout.m_nUV >= 0)
					memcpy(&m_aUV[(size_t)nVert * 2], pVert + cLayout.m_nUV, 2 * sizeof(float));
				if (cLayout.m_nBlend >= 0)
					memcpy(&m_aBlend[(size_t)nVert * m_nBlendPerVert], pVert + cLayout.m_nBlend,
					       m_nBlendPerVert * sizeof(float));
				if (cLayout.m_nIndex >= 0)
					memcpy(&m_aBoneIdx[(size_t)nVert * 4], pVert + cLayout.m_nIndex, 4);
			}
		}

		m_aIndices.resize((size_t)m_nPolyCount * 3);
		if (m_nPolyCount)
			File.Read(&m_aIndices[0], (uint32)(m_aIndices.size() * sizeof(uint16)));
		return true;
	}
};

class RRigidMesh : public RModelMesh
{
public:
	uint32 m_nBoneEffector;

	RRigidMesh() : m_nBoneEffector(0) { m_Type = eRigidMesh; }

	virtual bool Load(ILTStream &File, LTB_Header &Header)
	{
		uint32 nObjSize = 0;
		File.Read(&nObjSize, sizeof(nObjSize));
		uint32 nEndPos = 0;
		File.GetPos(&nEndPos);
		nEndPos += nObjSize;

		bool bOk = (Header.m_iFileType == LTB_D3D_MODEL_FILE &&
		            Header.m_iVersion == CD3D_LTB_LOAD_VERSION);
		if (bOk)
		{
			uint32 nMaxBonesPerTri, nMaxBonesPerVert, aStreamFlags[4];
			File.Read(&m_nVertCount, sizeof(m_nVertCount));
			File.Read(&m_nPolyCount, sizeof(m_nPolyCount));
			File.Read(&nMaxBonesPerTri, sizeof(nMaxBonesPerTri));
			File.Read(&nMaxBonesPerVert, sizeof(nMaxBonesPerVert));
			File.Read(&aStreamFlags[0], sizeof(aStreamFlags));
			File.Read(&m_nBoneEffector, sizeof(m_nBoneEffector));

			bOk = ReadStreams(File, aStreamFlags, kRMBlend_None);
		}

		File.SeekTo(nEndPos);   // objSize brackets the parse — never desync
		return bOk;
	}
};

// ---------------------------------------------------------------------------
// Vertex-animated mesh (CRenderObject::eVAMesh).
//
// The geometry is a plain rigid mesh attached to one node; what makes it "VA"
// is that the D3D renderer overwrites the vertex POSITIONS every frame from the
// model's vertex-animation data (CD3DVAMesh::UpdateVA lerps between stored
// frames). We load and draw the mesh as authored, which is the animation's
// first frame — so a VA effect appears, correctly placed and textured, but
// STATIC. That is what NOLF2 uses for waterfall sheets, flags and similar.
//
// Until this existed the whole piece was dropped with "unsupported LOD type",
// which is why the C01S01 waterfall showed only its ParticleSystem spray
// (JA_Waterfall_2) and never the falling water itself (JA_Waterfall, two
// LTBModel keys on FX\WATERFALL1.LTB / WATERFALL2.LTB).
//
// Layout authority: CD3DVAMesh::Load (d3dmeshrendobj_vertanim.cpp:82).
// ---------------------------------------------------------------------------
class RVAMesh : public RModelMesh
{
public:
	uint32 m_nBoneEffector;      // the node this mesh rides on (as per rigid)
	uint32 m_nAnimNodeIdx;       // node holding the vertex-animation data
	uint32 m_nUnDupVertCount;    // verts with their own animated position

	// UV seams duplicate verts; the anim data only stores the unique ones and
	// this map copies each animated position out to its duplicates.
	struct DupMap { uint16 m_nSrcVert, m_nDstVert; };
	std::vector<DupMap> m_aDupMap;

	RVAMesh() : m_nBoneEffector(0), m_nAnimNodeIdx(0), m_nUnDupVertCount(0)
	{ m_Type = eVAMesh; }

	// ★ WITHOUT THIS THE MESH COLLAPSES TO THE ORIGIN.
	// model_load.cpp:944 calls CalcUsedNodes on every LOD at load; the base
	// implementation is an empty stub, so m_pUsedNodeList stays NULL,
	// ModelInstance::SetupLODNodePath marks nothing, the node is never
	// evaluated and its transform stays a ZERO matrix — every vertex maps to
	// (0,0,0). The symptom is a piece that loads with correct geometry
	// (firstVert=(-66.7 1137.9 -301.7)) and still reports
	// "box (0 0 0)-(0 0 0)" after transform.
	// Byte-identical to CD3DVAMesh::CalcUsedNodes: node 0, not the effector.
	virtual void CalcUsedNodes(Model *)
	{
		CreateUsedNodeList(1);
		m_pUsedNodeList[0] = 0;
	}

	// Fill m_aPos from the model's vertex animation — port of CD3DVAMesh::UpdateVA
	// (d3dmeshrendobj_vertanim.cpp:180). THIS IS WHERE THE GEOMETRY COMES FROM:
	// the LTB's own vertex stream carries zeroed positions for a VA mesh (our
	// first cut drew an empty box (0 0 0)-(0 0 0) because of exactly that).
	void UpdateVA(Model *pModel, const AnimTimeRef &cTimeRef)
	{
		// LT_TRACE_VA=1: name the exact step that fails. "no geometry" with no
		// reason is a dead end.
		static bool s_bTrace = (getenv("LT_TRACE_VA") != 0);
		static bool s_bLogged = false;
		bool bLog = s_bTrace && !s_bLogged;

		// ★ IS THE ANIMATION ADVANCING? A one-shot log cannot answer that, and
		// "we always draw one frozen frame" looks exactly like a still sheet.
		// Report the time reference whenever the frame index or the interpolation
		// actually CHANGES, plus a periodic heartbeat so a frozen tracker is
		// visible as silence rather than as absence of instrumentation.
		if (s_bTrace)
		{
			static uint32 s_nLastFrame = 0xFFFFFFFF, s_nLastAnim = 0xFFFFFFFF;
			static int    s_nCalls = 0, s_nChanges = 0;
			++s_nCalls;
			bool bChanged = (cTimeRef.m_Cur.m_iFrame != s_nLastFrame ||
			                 cTimeRef.m_Cur.m_iAnim  != s_nLastAnim);
			if (bChanged)
				++s_nChanges;
			if (bChanged || (s_nCalls % 120) == 0)
			{
				fprintf(stderr, "[va] call %d: anim %u->%u frame %u->%u pct=%.3f  (%d changes so far)%s\n",
				        s_nCalls, cTimeRef.m_Prev.m_iAnim, cTimeRef.m_Cur.m_iAnim,
				        cTimeRef.m_Prev.m_iFrame, cTimeRef.m_Cur.m_iFrame,
				        cTimeRef.m_Percent, s_nChanges,
				        bChanged ? "" : "  <-- HEARTBEAT, tracker not moving");
			}
			s_nLastFrame = cTimeRef.m_Cur.m_iFrame;
			s_nLastAnim  = cTimeRef.m_Cur.m_iAnim;
		}

		if (!pModel || m_aPos.empty())
		{
			if (bLog) { s_bLogged = true; fprintf(stderr, "[va] no model/pos\n"); }
			return;
		}

		ModelAnim *pPrevAnim = pModel->GetAnim(cTimeRef.m_Prev.m_iAnim);
		ModelAnim *pCurAnim  = pModel->GetAnim(cTimeRef.m_Cur.m_iAnim);
		if (!pPrevAnim || !pCurAnim)
		{
			if (bLog) { s_bLogged = true; fprintf(stderr, "[va] no anim (prev=%u cur=%u of %u)\n",
			            cTimeRef.m_Prev.m_iAnim, cTimeRef.m_Cur.m_iAnim, pModel->NumAnims()); }
			return;
		}

		AnimNode *pPrevNode = pPrevAnim->GetAnimNode(m_nAnimNodeIdx);
		AnimNode *pCurNode  = pCurAnim->GetAnimNode(m_nAnimNodeIdx);
		if (!pPrevNode || !pCurNode)
		{
			if (bLog) { s_bLogged = true; fprintf(stderr, "[va] no anim node %u\n", m_nAnimNodeIdx); }
			return;
		}

		CDefVertexLst *pPrev = pPrevNode->GetVertexData(cTimeRef.m_Prev.m_iFrame);
		CDefVertexLst *pCur  = pCurNode->GetVertexData(cTimeRef.m_Cur.m_iFrame);
		if (!pPrev)
		{
			if (bLog) { s_bLogged = true; fprintf(stderr, "[va] no vertex data for frame %u\n",
			            cTimeRef.m_Prev.m_iFrame); }
			return;
		}
		if (bLog)
		{
			s_bLogged = true;
			fprintf(stderr, "[va] OK: animNode=%u boneEffector=%u undup=%u/%u dupmap=%u prevVerts=%u"
			        " firstVert=(%.1f %.1f %.1f)\n",
			        m_nAnimNodeIdx, m_nBoneEffector, m_nUnDupVertCount, m_nVertCount,
			        (unsigned)m_aDupMap.size(), pPrev->size(),
			        pPrev->getValue(0)[0], pPrev->getValue(0)[1], pPrev->getValue(0)[2]);
		}
		if (!pCur)
			pCur = pPrev;          // D3D does the same when the target frame is absent

		const float fPercent = cTimeRef.m_Percent;
		uint32 nCount = m_nUnDupVertCount;
		if (nCount > m_nVertCount) nCount = m_nVertCount;
		if (nCount > pPrev->size()) nCount = pPrev->size();
		if (nCount > pCur->size())  nCount = pCur->size();

		for (uint32 i = 0; i < nCount; ++i)
		{
			const float *pA = pPrev->getValue(i);
			const float *pB = pCur->getValue(i);
			float *pOut = &m_aPos[(size_t)i * 3];
			pOut[0] = pA[0] + ((pB[0] - pA[0]) * fPercent);
			pOut[1] = pA[1] + ((pB[1] - pA[1]) * fPercent);
			pOut[2] = pA[2] + ((pB[2] - pA[2]) * fPercent);
		}

		// Propagate to the UV-seam duplicates.
		for (size_t i = 0; i < m_aDupMap.size(); ++i)
		{
			uint32 nDst = m_aDupMap[i].m_nDstVert, nSrc = m_aDupMap[i].m_nSrcVert;
			if (nDst >= m_nVertCount || nSrc >= m_nVertCount)
				continue;
			memcpy(&m_aPos[(size_t)nDst * 3], &m_aPos[(size_t)nSrc * 3], 3 * sizeof(float));
		}
	}

	virtual bool Load(ILTStream &File, LTB_Header &Header)
	{
		uint32 nObjSize = 0;
		File.Read(&nObjSize, sizeof(nObjSize));
		uint32 nEndPos = 0;
		File.GetPos(&nEndPos);
		nEndPos += nObjSize;

		bool bOk = (Header.m_iFileType == LTB_D3D_MODEL_FILE &&
		            Header.m_iVersion == CD3D_LTB_LOAD_VERSION);
		if (bOk)
		{
			// ⚠️ Field order differs from the rigid mesh: an UnDupVertCount sits
			// between the vertex and poly counts, and two node indices follow
			// the stream flags. Getting this wrong desyncs the whole stream.
			uint32 nUnDupVertCount, nMaxBonesPerTri, nMaxBonesPerVert, aStreamFlags[4];
			File.Read(&m_nVertCount, sizeof(m_nVertCount));
			File.Read(&nUnDupVertCount, sizeof(nUnDupVertCount));
			File.Read(&m_nPolyCount, sizeof(m_nPolyCount));
			File.Read(&nMaxBonesPerTri, sizeof(nMaxBonesPerTri));
			File.Read(&nMaxBonesPerVert, sizeof(nMaxBonesPerVert));
			File.Read(&aStreamFlags[0], sizeof(aStreamFlags));
			File.Read(&m_nAnimNodeIdx, sizeof(m_nAnimNodeIdx));
			File.Read(&m_nBoneEffector, sizeof(m_nBoneEffector));

			m_nUnDupVertCount = nUnDupVertCount;

			bOk = ReadStreams(File, aStreamFlags, kRMBlend_None);

			// The DupMap list follows: uint32 count + count * {uint16 src, uint16 dst}.
			if (bOk)
			{
				uint32 nDupCount = 0;
				File.Read(&nDupCount, sizeof(nDupCount));
				if (nDupCount < 1000000)
				{
					m_aDupMap.resize(nDupCount);
					if (nDupCount)
						File.Read(&m_aDupMap[0], (uint32)(nDupCount * sizeof(DupMap)));
				}
			}
		}

		File.SeekTo(nEndPos);   // objSize brackets the parse — never desync
		return bOk;
	}
};

class RSkelMesh : public RModelMesh
{
public:
	// Render-direct (non-indexed) data: contiguous vertex ranges that share a
	// bone set of up to 4 node indices.
	struct BoneSet          // matches BoneSetListItem's file layout (12 bytes)
	{
		uint16 m_nFirstVert, m_nVertCount;
		uint8  m_aBones[4];
		uint32 m_nIndexIntoIndexBuff;
	};
	std::vector<BoneSet> m_aBoneSets;

	std::vector<uint32> m_aReIndexedBones;   // matrix-palette bone remap (optional)
	uint32 m_nBonesPerVert;                  // total bones blended per vert (1..4)
	bool   m_bMatrixPalette;

	RSkelMesh() : m_nBonesPerVert(1), m_bMatrixPalette(false) { m_Type = eSkelMesh; }

	virtual bool Load(ILTStream &File, LTB_Header &Header)
	{
		uint32 nObjSize = 0;
		File.Read(&nObjSize, sizeof(nObjSize));
		uint32 nEndPos = 0;
		File.GetPos(&nEndPos);
		nEndPos += nObjSize;

		bool bOk = (Header.m_iFileType == LTB_D3D_MODEL_FILE &&
		            Header.m_iVersion == CD3D_LTB_LOAD_VERSION);
		if (bOk)
		{
			uint32 nMaxBonesPerTri, nMaxBonesPerVert, aStreamFlags[4];
			bool bReIndexedBones = false, bUseMatrixPalettes = false;
			File.Read(&m_nVertCount, sizeof(m_nVertCount));
			File.Read(&m_nPolyCount, sizeof(m_nPolyCount));
			File.Read(&nMaxBonesPerTri, sizeof(nMaxBonesPerTri));
			File.Read(&nMaxBonesPerVert, sizeof(nMaxBonesPerVert));
			File.Read(&bReIndexedBones, sizeof(bReIndexedBones));
			File.Read(&aStreamFlags[0], sizeof(aStreamFlags));
			File.Read(&bUseMatrixPalettes, sizeof(bUseMatrixPalettes));
			m_bMatrixPalette = bUseMatrixPalettes;

			if (m_bMatrixPalette)
			{
				// Matrix-palette export: per-vertex uint8 bone indices.
				uint32 nMinBone, nMaxBone;
				File.Read(&nMinBone, sizeof(nMinBone));
				File.Read(&nMaxBone, sizeof(nMaxBone));

				int nBlendType;
				switch (nMaxBonesPerVert)
				{
					case 2:  nBlendType = kRMBlend_Indexed_B1; break;
					case 3:  nBlendType = kRMBlend_Indexed_B2; break;
					case 4:  nBlendType = kRMBlend_Indexed_B3; break;
					default: nBlendType = -1; break;
				}
				m_nBonesPerVert = nMaxBonesPerVert;

				if (bReIndexedBones)
				{
					uint32 nBoneCount = 0;
					File.Read(&nBoneCount, sizeof(nBoneCount));
					bOk = (nBoneCount < 10000);
					if (bOk)
					{
						m_aReIndexedBones.resize(nBoneCount);
						if (nBoneCount)
							File.Read(&m_aReIndexedBones[0], nBoneCount * sizeof(uint32));
					}
				}

				if (bOk && nBlendType >= 0)
					bOk = ReadStreams(File, aStreamFlags, nBlendType);
				else
					bOk = false;
			}
			else
			{
				// Render-direct export: bone-set list after the geometry.
				int nBlendType;
				switch (nMaxBonesPerTri)
				{
					case 1:  nBlendType = kRMBlend_None; break;
					case 2:  nBlendType = kRMBlend_NonIndexed_B1; break;
					case 3:  nBlendType = kRMBlend_NonIndexed_B2; break;
					case 4:  nBlendType = kRMBlend_NonIndexed_B3; break;
					default: nBlendType = -1; break;
				}
				m_nBonesPerVert = nMaxBonesPerTri;

				if (nBlendType >= 0)
					bOk = ReadStreams(File, aStreamFlags, nBlendType);
				else
					bOk = false;

				if (bOk)
				{
					uint32 nBoneSetCount = 0;
					File.Read(&nBoneSetCount, sizeof(nBoneSetCount));
					bOk = (nBoneSetCount < 100000);
					if (bOk)
					{
						m_aBoneSets.resize(nBoneSetCount);
						if (nBoneSetCount)
							File.Read(&m_aBoneSets[0], nBoneSetCount * (uint32)sizeof(BoneSet));
					}
				}
			}
		}

		File.SeekTo(nEndPos);
		return bOk;
	}
};

// ---------------------------------------------------------------------------
// Factory (RenderStruct seam)
// ---------------------------------------------------------------------------

CRenderObject *RModel_CreateRenderObject(CRenderObject::RENDER_OBJECT_TYPES eType)
{
	switch (eType)
	{
		case CRenderObject::eRigidMesh: return new RRigidMesh;
		case CRenderObject::eSkelMesh:  return new RSkelMesh;
		case CRenderObject::eVAMesh:    return new RVAMesh;
		default:
			// Null/debug meshes: the base drawable's Load skips its
			// (size-prefixed) data — safe placeholder.
			return new CDIModelDrawable;
	}
}

bool RModel_DestroyRenderObject(CRenderObject *pObject)
{
	delete pObject;
	return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Scratch buffers for CPU skinning (world-space results).
static std::vector<LTVector> g_aSkinnedPos, g_aSkinnedNormal;

static void rm_SkinVertex(const RModelMesh *pMesh, uint32 nVert,
                           const DDMatrix *const *ppBones, const float *pWeights,
                           uint32 nBoneCount)
{
	const float *pPos    = &pMesh->m_aPos[(size_t)nVert * 3];
	const float *pNormal = &pMesh->m_aNormal[(size_t)nVert * 3];

	LTVector vPos(0, 0, 0), vNormal(0, 0, 0), vTmp;
	for (uint32 nBone = 0; nBone < nBoneCount; ++nBone)
	{
		float fWeight = pWeights[nBone];
		if (fWeight == 0.0f)
			continue;
		rm_TransformPoint(*ppBones[nBone], pPos, vTmp);
		vPos += vTmp * fWeight;
		rm_RotateVector(*ppBones[nBone], pNormal, vTmp);
		vNormal += vTmp * fWeight;
	}
	g_aSkinnedPos[nVert]    = vPos;
	g_aSkinnedNormal[nVert] = vNormal;
}

// Transform the whole mesh into g_aSkinnedPos/Normal (world space).
static bool rm_SkinMesh(const RModelMesh *pMesh, const DDMatrix *pTransforms,
                         uint32 nNumNodes)
{
	g_aSkinnedPos.resize(pMesh->m_nVertCount);
	g_aSkinnedNormal.resize(pMesh->m_nVertCount);

	const DDMatrix *apBones[4];
	float aWeights[4];

	// A VA mesh rides on a single node exactly like a rigid one — the only
	// difference is the per-frame vertex rewrite we do not implement.
	if (((RModelMesh*)pMesh)->GetType() == CRenderObject::eRigidMesh ||
	    ((RModelMesh*)pMesh)->GetType() == CRenderObject::eVAMesh)
	{
		uint32 nBone = (((RModelMesh*)pMesh)->GetType() == CRenderObject::eVAMesh)
		             ? ((const RVAMesh*)pMesh)->m_nBoneEffector
		             : ((const RRigidMesh*)pMesh)->m_nBoneEffector;
		if (nBone >= nNumNodes)
			nBone = 0;
		apBones[0] = &pTransforms[nBone];
		aWeights[0] = 1.0f;
		for (uint32 nVert = 0; nVert < pMesh->m_nVertCount; ++nVert)
			rm_SkinVertex(pMesh, nVert, apBones, aWeights, 1);
		return true;
	}

	const RSkelMesh *pSkel = (const RSkelMesh*)pMesh;
	uint32 nBoneCount = pSkel->m_nBonesPerVert;
	if (nBoneCount < 1 || nBoneCount > 4)
		return false;

	if (pSkel->m_bMatrixPalette)
	{
		if (pSkel->m_aBoneIdx.empty())
			return false;
		for (uint32 nVert = 0; nVert < pMesh->m_nVertCount; ++nVert)
		{
			const uint8 *pIdx = &pSkel->m_aBoneIdx[(size_t)nVert * 4];
			const float *pBlend = pSkel->m_aBlend.empty()
			                      ? 0 : &pSkel->m_aBlend[(size_t)nVert * pSkel->m_nBlendPerVert];
			float fTotal = 0.0f;
			for (uint32 nBone = 0; nBone < nBoneCount; ++nBone)
			{
				uint32 nNode = pIdx[nBone];
				if (!pSkel->m_aReIndexedBones.empty() && nNode < pSkel->m_aReIndexedBones.size())
					nNode = pSkel->m_aReIndexedBones[nNode];
				if (nNode >= nNumNodes)
					nNode = 0;
				apBones[nBone] = &pTransforms[nNode];
				if (nBone + 1 < nBoneCount)
				{
					aWeights[nBone] = pBlend ? pBlend[nBone] : 0.0f;
					fTotal += aWeights[nBone];
				}
			}
			aWeights[nBoneCount - 1] = 1.0f - fTotal;
			rm_SkinVertex(pMesh, nVert, apBones, aWeights, nBoneCount);
		}
		return true;
	}

	// Render-direct: vertex ranges per bone set.
	for (size_t nSet = 0; nSet < pSkel->m_aBoneSets.size(); ++nSet)
	{
		const RSkelMesh::BoneSet &cSet = pSkel->m_aBoneSets[nSet];
		for (uint32 nBone = 0; nBone < nBoneCount; ++nBone)
		{
			uint32 nNode = cSet.m_aBones[nBone];
			if (nNode >= nNumNodes)
				nNode = 0;
			apBones[nBone] = &pTransforms[nNode];
		}

		uint32 nLast = LTMIN((uint32)cSet.m_nFirstVert + cSet.m_nVertCount, pMesh->m_nVertCount);
		for (uint32 nVert = cSet.m_nFirstVert; nVert < nLast; ++nVert)
		{
			const float *pBlend = pSkel->m_aBlend.empty()
			                      ? 0 : &pSkel->m_aBlend[(size_t)nVert * pSkel->m_nBlendPerVert];
			float fTotal = 0.0f;
			for (uint32 nBone = 0; nBone + 1 < nBoneCount; ++nBone)
			{
				aWeights[nBone] = pBlend ? pBlend[nBone] : 0.0f;
				fTotal += aWeights[nBone];
			}
			aWeights[nBoneCount - 1] = 1.0f - fTotal;
			rm_SkinVertex(pMesh, nVert, apBones, aWeights, nBoneCount);
		}
	}
	return true;
}

// bLit == false -> emit the mesh FULLBRIGHT (no shading term at all).
//
// ★ The key light below is OUR INVENTION, a stand-in for the model lighting we
// have not ported (§25). D3D has no such term, and two of its own gates say
// when there must be no environment lighting whatsoever (setupmodel.cpp):
//   :314  g_CV_LightModels == 0 || FLAG_NOLIGHT  -> AddAmbient(255,255,255)
//   :374/:413  ALL ambient and world-light accumulation is `if (g_have_world)`
// So a model drawn with no world loaded -- every interface model on the main
// menu and the loading screens -- gets a flat, unshaded colour on D3D.
// Shading it anyway is what produced the "dark band" over the menu: the menu
// backdrop is MAINMIDDLE/MAINFRONT.LTB, big flat art quads whose normals gave
// N.L = 0.46 -> shade 192, so the art drew at 192/255 = 75% while the region
// beyond those quads showed the background SPRITE (drawn at full white) --
// measured exactly: orange (255,190,19) * 192/255 = (192,143,14).
// ★★★ THE AUTHORED AMBIENT FOR THIS INSTANCE (§68), or (255,255,255) when the
// model is unlit. Set once per instance by rm_SetupInstanceLight().
static float g_fModelAmbient[3] = { 255.0f, 255.0f, 255.0f };

// Look the instance's position up in the world's precomputed LIGHT GRID —
// exactly what D3D does for a model's ambient term (setupmodel.cpp:374,
// `w_DoLightLookup(...LightTable(), &vInstancePosition, &ambientColor)` ->
// LightList.AddAmbient). ⚠️ Gated on g_have_world in D3D; our equivalent gate is
// the caller's bLit, which already covers the interface pass and FLAG_NOLIGHT.
//
// ★★★ ON BY DEFAULT since 2026-08-05, at the user's decision (§71). All FOUR of
// D3D's terms are ported, for world AND player-view models:
//   1 the world light grid      3 the dynamic lights (per vertex — §71)
//   2 the sun (§70)             4 the touching static lights (§68)
// light-group tinted, attenuated, capped at 4 with the overflow folded into
// ambient rather than dropped. Measured: over C01S01 a model beside a lantern
// resolves to (301 284 252) warm from 4 lights while one in the dark stays at
// the grid floor of 24; C12S01 spans 17..251; C07S01 daylight reads
// (196 238 202) sunlit vs (118 128 111) in shadow.
//
// ⚠️ `LT_NO_MODEL_LIGHTING=1` restores the OLD stand-in key light —
// `140 + 115*|N.L|` against an invented direction, which lit every model to at
// least 55% wherever it stood (§25's "one bright green tree"). It is a
// bisection tool for "did model lighting cause this", not a fallback anyone
// should ship with. `LT_MODEL_LIGHTING=1` is kept as a no-op so the many
// commands in the handoff still work.
static bool rm_UseAmbient()
{
	// ⚠️ RESPECT THE VALUE, NOT JUST THE PRESENCE. This used to be
	// `getenv(...) ? 0 : 1`, so `LT_NO_MODEL_LIGHTING=0` ALSO disabled model
	// lighting — a user bisecting a bug ran =1 against =0, got the same result
	// both times, and had in fact tested "off" against "off". A switch whose
	// documented off-value does not work silently corrupts the bisection it
	// exists to serve.
	static int s_n = -1;
	if (s_n < 0)
	{
		const char *pEnv = getenv("LT_NO_MODEL_LIGHTING");
		const bool bDisable = pEnv && pEnv[0] && pEnv[0] != '0';
		s_n = bDisable ? 0 : 1;
	}
	return s_n != 0;
}

// ── The static-light term (D3D's term 4) ─────────────────────────────────────
//
// ★ THE KEY SIMPLIFICATION, AND IT IS D3D'S OWN: for the two attenuation models
// this game actually authors (Quartic — the StaticLight default — and Linear),
// `CRenderLight::CreatePointLight*` does NOT build an attenuated point light.
// It calls CreateSampledDirLight (renderlight.cpp:196-204): SAMPLE the light's
// colour once at the object position, then treat it as a DIRECTIONAL light
// pointing from the light to the object. So per vertex there is no attenuation
// maths at all — just colour x N.L against a fixed direction.
//
// Attenuation, from CRenderLight::GetLightSample (renderlight.cpp:265):
//     bail if d^2 >= r^2
//     Quartic : f = (1 - d^2/r^2)^2
//     Linear  : f = 1 - d/r
//     D3D     : f = 1/(a0 + a1*d + a2*d^2)
//     spot    : also x (cos - cosFOV)/(1 - cosFOV), zero outside the cone
//
// ★ TERM 3 IS THE EXCEPTION (§71). DYNAMIC lights — muzzle flashes, the
// flashlight, projectiles and explosions — are authored with
// `eAttenuation_D3D`, and that is the ONE branch of CreatePointLight which does
// NOT collapse to a sampled dir light: it installs a real hardware point light
// (renderlight.cpp:183) with Range = radius and coefficients
// `(1, 0, 19/r^2)` straight from setupmodel.cpp:407. So a dynamic light really
// does attenuate and change direction ACROSS the model, and we evaluate it per
// vertex against g_aSkinnedPos. That matters precisely where these lights live:
// a muzzle flash sits inside the player-view weapon's own bounds, where a
// single sample at the root node would light the whole model uniformly.
struct RModelLight
{
	// dir lights: the colour already sampled and attenuated at the instance
	// position. point lights: the RAW colour — attenuation is per vertex.
	LTVector m_vColor;
	// What the light contributes AT the instance position, already scaled by
	// (1 - convertToAmbient). Only used when the light is pushed out of the list
	// and folded into ambient; for a dir light it equals m_vColor.
	LTVector m_vSample;
	LTVector m_vToLight;  // dir lights: normalized, FROM the object TOWARD the light
	float    m_fScore;

	bool     m_bPoint;    // ★ term 3: shade this one per vertex
	LTVector m_vPos;      // point lights: in the same space as g_aSkinnedPos
	float    m_fRadiusSqr;
	LTVector m_vAttCoef;  // point lights: a0, a1, a2

	RModelLight()
		: m_vColor(0, 0, 0), m_vSample(0, 0, 0), m_vToLight(0, 1, 0)
		, m_fScore(0.0f), m_bPoint(false), m_vPos(0, 0, 0)
		, m_fRadiusSqr(0.0f), m_vAttCoef(1, 0, 0) {}
};

// D3D caps this with `MaxModelLights` (default 4, rendererconsolevars.h:185).
enum { kMaxModelLights = 4 };
static RModelLight g_aModelLights[kMaxModelLights];
static int          g_nModelLights = 0;

// ⚠️ SET FOR THE DURATION OF ONE rm_SetupInstanceLight CALL, and the reason the
// world->camera rotation now lives INSIDE rm_InsertLight (§71).
//
// §69 rotated the finished list in one loop after the static-light query. When
// §70 added the sun — which rotates its own direction at insertion, before that
// loop runs — every player-view instance rotated the sun's direction TWICE, so
// Cate's hands and weapon took their sunlight from the wrong hemisphere in the
// 44 of 53 worlds that place a StaticSunLight. Rotating once, at the single
// point where a light enters the list, is the structural fix: it cannot be
// applied twice and cannot be forgotten by a new term.
static bool g_bModelLightCamSpace = false;

namespace
{
	// world -> the ENGINE's LEFT-HANDED camera space (§69: +Z forward, and NO Z
	// flip belongs here — that happens later, in the pass's kFlipZ modelview).
	inline LTVector rm_DirToCam(const LTVector &v)
	{
		return LTVector(v.Dot(g_vSprCamRight), v.Dot(g_vSprCamUp), v.Dot(g_vSprCamFwd));
	}
	inline LTVector rm_PosToCam(const LTVector &v)
	{
		return rm_DirToCam(v - g_vSprCamPos);
	}

	// Mirrors CRenderLight::GetLightSample for a StaticLight.
	bool rm_SampleLight(const StaticLight *pLight, const LTVector &vPos,
	                     LTVector &vOutColor, LTVector &vOutToLight)
	{
		// ⚠️ The light-group tint comes FIRST, and its early-out is what makes a
		// switched-off group cost nothing (StaticLightCB, setupmodel.cpp:127).
		LTVector vColor = pLight->m_Color;
		if (pLight->m_pLightGroupColor)
		{
			vColor.x *= pLight->m_pLightGroupColor->x;
			vColor.y *= pLight->m_pLightGroupColor->y;
			vColor.z *= pLight->m_pLightGroupColor->z;
		}
		if (vColor.x < 1.0f && vColor.y < 1.0f && vColor.z < 1.0f)
			return false;

		const LTVector vToSample = vPos - pLight->m_Pos;   // light -> object
		const float fDistSqr = vToSample.MagSqr();
		const float fRadiusSqr = pLight->m_Radius * pLight->m_Radius;
		if (fRadiusSqr <= 0.0f || fDistSqr >= fRadiusSqr)
			return false;

		const float fDist = sqrtf(fDistSqr);
		float fAtten = 0.0f;
		switch (pLight->m_eAttenuation)
		{
			case eAttenuation_Quartic:
				fAtten = 1.0f - (fDistSqr / fRadiusSqr);
				fAtten *= fAtten;
				break;
			case eAttenuation_Linear:
				fAtten = 1.0f - (fDist / pLight->m_Radius);
				break;
			default:   // eAttenuation_D3D
			{
				const float fD = pLight->m_AttCoefs.x + pLight->m_AttCoefs.y * fDist
				               + pLight->m_AttCoefs.z * fDistSqr;
				fAtten = (fD > 0.0001f) ? (1.0f / fD) : 0.0f;
				break;
			}
		}

		// Spot cone (m_FOV is cos(fov/2); -1 means omnidirectional).
		if (pLight->m_FOV > -0.99f)
		{
			if (fDist < 0.0001f)
				return false;
			const float fDot = vToSample.Dot(pLight->m_Dir) / fDist;
			if (fDot <= pLight->m_FOV)
				return false;
			fAtten *= (fDot - pLight->m_FOV) / (1.0f - pLight->m_FOV);
		}

		if (fAtten <= 0.0f)
			return false;

		vOutColor = vColor * fAtten;
		vOutToLight = (fDist > 0.0001f) ? (-vToSample / fDist) : LTVector(0.0f, 1.0f, 0.0f);
		return true;
	}

	// Mirrors CRelevantLightList::InsertLight: keep the strongest kMaxModelLights,
	// and fold everything that does not fit into the ambient rather than dropping
	// it — that is what keeps a crowd of weak lights from vanishing.
	//
	// ⚠️ `vSample` is what the light delivers AT THE INSTANCE POSITION, and it is
	// what D3D ranks and folds into ambient with (relevantlightlist.cpp:24/47/62)
	// — NOT the colour used to shade. For every dir light the two are the same
	// value; for a term-3 point light they differ, because its shading colour is
	// unattenuated and the attenuation happens per vertex.
	void rm_InsertLight(RModelLight cLight, const LTVector &vSample,
	                     float fConvertToAmbient, float *pAmbient)
	{
		const float fKeep   = 1.0f - fConvertToAmbient;
		const float fSample = LTMAX(vSample.x, LTMAX(vSample.y, vSample.z));
		const float fScore  = fSample * fKeep;

		int nAt = 0;
		while (nAt < g_nModelLights && fScore <= g_aModelLights[nAt].m_fScore)
			++nAt;

		if (nAt >= kMaxModelLights)
		{
			pAmbient[0] += vSample.x; pAmbient[1] += vSample.y; pAmbient[2] += vSample.z;
			return;
		}

		if (g_nModelLights >= kMaxModelLights)
		{
			// The one being pushed out becomes ambient.
			const RModelLight &cOut = g_aModelLights[kMaxModelLights - 1];
			pAmbient[0] += cOut.m_vSample.x;
			pAmbient[1] += cOut.m_vSample.y;
			pAmbient[2] += cOut.m_vSample.z;
		}
		else
			++g_nModelLights;

		for (int i = g_nModelLights - 1; i > nAt; --i)
			g_aModelLights[i] = g_aModelLights[i - 1];

		// ★ THE ONE PLACE world -> camera happens, for every term (see
		// g_bModelLightCamSpace). A point light needs its POSITION carried over,
		// not just a direction.
		if (g_bModelLightCamSpace)
		{
			cLight.m_vToLight = rm_DirToCam(cLight.m_vToLight);
			if (cLight.m_bPoint)
				cLight.m_vPos = rm_PosToCam(cLight.m_vPos);
		}

		cLight.m_vColor  *= fKeep;
		cLight.m_vSample  = vSample * fKeep;
		cLight.m_fScore   = fScore;
		g_aModelLights[nAt] = cLight;

		pAmbient[0] += fConvertToAmbient * vSample.x;
		pAmbient[1] += fConvertToAmbient * vSample.y;
		pAmbient[2] += fConvertToAmbient * vSample.z;
	}

	// ── TERM 3: THE DYNAMIC LIGHTS (setupmodel.cpp:392) ──────────────────────
	//
	// D3D walks `g_ObjectDynamicLights`, the per-frame list `d3d_ProcessLight`
	// fills from the visible OT_LIGHT objects (drawlight.cpp:28). We do not build
	// that list, so we walk the client's OT_LIGHT object list directly — the same
	// source, and the same one the GL world's additive dynamic-light pass already
	// uses (rw_CollectDynamicLights). ⚠️ Divergence worth knowing: D3D's list is
	// the VISIBLE set, ours is every live light, so a light just off screen can
	// still reach a model here. That is the safer direction (a muzzle flash
	// behind the camera should still light the weapon in front of it) but it is
	// not byte parity.
	//
	// ⚠️ FLAG_ONLYLIGHTWORLD is the authored "this one is scenery lighting, keep
	// it off the characters" opt-out and is checked first, exactly as D3D does.
	void rm_AddDynamicLights(const LTVector &vPos, float *pAmbient, bool bTrace)
	{
		if (!g_CV_DynamicLight.m_Val)
			return;

		// One prepared light, filled in per source below.
		RModelLight cLight;
		cLight.m_bPoint = true;

		struct Src { LTVector m_vPos; float m_fRadius; LTVector m_vColor; };
		std::vector<Src> aSrc;

		if (g_pClientMgr)
		{
			LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_LIGHT].m_Head;
			for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
			{
				DynamicLight *pLight = (DynamicLight*)pCur->m_pData;
				if (!pLight || !(pLight->m_Flags & FLAG_VISIBLE))
					continue;
				if (pLight->m_Flags & FLAG_ONLYLIGHTWORLD)
					continue;
				if (pLight->m_LightRadius <= 1.0f)
					continue;
				Src cSrc;
				cSrc.m_vPos    = pLight->GetPos();
				cSrc.m_fRadius = pLight->m_LightRadius;
				cSrc.m_vColor.Init((float)pLight->m_ColorR, (float)pLight->m_ColorG,
				                   (float)pLight->m_ColorB);
				aSrc.push_back(cSrc);
			}
		}

		// The headless injection set — the null shell never creates a real
		// dynamic light, so this is how the term is exercised without playing.
		const RWTestDynLight *pTest = 0;
		const uint32 nTest = RWorld_GetTestDynLights(&pTest);
		for (uint32 n = 0; n < nTest; ++n)
		{
			if (pTest[n].m_fRadius <= 1.0f)
				continue;
			Src cSrc;
			cSrc.m_vPos    = pTest[n].m_vPos;
			cSrc.m_fRadius = pTest[n].m_fRadius;
			cSrc.m_vColor  = pTest[n].m_vColor255;
			aSrc.push_back(cSrc);
		}

		// ⚠️ THE COUNT WORTH TRACING IS PER FRAME, NOT PER INSTANCE. The first
		// version of this reported how many lights each MODEL took, on change —
		// and since some models are inside a light's radius and their neighbours
		// are not, it alternated 1/0/1/0 and produced 30,344 lines in 55 s. The
		// CANDIDATE count is a property of the frame: identical for every
		// instance, changing only when a light is actually created or destroyed.
		if (bTrace)
		{
			static size_t s_nLastCandidates = (size_t)-1;
			if (aSrc.size() != s_nLastCandidates)
			{
				s_nLastCandidates = aSrc.size();
				fprintf(stderr, "[mlight] dynamic light candidates: %u\n", (uint32)aSrc.size());
				for (size_t n = 0; n < aSrc.size(); ++n)
					fprintf(stderr, "[mlight]   dyn[%u] rgb=(%3.0f %3.0f %3.0f) r=%.0f @(%.0f %.0f %.0f)\n",
					        (uint32)n, aSrc[n].m_vColor.x, aSrc[n].m_vColor.y, aSrc[n].m_vColor.z,
					        aSrc[n].m_fRadius, aSrc[n].m_vPos.x, aSrc[n].m_vPos.y, aSrc[n].m_vPos.z);
			}
		}

		for (size_t n = 0; n < aSrc.size(); ++n)
		{
			const float fRadiusSqr = aSrc[n].m_fRadius * aSrc[n].m_fRadius;

			// The ranking sample: GetLightSample at the instance position, with
			// the D3D attenuation model (renderlight.cpp:292).
			const LTVector vToSample = vPos - aSrc[n].m_vPos;   // light -> object
			const float fDistSqr = vToSample.MagSqr();
			if (fDistSqr >= fRadiusSqr)
				continue;

			const LTVector vAttCoef(1.0f, 0.0f, 19.0f / fRadiusSqr);
			const float fDist = sqrtf(fDistSqr);
			const float fDen  = vAttCoef.x + vAttCoef.y * fDist + vAttCoef.z * fDistSqr;
			if (fDen <= 0.0001f)
				continue;

			cLight.m_vColor     = aSrc[n].m_vColor;
			cLight.m_vPos       = aSrc[n].m_vPos;
			cLight.m_fRadiusSqr = fRadiusSqr;
			cLight.m_vAttCoef   = vAttCoef;
			// Only a fallback for a vertex sitting exactly on the light.
			cLight.m_vToLight   = (fDist > 0.0001f) ? (-vToSample / fDist)
			                                        : LTVector(0.0f, 1.0f, 0.0f);

			// ⚠️ convertToAmbient is 0 for dynamic lights (setupmodel.cpp:409).
			rm_InsertLight(cLight, aSrc[n].m_vColor * (1.0f / fDen), 0.0f, pAmbient);
		}
	}

	// ★ TERM 2, THE SUN. 44 of NOLF2's 53 retail worlds place a `StaticSunLight`,
	// and the server sends its colour over SMSG_GLOBALLIGHT
	// (sm_TellClientAboutGlobalLight, s_client.cpp:1574) as
	// `InnerColor * BrightScale * ObjectBrightScale`. C07S01 delivers
	// (178 168 115) with 40% converted to ambient — a large term, and the reason
	// models in daylight looked flat without it.
	//
	// Visibility is a ray toward the sun that must land on a SURF_SKY polygon
	// (CastRayAtSky, setupmodel.cpp:171).
	bool rm_CastRayAtSky(const LTVector &vFrom, const LTVector &vDir)
	{
		if (!g_pModelWorldClient || !g_pModelWorldClient->ClientTree())
			return false;

		IntersectQuery cQuery;
		IntersectInfo  cInfo;
		cQuery.m_From  = vFrom;
		cQuery.m_To    = vFrom + vDir;
		cQuery.m_Flags = INTERSECT_HPOLY;

		if (!i_IntersectSegment(&cQuery, &cInfo, g_pModelWorldClient->ClientTree()))
			return false;   // hit nothing at all -> not sky

		WorldPoly *pPoly = g_pModelWorldClient->GetPolyFromHPoly(cInfo.m_hPoly);
		return pPoly && (pPoly->GetSurface()->GetFlags() & SURF_SKY) != 0;
	}

	// World-tree callback: one touching static light.
	LTVector g_vLightQueryPos;
	float   *g_pLightQueryAmbient = 0;

	void rm_StaticLightCB(WorldTreeObj *pObj, void * /*pUser*/)
	{
		if (!pObj || pObj->GetObjType() != WTObj_Light)
			return;
		const StaticLight *pLight = (const StaticLight*)pObj;
		LTVector vColor, vToLight;
		if (rm_SampleLight(pLight, g_vLightQueryPos, vColor, vToLight))
		{
			RModelLight cLight;
			cLight.m_vColor   = vColor;
			cLight.m_vToLight = vToLight;
			rm_InsertLight(cLight, vColor, pLight->m_fConvertToAmbient, g_pLightQueryAmbient);
		}
	}
}

// ⚠️⚠️ FLAG_REALLYCLOSE MODELS LIVE IN CAMERA SPACE (§53b, §69).
//
// Cate's hands and weapon are player-view models: `LTObject::InsertSpecial`
// keeps them out of the world tree and their m_Pos is ALREADY camera-relative.
// Looking their lighting up at that raw position samples a meaningless point
// near the world origin — which is why the weapon's shading never changed no
// matter where the player stood.
//
// D3D does the round trip explicitly (setupmodel.cpp):
//   :330  mInvView.Apply(vInstancePosition)      camera -> WORLD, to sample
//   :143  vPos = mView * lightPos, Apply3x3(dir) world  -> CAMERA, to shade
// We do the same, but rotate the resolved TO-LIGHT DIRECTION rather than each
// light position — identical for a rigid transform and much cheaper.
//
// ⚠️ The space here is the ENGINE's LEFT-HANDED camera space (+Z forward), the
// one the player-view pass feeds to GL before its kFlipZ modelview. The skinned
// normals we dot against are in that same space, so no Z flip belongs in this
// maths — adding one would invert every player-view light.
static void rm_SetupInstanceLight(ModelInstance *pInstance, const LTVector &vPosIn,
                                   float fRadius, bool bLit, bool bReallyClose)
{
	g_fModelAmbient[0] = g_fModelAmbient[1] = g_fModelAmbient[2] = 255.0f;
	g_nModelLights = 0;
	g_bModelLightCamSpace = false;
	if (!bLit || !g_pModelWorldBSP)
		return;

	// Every light that enters the list from here on is rotated (and, if it is a
	// point light, translated) into camera space by rm_InsertLight — once.
	g_bModelLightCamSpace = bReallyClose;

	// camera -> world for a player-view instance
	LTVector vPos = vPosIn;
	if (bReallyClose)
	{
		vPos = g_vSprCamPos
		     + g_vSprCamRight * vPosIn.x
		     + g_vSprCamUp    * vPosIn.y
		     + g_vSprCamFwd   * vPosIn.z;
	}

	// ⚠️ A player-view instance can be evaluated before its transform is valid —
	// the trace caught one at ~1e24. Feeding that to FindObjectsInBox2 would walk
	// the whole world tree with an absurd box. Bail to unlit instead; the next
	// frame has a real position.
	const float fSane = 1.0e7f;
	if (!(vPos.x > -fSane && vPos.x < fSane &&
	      vPos.y > -fSane && vPos.y < fSane &&
	      vPos.z > -fSane && vPos.z < fSane))
		return;

	LTRGBColor cAmbient;
	w_DoLightLookup(g_pModelWorldBSP->LightTable(), &vPos, &cAmbient);
	g_fModelAmbient[0] = (float)cAmbient.rgb.r;
	g_fModelAmbient[1] = (float)cAmbient.rgb.g;
	g_fModelAmbient[2] = (float)cAmbient.rgb.b;

	// LT_TRACE_MODELLIGHT=1 — one line per DISTINCT ambient value, with the
	// position it came from. The tell for "is the grid actually being read":
	// a single repeated value means the lookup is inert (or the grid is empty)
	// and every model is still uniformly lit, which is the §25 bug wearing a
	// different mask.
	// ── term 2: the SUN ─────────────────────────────────────────────────────
	// ⚠️ CACHED PER INSTANCE. The visibility ray crosses the whole world, so
	// casting it for every model every frame is not free — D3D caches it in
	// ModelInstance::m_LastDirLightAmount and only recasts when the instance has
	// moved further than `ModelSunVariance` (setupmodel.cpp:340).
	if (g_pRenderStruct && pInstance &&
	    (g_pRenderStruct->m_GlobalLightColor.x != 0.0f ||
	     g_pRenderStruct->m_GlobalLightColor.y != 0.0f ||
	     g_pRenderStruct->m_GlobalLightColor.z != 0.0f))
	{
		float fAmount = pInstance->m_LastDirLightAmount;
		if (fAmount < 0.0f || !vPos.NearlyEquals(pInstance->m_LastDirLightPos, 10.0f))
		{
			// Shoot back along the sun direction, far enough to leave the world.
			const WorldTreeNode *pRoot = (g_pModelWorldClient && g_pModelWorldClient->ClientTree())
			                           ? g_pModelWorldClient->ClientTree()->GetRootNode() : 0;
			const float fFar = pRoot ? (pRoot->GetBBoxMax() - pRoot->GetBBoxMin()).Mag() : 100000.0f;
			fAmount = rm_CastRayAtSky(vPos, g_pRenderStruct->m_GlobalLightDir * -fFar) ? 1.0f : 0.0f;
			pInstance->m_LastDirLightPos    = vPos;
			pInstance->m_LastDirLightAmount = fAmount;
		}

		if (fAmount > 0.0f)
		{
			// ⚠️ The light TRAVELS along m_GlobalLightDir, so the vector toward
			// it is the negation — the same sign trap as the static lights.
			LTVector vToSun = -g_pRenderStruct->m_GlobalLightDir;
			const float fLen = vToSun.Mag();
			if (fLen > 0.0001f)
			{
				// ⚠️ NO rotation here any more — rm_InsertLight does it once,
				// for every term. Rotating it here as well is exactly the
				// double-rotation §71 found (see g_bModelLightCamSpace).
				vToSun /= fLen;
				RModelLight cSun;
				cSun.m_vColor   = g_pRenderStruct->m_GlobalLightColor * fAmount;
				cSun.m_vToLight = vToSun;
				rm_InsertLight(cSun, cSun.m_vColor,
				                g_pRenderStruct->m_GlobalLightConvertToAmbient, g_fModelAmbient);
			}
		}
	}

	// ── term 3: the DYNAMIC lights ──────────────────────────────────────────
	// ⚠️ BEFORE the static lights, because D3D inserts in that order
	// (setupmodel.cpp:392 then :413) and CRelevantLightList's "does this one beat
	// what is already here" test is a STRICT >: on a tie the light inserted first
	// keeps the slot. Insert out of order and a model standing in a tie between a
	// muzzle flash and a lamp gets the other one.
	static int s_nTraceDyn = -1;
	if (s_nTraceDyn < 0) s_nTraceDyn = getenv("LT_TRACE_MODELLIGHT") ? 1 : 0;
	rm_AddDynamicLights(vPos, g_fModelAmbient, s_nTraceDyn != 0);

	// ⚠️ COUNT THE POINT LIGHTS, NOT THE LIST LENGTH. `g_nModelLights` here also
	// holds the SUN, which term 2 inserted just above — and that is not a
	// hypothetical: the first version of this read the list length and reported
	// "dynamic lights ON A MODEL: 1" all through C01S01, which authors a blue
	// moonlight (42 55 92) and not one dynamic light. A diagnostic must count the
	// thing it names.
	int nDynLights = 0;
	for (int nL = 0; nL < g_nModelLights; ++nL)
		if (g_aModelLights[nL].m_bPoint)
			++nDynLights;

	// ── term 4: the touching STATIC lights ──────────────────────────────────
	// D3D queries the client world tree for NOA_Lights in a box of +/- the
	// model's vis radius about the instance position (setupmodel.cpp:422).
	if (g_pModelWorldClient && g_pModelWorldClient->ClientTree() && fRadius > 0.0f)
	{
		g_vLightQueryPos     = vPos;
		g_pLightQueryAmbient = g_fModelAmbient;

		FindObjInfo cInfo;
		cInfo.m_iObjArray = NOA_Lights;
		cInfo.m_Min = vPos - LTVector(fRadius, fRadius, fRadius);
		cInfo.m_Max = vPos + LTVector(fRadius, fRadius, fRadius);
		cInfo.m_CB = rm_StaticLightCB;
		cInfo.m_pCBUser = 0;
		g_pModelWorldClient->ClientTree()->FindObjectsInBox2(&cInfo);

		g_pLightQueryAmbient = 0;
		// ⚠️ The world->camera loop that used to sit here is GONE (§71). It walked
		// the whole list, which by then also held the sun — already rotated at its
		// own insertion — and rotated it a second time. rm_InsertLight now does it
		// once, per light, as it enters.
	}

	static int s_nTrace = -1;
	if (s_nTrace < 0) s_nTrace = getenv("LT_TRACE_MODELLIGHT") ? 1 : 0;
	if (s_nTrace)
	{
		// ⚠️ ON CHANGE, NOT ONCE. The first version of this fired on the first
		// model lit and reported color=(0 0 0) — which was simply EARLIER than
		// the server's SMSG_GLOBALLIGHT packet, and led me to conclude the game
		// authors no sun at all. It authors one in 44 of 53 levels. This is the
		// handoff's own "a diagnostic that fires once lies" rule.
		static LTVector s_vLastSun(-1.0f, -1.0f, -1.0f);
		if (g_pRenderStruct && !g_pRenderStruct->m_GlobalLightColor.NearlyEquals(s_vLastSun, 0.01f))
		{
			s_vLastSun = g_pRenderStruct->m_GlobalLightColor;
			fprintf(stderr, "[mlight] LEVEL SUN: color=(%.0f %.0f %.0f) dir=(%.2f %.2f %.2f) toAmbient=%.2f\n",
			        g_pRenderStruct->m_GlobalLightColor.x, g_pRenderStruct->m_GlobalLightColor.y,
			        g_pRenderStruct->m_GlobalLightColor.z, g_pRenderStruct->m_GlobalLightDir.x,
			        g_pRenderStruct->m_GlobalLightDir.y, g_pRenderStruct->m_GlobalLightDir.z,
			        g_pRenderStruct->m_GlobalLightConvertToAmbient);
		}
	}
	if (s_nTrace)
	{
		// ★ TERM 3's SECOND HALF: a HIGH-WATER MARK, not a per-instance report.
		// The candidate count (rm_AddDynamicLights) says a light EXISTS; this
		// says one actually reached a model and won a slot. Reported only when it
		// rises, so a muzzle flash cannot spam it — and unlike a one-shot it
		// cannot claim "term 3 never fires" from a moment before the first shot.
		//
		// ⚠️ SEPARATE MARKS FOR WORLD AND PLAYER-VIEW. One shared counter would
		// report the first world model to take a dynamic light and then stay
		// silent forever, so the PV path — the one carrying the new camera-space
		// transform, and the §53b family that has bitten three times — would
		// never appear even when it never ran at all. The boring case is the one
		// being hunted (§53c).
		static int s_aMaxDyn[2] = { 0, 0 };
		int &nMax = s_aMaxDyn[bReallyClose ? 1 : 0];
		if (nDynLights > nMax)
		{
			nMax = nDynLights;
			fprintf(stderr, "[mlight]%s dynamic lights ON A MODEL, new high-water mark: %d\n",
			        bReallyClose ? " [PV]" : "     ", nDynLights);
		}
	}
	if (s_nTrace)
	{
		static std::set<uint32> s_seen;
		// ⚠️ Key on the RESOLVED ambient, not the raw grid sample. Keyed on the
		// grid alone, every value was recorded during load — before the server's
		// sun packet arrived — and each later, brighter result collided with an
		// existing key and was suppressed. The trace then "proved" the sun made
		// no difference, which was an artefact of its own key.
		uint32 nKey = ((uint32)(g_fModelAmbient[0]) << 16) |
		              ((uint32)(g_fModelAmbient[1]) << 8)  |
		              ((uint32)(g_fModelAmbient[2]))       |
		              ((uint32)g_nModelLights << 24);
		if (s_seen.insert(nKey).second)
			fprintf(stderr, "[mlight]%s grid=(%3u %3u %3u) +%d lights -> ambient=(%3.0f %3.0f %3.0f) @(%.0f %.0f %.0f)\n",
			        bReallyClose ? " [PV]" : "     ",
			        cAmbient.rgb.r, cAmbient.rgb.g, cAmbient.rgb.b, g_nModelLights,
			        g_fModelAmbient[0], g_fModelAmbient[1], g_fModelAmbient[2],
			        vPos.x, vPos.y, vPos.z);
	}
}

// The authored render state for the mesh about to be emitted. The GL path sets
// it as ambient GL state; Metal needs it as a description, so the state-setting
// code below fills this in lockstep and rm_EmitMesh hands it over. One
// description of the draw, read by both backends (§85's RWDrawParams pattern).
static REmitState g_cEmitState;

static void rm_EmitMesh(const RModelMesh *pMesh, bool bLit, uint8 nAlpha)
{
	// ⚠️ THE OLD KEY LIGHT IS GONE (§68). It was `140 + 115*|N.L|` against an
	// invented direction — so every model rendered at >=55% brightness wherever
	// it stood, which is why a tree in a night scene glowed against a correctly
	// dark world (§25's "one bright green tree"). The world's light grid was
	// loaded the whole time and simply never read.
	//
	// All four terms are ported and this is now the default path (§71); the
	// branch below survives only for LT_NO_MODEL_LIGHTING=1. See rm_UseAmbient().
	const bool bAmbient = rm_UseAmbient();
	const float fR = g_fModelAmbient[0], fG = g_fModelAmbient[1], fB = g_fModelAmbient[2];
	const uint8 nR = (uint8)((fR < 0.0f) ? 0.0f : (fR > 255.0f ? 255.0f : fR));
	const uint8 nG = (uint8)((fG < 0.0f) ? 0.0f : (fG > 255.0f ? 255.0f : fG));
	const uint8 nB = (uint8)((fB < 0.0f) ? 0.0f : (fB > 255.0f ? 255.0f : fB));

	// The old stand-in key light — LT_NO_MODEL_LIGHTING=1 only.
	static const LTVector s_vLight(0.42f, 0.78f, 0.46f);

	// ★ ONE LOOP, TWO CONSUMERS. The colour below is the whole of §68-§71 --
	// four D3D lighting terms resolved per vertex -- and it must not fork.
	// Metal collects the same values into a triangle list instead of emitting
	// them immediately; everything above and inside this loop is shared.
	static std::vector<MTLModelVert> s_aMetalVerts;   // static: no per-mesh alloc
	s_aMetalVerts.clear();
	s_aMetalVerts.reserve(pMesh->m_aIndices.size());

	// ★★ THE LIGHTING IS PER *VERTEX*, BUT THIS LOOP WALKS *INDICES*.
	// A vertex is referenced by every triangle that shares it -- around six in a
	// closed mesh -- so the colour below (including the whole per-light loop,
	// with its normalise, dot and attenuation divide for every point light) used
	// to be recomputed that many times for the same answer. It depends only on
	// nVert: g_aSkinnedNormal/g_aSkinnedPos plus per-instance globals that are
	// constant for the duration of this call. So compute it once per vertex.
	//
	// A GENERATION STAMP rather than clearing: clearing would cost O(verts) per
	// mesh even when few are used, and this way a vertex no index touches is
	// never lit at all. s_nStamp is bumped per call, so every entry from a
	// previous mesh is stale by construction.
	// LT_NO_VERTCACHE=1 -- bypass the cache and light per index, the way this
	// loop worked before. Kept as the A/B for the optimisation: it is the only
	// way to measure the win in one binary and one scene, which matters because
	// two real-shell runs land in different places (and, as this change proved,
	// can even end up in different VSYNC states).
	static int s_nNoVertCache = -1;
	if (s_nNoVertCache < 0) s_nNoVertCache = getenv("LT_NO_VERTCACHE") ? 1 : 0;

	static std::vector<uint32> s_aVertColor;   // packed 0x00BBGGRR
	static std::vector<uint32> s_aVertStamp;
	static uint32 s_nStamp = 0;
	if (s_aVertColor.size() < pMesh->m_nVertCount)
	{
		s_aVertColor.resize(pMesh->m_nVertCount);
		s_aVertStamp.resize(pMesh->m_nVertCount, 0);
	}
	if (++s_nStamp == 0)   // wrapped: every stored stamp would alias. ~4e9 meshes.
	{
		std::fill(s_aVertStamp.begin(), s_aVertStamp.end(), 0u);
		s_nStamp = 1;
	}

	for (size_t nIdx = 0; nIdx < pMesh->m_aIndices.size(); ++nIdx)
	{
		uint32 nVert = pMesh->m_aIndices[nIdx];
		if (nVert >= pMesh->m_nVertCount)
			continue;

		uint8 nCR = 255, nCG = 255, nCB = 255;

		if (!s_nNoVertCache && s_aVertStamp[nVert] == s_nStamp)
		{
			const uint32 nPacked = s_aVertColor[nVert];
			nCR = (uint8)(nPacked);
			nCG = (uint8)(nPacked >> 8);
			nCB = (uint8)(nPacked >> 16);
		}
		else
		{

		// ⚠️ glColor3ub would force alpha to 1.0. The object's own alpha is the
		// DIFFUSE alpha the authored stage ops modulate against -- D3D feeds it
		// in as RenderPieceList(m_ColorA/255) (drawmodel.cpp:96).
		if (bAmbient)
		{
			// ambient + SUM(light colour x N.L), exactly D3D's accumulation.
			// The lights are already attenuated and already scaled by
			// (1 - convertToAmbient); their direction is fixed per instance.
			float fSR = fR, fSG = fG, fSB = fB;
			if (g_nModelLights > 0)
			{
				LTVector vN = g_aSkinnedNormal[nVert];
				const float fLen = vN.Mag();
				if (fLen > 0.0001f)
				{
					vN /= fLen;
					for (int nL = 0; nL < g_nModelLights; ++nL)
					{
						const RModelLight &cL = g_aModelLights[nL];
						LTVector vToLight = cL.m_vToLight;
						float    fAtten   = 1.0f;

						// ★ TERM 3 ONLY: a real point light, so both the direction
						// and the attenuation are per vertex (§71). Every other
						// term is a dir light and falls straight through with
						// fAtten = 1, which is bit-for-bit what it did before.
						if (cL.m_bPoint)
						{
							vToLight = cL.m_vPos - g_aSkinnedPos[nVert];
							const float fD2 = vToLight.MagSqr();
							if (fD2 >= cL.m_fRadiusSqr) continue;   // Range cull
							const float fD = sqrtf(fD2);
							if (fD < 0.0001f)
								vToLight = vN;                      // sitting on it
							else
								vToLight /= fD;
							const float fDen = cL.m_vAttCoef.x + cL.m_vAttCoef.y * fD
							                 + cL.m_vAttCoef.z * fD2;
							if (fDen <= 0.0001f) continue;
							fAtten = 1.0f / fDen;
						}

						const float fNdotL = vN.Dot(vToLight);
						if (fNdotL <= 0.0f) continue;
						const float fScale = fAtten * fNdotL;
						fSR += cL.m_vColor.x * fScale;
						fSG += cL.m_vColor.y * fScale;
						fSB += cL.m_vColor.z * fScale;
					}
				}
			}
			nCR = (uint8)(fSR > 255.0f ? 255.0f : fSR);
			nCG = (uint8)(fSG > 255.0f ? 255.0f : fSG);
			nCB = (uint8)(fSB > 255.0f ? 255.0f : fSB);
		}
		else
		{
			uint8 nShade = 255;
			if (bLit)
			{
				LTVector vNormal = g_aSkinnedNormal[nVert];
				float fLen = vNormal.Mag();
				float fNdotL = (fLen > 0.0001f) ? vNormal.Dot(s_vLight) / fLen : 1.0f;
				if (fNdotL < 0.0f) fNdotL = -fNdotL;
				nShade = (uint8)(140.0f + 115.0f * fNdotL);
			}
			nCR = nCG = nCB = nShade;
		}

		s_aVertColor[nVert] = (uint32)nCR | ((uint32)nCG << 8) | ((uint32)nCB << 16);
		s_aVertStamp[nVert] = s_nStamp;
		}   // end of the per-vertex lighting (cache miss)

		const LTVector &vPos = g_aSkinnedPos[nVert];
		const float fU = pMesh->m_aUV[(size_t)nVert * 2];
		const float fV = pMesh->m_aUV[(size_t)nVert * 2 + 1];

		MTLModelVert cV = { vPos.x, vPos.y, vPos.z, fU, fV, nCR, nCG, nCB, nAlpha };
		s_aMetalVerts.push_back(cV);
	}

	// ★ LT_TRACE_MODELEMIT=1 — one line per distinct authored state, with the
	// FIRST vertex colour the shared lighting produced. Both backends run the
	// same colour maths, so if these lines agree the difference is downstream
	// (the shader or the state), and if they disagree it is upstream.
	// ⚠️ CACHED. getenv() is a linear scan of the environment and this runs once
	// per MESH per FRAME; every other gate in this file already uses this
	// pattern. Uncached it showed up in the profile as real frame cost.
	static int s_nTraceEmit = -1;
	if (s_nTraceEmit < 0) s_nTraceEmit = getenv("LT_TRACE_MODELEMIT") ? 1 : 0;
	if (s_nTraceEmit)
	{
		static std::vector<uint64> s_aSeen;
		const uint64 nKey = (uint64)(uintptr_t)g_cEmitState.m_pTexture
		                  ^ ((uint64)g_cEmitState.m_bIgnoreDiffuse << 1)
		                  ^ ((uint64)(uint32)(g_cEmitState.m_fColorScale * 16.0f) << 4)
		                  ^ ((uint64)g_cEmitState.m_bTextureAlpha << 12)
		                  ^ ((uint64)g_cEmitState.m_bBlend << 13)
		                  ^ ((uint64)g_cEmitState.m_nSrcBlend << 20);
		bool bNew = true;
		for (size_t i = 0; i < s_aSeen.size(); ++i)
			if (s_aSeen[i] == nKey) { bNew = false; break; }
		if (bNew && !s_aMetalVerts.empty())
		{
			s_aSeen.push_back(nKey);
			uint8 r = 255, g = 255, b = 255, a = 255;
			if (!s_aMetalVerts.empty())
			{
				r = s_aMetalVerts[0].r; g = s_aMetalVerts[0].g;
				b = s_aMetalVerts[0].b; a = s_aMetalVerts[0].a;
			}
			fprintf(stderr, "[memit] tex=%p noDiffuse=%d scale=%.1f texAlpha=%d blend=%d "
			                "src=0x%X dst=0x%X zw=%d aTest=%d ref=%.2f v0rgba=(%u %u %u %u)\n",
			        (void*)g_cEmitState.m_pTexture, (int)g_cEmitState.m_bIgnoreDiffuse,
			        g_cEmitState.m_fColorScale, (int)g_cEmitState.m_bTextureAlpha,
			        (int)g_cEmitState.m_bBlend, g_cEmitState.m_nSrcBlend,
			        g_cEmitState.m_nDstBlend, (int)g_cEmitState.m_bZWrite,
			        (int)g_cEmitState.m_bAlphaTest, g_cEmitState.m_fAlphaRef,
			        r, g, b, a);
		}
	}

	MTLModel_DrawTris(s_aMetalVerts.empty() ? 0 : &s_aMetalVerts[0],
	                  (uint32)s_aMetalVerts.size(), &g_cEmitState);
}

// Draws one model instance (pieces + attachments). Shared by the world-space
// pass and the really-close (player-view) pass -- the two differ only in the
// matrices set up around them.
// Draws one model instance (pieces + attachments). Shared by the world-space
// pass and the really-close (player-view) pass -- the two differ only in the
// matrices set up around them.
static void rm_DrawModelInstance(ModelInstance *pInstance, bool bLog)
{
	Model *pModel = pInstance->GetModelDB();
	if (!pModel)
		return;

	// LT_TRACE_PVMODEL: the player-view (really-close) models, every frame they
	// change -- which model, where in camera space, and which pieces are hidden.
	static int s_nTracePVModel = -1;
	if (s_nTracePVModel < 0) s_nTracePVModel = getenv("LT_TRACE_PVMODEL") ? 1 : 0;
	if ((pInstance->m_Flags & FLAG_REALLYCLOSE) && s_nTracePVModel)
	{
		static const Model *s_pLastModel = 0;
		static uint32 s_nLastHidden = 0xFFFFFFFF;
		uint32 nHiddenMask = 0;
		for (uint32 n = 0; n < pModel->NumPieces() && n < 32; ++n)
			if (pInstance->IsPieceHidden(n)) nHiddenMask |= (1u << n);
		if (pModel != s_pLastModel || nHiddenMask != s_nLastHidden)
		{
			s_pLastModel = pModel; s_nLastHidden = nHiddenMask;
			bLog = true;   // also dump this model's per-piece boxes below
			fprintf(stderr, "[pv] %s: %u pieces (hidden mask 0x%x) @cam(%.1f %.1f %.1f) scale(%.2f %.2f %.2f)\n",
			        pModel->GetFilename(), pModel->NumPieces(), nHiddenMask,
			        pInstance->m_Pos.x, pInstance->m_Pos.y, pInstance->m_Pos.z,
			        pInstance->m_Scale.x, pInstance->m_Scale.y, pInstance->m_Scale.z);
		}
	}

	if (bLog)
		fprintf(stderr, "[glm] model @(%.0f %.0f %.0f): %s (%u pieces, %u nodes)%s\n",
		        pInstance->m_Pos.x, pInstance->m_Pos.y, pInstance->m_Pos.z,
		        pModel->GetFilename(), pModel->NumPieces(), pModel->NumNodes(),
		        (pInstance->m_Flags & FLAG_REALLYCLOSE) ? "  [REALLYCLOSE / player view]" : "");

	// The object-level render state D3D feeds to RenderPieceList/d3d_GetBlendStates
	// -- none of which the GL model path currently reads (see LT_TRACE_UI).
	if (g_bRTraceUIFrame)
	{
		fprintf(stderr, "[ui]   %s: flags=0x%x flags2=0x%x color=(%u %u %u a=%u) "
		                "translucent=%d scale=(%.2f %.2f %.2f)\n",
		        pModel->GetFilename(), pInstance->m_Flags, pInstance->m_Flags2,
		        pInstance->m_ColorR, pInstance->m_ColorG, pInstance->m_ColorB,
		        pInstance->m_ColorA, (int)pInstance->IsTranslucent(),
		        pInstance->m_Scale.x, pInstance->m_Scale.y, pInstance->m_Scale.z);
	}

	// (Attachments are no longer processed here — RModel_ProcessAttachments
	// does it for EVERY object type once per scene, before any draw pass. Doing
	// it in the model draw only ever covered OT_MODEL parents, which is why a
	// handle attached to a world-model DOOR never turned with it.)

	// Mark every drawn piece's used nodes for evaluation BEFORE pulling
	// the transforms — unmarked nodes stay zero matrices (verts collapse
	// to the origin). Mirrors setupmodel.cpp's SetupLODNodePath calls.
	for (uint32 nPiece = 0; nPiece < pModel->NumPieces(); ++nPiece)
	{
		if (pInstance->IsPieceHidden(nPiece))
			continue;
		ModelPiece *pPiece = pModel->GetPiece(nPiece);
		CDIModelDrawable *pLOD = pPiece ? pPiece->GetLOD((uint32)0) : 0;
		if (pLOD)
			pInstance->SetupLODNodePath(pLOD);
	}

	DDMatrix *pTransforms = pInstance->GetRenderingTransforms();
	if (!pTransforms)
		return;
	uint32 nNumNodes = pModel->NumNodes();

	// The object's alpha rides the vertex colour (see rm_EmitMesh).
	const uint8 nObjAlpha = pInstance->m_ColorA;

	// Does this instance get the stand-in key light at all? See rm_EmitMesh --
	// D3D lights a model only from the world, and not at all under FLAG_NOLIGHT.
	// The interface pass draws no world, so nothing there is lit either.
	const bool bLit = !g_bRInterfacePass && RWorld_IsLoaded() &&
	                  !(pInstance->m_Flags & FLAG_NOLIGHT);

	// Resolve this instance's ambient from the world light grid once, here —
	// not per vertex. D3D likewise evaluates it once per instance, at the root
	// node position (setupmodel.cpp:305).
	rm_SetupInstanceLight(pInstance, pInstance->GetPos(),
	                       pModel ? pModel->m_VisRadius : 0.0f, bLit,
	                       (pInstance->m_Flags & FLAG_REALLYCLOSE) != 0);

	// ★★ DRAW ORDER IS AUTHORED: ModelPiece::m_nRenderPriority.
	// D3D sorts its piece list on it (rendermodelpiecelist.cpp:273 — "render
	// lowest priority first, then higher, and so on"); we used to draw in flat
	// file order. chars\models\player_action.ltb authors body and head at
	// priority 0 and hair, eyelashes and the hair-scalp piece at priority 1 —
	// and the priority-1 pieces are exactly the blended/alpha-tested ones, so
	// in file order the hair composited before the head it sits on.
	//
	// Within one priority, blended pieces go LAST: a blended piece needs what
	// is behind it already in the frame buffer. The sort is STABLE so ties
	// otherwise keep file order (D3D's next tie-breakers are style and texture
	// address, which are grouping-for-batching, not correctness).
	static std::vector<uint32> s_aPieceOrder;   // static: no per-frame alloc
	s_aPieceOrder.clear();
	for (uint32 nPiece = 0; nPiece < pModel->NumPieces(); ++nPiece)
		s_aPieceOrder.push_back(nPiece);

	std::stable_sort(s_aPieceOrder.begin(), s_aPieceOrder.end(),
		[pModel, pInstance](uint32 nA, uint32 nB) -> bool
		{
			ModelPiece *pA = pModel->GetPiece(nA);
			ModelPiece *pB = pModel->GetPiece(nB);
			if (!pA || !pB)
				return false;

			if (pA->m_nRenderPriority != pB->m_nRenderPriority)
				return pA->m_nRenderPriority < pB->m_nRenderPriority;

			// Same priority: opaque before blended.
			CRenderStyle *pSA = LTNULL, *pSB = LTNULL;
			pInstance->GetRenderStyle((uint32)pA->m_iRenderStyle, &pSA);
			pInstance->GetRenderStyle((uint32)pB->m_iRenderStyle, &pSB);
			RRenderStyleState cA, cB;
			RRenderStyle_GetState(pSA, 0, &cA);
			RRenderStyle_GetState(pSB, 0, &cB);
			if (cA.bBlend != cB.bBlend)
				return !cA.bBlend;

			return false;   // stable: keep file order
		});

	for (size_t nOrder = 0; nOrder < s_aPieceOrder.size(); ++nOrder)
	{
		const uint32 nPiece = s_aPieceOrder[nOrder];

		if (pInstance->IsPieceHidden(nPiece))
			continue;

		ModelPiece *pPiece = pModel->GetPiece(nPiece);
		CDIModelDrawable *pLOD = pPiece ? pPiece->GetLOD((uint32)0) : 0;
		if (!pLOD ||
		    (pLOD->GetType() != CRenderObject::eRigidMesh &&
		     pLOD->GetType() != CRenderObject::eSkelMesh &&
		     pLOD->GetType() != CRenderObject::eVAMesh))
		{
			if (bLog)
				fprintf(stderr, "[glm]   piece %u SKIPPED: %s\n", nPiece,
				        pLOD ? "unsupported LOD type" : "no LOD0");
			continue;
		}

		RModelMesh *pMesh = (RModelMesh*)pLOD;

		// A VA mesh's positions live in the ANIMATION, not in the LTB vertex
		// stream (which is zeroed) — refresh them from the current frame before
		// anything reads m_aPos. Mirrors rendermodelpiecelist.cpp:98.
		if (pLOD->GetType() == CRenderObject::eVAMesh)
			((RVAMesh*)pMesh)->UpdateVA(pModel, pInstance->m_AnimTracker.m_TimeRef);

		if (!pMesh->m_nVertCount || pMesh->m_aIndices.empty())
		{
			if (bLog)
				fprintf(stderr, "[glm]   piece %u SKIPPED: empty mesh (%u verts)\n",
				        nPiece, pMesh->m_nVertCount);
			continue;
		}

		if (!rm_SkinMesh(pMesh, pTransforms, nNumNodes))
		{
			if (bLog)
				fprintf(stderr, "[glm]   piece %u SKIPPED: skinning failed\n", nPiece);
			continue;
		}

		if (bLog && pMesh->m_nVertCount)
		{
			LTVector vMin = g_aSkinnedPos[0], vMax = g_aSkinnedPos[0];
			for (uint32 nVert = 1; nVert < pMesh->m_nVertCount; ++nVert)
			{
				VEC_MIN(vMin, vMin, g_aSkinnedPos[nVert]);
				VEC_MAX(vMax, vMax, g_aSkinnedPos[nVert]);
			}
			fprintf(stderr, "[glm]   piece %u (%s, %u verts): box (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
			        nPiece, pLOD->GetType() == CRenderObject::eSkelMesh ? "skel" :
			                (pLOD->GetType() == CRenderObject::eVAMesh ? "va" : "rigid"),
			        pMesh->m_nVertCount, vMin.x, vMin.y, vMin.z, vMax.x, vMax.y, vMax.z);
		}

		// Base texture from the instance skins.
		unsigned nTexName = 0;
		unsigned int nAlphaRef = 0;
		int nSkin = pPiece->m_iTextures[0];
		if (nSkin >= 0 && nSkin < MAX_MODEL_TEXTURES && pInstance->m_pSkins[nSkin])
		{
			// Neutral queries -- see render_texture.h. The GL-era GLTex_GetName
			// returns garbage that is non-zero, so `if (nTexName)` would still
			// pass while the AUTHORED AlphaRef fallback read nonsense.
			SharedTexture *pSkinTex = pInstance->m_pSkins[nSkin];
			nTexName  = RTex_IsValid(pSkinTex)
			          ? 1u : 0u;   // diagnostics only
			nAlphaRef = RTex_GetAlphaRef(pSkinTex);
		}

		// ★ THE AUTHORED RENDER STATE FOR A MODEL PIECE IS ITS RENDER STYLE.
		// D3D's model path takes alpha test / blend / z entirely from the
		// style's render pass (d3d_renderstatemgr.cpp), NOT from the texture --
		// the texture's AlphaRef is a WORLD-shader mechanism
		// (d3d_rendershader_gouraud*.cpp). So the style wins when there is one.
		// Without this, a leaf card whose skin carries no AlphaRef had nothing
		// marking it a cutout and drew the artist's flat fill colour: the solid
		// red/green tree canopies and the opaque bicycle wheels.
		CRenderStyle *pRenderStyle = LTNULL;
		pInstance->GetRenderStyle((uint32)pPiece->m_iRenderStyle, &pRenderStyle);

		RRenderStyleState rsState;
		bool bHaveStyle = RRenderStyle_GetState(pRenderStyle, 0, &rsState);

		if (bLog)
		{
			fprintf(stderr, "[glm]     style=%s alphaTest=%d ref=%.3f blend=%d | texAlphaRef=%u\n",
			        (pRenderStyle && pRenderStyle->GetFilename()) ? pRenderStyle->GetFilename() : "<none>",
			        (int)(bHaveStyle && rsState.bAlphaTest), rsState.fAlphaRef,
			        (int)(bHaveStyle && rsState.bBlend), nAlphaRef);
		}

		// Start each piece from the fixed-function defaults, then let the
		// authored style overwrite exactly what it authors -- the same order
		// the GL calls below apply in.
		g_cEmitState = REmitState();
		g_cEmitState.m_pTexture = (nSkin >= 0 && nSkin < MAX_MODEL_TEXTURES)
		                        ? pInstance->m_pSkins[nSkin] : 0;

		if (nTexName)
		{
			if (bHaveStyle)
			{
				// ★ Authored style -- and ALL of its passes, not just pass 0.
				// RS/glass.ltb is the poster child: pass 0 alpha-tests
				// GEQUAL ~250 so only the near-opaque frame survives, and
				// pass 1 BLENDS the translucent glass on top. Drawing only
				// pass 0 made every glass object (bottles, the kerosene
				// lamp's chimney, shed windows) lose exactly its glass.
				uint32 nPassCount = pRenderStyle ? pRenderStyle->GetRenderPassCount() : 1;
				if (nPassCount < 1) nPassCount = 1;

				for (uint32 nPass = 0; nPass < nPassCount; ++nPass)
				{
					RRenderStyleState cPass;
					if (!RRenderStyle_GetState(pRenderStyle, nPass, &cPass))
						continue;

					// The pass is DESCRIBED, never applied -- everything below
					// is read per draw out of REmitState.
					g_cEmitState.m_bAlphaTest     = cPass.bAlphaTest;
					g_cEmitState.m_nAlphaFunc     = cPass.nAlphaFunc;
					g_cEmitState.m_fAlphaRef      = cPass.fAlphaRef;
					g_cEmitState.m_bBlend         = cPass.bBlend;
					g_cEmitState.m_nSrcBlend      = cPass.nSrcBlend;
					g_cEmitState.m_nDstBlend      = cPass.nDstBlend;
					g_cEmitState.m_bZTest         = cPass.bZRead;
					g_cEmitState.m_bZWrite        = cPass.bZWrite;
					g_cEmitState.m_bIgnoreDiffuse = cPass.bIgnoreDiffuse;
					// ⚠️ THE EFFECTIVE SCALE, NOT THE AUTHORED ONE. MODULATE2X
					// is parked behind LT_RS_COLORSCALE (§68/§71) and the GL
					// path forces 1.0 when it is off; passing the authored 2.0
					// to Metal un-parked it and made every character wearing
					// such a style up to +48/255 too bright (§89).
					g_cEmitState.m_fColorScale    =
						RRenderStyle_ColorScaleEnabled() ? cPass.fColorScale : 1.0f;
					g_cEmitState.m_bTextureAlpha  = cPass.bTextureAlpha;

					// ★ The authored colour pipeline for this pass. A
					// "NODIFFUSE" style (ColorOp SELECTARG1 / Arg1 TEXTURE)
					// means the vertex colour must not touch the texture at
					// all -- carried by m_bIgnoreDiffuse / m_fColorScale above.
					rm_EmitMesh(pMesh, bLit, nObjAlpha);
				}

				continue;
			}
			else if (nAlphaRef != 0)
			{
				// No style on this model -- fall back to the texture's authored
				// AlphaRef (§14/§15b). This is what kept the gift boxes right
				// before render styles existed, so it stays as the fallback.
				g_cEmitState.m_bAlphaTest = true;
				g_cEmitState.m_nAlphaFunc = kRAlpha_GEqual;
				g_cEmitState.m_fAlphaRef  = (float)nAlphaRef / 255.0f;
			}
		}
		else
		{
			g_cEmitState.m_pTexture = 0;
		}

		rm_EmitMesh(pMesh, bLit, nObjAlpha);
	}
}

// Is this instance drawn in the player-view (camera-space) pass?
static inline bool rm_IsDrawable(ModelInstance *pInstance)
{
	if (!pInstance || !(pInstance->m_Flags & FLAG_VISIBLE))
		return false;

	// Hidden render group (d3d parity: tagnodes.cpp gates visibility on this
	// too) -- this is how the game hides the player's own body in first person.
	if (g_pRenderStruct && g_pRenderStruct->IsObjectGroupEnabled &&
	    !g_pRenderStruct->IsObjectGroupEnabled(pInstance->m_nRenderGroup))
		return false;

	return true;
}

// ★★ THE PLACEHOLDER MUST NOT BE DRAWN IN PLAYER VIEW.
//
// setupobject.cpp binds `models\\default.ltb` -- a 1-unit cube -- to any model
// that is missing OR that names no file at all, and records the interned Model*
// here. In the WORLD that cube is harmless noise (and a useful tell: it is how
// FX\\WATERFALL2.LTB was caught, PHASE2_HANDOFF §36). In the PLAYER-VIEW pass it
// is catastrophic: the pass uses a 0.1 near plane and the model sits ~0.2 units
// from the camera, so the box straddles the near plane, clips open, and fills
// the screen with its untextured interior.
//
// That is the holster white-out: WEAPONS.TXT [Weapon30] is named "Holster" and
// authors PVModel = "" (an EMPTY string), CClientWeapon::CreateWeaponModel has
// no empty-name guard, and the object arrives here bound to the placeholder with
// FLAG_VISIBLE|FLAG_REALLYCLOSE.
//
// ⚠️ Compared by POINTER IDENTITY against the interned Model, never by filename
// string -- models are interned, so this is exact, and it cannot be fooled by
// path spelling or case the way a strcmp would be.
//
// ⚠️ Scoped to the PLAYER-VIEW pass only. Do NOT extend it to the world pass:
// the visible cube there is the diagnostic that makes a missing asset obvious,
// and suppressing it would re-hide exactly the class of bug §36 was about.
extern Model *g_pPlaceholderModel;

static inline bool rm_IsPlaceholderModel(ModelInstance *pInstance)
{
	return g_pPlaceholderModel &&
	       pInstance->GetModelDB() == g_pPlaceholderModel;
}

// LT_TRACE_PV=1 -- trace the PLAYER-VIEW (FLAG_REALLYCLOSE) model set.
static bool rm_TracePV()
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_TRACE_PV") ? 1 : 0;
	return s_n != 0;
}

static bool g_bCensusDone = false;
void RModel_ArmCensus(void) { g_bCensusDone = false; }

// LT_TRACE_UI=<sceneframe>: dump the per-object render state of EVERY model and
// sprite drawn on that one scene frame. The ordinary [glm] census only fires
// once a world is loaded, so it can never describe the menu / loading screens --
// which is exactly where the interface objects live.
int  g_nRSceneFrame = 0;
bool g_bRTraceUIFrame = false;
// True while RObjectList_Draw is running (the DRAWMODE_OBJECTLIST interface pass).
bool g_bRInterfacePass = false;
// Called once per rendered scene, from nr_RenderScene -- so it counts the
// object-list (interface) passes too, which do not go through
// RModel_DrawModels at all.
void RModel_BeginSceneFrame(void);
static int rui_TargetFrame(void)
{
	static int s_nTarget = -2;
	if (s_nTarget == -2)
	{
		const char *p = getenv("LT_TRACE_UI");
		s_nTarget = p ? atoi(p) : -1;
	}
	return s_nTarget;
}

void RModel_BeginSceneFrame(void)
{
	++g_nRSceneFrame;
	g_bRTraceUIFrame = (rui_TargetFrame() > 0 && g_nRSceneFrame == rui_TargetFrame());
	if (g_bRTraceUIFrame)
		fprintf(stderr, "[ui] ===== scene frame %d: model + sprite render state =====\n",
		        g_nRSceneFrame);
}

void RModel_DrawModels(float fAspect)
{
	if (!g_pClientMgr)
		return;

	// One-shot inventory of what's being drawn (bring-up diagnostics).
	// Fire the census on the first frame WITH A WORLD LOADED. Firing it on the
	// very first frame (the old behaviour) only ever described the main menu,
	// which is why "what is actually in the level" was never visible here.
	bool bLog = !g_bCensusDone && RWorld_IsLoaded();
	if (bLog)
		g_bCensusDone = true;

	if (g_bRTraceUIFrame)
		bLog = true;

	// Census of EVERY client object type vs. what this renderer actually draws.
	if (bLog)
	{
		static const char *s_apTypeNames[NUM_OBJECTTYPES] =
		{
			"Normal", "Model", "WorldModel", "Sprite", "Light", "Camera",
			"ParticleSystem", "PolyGrid", "LineSystem", "Container",
			"Canvas", "VolumeEffect"
		};
		fprintf(stderr, "[glm] --- client object census (visible/total) ---\n");
		for (uint32 nType = 0; nType < NUM_OBJECTTYPES; ++nType)
		{
			uint32 nTotal = 0, nVisible = 0;
			LTLink *pTypeHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[nType].m_Head;
			for (LTLink *pIt = pTypeHead->m_pNext; pIt != pTypeHead; pIt = pIt->m_pNext)
			{
				LTObject *pObj = (LTObject*)pIt->m_pData;
				if (!pObj) continue;
				++nTotal;
				if (pObj->m_Flags & FLAG_VISIBLE) ++nVisible;
			}
			if (nTotal)
			{
				// ⚠️ Keep this list honest — it is the first thing anyone reads
				// when something is missing, and a stale "NOT DRAWN" sends the
				// next investigation down the wrong path.
				bool bDrawn = (nType == OT_MODEL || nType == OT_WORLDMODEL ||
				               nType == OT_SPRITE || nType == OT_POLYGRID ||
				               nType == OT_PARTICLESYSTEM);
				fprintf(stderr, "[glm]   %-15s %3u/%3u  %s\n", s_apTypeNames[nType],
				        nVisible, nTotal, bDrawn ? "drawn" : "NOT DRAWN by the renderer");
			}
		}
	}

	// Under Metal every one of these is a per-draw parameter (REmitState /
	// the pipeline state), applied in MTLModel_DrawTris. The scene transform was
	// published by nr_RenderScene.
	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_MODEL].m_Head;

	// --- Pass 1: ordinary world-space models (caller has set the scene matrices).
	uint32 nTot = 0, nInvisible = 0, nGroupOff = 0, nNoDB = 0, nDrawn = 0;
	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		ModelInstance *pInstance = (ModelInstance*)pCur->m_pData;
		if (!pInstance)
			continue;
		++nTot;
		if (!(pInstance->m_Flags & FLAG_VISIBLE)) { ++nInvisible; continue; }
		if (g_pRenderStruct && g_pRenderStruct->IsObjectGroupEnabled &&
		    !g_pRenderStruct->IsObjectGroupEnabled(pInstance->m_nRenderGroup)) { ++nGroupOff; continue; }
		if (pInstance->m_Flags & FLAG_REALLYCLOSE)
			continue;
		if (!pInstance->GetModelDB()) { ++nNoDB; continue; }
		++nDrawn;
		rm_DrawModelInstance(pInstance, bLog);
	}
	if (bLog)
		fprintf(stderr, "[glm] OT_MODEL: %u total, %u drawn | skipped: %u !VISIBLE, "
		                "%u render-group off, %u no model DB\n",
		        nTot, nDrawn, nInvisible, nGroupOff, nNoDB);

}

// ---------------------------------------------------------------------------
// The player-view (FLAG_REALLYCLOSE) pass -- Cate's hands and the weapon she is
// holding.
//
// ⚠️ THIS MUST BE THE LAST THING DRAWN IN THE SCENE, because it CLEARS THE DEPTH
// BUFFER (see the glClear below). Anything world-space drawn after it depth-tests
// against a buffer that no longer contains the world, so it paints over
// everything. That is exactly what happened to the water: the translucent
// polygrid pass ran after this one and the river floated over the entire level --
// while cinematics, which draw no player-view weapon and so never clear, looked
// correct. Split out of RModel_DrawModels for that reason (2026-07-25).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ★★ ATTACHMENT TRANSFORMS, FOR EVERY OBJECT TYPE.
//
// An attached object does not carry its own world transform: the renderer is
// what recomputes it from the parent each frame, via
// RenderStruct::ProcessAttachment -> r_ProcessAttachment (render.cpp:351) ->
// CLTCommonShared::GetAttachmentTransform, which composes
//     rot = parentRot * offsetRot
//     pos = parentRot * offsetPos + parentPos
// and then MOVES and ROTATES the child object.
//
// ⚠️ WE ONLY EVER DID THIS FOR OT_MODEL PARENTS (inside the model draw walk).
// D3D does it for every object it processes — d3d_ReallyProcessObject ends with
// `if(pObject->m_Attachments) d3d_ProcessAttachments(pObject, 0)`
// (tagnodes.cpp:104) — and doors/windows in this game are WORLD MODELS. So a
// door handle or a window pane attached to one was never re-oriented on the
// client: it tracked the parent's POSITION (which the server replicates) but
// kept its original ROTATION. Symptom: the handle slides along with the door
// but never turns, and the India window's glass stays at the closed angle,
// blocking the gap you are supposed to climb through.
//
// Recursion mirrors d3d_ProcessAttachments, including its depth cap: an
// attachment chain can nest (parent -> child -> grandchild).
// ---------------------------------------------------------------------------
static void rm_ProcessAttachmentsRecurse(LTObject *pObject, uint32 nDepth)
{
	if (!pObject || nDepth >= 32)      // d3d asserts depth < 32; just stop.
		return;

	for (Attachment *pAtt = pObject->m_Attachments; pAtt; pAtt = pAtt->m_pNext)
	{
		LTObject *pChild = g_pRenderStruct->ProcessAttachment(pObject, pAtt);
		if (pChild)
			rm_ProcessAttachmentsRecurse(pChild, nDepth + 1);
	}
}

void RModel_ProcessAttachments(void)
{
	if (!g_pClientMgr || !g_pRenderStruct || !g_pRenderStruct->ProcessAttachment)
		return;

	// Every object type, not just models — that is the whole point of this pass.
	for (uint32 nType = 0; nType < NUM_OBJECTTYPES; ++nType)
	{
		LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[nType].m_Head;
		for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
		{
			LTObject *pObject = (LTObject*)pCur->m_pData;
			if (pObject && pObject->m_Attachments)
				rm_ProcessAttachmentsRecurse(pObject, 0);
		}
	}
}

void RModel_DrawPlayerView(float fAspect)
{
	if (!g_pClientMgr)
		return;

	const bool bLog = false;
	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_MODEL].m_Head;

	// FLAG_REALLYCLOSE models: their m_Pos/m_Rotation are already in CAMERA
	// space, not world space (LTObject::InsertSpecial keeps them out of the
	// world tree entirely), so drawing them with the scene view matrix put them
	// near the world origin -- which is why no weapon was ever visible.
	// Mirrors d3d_SetReallyClose (3d_ops.cpp): identity view, its own narrow
	// projection, and a compressed depth range so the gun can never poke into
	// or be clipped by world geometry.
	bool bAnyReallyClose = false;
	std::string sPVSet;                 // LT_TRACE_PV signature, see below
	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		ModelInstance *pInstance = (ModelInstance*)pCur->m_pData;
		if (!rm_IsDrawable(pInstance) || !(pInstance->m_Flags & FLAG_REALLYCLOSE))
			continue;

		// A holstered weapon (or any PV model that failed to load) is bound to
		// the engine placeholder; drawing it whites out the screen. See
		// rm_IsPlaceholderModel.
		if (rm_IsPlaceholderModel(pInstance))
		{
			static bool s_bSaid = false;
			if (!s_bSaid)
			{
				s_bSaid = true;
				fprintf(stderr, "[pv] skipping placeholder model (models/default.ltb) "
				                "in the player-view pass\n");
			}
			continue;
		}

		if (!bAnyReallyClose)
		{
			bAnyReallyClose = true;

			// reallyclose_near / reallyclose_far / PVModelFOV defaults from
			// rendererconsolevars.h. PVModelFOV is the VERTICAL fov, in degrees.
			const float fNear = 0.1f, fFar = 7.0f;
			const float fFovY = 75.0f * 0.01745329251994f;
			float fTop   = fNear * tanf(fFovY * 0.5f);
			float fRight = fTop * ((fAspect > 0.01f) ? fAspect : 1.333f);

			// ★ THE PLAYER-VIEW SETUP: its own narrow projection, the LH->RH z
			// flip as the view (the engine is left-handed, +Z forward), a depth
			// range squeezed into the front tenth of the buffer (d3d sets the
			// viewport MinZ/MaxZ to 0..0.1 for exactly this), and a DEPTH-ONLY
			// clear so the weapon can never be clipped by the world.
			// ⚠️ Colour must NOT be cleared -- the world is already in the frame
			// buffer.
			{
				float aProj[16], aFlipZ[16];
				mtl_Frustum(aProj, fRight, fTop, fNear, fFar);
				mtl_Identity(aFlipZ);
				aFlipZ[10] = -1.0f;
				MTLModel_SetTransform(aFlipZ, aProj);
				MTLDev_SetDepthRange(0.0f, 0.1f);
				MTLDev_Clear(false, true, 0.0f, 0.0f, 0.0f, 1.0f);
			}
		}

		// LT_TRACE_PV=1: collect the player-view set; reported below ON CHANGE.
		if (rm_TracePV())
		{
			Model *pMD = pInstance->GetModelDB();
			const char *pszName = (pMD && pMD->GetFilename()) ? pMD->GetFilename() : "<null>";
			const LTVector &vP = pInstance->GetPos();
			char szLine[256];
			snprintf(szLine, sizeof(szLine),
			         "  %-34s pos(%.2f %.2f %.2f) flags=0x%x pieces=%d\n",
			         pszName, vP.x, vP.y, vP.z, pInstance->m_Flags,
			         pMD ? (int)pMD->NumPieces() : -1);
			sPVSet += szLine;

			// ★ PER-PIECE state. A weapon's EFFECT geometry is usually a piece
			// that the game SHOWS while firing (the welder's flame is skinned
			// with TexFX\RenderStyles\clouds.dtx, WEAPONS.TXT PVSkin2), so the
			// hidden mask has to be part of the signature — otherwise the set
			// never "changes" when the effect appears and this never reports it.
			uint32 nPieces = pMD ? pMD->NumPieces() : 0;
			for (uint32 nP = 0; nP < nPieces && nP < 16; ++nP)
			{
				ModelPiece *pPc = pMD->GetPiece(nP);
				CRenderStyle *pRS = LTNULL;
				if (pPc)
					pInstance->GetRenderStyle((uint32)pPc->m_iRenderStyle, &pRS);
				RRenderStyleState cSt;
				bool bHaveSt = RRenderStyle_GetState(pRS, 0, &cSt);
				snprintf(szLine, sizeof(szLine),
				         "      piece %u %-9s prio=%u style=%-24s blend=%d aTest=%d texAlpha=%d\n",
				         nP, pInstance->IsPieceHidden(nP) ? "HIDDEN" : "shown",
				         pPc ? (unsigned)pPc->m_nRenderPriority : 0,
				         (pRS && pRS->GetFilename()) ? pRS->GetFilename() : "<none>",
				         (int)(bHaveSt && cSt.bBlend), (int)(bHaveSt && cSt.bAlphaTest),
				         (int)cSt.bTextureAlpha);
				sPVSet += szLine;
			}
		}

		rm_DrawModelInstance(pInstance, bLog);
	}

	// ⚠️ Report on CHANGE, not per frame: this runs every frame and the
	// interesting event is the weapon SWITCHING. A per-frame dump would bury it.
	if (rm_TracePV())
	{
		static std::string s_sLastPVSet = "\x01";   // sentinel: never equal to a real set
		if (sPVSet != s_sLastPVSet)
		{
			s_sLastPVSet = sPVSet;
			if (sPVSet.empty())
				fprintf(stderr, "[pv] player-view set CHANGED: (empty -- nothing drawn)\n");
			else
				fprintf(stderr, "[pv] player-view set CHANGED:\n%s", sPVSet.c_str());
		}
	}

	if (bAnyReallyClose)
	{
		// ★ Player-view PARTICLE systems draw here, inside this pass's
		// projection/view — first-person muzzle and tool effects (the welder's
		// flame is Welder_PV_Muzz's ParticleSystem on the weapon's "Flash"
		// socket). In the world pass they land in the wrong space entirely.
		// ⚠️ Gated on bAnyReallyClose because the projection is only installed
		// when a player-view MODEL was drawn. Every PV effect in the retail
		// data attaches to the PV weapon, so the model is always there — but a
		// standalone PV particle system with no PV model would not draw.
		RParticle_DrawPlayerView();
		RSprite_DrawPlayerView();

		MTLDev_SetDepthRange(0.0f, 1.0f);
	}

}

// ---------------------------------------------------------------------------
// Sprites: camera-facing textured quads (lamp halos, glows, menu ScaleFX).
// Walks the client OT_SPRITE list; the engine's sprite manager owns .spr
// loading and frame animation — we just draw m_pCurFrame's texture. Billboard
// math mirrors d3d_DrawSprite: quad = pos ± camRight×(texW×scale.x)
//                                        ± camUp×(texH×scale.y).
// ---------------------------------------------------------------------------

#include "de_sprite.h"

static inline double vPosCheckSafe(float f) { return (double)f; }


void RSprite_SetCamera(const LTVector &vRight, const LTVector &vUp,
                        const LTVector &vForward, const LTVector &vPos)
{
	g_vSprCamRight = vRight;
	g_vSprCamUp    = vUp;
	g_vSprCamFwd   = vForward;
	g_vSprCamPos   = vPos;
}

// Shared GL state for a run of sprites. Split out of RSprite_DrawSprites so the
// object-list (interface) pass can draw sprites through the same code.
// NoZ (lamp-halo) census: how many exist, and how many survived the occlusion
// test. ⚠️ This is the ONLY visible difference between the GL depth-buffer
// readback and Metal's flatten-to-centre-depth equivalent, so it is the number
// to compare between backends.
static uint32 g_nSprNoZ = 0, g_nSprNoZDrawn = 0;

static void rspr_BeginSprites()
{
	// Every one of these is a per-draw parameter carried by REmitState, so
	// there is no ambient state left to set here.
}

// Draw ONE sprite instance. Returns true if it actually issued geometry.
// Assumes rspr_BeginSprites() has run.
static bool rspr_DrawSpriteInstance(SpriteInstance *pInstance, bool bLog, bool bPlayerView)
{
	{
		if (!pInstance)
			return false;
		if (!(pInstance->m_Flags & FLAG_VISIBLE))
			return false;
		if (g_pRenderStruct && g_pRenderStruct->IsObjectGroupEnabled &&
		    !g_pRenderStruct->IsObjectGroupEnabled(pInstance->m_nRenderGroup))
			return false;

		SpriteEntry *pFrame = pInstance->m_SpriteTracker.m_pCurFrame;
		SharedTexture *pTex = pFrame ? pFrame->m_pTex : 0;
		// ⚠️ Through the NEUTRAL queries: GLTex_* would reinterpret the Metal
		// entry and hand back a garbage name with zero dimensions, which this
		// very guard then turns into "sprite not drawn" (see render_texture.h).
		const bool bTexOk = RTex_IsValid(pTex);
		const unsigned nName = bTexOk ? 1u : 0u;   // diagnostics only
		uint32 nTexW = 0, nTexH = 0;
		if (!bTexOk || !RTex_GetDims(pTex, nTexW, nTexH))
		{
			if (bLog)
				fprintf(stderr, "[gls] sprite @(%.0f %.0f %.0f) NOT drawn: sprite=%p anim=%p frame=%p tex=%p gl=%u\n",
				        pInstance->m_Pos.x, pInstance->m_Pos.y, pInstance->m_Pos.z,
				        (void*)pInstance->m_SpriteTracker.m_pSprite,
				        (void*)pInstance->m_SpriteTracker.m_pCurAnim,
				        (void*)pFrame, (void*)pTex, nName);
			return false;
		}

		// Cull behind the camera (billboards have no depth extent of their own).
		// ⚠️ NOT for player-view sprites: their m_Pos is in camera space, so
		// subtracting the camera's WORLD position gives a meaningless distance.
		// D3D skips this test for FLAG_REALLYCLOSE too (drawsprite.cpp:408).
		float fZ = (pInstance->m_Pos - g_vSprCamPos).Dot(g_vSprCamFwd);
		if (!bPlayerView && fZ < 1.0f)
			return false;

		float fSizeX = (float)nTexW * pInstance->m_Scale.x;
		float fSizeY = (float)nTexH * pInstance->m_Scale.y;

		if (pInstance->m_Flags & FLAG_GLOWSPRITE)
		{
			// d3d_DrawSprite parity: glow sprites scale with camera distance.
			float fFactor = (fZ - 10.0f) / (500.0f - 10.0f);
			fFactor = LTCLAMP(fFactor, 0.0f, 1.0f);
			fFactor = 0.1f + 1.9f * fFactor;
			fSizeX *= fFactor;
			fSizeY *= fFactor;
		}

		REmitState cSprite;
		cSprite.m_pTexture   = pTex;
		cSprite.m_bZWrite    = false;   // translucent: test but do not write
		cSprite.m_bBlend     = true;
		cSprite.m_nSrcBlend  = (pInstance->m_Flags2 & FLAG2_ADDITIVE) ? kRBlend_One : kRBlend_SrcAlpha;
		cSprite.m_nDstBlend  = (pInstance->m_Flags2 & FLAG2_ADDITIVE) ? kRBlend_One : kRBlend_InvSrcAlpha;

		// Fog for a translucent object follows its blend mode, not the scene
		// (additive -> black fog, multiply -> white); see RWorld_ApplyObjectFog.
		RWorld_ApplyObjectFog(pInstance->m_Flags, pInstance->m_Flags2);
		if (pInstance->m_Flags & FLAG_SPRITE_NOZ)
			++g_nSprNoZ;

		if (pInstance->m_Flags & FLAG_SPRITE_NOZ)
		{
			// See REmitState::m_bFlattenDepth: the depth unit performs the
			// occlusion test the GL branch below does with a depth readback.
			float fNDCz = 0.0f;
			if (!MTLModel_ProjectDepth(pInstance->m_Pos.x, pInstance->m_Pos.y,
			                           pInstance->m_Pos.z, &fNDCz))
				return false;                       // behind the near plane
			cSprite.m_bFlattenDepth = true;
			cSprite.m_fFlatNDCz     = fNDCz - 0.002f;   // the GL probe's bias
			cSprite.m_bZTest        = true;
		}


		const LTVector &vPos = pInstance->m_Pos;
		// ★ A player-view sprite is already in CAMERA space, so the camera's
		// world basis is meaningless — D3D uses the untransformed axes
		// (drawsprite.cpp:384: right(1,0,0) up(0,1,0)). This is the welder's
		// flame: Welder_PV_Muzz is ParticleSystem + Sprite + DynaLight, and the
		// Sprite is the visible torch cone.
		LTVector vRight = (bPlayerView ? LTVector(1.0f, 0.0f, 0.0f) : g_vSprCamRight) * fSizeX;
		LTVector vUp    = (bPlayerView ? LTVector(0.0f, 1.0f, 0.0f) : g_vSprCamUp)    * fSizeY;

		if (pInstance->m_Flags & FLAG_SPRITE_NOZ)
			++g_nSprNoZDrawn;

		{
			// The same four corners, expanded to two triangles (Metal has no
			// GL_QUADS) in the same winding -- (0,1,2)(0,2,3).
			const float aCorner[4][2] = { { -1.0f, +1.0f }, { +1.0f, +1.0f },
			                              { +1.0f, -1.0f }, { -1.0f, -1.0f } };
			const float aUV[4][2] = { { 0,0 }, { 1,0 }, { 1,1 }, { 0,1 } };
			MTLModelVert aQuad[4];
			for (int i = 0; i < 4; ++i)
			{
				aQuad[i].x = vPos.x + vUp.x * aCorner[i][1] + vRight.x * aCorner[i][0];
				aQuad[i].y = vPos.y + vUp.y * aCorner[i][1] + vRight.y * aCorner[i][0];
				aQuad[i].z = vPos.z + vUp.z * aCorner[i][1] + vRight.z * aCorner[i][0];
				aQuad[i].u = aUV[i][0];
				aQuad[i].v = aUV[i][1];
				aQuad[i].r = pInstance->m_ColorR;
				aQuad[i].g = pInstance->m_ColorG;
				aQuad[i].b = pInstance->m_ColorB;
				aQuad[i].a = pInstance->m_ColorA;
			}
			static const int kTri[6] = { 0, 1, 2, 0, 2, 3 };
			MTLModelVert aTris[6];
			for (int i = 0; i < 6; ++i)
				aTris[i] = aQuad[kTri[i]];
			MTLModel_DrawTris(aTris, 6, &cSprite);
		}

		if (bLog)
			fprintf(stderr, "[gls] sprite @(%.0f %.0f %.0f) scale=(%.2f %.2f) tex=%ux%u flags=0x%x"
			                " flags2=0x%x color=(%u %u %u a=%u)\n",
			        vPos.x, vPos.y, vPos.z, pInstance->m_Scale.x, pInstance->m_Scale.y,
			        nTexW, nTexH, pInstance->m_Flags, pInstance->m_Flags2,
			        pInstance->m_ColorR, pInstance->m_ColorG, pInstance->m_ColorB,
			        pInstance->m_ColorA);
		return true;
	}
}

// Restore the state the rest of the scene assumes after a run of sprites.
static void rspr_EndSprites()
{
	RWorld_RestoreSceneFog();   // undo the per-sprite fog colour/enable
}

// Shared body. bPlayerView selects WHICH set: the world-space sprites or the
// FLAG_REALLYCLOSE (player-view) ones. They are in different spaces under
// different projections and cannot be drawn together.
static void rspr_DrawPass(bool bPlayerView)
{
	if (!g_pClientMgr)
		return;

	static bool s_bLogged = false;
	bool bLog = !s_bLogged || g_bRTraceUIFrame;
	if (!bPlayerView)
		s_bLogged = true;

	uint32 nTotal = 0, nDrawn = 0;
	g_nSprNoZ = 0; g_nSprNoZDrawn = 0;

	rspr_BeginSprites();

	LTLink *pHead = &g_pClientMgr->m_ObjectMgr.m_ObjectLists[OT_SPRITE].m_Head;
	for (LTLink *pCur = pHead->m_pNext; pCur != pHead; pCur = pCur->m_pNext)
	{
		SpriteInstance *pInstance = (SpriteInstance*)pCur->m_pData;
		if (!pInstance)
			continue;
		// Each pass takes only its own set.
		if (((pInstance->m_Flags & FLAG_REALLYCLOSE) != 0) != bPlayerView)
			continue;
		++nTotal;
		if (rspr_DrawSpriteInstance(pInstance, bLog, bPlayerView))
			++nDrawn;
	}

	// ⚠️ REPORT ON CHANGE, not once. The one-shot version fired on the first
	// frame -- before any world exists -- so it only ever described an empty
	// scene, which is the §28/§70 trap ("a diagnostic that can only fire once
	// lies"). The counts are a property of the frame, so key on them.
	static int s_nTraceSprites = -1;
	if (s_nTraceSprites < 0) s_nTraceSprites = getenv("LT_TRACE_SPRITES") ? 1 : 0;
	if (s_nTraceSprites)
	{
		static uint32 s_aLast[2][2] = { { 0xFFFFFFFFu, 0xFFFFFFFFu },
		                                { 0xFFFFFFFFu, 0xFFFFFFFFu } };
		const int nSet = bPlayerView ? 1 : 0;
		if (s_aLast[nSet][0] != nTotal || s_aLast[nSet][1] != nDrawn)
		{
			s_aLast[nSet][0] = nTotal; s_aLast[nSet][1] = nDrawn;
			fprintf(stderr, "[gls] %u %s sprites (%u drawn) | NoZ %u (%u survived occlusion)\n",
			        nTotal, bPlayerView ? "player-view" : "world", nDrawn,
			        g_nSprNoZ, g_nSprNoZDrawn);
		}
	}
	else if (bLog && (nTotal || !bPlayerView))
		fprintf(stderr, "[gls] %u %s sprites (%u drawn)\n", nTotal,
		        bPlayerView ? "player-view" : "world", nDrawn);

	rspr_EndSprites();
}

void RSprite_DrawSprites()
{
	rspr_DrawPass(false);
}

// ⚠️ Call from INSIDE the player-view pass, while its projection/view are
// installed. These sprites are in camera space; drawn in the world pass they
// are culled or land nowhere — that was the welder's missing flame.
void RSprite_DrawPlayerView()
{
	rspr_DrawPass(true);
}

// ---------------------------------------------------------------------------
// ★ DRAWMODE_OBJECTLIST -- render ONLY the objects the caller named, and no
// world at all.
//
// This is how the whole interface is drawn: CInterfaceMgr::UpdateInterfaceSFX
// ends in `g_pLTClient->RenderObjects(m_hInterfaceCamera, objs, next, ...)`,
// which reaches the renderer as SceneDesc::m_DrawMode == DRAWMODE_OBJECTLIST
// with m_pObjectList/m_ObjectListSize (clientmgr.cpp:1879, renderstruct.h:55).
// We ignored the draw mode and always drew the world plus EVERY client object,
// so a screen opened in-game (Load Game, Options, the loading screens) rendered
// the live level and the player-view weapon behind the menu art.
//
// Interface objects are drawn FULLBRIGHT: this pass has no world, so D3D's
// environment lighting -- all of which is gated on `g_have_world`
// (setupmodel.cpp:374/:413) -- contributes nothing to them.
// ---------------------------------------------------------------------------
void RObjectList_Draw(LTObject **ppObjects, int nCount)
{
	if (!ppObjects || nCount <= 0)
		return;

	const bool bLog = g_bRTraceUIFrame;
	if (bLog)
	{
		// ⚠️ This used to dump the AMBIENT GL state (fog, blend, lighting,
		// texenv…) because under fixed-function that state was the whole
		// answer to "why does the menu look like this". There is no ambient
		// state to read now -- every one of those is a per-draw parameter in
		// REmitState -- so the useful census is the object count plus the
		// per-object [ui] lines rm_DrawModelInstance already prints.
		fprintf(stderr, "[ui] object-list pass: %d objects\n", nCount);
	}

	// --- Models.
	g_bRInterfacePass = true;
	for (int n = 0; n < nCount; ++n)
	{
		LTObject *pObject = ppObjects[n];
		if (!pObject || pObject->m_ObjectType != OT_MODEL)
			continue;
		ModelInstance *pInstance = (ModelInstance*)pObject;
		if (!(pInstance->m_Flags & FLAG_VISIBLE) || !pInstance->GetModelDB())
			continue;
		rm_DrawModelInstance(pInstance, bLog);
	}
	g_bRInterfacePass = false;

	// --- Sprites (the menu backdrop and the title art are OT_SPRITE).
	bool bAnySprite = false;
	uint32 nSprites = 0;
	for (int n = 0; n < nCount; ++n)
	{
		LTObject *pObject = ppObjects[n];
		if (!pObject || pObject->m_ObjectType != OT_SPRITE)
			continue;
		if (!bAnySprite)
		{
			bAnySprite = true;
			rspr_BeginSprites();
		}
		// The interface pass names its objects explicitly and renders them with
		// the interface camera, so the world billboard basis is the right one.
		if (rspr_DrawSpriteInstance((SpriteInstance*)pObject, bLog, false))
			++nSprites;
	}
	if (bAnySprite)
		rspr_EndSprites();

	if (bLog)
		fprintf(stderr, "[ui] object-list pass: %u sprites drawn\n", nSprites);
}
