// ----------------------------------------------------------------------- //
//
// MODULE  : s_coreaudio.cpp
//
// PURPOSE : macOS sound driver -- an ILTSoundSys implementation on CoreAudio.
//
//           WHY A HAND-WRITTEN MIXER. The engine hands the driver
//           already-decoded PCM plus a WAVEFORMATEX
//           (CSoundInstance::Play -> InitSampleFromAddress), and then drives
//           volume/pan/position/loop-block/ms-position per sample with Miles'
//           exact semantics (volume 0..127, pan 0..127 with 64 = centre). The
//           cheapest faithful way to honour that is to own the mix: one output
//           AudioUnit, a fixed voice pool, and a render callback that
//           resamples, applies gain/pan and accumulates. AVAudioEnvironmentNode
//           would supply 3D for free but its distance model is not LithTech's,
//           and it is awkward to drive from C++.
//
//           STRUCTURE
//             CMacVoice      - one playing sample (2D or 3D)
//             CMacSoundSys   - the ILTSoundSys implementation, owns the pool
//             CMacSoundFactory - registers itself as ILTSoundFactory at static
//                              init, which is the hook soundmgr.cpp looks for
//                              (it was NULL on macOS, hence "no sound driver").
//
//           ⚠️ VERIFICATION: this code cannot be checked by listening from a
//           headless run, so **LT_AUDIO_DUMP=<file.wav>** captures the final
//           mixed output to a WAV. That is the audio equivalent of
//           LT_DUMP_FRAME and it is the primary tell: a silent file means the
//           chain is broken somewhere above the mixer.
//
// ----------------------------------------------------------------------- //

#include "iltsound.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

#include <atomic>
#include <sched.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

typedef sint16 S16;
typedef uint16 U16;
typedef sint32 S32;
typedef uint32 U32;
typedef lpvoid PTR;

// --------------------------------------------------------------------------
// Tunables
// --------------------------------------------------------------------------
static const uint32 kOutputRate    = 44100;
static const uint32 kOutputChans   = 2;
// ⚠️ THE ENGINE PRE-ALLOCATES ITS WHOLE SAMPLE POOL UP FRONT and can ask for
// SOUNDMGR_MAXSOUNDINSTANCES (255) of them, split between SW and 3D
// (CSoundMgr::SetSampleCounts). It then RE-allocates the entire list on a level
// change. So the pool must comfortably exceed 255, and streams must not be able
// to eat into it: with a 64-voice shared pool the engine took 56 at startup,
// leaving 8 for streams, and once leaked/concurrent streams pushed the free
// count below what the engine asked for on the next level load, its sample list
// came back short and ALL sound stopped -- permanently. Hence two disjoint
// ranges rather than one pool.
// WAVE format tags we care about (mmreg.h names).
static const uint32 kWaveFormatPCM = 1;
static const uint32 kWaveFormatMP3 = 85;   // WAVE_FORMAT_MPEGLAYER3 -- NOLF2 dialogue

static const uint32 kMaxSampleVoices = 256;   // [0, kMaxSampleVoices)  engine samples (2D + 3D)
static const uint32 kMaxStreamVoices = 32;    // [kMaxSampleVoices, kMaxVoices)  streams
static const uint32 kMaxVoices       = kMaxSampleVoices + kMaxStreamVoices;

// Debug capture: 60s of stereo int16 at the output rate.
static const uint32 kDumpMaxFrames = kOutputRate * 60;

static bool snd_Trace()
{
	static bool s_bTrace = (getenv("LT_TRACE_SOUND") != NULL);
	return s_bTrace;
}

// --------------------------------------------------------------------------
// CMacVoice -- one playing sample.
//
// ⚠️ THREADING. Fields are written by the game thread and read by the CoreAudio
// render thread, with no lock: a lock in a real-time callback is worse than the
// failure it prevents. The discipline that makes this safe:
//   * m_pData / m_nFrames / format are only ever written while m_bPlaying is
//     false, and m_bPlaying is set true only AFTER them;
//   * m_bPlaying is the single publication point (volatile, written last);
//   * m_fVolume / m_fPan / 3D position are floats updated live -- a torn read
//     costs at most one sample of a slightly wrong gain, i.e. inaudible.
// Anything that needs more than that (freeing m_pData) goes through Stop first.
// --------------------------------------------------------------------------
struct CMacVoice
{
	// --- identity / lifetime ---
	bool            m_bAllocated;       // handed out by Allocate*SampleHandle
	bool            m_b3D;
	volatile bool   m_bPlaying;         // publication flag (see above)
	volatile bool   m_bPaused;
	bool            m_bLoop;
	bool            m_bEverStarted;     // LS_DONE vs LS_STOPPED

	// --- source data ---
	// m_pData is owned by the ENGINE for samples; for STREAMS we own the
	// file image ourselves (m_pOwnedData) and free it in CloseStream.
	const uint8    *m_pData;
	uint8          *m_pOwnedData;
	bool            m_bStream;
	// ★ Deferred retirement (§66): a closed voice's buffer cannot be freed until
	// the mixer has provably left it, so it parks here with the callback count
	// at which that is guaranteed.
	uint8*          m_pRetiredData;
	uint32          m_nRetireAt;
	bool            m_bDecoded;         // PCM came from an MP3 decode: byte
	                                    // offsets into the ORIGINAL data are
	                                    // meaningless for this voice
	uint32          m_nFrames;          // frames, not bytes
	uint32          m_nSrcChannels;
	uint32          m_nSrcBits;
	uint32          m_nSrcRate;

	// --- playback cursor, in SOURCE frames ---
	double          m_fPos;
	double          m_fStep;            // src frames advanced per output frame

	// --- loop block (Miles: byte offsets into the sample data) ---
	bool            m_bLoopBlock;
	uint32          m_nLoopStart;       // frames
	uint32          m_nLoopEnd;         // frames

	// --- mix parameters ---
	float           m_fVolume;          // 0..1
	float           m_fPan;             // 0 = hard left, 1 = hard right
	float           m_fGainL, m_fGainR; // resolved, recomputed on change

	// --- 3D ---
	float           m_vPos[3];
	float           m_vVel[3];
	float           m_fMinDist, m_fMaxDist;
	// ★ DIAGNOSTIC (§78): how many times Render stopped this voice because it
	// ran off the end with m_bLoop FALSE. Counting is RT-safe; printing is not.
	uint32          m_nEndStops;
	float           m_fObstruction, m_fOcclusion;

	// ⚠️ POINTER-SIZED: the engine stores CSample*/CSoundBuffer*/CSoundInstance*
	// in these slots (soundmgr.cpp SAMPLE_LISTITEM/BUFFER/INSTANCE). See the
	// LTSOUNDUSERDATA note in iltsound.h -- sint32 here truncated them.
	LTSOUNDUSERDATA m_aUserData[8];

	void Reset()
	{
		memset(this, 0, sizeof(*this));
		m_fVolume  = 1.0f;
		m_fPan     = 0.5f;
		m_fGainL   = 1.0f;
		m_fGainR   = 1.0f;
		m_nEndStops = 0;
		m_fMinDist = 1.0f;
		m_fMaxDist = 1000.0f;
		m_fStep    = 1.0;
	}

	void ResolveGains()
	{
		// Equal-power pan, which is what a constant-power panner should do and
		// what Miles approximated. m_fPan 0..1.
		float fAngle = m_fPan * 1.5707963f;   // 0..pi/2
		m_fGainL = cosf(fAngle) * m_fVolume;
		m_fGainR = sinf(fAngle) * m_fVolume;
	}
};

// --------------------------------------------------------------------------
// CMacSoundSys
// --------------------------------------------------------------------------
class CMacSoundSys : public ILTSoundSys
{
public:
	CMacSoundSys();
	virtual ~CMacSoundSys() {}

	virtual bool        Init();
	virtual void        Term();
	virtual void*       GetDDInterface(uint uiDDInterfaceId);

	// system wide
	virtual void        Lock(void);
	virtual void        Unlock(void);
	virtual S32         Startup(void);
	virtual void        Shutdown(void);
	virtual U32         MsCount(void);
	virtual S32         SetPreference(U32 uiNumber, S32 siValue);
	virtual S32         GetPreference(U32 uiNumber);
	virtual void        MemFreeLock(void* ptr);
	virtual void*       MemAllocLock(U32 uiSize);
	virtual char*       LastError(void);

	// digital driver
	virtual S32         WaveOutOpen(LHDIGDRIVER* phDriver, PHWAVEOUT* pphWaveOut, S32 siDeviceId, WAVEFORMAT* pWaveFormat);
	virtual void        WaveOutClose(LHDIGDRIVER hDriver);
	virtual void        SetDigitalMasterVolume(LHDIGDRIVER hDig, S32 siMasterVolume);
	virtual S32         GetDigitalMasterVolume(LHDIGDRIVER hDig);
	virtual S32         DigitalHandleRelease(LHDIGDRIVER hDriver);
	virtual S32         DigitalHandleReacquire(LHDIGDRIVER hDriver);
	virtual bool        SetEAX20Filter(bool bEnable, LTSOUNDFILTERDATA* pFilterData);
	virtual bool        SupportsEAX20Filter();
	virtual bool        SetEAX20BufferSettings(LHSAMPLE hSample, LTSOUNDFILTERDATA* pFilterData);

	// 3d provider
	virtual void        Set3DProviderMinBuffers(U32 uiMinBuffers);
	virtual S32         Open3DProvider(LHPROVIDER hLib);
	virtual void        Close3DProvider(LHPROVIDER hLib);
	virtual void        Set3DProviderPreference(LHPROVIDER hLib, char* sName, void* pVal);
	virtual void        Get3DProviderAttribute(LHPROVIDER hLib, char* sName, void* pVal);
	virtual S32         Enumerate3DProviders(LHPROENUM* phNext, LHPROVIDER* phDest, char** psName);

	// 3d listener
	virtual LH3DPOBJECT Open3DListener(LHPROVIDER hLib);
	virtual void        Close3DListener(LH3DPOBJECT hListener);
	virtual void        SetListenerDoppler(LH3DPOBJECT hListener, float fDoppler);
	virtual void        CommitDeferred();

	// 3d object
	virtual void        Set3DPosition(LH3DPOBJECT hObj, float fX, float fY, float fZ);
	virtual void        Set3DVelocityVector(LH3DPOBJECT hObj, float fDX, float fDY, float fDZ);
	virtual void        Set3DOrientation(LH3DPOBJECT hObj, float fXf, float fYf, float fZf, float fXu, float fYu, float fZu);
	virtual void        Set3DUserData(LH3DPOBJECT hObj, U32 uiIndex, LTSOUNDUSERDATA siValue);
	virtual void        Get3DPosition(LH3DPOBJECT hObj, float* pfX, float* pfY, float* pfZ);
	virtual void        Get3DVelocity(LH3DPOBJECT hObj, float* pfDX, float* pfDY, float* pfDZ);
	virtual void        Get3DOrientation(LH3DPOBJECT hObj, float* pfXf, float* pfYf, float* pfZf, float* pfXu, float* pfYu, float* pfZu);
	virtual LTSOUNDUSERDATA Get3DUserData(LH3DPOBJECT hObj, U32 uiIndex);

	// 3d sample
	virtual LH3DSAMPLE  Allocate3DSampleHandle(LHPROVIDER hLib);
	virtual void        Release3DSampleHandle(LH3DSAMPLE hS);
	virtual void        Stop3DSample(LH3DSAMPLE hS);
	virtual void        Start3DSample(LH3DSAMPLE hS);
	virtual void        Resume3DSample(LH3DSAMPLE hS);
	virtual void        End3DSample(LH3DSAMPLE hS);
	virtual S32         Init3DSampleFromAddress(LH3DSAMPLE hS, void* pStart, U32 uiLen, WAVEFORMATEX* pWaveFormat, S32 siPlaybackRate, LTSOUNDFILTERDATA* pFilterData);
	virtual S32         Init3DSampleFromFile(LH3DSAMPLE hS, void* pFile_image, S32 siBlock, S32 siPlaybackRate, LTSOUNDFILTERDATA* pFilterData);
	virtual S32         Get3DSampleVolume(LH3DSAMPLE hS);
	virtual void        Set3DSampleVolume(LH3DSAMPLE hS, S32 siVolume);
	virtual U32         Get3DSampleStatus(LH3DSAMPLE hS);
	virtual void        Set3DSampleMsPosition(LHSAMPLE hS, S32 siMilliseconds);
	virtual S32         Set3DSampleInfo(LH3DSAMPLE hS, LTSOUNDINFO* pInfo);
	virtual void        Set3DSampleDistances(LH3DSAMPLE hS, float fMax_dist, float fMin_dist);
	virtual void        Set3DSamplePreference(LH3DSAMPLE hSample, char* sName, void* pVal);
	virtual void        Set3DSampleLoopBlock(LH3DSAMPLE hS, S32 siLoopStart, S32 siLoopEnd, bool bEnable);
	virtual void        Set3DSampleLoop(LH3DSAMPLE hS, bool bLoop);
	virtual void        Set3DSampleObstruction(LH3DSAMPLE hS, float fObstruction);
	virtual float       Get3DSampleObstruction(LH3DSAMPLE hS);
	virtual void        Set3DSampleOcclusion(LH3DSAMPLE hS, float fOcclusion);
	virtual float       Get3DSampleOcclusion(LH3DSAMPLE hS);

	// 2d sample
	virtual LHSAMPLE    AllocateSampleHandle(LHDIGDRIVER hDig);
	virtual void        ReleaseSampleHandle(LHSAMPLE hS);
	virtual void        InitSample(LHSAMPLE hS);
	virtual void        StopSample(LHSAMPLE hS);
	virtual void        StartSample(LHSAMPLE hS);
	virtual void        ResumeSample(LHSAMPLE hS);
	virtual void        EndSample(LHSAMPLE hS);
	virtual void        SetSampleVolume(LHSAMPLE hS, S32 siVolume);
	virtual void        SetSamplePan(LHSAMPLE hS, S32 siPan);
	virtual S32         GetSampleVolume(LHSAMPLE hS);
	virtual S32         GetSamplePan(LHSAMPLE hS);
	virtual void        SetSampleUserData(LHSAMPLE hS, U32 uiIndex, LTSOUNDUSERDATA siValue);
	virtual void        GetDirectSoundInfo(LHSAMPLE hS, PTDIRECTSOUND* ppDS, PTDIRECTSOUNDBUFFER* ppDSB);
	virtual void        SetSampleReverb(LHSAMPLE hS, float fReverb_level, float fReverb_reflect_time, float fReverb_decay_time);
	virtual S32         InitSampleFromAddress(LHSAMPLE hS, void* pStart, U32 uiLen, WAVEFORMATEX* pWaveFormat, S32 siPlaybackRate, LTSOUNDFILTERDATA* pFilterData);
	virtual S32         InitSampleFromFile(LHSAMPLE hS, void* pFile_image, S32 siBlock, S32 siPlaybackRate, LTSOUNDFILTERDATA* pFilterData);
	virtual void        SetSampleLoopBlock(LHSAMPLE hS, S32 siLoopStart, S32 siLoopEnd, bool bEnable);
	virtual void        SetSampleLoop(LHSAMPLE hS, bool bLoop);
	virtual void        SetSampleMsPosition(LHSAMPLE hS, S32 siMilliseconds);
	virtual LTSOUNDUSERDATA GetSampleUserData(LHSAMPLE hS, U32 uiIndex);
	virtual U32         GetSampleStatus(LHSAMPLE hS);

	// streams
	virtual LHSTREAM    OpenStream(char* sFilename, U32 nOffset, LHDIGDRIVER hDig, char* sStream, S32 siStream_mem);
	virtual void        SetStreamLoop(LHSTREAM hStream, bool bLoop);
	virtual void        SetStreamPlaybackRate(LHSTREAM hStream, S32 siRate);
	virtual void        SetStreamMsPosition(LHSTREAM hS, S32 siMilliseconds);
	virtual void        SetStreamUserData(LHSTREAM hS, U32 uiIndex, LTSOUNDUSERDATA siValue);
	virtual LTSOUNDUSERDATA GetStreamUserData(LHSTREAM hS, U32 uiIndex);
	virtual void        CloseStream(LHSTREAM hStream);
	virtual void        StartStream(LHSTREAM hStream);
	virtual void        PauseStream(LHSTREAM hStream, S32 siOnOff);
	virtual void        ResetStream(LHSTREAM hStream);
	virtual void        SetStreamVolume(LHSTREAM hStream, S32 siVolume);
	virtual void        SetStreamPan(LHSTREAM hStream, S32 siPan);
	virtual S32         GetStreamVolume(LHSTREAM hStream);
	virtual S32         GetStreamPan(LHSTREAM hStream);
	virtual U32         GetStreamStatus(LHSTREAM hStream);
	virtual S32         GetStreamBufferParam(LHSTREAM hStream, U32 uiParam);
	virtual void        ClearStreamBuffer(LHSTREAM hStream, bool bClearStreamDataQueue = true);

	// decompression
	virtual S32         DecompressADPCM(LTSOUNDINFO* pInfo, void** ppOutData, U32* puiOutSize);
	virtual S32         DecompressASI(void* pInData, U32 uiInSize, char* sFilename_ext, void** ppWav, U32* puiWavSize, LTLENGTHYCB fnCallback);

	virtual U32         GetThreadedSoundTicks();
	virtual bool        HasOnBoardMemory();

public:
	// The render callback, and the mix it drives.
	OSStatus            Render(AudioUnitRenderActionFlags* ioFlags, const AudioTimeStamp* inTs,
	                           UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList* ioData);
	void                FinishDump();       // public: also driven from a signal handler
	static void         FinishDumpAtExit(); // ...and from atexit (a clean quit never reaches Term)
	void                ReportMixStats();   // ditto -- async-signal-safe enough for a diagnostic

	static CMacSoundSys m_MacSoundSys;
	static char*        m_pcDesc;

private:
	CMacVoice*          VoiceFromHandle(void* h);
	CMacVoice*          AllocVoice(bool b3D, bool bStream = false);
	void                Update3DGains(CMacVoice* pVoice);
	void                StartDump();
	void                InstallDumpSignalHandlers();

	// ★★ RT-THREAD HANDSHAKE — see the note above WaitForRTQuiesce().
	void                WaitForRTQuiesce();
	void                ReapRetiredBuffers(bool bForce);
	std::atomic<uint32> m_nRTCallbacks;   // bumped once per Render(), on the RT thread

	AudioUnit           m_OutputUnit;
	bool                m_bInited;
	bool                m_bStarted;
	float               m_fMasterVolume;

	CMacVoice           m_aVoices[kMaxVoices];

	// listener state (3D)
	float               m_vListenerPos[3];
	float               m_vListenerForward[3];
	float               m_vListenerRight[3];
	float               m_fDoppler;
	LTSOUNDUSERDATA     m_aListenerUserData[8];

	// debug capture
	sint16*             m_pDumpBuf;
	// ⚠️ TOTAL frames ever captured, not the fill level — the capture is a RING
	// that keeps the LAST kDumpMaxFrames (§73). 64-bit so a long session cannot
	// wrap it.
	volatile uint64     m_nDumpFrames;
	char                m_szDumpPath[512];
	int                 m_nDumpFd;
	volatile bool       m_bDumpFlushed;
	bool                m_bSilent;          // LT_AUDIO_SILENT: capture but do not play

	// running mix statistics -- the headless tell (see LT_TRACE_SOUND)
	uint64              m_nFramesRendered;
	float               m_fPeak;          // whole-run high-water mark
	float               m_fIntervalPeak;  // since the last heartbeat -- the useful one
	uint32              m_nPeakVoices;
	volatile uint64     m_nClipped;      // output samples that hit full scale
};

CMacSoundSys CMacSoundSys::m_MacSoundSys;
char* CMacSoundSys::m_pcDesc = (char*)"CoreAudio";

// --------------------------------------------------------------------------
// Construction / init
// --------------------------------------------------------------------------

CMacSoundSys::CMacSoundSys()
{
	m_OutputUnit      = NULL;
	m_bInited         = false;
	m_bStarted        = false;
	m_nRTCallbacks.store(0, std::memory_order_relaxed);
	m_fMasterVolume   = 1.0f;
	m_pDumpBuf        = NULL;
	m_nDumpFrames     = 0;
	m_szDumpPath[0]   = 0;
	m_nDumpFd         = -1;
	m_bDumpFlushed    = false;
	m_bSilent         = (getenv("LT_AUDIO_SILENT") != NULL);
	m_nFramesRendered = 0;
	m_fPeak           = 0.0f;
	m_fIntervalPeak   = 0.0f;
	m_nPeakVoices     = 0;
	m_nClipped        = 0;
	m_fDoppler        = 1.0f;

	for (uint32 i = 0; i < kMaxVoices; ++i)
		m_aVoices[i].Reset();

	memset(m_vListenerPos, 0, sizeof(m_vListenerPos));
	memset(m_aListenerUserData, 0, sizeof(m_aListenerUserData));
	m_vListenerForward[0] = 0.0f; m_vListenerForward[1] = 0.0f; m_vListenerForward[2] = 1.0f;
	m_vListenerRight[0]   = 1.0f; m_vListenerRight[1]   = 0.0f; m_vListenerRight[2]   = 0.0f;
}

static OSStatus snd_RenderCallback(void* inRefCon, AudioUnitRenderActionFlags* ioFlags,
                                   const AudioTimeStamp* inTs, UInt32 inBus,
                                   UInt32 inNumberFrames, AudioBufferList* ioData)
{
	return ((CMacSoundSys*)inRefCon)->Render(ioFlags, inTs, inBus, inNumberFrames, ioData);
}

bool CMacSoundSys::Init()
{
	if (m_bInited)
		return true;

	AudioComponentDescription desc;
	memset(&desc, 0, sizeof(desc));
	desc.componentType         = kAudioUnitType_Output;
	desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AudioComponent comp = AudioComponentFindNext(NULL, &desc);
	if (!comp)
	{
		fprintf(stderr, "[snd] no default output AudioUnit\n");
		return false;
	}

	OSStatus err = AudioComponentInstanceNew(comp, &m_OutputUnit);
	if (err != noErr)
	{
		fprintf(stderr, "[snd] AudioComponentInstanceNew failed (%d)\n", (int)err);
		return false;
	}

	// Canonical macOS output format: 32-bit float, non-interleaved.
	AudioStreamBasicDescription asbd;
	memset(&asbd, 0, sizeof(asbd));
	asbd.mSampleRate       = (Float64)kOutputRate;
	asbd.mFormatID         = kAudioFormatLinearPCM;
	asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
	                         kAudioFormatFlagIsNonInterleaved;
	asbd.mChannelsPerFrame = kOutputChans;
	asbd.mBitsPerChannel   = 32;
	asbd.mFramesPerPacket  = 1;
	asbd.mBytesPerFrame    = 4;     // per channel: non-interleaved
	asbd.mBytesPerPacket   = 4;

	err = AudioUnitSetProperty(m_OutputUnit, kAudioUnitProperty_StreamFormat,
	                           kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
	if (err != noErr)
	{
		fprintf(stderr, "[snd] SetProperty(StreamFormat) failed (%d)\n", (int)err);
		AudioComponentInstanceDispose(m_OutputUnit);
		m_OutputUnit = NULL;
		return false;
	}

	AURenderCallbackStruct cb;
	cb.inputProc       = snd_RenderCallback;
	cb.inputProcRefCon = this;
	err = AudioUnitSetProperty(m_OutputUnit, kAudioUnitProperty_SetRenderCallback,
	                           kAudioUnitScope_Input, 0, &cb, sizeof(cb));
	if (err != noErr)
	{
		fprintf(stderr, "[snd] SetProperty(RenderCallback) failed (%d)\n", (int)err);
		AudioComponentInstanceDispose(m_OutputUnit);
		m_OutputUnit = NULL;
		return false;
	}

	err = AudioUnitInitialize(m_OutputUnit);
	if (err != noErr)
	{
		fprintf(stderr, "[snd] AudioUnitInitialize failed (%d)\n", (int)err);
		AudioComponentInstanceDispose(m_OutputUnit);
		m_OutputUnit = NULL;
		return false;
	}

	StartDump();

	m_bInited = true;
	fprintf(stderr, "[snd] CoreAudio output opened: %u Hz, %u ch, float32%s\n",
	        kOutputRate, kOutputChans, m_pDumpBuf ? " (capturing)" : "");

	// Shout about it. LT_AUDIO_SILENT is an env var, so it survives in a shell
	// for every later command -- someone who once ran a headless capture in that
	// terminal would then get a completely silent game with no other symptom,
	// which is indistinguishable from the driver being broken.
	if (m_bSilent)
		fprintf(stderr, "[snd] ⚠️⚠️ LT_AUDIO_SILENT is set: the mix is being CAPTURED BUT NOT PLAYED.\n"
		                "[snd] ⚠️⚠️ Unset it (or start a fresh shell) to actually hear the game.\n");
	return true;
}

void CMacSoundSys::Term()
{
	if (m_OutputUnit)
	{
		AudioOutputUnitStop(m_OutputUnit);
		AudioUnitUninitialize(m_OutputUnit);
		AudioComponentInstanceDispose(m_OutputUnit);
		m_OutputUnit = NULL;
	}
	m_bStarted = false;
	m_bInited  = false;

	ReapRetiredBuffers(true);   // RT thread is stopped; safe to free unconditionally

	ReportMixStats();
	FinishDump();
}

S32 CMacSoundSys::Startup(void)
{
	if (!m_bInited && !Init())
		return LS_ERROR;

	if (!m_bStarted && m_OutputUnit)
	{
		OSStatus err = AudioOutputUnitStart(m_OutputUnit);
		if (err != noErr)
		{
			fprintf(stderr, "[snd] AudioOutputUnitStart failed (%d)\n", (int)err);
			return LS_ERROR;
		}
		m_bStarted = true;
	}
	return LS_OK;
}

void CMacSoundSys::Shutdown(void)
{
	if (m_bStarted && m_OutputUnit)
	{
		AudioOutputUnitStop(m_OutputUnit);
		m_bStarted = false;
	}
}

// --------------------------------------------------------------------------
// The mixer
// --------------------------------------------------------------------------

// One source frame -> float, channel-averaged to mono (we pan ourselves).
static inline float snd_SampleAt(const CMacVoice* pV, uint32 nFrame)
{
	if (nFrame >= pV->m_nFrames)
		return 0.0f;

	if (pV->m_nSrcBits == 16)
	{
		const sint16* p = (const sint16*)(pV->m_pData) + (size_t)nFrame * pV->m_nSrcChannels;
		if (pV->m_nSrcChannels == 1)
			return (float)p[0] * (1.0f / 32768.0f);
		return ((float)p[0] + (float)p[1]) * (0.5f / 32768.0f);
	}

	// 8-bit WAV PCM is UNSIGNED, centred on 128.
	const uint8* p = pV->m_pData + (size_t)nFrame * pV->m_nSrcChannels;
	if (pV->m_nSrcChannels == 1)
		return ((float)p[0] - 128.0f) * (1.0f / 128.0f);
	return (((float)p[0] - 128.0f) + ((float)p[1] - 128.0f)) * (0.5f / 128.0f);
}

OSStatus CMacSoundSys::Render(AudioUnitRenderActionFlags* ioFlags, const AudioTimeStamp* inTs,
                              UInt32 inBus, UInt32 inNumberFrames, AudioBufferList* ioData)
{
	float* pL = (float*)ioData->mBuffers[0].mData;
	float* pR = (ioData->mNumberBuffers > 1) ? (float*)ioData->mBuffers[1].mData : pL;

	memset(pL, 0, inNumberFrames * sizeof(float));
	if (pR != pL)
		memset(pR, 0, inNumberFrames * sizeof(float));

	uint32 nActive = 0;

	for (uint32 nVoice = 0; nVoice < kMaxVoices; ++nVoice)
	{
		CMacVoice* pV = &m_aVoices[nVoice];

		// m_bPlaying is the publication flag. ⚠️ Reading it here does NOT pin the
		// voice for the rest of this callback -- the game thread can clear it and
		// free the buffer mid-loop. What makes the loop below safe is that every
		// site which frees or republishes m_pData calls WaitForRTQuiesce() after
		// clearing the flag (§66). Do not add a new one without doing the same.
		if (!pV->m_bPlaying || pV->m_bPaused || !pV->m_pData || !pV->m_nFrames)
			continue;

		++nActive;

		const float fGainL = pV->m_fGainL * m_fMasterVolume;
		const float fGainR = pV->m_fGainR * m_fMasterVolume;

		double fPos  = pV->m_fPos;
		double fStep = pV->m_fStep;

		const uint32 nLoopEnd   = (pV->m_bLoopBlock && pV->m_nLoopEnd > pV->m_nLoopStart)
		                            ? pV->m_nLoopEnd : pV->m_nFrames;
		const uint32 nLoopStart = pV->m_bLoopBlock ? pV->m_nLoopStart : 0;

		for (UInt32 i = 0; i < inNumberFrames; ++i)
		{
			if (fPos >= (double)nLoopEnd)
			{
				if (!pV->m_bLoop)
				{
					++pV->m_nEndStops;        // §78: who stopped this voice?
					pV->m_bPlaying = false;   // ran off the end
					break;
				}
				fPos = (double)nLoopStart + (fPos - (double)nLoopEnd);
				if (fPos < 0.0) fPos = 0.0;
			}

			// Linear interpolation between neighbouring source frames.
			uint32 n0 = (uint32)fPos;
			float  fFrac = (float)(fPos - (double)n0);
			float  s0 = snd_SampleAt(pV, n0);
			float  s1 = snd_SampleAt(pV, n0 + 1);
			float  s  = s0 + (s1 - s0) * fFrac;

			pL[i] += s * fGainL;
			pR[i] += s * fGainR;

			fPos += fStep;
		}

		pV->m_fPos = fPos;
	}

	// ★ Publish that this callback is finished. WaitForRTQuiesce() watches this
	// to know when the mixer can no longer be inside a voice it is about to free.
	m_nRTCallbacks.fetch_add(1, std::memory_order_release);

	// Clip and gather the tell.
	float fPeak = m_fPeak;
	for (UInt32 i = 0; i < inNumberFrames; ++i)
	{
		// Hard clip, as the original mixer did. Counted, because a clipped mix
		// is audible as crackle and is the first thing to rule out when someone
		// reports "a few cracks" -- guessing at buffer bugs is a waste of time
		// if the answer is simply that the sum went past full scale.
		if (pL[i] >  1.0f || pL[i] < -1.0f || pR[i] > 1.0f || pR[i] < -1.0f)
			++m_nClipped;
		if (pL[i] >  1.0f) pL[i] =  1.0f;
		if (pL[i] < -1.0f) pL[i] = -1.0f;
		if (pR[i] >  1.0f) pR[i] =  1.0f;
		if (pR[i] < -1.0f) pR[i] = -1.0f;
		float a = fabsf(pL[i]); if (a > fPeak) fPeak = a;
		float b = fabsf(pR[i]); if (b > fPeak) fPeak = b;
		if (a > m_fIntervalPeak) m_fIntervalPeak = a;
		if (b > m_fIntervalPeak) m_fIntervalPeak = b;
	}
	m_fPeak = fPeak;
	m_nFramesRendered += inNumberFrames;
	if (nActive > m_nPeakVoices)
		m_nPeakVoices = nActive;

	// Debug capture -- into a preallocated buffer only. No file I/O on the
	// real-time thread; the WAV is written at Term().
	// ★★ A RING THAT KEEPS THE LAST 60 SECONDS (§73), not the first.
	//
	// ⚠️ It used to stop dead once the buffer was full, so a capture only ever
	// held the opening 60 s — which is precisely the part of a session nobody is
	// reporting. "I heard it just now, five minutes in" is the whole use case,
	// and the old form could not answer it. Wrapping costs one modulo.
	if (m_pDumpBuf)
	{
		uint32 nCopy = inNumberFrames;
		if (nCopy > kDumpMaxFrames)
			nCopy = kDumpMaxFrames;
		uint32 nAt = (uint32)(m_nDumpFrames % (uint64)kDumpMaxFrames);
		for (uint32 i = 0; i < nCopy; ++i)
		{
			sint16* pOut = m_pDumpBuf + (size_t)nAt * 2;
			pOut[0] = (sint16)(pL[i] * 32767.0f);
			pOut[1] = (sint16)(pR[i] * 32767.0f);
			if (++nAt >= kDumpMaxFrames)
				nAt = 0;
		}
		m_nDumpFrames += nCopy;
	}

	// ⚠️ HEARTBEAT. The shutdown "mix stats" line never appears on a run that is
	// killed (timed harness, force-quit, crash) -- which is exactly the run a
	// "no sound" report comes from. Print the same numbers every 5s instead, so
	// ONE run always answers: is the callback firing, are voices playing, is
	// the buffer non-silent, and is it actually being sent to the device?
	{
		static uint64 s_nNextBeat = 0;
		if (m_nFramesRendered >= s_nNextBeat)
		{
			s_nNextBeat = m_nFramesRendered + (uint64)kOutputRate * 5;
			if (getenv("LT_TRACE_SOUND"))
				fprintf(stderr, "[snd] alive: %.0fs rendered, %u voices now, peak %.1f%% FS "
				                "(this interval), %llu clipped, output=%s\n",
				        (double)m_nFramesRendered / (double)kOutputRate, nActive,
				        m_fIntervalPeak * 100.0f, (unsigned long long)m_nClipped,
				        m_bSilent ? "MUTED (LT_AUDIO_SILENT)" : "device");
			// ⚠️ Reset it: a monotonic high-water mark reads "67.7% FS" forever
			// after one loud moment and made a DEAD mix look healthy.
			m_fIntervalPeak = 0.0f;
		}
	}

	// LT_AUDIO_SILENT=1 -- capture the mix but emit silence. The analogue of
	// LT_NO_MOUSE_CAPTURE: an automated run should not blast game audio at
	// whoever is at the machine, but we still want the dump to analyse.
	if (m_bSilent)
	{
		memset(pL, 0, inNumberFrames * sizeof(float));
		if (pR != pL)
			memset(pR, 0, inNumberFrames * sizeof(float));
	}

	return noErr;
}

// --------------------------------------------------------------------------
// Debug capture (LT_AUDIO_DUMP)
// --------------------------------------------------------------------------

// ⚠️ Timed/automated runs are killed by SIGALRM (the `perl -e 'alarm N'`
// harness), so Term() never runs and a dump written only at shutdown never
// exists. The capture therefore has to survive a signal: the file is opened and
// its header written UP FRONT, and the flush uses write()/pwrite() -- both
// async-signal-safe -- so it can be driven from a handler.
static CMacSoundSys* gs_pDumpOwner = NULL;

void CMacSoundSys::StartDump()
{
	const char* pPath = getenv("LT_AUDIO_DUMP");
	if (!pPath || !pPath[0])
		return;

	m_pDumpBuf = (sint16*)malloc((size_t)kDumpMaxFrames * 2 * sizeof(sint16));
	if (!m_pDumpBuf)
		return;
	memset(m_pDumpBuf, 0, (size_t)kDumpMaxFrames * 2 * sizeof(sint16));
	m_nDumpFrames = 0;
	strncpy(m_szDumpPath, pPath, sizeof(m_szDumpPath) - 1);
	m_szDumpPath[sizeof(m_szDumpPath) - 1] = 0;

	m_nDumpFd = open(m_szDumpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (m_nDumpFd < 0)
	{
		free(m_pDumpBuf);
		m_pDumpBuf = NULL;
		return;
	}

	// 44-byte canonical WAV header; the two size fields are patched on flush.
	uint8 aHdr[44];
	uint16 nCh = 2, nBits = 16, nFmt = 1;
	uint32 nRate = kOutputRate;
	uint32 nByteRate = nRate * nCh * (nBits / 8);
	uint16 nAlign = nCh * (nBits / 8);
	uint32 n16 = 16, nZero = 0;
	memcpy(aHdr +  0, "RIFF", 4);  memcpy(aHdr +  4, &nZero, 4);
	memcpy(aHdr +  8, "WAVE", 4);
	memcpy(aHdr + 12, "fmt ", 4);  memcpy(aHdr + 16, &n16, 4);
	memcpy(aHdr + 20, &nFmt, 2);   memcpy(aHdr + 22, &nCh, 2);
	memcpy(aHdr + 24, &nRate, 4);  memcpy(aHdr + 28, &nByteRate, 4);
	memcpy(aHdr + 32, &nAlign, 2); memcpy(aHdr + 34, &nBits, 2);
	memcpy(aHdr + 36, "data", 4);  memcpy(aHdr + 40, &nZero, 4);
	(void)!write(m_nDumpFd, aHdr, sizeof(aHdr));

	gs_pDumpOwner = this;
	InstallDumpSignalHandlers();

	// ⚠️ AND atexit — BECAUSE A CLEAN QUIT IS NOT COVERED BY EITHER OF THE OTHER
	// TWO PATHS (§73). The flush lives in CMacSoundSys::Term(), and the engine's
	// normal shutdown NEVER CALLS IT: a real user capture came back as a 44-byte
	// header-only WAV, and the tell in its log was "[mac] shutting down..." with
	// no "[snd] mix stats" line before it. Signal handlers covered the killed
	// runs I tested with, so the hole only showed up on the one run that
	// mattered. FinishDump is idempotent (m_bDumpFlushed), so all three routes
	// can fire safely.
	static bool s_bAtExit = false;
	if (!s_bAtExit)
	{
		s_bAtExit = true;
		atexit(&CMacSoundSys::FinishDumpAtExit);
	}
}

void CMacSoundSys::FinishDumpAtExit()
{
	if (gs_pDumpOwner)
	{
		gs_pDumpOwner->ReportMixStats();
		gs_pDumpOwner->FinishDump();
	}
}

void CMacSoundSys::FinishDump()
{
	if (m_nDumpFd < 0 || m_bDumpFlushed)
		return;
	m_bDumpFlushed = true;

	const uint64 nTotal = m_nDumpFrames;     // snapshot; the RT thread may still run
	uint32 nFrames = (nTotal > (uint64)kDumpMaxFrames) ? kDumpMaxFrames : (uint32)nTotal;
	uint32 nData = nFrames * 2 * (uint32)sizeof(sint16);

	if (m_pDumpBuf && nData)
	{
		if (nTotal <= (uint64)kDumpMaxFrames)
		{
			(void)!write(m_nDumpFd, m_pDumpBuf, nData);
		}
		else
		{
			// Wrapped: the OLDEST frame is the one about to be overwritten, so
			// write from there to the end of the ring, then the head. Two
			// write()s, both async-signal-safe like the single one they replace.
			const uint32 nOldest = (uint32)(nTotal % (uint64)kDumpMaxFrames);
			const uint32 nTail   = (kDumpMaxFrames - nOldest) * 2 * (uint32)sizeof(sint16);
			(void)!write(m_nDumpFd, m_pDumpBuf + (size_t)nOldest * 2, nTail);
			if (nOldest)
				(void)!write(m_nDumpFd, m_pDumpBuf, nOldest * 2 * (uint32)sizeof(sint16));
		}
	}

	// Patch the two RIFF size fields in place (pwrite is async-signal-safe).
	uint32 nRiff = 36 + nData;
	(void)!pwrite(m_nDumpFd, &nRiff, 4, 4);
	(void)!pwrite(m_nDumpFd, &nData, 4, 40);
	close(m_nDumpFd);
	m_nDumpFd = -1;
}

void CMacSoundSys::ReportMixStats()
{
	fprintf(stderr, "[snd] mix stats: %.1fs rendered, peak %.1f%% FS, %llu clipped samples, "
	                "%u voices max, output=%s\n",
	        (double)m_nFramesRendered / (double)kOutputRate, m_fPeak * 100.0f,
	        (unsigned long long)m_nClipped, m_nPeakVoices,
	        m_bSilent ? "MUTED (LT_AUDIO_SILENT)" : "device");
}

// Flush the capture, then let the signal do what it was going to do.
static void snd_DumpSignalHandler(int nSig)
{
	if (gs_pDumpOwner)
	{
		gs_pDumpOwner->ReportMixStats();   // killed runs get the numbers too
		gs_pDumpOwner->FinishDump();
	}
	signal(nSig, SIG_DFL);
	raise(nSig);
}

void CMacSoundSys::InstallDumpSignalHandlers()
{
	signal(SIGALRM, snd_DumpSignalHandler);   // the timed-run harness
	signal(SIGINT,  snd_DumpSignalHandler);
	signal(SIGTERM, snd_DumpSignalHandler);
}

// --------------------------------------------------------------------------
// Voice pool
// --------------------------------------------------------------------------

CMacVoice* CMacSoundSys::VoiceFromHandle(void* h)
{
	CMacVoice* pV = (CMacVoice*)h;
	if (!pV || pV < &m_aVoices[0] || pV > &m_aVoices[kMaxVoices - 1])
		return NULL;
	return pV;
}

CMacVoice* CMacSoundSys::AllocVoice(bool b3D, bool bStream)
{
	// Disjoint ranges: streams can never starve the engine's sample pool.
	uint32 nFirst = bStream ? kMaxSampleVoices : 0;
	uint32 nEnd   = bStream ? kMaxVoices       : kMaxSampleVoices;

	ReapRetiredBuffers(false);

	for (uint32 i = nFirst; i < nEnd; ++i)
	{
		if (!m_aVoices[i].m_bAllocated)
		{
			// ⚠️ Reusing a voice whose retired buffer is still parked would leak
			// it and could republish m_pData under a live mixer. Rare (it needs a
			// reuse within ~two callbacks of the close), so blocking is fine here.
			if (m_aVoices[i].m_pRetiredData)
			{
				WaitForRTQuiesce();
				ReapRetiredBuffers(true);
			}
			m_aVoices[i].Reset();
			m_aVoices[i].m_bAllocated = true;
			m_aVoices[i].m_b3D = b3D;
			return &m_aVoices[i];
		}
	}

	// Loud, not trace-gated: running out here degrades or silences the game and
	// has no other visible symptom.
	fprintf(stderr, "[snd] ⚠️ voice pool exhausted (%s range) -- sound will be lost\n",
	        bStream ? "stream" : "sample");
	return NULL;
}

// --------------------------------------------------------------------------
// 2D samples
// --------------------------------------------------------------------------

LHSAMPLE CMacSoundSys::AllocateSampleHandle(LHDIGDRIVER hDig)
{
	CMacVoice* pV = AllocVoice(false);
	if (snd_Trace())
		fprintf(stderr, "[snd] AllocateSampleHandle -> %s\n", pV ? "ok" : "POOL EXHAUSTED");
	return pV;
}

void CMacSoundSys::ReleaseSampleHandle(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	pV->m_bPlaying = false;
	pV->m_bAllocated = false;
}

void CMacSoundSys::InitSample(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	// ⚠️ Reset() memsets the voice, which would strand a parked buffer (§66).
	// AllocVoice already reaps before handing a voice out, so this should never
	// fire — it is here so the invariant holds even if that changes.
	if (pV->m_pRetiredData)
	{
		WaitForRTQuiesce();
		ReapRetiredBuffers(true);
	}
	bool b3D = pV->m_b3D;
	pV->Reset();
	pV->m_bAllocated = true;
	pV->m_b3D = b3D;
}

void CMacSoundSys::StopSample(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	pV->m_bPlaying = false;
	pV->m_bPaused  = false;
	pV->m_fPos     = 0.0;
}

void CMacSoundSys::StartSample(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	pV->m_fPos = 0.0;
	pV->m_bPaused = false;
	pV->m_bEverStarted = true;
	pV->ResolveGains();
	pV->m_bPlaying = true;      // publication: LAST
	if (snd_Trace())
		fprintf(stderr, "[snd] start 2D voice %ld: %u frames @%u Hz %uch/%ub vol=%.2f\n",
		        (long)(pV - &m_aVoices[0]), pV->m_nFrames, pV->m_nSrcRate,
		        pV->m_nSrcChannels, pV->m_nSrcBits, pV->m_fVolume);
}

// ⚠️ RESUME IS "PLAY", NOT "UNPAUSE". This is the call that actually starts a
// 2D sound: CSoundInstance::Play never calls StartSample for m_hSample -- it
// does SetSampleMsPosition + ResumeSample, and only 3D samples get
// Start3DSample. s_dx8 confirms the split: ResumeSample == Play(), while
// StartSample == Stop(rewind) + Play(). Treating Resume as a mere unpause
// meant no 2D sound ever played at all.
void CMacSoundSys::ResumeSample(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV || !pV->m_pData || !pV->m_nFrames)
		return;
	pV->m_bPaused = false;
	if (!pV->m_bPlaying)
	{
		pV->m_bEverStarted = true;
		pV->ResolveGains();
		pV->m_bPlaying = true;      // publication: LAST
		if (snd_Trace())
			fprintf(stderr, "[snd] start 2D (resume) voice %ld: %u frames @%u Hz %uch/%ub vol=%.2f"
			                " loop=%d loopblock=%d[%u..%u] pos=%.0f endstops=%u\n",
			        (long)(pV - &m_aVoices[0]), pV->m_nFrames, pV->m_nSrcRate,
			        pV->m_nSrcChannels, pV->m_nSrcBits, pV->m_fVolume,
			        (int)pV->m_bLoop, (int)pV->m_bLoopBlock,
			        pV->m_nLoopStart, pV->m_nLoopEnd, pV->m_fPos, pV->m_nEndStops);
	}
}

void CMacSoundSys::EndSample(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	pV->m_bPlaying = false;
	pV->m_fPos = 0.0;
}

void CMacSoundSys::SetSampleVolume(LHSAMPLE hS, S32 siVolume)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	// Miles volume is 0..127.
	float f = (float)siVolume / 127.0f;
	pV->m_fVolume = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
	pV->ResolveGains();
}

void CMacSoundSys::SetSamplePan(LHSAMPLE hS, S32 siPan)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	// Miles pan is 0..127 with 64 = centre.
	float f = (float)siPan / 127.0f;
	pV->m_fPan = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
	pV->ResolveGains();
}

S32 CMacSoundSys::GetSampleVolume(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	return pV ? (S32)(pV->m_fVolume * 127.0f) : 0;
}

S32 CMacSoundSys::GetSamplePan(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	return pV ? (S32)(pV->m_fPan * 127.0f) : 64;
}

void CMacSoundSys::SetSampleUserData(LHSAMPLE hS, U32 uiIndex, LTSOUNDUSERDATA siValue)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (pV && uiIndex < 8) pV->m_aUserData[uiIndex] = siValue;
}

LTSOUNDUSERDATA CMacSoundSys::GetSampleUserData(LHSAMPLE hS, U32 uiIndex)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	return (pV && uiIndex < 8) ? pV->m_aUserData[uiIndex] : 0;
}

U32 CMacSoundSys::GetSampleStatus(LHSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return LS_DONE;
	if (pV->m_bPlaying) return LS_PLAYING;
	return pV->m_bEverStarted ? LS_DONE : LS_DONE;
}

// ⚠️ RETURN-VALUE CONVENTIONS ARE NOT UNIFORM IN ILTSoundSys.
// The Init*From* family and Open3DProvider return a BOOLEAN (LTTRUE/LTFALSE),
// while Startup/WaveOutOpen/Set3DSampleInfo use LS_OK/LS_ERROR -- and LS_OK is
// ZERO. Callers test them accordingly (`if (!InitSampleFromAddress(...))` vs
// `if (WaveOutOpen(...) != LS_OK)`), so returning LS_OK from an Init reads as
// FAILURE and the engine retries forever without ever starting a voice.
// Verified against sys/s_dx8 (the reference driver) per method -- do not
// "tidy" these into one convention.
// The engine hands us already-decoded PCM plus its format; we do not own the
// memory, so a voice must be stopped before the engine frees its buffer.
S32 CMacSoundSys::InitSampleFromAddress(LHSAMPLE hS, void* pStart, U32 uiLen,
                                        WAVEFORMATEX* pWaveFormat, S32 siPlaybackRate,
                                        LTSOUNDFILTERDATA* pFilterData)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (snd_Trace())
		fprintf(stderr, "[snd] InitSampleFromAddress: voice=%s len=%u fmt=%s\n",
		        pV ? "ok" : "BAD-HANDLE", uiLen,
		        pWaveFormat ? "ok" : "NULL");
	if (!pV || !pStart || !pWaveFormat || !uiLen)
		return LTFALSE;

	uint32 nChannels = pWaveFormat->nChannels ? pWaveFormat->nChannels : 1;
	uint32 nBits     = pWaveFormat->wBitsPerSample ? pWaveFormat->wBitsPerSample : 16;
	uint32 nRate     = pWaveFormat->nSamplesPerSec ? pWaveFormat->nSamplesPerSec : kOutputRate;

	// ★★ GATE ON THE FORMAT TAG, NOT JUST THE BIT DEPTH (§73).
	//
	// ⚠️ The bits check alone has a hole big enough to drive an MP3 through:
	// `wBitsPerSample` is 0 for WAVE_FORMAT_MPEGLAYER3, and the line above
	// substitutes 16 for a zero — so a compressed sample arriving here passes as
	// "16-bit PCM" and gets mixed byte-for-byte as raw samples. That is not a
	// subtle artefact, it is a burst of full-scale white noise. The STREAMING
	// path already refuses this correctly (`nTag == kWaveFormatPCM`, and its
	// comment says "which is a burst of white noise"); this path never did.
	//
	// ⚠️ This is NOT trace-gated. A sound that plays as static is a defect a
	// player will hear and report, and the silent-failure-path rule says the
	// diagnostic has to be there when it happens, not on a rerun. Once per
	// distinct tag, so a repeating effect cannot flood the log.
	const uint32 nTag = pWaveFormat->wFormatTag;
	if (nTag != kWaveFormatPCM || (nBits != 8 && nBits != 16))
	{
		static uint32 s_aReported[8] = { 0 };
		static uint32 s_nReported = 0;
		bool bSeen = false;
		for (uint32 n = 0; n < s_nReported; ++n)
			if (s_aReported[n] == nTag) { bSeen = true; break; }
		if (!bSeen)
		{
			if (s_nReported < 8)
				s_aReported[s_nReported++] = nTag;
			fprintf(stderr, "[snd] REFUSING an unsupported sample format: tag %u, %u bits "
			                "(only PCM 8/16 can be mixed — playing it would be static)\n",
			        nTag, pWaveFormat->wBitsPerSample);
		}
		return LTFALSE;
	}

	pV->m_bPlaying     = false;      // stop before republishing the data
	WaitForRTQuiesce();              // ...and wait for the mixer to leave it (§66)
	pV->m_pData        = (const uint8*)pStart;
	pV->m_nSrcChannels = nChannels;
	pV->m_nSrcBits     = nBits;
	pV->m_nSrcRate     = (siPlaybackRate > 0) ? (uint32)siPlaybackRate : nRate;
	pV->m_nFrames      = uiLen / (nChannels * (nBits / 8));
	pV->m_fPos         = 0.0;
	pV->m_fStep        = (double)pV->m_nSrcRate / (double)kOutputRate;
	pV->ResolveGains();

	return LTTRUE;
}

S32 CMacSoundSys::InitSampleFromFile(LHSAMPLE hS, void* pFile_image, S32 siBlock,
                                     S32 siPlaybackRate, LTSOUNDFILTERDATA* pFilterData)
{
	// Unused by this engine's client path (it always decodes first).
	// LTFALSE, not LS_ERROR: LS_ERROR is 1, which this caller reads as SUCCESS.
	return LTFALSE;
}

void CMacSoundSys::SetSampleLoopBlock(LHSAMPLE hS, S32 siLoopStart, S32 siLoopEnd, bool bEnable)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;

	// ⚠️ Loop-block offsets are BYTE positions into the sound's ORIGINAL data
	// (CSoundInstance computes them against GetSoundDataLen). For an
	// MP3-decoded voice the original data is the COMPRESSED bitstream, so
	// those offsets cannot be mapped onto decoded frames -- dividing them by
	// the PCM frame size landed the loop end at ~9% of the real audio and a
	// looping ambient repeated its first fragment forever ("a strange sound
	// artifact that stays for a while"). For decoded voices, loop the WHOLE
	// buffer instead; the cue region is unrecoverable without frame-accurate
	// MP3 seek tables, and full-buffer looping is what the same sound does
	// when it has no cue points at all.
	if (pV->m_bDecoded)
	{
		pV->m_bLoopBlock = false;
		if (bEnable && snd_Trace())
			fprintf(stderr, "[snd] loop block ignored on MP3-decoded voice %ld (byte offsets are compressed-domain)\n",
			        (long)(pV - &m_aVoices[0]));
		return;
	}

	uint32 nBytesPerFrame = pV->m_nSrcChannels * (pV->m_nSrcBits / 8);
	if (!nBytesPerFrame) nBytesPerFrame = 2;
	pV->m_bLoopBlock = bEnable;
	pV->m_nLoopStart = (siLoopStart > 0) ? ((uint32)siLoopStart / nBytesPerFrame) : 0;
	pV->m_nLoopEnd   = (siLoopEnd   > 0) ? ((uint32)siLoopEnd   / nBytesPerFrame) : pV->m_nFrames;

	// Clamp: a loop block beyond the data would loop silence (or worse).
	if (pV->m_nLoopEnd > pV->m_nFrames)                pV->m_nLoopEnd   = pV->m_nFrames;
	if (pV->m_nLoopStart >= pV->m_nLoopEnd)            { pV->m_nLoopStart = 0; }
}

void CMacSoundSys::SetSampleLoop(LHSAMPLE hS, bool bLoop)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (pV) pV->m_bLoop = bLoop;
}

void CMacSoundSys::SetSampleMsPosition(LHSAMPLE hS, S32 siMilliseconds)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV || siMilliseconds < 0) return;
	double fFrame = (double)siMilliseconds * (double)pV->m_nSrcRate / 1000.0;
	pV->m_fPos = (fFrame < (double)pV->m_nFrames) ? fFrame : 0.0;
}

void CMacSoundSys::SetSampleReverb(LHSAMPLE hS, float, float, float) {}
void CMacSoundSys::GetDirectSoundInfo(LHSAMPLE, PTDIRECTSOUND* ppDS, PTDIRECTSOUNDBUFFER* ppDSB)
{
	if (ppDS)  *ppDS  = NULL;
	if (ppDSB) *ppDSB = NULL;
}

// --------------------------------------------------------------------------
// 3D samples -- same voices, with distance/pan resolved from the listener
// --------------------------------------------------------------------------

void CMacSoundSys::Update3DGains(CMacVoice* pV)
{
	if (!pV) return;

	float dx = pV->m_vPos[0] - m_vListenerPos[0];
	float dy = pV->m_vPos[1] - m_vListenerPos[1];
	float dz = pV->m_vPos[2] - m_vListenerPos[2];
	float fDist = sqrtf(dx * dx + dy * dy + dz * dz);

	// LithTech's model: full volume inside min, silent past max, inverse
	// between. Matches what Set3DSampleDistances hands us.
	float fAtten;
	if (fDist <= pV->m_fMinDist)
		fAtten = 1.0f;
	else if (fDist >= pV->m_fMaxDist)
		fAtten = 0.0f;
	else
		fAtten = pV->m_fMinDist / fDist;

	// Obstruction/occlusion just darken for now (no filtering yet).
	fAtten *= (1.0f - 0.5f * pV->m_fObstruction) * (1.0f - 0.7f * pV->m_fOcclusion);

	// Pan from the component along the listener's right vector.
	float fPan = 0.5f;
	if (fDist > 0.0001f)
	{
		float fRight = (dx * m_vListenerRight[0] + dy * m_vListenerRight[1] + dz * m_vListenerRight[2]) / fDist;
		fPan = 0.5f + 0.5f * fRight;
		if (fPan < 0.0f) fPan = 0.0f;
		if (fPan > 1.0f) fPan = 1.0f;
	}

	float fAngle = fPan * 1.5707963f;
	pV->m_fGainL = cosf(fAngle) * pV->m_fVolume * fAtten;
	pV->m_fGainR = sinf(fAngle) * pV->m_fVolume * fAtten;
}

LH3DSAMPLE CMacSoundSys::Allocate3DSampleHandle(LHPROVIDER hLib)
{
	CMacVoice* pV = AllocVoice(true);
	if (snd_Trace())
		fprintf(stderr, "[snd] Allocate3DSampleHandle -> %s\n", pV ? "ok" : "POOL EXHAUSTED");
	return pV;
}

void CMacSoundSys::Release3DSampleHandle(LH3DSAMPLE hS) { ReleaseSampleHandle(hS); }
void CMacSoundSys::Stop3DSample(LH3DSAMPLE hS)          { StopSample(hS); }
void CMacSoundSys::Resume3DSample(LH3DSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (pV) Update3DGains(pV);
	ResumeSample(hS);
}
void CMacSoundSys::End3DSample(LH3DSAMPLE hS)           { EndSample(hS); }

void CMacSoundSys::Start3DSample(LH3DSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	Update3DGains(pV);
	pV->m_fPos = 0.0;
	pV->m_bPaused = false;
	pV->m_bEverStarted = true;
	pV->m_bPlaying = true;
	if (snd_Trace())
		fprintf(stderr, "[snd] start 3D voice %ld at (%.0f %.0f %.0f) dist=%.0f..%.0f\n",
		        (long)(pV - &m_aVoices[0]), pV->m_vPos[0], pV->m_vPos[1], pV->m_vPos[2],
		        pV->m_fMinDist, pV->m_fMaxDist);
}

S32 CMacSoundSys::Init3DSampleFromAddress(LH3DSAMPLE hS, void* pStart, U32 uiLen,
                                          WAVEFORMATEX* pWaveFormat, S32 siPlaybackRate,
                                          LTSOUNDFILTERDATA* pFilterData)
{
	return InitSampleFromAddress(hS, pStart, uiLen, pWaveFormat, siPlaybackRate, pFilterData);
}

S32 CMacSoundSys::Init3DSampleFromFile(LH3DSAMPLE, void*, S32, S32, LTSOUNDFILTERDATA*) { return LTFALSE; }

S32 CMacSoundSys::Get3DSampleVolume(LH3DSAMPLE hS) { return GetSampleVolume(hS); }

void CMacSoundSys::Set3DSampleVolume(LH3DSAMPLE hS, S32 siVolume)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	float f = (float)siVolume / 127.0f;
	pV->m_fVolume = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
	Update3DGains(pV);
}

U32  CMacSoundSys::Get3DSampleStatus(LH3DSAMPLE hS) { return GetSampleStatus(hS); }
void CMacSoundSys::Set3DSampleMsPosition(LHSAMPLE hS, S32 ms) { SetSampleMsPosition(hS, ms); }
S32  CMacSoundSys::Set3DSampleInfo(LH3DSAMPLE, LTSOUNDINFO*) { return LS_OK; }

void CMacSoundSys::Set3DSampleDistances(LH3DSAMPLE hS, float fMax_dist, float fMin_dist)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (!pV) return;
	pV->m_fMaxDist = fMax_dist;
	pV->m_fMinDist = (fMin_dist > 0.0f) ? fMin_dist : 1.0f;
	Update3DGains(pV);
}

void CMacSoundSys::Set3DSamplePreference(LH3DSAMPLE, char*, void*) {}

void CMacSoundSys::Set3DSampleLoopBlock(LH3DSAMPLE hS, S32 s, S32 e, bool b) { SetSampleLoopBlock(hS, s, e, b); }
void CMacSoundSys::Set3DSampleLoop(LH3DSAMPLE hS, bool bLoop)                { SetSampleLoop(hS, bLoop); }

void CMacSoundSys::Set3DSampleObstruction(LH3DSAMPLE hS, float f)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (pV) { pV->m_fObstruction = f; Update3DGains(pV); }
}
float CMacSoundSys::Get3DSampleObstruction(LH3DSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	return pV ? pV->m_fObstruction : 0.0f;
}
void CMacSoundSys::Set3DSampleOcclusion(LH3DSAMPLE hS, float f)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	if (pV) { pV->m_fOcclusion = f; Update3DGains(pV); }
}
float CMacSoundSys::Get3DSampleOcclusion(LH3DSAMPLE hS)
{
	CMacVoice* pV = VoiceFromHandle(hS);
	return pV ? pV->m_fOcclusion : 0.0f;
}

// --------------------------------------------------------------------------
// 3D listener + objects.
//
// The engine opens ONE 3D "object" for the listener and then reuses the same
// Set3DPosition/Set3DOrientation calls for both it and 3D samples, keyed on the
// handle. We distinguish by the handle: the listener is a sentinel, samples are
// voices in the pool.
// --------------------------------------------------------------------------

static int gs_ListenerSentinel = 0;
#define LISTENER_HANDLE ((LH3DPOBJECT)&gs_ListenerSentinel)

LH3DPOBJECT CMacSoundSys::Open3DListener(LHPROVIDER hLib) { return LISTENER_HANDLE; }
void        CMacSoundSys::Close3DListener(LH3DPOBJECT)    {}
void        CMacSoundSys::SetListenerDoppler(LH3DPOBJECT, float fDoppler) { m_fDoppler = fDoppler; }

void CMacSoundSys::CommitDeferred()
{
	// Re-resolve every live 3D voice against the current listener. The engine
	// batches its position updates and then calls this once, which is exactly
	// the right place to do the (cheap) gain maths off the audio thread.
	for (uint32 i = 0; i < kMaxVoices; ++i)
	{
		CMacVoice* pV = &m_aVoices[i];
		if (pV->m_bAllocated && pV->m_b3D && pV->m_bPlaying)
			Update3DGains(pV);
	}
}

void CMacSoundSys::Set3DPosition(LH3DPOBJECT hObj, float fX, float fY, float fZ)
{
	if (hObj == LISTENER_HANDLE)
	{
		m_vListenerPos[0] = fX; m_vListenerPos[1] = fY; m_vListenerPos[2] = fZ;
		return;
	}
	CMacVoice* pV = VoiceFromHandle(hObj);
	if (!pV) return;
	pV->m_vPos[0] = fX; pV->m_vPos[1] = fY; pV->m_vPos[2] = fZ;
	Update3DGains(pV);
}

void CMacSoundSys::Set3DVelocityVector(LH3DPOBJECT hObj, float fDX, float fDY, float fDZ)
{
	if (hObj == LISTENER_HANDLE) return;
	CMacVoice* pV = VoiceFromHandle(hObj);
	if (!pV) return;
	pV->m_vVel[0] = fDX; pV->m_vVel[1] = fDY; pV->m_vVel[2] = fDZ;
}

void CMacSoundSys::Set3DOrientation(LH3DPOBJECT hObj, float fXf, float fYf, float fZf,
                                    float fXu, float fYu, float fZu)
{
	if (hObj != LISTENER_HANDLE)
		return;

	m_vListenerForward[0] = fXf; m_vListenerForward[1] = fYf; m_vListenerForward[2] = fZf;

	// right = up x forward (left-handed, matching the engine's convention)
	m_vListenerRight[0] = fYu * fZf - fZu * fYf;
	m_vListenerRight[1] = fZu * fXf - fXu * fZf;
	m_vListenerRight[2] = fXu * fYf - fYu * fXf;

	float fLen = sqrtf(m_vListenerRight[0] * m_vListenerRight[0] +
	                   m_vListenerRight[1] * m_vListenerRight[1] +
	                   m_vListenerRight[2] * m_vListenerRight[2]);
	if (fLen > 0.0001f)
	{
		m_vListenerRight[0] /= fLen;
		m_vListenerRight[1] /= fLen;
		m_vListenerRight[2] /= fLen;
	}
}

void CMacSoundSys::Set3DUserData(LH3DPOBJECT hObj, U32 uiIndex, LTSOUNDUSERDATA siValue)
{
	if (uiIndex >= 8) return;
	if (hObj == LISTENER_HANDLE) { m_aListenerUserData[uiIndex] = siValue; return; }
	CMacVoice* pV = VoiceFromHandle(hObj);
	if (pV) pV->m_aUserData[uiIndex] = siValue;
}

LTSOUNDUSERDATA CMacSoundSys::Get3DUserData(LH3DPOBJECT hObj, U32 uiIndex)
{
	if (uiIndex >= 8) return 0;
	if (hObj == LISTENER_HANDLE) return m_aListenerUserData[uiIndex];
	CMacVoice* pV = VoiceFromHandle(hObj);
	return pV ? pV->m_aUserData[uiIndex] : 0;
}

void CMacSoundSys::Get3DPosition(LH3DPOBJECT hObj, float* pfX, float* pfY, float* pfZ)
{
	const float* p = m_vListenerPos;
	CMacVoice* pV = (hObj == LISTENER_HANDLE) ? NULL : VoiceFromHandle(hObj);
	if (pV) p = pV->m_vPos;
	if (pfX) *pfX = p[0];
	if (pfY) *pfY = p[1];
	if (pfZ) *pfZ = p[2];
}

void CMacSoundSys::Get3DVelocity(LH3DPOBJECT hObj, float* pfDX, float* pfDY, float* pfDZ)
{
	CMacVoice* pV = (hObj == LISTENER_HANDLE) ? NULL : VoiceFromHandle(hObj);
	if (pfDX) *pfDX = pV ? pV->m_vVel[0] : 0.0f;
	if (pfDY) *pfDY = pV ? pV->m_vVel[1] : 0.0f;
	if (pfDZ) *pfDZ = pV ? pV->m_vVel[2] : 0.0f;
}

void CMacSoundSys::Get3DOrientation(LH3DPOBJECT hObj, float* pfXf, float* pfYf, float* pfZf,
                                    float* pfXu, float* pfYu, float* pfZu)
{
	if (pfXf) *pfXf = m_vListenerForward[0];
	if (pfYf) *pfYf = m_vListenerForward[1];
	if (pfZf) *pfZf = m_vListenerForward[2];
	if (pfXu) *pfXu = 0.0f;
	if (pfYu) *pfYu = 1.0f;
	if (pfZu) *pfZu = 0.0f;
}

// --------------------------------------------------------------------------
// Bits we deliberately do not implement yet
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// STREAMS -- this is where NOLF2's DIALOGUE lives.
//
// The engine hands us a real on-disk FILENAME plus a byte OFFSET
// (CSoundInstance::AcquireStream -> df_GetRawInfo), because a stream may sit
// inside a .rez archive. Unlike samples, nothing has parsed the RIFF for us.
//
// ⚠️ FIRST CUT: the whole stream is decoded into memory and played as an
// ordinary voice, rather than buffered incrementally. Dialogue lines are a few
// seconds (a 10s 22 kHz mono line is ~440 KB), so this is cheap and removes an
// entire class of buffer-underrun bug from the bring-up. It is NOT suitable for
// long music tracks -- but NOLF2's music is DirectMusic (.sgt), which does not
// come through here at all. Revisit if a genuinely long .wav stream shows up.
// --------------------------------------------------------------------------


// --------------------------------------------------------------------------
// MP3 decode (AudioToolbox).
//
// NOLF2's dialogue is MP3 INSIDE A WAV CONTAINER: `fmt ` carries
// wFormatTag = 85 (WAVE_FORMAT_MPEGLAYER3), bits = 0, and the `data` chunk is a
// raw MPEG bitstream. Playing those bytes as PCM is a short burst of white
// noise -- which is exactly what it sounded like.
//
// This is the payoff for building on CoreAudio: AudioToolbox decodes MP3 with
// no third-party dependency. We hand it the data chunk as an in-memory MP3
// file and ask ExtAudioFile to convert to 16-bit PCM.
// --------------------------------------------------------------------------
struct SndMemFile
{
	const uint8* m_pData;
	uint32       m_nLen;
};

static OSStatus snd_MemRead(void* inClientData, SInt64 inPosition, UInt32 nRequest,
                            void* pBuffer, UInt32* pnActual)
{
	SndMemFile* pMem = (SndMemFile*)inClientData;
	if (inPosition < 0 || (uint32)inPosition >= pMem->m_nLen) { *pnActual = 0; return noErr; }
	uint32 nAvail = pMem->m_nLen - (uint32)inPosition;
	uint32 nCopy  = (nRequest < nAvail) ? nRequest : nAvail;
	memcpy(pBuffer, pMem->m_pData + inPosition, nCopy);
	*pnActual = nCopy;
	return noErr;
}

static SInt64 snd_MemGetSize(void* inClientData)
{
	return (SInt64)((SndMemFile*)inClientData)->m_nLen;
}

// Decode an MP3 bitstream to interleaved 16-bit PCM. Caller owns *ppOut (free).
static bool snd_DecodeMP3(const uint8* pMP3, uint32 nMP3Len,
                          sint16** ppOut, uint32* pnFrames,
                          uint32* pnChannels, uint32* pnRate)
{
	SndMemFile mem = { pMP3, nMP3Len };

	AudioFileID af = NULL;
	OSStatus err = AudioFileOpenWithCallbacks(&mem, snd_MemRead, NULL,
	                                          snd_MemGetSize, NULL,
	                                          kAudioFileMP3Type, &af);
	if (err != noErr)
	{
		if (snd_Trace()) fprintf(stderr, "[snd] AudioFileOpenWithCallbacks failed (%d)\n", (int)err);
		return false;
	}

	ExtAudioFileRef ext = NULL;
	err = ExtAudioFileWrapAudioFileID(af, false, &ext);
	if (err != noErr)
	{
		AudioFileClose(af);
		if (snd_Trace()) fprintf(stderr, "[snd] ExtAudioFileWrapAudioFileID failed (%d)\n", (int)err);
		return false;
	}

	AudioStreamBasicDescription src;
	UInt32 nSize = sizeof(src);
	if (ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileDataFormat, &nSize, &src) != noErr)
	{
		ExtAudioFileDispose(ext); AudioFileClose(af);
		return false;
	}

	uint32 nCh   = src.mChannelsPerFrame ? src.mChannelsPerFrame : 1;
	uint32 nRate = src.mSampleRate ? (uint32)src.mSampleRate : kOutputRate;

	// Ask for interleaved signed 16-bit at the source rate; our mixer does the
	// rate conversion itself, consistently with every other voice.
	AudioStreamBasicDescription client;
	memset(&client, 0, sizeof(client));
	client.mSampleRate       = (Float64)nRate;
	client.mFormatID         = kAudioFormatLinearPCM;
	client.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	client.mChannelsPerFrame = nCh;
	client.mBitsPerChannel   = 16;
	client.mFramesPerPacket  = 1;
	client.mBytesPerFrame    = 2 * nCh;
	client.mBytesPerPacket   = 2 * nCh;

	if (ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat,
	                            sizeof(client), &client) != noErr)
	{
		ExtAudioFileDispose(ext); AudioFileClose(af);
		return false;
	}

	SInt64 nTotal = 0;
	nSize = sizeof(nTotal);
	ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &nSize, &nTotal);
	if (nTotal <= 0 || nTotal > 60 * 60 * (SInt64)nRate)   // sanity: an hour
		nTotal = 0;

	// Grow-as-we-go: the reported length can be an estimate for MPEG.
	uint32 nCap = (uint32)((nTotal > 0) ? nTotal : nRate) + nRate;
	sint16* pOut = (sint16*)malloc((size_t)nCap * nCh * sizeof(sint16));
	if (!pOut) { ExtAudioFileDispose(ext); AudioFileClose(af); return false; }

	uint32 nGot = 0;
	for (;;)
	{
		if (nGot + 4096 > nCap)
		{
			uint32 nNew = nCap * 2;
			sint16* pBigger = (sint16*)realloc(pOut, (size_t)nNew * nCh * sizeof(sint16));
			if (!pBigger) break;
			pOut = pBigger;
			nCap = nNew;
		}

		AudioBufferList abl;
		abl.mNumberBuffers = 1;
		abl.mBuffers[0].mNumberChannels = nCh;
		abl.mBuffers[0].mDataByteSize   = 4096 * nCh * sizeof(sint16);
		abl.mBuffers[0].mData           = pOut + (size_t)nGot * nCh;

		UInt32 nFrames = 4096;
		if (ExtAudioFileRead(ext, &nFrames, &abl) != noErr || nFrames == 0)
			break;
		nGot += nFrames;
	}

	ExtAudioFileDispose(ext);
	AudioFileClose(af);

	if (!nGot) { free(pOut); return false; }

	*ppOut       = pOut;
	*pnFrames    = nGot;
	*pnChannels  = nCh;
	*pnRate      = nRate;
	return true;
}

// Minimal RIFF/WAVE parse: locate 'fmt ' and 'data' within the file image.
static bool snd_ParseWav(const uint8* pFile, uint32 nFileLen,
                         uint32* pnDataOff, uint32* pnDataLen,
                         uint32* pnChannels, uint32* pnBits, uint32* pnRate,
                         uint32* pnFormatTag)
{
	if (nFileLen < 12 || memcmp(pFile, "RIFF", 4) || memcmp(pFile + 8, "WAVE", 4))
		return false;

	uint32 nOff = 12;
	bool bHaveFmt = false, bHaveData = false;

	while (nOff + 8 <= nFileLen)
	{
		char szId[5] = {0};
		memcpy(szId, pFile + nOff, 4);
		uint32 nChunk = 0;
		memcpy(&nChunk, pFile + nOff + 4, 4);
		uint32 nBody = nOff + 8;

		if (!memcmp(szId, "fmt ", 4) && nChunk >= 16 && nBody + 16 <= nFileLen)
		{
			uint16 nTag = 0, nCh = 0, nBits = 0;
			uint32 nRate = 0;
			memcpy(&nTag,  pFile + nBody + 0,  2);
			memcpy(&nCh,   pFile + nBody + 2,  2);
			memcpy(&nRate, pFile + nBody + 4,  4);
			memcpy(&nBits, pFile + nBody + 14, 2);
			*pnFormatTag = nTag;
			*pnChannels  = nCh ? nCh : 1;
			*pnRate      = nRate ? nRate : kOutputRate;
			// ⚠️ Do NOT default this to 16. Compressed WAVs legitimately carry
			// bits = 0, and papering over it defeated the format check right
			// below -- MP3 dialogue sailed through and was mixed as raw PCM.
			*pnBits      = nBits;
			bHaveFmt = true;
		}
		else if (!memcmp(szId, "data", 4))
		{
			*pnDataOff = nBody;
			*pnDataLen = (nBody + nChunk <= nFileLen) ? nChunk : (nFileLen - nBody);
			bHaveData = true;
		}

		nOff = nBody + nChunk + (nChunk & 1);   // chunks are word-aligned
		if (bHaveFmt && bHaveData)
			break;
	}

	return bHaveFmt && bHaveData;
}

LHSTREAM CMacSoundSys::OpenStream(char* sFilename, U32 nOffset, LHDIGDRIVER, char*, S32)
{
	if (!sFilename || !sFilename[0])
		return NULL;

	FILE* fp = fopen(sFilename, "rb");
	if (!fp)
	{
		if (snd_Trace()) fprintf(stderr, "[snd] stream: cannot open '%s'\n", sFilename);
		return NULL;
	}

	// The offset is where THIS resource starts inside the (possibly .rez) file.
	// ⚠️ READ THE RIFF SIZE, DO NOT READ TO EOF. A stream inside GAME2.rez would
	// otherwise pull "rest of a 1.1 GB archive" (capped, but still tens of MB)
	// into memory for every line of dialogue. The 8-byte RIFF header at the
	// offset gives the resource's true length, and df_GetRawInfo's size is not
	// passed through OpenStream's signature.
	uint8 aRiff[12];
	if (fseek(fp, (long)nOffset, SEEK_SET) != 0 ||
	    fread(aRiff, 1, sizeof(aRiff), fp) != sizeof(aRiff) ||
	    memcmp(aRiff, "RIFF", 4) || memcmp(aRiff + 8, "WAVE", 4))
	{
		if (snd_Trace()) fprintf(stderr, "[snd] stream '%s' +%u: not a RIFF/WAVE\n", sFilename, nOffset);
		fclose(fp);
		return NULL;
	}
	uint32 nRiffSize = 0;
	memcpy(&nRiffSize, aRiff + 4, 4);
	uint32 nLen = nRiffSize + 8;                                   // whole RIFF chunk
	if (nLen < 12 || nLen > 64u * 1024u * 1024u) { fclose(fp); return NULL; }

	uint8* pFile = (uint8*)malloc(nLen);
	if (!pFile) { fclose(fp); return NULL; }
	fseek(fp, (long)nOffset, SEEK_SET);
	size_t nRead = fread(pFile, 1, nLen, fp);
	fclose(fp);

	uint32 nDataOff = 0, nDataLen = 0, nCh = 1, nBits = 0, nRate = kOutputRate, nTag = 0;
	if (!snd_ParseWav(pFile, (uint32)nRead, &nDataOff, &nDataLen, &nCh, &nBits, &nRate, &nTag))
	{
		if (snd_Trace())
			fprintf(stderr, "[snd] stream '%s': unparsable RIFF\n", sFilename);
		free(pFile);
		return NULL;
	}

	uint8*  pOwned = NULL;      // what CloseStream must free
	const uint8* pPCM = NULL;   // where the playable samples start
	uint32  nFrames = 0;

	if (nTag == kWaveFormatMP3)
	{
		// Dialogue. The data chunk is a raw MPEG bitstream; decode it up front
		// (these are a few seconds each) into PCM we own.
		sint16* pDecoded = NULL;
		uint32  nDecCh = 0, nDecRate = 0;
		if (!snd_DecodeMP3(pFile + nDataOff, nDataLen, &pDecoded, &nFrames, &nDecCh, &nDecRate))
		{
			if (snd_Trace())
				fprintf(stderr, "[snd] stream '%s': MP3 decode failed\n", sFilename);
			free(pFile);
			return NULL;
		}
		free(pFile);                    // the container is no longer needed
		pOwned = (uint8*)pDecoded;
		pPCM   = (const uint8*)pDecoded;
		nCh    = nDecCh;
		nBits  = 16;
		nRate  = nDecRate;
	}
	else if (nTag == kWaveFormatPCM && (nBits == 8 || nBits == 16))
	{
		pOwned  = pFile;
		pPCM    = pFile + nDataOff;
		nFrames = nDataLen / (nCh * (nBits / 8));
	}
	else
	{
		// Anything else (ADPCM, other ASI codecs): refuse rather than mix the
		// compressed bytes as PCM, which is a burst of white noise.
		if (snd_Trace())
			fprintf(stderr, "[snd] stream '%s': unsupported format tag %u (bits=%u)\n",
			        sFilename, nTag, nBits);
		free(pFile);
		return NULL;
	}

	CMacVoice* pV = AllocVoice(false, /*bStream=*/true);
	if (!pV) { free(pOwned); return NULL; }

	// Unlike a sample, WE own this memory -- freed in CloseStream.
	pV->m_bStream       = true;
	pV->m_bDecoded      = (nTag == kWaveFormatMP3);
	pV->m_pOwnedData    = pOwned;
	pV->m_pData         = pPCM;
	pV->m_nSrcChannels  = nCh;
	pV->m_nSrcBits      = nBits;
	pV->m_nSrcRate      = nRate;
	pV->m_nFrames       = nFrames;
	pV->m_fPos          = 0.0;
	pV->m_fStep         = (double)nRate / (double)kOutputRate;
	pV->ResolveGains();

	if (snd_Trace())
		fprintf(stderr, "[snd] stream opened '%s' +%u: %u frames @%u Hz %uch/%ub (%.1fs)%s\n",
		        sFilename, nOffset, pV->m_nFrames, nRate, nCh, nBits,
		        (double)pV->m_nFrames / (double)nRate,
		        (nTag == kWaveFormatMP3) ? " [MP3-decoded]" : "");

	return (LHSTREAM)pV;
}

// ★★★ THE FIX FOR THE AUDIO-THREAD CRASH (§66).
//
// Clearing `m_bPlaying` DOES NOT retract a Render() pass that is already past
// its guard. Render reads the flag once and then mixes `inNumberFrames` samples
// through snd_SampleAt(pV, ...), dereferencing pV->m_pData the whole way. A game
// thread that clears the flag and immediately frees the buffer therefore races
// the mixer, and the crash lands in snd_SampleAt reading from a freed/NULL base
// — observed as `KERN_INVALID_ADDRESS at 0xcc4` and `at 0x4b0b0`, both of which
// are exactly (frame * channels * 2) offsets from NULL.
//
// The comment that used to sit on Render's guard — "read it once; everything
// below is stable while it is set" — was the mistaken assumption. There is no
// lock anywhere in this driver, so nothing made it stable.
//
// This waits until the RT thread cannot possibly still be inside the voice:
// after the caller has cleared m_bPlaying, TWO callback boundaries must pass.
// One is not enough — the callback in flight may already have passed the guard;
// the second is guaranteed to have started after the store and so observes the
// cleared flag.
//
// ⚠️ Bounded, and skipped entirely when the unit is not running: with no RT
// thread the counter never advances and an unbounded wait would hang shutdown.
void CMacSoundSys::WaitForRTQuiesce()
{
	if (!m_bStarted || !m_OutputUnit)
		return;   // no RT thread to race with

	const uint32 nStart = m_nRTCallbacks.load(std::memory_order_acquire);
	// ~200 ms ceiling: far longer than any callback, short enough that a wedged
	// audio device cannot stall the game thread indefinitely.
	for (int i = 0; i < 20000; ++i)
	{
		if ((m_nRTCallbacks.load(std::memory_order_acquire) - nStart) >= 2)
			return;
		usleep(10);
	}
	if (snd_Trace())
		fprintf(stderr, "[snd] WaitForRTQuiesce timed out — audio thread stalled?\n");
}

// Frees buffers parked by CloseStream once the mixer has provably moved past
// them. Pumped from CloseStream and AllocVoice, so the common case never blocks;
// bForce (shutdown) frees regardless, the RT thread being stopped by then.
void CMacSoundSys::ReapRetiredBuffers(bool bForce)
{
	const uint32 nNow = m_nRTCallbacks.load(std::memory_order_acquire);
	for (uint32 i = 0; i < kMaxVoices; ++i)
	{
		CMacVoice& cV = m_aVoices[i];
		if (!cV.m_pRetiredData)
			continue;
		// Signed compare: the counter wraps.
		if (!bForce && (sint32)(nNow - cV.m_nRetireAt) < 0)
			continue;
		// ★★★ CLEAR m_pData WHENEVER IT STILL POINTS AT THE BUFFER WE ARE ABOUT
		// TO FREE — NOT ONLY WHEN THE VOICE IS UNALLOCATED (§76).
		//
		// The old `if (!cV.m_bAllocated)` guard left a voice the engine still
		// HOLDS pointing at freed memory. That is the common case, not the rare
		// one: a dialogue line ends, CloseStream parks the buffer (§66) without
		// clearing m_pData — deliberately, because Render re-reads it per sample
		// — and the engine keeps the handle. Two callbacks later this reaps and
		// frees the buffer, and m_pData is left DANGLING.
		//
		// Nothing downstream catches it. `ResumeSample` guards with
		// `if (!pV->m_pData || !pV->m_nFrames) return;` and a dangling non-null
		// pointer passes; `m_nFrames` is still set from the old sound. So the
		// next Start/Resume on that voice mixes freed heap — full-scale white
		// noise, at whatever 3D position the voice last held, for as long as
		// something keeps restarting it.
		//
		// ⚠️ The comparison matters: if the voice was RE-INITIALISED with new
		// data after the close (`m_pData = pStart` republish, InitSample), then
		// m_pData is legitimately different and must be left alone. Only the
		// pointer we are actually invalidating gets cleared.
		//
		// ⚠️ Safe to do here and ONLY here: the 2-callback rule (§66) is exactly
		// the guarantee that the mixer is no longer inside this buffer.
		if (cV.m_pData == cV.m_pRetiredData)
		{
			cV.m_bPlaying = false;
			cV.m_pData    = NULL;
			cV.m_nFrames  = 0;
			cV.m_fPos     = 0.0;
		}
		free(cV.m_pRetiredData);
		cV.m_pRetiredData = NULL;
	}
}

void CMacSoundSys::CloseStream(LHSTREAM hStream)
{
	CMacVoice* pV = VoiceFromHandle(hStream);
	if (!pV) return;

	// ⚠️ DO NOT free here, and DO NOT clear m_pData here (§66). Render may be
	// mid-loop on this voice right now: it re-reads pV->m_pData for every one of
	// the ~512 samples it is mixing, so both the free and the NULL would race it.
	// Clearing m_bPlaying stops the NEXT callback from picking the voice up;
	// the buffer is parked and freed once two callback boundaries have passed.
	// Blocking here instead would be correct but costly — a music intensity
	// change closes several streams at once, which at ~12 ms per callback would
	// be a visible hitch.
	pV->m_bPlaying   = false;
	pV->m_bStream    = false;
	pV->m_bAllocated = false;
	if (pV->m_pOwnedData)
	{
		pV->m_pRetiredData = pV->m_pOwnedData;
		pV->m_pOwnedData   = NULL;
		pV->m_nRetireAt    = m_nRTCallbacks.load(std::memory_order_acquire) + 2;
	}
	else
	{
		pV->m_nRetireAt = m_nRTCallbacks.load(std::memory_order_acquire) + 2;
	}

	ReapRetiredBuffers(false);   // pump: free whatever became safe meanwhile
}

void CMacSoundSys::StartStream(LHSTREAM hStream)
{
	CMacVoice* pV = VoiceFromHandle(hStream);
	if (!pV || !pV->m_pData) return;
	pV->m_fPos = 0.0;
	pV->m_bPaused = false;
	pV->m_bEverStarted = true;
	pV->ResolveGains();
	pV->m_bPlaying = true;
	if (snd_Trace())
		fprintf(stderr, "[snd] stream start voice %ld vol=%.2f\n",
		        (long)(pV - &m_aVoices[0]), pV->m_fVolume);
}

void CMacSoundSys::PauseStream(LHSTREAM hStream, S32 siOnOff)
{
	CMacVoice* pV = VoiceFromHandle(hStream);
	if (!pV) return;
	if (siOnOff)
	{
		pV->m_bPaused = true;
	}
	else
	{
		// Same rule as ResumeSample: "un-pause" is what actually starts it.
		pV->m_bPaused = false;
		if (!pV->m_bPlaying && pV->m_pData)
		{
			pV->m_bEverStarted = true;
			pV->m_bPlaying = true;
		}
	}
}

void CMacSoundSys::ResetStream(LHSTREAM hStream)
{
	CMacVoice* pV = VoiceFromHandle(hStream);
	if (pV) pV->m_fPos = 0.0;
}

void CMacSoundSys::SetStreamLoop(LHSTREAM hStream, bool bLoop)     { SetSampleLoop(hStream, bLoop); }
void CMacSoundSys::SetStreamPlaybackRate(LHSTREAM, S32)            {}
void CMacSoundSys::SetStreamMsPosition(LHSTREAM hS, S32 ms)        { SetSampleMsPosition(hS, ms); }
void CMacSoundSys::SetStreamUserData(LHSTREAM hS, U32 i, LTSOUNDUSERDATA v) { SetSampleUserData(hS, i, v); }
LTSOUNDUSERDATA CMacSoundSys::GetStreamUserData(LHSTREAM hS, U32 i){ return GetSampleUserData(hS, i); }
void CMacSoundSys::SetStreamVolume(LHSTREAM hS, S32 v)             { SetSampleVolume(hS, v); }
void CMacSoundSys::SetStreamPan(LHSTREAM hS, S32 p)                { SetSamplePan(hS, p); }
S32  CMacSoundSys::GetStreamVolume(LHSTREAM hS)                    { return GetSampleVolume(hS); }
S32  CMacSoundSys::GetStreamPan(LHSTREAM hS)                       { return GetSamplePan(hS); }
U32  CMacSoundSys::GetStreamStatus(LHSTREAM hS)                    { return GetSampleStatus(hS); }
S32  CMacSoundSys::GetStreamBufferParam(LHSTREAM, U32)             { return 0; }
void CMacSoundSys::ClearStreamBuffer(LHSTREAM, bool)               {}

// EAX 2.0 environmental reverb. Reporting "unsupported" is the correct answer
// rather than a lie -- the engine then skips the whole filter path.
bool CMacSoundSys::SetEAX20Filter(bool, LTSOUNDFILTERDATA*) { return false; }
bool CMacSoundSys::SupportsEAX20Filter() { return false; }
bool CMacSoundSys::SetEAX20BufferSettings(LHSAMPLE, LTSOUNDFILTERDATA*) { return false; }

// ADPCM: not yet. If any asset needs it, InitSampleFromAddress rejects the
// format rather than mixing garbage, and LT_TRACE_SOUND says so.
S32 CMacSoundSys::DecompressADPCM(LTSOUNDINFO*, void**, U32*) { return 0; }
S32 CMacSoundSys::DecompressASI(void*, U32, char*, void**, U32*, LTLENGTHYCB) { return 0; }

// --------------------------------------------------------------------------
// Odds and ends
// --------------------------------------------------------------------------

void* CMacSoundSys::GetDDInterface(uint) { return NULL; }
void  CMacSoundSys::Lock(void)   {}
void  CMacSoundSys::Unlock(void) {}

U32 CMacSoundSys::MsCount(void)
{
	// Milliseconds since the first call. CLOCK_MONOTONIC for the same reason
	// time_GetMSTime uses it (§16): a based, small, un-steppable counter.
	static uint64 s_nBase = 0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64 nNow = (uint64)ts.tv_sec * 1000ULL + (uint64)ts.tv_nsec / 1000000ULL;
	if (!s_nBase) s_nBase = nNow;
	return (U32)(nNow - s_nBase);
}

S32   CMacSoundSys::SetPreference(U32, S32) { return LS_OK; }
S32   CMacSoundSys::GetPreference(U32) { return 0; }
void  CMacSoundSys::MemFreeLock(void* ptr) { free(ptr); }
void* CMacSoundSys::MemAllocLock(U32 uiSize) { return malloc(uiSize); }
char* CMacSoundSys::LastError(void) { return (char*)""; }

S32 CMacSoundSys::WaveOutOpen(LHDIGDRIVER* phDriver, PHWAVEOUT* pphWaveOut, S32, WAVEFORMAT*)
{
	if (snd_Trace()) fprintf(stderr, "[snd] WaveOutOpen\n");
	if (Startup() != LS_OK)
		return LS_ERROR;
	// A non-NULL cookie: the engine only ever tests it and passes it back.
	if (phDriver)    *phDriver = (LHDIGDRIVER)this;
	if (pphWaveOut)  *pphWaveOut = NULL;
	return LS_OK;
}

void CMacSoundSys::WaveOutClose(LHDIGDRIVER) { Shutdown(); }

void CMacSoundSys::SetDigitalMasterVolume(LHDIGDRIVER, S32 siMasterVolume)
{
	float f = (float)siMasterVolume / 127.0f;
	m_fMasterVolume = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
}
S32  CMacSoundSys::GetDigitalMasterVolume(LHDIGDRIVER) { return (S32)(m_fMasterVolume * 127.0f); }
S32  CMacSoundSys::DigitalHandleRelease(LHDIGDRIVER)   { return LS_OK; }
S32  CMacSoundSys::DigitalHandleReacquire(LHDIGDRIVER) { return LS_OK; }

void CMacSoundSys::Set3DProviderMinBuffers(U32) {}
S32  CMacSoundSys::Open3DProvider(LHPROVIDER)   { return LTTRUE; }   // boolean, not LS_OK
void CMacSoundSys::Close3DProvider(LHPROVIDER)  {}
void CMacSoundSys::Set3DProviderPreference(LHPROVIDER, char*, void*) {}
void CMacSoundSys::Get3DProviderAttribute(LHPROVIDER, char* sName, void* pVal)
{
	// The engine asks for "Max samples"; anything else it tolerates as 0.
	if (pVal && sName && strstr(sName, "ax samples"))
		*(S32*)pVal = (S32)kMaxVoices;
	else if (pVal)
		*(S32*)pVal = 0;
}

S32 CMacSoundSys::Enumerate3DProviders(LHPROENUM* phNext, LHPROVIDER* phDest, char** psName)
{
	// ★ THE NAME IS NOT COSMETIC -- THE GAME MATCHES IT BY STRING.
	// With no "3DSoundProviderName" console var set (the retail config has
	// none), CGameClientShell::SetSoundInit defaults to looking for a provider
	// literally named "DirectSound Hardware" and compares with strcmp. A
	// provider called anything else NEVER MATCHES, so m_sz3DProvider stays
	// empty, CSoundMgr::Set3DProvider fails, and -- because m_nMax3DSamples is
	// only assigned inside `if (m_3DProvider.m_hProvider)` -- the 3D sample
	// pool stays at ZERO. Every positional sound in the world then falls
	// through to the 28-slot 2D pool and the engine thrashes, stealing samples
	// from itself hundreds of times a second (visible as an endless
	// InitSampleFromAddress + "start 2D (resume)" storm in LT_TRACE_SOUND, with
	// every sound restarting at position 0 before it can be heard).
	//
	// Also note the ID: Get3DProviderLists does `dwProviderID = hProvider`, so
	// the handle VALUE is the SOUND3DPROVIDERID_*. 1 = DS3D_HARDWARE, which is
	// what the game asks for when hardware sound is enabled.
	// ★★★ ON BY DEFAULT since 2026-08-06, at the user's decision (§74).
	//
	// This used to be gated off for ONE reason: advertising the matching name
	// made the engine take a path that crashed in CSoundMgr::Init before the
	// sample pools were built. That crash is now root-caused and fixed -- it was
	// `typedef long sint32` in iltsound.h being 8 bytes on LP64, so
	// `*(S32*)pVal` in Get3DProviderAttribute overwrote four bytes of the
	// CALLER's stack (see §74). Nothing else was ever wrong with this path.
	//
	// Turning it on is what stops the voice-stealing storm described above:
	// `Snd/amb/JpEvening.wav` went from 800+ re-acquisitions per 60 s to ZERO,
	// and positional audio gets its own 30-voice pool instead of sharing the
	// 28-slot 2D one.
	//
	// ⚠️ `LT_NO_SOUND_3D=1` restores the old 2D-only fallback -- a bisection tool
	// for "did 3D sound cause this", not a mode anyone should ship with.
	// `LT_SOUND_3D=1` is kept as a no-op so existing commands still work.
	static char s_szMatching[]    = "DirectSound Hardware";
	static char s_szNonMatching[] = "CoreAudio 3D (2D fallback)";
	char* s_szName = getenv("LT_NO_SOUND_3D") ? s_szNonMatching : s_szMatching;
	if (!phNext || !phDest || !psName)
		return 0;
	if (*phNext != 0)
		return 0;
	*phNext = 1;
	*phDest = (LHPROVIDER)1;
	*psName = s_szName;
	return 1;
}

U32  CMacSoundSys::GetThreadedSoundTicks() { return 0; }
bool CMacSoundSys::HasOnBoardMemory()      { return false; }

// --------------------------------------------------------------------------
// The factory -- registers itself at static init, which is the hook
// soundmgr.cpp's GetSoundSys() looks for. It was NULL on macOS until now,
// which is precisely why every PlaySound reported "no sound driver".
// --------------------------------------------------------------------------

class CMacSoundFactory : public ILTSoundFactory
{
public:
	CMacSoundFactory()  { m_pSoundFactory = this; }
	virtual ~CMacSoundFactory() { m_pSoundFactory = NULL; }

	virtual bool FillSoundSystems(char* pcSoundSysNames, uint uiMaxStringLen)
	{
		// Double-NUL-terminated list of one entry, matching the Win32 factory.
		if (!pcSoundSysNames || uiMaxStringLen < 16)
			return false;
		strcpy(pcSoundSysNames, CMacSoundSys::m_pcDesc);
		pcSoundSysNames[strlen(CMacSoundSys::m_pcDesc) + 1] = 0;
		return true;
	}

	virtual ILTSoundSys* MakeSoundSystem(const char* pcSoundSystemName)
	{
		return &CMacSoundSys::m_MacSoundSys;
	}

	static CMacSoundFactory m_soundFactory;
};

CMacSoundFactory CMacSoundFactory::m_soundFactory;
