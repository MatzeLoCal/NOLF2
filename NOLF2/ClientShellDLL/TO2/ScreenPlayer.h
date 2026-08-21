// ----------------------------------------------------------------------- //
//
// MODULE  : ScreenPlayer.h
//
// PURPOSE : Interface screen for player setup
//
// (c) 2002 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#ifndef _SCREEN_PLAYER_H_
#define _SCREEN_PLAYER_H_

#include "BaseScreen.h"
#include "LayoutMgr.h"

class CScreenPlayer : public CBaseScreen
{
public:
	CScreenPlayer();
	virtual ~CScreenPlayer();


	// Build the screen
    LTBOOL   Build();
	void	Term();

    void    OnFocus(LTBOOL bFocus);

	virtual LTBOOL OnLeft();
	virtual LTBOOL OnRight();
	virtual LTBOOL OnUp();
	virtual LTBOOL OnDown();
    virtual LTBOOL OnMouseMove(int x, int y);
    LTBOOL   OnLButtonUp(int x, int y);
    LTBOOL   OnRButtonUp(int x, int y);


	void	NextModel();
	void	PrevModel();

protected:
	// ⚠️⚠️ uintptr_t, NOT uint32 — dwParam1 IS A char* AND THIS IS LP64.
	//
	// The message-box edit path hands the typed string down as a pointer:
	//   CMessageBox::Close   m_pData = (void*)m_pEdit->GetText()
	//     -> EditNameCallBack(LTBOOL, void* pData)
	//       -> SendCommand(CMD_OK, (uintptr_t)pData, CMD_EDIT_NAME)
	//         -> CScreenPlayer::OnCommand(uint32, uintptr_t, uintptr_t)
	//           -> HandleCallback(...)          <- last hop
	//             -> char* pName = (char*)dwParam1;
	// Every hop above was already widened; while THIS one was still uint32 the
	// pointer lost its top 32 bits here and the dereference segfaulted on a
	// plausible-looking address (0x61aa3500 = the low half of a real heap
	// pointer). Entering a multiplayer player name crashed the game every time.
	// The same signature exists on ScreenHost/Team/Multi/Join and all of them
	// cast dwParam1 to char* — name, password, scmd password, port, bandwidth,
	// CD key. Do not narrow any of them back.
	void	HandleCallback(uintptr_t dwParam1, uintptr_t dwParam2);

	void	UpdateBandwidth();

	void	UpdateChar();

    uint32  OnCommand(uint32 dwCommand, uintptr_t dwParam1, uintptr_t dwParam2);

	CLTGUIColumnCtrl*	m_pName;
	CLTGUITextCtrl*		m_pModel;
	CLTGUITextCtrl*		m_pSkills;
	CLTGUIButton*		m_pLeft;
	CLTGUIButton*		m_pRight;

	int			m_nCurrentModel;
	std::string m_sPlayerName;

	CLTGUICycleCtrl*	m_pBandwidthCycle;
	CLTGUIColumnCtrl*	m_pBandwidth;

	uint8				m_nBandwidth;
	CString				m_sBandwidth;

};


#endif // _SCREEN_PLAYER_H_