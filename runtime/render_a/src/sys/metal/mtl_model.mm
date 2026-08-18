// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_model.mm
//
// PURPOSE : See mtl_model.h. Turns one CPU-skinned, already-lit triangle list
//           into a Metal draw with the authored render-style state.
//
//           Models are DYNAMIC — every mesh is re-skinned on the CPU every
//           frame — so unlike the world (static per-block buffers) they stream
//           through a per-frame ring, exactly as the drawprim pass does.
//
//           ⚠️ NO BACKFACE CULLING, EVER. The game disables it for models
//           outright (model_renderdata.cpp: `glDisable(GL_CULL_FACE)`) because NOLF2's
//           model winding is inconsistent; any cull mode drops real triangles.
//           The world's single global winding rule (§85) does NOT carry over.
//
// ----------------------------------------------------------------------- //

#import <Metal/Metal.h>

#include "bdefs.h"
#include "de_world.h"
#include "renderstruct.h"
#include "sys/shared/model_renderdata.h"       // REmitState -- the shared authored state
#include "sys/shared/render_globals.h" // g_pRenderStruct (the engine function table)
#include "mtl_device.h"
#include "mtl_matrix.h"
#include "mtl_texture.h"
#include "mtl_model.h"
#include "sys/shared/world_renderdata.h"   // RWorld_GetEnvMatrix
#include "mtl_world.h"   // MTLWorld_GetFog -- one fog state for every scene pass
#include "sys/shared/render_texture.h"  // RTex_* — neutral texture queries

#include "sys/shared/render_state.h" // kRBlend_*/kRAlpha_* — the neutral draw
                                    // description render styles resolve to (was <OpenGL/gl.h>)
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>

// ==========================================================================
// The shader.
//
// Everything expensive already happened on the CPU: positions are skinned,
// the colour is ambient + SUM(light x N.L) with all four D3D terms resolved.
// What is left is the authored TEXTURE STAGE — and it has two independent
// halves that a fixed-function port keeps wanting to conflate:
//   * the COLOUR op   -- does the diffuse touch the texture, and at what scale
//   * the ALPHA op    -- does alpha come from the TEXTURE or from the OBJECT
// Getting the second wrong deletes faces: alpha in a NOLF2 skin is frequently
// an environment mask, not opacity (CateCasualHead.dtx is 76.9% below 128).
// ==========================================================================
static const char *kModelMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct MVSIn {
    float3 pos    [[attribute(0)]];
    float2 uv     [[attribute(1)]];
    float4 color  [[attribute(2)]];
    float3 normal [[attribute(3)]];
};

struct MVSOut {
    float4 pos [[position]];
    float2 uv;
    float4 color;
    float3 env;     // reflection coords: 2D uses .xy, cube uses .xyz
    // ⚠️ SIGNED EYE Z, not a per-vertex factor -- the factor is built per
    // fragment. Same defect as the world shader (§103): abs() per vertex is
    // wrong for any triangle with a vertex behind the eye, and the PLAYER-VIEW
    // weapon is the closest geometry in the game.
    float  eyeZ;
};

struct MUniforms {
    float4x4 mvp;
    float4x4 mv;
    float4   fogColor;    // rgb, w = enable
    float2   fogRange;    // near, far
    float    colorScale;
    float    alphaRef;
    int      alphaFunc;   // 0 none, 1 <, 2 <=, 3 >, 4 >=, 5 ==, 6 !=
    int      flattenDepth;
    float    flatNDCz;
    int      pad0;
    float4x4 envMtx;      // camera->world rotation (columns R, U, -F)
};

vertex MVSOut m_vertex(MVSIn in [[stage_in]],
                       constant MUniforms &u [[buffer(1)]])
{
    MVSOut o;
    float4 p = float4(in.pos, 1.0);
    o.pos   = u.mvp * p;
    o.uv    = in.uv;
    o.color = in.color;

    // NoZ sprite: flatten the quad to its centre's depth so the depth unit
    // performs the occlusion test the GL path did with a readback.
    if (u.flattenDepth != 0)
        o.pos.z = u.flatNDCz * o.pos.w;

    // ★ THE POLYGRID'S REFLECTION. Same eye-space reflection vector GL's
    // GL_REFLECTION_MAP emits, rotated into world space by envMtx.
    // ⚠️ A 2D lookup projects **XZ** here (u = 0.5*Rw.x + .5, v = 0.5*Rw.z + .5)
    // — the WORLD's 2D env projects XY (§59 says explicitly not to copy one to
    // the other). A cube map skips the projection entirely and takes all three.
    o.env = float3(0.0);
    if (u.envMtx[3][3] > 0.5) {
        float3 pe = (u.mv * p).xyz;
        float3 ne = normalize((u.mv * float4(in.normal, 0.0)).xyz);
        float3 ee = normalize(pe);
        float3 r  = ee - 2.0 * ne * dot(ne, ee);
        float3 rw = (u.envMtx * float4(r, 0.0)).xyz;
        o.env = (u.envMtx[3][3] > 1.5) ? rw
              : float3(0.5 * rw.x + 0.5, 0.5 * rw.z + 0.5, 0.0);
    }

    // The same GL_LINEAR range fog the world pass applies -- under GL this was
    // simply glEnable(GL_FOG) still being on when the models drew.
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
    // See the world shader: interpolate SIGNED eye z (exact -- it is affine in
    // eye space) and take abs() per fragment, which is what GL_FOG_HINT/
    // GL_NICEST gives the GL path.
    o.eyeZ = (u.mv * p).z;
    return o;
}

// Specialised on the two authored ops; the alpha COMPARISON stays a uniform
// because render styles use several functions and the combination count would
// otherwise multiply out for no benefit.
constant int kIgnoreDiffuse [[function_constant(0)]];   // colour = texture alone
constant int kTextureAlpha  [[function_constant(1)]];   // alpha from texture vs object
constant int kTextured      [[function_constant(2)]];
// 0 none, 1 env 2D (XZ projection), 2 env CUBE
constant int kEnvKind       [[function_constant(3)]];

fragment float4 m_fragment(MVSOut in [[stage_in]],
                           constant MUniforms &u      [[buffer(0)]],
                           texture2d<float>   tex     [[texture(0)]],
                           texture2d<float>   envTex  [[texture(1)]],
                           texturecube<float> envCube [[texture(2)]],
                           sampler            smp     [[sampler(0)]],
                           sampler            envSmp  [[sampler(1)]])
{
    float4 c;
    if (kTextured == 0) {
        c = in.color;
    } else {
        float4 t = tex.sample(smp, in.uv);
        float3 rgb = (kIgnoreDiffuse != 0) ? t.rgb
                                           : t.rgb * in.color.rgb * u.colorScale;
        float  a   = (kTextureAlpha  != 0) ? t.a * in.color.a : in.color.a;
        c = float4(rgb, a);
    }

    // Stage 1 of the polygrid: MODULATE(reflection, previous), x2 when a base
    // texture sits underneath. ⚠️ The ALPHA is the vertex alpha only — the
    // reflection's own alpha must never reach it, or an env map with an alpha
    // channel silently dims the water.
    if (kEnvKind != 0) {
        float3 e = (kEnvKind == 2) ? envCube.sample(envSmp, in.env).rgb
                                   : envTex.sample(envSmp, in.env.xy).rgb;
        float s = (kTextured != 0) ? 2.0 : 1.0;
        c = float4(saturate(c.rgb * e * s), in.color.a);
    }

    if (u.alphaFunc != 0) {
        bool bPass = true;
        if      (u.alphaFunc == 1) bPass = (c.a <  u.alphaRef);
        else if (u.alphaFunc == 2) bPass = (c.a <= u.alphaRef);
        else if (u.alphaFunc == 3) bPass = (c.a >  u.alphaRef);
        else if (u.alphaFunc == 4) bPass = (c.a >= u.alphaRef);
        else if (u.alphaFunc == 5) bPass = (c.a == u.alphaRef);
        else                       bPass = (c.a != u.alphaRef);
        if (!bPass)
            discard_fragment();
    }

    // Clamp before fogging: fixed-function GL clamps the fragment colour to
    // [0,1] at the end of the texture-environment stage, so its fog stage always
    // mixes against a colour <= 1, and the world renders at 2x saturate (§27).
    // ⚠️ MEASURED, AND IT CHANGED NOTHING -- the c01s01 orbit frames are
    // BIT-IDENTICAL with and without the saturate. It was written as a candidate
    // explanation for the fog-shaped difference and is NOT one. Kept because it
    // is the correct semantics, not because it fixed anything (the same honesty
    // the per-vertex fog comment above already owed the reader).
    const float fFogZ = abs(in.eyeZ);
    const float fFogF = (u.fogColor.w > 0.5)
                      ? (u.fogRange.y - fFogZ) / max(u.fogRange.y - u.fogRange.x, 1e-3)
                      : 1.0;
    c.rgb = mix(u.fogColor.rgb, saturate(c.rgb), clamp(fFogF, 0.0, 1.0));
    return c;
}
)MSL";

// ==========================================================================
struct MMUniforms
{
	float m_aMVP[16];
	float m_aMV[16];
	float m_aFogColor[4];
	float m_aFogRange[2];
	float m_fColorScale;
	float m_fAlphaRef;
	int   m_nAlphaFunc;
	int   m_nFlattenDepth;
	float m_fFlatNDCz;
	int   m_nPad0;
	float m_aEnvMtx[16];
};

// ⚠️ Assert every offset of a shader-shared struct; never hand-pad one blind.
// A mismatch does not error anywhere -- the shader just reads later fields from
// the wrong place, and it looks like a rendering bug (the CharacterViewer port
// lost a session to exactly this).
static_assert(offsetof(MMUniforms, m_aMVP)        ==   0, "MUniforms.mvp moved");
static_assert(offsetof(MMUniforms, m_aMV)         ==  64, "MUniforms.mv moved");
static_assert(offsetof(MMUniforms, m_aFogColor)   == 128, "MUniforms.fogColor moved");
static_assert(offsetof(MMUniforms, m_aFogRange)   == 144, "MUniforms.fogRange moved");
static_assert(offsetof(MMUniforms, m_fColorScale) == 152, "MUniforms.colorScale moved");
static_assert(offsetof(MMUniforms, m_fAlphaRef)   == 156, "MUniforms.alphaRef moved");
static_assert(offsetof(MMUniforms, m_nAlphaFunc)    == 160, "MUniforms.alphaFunc moved");
static_assert(offsetof(MMUniforms, m_nFlattenDepth) == 164, "MUniforms.flattenDepth moved");
static_assert(offsetof(MMUniforms, m_fFlatNDCz)     == 168, "MUniforms.flatNDCz moved");
static_assert(offsetof(MMUniforms, m_aEnvMtx)       == 176, "MUniforms.envMtx moved");
static_assert(sizeof(MMUniforms)                    == 240, "MUniforms size changed");

static id<MTLLibrary>         g_Library    = nil;
static id<MTLFunction>        g_VertexFn   = nil;
static MTLVertexDescriptor*   g_VertexDesc = nil;
static id<MTLTexture>         g_WhiteTex   = nil;
static id<MTLTexture>         g_WhiteCube  = nil;
static id<MTLSamplerState>    g_WhiteSmp   = nil;
static id<MTLSamplerState>    g_ClampSmp   = nil;   // reflection lookups clamp
static bool                   g_bReady     = false;
static bool                   g_bFailed    = false;

// [zTest][zWrite]
static id<MTLDepthStencilState> g_aDepth[2][2] = { { nil, nil }, { nil, nil } };

// PSO cache. The key space here is not as tidily bounded as the world's (a
// render style can author any src/dst blend pair), so this one is a map.
static std::map<uint64_t, CFTypeRef> g_cPipelines;

static float g_aView[16], g_aProj[16];
static bool  g_bTransformValid = false;

// The per-frame vertex ring, same shape as the drawprim pass's.
struct MMRingSlot
{
	id<MTLBuffer> m_Buffer;
	NSUInteger    m_nOffset;
	NSUInteger    m_nCapacity;
};
static MMRingSlot g_aRing[MTLDEV_FRAMES_IN_FLIGHT];
static int        g_nLastSlot = -1;
static NSUInteger g_nRingHighWater = 0;
static const NSUInteger kMMRingInitial = 1024 * 1024;
static const NSUInteger kMMAlign       = 256;

static bool mm_Trace(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_TRACE_MTLMODEL") ? 1 : 0;
	return s_n != 0;
}

// ==========================================================================
// GL enum -> Metal
// ==========================================================================
static MTLBlendFactor mm_BlendFactor(uint32 nFactor)
{
	switch (nFactor)
	{
		case kRBlend_Zero:              return MTLBlendFactorZero;
		case kRBlend_One:               return MTLBlendFactorOne;
		case kRBlend_SrcColor:          return MTLBlendFactorSourceColor;
		case kRBlend_InvSrcColor:       return MTLBlendFactorOneMinusSourceColor;
		case kRBlend_DstColor:          return MTLBlendFactorDestinationColor;
		case kRBlend_InvDstColor:       return MTLBlendFactorOneMinusDestinationColor;
		case kRBlend_SrcAlpha:          return MTLBlendFactorSourceAlpha;
		case kRBlend_InvSrcAlpha:       return MTLBlendFactorOneMinusSourceAlpha;
		case kRBlend_DstAlpha:          return MTLBlendFactorDestinationAlpha;
		case kRBlend_InvDstAlpha:       return MTLBlendFactorOneMinusDestinationAlpha;
		case kRBlend_SrcAlphaSaturate:  return MTLBlendFactorSourceAlphaSaturated;
		default:                        return MTLBlendFactorOne;
	}
}

// The authored alpha comparison -> the shader's alphaFunc code. 0 = no test.
static int mm_AlphaFunc(uint32 nFunc)
{
	switch (nFunc)
	{
		case kRAlpha_Less:      return 1;
		case kRAlpha_LEqual:    return 2;
		case kRAlpha_Greater:   return 3;
		case kRAlpha_GEqual:    return 4;
		case kRAlpha_Equal:     return 5;
		case kRAlpha_NotEqual:  return 6;
		case kRAlpha_Always:    return 0;
		// kRAlpha_Never would discard everything; nothing authors it, and treating it
		// as "no test" would be a silent lie -- clamp to "fails" via >= 2.
		case kRAlpha_Never:     return 3;   // a > 1.0 test never passes
		default:          return 0;
	}
}

// ==========================================================================
static bool mm_EnsureShaders(void)
{
	if (g_bReady)  return true;
	if (g_bFailed) return false;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev)
		return false;

	NSError *err = nil;
	g_Library = [dev newLibraryWithSource:[NSString stringWithUTF8String:kModelMSL]
	                              options:nil error:&err];
	if (!g_Library)
	{
		g_bFailed = true;
		fprintf(stderr, "[mtlm] model MSL compile FAILED: %s\n",
		        err ? [[err localizedDescription] UTF8String] : "(no error)");
		return false;
	}
	g_VertexFn = [g_Library newFunctionWithName:@"m_vertex"];
	if (!g_VertexFn)
	{
		g_bFailed = true;
		fprintf(stderr, "[mtlm] m_vertex missing from the compiled library\n");
		return false;
	}

	g_VertexDesc = [[MTLVertexDescriptor alloc] init];
	g_VertexDesc.attributes[0].format      = MTLVertexFormatFloat3;
	g_VertexDesc.attributes[0].offset      = 0;
	g_VertexDesc.attributes[0].bufferIndex = 0;
	g_VertexDesc.attributes[1].format      = MTLVertexFormatFloat2;
	g_VertexDesc.attributes[1].offset      = 12;
	g_VertexDesc.attributes[1].bufferIndex = 0;
	g_VertexDesc.attributes[2].format      = MTLVertexFormatUChar4Normalized;
	g_VertexDesc.attributes[2].offset      = 20;
	g_VertexDesc.attributes[2].bufferIndex = 0;
	g_VertexDesc.attributes[3].format      = MTLVertexFormatFloat3;   // normal
	g_VertexDesc.attributes[3].offset      = 24;
	g_VertexDesc.attributes[3].bufferIndex = 0;
	g_VertexDesc.layouts[0].stride         = sizeof(MTLModelVert);
	g_VertexDesc.layouts[0].stepFunction   = MTLVertexStepFunctionPerVertex;

	{
		MTLTextureDescriptor *d =
			[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                   width:1 height:1 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		g_WhiteTex = [dev newTextureWithDescriptor:d];
		const uint8 aWhite[4] = { 255, 255, 255, 255 };
		[g_WhiteTex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
		              mipmapLevel:0 withBytes:aWhite bytesPerRow:4];
		MTLSamplerDescriptor *s = [[MTLSamplerDescriptor alloc] init];
		s.minFilter = MTLSamplerMinMagFilterLinear;
		s.magFilter = MTLSamplerMinMagFilterLinear;
		g_WhiteSmp  = [dev newSamplerStateWithDescriptor:s];
		s.mipFilter    = MTLSamplerMipFilterLinear;
		s.sAddressMode = MTLSamplerAddressModeClampToEdge;
		s.tAddressMode = MTLSamplerAddressModeClampToEdge;
		g_ClampSmp     = [dev newSamplerStateWithDescriptor:s];

		MTLTextureDescriptor *dc =
			[MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                      size:1 mipmapped:NO];
		dc.usage = MTLTextureUsageShaderRead;
		g_WhiteCube = [dev newTextureWithDescriptor:dc];
		const uint8 aW[4] = { 255, 255, 255, 255 };
		for (NSUInteger f = 0; f < 6; ++f)
			[g_WhiteCube replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
			               mipmapLevel:0 slice:f withBytes:aW bytesPerRow:4 bytesPerImage:4];
	}

	for (int nTest = 0; nTest < 2; ++nTest)
		for (int nWrite = 0; nWrite < 2; ++nWrite)
		{
			MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
			// LESS, matching the rest of the renderer -- the GL model path never
			// calls glDepthFunc either, so it runs on GL's default (§86).
			d.depthCompareFunction = nTest ? MTLCompareFunctionLess
			                               : MTLCompareFunctionAlways;
			d.depthWriteEnabled    = nWrite ? YES : NO;
			g_aDepth[nTest][nWrite] = [dev newDepthStencilStateWithDescriptor:d];
		}

	g_bReady = true;
	fprintf(stderr, "[mtlm] model MSL compiled at runtime\n");
	return true;
}

static id<MTLRenderPipelineState> mm_Pipeline(bool bTextured, bool bIgnoreDiffuse,
                                              bool bTextureAlpha, bool bBlend,
                                              uint32 nSrc, uint32 nDst, int nEnvKind)
{
	const uint64_t nKey = (uint64_t)nEnvKind << 60
	                    | (uint64_t)(bTextured ? 1 : 0)
	                    | ((uint64_t)(bIgnoreDiffuse ? 1 : 0) << 1)
	                    | ((uint64_t)(bTextureAlpha ? 1 : 0)  << 2)
	                    | ((uint64_t)(bBlend ? 1 : 0)         << 3)
	                    | ((uint64_t)nSrc << 8) | ((uint64_t)nDst << 32);

	std::map<uint64_t, CFTypeRef>::iterator it = g_cPipelines.find(nKey);
	if (it != g_cPipelines.end())
		return (__bridge id<MTLRenderPipelineState>)it->second;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev)
		return nil;

	NSError *err = nil;
	MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues alloc] init];
	int nID = bIgnoreDiffuse ? 1 : 0, nTA = bTextureAlpha ? 1 : 0, nTex = bTextured ? 1 : 0;
	int nEK = nEnvKind;
	[cv setConstantValue:&nID  type:MTLDataTypeInt atIndex:0];
	[cv setConstantValue:&nTA  type:MTLDataTypeInt atIndex:1];
	[cv setConstantValue:&nTex type:MTLDataTypeInt atIndex:2];
	[cv setConstantValue:&nEK  type:MTLDataTypeInt atIndex:3];
	id<MTLFunction> fs = [g_Library newFunctionWithName:@"m_fragment"
	                                     constantValues:cv error:&err];
	if (!fs)
	{
		fprintf(stderr, "[mtlm] fragment specialisation failed: %s\n",
		        err ? [[err localizedDescription] UTF8String] : "(none)");
		return nil;
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	pd.vertexFunction   = g_VertexFn;
	pd.fragmentFunction = fs;
	pd.vertexDescriptor = g_VertexDesc;
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
	if (bBlend)
	{
		pd.colorAttachments[0].blendingEnabled     = YES;
		pd.colorAttachments[0].rgbBlendOperation   = MTLBlendOperationAdd;
		pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
		const MTLBlendFactor eS = mm_BlendFactor(nSrc), eD = mm_BlendFactor(nDst);
		pd.colorAttachments[0].sourceRGBBlendFactor        = eS;
		pd.colorAttachments[0].sourceAlphaBlendFactor      = eS;
		pd.colorAttachments[0].destinationRGBBlendFactor   = eD;
		pd.colorAttachments[0].destinationAlphaBlendFactor = eD;
	}

	id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!pso)
	{
		fprintf(stderr, "[mtlm] pipeline failed: %s\n",
		        err ? [[err localizedDescription] UTF8String] : "(none)");
		return nil;
	}
	g_cPipelines[nKey] = CFBridgingRetain(pso);
	if (mm_Trace())
		fprintf(stderr, "[mtlm] new pipeline tex=%d noDiffuse=%d texAlpha=%d blend=%d "
		                "src=0x%X dst=0x%X (cache %u)\n",
		        (int)bTextured, (int)bIgnoreDiffuse, (int)bTextureAlpha, (int)bBlend,
		        nSrc, nDst, (uint32)g_cPipelines.size());
	return pso;
}

// ==========================================================================
static bool mm_RingAlloc(const MTLModelVert *pVerts, NSUInteger nCount,
                         id<MTLBuffer> __strong &outBuf, NSUInteger &outOffset)
{
	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev || !nCount)
		return false;

	const int nSlot = MTLDev_FrameSlot();
	if (nSlot != g_nLastSlot)
	{
		g_nLastSlot = nSlot;
		g_aRing[nSlot].m_nOffset = 0;
	}
	MMRingSlot &cSlot = g_aRing[nSlot];

	const NSUInteger nBytes   = nCount * sizeof(MTLModelVert);
	const NSUInteger nAligned = (nBytes + kMMAlign - 1) & ~(kMMAlign - 1);

	if (!cSlot.m_Buffer || cSlot.m_nOffset + nAligned > cSlot.m_nCapacity)
	{
		NSUInteger nNeed = cSlot.m_nOffset + nAligned;
		NSUInteger nCap  = cSlot.m_nCapacity ? cSlot.m_nCapacity : kMMRingInitial;
		while (nCap < nNeed)
			nCap *= 2;
		id<MTLBuffer> newBuf = [dev newBufferWithLength:nCap
		                                        options:MTLResourceStorageModeShared];
		if (!newBuf)
			return false;
		// Dropping the old buffer mid-frame is safe: a command buffer retains
		// every resource encoded into it, so draws already issued keep reading
		// the buffer they were given.
		cSlot.m_Buffer    = newBuf;
		cSlot.m_nCapacity = nCap;
		cSlot.m_nOffset   = 0;
		if (mm_Trace())
			fprintf(stderr, "[mtlm] vertex ring slot %d grown to %lu KB\n",
			        nSlot, (unsigned long)(nCap / 1024));
	}

	outBuf    = cSlot.m_Buffer;
	outOffset = cSlot.m_nOffset;
	memcpy((uint8*)[cSlot.m_Buffer contents] + outOffset, pVerts, nBytes);
	cSlot.m_nOffset += nAligned;
	if (cSlot.m_nOffset > g_nRingHighWater)
		g_nRingHighWater = cSlot.m_nOffset;
	return true;
}

// ==========================================================================
void MTLModel_SetTransform(const float *pView16, const float *pProj16)
{
	if (!pView16 || !pProj16)
		return;
	memcpy(g_aView, pView16, sizeof(g_aView));
	memcpy(g_aProj, pProj16, sizeof(g_aProj));
	g_bTransformValid = true;
}

bool MTLModel_ProjectDepth(float x, float y, float z, float *pOutNDCz)
{
	if (!g_bTransformValid)
		return false;
	float aMVP[16];
	mtl_Mul(aMVP, g_aProj, g_aView);
	// Column-major: clip = MVP * (x,y,z,1).
	const float fZ = aMVP[2] * x + aMVP[6] * y + aMVP[10] * z + aMVP[14];
	const float fW = aMVP[3] * x + aMVP[7] * y + aMVP[11] * z + aMVP[15];
	if (fW <= 0.0001f)
		return false;
	if (pOutNDCz)
		*pOutNDCz = fZ / fW;
	return true;
}

void MTLModel_DrawTris(const MTLModelVert *pVerts, uint32_t nCount,
                       const REmitState *pState)
{
	static REmitState s_cDefaults;
	const REmitState &cS = pState ? *pState : s_cDefaults;

	if (!pVerts || nCount < 3 || !g_bTransformValid || !mm_EnsureShaders())
		return;
	if (!MTLDev_EnsureFrame())
		return;
	id<MTLRenderCommandEncoder> enc =
		(__bridge id<MTLRenderCommandEncoder>)MTLDev_Encoder();
	if (!enc)
		return;

	id<MTLTexture> tex = cS.m_pTexture
		? (__bridge id<MTLTexture>)MTLTex_Get(cS.m_pTexture) : nil;
	const bool bTextured = (tex != nil);

	int nEnvKind = 0;
	id<MTLTexture> envTex = nil;
	if (cS.m_pEnvMap)
	{
		envTex = (__bridge id<MTLTexture>)MTLTex_Get(cS.m_pEnvMap);
		if (envTex)
			nEnvKind = RTex_IsCubeMap(cS.m_pEnvMap) ? 2 : 1;
	}

	id<MTLRenderPipelineState> pso = mm_Pipeline(bTextured, cS.m_bIgnoreDiffuse,
	                                             cS.m_bTextureAlpha, cS.m_bBlend,
	                                             cS.m_nSrcBlend, cS.m_nDstBlend,
	                                             nEnvKind);
	if (!pso)
		return;

	MMUniforms cU;
	float aModelView[16];
	if (cS.m_pModelMatrix)
		mtl_Mul(aModelView, g_aView, cS.m_pModelMatrix);
	else
		memcpy(aModelView, g_aView, sizeof(g_aView));   // pre-transformed
	mtl_Mul(cU.m_aMVP, g_aProj, aModelView);
	memcpy(cU.m_aMV, aModelView, sizeof(aModelView));
	MTLWorld_GetFog(cU.m_aFogColor, &cU.m_aFogRange[0], &cU.m_aFogRange[1]);
	cU.m_fColorScale = cS.m_fColorScale;
	cU.m_fAlphaRef   = cS.m_fAlphaRef;
	cU.m_nAlphaFunc    = cS.m_bAlphaTest ? mm_AlphaFunc(cS.m_nAlphaFunc) : 0;
	cU.m_nFlattenDepth = cS.m_bFlattenDepth ? 1 : 0;
	cU.m_fFlatNDCz     = cS.m_fFlatNDCz;
	cU.m_nPad0         = 0;
	// envMtx[3][3] doubles as the enable/kind flag the vertex stage reads
	// (0 off, 1 = 2D XZ, 2 = cube) — function constants are fragment-scope only
	// and referencing them in the vertex stage forces it to specialise (§91).
	memset(cU.m_aEnvMtx, 0, sizeof(cU.m_aEnvMtx));
	if (nEnvKind)
	{
		RWorld_GetEnvMatrix(cU.m_aEnvMtx);   // columns R, U, -F
		cU.m_aEnvMtx[15] = (float)nEnvKind;
	}

	id<MTLBuffer> buf = nil;
	NSUInteger    nOffset = 0;
	if (!mm_RingAlloc(pVerts, nCount, buf, nOffset))
		return;

	[enc setRenderPipelineState:pso];
	[enc setDepthStencilState:g_aDepth[cS.m_bZTest ? 1 : 0][cS.m_bZWrite ? 1 : 0]];
	[enc setVertexBuffer:buf offset:nOffset atIndex:0];
	[enc setVertexBytes:&cU length:sizeof(cU) atIndex:1];
	[enc setFragmentBytes:&cU length:sizeof(cU) atIndex:0];
	[enc setFragmentTexture:(tex ? tex : g_WhiteTex) atIndex:0];
	[enc setFragmentTexture:((envTex && nEnvKind == 1) ? envTex : g_WhiteTex)  atIndex:1];
	[enc setFragmentTexture:((envTex && nEnvKind == 2) ? envTex : g_WhiteCube) atIndex:2];
	{
		// A reflection lookup must CLAMP: the coords leave 0..1 wherever the
		// reflection points away from the mapped hemisphere.
		id<MTLSamplerState> eSmp = g_ClampSmp;
		if (envTex)
		{
			void *p = MTLTex_Sampler(cS.m_pEnvMap);
			if (p && nEnvKind == 2) eSmp = (__bridge id<MTLSamplerState>)p;
		}
		[enc setFragmentSamplerState:eSmp atIndex:1];
	}
	{
		id<MTLSamplerState> smp = cS.m_pTexture
			? (__bridge id<MTLSamplerState>)MTLTex_Sampler(cS.m_pTexture) : nil;
		[enc setFragmentSamplerState:(smp ? smp : g_WhiteSmp) atIndex:0];
	}

	// ⚠️ TWO-SIDED, ALWAYS. See the file header: NOLF2 model winding is
	// inconsistent and the game itself culls nothing here.
	[enc setCullMode:MTLCullModeNone];

	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:nCount];
}

// ==========================================================================
void MTLModel_Term(void)
{
	for (std::map<uint64_t, CFTypeRef>::iterator it = g_cPipelines.begin();
	     it != g_cPipelines.end(); ++it)
		if (it->second)
			CFBridgingRelease(it->second);
	g_cPipelines.clear();

	for (int i = 0; i < MTLDEV_FRAMES_IN_FLIGHT; ++i)
	{
		g_aRing[i].m_Buffer    = nil;
		g_aRing[i].m_nCapacity = 0;
		g_aRing[i].m_nOffset   = 0;
	}
	g_nLastSlot = -1;

	if (mm_Trace())
		fprintf(stderr, "[mtlm] vertex ring high-water %lu KB\n",
		        (unsigned long)(g_nRingHighWater / 1024));

	g_VertexFn = nil; g_Library = nil; g_VertexDesc = nil;
	g_WhiteTex = nil; g_WhiteSmp = nil; g_WhiteCube = nil; g_ClampSmp = nil;
	for (int t = 0; t < 2; ++t)
		for (int w = 0; w < 2; ++w)
			g_aDepth[t][w] = nil;
	g_bReady = false;
	g_bTransformValid = false;
}
