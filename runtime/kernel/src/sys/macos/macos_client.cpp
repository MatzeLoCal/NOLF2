// ----------------------------------------------------------------------- //
//
// MODULE  : macos_client.cpp
//
// PURPOSE : macOS client glue — the portable C++ side of the platform shell.
//           Companion to macos_window.mm (Cocoa/GL).
//
//           NOTE on architecture: the Win32 client is built around a global
//           `ClientGlob g_ClientGlob` (declared in sys/win/dsys_interface.h).
//           On macOS the engine uses sys/linux/linuxdsys.{h,cpp} for the
//           display-system interface, which has NO ClientGlob and shares the
//           `__DSYS_INTERFACE_H__` include guard with the Win32 header — so the
//           Win32 ClientGlob path is unreachable here by design. We avoid the
//           dsys headers entirely (forward-declaring dsi_Init/dsi_Term) so no
//           engine header on the client include path drags in the Win32
//           dsys_interface.h / ClientGlob.
//
//           main() below replaces the Win32 WinMain/RunClientApp/StartClient
//           chain (all excluded via #ifndef LT_MACOS in sys/win/client.cpp),
//           calling the same engine entry points (dsi_Init / cm_Init /
//           g_pClientMgr->Init / ->Update / ->Term) without the g_ClientGlob
//           window plumbing — the Cocoa window/GL context is owned by the
//           LTMacWin_* C ABI instead.
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "clientmgr.h"      // cm_Init, g_pClientMgr, MAX_RESTREES
#include "consolecommands.h"  // c_CommandHandler (LT_TEST_CONSOLE)
#include "icommandlineargs.h"
#include "ltmacwindow.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>         // _exit (see the end of main)

// dsi_Init / dsi_Term live in sys/linux/linuxdsys.cpp (the macOS dsys backend).
// That directory's headers are not on the client include path, and the Win32
// sys/win/dsys_interface.h that *is* on the path would drag in the forbidden
// ClientGlob, so forward-declare just the two entry points we call. (clientmgr.h
// pulls no dsys header, so there is no ClientGlob hazard from the includes.)
int  dsi_Init();
void dsi_Term();

// command_line_args is resolved through the interface-DB holder pattern. The
// client's normal entry TU (sys/win/client.cpp) that declared it is excluded on
// macOS, so declare our own file-local holder here (clientmgr.cpp registers the
// implementation; multiple holders for one interface are fine).
static ICommandLineArgs *command_line_args;
define_holder(ICommandLineArgs, command_line_args);


// Per-frame bridge: pump pending Cocoa events and report whether the user asked
// to quit. The engine's frame loop calls this each tick; the renderer presents
// through the GL context (LTMacWin_SwapBuffers).
//
// Returns false when the window wants to close (engine should shut down).
extern "C" bool LTMacClient_FrameTick(void)
{
    LTMacWin_PumpEvents();
    return !LTMacWin_ShouldClose();
}

// Process entry point — replaces the Win32 WinMain in the (excluded) client.cpp.
// The engine's own "lock the mouse" flag. On Win32 g_CV_CursorCenter makes the
// main loop re-centre the cursor every frame and ClipCursor confine it to the
// window (client.cpp / render.cpp); on macOS the equivalent is real pointer
// lock, so the window layer polls this every pump. The game drives it from
// CCursorMgr::UseCursor -> "CursorCenter 0/1", which is the ONLY reliable
// gameplay-vs-menu signal: cursor MODE is not one, because NOLF2 draws its own
// cursor sprite and therefore asks for CM_None in menus too.
extern int32 g_CV_CursorCenter;
extern "C" bool LTMacWin_EngineWantsCursorLock(void) { return g_CV_CursorCenter != 0; }

// --------------------------------------------------------------------------
// LT_TEST_CONSOLE="cmd;cmd;..." — run console command strings once, from
// LT_TEST_CONSOLE_START seconds of wall clock (default 60, i.e. after the
// menu drive-through has reached GS_PLAYING). This is the headless equivalent
// of typing into the engine console, which is how the game's own commands
// (including "Cheat <code>") are reached; there is otherwise no way to set up
// an in-world state (give a weapon, warp, toggle a var) from an automated run.
// Commands run one per LT_TEST_CONSOLE_INTERVAL seconds (default 1).
// --------------------------------------------------------------------------
static void LTMacClient_TickTestConsole(void)
{
    static bool   s_bParsed = false;
    static char  *s_pCmds[16];
    static unsigned s_nCmds = 0, s_nNext = 0;
    static double s_fStart = 60.0, s_fInterval = 1.0, s_fT0 = 0.0;

    if (!s_bParsed)
    {
        s_bParsed = true;
        const char *pEnv = getenv("LT_TEST_CONSOLE");
        if (pEnv)
        {
            char *pDup = strdup(pEnv);
            for (char *p = strtok(pDup, ";"); p && s_nCmds < 16; p = strtok(NULL, ";"))
                s_pCmds[s_nCmds++] = p;
            if (const char *pS = getenv("LT_TEST_CONSOLE_START"))    s_fStart = atof(pS);
            if (const char *pI = getenv("LT_TEST_CONSOLE_INTERVAL")) s_fInterval = atof(pI);
            if (s_fInterval <= 0.0) s_fInterval = 1.0;
            fprintf(stderr, "[con] %u test commands, start %.1fs, every %.1fs\n",
                    s_nCmds, s_fStart, s_fInterval);
        }
    }

    if (s_nNext >= s_nCmds)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double fNow = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    if (s_fT0 == 0.0) { s_fT0 = fNow; return; }

    double fElapsed = fNow - s_fT0;
    if (fElapsed < s_fStart + s_fInterval * (double)s_nNext)
        return;

    const char *pCmd = s_pCmds[s_nNext++];
    fprintf(stderr, "[con] running \"%s\" (t=%.1fs)\n", pCmd, fElapsed);
    c_CommandHandler(pCmd);
}

extern "C" int main(int argc, char** argv)
{
    // Window size in POINTS. On a Retina display the GL drawable is 2x this
    // (LTMacWin_GetSize reports backing pixels, and the whole pipeline —
    // viewport, camera rect, 2D ortho — follows it), so 1280x960 points renders
    // at 2560x1920. Override with LT_WINDOW_SIZE=WxH.
    //
    // NOTE: this is NOT yet driven by the retail config's screenwidth/height
    // (autoexec.cfg asks for 1600x1200) — the console vars aren't loaded until
    // g_pClientMgr->Init() below, which happens after the window exists.
    int nWinW = 1280, nWinH = 960;
    {
        const char *pSize = getenv("LT_WINDOW_SIZE");
        int w = 0, h = 0;
        if (pSize && sscanf(pSize, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
        {
            nWinW = w;
            nWinH = h;
        }
    }
    // FULLSCREEN IS THE DEFAULT (this is a 2002 FPS, not a windowed app): a
    // borderless screen-sized window with the menu bar and Dock hidden.
    // LT_WINDOWED=1 gives a normal window back -- which is what the automated
    // runs and frame dumps use. The drawable size flows back to the engine
    // through nr_Init either way, so the camera rect and 2D layout follow.
    bool bFullscreen = (getenv("LT_WINDOWED") == NULL);

    fprintf(stderr, "[mac] LTMacWin_Create %dx%d (points)%s...\n",
            nWinW, nWinH, bFullscreen ? " FULLSCREEN" : "");
    if (!LTMacWin_Create("Lithtech (NOLF2)", nWinW, nWinH, bFullscreen))
        return 1;

    // --- Engine startup (mirrors Win32 RunClientApp + StartClient). ---

    // Command line. The -rez / -config scanning in the engine tolerates argv[0]
    // (the program name) being present, so pass argc/argv straight through.
    fprintf(stderr, "[mac] command_line_args->Init...\n");
    command_line_args->Init(argc, argv);

    // System-dependent init: memory / string / file managers.
    fprintf(stderr, "[mac] dsi_Init...\n");
    if (dsi_Init() != 0)
    {
        fprintf(stderr, "[mac] dsi_Init FAILED\n");
        LTMacWin_Destroy();
        return 1;
    }

    // Create the client manager (g_pClientMgr = new CClientMgr).
    fprintf(stderr, "[mac] cm_Init...\n");
    cm_Init();

    // Resource trees: every "-rez X" argument, then the default engine
    // resource — mirroring the Win32 StartClient() scan.
    const char *resTrees[MAX_RESTREES];
    uint32 nResTrees = 0;
    for (uint32 i = 0; i + 1 < command_line_args->Argc() && nResTrees < MAX_RESTREES - 1; i++)
    {
        if (stricmp(command_line_args->Argv(i), "-rez") == 0)
        {
            resTrees[nResTrees++] = command_line_args->Argv(i + 1);
            fprintf(stderr, "[mac]   res tree: %s\n", command_line_args->Argv(i + 1));
        }
    }
    resTrees[nResTrees++] = "engine.rez";

    const char *configFiles[2] = { "autoexec.cfg", "display.cfg" };

    fprintf(stderr, "[mac] g_pClientMgr->Init (resTrees=%u)...\n", nResTrees);
    LTRESULT initResult = g_pClientMgr->Init(resTrees, nResTrees, 2, configFiles);

    // DO NOT write autoexec.cfg back out during the bring-up. The engine saves
    // the live console state on shutdown, which against a REAL retail install
    // is destructive:
    //   * our input layer is still a null stub, so there are no key bindings in
    //     memory to save — the rewritten file loses every AddAction/rangebind
    //     (i.e. the player's entire control setup);
    //   * the real CShell's performance manager rewrites quality vars (it
    //     dropped GroupOffset1/2/3 to 1/1/2, halving/quartering texture
    //     resolution, plus detail/shadow/AA settings);
    //   * nr_Init reports the actual drawable, so screenwidth/height get
    //     overwritten with the window size.
    // Re-enable deliberately with LT_SAVE_CONFIG=1 once input is ported.
    if (g_pClientMgr)
    {
        const char *pSaveCfg = getenv("LT_SAVE_CONFIG");
        if (!(pSaveCfg && pSaveCfg[0] && pSaveCfg[0] != '0'))
            g_pClientMgr->m_bCanSaveConfigFile = false;
    }

    if (initResult != LT_OK)
    {
        fprintf(stderr, "[mac] CClientMgr::Init FAILED (0x%X)\n", (unsigned)initResult);
    }
    else
    {
        fprintf(stderr, "[mac] CClientMgr::Init OK — entering frame loop\n");
        while (LTMacClient_FrameTick())
        {
            LTMacClient_TickTestConsole();
            if (g_pClientMgr->Update() != LT_OK)
                break;
        }
    }

    // Shutdown (mirrors END_MAINLOOP).
    fprintf(stderr, "[mac] shutting down...\n");
    if (g_pClientMgr)
    {
        g_pClientMgr->Term();
        delete g_pClientMgr;
        g_pClientMgr = LTNULL;
    }
    dsi_Term();
    LTMacWin_Destroy();

    // ⚠️ _exit, NOT return: skip the C++ STATIC DESTRUCTORS of the game dylibs.
    //
    // instantiate_interface (sdk/inc/ltmodule.h) declares each interface impl as
    // a static global, so libCShell owns a static CTO2GameClientShell whose
    // destructor chain (~CTO2PlayerMgr -> ~CPlayerMgr -> ~CPlayerCamera) calls
    // back into the engine: `g_pLTClient->RemoveObject(m_hCollisionObject)` on a
    // raw HOBJECT nobody ever cleared. __cxa_finalize runs that AFTER the code
    // above has deleted g_pClientMgr, so it lands in CClientMgr::
    // RemoveClientObject with this == NULL (EXC_BAD_ACCESS at 0x4d8).
    //
    // Win32 has the identical structure (END_MAINLOOP deletes g_pClientMgr and
    // never unloads cshell.dll) and got away with it only because its freed
    // LTObject still read back an m_ObjectID != -1, taking RemoveObject's
    // early-out. Fixing the shell's dangling handles is game-code surgery for no
    // gain: every teardown that matters (world, sound device, config, files) has
    // already run in Term()/dsi_Term(), so there is nothing left to flush.
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
