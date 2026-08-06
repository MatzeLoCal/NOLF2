// ----------------------------------------------------------------------- //
//
// MODULE  : gl_renderstyle.h
//
// PURPOSE : Render styles for the GL renderer.
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

#ifndef __GL_RENDERSTYLE_H__
#define __GL_RENDERSTYLE_H__

#include "ltrenderstyle.h"

#include <vector>

// Version of the render-style LTB chunk this parser understands. Must match
// d3d_renderstyle.h's RENDERSTYLE_D3D_VERSION -- the files are shared.
#define GL_RENDERSTYLE_VERSION      3

class CGLRenderStyle : public CRenderStyle
{
public:
    CGLRenderStyle();
    virtual ~CGLRenderStyle() {}

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
    virtual bool    IsSupportedOnDevice()   { return true; }   // fixed-function GL: everything we honour is supported

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
// styles are an effects feature we do not implement yet (see GLRenderStyle_
// GetPassCount if you need to know).
struct GLRenderStyleState
{
    bool    bAlphaTest;      // enable GL_ALPHA_TEST
    uint32  nAlphaFunc;      // GLenum for glAlphaFunc
    float   fAlphaRef;       // 0..1 reference for glAlphaFunc
    bool    bBlend;          // enable GL_BLEND
    uint32  nSrcBlend;       // GLenum
    uint32  nDstBlend;       // GLenum
    bool    bZRead;
    bool    bZWrite;
    bool    bCull;           // enable GL_CULL_FACE
    uint32  nCullFace;       // GLenum (GL_BACK / GL_FRONT)

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

// Apply pOut's colour pipeline to the active texture unit's texture environment,
// and restore the plain GL_MODULATE default afterwards.
void GLRenderStyle_ApplyColorOp(const GLRenderStyleState* pState);
void GLRenderStyle_ResetColorOp(void);

// Resolve pass `nPass` of `pStyle` into GL state. Returns false (and leaves
// pOut at the fixed-function defaults) when the style is NULL or the pass does
// not exist, so callers can use one code path.
bool GLRenderStyle_GetState(CRenderStyle* pStyle, uint32 nPass, GLRenderStyleState* pOut);

// Fill pOut with the state used when an object has no render style at all.
// This is CD3DRenderStyle::SetDefaults(): opaque, no alpha test, z read+write.
void GLRenderStyle_GetDefaultState(GLRenderStyleState* pOut);

#endif // __GL_RENDERSTYLE_H__
