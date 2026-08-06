// ----------------------------------------------------------------------- //
//
// MODULE  : LTGUICommandHandler.h
//
// PURPOSE : Base clase for objects that receive messages from controls
//
// (c) 2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#if !defined(_LTGUICOMMANDHANDLER_H_)
#define _LTGUICOMMANDHANDLER_H_

class CLTGUICommandHandler
{
public:
	CLTGUICommandHandler();
	virtual ~CLTGUICommandHandler();

    // ⚠️ nParam1/nParam2 are uintptr_t, NOT uint32. Half the game's message-box
    // callbacks hand the edited STRING through here as a pointer
    // (ScreenSave's EditCallBack, ScreenProfile, ScreenHost, ScreenJoin,
    // ScreenMulti's CD key, ...). On LP64 a uint32 truncated every one of them
    // to its low half, and the receiver's `(char*)dwParam1` then produced an
    // address with the top 32 bits zeroed — `strlen` on that is the
    // save-game crash (§51). Do NOT narrow these back.
    uint32  SendCommand(uint32 nCommand, uintptr_t nParam1, uintptr_t nParam2)
	{
		return OnCommand(nCommand, nParam1, nParam2);
	}

protected:
    virtual uint32 OnCommand(uint32 nCommand, uintptr_t nParam1, uintptr_t nParam2)
	{
		return 0;
	}
};

#endif // _LTGUICOMMANDHANDLER_H_