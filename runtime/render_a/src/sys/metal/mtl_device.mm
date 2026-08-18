// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_device.mm
//
// PURPOSE : See mtl_device.h. Device, layer, per-frame command buffer and
//           render encoder, plus the frame readback used for verification.
//
// ----------------------------------------------------------------------- //

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "mtl_device.h"
#include "ltmacwindow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------
static id<MTLDevice>              g_Device      = nil;
static id<MTLCommandQueue>        g_Queue       = nil;
static CAMetalLayer*              g_Layer       = nil;

static id<MTLTexture>             g_DepthTex    = nil;
static id<CAMetalDrawable>        g_Drawable    = nil;
static id<MTLCommandBuffer>       g_CmdBuf      = nil;

// ★ LT_PROFILE=1 -- the first actual measurement of this renderer's cost.
// §94 item 8 names two suspects (render-pass splits on a TBDR, and ~2000
// unbatched draws) and says explicitly not to guess which dominates. GPU time
// comes from the command buffer's own timestamps, which is the real number;
// CPU frame time and the PASS COUNT sit next to it so the three can be read
// together. Reported as a rolling average, because a single frame is noise.
static bool mtl_Profile(void)
{
    static int s_n = -1;
    if (s_n < 0) s_n = getenv("LT_PROFILE") ? 1 : 0;
    return s_n != 0;
}
static uint32_t g_nPassesThisFrame = 0;
static id<MTLRenderCommandEncoder> g_Encoder    = nil;

static MTLClearColor              g_ClearColor  = { 0.0, 0.0, 0.0, 1.0 };
static bool                       g_bInited     = false;

// The command buffer of the frame just presented, kept only so that a readback
// can wait for it. Without the wait, getBytes races the GPU and returns a
// half-drawn frame -- which as a verification instrument is worse than useless,
// because it fails intermittently and looks like a renderer bug.
static id<MTLCommandBuffer>       g_LastCmdBuf  = nil;

// CPU/GPU pacing. A per-frame ring (the drawprim vertex buffer) is only safe to
// overwrite once the GPU is done with the frame that used that slot, so the
// frame counter is not allowed to run more than MTLDEV_FRAMES_IN_FLIGHT ahead.
// The viewport is remembered because Metal folds the depth range into it: a
// caller changing only the range must not lose the rect, and a new pass (every
// mid-frame clear opens one) has to restore both.
static double                     g_aViewport[4] = { 0.0, 0.0, 0.0, 0.0 };
static double                     g_fDepthNear = 0.0, g_fDepthFar = 1.0;
static void mtl_ApplyViewport(void);

static dispatch_semaphore_t       g_FrameSem    = nil;
static uint64_t                   g_nFrameIndex = 0;
static int                        g_nFrameSlot  = 0;

// ⚠️ The LAST PRESENTED drawable's texture, retained for readback. A drawable
// is recycled the moment it is presented, so a dump taken after EndFrame would
// otherwise race the compositor and read whatever is being drawn next. Holding
// the texture (not the drawable) keeps the pixels alive without stalling the
// swap chain.
static id<MTLTexture>             g_LastPresented = nil;

static bool mtl_Trace(void)
{
    static int s_n = -1;
    if (s_n < 0) s_n = getenv("LT_TRACE_METAL") ? 1 : 0;
    return s_n != 0;
}

// --------------------------------------------------------------------------
// Depth buffer -- recreated whenever the drawable size changes.
// --------------------------------------------------------------------------
static void mtl_EnsureDepth(NSUInteger w, NSUInteger h)
{
    if (g_DepthTex && g_DepthTex.width == w && g_DepthTex.height == h)
        return;

    MTLTextureDescriptor* d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:w
                                                          height:h
                                                       mipmapped:NO];
    // ⚠️ Private storage + RenderTarget usage: a depth buffer is never read by
    // the CPU, and leaving it Managed costs a needless copy on every resize.
    d.usage       = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    // LT_DUMP_DEPTH=1 makes the depth buffer CPU-readable so the winning
    // fragment's depth can be diffed against the GL build's. Off by default --
    // shared storage on a depth buffer is a cost paid for nothing in normal use.
    if (getenv("LT_DUMP_DEPTH"))
        d.storageMode = MTLStorageModeShared;
    g_DepthTex = [g_Device newTextureWithDescriptor:d];

    if (mtl_Trace())
        fprintf(stderr, "[mtl] depth buffer %lux%lu\n",
                (unsigned long)w, (unsigned long)h);
}

// --------------------------------------------------------------------------
// Which backend the window was built for. Cached: LTMacWin_GetMetalLayer()
// cannot change after the window exists, and this is called per draw.
// --------------------------------------------------------------------------
bool MTLDev_IsMetalBackend(void)
{
    static int s_n = -1;
    if (s_n < 0)
        s_n = LTMacWin_GetMetalLayer() ? 1 : 0;
    return s_n != 0;
}

// --------------------------------------------------------------------------
bool MTLDev_Init(void)
{
    if (g_bInited)
        return true;

    g_Device = MTLCreateSystemDefaultDevice();
    if (!g_Device)
    {
        fprintf(stderr, "[mtl] no Metal device available\n");
        return false;
    }

    // __bridge, not __bridge_transfer: the layer is owned by the view.
    g_Layer = (__bridge CAMetalLayer*)LTMacWin_GetMetalLayer();
    if (!g_Layer)
    {
        fprintf(stderr, "[mtl] window has no CAMetalLayer\n");
        return false;
    }

    g_Layer.device          = g_Device;
    g_Layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    // Opaque: the game fills every pixel, and a non-opaque layer makes the
    // compositor blend the whole frame for nothing.
    g_Layer.opaque          = YES;
    g_Layer.framebufferOnly = NO;   // readback needs the texture to be readable

    g_Queue    = [g_Device newCommandQueue];
    g_FrameSem = dispatch_semaphore_create(MTLDEV_FRAMES_IN_FLIGHT);
    g_bInited  = (g_Queue != nil);

    if (g_bInited)
        fprintf(stderr, "[mtl] device '%s'%s%s\n",
                [[g_Device name] UTF8String],
                g_Device.hasUnifiedMemory ? " (unified memory)" : "",
                g_Device.supportsBCTextureCompression ? " BC" : " NO-BC");

    return g_bInited;
}

void MTLDev_Term(void)
{
    g_Encoder = nil; g_CmdBuf = nil; g_Drawable = nil;
    g_LastPresented = nil; g_LastCmdBuf = nil; g_DepthTex = nil;
    g_Queue = nil; g_Device = nil; g_Layer = nil;
    g_FrameSem = nil;
    g_bInited = false;
}

bool MTLDev_IsReady(void) { return g_bInited; }

// Unretained handles: the globals here own the objects for the frame's
// lifetime, and the callers use them within it.
MTLEncoderRef MTLDev_Encoder(void) { return (__bridge MTLEncoderRef)g_Encoder; }
MTLDeviceRef  MTLDev_Device(void)  { return (__bridge MTLDeviceRef)g_Device;  }

bool MTLDev_FrameActive(void) { return g_Encoder != nil; }
int  MTLDev_FrameSlot(void)   { return g_nFrameSlot; }

bool MTLDev_TraceFrame(void)
{
	static long s_nFirst = -2;
	static double s_fAtSec = -1.0, s_fStart = 0.0;
	static long s_nArmed = -1;
	if (s_nFirst == -2)
	{
		const char *p = getenv("LT_TRACE_MTLFRAME");
		s_nFirst = (p && p[0]) ? atol(p) : -1;
		// ★ ...OR a WALL-CLOCK second. A frame NUMBER is not reproducible
		// between runs (the menu and the loading screen present as fast as they
		// can), which is the same reason LT_DUMP_SWAP_T exists. A time is a
		// repeatable address for a place in the game; a frame index is not.
		const char *pT = getenv("LT_TRACE_MTLFRAME_T");
		s_fAtSec = (pT && pT[0]) ? atof(pT) : -1.0;
		struct timeval tv; gettimeofday(&tv, NULL);
		s_fStart = (double)tv.tv_sec + tv.tv_usec * 1e-6;
	}

	if (s_fAtSec >= 0.0)
	{
		if (s_nArmed < 0)
		{
			struct timeval tv; gettimeofday(&tv, NULL);
			const double fElapsed = ((double)tv.tv_sec + tv.tv_usec * 1e-6) - s_fStart;
			if (fElapsed < s_fAtSec)
				return false;
			s_nArmed = (long)g_nFrameIndex;
			fprintf(stderr, "[mtl] frame trace armed at t=%.1fs, frame %ld\n",
			        fElapsed, s_nArmed);
		}
		return (long)g_nFrameIndex >= s_nArmed && (long)g_nFrameIndex < s_nArmed + 3;
	}

	if (s_nFirst < 0)
		return false;
	return (long)g_nFrameIndex >= s_nFirst && (long)g_nFrameIndex < s_nFirst + 3;
}

void MTLDev_SetClearColor(float r, float g, float b, float a)
{
    g_ClearColor = MTLClearColorMake(r, g, b, a);
}

void MTLDev_GetDrawableSize(int* w, int* h)
{
    if (w) *w = g_Layer ? (int)g_Layer.drawableSize.width  : 0;
    if (h) *h = g_Layer ? (int)g_Layer.drawableSize.height : 0;
}

// --------------------------------------------------------------------------
// Opens a render pass on the drawable already acquired for this frame.
// bClearColor/bClearDepth choose Clear vs Load per attachment -- Load is how a
// second pass keeps everything the first one drew.
// --------------------------------------------------------------------------
static void mtl_OpenPass(bool bClearColor, bool bClearDepth)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = g_Drawable.texture;
    rp.colorAttachments[0].loadAction  = bClearColor ? MTLLoadActionClear
                                                     : MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor  = g_ClearColor;
    rp.depthAttachment.texture         = g_DepthTex;
    rp.depthAttachment.loadAction      = bClearDepth ? MTLLoadActionClear
                                                     : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth      = 1.0;
    // ⚠️ Depth MUST be stored, not DontCare: the engine opens several passes
    // per frame (every mid-frame clear starts one), and a discarded depth
    // buffer would leave the next pass depth-testing against garbage. It is
    // never sampled after the frame, but it is very much read within it.
    rp.depthAttachment.storeAction     = MTLStoreActionStore;

    g_Encoder = [g_CmdBuf renderCommandEncoderWithDescriptor:rp];
    ++g_nPassesThisFrame;

    // Default viewport: the whole drawable, unless a caller has already chosen
    // one -- a mid-frame clear opens a NEW encoder, and losing the viewport
    // there would silently reset the player-view pass's depth range.
    if (g_aViewport[2] <= 0.0 || g_aViewport[3] <= 0.0)
    {
        g_aViewport[0] = 0.0; g_aViewport[1] = 0.0;
        g_aViewport[2] = (double)g_Drawable.texture.width;
        g_aViewport[3] = (double)g_Drawable.texture.height;
    }
    mtl_ApplyViewport();
}

bool MTLDev_BeginFrame(void)
{
    if (!g_bInited || g_Encoder)
        return false;

    @autoreleasepool {
        // Do not run more than MTLDEV_FRAMES_IN_FLIGHT ahead of the GPU: the
        // per-frame ring buffers are indexed by MTLDev_FrameSlot and would
        // otherwise be rewritten while still being read.
        dispatch_semaphore_wait(g_FrameSem, DISPATCH_TIME_FOREVER);

        // ⚠️ nextDrawable CAN and does return nil -- an occluded window or a
        // resize in flight will do it, and it blocks for up to a second first.
        // Returning false here (rather than pressing on with a nil drawable)
        // is what keeps a hidden window from taking the process down.
        g_Drawable = [g_Layer nextDrawable];
        if (!g_Drawable)
        {
            dispatch_semaphore_signal(g_FrameSem);   // nothing will signal it
            return false;
        }

        mtl_EnsureDepth(g_Drawable.texture.width, g_Drawable.texture.height);

        g_nFrameSlot = (int)(g_nFrameIndex % MTLDEV_FRAMES_IN_FLIGHT);
        g_CmdBuf     = [g_Queue commandBuffer];
        mtl_OpenPass(true, true);
        if (MTLDev_TraceFrame())
            fprintf(stderr, "[mtlf %llu] BEGIN (pass 1, clear)\n",
                    (unsigned long long)g_nFrameIndex);
    }
    return true;
}

bool MTLDev_EnsureFrame(void)
{
    if (g_Encoder)
        return true;
    return MTLDev_BeginFrame();
}

void MTLDev_Clear(bool bColor, bool bDepth, float r, float g, float b, float a)
{
    MTLDev_SetClearColor(r, g, b, a);

    // No frame yet: opening one applies the clear as a load action, which is
    // free. This is the common case -- the engine's clear IS the first thing
    // that touches the drawable in a menu frame.
    if (!g_Encoder)
    {
        MTLDev_BeginFrame();
        return;
    }
    if (!bColor && !bDepth)
        return;

    @autoreleasepool {
        [g_Encoder endEncoding];
        g_Encoder = nil;
        mtl_OpenPass(bColor, bDepth);
    }
    if (MTLDev_TraceFrame())
        fprintf(stderr, "[mtlf %llu] CLEAR mid-frame (colour=%d depth=%d) -> new pass\n",
                (unsigned long long)g_nFrameIndex, (int)bColor, (int)bDepth);
}

static void mtl_ApplyViewport(void)
{
    if (!g_Encoder || g_aViewport[2] <= 0.0 || g_aViewport[3] <= 0.0)
        return;
    MTLViewport vp = { g_aViewport[0], g_aViewport[1], g_aViewport[2], g_aViewport[3],
                       g_fDepthNear, g_fDepthFar };
    [g_Encoder setViewport:vp];
}

void MTLDev_SetViewport(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    g_aViewport[0] = (double)x; g_aViewport[1] = (double)y;
    g_aViewport[2] = (double)w; g_aViewport[3] = (double)h;
    mtl_ApplyViewport();
}

void MTLDev_SetDepthRange(float fNear, float fFar)
{
    g_fDepthNear = (double)fNear;
    g_fDepthFar  = (double)fFar;
    mtl_ApplyViewport();
}

void MTLDev_EndFrame(void)
{
    if (!g_Encoder)
        return;

    [g_Encoder endEncoding];

    // Keep the presented pixels alive for MTLDev_ReadbackFrame -- see the note
    // on g_LastPresented.
    g_LastPresented = g_Drawable.texture;
    g_LastCmdBuf    = g_CmdBuf;

    // ⚠️ A drawable texture is MANAGED on a discrete-GPU Mac, so getBytes there
    // reads a stale CPU copy unless the GPU writes are synchronised back first.
    // Apple silicon (this port's target) has unified memory and needs neither.
    if (!g_Device.hasUnifiedMemory)
    {
        id<MTLBlitCommandEncoder> blit = [g_CmdBuf blitCommandEncoder];
        [blit synchronizeResource:g_Drawable.texture];
        [blit endEncoding];
    }

    __block dispatch_semaphore_t sem = g_FrameSem;
    [g_CmdBuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        (void)cb;
        dispatch_semaphore_signal(sem);
    }];

    if (MTLDev_TraceFrame())
        fprintf(stderr, "[mtlf %llu] END + present\n",
                (unsigned long long)g_nFrameIndex);

    if (mtl_Profile())
    {
        // GPUStartTime/GPUEndTime are filled in by the driver when the buffer
        // completes -- the actual GPU cost of this frame, not a guess from FPS.
        const uint32_t nPasses = g_nPassesThisFrame;
        [g_CmdBuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            static double s_fGPU = 0.0, s_fCPU = 0.0;
            // ⚠️ AVERAGES HIDE THE ONLY THING WORTH FINDING. The frame is
            // vsync-locked at 60, so the mean is always ~16.8 ms and always
            // says "fine"; the interesting frames are the ones that MISS the
            // deadline and halve the rate. Track the worst in each window.
            static double s_fGPUMax = 0.0, s_fCPUMax = 0.0;
            static uint32_t s_nSlow = 0;
            static uint32_t s_nPasses = 0, s_nFrames = 0;
            static double s_fLastReport = 0.0;
            static double s_fPrevCPU = 0.0;

            const double fNow = CACurrentMediaTime();
            const double fGPUms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
            if (s_fPrevCPU > 0.0)
            {
                const double fDelta = (fNow - s_fPrevCPU) * 1000.0;
                s_fCPU += fDelta;
                if (fDelta > s_fCPUMax) s_fCPUMax = fDelta;
                if (fDelta > 18.0) ++s_nSlow;      // missed a 60 Hz deadline
            }
            s_fPrevCPU = fNow;
            s_fGPU += fGPUms;
            if (fGPUms > s_fGPUMax) s_fGPUMax = fGPUms;
            s_nPasses += nPasses;
            ++s_nFrames;

            if (s_fLastReport == 0.0) s_fLastReport = fNow;
            if (fNow - s_fLastReport >= 2.0 && s_nFrames > 1)
            {
                fprintf(stderr,
                    "[prof] GPU avg %5.2f max %6.2f | frame avg %6.2f max %7.2f ms"
                    " | %.1f passes | %.0f fps | MISSED %u/%u\n",
                    s_fGPU / s_nFrames, s_fGPUMax,
                    s_fCPU / (s_nFrames - 1), s_fCPUMax,
                    (double)s_nPasses / s_nFrames,
                    (double)(s_nFrames - 1) / (s_fCPU / 1000.0),
                    s_nSlow, s_nFrames);
                s_fGPU = s_fCPU = 0.0; s_fGPUMax = s_fCPUMax = 0.0;
                s_nSlow = 0; s_nPasses = 0; s_nFrames = 0;
                s_fLastReport = fNow;
            }
        }];
    }
    g_nPassesThisFrame = 0;

    [g_CmdBuf presentDrawable:g_Drawable];
    [g_CmdBuf commit];

    ++g_nFrameIndex;
    g_Encoder  = nil;
    g_CmdBuf   = nil;
    g_Drawable = nil;
}

// --------------------------------------------------------------------------
// Frame readback -- the verification instrument.
// --------------------------------------------------------------------------
// The depth analogue of MTLDev_ReadbackFrame; needs LT_DUMP_DEPTH so the
// texture was created shared. Returns window-space depth in 0..1, which is
// directly comparable with GL's glReadPixels(GL_DEPTH_COMPONENT): both
// projections map eye z to the same 0..1 window range.
bool MTLDev_ReadbackDepth(float* pOut, int* pW, int* pH)
{
    if (!g_DepthTex || !pOut || g_DepthTex.storageMode != MTLStorageModeShared)
        return false;
    if (g_LastCmdBuf)
        [g_LastCmdBuf waitUntilCompleted];
    const NSUInteger w = g_DepthTex.width, h = g_DepthTex.height;
    [g_DepthTex getBytes:pOut
             bytesPerRow:w * sizeof(float)
              fromRegion:MTLRegionMake2D(0, 0, w, h)
             mipmapLevel:0];
    if (pW) *pW = (int)w;
    if (pH) *pH = (int)h;
    return true;
}

bool MTLDev_ReadbackFrame(uint8_t* pRGB, int* pW, int* pH)
{
    if (!g_LastPresented || !pRGB)
        return false;

    // The frame was committed, not finished. Reading it without waiting gives a
    // partially drawn image -- an instrument that lies intermittently.
    if (g_LastCmdBuf)
        [g_LastCmdBuf waitUntilCompleted];

    const NSUInteger w = g_LastPresented.width;
    const NSUInteger h = g_LastPresented.height;
    if (pW) *pW = (int)w;
    if (pH) *pH = (int)h;

    const NSUInteger stride = w * 4;
    uint8_t* pBGRA = (uint8_t*)malloc(stride * h);
    if (!pBGRA)
        return false;

    [g_LastPresented getBytes:pBGRA
                  bytesPerRow:stride
                   fromRegion:MTLRegionMake2D(0, 0, w, h)
                  mipmapLevel:0];

    // ⚠️ The layer is BGRA8Unorm, so the channel order must be swapped on the
    // way out. Everything downstream (the PPM dumps, the A/B diffs) is RGB.
    for (NSUInteger y = 0; y < h; ++y)
    {
        const uint8_t* src = pBGRA + y * stride;
        uint8_t*       dst = pRGB  + y * w * 3;
        for (NSUInteger x = 0; x < w; ++x)
        {
            dst[x * 3 + 0] = src[x * 4 + 2];   // R
            dst[x * 3 + 1] = src[x * 4 + 1];   // G
            dst[x * 3 + 2] = src[x * 4 + 0];   // B
        }
    }

    free(pBGRA);
    return true;
}
