
#include <windows.h>
#include "ltbasedefs.h"
#include "ltbasetypes.h"
#include "colorops.h"
#include "renderstruct.h"
#if defined(__APPLE__)
#include <stdlib.h>   // malloc lives in stdlib.h on macOS (no <malloc.h>)
#else
#include <malloc.h>
#endif
#include <string.h>

#ifdef LT_MACOS
// macOS: this "null" renderer is the real renderer's front door. It drives the
// Metal backend through the LTMacWin_* C ABI instead of the Win32 GDI/DIB path
// used elsewhere in this file.
#include <stdio.h>
#include <math.h>
#include <sys/time.h>   // gettimeofday (FPS counter)
#include "ltmacwindow.h"
#include "sys/shared/world_renderdata.h"
#include "sys/shared/model_renderdata.h"
#include "sys/shared/render_drawprim.h"
#include "sys/shared/render_convar.h"  // renderer console vars (see render_convar.cpp)
#include "sys/shared/render_polygrid.h"   // OT_POLYGRID = water / ice
#include "sys/shared/render_particles.h"  // OT_PARTICLESYSTEM = fire, smoke, waterfall
// ★ THE METAL BACKEND -- the only backend. The gl_* headers above no longer
// name a GL module: those files hold the backend-neutral parse/walk/scene code
// the Metal passes consume, and are being renamed out of gl/ as they move.
#include "mtl_device.h"
#include "mtl_texture.h"
#include "mtl_drawprim.h"
#include "mtl_matrix.h"
#include "mtl_world.h"
#include "mtl_model.h"
#include "pixelformat.h"   // CalcImageSize (ConvertTexDataToDD passthrough)
#include "de_objects.h"    // MAX_OBJECT_RENDER_GROUPS
#include "renderinfostruct.h"
#include <set>
// Clear colour. RETAIL CLEARS TO BLACK — the loud green here was a bring-up
// diagnostic (a green window proved the *engine renderer* was driving the GL
// context rather than the Cocoa view's placeholder). It is kept available
// behind LT_DEBUG_CLEAR=1 for exactly that purpose, but must never be the
// default: it shows through wherever the game hasn't painted (e.g. the main
// menu, whose orange collage comes from the not-yet-ported ClientFX).
static float g_fClearR = 0.10f, g_fClearG = 0.55f, g_fClearB = 0.25f;

static bool nr_UseDebugClear()
{
	static int s_nDebugClear = -1;
	if (s_nDebugClear < 0)
		s_nDebugClear = getenv("LT_DEBUG_CLEAR") ? 1 : 0;
	return s_nDebugClear != 0;
}

// fScale dims the debug colour (the scene pass uses a darker variant).
// The clear happens in the render pass's load action, so this only records the
// colour BeginFrame/MTLDev_Clear will use.
static void nr_SetMetalClearColor(float fScale)
{
	if (nr_UseDebugClear())
		MTLDev_SetClearColor(g_fClearR * fScale, g_fClearG * fScale, g_fClearB * fScale, 1.0f);
	else
		MTLDev_SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}
#endif

typedef uint32 DDWORD;

typedef struct DIB_BMI256_struct
{
	BITMAPINFOHEADER	hdr;
	RGBQUAD				colors[256];
} DIB_BMI;

typedef struct NullBuf_t
{
	DDWORD			m_Width, m_Height;
	unsigned short	m_Data[1];
} NullBuf;



DIB_BMI g_bmi;
HWND g_hWnd = NULL;
HBITMAP g_hBitmap = NULL;
void *g_pDibBytes = NULL;
DWORD g_DibWidth, g_DibHeight, g_DibPitchBytes;
bool g_bInOptimized2D=false;
bool g_bIn3D=false;


// ---------------------------------------------------------------- //
// Internal functions.
// ---------------------------------------------------------------- //

int nr_Init(struct RenderStructInit *pInit)
{
#ifdef LT_MACOS
	pInit->m_RendererVersion = LTRENDER_VERSION;

	if (!MTLDev_Init())
	{
		fprintf(stderr, "[nr] Metal init FAILED\n");
		return RENDER_ERROR;
	}
	int vpW = (int)pInit->m_Mode.m_Width, vpH = (int)pInit->m_Mode.m_Height;
	LTMacWin_GetSize(&vpW, &vpH);          // actual (point/backing) drawable size

	// Report the ACTUAL drawable as the mode we set (the contract allows the
	// renderer to adjust it). display.cfg asks for the retail resolution
	// (e.g. 1600x1200); until the window is resizable the drawable is the
	// truth — this keeps the engine's screen coordinate system (console/2D
	// layout, camera rects) aligned with the pixels we actually have.
	pInit->m_Mode.m_Width  = (uint32)vpW;
	pInit->m_Mode.m_Height = (uint32)vpH;
	g_DibWidth  = (uint32)vpW;
	g_DibHeight = (uint32)vpH;
	if (g_pRenderStruct)
	{
		g_pRenderStruct->m_Width  = (uint32)vpW;   // engine reads these before Init returns them
		g_pRenderStruct->m_Height = (uint32)vpH;
	}

	// Publish the renderer's console variables (rendererconsolevars.h) to the
	// engine console — mirrors d3d_CreateConsoleVariables at common_init.cpp:120.
	// MUST happen here, before any world loads: WorldProperties writes the
	// level's authored fog/far-Z/sky settings into these by name, and a write to
	// a variable that does not exist is silently dropped. See render_convar.cpp.
	RenderConVar_Create(g_pRenderStruct);
	RenderConVar_Read(g_pRenderStruct);

	// One cleared frame, so a successful render-init is immediately visible in
	// the window.
	nr_SetMetalClearColor(1.0f);
	if (MTLDev_EnsureFrame())
		MTLDev_EndFrame();
	return RENDER_OK;
#else
	RECT screenRect, wndRect;
	HDC hDC;

	

	pInit->m_RendererVersion = LTRENDER_VERSION;
	g_hWnd = (HWND)pInit->m_hWnd;
	
	// Size the window to how they want it.
	GetWindowRect(GetDesktopWindow(), &screenRect);
	
	// Setup the client rectangle.
	wndRect.left = 0;//((screenRect.right - screenRect.left) - pInit->m_Mode.m_Width) / 2;
	wndRect.top = 0;//((screenRect.right - screenRect.left) - pInit->m_Mode.m_Width) / 2;
	wndRect.right = 0;//wndRect.left + pInit->m_Mode.m_Width;
	wndRect.bottom = 0;//wndRect.top + pInit->m_Mode.m_Height;

	// Figure out the full window coordinates given the client coordinates.
	AdjustWindowRect(&wndRect, WS_OVERLAPPEDWINDOW, FALSE);

	SetWindowPos(g_hWnd, 0, wndRect.left, wndRect.top, wndRect.right-wndRect.left,
		wndRect.bottom-wndRect.top, SWP_NOREPOSITION);

	
	// Create our DIB buffer for the 'screen'.
	hDC = GetDC(NULL);
	if(hDC)
	{
		memset(&g_bmi.hdr, 0, sizeof(BITMAPINFOHEADER));

		g_bmi.hdr.biSize         = sizeof(BITMAPINFOHEADER);
		g_bmi.hdr.biWidth        = pInit->m_Mode.m_Width;
		g_bmi.hdr.biHeight       = -((int)pInit->m_Mode.m_Height);
		g_bmi.hdr.biBitCount     = 16;
		g_bmi.hdr.biPlanes       = 1;
		g_bmi.hdr.biCompression  = BI_RGB;
		g_bmi.hdr.biSizeImage    = 0L;
		g_bmi.hdr.biClrUsed      = 0;
		g_bmi.hdr.biClrImportant = 0;

		g_hBitmap = CreateDIBSection(hDC, (BITMAPINFO*)&g_bmi, DIB_PAL_COLORS, (void**)&g_pDibBytes, NULL, 0);
		if(g_hBitmap)
		{
			g_DibWidth = pInit->m_Mode.m_Width;
			g_DibHeight = pInit->m_Mode.m_Height;
			g_DibPitchBytes = g_DibWidth * 2;
		}

		ReleaseDC(NULL, hDC);
	}

	return RENDER_OK;
#endif // LT_MACOS
}


void nr_Term(bool bFullTerm)
{
#ifdef LT_MACOS
	// ⚠️ No RWorld_Free() here. The GL branch used to call it at Term; Metal
	// never did — RWorld_Load frees the previous world at load time, which is
	// the path every level change takes. Leaving it out preserves the Metal
	// behaviour that has always shipped; the only cost is that the last loaded
	// world is still allocated at process exit.
	MTLDrawPrim_Term();
	MTLDev_Term();
#endif
	g_hWnd = 0;

	if(g_hBitmap)
	{
		DeleteObject(g_hBitmap);
		g_hBitmap = 0;
	}
	
	g_pDibBytes = NULL;
}


// SharedTexture::m_pRenderData belongs to the Metal texture manager alone.
void nr_BindTexture(SharedTexture *pTexture, bool bTextureChanged)
{
#ifdef LT_MACOS
	MTLTex_Bind(pTexture, bTextureChanged);
#endif
}


void nr_UnbindTexture(SharedTexture *pTexture)
{
#ifdef LT_MACOS
	MTLTex_Unbind(pTexture);
#endif
}


HRENDERCONTEXT nr_CreateContext()
{
	HRENDERCONTEXT p;
	LT_MEM_TRACK_ALLOC(p = (HRENDERCONTEXT)LTMemAlloc(1),LT_MEM_TYPE_RENDERER);
	return p;
}


void nr_DeleteContext(HRENDERCONTEXT hContext)
{
	if(hContext)
	{
		LTMemFree(hContext);
	}
}


void nr_Clear(LTRect *pRect, DDWORD flags, LTRGBColor& ClearColor)
{
#ifdef LT_MACOS
	// Honour the colour the engine asked for (the game clears to black before
	// drawing the interface).
	if (nr_UseDebugClear())
		nr_SetMetalClearColor(1.0f);
	MTLDev_Clear(true, true,
	             nr_UseDebugClear() ? g_fClearR : ClearColor.rgb.r * (1.0f / 255.0f),
	             nr_UseDebugClear() ? g_fClearG : ClearColor.rgb.g * (1.0f / 255.0f),
	             nr_UseDebugClear() ? g_fClearB : ClearColor.rgb.b * (1.0f / 255.0f),
	             1.0f);
#else
	BYTE *pCurLine, *pEndLine;

	if(g_pDibBytes && g_hBitmap)
	{
		pCurLine = (BYTE*)g_pDibBytes;
		pEndLine = pCurLine + g_DibHeight*g_DibPitchBytes;
		while(pCurLine != pEndLine)
		{
			memset(pCurLine, 0, g_DibPitchBytes);
			pCurLine += g_DibPitchBytes;
		}
	}
#endif // LT_MACOS
}


#ifdef LT_MACOS
// Dump the current back buffer to a .ppm (headless verification).
// A single requested frame keeps the historic /tmp/nolf2_frame.ppm name; a
// LIST of frames writes one numbered file each, so a run can be sampled.
static void nr_DumpDrawable(int nFrame, const char *pWhat, bool bNumbered)
{
	int nDrawW = 0, nDrawH = 0;
	LTMacWin_GetSize(&nDrawW, &nDrawH);
	if (nDrawW <= 0 || nDrawH <= 0)
		return;
	unsigned char *pPixels = (unsigned char*)malloc((size_t)nDrawW * nDrawH * 3);
	if (!pPixels)
		return;

	// ★ Metal reads back the LAST PRESENTED frame and hands it over top-down
	// already, so it needs no row flip (GL's readback was bottom-up and the
	// writer below flipped it). It also waits for the GPU first --
	// see MTLDev_ReadbackFrame.
	{
		int nGotW = 0, nGotH = 0;
		if (!MTLDev_ReadbackFrame(pPixels, &nGotW, &nGotH))
		{
			free(pPixels);
			return;
		}
		nDrawW = nGotW; nDrawH = nGotH;
	}
	char sPath[128];
	if (bNumbered)
		snprintf(sPath, sizeof(sPath), "/tmp/nolf2_%s_%d.ppm", pWhat, nFrame);
	else
		snprintf(sPath, sizeof(sPath), "/tmp/nolf2_frame.ppm");
	FILE *fp = fopen(sPath, "wb");
	if (fp)
	{
		fprintf(fp, "P6\n%d %d\n255\n", nDrawW, nDrawH);
		fwrite(pPixels, 1, (size_t)nDrawW * nDrawH * 3, fp);
		fclose(fp);
		fprintf(stderr, "[nr] %s %d dumped to %s (%dx%d)\n",
		        pWhat, nFrame, sPath, nDrawW, nDrawH);
	}
	free(pPixels);
}

// Parse a "n" or "n,m,o,..." frame list out of an env var. Returns the count
// of frames parsed; nCount==1 means "the historic single-file behaviour".
enum { NR_MAX_DUMP_FRAMES = 16 };

// The presented-frame counter LT_DUMP_SWAP indexes. Exposed so that a one-shot
// census can say WHICH swap frame it fired on.
//
// ⚠️ AND SO THAT NOBODY TRUSTS IT ACROSS RUNS. The main menu presents with no
// world loaded and therefore runs at an enormous frame rate: measured over three
// runs of the identical command line, the frame on which C01S01's water first
// drew was 1706, 59171 and 70928. A frame NUMBER is only meaningful within one
// run — which is exactly why LT_DUMP_SWAP_T below exists.
int g_nSwapCount = 0;
static int nr_ParseDumpList(const char *pEnv, int *pOut)
{
	if (!pEnv)
		return 0;
	int nCount = 0;
	const char *p = pEnv;
	while (*p && nCount < NR_MAX_DUMP_FRAMES)
	{
		while (*p == ',' || *p == ' ')
			++p;
		if (!*p)
			break;
		int nVal = atoi(p);
		if (nVal > 0)
			pOut[nCount++] = nVal;
		while (*p && *p != ',' && *p != ' ')
			++p;
	}
	return nCount;
}
#endif

// ★ LT_DUMP_FRAME under Metal. The GL path reads the back buffer mid-frame,
// right after the scene and before any 2D. Metal cannot: its readback retains
// the LAST PRESENTED texture, so the dump has to be deferred to just after
// EndFrame in nr_SwapBuffers. With the null shell (which draws only the
// console) that is the same picture; with the real game shell it also carries
// the HUD, which is why LT_DUMP_SWAP_T remains the one to reach for there.
// LT_DUMP_DEPTH=1 with LT_DUMP_FRAME=<n>: write the WINNING FRAGMENT'S DEPTH
// for frame n to /tmp/nolf2_depth_<n>.f32 ("DEPTH <w> <h>\n" then w*h float32,
// window space 0..1). Both backends produce the same encoding, so the two files
// diff directly.
//
// ⚠️ THIS IS THE INSTRUMENT FOR "WHICH LAYER WON THE PIXEL". Every comparison
// of the INPUTS to the foliage difference (draws, fog state, fog factor, alpha
// gate/ref/operand, filtering, mip chains) now says the two backends agree; the
// only thing left that can differ is which fragment survived, and that is a
// depth question, not a colour one.
static void nr_DumpDepth(int nFrame)
{
	// Same size source as nr_DumpDrawable: LTMacWin_GetSize is the drawable in
	// pixels; Metal then reports back what it actually read.
	int w = 0, h = 0;
	LTMacWin_GetSize(&w, &h);
	if (w <= 0 || h <= 0)
		return;
	std::vector<float> aDepth((size_t)w * h, 0.0f);

	if (!MTLDev_ReadbackDepth(&aDepth[0], &w, &h))
	{
		fprintf(stderr, "[nr] depth dump: readback failed (LT_DUMP_DEPTH set at startup?)\n");
		return;
	}

	char sPath[128];
	snprintf(sPath, sizeof(sPath), "/tmp/nolf2_depth_%d.f32", nFrame);
	FILE *fp = fopen(sPath, "wb");
	if (!fp)
		return;
	fprintf(fp, "DEPTH %d %d\n", w, h);
	fwrite(&aDepth[0], sizeof(float), (size_t)w * h, fp);
	fclose(fp);
	fprintf(stderr, "[nr] depth %d dumped to %s (%dx%d)\n", nFrame, sPath, w, h);
}

// ★ PASS ISOLATION, AND BOTH BACKENDS MUST OBEY IT.
//   LT_SKY_ONLY=1   draws the sky and nothing else.
//   LT_WORLD_ONLY=1 draws the sky + the BSP world and nothing else.
// "Which pass produces this artifact?" is the question these answer, and it is
// the one that keeps coming up (§48 found the world backface z-fighting exactly
// this way).
//
// ⚠️⚠️ LT_SKY_ONLY USED TO BE READ ONLY IN THE METAL BRANCH of nr_RenderScene.
// Under GL it therefore did NOTHING -- the whole scene drew -- so a GL-vs-Metal
// "sky isolation" A/B compared a FULL SCENE against a bare sky and reported
// ~52 mean abs of pure instrument error. Both branches read one decision now,
// from above the backend split, exactly as LT_NO_FOG had to (§96).
// ⇒ An isolation switch that only one backend obeys is worse than no switch at
// all: it produces numbers.
static bool nr_SkyOnly(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_SKY_ONLY") ? 1 : 0;
	return s_n != 0;
}

static bool nr_WorldOnly(void)
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_WORLD_ONLY") ? 1 : 0;
	return s_n != 0;
}

static int g_nPendingSceneDump = 0;
static bool g_bPendingSceneNumbered = false;

#ifdef LT_MACOS
// LT_TEST_LIGHTGROUPS_AT=<n>: switch the pure-Gouraud light groups OFF after
// the n-th scene — i.e. once the world has already been drawn and, under Metal,
// once its vertex-colour buffers are already on the GPU. That is the whole
// point: the load-time LT_TEST_LIGHTGROUPS_ON runs before any buffer exists, so
// only a MID-RUN change can prove the re-upload. Dump a later frame and A/B it
// against LT_RENDER_GL=1; the change takes effect from frame n+1.
// ⚠️ Called from BOTH branches of nr_RenderScene. The Metal branch returns
// early, so a hook placed only in the GL tail never runs under Metal — which is
// exactly how this one silently did nothing on its first outing.
static void nr_TestLightGroupTick(int nSceneCount)
{
	static int s_nFrame = -2;
	if (s_nFrame == -2)
	{
		const char *pAt = getenv("LT_TEST_LIGHTGROUPS_AT");
		s_nFrame = pAt ? atoi(pAt) : -1;
	}
	if (s_nFrame > 0 && nSceneCount == s_nFrame)
		fprintf(stderr, "[nr] TEST: frame %d switched %u Gouraud light groups OFF\n",
		        nSceneCount, RWorld_TestSwitchGouraudLightGroupsOff());
}
#endif

int nr_RenderScene(struct SceneDesc *pScene)
{
#ifdef LT_MACOS
	{
		// Per-scene bookkeeping (the LT_TRACE_UI frame counter). The GL path
		// calls this at the top of every scene; the Metal branch was returning
		// before it, which silently disabled every UI diagnostic under Metal.
		RModel_BeginSceneFrame();

		const bool bObjList = (pScene->m_DrawMode == DRAWMODE_OBJECTLIST) &&
		                      pScene->m_pObjectList && pScene->m_ObjectListSize > 0;

		int nWinW = 0, nWinH = 0;
		LTMacWin_GetSize(&nWinW, &nWinH);
		if (nWinW <= 0 || nWinH <= 0) { nWinW = 640; nWinH = 480; }

		int nVpX = pScene->m_Rect.left;
		int nVpY = pScene->m_Rect.top;
		int nVpW = pScene->m_Rect.right  - pScene->m_Rect.left;
		int nVpH = pScene->m_Rect.bottom - pScene->m_Rect.top;
		if (nVpW <= 0 || nVpH <= 0) { nVpX = 0; nVpY = 0; nVpW = nWinW; nVpH = nWinH; }

		// ⚠️ AN OBJECT-LIST PASS MUST NOT CLEAR: CInterfaceMgr calls
		// RenderObjects once per menu LAYER, so several object-list scenes
		// compose into one frame and a clear here wipes all but the last.
		// (The GAME owns the clear in those states -- it reaches us as nr_Clear.)
		if (!bObjList)
		{
			// ⚠️ WITH FOG ON, CLEAR TO THE FOG COLOUR. Anywhere the world does
			// not reach should read as haze, not as the void behind it — the GL
			// path below does exactly this, and c01s01's fog is WHITE, so a
			// black clear is not a subtle difference. (The fog state for this
			// frame was pushed by RWorld_ApplyFog just above.)
			float fFogR, fFogG, fFogB;
			if (RWorld_GetFogColor(fFogR, fFogG, fFogB))
				MTLDev_Clear(true, true, fFogR, fFogG, fFogB, 1.0f);
			else
			{
				nr_SetMetalClearColor(0.25f);
				MTLDev_Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
			}
		}
		else
			MTLDev_EnsureFrame();

		// Metal's viewport origin is top-left, like the engine's rect -- unlike
		// GL, no y flip.
		MTLDev_SetViewport(nVpX, nVpY, nVpW, nVpH);

		// Same LTRotation -> view matrix as the GL path below (third row
		// negated: Lithtech is left-handed, eye space looks down -Z).
		const float fZNear = 5.0f, fZFar = 100000.0f;
		const float fFovY = (pScene->m_yFov > 0.01f) ? pScene->m_yFov : 1.2f;
		const float fFovX = (pScene->m_xFov > 0.01f) ? pScene->m_xFov : 1.6f;
		const float fTop   = fZNear * tanf(fFovY * 0.5f);
		const float fRight = fZNear * tanf(fFovX * 0.5f);
		LTVector vR = pScene->m_Rotation.Right();
		LTVector vU = pScene->m_Rotation.Up();
		LTVector vF = pScene->m_Rotation.Forward();
		const LTVector &vP = pScene->m_Pos;

		float aView[16];
		aView[0] =  vR.x; aView[4] =  vR.y; aView[8]  =  vR.z; aView[12] = -vR.Dot(vP);
		aView[1] =  vU.x; aView[5] =  vU.y; aView[9]  =  vU.z; aView[13] = -vU.Dot(vP);
		aView[2] = -vF.x; aView[6] = -vF.y; aView[10] = -vF.z; aView[14] =  vF.Dot(vP);
		aView[3] = 0.0f;  aView[7] = 0.0f;  aView[11] = 0.0f;  aView[15] = 1.0f;

		// Billboards need the camera basis (they face it).
		RSprite_SetCamera(vR, vU, vF, vP);
		RParticle_SetCamera(vR, vU, vF, vP);
		RPolyGrid_SetCamera(vR, vU, vF, vP);

		// Publish the scene transform for CAMERA/WORLD-space drawprims, and the
		// matching Metal projection for the world and model passes.
		RenderDrawPrim_SetSceneTransform(aView, fRight, fTop, fZNear, fZFar);
		float aProj0[16];
		mtl_Frustum(aProj0, fRight, fTop, fZNear, fZFar);

		if (bObjList)
		{
			// ★ DRAWMODE_OBJECTLIST: the caller named exactly what to draw and
			// wants NO world -- the whole interface (menus, loading screens).
			// Models are converted now; sprites and particles are not.
			MTLModel_SetTransform(aView, aProj0);
			RModel_ProcessAttachments();
			RObjectList_Draw(pScene->m_pObjectList, pScene->m_ObjectListSize);
			return 0;
		}

		// --- sky pass (backdrop, before the world) ---
		// Same sky camera as the GL path: the camera's normalized position
		// within the world extents mapped into the SkyDef view box, with a much
		// nearer near-plane. Depth is untouched (the params say so).
		RWorld_SetSkyObjects(pScene->m_SkyObjects, pScene->m_nSkyObjects);
		LTVector vBoundsCenter, vBoundsHalf;
		if (pScene->m_nSkyObjects > 0 && RWorld_GetBounds(vBoundsCenter, vBoundsHalf))
		{
			LTVector vMin = vBoundsCenter - vBoundsHalf;
			LTVector vMax = vBoundsCenter + vBoundsHalf;
			LTVector vPercent(0.5f, 0.5f, 0.5f);
			if (vMax.x > vMin.x) vPercent.x = (vP.x - vMin.x) / (vMax.x - vMin.x);
			if (vMax.y > vMin.y) vPercent.y = (vP.y - vMin.y) / (vMax.y - vMin.y);
			if (vMax.z > vMin.z) vPercent.z = (vP.z - vMin.z) / (vMax.z - vMin.z);

			const SkyDef &cSky = pScene->m_SkyDef;
			LTVector vSkyPos(
				cSky.m_ViewMin.x + (cSky.m_ViewMax.x - cSky.m_ViewMin.x) * vPercent.x,
				cSky.m_ViewMin.y + (cSky.m_ViewMax.y - cSky.m_ViewMin.y) * vPercent.y,
				cSky.m_ViewMin.z + (cSky.m_ViewMax.z - cSky.m_ViewMin.z) * vPercent.z);

			const float fSkyNear = 0.3f, fSkyFar = 30000.0f;
			float aSkyProj[16];
			mtl_Frustum(aSkyProj, tanf(fFovX * 0.5f) * fSkyNear,
			            tanf(fFovY * 0.5f) * fSkyNear, fSkyNear, fSkyFar);

			float aSkyView[16];
			memcpy(aSkyView, aView, sizeof(aSkyView));
			aSkyView[12] = -vR.Dot(vSkyPos);
			aSkyView[13] = -vU.Dot(vSkyPos);
			aSkyView[14] =  vF.Dot(vSkyPos);

			RWorld_ApplyFog(true);   // SkyFogNearZ/FarZ
			MTLWorld_SetSceneTransform(aSkyView, aSkyProj);
			RWorld_DrawSkyWorldModels();
		}

		// --- main scene ---
		RWorld_ApplyFog(false);      // swap in the world's fog range
		MTLWorld_SetSceneTransform(aView, aProj0);
		MTLModel_SetTransform(aView, aProj0);

		// ★ FIRST: recompute every attached object's transform from its parent.
		// Must precede all draw passes and must cover every object type -- a
		// door handle attached to a world-model door is only re-oriented here.
		RModel_ProcessAttachments();

		// ⚠️ The two world-model calls are DELIBERATELY not collapsed: D3D draws
		// solid world models, then models, then the translucent set
		// (drawobjects.cpp:218). Models are not converted yet, so nothing sits
		// between them today -- keep the shape, so the ordering is already right
		// when the model pass lands (§43: collapsing it let a glass pane's depth
		// write hide every character behind it).
		// Pass isolation, the Metal analogue of LT_WORLD_ONLY: LT_SKY_ONLY=1
		// draws the sky and nothing else, LT_WORLD_ONLY=1 the reverse. "Which
		// pass produces this artifact?" is the question these answer, and it is
		// the one that keeps coming up (§48 found the world backface z-fighting
		// exactly this way).
		if (!nr_SkyOnly())
		{
			RWorld_Draw();
			if (!nr_WorldOnly())
			{
				// ⚠️ D3D's order (drawobjects.cpp:218): solid world models ->
				// SOLID MODELS -> the whole translucent set. Running the world
				// models together let a glass pane's depth write reject every
				// character behind it (§43).
				RWorld_DrawWorldModels(false);        // solid
				RPolyGrid_Draw(false);                // opaque water/ice
				RModel_DrawModels(fRight / fTop);     // world-space models
				RWorld_DrawDynamicLights();           // lamp pools, muzzle flashes
				RWorld_DrawWorldModels(true);         // glass etc.
				RPolyGrid_Draw(true);                 // translucent water
				RParticle_DrawSystems();              // fire, smoke, sparks
				RSprite_DrawSprites();                // lamp halos / glows

				// ⚠️ LAST. The player-view pass CLEARS THE DEPTH BUFFER so the
				// weapon can never clip into the world; anything world-space
				// drawn after it would then depth-test against an empty buffer
				// and paint over the whole level.
				RModel_DrawPlayerView(fRight / fTop);
			}
		}

		// Everything after this point is 2D (console, HUD) and must not fog.
		RWorld_DisableFog();

		// Frame dump for headless verification, same counter and env var as the
		// GL path below; serviced after the present (see g_nPendingSceneDump).
		{
			static int s_aDumpFrame[NR_MAX_DUMP_FRAMES];
			static int s_nDumpFrameCount = -1;
			if (s_nDumpFrameCount < 0)
				s_nDumpFrameCount = nr_ParseDumpList(getenv("LT_DUMP_FRAME"), s_aDumpFrame);
			static int s_nSceneCount = 0;
			++s_nSceneCount;
			nr_TestLightGroupTick(s_nSceneCount);
			for (int i = 0; i < s_nDumpFrameCount; ++i)
				if (s_nSceneCount == s_aDumpFrame[i])
				{
					g_nPendingSceneDump = s_nSceneCount;
					g_bPendingSceneNumbered = (s_nDumpFrameCount > 1);
				}
		}
		return 0;
	}
	return 0;
#else
	return 0;
#endif
}

#ifdef LT_MACOS
// World render-data loader for the RenderStruct seam.
bool nr_LoadWorldData(ILTStream *pStream)
{
	return RWorld_Load(pStream);
}
#endif


// The engine's texture code stores data in the file's native format on macOS
// (no D3D-format conversion), so "conversion" is a straight copy when the
// formats already match — enough for ILTTexInterface::CreateTextureFromData.
bool nr_ConvertTexDataToDD(uint8 *pSrcData, PFormat *pSrcFormat, uint32 SrcWidth, uint32 SrcHeight,
                           uint8 *pDstData, PFormat *pDstFormat, BPPIdent eDstType,
                           uint32 nDstFlags, uint32 DstWidth, uint32 DstHeight)
{
	if (pSrcFormat->GetType() != pDstFormat->GetType() ||
	    SrcWidth != DstWidth || SrcHeight != DstHeight)
		return false;
	memcpy(pDstData, pSrcData, CalcImageSize(pSrcFormat->GetType(), SrcWidth, SrcHeight));
	return true;
}

// Glow render-style hooks: the real game client shell (CShell) calls these
// during render init. The GL bring-up renderer has no glow post-process, so
// they succeed as no-ops (NULL fn-ptrs here would crash — the RenderStruct is
// memset to 0, so anything left unset is a jump-to-0).
bool nr_AddGlowRenderStyleMapping(const char *, const char *) { return true; }
bool nr_SetGlowDefaultRenderStyle(const char *) { return true; }
bool nr_SetNoGlowRenderStyle(const char *) { return true; }

// --- Object render groups ---------------------------------------------------
// Port of CObjectGroupMgr (runtime/render_a/src/sys/d3d/objectgroupmgr.h). Every
// LTObject carries an m_nRenderGroup; the game hides whole groups at once (the
// player's own body in first person, cinematic actors, ...). Disabling is
// REF-COUNTED, not a flag, so nested hide/show pairs balance out.
// CGameClientShell::OnEnterWorld calls SetAllObjectGroupEnabled through
// CLTClient::SetAllObjectRenderGroupEnabled the moment the world loads, so these
// three hooks are on the critical path into a level -- unset they are a jump-to-0.
static uint16 s_nObjectGroups[MAX_OBJECT_RENDER_GROUPS] = { 0 };

bool nr_IsObjectGroupEnabled(uint32 nGroup)
{
	if (nGroup >= MAX_OBJECT_RENDER_GROUPS)
		return true;
	return s_nObjectGroups[nGroup] == 0;
}

void nr_SetObjectGroupEnabled(uint32 nGroup, bool bEnable)
{
	if (nGroup >= MAX_OBJECT_RENDER_GROUPS)
		return;

	if (bEnable)
	{
		if (s_nObjectGroups[nGroup])
			s_nObjectGroups[nGroup]--;
	}
	else
	{
		s_nObjectGroups[nGroup]++;
	}
}

void nr_SetAllObjectGroupEnabled()
{
	for (uint32 nGroup = 0; nGroup < MAX_OBJECT_RENDER_GROUPS; ++nGroup)
		s_nObjectGroups[nGroup] = 0;
}

// --- Remaining RenderStruct hooks -------------------------------------------
// RenderStruct is memset to 0 and its members are called WITHOUT null checks,
// so every unfilled hook is a latent jump-to-0 that fires the first time the
// real game shell walks into that feature (SetAllObjectRenderGroupEnabled on
// world entry and SetOccluderEnabled from CDynamicOccluderVolumeFX each cost a
// debugging round-trip). These are the rest of them, filled with the honest
// minimum for the GL bring-up renderer so no path can crash on a null pointer.

// Occluders: the GL renderer draws brute-force with no occlusion culling, so
// enabling/disabling an occluder is a no-op -- but it must SUCCEED, because the
// game treats a failure as a broken world. State is tracked so Get mirrors Set.
static std::set<uint32> s_DisabledOccluders;

LTRESULT nr_SetOccluderEnabled(uint32 nID, bool bEnabled)
{
	if (bEnabled)
		s_DisabledOccluders.erase(nID);
	else
		s_DisabledOccluders.insert(nID);
	return LT_OK;
}

LTRESULT nr_GetOccluderEnabled(uint32 nID, bool *pEnabled)
{
	if (!pEnabled)
		return LT_INVALIDPARAMS;
	*pEnabled = (s_DisabledOccluders.find(nID) == s_DisabledOccluders.end());
	return LT_OK;
}

// Texture effects = the D3D render-shader ("effect") variables. No shader
// pipeline here, so there are no variables to name: ID 0 == "none", and setting
// one fails honestly rather than pretending it took.
uint32 nr_GetTextureEffectVarID(const char *, uint32) { return 0; }
bool   nr_SetTextureEffectVar(uint32, uint32, float)  { return false; }

// Optimized-2D render state. Our 2D goes through nr_BlitToScreen /
// nr_WarpToScreen, which apply the blend themselves; these just have to keep
// the state coherent for the engine's Get/Set pairs.
static LTSurfaceBlend s_Optimized2DBlend = LTSURFACEBLEND_ALPHA;
static HLTCOLOR       s_Optimized2DColor = 0;

bool nr_SetOptimized2DBlend(LTSurfaceBlend blend) { s_Optimized2DBlend = blend; return true; }
bool nr_GetOptimized2DBlend(LTSurfaceBlend &blend) { blend = s_Optimized2DBlend; return true; }
bool nr_SetOptimized2DColor(HLTCOLOR color) { s_Optimized2DColor = color; return true; }
bool nr_GetOptimized2DColor(HLTCOLOR &color) { color = s_Optimized2DColor; return true; }

// DrawPrim texture selection. The engine routes DrawPrim through the ILTDrawPrim
// interface (gl_drawprim.cpp), which owns its own texture binding; these
// RenderStruct entry points exist for the D3D path and have nothing to do here.
void nr_DrawPrimSetTexture(SharedTexture *) {}
void nr_DrawPrimDisableTextures() {}

// Texture-format queries. The GL uploader (gl_texture.cpp) accepts whatever the
// DTX carries and converts, so every format the engine can hand us is
// "supported"; reporting a D3DFORMAT is meaningless without D3D.
bool nr_QueryDDSupport(PFormat *) { return true; }
D3DFORMAT nr_GetTextureDDFormat1(BPPIdent, uint32) { return (D3DFORMAT)0; }
bool nr_GetTextureDDFormat2(BPPIdent, uint32, PFormat *) { return false; }

uint16 nr_IncCurTextureFrameCode() { return 0; }

// Cubic env maps are a D3D render-target feature; no reflective surfaces here.
void nr_MakeCubicEnvMap(const char *, uint32, const SceneDesc &) {}

// Per-frame poly counters for the F1 render-info overlay. The GL path does not
// keep these statistics yet -- report zeroes rather than leaving a null hook.
void nr_GetRenderInfo(RenderInfoStruct *pStruct)
{
	if (pStruct)
	{
		pStruct->m_dwWorldPolysDrawn     = 0;
		pStruct->m_dwWorldPolysProcessed = 0;
		pStruct->m_dwModelPolysDrawn     = 0;
	}
}

// Screen -> surface readback (screenshots go through nr_MakeScreenShot instead).
void nr_BlitFromScreen(BlitRequest *) {}

// The engine occasionally asks the renderer for the raw D3D device (movie
// playback, effects). There is none.
IDirect3DDevice9* nr_GetD3DDevice() { return NULL; }

void nr_RenderCommand(int argc, char **argv)
{
}


void* nr_GetHook(char *pHook)
{
	return 0;
}


void nr_SwapBuffers(uint flags)
{
#ifdef LT_MACOS
	// ★ THE FRAME ENDS HERE, BEFORE THE DUMPS. Metal's readback retains the
	// LAST PRESENTED texture, so a dump taken before EndFrame would capture the
	// previous frame. EnsureFrame first, so a frame in which nothing drew still
	// presents its clear instead of leaving the last frame on screen.
	MTLDev_EnsureFrame();
	MTLDev_EndFrame();
	if (g_nPendingSceneDump)
	{
		nr_DumpDrawable(g_nPendingSceneDump, "scene", g_bPendingSceneNumbered);
		if (getenv("LT_DUMP_DEPTH"))
			nr_DumpDepth(g_nPendingSceneDump);
		g_nPendingSceneDump = 0;
	}

	// FPS counter in the window title, refreshed once a second.
	{
		static uint32 s_nFrames = 0;
		static double s_fLastTime = 0.0;
		++s_nFrames;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		double fNow = (double)tv.tv_sec + tv.tv_usec * 1e-6;
		if (s_fLastTime == 0.0)
			s_fLastTime = fNow;
		else if (fNow - s_fLastTime >= 1.0)
		{
			char sTitle[128];
			snprintf(sTitle, sizeof(sTitle), "No One Lives Forever 2 — %.0f FPS",
			         (double)s_nFrames / (fNow - s_fLastTime));
			LTMacWin_SetTitle(sTitle);
			s_nFrames = 0;
			s_fLastTime = fNow;
		}
	}

	// LT_DUMP_SWAP=<n>[,<n>...]: dump the n-th PRESENTED frame(s) (after
	// console/2D drew on top of the scene — unlike LT_DUMP_FRAME, which is
	// scene-only). A list writes /tmp/nolf2_swap_<n>.ppm per frame.
	static int s_aDumpSwap[NR_MAX_DUMP_FRAMES];
	static int s_nDumpSwapCount = -1;
	if (s_nDumpSwapCount < 0)
		s_nDumpSwapCount = nr_ParseDumpList(getenv("LT_DUMP_SWAP"), s_aDumpSwap);
	++g_nSwapCount;
	const int s_nSwapCount = g_nSwapCount;
	for (int i = 0; i < s_nDumpSwapCount; ++i)
	{
		if (s_nSwapCount == s_aDumpSwap[i])
			nr_DumpDrawable(s_nSwapCount, "swap", s_nDumpSwapCount > 1);
	}

	// ★ LT_DUMP_SWAP_T="<sec>[,<sec>...]" — dump the first presented frame at or
	// after each of those ELAPSED SECONDS, to /tmp/nolf2_swapt_<sec>.ppm.
	//
	// This is the one to reach for when you want a specific place in the game.
	// LT_DUMP_SWAP's frame numbers are not reproducible between runs (see
	// g_nSwapCount above: 1706 vs 59171 vs 70928 for the same spot, because the
	// world-less main menu presents as fast as it can). Wall-clock IS stable —
	// the campaign takes the same time to reach a level every run — so a time is
	// a repeatable address for a scene and a frame number is not.
	static int s_aDumpT[NR_MAX_DUMP_FRAMES];
	static int s_nDumpTCount = -1;
	static bool s_bDumpTDone[NR_MAX_DUMP_FRAMES];
	static double s_fDumpTStart = 0.0;
	if (s_nDumpTCount < 0)
	{
		s_nDumpTCount = nr_ParseDumpList(getenv("LT_DUMP_SWAP_T"), s_aDumpT);
		for (int i = 0; i < NR_MAX_DUMP_FRAMES; ++i)
			s_bDumpTDone[i] = false;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		s_fDumpTStart = (double)tv.tv_sec + tv.tv_usec * 1e-6;
	}
	if (s_nDumpTCount > 0)
	{
		struct timeval tv;
		gettimeofday(&tv, NULL);
		const double fElapsed = ((double)tv.tv_sec + tv.tv_usec * 1e-6) - s_fDumpTStart;
		for (int i = 0; i < s_nDumpTCount; ++i)
		{
			if (!s_bDumpTDone[i] && fElapsed >= (double)s_aDumpT[i])
			{
				s_bDumpTDone[i] = true;
				nr_DumpDrawable(s_aDumpT[i], "swapt", true);
			}
		}
	}

#else
	BOOL ret;
	HDC hDC;

	if(g_hBitmap && g_hWnd && g_pDibBytes)
	{
		hDC = GetDC(g_hWnd);
		if(hDC)
		{
			ret = StretchDIBits(hDC,
					 0, 0, g_DibWidth, g_DibHeight,
					 0, 0, g_DibWidth, g_DibHeight,
					 g_pDibBytes, (BITMAPINFO*)&g_bmi, DIB_RGB_COLORS, SRCCOPY);
			
			ReleaseDC(g_hWnd, hDC);
		}
	}
#endif // LT_MACOS
}

HLTBUFFER nr_CreateSurface(int width, int height)
{
	NullBuf *pBuf;

	LT_MEM_TRACK_ALLOC(pBuf = (NullBuf*)LTMemAlloc(sizeof(NullBuf) + ((width*height)-1) * sizeof(unsigned short)),LT_MEM_TYPE_RENDERER);
	if(pBuf)
	{
		pBuf->m_Width = width;
		pBuf->m_Height = height;
		return (HLTBUFFER)pBuf;
	}
	else
	{
		return LTNULL;
	}
}


void nr_DeleteSurface(HLTBUFFER hSurf)
{
	if(hSurf)
	{
		LTMemFree(hSurf);
	}
}


void nr_GetSurfaceInfo(HLTBUFFER hSurf, DDWORD *pWidth, DDWORD *pHeight)
{
	NullBuf *pBuf;

	pBuf = (NullBuf*)hSurf;
	
	if(pWidth) *pWidth = pBuf->m_Width;
	if(pHeight) *pHeight = pBuf->m_Height;
//	if(pPitchBytes) *pPitchBytes = pBuf->m_Width*2;
}


void* nr_LockSurface(HLTBUFFER hSurf, uint32& Pitch)
{
	NullBuf *pBuf;

	pBuf = (NullBuf*)hSurf;
	// Surfaces are 16-bit (RGB555); callers step rows by this pitch when they
	// write into the locked buffer. Leaving it uninitialised scattered every
	// loaded image across the buffer (interface surfaces rendered as noise).
	Pitch = pBuf->m_Width * (uint32)sizeof(unsigned short);
	return pBuf->m_Data;
}


void nr_UnlockSurface(HLTBUFFER hSurf)
{
}


bool nr_LockScreen(int left, int top, int right, int bottom, void **pData, long *pPitch)
{
	BYTE *pStartLine;

	if(!g_pDibBytes)
		return LTFALSE;

	pStartLine = (BYTE*)g_pDibBytes;
	pStartLine += (DWORD)top * g_DibPitchBytes + (DWORD)(left << 1);
	*pData = pStartLine;
	*pPitch = g_DibPitchBytes;
	return LTTRUE;
}


void nr_UnlockScreen()
{
}


// --------------------------------------------------------------------------
// GL presentation of the engine's software interface surfaces.
//
// The game interface (splash, menu backgrounds, screen fades, HUD bitmaps)
// draws into 16-bit RGB555 software surfaces (NullBuf) and hands them to the
// renderer to put on screen: BlitToScreen for an axis-aligned rect, WarpToScreen
// for an arbitrary quad (used by ScaleSurfaceToSurface / rotations). With these
// hooks NULL the whole 2D interface was invisible (and the engine fell back to a
// slow full-screen software blit). We upload the surface as a texture and draw a
// screen-space quad -- correct AND GPU-fast.
// --------------------------------------------------------------------------

static uint8 *g_SurfBuf = NULL;
static size_t g_SurfBufCap = 0;

static void nr_ExpandSurfaceRGBA(NullBuf *pBuf, bool bTransparent, uint16 transColor)
{
	uint32 w = pBuf->m_Width, h = pBuf->m_Height;
	size_t need = (size_t)w * h * 4;
	if (need > g_SurfBufCap)
	{
		g_SurfBuf = (uint8*)realloc(g_SurfBuf, need);
		g_SurfBufCap = need;
	}
	uint16 tc = (uint16)(transColor & 0x7FFF);
	const unsigned short *pIn = pBuf->m_Data;
	uint8 *pOut = g_SurfBuf;
	for (uint32 i = 0; i < w * h; ++i)
	{
		uint16 px = pIn[i];
		uint32 r5 = (px & RGB555_RMASK) >> 10;
		uint32 g5 = (px & RGB555_GMASK) >> 5;
		uint32 b5 = (px & RGB555_BMASK);
		pOut[0] = (uint8)((r5 << 3) | (r5 >> 2));   // 5 -> 8 bit
		pOut[1] = (uint8)((g5 << 3) | (g5 >> 2));
		pOut[2] = (uint8)((b5 << 3) | (b5 >> 2));
		pOut[3] = (bTransparent && (uint16)(px & 0x7FFF) == tc) ? 0 : 255;
		pOut += 4;
	}
}

// dst = 4 screen-space quad corners; src = the matching source pixel coords.
static void nr_PresentSurface(NullBuf *pBuf, bool bTransparent, uint16 transColor,
                              float alpha, const float dst[4][2], const float src[4][2])
{
	if (!pBuf || pBuf->m_Width == 0 || pBuf->m_Height == 0)
		return;

	// ⚠️ RESOLVE THE BLEND ONCE, HERE. The GL code below reads both the
	// transparency flag and the alpha to decide; passing the decision to Metal
	// rather than letting it re-derive one keeps the two from drifting (§89).
	const bool bBlend = (bTransparent || alpha < 0.999f);

	nr_ExpandSurfaceRGBA(pBuf, bTransparent, transColor);
	MTLDrawPrim_PresentSurface(g_SurfBuf, pBuf->m_Width, pBuf->m_Height,
	                           dst, src, alpha, bBlend);
}

// LT_TRACE_SURFACE=1: a census of the optimized-2D SURFACE path -- the software
// interface surfaces the engine hands to BlitToScreen / WarpToScreen. It exists
// because that path is GL-only: under Metal nr_PresentSurface's 34 GL calls are
// no-ops without a context, so ANY surface presented here is invisible. The
// question the census answers is which screens actually use it. Deduped per
// distinct descriptor -- the interface re-blits the same surface every frame.
static void nr_TraceSurface(const char *pWhat, NullBuf *pBuf, float alpha,
                            bool bTransparent, float dx0, float dy0,
                            float dx1, float dy1)
{
	static int s_nTrace = -1;
	if (s_nTrace < 0) s_nTrace = getenv("LT_TRACE_SURFACE") ? 1 : 0;
	if (!s_nTrace)
		return;

	char sKey[256];
	snprintf(sKey, sizeof(sKey), "%s %ux%u -> (%.0f %.0f)-(%.0f %.0f) alpha=%.2f trans=%d",
	         pWhat, pBuf->m_Width, pBuf->m_Height, dx0, dy0, dx1, dy1, alpha,
	         bTransparent ? 1 : 0);

	static std::vector<std::string> s_aSeen;
	for (size_t i = 0; i < s_aSeen.size(); ++i)
		if (s_aSeen[i] == sKey)
			return;
	s_aSeen.push_back(sKey);
	fprintf(stderr, "[surf] %s   (distinct #%u)\n", sKey, (uint32)s_aSeen.size());
}

void nr_BlitToScreen(BlitRequest *pRequest)
{
	NullBuf *pBuf = (NullBuf*)pRequest->m_hBuffer;
	if (!pBuf)
		return;

	LTRect *ps = pRequest->m_pSrcRect;
	LTRect *pd = pRequest->m_pDestRect;
	float sx0 = ps ? (float)ps->left  : 0.0f;
	float sy0 = ps ? (float)ps->top   : 0.0f;
	float sx1 = ps ? (float)ps->right : (float)pBuf->m_Width;
	float sy1 = ps ? (float)ps->bottom: (float)pBuf->m_Height;
	float dx0 = pd ? (float)pd->left  : sx0;
	float dy0 = pd ? (float)pd->top   : sy0;
	float dx1 = pd ? (float)pd->right : sx1;
	float dy1 = pd ? (float)pd->bottom: sy1;

	const float dst[4][2] = { {dx0,dy0}, {dx1,dy0}, {dx1,dy1}, {dx0,dy1} };
	const float src[4][2] = { {sx0,sy0}, {sx1,sy0}, {sx1,sy1}, {sx0,sy1} };
	nr_TraceSurface("blit", pBuf, pRequest->m_Alpha,
	                (pRequest->m_BlitOptions & BLIT_TRANSPARENT) != 0, dx0, dy0, dx1, dy1);
	nr_PresentSurface(pBuf, (pRequest->m_BlitOptions & BLIT_TRANSPARENT) != 0,
	                  pRequest->m_TransparentColor.wVal, pRequest->m_Alpha, dst, src);
}

bool nr_WarpToScreen(BlitRequest *pRequest)
{
	NullBuf *pBuf = (NullBuf*)pRequest->m_hBuffer;
	if (!pBuf || !pRequest->m_pWarpPts || pRequest->m_nWarpPts < 4)
		return false;

	LTWarpPt *wp = pRequest->m_pWarpPts;
	float dst[4][2], src[4][2];
	for (int i = 0; i < 4; ++i)
	{
		dst[i][0] = wp[i].dest_x;   dst[i][1] = wp[i].dest_y;
		src[i][0] = wp[i].source_x; src[i][1] = wp[i].source_y;
	}
	nr_TraceSurface("warp", pBuf, pRequest->m_Alpha,
	                (pRequest->m_BlitOptions & BLIT_TRANSPARENT) != 0,
	                dst[0][0], dst[0][1], dst[2][0], dst[2][1]);
	nr_PresentSurface(pBuf, (pRequest->m_BlitOptions & BLIT_TRANSPARENT) != 0,
	                  pRequest->m_TransparentColor.wVal, pRequest->m_Alpha, dst, src);
	return true;
}


// ★ SCREENSHOTS. Empty in this port until now (and in the GL build too, so this
// was a missing FEATURE, not a Metal regression).
//
// Writes a 24-bit uncompressed BMP because that is what the engine's own caller
// names the file: client.cpp builds "<SSFile><n>.bmp" and hands it here. BMP is
// bottom-up and BGR; Metal's readback is top-down, so the rows are walked
// backwards -- the reverse of the PPM dump above.
void nr_MakeScreenShot(const char *pFilename)
{
	if (!pFilename || !pFilename[0])
		return;

	int nW = 0, nH = 0;
	LTMacWin_GetSize(&nW, &nH);
	if (nW <= 0 || nH <= 0)
		return;

	std::vector<uint8> aRGB((size_t)nW * nH * 3);
	{
		int nGotW = 0, nGotH = 0;
		if (!MTLDev_ReadbackFrame(&aRGB[0], &nGotW, &nGotH))
			return;
		nW = nGotW; nH = nGotH;
		aRGB.resize((size_t)nW * nH * 3);
	}

	FILE *fp = fopen(pFilename, "wb");
	if (!fp)
	{
		fprintf(stderr, "[nr] screenshot FAILED to open '%s'\n", pFilename);
		return;
	}

	// BMP rows are padded to a 4-byte boundary.
	const uint32 nRowBytes = (uint32)nW * 3;
	const uint32 nPad      = (4 - (nRowBytes & 3)) & 3;
	const uint32 nImage    = (nRowBytes + nPad) * (uint32)nH;
	const uint32 nOffset   = 14 + 40;

	uint8 aHdr[54];
	memset(aHdr, 0, sizeof(aHdr));
	aHdr[0] = 'B'; aHdr[1] = 'M';
	*(uint32*)&aHdr[2]  = nOffset + nImage;   // file size
	*(uint32*)&aHdr[10] = nOffset;            // pixel data offset
	*(uint32*)&aHdr[14] = 40;                 // BITMAPINFOHEADER size
	*(int32*) &aHdr[18] = (int32)nW;
	*(int32*) &aHdr[22] = (int32)nH;          // positive = bottom-up
	*(uint16*)&aHdr[26] = 1;                  // planes
	*(uint16*)&aHdr[28] = 24;                 // bits per pixel
	*(uint32*)&aHdr[34] = nImage;
	fwrite(aHdr, 1, sizeof(aHdr), fp);

	const uint8 aPad[3] = { 0, 0, 0 };
	std::vector<uint8> aRow((size_t)nW * 3);
	for (int nOut = 0; nOut < nH; ++nOut)
	{
		// BMP stores the BOTTOM row first; Metal's readback is top-down.
		const int nSrc = nH - 1 - nOut;
		const uint8 *pSrc = &aRGB[(size_t)nSrc * nW * 3];
		for (int x = 0; x < nW; ++x)
		{
			aRow[x * 3 + 0] = pSrc[x * 3 + 2];   // B
			aRow[x * 3 + 1] = pSrc[x * 3 + 1];   // G
			aRow[x * 3 + 2] = pSrc[x * 3 + 0];   // R
		}
		fwrite(&aRow[0], 1, nRowBytes, fp);
		if (nPad)
			fwrite(aPad, 1, nPad, fp);
	}
	fclose(fp);
	fprintf(stderr, "[nr] screenshot written: %s (%dx%d)\n", pFilename, nW, nH);
}


bool nr_Start3D()
{
	g_bIn3D = TRUE;
	return LTTRUE;
}


bool nr_End3D()
{
	g_bIn3D = FALSE;
	return LTTRUE;
}


bool nr_IsIn3D()
{
	return g_bIn3D;
}


bool nr_StartOptimized2D()
{
	g_bInOptimized2D = TRUE;
	return LTTRUE;
}


void nr_EndOptimized2D()
{
	g_bInOptimized2D = FALSE;
}


bool nr_IsInOptimized2D()
{
	return g_bInOptimized2D;
}


bool nr_OptimizeSurface(HLTBUFFER hBuffer, DDWORD transparentColor)
{
	return LTFALSE;
}


void nr_UnoptimizeSurface(HLTBUFFER hBuffer)
{
}


bool nr_QueryDeletePalette(struct DEPalette_t *pPalette)
{
	return LTTRUE;
}


void nr_ReadConsoleVariables()
{
	RenderConVar_Read(g_pRenderStruct);
}


LTBOOL nr_SetMasterPalette(SharedTexture *pTexture)
{
	return FALSE;
}


bool nr_GetScreenFormat(PFormat *pFormat)
{
	pFormat->Init(BPP_16, 0, RGB555_RMASK, RGB555_GMASK, RGB555_BMASK);
	return true;
}


// ---------------------------------------------------------------- //
// DLL export functions.
// ---------------------------------------------------------------- //

extern "C"
{
	void RenderDLLSetup(RenderStruct *pStruct);
	RMode* GetSupportedModes();
	void FreeModeList(RMode *pModes);
};


void rdll_RenderDLLSetup(RenderStruct *pStruct)
{
#ifdef LT_MACOS
	g_pRenderStruct = pStruct;   // engine-side services for the GL texture/world code
#endif
	pStruct->Start3D = nr_Start3D;
	pStruct->End3D = nr_End3D;
	pStruct->IsIn3D = nr_IsIn3D;
	pStruct->StartOptimized2D = nr_StartOptimized2D;
	pStruct->EndOptimized2D = nr_EndOptimized2D;
	pStruct->IsInOptimized2D = nr_IsInOptimized2D;
	pStruct->OptimizeSurface = nr_OptimizeSurface;
	pStruct->UnoptimizeSurface = nr_UnoptimizeSurface;
	pStruct->Init = nr_Init;
	pStruct->Term = nr_Term;
#ifdef LT_MACOS
	pStruct->LoadWorldData = nr_LoadWorldData;   // Phase-2 GL world loader
	pStruct->CreateRenderObject = RModel_CreateRenderObject;   // model LTB meshes
	pStruct->DestroyRenderObject = RModel_DestroyRenderObject;
	pStruct->ConvertTexDataToDD = nr_ConvertTexDataToDD;   // same-format copy
	pStruct->AddGlowRenderStyleMapping = nr_AddGlowRenderStyleMapping;   // glow: no-op
	pStruct->SetGlowDefaultRenderStyle = nr_SetGlowDefaultRenderStyle;
	pStruct->SetNoGlowRenderStyle = nr_SetNoGlowRenderStyle;
	pStruct->IsObjectGroupEnabled = nr_IsObjectGroupEnabled;      // per-group object visibility
	pStruct->SetObjectGroupEnabled = nr_SetObjectGroupEnabled;
	pStruct->SetAllObjectGroupEnabled = nr_SetAllObjectGroupEnabled;
	pStruct->SetOccluderEnabled = nr_SetOccluderEnabled;          // no occlusion culling: no-op, but must succeed
	pStruct->GetOccluderEnabled = nr_GetOccluderEnabled;
	pStruct->GetTextureEffectVarID = nr_GetTextureEffectVarID;    // no shader pipeline
	pStruct->SetTextureEffectVar = nr_SetTextureEffectVar;
	pStruct->SetOptimized2DBlend = nr_SetOptimized2DBlend;        // 2D state (applied in Blit/WarpToScreen)
	pStruct->GetOptimized2DBlend = nr_GetOptimized2DBlend;
	pStruct->SetOptimized2DColor = nr_SetOptimized2DColor;
	pStruct->GetOptimized2DColor = nr_GetOptimized2DColor;
	pStruct->DrawPrimSetTexture = nr_DrawPrimSetTexture;          // gl_drawprim owns its own binding
	pStruct->DrawPrimDisableTextures = nr_DrawPrimDisableTextures;
	pStruct->QueryDDSupport = nr_QueryDDSupport;                  // GL uploader converts any DTX format
	pStruct->GetTextureDDFormat1 = nr_GetTextureDDFormat1;
	pStruct->GetTextureDDFormat2 = nr_GetTextureDDFormat2;
	pStruct->IncCurTextureFrameCode = nr_IncCurTextureFrameCode;
	pStruct->MakeCubicEnvMap = nr_MakeCubicEnvMap;
	pStruct->GetRenderInfo = nr_GetRenderInfo;
	pStruct->BlitFromScreen = nr_BlitFromScreen;
	pStruct->GetD3DDevice = nr_GetD3DDevice;
#endif
	pStruct->BindTexture = nr_BindTexture;
	pStruct->UnbindTexture = nr_UnbindTexture;
	pStruct->CreateContext = nr_CreateContext;
	pStruct->DeleteContext = nr_DeleteContext;
	pStruct->Clear = nr_Clear;
	pStruct->RenderScene = nr_RenderScene;
	pStruct->RenderCommand = nr_RenderCommand;
	pStruct->SwapBuffers = nr_SwapBuffers;
	pStruct->CreateSurface = nr_CreateSurface;
	pStruct->DeleteSurface = nr_DeleteSurface;
	pStruct->GetSurfaceInfo = nr_GetSurfaceInfo;
	pStruct->LockSurface = nr_LockSurface;
	pStruct->UnlockSurface = nr_UnlockSurface;
	pStruct->LockScreen = nr_LockScreen;
	pStruct->UnlockScreen = nr_UnlockScreen;
	pStruct->MakeScreenShot = nr_MakeScreenShot;
	pStruct->ReadConsoleVariables = nr_ReadConsoleVariables;
	pStruct->GetScreenFormat = nr_GetScreenFormat;
	pStruct->BlitToScreen = nr_BlitToScreen;   // present software interface surfaces to GL
	pStruct->WarpToScreen = nr_WarpToScreen;   // scaled/warped surface blits (menu bg, splash)
	pStruct->SetLightGroupColor = RWorld_SetLightGroupColor;   // switchable lights (lamps)
}


RMode* rdll_GetSupportedModes()
{
#ifdef LT_MACOS
	// The game's display screen (CScreenDisplay::GetRendererData) only ACCEPTS
	// modes that are >= 640x480, exactly 32bpp and exactly 4:3
	// (m_Width == m_Height * 4 / 3). Anything else it silently drops, and if
	// the resulting resolution array ends up EMPTY, GetRendererModeStruct
	// indexes it anyway and crashes -- that is the "Escape from the display
	// options screen" crash in the user's 2026-07-24 logs.
	//
	// So report the retail 4:3 ladder, capped at the drawable we actually have.
	// ⚠️ Honest caveat: picking one of these does not really change anything --
	// nr_Init always overrides the requested mode with the true drawable size
	// (see the comment there). The list exists so the screen has valid data.
	static const struct { uint32 w, h; } kLadder[] = {
		{  640,  480 }, {  800,  600 }, { 1024,  768 },
		{ 1152,  864 }, { 1280,  960 }, { 1600, 1200 },
	};

	int nMaxW = 640, nMaxH = 480;
	LTMacWin_GetSize(&nMaxW, &nMaxH);

	RMode *pListHead = LTNULL;

	// Order does not matter: the game re-sorts in CScreenDisplay::SortRenderModes.
	for (int i = (int)(sizeof(kLadder)/sizeof(kLadder[0])) - 1; i >= 0; --i)
	{
		// Always keep 640x480 (i == 0) so the list can never be empty, even on
		// a tiny window -- an empty list is the crash we are fixing.
		if (i != 0 && ((int)kLadder[i].w > nMaxW || (int)kLadder[i].h > nMaxH))
			continue;

		RMode *pMode;
		LT_MEM_TRACK_ALLOC(pMode = new RMode, LT_MEM_TYPE_RENDERER);
		if (!pMode)
			break;

		LTStrCpy(pMode->m_Description, "OpenGL (macOS)", sizeof(pMode->m_Description));
		LTStrCpy(pMode->m_InternalName, "OpenGL", sizeof(pMode->m_InternalName));

		pMode->m_Width    = kLadder[i].w;
		pMode->m_Height   = kLadder[i].h;
		pMode->m_BitDepth = 32;
		pMode->m_bHWTnL   = true;
		pMode->m_pNext    = pListHead;
		pListHead = pMode;
	}

	return pListHead;
#else
	RMode *pMode;

	LT_MEM_TRACK_ALLOC(pMode = new RMode, LT_MEM_TYPE_RENDERER);
	if(pMode)
	{
		LTStrCpy(pMode->m_Description, "NullRender: (debug renderer)", sizeof(pMode->m_Description));
		LTStrCpy(pMode->m_InternalName, "NullRender", sizeof(pMode->m_InternalName));

		pMode->m_Width		= 640;
		pMode->m_Height		= 480;
		pMode->m_BitDepth	= 32;
		pMode->m_bHWTnL		= true;
		pMode->m_pNext		= LTNULL;

		return pMode;
	}
	else
	{
		return LTNULL;
	}
#endif
}


void rdll_FreeModeList(RMode *pModes)
{
//	free(pModes);
	RMode* pCur = pModes;
	while (pCur) {
		RMode* pNext = pCur->m_pNext;
		LTMemFree(pCur);
		pCur = pNext; }
}
