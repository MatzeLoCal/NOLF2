// PopupText.cpp: implementation of the CPopupText class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PopupText.h"
#include "InterfaceMgr.h"
#include "GameClientShell.h"
#include "ClientWeaponMgr.h"
#include "PopupMgr.h"


namespace
{
}


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CPopupText::CPopupText()
{
	m_bVisible			= LTFALSE;
	m_bUsePopupState	= true;
	m_bWeaponsEnabled	= true;
}



CPopupText::~CPopupText()
{
	Term();
}


void CPopupText::Init(bool bUsePopupState/*=true*/)
{
	m_bVisible		= LTFALSE;
	m_bUsePopupState = bUsePopupState;

	m_Text.Create(" ",0,0,g_pInterfaceResMgr->GetFont(0),8,LTNULL);
	m_Frame.Create(g_pInterfaceResMgr->GetTexture("interface\\menu\\sprtex\\frame.dtx"),200,320,LTTRUE);

}

void CPopupText::Term()
{
	if (m_bVisible)
	{
		Close();
	}

}


void CPopupText::Close()
{

	m_bVisible = LTFALSE;


	//if we had our weapons out before, show them again now
	if (g_pInterfaceMgr && g_pPlayerMgr && m_bWeaponsEnabled)
	{
		g_pPlayerMgr->GetClientWeaponMgr()->EnableWeapons();
		g_pInterfaceMgr->EnableCrosshair(LTTRUE);
	}
}

void CPopupText::Draw()
{
	if (!m_bVisible) return;

	m_Frame.Render();
	m_Text.Render();

}


void CPopupText::DisplayPopup(uint8 nPopupId, char *pText, uint32 nTextId)
{
	POPUP* pPopup = g_pPopupMgr->GetPopup(nPopupId);
	if (!pPopup) return;

	CUIFont *pFont = g_pInterfaceResMgr->GetFont(pPopup->nFont);

	// ★ THE SHEET IS AUTHORED IN 640x480 AND SCALED, SO IT MUST TAKE THE SIZE
	// RATIO AND BE CENTRED ON THE REAL SCREEN -- NOT ON THE REFERENCE SPACE.
	//
	// The original scales frame and text by XRatio and centres both within
	// 640x480 ((640-w)/2, (480-h)/2), which is the middle of the display only
	// while XRatio == YRatio. A document sheet is 278x370, so its bottom edge
	// sits at ((480+370)/2)*XRatio = 425*XRatio -- 88.5% of the way down a
	// 480*XRatio screen, comfortably inside at 4:3. On a wider drawable the
	// sheet keeps growing with XRatio while the screen's height only grows with
	// YRatio, and at 2940x1912 (XRatio 4.594, YRatio 3.983) the bottom reaches
	// 1952 against a 1912-pixel screen: THE LAST 40 PIXELS OF THE SHEET, AND
	// THE FINAL LINE OF TEXT WITH THEM, FALL OFF THE BOTTOM OF THE DISPLAY.
	//
	// GetFontRatio() is min(XRatio,YRatio) (§67), so the sheet can never outgrow
	// either axis. The base position is then the real screen size divided back
	// through that scale, because CLTGUICtrl::SetScale computes
	// m_pos = m_basePos * fScale -- which lands the sheet truly centred.
	float fScale = g_pInterfaceResMgr->GetFontRatio();
	if (fScale <= 0.0f)
		fScale = 1.0f;

	LTIntPt pos( (int)(((float)g_pInterfaceResMgr->GetScreenWidth()  / fScale) - (float)pPopup->sSize.x) / 2,
	             (int)(((float)g_pInterfaceResMgr->GetScreenHeight() / fScale) - (float)pPopup->sSize.y) / 2 );

	m_Frame.SetFrame(g_pInterfaceResMgr->GetTexture(pPopup->szFrame));
	m_Frame.SetSize(pPopup->sSize.x,pPopup->sSize.y);
	m_Frame.SetBasePos(pos);
	m_Frame.SetScale(fScale);


	pos.x += pPopup->sTextOffset.x;
	pos.y += pPopup->sTextOffset.y;

	m_Text.SetScale(1.0f);

	// If we were given a string use it, otherwise get the string from the ID...

	if( pText )
	{
		m_Text.SetString( pText );
	}
	else
	{
		char szString[1024] = "";
		LoadString(nTextId,szString,sizeof(szString));

		char *pBody = strchr(szString,'@');
		if (pBody)
		{
			++pBody;
			m_Text.SetString(pBody);
		}
		else
		{
			m_Text.SetString(szString);
		}
	}

	m_Text.SetFont(pFont,pPopup->nFontSize);
	m_Text.SetColors(pPopup->argbTextColor,pPopup->argbTextColor,pPopup->argbTextColor);
	m_Text.SetFixedWidth(pPopup->nTextWidth);
	m_Text.SetBasePos(pos);
	m_Text.SetScale(fScale);		// same ratio as the frame, or it drifts off it

	m_bVisible = LTTRUE;

	//remember whether our weapons were out
	m_bWeaponsEnabled = g_pPlayerMgr->GetClientWeaponMgr()->WeaponsEnabled();

	if (m_bUsePopupState)
	{
		g_pInterfaceMgr->ChangeState(GS_POPUP);
	}
}


void CPopupText::Update()
{
}

LTBOOL CPopupText::OnKeyDown(int key, int rep)
{
	// They pressed escape - close the popup
	if (key == VK_ESCAPE)
	{
		Close();
        return LTTRUE;
	}
	return LTFALSE;
}
