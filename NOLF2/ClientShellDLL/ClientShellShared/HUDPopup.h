// ----------------------------------------------------------------------- //
//
// MODULE  : HUDPopup.h
//
// PURPOSE : Definition of CHUDPopup to display Popups
//
// (c) 2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#ifndef __HUD_POPUP_H
#define __HUD_POPUP_H

//
// Includes...
//
	
	#include "HUDItem.h"
	#include "LTGuiMgr.h"


class CHUDPopup : public CHUDItem
{
public:
	CHUDPopup();
	

    virtual LTBOOL      Init();
	virtual void		Term();

    virtual void        Render();
    virtual void        Update();

	//hide or show 
	void	Show( uint8 nPopupID, const char *pText );

	//close window and tell server
	void	Hide();

	// Lays the sheet out at ONE scale for x, y and the font, centred on the
	// real screen (GitHub issue #18 -- see HUDPopup.cpp). Update() passes
	// GetFontRatio().
	void	SetScale(float fScale);
	void	SetTextColor( uint8 nR, uint8 nG, uint8 nB, uint8 nA )
	{
		m_bColorOverride = LTTRUE;
		m_Text.SetColors( SET_ARGB(nA,nR,nG,nB), SET_ARGB(nA,nR,nG,nB), SET_ARGB(nA,nR,nG,nB) );
	}

	LTBOOL	IsVisible() {return m_bVisible;}

protected:

    
	float		m_fScale;
	LTBOOL		m_bVisible;
	LTBOOL		m_bColorOverride;

	// What SetScale needs to lay the sheet out again on its own, and the
	// screen ratios it was last laid out for (Update re-lays it on a change).
	LTIntPt		m_SheetSize;
	LTIntPt		m_TextOffset;
	float		m_fLaidOutX;
	float		m_fLaidOutY;


	CLTGUIFrame			m_Frame;
	CLTGUITextCtrl		m_Text;

};

#endif // __HUD_POPUP_H