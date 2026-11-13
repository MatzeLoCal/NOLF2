// ----------------------------------------------------------------------- //
//
// MODULE  : Trigger.cpp
//
// PURPOSE : Trigger - Implementation
//
// CREATED : 10/6/97
//
// (c) 1997-2002 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "Trigger.h"
#include "iltserver.h"
#include "MsgIds.h"
#include "PlayerObj.h"
#include "gameservershell.h"
#include "SoundMgr.h"
#include "ObjectMsgs.h"
#include "ParsedMsg.h"
#include "ServerSoundMgr.h"
#include <stdio.h>
#include "AIUtils.h"

LINKFROM_MODULE( Trigger );

#pragma force_active on
BEGIN_CLASS(Trigger)
	ADD_VECTORPROP_VAL_FLAG(Dims, 16.0f, 16.0f, 16.0f, PF_DIMS)
	ADD_LONGINTPROP(NumberOfActivations, 1)
	ADD_REALPROP(SendDelay, 0.0f)
	ADD_REALPROP(TriggerDelay, 0.0)

	PROP_DEFINEGROUP(Commands, PF_GROUP(1))
		ADD_STRINGPROP_FLAG(Command1, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command2, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command3, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command4, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command5, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command6, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command7, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command8, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command9, "", PF_GROUP(1) | PF_NOTIFYCHANGE)
		ADD_STRINGPROP_FLAG(Command10, "", PF_GROUP(1) | PF_NOTIFYCHANGE)

	ADD_BOOLPROP(TriggerTouch, 0)
	ADD_STRINGPROP_FLAG(CommandTouch, "", PF_NOTIFYCHANGE)
	ADD_BOOLPROP(PlayerTriggerable, 1)
	ADD_STRINGPROP_FLAG(PlayerKey, "<None>", PF_STATICLIST)
	ADD_BOOLPROP(AITriggerable, 0)
	ADD_STRINGPROP(AITriggerName, "")
	ADD_BOOLPROP(BodyTriggerable, 0)
	ADD_STRINGPROP(BodyTriggerName, "")
    ADD_BOOLPROP(WeightedTrigger, LTFALSE)
	ADD_REALPROP(Message1Weight, .5)
    ADD_BOOLPROP(TimedTrigger, LTFALSE)
	ADD_REALPROP(MinTriggerTime, 0.0f)
	ADD_REALPROP(MaxTriggerTime, 10.0f)
	ADD_LONGINTPROP(ActivationCount, 1)
	ADD_BOOLPROP(Locked, 0)
	ADD_STRINGPROP_FLAG(ActivationSound, "", PF_FILENAME)
	ADD_REALPROP_FLAG(SoundRadius, 200.0f, PF_RADIUS)
	ADD_STRINGPROP_FLAG(AttachToObject, "", PF_OBJECTLINK)
	ADD_REALPROP_FLAG(HUDLookAtDist, -1.0f, PF_RADIUS)
	ADD_REALPROP_FLAG(HUDAlwaysOnDist, -1.0f, PF_RADIUS)
	ADD_STRINGPROP_FLAG( TriggerType, "<none>", PF_STATICLIST )
END_CLASS_DEFAULT_FLAGS_PLUGIN(Trigger, GameBase, NULL, NULL, 0, CTriggerPlugin)
#pragma force_active off

//
// Register the class with the command mgr plugin and add any messages to the class
//

CMDMGR_BEGIN_REGISTER_CLASS( Trigger )

	CMDMGR_ADD_MSG( LOCK, 1, NULL, "LOCK" )
	CMDMGR_ADD_MSG( UNLOCK, 1, NULL, "UNLOCK" )
	CMDMGR_ADD_MSG( TRIGGER, 1, NULL, "TRIGGER" )

CMDMGR_END_REGISTER_CLASS( Trigger, GameBase )

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTriggerPlugin::PreHook_EditStringList()
//
//	PURPOSE:	Fill out the list of key types
//
// ----------------------------------------------------------------------- //

LTRESULT CTriggerPlugin::PreHook_EditStringList(
	const char* szRezPath,
	const char* szPropName,
	char** aszStrings,
    uint32* pcStrings,
    const uint32 cMaxStrings,
    const uint32 cMaxStringLength)
{
	if( !cMaxStrings || !cMaxStringLength )
		return LT_ERROR;

	if (stricmp(szPropName, "PlayerKey") == 0)
	{
		// Clear the first entry, so you can always clear out this field
		strcpy(aszStrings[0], "<None>");

		// Hand off the rest to the keymgr
		LTBOOL bResult = m_KeyMgrPlugin.PopulateStringList(aszStrings + 1, pcStrings, cMaxStrings - 1, cMaxStringLength);
		++(*pcStrings);
		return (bResult) ? LT_OK : LT_ERROR;
	}
	else if( stricmp( szPropName, "TriggerType" ) == 0 )
	{
		if( m_TriggerTypeMgrPlugin.PreHook_EditStringList( szRezPath,
														   szPropName,
														   aszStrings,
														   pcStrings, 
														   cMaxStrings, 
														   cMaxStringLength ) == LT_OK )
		{
			return LT_OK;
		}
	}
	else 
		return LT_OK;

	return LT_UNSUPPORTED;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CTriggerPlugin::PreHook_PropChanged()
//
//	PURPOSE:	Check our commands
//
// ----------------------------------------------------------------------- //

LTRESULT CTriggerPlugin::PreHook_PropChanged( const char *szObjName,
											  const char *szPropName,
											  const int nPropType,
											  const GenericProp &gpPropValue,
											  ILTPreInterface *pInterface,
											  const char *szModifiers )
{
	// Just send down to the command mgr plugin...

	if( m_CommandMgrPlugin.PreHook_PropChanged( szObjName,
												szPropName,
												nPropType,
												gpPropValue,
												pInterface,
												szModifiers ) == LT_OK )
	{
		return LT_OK;
	}

	return LT_UNSUPPORTED;
}


// Static global variables...

static char *g_szLock    = "LOCK";
static char *g_szUnLock  = "UNLOCK";
static char *g_szTrigger = "TRIGGER";

#define UPDATE_DELTA					0.1f
#define TRIGGER_DEACTIVATION_TIME		0.001f

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::Trigger()
//
//	PURPOSE:	Initialize object
//
// ----------------------------------------------------------------------- //

Trigger::Trigger() : GameBase()
{
	m_vDims.Init(5.0f, 5.0f, 5.0f);
	m_fTriggerDelay	= 0.0f;

	for (int i=0; i < MAX_NUM_COMMANDS; i++)
	{
        m_hstrCommand[i] = LTNULL;
	}

    m_bTriggerTouch         = LTFALSE;
    m_bTouchNotifyActivation= LTFALSE;
    m_hTouchObject          = LTNULL;
    m_hstrCommandTouch      = LTNULL;
    m_hstrActivationSound   = LTNULL;
    m_hstrAttachToObject    = LTNULL;
    m_bAttached             = LTFALSE;
	m_fSoundRadius			= 200.0f;
    m_bActive               = LTTRUE;
    m_bPlayerTriggerable    = LTTRUE;
	m_nPlayerKeyID			= KEY_INVALID_ID;
    m_bAITriggerable        = LTFALSE;
    m_bLocked               = LTFALSE;
	m_bPendingMsgTrigger	= LTFALSE;
    m_hstrAIName            = LTNULL;
    m_bBodyTriggerable      = LTFALSE;
    m_hstrBodyName          = LTNULL;
    m_bDelayingActivate     = LTFALSE;
	m_fStartDelayTime		= 0.0f;
	m_fSendDelay			= 0.0f;

	m_fLastTouchTime		= 0.0f;

	m_nCurrentActivation	= 0;
	m_nActivationCount		= 1;

	m_nNumActivations		= 1;
	m_nNumTimesActivated	= 0;

    m_bWeightedTrigger      = LTFALSE;
	m_fMessage1Weight		= .5f;

    m_bTimedTrigger         = LTFALSE;
	m_fMinTriggerTime		= 0.0f;
	m_fMaxTriggerTime		= 1.0f;

	m_fNextTriggerTime		= 0.0f;

	m_dwFlags				= (FLAG_TOUCH_NOTIFY | FLAG_GOTHRUWORLD);

	m_bSendTriggerFXMsg		= false;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::~Trigger()
//
//	PURPOSE:	Deallocate object
//
// ----------------------------------------------------------------------- //

Trigger::~Trigger()
{
	for (int i=0; i < MAX_NUM_COMMANDS; i++)
	{
		if (m_hstrCommand[i])
		{
            g_pLTServer->FreeString(m_hstrCommand[i]);
		}
	}

	if (m_hstrCommandTouch)
	{
        g_pLTServer->FreeString(m_hstrCommandTouch);
	}

	if (m_hstrActivationSound)
	{
        g_pLTServer->FreeString(m_hstrActivationSound);
	}

	if (m_hstrAttachToObject)
	{
        g_pLTServer->FreeString(m_hstrAttachToObject);
	}

	if (m_hstrAIName)
	{
        g_pLTServer->FreeString(m_hstrAIName);
	}

	if (m_hstrBodyName)
	{
        g_pLTServer->FreeString(m_hstrBodyName);
	}
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::EngineMessageFn
//
//	PURPOSE:	Handle engine messages
//
// ----------------------------------------------------------------------- //

uint32 Trigger::EngineMessageFn(uint32 messageID, void *pData, LTFLOAT fData)
{
	switch(messageID)
	{
		case MID_UPDATE:
		{
			if (!Update())
			{
                g_pLTServer->RemoveObject(m_hObject);
			}
		}
		break;

		case MID_TOUCHNOTIFY:
		{
			ObjectTouch((HOBJECT)pData);
		}
		break;

		case MID_PRECREATE:
		{
			ObjectCreateStruct *pStruct = (ObjectCreateStruct *)pData;
			if (!pStruct) return 0;

			if (fData == PRECREATE_WORLDFILE)
			{
				ReadProp(pStruct);
			}

			pStruct->m_UserData = USRFLG_IGNORE_PROJECTILES;
		}
		break;

		case MID_INITIALUPDATE:
		{
			if (fData != INITIALUPDATE_SAVEGAME)
			{
				InitialUpdate();
			}
		}
		break;

		case MID_SAVEOBJECT:
		{
            Save((ILTMessage_Write*)pData, (uint32)fData);
		}
		break;

		case MID_LOADOBJECT:
		{
            Load((ILTMessage_Read*)pData, (uint32)fData);
		}
		break;

		case MID_PARENTATTACHMENTREMOVED :
		{
			// Go away if our parent is removed...

            g_pLTServer->RemoveObject(m_hObject);
		}
		break;

		default : break;
	}


	return GameBase::EngineMessageFn(messageID, pData, fData);
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::OnTrigger
//
//	PURPOSE:	Handle trigger messages
//
// ----------------------------------------------------------------------- //

// ★★ LT_TRACE_TRIGGER -- THE LEVEL'S LOGIC CHAIN, AS IT ACTUALLY RUNS.
//
// A level like c08s04 ("The Getaway") is one long single-threaded chain of
// Triggers unlocking each other; when a link is lost the level simply stops
// being finishable and NOTHING is reported. The lines that matter most are the
// REFUSED ones -- a `trigger` message arriving at a LOCKED trigger, or at one
// that has used up its NumberOfActivations, is discarded in silence by design
// (see DoTrigger / Activate below), and that silence is the whole difficulty.
bool g_bTraceTrigger = (getenv("LT_TRACE_TRIGGER") != LTNULL);

// ⚠️ stderr, NOT g_pLTServer->CPrint. CPrint goes to dsi_PrintToConsole, which
// on this port mirrors to stdout ONLY when LT_TRACE_CONSOLE=1 is ALSO set (and
// otherwise reaches nothing at all on a client, since there is no dedicated
// server app handler to receive it). A diagnostic that needs a second switch
// to appear is a diagnostic that reports an empty log as "nothing happened" --
// the exact trap dsi_PrintToConsole's own comment describes. Every other probe
// in this port writes to stderr ([wobj], [in], [df], [seemask]); so does this.
void TrigTrace(const char *pMsg, ...)
{
	va_list marker;
	va_start(marker, pMsg);
	vfprintf(stderr, pMsg, marker);
	va_end(marker);
	fputc('\n', stderr);
}

static const char *TrigName(HOBJECT hObj)
{
	static char s_szName[64];
	s_szName[0] = 0;
	if (hObj && g_pLTServer)
		g_pLTServer->GetObjectName(hObj, s_szName, sizeof(s_szName));
	return s_szName[0] ? s_szName : "?";
}

bool Trigger::OnTrigger(HOBJECT hSender, const CParsedMsg &cMsg)
{
	static CParsedMsg::CToken s_cTok_Trigger(g_szTrigger);
	static CParsedMsg::CToken s_cTok_Lock(g_szLock);
	static CParsedMsg::CToken s_cTok_Unlock(g_szUnLock);

	// See if we should trigger the trigger...

	if (cMsg.GetArg(0) == s_cTok_Trigger)
	{
		if (g_bTraceTrigger)
			TrigTrace("[trig] MSG trigger -> %s (from %s) locked=%d used=%d/%d",
				TrigName(m_hObject), TrigName(hSender), (int)m_bLocked,
				(int)m_nNumTimesActivated, (int)m_nNumActivations);

		// ★★ LATCH A REFUSED `trigger` MESSAGE INSTEAD OF LOSING IT.
		//
		// ⚠️ WHY: a level's logic chain is single-threaded, and the thing that
		// sends such a message is usually a ONE-SHOT trigger. c08s04 ("The
		// Getaway") is the case that found this. Pierre is escorted node to
		// node: each pierre_gotoN unlocks p_triggerN on arrival, and the
		// player walking into continuetriggerN sends `msg p_triggerN trigger`
		// to make him run on. continuetrigger1..7 are NOT locked and have
		// NumberOfActivations=1 -- so a player who reaches continuetrigger7
		// BEFORE Pierre reaches pierre_goto7 spends its single activation on a
		// message that p_trigger7, still locked, discards. Nothing can ever
		// send it again: Pierre stops at node 7 for good, pierre_goto8's
		// command never runs, continuetrigger8 stays locked, and the three
		// mimes it spawns (Spawner5/6/06) never appear. The level cannot be
		// finished, and whether it happens depends purely on whether the
		// player outran the AI -- which is why it reproduces every time for
		// one player and never for another.
		//
		// Latching only applies to an explicit MESSAGE, never to a touch and
		// never to a timed trigger's self-fire (both reach DoTrigger directly,
		// not through here), so a locked trigger still swallows a player who
		// walks into it early. LT_NO_TRIGGER_LATCH=1 restores the original
		// lose-it behaviour for an A/B.
		if (m_bLocked && !getenv("LT_NO_TRIGGER_LATCH"))
		{
			m_bPendingMsgTrigger = LTTRUE;
			if (g_bTraceTrigger)
				TrigTrace("[trig] LATCHED %s: locked now, will fire on unlock",
					TrigName(m_hObject));
		}

        DoTrigger(hSender, LTFALSE);
	}
	else if (cMsg.GetArg(0) == s_cTok_Lock)  // See if we should lock the trigger...
	{
		if (g_bTraceTrigger)
			TrigTrace("[trig] LOCK   %s (from %s)",
				TrigName(m_hObject), TrigName(hSender));
        m_bLocked = LTTRUE;
		SendLockedMsg();
	}
	else if (cMsg.GetArg(0) == s_cTok_Unlock) // See if we should unlock the trigger...
	{
		if (g_bTraceTrigger)
			TrigTrace("[trig] UNLOCK %s (from %s) used=%d/%d",
				TrigName(m_hObject), TrigName(hSender),
				(int)m_nNumTimesActivated, (int)m_nNumActivations);
        m_bLocked = LTFALSE;
		SendLockedMsg();

		// Replay a message that arrived while we were locked.
		if (m_bPendingMsgTrigger)
		{
			m_bPendingMsgTrigger = LTFALSE;
			if (g_bTraceTrigger)
				TrigTrace("[trig] REPLAY %s: firing the latched trigger",
					TrigName(m_hObject));
			DoTrigger(hSender, LTFALSE);
		}
	}
	else
		return GameBase::OnTrigger(hSender, cMsg);

	return true;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::ReadProp
//
//	PURPOSE:	Set property value
//
// ----------------------------------------------------------------------- //

LTBOOL Trigger::ReadProp(ObjectCreateStruct *pData)
{
    if (!pData) return LTFALSE;

	const int nMaxFilesize = 500;
	char buf[nMaxFilesize + 1];
	buf[0] = '\0';

	char propName[50];
	for (int i=0; i < MAX_NUM_COMMANDS; i++)
	{
		sprintf(propName, "Command%d", i+1);
		buf[0] = '\0';
        if (g_pLTServer->GetPropString(propName, buf, nMaxFilesize) == LT_OK)
		{
			if (buf[0] && strlen(buf))
			{
                m_hstrCommand[i] = g_pLTServer->CreateString(buf);
			}
		}
	}

	bool bFlag;
    g_pLTServer->GetPropBool("TriggerTouch", &bFlag);
	m_bTriggerTouch = (bFlag ? LTTRUE : LTFALSE);

	buf[0] = '\0';
    g_pLTServer->GetPropString("CommandTouch", buf, nMaxFilesize);
    if (buf[0] && strlen(buf)) m_hstrCommandTouch = g_pLTServer->CreateString(buf);

	buf[0] = '\0';
    g_pLTServer->GetPropString("ActivationSound", buf, nMaxFilesize);
    if (buf[0] && strlen(buf)) m_hstrActivationSound = g_pLTServer->CreateString(buf);

	buf[0] = '\0';
    g_pLTServer->GetPropString("AttachToObject", buf, nMaxFilesize);
    if (buf[0] && strlen(buf)) m_hstrAttachToObject = g_pLTServer->CreateString(buf);

    g_pLTServer->GetPropVector("Dims", &m_vDims);
    g_pLTServer->GetPropReal("TriggerDelay", &m_fTriggerDelay);
    g_pLTServer->GetPropReal("SendDelay", &m_fSendDelay);
    g_pLTServer->GetPropReal("SoundRadius", &m_fSoundRadius);
    g_pLTServer->GetPropBool("PlayerTriggerable", &bFlag);
	m_bPlayerTriggerable = (bFlag ? LTTRUE : LTFALSE);
    g_pLTServer->GetPropBool("AITriggerable", &bFlag);
	m_bAITriggerable = (bFlag ? LTTRUE : LTFALSE);
    g_pLTServer->GetPropBool("Locked", &bFlag);
	m_bLocked = (bFlag ? LTTRUE : LTFALSE);

    g_pLTServer->GetPropReal("SoundRadius", &m_fSoundRadius);

	buf[0] = '\0';
    g_pLTServer->GetPropString("AITriggerName", buf, nMaxFilesize);
    if (buf[0] && strlen(buf)) m_hstrAIName = g_pLTServer->CreateString(buf);

    g_pLTServer->GetPropBool("BodyTriggerable", &bFlag);
	m_bBodyTriggerable = (bFlag ? LTTRUE : LTFALSE);
	buf[0] = '\0';
    g_pLTServer->GetPropString("BodyTriggerName", buf, nMaxFilesize);
    if (buf[0] && strlen(buf)) m_hstrBodyName = g_pLTServer->CreateString(buf);

	int32 nLongVal;
    if(g_pLTServer->GetPropLongInt("ActivationCount", &nLongVal) == LT_OK)
	{
		m_nActivationCount = nLongVal;
	}

    if(g_pLTServer->GetPropLongInt("NumberOfActivations", &nLongVal) == LT_OK)
	{
		m_nNumActivations = nLongVal;
	}

    g_pLTServer->GetPropBool("WeightedTrigger", &bFlag);
	m_bWeightedTrigger = (bFlag ? LTTRUE : LTFALSE);
    g_pLTServer->GetPropReal("Message1Weight", &m_fMessage1Weight);

    g_pLTServer->GetPropBool("TimedTrigger", &bFlag);
	m_bTimedTrigger = (bFlag ? LTTRUE : LTFALSE);
    g_pLTServer->GetPropReal("MinTriggerTime", &m_fMinTriggerTime);
    g_pLTServer->GetPropReal("MaxTriggerTime", &m_fMaxTriggerTime);

    m_fNextTriggerTime = g_pLTServer->GetTime() + m_fMinTriggerTime;

	m_nPlayerKeyID = KEY_INVALID_ID;
	buf[0] = '\0';
    g_pLTServer->GetPropString("PlayerKey", buf, nMaxFilesize);
    if (buf[0])
	{
		KEY *pKey = g_pKeyMgr->GetKey(buf);
		if (pKey)
			m_nPlayerKeyID = pKey->nId;
	}

	g_pLTServer->GetPropReal( "HUDLookAtDist", &m_TCS.fHUDLookAtDist );
	g_pLTServer->GetPropReal( "HUDAlwaysOnDist", &m_TCS.fHUDAlwaysOnDist );

	buf[0] = '\0';
	g_pLTServer->GetPropString( "TriggerType", buf, nMaxFilesize );
	if( buf[0] )
	{
		TRIGGERTYPE	*pType = g_pTriggerTypeMgr->GetTriggerType( buf );
		if( pType )
		{
			m_TCS.nTriggerTypeId = pType->nId;
		}
	}

    return LTTRUE;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::ObjectTouch
//
//	PURPOSE:	Handle object touch
//
// ----------------------------------------------------------------------- //

void Trigger::ObjectTouch(HOBJECT hObj)
{
	// Only AI and players and bodies can trigger things...

	bool bIsPlayer	= (IsPlayer(hObj) ? true : false);
	bool bIsAI		= (IsAI(hObj) ? true : false);
	bool bIsBody	= (IsBody(hObj) ? true : false);

    if (!bIsAI && !bIsPlayer && !bIsBody)
	{
		return;
	}


	// If we're AI, make sure we can activate this trigger...

	if (m_bAITriggerable)
	{
        if ( bIsAI )
		{
			if (m_hstrAIName) // See if only a specific AI can trigger it...
			{
                const char* pAIName  = g_pLTServer->GetStringData(m_hstrAIName);
                const char* pObjName = GetObjectName(hObj);

				if (pAIName && pObjName)
				{
					if ( stricmp(pAIName, pObjName) != 0 )
					{
						return;
					}
				}
			}
		}
		
		// Check for knocked out AIs carried by players too...

		if (bIsPlayer && m_hstrAIName)
		{
            CPlayerObj* pPlayer = (CPlayerObj*)g_pLTServer->HandleToObject(hObj);
			if (pPlayer)
			{
				HOBJECT hBody = pPlayer->GetCarriedObject();
				if (hBody)
				{
					const char* pBodyName  = g_pLTServer->GetStringData(m_hstrAIName);
					const char* pObjName = GetObjectName(hBody);

					if (pBodyName && pObjName)
					{
						if ( stricmp(pBodyName, pObjName) == 0 )
						{
							hObj = hBody;
						}
					}
				}
			}
		}
	}
	else  // Not AI triggerable
	{
        if ( bIsAI )
		{
			return;
		}
	}

	// If we're Body, make sure we can activate this trigger...

	if (m_bBodyTriggerable)
	{
        if ( bIsBody )
		{
			if (m_hstrBodyName) // See if only a specific Body can trigger it...
			{
                const char* pBodyName  = g_pLTServer->GetStringData(m_hstrBodyName);
                const char* pObjName = GetObjectName(hObj);

				if (pBodyName && pObjName)
				{
					if ( stricmp(pBodyName, pObjName) != 0 )
					{
						return;
					}
				}
			}
		}

		//check for bodies carried by players too
		if (bIsPlayer && m_hstrBodyName)
		{
            CPlayerObj* pPlayer = (CPlayerObj*)g_pLTServer->HandleToObject(hObj);
			if (pPlayer)
			{
				HOBJECT hBody = pPlayer->GetCarriedObject();
				if (hBody)
				{
					const char* pBodyName  = g_pLTServer->GetStringData(m_hstrBodyName);
					const char* pObjName = GetObjectName(hBody);

					if (pBodyName && pObjName)
					{
						if ( stricmp(pBodyName, pObjName) == 0 )
						{
							hObj = hBody;
						}
					}
				}
			}
		}

	}
	else  // Not Body triggerable
	{
        if ( bIsBody )
		{
			return;
		}
	}


	// If we're the player, make sure we can activate this trigger...
	if (m_bPlayerTriggerable)
	{
		if (bIsPlayer)
		{
            CPlayerObj* pPlayer = (CPlayerObj*)g_pLTServer->HandleToObject(hObj);
			if (!pPlayer) return;

			// Check to make sure the player has this key
			if (m_nPlayerKeyID != KEY_INVALID_ID)
			{
				uint8 nDummy;
				if (!pPlayer->GetKeyList()->Have(m_nPlayerKeyID, nDummy))
					return;
			}
		}

	}
	else //not player triggerable
	{
		// Note we check IsPlayer() here instead of bIsPlayer because hObj may have
		// changed above...
		if ( IsPlayer(hObj) )
		{
			return;
		}
	}


    DoTrigger(hObj, LTTRUE);

}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::DoTrigger
//
//	PURPOSE:	Determine if we can be triggered, and if so do it...
//
// ----------------------------------------------------------------------- //

void Trigger::DoTrigger(HOBJECT hObj, LTBOOL bTouchNotify)
{

	// Okay ready to trigger.  Make sure we've waited long enough before triggering...
    LTFLOAT fTime = g_pLTServer->GetTime();

	if (fTime < m_fLastTouchTime + m_fTriggerDelay)
	{
		return;	
	}
	m_fLastTouchTime = fTime;

	m_bTouchNotifyActivation = bTouchNotify;
	m_hTouchObject = hObj;

	if (m_bActive)
	{
		if (!m_bLocked)
		{
			RequestActivate();
		}
		else if (g_bTraceTrigger)
		{
			// ⚠️ THE SILENT LOSS. The message is gone -- unlocking later does
			// not replay it, and if whatever sent it was itself a one-shot
			// trigger, nothing will ever send it again.
			TrigTrace("[trig] REFUSED %s: LOCKED (touch=%d from %s)",
				TrigName(m_hObject), (int)bTouchNotify, TrigName(hObj));
		}
	}
	else if (g_bTraceTrigger)
	{
		TrigTrace("[trig] REFUSED %s: not active yet", TrigName(m_hObject));
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::InitialUpdate
//
//	PURPOSE:	Initial update
//
// ----------------------------------------------------------------------- //

LTBOOL Trigger::InitialUpdate()
{
	g_pPhysicsLT->SetObjectDims(m_hObject, &m_vDims, 0);
	g_pCommonLT->SetObjectFlags(m_hObject, OFT_Flags, m_dwFlags, FLAGMASK_ALL);
	g_pCommonLT->SetObjectFlags(m_hObject, OFT_User, USRFLG_IGNORE_PROJECTILES, FLAGMASK_ALL);

	// If I'm not a timed trigger, my object touch notification
	// will trigger new updates until then, I don't care...

	if (m_bTimedTrigger || m_hstrAttachToObject)
	{
        SetNextUpdate(UPDATE_DELTA);
	}
	else
	{
        SetNextUpdate(UPDATE_NEVER);
	}

	// Create the specialfx message...

	if( m_TCS.fHUDLookAtDist > 0.0f || m_TCS.fHUDAlwaysOnDist > 0.0f )
	{
		// Only send the message if we need to...
		
		m_bSendTriggerFXMsg = true;
	}

	if( m_bSendTriggerFXMsg )
	{
		g_pCommonLT->SetObjectFlags( m_hObject, OFT_Flags, FLAG_FORCECLIENTUPDATE , FLAG_FORCECLIENTUPDATE );
		CreateSpecialFX();
	}

    return LTTRUE;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::Update
//
//	PURPOSE:	Handle Update
//
// ----------------------------------------------------------------------- //

LTBOOL Trigger::Update()
{
	// Handle timed trigger...

	if (m_bTimedTrigger)
	{
        LTFLOAT fTime = g_pLTServer->GetTime();
		if (fTime > m_fNextTriggerTime)
		{
			m_fNextTriggerTime = fTime + GetRandom(m_fMinTriggerTime, m_fMaxTriggerTime);
            DoTrigger(LTNULL, LTFALSE);
		}
	}


	// Attach the trigger to the object...

	if (m_hstrAttachToObject && !m_bAttached)
	{
		AttachToObject();
        m_bAttached = LTTRUE;
	}



	if (m_bDelayingActivate)
	{
		UpdateDelayingActivate();
	}
	else
	{
        m_bActive = LTTRUE;

		// If not a timed trigger, my object touch notification will trigger
		// new updates until then, I don't care.

		if (m_bTimedTrigger)
		{
            SetNextUpdate(UPDATE_DELTA);
		}
		else
		{
            SetNextUpdate(UPDATE_NEVER);
		}
	}

    return LTTRUE;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::Unlock
//
//	PURPOSE:	Unlock the trigger and trigger it
//
// ----------------------------------------------------------------------- //

void Trigger::Unlock()
{
    m_bLocked = LTFALSE;
	RequestActivate();
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::RequestActivate
//
//	PURPOSE:	Request activation of the trigger
//
// ----------------------------------------------------------------------- //

void Trigger::RequestActivate()
{
	if (m_bActive)
	{
        m_fStartDelayTime = g_pLTServer->GetTime();

        m_bDelayingActivate = LTTRUE;
        m_bActive           = LTFALSE;
		if (m_fTriggerDelay > 0.0f)
			SetNextUpdate(UPDATE_NEXT_FRAME);
		else
			Update();
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::UpdateDelayingActivate
//
//	PURPOSE:	Update the delaying (and possibly activate) the trigger
//
// ----------------------------------------------------------------------- //

void Trigger::UpdateDelayingActivate()
{
	if (!m_bDelayingActivate) return;

    LTFLOAT fTime = g_pLTServer->GetTime();

	if (fTime >= m_fStartDelayTime + m_fSendDelay)
	{
		Activate();
        m_bDelayingActivate = LTFALSE;
        m_bActive           = LTTRUE;
	}
	else
	{
        SetNextUpdate(UPDATE_NEXT_FRAME);
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::Activate
//
//	PURPOSE:	Activate the trigger.
//
// ----------------------------------------------------------------------- //

LTBOOL Trigger::Activate()
{
	// Make us wait a bit before we can be triggered again...

	if (m_bTimedTrigger)
	{
        SetNextUpdate(UPDATE_DELTA);
 	}
	else
	{
        SetNextUpdate(m_fTriggerDelay);
	}


	// If this is a counter trigger, determine if we can activate or not...

	if (++m_nCurrentActivation < m_nActivationCount)
	{
        return LTFALSE;
	}
	else
	{
		m_nCurrentActivation = 0;
	}


	// Only allow the object to be activated the number of specified times...

	if (m_nNumActivations > 0)
	{
		if (m_nNumTimesActivated >= m_nNumActivations)
		{
			if (g_bTraceTrigger)
				TrigTrace("[trig] REFUSED %s: used up (%d/%d)",
					TrigName(m_hObject), (int)m_nNumTimesActivated,
					(int)m_nNumActivations);
            return LTFALSE;
		}

		m_nNumTimesActivated++;
		if (g_bTraceTrigger)
			TrigTrace("[trig] FIRE   %s (%d/%d)", TrigName(m_hObject),
				(int)m_nNumTimesActivated, (int)m_nNumActivations);
	}
	else if (g_bTraceTrigger)
	{
		TrigTrace("[trig] FIRE   %s (unlimited)", TrigName(m_hObject));
	}


	if (m_hstrActivationSound)
	{
        const char* pSound = g_pLTServer->GetStringData(m_hstrActivationSound);
		if (pSound && pSound[0] != '\0')
		{
			LTVector vPos;
			g_pLTServer->GetObjectPos(m_hObject, &vPos);
            g_pServerSoundMgr->PlaySoundFromPos(vPos, pSound, m_fSoundRadius, SOUNDPRIORITY_MISC_HIGH);
        }
	}

    LTBOOL bTriggerMsg1 = LTTRUE;
    LTBOOL bTriggerMsg2 = LTTRUE;

	if (m_bWeightedTrigger)
	{
        bTriggerMsg1 = (GetRandom(0.0f, 1.0f) < m_fMessage1Weight ? LTTRUE : LTFALSE);
		bTriggerMsg2 = !bTriggerMsg1;
	}

	// Loop through the commands and execute them...
	
	for (int i=0; i < MAX_NUM_COMMANDS; i++)
	{
        LTBOOL bOkayToSend = LTTRUE;

        if (i == 0 && !bTriggerMsg1) bOkayToSend = LTFALSE;
        else if (i == 1 && !bTriggerMsg2) bOkayToSend = LTFALSE;

		if (bOkayToSend && m_hstrCommand[i])
		{
			const char *pCmd = g_pLTServer->GetStringData( m_hstrCommand[i] );

			if( g_pCmdMgr->IsValidCmd( pCmd ) )
			{
				g_pCmdMgr->Process( pCmd, m_hTouchObject, m_hObject );
			}
		}
	}

	if (m_bTouchNotifyActivation && m_hTouchObject && m_bTriggerTouch && m_hstrCommandTouch)
	{
		const char *pCmd = g_pLTServer->GetStringData( m_hstrCommandTouch );

		if( pCmd && g_pCmdMgr->IsValidCmd( pCmd ))
		{
			g_pCmdMgr->Process( pCmd, m_hTouchObject, m_hTouchObject );
		}	

		m_bTouchNotifyActivation = LTFALSE;
        m_hTouchObject = LTNULL;
	}

	// Clear the toucher.
	m_hTouchObject = LTNULL;

    return LTTRUE;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::AttachToObject()
//
//	PURPOSE:	Attach the trigger to an object
//
// ----------------------------------------------------------------------- //

void Trigger::AttachToObject()
{
	if (!m_hstrAttachToObject) return;

    const char* pObjName = g_pLTServer->GetStringData(m_hstrAttachToObject);
	if (!pObjName) return;

	// Find object to attach to...

	ObjArray <HOBJECT, MAX_OBJECT_ARRAY_SIZE> objArray;
    g_pLTServer->FindNamedObjects(pObjName, objArray);

	if (!objArray.NumObjects()) return;

	HOBJECT hObj = objArray.GetObject(0);

	if (!hObj) return;

    LTVector vOffset(0, 0, 0);

    LTRotation rOffset;

	HATTACHMENT hAttachment;
    g_pLTServer->CreateAttachment(hObj, m_hObject, LTNULL, &vOffset, &rOffset, &hAttachment);
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::Save
//
//	PURPOSE:	Save the object
//
// ----------------------------------------------------------------------- //

void Trigger::Save(ILTMessage_Write *pMsg, uint32 dwSaveFlags)
{
	if (!pMsg) return;

	m_TCS.Write( pMsg );

    SAVE_HOBJECT(m_hTouchObject);
    SAVE_BOOL(m_bAttached);
    SAVE_BOOL(m_bActive);
    SAVE_BOOL(m_bTriggerTouch);
    SAVE_BOOL(m_bTouchNotifyActivation);
    SAVE_BOOL(m_bPlayerTriggerable);
	SAVE_DWORD(m_nPlayerKeyID);
    SAVE_BOOL(m_bAITriggerable);
    SAVE_BOOL(m_bBodyTriggerable);
    SAVE_BOOL(m_bLocked);
    SAVE_BOOL(m_bDelayingActivate);
    SAVE_BOOL(m_bWeightedTrigger);
    SAVE_BOOL(m_bTimedTrigger);

    SAVE_TIME(m_fStartDelayTime);
    SAVE_FLOAT(m_fSendDelay);
    SAVE_TIME(m_fLastTouchTime);
    SAVE_FLOAT(m_fMessage1Weight);
    SAVE_FLOAT(m_fMinTriggerTime);
    SAVE_FLOAT(m_fMaxTriggerTime);
    SAVE_TIME(m_fNextTriggerTime);
    SAVE_FLOAT(m_fTriggerDelay);
    SAVE_FLOAT(m_fSoundRadius);
    SAVE_INT(m_nNumActivations);

    SAVE_DWORD(m_nNumTimesActivated);
    SAVE_DWORD(m_nActivationCount);
    SAVE_DWORD(m_nCurrentActivation);


    SAVE_HSTRING(m_hstrAIName);
    SAVE_HSTRING(m_hstrBodyName);
    SAVE_HSTRING(m_hstrCommandTouch);
    SAVE_HSTRING(m_hstrActivationSound);

	SAVE_bool(m_bSendTriggerFXMsg);

	for (int i=0; i < MAX_NUM_COMMANDS; i++)
	{
        SAVE_HSTRING(m_hstrCommand[i]);
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::Load
//
//	PURPOSE:	Load the object
//
// ----------------------------------------------------------------------- //

void Trigger::Load(ILTMessage_Read *pMsg, uint32 dwLoadFlags)
{
	if (!pMsg) return;

	m_TCS.Read( pMsg );

    LOAD_HOBJECT(m_hTouchObject);

    LOAD_BOOL(m_bAttached);
    LOAD_BOOL(m_bActive);
    LOAD_BOOL(m_bTriggerTouch);
    LOAD_BOOL(m_bTouchNotifyActivation);
    LOAD_BOOL(m_bPlayerTriggerable);
	LOAD_DWORD(m_nPlayerKeyID);
    LOAD_BOOL(m_bAITriggerable);
    LOAD_BOOL(m_bBodyTriggerable);
    LOAD_BOOL(m_bLocked);
    LOAD_BOOL(m_bDelayingActivate);
    LOAD_BOOL(m_bWeightedTrigger);
    LOAD_BOOL(m_bTimedTrigger);

    LOAD_TIME(m_fStartDelayTime);
    LOAD_FLOAT(m_fSendDelay);
    LOAD_TIME(m_fLastTouchTime);
    LOAD_FLOAT(m_fMessage1Weight);
    LOAD_FLOAT(m_fMinTriggerTime);
    LOAD_FLOAT(m_fMaxTriggerTime);
    LOAD_TIME(m_fNextTriggerTime);
    LOAD_FLOAT(m_fTriggerDelay);
    LOAD_FLOAT(m_fSoundRadius);
    LOAD_INT(m_nNumActivations);

    LOAD_DWORD(m_nNumTimesActivated);
    LOAD_DWORD(m_nActivationCount);
    LOAD_DWORD(m_nCurrentActivation);

    LOAD_HSTRING(m_hstrAIName);
    LOAD_HSTRING(m_hstrBodyName);
    LOAD_HSTRING(m_hstrCommandTouch);
    LOAD_HSTRING(m_hstrActivationSound);

	LOAD_bool(m_bSendTriggerFXMsg);

	for (int i=0; i < MAX_NUM_COMMANDS; i++)
	{
        LOAD_HSTRING(m_hstrCommand[i]);
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::CreateSpecialFX()
//
//	PURPOSE:	Add client-side special fx
//
// ----------------------------------------------------------------------- //

void Trigger::CreateSpecialFX( LTBOOL bUpdateClients /* =LTFALSE */ )
{
	m_TCS.bLocked			= !!m_bLocked;
	m_TCS.vDims				= m_vDims;

	if( !m_bSendTriggerFXMsg )
		return;
	
	{
		CAutoMessage cMsg;
		cMsg.Writeuint8( SFX_TRIGGER_ID );
		m_TCS.Write( cMsg );
		g_pLTServer->SetObjectSFXMessage( m_hObject, cMsg.Read() );
	}

	if( bUpdateClients )
	{
		CAutoMessage cMsg;
		cMsg.Writeuint8( MID_SFX_MESSAGE );
		cMsg.Writeuint8( SFX_TRIGGER_ID );
		cMsg.WriteObject( m_hObject );
		cMsg.Writeuint8( TRIGFX_ALLFX_MSG );
		m_TCS.Write( cMsg );
		g_pLTServer->SendToClient( cMsg.Read(), LTNULL, MESSAGE_GUARANTEED );
	}
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	Trigger::SendLockedMsg()
//
//	PURPOSE:	Add client-side special fx
//
// ----------------------------------------------------------------------- //

void Trigger::SendLockedMsg()
{
	if( !m_bSendTriggerFXMsg )
		return;

	CAutoMessage cMsg;
	cMsg.Writeuint8( MID_SFX_MESSAGE );
	cMsg.Writeuint8( SFX_TRIGGER_ID );
	cMsg.WriteObject( m_hObject );
	cMsg.Writeuint8( TRIGFX_LOCKED_MSG );
	cMsg.Writeuint8( m_bLocked );
	g_pLTServer->SendToClient( cMsg.Read(), LTNULL, MESSAGE_GUARANTEED );

	CreateSpecialFX();
}