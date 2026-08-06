// Stub of <d3d9.h> for the macOS build.
//
// The Direct3D 9 renderer (runtime/render_a/.../d3d) is NOT compiled on macOS — a
// null renderer is used instead.  However a handful of *client* and shared
// headers (renderstruct.h, d3d_device.h, d3d_device_wrapper.h, d3d_utils.h) name
// the D3D9 interfaces and value types in pointer members and inline forwarding
// wrappers, so they must be declared completely enough to *compile*.  None of
// these methods are ever invoked: the engine only ever holds a NULL device on
// macOS, and the GL renderer (Phase 2) will replace this surface entirely.
//
// IDirect3DDevice9 is therefore a complete no-op interface (every method inline,
// returning D3D_OK / 0).  The other IDirect3D*9 interfaces are only ever used as
// opaque pointers, so a forward declaration suffices for them.
#ifndef __LT_COMPAT_D3D9_H__
#define __LT_COMPAT_D3D9_H__

#include <windows.h>
#include <unknwn.h>      // REFIID, GUID
#include <d3d9types.h>

// --- Opaque interfaces (only ever used as pointers) -------------------------
struct IDirect3D9;
struct IDirect3DDevice9;        // completed below
struct IDirect3DBaseTexture9;
struct IDirect3DTexture9;
struct IDirect3DCubeTexture9;
struct IDirect3DVolumeTexture9;
struct IDirect3DSurface9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct IDirect3DSwapChain9;
struct IDirect3DQuery9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DStateBlock9;
struct IDirect3DVertexDeclaration9;

typedef struct IDirect3D9*             LPDIRECT3D9;
typedef struct IDirect3DDevice9*       LPDIRECT3DDEVICE9;
typedef struct IDirect3DTexture9*      LPDIRECT3DTEXTURE9;
typedef struct IDirect3DBaseTexture9*  LPDIRECT3DBASETEXTURE9;
typedef struct IDirect3DSurface9*      LPDIRECT3DSURFACE9;
typedef struct IDirect3DVertexBuffer9* LPDIRECT3DVERTEXBUFFER9;
typedef struct IDirect3DIndexBuffer9*  LPDIRECT3DINDEXBUFFER9;
typedef struct IDirect3DDevice9*       PD3DDEVICE;        // engine shorthand
typedef struct IDirect3DTexture9*      PD3DTEXTURE;
typedef struct IDirect3DSurface9*      PD3DSURFACE;

// --- Flexible Vertex Format flags (referenced by d3d_utils.h vertex sizing) -
#define D3DFVF_XYZ              0x002
#define D3DFVF_XYZRHW          0x004
#define D3DFVF_XYZB1           0x006
#define D3DFVF_XYZB2           0x008
#define D3DFVF_XYZB3           0x00a
#define D3DFVF_XYZB4           0x00c
#define D3DFVF_XYZB5           0x00e
#define D3DFVF_NORMAL          0x010
#define D3DFVF_PSIZE           0x020
#define D3DFVF_DIFFUSE         0x040
#define D3DFVF_SPECULAR        0x080
#define D3DFVF_TEX0            0x000
#define D3DFVF_TEX1            0x100
#define D3DFVF_TEX2            0x200
#define D3DFVF_TEX3            0x300
#define D3DFVF_TEX4            0x400
#define D3DFVF_TEX5            0x500
#define D3DFVF_TEX6            0x600
#define D3DFVF_TEX7            0x700
#define D3DFVF_TEX8            0x800
#define D3DFVF_TEXCOUNT_MASK   0xf00
#define D3DFVF_TEXCOUNT_SHIFT  8

// Clear() flags
#ifndef D3DCLEAR_TARGET
#define D3DCLEAR_TARGET   0x1
#define D3DCLEAR_ZBUFFER  0x2
#define D3DCLEAR_STENCIL  0x4
#endif

// ---------------------------------------------------------------------------
// Complete (no-op) IDirect3DDevice9.
// Signatures mirror the real D3D9 device so that d3d_device_wrapper.h's inline
// forwarders type-check; bodies do nothing and return success.
// ---------------------------------------------------------------------------
struct IDirect3DDevice9
{
    /*** IUnknown ***/
    HRESULT QueryInterface(REFIID, void**)                  { return D3D_OK; }
    ULONG   AddRef()                                        { return 1; }
    ULONG   Release()                                       { return 0; }

    /*** IDirect3DDevice9 ***/
    HRESULT TestCooperativeLevel()                          { return D3D_OK; }
    UINT    GetAvailableTextureMem()                        { return 0; }
    HRESULT EvictManagedResources()                         { return D3D_OK; }
    HRESULT GetDirect3D(IDirect3D9**)                       { return D3D_OK; }
    HRESULT GetDeviceCaps(D3DCAPS9*)                        { return D3D_OK; }
    HRESULT GetDisplayMode(UINT, D3DDISPLAYMODE*)           { return D3D_OK; }
    HRESULT GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS*) { return D3D_OK; }
    HRESULT SetCursorProperties(UINT, UINT, IDirect3DSurface9*)   { return D3D_OK; }
    void    SetCursorPosition(int, int, DWORD)              {}
    BOOL    ShowCursor(BOOL)                                { return 0; }
    HRESULT CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**) { return D3D_OK; }
    HRESULT Reset(D3DPRESENT_PARAMETERS*)                   { return D3D_OK; }
    HRESULT Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) { return D3D_OK; }
    HRESULT GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**) { return D3D_OK; }
    HRESULT GetRasterStatus(UINT, D3DRASTER_STATUS*)        { return D3D_OK; }
    void    SetGammaRamp(UINT, DWORD, CONST D3DGAMMARAMP*)  {}
    void    GetGammaRamp(UINT, D3DGAMMARAMP*)               {}
    HRESULT CreateTexture(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateDepthStencilSurface(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateOffscreenPlainSurface(UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*) { return D3D_OK; }
    HRESULT CreateQuery(D3DQUERYTYPE, IDirect3DQuery9**)    { return D3D_OK; }
    HRESULT UpdateSurface(IDirect3DSurface9*, CONST RECT*, IDirect3DSurface9*, CONST POINT*) { return D3D_OK; }
    HRESULT UpdateTexture(IDirect3DBaseTexture9*, IDirect3DBaseTexture9*) { return D3D_OK; }
    HRESULT SetRenderTarget(DWORD, IDirect3DSurface9*)      { return D3D_OK; }
    HRESULT SetDepthStencilSurface(IDirect3DSurface9*)      { return D3D_OK; }
    HRESULT GetRenderTarget(DWORD, IDirect3DSurface9**)     { return D3D_OK; }
    HRESULT GetDepthStencilSurface(IDirect3DSurface9**)     { return D3D_OK; }
    HRESULT BeginScene()                                    { return D3D_OK; }
    HRESULT EndScene()                                      { return D3D_OK; }
    HRESULT Clear(DWORD, CONST D3DRECT*, DWORD, D3DCOLOR, float, DWORD) { return D3D_OK; }
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) { return D3D_OK; }
    HRESULT GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) { return D3D_OK; }
    HRESULT MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) { return D3D_OK; }
    HRESULT SetViewport(CONST D3DVIEWPORT9*)               { return D3D_OK; }
    HRESULT GetViewport(D3DVIEWPORT9*)                     { return D3D_OK; }
    HRESULT SetMaterial(CONST D3DMATERIAL9*)              { return D3D_OK; }
    HRESULT GetMaterial(D3DMATERIAL9*)                   { return D3D_OK; }
    HRESULT SetLight(DWORD, CONST D3DLIGHT9*)            { return D3D_OK; }
    HRESULT GetLight(DWORD, D3DLIGHT9*)                  { return D3D_OK; }
    HRESULT LightEnable(DWORD, BOOL)                     { return D3D_OK; }
    HRESULT GetLightEnable(DWORD, BOOL*)                 { return D3D_OK; }
    HRESULT SetClipPlane(DWORD, CONST float*)            { return D3D_OK; }
    HRESULT GetClipPlane(DWORD, float*)                  { return D3D_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE, DWORD)    { return D3D_OK; }
    HRESULT GetRenderState(D3DRENDERSTATETYPE, DWORD*)   { return D3D_OK; }
    HRESULT BeginStateBlock()                            { return D3D_OK; }
    HRESULT EndStateBlock(IDirect3DStateBlock9**)        { return D3D_OK; }
    HRESULT CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**) { return D3D_OK; }
    HRESULT SetClipStatus(CONST D3DCLIPSTATUS9*)         { return D3D_OK; }
    HRESULT GetClipStatus(D3DCLIPSTATUS9*)               { return D3D_OK; }
    HRESULT GetTexture(DWORD, IDirect3DBaseTexture9**)   { return D3D_OK; }
    HRESULT SetTexture(DWORD, IDirect3DBaseTexture9*)    { return D3D_OK; }
    HRESULT GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD*) { return D3D_OK; }
    HRESULT SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)  { return D3D_OK; }
    HRESULT ValidateDevice(DWORD*)                       { return D3D_OK; }
    HRESULT SetPaletteEntries(UINT, CONST PALETTEENTRY*) { return D3D_OK; }
    HRESULT GetPaletteEntries(UINT, PALETTEENTRY*)       { return D3D_OK; }
    HRESULT SetCurrentTexturePalette(UINT)               { return D3D_OK; }
    HRESULT GetCurrentTexturePalette(UINT*)              { return D3D_OK; }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT)  { return D3D_OK; }
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) { return D3D_OK; }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, CONST void*, UINT) { return D3D_OK; }
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT) { return D3D_OK; }
    HRESULT ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD) { return D3D_OK; }
    HRESULT CreateVertexDeclaration(CONST D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) { return D3D_OK; }
    HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9*)  { return D3D_OK; }
    HRESULT GetVertexDeclaration(IDirect3DVertexDeclaration9**) { return D3D_OK; }
    HRESULT CreateVertexShader(CONST DWORD*, IDirect3DVertexShader9**) { return D3D_OK; }
    HRESULT SetVertexShader(IDirect3DVertexShader9*)     { return D3D_OK; }
    HRESULT GetVertexShader(IDirect3DVertexShader9**)    { return D3D_OK; }
    HRESULT SetVertexShaderConstantB(UINT, CONST BOOL*, UINT)  { return D3D_OK; }
    HRESULT GetVertexShaderConstantB(UINT, BOOL*, UINT)        { return D3D_OK; }
    HRESULT SetVertexShaderConstantF(UINT, CONST float*, UINT) { return D3D_OK; }
    HRESULT GetVertexShaderConstantF(UINT, float*, UINT)       { return D3D_OK; }
    HRESULT SetVertexShaderConstantI(UINT, CONST int*, UINT)   { return D3D_OK; }
    HRESULT GetVertexShaderConstantI(UINT, int*, UINT)         { return D3D_OK; }
    HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer9*, UINT, UINT) { return D3D_OK; }
    HRESULT GetStreamSource(UINT, IDirect3DVertexBuffer9**, UINT*, UINT*) { return D3D_OK; }
    HRESULT SetIndices(IDirect3DIndexBuffer9*)           { return D3D_OK; }
    HRESULT GetIndices(IDirect3DIndexBuffer9**)          { return D3D_OK; }
    HRESULT CreatePixelShader(CONST DWORD*, IDirect3DPixelShader9**) { return D3D_OK; }
    HRESULT SetPixelShader(IDirect3DPixelShader9*)       { return D3D_OK; }
    HRESULT GetPixelShader(IDirect3DPixelShader9**)      { return D3D_OK; }
    HRESULT SetPixelShaderConstantB(UINT, CONST BOOL*, UINT)   { return D3D_OK; }
    HRESULT GetPixelShaderConstantB(UINT, BOOL*, UINT)         { return D3D_OK; }
    HRESULT SetPixelShaderConstantF(UINT, CONST float*, UINT)  { return D3D_OK; }
    HRESULT GetPixelShaderConstantF(UINT, float*, UINT)        { return D3D_OK; }
    HRESULT SetPixelShaderConstantI(UINT, CONST int*, UINT)    { return D3D_OK; }
    HRESULT GetPixelShaderConstantI(UINT, int*, UINT)          { return D3D_OK; }
    HRESULT SetFVF(DWORD)                                { return D3D_OK; }
    HRESULT GetFVF(DWORD*)                               { return D3D_OK; }
    HRESULT SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD)  { return D3D_OK; }
    HRESULT GetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD*) { return D3D_OK; }
    HRESULT DrawRectPatch(UINT, CONST float*, CONST D3DRECTPATCH_INFO*) { return D3D_OK; }
    HRESULT DrawTriPatch(UINT, CONST float*, CONST D3DTRIPATCH_INFO*)   { return D3D_OK; }
    HRESULT DeletePatch(UINT)                            { return D3D_OK; }
    HRESULT SetSoftwareVertexProcessing(BOOL)            { return D3D_OK; }
};

#endif // __LT_COMPAT_D3D9_H__
