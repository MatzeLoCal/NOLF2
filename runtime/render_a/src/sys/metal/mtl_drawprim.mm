// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_drawprim.mm
//
// PURPOSE : See mtl_drawprim.h. The Metal 2D/immediate draw path.
//
//           Three things this file exists to establish, because every later
//           pass (world, models, particles) will reuse them:
//
//           1. THE MSL LIBRARY IS COMPILED AT RUNTIME from the string below
//              (newLibraryWithSource:). This is the pattern CharacterViewer
//              proved, and it keeps Metal shader compilation out of the CMake
//              build entirely -- no .metal files, no metallib to ship or sign.
//           2. A PIPELINE-STATE CACHE keyed on (blend, colour op, alpha test).
//              The survey (§82) counted 10 distinct glBlendFunc combinations
//              and a small bounded combiner set across the whole renderer, so
//              the cache is a flat array, not a hash.
//           3. A DYNAMIC VERTEX BUFFER -- a triple-buffered ring with a bump
//              allocator, per the user's "proper vertex buffers from the
//              start, no immediate-mode emulation shim".
//
//           ⚠️ METAL HAS NO ALPHA TEST and no GL_QUADS and no triangle FAN.
//           The alpha test becomes a discard in the fragment shader; quads and
//           fans are expanded to triangle lists on the way into the ring.
//
// ----------------------------------------------------------------------- //

#import <Metal/Metal.h>

#include "bdefs.h"
#include "renderstruct.h"
#include "dtxmgr.h"
#include "de_world.h"
#include "ltmacwindow.h"
#include "sys/shared/render_globals.h" // g_pRenderStruct (the engine function table)
#include "sys/shared/render_drawprim.h"  // CRenderDrawPrim -- the neutral base
#include "mtl_device.h"
#include "mtl_matrix.h"
#include "mtl_texture.h"
#include "mtl_drawprim.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>

// ==========================================================================
// The shader. Texture x vertex colour with combiner variations -- and that is
// genuinely all it needs to be: the survey found glLight/glMaterial ZERO in the
// whole renderer, because all lighting is already resolved to vertex colours on
// the CPU (§68-§71). The single biggest piece of good news in this conversion.
// ==========================================================================
static const char *kDrawPrimMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSIn {
    float3 pos   [[attribute(0)]];
    float2 uv    [[attribute(1)]];
    float4 color [[attribute(2)]];
};

struct VSOut {
    float4 pos [[position]];
    float2 uv;
    float4 color;
};

struct DPUniforms {
    float4x4 mvp;
    float    alphaRef;
    float    pad0, pad1, pad2;
};

vertex VSOut dp_vertex(VSIn in [[stage_in]],
                       constant DPUniforms &u [[buffer(1)]])
{
    VSOut o;
    o.pos   = u.mvp * float4(in.pos, 1.0);
    o.uv    = in.uv;
    o.color = in.color;
    return o;
}

// Specialisation constants -- these are what the pipeline-state cache is keyed
// on, so the fragment shader has no per-pixel branching on render state.
//   kColorOp:   0 vertex colour only, 1 modulate, 2 add, 3 decal/replace
//   kAlphaFunc: 0 none, 1 <, 2 <=, 3 >, 4 >=, 5 ==, 6 !=
constant int kColorOp   [[function_constant(0)]];
constant int kAlphaFunc [[function_constant(1)]];

fragment float4 dp_fragment(VSOut in [[stage_in]],
                            constant DPUniforms &u  [[buffer(0)]],
                            texture2d<float> tex    [[texture(0)]],
                            sampler          smp    [[sampler(0)]])
{
    float4 c;
    if (kColorOp == 0) {
        c = in.color;
    } else {
        float4 t = tex.sample(smp, in.uv);
        if (kColorOp == 2) {
            // GL_ADD: Cv = Cp + Cs, Av = Ap * As
            c = float4(saturate(t.rgb + in.color.rgb), t.a * in.color.a);
        } else if (kColorOp == 3) {
            // GL_REPLACE (DRAWPRIM_DECAL)
            c = t;
        } else {
            // GL_MODULATE
            c = t * in.color;
        }
    }

    if (kAlphaFunc != 0) {
        bool bPass = true;
        if      (kAlphaFunc == 1) bPass = (c.a <  u.alphaRef);
        else if (kAlphaFunc == 2) bPass = (c.a <= u.alphaRef);
        else if (kAlphaFunc == 3) bPass = (c.a >  u.alphaRef);
        else if (kAlphaFunc == 4) bPass = (c.a >= u.alphaRef);
        else if (kAlphaFunc == 5) bPass = (c.a == u.alphaRef);
        else                      bPass = (c.a != u.alphaRef);
        if (!bPass)
            discard_fragment();
    }
    return c;
}
)MSL";

// ==========================================================================
// Vertex + uniform layout
// ==========================================================================
struct MDPVert
{
	float x, y, z;
	float u, v;
	uint8 r, g, b, a;
};
static const NSUInteger kMDPVertStride = sizeof(MDPVert);   // 24

struct MDPUniforms
{
	float m_aMVP[16];
	float m_fAlphaRef;
	float m_aPad[3];
};

// ⚠️⚠️ NEVER HAND-PAD A SHADER-SHARED STRUCT WITHOUT ASSERTING ITS LAYOUT.
// CharacterViewer's Metal port lost a session to exactly this: a hand-written
// `float _pad0[3]` pushed every later field by 16 bytes, the fragment shader
// read its alpha-test mode out of garbage, discard_fragment() ran on every
// pixel -- and NOTHING ERRORED. The symptom was a blank view.
// These offsets are what MSL's rules give for DPUniforms in the shader above:
// float4x4 is 16-byte aligned and 64 bytes, plain floats then pack, and the
// struct rounds up to its 16-byte alignment.
static_assert(offsetof(MDPUniforms, m_aMVP)      ==  0, "DPUniforms.mvp moved");
static_assert(offsetof(MDPUniforms, m_fAlphaRef) == 64, "DPUniforms.alphaRef moved");
static_assert(sizeof(MDPUniforms)                == 80, "DPUniforms size changed");

// ==========================================================================
// Device objects
// ==========================================================================
static id<MTLLibrary>             g_Library    = nil;
static id<MTLFunction>            g_VertexFn   = nil;
static MTLVertexDescriptor*       g_VertexDesc = nil;
static id<MTLTexture>             g_WhiteTex   = nil;   // bound when untextured
static id<MTLSamplerState>        g_WhiteSmp   = nil;
static id<MTLDepthStencilState>   g_aDepthState[3] = { nil, nil, nil };
static bool                       g_bShadersReady  = false;
static bool                       g_bShaderFailed  = false;

// Pipeline-state cache. Flat, because the key space is small and bounded:
// 11 blend modes x 4 colour ops x 7 alpha-test funcs = 308 slots of pointer.
enum { kMDPBlendCount = 11, kMDPColorOpCount = 4, kMDPAlphaCount = 7 };
static CFTypeRef g_aPipelines[kMDPBlendCount][kMDPColorOpCount][kMDPAlphaCount];

// The dynamic vertex ring: one buffer per frame in flight, bump-allocated.
// MTLDev_FrameSlot() advances once per presented frame and MTLDev_BeginFrame
// will not hand out a slot whose GPU work is still running, so writing into
// the slot's buffer from the CPU cannot race the GPU.
struct MDPRingSlot
{
	id<MTLBuffer> m_Buffer;
	NSUInteger    m_nOffset;
	NSUInteger    m_nCapacity;
};
static MDPRingSlot g_aRing[MTLDEV_FRAMES_IN_FLIGHT];
static int         g_nLastSlot = -1;
static NSUInteger  g_nRingHighWater = 0;

static const NSUInteger kMDPRingInitial = 512 * 1024;
static const NSUInteger kMDPAlign       = 256;

static bool mdp_Trace(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_TRACE_DRAWPRIM") ? 1 : 0;
	return s_n != 0;
}

// ==========================================================================
// Shader + fixed state creation (once)
// ==========================================================================
static bool mdp_EnsureShaders(void)
{
	if (g_bShadersReady) return true;
	if (g_bShaderFailed) return false;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev)
		return false;

	NSError *err = nil;
	NSString *src = [NSString stringWithUTF8String:kDrawPrimMSL];
	g_Library = [dev newLibraryWithSource:src options:nil error:&err];
	if (!g_Library)
	{
		// ⚠️ Fail loudly and ONCE. A runtime-compiled shader that silently
		// fails leaves an empty screen with no clue anywhere -- exactly the
		// failure mode this port keeps paying for.
		g_bShaderFailed = true;
		fprintf(stderr, "[mtldp] MSL compile FAILED: %s\n",
		        err ? [[err localizedDescription] UTF8String] : "(no error)");
		return false;
	}
	g_VertexFn = [g_Library newFunctionWithName:@"dp_vertex"];
	if (!g_VertexFn)
	{
		g_bShaderFailed = true;
		fprintf(stderr, "[mtldp] dp_vertex not found in the compiled library\n");
		return false;
	}

	// Interleaved: pos(12) uv(8) rgba8(4) = 24 bytes.
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
	g_VertexDesc.layouts[0].stride         = kMDPVertStride;
	g_VertexDesc.layouts[0].stepFunction   = MTLVertexStepFunctionPerVertex;

	// A 1x1 opaque white texture, bound whenever the primitive is untextured.
	// The fragment function declares a texture argument in every
	// specialisation, and Metal validation wants one bound whether or not the
	// specialised code samples it. Cheaper than a second shader family.
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
	}

	// Depth states, one per ELTZBufferMode.
	// ⚠️ LESS. The GL drawprim never set glDepthFunc either, so it inherited
	// the pipeline's standing value -- which is GL's default GL_LESS, not
	// LEQUAL as an earlier version of this comment claimed (the renderer's only
	// glDepthFunc calls are in the dynamic-light pass, and it restores LESS).
	// Naming it here removes the dependence on draw order the GL path had by
	// accident.
	{
		MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
		d.depthCompareFunction = MTLCompareFunctionLess;
		d.depthWriteEnabled    = YES;
		g_aDepthState[DRAWPRIM_ZRW] = [dev newDepthStencilStateWithDescriptor:d];
		d.depthWriteEnabled    = NO;
		g_aDepthState[DRAWPRIM_ZRO] = [dev newDepthStencilStateWithDescriptor:d];
		d.depthCompareFunction = MTLCompareFunctionAlways;
		g_aDepthState[DRAWPRIM_NOZ] = [dev newDepthStencilStateWithDescriptor:d];
	}

	memset(g_aPipelines, 0, sizeof(g_aPipelines));
	g_bShadersReady = true;
	fprintf(stderr, "[mtldp] MSL library compiled at runtime, drawprim ready\n");
	return true;
}

// --------------------------------------------------------------------------
// Blend state, straight across from the GL drawprim's glBlendFunc table.
// --------------------------------------------------------------------------
static void mdp_ApplyBlend(MTLRenderPipelineColorAttachmentDescriptor *att, int nBlend)
{
	if (nBlend == DRAWPRIM_NOBLEND)
	{
		att.blendingEnabled = NO;
		return;
	}
	att.blendingEnabled = YES;
	att.rgbBlendOperation   = MTLBlendOperationAdd;
	att.alphaBlendOperation = MTLBlendOperationAdd;

	MTLBlendFactor eSrc = MTLBlendFactorOne, eDst = MTLBlendFactorZero;
	switch (nBlend)
	{
		case DRAWPRIM_BLEND_ADD:               eSrc = MTLBlendFactorOne;                     eDst = MTLBlendFactorOne;                       break;
		case DRAWPRIM_BLEND_SATURATE:          eSrc = MTLBlendFactorOneMinusDestinationColor;eDst = MTLBlendFactorOne;                       break;
		case DRAWPRIM_BLEND_MOD_SRCALPHA:      eSrc = MTLBlendFactorSourceAlpha;             eDst = MTLBlendFactorOneMinusSourceAlpha;       break;
		case DRAWPRIM_BLEND_MOD_SRCCOLOR:      eSrc = MTLBlendFactorSourceColor;             eDst = MTLBlendFactorOneMinusSourceColor;       break;
		case DRAWPRIM_BLEND_MOD_DSTCOLOR:      eSrc = MTLBlendFactorDestinationColor;        eDst = MTLBlendFactorOneMinusDestinationColor;  break;
		case DRAWPRIM_BLEND_MUL_SRCCOL_DSTCOL: eSrc = MTLBlendFactorSourceColor;             eDst = MTLBlendFactorDestinationColor;          break;
		case DRAWPRIM_BLEND_MUL_SRCALPHA_ONE:  eSrc = MTLBlendFactorSourceAlpha;             eDst = MTLBlendFactorOne;                       break;
		case DRAWPRIM_BLEND_MUL_SRCALPHA:      eSrc = MTLBlendFactorSourceAlpha;             eDst = MTLBlendFactorZero;                      break;
		case DRAWPRIM_BLEND_MUL_SRCCOL_ONE:    eSrc = MTLBlendFactorSourceColor;             eDst = MTLBlendFactorOne;                       break;
		case DRAWPRIM_BLEND_MUL_DSTCOL_ZERO:   eSrc = MTLBlendFactorDestinationColor;        eDst = MTLBlendFactorZero;                      break;
		default: break;
	}
	att.sourceRGBBlendFactor        = eSrc;
	att.destinationRGBBlendFactor   = eDst;
	att.sourceAlphaBlendFactor      = eSrc;
	att.destinationAlphaBlendFactor = eDst;
}

static id<MTLRenderPipelineState> mdp_Pipeline(int nBlend, int nColorOp, int nAlphaFunc)
{
	if (nBlend    < 0 || nBlend    >= kMDPBlendCount)    nBlend    = 0;
	if (nColorOp  < 0 || nColorOp  >= kMDPColorOpCount)  nColorOp  = 0;
	if (nAlphaFunc< 0 || nAlphaFunc>= kMDPAlphaCount)    nAlphaFunc= 0;

	CFTypeRef cached = g_aPipelines[nBlend][nColorOp][nAlphaFunc];
	if (cached)
		return (__bridge id<MTLRenderPipelineState>)cached;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev)
		return nil;

	NSError *err = nil;
	MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues alloc] init];
	int nOp = nColorOp, nAF = nAlphaFunc;
	[cv setConstantValue:&nOp type:MTLDataTypeInt atIndex:0];
	[cv setConstantValue:&nAF type:MTLDataTypeInt atIndex:1];

	id<MTLFunction> fs = [g_Library newFunctionWithName:@"dp_fragment"
	                                     constantValues:cv
	                                              error:&err];
	if (!fs)
	{
		fprintf(stderr, "[mtldp] fragment specialisation (op=%d af=%d) failed: %s\n",
		        nColorOp, nAlphaFunc,
		        err ? [[err localizedDescription] UTF8String] : "(no error)");
		return nil;
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	pd.vertexFunction   = g_VertexFn;
	pd.fragmentFunction = fs;
	pd.vertexDescriptor = g_VertexDesc;
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat      = MTLPixelFormatDepth32Float;
	mdp_ApplyBlend(pd.colorAttachments[0], nBlend);

	id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!pso)
	{
		fprintf(stderr, "[mtldp] pipeline (blend=%d op=%d af=%d) failed: %s\n",
		        nBlend, nColorOp, nAlphaFunc,
		        err ? [[err localizedDescription] UTF8String] : "(no error)");
		return nil;
	}

	g_aPipelines[nBlend][nColorOp][nAlphaFunc] = CFBridgingRetain(pso);
	if (mdp_Trace())
		fprintf(stderr, "[mtldp] new pipeline blend=%d op=%d alphaFunc=%d\n",
		        nBlend, nColorOp, nAlphaFunc);
	return pso;
}

// ==========================================================================
// The vertex ring
// ==========================================================================
// ⚠️ __strong on the out-reference: under ARC a plain `id<MTLBuffer>&`
// parameter is __autoreleasing, which does not match a __strong local at the
// call site.
static bool mdp_RingAlloc(const MDPVert *pVerts, NSUInteger nCount,
                          id<MTLBuffer> __strong &outBuf, NSUInteger &outOffset)
{
	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev || !nCount)
		return false;

	const int nSlot = MTLDev_FrameSlot();
	if (nSlot != g_nLastSlot)
	{
		// New frame -> this slot's previous contents are finished with.
		g_nLastSlot = nSlot;
		g_aRing[nSlot].m_nOffset = 0;
	}
	MDPRingSlot &cSlot = g_aRing[nSlot];

	const NSUInteger nBytes   = nCount * kMDPVertStride;
	const NSUInteger nAligned = (nBytes + kMDPAlign - 1) & ~(kMDPAlign - 1);

	if (!cSlot.m_Buffer || cSlot.m_nOffset + nAligned > cSlot.m_nCapacity)
	{
		// Grow (or create). ⚠️ Dropping our reference to the old buffer is
		// safe even mid-frame: a command buffer retains every resource that
		// has been encoded into it, so draws already issued this frame keep
		// reading the buffer they were given.
		NSUInteger nNeed = cSlot.m_nOffset + nAligned;
		NSUInteger nCap  = cSlot.m_nCapacity ? cSlot.m_nCapacity : kMDPRingInitial;
		while (nCap < nNeed)
			nCap *= 2;

		id<MTLBuffer> newBuf = [dev newBufferWithLength:nCap
		                                        options:MTLResourceStorageModeShared];
		if (!newBuf)
			return false;
		// Carry nothing over: the offset only ever moves forward within a
		// frame, and the already-encoded draws still hold the old buffer.
		cSlot.m_Buffer   = newBuf;
		cSlot.m_nCapacity= nCap;
		cSlot.m_nOffset  = 0;
		if (mdp_Trace())
			fprintf(stderr, "[mtldp] vertex ring slot %d grown to %lu KB\n",
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
// Matrices: mtl_matrix.h (shared with the world pass). mtl_Ortho maps the
// engine's top-left screen pixels to clip space; mtl_Frustum is the Metal
// 0..1-depth perspective for CAMERA/WORLD-space prims.
// ==========================================================================

// ==========================================================================
// The implementation class
// ==========================================================================
class CMTLDrawPrim : public CRenderDrawPrim
{
public:
	declare_interface(CMTLDrawPrim);

	virtual LTRESULT DrawPrim(LT_POLYGT3 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYFT3 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYG3 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYF3 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYGT4 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYGT4 **ppPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYFT4 *pPrim, const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYG4 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_POLYF4 *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEGT *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEFT *pPrim,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEG *pPrim,   const uint32 nCount = 1);
	virtual LTRESULT DrawPrim(LT_LINEF *pPrim,   const uint32 nCount = 1);
	virtual LTRESULT DrawPrimPoint(LT_VERTGT *pVerts, const uint32 nCount = 1);
	virtual LTRESULT DrawPrimPoint(LT_VERTG *pVerts,  const uint32 nCount = 1);
	virtual LTRESULT DrawPrimFan(LT_VERTGT *pVerts, const uint32 nCount);
	virtual LTRESULT DrawPrimFan(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba);
	virtual LTRESULT DrawPrimFan(LT_VERTG *pVerts,  const uint32 nCount);
	virtual LTRESULT DrawPrimFan(LT_VERTF *pVerts,  const uint32 nCount, LT_VERTRGBA rgba);
	virtual LTRESULT DrawPrimStrip(LT_VERTGT *pVerts, const uint32 nCount);
	virtual LTRESULT DrawPrimStrip(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba);
	virtual LTRESULT DrawPrimStrip(LT_VERTG *pVerts,  const uint32 nCount);
	virtual LTRESULT DrawPrimStrip(LT_VERTF *pVerts,  const uint32 nCount, LT_VERTRGBA rgba);

private:
	// Encodes the assembled vertices with the current CGenDrawPrim state.
	void Draw(MTLPrimitiveType eType, const std::vector<MDPVert> &cVerts,
	          bool bTextured);
};

define_interface(CMTLDrawPrim, ILTDrawPrim);
instantiate_interface(CMTLDrawPrim, ILTDrawPrim, Internal);

// ==========================================================================
// The one place that touches the encoder
// ==========================================================================
void CMTLDrawPrim::Draw(MTLPrimitiveType eType, const std::vector<MDPVert> &cVerts,
                        bool bTextured)
{
	if (cVerts.empty() || !mdp_EnsureShaders())
		return;
	if (!MTLDev_EnsureFrame())
		return;   // no drawable this frame -- skip, never draw anyway

	id<MTLRenderCommandEncoder> enc =
		(__bridge id<MTLRenderCommandEncoder>)MTLDev_Encoder();
	if (!enc)
		return;

	// ---- resolve the texture and the effective combiner op ----
	id<MTLTexture>      tex = g_WhiteTex;
	id<MTLSamplerState> smp = g_WhiteSmp;
	int nColorOp = 0;
	if (bTextured && m_pTexture && m_ColorOp != DRAWPRIM_NOCOLOROP)
	{
		void *pTex = MTLTex_Get((SharedTexture*)m_pTexture);
		if (pTex)
		{
			tex = (__bridge id<MTLTexture>)pTex;
			void *pSmp = MTLTex_Sampler((SharedTexture*)m_pTexture);
			if (pSmp) smp = (__bridge id<MTLSamplerState>)pSmp;
			// DRAWPRIM_MODULATE 1, DRAWPRIM_ADD 2, DRAWPRIM_DECAL 3 -- the
			// shader's kColorOp uses the same numbering on purpose.
			nColorOp = (int)m_ColorOp;
		}
	}

	// ---- alpha test -> discard. The reference is 0.5 per the ILTDrawPrim
	// contract (and is what the GL path used); the AUTHORED AlphaRef belongs
	// to the world/model passes, not to 2D prims. ----
	int nAlphaFunc = 0;
	switch (m_eTestMode)
	{
		case DRAWPRIM_ALPHATEST_LESS:         nAlphaFunc = 1; break;
		case DRAWPRIM_ALPHATEST_LESSEQUAL:    nAlphaFunc = 2; break;
		case DRAWPRIM_ALPHATEST_GREATER:      nAlphaFunc = 3; break;
		case DRAWPRIM_ALPHATEST_GREATEREQUAL: nAlphaFunc = 4; break;
		case DRAWPRIM_ALPHATEST_EQUAL:        nAlphaFunc = 5; break;
		case DRAWPRIM_ALPHATEST_NOTEQUAL:     nAlphaFunc = 6; break;
		default:                              nAlphaFunc = 0; break;
	}

	id<MTLRenderPipelineState> pso = mdp_Pipeline((int)m_BlendMode, nColorOp, nAlphaFunc);
	if (!pso)
		return;

	// A DISTINCT-STATE census: one line per combination of render state that
	// actually reaches the encoder. Keyed on the state, so it reports what it
	// names and stays quiet once the set stops growing (§70/§71).
	if (mdp_Trace())
	{
		static std::vector<uint32> s_cSeen;
		uint32 nKey = ((uint32)m_eTransType    <<  0) | ((uint32)m_BlendMode << 4)
		            | ((uint32)m_eZBufferMode  << 12) | ((uint32)nColorOp    << 16)
		            | ((uint32)nAlphaFunc      << 20) | ((uint32)m_eCullMode << 24);
		bool bNew = true;
		for (size_t i = 0; i < s_cSeen.size(); ++i)
			if (s_cSeen[i] == nKey) { bNew = false; break; }
		if (bNew)
		{
			s_cSeen.push_back(nKey);
			fprintf(stderr, "[mtldp] state: trans=%d blend=%d z=%d colorop=%d alphaFunc=%d cull=%d"
			                " verts=%zu tex=%s\n",
			        (int)m_eTransType, (int)m_BlendMode, (int)m_eZBufferMode,
			        nColorOp, nAlphaFunc, (int)m_eCullMode, cVerts.size(),
			        (tex == g_WhiteTex) ? "none" : "yes");
		}
	}

	// ---- transform ----
	int nDrawW = 640, nDrawH = 480;
	LTMacWin_GetSize(&nDrawW, &nDrawH);

	MDPUniforms cU;
	cU.m_fAlphaRef = 0.5f;
	cU.m_aPad[0] = cU.m_aPad[1] = cU.m_aPad[2] = 0.0f;

	if (m_eTransType == DRAWPRIM_TRANSFORM_SCREEN)
	{
		// Engine screen coordinates over the whole drawable, whatever
		// resolution the engine believes it runs at.
		float fScreenW = (g_pRenderStruct && g_pRenderStruct->m_Width)  ? (float)g_pRenderStruct->m_Width  : (float)nDrawW;
		float fScreenH = (g_pRenderStruct && g_pRenderStruct->m_Height) ? (float)g_pRenderStruct->m_Height : (float)nDrawH;
		mtl_Ortho(cU.m_aMVP, fScreenW, fScreenH);

		// ⚠️ RESTORE THE FULL-DRAWABLE VIEWPORT, as the GL path's SetupState
		// did with glViewport. A Metal viewport is ENCODER state and survives
		// every draw, so a scene that set a sub-rect (a split-screen or
		// letterboxed camera rect) would otherwise squash the console and the
		// whole HUD into that rect for the rest of the frame.
		MTLDev_SetViewport(0, 0, nDrawW, nDrawH);
	}
	else
	{
		float aView[16], fRight = 1.0f, fTop = 0.75f, fNear = 5.0f, fFar = 100000.0f;
		const bool bScene = RenderDrawPrim_GetSceneTransform(aView, fRight, fTop, fNear, fFar);
		float aProj[16];
		mtl_Frustum(aProj, fRight, fTop, fNear, fFar);

		if (m_eTransType == DRAWPRIM_TRANSFORM_WORLD && bScene)
		{
			mtl_Mul(cU.m_aMVP, aProj, aView);
		}
		else
		{
			// Camera space: Lithtech looks down +Z, the projection down -Z.
			float aFlip[16];
			mtl_Identity(aFlip);
			aFlip[10] = -1.0f;
			mtl_Mul(cU.m_aMVP, aProj, aFlip);
		}
	}

	// ---- encode ----
	id<MTLBuffer> buf = nil;
	NSUInteger    nOffset = 0;
	if (!mdp_RingAlloc(cVerts.data(), cVerts.size(), buf, nOffset))
		return;

	[enc setRenderPipelineState:pso];
	[enc setDepthStencilState:g_aDepthState[m_eZBufferMode]];
	[enc setVertexBuffer:buf offset:nOffset atIndex:0];
	[enc setVertexBytes:&cU length:sizeof(cU) atIndex:1];
	[enc setFragmentBytes:&cU length:sizeof(cU) atIndex:0];
	[enc setFragmentTexture:tex atIndex:0];
	[enc setFragmentSamplerState:smp atIndex:0];

	// ⚠️ WINDING IS **NOT** INVERTED RELATIVE TO GL. §84 claimed it was, from
	// reasoning about Metal's top-left framebuffer origin; the world pass
	// MEASURED it (mtl_world.mm) and the sense matches GL's. So the GL path's
	// glFrontFace(CULL_CCW ? GL_CW : GL_CCW) maps straight across.
	// (No drawprim call has ever used anything but CULL_NONE, so this line has
	// never actually run -- it is corrected here so it is not copied wrong.)
	if (m_eCullMode == DRAWPRIM_CULL_NONE)
	{
		[enc setCullMode:MTLCullModeNone];
	}
	else
	{
		[enc setFrontFacingWinding:(m_eCullMode == DRAWPRIM_CULL_CCW)
		                            ? MTLWindingClockwise
		                            : MTLWindingCounterClockwise];
		[enc setCullMode:MTLCullModeBack];
	}
	[enc setTriangleFillMode:(m_eFillMode == DRAWPRIM_WIRE)
	                          ? MTLTriangleFillModeLines
	                          : MTLTriangleFillModeFill];

	if (MTLDev_TraceFrame())
	{
		const MDPVert &v0 = cVerts[0];
		fprintf(stderr, "[mtlf ] draw %5zu verts trans=%d blend=%d z=%d op=%d"
		                "  v0=(%.0f,%.0f,%.2f) uv=(%.3f,%.3f) rgba=(%u,%u,%u,%u)\n",
		        cVerts.size(), (int)m_eTransType, (int)m_BlendMode,
		        (int)m_eZBufferMode, nColorOp,
		        v0.x, v0.y, v0.z, v0.u, v0.v, v0.r, v0.g, v0.b, v0.a);
	}

	[enc drawPrimitives:eType vertexStart:0 vertexCount:cVerts.size()];
}

// ==========================================================================
// Vertex conversion + primitive expansion
//
// Metal has neither GL_QUADS nor GL_TRIANGLE_FAN, so both are expanded here.
// The quad split (0,1,2)(0,2,3) and the fan split (0,i,i+1) both PRESERVE the
// source winding, which is what keeps the cull mode above meaningful.
// ==========================================================================
static inline MDPVert mdp_FromGT(const LT_VERTGT &v)
{
	MDPVert o = { v.x, v.y, v.z, v.u, v.v, v.rgba.r, v.rgba.g, v.rgba.b, v.rgba.a };
	return o;
}
static inline MDPVert mdp_FromG(const LT_VERTG &v)
{
	MDPVert o = { v.x, v.y, v.z, 0.0f, 0.0f, v.rgba.r, v.rgba.g, v.rgba.b, v.rgba.a };
	return o;
}
static inline MDPVert mdp_FromFT(const LT_VERTFT &v, const LT_VERTRGBA &c)
{
	MDPVert o = { v.x, v.y, v.z, v.u, v.v, c.r, c.g, c.b, c.a };
	return o;
}
static inline MDPVert mdp_FromF(const LT_VERTF &v, const LT_VERTRGBA &c)
{
	MDPVert o = { v.x, v.y, v.z, 0.0f, 0.0f, c.r, c.g, c.b, c.a };
	return o;
}

// Scratch, reused across calls -- the render path is single-threaded and this
// keeps a per-draw allocation out of the console/HUD hot path.
static std::vector<MDPVert> g_cScratch;

// ---- triangles ----------------------------------------------------------
LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYGT3 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			g_cScratch.push_back(mdp_FromGT(pPrim[n].verts[v]));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYFT3 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			g_cScratch.push_back(mdp_FromFT(pPrim[n].verts[v], pPrim[n].rgba));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYG3 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			g_cScratch.push_back(mdp_FromG(pPrim[n].verts[v]));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, false);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYF3 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 3; ++v)
			g_cScratch.push_back(mdp_FromF(pPrim[n].verts[v], pPrim[n].rgba));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, false);
	return LT_OK;
}

// ---- quads (expanded to two triangles each) -----------------------------
LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYGT4 *pPrim, const uint32 nCount)
{
	if (mdp_Trace() && nCount > 0)
		fprintf(stderr, "[dp] GT4 n=%u trans=%d tex=%p v0=(%.1f,%.1f) v2=(%.1f,%.1f)\n",
			nCount, (int)m_eTransType, m_pTexture,
			pPrim[0].verts[0].x, pPrim[0].verts[0].y,
			pPrim[0].verts[2].x, pPrim[0].verts[2].y);
	g_cScratch.clear();
	static const int kQuad[6] = { 0, 1, 2, 0, 2, 3 };
	for (uint32 n = 0; n < nCount; ++n)
		for (int i = 0; i < 6; ++i)
			g_cScratch.push_back(mdp_FromGT(pPrim[n].verts[kQuad[i]]));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYGT4 **ppPrim, const uint32 nCount)
{
	g_cScratch.clear();
	static const int kQuad[6] = { 0, 1, 2, 0, 2, 3 };
	for (uint32 n = 0; n < nCount; ++n)
		for (int i = 0; i < 6; ++i)
			g_cScratch.push_back(mdp_FromGT(ppPrim[n]->verts[kQuad[i]]));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYFT4 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	static const int kQuad[6] = { 0, 1, 2, 0, 2, 3 };
	for (uint32 n = 0; n < nCount; ++n)
		for (int i = 0; i < 6; ++i)
			g_cScratch.push_back(mdp_FromFT(pPrim[n].verts[kQuad[i]], pPrim[n].rgba));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYG4 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	static const int kQuad[6] = { 0, 1, 2, 0, 2, 3 };
	for (uint32 n = 0; n < nCount; ++n)
		for (int i = 0; i < 6; ++i)
			g_cScratch.push_back(mdp_FromG(pPrim[n].verts[kQuad[i]]));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, false);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_POLYF4 *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	static const int kQuad[6] = { 0, 1, 2, 0, 2, 3 };
	for (uint32 n = 0; n < nCount; ++n)
		for (int i = 0; i < 6; ++i)
			g_cScratch.push_back(mdp_FromF(pPrim[n].verts[kQuad[i]], pPrim[n].rgba));
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, false);
	return LT_OK;
}

// ---- lines ---------------------------------------------------------------
LTRESULT CMTLDrawPrim::DrawPrim(LT_LINEGT *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			g_cScratch.push_back(mdp_FromGT(pPrim[n].verts[v]));
	Draw(MTLPrimitiveTypeLine, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_LINEFT *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			g_cScratch.push_back(mdp_FromFT(pPrim[n].verts[v], pPrim[n].rgba));
	Draw(MTLPrimitiveTypeLine, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_LINEG *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			g_cScratch.push_back(mdp_FromG(pPrim[n].verts[v]));
	Draw(MTLPrimitiveTypeLine, g_cScratch, false);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrim(LT_LINEF *pPrim, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		for (uint32 v = 0; v < 2; ++v)
			g_cScratch.push_back(mdp_FromF(pPrim[n].verts[v], pPrim[n].rgba));
	Draw(MTLPrimitiveTypeLine, g_cScratch, false);
	return LT_OK;
}

// ---- points --------------------------------------------------------------
LTRESULT CMTLDrawPrim::DrawPrimPoint(LT_VERTGT *pVerts, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		g_cScratch.push_back(mdp_FromGT(pVerts[n]));
	Draw(MTLPrimitiveTypePoint, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimPoint(LT_VERTG *pVerts, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		g_cScratch.push_back(mdp_FromG(pVerts[n]));
	Draw(MTLPrimitiveTypePoint, g_cScratch, false);
	return LT_OK;
}

// ---- fans (expanded to a triangle list) ---------------------------------
LTRESULT CMTLDrawPrim::DrawPrimFan(LT_VERTGT *pVerts, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 i = 1; i + 1 < nCount; ++i)
	{
		g_cScratch.push_back(mdp_FromGT(pVerts[0]));
		g_cScratch.push_back(mdp_FromGT(pVerts[i]));
		g_cScratch.push_back(mdp_FromGT(pVerts[i + 1]));
	}
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimFan(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	g_cScratch.clear();
	for (uint32 i = 1; i + 1 < nCount; ++i)
	{
		g_cScratch.push_back(mdp_FromFT(pVerts[0], rgba));
		g_cScratch.push_back(mdp_FromFT(pVerts[i], rgba));
		g_cScratch.push_back(mdp_FromFT(pVerts[i + 1], rgba));
	}
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimFan(LT_VERTG *pVerts, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 i = 1; i + 1 < nCount; ++i)
	{
		g_cScratch.push_back(mdp_FromG(pVerts[0]));
		g_cScratch.push_back(mdp_FromG(pVerts[i]));
		g_cScratch.push_back(mdp_FromG(pVerts[i + 1]));
	}
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, false);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimFan(LT_VERTF *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	g_cScratch.clear();
	for (uint32 i = 1; i + 1 < nCount; ++i)
	{
		g_cScratch.push_back(mdp_FromF(pVerts[0], rgba));
		g_cScratch.push_back(mdp_FromF(pVerts[i], rgba));
		g_cScratch.push_back(mdp_FromF(pVerts[i + 1], rgba));
	}
	Draw(MTLPrimitiveTypeTriangle, g_cScratch, false);
	return LT_OK;
}

// ---- strips (Metal has these natively) -----------------------------------
LTRESULT CMTLDrawPrim::DrawPrimStrip(LT_VERTGT *pVerts, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		g_cScratch.push_back(mdp_FromGT(pVerts[n]));
	Draw(MTLPrimitiveTypeTriangleStrip, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimStrip(LT_VERTFT *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		g_cScratch.push_back(mdp_FromFT(pVerts[n], rgba));
	Draw(MTLPrimitiveTypeTriangleStrip, g_cScratch, true);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimStrip(LT_VERTG *pVerts, const uint32 nCount)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		g_cScratch.push_back(mdp_FromG(pVerts[n]));
	Draw(MTLPrimitiveTypeTriangleStrip, g_cScratch, false);
	return LT_OK;
}

LTRESULT CMTLDrawPrim::DrawPrimStrip(LT_VERTF *pVerts, const uint32 nCount, LT_VERTRGBA rgba)
{
	g_cScratch.clear();
	for (uint32 n = 0; n < nCount; ++n)
		g_cScratch.push_back(mdp_FromF(pVerts[n], rgba));
	Draw(MTLPrimitiveTypeTriangleStrip, g_cScratch, false);
	return LT_OK;
}

// ==========================================================================
// The optimized-2D SURFACE path: nr_BlitToScreen / nr_WarpToScreen.
//
// The interface draws into 16-bit software surfaces (NullBuf) and hands them to
// the renderer as a rect or an arbitrary quad. Everything else in this file goes
// through ILTDrawPrim; this does not, so it needs its own entry — but it wants
// exactly the same machinery (shaders, pipeline cache, vertex ring, screen
// ortho), which is why it lives here rather than in a fourth Metal module.
//
// ⚠️ Under GL this path was 34 immediate-mode calls in nullrender.cpp. Under
// Metal those are no-ops without a context, so until now EVERY surface was
// invisible: the startup splash (a 1024x768 opaque blit), the cinematic
// letterbox bars, and every screen fade (a 2x2 surface stretched full-screen
// with a ramping alpha). Those are the only three users the census found.
// ==========================================================================
void MTLDrawPrim_PresentSurface(const uint8_t *pRGBA, uint32_t nWidth, uint32_t nHeight,
                                const float dst[4][2], const float src[4][2],
                                float fAlpha, bool bBlend)
{
	if (!pRGBA || !nWidth || !nHeight || !mdp_EnsureShaders())
		return;
	if (!MTLDev_EnsureFrame())
		return;

	id<MTLRenderCommandEncoder> enc =
		(__bridge id<MTLRenderCommandEncoder>)MTLDev_Encoder();
	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!enc || !dev)
		return;

	// ⚠️ A FRESH texture per call, deliberately. The obvious cache — one
	// texture grown on demand — would be overwritten by the CPU while the GPU
	// is still reading the previous blit of the same frame, and a frame really
	// does present several (both letterbox bars plus the fade). A command
	// buffer retains every resource encoded into it, so an unshared texture is
	// safe to drop immediately; the cost is a 2x2 allocation a few times a
	// frame, and one 1024x768 at startup.
	MTLTextureDescriptor *td =
		[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                   width:nWidth
		                                                  height:nHeight
		                                               mipmapped:NO];
	td.usage       = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
	if (!tex)
		return;
	[tex replaceRegion:MTLRegionMake2D(0, 0, nWidth, nHeight)
	       mipmapLevel:0
	         withBytes:pRGBA
	       bytesPerRow:(NSUInteger)nWidth * 4];

	// The GL path resolved "does this blend?" from the transparency flag AND
	// the alpha; it is passed in already resolved rather than re-derived here,
	// so the two backends cannot drift apart (§89's lesson).
	id<MTLRenderPipelineState> pso =
		mdp_Pipeline(bBlend ? DRAWPRIM_BLEND_MOD_SRCALPHA : DRAWPRIM_NOBLEND,
		             DRAWPRIM_MODULATE, 0 /* no alpha test: GL disabled it */);
	if (!pso)
		return;

	// Screen ortho over the engine's logical screen, and the full-drawable
	// viewport — the same restore CMTLDrawPrim::Draw does for SCREEN prims,
	// and for the same reason (a scene may have left a sub-rect behind).
	int nDrawW = 640, nDrawH = 480;
	LTMacWin_GetSize(&nDrawW, &nDrawH);
	const float fScreenW = (g_pRenderStruct && g_pRenderStruct->m_Width)  ? (float)g_pRenderStruct->m_Width  : (float)nDrawW;
	const float fScreenH = (g_pRenderStruct && g_pRenderStruct->m_Height) ? (float)g_pRenderStruct->m_Height : (float)nDrawH;

	MDPUniforms cU;
	cU.m_fAlphaRef = 0.5f;
	cU.m_aPad[0] = cU.m_aPad[1] = cU.m_aPad[2] = 0.0f;
	mtl_Ortho(cU.m_aMVP, fScreenW, fScreenH);
	MTLDev_SetViewport(0, 0, nDrawW, nDrawH);

	const uint8 nA = (uint8)(LTCLAMP(fAlpha, 0.0f, 1.0f) * 255.0f + 0.5f);
	const float fIW = 1.0f / (float)nWidth;
	const float fIH = 1.0f / (float)nHeight;
	MDPVert aQuad[4];
	for (int i = 0; i < 4; ++i)
	{
		aQuad[i].x = dst[i][0];  aQuad[i].y = dst[i][1];  aQuad[i].z = 0.0f;
		aQuad[i].u = src[i][0] * fIW;
		aQuad[i].v = src[i][1] * fIH;
		aQuad[i].r = aQuad[i].g = aQuad[i].b = 255;
		aQuad[i].a = nA;
	}
	// Metal has no GL_QUADS: (0,1,2)(0,2,3), the same split the drawprim
	// expansion uses, which preserves the source winding.
	std::vector<MDPVert> cVerts;
	cVerts.reserve(6);
	cVerts.push_back(aQuad[0]); cVerts.push_back(aQuad[1]); cVerts.push_back(aQuad[2]);
	cVerts.push_back(aQuad[0]); cVerts.push_back(aQuad[2]); cVerts.push_back(aQuad[3]);

	id<MTLBuffer> buf = nil;
	NSUInteger    nOffset = 0;
	if (!mdp_RingAlloc(cVerts.data(), cVerts.size(), buf, nOffset))
		return;

	[enc setRenderPipelineState:pso];
	[enc setDepthStencilState:g_aDepthState[DRAWPRIM_NOZ]];   // GL: no test, no write
	[enc setVertexBuffer:buf offset:nOffset atIndex:0];
	[enc setVertexBytes:&cU length:sizeof(cU) atIndex:1];
	[enc setFragmentBytes:&cU length:sizeof(cU) atIndex:0];
	[enc setFragmentTexture:tex atIndex:0];
	// g_WhiteSmp is linear + the Metal default clamp-to-edge, which is exactly
	// what the GL path asked for (GL_LINEAR, GL_CLAMP_TO_EDGE).
	[enc setFragmentSamplerState:g_WhiteSmp atIndex:0];
	[enc setCullMode:MTLCullModeNone];
	[enc setTriangleFillMode:MTLTriangleFillModeFill];

	if (mdp_Trace() || MTLDev_TraceFrame())
	{
		static int s_nSaid = 0;
		if (s_nSaid < 8 || MTLDev_TraceFrame())
			fprintf(stderr, "[mtldp] surface %ux%u -> (%.0f %.0f)-(%.0f %.0f) "
			                "alpha=%.2f blend=%d\n",
			        nWidth, nHeight, dst[0][0], dst[0][1], dst[2][0], dst[2][1],
			        fAlpha, bBlend ? 1 : 0), ++s_nSaid;
	}

	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

// ==========================================================================
void MTLDrawPrim_Term(void)
{
	for (int b = 0; b < kMDPBlendCount; ++b)
		for (int o = 0; o < kMDPColorOpCount; ++o)
			for (int a = 0; a < kMDPAlphaCount; ++a)
				if (g_aPipelines[b][o][a])
				{
					CFBridgingRelease(g_aPipelines[b][o][a]);
					g_aPipelines[b][o][a] = NULL;
				}

	for (int i = 0; i < MTLDEV_FRAMES_IN_FLIGHT; ++i)
	{
		g_aRing[i].m_Buffer    = nil;
		g_aRing[i].m_nCapacity = 0;
		g_aRing[i].m_nOffset   = 0;
	}
	g_nLastSlot = -1;

	if (mdp_Trace())
		fprintf(stderr, "[mtldp] vertex ring high-water %lu KB\n",
		        (unsigned long)(g_nRingHighWater / 1024));

	g_VertexFn = nil; g_Library = nil; g_VertexDesc = nil;
	g_WhiteTex = nil; g_WhiteSmp = nil;
	for (int i = 0; i < 3; ++i) g_aDepthState[i] = nil;
	g_bShadersReady = false;
}
