// Minimal stub of <d3d9types.h> for the macOS build.
// The Direct3D 9 renderer (runtime/render_a/.../d3d) is NOT built on macOS — a
// null renderer is used — but a few D3D value types leak into shared interface
// headers (renderstruct.h, d3dddstructs.h). Provide just those, with the real
// Win32/D3D memory layout so any serialized data stays compatible.
#ifndef __LT_COMPAT_D3D9TYPES_H__
#define __LT_COMPAT_D3D9TYPES_H__

#include <windows.h>

typedef DWORD D3DCOLOR;

typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN = 0
} D3DFORMAT;

typedef struct _D3DVECTOR {
    float x, y, z;
} D3DVECTOR;

typedef struct _D3DCOLORVALUE {
    float r, g, b, a;
} D3DCOLORVALUE;

#ifndef D3D_OK
#define D3D_OK 0
#endif

typedef enum _D3DSAMPLERSTATETYPE {
    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_ADDRESSW = 3,
    D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7,
    D3DSAMP_MIPMAPLODBIAS = 8, D3DSAMP_MAXANISOTROPY = 10
} D3DSAMPLERSTATETYPE;

typedef enum _D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2, D3DTEXF_ANISOTROPIC = 3
} D3DTEXTUREFILTERTYPE;

typedef struct _D3DVIEWPORT9 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT9;

typedef struct _D3DVERTEXELEMENT9 {
    WORD Stream, Offset;
    BYTE Type, Method, Usage, UsageIndex;
} D3DVERTEXELEMENT9;

typedef struct _D3DCAPS9 {
    DWORD DeviceType, Caps, Caps2, Caps3;
    DWORD MaxTextureWidth, MaxTextureHeight;
    DWORD MaxSimultaneousTextures, MaxTextureBlendStages;
    DWORD MaxStreams, MaxVertexShaderConst;
    DWORD PixelShaderVersion, VertexShaderVersion;
    float MaxPointSize;
    DWORD TextureFilterCaps, TextureCaps;
} D3DCAPS9;

typedef struct _D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;

// ---------------------------------------------------------------------------
// Additional D3D9 value types referenced by the device interface and the
// engine's d3d_device_wrapper.h / d3d_utils.h.  These are all consumed by the
// null renderer, which never issues a real D3D call, so the enums only need to
// exist as types and the structs only need representative layouts.
// ---------------------------------------------------------------------------

// --- Enumerations (used only as parameter types; values are not interpreted,
//     except the few named constants the engine code references directly) ---
typedef enum _D3DPOOL {
    D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3
} D3DPOOL;

typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0, D3DMULTISAMPLE_NONMASKABLE = 1, D3DMULTISAMPLE_2_SAMPLES = 2
} D3DMULTISAMPLE_TYPE;

typedef enum _D3DBACKBUFFER_TYPE {
    D3DBACKBUFFER_TYPE_MONO = 0, D3DBACKBUFFER_TYPE_LEFT = 1, D3DBACKBUFFER_TYPE_RIGHT = 2
} D3DBACKBUFFER_TYPE;

typedef enum _D3DQUERYTYPE {
    D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9
} D3DQUERYTYPE;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3,
    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
} D3DPRIMITIVETYPE;

typedef enum _D3DDEVTYPE {
    D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2, D3DDEVTYPE_SW = 3
} D3DDEVTYPE;

typedef enum _D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3
} D3DSWAPEFFECT;

typedef enum _D3DLIGHTTYPE {
    D3DLIGHT_POINT = 1, D3DLIGHT_SPOT = 2, D3DLIGHT_DIRECTIONAL = 3
} D3DLIGHTTYPE;

typedef enum _D3DSTATEBLOCKTYPE {
    D3DSBT_ALL = 1, D3DSBT_PIXELSTATE = 2, D3DSBT_VERTEXSTATE = 3
} D3DSTATEBLOCKTYPE;

typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_WORLD = 256
} D3DTRANSFORMSTATETYPE;

// Only D3DRS_LIGHTING is referenced by name (in d3d_device_wrapper.h); the rest
// round out the enum so any other leaked reference resolves.
typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9,
    D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23,
    D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25, D3DRS_DITHERENABLE = 26,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_LIGHTING = 137,
    D3DRS_CLIPPING = 136, D3DRS_AMBIENT = 139, D3DRS_COLORVERTEX = 141
} D3DRENDERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3,
    D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5, D3DTSS_ALPHAARG2 = 6,
    D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24
} D3DTEXTURESTAGESTATETYPE;

// --- Structures (opaque to the null renderer; representative layouts) ---
typedef struct _D3DDISPLAYMODE {
    UINT Width, Height, RefreshRate;
    D3DFORMAT Format;
} D3DDISPLAYMODE;

typedef struct _D3DDEVICE_CREATION_PARAMETERS {
    UINT AdapterOrdinal;
    D3DDEVTYPE DeviceType;
    HWND hFocusWindow;
    DWORD BehaviorFlags;
} D3DDEVICE_CREATION_PARAMETERS;

typedef struct _D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct _D3DRASTER_STATUS { BOOL InVBlank; UINT ScanLine; } D3DRASTER_STATUS;

typedef struct _D3DGAMMARAMP { WORD red[256], green[256], blue[256]; } D3DGAMMARAMP;

typedef struct _D3DRECT { LONG x1, y1, x2, y2; } D3DRECT;

typedef struct _D3DMATERIAL9 {
    D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive;
    float Power;
} D3DMATERIAL9;

typedef struct _D3DLIGHT9 {
    D3DLIGHTTYPE Type;
    D3DCOLORVALUE Diffuse, Specular, Ambient;
    D3DVECTOR Position, Direction;
    float Range, Falloff;
    float Attenuation0, Attenuation1, Attenuation2;
    float Theta, Phi;
} D3DLIGHT9;

typedef struct _D3DCLIPSTATUS9 { DWORD ClipUnion, ClipIntersection; } D3DCLIPSTATUS9;

typedef struct _D3DRECTPATCH_INFO {
    UINT StartVertexOffsetWidth, StartVertexOffsetHeight, Width, Height, Stride;
    DWORD Basis, Degree;
} D3DRECTPATCH_INFO;

typedef struct _D3DTRIPATCH_INFO {
    UINT StartVertexOffset, NumVertices;
    DWORD Basis, Degree;
} D3DTRIPATCH_INFO;

#endif // __LT_COMPAT_D3D9TYPES_H__
