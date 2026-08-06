/****************************************************************************
;
;	MODULE:		LTDirectMusic_Impl (.H)
;
;	PURPOSE:	No-op stub of the LithTech DirectMusic manager.
;
;	DirectMusic (the dmusici.h "Interactive" API: IDirectMusicPerformance8,
;	IDirectMusicSegment8, IDirectMusicLoader8, ...) was removed from modern
;	Windows / DirectX SDKs.  The original dynamic-music implementation cannot
;	be built against current SDKs, so this stub satisfies the ILTDirectMusicMgr
;	interface with no-ops.  The engine builds and runs; there is simply no
;	interactive music.  The string<->enact-type helpers are kept functional so
;	level triggers that reference music still parse correctly.
;
***************************************************************************/

#ifndef __LTDIRECTMUSIC_IMPL_H__
#define __LTDIRECTMUSIC_IMPL_H__

#ifndef __ILTDIRECTMUSIC_H__
#include "iltdirectmusic.h"
#endif

// Main class for LTDirectMusicMgr (DirectMusic disabled - all playback no-ops)
class CLTDirectMusicMgr : public ILTDirectMusicMgr
{
public:
	declare_interface(CLTDirectMusicMgr);

	CLTDirectMusicMgr();
	~CLTDirectMusicMgr();

	virtual LTRESULT Init();
	virtual LTRESULT Term();
	virtual LTRESULT InitLevel(const char* sWorkingDirectory, const char* sControlFileName,
		const char* sDefine1 = NULL, const char* sDefine2 = NULL, const char* sDefine3 = NULL);
	virtual LTRESULT TermLevel();
	virtual LTRESULT Play();
	virtual LTRESULT Stop(const LTDMEnactTypes nStart = LTDMEnactDefault);
	virtual LTRESULT Pause(const LTDMEnactTypes nStart = LTDMEnactDefault);
	virtual LTRESULT UnPause();
	virtual LTRESULT SetVolume(const long nVolume);
	virtual LTRESULT ChangeIntensity(const int nNewIntensity, const LTDMEnactTypes nStart = LTDMEnactInvalid);
	virtual LTRESULT PlaySecondary(const char* sSecondarySegment, const LTDMEnactTypes nStart = LTDMEnactDefault);
	virtual LTRESULT StopSecondary(const char* sSecondarySegment = NULL, const LTDMEnactTypes nStart = LTDMEnactDefault);
	virtual LTRESULT PlayMotif(const char* sStyleName, const char* sMotifName, const LTDMEnactTypes nStart = LTDMEnactDefault);
	virtual LTRESULT StopMotif(const char* sStyleName, const char* sMotifName = NULL, const LTDMEnactTypes nStart = LTDMEnactDefault);
	virtual int GetCurIntensity();
	virtual LTDMEnactTypes StringToEnactType(const char* sName);
	virtual void EnactTypeToString(LTDMEnactTypes nType, char* sName);
	virtual int GetNumIntensities();
	virtual int GetInitialIntensity();
	virtual int GetInitialVolume();
	virtual int GetVolumeOffset();
};

#endif //! __LTDIRECTMUSIC_IMPL_H__
