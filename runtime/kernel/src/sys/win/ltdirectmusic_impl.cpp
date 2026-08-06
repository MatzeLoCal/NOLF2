/****************************************************************************

	MODULE:		LTDirectMusic_impl (.CPP)

	PURPOSE:	No-op stub of the LithTech DirectMusic manager.

	The original implementation drove Microsoft DirectMusic through the
	dmusici.h "Interactive" API (IDirectMusicPerformance8, IDirectMusicSegment8,
	IDirectMusicLoader8, ...).  Those headers and their GUIDs were removed from
	modern Windows / DirectX SDKs, so the subsystem is stubbed: all playback
	calls succeed silently and produce no music.  The string<->enact-type
	helpers remain functional so that level triggers referencing music still
	parse without error.

***************************************************************************/

#include "bdefs.h"
#include "ltdirectmusic_impl.h"

#include <string.h>

#ifdef LT_MACOS
// ★ NOT A STUB ANY MORE ON macOS. The header above describes DirectMusic as
// unbuildable against modern SDKs, which is true -- but NOLF2 never needed the
// DirectMusic SYNTHESISER: its .SGT files are wave-track playlists over
// MP3-in-WAV files, so the whole subsystem is a scheduler. macos_music.cpp is
// that scheduler; everything here just forwards to it.
#include "sys/macos/macos_music.h"
#endif

// instantiate our implementation class of ILTDirectMusicMgr
define_interface(CLTDirectMusicMgr, ILTDirectMusicMgr);

CLTDirectMusicMgr::CLTDirectMusicMgr()
{
}

CLTDirectMusicMgr::~CLTDirectMusicMgr()
{
}

LTRESULT CLTDirectMusicMgr::Init()
{
#ifdef LT_MACOS
	LTMusic_Init();
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::Term()
{
#ifdef LT_MACOS
	LTMusic_Term();
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::InitLevel(const char* sWorkingDirectory, const char* sControlFileName,
	const char* /*sDefine1*/, const char* /*sDefine2*/, const char* /*sDefine3*/)
{
#ifdef LT_MACOS
	// ⚠️ Returning an error here makes CMusic::InitLevel bail and leave the game
	// with no music state at all, so a missing control file must still report
	// LT_OK -- the level simply plays nothing.
	LTMusic_InitLevel(sWorkingDirectory, sControlFileName);
#else
	(void)sWorkingDirectory; (void)sControlFileName;
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::TermLevel()
{
#ifdef LT_MACOS
	LTMusic_TermLevel();
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::Play()
{
#ifdef LT_MACOS
	LTMusic_Play();
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::Stop(const LTDMEnactTypes /*nStart*/)
{
#ifdef LT_MACOS
	LTMusic_Stop();
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::Pause(const LTDMEnactTypes /*nStart*/)
{
#ifdef LT_MACOS
	LTMusic_Pause(true);
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::UnPause()
{
#ifdef LT_MACOS
	LTMusic_Pause(false);
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::SetVolume(const long nVolume)
{
#ifdef LT_MACOS
	LTMusic_SetVolume(nVolume);
#else
	(void)nVolume;
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::ChangeIntensity(const int nNewIntensity, const LTDMEnactTypes /*nStart*/)
{
#ifdef LT_MACOS
	// The enact type (Immediate/Beat/Measure/Segment) selects WHEN DirectMusic
	// would cut over. We always change at the end of the bridging transition
	// segment, or immediately when none is authored -- see macos_music.cpp.
	LTMusic_ChangeIntensity(nNewIntensity);
#else
	(void)nNewIntensity;
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::PlaySecondary(const char* sSecondarySegment, const LTDMEnactTypes /*nStart*/)
{
#ifdef LT_MACOS
	LTMusic_PlaySecondary(sSecondarySegment);
#else
	(void)sSecondarySegment;
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::StopSecondary(const char* sSecondarySegment, const LTDMEnactTypes /*nStart*/)
{
#ifdef LT_MACOS
	LTMusic_StopSecondary(sSecondarySegment);
#else
	(void)sSecondarySegment;
#endif
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::PlayMotif(const char* /*sStyleName*/, const char* /*sMotifName*/, const LTDMEnactTypes /*nStart*/)
{
	return LT_OK;
}

LTRESULT CLTDirectMusicMgr::StopMotif(const char* /*sStyleName*/, const char* /*sMotifName*/, const LTDMEnactTypes /*nStart*/)
{
	return LT_OK;
}

int CLTDirectMusicMgr::GetCurIntensity()
{
#ifdef LT_MACOS
	return LTMusic_GetCurIntensity();
#else
	return 0;
#endif
}

LTDMEnactTypes CLTDirectMusicMgr::StringToEnactType(const char* sName)
{
	if (sName == NULL) return LTDMEnactInvalid;
	if (stricmp(sName, "Invalid") == 0) return LTDMEnactInvalid;
	if (stricmp(sName, "Default") == 0) return LTDMEnactDefault;
	if (stricmp(sName, "Immediatly") == 0) return LTDMEnactImmediately;
	if (stricmp(sName, "Immediately") == 0) return LTDMEnactImmediately;
	if (stricmp(sName, "Immediate") == 0) return LTDMEnactImmediately;
	if (stricmp(sName, "NextBeat") == 0) return LTDMEnactNextBeat;
	if (stricmp(sName, "NextMeasure") == 0) return LTDMEnactNextMeasure;
	if (stricmp(sName, "NextGrid") == 0) return LTDMEnactNextGrid;
	if (stricmp(sName, "NextSegment") == 0) return LTDMEnactNextSegment;
	if (stricmp(sName, "NextMarker") == 0) return LTDMEnactNextMarker;
	if (stricmp(sName, "Beat") == 0) return LTDMEnactNextBeat;
	if (stricmp(sName, "Measure") == 0) return LTDMEnactNextMeasure;
	if (stricmp(sName, "Grid") == 0) return LTDMEnactNextGrid;
	if (stricmp(sName, "Segment") == 0) return LTDMEnactNextSegment;
	if (stricmp(sName, "Marker") == 0) return LTDMEnactNextMarker;
	return LTDMEnactInvalid;
}

void CLTDirectMusicMgr::EnactTypeToString(LTDMEnactTypes nType, char* sName)
{
	switch (nType)
	{
		case LTDMEnactInvalid:		strcpy(sName, "Invalid");	break;
		case LTDMEnactDefault:		strcpy(sName, "Default");	break;
		case LTDMEnactImmediately:	strcpy(sName, "Immediate");	break;
		case LTDMEnactNextBeat:		strcpy(sName, "Beat");		break;
		case LTDMEnactNextMeasure:	strcpy(sName, "Measure");	break;
		case LTDMEnactNextGrid:		strcpy(sName, "Grid");		break;
		case LTDMEnactNextSegment:	strcpy(sName, "Segment");	break;
		case LTDMEnactNextMarker:	strcpy(sName, "Marker");	break;
		default:					strcpy(sName, "");			break;
	}
}

int CLTDirectMusicMgr::GetNumIntensities()
{
#ifdef LT_MACOS
	return LTMusic_GetNumIntensities();
#else
	return 0;
#endif
}

int CLTDirectMusicMgr::GetInitialIntensity()
{
#ifdef LT_MACOS
	return LTMusic_GetInitialIntensity();
#else
	return 0;
#endif
}

int CLTDirectMusicMgr::GetInitialVolume()
{
#ifdef LT_MACOS
	return LTMusic_GetInitialVolume();
#else
	return 0;
#endif
}

int CLTDirectMusicMgr::GetVolumeOffset()
{
#ifdef LT_MACOS
	return LTMusic_GetVolumeOffset();
#else
	return 0;
#endif
}
