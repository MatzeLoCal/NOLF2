#include "bdefs.h"
#include <windows.h>   // Win32 types (macOS shim)

#include "iltcursor.h"


//------------------------------------------------------------------
//------------------------------------------------------------------
// Holders and their headers.
//------------------------------------------------------------------
//------------------------------------------------------------------

//the ILTClient game interface
#include "iltclient.h"
#ifdef LT_MACOS
#include "ltmacwindow.h"   // LTMacWin_SetCursorVisible, LTMacWin_CreateCursor/SetCursor
#include "macos_cres.h"    // LTMacCRes_LoadCursor (the pointer image in cres.dll)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#endif
static ILTClient *ilt_client;
define_holder(ILTClient, ilt_client);


//
//Internal implementation class for ILTCursorInst.
//

class CLTCursorInst : public ILTCursorInst {
public:

    CLTCursorInst() { m_hCursor = LTNULL; };
    ~CLTCursorInst() {};

    virtual LTRESULT IsValid() { return (m_hCursor ? LT_YES : LT_NO); };
    HCURSOR GetCursor() { return m_hCursor; };
    virtual void SetData(const void *pData) { m_hCursor = (HCURSOR)(pData); };
    virtual void *GetData() { return (void *)m_hCursor; };

    HCURSOR m_hCursor;
};


//
//Our implementation class for the ILTCursor interface.
//

class CLTCursor : public ILTCursor {
public:
    declare_interface(CLTCursor);

    CLTCursor() {m_hCurrentCursor = LTNULL;  m_eCursorMode = CM_None; }

    // Enable/disable hardware cursor.
    virtual LTRESULT    SetCursorMode(CursorMode cMode, bool bForce = false);

    // Get current cursor mode.  Always returns LT_OK and always fills in cMode.
    virtual LTRESULT    GetCursorMode(CursorMode &cMode);

    // Returns LT_YES if a hardware cursor can be used, LT_NO otherwise.
    // Since we can't detect this, just return LT_YES for now.
    virtual LTRESULT IsCursorModeAvailable(CursorMode cMode);

    // Set the current hardware cursor bitmap.  The bitmap comes from cshell.dll.
    virtual LTRESULT LoadCursorBitmapResource(const char *pName, HLTCURSOR &hCursor);

    // Free a cursor.
    virtual LTRESULT FreeCursor(const HLTCURSOR hCursor);

    // Set the current cursor.
    virtual LTRESULT SetCursor(HLTCURSOR hCursor);

    // Check if an HLTCURSOR is a valid one; returns LT_YES or LT_NO
    virtual LTRESULT IsValidCursor(HLTCURSOR hCursor);

    // Refresh the cursor
    virtual LTRESULT RefreshCursor();

protected:

    LTRESULT        PreSetMode(CursorMode eNewMode);
    LTRESULT        PostSetMode(CursorMode eOldMode);

    CursorMode      m_eCursorMode;
    HLTCURSOR       m_hCurrentCursor;
};

//instantiate our implementation class.
define_interface(CLTCursor, ILTCursor);



LTRESULT CLTCursor::PreSetMode(CursorMode eNewMode)
{
    int temp;

    temp = 0;

    switch(eNewMode)
    {
        case CM_Hardware:
            do {
                temp = ShowCursor(LTTRUE);
            } while (temp < 0);
            break;
        case CM_None:
            do {
                temp = ShowCursor(LTFALSE);
            } while (temp >= 0);
            break;
        default:
            break;
    }

    return LT_OK;
}

LTRESULT CLTCursor::PostSetMode(CursorMode eOldMode)
{
    switch(eOldMode)
    {
        case CM_Hardware:
            break;
        case CM_None:
            break;
        default:
            break;
    }

    return LT_OK;
}

LTRESULT CLTCursor::SetCursorMode(CursorMode cMode, bool bForce)
{
    CursorMode eOldMode;

    // Saaaaaanity check
    if (!IsCursorModeAvailable(cMode)) {
        return LT_UNSUPPORTED;
    }

#ifdef LT_MACOS
    // Hardware-cursor VISIBILITY (independent of pointer lock, which follows the
    // CursorCenter console var). NOLF2 draws its own cursor sprite, so it asks
    // for CM_None in menus as well as in gameplay -- without this the macOS
    // arrow sits on top of the game's own cursor. Done BEFORE the unchanged-mode
    // early-out: PreSetMode only runs on transitions, and the mode is first set
    // during CCursorMgr::Init, long before the window matters.
    LTMacWin_SetCursorVisible(cMode == CM_Hardware);
#endif

    // If we're already in this mode, let's just save some cycles, shall we?
    if (cMode == m_eCursorMode && !bForce) {
        return LT_OK;
    }

    /* In the future, as more things (such as software cursor support) are added,
     * this routine will become more expensive, likely. */

    PreSetMode(cMode);
    eOldMode = m_eCursorMode;
    m_eCursorMode = cMode;
    PostSetMode(eOldMode);

    return LT_OK;
}

LTRESULT CLTCursor::IsValidCursor(HLTCURSOR hCursor)
{
    return hCursor->IsValid();
}

LTRESULT CLTCursor::SetCursor(HLTCURSOR hCursor)
{
    if (!hCursor->IsValid())
        return LT_INVALIDPARAMS;

#ifdef LT_MACOS
    // The native half of ::SetCursor, which is an empty shim on this platform.
    // A handle with no image (see LoadCursorBitmapResource) selects the system
    // arrow. ⚠️ The IsValid() test above does not keep such a handle out: it
    // returns LT_YES or LT_NO, which are 86 and 87 -- both "true" -- so it has
    // never rejected anything on any platform. Leave it; CCursorMgr::Init
    // treats a failure here as fatal to the whole interface.
    LTMacWin_SetCursor(hCursor->GetData());
#endif
    ::SetCursor((HCURSOR)hCursor->GetData());

    return LT_OK;
}

LTRESULT CLTCursor::GetCursorMode(CursorMode &cMode)
{
    cMode = m_eCursorMode;

    return LT_OK;
}

LTRESULT CLTCursor::FreeCursor(const HLTCURSOR hCursor)
{
    /* We don't really do anything here, since we're not presently creating the cursor
     * from scratch. */

    delete ((CLTCursorInst*)hCursor);

    return LT_OK;
}

LTRESULT CLTCursor::IsCursorModeAvailable(CursorMode cMode)
{
    /* Eventually there may be more checks here */

    return LT_YES;
}

#ifdef LT_MACOS
// ★ THE HARDWARE CURSOR'S IMAGE (GitHub issue #6).
//
// This used to hand back a handle with no image at all, on the reasoning that
// "the cursor the game actually draws is a surface loaded from
// interface\cursor0.pcx". That is only the FALLBACK. NOLF2's default is
// "Hardware Cursor: On", where the game draws nothing and expects the OS to
// show the pointer it loaded here -- so the menus ran under the macOS arrow.
// The image is where Windows finds it, in cres.dll (see macos_cres.h).
//
// Returns NULL -- the system arrow -- when there is no image to be had. That is
// deliberately not an error: CCursorMgr::Init returns FALSE on one, and that
// takes the whole InterfaceMgr and the client shell down with it.
static void *ltmac_NativeCursorFor(const char *pName)
{
    // MAKEINTRESOURCE: a numeric id travelling as a pointer.
    const uintptr_t nRaw = (uintptr_t)pName;
    if (nRaw >> 16)
    {
        fprintf(stderr, "[cur] cursor resource '%s' is asked for by NAME -- only numeric "
                        "ids are read from cres.dll; the system arrow is used\n", pName);
        return NULL;
    }
    const uint32 nID = (uint32)nRaw;

    // One native cursor per id, however often the game re-initialises
    // (CCursorMgr::Init runs again after every renderer restart).
    static std::vector<std::pair<uint32, void*> > s_aLoaded;
    for (size_t i = 0; i < s_aLoaded.size(); ++i)
    {
        if (s_aLoaded[i].first == nID)
            return s_aLoaded[i].second;
    }

    void *pNative = NULL;
    if (getenv("LT_NO_GAME_CURSOR"))
    {
        // The A/B arm: the behaviour before this existed.
        fprintf(stderr, "[cur] LT_NO_GAME_CURSOR on -- cursor resource %u NOT loaded, the "
                        "hardware cursor is the system arrow\n", nID);
    }
    else
    {
        LTMacCursorImage cImage;
        if (!LTMacCRes_LoadCursor(nID, cImage))
        {
            fprintf(stderr, "[cur] cursor resource %u is not in cres.dll (or will not "
                            "decode) -- the hardware cursor is the system arrow\n", nID);
        }
        else
        {
            pNative = LTMacWin_CreateCursor(&cImage.m_aRGBA[0], cImage.m_nWidth, cImage.m_nHeight,
                                            cImage.m_nHotX, cImage.m_nHotY);
            fprintf(stderr, "[cur] hardware cursor: resource %u from cres.dll, %dx%d, %d bpp, "
                            "hot spot (%d,%d)%s\n",
                    nID, cImage.m_nWidth, cImage.m_nHeight, cImage.m_nBitCount,
                    cImage.m_nHotX, cImage.m_nHotY,
                    pNative ? "" : " -- BUT the system refused the image, using its arrow");
        }
    }
    s_aLoaded.push_back(std::make_pair(nID, pNative));
    return pNative;
}
#endif // LT_MACOS

LTRESULT CLTCursor::LoadCursorBitmapResource(const char *pName, HLTCURSOR &hCursor)
{
#ifdef LT_MACOS
    HLTCURSOR hNewMac;
    LT_MEM_TRACK_ALLOC(hNewMac = new CLTCursorInst(),LT_MEM_TYPE_MISC);
    hNewMac->SetData(ltmac_NativeCursorFor(pName));
    hCursor = hNewMac;
    return LT_OK;
#else
    HINSTANCE hInst;
    LTRESULT dRes;
    HCURSOR hWinCursor;
    HLTCURSOR hNew;

    if ((dRes = ilt_client->GetEngineHook("cres_hinstance",(void **)&hInst)) != LT_OK)
        return dRes;

    hWinCursor = (HCURSOR)(::LoadImage(hInst, pName, IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR));

    if (!hWinCursor)
        return LT_MISSINGCURSORRESOURCE;

    LT_MEM_TRACK_ALLOC(hNew = new CLTCursorInst(),LT_MEM_TYPE_MISC);

    hNew->SetData((void *)hWinCursor);
    hCursor = hNew;

    return LT_OK;
#endif // LT_MACOS
}

LTRESULT CLTCursor::RefreshCursor()
{
    int temp;

    temp = 0;

    switch(m_eCursorMode)
    {
        case CM_Hardware:
            do {
                temp = ShowCursor(LTTRUE);
            } while (temp < 0);
            break;
        case CM_None:
            do {
                temp = ShowCursor(LTFALSE);
            } while (temp >= 0);
            break;
        default:
            break;
    }

    return LT_OK;
}
