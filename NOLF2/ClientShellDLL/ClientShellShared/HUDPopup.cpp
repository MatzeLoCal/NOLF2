// ----------------------------------------------------------------------- //
//
// MODULE  : HUDPopup.cpp
//
// PURPOSE : Implementation of CHUDPopup to display popups
//
// (c) 2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

//
// Includes...
//

	#include "StdAfx.h"
	#include "HUDMgr.h"
	#include "InterfaceResMgr.h"
	#include "PopupMgr.h"
	#include "HUDPopup.h"



// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::CHUDPopup
//
//  PURPOSE:	Constructor
//
// ----------------------------------------------------------------------- //

CHUDPopup::CHUDPopup()
:	CHUDItem		( )
{
	m_UpdateFlags	= kHUDFrame;
	m_bVisible		= LTFALSE;
	m_bColorOverride = LTFALSE;
	m_fScale		= 0.0f;
	m_SheetSize		= LTIntPt(0,0);
	m_TextOffset	= LTIntPt(0,0);
	m_fLaidOutX		= 0.0f;
	m_fLaidOutY		= 0.0f;
}


// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::Init
//
//  PURPOSE:	Initialize the popup...
//
// ----------------------------------------------------------------------- //

LTBOOL CHUDPopup::Init()
{
	m_Text.Create(" ",0,0,g_pInterfaceResMgr->GetFont(0),8,LTNULL);
	m_Frame.Create(g_pInterfaceResMgr->GetTexture("interface\\menu\\sprtex\\frame.dtx"),200,320,LTTRUE);

	return LTTRUE;

}

// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::Term
//
//  PURPOSE:	Destroy thyself...
//
// ----------------------------------------------------------------------- //

void CHUDPopup::Term()
{
	m_Frame.Destroy();
	m_Text.Destroy();
}


// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::Hide
//
//  PURPOSE:	NONE
//
// ----------------------------------------------------------------------- //

void CHUDPopup::Hide( )
{
	m_bVisible = LTFALSE;
}

// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::Render
//
//  PURPOSE:	Draw the popup...
//
// ----------------------------------------------------------------------- //

void CHUDPopup::Render()
{
	if (!m_bVisible) return;
	m_Frame.Render();
	m_Text.Render();
}


// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::Update
//
//  PURPOSE:	Update the popup...
//
// ----------------------------------------------------------------------- //

void CHUDPopup::Update()
{
	// Sanity checks...
	if( !IsVisible() ) return;

	// Both ratios, not XRatio alone: the sheet is centred on the real screen,
	// so a change of height alone moves it too.
	if( m_fLaidOutX != g_pInterfaceResMgr->GetXRatio() ||
		m_fLaidOutY != g_pInterfaceResMgr->GetYRatio() )
		SetScale( g_pInterfaceResMgr->GetFontRatio() );

}


// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::SetScale
//
//  PURPOSE:	Set the items scale...
//
// ----------------------------------------------------------------------- //

// ★ THE SHEET KEEPS ITS SHAPE (GitHub issue #18). This popup is the code
// breaker's sheet (CGadgetDisabler, popups 2 and 4: the same 278x370 paper as
// a document). The original scaled it by XRatio and centred it in 640x480,
// which is the middle of the screen only at 4:3; with this port's widescreen
// rule (x by XRatio, y by YRatio) it kept its height but drew 4/3 too wide at
// 16:9, with its text wrapping a third wider than written. It is laid out the
// way CPopupText lays out a document: one scale -- GetFontRatio(), so it fits
// both ways -- for frame, text, wrap width and font, inside CLTGUIUniformScale,
// and centred on the REAL screen. At 4:3 this is the original layout.
void CHUDPopup::SetScale(float fScale)
{
	if (fScale <= 0.0f)
		fScale = 1.0f;

	CLTGUIUniformScale uniform;

	// SetScale computes m_pos = m_basePos * fScale (uniform here), so the base
	// position is the real screen measured in sheet units.
	LTIntPt pos( (int)(((float)g_pInterfaceResMgr->GetScreenWidth()  / fScale) - (float)m_SheetSize.x) / 2,
	             (int)(((float)g_pInterfaceResMgr->GetScreenHeight() / fScale) - (float)m_SheetSize.y) / 2 );

	m_Frame.SetBasePos(pos);
	m_Frame.SetScale( fScale );

	pos.x += m_TextOffset.x;
	pos.y += m_TextOffset.y;
	m_Text.SetBasePos(pos);
	m_Text.SetScale( fScale );

	m_fScale = fScale;
	m_fLaidOutX = g_pInterfaceResMgr->GetXRatio();
	m_fLaidOutY = g_pInterfaceResMgr->GetYRatio();
}


// ----------------------------------------------------------------------- //
//
//  ROUTINE:	CHUDPopup::Show
//
//  PURPOSE:	Display the popup with the passed in text...
//
// ----------------------------------------------------------------------- //

void CHUDPopup::Show( uint8 nPopupID, const char *pText )
{
	POPUP* pPopup = g_pPopupMgr->GetPopup(nPopupID);
	if (!pPopup) return;

	CUIFont *pFont = g_pInterfaceResMgr->GetFont(pPopup->nFont);

	// Placement and scale are SetScale's job now (see there); this only sets
	// up what the sheet IS. The original's base position here, centred in
	// 640x480, is what SetScale replaces with the real screen's centre.
	m_SheetSize  = pPopup->sSize;
	m_TextOffset = pPopup->sTextOffset;

	m_Frame.SetFrame(g_pInterfaceResMgr->GetTexture(pPopup->szFrame));
	m_Frame.SetSize(pPopup->sSize.x,pPopup->sSize.y);

	m_Text.SetScale(1.0f);

	m_Text.SetString( (pText ? pText : "") );

	m_Text.SetFont(pFont,pPopup->nFontSize);

	if( !m_bColorOverride )
		m_Text.SetColors(pPopup->argbTextColor,pPopup->argbTextColor,pPopup->argbTextColor);

	m_bColorOverride = LTFALSE;
	
	m_Text.SetFixedWidth(pPopup->nTextWidth);

	SetScale(g_pInterfaceResMgr->GetFontRatio());

	m_bVisible = LTTRUE;
}