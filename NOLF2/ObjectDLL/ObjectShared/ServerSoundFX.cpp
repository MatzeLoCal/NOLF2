// ----------------------------------------------------------------------- //
//
// MODULE  : ServerSoundFX.cpp
//
// PURPOSE : A start/stoppable ambient sound object.
//
// CREATED : 09/11/98
//
// (c) 1998-1999 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "ServerSoundFX.h"
#include "iltserver.h"
#include "ServerUtilities.h"
#include "ObjectMsgs.h"
#include "ParsedMsg.h"
#include "ServerSoundMgr.h"
#include "SoundbuteMgr.h"

LINKFROM_MODULE( ServerSoundFx );

BEGIN_CLASS(SoundFX)
    ADD_BOOLPROP(StartOn, LTTRUE)
	ADD_STRINGPROP_FLAG(Sound, "", PF_FILENAME)
	ADD_LONGINTPROP(Priority, 0.0f)
	ADD_REALPROP_FLAG(OuterRadius, 500.0f, PF_RADIUS)
	ADD_REALPROP_FLAG(InnerRadius, 100.0f, PF_RADIUS)
	ADD_LONGINTPROP(Volume, 100)
	ADD_REALPROP(PitchShift, 1.0f)
	ADD_BOOLPROP(Ambient, 1)
	ADD_BOOLPROP(Loop, 1)
	ADD_BOOLPROP(PlayAttached, 0)
	ADD_STRINGPROP_FLAG(Filter, "UnFiltered", PF_STATICLIST)
END_CLASS_DEFAULT_FLAGS_PLUGIN(SoundFX, BaseClass, NULL, NULL, 0, CSoundFXPlugin)

CMDMGR_BEGIN_REGISTER_CLASS( SoundFX )

	CMDMGR_ADD_MSG( TOGGLE,	1, NULL, "TOGGLE" )
	CMDMGR_ADD_MSG( ON,		1, NULL, "ON" )
	CMDMGR_ADD_MSG( TRIGGER, 1, NULL, "TRIGGER" )
	CMDMGR_ADD_MSG( OFF,	1,	NULL, "OFF" )
	CMDMGR_ADD_MSG( KILL,	1,	NULL, "KILL" )

CMDMGR_END_REGISTER_CLASS( SoundFX, BaseClass )


LTRESULT CSoundFXPlugin::PreHook_EditStringList(
	const char* szRezPath,
	const char* szPropName,
	char** aszStrings,
    uint32* pcStrings,
    const uint32 cMaxStrings,
    const uint32 cMaxStringLength)
{
	if (_strcmpi("Filter", szPropName) == 0)
	{
		m_SoundFilterMgrPlugin.PreHook_EditStringList(szRezPath, szPropName,
			aszStrings, pcStrings, cMaxStrings, cMaxStringLength);

		if (!m_SoundFilterMgrPlugin.PopulateStringList(aszStrings, pcStrings,
			 cMaxStrings, cMaxStringLength)) return LT_UNSUPPORTED;

		return LT_OK;
	}

	return LT_UNSUPPORTED;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX::SoundFX()
//
//	PURPOSE:	Constructor
//
// ----------------------------------------------------------------------- //

SoundFX::SoundFX() : GameBase(OT_NORMAL)
{
    m_bStartOn              = LTTRUE;

    m_hstrSound             = LTNULL;
    m_hsndSound             = LTNULL;
	m_nFilterId				= 0;

	m_fOuterRadius			= 0.0f;
	m_fInnerRadius			= 0.0f;
	m_nVolume				= 0;
	m_fPitchShift			= 1.0f;
    m_bAmbient              = LTFALSE;
    m_bLooping              = LTTRUE;
    m_bAttached             = LTFALSE;
	m_nPriority				= SOUNDPRIORITY_MISC_LOW;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX::~SoundFX()
//
//	PURPOSE:	Destructor
//
// ----------------------------------------------------------------------- //

SoundFX::~SoundFX()
{
	if (m_hstrSound)
	{
        g_pLTServer->FreeString(m_hstrSound);
	}

	if (m_hsndSound)
	{
        g_pLTServer->SoundMgr()->KillSound(m_hsndSound);
	}
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	EngineMessageFn
//
//	PURPOSE:	Handle engine messages
//
// ----------------------------------------------------------------------- //

uint32 SoundFX::EngineMessageFn(uint32 messageID, void *pData, LTFLOAT fData)
{
	switch(messageID)
	{
		case MID_PRECREATE:
		{
            uint32 dwRet = BaseClass::EngineMessageFn(messageID, pData, fData);

			if (fData == PRECREATE_WORLDFILE || fData == PRECREATE_STRINGPROP)
			{
				ReadProp((ObjectCreateStruct*)pData);
			}

			PostPropRead((ObjectCreateStruct*)pData);

			return dwRet;
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

		case MID_UPDATE:
		{
			if (m_hsndSound)
			{
                g_pLTServer->SoundMgr()->KillSound(m_hsndSound);
				m_hsndSound = NULL;
			}

			SetNextUpdate(UPDATE_NEVER);
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

		default : break;
	}

	return BaseClass::EngineMessageFn(messageID, pData, fData);
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX:HandleTrigger()
//
//	PURPOSE:	Called when triggered.
//
// ----------------------------------------------------------------------- //

bool SoundFX::OnTrigger(HOBJECT hSender, const CParsedMsg &cMsg)
{
	static CParsedMsg::CToken s_cTok_Toggle("TOGGLE");
	static CParsedMsg::CToken s_cTok_On("ON");
	static CParsedMsg::CToken s_cTok_Trigger("TRIGGER");
	static CParsedMsg::CToken s_cTok_Off("OFF");
	static CParsedMsg::CToken s_cTok_Kill("KILL");

    if (cMsg.GetArg(0) == s_cTok_Toggle)
    {
		if (m_hsndSound)
		{
			if (m_bLooping)
			{
                g_pLTServer->SoundMgr()->KillSoundLoop(m_hsndSound);
			}
			else
			{
                g_pLTServer->SoundMgr()->KillSound(m_hsndSound);
			}

			m_hsndSound = NULL;
		}
		else
		{
			PlaySound( );
		}
    }
    else if ((cMsg.GetArg(0) == s_cTok_On) || (cMsg.GetArg(0) == s_cTok_Trigger))
    {
		PlaySound();
    }
    else if (cMsg.GetArg(0) == s_cTok_Off)
    {
		if (m_hsndSound)
		{
			if (m_bLooping)
			{
                g_pLTServer->SoundMgr()->KillSoundLoop(m_hsndSound);
			}
			else
			{
                g_pLTServer->SoundMgr()->KillSound(m_hsndSound);
			}

			m_hsndSound = NULL;
		}
    }
    else if (cMsg.GetArg(0) == s_cTok_Kill)
    {
		if (m_hsndSound)
		{
            g_pLTServer->SoundMgr()->KillSound(m_hsndSound);
			m_hsndSound = NULL;
		}
    }
	else
		return GameBase::OnTrigger(hSender, cMsg);

	return true;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX:ReadProp
//
//	PURPOSE:	Set property value
//
// ----------------------------------------------------------------------- //

LTBOOL SoundFX::ReadProp(ObjectCreateStruct *)
{
	GenericProp genProp;

    if (g_pLTServer->GetPropGeneric("StartOn", &genProp) == LT_OK)
	{
		m_bStartOn = genProp.m_Bool;
	}

    if (g_pLTServer->GetPropGeneric("Sound", &genProp) == LT_OK)
	{
		if (genProp.m_String[0])
		{
             m_hstrSound = g_pLTServer->CreateString(genProp.m_String);
		}
	}

    if (g_pLTServer->GetPropGeneric("Priority", &genProp) == LT_OK)
	{
		m_nPriority = (unsigned char) genProp.m_Long;
	}

    if (g_pLTServer->GetPropGeneric("OuterRadius", &genProp) == LT_OK)
	{
		m_fOuterRadius = genProp.m_Float;
	}

    if (g_pLTServer->GetPropGeneric("InnerRadius", &genProp) == LT_OK)
	{
		m_fInnerRadius = genProp.m_Float;
	}

    if (g_pLTServer->GetPropGeneric("Volume", &genProp) == LT_OK)
	{
        m_nVolume = (uint8) genProp.m_Long;
	}

    if (g_pLTServer->GetPropGeneric("PitchShift", &genProp) == LT_OK)
	{
		m_fPitchShift = genProp.m_Float;
	}

    if (g_pLTServer->GetPropGeneric("Ambient", &genProp) == LT_OK)
	{
		m_bAmbient = genProp.m_Bool;
	}

    if (g_pLTServer->GetPropGeneric("Loop", &genProp) == LT_OK)
	{
		m_bLooping = genProp.m_Bool;
	}

    if (g_pLTServer->GetPropGeneric("PlayAttached", &genProp) == LT_OK)
	{
		m_bAttached = genProp.m_Bool;
	}

    if (g_pLTServer->GetPropGeneric("Filter", &genProp) == LT_OK)
	{
		SOUNDFILTER* pFilter = g_pSoundFilterMgr->GetFilter(genProp.m_String);
		if (pFilter)
		{
			m_nFilterId = pFilter->nId;
		}
	}
    return LTTRUE;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX:PostPropRead()
//
//	PURPOSE:	Update properties
//
// ----------------------------------------------------------------------- //

void SoundFX::PostPropRead(ObjectCreateStruct *pStruct)
{
	if (!pStruct) return;

    // Set the Update!

	pStruct->m_NextUpdate = 0.0f;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX:InitialUpdate()
//
//	PURPOSE:	Handle initial update
//
// ----------------------------------------------------------------------- //

LTBOOL SoundFX::InitialUpdate()
{
	if (m_bStartOn)
	{
		PlaySound();
	}

    SetNextUpdate(UPDATE_NEVER);

    return LTTRUE;
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX::PlaySound
//
//	PURPOSE:	Plays the requested sound file.
//
// ----------------------------------------------------------------------- //

void SoundFX::PlaySound()
{
	// Kill the current sound

	if (m_hsndSound)
	{
        g_pLTServer->SoundMgr()->KillSound(m_hsndSound);
        m_hsndSound = LTNULL;
	}

    const char *pSoundFile = g_pLTServer->GetStringData(m_hstrSound);

	// ★ LT_SOUNDFX_OVERRIDE="ObjectName=file[;ObjectName=file…]" (§77).
	//
	// Repoint one PLACED ambient object at a different .wav without touching the
	// retail world file — the authored data stays read-only and the experiment
	// is one environment variable. Written for the `water lapping_02` case: the
	// object is NAMED _02 but its authored Sound property is
	// `Snd\amb\waterlapping_01.WAV`, while a genuinely different `_02` recording
	// exists, so the same 7.16 s loop plays from two points 990 apart and combs.
	//
	//   LT_SOUNDFX_OVERRIDE="water lapping_02=Snd\amb\waterlapping_02.WAV"
	static char s_szOverrideBuf[1024];
	if (m_hObject && getenv("LT_SOUNDFX_OVERRIDE"))
	{
		char szName[64];
		szName[0] = 0;
		g_pLTServer->GetObjectName(m_hObject, szName, sizeof(szName));

		strncpy(s_szOverrideBuf, getenv("LT_SOUNDFX_OVERRIDE"), sizeof(s_szOverrideBuf) - 1);
		s_szOverrideBuf[sizeof(s_szOverrideBuf) - 1] = 0;

		for (char *pTok = strtok(s_szOverrideBuf, ";"); pTok; pTok = strtok(NULL, ";"))
		{
			char *pEq = strchr(pTok, '=');
			if (!pEq)
				continue;
			*pEq = 0;
			if (szName[0] && stricmp(szName, pTok) == 0)
			{
				pSoundFile = pEq + 1;
				fprintf(stderr, "[sfx] OVERRIDE '%s' -> '%s'\n", szName, pSoundFile);
				break;
			}
		}
	}

	// ★ LT_TRACE_SOUNDFX=1 — THE AUTHORED AMBIENT SOUND PLACEMENTS (§75).
	//
	// "Which sound is near that spot?" is a question about the LEVEL DATA, not
	// about the mixer, and this is the only place that has all of it at once:
	// the file, the world position, and the inner/outer radii that decide where
	// it is audible. Dumping it is how you answer "is anything even loaded
	// here?" without standing in the level and guessing by ear.
	//
	// ⚠️ Prints on every PlaySound, INCLUDING retriggers — a line appearing over
	// and over for one object IS the finding, so do not de-duplicate it.
	if (getenv("LT_TRACE_SOUNDFX"))
	{
		LTVector vPos(0.0f, 0.0f, 0.0f);
		if (m_hObject)
			g_pLTServer->GetObjectPos(m_hObject, &vPos);

		char szName[64];
		szName[0] = 0;
		if (m_hObject)
			g_pLTServer->GetObjectName(m_hObject, szName, sizeof(szName));
		const char *pszName = szName[0] ? szName : "?";

		fprintf(stderr, "[sfx] SoundFX '%s' file='%s' @(%.0f %.0f %.0f) "
		                "inner=%.0f outer=%.0f vol=%u loop=%d ambient=%d prio=%u\n",
		        pszName, pSoundFile ? pSoundFile : "<NONE>",
		        vPos.x, vPos.y, vPos.z, m_fInnerRadius, m_fOuterRadius,
		        (unsigned)m_nVolume, (int)m_bLooping, (int)m_bAmbient,
		        (unsigned)m_nPriority);
	}

	// Play the sound...

	if (pSoundFile)
	{
		if( g_pSoundButeMgr->GetSoundSetFromName( pSoundFile ) != INVALID_SOUND_BUTE )
		{
			uint32 dwFlags = PLAYSOUND_GETHANDLE | PLAYSOUND_TIME | PLAYSOUND_REVERB;

			dwFlags |= (m_nVolume < 100 ? PLAYSOUND_CTRL_VOL : 0);
			dwFlags |= (m_fPitchShift != 1.0F ? PLAYSOUND_CTRL_PITCH : 0);
			dwFlags |= (m_bAmbient ? PLAYSOUND_AMBIENT : PLAYSOUND_3D);

			m_hsndSound = g_pServerSoundMgr->PlaySoundFromObject( m_hObject, pSoundFile, m_fOuterRadius, SOUNDPRIORITY_MISC_MEDIUM, 
																  dwFlags, m_nVolume, 1.0f, m_fInnerRadius );
		}
		else
		{
			PlaySoundInfo playSoundInfo;

			PLAYSOUNDINFO_INIT(playSoundInfo);

			playSoundInfo.m_dwFlags = PLAYSOUND_GETHANDLE | PLAYSOUND_TIME | PLAYSOUND_REVERB;
			if (m_bLooping)
			{
				playSoundInfo.m_dwFlags |= PLAYSOUND_LOOP;
			}

			if (m_bAttached)
			{
				playSoundInfo.m_dwFlags |= PLAYSOUND_ATTACHED;
				playSoundInfo.m_hObject = m_hObject;
			}

			SAFE_STRCPY(playSoundInfo.m_szSoundName, pSoundFile);
			playSoundInfo.m_nPriority = m_nPriority;
			playSoundInfo.m_fOuterRadius = m_fOuterRadius;
			playSoundInfo.m_fInnerRadius = m_fInnerRadius;
			if (m_nVolume < 100)
			{
				playSoundInfo.m_nVolume = m_nVolume;
				playSoundInfo.m_dwFlags |= PLAYSOUND_CTRL_VOL;
			}

			if (m_fPitchShift != 1.0f)
			{
				playSoundInfo.m_dwFlags |= PLAYSOUND_CTRL_PITCH;
				playSoundInfo.m_fPitchShift = m_fPitchShift;
			}

			g_pLTServer->GetObjectPos(m_hObject, &playSoundInfo.m_vPosition);
			if (m_bAmbient)
			{
				playSoundInfo.m_dwFlags |= PLAYSOUND_AMBIENT;
			}
			else
			{
				playSoundInfo.m_dwFlags |= PLAYSOUND_3D;
			}

			playSoundInfo.m_UserData = m_nFilterId;

			m_hsndSound = g_pServerSoundMgr->PlaySoundDirect(playSoundInfo);
		}

		if (!m_bLooping && m_hsndSound)
		{
            LTFLOAT fDuration;
            g_pLTServer->SoundMgr()->GetSoundDuration(m_hsndSound, fDuration);
            SetNextUpdate(fDuration);
		}
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX::Save
//
//	PURPOSE:	Save the object
//
// ----------------------------------------------------------------------- //

void SoundFX::Save(ILTMessage_Write *pMsg, uint32 dwSaveFlags)
{
	if (!pMsg) return;

	SAVE_HSTRING(m_hstrSound);

	SAVE_FLOAT(m_fOuterRadius);
    SAVE_FLOAT(m_fInnerRadius);
    SAVE_BYTE(m_nVolume);
    SAVE_BYTE(m_nFilterId);
    SAVE_FLOAT(m_fPitchShift);
    SAVE_BOOL(m_bAmbient);
    SAVE_BOOL(m_bLooping);
    SAVE_BOOL(m_bAttached);
    SAVE_BYTE(m_nPriority);
    SAVE_bool(m_hsndSound != 0);
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	SoundFX::Load
//
//	PURPOSE:	Load the object
//
// ----------------------------------------------------------------------- //

void SoundFX::Load(ILTMessage_Read *pMsg, uint32 dwLoadFlags)
{
	if (!pMsg) return;

    m_hsndSound         = LTNULL;
    LOAD_HSTRING(m_hstrSound);

    LOAD_FLOAT(m_fOuterRadius);
    LOAD_FLOAT(m_fInnerRadius);
    LOAD_BYTE(m_nVolume);
    LOAD_BYTE(m_nFilterId);
    LOAD_FLOAT(m_fPitchShift);
    LOAD_BOOL(m_bAmbient);
    LOAD_BOOL(m_bLooping);
    LOAD_BOOL(m_bAttached);
    LOAD_BYTE(m_nPriority);

	bool bSoundPlaying;
	LOAD_bool(bSoundPlaying);
    if (bSoundPlaying && m_bLooping)
	{
		PlaySound();
	}
}


