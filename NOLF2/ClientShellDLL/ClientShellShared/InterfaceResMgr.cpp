// ----------------------------------------------------------------------- //
//
// MODULE  : InterfaceResMgr.cpp
//
// PURPOSE : Manager for resources associated with the interface
//
// (c) 1999-2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "StdAfx.h"
#include "GameClientShell.h"
#include "InterfaceResMgr.h"
#include "ClientButeMgr.h"


CInterfaceResMgr*   g_pInterfaceResMgr = LTNULL;

namespace
{
	const uint8 kIndent = 8;
	const uint16 kBaseWidth = 320;
	const uint16 kBaseHeight = 200;
	const uint16 kBigWidth = 480;
	const uint16 kMinWidth = 160;
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CInterfaceResMgr::CInterfaceResMgr()
{
	g_pInterfaceResMgr = this;

	m_hSurfCursor = NULL;

	m_dwScreenWidth = 0;
	m_dwScreenHeight = 0;

	m_fXRatio = 1.0f;
	m_fYRatio = 1.0f;

	m_pMsgText = LTNULL;

}

CInterfaceResMgr::~CInterfaceResMgr()
{
    g_pInterfaceResMgr = LTNULL;
}

//////////////////////////////////////////////////////////////////////
// Function name	: CInterfaceResMgr::Init
// Description	    :
// Return type      : LTBOOL
//////////////////////////////////////////////////////////////////////
// See the banner on TraceLayout in the header. Announces itself once, so a run
// that was meant to carry the trace and did not is obvious in the log.
void CInterfaceResMgr::TraceLayout(const char *pWho, int nBaseX, int nBaseY, int nBaseFont,
                                   float fTextScale)
{
	static int s_nOn = -1;
	if (s_nOn < 0)
	{
		const char *p = getenv("LT_TRACE_UILAYOUT");
		s_nOn = (p && *p && *p != '0') ? 1 : 0;
		if (s_nOn)
			fprintf(stderr, "[uilay] LT_TRACE_UILAYOUT on -- 'old' is the shipped "
			                "XRatio rule, 'new' is y:YRatio / font:min(X,Y)\n");
	}
	if (!s_nOn || !g_pInterfaceResMgr) return;

	const float fX = g_pInterfaceResMgr->GetXRatio();
	const float fY = g_pInterfaceResMgr->GetYRatio();
	const float fF = g_pInterfaceResMgr->GetFontRatio();
	// The 'new' font figure is what the control actually set, so it carries the
	// HUD text scale when the caller applied one; the suffix says so, and gives
	// the unscaled figure, so the two effects are never confused in a log.
	char szHud[48] = "";
	if (fTextScale != 1.0f)
		snprintf(szHud, sizeof(szHud), " (hud text %d%% of %d)",
		         (int)(fTextScale * 100.0f + 0.5f), (int)(nBaseFont * fF));
	fprintf(stderr,
	        "[uilay] %-14s base(%d,%d) font %2d | X %.3f Y %.3f | "
	        "y %4d -> %4d | font %3d -> %3d%s\n",
	        pWho, nBaseX, nBaseY, nBaseFont, fX, fY,
	        (int)(nBaseY * fX), (int)(nBaseY * fY),
	        (int)(nBaseFont * fX), (int)(nBaseFont * fF * fTextScale), szHud);
}

// See the banner on GetHUDTextScale in the header.
//
// ⚠️ ANNOUNCES ITSELF, on stderr, once, whenever the variable is set at all --
// including when the value is refused. A scale that silently fell back to 100
// would look exactly like a setting that "does nothing", and the launcher's
// log header only proves the variable was PASSED, not that it was understood.
// Unset says nothing: that is the default and every ordinary run.
float CInterfaceResMgr::GetHUDTextScale()
{
	static float s_fScale = -1.0f;
	if (s_fScale > 0.0f) return s_fScale;

	s_fScale = 1.0f;
	const char *p = getenv("LT_HUD_TEXT_SCALE");
	if (!p || !*p) return s_fScale;

	// The bounds are sanity, not taste: below 50% a 14-unit subtitle is under
	// ten pixels on a 1280x720 window, and above 150% the three-line subtitle
	// block runs off the bottom edge again. The launcher offers 50..100.
	const int kMin = 50, kMax = 150;
	char *pEnd = NULL;
	long n = strtol(p, &pEnd, 10);
	if (pEnd == p || n <= 0)
	{
		fprintf(stderr, "[hud] LT_HUD_TEXT_SCALE='%s' IGNORED -- expects a percentage "
		                "(%d..%d, e.g. 80); HUD text stays at 100%%\n", p, kMin, kMax);
		return s_fScale;
	}
	long nUse = (n < kMin) ? kMin : (n > kMax) ? kMax : n;
	s_fScale = (float)nUse / 100.0f;
	if (nUse != n)
		fprintf(stderr, "[hud] LT_HUD_TEXT_SCALE=%ld is outside %d..%d -- using %ld%%\n",
		        n, kMin, kMax, nUse);
	fprintf(stderr, "[hud] LT_HUD_TEXT_SCALE on -- HUD message text, subtitles, mission "
	                "text and the activation label at %ld%% of their authored size\n", nUse);
	return s_fScale;
}

// See the banner on GetPointScale in the header. The same query, and the same
// bounds, as CCursorMgr's GetSoftwareCursorScale (GitHub issue #6): the engine
// hands out a pointer to a LIVE value, so one lookup follows the game view
// across a move between displays of different density.
float CInterfaceResMgr::GetPointScale()
{
#ifdef LT_MACOS
	static const float *s_pScale = LTNULL;
	static bool s_bAsked = false;
	if (!s_bAsked)
	{
		s_bAsked = true;
		void *pSlot = LTNULL;
		if (g_pLTClient->GetEngineHook("MacPointScale", &pSlot) == LT_OK && pSlot)
			s_pScale = (const float*)pSlot;
	}
	const float fScale = s_pScale ? *s_pScale : 1.0f;
	return (fScale > 1.0f && fScale <= 8.0f) ? fScale : 1.0f;
#else
	return 1.0f;
#endif
}

LTBOOL CInterfaceResMgr::Init()
{


	// Set the English flag
    if (stricmp((const char*)"english", LoadTempString(IDS_GAME_LANGUAGE)) != 0)
    {
        m_bEnglish=LTFALSE;
	}
	else
    {
        m_bEnglish=LTTRUE;
	}

	// Load the virtual key codes for yes responses
	char szTmp[64] = "";
    LoadString(IDS_MENU_VKEY_YES,szTmp,sizeof(szTmp));
	if (strlen(szTmp))
	{
        m_nYesVKeyCode=atoi(szTmp);
	}

	// Load the virtual key codes for no responses
    LoadString(IDS_MENU_VKEY_NO,szTmp,sizeof(szTmp));
	if (strlen(szTmp))
	{
        m_nNoVKeyCode=atoi(szTmp);
	}

	// Init the InterfaceSurfMgr class
    m_InterfaceSurfMgr.Init(g_pLTClient);

	// Initialize the fonts
    if (!InitFonts())
	{
        return LTFALSE;
	}

	// set resolution dependant variables
	ScreenDimsChanged();


	uint8 nFont = g_pLayoutMgr->GetDialogFontFace();
	CUIFont* pFont = GetFont(nFont);
	uint8 nFontSize = g_pLayoutMgr->GetDialogFontSize();

	m_pMsgText = debug_new(CLTGUITextCtrl);
    if (!m_pMsgText->Create("", LTNULL, LTNULL, pFont, nFontSize, LTNULL))
	{
		debug_delete(m_pMsgText);
		m_pMsgText = LTNULL;
        return LTFALSE;
	}
	m_pMsgText->SetColors(argbBlack, argbBlack, argbBlack);
	m_pMsgText->SetFixedWidth(kBaseWidth-2*kIndent);
	m_pMsgText->Enable(LTFALSE);

		char szBack[128] = "";
	g_pLayoutMgr->GetDialogFrame(szBack,sizeof(szBack));

	m_MsgDlg.Create(GetTexture(szBack),kBaseWidth,kBaseHeight);
	LTIntPt tmp(0,0);
	m_MsgDlg.AddControl(m_pMsgText, tmp);

    return LTTRUE;
}

//////////////////////////////////////////////////////////////////////
// Function name	: CInterfaceResMgr::Term
// Description	    :
// Return type		: void
//////////////////////////////////////////////////////////////////////

void CInterfaceResMgr::Term()
{
	Clean();

	m_MsgDlg.Destroy();

	// Terminate the InterfaceSurfMgr class
	m_InterfaceSurfMgr.Term();

	// Destroy CUI fonts through the font manager
	if (g_pFontManager)
	{
		FontArray::iterator iter = m_FontArray.begin();
		while (iter != m_FontArray.end())
		{
			g_pFontManager->DestroyFont(*iter);
			iter++;
		}
		m_FontArray.clear();
	}

	// Release any textures that the InterfaceResMgr has.
	for (TextureSet::iterator iter = m_TextureSet.begin(); iter != m_TextureSet.end(); iter++)
	{
		g_pTexInterface->ReleaseTextureHandle(*iter);
	}
	m_TextureSet.clear();

	if (m_hSurfCursor)
	{
        g_pLTClient->DeleteSurface(m_hSurfCursor);
		m_hSurfCursor = NULL;
	}

}

//////////////////////////////////////////////////////////////////////
// Function name	: CInterfaceResMgr::Setup
// Description	    :
// Return type      : LTBOOL
//////////////////////////////////////////////////////////////////////

LTBOOL CInterfaceResMgr::Setup()
{
	//preload common surfaces

    return LTTRUE;
}

//////////////////////////////////////////////////////////////////////
// Function name	: CInterfaceResMgr::Clean
// Description	    :
// Return type		: void
//////////////////////////////////////////////////////////////////////

void CInterfaceResMgr::Clean()
{
    if (g_pLTClient)
	{

		// free shared surfaces
		m_InterfaceSurfMgr.FreeAllSurfaces();


	}
}

//////////////////////////////////////////////////////////////////////
// Function name	: CInterfaceResMgr::DrawScreen
// Description	    :
// Return type		: void
//////////////////////////////////////////////////////////////////////
void CInterfaceResMgr::DrawScreen()
{
    _ASSERT(g_pLTClient);
    if (!g_pLTClient) return;

	if (m_dwScreenWidth == 0)
		ScreenDimsChanged();

	// The screen surface
    HSURFACE hScreen = g_pLTClient->GetScreenSurface();

	// Render the current screen
	g_pInterfaceMgr->GetScreenMgr()->Render(hScreen);

	return;
}



void CInterfaceResMgr::DrawMessage(int nMessageId, uint8 nFontSize)
{
	DrawMessage(LoadTempString(nMessageId),nFontSize);
}

void CInterfaceResMgr::DrawMessage(const char *pString, uint8 nFontSize)
{
    _ASSERT(g_pLTClient);
    if (!g_pLTClient || !pString) return;


	uint8 nSize;
	if (nFontSize)
		nSize = nFontSize;
	else
		nSize = g_pLayoutMgr->GetDialogFontSize();

	m_MsgDlg.SetScale(1.0f);

	m_pMsgText->SetFixedWidth(kBaseWidth-2*kIndent);
	m_pMsgText->SetString(pString);
	m_pMsgText->SetFont(LTNULL,nSize);


	float fw,fh;
	m_pMsgText->GetString()->GetDims(&fw,&fh);
	int nWidth = (int)fw;
	int nHeight = (int)fh;

	if (nHeight > kBaseWidth)
	{
		m_pMsgText->SetFixedWidth(kBigWidth-2*kIndent);
		m_pMsgText->GetString()->GetDims(&fw,&fh);
		nWidth = (int)fw;
		nHeight = (int)fh;
	}

	uint16 nDlgWidth = nWidth + 2*kIndent;
	if (nDlgWidth < kMinWidth)
		nDlgWidth = kMinWidth;
	uint16 nDlgHeight = nHeight + 2*kIndent;

	LTIntPt offset;

	m_MsgDlg.SetSize(nDlgWidth,nDlgHeight);

	offset.x = (nDlgWidth - nWidth) / 2;
	offset.y = kIndent;
	m_MsgDlg.SetControlOffset(m_pMsgText,offset);



	offset.x =  (640 - nDlgWidth) / 2;
	offset.y = (480 - nDlgHeight) / 2;
	m_MsgDlg.SetBasePos(offset);
	m_MsgDlg.SetScale(GetXRatio());

	m_MsgDlg.Show(LTTRUE);

	g_pLTClient->Start3D();
	g_pLTClient->StartOptimized2D();

	m_MsgDlg.Render();

	g_pLTClient->EndOptimized2D();
	g_pLTClient->End3D(END3D_CANDRAWCONSOLE);
	g_pLTClient->FlipScreen(0);

	m_MsgDlg.Show(LTFALSE);

}

LTBOOL CInterfaceResMgr::InitFonts()
{

	if (!g_pFontManager || !g_pTexInterface)
		return (LTFALSE);

	int nNumFonts = g_pLayoutMgr->GetNumFonts();
	char szFontName[_MAX_PATH];
	char szFontFace[_MAX_PATH];
	for (int i = 0; i < nNumFonts; i++)
	{
		g_pLayoutMgr->GetFontName( i, szFontName, sizeof( szFontName ), szFontFace, sizeof( szFontFace ));
		uint8 nPtSize = g_pLayoutMgr->GetFontSize(i);
		if (!nPtSize)
			nPtSize = 24;
		CUIFont *pFont = CreateFont( szFontName, szFontFace, nPtSize );
		if (pFont)
			m_FontArray.push_back(pFont);
		else
		{
			g_pLTClient->CPrint("Failed to create font <%s> at size %d.",szFontName,nPtSize);
		}
	}

	if ((int)m_FontArray.size() != nNumFonts)
	{
		return LTFALSE;
	}

    return LTTRUE;

}

#define USABLE_HEIGHT_I 480
#define USABLE_HEIGHT_F 480.0f

void CInterfaceResMgr::ScreenDimsChanged()
{
    if (!g_pLTClient) return;

	RMode currentMode;
    g_pLTClient->GetRenderMode(&currentMode);

	m_fXRatio = (float)currentMode.m_Width / 640.0f;
	m_fYRatio = (float)currentMode.m_Height / USABLE_HEIGHT_F;

	m_dwScreenWidth = currentMode.m_Width;
	m_dwScreenHeight = currentMode.m_Height;

	// ★★★★ TELL THE GUI LIBRARY THAT X AND Y NO LONGER SHARE A RATIO.
	//
	// Every CLTGUICtrl::SetScale caller passes GetXRatio(), and the control then
	// used that ONE number for x, y and character height. That is correct only
	// while the display is 4:3. At 16:9 the horizontal ratio is 4/3 larger than
	// the vertical one, so the bottom of every front-end screen — the Quit item
	// at BackPos (32,436), the version string, the help line — was pushed off the
	// bottom edge, and the text was a third too large.
	//
	// These two multipliers convert the horizontal scale each caller passes into
	// the vertical and character-height scales the rule in this header calls for.
	// At 4:3 both are exactly 1.0f, i.e. the shipped behaviour, unchanged.
	CLTGUICtrl::SetLayoutRatios(m_fYRatio / m_fXRatio,
	                            GetFontRatio() / m_fXRatio);
}


HSURFACE CInterfaceResMgr::GetSurfaceCursor()
{
	if (!m_hSurfCursor)
        m_hSurfCursor = g_pLTClient->CreateSurfaceFromBitmap("interface\\cursor0.pcx");
	_ASSERT(m_hSurfCursor);
	return m_hSurfCursor;
};


const char *CInterfaceResMgr::GetSoundSelect()
{
	if (!strlen(m_szSoundSelect))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("SelectSound",m_szSoundSelect,sizeof(m_szSoundSelect));
	}
	_ASSERT(strlen(m_szSoundSelect));
	return m_szSoundSelect;
};

const char *CInterfaceResMgr::GetSoundUnselectable()
{
	if (!strlen(m_szSoundUnselectable))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("UnselectableSound",m_szSoundUnselectable,sizeof(m_szSoundUnselectable));
	}
	_ASSERT(strlen(m_szSoundUnselectable));
	return m_szSoundUnselectable;
};

const char *CInterfaceResMgr::GetSoundChange()
{
	if (!strlen(m_szSoundChange))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("SelectChangeSound",m_szSoundChange,sizeof(m_szSoundChange));
	}
	_ASSERT(strlen(m_szSoundChange));
	return m_szSoundChange;
};

const char *CInterfaceResMgr::GetSoundPageChange()
{
	if (!strlen(m_szSoundPageChange))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("PageChangeSound",m_szSoundPageChange,sizeof(m_szSoundPageChange));
	}
	_ASSERT(strlen(m_szSoundPageChange));
	return m_szSoundPageChange;
};

const char *CInterfaceResMgr::GetSoundArrowUp()
{
	if (!strlen(m_szSoundArrowUp))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ArrowUpSound",m_szSoundArrowUp,sizeof(m_szSoundArrowUp));
	}
	_ASSERT(strlen(m_szSoundArrowUp));
	return m_szSoundArrowUp;
};

const char *CInterfaceResMgr::GetSoundArrowDown()
{
	if (!strlen(m_szSoundArrowDown))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ArrowDownSound",m_szSoundArrowDown,sizeof(m_szSoundArrowDown));
	}
	_ASSERT(strlen(m_szSoundArrowDown));
	return m_szSoundArrowDown;
};

const char *CInterfaceResMgr::GetSoundArrowLeft()
{
	if (!strlen(m_szSoundArrowLeft))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ArrowLeftSound",m_szSoundArrowLeft,sizeof(m_szSoundArrowLeft));
	}
	_ASSERT(strlen(m_szSoundArrowLeft));
	return m_szSoundArrowLeft;
};

const char *CInterfaceResMgr::GetSoundArrowRight()
{
	if (!strlen(m_szSoundArrowLeft))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ArrowRightSound",m_szSoundArrowLeft,sizeof(m_szSoundArrowLeft));
	}
	_ASSERT(strlen(m_szSoundArrowLeft));
	return m_szSoundArrowLeft;
};



const char *CInterfaceResMgr::GetObjectiveAddedSound()
{
	if (!strlen(m_szSoundObjAdd))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ObjAddSound",m_szSoundObjAdd,sizeof(m_szSoundObjAdd));
	}
	_ASSERT(strlen(m_szSoundObjAdd));
	return m_szSoundObjAdd;
};

const char *CInterfaceResMgr::GetObjectiveRemovedSound()
{
	if (!strlen(m_szSoundObjRemove))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ObjRemoveSound",m_szSoundObjRemove,sizeof(m_szSoundObjRemove));
	}
	_ASSERT(strlen(m_szSoundObjRemove));
	return m_szSoundObjRemove;
};

const char *CInterfaceResMgr::GetObjectiveCompletedSound()
{
	if (!strlen(m_szSoundObjComplete))
	{
		g_pClientButeMgr->GetInterfaceAttributeString("ObjCompleteSound",m_szSoundObjComplete,sizeof(m_szSoundObjComplete));
	}
	_ASSERT(strlen(m_szSoundObjComplete));
	return m_szSoundObjComplete;
};



void CInterfaceResMgr::ConvertScreenPos(int &x, int &y)
{
	if (m_dwScreenWidth == 640 && m_dwScreenHeight == USABLE_HEIGHT_I)
		return;

	x = (int)( (LTFLOAT)x * m_fXRatio );
	y = (int)( (LTFLOAT)y * m_fYRatio );
};

void CInterfaceResMgr::ConvertScreenRect(LTRect &rect)
{
	if (m_dwScreenWidth == 640 && m_dwScreenHeight == USABLE_HEIGHT_I)
		return;

	rect.left = (int)( (LTFLOAT)rect.left * m_fXRatio );
	rect.right = (int)( (LTFLOAT)rect.right * m_fXRatio );
	rect.top = (int)( (LTFLOAT)rect.top * m_fYRatio );
	rect.bottom = (int)( (LTFLOAT)rect.bottom * m_fYRatio );

};

CUIFont* CInterfaceResMgr::GetFont(uint32 nIndex)
{
	if (nIndex >= m_FontArray.size())
		return LTNULL;
	return m_FontArray.at(nIndex);
}

//////////////////////////////////////////////////////////////////////
// Load font information from file and build the font
CUIFont* CInterfaceResMgr::CreateFont(char const* pszFontFile, char const* pszFontFace, uint8 ptSize)
{
	// Must specify a font face.
	if ( !pszFontFace ) 
		return LTNULL;

	CUIFont * pFont = g_pFontManager->CreateFont( pszFontFile, pszFontFace, ptSize, 33, 255);
	
	//this will set the width of the space character for proportional fonts
	if (pFont)
	{
		// ★ RESTORED (GitHub issue #20: "( 1 )" instead of "(1)"). The space
		// is not in the font texture -- the set starts at 33 -- so every space
		// is drawn at the font's default width. The shipped game set that to
		// ptSize/4 HERE: retail CSHELL.DLL does `shr bl,2` and calls
		// SetDefCharWidth right after CreateFont (0x1003ebb0). This source
		// release had the call commented out with the note below, leaving the
		// engine's own default, the font's full space ADVANCE: 0.609 em for
		// Typist, twice the retail space. Every gap in the HUD text ("FMJ ( 18 )",
		// "Can't use the Bandages!") was about a letter wide instead of a third.
		//[dlj] This is now handled programmatically in cuiformattedpolystring_impl
		pFont->SetDefCharWidth((ptSize/4));
		pFont->SetDefColor(argbBlack);
	}

	if (pFont)
	{
		uint32 w,h;
		g_pTexInterface->GetTextureDims(pFont->GetTexture(),w,h);
		g_pLTClient->CPrint("Created font <%s> using a %dX%d texture.",pszFontFace,w,h);

	}

	
	return (pFont);

/*
//create a font from a texture

	HTEXTURE hTex = LTNULL;
	LTIntPt ptSize(23,23);

	char szFontName[128] = "Interface/Fonts/font_large.dtx";
	char szFontTable[128] = "Interface/Fonts/font_large.ftb";

	g_pTexInterface->CreateTextureFromName(hTex, szFontName);

	// Note valid textures which we will need to release during termination
	if (hTex)
		m_TextureSet.Add(hTex);

	// Got a table name, use it.
	uint8 *pBuf = LTNULL;
	ILTStream *pStream;
	int32 iSize;

	// Slurp the font table into memory
	if(g_pLTClient->OpenFile(szFontTable, &pStream) == LT_OK)
	{
		iSize = pStream->GetLen();
		pBuf = debug_newa(uint8, iSize);

		if (!pBuf)
		{
			g_pLTClient->CPrint("Couldn't alloc for font table");
			pStream->Release();
			return LTNULL;
		}

		// Slurp the whole file into memory.
		pStream->Read(pBuf, iSize);
		pStream->Release();
	}
	pFont = g_pFontManager->CreateFont(hTex, pBuf, ptSize.y);

	if (!pFont)
		return (pDefault);

	// Perform any necessary rescaling
	if (ht != 0) pFont->SetDefCharHeight(ht);

	pFont->SetDefColor(0xFF000000);
	return (pFont);

*/			

}


HTEXTURE CInterfaceResMgr::GetTexture(const char *szTexName)
{
	HTEXTURE hTex = LTNULL;

	LTRESULT res = g_pTexInterface->FindTextureFromName(hTex, szTexName);

	//if we found it, see if it's in our list
	if (res == LT_OK)
	{
		TextureSet::iterator iter = m_TextureSet.find(hTex);

		//if is not in our list, inc the ref count so no one else frees it
		if (iter == m_TextureSet.end())
		{
//			g_pLTClient->CPrint("CInterfaceResMgr::GetTexture() : increasing ref count to existing texture %s",szTexName);
			g_pTexInterface->AddRefTextureHandle(hTex);
			m_TextureSet.insert(hTex);
		}
	}
	else
	{
//		g_pLTClient->CPrint("CInterfaceResMgr::GetTexture() : creating texture %s",szTexName);
		g_pTexInterface->CreateTextureFromName(hTex, szTexName);
		// Note valid textures which we will need to release during termination
		if (hTex)
			m_TextureSet.insert(hTex);

	}

	return hTex;
}
