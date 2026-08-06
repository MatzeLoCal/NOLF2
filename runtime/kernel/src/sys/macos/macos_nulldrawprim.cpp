// ----------------------------------------------------------------------- //
//
// MODULE  : macos_nulldrawprim.cpp
//
// PURPOSE : No-op ILTDrawPrim for the macOS bring-up.
//
//           The real implementation (CD3DDrawPrim, render_b/.../d3ddrawprim.cpp)
//           is D3D-bound and excluded from the macOS build, but the engine
//           resolves TWO instances of this interface and calls them on the
//           frame path: "Internal" (CClientMgr::Render -> SetCamera) and
//           "Default" (console/UI 2D drawing). These no-ops keep both callers
//           alive until the Phase-2 GL DrawPrim exists (2D text/HUD).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "iltdrawprim.h"

class CNullDrawPrim : public ILTDrawPrim
{
public:
    declare_interface(CNullDrawPrim);

    virtual LTRESULT BeginDrawPrim() { return LT_OK; }
    virtual LTRESULT EndDrawPrim()   { return LT_OK; }

    virtual LTRESULT SetCamera(const HOBJECT hCamera)                 { return LT_OK; }
    virtual LTRESULT SetTexture(const HTEXTURE hTexture)              { return LT_OK; }
    virtual LTRESULT SetTransformType(const ELTTransformType eType)   { return LT_OK; }
    virtual LTRESULT SetColorOp(const ELTColorOp eColorOp)            { return LT_OK; }
    virtual LTRESULT SetAlphaBlendMode(const ELTBlendMode eBlendMode) { return LT_OK; }
    virtual LTRESULT SetZBufferMode(const ELTZBufferMode eZBufferMode){ return LT_OK; }
    virtual LTRESULT SetAlphaTestMode(const ELTTestMode eTestMode)    { return LT_OK; }
    virtual LTRESULT SetClipMode(const ELTClipMode eClipMode)        { return LT_OK; }
    virtual LTRESULT SetFillMode(ELTDPFillMode efillmode)            { return LT_OK; }
    virtual LTRESULT SetCullMode(ELTDPCullMode ecullmode)            { return LT_OK; }
    virtual LTRESULT SetFogEnable(bool bFogEnable)                   { return LT_OK; }
    virtual LTRESULT SetReallyClose(bool bReallyClose)               { return LT_OK; }
    virtual LTRESULT SetEffectShaderID(uint32 nEffectShaderID)       { return LT_OK; }
    virtual void SaveViewport()    {}
    virtual void RestoreViewport() {}

    virtual LTRESULT DrawPrim(LT_POLYGT3 *pPrim, const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYFT3 *pPrim, const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYG3 *pPrim,  const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYF3 *pPrim,  const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYGT4 *pPrim, const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYGT4 **ppPrim, const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYFT4 *pPrim, const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYG4 *pPrim,  const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_POLYF4 *pPrim,  const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_LINEGT *pPrim,  const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_LINEFT *pPrim,  const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_LINEG *pPrim,   const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrim(LT_LINEF *pPrim,   const uint32 nCount = 1) { return LT_OK; }

    virtual LTRESULT DrawPrimPoint(LT_VERTGT *pVerts, const uint32 nCount = 1) { return LT_OK; }
    virtual LTRESULT DrawPrimPoint(LT_VERTG *pVerts,  const uint32 nCount = 1) { return LT_OK; }

    virtual LTRESULT DrawPrimFan(LT_VERTGT *pVerts, const uint32 nCount) { return LT_OK; }
    virtual LTRESULT DrawPrimFan(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba) { return LT_OK; }
    virtual LTRESULT DrawPrimFan(LT_VERTG *pVerts,  const uint32 nCount) { return LT_OK; }
    virtual LTRESULT DrawPrimFan(LT_VERTF *pVerts,  const uint32 nCount, LT_VERTRGBA rgba) { return LT_OK; }

    virtual LTRESULT DrawPrimStrip(LT_VERTGT *pVerts, const uint32 nCount) { return LT_OK; }
    virtual LTRESULT DrawPrimStrip(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba) { return LT_OK; }
    virtual LTRESULT DrawPrimStrip(LT_VERTG *pVerts,  const uint32 nCount) { return LT_OK; }
    virtual LTRESULT DrawPrimStrip(LT_VERTF *pVerts,  const uint32 nCount, LT_VERTRGBA rgba) { return LT_OK; }

    virtual void SetXY4(LT_POLYGT4 *pPrim, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {}
    virtual void SetXY4(LT_POLYFT4 *pPrim, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {}
    virtual void SetXY4(LT_POLYG4 *pPrim,  float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {}
    virtual void SetXY4(LT_POLYF4 *pPrim,  float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {}

    virtual void SetXYWH(LT_POLYGT4 *pPrim, float x, float y, float w, float h) {}
    virtual void SetXYWH(LT_POLYFT4 *pPrim, float x, float y, float w, float h) {}
    virtual void SetXYWH(LT_POLYG4 *pPrim,  float x, float y, float w, float h) {}
    virtual void SetXYWH(LT_POLYF4 *pPrim,  float x, float y, float w, float h) {}

    virtual void SetUV4(LT_POLYGT4 *pPrim, float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3) {}
    virtual void SetUV4(LT_POLYFT4 *pPrim, float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3) {}

    virtual void SetUVWH(LT_POLYGT4 *pPrim, float u, float v, float w, float h) {}
    virtual void SetUVWH(LT_POLYGT4 *pPrim, HTEXTURE pTex, float u, float v, float w, float h) {}
    virtual void SetUVWH(LT_POLYFT4 *pPrim, float u, float v, float w, float h) {}

    virtual void SetRGB(LT_POLYGT4 *pPrim, uint32 color) {}
    virtual void SetRGB(LT_POLYFT4 *pPrim, uint32 color) {}
    virtual void SetRGB(LT_POLYG4 *pPrim,  uint32 color) {}
    virtual void SetRGB(LT_POLYF4 *pPrim,  uint32 color) {}

    virtual void SetRGBA(LT_POLYGT4 *pPrim, uint32 color) {}
    virtual void SetRGBA(LT_POLYFT4 *pPrim, uint32 color) {}
    virtual void SetRGBA(LT_POLYG4 *pPrim,  uint32 color) {}
    virtual void SetRGBA(LT_POLYF4 *pPrim,  uint32 color) {}

    virtual void SetRGB4(LT_POLYGT4 *pPrim, uint32 color0, uint32 color1, uint32 color2, uint32 color3) {}
    virtual void SetRGB4(LT_POLYG4 *pPrim,  uint32 color0, uint32 color1, uint32 color2, uint32 color3) {}

    virtual void SetRGBA4(LT_POLYGT4 *pPrim, uint32 color0, uint32 color1, uint32 color2, uint32 color3) {}
    virtual void SetRGBA4(LT_POLYG4 *pPrim,  uint32 color0, uint32 color1, uint32 color2, uint32 color3) {}

    virtual void SetALPHA(LT_POLYGT4 *pPrim, uint8 alpha) {}
    virtual void SetALPHA(LT_POLYG4 *pPrim,  uint8 alpha) {}
    virtual void SetALPHA(LT_POLYFT4 *pPrim, uint8 alpha) {}
    virtual void SetALPHA(LT_POLYF4 *pPrim,  uint8 alpha) {}

    virtual void SetALPHA4(LT_POLYGT4 *pPrim, uint8 alpha0, uint8 alpha1, uint8 alpha2, uint8 alpha3) {}
    virtual void SetALPHA4(LT_POLYG4 *pPrim,  uint8 alpha0, uint8 alpha1, uint8 alpha2, uint8 alpha3) {}
};

// The engine resolves both the plain holder (console/UI) and the "Internal"
// instance (CClientMgr::Render).
define_interface(CNullDrawPrim, ILTDrawPrim);
instantiate_interface(CNullDrawPrim, ILTDrawPrim, Internal);
