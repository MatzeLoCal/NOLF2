// ----------------------------------------------------------------------- //
//
// MODULE  : macos_clientshell.cpp
//
// PURPOSE : Null IClientShell for the macOS engine bring-up (Phases 1-2).
//
//           The engine requires an IClientShell implementation: CClientMgr::Init
//           dereferences the `i_client_shell` holder (OnEngineInitialized), and
//           the frame loop calls PreUpdate/Update/PostUpdate on it every tick
//           (~49 call sites across the client). The real shell is the game's
//           ClientShellDLL (Phase 3, not built yet), so this is a no-op stand-in.
//
//           IClientShellStub (sdk/inc/iclientshell.h) supplies empty bodies for
//           every callback. We override two:
//             - OnEngineInitialized: return LT_OK (the stub default LT_ERROR
//               would shut the engine down) AND bring up the renderer via
//               SetRenderMode, which the real client shell is responsible for.
//             - Update: drive a present each frame so the GL renderer's output
//               is visible (Phase 2 first pixel; no world/camera yet).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "iclientshell.h"
#include "iltclient.h"
#include "ltobjectcreate.h"   // full ObjectCreateStruct (headers only forward-declare it)
#include "icommandlineargs.h"
#include "sys/shared/world_renderdata.h"   // world bounds for the bring-up orbit camera
#include "ltmacwindow.h"           // LTMacWin_GetSize (actual drawable size)

#include <stdio.h>
#include <math.h>

// Defines the global g_pLTClient and its interface-DB holder (resolves to the
// engine's CLTClient). We use it to drive render mode + presentation.
SETUP_GPLTCLIENT();

// Command line access (for the -world integration test hook below).
static ICommandLineArgs *command_line_args;
define_holder(ICommandLineArgs, command_line_args);

class CNullClientShell : public IClientShellStub
{
public:
    declare_interface(CNullClientShell);

    virtual LTRESULT OnEngineInitialized(RMode *pMode, LTGUID *pAppGuid)
    {
        // Bring up the (GL) renderer for the engine's default mode. In the real
        // game the client shell owns this call; here we just kick it so the
        // window gets a live render context and CClientMgr::Init can complete.
        if (g_pLTClient)
            g_pLTClient->SetRenderMode(pMode);
        return LT_OK;
    }

    // World-entry hook: note that a world is live so Update() starts rendering
    // it through the bring-up orbit camera.
    virtual void OnEnterWorld()
    {
        m_bInWorld = true;
        fprintf(stderr, "[mac] OnEnterWorld\n");
    }

    virtual void OnExitWorld()
    {
        m_bInWorld = false;
        m_hCamera = 0;   // engine owns/frees client objects with the world
    }

    // Render one frame of the loaded world through an orbiting camera (the
    // renderer draws via RenderScene). Returns false if not possible yet.
    bool RenderWorldFrame()
    {
        if (!m_bInWorld || !g_pLTClient || !RWorld_IsLoaded())
            return false;

        if (!m_hCamera)
        {
            ObjectCreateStruct ocs;
            ocs.Clear();
            ocs.m_ObjectType = OT_CAMERA;
            m_hCamera = g_pLTClient->CreateObject(&ocs);
            if (!m_hCamera)
                return false;

            // Fill the whole drawable (backing pixels). 90° horizontal FOV,
            // vertical derived from the real aspect ratio.
            int nW = 640, nH = 480;
            LTMacWin_GetSize(&nW, &nH);
            if (nW <= 0 || nH <= 0) { nW = 640; nH = 480; }
            const float kFovX = 1.5708f;
            float fFovY = 2.0f * atanf(tanf(kFovX * 0.5f) * ((float)nH / (float)nW));
            g_pLTClient->SetCameraFOV(m_hCamera, kFovX, fFovY);
            g_pLTClient->SetCameraRect(m_hCamera, true, 0, 0, nW, nH);
            fprintf(stderr, "[mac] camera created (%dx%d drawable)\n", nW, nH);

            // LT_TEST_SPRITE="file x y z sx sy": create a client sprite via the
            // full engine pipeline (CSpriteMgr .spr load + tracker) so the GL
            // sprite pass can be exercised without the game shell.
            if (const char *pTest = getenv("LT_TEST_SPRITE"))
            {
                char sFile[128]; float fX, fY, fZ, fSX, fSY;
                if (sscanf(pTest, "%127s %f %f %f %f %f", sFile, &fX, &fY, &fZ, &fSX, &fSY) == 6)
                {
                    ObjectCreateStruct sprOcs;
                    sprOcs.Clear();
                    sprOcs.m_ObjectType = OT_SPRITE;
                    sprOcs.m_Flags = FLAG_VISIBLE;
                    sprOcs.m_Pos.Init(fX, fY, fZ);
                    sprOcs.m_Scale.Init(fSX, fSY, 1.0f);
                    strncpy(sprOcs.m_Filename, sFile, sizeof(sprOcs.m_Filename) - 1);
                    HLOCALOBJ hSprite = g_pLTClient->CreateObject(&sprOcs);
                    fprintf(stderr, "[mac] test sprite '%s' @(%.0f %.0f %.0f) scale=(%.2f %.2f): %s\n",
                            sFile, fX, fY, fZ, fSX, fSY, hSprite ? "OK" : "FAILED");
                }
            }
        }

        // Slow orbit INSIDE the level (deep enough to see interiors), looking
        // at the world centre. LT_CAM_TARGET="x y z" [+ LT_CAM_DIST=<units>]
        // orbits a fixed point instead (bring-up: aim at a known object).
        LTVector vCenter, vHalfDims;
        RWorld_GetBounds(vCenter, vHalfDims);
        float fRadius = LTMAX(vHalfDims.x, vHalfDims.z) * 0.30f + 64.0f;
        float fHeight = vHalfDims.y * 0.10f;

        static int s_nHaveTarget = -1;
        static LTVector s_vTarget;
        static float s_fDist = 200.0f;
        static float s_fHeight = -10000.0f;   // sentinel: default = dist * 0.25
        if (s_nHaveTarget < 0)
        {
            const char *pTarget = getenv("LT_CAM_TARGET");
            s_nHaveTarget = (pTarget &&
                sscanf(pTarget, "%f %f %f", &s_vTarget.x, &s_vTarget.y, &s_vTarget.z) == 3) ? 1 : 0;
            const char *pDist = getenv("LT_CAM_DIST");
            if (pDist) s_fDist = (float)atof(pDist);
            const char *pHeight = getenv("LT_CAM_HEIGHT");   // negative = look up
            if (pHeight) s_fHeight = (float)atof(pHeight);
        }
        if (s_nHaveTarget == 1)
        {
            vCenter = s_vTarget;
            fRadius = s_fDist;
            fHeight = (s_fHeight > -9999.0f) ? s_fHeight : s_fDist * 0.25f;
        }

        m_fOrbitAngle += 0.004f;
        LTVector vPos(vCenter.x + sinf(m_fOrbitAngle) * fRadius,
                      vCenter.y + fHeight,
                      vCenter.z + cosf(m_fOrbitAngle) * fRadius);

        LTVector vForward = vCenter - vPos;
        vForward.Norm();
        LTRotation rRot(vForward, LTVector(0.0f, 1.0f, 0.0f));
        g_pLTClient->SetObjectPosAndRotation(m_hCamera, &vPos, &rRot);

        if (g_pLTClient->Start3D() != LT_OK)
            return false;
        g_pLTClient->RenderCamera(m_hCamera, 0.016f);
        g_pLTClient->End3D(END3D_CANDRAWCONSOLE);
        g_pLTClient->FlipScreen(0);
        return true;
    }

    // Present every frame. With no world/camera yet this shows the renderer's
    // clear colour (Phase 2 first pixel) via FlipScreen -> RenderStruct::SwapBuffers.
    virtual void Update()
    {
        // Phase-3 integration test: if "-world <name>" was passed, start a
        // local (single-player) game once. This drives the whole server-side
        // chain: StartupLocal -> LoadBinaries (dlopen Object.lto) ->
        // DoStartWorld (load + run the world). One-shot; runs on the first
        // engine Update tick so the engine is fully initialised.
        static bool s_bTriedStartGame = false;
        if (!s_bTriedStartGame)
        {
            s_bTriedStartGame = true;
            const char* pWorld = command_line_args ? command_line_args->FindArgDash("world") : 0;
            if (pWorld && g_pLTClient)
            {
                StartGameRequest req;
                req.m_Type = STARTGAME_NORMAL;
                LTStrCpy(req.m_WorldName, pWorld, sizeof(req.m_WorldName));
                fprintf(stderr, "[mac] StartGame(NORMAL, '%s')...\n", pWorld);
                LTRESULT dResult = g_pLTClient->StartGame(&req);
                fprintf(stderr, "[mac] StartGame -> 0x%X %s\n", (unsigned)dResult,
                        dResult == LT_OK ? "(LT_OK)" : "(FAILED)");
            }
        }

        // In-world: render through the orbit camera; otherwise just present the
        // renderer's bring-up clear so the window stays alive.
        if (!RenderWorldFrame() && g_pLTClient)
            g_pLTClient->FlipScreen(0);
    }

private:
    bool     m_bInWorld    = false;
    HLOCALOBJ m_hCamera    = 0;
    float    m_fOrbitAngle = 0.0f;
};

// Registers a single "Default" instance with the interface manager so the
// engine's `define_holder(IClientShell, i_client_shell)` resolves to it. Compiled
// directly into the EXE, so the static registrar is not dead-stripped.
define_interface(CNullClientShell, IClientShell);
