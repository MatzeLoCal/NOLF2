// ----------------------------------------------------------------------- //
//
// MODULE  : InterfaceResMgr.h
//
// PURPOSE : Manager for resources associated with the interface
//
// (c) 1999-2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#if !defined(_INTERFACERESMGR_H_)
#define _INTERFACERESMGR_H_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#include "LTGUIMgr.h"
#include "InterfaceSurfMgr.h"
#include "iltfontmanager.h"

class CGameClientShell;
class CInterfaceResMgr;
extern CInterfaceResMgr* g_pInterfaceResMgr;

typedef std::vector<CUIFont*> FontArray;
typedef std::set<HTEXTURE> TextureSet;

const int kMaxSoundFilename = 64;

class CInterfaceResMgr
{
public:
	CInterfaceResMgr();
	virtual ~CInterfaceResMgr();

    LTBOOL              Init();
	void				Term();
	HSURFACE			GetSharedSurface(char *lpszPath)	{ return m_InterfaceSurfMgr.GetSurface(lpszPath); }
	void				FreeSharedSurface(char *lpszPath)	{ m_InterfaceSurfMgr.FreeSurface(lpszPath); }
	void				FreeSharedSurface(HSURFACE hSurf)	{ m_InterfaceSurfMgr.FreeSurface(hSurf); }

	HTEXTURE			GetTexture(const char *szTexName);

	CUIFont*			GetFont(uint32 nIndex);

	HSURFACE			GetSurfaceCursor();

	const char			*GetSoundSelect();
	const char			*GetSoundChange();
	const char			*GetSoundPageChange();
	const char			*GetSoundUnselectable();
	const char			*GetSoundArrowUp();
	const char			*GetSoundArrowDown();
	const char			*GetSoundArrowLeft();
	const char			*GetSoundArrowRight();
	const char			*GetObjectiveAddedSound();
	const char			*GetObjectiveRemovedSound();
	const char			*GetObjectiveCompletedSound();

	void				ScreenDimsChanged();


    LTBOOL              IsEnglish()                         { return m_bEnglish; }

	void				DrawScreen();


	void				ConvertScreenRect(LTRect &rect);
	void				ConvertScreenPos(LTIntPt &pos) {ConvertScreenPos(pos.x,pos.y);}
	void				ConvertScreenPos(int &x, int &y);
	// ★ LT_TRACE_UILAYOUT=1 -- one stderr line per control that recomputes its
	// layout, giving the authored base values, the live ratios, and the pixel
	// result BOTH ways: what the shipped rule produced and what the corrected
	// rule produces. One run therefore carries its own A/B, which matters here
	// because the controls it covers (subtitles, mission text) are driven by
	// level logic and cannot be made to appear in a +runworld harness at all.
	// ⚠️ stderr, not CPrint: CPrint needs LT_TRACE_CONSOLE as a second switch.
	// fTextScale is the extra factor the caller applied to the character height
	// (GetHUDTextScale() for HUD text, 1 for everything else); it is folded into
	// the 'new' font figure and named on the line when it is not 1.
	static void		TraceLayout(const char *pWho, int nBaseX, int nBaseY, int nBaseFont,
	                            float fTextScale = 1.0f);

    LTFLOAT             GetXRatio()                         {return m_fXRatio;}
    LTFLOAT             GetYRatio()                         {return m_fYRatio;}

	// ★★ THE ASPECT-RATIO FIX FOR TEXT (§67).
	//
	// The whole interface is authored in a 640x480 reference space, and the
	// original code scales FONT SIZES by GetXRatio() while scaling vertical
	// POSITIONS by GetYRatio(). On 4:3 those are equal and everything lines up
	// (retail at 1600x1200: 2.5 and 2.5). On any wider display XRatio runs ahead
	// of YRatio, so the text grows faster than the boxes it has to fit inside —
	// at 1470x956 that is 2.297 vs 1.992, i.e. text 15% too tall. The visible
	// result is the loading screen's briefing overflowing its panel into the
	// "LOADING" caption and the tip clipping off the bottom edge.
	//
	// ⚠️ This is ORIGINAL GAME CODE, not a port artefact — it is the well-known
	// NOLF2 widescreen layout problem, and the reason the community
	// WideScreenFix mod ships nothing but a rewritten Attributes\Layout.txt.
	//
	// Use this for anything measured in CHARACTER HEIGHT. Keep GetXRatio() for
	// horizontal extents (wrap widths, x positions) and GetYRatio() for vertical
	// positions: scaling those uniformly would pull edge-anchored HUD elements
	// inward, which is a different and worse bug.
	LTFLOAT             GetFontRatio()                       {return (m_fXRatio < m_fYRatio) ? m_fXRatio : m_fYRatio;}

	// ★★ THE PLAYER'S HUD TEXT SCALE -- LT_HUD_TEXT_SCALE=<percent>, set by the
	// launcher's "HUD text" control. NOT part of the aspect-ratio fix above.
	//
	// GetFontRatio() already puts every character at its AUTHORED size: a
	// 16-unit message is 16/480 = 3.3% of the screen height, exactly what the
	// original shows on the 4:3 displays it was laid out for. That is the
	// correct answer and it is still large -- the game was authored for a
	// 640x480..1024x768 CRT, and 3.3% of a 27" panel is a lot of text. Making
	// it smaller is therefore a PREFERENCE, a deliberate departure from the
	// original, and it stays behind a switch that defaults to "unchanged".
	//
	// ⚠️ HUD RUNNING TEXT ONLY: the message queues (rewards, pickups, chat,
	// search), transmissions, subtitles, mission text and the crosshair's
	// activation label. Every one of those is anchored at a point and grows
	// from it, so a smaller glyph simply leaves more room. It is NOT applied to
	// menus, popups or the decision/radio dialogs (text there sits inside
	// authored frames and columns), nor to the health/armor/ammo/air figures
	// (each belongs to the gauge drawn beside it).
	//
	// ⚠️ CHARACTER HEIGHT ONLY -- never a position or a wrap width. Those stay
	// on XRatio/YRatio so every block keeps its authored anchor; scaling them
	// would pull edge-anchored elements inward, the same mistake the note on
	// GetFontRatio() warns about.
	//
	// A PERCENTAGE, not a fraction: "80", not "0.8". A decimal point is parsed
	// according to the C locale in force, and "0.8" read under a comma locale
	// is 0. 100 (or unset) is the original size. Read once; see the .cpp.
	static float		GetHUDTextScale();
	LTFLOAT             GetHUDFontRatio()                    {return GetFontRatio() * GetHUDTextScale();}

	// ★★ THE 4:3 LETTERBOX FOR FULL-SCREEN AUTHORED ART (loading + postload).
	//
	// Screens whose backdrop is a single authored 640x480 image cannot use
	// GetXRatio() for x positions. The art is presented as a 4:3 box centred on
	// the display; text scaled by XRatio is laid out across the FULL width, so
	// on 16:9 it drifts left of the art and its first character falls onto the
	// margin. Black text on a black margin reads as "clipped and oversized" --
	// see the note on ls_GetBox in LoadingScreen.cpp for the full diagnosis.
	//
	// Shared by CLoadingScreen and CScreenPostload, which draw the same
	// [LoadScreenDefault] layout and must agree pixel-for-pixel: the postload
	// screen replaces the loading screen in place, so any disagreement shows up
	// as the text jumping when the level finishes loading.
	//
	// ★ At 4:3 this returns fLeft = 0 and fScale == GetXRatio(), i.e. exactly
	// the shipped behaviour on the aspect these screens were authored for.
	void                GetLayoutBox(float &fLeft, float &fScale)
	{
		const float fW = (float)GetScreenWidth();
		const float fH = (float)GetScreenHeight();

		fScale = (fH > 0.0f) ? (fH / 480.0f) : 1.0f;

		const float fBoxW = fH * (4.0f / 3.0f);
		fLeft = (fW > fBoxW) ? ((fW - fBoxW) * 0.5f) : 0.0f;
	}

    uint32              GetScreenWidth();
    uint32              GetScreenHeight();

	void				DrawMessage(int nMessageId, uint8 nFontSize = 0);
	void				DrawMessage(const char *pString, uint8 nFontSize = 0);


	//call Setup() before entering a 2-d state (screen)
	//call Clean() before returning to the game
    LTBOOL               Setup();
	void				Clean();

protected:
	// More initialization

    LTBOOL              InitFonts();
	CUIFont*			CreateFont(char const* pszFontFile, char const* pszFontFace, uint8 ptSize);

protected:
    LTBOOL				m_bEnglish;             // True if the resource file has English as the specified language

	TextureSet			m_TextureSet;			// list of used textures

	FontArray			m_FontArray;

	HSURFACE			m_hSurfCursor;			// The software cursor surface

	int					m_nYesVKeyCode;			// The virtual key code for "yes" responses
	int					m_nNoVKeyCode;			// The virtual key code for "no" responses

	CInterfaceSurfMgr	m_InterfaceSurfMgr;	// Used to share title graphics

    LTFLOAT              m_fXRatio;
    LTFLOAT              m_fYRatio;
    uint32              m_dwScreenWidth;
    uint32              m_dwScreenHeight;

	char				m_szSoundSelect[kMaxSoundFilename];
	char				m_szSoundChange[kMaxSoundFilename];
	char				m_szSoundPageChange[kMaxSoundFilename];
	char				m_szSoundUnselectable[kMaxSoundFilename];
	char				m_szSoundArrowUp[kMaxSoundFilename];
	char				m_szSoundArrowDown[kMaxSoundFilename];
	char				m_szSoundArrowLeft[kMaxSoundFilename];
	char				m_szSoundArrowRight[kMaxSoundFilename];
	char				m_szSoundObjAdd[kMaxSoundFilename];
	char				m_szSoundObjRemove[kMaxSoundFilename];
	char				m_szSoundObjComplete[kMaxSoundFilename];

	CLTGUIWindow	    m_MsgDlg;
	CLTGUITextCtrl*		m_pMsgText;

};

#define TERMSHAREDSURF(surf) if(surf) { g_pInterfaceResMgr->FreeSharedSurface(surf); surf = NULL; }

inline uint32 CInterfaceResMgr::GetScreenWidth()
{
	if (m_dwScreenWidth < 0)
		ScreenDimsChanged();
	return m_dwScreenWidth;
}
inline uint32 CInterfaceResMgr::GetScreenHeight()
{
	if (m_dwScreenHeight < 0)
		ScreenDimsChanged();
	return m_dwScreenHeight;
}


#endif // !defined(_INTERFACERESMGR_H_)