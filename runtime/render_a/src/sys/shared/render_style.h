// ----------------------------------------------------------------------- //
//
// MODULE  : render_style.h
//
// PURPOSE : Authored render styles (backend-neutral).
//
//           A RENDER STYLE IS THE AUTHORED RENDER STATE FOR A MODEL PIECE --
//           blend mode, alpha test mode + reference, z read/write, cull and
//           fill -- shipped as an `RS\*.ltb` file and named from the game's
//           attribute files (PropTypes.txt `RenderStyle0`, ModelButes.txt
//           `RenderStyle`, Weapons.txt `PVRenderStyle0`, ...).
//
//           This is the model-path equivalent of the world path's authored
//           `AlphaRef` (PHASE2_HANDOFF §14): rendering decisions come from
//           authored data, never from texture content. Without it a leaf card
//           whose skin carries no AlphaRef has nothing telling the renderer it
//           is a cutout, and the transparent region draws as the artist's flat
//           fill colour -- the solid red/green tree canopies and the opaque
//           bicycle wheels.
//
//           Mirrors runtime/render_a/src/sys/d3d/d3d_renderstyle.{h,cpp};
//           the LTB parsing is deliberately kept byte-identical to
//           CD3DRenderStyle::Load_LTBData, including reading the D3D-only
//           fields we then discard -- they are IN the stream, so skipping them
//           would desync it.
//
// ----------------------------------------------------------------------- //

#ifndef __RENDER_STYLE_H__
#define __RENDER_STYLE_H__

#include "sys/shared/render_state.h"   // kRBlend_* / kRAlpha_* (was GLenum)
#include "ltrenderstyle.h"

#include <vector>

// Version of the render-style LTB chunk this parser understands. Must match
// d3d_renderstyle.h's RENDERSTYLE_D3D_VERSION -- the files are shared.
#define R_RENDERSTYLE_VERSION      3

class CRRenderStyle : public CRenderStyle
{
public:
    CRRenderStyle();
    virtual ~CRRenderStyle() {}

    // Lighting material
    virtual bool    SetLightingMaterial(LightingMaterial& LightMaterial);
    virtual bool    GetLightingMaterial(LightingMaterial* pLightMaterial);

    // Render passes
    virtual bool    AddRenderPass(RenderPassOp& RenderPass);
    virtual bool    RemoveRenderPass(uint32 iPass);
    virtual bool    SetRenderPass(uint32 iPass, RenderPassOp& RenderPass);
    virtual bool    GetRenderPass(uint32 iPass, RenderPassOp* pRenderPass);
    virtual uint32  GetRenderPassCount();

    virtual bool    Load_LTBData(ILTStream* pFileStream);
    virtual bool    Compile()               { return true; }   // nothing to precompute
    virtual void    SetDefaults();
    virtual bool    CopyRenderStyle(CRenderStyle* pSrcRenderStyle);
    virtual bool    IsSupportedOnDevice()   { return true; }   // everything we honour is supported

    // NOTE: SetFilename/GetFilename come from CRenderStyle -- m_pFilename is
    // private in the base, so do not shadow them here.

private:
    LightingMaterial            m_LightingMaterial;
    std::vector<RenderPassOp>   m_RenderPasses;
};

// --- The GL side's consumption of a style ---------------------------------
//
// What a draw path actually needs out of a style, resolved once so callers do
// not each re-interpret the enums. Pass index is almost always 0: multi-pass
// styles are an effects feature we do not implement yet (see RRenderStyle_
// GetPassCount if you need to know).
struct RRenderStyleState
{
    bool    bAlphaTest;      // enable alpha testing
    uint32  nAlphaFunc;      // ERAlphaFunc (kRAlpha_*)
    float   fAlphaRef;       // 0..1 alpha-test reference
    bool    bBlend;          // enable blending
    uint32  nSrcBlend;       // ERBlendFactor (kRBlend_*)
    uint32  nDstBlend;       // ERBlendFactor (kRBlend_*)
    bool    bZRead;
    bool    bZWrite;
    bool    bCull;           // enable backface culling

    // ★ THE AUTHORED COLOUR PIPELINE (RenderPassOp::TextureStages[0]).
    // D3D's ColorOp/ColorArg1/ColorArg2 say how the texture combines with the
    // DIFFUSE (vertex) colour, and it is NOT always a modulate:
    //   SELECTARG1 + arg1=TEXTURE  -> texture only, diffuse IGNORED
    //                                 (this is what "NODIFFUSE" styles author:
    //                                  RS\INTERFACE_BACK/FRONT.LTB and
    //                                  RS\TRANSLUCENT_NODIFFUSE.LTB)
    //   MODULATE                   -> texture * diffuse
    //   MODULATE2X                 -> texture * diffuse * 2 (RS\DEFAULT.LTB)
    // We used to hardcode GL_MODULATE, so every "ignore the diffuse" surface was
    // multiplied by our stand-in key light anyway.
    bool    bIgnoreDiffuse;  // colour comes from the texture alone (GL_REPLACE)
    float   fColorScale;     // 1.0, or 2.0 for the MODULATE2X ops

    // ★★ THE AUTHORED ALPHA PIPELINE — SEPARATE FROM THE COLOUR ONE.
    // TextureStages[0] carries AlphaOp/AlphaArg1/AlphaArg2 alongside the colour
    // ops, and they say something different: WHERE THE FRAGMENT'S ALPHA COMES
    // FROM. It is not always the texture:
    //   SELECTARG2 + arg2=DIFFUSE -> alpha is the OBJECT's (vertex) alpha; the
    //                                texture's alpha channel must NOT reach the
    //                                blend at all. RS\NinjaTranslucent.ltb and
    //                                RS\SpotlightFalloffSaturate.ltb author this.
    //   SELECTARG1 + arg1=TEXTURE -> texture alpha (RS\default, glass,
    //                                transparent, INTERFACE_BACK)
    //   MODULATE                  -> texture.a * diffuse.a (RS\FurShell/FurFins)
    //
    // ⚠️ WHY THIS MATTERS MORE THAN IT LOOKS: alpha in a NOLF2 skin is FREQUENTLY
    // AN ENVIRONMENT/SPECULAR MASK, NOT OPACITY — CateCasualHead.dtx has 76.9% of
    // its texels at alpha <= 128. Letting that reach a SRC_ALPHA blend does not
    // make a face translucent, it DELETES it. Only the authored alpha op says
    // what a skin's alpha channel means. (Same rule as §13/§14's AlphaRef: the
    // authored data decides, never the texture's content.)
    bool    bTextureAlpha;   // does TextureStages[0]'s ALPHA op read the TEXTURE?
};

// ⚠️ IS THE AUTHORED COLOUR SCALE (MODULATE2X) ACTUALLY IN EFFECT?
// It is PARKED behind LT_RS_COLORSCALE (§68/§71). Any backend applying
// fColorScale MUST ask this first, or it silently un-parks the feature and
// renders those pieces at 2x -- exactly what the Metal model pass did (§89):
// the world matched to 0.00 while characters came out up to +48/255 too bright.
// ⚠️ The gate's own precondition ("turn this on together with real model
// lighting") has been satisfied since §71; nobody has revisited it.
bool RRenderStyle_ColorScaleEnabled(void);

// Resolve pass `nPass` of `pStyle` into GL state. Returns false (and leaves
// pOut at the fixed-function defaults) when the style is NULL or the pass does
// not exist, so callers can use one code path.
bool RRenderStyle_GetState(CRenderStyle* pStyle, uint32 nPass, RRenderStyleState* pOut);

// Fill pOut with the state used when an object has no render style at all.
// This is CD3DRenderStyle::SetDefaults(): opaque, no alpha test, z read+write.
void RRenderStyle_GetDefaultState(RRenderStyleState* pOut);

#endif // __RENDER_STYLE_H__
