// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_world.mm
//
// PURPOSE : See mtl_world.h. Turns a parsed RWorld into Metal draw calls.
//
//           Where the drawprim pass streams into a per-frame ring, the world is
//           STATIC: one vertex buffer and one index buffer per render block,
//           built on first use and kept. 164k verts / 98k tris for C01S01 is
//           ~9 MB uploaded once, against re-specifying every vertex every frame
//           through glBegin/glEnd, which is what the GL path did.
//
//           The composed vertex colours live in their own buffer because they
//           are the one part that CHANGES at runtime: a light switch calls
//           RWorld_SetLightGroupColor, which recomposes them (§ light groups).
//
// ----------------------------------------------------------------------- //

#import <Metal/Metal.h>

#include "bdefs.h"
#include "de_world.h"
#include "renderstruct.h"
#include "sys/shared/world_renderdata.h"   // the shared parsed world + RWDrawParams
#include "sys/shared/render_globals.h" // g_pRenderStruct (the engine function table)
#include "mtl_device.h"
#include "mtl_matrix.h"
#include "mtl_texture.h"
#include "mtl_world.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// The vertex buffer IS the parsed array -- no repacking, no copy into a
// renderer-private format. That only works while the file layout and the
// struct agree, so say so at compile time rather than debugging garbage UVs.
static_assert(sizeof(RWVertex) == 44, "RWVertex must stay the retail 44-byte world vertex");

// PC shader codes from the world packer (mirrors world_renderdata.cpp).
enum
{
	kPCShader_None             = 0,
	kPCShader_SkyPortal        = 6,
	kPCShader_Occluder         = 7
};

// ==========================================================================
// The shader.
//
// Four lighting modes, which is the whole of the GL path's texture-env matrix
// once you strip the fixed-function ceremony:
//   0 GOURAUD    base texture x composed vertex colour      (most of the world)
//   1 LIGHTMAP   base texture x lightmap                    (multitextured)
//   2 LMONLY     the lightmap IS the surface, sampled UV1   (no base texture)
//   3 UNTEXTURED composed vertex colour only                (the Siberia dome)
// x SATURATE (retail renders the world at 2x, §27).
// ==========================================================================
static const char *kWorldMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct WVSIn {
    float3 pos    [[attribute(0)]];
    float2 uv0    [[attribute(1)]];
    float2 uv1    [[attribute(2)]];
    float3 normal [[attribute(3)]];
    float4 color  [[attribute(4)]];
};

struct WVSOut {
    float4 pos [[position]];
    float2 uv0;
    float2 uv1;
    float3 sec;     // second-layer coords: detail/env2D use .xy, cube uses .xyz
    float4 color;
    // ⚠️ PERSPECTIVE-CORRECT, THE METAL DEFAULT -- and MEASURED to be right.
    // Fixed-function GL is classically described as interpolating fog linearly
    // in window space; marking this [[center_no_perspective]] to match that is
    // ~3x WORSE (2.61 / 5.38 / 2.37 / 3.33 against 0.06 / 1.28 / 0.08 / 1.12).
    // GL's fog here is perspective-correct. Do not "fix" this to match the
    // folklore (§101).
    // ⚠️ SIGNED EYE Z, NOT A PER-VERTEX FOG FACTOR. See the fragment shader:
    // the factor is computed PER FRAGMENT, because abs() applied per vertex is
    // wrong for any triangle with a vertex BEHIND the eye (§103).
    float  eyeZ;
};

struct WUniforms {
    float4x4 mvp;
    float4x4 mv;
    float4x4 envMtx;      // camera->world rotation (columns R, U, -F)
    float4   fogColor;    // rgb, w = enable
    float2   fogRange;    // near, far
    float    objectAlpha;
    float    alphaRef;
    float4   secParams;   // detail: (scale, cos, sin, kind); env2D: (envScale, -, -, kind)
    float4   dynPos;      // xyz = light position (this world's space), w = radius
    float4   dynColor;    // rgb = light colour 0..1, w = 1 when the light pass is live
};

vertex WVSOut w_vertex(WVSIn in [[stage_in]],
                       constant WUniforms &u [[buffer(2)]])
{
    WVSOut o;
    float4 p = float4(in.pos, 1.0);
    o.pos   = u.mvp * p;
    o.uv0   = in.uv0;
    o.uv1   = in.uv1;
    // ★ THE ADDITIVE DYNAMIC-LIGHT PASS computes its colour here, from the
    // position and normal already in the static per-block vertex buffer — so
    // this pass needs no streamed geometry at all, unlike GL's glBegin/glEnd
    // re-emit. Same maths per vertex: (1 - d/r) * max(N.L, 0).
    if (u.dynColor.w > 0.5) {
        float3 toLight = u.dynPos.xyz - in.pos;
        float  d       = length(toLight);
        float  atten   = 1.0 - d / max(u.dynPos.w, 1e-4);
        if (atten > 0.0 && d > 0.1) atten *= max(dot(in.normal, toLight / d), 0.0);
        else                        atten  = 0.0;
        o.color = float4(u.dynColor.rgb * atten, 1.0);
    } else {
        // ★★★★ THE PER-VERTEX ALPHA MUST SURVIVE. This used to be
        //     o.color = float4(in.color.rgb, u.objectAlpha);
        // which read the vertex colour and then THREW ITS ALPHA AWAY, replacing
        // it with the object-alpha uniform. Both alphas are real and they
        // multiply, exactly as D3D's texture stages stack them:
        //   * in.color.a  — the AUTHORED per-vertex diffuse alpha, i.e. a
        //     WorldModel's `Alpha` property baked into the vertex colours by
        //     the level pre-processor. This is what makes Siberia's window
        //     glass see-through (c04s05 authors it at 127 and 153); its texture
        //     GlUW002.dtx is a fully opaque DXT1 and cannot supply any.
        //   * u.objectAlpha — the per-INSTANCE alpha (LTObject::m_ColorA).
        // Dropping the first made every authored pane composite at 1.0 and read
        // as a solid wall. ⚠️ Both are 1.0 for ordinary geometry, so this costs
        // nothing anywhere else.
        o.color = float4(in.color.rgb, in.color.a * u.objectAlpha);
    }

    // ── THE SECOND LAYER'S COORDINATES ──────────────────────────────────
    // DETAIL takes the BASE UVs through the authored scale+rotation (§60):
    //     u1 = u0*S*cos + v0*S*(-sin),  v1 = u0*S*sin + v0*S*cos
    // ENV generates a REFLECTION VECTOR, which is what GL_REFLECTION_MAP did
    // from the eye-space position and normal, then rotates it into world space
    // with envMtx (d3d_SetEnvMapTransform's m_mWorldEnvMap). A cube map takes
    // the raw 3 components; a 2D map takes the scaled-and-biased x/y pair
    // (⚠️ x and Y — the polygrid projects XZ because it builds a different
    // matrix; §59 says explicitly not to copy that one here).
    // ⚠️ The KIND is a uniform here, not a function constant. Function
    // constants declared for the fragment stage are not in scope for the vertex
    // one, and referencing them would force the vertex function to be
    // specialised too — a second function per combination for no benefit. The
    // fragment stage still specialises, which is where the cost would be.
    const int secKind = int(u.secParams.w);
    o.sec = float3(0.0);
    if (secKind == 3) {
        const float S = u.secParams.x, C = u.secParams.y, Sn = u.secParams.z;
        o.sec = float3(in.uv0.x * S * C + in.uv0.y * S * (-Sn),
                       in.uv0.x * S * Sn + in.uv0.y * S * C, 0.0);
    } else if (secKind != 0) {
        float3 pe = (u.mv * p).xyz;
        float3 ne = normalize((u.mv * float4(in.normal, 0.0)).xyz);
        float3 ee = normalize(pe);
        float3 r  = ee - 2.0 * ne * dot(ne, ee);
        float3 rw = (u.envMtx * float4(r, 0.0)).xyz;
        o.sec = (secKind == 2) ? rw
              : float3(u.secParams.x * rw.x + 0.5, u.secParams.x * rw.y + 0.5, 0.0);
    }

    // GL_LINEAR fog over the eye-space depth, which is what glFogf(GL_FOG_START
    // /END) means. Computed per vertex, as fixed-function GL did.
    // ⚠️ UNCLAMPED HERE ON PURPOSE — the fragment shader clamps.
    // Clamping per VERTEX and then interpolating is not the same as
    // interpolating and then clamping: a big ground polygon with one vertex
    // beyond the far fog plane (f<0 -> 0) and one inside (f>1 -> 1) gets a
    // factor that is wrong everywhere in between. GL fogs per FRAGMENT (the GL
    // path even asks for GL_FOG_HINT/GL_NICEST).
    // ⚠️ MEASURED: this is the correct semantics, but it changed NOTHING on the
    // c01s01 case it was written for (10.40 before and after) -- the geometry
    // there does not straddle the fog planes. Keep it because it is right, not
    // because it fixed anything.
    // ⚠️⚠️ DO NOT abs() HERE, AND DO NOT BUILD THE FACTOR HERE.
    // The ground plane extends UNDER AND BEHIND the camera, so a big near
    // triangle can have a vertex with eye z on the far side of the eye. abs()
    // turns that into a positive distance, the clipper then interpolates from
    // the poisoned value, and every such triangle gets a fog factor that is too
    // low -- per-triangle steps on near ground, growing as z -> 0, exact in the
    // distance. MEASURED against the analytic factor recovered from the depth
    // buffer: GL is exact everywhere, this was -0.0170 at z<150 and -0.0061 at
    // z 150-300 (§103). Interpolating SIGNED eye z is exact (it is affine in eye
    // space) and the fragment does the abs -- which is per-fragment fog, exactly
    // what GL_FOG_HINT/GL_NICEST gives the GL path.
    o.eyeZ = (u.mv * p).z;
    return o;
}

constant int kLightMode  [[function_constant(0)]];
constant int kSaturate   [[function_constant(1)]];
// 0 none, 1 env 2D, 2 env CUBE, 3 detail
constant int kSecondKind [[function_constant(2)]];
// 0 modulate, 1 add-signed, 2 modulate-alpha-add-colour
constant int kSecondMode [[function_constant(3)]];

fragment float4 w_fragment(WVSOut in [[stage_in]],
                           constant WUniforms &u      [[buffer(0)]],
                           texture2d<float>   baseTex [[texture(0)]],
                           texture2d<float>   lmTex   [[texture(1)]],
                           texture2d<float>   secTex  [[texture(2)]],
                           texturecube<float> secCube [[texture(3)]],
                           sampler            baseSmp [[sampler(0)]],
                           sampler            lmSmp   [[sampler(1)]],
                           sampler            secSmp  [[sampler(2)]])
{
    const float s = (kSaturate != 0) ? 2.0 : 1.0;
    float4 c;

    // ★★★★ WHICH ALPHA A WORLD SURFACE USES IS PER-SHADER, AND D3D IS NOT
    // UNIFORM ABOUT IT. Getting this wrong in either direction is visible:
    //   * gouraud UNTEXTURED   ALPHAOP=SELECTARG2, ARG2=DIFFUSE
    //                          -> the vertex alpha         (gouraud.cpp:82)
    //   * gouraud TEXTURED     ALPHAOP=MODULATE(TEXTURE, DIFFUSE)
    //                          -> texture x vertex alpha   (gouraud.cpp:196)
    //   * LIGHTMAP shaders     ALPHAOP=SELECTARG1, ARG1=TEXTURE
    //                          -> texture alpha ONLY, the vertex alpha is
    //                             IGNORED                  (lightmap.cpp:51)
    // ⚠️ That last one is not a detail. c01s01's MAIN WORLD authors a vertex
    // alpha on 94 of its 139 blocks, ranging down to 0; feeding those into a
    // lightmapped surface's alpha runs them into the alpha test and discards
    // real geometry. It broke the frame contract the moment I applied the
    // vertex alpha everywhere (0.04/0.03/0.08/0.06 against a 0.00 gate).
    // in.color.a carries vertexAlpha * objectAlpha from the vertex stage; the
    // branches that must ignore the vertex part use u.objectAlpha directly.
    if (kLightMode == 3) {
        c = in.color;
    } else if (kLightMode == 2) {
        // The lightmap is the surface colour. Its own UV set, and no vertex
        // colour: a lightmapped surface carries its light in the map, exactly
        // as D3D's lightmap shader does (it does not modulate vertex diffuse).
        c = float4(lmTex.sample(lmSmp, in.uv1).rgb, u.objectAlpha);   // lightmap: no vertex alpha
    } else {
        float4 t = baseTex.sample(baseSmp, in.uv0);

        // ── base (+) SECOND LAYER, before the lighting multiply ──────────
        // D3D's stage 1 combines against CURRENT (the bare base texture) and
        // the layer's TEXTURE; the layer's ALPHA never reaches the output, or a
        // detail map with an alpha channel would silently dim the surface.
        // ⚠️ The lighting multiply stays DOWNSTREAM: ADDSIGNED and the
        // alpha-add are not commutative with it, so retail's
        // (base (+) second) * diffuse is not base*diffuse (+) second (§59).
        if (kSecondKind != 0) {
            float3 sec = (kSecondKind == 2)
                       ? secCube.sample(secSmp, in.sec).rgb
                       : secTex.sample(secSmp, in.sec.xy).rgb;
            float3 rgb;
            if      (kSecondMode == 1) rgb = t.rgb + sec - 0.5;   // ADD_SIGNED
            else if (kSecondMode == 2) rgb = sec * t.a + t.rgb;   // MODULATEALPHA_ADDCOLOR
            else                       rgb = t.rgb * sec;         // MODULATE
            // Fixed-function clamps at every stage; float does not.
            t.rgb = saturate(rgb);
        }

        if (kLightMode == 1) {
            float3 l = lmTex.sample(lmSmp, in.uv1).rgb;
            c = float4(t.rgb * l * s, t.a * u.objectAlpha);   // lightmap+texture: TEXTURE alpha only
        } else {
            c = float4(t.rgb * in.color.rgb * s, t.a * in.color.a);
        }
    }

    // ⚠️ THE ALPHA TEST NEEDS NO SPECIALISATION. D3D's test is GEQUAL against
    // the AUTHORED reference (§13/§14/§80), so "discard below the reference"
    // covers it, and a reference of 0 -- ALPHAREF_NONE, i.e. do not test this
    // surface -- disables it for free because alpha is never negative.
    if (c.a < u.alphaRef)
        discard_fragment();

    // Clamp before fogging: fixed-function GL clamps the fragment colour to
    // [0,1] at the end of the texture-environment stage, so its fog stage always
    // mixes against a colour <= 1, and the world renders at 2x saturate (§27).
    // ⚠️ MEASURED, AND IT CHANGED NOTHING -- the c01s01 orbit frames are
    // BIT-IDENTICAL with and without the saturate. It was written as a candidate
    // explanation for the fog-shaped difference and is NOT one. Kept because it
    // is the correct semantics, not because it fixed anything (the same honesty
    // the per-vertex fog comment above already owed the reader).
    // PER-FRAGMENT fog, from the interpolated signed eye z.
    const float fFogZ = abs(in.eyeZ);
    const float fFogF = (u.fogColor.w > 0.5)
                      ? (u.fogRange.y - fFogZ) / max(u.fogRange.y - u.fogRange.x, 1e-3)
                      : 1.0;

#ifdef FOG_DEBUG
    // LT_DEBUG_FOGFACTOR: emit the FOG FACTOR ITSELF as greyscale, before it is
    // applied to anything. The GL path has no shader, so it produces the same
    // picture the only way fixed function can -- a WHITE untextured surface with
    // BLACK fog, where mix(black, white, f) == f. Diffing the two images turns
    // "Metal's factor is 0.02 low somewhere near the camera" into a picture with
    // geometry attached.
    const float fDbg = clamp(fFogF, 0.0, 1.0);
    return float4(fDbg, fDbg, fDbg, 1.0);
#endif
    c.rgb = mix(u.fogColor.rgb, saturate(c.rgb), clamp(fFogF, 0.0, 1.0));
    return c;
}
)MSL";

// ==========================================================================
// State
// ==========================================================================
struct MWUniforms
{
	float m_aMVP[16];
	float m_aMV[16];
	float m_aEnvMtx[16];
	float m_aFogColor[4];    // rgb + enable
	float m_aFogRange[2];
	float m_fObjectAlpha;
	float m_fAlphaRef;
	float m_aSecParams[4];
	float m_aDynPos[4];
	float m_aDynColor[4];
};

// ⚠️⚠️ ASSERT EVERY OFFSET OF A SHADER-SHARED STRUCT. A layout mismatch between
// this and WUniforms in the MSL above does not error anywhere -- the shader
// simply reads later fields from the wrong place, and the failure looks like a
// rendering bug (CharacterViewer's port: a blank view from one hand-written
// pad). MSL rules: float4x4 = 64 bytes/16-aligned, float4 = 16/16,
// float2 = 8/8, float = 4/4.
static_assert(offsetof(MWUniforms, m_aMVP)        ==   0, "WUniforms.mvp moved");
static_assert(offsetof(MWUniforms, m_aMV)         ==  64, "WUniforms.mv moved");
static_assert(offsetof(MWUniforms, m_aEnvMtx)     == 128, "WUniforms.envMtx moved");
static_assert(offsetof(MWUniforms, m_aFogColor)   == 192, "WUniforms.fogColor moved");
static_assert(offsetof(MWUniforms, m_aFogRange)   == 208, "WUniforms.fogRange moved");
static_assert(offsetof(MWUniforms, m_fObjectAlpha)== 216, "WUniforms.objectAlpha moved");
static_assert(offsetof(MWUniforms, m_fAlphaRef)   == 220, "WUniforms.alphaRef moved");
static_assert(offsetof(MWUniforms, m_aSecParams)  == 224, "WUniforms.secParams moved");
static_assert(offsetof(MWUniforms, m_aDynPos)     == 240, "WUniforms.dynPos moved");
static_assert(offsetof(MWUniforms, m_aDynColor)   == 256, "WUniforms.dynColor moved");
static_assert(sizeof(MWUniforms)                  == 272, "WUniforms size changed");

struct MWBlockGPU
{
	id<MTLBuffer> m_Vtx;     // the parsed RWVertex array, verbatim
	id<MTLBuffer> m_Col;     // composed vertex colours, RGBA8
	id<MTLBuffer> m_Idx;     // uint32 indices
	uint32        m_nVerts;
	uint32        m_nIndices;
	// ⚠️ Which generation of the composed vertex colours m_Col holds. A light
	// group can recompose a block that is NOT visible this frame, so a single
	// global "dirty" flag cleared at the end of the draw would upload the
	// visible blocks and silently lose every culled one.
	uint32        m_nColorGen;
};

static id<MTLLibrary>           g_Library   = nil;
static id<MTLFunction>          g_VertexFn  = nil;
static MTLVertexDescriptor*     g_VertexDesc = nil;
static id<MTLSamplerState>      g_LMSampler = nil;   // clamp, no mips
static id<MTLSamplerState>      g_RepeatSmp = nil;   // detail layers tile
static id<MTLTexture>           g_WhiteTex  = nil;
// ⚠️ The fragment function declares BOTH a texture2d and a texturecube for the
// second layer in every specialisation, so Metal wants something bound in each
// slot even when the specialised code samples neither. A 1x1 white cube is
// cheaper than a second shader family.
static id<MTLTexture>           g_WhiteCube = nil;
static bool                     g_bReady    = false;
static bool                     g_bFailed   = false;

// PSO cache: 4 lighting modes x 2 saturate x 3 blend modes.
enum { kMWModes = 4, kMWSat = 2, kMWBlends = 4, kMWSecKinds = 4, kMWSecModes = 3 };
static CFTypeRef g_aPipelines[kMWModes][kMWSat][kMWBlends][kMWSecKinds][kMWSecModes];

// Depth states: [compare: 0 always, 1 less, 2 lessEqual][depthWrite].
static id<MTLDepthStencilState> g_aDepth[3][2];

static std::vector<MWBlockGPU> g_aBlockGPU;      // handle = index + 1
static std::vector<CFTypeRef>  g_aLightmaps;     // handle = index + 1

static float g_aView[16], g_aProj[16];
static bool  g_bSceneValid = false;
static float g_aFogColor[4] = { 0, 0, 0, 0 };
static float g_fFogNear = 0.0f, g_fFogFar = 2000.0f;
// Bumped by MTLWorld_InvalidateVertexColors; a block whose m_nColorGen differs
// is re-uploaded the next time it is drawn, however many frames later that is.
static uint32 g_nColorGen = 1;

static uint32 g_nDrawnSections = 0, g_nDrawnTris = 0;   // per-frame census

static bool mw_Trace(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_TRACE_MTLWORLD") ? 1 : 0;
	return s_n != 0;
}

// ==========================================================================
// Lightmaps
// ==========================================================================
uintptr_t MTLWorld_CreateLightmap(const uint8_t *pBGR, uint32_t nWidth, uint32_t nHeight,
                                  uint32_t nPadW, uint32_t nPadH)
{
	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev || !pBGR || !nWidth || !nHeight)
		return 0;

	@autoreleasepool {
		MTLTextureDescriptor *d =
			[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                   width:nPadW
			                                                  height:nPadH
			                                               mipmapped:NO];
		d.usage       = MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		id<MTLTexture> tex = [dev newTextureWithDescriptor:d];
		if (!tex)
			return 0;

		// The stream's lightmap is 24-bit B,G,R. Metal has no 24-bit format, so
		// expand to BGRA8 -- the same one-off CPU cost mtl_texture pays for the
		// 16-bit DTX formats, and for the same reason.
		std::vector<uint8> aRGBA((size_t)nWidth * nHeight * 4);
		for (size_t i = 0; i < (size_t)nWidth * nHeight; ++i)
		{
			aRGBA[i * 4 + 0] = pBGR[i * 3 + 0];
			aRGBA[i * 4 + 1] = pBGR[i * 3 + 1];
			aRGBA[i * 4 + 2] = pBGR[i * 3 + 2];
			aRGBA[i * 4 + 3] = 255;
		}
		[tex replaceRegion:MTLRegionMake2D(0, 0, nWidth, nHeight)
		       mipmapLevel:0
		         withBytes:&aRGBA[0]
		       bytesPerRow:(NSUInteger)nWidth * 4];

		g_aLightmaps.push_back(CFBridgingRetain(tex));
		if (mw_Trace() && (g_aLightmaps.size() & 255) == 1)
			fprintf(stderr, "[mtlw] %u lightmaps created (latest %ux%u -> %ux%u)\n",
			        (uint32)g_aLightmaps.size(), nWidth, nHeight, nPadW, nPadH);
		return (uintptr_t)g_aLightmaps.size();   // index + 1
	}
}

static id<MTLTexture> mw_Lightmap(uintptr_t h)
{
	if (!h || h > g_aLightmaps.size())
		return nil;
	return (__bridge id<MTLTexture>)g_aLightmaps[h - 1];
}

void MTLWorld_UpdateLightmap(uintptr_t hTex, const uint8_t *pBGR,
                             uint32_t nWidth, uint32_t nHeight)
{
	id<MTLTexture> tex = mw_Lightmap(hTex);
	if (!tex || !pBGR || !nWidth || !nHeight)
		return;

	std::vector<uint8> aRGBA((size_t)nWidth * nHeight * 4);
	for (size_t i = 0; i < (size_t)nWidth * nHeight; ++i)
	{
		aRGBA[i * 4 + 0] = pBGR[i * 3 + 0];
		aRGBA[i * 4 + 1] = pBGR[i * 3 + 1];
		aRGBA[i * 4 + 2] = pBGR[i * 3 + 2];
		aRGBA[i * 4 + 3] = 255;
	}
	[tex replaceRegion:MTLRegionMake2D(0, 0, nWidth, nHeight)
	       mipmapLevel:0
	         withBytes:&aRGBA[0]
	       bytesPerRow:(NSUInteger)nWidth * 4];
}

// ⚠️ The vertex colours are invalidated from RWorld_SetLightGroupColor, NOT
// from the lightmap upload above. A light group that touches only VERTEX-LIT
// (Gouraud) surfaces has no lightmapped section, so MTLWorld_UpdateLightmap
// never runs for it -- the CPU recomposed the colours and nothing re-uploaded
// them. In game: a light switch that does not change Gouraud surfaces.
void MTLWorld_InvalidateVertexColors(void)
{
	++g_nColorGen;
}

void MTLWorld_DestroyLightmap(uintptr_t hTex)
{
	if (!hTex || hTex > g_aLightmaps.size() || !g_aLightmaps[hTex - 1])
		return;
	CFBridgingRelease(g_aLightmaps[hTex - 1]);
	g_aLightmaps[hTex - 1] = NULL;
}

// ==========================================================================
// Shaders, samplers, depth states (once)
// ==========================================================================
static bool mw_EnsureShaders(void)
{
	if (g_bReady)  return true;
	if (g_bFailed) return false;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev)
		return false;

	NSError *err = nil;
	// ⚠️ The library is compiled from a STRING at runtime, so the debug variant
	// is a source prefix rather than another function-constant dimension -- the
	// pipeline cache key (mode x saturate x blend x secKind x secMode) stays
	// exactly as it is, and an off switch costs nothing at all.
	NSString *srcWorld = [NSString stringWithUTF8String:kWorldMSL];
	if (getenv("LT_DEBUG_FOGFACTOR"))
	{
		srcWorld = [@"#define FOG_DEBUG 1\n" stringByAppendingString:srcWorld];
		fprintf(stderr, "[mtlw] LT_DEBUG_FOGFACTOR: world shader emits the fog factor\n");
	}
	g_Library = [dev newLibraryWithSource:srcWorld
	                              options:nil error:&err];
	if (!g_Library)
	{
		g_bFailed = true;
		fprintf(stderr, "[mtlw] world MSL compile FAILED: %s\n",
		        err ? [[err localizedDescription] UTF8String] : "(no error)");
		return false;
	}
	g_VertexFn = [g_Library newFunctionWithName:@"w_vertex"];
	if (!g_VertexFn)
	{
		g_bFailed = true;
		fprintf(stderr, "[mtlw] w_vertex missing from the compiled library\n");
		return false;
	}

	// Buffer 0 is the parsed vertex array itself; buffer 1 the composed colours.
	g_VertexDesc = [[MTLVertexDescriptor alloc] init];
	g_VertexDesc.attributes[0].format = MTLVertexFormatFloat3;          // pos
	g_VertexDesc.attributes[0].offset = 0;
	g_VertexDesc.attributes[0].bufferIndex = 0;
	g_VertexDesc.attributes[1].format = MTLVertexFormatFloat2;          // uv0
	g_VertexDesc.attributes[1].offset = 12;
	g_VertexDesc.attributes[1].bufferIndex = 0;
	g_VertexDesc.attributes[2].format = MTLVertexFormatFloat2;          // uv1
	g_VertexDesc.attributes[2].offset = 20;
	g_VertexDesc.attributes[2].bufferIndex = 0;
	g_VertexDesc.attributes[3].format = MTLVertexFormatFloat3;          // normal
	g_VertexDesc.attributes[3].offset = 32;
	g_VertexDesc.attributes[3].bufferIndex = 0;
	g_VertexDesc.layouts[0].stride = sizeof(RWVertex);
	g_VertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

	g_VertexDesc.attributes[4].format = MTLVertexFormatUChar4Normalized; // colour
	g_VertexDesc.attributes[4].offset = 0;
	g_VertexDesc.attributes[4].bufferIndex = 1;
	g_VertexDesc.layouts[1].stride = 4;
	g_VertexDesc.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;

	{
		// ⚠️ The lightmap must CLAMP. It is padded to pow2 and its UVs address
		// the used sub-rect; repeating would wrap a surface's light onto the
		// undefined padding.
		MTLSamplerDescriptor *s = [[MTLSamplerDescriptor alloc] init];
		s.minFilter    = MTLSamplerMinMagFilterLinear;
		s.magFilter    = MTLSamplerMinMagFilterLinear;
		s.sAddressMode = MTLSamplerAddressModeClampToEdge;
		s.tAddressMode = MTLSamplerAddressModeClampToEdge;
		g_LMSampler = [dev newSamplerStateWithDescriptor:s];

		// ⚠️ An ENV 2D map clamps (a reflection addresses it once) but must still
		// be MIPMAPPED — the lightmap descriptor above deliberately is not, and
		// reusing it for the second layer cost ~1.3 mean abs of aliasing.
		// A DETAIL layer uses the texture's OWN sampler (repeat + mips) via
		// MTLTex_Sampler, exactly as the GL path re-binds GL_REPEAT for it.
		s.mipFilter = MTLSamplerMipFilterLinear;
		g_RepeatSmp = [dev newSamplerStateWithDescriptor:s];
	}
	{
		MTLTextureDescriptor *d =
			[MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                      size:1 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		g_WhiteCube = [dev newTextureWithDescriptor:d];
		const uint8 aWhite[4] = { 255, 255, 255, 255 };
		for (NSUInteger f = 0; f < 6; ++f)
			[g_WhiteCube replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
			               mipmapLevel:0 slice:f
			                 withBytes:aWhite bytesPerRow:4 bytesPerImage:4];
	}
	{
		MTLTextureDescriptor *d =
			[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                   width:1 height:1 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		g_WhiteTex = [dev newTextureWithDescriptor:d];
		const uint8 aWhite[4] = { 255, 255, 255, 255 };
		[g_WhiteTex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
		              mipmapLevel:0 withBytes:aWhite bytesPerRow:4];
	}

	for (int nTest = 0; nTest < 3; ++nTest)
		for (int nWrite = 0; nWrite < 2; ++nWrite)
		{
			MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
			// ⚠️ LESS, NOT LESSEQUAL. The GL world pass never calls glDepthFunc,
			// so it runs on GL's DEFAULT of GL_LESS (the only two calls in the
			// renderer are in the dynamic-light pass, which sets LEQUAL and
			// restores LESS). The difference is not academic: NOLF2 is full of
			// exactly COINCIDENT faces (§49 -- the noren curtain, the "texture
			// flicker"), and LESS keeps the FIRST-drawn one while LEQUAL lets
			// the LAST one win. Indoors that is the difference between a lit
			// wall and its unlit backside.
			d.depthCompareFunction = (nTest == 0) ? MTLCompareFunctionAlways
			                       : (nTest == 1) ? MTLCompareFunctionLess
			                                      : MTLCompareFunctionLessEqual;
			d.depthWriteEnabled    = nWrite ? YES : NO;
			g_aDepth[nTest][nWrite] = [dev newDepthStencilStateWithDescriptor:d];
		}

	memset(g_aPipelines, 0, sizeof(g_aPipelines));
	g_bReady = true;
	fprintf(stderr, "[mtlw] world MSL compiled at runtime\n");
	return true;
}

static id<MTLRenderPipelineState> mw_Pipeline(int nMode, int nSaturate, int nBlend,
                                              int nSecKind, int nSecMode)
{
	if (nMode     < 0 || nMode     >= kMWModes)    nMode     = 0;
	if (nSaturate < 0 || nSaturate >= kMWSat)      nSaturate = 0;
	if (nBlend    < 0 || nBlend    >= kMWBlends)   nBlend    = 0;
	if (nSecKind  < 0 || nSecKind  >= kMWSecKinds) nSecKind  = 0;
	if (nSecMode  < 0 || nSecMode  >= kMWSecModes) nSecMode  = 0;

	CFTypeRef cached = g_aPipelines[nMode][nSaturate][nBlend][nSecKind][nSecMode];
	if (cached)
		return (__bridge id<MTLRenderPipelineState>)cached;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev)
		return nil;

	NSError *err = nil;
	MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues alloc] init];
	int nM = nMode, nS = nSaturate, nK = nSecKind, nO = nSecMode;
	[cv setConstantValue:&nM type:MTLDataTypeInt atIndex:0];
	[cv setConstantValue:&nS type:MTLDataTypeInt atIndex:1];
	[cv setConstantValue:&nK type:MTLDataTypeInt atIndex:2];
	[cv setConstantValue:&nO type:MTLDataTypeInt atIndex:3];
	id<MTLFunction> fs = [g_Library newFunctionWithName:@"w_fragment"
	                                     constantValues:cv error:&err];
	if (!fs)
	{
		fprintf(stderr, "[mtlw] fragment specialisation (mode=%d sat=%d sec=%d/%d) failed: %s\n",
		        nMode, nSaturate, nSecKind, nSecMode,
		        err ? [[err localizedDescription] UTF8String] : "(none)");
		return nil;
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	pd.vertexFunction   = g_VertexFn;
	pd.fragmentFunction = fs;
	pd.vertexDescriptor = g_VertexDesc;
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
	if (nBlend != kRWBlend_None)
	{
		pd.colorAttachments[0].blendingEnabled = YES;
		pd.colorAttachments[0].rgbBlendOperation   = MTLBlendOperationAdd;
		pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
		// ⚠️ The light pass is ONE/ONE, not SRC_ALPHA/ONE. Same distinction §57
		// makes for particles: an additive contribution must not be scaled by an
		// alpha channel that the authored texture may not even have.
		const MTLBlendFactor eSrc = (nBlend == kRWBlend_AddOne)
		                          ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
		const MTLBlendFactor eDst = (nBlend == kRWBlend_None)
		                          ? MTLBlendFactorZero
		                          : ((nBlend == kRWBlend_Alpha)
		                             ? MTLBlendFactorOneMinusSourceAlpha
		                             : MTLBlendFactorOne);
		pd.colorAttachments[0].sourceRGBBlendFactor        = eSrc;
		pd.colorAttachments[0].sourceAlphaBlendFactor      = eSrc;
		pd.colorAttachments[0].destinationRGBBlendFactor   = eDst;
		pd.colorAttachments[0].destinationAlphaBlendFactor = eDst;
	}

	id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!pso)
	{
		fprintf(stderr, "[mtlw] pipeline (mode=%d sat=%d blend=%d) failed: %s\n",
		        nMode, nSaturate, nBlend,
		        err ? [[err localizedDescription] UTF8String] : "(none)");
		return nil;
	}
	g_aPipelines[nMode][nSaturate][nBlend][nSecKind][nSecMode] = CFBridgingRetain(pso);
	if (mw_Trace())
		fprintf(stderr, "[mtlw] new pipeline mode=%d saturate=%d blend=%d "
		                "second=%d(mode %d)\n",
		        nMode, nSaturate, nBlend, nSecKind, nSecMode);
	return pso;
}

// ==========================================================================
// Per-block GPU buffers, built once
// ==========================================================================
static void mw_FillColorBuffer(const RWBlock &cBlock, MWBlockGPU &cGPU)
{
	uint8 *pDst = (uint8*)[cGPU.m_Col contents];
	const size_t nVerts = cBlock.m_aVertices.size();
	for (size_t i = 0; i < nVerts; ++i)
	{
		if (i * 3 + 2 < cBlock.m_aComposedColor.size())
		{
			pDst[i * 4 + 0] = cBlock.m_aComposedColor[i * 3 + 0];
			pDst[i * 4 + 1] = cBlock.m_aComposedColor[i * 3 + 1];
			pDst[i * 4 + 2] = cBlock.m_aComposedColor[i * 3 + 2];
		}
		else
		{
			pDst[i * 4 + 0] = pDst[i * 4 + 1] = pDst[i * 4 + 2] = 255;
		}
		// ★ THE AUTHORED PER-VERTEX ALPHA (a WorldModel's `Alpha` property, baked
		// into the vertex colours by the level pre-processor). Empty means the
		// whole block is opaque, which is the overwhelmingly common case.
		// ⚠️ This used to be an unconditional 255 — "object alpha is a uniform,
		// not per vertex" — which was true of the OBJECT alpha and wrong about
		// the VERTEX alpha, and it is what made Siberia's window glass solid.
		pDst[i * 4 + 3] = (i < cBlock.m_aComposedAlpha.size())
		                ? cBlock.m_aComposedAlpha[i] : 255;
	}
}

static MWBlockGPU *mw_BlockGPU(const RWBlock &cBlock)
{
	if (cBlock.m_hGPU)
	{
		MWBlockGPU *pGPU = &g_aBlockGPU[cBlock.m_hGPU - 1];
		if (pGPU->m_nColorGen != g_nColorGen)
		{
			mw_FillColorBuffer(cBlock, *pGPU);
			pGPU->m_nColorGen = g_nColorGen;
		}
		return pGPU;
	}

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev || cBlock.m_aVertices.empty() || cBlock.m_aIndices.empty())
		return NULL;

	MWBlockGPU cGPU;
	cGPU.m_nColorGen = g_nColorGen;
	cGPU.m_nVerts   = (uint32)cBlock.m_aVertices.size();
	cGPU.m_nIndices = (uint32)cBlock.m_aIndices.size();
	// The parsed array IS the vertex buffer -- no repack (see the static_assert).
	cGPU.m_Vtx = [dev newBufferWithBytes:&cBlock.m_aVertices[0]
	                              length:cGPU.m_nVerts * sizeof(RWVertex)
	                             options:MTLResourceStorageModeShared];
	cGPU.m_Idx = [dev newBufferWithBytes:&cBlock.m_aIndices[0]
	                              length:cGPU.m_nIndices * sizeof(uint32)
	                             options:MTLResourceStorageModeShared];
	cGPU.m_Col = [dev newBufferWithLength:(NSUInteger)cGPU.m_nVerts * 4
	                              options:MTLResourceStorageModeShared];
	if (!cGPU.m_Vtx || !cGPU.m_Idx || !cGPU.m_Col)
		return NULL;

	mw_FillColorBuffer(cBlock, cGPU);

	g_aBlockGPU.push_back(cGPU);
	const_cast<RWBlock&>(cBlock).m_hGPU = (uintptr_t)g_aBlockGPU.size();
	return &g_aBlockGPU.back();
}

// ==========================================================================
void MTLWorld_SetSceneTransform(const float *pView16, const float *pProj16)
{
	if (!pView16 || !pProj16)
		return;
	memcpy(g_aView, pView16, sizeof(g_aView));
	memcpy(g_aProj, pProj16, sizeof(g_aProj));
	g_bSceneValid = true;
}

void MTLWorld_SetFog(bool bEnable, float fR, float fG, float fB,
                     float fNearZ, float fFarZ)
{
	// ⚠️ LT_NO_FOG USED TO BE APPLIED HERE, and only here -- so it disabled fog
	// under Metal while the GL reference build kept fogging. Every A/B run with
	// it was therefore comparing a fogged frame against an unfogged one, which
	// is where §90's "LT_NO_FOG makes f200 far worse (0.90 -> 59.10)" came from.
	// It now lives in RWorld_ApplyFog, above the backend branch, so both
	// renderers see the same decision.

	if (mw_Trace())
	{
		static float s_aLast[6] = { -1, -1, -1, -1, -1, -1 };
		const float aNow[6] = { bEnable ? 1.0f : 0.0f, fR, fG, fB, fNearZ, fFarZ };
		if (memcmp(s_aLast, aNow, sizeof(aNow)) != 0)
		{
			memcpy(s_aLast, aNow, sizeof(aNow));
			fprintf(stderr, "[mtlw] fog %s color=(%.0f %.0f %.0f) range=%.0f..%.0f\n",
			        bEnable ? "ON " : "OFF", fR * 255.0f, fG * 255.0f, fB * 255.0f,
			        fNearZ, fFarZ);
		}
	}

	g_aFogColor[0] = fR; g_aFogColor[1] = fG; g_aFogColor[2] = fB;
	g_aFogColor[3] = bEnable ? 1.0f : 0.0f;
	g_fFogNear = fNearZ;
	g_fFogFar  = fFarZ;
}

void MTLWorld_GetFog(float *pColor4, float *pNearZ, float *pFarZ)
{
	if (pColor4) memcpy(pColor4, g_aFogColor, sizeof(g_aFogColor));
	if (pNearZ)  *pNearZ = g_fFogNear;
	if (pFarZ)   *pFarZ  = g_fFogFar;
}

// ==========================================================================
// The draw
// ==========================================================================
void MTLWorld_DrawWorld(const RWorld *pWorld, const RWDrawParams *pParams)
{
	static RWDrawParams s_cDefaults;
	const RWDrawParams &cP = pParams ? *pParams : s_cDefaults;

	if (!pWorld || !g_bSceneValid || !mw_EnsureShaders())
		return;
	if (!MTLDev_EnsureFrame())
		return;
	id<MTLRenderCommandEncoder> enc =
		(__bridge id<MTLRenderCommandEncoder>)MTLDev_Encoder();
	if (!enc)
		return;

	// --- transforms ---
	float aModelView[16];
	if (cP.m_pModelMatrix)
		mtl_Mul(aModelView, g_aView, cP.m_pModelMatrix);
	else
		memcpy(aModelView, g_aView, sizeof(aModelView));

	MWUniforms cU;
	mtl_Mul(cU.m_aMVP, g_aProj, aModelView);
	memcpy(cU.m_aMV, aModelView, sizeof(aModelView));
	cU.m_aDynPos[0] = cU.m_aDynPos[1] = cU.m_aDynPos[2] = 0.0f;
	cU.m_aDynPos[3] = 1.0f;
	cU.m_aDynColor[0] = cU.m_aDynColor[1] = cU.m_aDynColor[2] = cU.m_aDynColor[3] = 0.0f;
	if (cP.m_pDynLight)
	{
		cU.m_aDynPos[0]   = cP.m_pDynLight->m_vPos.x;
		cU.m_aDynPos[1]   = cP.m_pDynLight->m_vPos.y;
		cU.m_aDynPos[2]   = cP.m_pDynLight->m_vPos.z;
		cU.m_aDynPos[3]   = cP.m_pDynLight->m_fRadius;
		cU.m_aDynColor[0] = cP.m_pDynLight->m_vColor.x;
		cU.m_aDynColor[1] = cP.m_pDynLight->m_vColor.y;
		cU.m_aDynColor[2] = cP.m_pDynLight->m_vColor.z;
		cU.m_aDynColor[3] = 1.0f;
	}

	if (!RWorld_GetEnvMatrix(cU.m_aEnvMtx))
		mtl_Identity(cU.m_aEnvMtx);
	cU.m_aSecParams[0] = cU.m_aSecParams[1] = 1.0f;
	cU.m_aSecParams[2] = cU.m_aSecParams[3] = 0.0f;   // [3] = second-layer kind
	memcpy(cU.m_aFogColor, g_aFogColor, sizeof(g_aFogColor));
	cU.m_aFogRange[0] = g_fFogNear;
	cU.m_aFogRange[1] = g_fFogFar;
	cU.m_fObjectAlpha = (float)cP.m_nObjectAlpha / 255.0f;

	// --- fixed per-call state ---
	// ★★★ WINDING — MEASURED, AND IT IS **NOT** FLIPPED RELATIVE TO GL.
	//
	// I reasoned in §84 that Metal decides facing in framebuffer coordinates
	// (origin top-left) where GL uses window coordinates (origin bottom-left),
	// so the same triangle would read CCW in one and CW in the other. THAT WAS
	// WRONG. Metal's winding is evaluated in the same sense GL's is, so the GL
	// path's glFrontFace(GL_CW) maps straight to MTLWindingClockwise.
	//
	// How it was settled, after the wrong version rendered a world that LOOKED
	// fine: the SKYBOX is the case that cannot hide it -- it is viewed from
	// inside, so getting the sense backwards culls all six faces and the sky
	// goes black while the outdoor world still looks plausible. Measured at the
	// same wall-clock second against the GL frame:
	//     MTLWindingClockwise         mean abs diff  3.19   <-- correct
	//     MTLWindingCounterClockwise  mean abs diff 22.22
	// ⇒ AN OUTDOOR SCENE IS A BAD CULLING TEST. Use closed geometry.
	// LT_MTLW_WINDING=cw|ccw re-runs that A/B; LT_WORLD_NOCULL=1 draws
	// two-sided, which is what first showed the sky was being culled.
	static int s_nNoCull = -1;
	if (s_nNoCull < 0) s_nNoCull = getenv("LT_WORLD_NOCULL") ? 1 : 0;
	// LT_MTLW_WINDING=cw|ccw overrides it for the A/B that settles this.
	static int s_nWinding = -1;
	if (s_nWinding < 0)
	{
		const char *p = getenv("LT_MTLW_WINDING");
		s_nWinding = (p && p[0] == 'c' && p[1] == 'c') ? 1 : 0;   // default CW
	}
	if (s_nNoCull || cP.m_pDynLight)   // GL disables culling for the light pass
		[enc setCullMode:MTLCullModeNone];
	else
	{
		[enc setFrontFacingWinding:(s_nWinding == 1) ? MTLWindingCounterClockwise
		                                             : MTLWindingClockwise];
		[enc setCullMode:MTLCullModeBack];
	}
	[enc setDepthStencilState:g_aDepth[cP.m_bDepthTest ? (cP.m_bDepthEqual ? 2 : 1) : 0]
	                           [cP.m_bDepthWrite ? 1 : 0]];

	static int s_nNoAlphaTest = -1;
	if (s_nNoAlphaTest < 0)
	{
		const char *p = getenv("LT_NO_ALPHATEST");
		s_nNoAlphaTest = (p && p[0] && p[0] != '0') ? 1 : 0;
	}
	static int s_nDrawNoTex = -1;
	if (s_nDrawNoTex < 0) s_nDrawNoTex = getenv("LT_DRAW_NOTEX") ? 1 : 0;

	const int nSaturate = RWorld_SaturateOn() ? 1 : 0;

	for (size_t nBlock = 0; nBlock < pWorld->m_aBlocks.size(); ++nBlock)
	{
		const RWBlock &cBlock = pWorld->m_aBlocks[nBlock];

		// Sphere-vs-AABB reject, identical to rw_LightWorldBlocks'.
		if (cP.m_pDynLight)
		{
			const LTVector vMin = cBlock.m_vCenter - cBlock.m_vHalfDims;
			const LTVector vMax = cBlock.m_vCenter + cBlock.m_vHalfDims;
			const LTVector &vL  = cP.m_pDynLight->m_vPos;
			LTVector vClamped(LTCLAMP(vL.x, vMin.x, vMax.x),
			                  LTCLAMP(vL.y, vMin.y, vMax.y),
			                  LTCLAMP(vL.z, vMin.z, vMax.z));
			if ((vClamped - vL).MagSqr() >
			    cP.m_pDynLight->m_fRadius * cP.m_pDynLight->m_fRadius)
				continue;
		}

		MWBlockGPU *pGPU = mw_BlockGPU(cBlock);
		if (!pGPU)
			continue;

		bool bBoundBlock = false;

		for (size_t nSection = 0; nSection < cBlock.m_aSections.size(); ++nSection)
		{
			const RWSection &cSection = cBlock.m_aSections[nSection];

			// Invisible / helper geometry (§4's shader codes).
			if (cSection.m_nShaderCode == kPCShader_None ||
			    cSection.m_nShaderCode == kPCShader_SkyPortal ||
			    cSection.m_nShaderCode == kPCShader_Occluder)
				continue;

			id<MTLTexture> baseTex = cSection.m_pTexture
				? (__bridge id<MTLTexture>)MTLTex_Get(cSection.m_pTexture) : nil;
			id<MTLTexture> lmTex   = mw_Lightmap(cSection.m_hLMTexture);

			// A lightmapped section whose base texture never resolved would
			// otherwise draw its bare lightmap -- a flat glowing panel, which is
			// much further from retail than drawing nothing (§14's refinement;
			// a GOURAUD section with a failed texture is still drawn, as solid
			// vertex colour, because retail does that and the Siberia sky dome
			// depends on it).
			// ⚠️ NOT ON THE LIGHT PASS. rw_LightWorldBlocks filters only shader
			// codes 0/6/7 — it has no LightAnim skip, so those sections DO
			// receive dynamic light even though the base pass declines to draw
			// their bare lightmap (§14). Applying the base rule here left them
			// unlit under Metal.
			if (!baseTex && lmTex && !s_nDrawNoTex && !cP.m_pDynLight)
				continue;

			int nMode;
			if (cP.m_pDynLight)
			{
				// texture x (light colour * attenuation), exactly GL's MODULATE
				// against the per-vertex colour. Never the lightmap, and never
				// the Saturate 2x — this is an additive contribution, not the
				// surface's own lighting.
				nMode = baseTex ? 0 : 3;
			}
			else if (baseTex) nMode = lmTex ? 1 : 0;
			else if (lmTex)   nMode = 2;
			else              nMode = 3;

			// One-shot census of what each section actually resolved to. "The
			// world is too dark" and "the lightmaps never bound" look identical
			// in a screenshot; this says which.
			if (mw_Trace())
			{
				static uint32 s_aModes[4] = { 0, 0, 0, 0 };
				static uint32 s_nLMHandles = 0, s_nTotal = 0;
				++s_aModes[nMode];
				++s_nTotal;
				if (cSection.m_hLMTexture)
					++s_nLMHandles;
				if (s_nTotal == 3000)
					fprintf(stderr, "[mtlw] section census over %u draws: gouraud=%u "
					                "lightmap=%u lmonly=%u untextured=%u | sections carrying an "
					                "LM HANDLE=%u (table size %u)\n",
					        s_nTotal, s_aModes[0], s_aModes[1], s_aModes[2], s_aModes[3],
					        s_nLMHandles, (uint32)g_aLightmaps.size());
			}

			// Saturate applies to the two textured-with-lighting modes only;
			// a bare lightmap (mode 2) is 1x on the GL path too.
			const int nSat = (cP.m_pDynLight) ? 0
			               : ((nMode == 0 || nMode == 1) ? nSaturate : 0);

			// ★ THE SECOND LAYER (§59 env map / §60 detail). Resolved ONLY through
			// RWorld_ResolveSecondLayer, which applies the console variables and
			// the LT_NO_ENVMAP / LT_NO_DETAIL gates — reading the authored texture
			// links directly is exactly §89's bug (see world_renderdata.h).
			// ⚠️ Eligibility for the EnvMapAlpha variant follows the section's
			// authored SHADER CODE, not whether a lightmap happened to resolve.
			// ★ THE SECOND TEXTURE LAYER (§59 env map / §60 detail), ON by
			// default. Resolved ONLY through RWorld_ResolveSecondLayer, which
			// applies every console variable and the LT_NO_ENVMAP /
			// LT_NO_DETAIL gates to BOTH backends — those two are also the
			// bisection switch, so this needs no separate one.
			// ⚠️ Eligibility for the EnvMapAlpha variant follows the section's
			// authored SHADER CODE, not whether a lightmap happened to resolve.
			int nSecKind = 0, nSecMode = 0;
			cU.m_aSecParams[3] = 0.0f;
			id<MTLTexture> secTex = nil;
			bool bSecCube = false;
			RWSecondLayer cSecond;
			if (baseTex && nMode != 2 && !cP.m_pDynLight &&
			    RWorld_ResolveSecondLayer(cSection,
			        cSection.m_nShaderCode == 4 || cSection.m_nShaderCode == 2 ||
			        cSection.m_nShaderCode == 9, &cSecond))
			{
				secTex = (__bridge id<MTLTexture>)MTLTex_Get(cSecond.m_pTex);
				if (secTex)
				{
					bSecCube = cSecond.m_bCube;
					nSecKind = (cSecond.m_eKind == kRWSecond_Detail) ? 3
					         : (bSecCube ? 2 : 1);
					nSecMode = (int)cSecond.m_eMode;
					cU.m_aSecParams[0] = (cSecond.m_eKind == kRWSecond_Detail)
					                   ? cSecond.m_fScale : cSecond.m_fEnvScale;
					cU.m_aSecParams[1] = cSecond.m_fCos;
					cU.m_aSecParams[2] = cSecond.m_fSin;
					cU.m_aSecParams[3] = (float)nSecKind;   // the vertex stage reads this
				}
			}

			id<MTLRenderPipelineState> pso =
				mw_Pipeline(nMode, nSat, (int)cP.m_eBlend, nSecKind, nSecMode);
			if (!pso)
				continue;

			// The AUTHORED alpha-test reference (DTX "AlphaRef <n>"), 0 =
			// ALPHAREF_NONE = do not test. Content-based guessing is gone (§13).
			float fAlphaRef = 0.0f;
			// ⚠️⚠️ THE LIGHT PASS ALPHA-TESTS UNCONDITIONALLY (§80). It must
			// honour the same authored cutout as the base pass or it lights the
			// WHOLE foliage quad — firing in C03S01 lit the woods as a field of
			// glowing white rectangles. Deliberately NOT gated on
			// m_bAllowAlphaTest or LT_NO_ALPHATEST: that switch exists to isolate
			// the BASE pass, and honouring it here brings the glow back.
			const bool bTestAlpha = cP.m_pDynLight
			                      ? (baseTex != nil)
			                      : (baseTex && cP.m_bAllowAlphaTest && !s_nNoAlphaTest);
			if (bTestAlpha)
			{
				unsigned int nRef = MTLTex_GetAlphaRef(cSection.m_pTexture);
				if (nRef)
					fAlphaRef = (float)nRef / 255.0f;
			}
			// FLAG2_FORCETRANSLUCENT (the car wheels, §15): discard only the
			// FULLY transparent texels -- glAlphaFunc(GL_GREATER, 0).
			if (cP.m_bDiscardZeroAlpha && fAlphaRef == 0.0f)
				fAlphaRef = 1.0f / 512.0f;
			cU.m_fAlphaRef = fAlphaRef;

			if (!bBoundBlock)
			{
				[enc setVertexBuffer:pGPU->m_Vtx offset:0 atIndex:0];
				[enc setVertexBuffer:pGPU->m_Col offset:0 atIndex:1];
				bBoundBlock = true;
			}
			[enc setRenderPipelineState:pso];
			[enc setVertexBytes:&cU length:sizeof(cU) atIndex:2];
			[enc setFragmentBytes:&cU length:sizeof(cU) atIndex:0];
			[enc setFragmentTexture:(baseTex ? baseTex : g_WhiteTex) atIndex:0];
			[enc setFragmentTexture:(lmTex   ? lmTex   : g_WhiteTex) atIndex:1];
			[enc setFragmentTexture:((secTex && !bSecCube) ? secTex : g_WhiteTex)  atIndex:2];
			[enc setFragmentTexture:((secTex &&  bSecCube) ? secTex : g_WhiteCube) atIndex:3];
			// ⚠️ An env map CLAMPS (a reflection addresses the map once); a detail
			// texture REPEATS many times across the surface.
			{
				// Detail: the texture's own sampler (repeat + mipmapped).
				// Env: clamped, still mipmapped.
				id<MTLSamplerState> secSmp = g_RepeatSmp;
				if (secTex && nSecKind == 3)
				{
					void *p = MTLTex_Sampler(cSecond.m_pTex);
					if (p) secSmp = (__bridge id<MTLSamplerState>)p;
				}
				else if (secTex && nSecKind == 2)
				{
					void *p = MTLTex_Sampler(cSecond.m_pTex);   // cube: clamps all axes
					if (p) secSmp = (__bridge id<MTLSamplerState>)p;
				}
				[enc setFragmentSamplerState:secSmp atIndex:2];
			}
			id<MTLSamplerState> baseSmp = cSection.m_pTexture
				? (__bridge id<MTLSamplerState>)MTLTex_Sampler(cSection.m_pTexture)
				: g_LMSampler;
			[enc setFragmentSamplerState:(baseSmp ? baseSmp : g_LMSampler) atIndex:0];
			[enc setFragmentSamplerState:g_LMSampler atIndex:1];

			if (MTLDev_TraceFrame())
			{
				const uint32 nV = cBlock.m_aIndices[cSection.m_nStartIndex];
				const RWVertex &cV = cBlock.m_aVertices[nV];
				const uint8 *pC = (nV * 3 + 2 < cBlock.m_aComposedColor.size())
				                ? &cBlock.m_aComposedColor[nV * 3] : NULL;
				fprintf(stderr, "[mtlf ] world blk%zu sec%zu shader=%u mode=%d sat=%d tris=%u "
				                "pos=(%.0f %.0f %.0f) vcol=(%u %u %u) alphaRef=%.2f\n",
				        nBlock, nSection, (unsigned)cSection.m_nShaderCode, nMode, nSat,
				        cSection.m_nTriCount, cV.m_vPos.x, cV.m_vPos.y, cV.m_vPos.z,
				        pC ? pC[0] : 255, pC ? pC[1] : 255, pC ? pC[2] : 255, fAlphaRef);
			}

			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                indexCount:cSection.m_nTriCount * 3
			                 indexType:MTLIndexTypeUInt32
			               indexBuffer:pGPU->m_Idx
			         indexBufferOffset:(NSUInteger)cSection.m_nStartIndex * sizeof(uint32)];

			++g_nDrawnSections;
			g_nDrawnTris += cSection.m_nTriCount;
		}
	}

	// A per-FRAME census, reported on change: "how much of the world reached
	// the encoder" is a property of the frame, and a per-section line would be
	// 1000+ lines a frame (§71's rule about scope).
	if (mw_Trace())
	{
		static uint32 s_nLastSections = 0xFFFFFFFFu;
		if (g_nDrawnSections != s_nLastSections)
		{
			s_nLastSections = g_nDrawnSections;
			fprintf(stderr, "[mtlw] drew %u sections / %u tris (blocks with GPU data: %u)\n",
			        g_nDrawnSections, g_nDrawnTris, (uint32)g_aBlockGPU.size());
		}
	}
	g_nDrawnSections = 0;
	g_nDrawnTris = 0;
}

// ==========================================================================
void MTLWorld_Free(void)
{
	for (size_t i = 0; i < g_aLightmaps.size(); ++i)
		if (g_aLightmaps[i])
			CFBridgingRelease(g_aLightmaps[i]);
	g_aLightmaps.clear();

	for (size_t i = 0; i < g_aBlockGPU.size(); ++i)
	{
		g_aBlockGPU[i].m_Vtx = nil;
		g_aBlockGPU[i].m_Col = nil;
		g_aBlockGPU[i].m_Idx = nil;
	}
	g_aBlockGPU.clear();
	g_bSceneValid = false;
}
