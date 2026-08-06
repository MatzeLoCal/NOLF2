
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
// macOS: this "null" renderer is the seed of the real GL renderer (Phase 2). It
// draws into the Cocoa NSOpenGLContext via the LTMacWin_* C ABI instead of the
// Win32 GDI/DIB path used elsewhere in this file.
#include <OpenGL/gl.h>
#include <stdio.h>
#include <math.h>
#include <sys/time.h>   // gettimeofday (FPS counter)
#include "ltmacwindow.h"
#include "sys/gl/gl_worlddata.h"
#include "sys/gl/gl_texture.h"
#include "sys/gl/gl_model.h"
#include "sys/gl/gl_drawprim.h"
#include "sys/gl/gl_convar.h"   // renderer console vars (see gl_convar.cpp)
#include "sys/gl/gl_polygrid.h"   // OT_POLYGRID = water / ice
#include "sys/gl/gl_particles.h"  // OT_PARTICLESYSTEM = fire, smoke, waterfall
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

// fScale dims the debug colour (the scene pass used a darker variant).
static void nr_SetClearColor(float fScale)
{
	if (nr_UseDebugClear())
		glClearColor(g_fClearR * fScale, g_fClearG * fScale, g_fClearB * fScale, 1.0f);
	else
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
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

	// Bring up GL on the Cocoa context and show one cleared frame, so a
	// successful render-init is immediately visible in the window.
	LTMacWin_MakeCurrent();
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
	if (g_pGLStruct)
	{
		g_pGLStruct->m_Width  = (uint32)vpW;   // engine reads these before Init returns them
		g_pGLStruct->m_Height = (uint32)vpH;
	}

	// Publish the renderer's console variables (rendererconsolevars.h) to the
	// engine console — mirrors d3d_CreateConsoleVariables at common_init.cpp:120.
	// MUST happen here, before any world loads: WorldProperties writes the
	// level's authored fog/far-Z/sky settings into these by name, and a write to
	// a variable that does not exist is silently dropped. See gl_convar.cpp.
	GLConVar_Create(g_pGLStruct);
	GLConVar_Read(g_pGLStruct);

	// LT_GL_INFO=1 — version/renderer plus the fixed-function limits and the
	// texture-env extensions. Which combiner ops exist decides how the authored
	// D3D texture stages can be reproduced at all (EnvMapAlpha's
	// D3DTOP_MODULATEALPHA_ADDCOLOR needs GL_ATI_texture_env_combine3), so this
	// is a design input, not just decoration.
	if (getenv("LT_GL_INFO"))
	{
		GLint nMaxUnits = 0, nMaxImageUnits = 0;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &nMaxUnits);
		glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &nMaxImageUnits);
		const char *pExt = (const char*)glGetString(GL_EXTENSIONS);
		fprintf(stderr, "[glinfo] version='%s'\n[glinfo] renderer='%s'\n"
		                "[glinfo] vendor='%s'\n"
		                "[glinfo] MAX_TEXTURE_UNITS=%d MAX_TEXTURE_IMAGE_UNITS=%d\n",
		        (const char*)glGetString(GL_VERSION),
		        (const char*)glGetString(GL_RENDERER),
		        (const char*)glGetString(GL_VENDOR),
		        (int)nMaxUnits, (int)nMaxImageUnits);
		static const char *kInteresting[] = {
			"GL_ARB_texture_env_combine", "GL_EXT_texture_env_combine",
			"GL_ATI_texture_env_combine3", "GL_NV_texture_env_combine4",
			"GL_ARB_texture_env_dot3", "GL_ARB_texture_env_crossbar",
			"GL_EXT_texture_compression_s3tc", 0 };
		for (int i = 0; kInteresting[i]; ++i)
			fprintf(stderr, "[glinfo] %-34s %s\n", kInteresting[i],
			        (pExt && strstr(pExt, kInteresting[i])) ? "YES" : "no");
	}

	glViewport(0, 0, vpW, vpH);
	nr_SetClearColor(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	LTMacWin_SwapBuffers();
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
	GLWorld_Free();
#endif
	g_hWnd = 0;

	if(g_hBitmap)
	{
		DeleteObject(g_hBitmap);
		g_hBitmap = 0;
	}
	
	g_pDibBytes = NULL;
}


void nr_BindTexture(SharedTexture *pTexture, bool bTextureChanged)
{
#ifdef LT_MACOS
	LTMacWin_MakeCurrent();
	GLTex_Bind(pTexture, bTextureChanged);
#endif
}


void nr_UnbindTexture(SharedTexture *pTexture)
{
#ifdef LT_MACOS
	LTMacWin_MakeCurrent();
	GLTex_Unbind(pTexture);
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
	// drawing the interface). A future pass can map pRect to glScissor.
	// Reassert it each call — the Cocoa view's drawRect also sets glClearColor.
	LTMacWin_MakeCurrent();
	if (nr_UseDebugClear())
		nr_SetClearColor(1.0f);
	else
		glClearColor(ClearColor.rgb.r * (1.0f / 255.0f),
		             ClearColor.rgb.g * (1.0f / 255.0f),
		             ClearColor.rgb.b * (1.0f / 255.0f), 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, nDrawW, nDrawH, GL_RGB, GL_UNSIGNED_BYTE, pPixels);
	char sPath[128];
	if (bNumbered)
		snprintf(sPath, sizeof(sPath), "/tmp/nolf2_%s_%d.ppm", pWhat, nFrame);
	else
		snprintf(sPath, sizeof(sPath), "/tmp/nolf2_frame.ppm");
	FILE *fp = fopen(sPath, "wb");
	if (fp)
	{
		fprintf(fp, "P6\n%d %d\n255\n", nDrawW, nDrawH);
		// GL rows are bottom-up; PPM wants top-down.
		for (int nRow = nDrawH - 1; nRow >= 0; --nRow)
			fwrite(pPixels + (size_t)nRow * nDrawW * 3, 1, (size_t)nDrawW * 3, fp);
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

int nr_RenderScene(struct SceneDesc *pScene)
{
#ifdef LT_MACOS
	// Phase-2 GL: draw the loaded world from the camera in pScene.
	LTMacWin_MakeCurrent();

	// ★ DRAWMODE_OBJECTLIST (renderstruct.h:55) -- "only render the objects in
	// m_pObjectList", and no world. The interface uses it for every menu,
	// loading screen and the paused state (CInterfaceMgr::DrawSFX ->
	// ILTClient::RenderObjects). D3D also drops g_have_world for this mode
	// (common_draw.cpp:423), which is why interface models get no environment
	// lighting there.
	const bool bObjectList = (pScene->m_DrawMode == DRAWMODE_OBJECTLIST) &&
	                         pScene->m_pObjectList && pScene->m_ObjectListSize > 0;

	GLModel_BeginSceneFrame();   // LT_TRACE_UI frame counter (counts every pass)

	int nWinW = 0, nWinH = 0;
	LTMacWin_GetSize(&nWinW, &nWinH);
	if (nWinW <= 0 || nWinH <= 0) { nWinW = 640; nWinH = 480; }

	// Viewport from the scene rect (RenderCamera fills it from the camera).
	int nVpX = pScene->m_Rect.left;
	int nVpW = pScene->m_Rect.right  - pScene->m_Rect.left;
	int nVpH = pScene->m_Rect.bottom - pScene->m_Rect.top;
	if (nVpW <= 0 || nVpH <= 0) { nVpX = 0; nVpW = nWinW; nVpH = nWinH; }
	// GL's viewport origin is bottom-left; the engine rect's is top-left.
	int nVpY = nWinH - pScene->m_Rect.top - nVpH;
	if (nVpY < 0) nVpY = 0;
	glViewport(nVpX, nVpY, nVpW, nVpH);

	// Fog state for this frame (read from the level's console vars every frame —
	// WorldProperties and VolumeBrushes both drive them at runtime). Applied
	// before the sky pass with the SkyFog range, then re-applied with the main
	// range for the world/models; disabled again before any 2D.
	if (bObjectList)
		GLWorld_DisableFog();
	else
		GLWorld_ApplyFog(true);

	// ⚠️ AN OBJECT-LIST PASS MUST NOT CLEAR. CInterfaceMgr::UpdateInterfaceSFX
	// calls RenderObjects ONCE PER MENU LAYER (InterfaceMgr.cpp:5594 — a while
	// loop over the render list, one call per nLayer), so several object-list
	// scenes compose into a single frame. Clearing here wiped every layer but
	// the last and the menu went black. The GAME owns the clear in these states:
	// CInterfaceMgr::Update -> ClearScreen(CLEARSCREEN_SCREEN|CLEARSCREEN_RENDER)
	// (InterfaceMgr.cpp:784-795), which reaches us as nr_Clear.
	if (!bObjectList)
	{
		// With fog on, clear to the fog colour: anywhere the world does not reach
		// should read as haze, not as the void behind it.
		float fFogR, fFogG, fFogB;
		if (GLWorld_GetFogColor(fFogR, fFogG, fFogB))
			glClearColor(fFogR, fFogG, fFogB, 1.0f);
		else
			nr_SetClearColor(0.25f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	// Projection parameters from the scene FOV (full angles, radians).
	float fZNear = 5.0f, fZFar = 100000.0f;
	float fFovY = (pScene->m_yFov > 0.01f) ? pScene->m_yFov : 1.2f;
	float fFovX = (pScene->m_xFov > 0.01f) ? pScene->m_xFov : 1.6f;
	float fTop   = fZNear * tanf(fFovY * 0.5f);
	float fRight = fZNear * tanf(fFovX * 0.5f);

	// View from the camera transform. Lithtech is left-handed (+Z forward);
	// GL eye space looks down -Z, so the third row is the negated forward.
	LTVector vR = pScene->m_Rotation.Right();
	LTVector vU = pScene->m_Rotation.Up();
	LTVector vF = pScene->m_Rotation.Forward();
	const LTVector &vPos = pScene->m_Pos;

	float aView[16];
	aView[0] =  vR.x; aView[4] =  vR.y; aView[8]  =  vR.z; aView[12] = -vR.Dot(vPos);
	aView[1] =  vU.x; aView[5] =  vU.y; aView[9]  =  vU.z; aView[13] = -vU.Dot(vPos);
	aView[2] = -vF.x; aView[6] = -vF.y; aView[10] = -vF.z; aView[14] =  vF.Dot(vPos);
	aView[3] = 0.0f;  aView[7] = 0.0f;  aView[11] = 0.0f;  aView[15] = 1.0f;

	// --- Sky pass (backdrop, before the world). The sky camera keeps the
	// scene rotation but sits inside the sky box: the camera's normalized
	// position within the world extents maps into SkyDef view min/max
	// (mirrors d3d_SetupSkyStuff/drawsky.cpp). Sky geometry is small and
	// close, so use a much nearer near-plane; depth stays untouched.
	GLWorld_SetSkyObjects(pScene->m_SkyObjects, pScene->m_nSkyObjects);
	static bool s_bSkyLogged = false;
	if (!s_bSkyLogged && GLWorld_IsLoaded())
	{
		s_bSkyLogged = true;
		fprintf(stderr, "[nr] sky: %d objects, viewbox (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
		        pScene->m_nSkyObjects,
		        pScene->m_SkyDef.m_ViewMin.x, pScene->m_SkyDef.m_ViewMin.y, pScene->m_SkyDef.m_ViewMin.z,
		        pScene->m_SkyDef.m_ViewMax.x, pScene->m_SkyDef.m_ViewMax.y, pScene->m_SkyDef.m_ViewMax.z);
	}
	LTVector vBoundsCenter, vBoundsHalf;
	if (!bObjectList && pScene->m_nSkyObjects > 0 && GLWorld_GetBounds(vBoundsCenter, vBoundsHalf))
	{
		LTVector vMin = vBoundsCenter - vBoundsHalf;
		LTVector vMax = vBoundsCenter + vBoundsHalf;
		LTVector vPercent(0.5f, 0.5f, 0.5f);
		if (vMax.x > vMin.x) vPercent.x = (vPos.x - vMin.x) / (vMax.x - vMin.x);
		if (vMax.y > vMin.y) vPercent.y = (vPos.y - vMin.y) / (vMax.y - vMin.y);
		if (vMax.z > vMin.z) vPercent.z = (vPos.z - vMin.z) / (vMax.z - vMin.z);

		const SkyDef &cSky = pScene->m_SkyDef;
		LTVector vSkyPos(
			cSky.m_ViewMin.x + (cSky.m_ViewMax.x - cSky.m_ViewMin.x) * vPercent.x,
			cSky.m_ViewMin.y + (cSky.m_ViewMax.y - cSky.m_ViewMin.y) * vPercent.y,
			cSky.m_ViewMin.z + (cSky.m_ViewMax.z - cSky.m_ViewMin.z) * vPercent.z);

		const float fSkyNear = 0.3f, fSkyFar = 30000.0f;
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glFrustum(-tanf(fFovX * 0.5f) * fSkyNear, tanf(fFovX * 0.5f) * fSkyNear,
		          -tanf(fFovY * 0.5f) * fSkyNear, tanf(fFovY * 0.5f) * fSkyNear,
		          fSkyNear, fSkyFar);

		float aSkyView[16];
		memcpy(aSkyView, aView, sizeof(aSkyView));
		aSkyView[12] = -vR.Dot(vSkyPos);
		aSkyView[13] = -vU.Dot(vSkyPos);
		aSkyView[14] =  vF.Dot(vSkyPos);
		glMatrixMode(GL_MODELVIEW);
		glLoadMatrixf(aSkyView);

		GLWorld_DrawSkyWorldModels();
	}

	// --- Main scene matrices ---
	if (!bObjectList)
		GLWorld_ApplyFog(false);   // swap SkyFogNearZ/FarZ for the world's range
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-fRight, fRight, -fTop, fTop, fZNear, fZFar);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(aView);

	// Let the DrawPrim interface reuse this scene's transforms for
	// CAMERA/WORLD-space primitives.
	GLDrawPrim_SetSceneTransform(aView, fRight, fTop, fZNear, fZFar);

	GLSprite_SetCamera(vR, vU, vF, vPos);   // billboard basis + glow-scale reference
	GLParticle_SetCamera(vR, vU, vF, vPos); // particles billboard off the same basis
	GLPolyGrid_SetCamera(vR, vU, vF, vPos); // water env-map transform + Fresnel view vector
	GLWorld_SetCamera(vR, vU, vF);          // world-surface reflection transform (§40c)

	if (bObjectList)
	{
		// ★ DRAWMODE_OBJECTLIST: the caller named exactly what to draw and
		// wants NO world. This is the whole interface (menus, loading screens,
		// the in-game Options/Load screens) via ILTClient::RenderObjects.
		// Drawing the world here is what put the live level and the player-view
		// weapon behind every in-game menu.
		// The interface pass draws models too, and the attachment loop used to
		// live inside the model draw — so process them here as well, or an
		// interface model with an attachment would stop tracking its parent.
		GLModel_ProcessAttachments();

		GLObjectList_Draw(pScene->m_pObjectList, pScene->m_ObjectListSize);
	}
	else
	{
		// ★ FIRST: recompute every attached object's transform from its parent.
		// Must precede all draw passes, and must cover every object type — a
		// door handle attached to a world-model door is only re-oriented here.
		// (D3D does this inside its object walk; see GLModel_ProcessAttachments.)
		GLModel_ProcessAttachments();

		// LT_WORLD_ONLY=1 draws the BSP world and nothing else — pass isolation
		// for "which pass produces this artifact?". This is what identified
		// the world backface z-fighting in §48.
		static int s_nWorldOnly = -1;
		if (s_nWorldOnly < 0) s_nWorldOnly = getenv("LT_WORLD_ONLY") ? 1 : 0;

		GLWorld_Draw();
		if (s_nWorldOnly)
		{
			GLWorld_DisableFog();
			goto scene_done;
		}
		GLWorld_DrawWorldModels(false);      // SOLID world models
		GLPolyGrid_Draw(false);        // OPAQUE polygrids draw with the world
		GLModel_DrawModels(fRight / fTop);   // world-space models only
		GLWorld_DrawDynamicLights();   // lamp pools etc. (additive, depth-tested)
		// ⚠️ TRANSLUCENT world models come AFTER the models, exactly as
		// d3d_FlushObjectQueues orders them (drawobjects.cpp:218: solid world
		// models -> solid models -> the sorted translucent set). Drawing them
		// with the solid set made C08S03's glass cage depth-reject every
		// character inside and outside it.
		GLWorld_DrawWorldModels(true);       // glass etc.
		GLPolyGrid_Draw(true);         // water/ice: translucent, after the opaque scene
		GLParticle_DrawSystems();      // fire, smoke, the waterfall sheet
		GLSprite_DrawSprites();        // lamp halos / glows (translucent quads)

		// ⚠️ LAST. The player-view pass CLEARS THE DEPTH BUFFER so the weapon can
		// never clip into the world; anything world-space drawn after it would then
		// depth-test against an empty buffer and paint over the whole level. That is
		// precisely what made the water "float above the world" in gameplay while
		// looking correct in cinematics (which draw no player-view weapon).
		GLModel_DrawPlayerView(fRight / fTop);
	}
scene_done:;      // LT_WORLD_ONLY lands here

	// Scene done: fog is scene state, and everything drawn after this point
	// (console, HUD, menus, the frame dump) is 2D.
	GLWorld_DisableFog();

	// One-time confirmation that a scene actually rendered.
	static bool s_bReportedScene = false;
	if (!s_bReportedScene && GLWorld_IsLoaded())
	{
		s_bReportedScene = true;
		fprintf(stderr, "[nr] RenderScene: cam=(%.0f %.0f %.0f) fov=(%.2f %.2f) vp=%dx%d\n",
		        vPos.x, vPos.y, vPos.z, fFovX, fFovY, nVpW, nVpH);
	}

	// Frame dump for headless verification (LT_DUMP_FRAME=<n>[,<n>...] dumps
	// the n-th rendered scene — world only; LT_DUMP_SWAP in nr_SwapBuffers
	// captures the final presented frame incl. console/2D).
	static int s_aDumpFrame[NR_MAX_DUMP_FRAMES];
	static int s_nDumpFrameCount = -1;
	if (s_nDumpFrameCount < 0)
		s_nDumpFrameCount = nr_ParseDumpList(getenv("LT_DUMP_FRAME"), s_aDumpFrame);
	static int s_nSceneCount = 0;
	++s_nSceneCount;
	for (int i = 0; i < s_nDumpFrameCount; ++i)
	{
		if (s_nSceneCount == s_aDumpFrame[i])
			nr_DumpDrawable(s_nSceneCount, "scene", s_nDumpFrameCount > 1);
	}
#endif
	return 0;
}

#ifdef LT_MACOS
// World render-data loader for the RenderStruct seam (Phase-2 GL).
bool nr_LoadWorldData(ILTStream *pStream)
{
	// The loader creates GL textures (lightmaps) as it parses.
	LTMacWin_MakeCurrent();
	return GLWorld_Load(pStream);
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
	// Present the back buffer. Scene content is drawn by nr_RenderScene (and 2D
	// by the optimized-2D path later); presenting must not clear it.
	LTMacWin_MakeCurrent();

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
			snprintf(sTitle, sizeof(sTitle), "No One Lives Forever 2 (GL) — %.0f FPS",
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

	LTMacWin_SwapBuffers();

	// If no scene has ever been rendered (world not loaded yet), keep the
	// window visibly alive with the bring-up clear for the NEXT frame.
	if (!GLWorld_IsLoaded())
	{
		nr_SetClearColor(1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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

static GLuint g_SurfTex = 0;
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

	LTMacWin_MakeCurrent();
	nr_ExpandSurfaceRGBA(pBuf, bTransparent, transColor);

	int drawW = 640, drawH = 480;
	LTMacWin_GetSize(&drawW, &drawH);
	float scrW = (g_pGLStruct && g_pGLStruct->m_Width)  ? (float)g_pGLStruct->m_Width  : (float)drawW;
	float scrH = (g_pGLStruct && g_pGLStruct->m_Height) ? (float)g_pGLStruct->m_Height : (float)drawH;

	if (!g_SurfTex)
		glGenTextures(1, &g_SurfTex);
	glBindTexture(GL_TEXTURE_2D, g_SurfTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pBuf->m_Width, pBuf->m_Height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, g_SurfBuf);

	// Screen ortho over the engine's logical screen (== the backing drawable).
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glViewport(0, 0, drawW, drawH);
	glOrtho(0.0, scrW, scrH, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);       // 2D surface blit: D3D forces FOGENABLE FALSE too
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	if (bTransparent || alpha < 0.999f)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);
	glColor4f(1.0f, 1.0f, 1.0f, alpha);

	float iw = 1.0f / (float)pBuf->m_Width;
	float ih = 1.0f / (float)pBuf->m_Height;
	glBegin(GL_QUADS);
	for (int i = 0; i < 4; ++i)
	{
		glTexCoord2f(src[i][0] * iw, src[i][1] * ih);
		glVertex2f(dst[i][0], dst[i][1]);
	}
	glEnd();

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	glDepthMask(GL_TRUE);
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
	nr_PresentSurface(pBuf, (pRequest->m_BlitOptions & BLIT_TRANSPARENT) != 0,
	                  pRequest->m_TransparentColor.wVal, pRequest->m_Alpha, dst, src);
	return true;
}


void nr_MakeScreenShot(const char *pFilename)
{
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
	GLConVar_Read(g_pGLStruct);
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
	g_pGLStruct = pStruct;   // engine-side services for the GL texture/world code
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
	pStruct->CreateRenderObject = GLModel_CreateRenderObject;   // model LTB meshes
	pStruct->DestroyRenderObject = GLModel_DestroyRenderObject;
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
	pStruct->SetLightGroupColor = GLWorld_SetLightGroupColor;   // switchable lights (lamps)
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
