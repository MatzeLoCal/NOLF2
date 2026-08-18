// ----------------------------------------------------------------------- //
//
// MODULE  : render_style.cpp
//
// PURPOSE : Authored render styles -- see render_style.h.
//
//           Port of runtime/render_a/src/sys/d3d/d3d_renderstyle.cpp +
//           d3d_renderstyleinterface.cpp. The enum -> GL state mapping is
//           taken from d3d_renderstatemgr.cpp (which applies the pass) and
//           renderstylelookuptables.cpp (which holds the D3D constants), so
//           the translation is D3D-table -> GL-token rather than a guess.
//
// ----------------------------------------------------------------------- //

// NOTE: bdefs.h, not the d3d directory's precompile.h -- that one pulls in
// d3d9caps.h. Same include discipline as the other gl_*.cpp files.
#include "bdefs.h"

#include "render_style.h"

#include "client_filemgr.h"     // FileRef, IClientFileMgr
#include "ltb.h"                // LTB_Header, LTB_D3D_RENDERSTYLE_FILE
#include "iltstream.h"
#include "iltrenderstyles.h"

#include <stdio.h>
#include <string.h>

static IClientFileMgr* gls_client_file_mgr;
define_holder(IClientFileMgr, gls_client_file_mgr);

static bool rrs_Trace()
{
    static bool s_bTrace = (getenv("LT_TRACE_RENDERSTYLE") != NULL);
    return s_bTrace;
}

// --------------------------------------------------------------------------
// CRRenderStyle
// --------------------------------------------------------------------------

CRRenderStyle::CRRenderStyle()
{
    // CRenderStyle's own ctor zeroes m_iRefCnt and m_pFilename.
    SetDefaults();
}

bool CRRenderStyle::SetLightingMaterial(LightingMaterial& LightMaterial)
{
    m_LightingMaterial = LightMaterial;
    return true;
}

bool CRRenderStyle::GetLightingMaterial(LightingMaterial* pLightMaterial)
{
    if (!pLightMaterial) return false;
    *pLightMaterial = m_LightingMaterial;
    return true;
}

bool CRRenderStyle::AddRenderPass(RenderPassOp& RenderPass)
{
    if (m_RenderPasses.size() >= 4) return false;   // D3D asserts the same limit
    m_RenderPasses.push_back(RenderPass);
    return true;
}

bool CRRenderStyle::RemoveRenderPass(uint32 iPass)
{
    if (iPass >= m_RenderPasses.size()) return false;
    m_RenderPasses.erase(m_RenderPasses.begin() + iPass);
    return true;
}

bool CRRenderStyle::SetRenderPass(uint32 iPass, RenderPassOp& RenderPass)
{
    if (iPass >= m_RenderPasses.size()) return false;
    m_RenderPasses[iPass] = RenderPass;
    return true;
}

bool CRRenderStyle::GetRenderPass(uint32 iPass, RenderPassOp* pRenderPass)
{
    if (!pRenderPass || iPass >= m_RenderPasses.size()) return false;
    *pRenderPass = m_RenderPasses[iPass];
    return true;
}

uint32 CRRenderStyle::GetRenderPassCount()
{
    return (uint32)m_RenderPasses.size();
}

bool CRRenderStyle::CopyRenderStyle(CRenderStyle* pSrcRenderStyle)
{
    if (!pSrcRenderStyle) return false;

    LightingMaterial mat;
    if (pSrcRenderStyle->GetLightingMaterial(&mat))
        SetLightingMaterial(mat);

    m_RenderPasses.clear();
    uint32 nPasses = pSrcRenderStyle->GetRenderPassCount();
    for (uint32 i = 0; i < nPasses; ++i)
    {
        RenderPassOp pass;
        if (pSrcRenderStyle->GetRenderPass(i, &pass))
            AddRenderPass(pass);
    }
    return true;
}

// The state an object gets when it has no style: opaque, no alpha test,
// z read+write. Kept identical to CD3DRenderStyle::SetDefaults().
void CRRenderStyle::SetDefaults()
{
    memset(&m_LightingMaterial, 0, sizeof(m_LightingMaterial));
    m_LightingMaterial.Ambient       = FourFloatColor(1.0f, 1.0f, 1.0f, 1.0f);
    m_LightingMaterial.Diffuse       = FourFloatColor(0.8f, 0.8f, 0.8f, 1.0f);
    m_LightingMaterial.Emissive      = FourFloatColor(0.0f, 0.0f, 0.0f, 1.0f);
    m_LightingMaterial.Specular      = FourFloatColor(0.0f, 0.0f, 0.0f, 1.0f);
    m_LightingMaterial.SpecularPower = 20.0f;

    m_RenderPasses.clear();

    RenderPassOp pass;
    memset(&pass, 0, sizeof(pass));
    pass.BlendMode       = RENDERSTYLE_NOBLEND;
    pass.ZBufferMode     = RENDERSTYLE_ZRW;
    pass.CullMode        = RENDERSTYLE_CULL_CCW;
    pass.AlphaTestMode   = RENDERSTYLE_NOALPHATEST;
    pass.ZBufferTestMode = RENDERSTYLE_ALPHATEST_LESSEQUAL;
    pass.FillMode        = RENDERSTYLE_FILL;
    pass.TextureFactor   = 0x80808080;
    pass.AlphaRef        = 128;
    pass.DynamicLight    = true;
    m_RenderPasses.push_back(pass);
}

// Stream in the ltb file.
//
// ⚠️ Byte-for-byte the same read sequence as CD3DRenderStyle::Load_LTBData.
// The D3D-only fields (per-pass vertex/pixel shader ids, RSD3DOptions) are
// READ AND DISCARDED rather than skipped -- they are physically present in the
// stream, so not consuming them would desync every subsequent pass. Same
// discipline as the world render-data reader (§4: "EVERY level of recursion
// reads its own trailing worldModelCount").
bool CRRenderStyle::Load_LTBData(ILTStream* pFileStream)
{
    if (!pFileStream) return false;

    LTB_Header Header;
    uint32 iTotalSize, iSize, iRenStyleCnt, iRenderPasses;

    pFileStream->Read(&Header, sizeof(Header));
    if (Header.m_iFileType != LTB_D3D_RENDERSTYLE_FILE) return false;
    if (Header.m_iVersion  != R_RENDERSTYLE_VERSION)   return false;

    pFileStream->Read(&iTotalSize, sizeof(iTotalSize));
    if (iTotalSize > 1000000) return false;             // sanity, as D3D

    pFileStream->Read(&iRenStyleCnt, sizeof(iRenStyleCnt));
    if (iRenStyleCnt > 32) return false;                // sanity, as D3D

    for (uint32 iStyle = 0; iStyle < iRenStyleCnt; ++iStyle)
    {
        pFileStream->Read(&iSize, sizeof(iSize));
        pFileStream->Read(&m_LightingMaterial, sizeof(m_LightingMaterial));

        pFileStream->Read(&iRenderPasses, sizeof(iRenderPasses));
        if (iRenderPasses > 4) return false;

        m_RenderPasses.clear();

        for (uint32 iPass = 0; iPass < iRenderPasses; ++iPass)
        {
            RenderPassOp    RenderPass;
            uint8           iHasRSD3DRP;
            RSD3DRenderPass D3DRenderPass;

            pFileStream->Read(&RenderPass, sizeof(RenderPass));
            pFileStream->Read(&iHasRSD3DRP, sizeof(iHasRSD3DRP));

            if (!AddRenderPass(RenderPass))
                return false;

            if (iHasRSD3DRP)
            {
                // Consumed to keep the stream aligned; we have no shaders.
                pFileStream->Read(&D3DRenderPass.bUseVertexShader, sizeof(D3DRenderPass.bUseVertexShader));
                pFileStream->Read(&D3DRenderPass.VertexShaderID,   sizeof(D3DRenderPass.VertexShaderID));
                pFileStream->Read(&D3DRenderPass.bUsePixelShader,  sizeof(D3DRenderPass.bUsePixelShader));
                pFileStream->Read(&D3DRenderPass.PixelShaderID,    sizeof(D3DRenderPass.PixelShaderID));
            }
        }

        // Trailing RSD3DOptions. D3D tolerates this read failing (older files),
        // and so do we -- it is the last thing in the chunk.
        RSD3DOptions rsD3DOptions;
        pFileStream->Read(&rsD3DOptions, sizeof(rsD3DOptions));

        // D3D loops until it finds a style the device supports; fixed-function
        // GL supports every state we honour, so the first one always wins.
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------
// Enum -> GL translation
// --------------------------------------------------------------------------

static uint32 rrs_CompareFunc(ERenStyle_TestMode eMode)
{
    switch (eMode)
    {
        case RENDERSTYLE_ALPHATEST_LESS:            return kRAlpha_Less;
        case RENDERSTYLE_ALPHATEST_LESSEQUAL:       return kRAlpha_LEqual;
        case RENDERSTYLE_ALPHATEST_GREATER:         return kRAlpha_Greater;
        case RENDERSTYLE_ALPHATEST_GREATEREQUAL:    return kRAlpha_GEqual;
        case RENDERSTYLE_ALPHATEST_EQUAL:           return kRAlpha_Equal;
        case RENDERSTYLE_ALPHATEST_NOTEQUAL:        return kRAlpha_NotEqual;
        case RENDERSTYLE_NOALPHATEST:
        default:                                    return kRAlpha_Always;
    }
}

// src/dst factors straight out of renderstylelookuptables.cpp.
static void rrs_BlendFactors(ERenStyle_BlendMode eMode, uint32* pSrc, uint32* pDst)
{
    switch (eMode)
    {
        case RENDERSTYLE_BLEND_ADD:                 *pSrc = kRBlend_One;                 *pDst = kRBlend_One;                 break;
        case RENDERSTYLE_BLEND_SATURATE:            *pSrc = kRBlend_InvDstColor; *pDst = kRBlend_One;                 break;
        case RENDERSTYLE_BLEND_MOD_SRCALPHA:        *pSrc = kRBlend_SrcAlpha;           *pDst = kRBlend_InvSrcAlpha; break;
        case RENDERSTYLE_BLEND_MOD_SRCCOLOR:        *pSrc = kRBlend_SrcColor;           *pDst = kRBlend_InvSrcColor; break;
        case RENDERSTYLE_BLEND_MOD_DSTCOLOR:        *pSrc = kRBlend_DstColor;           *pDst = kRBlend_InvDstColor; break;
        case RENDERSTYLE_BLEND_MUL_SRCCOL_DSTCOL:   *pSrc = kRBlend_SrcColor;           *pDst = kRBlend_DstColor;           break;
        case RENDERSTYLE_BLEND_MUL_SRCCOL_ONE:      *pSrc = kRBlend_SrcColor;           *pDst = kRBlend_One;                 break;
        case RENDERSTYLE_BLEND_MUL_SRCALPHA_ZERO:   *pSrc = kRBlend_SrcAlpha;           *pDst = kRBlend_Zero;                break;
        case RENDERSTYLE_BLEND_MUL_SRCALPHA_ONE:    *pSrc = kRBlend_SrcAlpha;           *pDst = kRBlend_One;                 break;
        case RENDERSTYLE_BLEND_MUL_DSTCOL_ZERO:     *pSrc = kRBlend_DstColor;           *pDst = kRBlend_Zero;                break;
        case RENDERSTYLE_NOBLEND:
        default:                                    *pSrc = kRBlend_One;                 *pDst = kRBlend_Zero;                break;
    }
}

void RRenderStyle_GetDefaultState(RRenderStyleState* pOut)
{
    if (!pOut) return;
    pOut->bAlphaTest = false;
    pOut->nAlphaFunc = kRAlpha_Always;
    pOut->fAlphaRef  = 0.0f;
    pOut->bBlend     = false;
    pOut->nSrcBlend  = kRBlend_One;
    pOut->nDstBlend  = kRBlend_Zero;
    pOut->bZRead     = true;
    pOut->bZWrite    = true;
    // The world and model draws have always run with culling OFF (the D3D-era
    // winding does not survive our transform chain -- see §15a). Keep that as
    // the default so enabling render styles cannot silently change which faces
    // are drawn; the style's CullMode is resolved below but callers may ignore it.
    pOut->bCull      = false;
    pOut->bIgnoreDiffuse = false;
    pOut->fColorScale    = 1.0f;
    // Styleless objects keep the historic behaviour: texture alpha reaches the
    // output. (With bAlphaTest false and bBlend false above, nothing consumes
    // it anyway — but a caller that enables blending itself still gets what it
    // always got.)
    pOut->bTextureAlpha  = true;
}

// Resolve TextureStages[0]'s ColorOp/ColorArg pair into "does the diffuse
// participate?" and "is there a 2x/4x scale?". Authority: the D3D stage setup
// in d3d_renderstatemgr.cpp, which feeds these straight to
// D3DTSS_COLOROP/COLORARG1/COLORARG2.
static void rrs_ResolveColorOp(const RenderPassOp& pass, RRenderStyleState* pOut)
{
    const TextureStageOps& ts = pass.TextureStages[0];

    switch (ts.ColorOp)
    {
        case RENDERSTYLE_COLOROP_SELECTARG1:
            // Only Arg1 contributes. If that is the texture, the vertex colour
            // must not touch it -- the authored meaning of "NODIFFUSE".
            pOut->bIgnoreDiffuse = (ts.ColorArg1 == RENDERSTYLE_COLORARG_TEXTURE);
            break;

        case RENDERSTYLE_COLOROP_SELECTARG2:
            pOut->bIgnoreDiffuse = (ts.ColorArg2 == RENDERSTYLE_COLORARG_TEXTURE);
            break;

        case RENDERSTYLE_COLOROP_MODULATE2X:
            pOut->fColorScale = 2.0f;
            break;

        case RENDERSTYLE_COLOROP_MODULATE:
        default:
            break;   // texture * diffuse, which is the GL default
    }

    // ★ The ALPHA op is authored independently of the colour op — resolve it
    // separately. See the comment on RRenderStyleState::bTextureAlpha.
    switch (ts.AlphaOp)
    {
        case RENDERSTYLE_ALPHAOP_DISABLE:
            // The stage contributes no alpha; what reaches the blend is the
            // diffuse alpha that was already there.
            pOut->bTextureAlpha = false;
            break;

        case RENDERSTYLE_ALPHAOP_SELECTARG1:
            pOut->bTextureAlpha = (ts.AlphaArg1 == RENDERSTYLE_ALPHAARG_TEXTURE);
            break;

        case RENDERSTYLE_ALPHAOP_SELECTARG2:
            pOut->bTextureAlpha = (ts.AlphaArg2 == RENDERSTYLE_ALPHAARG_TEXTURE);
            break;

        default:
            // MODULATE and the arithmetic ops combine both args, so the texture
            // participates if either argument names it.
            pOut->bTextureAlpha = (ts.AlphaArg1 == RENDERSTYLE_ALPHAARG_TEXTURE) ||
                                  (ts.AlphaArg2 == RENDERSTYLE_ALPHAARG_TEXTURE);
            break;
    }
}

// ⚠️ MODULATE2X IS GATED OFF BY DEFAULT, DELIBERATELY.
// RS\DEFAULT.LTB authors MODULATE2X, i.e. texture * DIFFUSE * 2 -- but our
// diffuse is not the engine's: it is the invented key light in rm_EmitMesh
// (PHASE2_HANDOFF §25/§34a), because model lighting was never ported. Doubling
// an invented value is not D3D parity, it is compounding a stand-in, and it
// would blow out every model using the default style. Turn this on together
// with real model lighting, not before. `LT_RS_COLORSCALE=1` enables it for A/B.
static bool rrs_ColorScaleEnabled(void)
{
    static int s_nOn = -1;
    if (s_nOn < 0)
        s_nOn = getenv("LT_RS_COLORSCALE") ? 1 : 0;
    return s_nOn != 0;
}

bool RRenderStyle_ColorScaleEnabled(void)
{
    return rrs_ColorScaleEnabled();
}

bool RRenderStyle_GetState(CRenderStyle* pStyle, uint32 nPass, RRenderStyleState* pOut)
{
    if (!pOut) return false;
    RRenderStyle_GetDefaultState(pOut);

    if (!pStyle) return false;

    RenderPassOp pass;
    if (!pStyle->GetRenderPass(nPass, &pass)) return false;

    pOut->bAlphaTest = (pass.AlphaTestMode != RENDERSTYLE_NOALPHATEST);
    pOut->nAlphaFunc = rrs_CompareFunc(pass.AlphaTestMode);
    // D3D's D3DRS_ALPHAREF is 0..255; glAlphaFunc takes 0..1.
    pOut->fAlphaRef  = (float)(pass.AlphaRef & 0xFF) / 255.0f;

    pOut->bBlend = (pass.BlendMode != RENDERSTYLE_NOBLEND);
    uint32 nSrc, nDst;
    rrs_BlendFactors(pass.BlendMode, &nSrc, &nDst);
    pOut->nSrcBlend = nSrc;
    pOut->nDstBlend = nDst;

    pOut->bZRead  = (pass.ZBufferMode != RENDERSTYLE_NOZ);
    pOut->bZWrite = (pass.ZBufferMode == RENDERSTYLE_ZRW);

    rrs_ResolveColorOp(pass, pOut);

    return true;
}

// --------------------------------------------------------------------------
// ILTRenderStyles -- the engine-facing factory.
//
// Registering this is what makes setupobject.cpp's ModelSetRenderStyle stop
// bailing at `if (!renderstyles) return LT_ERROR`, which is why model render
// styles were NULL for the whole port up to now.
// --------------------------------------------------------------------------

class CRRenderStyleInterface : public ILTRenderStyles
{
public:
    declare_interface(CRRenderStyleInterface);

    virtual CRenderStyle* DuplicateRenderStyle(CRenderStyle* pRendStyle);
    virtual CRenderStyle* CreateRenderStyle(bool bSetToDefault = true);
    virtual CRenderStyle* LoadRenderStyle(const char* szFilename);
    virtual void          FreeRenderStyle(CRenderStyle* pRendStyle);
};

define_interface(CRRenderStyleInterface, ILTRenderStyles);

CRenderStyle* CRRenderStyleInterface::CreateRenderStyle(bool bSetToDefault)
{
    CRRenderStyle* pStyle = new CRRenderStyle;
    if (pStyle && bSetToDefault)
        pStyle->SetDefaults();
    return pStyle;
}

CRenderStyle* CRRenderStyleInterface::DuplicateRenderStyle(CRenderStyle* pRendStyle)
{
    CRRenderStyle* pNew = new CRRenderStyle;
    if (!pNew) return NULL;
    if (!pNew->CopyRenderStyle(pRendStyle)) { delete pNew; return NULL; }
    return pNew;
}

CRenderStyle* CRRenderStyleInterface::LoadRenderStyle(const char* szFilename)
{
    if (!szFilename || !szFilename[0] || !gls_client_file_mgr) return NULL;

    FileRef ref;
    ref.m_FileType  = TYPECODE_RSTYLE;
    ref.m_pFilename = szFilename;

    FileIdentifier* pIdent = gls_client_file_mgr->GetFileIdentifier(&ref, TYPECODE_RSTYLE);
    if (!pIdent)
    {
        if (rrs_Trace())
            fprintf(stderr, "[rs] '%s' NOT FOUND\n", szFilename);
        return NULL;
    }

    // Already loaded? The cache lives on the file identifier, exactly as D3D
    // does it, so every object naming the same style shares one instance.
    if (pIdent->m_pData)
    {
        CRRenderStyle* pStyle = (CRRenderStyle*)pIdent->m_pData;
        pStyle->IncRefCount();
        return pStyle;
    }

    ILTStream* pFileStream = gls_client_file_mgr->OpenFile(&ref);
    if (!pFileStream)
    {
        if (rrs_Trace())
            fprintf(stderr, "[rs] '%s' could not be opened\n", szFilename);
        return NULL;
    }

    CRRenderStyle* pStyle = new CRRenderStyle;
    if (!pStyle) { pFileStream->Release(); return NULL; }

    if (!pStyle->Load_LTBData(pFileStream))
    {
        if (rrs_Trace())
            fprintf(stderr, "[rs] '%s' FAILED to parse\n", szFilename);
        delete pStyle;
        pFileStream->Release();
        return NULL;
    }
    pFileStream->Release();

    pStyle->SetFilename(szFilename);
    pIdent->m_pData = pStyle;
    pStyle->IncRefCount();      // the cache's reference

    if (rrs_Trace())
    {
        uint32 nPasses = pStyle->GetRenderPassCount();
        fprintf(stderr, "[rs] loaded '%s': %u pass(es)\n", szFilename, nPasses);
        for (uint32 i = 0; i < nPasses; ++i)
        {
            RenderPassOp pass;
            if (!pStyle->GetRenderPass(i, &pass)) continue;
            RRenderStyleState st;
            RRenderStyle_GetState(pStyle, i, &st);
            fprintf(stderr, "[rs]   pass %u: alphaTest=%d mode=%d ref=%u (%.3f) "
                            "blend=%d mode=%d z=%d cull=%d fill=%d\n",
                    i, (int)st.bAlphaTest, (int)pass.AlphaTestMode, pass.AlphaRef, st.fAlphaRef,
                    (int)st.bBlend, (int)pass.BlendMode, (int)pass.ZBufferMode,
                    (int)pass.CullMode, (int)pass.FillMode);
            // ★ The TEXTURE STAGE ops are the authored colour pipeline -- this
            // is where a "NODIFFUSE" style actually says so (ColorArg = TEXTURE
            // rather than a MODULATE against DIFFUSE). Read the data; do not
            // infer the behaviour from the filename.
            for (uint32 s = 0; s < 4; ++s)
            {
                const TextureStageOps &ts = pass.TextureStages[s];
                if (s > 0 && ts.ColorOp == RENDERSTYLE_COLOROP_DISABLE)
                    continue;
                fprintf(stderr, "[rs]     stage %u: colorOp=%d arg1=%d arg2=%d | "
                                "alphaOp=%d aArg1=%d aArg2=%d | texParam=%d uvSource=%d\n",
                        s, (int)ts.ColorOp, (int)ts.ColorArg1, (int)ts.ColorArg2,
                        (int)ts.AlphaOp, (int)ts.AlphaArg1, (int)ts.AlphaArg2,
                        (int)ts.TextureParam, (int)ts.UVSource);
            }
        }
    }

    pStyle->IncRefCount();      // the caller's reference
    return pStyle;
}

void CRRenderStyleInterface::FreeRenderStyle(CRenderStyle* pRendStyle)
{
    if (!pRendStyle) return;
    pRendStyle->DecRefCount();
    // Deliberately NOT deleted at zero: the file-identifier cache above holds a
    // reference for the lifetime of the resource tree, mirroring D3D ("all
    // loaded render styles won't be freed until all files are flushed").
}
