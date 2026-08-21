
// This module implements all the dsi_interface functions.

#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>

#include "bdefs.h"
#if defined(LT_MACOS) && defined(DE_CLIENT_COMPILE)
#include "macos_input.h"   // key queue lives in the client's window layer
#include "ltmacwindow.h"   // LTMacWin_RequestClose (our PostQuitMessage)
#endif
#include "stdlterror.h"
#include "stringmgr.h"
#include "sysfile.h"
#include "de_objects.h"
#include "servermgr.h"
#include "classbind.h"
#include "bindmgr.h"
#include "console.h"


//IClientShell game client shell object.
#include "iclientshell.h"
static IClientShell *i_client_shell;
define_holder(IClientShell, i_client_shell);



void dsi_OnReturnError(int err)
{
}

static LTBOOL dsi_LoadResourceModule()
{
return LTTRUE;      // DAN - temporary
}

static void dsi_UnloadResourceModule()
{
}


LTRESULT dsi_SetupMessage(char *pMsg, int maxMsgLen, LTRESULT dResult, va_list marker)
{
return LT_OK;      // DAN - temporary
}


int dsi_Init()
{
	dm_Init();	// Memory manager.
	str_Init();	// String manager.
	df_Init();	// File manager.
//	obj_Init();	// Object manager.
//	packet_Init();
return 0;      // DAN - temporary
}

void dsi_Term()
{
//	packet_Term();
//	obj_Term();
	df_Term();
	str_Term();
	dm_Term();
	return;
}

void* dsi_GetResourceModule()
{
return NULL;      // DAN - temporary
}


LTRESULT _GetOrCopyFile(char *pTempPath, char *pFilename, char *pOutName, int outNameLen)
{
    return LTTRUE;      // DAN - temporary
}


LTRESULT dsi_LoadServerObjects(CClassMgr *pInfo)
{
	// macOS: the game logic is our natively built dylib. dlopen only searches
	// the CWD when the path contains a slash, so default to "./"; overridable
	// via LT_OBJECT_MODULE for testing.
	const char* pGameServerObjectName = getenv("LT_OBJECT_MODULE");
	if (!pGameServerObjectName || !pGameServerObjectName[0])
		pGameServerObjectName = "./libObject.lto";

    //load the GameServer shared object
    int version;
    int status = cb_LoadModule(pGameServerObjectName, false, pInfo->m_ClassModule, &version);

    //check for errors.
    if (status == CB_CANTFINDMODULE) 
	{
        return LT_INVALIDOBJECTDLL;
    }
    else if (status == CB_NOTCLASSMODULE)
	{
        return LT_INVALIDOBJECTDLL;
    }
    else if (status == CB_VERSIONMISMATCH) 
	{
		return LT_INVALIDOBJECTDLLVERSION;
	}
	
/*	
	    // Get sres.dll.
	bFileCopied = false;
    if ((GetOrCopyFile("sres.dll", fileName, sizeof(fileName),bFileCopied) != LT_OK)
        || (bm_BindModule(fileName, bFileCopied, pClassMgr->m_hServerResourceModule) != BIND_NOERROR))
    {
		cb_UnloadModule( pClassMgr->m_ClassModule );

        sm_SetupError(LT_ERRORCOPYINGFILE, "sres.dll");
        RETURN_ERROR_PARAM(1, LoadServerObjects, LT_ERRORCOPYINGFILE, "sres.dll");
    }

    //let the dll know it's instance handle.
    if (instance_handle_server != NULL) 
	{
        instance_handle_server->SetInstanceHandle( pClassMgr->m_ClassModule.m_hModule );
    }
*/
	
	//cb_LoadModule(fileName, false, pInfo->m_ClassModule, &version);

	/*
	pInfo->m_hShellModule = (ShellModule*)malloc(sizeof(ShellModule));
	pInfo->m_hShellModule->m_hModule = NULL;
	pInfo->m_CreateShellFn =
		pInfo->m_hShellModule->m_CreateFn = (CreateShellFn) CreateServerShell;
	pInfo->m_DeleteShellFn =
		pInfo->m_hShellModule->m_DeleteFn = (DeleteShellFn) DeleteServerShell;
	*/

	return LT_OK;
}

void dsi_Sleep(uint32 ms)
{
// Several possible implementations:
// poll (requires sys/poll.h)
//	poll(NULL, 0, ms);
// select (requires sys/time.h, sys/types.h, unistd.h)
	timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = ms*1000;
	select(0, NULL, NULL, NULL, &timeout);
// SIGALRM (requires sys/time.h, unistd.h)
//	itimerval timerconfig;
//	memset(&timerconfig, 0, sizeof(timerconfig));
//	timerconfig.it_value.tv_usec = ms*1000;
//	setitimer(ITIMER_REAL, &timerconfig, NULL);
//	pause();
}

void dsi_ServerSleep(uint32 ms)
{ dsi_Sleep(ms); }

extern int32 g_ScreenWidth, g_ScreenHeight;	// Console variables.

#ifdef DE_CLIENT_COMPILE
// Provided by the render module (nullrender.cpp on macOS). Forward-declared
// rather than pulling sys/win/render.h in here.
extern RMode* rdll_GetSupportedModes();
extern void   rdll_FreeModeList(RMode *pModes);

// Byte-for-byte the Win32 dsi_GetDLLModes: copy the renderer's list into
// engine-allocated nodes so the caller can free it with dfree, then hand the
// renderer's own list back for the renderer to free.
static void dsi_GetDLLModes(char *pDLLName, RMode **pMyList)
{
	RMode *pListHead = rdll_GetSupportedModes();

	RMode *pCur = pListHead;
	while (pCur)
	{
		RMode *pMyMode;
		LT_MEM_TRACK_ALLOC(pMyMode = (RMode*)dalloc(sizeof(RMode)), LT_MEM_TYPE_MISC);
		if (!pMyMode)
			break;
		memcpy(pMyMode, pCur, sizeof(RMode));

		pMyMode->m_pNext = *pMyList;
		*pMyList = pMyMode;

		pCur = pCur->m_pNext;
	}

	rdll_FreeModeList(pListHead);
}
#endif


RMode* dsi_GetRenderModes()
{
	// Was `return NULL` -- and an empty mode list is what crashed the game's
	// display-options screen: CScreenDisplay::GetRendererData built an empty
	// resolution array, then GetRendererModeStruct indexed it on focus change
	// (the "Escape from display options" crash in the 2026-07-24 logs).
#ifdef DE_CLIENT_COMPILE
	RMode *pList = LTNULL;
	dsi_GetDLLModes("integrated", &pList);

	uint32 nModes = 0;
	for (RMode *p = pList; p; p = p->m_pNext)
		++nModes;
	fprintf(stderr, "[mac] %u render modes reported", nModes);
	for (RMode *p = pList; p; p = p->m_pNext)
		fprintf(stderr, " %ux%ux%u", p->m_Width, p->m_Height, p->m_BitDepth);
	fprintf(stderr, "\n");
	fflush(stderr);

	return pList;
#else
	return LTNULL;   // no renderer in the server
#endif
}

void dsi_RelinquishRenderModes(RMode *pMode)
{
	// Must actually free -- the game calls this after every GetRenderModes.
	RMode *pCur = pMode;
	while (pCur)
	{
		RMode *pNext = pCur->m_pNext;
		dfree(pCur);
		pCur = pNext;
	}
}

#ifdef DE_CLIENT_COMPILE
// The current render mode, kept by sys/win/render.cpp (built on macOS too):
// r_InitRender copies the ACTUAL drawable-corrected mode into it.
extern RMode g_RMode;
#endif

LTRESULT dsi_GetRenderMode(RMode *pMode)
{
#ifdef DE_CLIENT_COMPILE
	// Mirror the Win32 dsi_GetRenderMode. The old `return LTTRUE;` stub left
	// *pMode UNINITIALIZED — the game's CInterfaceResMgr::ScreenDimsChanged
	// divided stack garbage by 640 and every menu was laid out at a scale of
	// ~6.7 million (invisible, and single glyphs got wider than any wrap width).
	memcpy(pMode, &g_RMode, sizeof(RMode));
#else
	// Server links this file too but has no renderer.
	memset(pMode, 0, sizeof(RMode));
#endif
	return LT_OK;
}

#ifdef DE_CLIENT_COMPILE
// Defined in sys/win/render.cpp (built on macOS too); brings up the renderer for
// the requested mode (rdll_RenderDLLSetup + g_Render.Init). Forward-declared to
// avoid pulling sys/win/render.h here. Client-only: the server links this file
// too but has no renderer.
LTRESULT r_InitRender(RMode *pMode);

LTRESULT dsi_SetRenderMode(RMode *pMode)
{
	// Actually initialise the renderer (previously a no-op stub). The client
	// shell triggers this via g_pLTClient->SetRenderMode() once the engine is up.
	return r_InitRender(pMode);
}
#else
LTRESULT dsi_SetRenderMode(RMode *pMode)
{
	return LT_OK;   // no renderer in the server
}
#endif

LTRESULT dsi_ShutdownRender(uint32 flags)
{
return LTTRUE;      // DAN - temporary
}

LTRESULT _GetOrCopyClientFile(char *pTempPath, char *pFilename, char *pOutName, int outNameLen)
{
return LTTRUE;      // DAN - temporary
}

LTRESULT dsi_InitClientShellDE()
{
	// macOS Phase-3b: optionally load the real game client shell
	// (libCShell.dylib). Its define_interface(CTO2GameClientShell, IClientShell)
	// registers LATER than the compiled-in null shell, and the interface DB
	// connects holders to the *latest* implementation (ltmodule.cpp
	// CInterfaceNameMgr::Add), so i_client_shell reconnects to the real shell —
	// no need to remove the null shell (it stays a harmless fallback).
	// Gated by LT_LOAD_CSHELL so the null-shell bring-up remains the default
	// until the real shell's runtime path is fully wired.
#if defined(LT_MACOS) && defined(DE_CLIENT_COMPILE)
	const char *pLoadCShell = getenv("LT_LOAD_CSHELL");
	if (pLoadCShell && pLoadCShell[0] && pLoadCShell[0] != '0')
	{
		const char *pShellName = getenv("LT_CSHELL_MODULE");
		if (!pShellName || !pShellName[0])
			pShellName = "./libCShell.dylib";   // dlopen needs the "./"

		// Keep the binding for the process lifetime (CClientMgr is incomplete
		// here; the module never unloads during the bring-up experiment).
		static CBindModuleType *s_pShellModule = 0;
		int status = bm_BindModule(pShellName, false, s_pShellModule);
		if (status != BIND_NOERROR)
			dsi_ConsolePrint("dsi_InitClientShellDE: failed to load %s (status %d)", pShellName, status);
		else
			dsi_ConsolePrint("dsi_InitClientShellDE: loaded real client shell %s", pShellName);
	}
#endif

	// have the user's cshell and the clientMgr exchange info
	if ((i_client_shell == NULL ))
    {
		CRITICAL_ERROR("dsys_interface", "Can't create CShell\n");
	}

	return LT_OK;
}

void dsi_OnMemoryFailure()
{
	// This must NOT return: every caller is an allocator that has already
	// failed and whose callers do not null-check. Win32 longjmps out (client)
	// or exits 2222 (server); the empty stub here just fell through and turned
	// an out-of-memory into an unexplained NULL deref far away.
	fprintf(stderr, "[lt] FATAL: out of memory\n");
	fflush(stderr);
	exit(2222);
}

// Client-only functions.
void dsi_ClientSleep(uint32 ms)
{
	dsi_ServerSleep(ms);
}

LTBOOL dsi_IsInputEnabled()
{
return LTTRUE;      // DAN - temporary
}

// The UI key queue: CClientMgr::ForwardMessagesToScript drains this each frame
// and hands it to the client shell as OnKeyDown/OnKeyUp (VK codes). Filled by
// the Cocoa event pump — see runtime/kernel/src/sys/macos/macos_input.mm.
#if defined(LT_MACOS) && defined(DE_CLIENT_COMPILE)
#  define LTMAC_KEYQUEUE 1
#else
#  define LTMAC_KEYQUEUE 0   // server: no window/input layer
#endif

uint16 dsi_NumKeyDowns()
{
#if LTMAC_KEYQUEUE
	return (uint16)LTMacInput_NumKeyDowns();
#else
	return 0;
#endif
}

uint16 dsi_NumKeyUps()
{
#if LTMAC_KEYQUEUE
	return (uint16)LTMacInput_NumKeyUps();
#else
	return 0;
#endif
}

uint32 dsi_GetKeyDown(uint32 i)
{
#if LTMAC_KEYQUEUE
	return LTMacInput_GetKeyDown(i);
#else
	return 0;
#endif
}

uint32 dsi_GetKeyDownRep(uint32 i)
{
#if LTMAC_KEYQUEUE
	return LTMacInput_GetKeyDownRep(i);
#else
	return 0;
#endif
}

uint32 dsi_GetKeyUp(uint32 i)
{
#if LTMAC_KEYQUEUE
	return LTMacInput_GetKeyUp(i);
#else
	return 0;
#endif
}

void dsi_ClearKeyDowns()
{
#if LTMAC_KEYQUEUE
	LTMacInput_ClearKeyDowns();
#endif
}

void dsi_ClearKeyUps()
{
#if LTMAC_KEYQUEUE
	LTMacInput_ClearKeyUps();
#endif
}

void dsi_ClearKeyMessages()
{
#if LTMAC_KEYQUEUE
	LTMacInput_ClearKeyDowns();
	LTMacInput_ClearKeyUps();
#endif
}

LTBOOL dsi_IsConsoleUp()
{
    return LTFALSE;
}

void dsi_SetConsoleUp(LTBOOL bUp)
{
}

// Console enable state (present in the Win32 dsys; the client console code
// references these, so provide them on the linux/macOS path too).
static bool g_bConsoleEnabled = true;
bool dsi_IsConsoleEnabled()
{
	return g_bConsoleEnabled;
}

void dsi_SetConsoleEnable(bool bEnabled)
{
	g_bConsoleEnabled = bEnabled;
}

#include "version_info.h"
LTRESULT dsi_GetVersionInfo(LTVersionInfo &info)
{
	// The Win32 build extracts this from the EXE's version resource; macOS has
	// no PE resource, so report a fixed engine version for the "version" cvar.
	info.m_MajorVersion = 1;
	info.m_MinorVersion = 0;
	return LT_OK;
}

// Minimal: return the filename as-is (no temp copy). Real impl would extract
// from the .rez into a temp path; fonts/etc. that need a real file are Phase 1+.
LTRESULT GetOrCopyClientFile( char const* pszFilename, char* pszOutName,
							 int outNameLen, bool& bFileCopied)
{
	bFileCopied = false;
	if (pszOutName && outNameLen > 0)
	{
		LTStrCpy(pszOutName, pszFilename ? pszFilename : "", outNameLen);
	}
	return LT_OK;
}

LTBOOL dsi_IsClientActive()
{
	return TRUE;
}

// The exit message the engine passed to dsi_OnClientShutdown (Win32 keeps this
// in g_ClientGlob.m_ExitMessage, which does not exist on macOS -- see §5).
// Empty for a clean, user-requested quit; set when the engine is bailing out
// with an error it wants shown.
static char g_szExitMessage[512] = "";

const char* dsi_GetExitMessage()
{
	return g_szExitMessage;
}

void dsi_OnClientShutdown( char *pMsg )
{
	if (pMsg && pMsg[0])
		LTStrCpy(g_szExitMessage, pMsg, sizeof(g_szExitMessage));
	else
		g_szExitMessage[0] = '\0';

	// Win32 shows this message in a dialog after the loop exits; we have no
	// dialog, and a silent bail-out is indistinguishable from a clean quit.
	// An empty message IS the clean, user-requested quit.
	fprintf(stderr, "[mac] client shutdown requested%s%s\n",
	        g_szExitMessage[0] ? ": " : " (clean quit)", g_szExitMessage);
	fflush(stderr);

#if defined(LT_MACOS) && defined(DE_CLIENT_COMPILE)
	// THE point of this function. Win32 calls PostQuitMessage(0) here, which is
	// what actually breaks its message loop -- our loop instead runs until
	// LTMacWin_ShouldClose(), so raise exactly that flag. Without it the menu's
	// "Quit" (CScreenMain / CInterfaceMgr -> g_pLTClient->Shutdown()) reached
	// the engine and then did nothing at all.
	LTMacWin_RequestClose();
#endif
}

char* dsi_GetDefaultWorld()
{
return NULL;     // DAN - temporary
}


#include "server_interface.h"

extern CServerMgr *g_pServerMgr;

void dsi_PrintToConsole(const char *pMsg, ...) {
    va_list marker;
    char msg[1000];

    // ⚠️ ORIGINAL BEHAVIOUR: this printed ONLY when a dedicated-server app handler
    // was installed. On the CLIENT there is no app handler, so every
    // dsi_ConsolePrint() in the engine was a SILENT NO-OP — including the entire
    // UDPDebug family in udpdriver.cpp (levels 1-3, the driver's own packet-level
    // tracing) and the netmgr/driver error paths.
    //
    // That matters because the silence is indistinguishable from "the code never
    // ran". During the multiplayer work a run with +UDPDebug 2 produced an empty
    // log, which reads as "no packets" but actually meant "the printer is
    // disconnected". LT_TRACE_CONSOLE=1 mirrors console output to stdout so the
    // engine's own diagnostics — which are extensive and cost nothing — become
    // usable on this port. Off by default: retail prints none of this.
    static int s_nTraceConsole = -1;
    if (s_nTraceConsole < 0)
        s_nTraceConsole = getenv("LT_TRACE_CONSOLE") ? 1 : 0;

    if (s_nTraceConsole) {
        va_start(marker, pMsg);
        vsnprintf(msg, sizeof(msg), pMsg, marker);
        va_end(marker);

        printf("[con] %s\n", msg);
        fflush(stdout);
    }

    if (g_pServerMgr && g_pServerMgr->m_pServerAppHandler) {
        va_start(marker, pMsg);
        vsnprintf(msg, 999, pMsg, marker);
        va_end(marker);

        g_pServerMgr->m_pServerAppHandler->ConsoleOutputFn(msg);
    }
}

void* dsi_GetInstanceHandle()
{
return NULL;     // DAN - temporary
}

void* dsi_GetMainWindow()
{
return NULL;     // DAN - temporary
}

// No message boxes on this port (headless-friendly): route both to stderr so a
// fatal engine message is at least SEEN. Silently swallowing these is how an
// engine bail-out used to look like a plain hang.
LTRESULT dsi_DoErrorMessage(char *pMessage)
{
	fprintf(stderr, "[lt] error: %s\n", pMessage ? pMessage : "(null)");
	fflush(stderr);
	return LT_OK;
}

void dsi_MessageBox(char *pMessage, char *pTitle)
{
	fprintf(stderr, "[lt] %s: %s\n", pTitle ? pTitle : "LithTech",
	        pMessage ? pMessage : "(null)");
	fflush(stderr);
}
