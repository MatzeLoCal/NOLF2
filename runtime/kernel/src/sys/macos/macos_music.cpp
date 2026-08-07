// ----------------------------------------------------------------------- //
//
// MODULE  : macos_music.cpp
//
// PURPOSE : Interactive music for the macOS port. See macos_music.h for why
//           this is a wave-track scheduler and not a DirectMusic port.
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "macos_music.h"

#include "client_filemgr.h"
#include "iltstream.h"
#include "soundmgr.h"      // GetSoundSys / GetClientILTSoundMgrImpl
#include "iltsound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>

// IClientFileMgr resolves a game-relative name to a real path + offset, which
// is what OpenStream needs (the same route CSoundInstance::AcquireStream takes).
static IClientFileMgr *music_file_mgr;
define_holder(IClientFileMgr, music_file_mgr);

// ⚠️ Declared locally rather than by including de_file.h: the io contract lives
// in sys/win/de_file.h, whose types collide with sys/linux/linuxfile.h (already
// on this target's include path, and the header the macOS build actually uses).
// CSoundInstance::AcquireStream declares it the same way for the same reason.
extern int df_GetRawInfo(HLTFileTree *hTree, const char *pName, char* sFileName,
                         unsigned int nMaxFileName, uint32* nPos, uint32* nSize);

extern float time_GetTime();

// --------------------------------------------------------------------------
// Tracing
// --------------------------------------------------------------------------
static bool mus_Trace()
{
	static int s_n = -1;
	if (s_n < 0) s_n = getenv("LT_TRACE_MUSIC") ? 1 : 0;
	return s_n != 0;
}

// --------------------------------------------------------------------------
// Authored data
// --------------------------------------------------------------------------
namespace
{
	// One wave reference inside a segment's wave track.
	struct MusicWave
	{
		std::string m_sFile;      // "JapanC1.wav", relative to the segment's dir
		int32       m_nTicks;     // mtLogicalTime, in MUSIC_TIME ticks
		float       m_fStartSec;  // resolved against the segment tempo
		MusicWave() : m_nTicks(0), m_fStartSec(0.0f) {}
	};

	struct MusicSegment
	{
		std::vector<MusicWave> m_aWaves;
		double m_fTempo;       // BPM from the tempo track
		uint32 m_nLenTicks;    // segh length
		float  m_fLenSec;      // resolved
		bool   m_bValid;
		MusicSegment() : m_fTempo(120.0), m_nLenTicks(0), m_fLenSec(0.0f), m_bValid(false) {}
	};

	// INTENSITY <n> <loops (-1 = forever)> <next (0 = stop)> <segment...>
	struct MusicIntensity
	{
		int m_nLoops;
		int m_nNext;
		std::vector<std::string> m_aSegments;
		size_t m_nCursor;      // round-robin position (see mus_EnterIntensity)
		MusicIntensity() : m_nLoops(0), m_nNext(0), m_nCursor(0) {}
	};

	// A scheduled stream. Waves within a segment overlap, so several of these
	// are live at once and each one starts at its own musical time.
	struct MusicVoice
	{
		LHSTREAM    m_hStream;
		float       m_fStartAt;    // absolute time_GetTime() seconds
		bool        m_bStarted;
		bool        m_bSecondary;
		std::string m_sName;
		MusicVoice() : m_hStream(0), m_fStartAt(0.0f), m_bStarted(false), m_bSecondary(false) {}
	};

	// ---- level state ----
	std::string s_sDir;                       // "MUSIC\JAPAN"
	std::string s_sControlFile;
	std::map<std::string, MusicSegment> s_Segments;    // parsed .SGT cache, keyed lowercase
	std::map<int, MusicIntensity>       s_Intensities;
	std::map<uint32, std::string>       s_Transitions; // from*4096+to -> segment ("" = none)

	int  s_nNumIntensities   = 0;
	int  s_nInitialIntensity = 0;
	int  s_nInitialVolume    = 0;
	int  s_nVolumeOffset     = 0;

	// ---- runtime state ----
	bool  s_bLevelReady   = false;
	bool  s_bPlaying      = false;
	bool  s_bPaused       = false;
	long  s_nVolume       = 0;      // hundredths of a dB
	int   s_nCurIntensity = 0;
	int   s_nPendingIntensity = -1; // queued behind a transition segment
	float s_fSegmentEndsAt = 0.0f;  // when the current segment hands over
	std::vector<MusicVoice> s_aVoices;

	std::string mus_Lower(const std::string &s)
	{
		std::string o(s);
		for (size_t i = 0; i < o.size(); ++i)
			o[i] = (char)tolower((unsigned char)o[i]);
		return o;
	}

	// The engine's file layer normalizes separators, so either works; keep '\'
	// to match how the game itself spells these paths.
	std::string mus_Join(const std::string &sDir, const std::string &sFile)
	{
		if (sDir.empty()) return sFile;
		std::string o(sDir);
		if (o[o.size() - 1] != '\\' && o[o.size() - 1] != '/')
			o += '\\';
		return o + sFile;
	}
}

// --------------------------------------------------------------------------
// Reading files through the engine (so .rez trees work as well as loose data)
// --------------------------------------------------------------------------
static bool mus_ReadWholeFile(const char *sName, std::vector<uint8> &aOut)
{
	aOut.clear();
	if (!music_file_mgr || !sName || !sName[0])
		return false;

	FileRef cRef;
	cRef.m_FileType = FILE_ANYFILE;
	cRef.m_pFilename = const_cast<char*>(sName);
	ILTStream *pStream = music_file_mgr->OpenFile(&cRef);
	if (!pStream)
		return false;

	uint32 nLen = 0;
	pStream->GetLen(&nLen);
	if (nLen == 0 || nLen > (32u * 1024u * 1024u))
	{
		pStream->Release();
		return false;
	}
	aOut.resize(nLen);
	pStream->Read(&aOut[0], nLen);
	bool bOK = (pStream->ErrorStatus() == LT_OK);
	pStream->Release();
	return bOK;
}

// --------------------------------------------------------------------------
// The .SGT reader
//
// Layout, verified against the retail data:
//   RIFF/DMSG
//     segh                      DMUS_IO_SEGMENT_HEADER (length at +4, in ticks)
//     LIST/trkl
//       RIFF/DMTK  ... tetr     tempo track: double BPM at +12
//       RIFF/DMTK  ... LIST/wavt
//                        LIST/wavp    one wave PART
//                          LIST/wavi
//                            LIST/wave
//                              waih   64B; mtLogicalTime at +48  ⚠️ see below
//                              LIST/DMRF ... file   the .wav name, UTF-16
//
// ⚠️ THE OFFSET THAT COST A ROUND: waih's REFERENCE_TIME members are 8-byte
// ALIGNED, so rtTime is at +16, not +12, and mtLogicalTime lands at +48. Read
// with the naive packing and the timings come out as millions of seconds.
// ⚠️ And it is mtLogicalTime that carries the schedule, NOT rtTime — rtTime is
// 0 or 1 ms in this data.
// --------------------------------------------------------------------------
namespace
{
	inline uint32 mus_RdU32(const uint8 *p) { uint32 v; memcpy(&v, p, 4); return v; }
	inline int32  mus_RdI32(const uint8 *p) { int32  v; memcpy(&v, p, 4); return v; }

	// DirectMusic wide strings are UTF-16LE; these names are all ASCII.
	std::string mus_WideToAscii(const uint8 *p, uint32 nBytes)
	{
		std::string o;
		for (uint32 i = 0; i + 1 < nBytes; i += 2)
		{
			uint16 w = (uint16)(p[i] | (p[i + 1] << 8));
			if (!w) break;
			o += (char)(w < 128 ? w : '?');
		}
		return o;
	}

	// Recursive RIFF walk. Collects the tempo, the segment length and the wave
	// references; everything else in the segment is ignored on purpose.
	void mus_ScanChunks(const uint8 *pData, uint32 nStart, uint32 nEnd,
	                    MusicSegment &cSeg, int32 &nPendingTicks, bool &bHavePending)
	{
		uint32 nPos = nStart;
		while (nPos + 8 <= nEnd)
		{
			const uint8 *pId = pData + nPos;
			uint32 nSize = mus_RdU32(pData + nPos + 4);
			uint32 nBody = nPos + 8;
			if (nBody > nEnd) break;
			uint32 nStop = nBody + nSize;
			if (nStop > nEnd) nStop = nEnd;

			if (!memcmp(pId, "RIFF", 4) || !memcmp(pId, "LIST", 4))
			{
				// 4-byte form id, then nested chunks.
				if (nBody + 4 <= nStop)
					mus_ScanChunks(pData, nBody + 4, nStop, cSeg, nPendingTicks, bHavePending);
			}
			else if (!memcmp(pId, "segh", 4) && nSize >= 8)
			{
				cSeg.m_nLenTicks = mus_RdU32(pData + nBody + 4);
			}
			else if (!memcmp(pId, "tetr", 4) && nSize >= 20)
			{
				double f = 0.0;
				memcpy(&f, pData + nBody + 12, 8);
				if (f > 1.0 && f < 1000.0)
					cSeg.m_fTempo = f;
			}
			else if (!memcmp(pId, "waih", 4) && nSize >= 52)
			{
				nPendingTicks = mus_RdI32(pData + nBody + 48);
				bHavePending  = true;
			}
			else if (!memcmp(pId, "file", 4) && bHavePending)
			{
				MusicWave cWave;
				cWave.m_sFile = mus_WideToAscii(pData + nBody, nSize);
				cWave.m_nTicks = nPendingTicks;
				if (!cWave.m_sFile.empty())
					cSeg.m_aWaves.push_back(cWave);
				bHavePending = false;
			}

			nPos = nBody + nSize + (nSize & 1);   // RIFF chunks are word-aligned
		}
	}

	// Loads (and caches) a segment by file name, e.g. "Combat1.sgt".
	const MusicSegment *mus_GetSegment(const std::string &sFile)
	{
		std::string sKey = mus_Lower(sFile);
		std::map<std::string, MusicSegment>::iterator it = s_Segments.find(sKey);
		if (it != s_Segments.end())
			return it->second.m_bValid ? &it->second : 0;

		MusicSegment &cSeg = s_Segments[sKey];   // insert (invalid until parsed)

		std::vector<uint8> aData;
		if (!mus_ReadWholeFile(mus_Join(s_sDir, sFile).c_str(), aData) || aData.size() < 12)
		{
			if (mus_Trace())
				fprintf(stderr, "[music] segment not found: %s\n",
				        mus_Join(s_sDir, sFile).c_str());
			return 0;
		}

		int32 nPending = 0; bool bHavePending = false;
		mus_ScanChunks(&aData[0], 0, (uint32)aData.size(), cSeg, nPending, bHavePending);

		// MUSIC_TIME is 768 ticks per quarter note; seconds = ticks/768 * 60/BPM.
		const double fTickSec = (cSeg.m_fTempo > 0.0)
		                      ? (60.0 / (cSeg.m_fTempo * 768.0)) : 0.0;
		for (size_t i = 0; i < cSeg.m_aWaves.size(); ++i)
			cSeg.m_aWaves[i].m_fStartSec = (float)(cSeg.m_aWaves[i].m_nTicks * fTickSec);
		cSeg.m_fLenSec = (float)(cSeg.m_nLenTicks * fTickSec);
		cSeg.m_bValid  = !cSeg.m_aWaves.empty();

		if (mus_Trace())
		{
			fprintf(stderr, "[music] segment '%s': tempo=%.0f len=%u ticks (%.2fs) %u waves\n",
			        sFile.c_str(), cSeg.m_fTempo, cSeg.m_nLenTicks, cSeg.m_fLenSec,
			        (unsigned)cSeg.m_aWaves.size());
			for (size_t i = 0; i < cSeg.m_aWaves.size(); ++i)
				fprintf(stderr, "[music]     +%6.2fs  %s\n",
				        cSeg.m_aWaves[i].m_fStartSec, cSeg.m_aWaves[i].m_sFile.c_str());
		}
		return cSeg.m_bValid ? &cSeg : 0;
	}
}

// --------------------------------------------------------------------------
// The control file
//
//   NUMINTENSITIES / INITIALINTENSITY / INITIALVOLUME / VOLUMEOFFSET
//   INTENSITY  <n> <loops> <next> <segment...>
//   TRANSITION <from> <to> <when> <AUTOMATIC|MANUAL> [segment]
//
// Everything else (PCHANNELS, VOICES, SYNTHSAMPLERATE, REVERB*, STYLE, MOTIF)
// belongs to the DirectMusic synthesiser and is deliberately ignored — see the
// header for why none of it is reachable from this game's data.
// --------------------------------------------------------------------------
static void mus_ParseControlFile(const std::vector<uint8> &aData)
{
	s_Intensities.clear();
	s_Transitions.clear();
	s_nNumIntensities = s_nInitialIntensity = s_nInitialVolume = s_nVolumeOffset = 0;

	std::string sText((const char*)&aData[0], aData.size());
	size_t nPos = 0;
	while (nPos <= sText.size())
	{
		size_t nEnd = sText.find('\n', nPos);
		if (nEnd == std::string::npos) nEnd = sText.size();
		std::string sLine = sText.substr(nPos, nEnd - nPos);
		nPos = nEnd + 1;

		// Strip CR and comments (';' to end of line).
		size_t nCut = sLine.find(';');
		if (nCut != std::string::npos) sLine = sLine.substr(0, nCut);
		while (!sLine.empty() && (sLine[sLine.size() - 1] == '\r' ||
		       sLine[sLine.size() - 1] == ' '  || sLine[sLine.size() - 1] == '\t'))
			sLine.erase(sLine.size() - 1);
		if (sLine.empty()) continue;

		// Tokenize on whitespace (the file mixes spaces and tabs freely).
		std::vector<std::string> aTok;
		size_t i = 0;
		while (i < sLine.size())
		{
			while (i < sLine.size() && (sLine[i] == ' ' || sLine[i] == '\t')) ++i;
			size_t nTokStart = i;
			while (i < sLine.size() && sLine[i] != ' ' && sLine[i] != '\t') ++i;
			if (i > nTokStart) aTok.push_back(sLine.substr(nTokStart, i - nTokStart));
		}
		if (aTok.empty()) continue;

		std::string sCmd = mus_Lower(aTok[0]);
		if (sCmd == "numintensities"   && aTok.size() >= 2) s_nNumIntensities   = atoi(aTok[1].c_str());
		else if (sCmd == "initialintensity" && aTok.size() >= 2) s_nInitialIntensity = atoi(aTok[1].c_str());
		else if (sCmd == "initialvolume"    && aTok.size() >= 2) s_nInitialVolume    = atoi(aTok[1].c_str());
		else if (sCmd == "volumeoffset"     && aTok.size() >= 2) s_nVolumeOffset     = atoi(aTok[1].c_str());
		else if (sCmd == "intensity" && aTok.size() >= 5)
		{
			MusicIntensity cInt;
			int nNum      = atoi(aTok[1].c_str());
			cInt.m_nLoops = atoi(aTok[2].c_str());
			cInt.m_nNext  = atoi(aTok[3].c_str());
			for (size_t t = 4; t < aTok.size(); ++t)
			{
				// ⚠️ A multi-segment intensity is COMMA-separated:
				//   INTENSITY 2 0 2 Explore1.sgt, Explore2.sgt, ... Explore15.sgt
				// (Island does exactly this with 15 segments.) Without stripping
				// the comma every lookup becomes "Explore1.sgt," and silently
				// misses — the failure looks like missing data, not a parse bug.
				std::string sSeg = aTok[t];
				while (!sSeg.empty() && (sSeg[sSeg.size()-1] == ',' || sSeg[sSeg.size()-1] == ' '))
					sSeg.erase(sSeg.size()-1);
				if (!sSeg.empty()) cInt.m_aSegments.push_back(sSeg);
			}
			if (nNum > 0) s_Intensities[nNum] = cInt;
		}
		else if (sCmd == "transition" && aTok.size() >= 5)
		{
			int nFrom = atoi(aTok[1].c_str());
			int nTo   = atoi(aTok[2].c_str());
			// aTok[3] = when (SEGMENT/MEASURE/BEAT/IMMEDIATE/...), aTok[4] = mode.
			// ⚠️ The segment is OPTIONAL: "TRANSITION 1 23 MEASURE MANUAL" with no
			// segment means "just switch", and the retail files use that form.
			std::string sSeg = (aTok.size() >= 6) ? aTok[5] : std::string();
			s_Transitions[(uint32)nFrom * 4096u + (uint32)nTo] = sSeg;
		}
	}
}

// --------------------------------------------------------------------------
// Voices
// --------------------------------------------------------------------------
static void mus_StopAllVoices(bool bSecondaryToo)
{
	for (size_t i = 0; i < s_aVoices.size(); )
	{
		if (!bSecondaryToo && s_aVoices[i].m_bSecondary) { ++i; continue; }
		if (s_aVoices[i].m_hStream)
		{
			GetSoundSys()->PauseStream(s_aVoices[i].m_hStream, 1);
			GetSoundSys()->CloseStream(s_aVoices[i].m_hStream);
		}
		s_aVoices.erase(s_aVoices.begin() + i);
	}
}

// ★ MUSIC HEADROOM — a CALIBRATION, not a derivation. Be honest about it.
//
// The authored numbers cancel out: the shipped profile has `musicvolume 1250`
// (+12.5 dB) and every control file sets `VOLUMEOFFSET -1250`, so the sum is
// 0 dB. Under DirectMusic that meant "no attenuation from the performance",
// but its output still went through a SYNTH mixing well below full scale.
// Playing the waves straight at unity therefore comes out far hotter than
// retail — the user's first listen was "quite a bit too loud", which is exactly
// this.
//
// ⚠️ There is nothing in the data to derive the missing headroom from: every
// authored per-wave volume (`waph`/`waih` lVolume) is 0 across all 1542
// segments, so no attenuation is being dropped on the floor. This constant is
// a tuned reference level, and `LT_MUSIC_GAIN=<dB>` trims it (e.g. "-6" or
// "+3") without a rebuild so it can be dialled in by ear in one run.
static const double kMusicHeadroomDB = -12.0;

static double mus_ExtraGainDB()
{
	static double s_fDB = 1e9;
	if (s_fDB > 1e8)
	{
		const char *pEnv = getenv("LT_MUSIC_GAIN");
		s_fDB = pEnv ? atof(pEnv) : 0.0;
	}
	return s_fDB;
}

// Volume arrives in hundredths of a dB and the driver wants Miles' 0..127.
static sint32 mus_DriverVolume()
{
	const double fDB = (double)(s_nVolume + s_nVolumeOffset) / 100.0
	                 + kMusicHeadroomDB + mus_ExtraGainDB();
	double fGain = pow(10.0, fDB / 20.0);
	if (fGain < 0.0) fGain = 0.0;
	if (fGain > 1.0) fGain = 1.0;
	return (sint32)(fGain * 127.0 + 0.5);
}

static void mus_ApplyVolume()
{
	const sint32 nVol = mus_DriverVolume();
	for (size_t i = 0; i < s_aVoices.size(); ++i)
		if (s_aVoices[i].m_hStream)
			GetSoundSys()->SetStreamVolume(s_aVoices[i].m_hStream, nVol);
}

// Opens one wave as a stream, scheduled to begin at fStartAt.
static bool mus_QueueWave(const std::string &sWaveFile, float fStartAt, bool bSecondary)
{
	if (!music_file_mgr)
		return false;

	std::string sFull = mus_Join(s_sDir, sWaveFile);
	FileRef cRef;
	cRef.m_FileType  = FILE_ANYFILE;
	cRef.m_pFilename = const_cast<char*>(sFull.c_str());
	FileIdentifier *pIdent = music_file_mgr->GetFileIdentifier(&cRef, TYPECODE_SOUND);
	if (!pIdent)
	{
		if (mus_Trace()) fprintf(stderr, "[music] wave missing: %s\n", sFull.c_str());
		return false;
	}

	// Same resolution CSoundInstance::AcquireStream uses: a real path plus the
	// offset of this resource inside it (non-zero when it lives in a .rez).
	char szPath[_MAX_PATH + 1];
	uint32 nPos = 0, nSize = 0;
	if (!df_GetRawInfo(pIdent->m_hFileTree, pIdent->m_Filename, szPath, _MAX_PATH, &nPos, &nSize))
	{
		if (mus_Trace()) fprintf(stderr, "[music] no raw info: %s\n", sFull.c_str());
		return false;
	}

	LHSTREAM hStream = GetSoundSys()->OpenStream(
		szPath, nPos, GetClientILTSoundMgrImpl()->GetDigDriver(), 0, 0);
	if (!hStream)
	{
		if (mus_Trace()) fprintf(stderr, "[music] OpenStream failed: %s\n", szPath);
		return false;
	}

	GetSoundSys()->SetStreamLoop(hStream, false);
	GetSoundSys()->SetStreamVolume(hStream, mus_DriverVolume());

	MusicVoice cVoice;
	cVoice.m_hStream    = hStream;
	cVoice.m_fStartAt   = fStartAt;
	cVoice.m_bStarted   = false;
	cVoice.m_bSecondary = bSecondary;
	cVoice.m_sName      = sWaveFile;
	s_aVoices.push_back(cVoice);
	return true;
}

// Schedules every wave of a segment and returns its length in seconds.
// ⚠️ The waves are NOT sequential — each starts at its own musical time and the
// previous one keeps ringing underneath. That overlap is the crossfade.
static float mus_StartSegment(const std::string &sSegFile, bool bSecondary)
{
	const MusicSegment *pSeg = mus_GetSegment(sSegFile);
	if (!pSeg)
		return 0.0f;

	const float fNow = time_GetTime();
	for (size_t i = 0; i < pSeg->m_aWaves.size(); ++i)
		mus_QueueWave(pSeg->m_aWaves[i].m_sFile, fNow + pSeg->m_aWaves[i].m_fStartSec, bSecondary);

	// The live voice count is the leak canary: the driver has 32 stream voices
	// and music must sit at a handful. A number that only ever grows means
	// finished streams are not being retired in LTMusic_Update.
	if (mus_Trace())
		fprintf(stderr, "[music] play segment '%s' (%.2fs, %u waves)%s  [voices=%u]\n",
		        sSegFile.c_str(), pSeg->m_fLenSec, (unsigned)pSeg->m_aWaves.size(),
		        bSecondary ? " [secondary]" : "", (unsigned)s_aVoices.size());
	return pSeg->m_fLenSec;
}

// Begins an intensity: its segment, and the point at which it hands over.
static void mus_EnterIntensity(int nIntensity)
{
	std::map<int, MusicIntensity>::iterator it = s_Intensities.find(nIntensity);
	if (it == s_Intensities.end() || it->second.m_aSegments.empty())
	{
		if (mus_Trace()) fprintf(stderr, "[music] intensity %d not defined — stopping\n", nIntensity);
		s_bPlaying = false;
		return;
	}

	s_nCurIntensity = nIntensity;

	// ★ A multi-segment intensity is a ROUND-ROBIN PLAYLIST, not a set of
	// alternatives. Island's intensity 2 lists 15 segments and points `next` at
	// ITSELF, so the only way it ever reaches segments 2..15 is by advancing a
	// cursor on each entry. Taking the first every time would loop one phrase
	// forever and throw away 14/15ths of that level's score.
	MusicIntensity &cInt = it->second;
	if (cInt.m_nCursor >= cInt.m_aSegments.size())
		cInt.m_nCursor = 0;
	const std::string sSeg = cInt.m_aSegments[cInt.m_nCursor];
	cInt.m_nCursor = (cInt.m_nCursor + 1) % cInt.m_aSegments.size();

	float fLen = mus_StartSegment(sSeg, false);
	if (fLen <= 0.0f)
	{
		// An unreadable segment must not wedge the chain — step on after a beat.
		fLen = 2.0f;
	}
	s_fSegmentEndsAt = time_GetTime() + fLen;
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------
bool LTMusic_Init()
{
	LTMusic_TermLevel();
	return true;
}

void LTMusic_Term()
{
	LTMusic_TermLevel();
}

bool LTMusic_InitLevel(const char *sWorkingDir, const char *sControlFile)
{
	// ★ LT_NO_MUSIC=1 — bisection partner to LT_MUTE_SOUNDS (§76). Refusing at
	// InitLevel means no control file is parsed and no segment ever starts, so
	// the music path contributes no streams at all — which is the point when
	// you are trying to decide whether a noise comes from music or from an
	// ambient sharing the same voice pool.
	const char *pNoMusic = getenv("LT_NO_MUSIC");
	if (pNoMusic && pNoMusic[0] && pNoMusic[0] != '0')
	{
		fprintf(stderr, "[music] DISABLED by LT_NO_MUSIC\n");
		return false;
	}

	LTMusic_TermLevel();

	if (!sWorkingDir || !sControlFile || !*sControlFile)
		return false;

	s_sDir         = sWorkingDir;
	s_sControlFile = sControlFile;

	std::vector<uint8> aData;
	if (!mus_ReadWholeFile(mus_Join(s_sDir, s_sControlFile).c_str(), aData))
	{
		fprintf(stderr, "[music] control file not found: %s\n",
		        mus_Join(s_sDir, s_sControlFile).c_str());
		return false;
	}

	mus_ParseControlFile(aData);
	s_bLevelReady = !s_Intensities.empty();
	s_nVolume     = s_nInitialVolume;

	fprintf(stderr, "[music] %s: %u intensities, %u transitions, initial=%d volume=%d offset=%d"
	                " -> %.1f dB (headroom %.1f%+.1f) = %d/127\n",
	        mus_Join(s_sDir, s_sControlFile).c_str(),
	        (unsigned)s_Intensities.size(), (unsigned)s_Transitions.size(),
	        s_nInitialIntensity, s_nInitialVolume, s_nVolumeOffset,
	        (double)(s_nVolume + s_nVolumeOffset) / 100.0 + kMusicHeadroomDB + mus_ExtraGainDB(),
	        kMusicHeadroomDB, mus_ExtraGainDB(), (int)mus_DriverVolume());

	return s_bLevelReady;
}

void LTMusic_TermLevel()
{
	mus_StopAllVoices(true);
	s_Segments.clear();
	s_Intensities.clear();
	s_Transitions.clear();
	s_sDir.clear();
	s_sControlFile.clear();
	s_bLevelReady = false;
	s_bPlaying = false;
	s_bPaused  = false;
	s_nCurIntensity = 0;
	s_nPendingIntensity = -1;
	s_fSegmentEndsAt = 0.0f;
}

void LTMusic_Play()
{
	if (!s_bLevelReady || s_bPlaying)
		return;
	s_bPlaying = true;
	s_bPaused  = false;
	if (s_nCurIntensity <= 0)
		s_nCurIntensity = (s_nInitialIntensity > 0) ? s_nInitialIntensity : 1;
	mus_EnterIntensity(s_nCurIntensity);
}

void LTMusic_Stop()
{
	s_bPlaying = false;
	s_nPendingIntensity = -1;
	mus_StopAllVoices(true);
}

void LTMusic_Pause(bool bPause)
{
	s_bPaused = bPause;
	for (size_t i = 0; i < s_aVoices.size(); ++i)
		if (s_aVoices[i].m_hStream && s_aVoices[i].m_bStarted)
			GetSoundSys()->PauseStream(s_aVoices[i].m_hStream, bPause ? 1 : 0);
}

void LTMusic_SetVolume(long nVolume)
{
	// ⚠️ Worth tracing: whether the game ever calls this at all decides the final
	// level. The profile's `musicvolume 1250` (+12.5 dB) exactly cancels every
	// control file's VOLUMEOFFSET -1250, so with the call the music sits at the
	// headroom constant and without it a further 12.5 dB below that.
	s_nVolume = nVolume;
	if (mus_Trace() || getenv("LT_TRACE_MUSICVOL"))
		fprintf(stderr, "[music] SetVolume(%ld) -> %.1f dB = %d/127\n",
		        nVolume, (double)(nVolume + s_nVolumeOffset) / 100.0
		                 + kMusicHeadroomDB + mus_ExtraGainDB(),
		        (int)mus_DriverVolume());
	mus_ApplyVolume();
}

void LTMusic_ChangeIntensity(int nNewIntensity)
{
	// ⚠️ THE PENDING TARGET COUNTS AS "ALREADY THERE".
	// While a transition segment bridges to nNewIntensity, s_nCurIntensity still
	// holds the OLD intensity — so comparing against it alone lets every repeat
	// of the same request restart the bridge. The game re-asserts its mood
	// continuously, which turned into `23 -> 41 via TransOhioCE0` firing ~5x a
	// second: 555 segments in 110s instead of ~15, with the stream churn to
	// match. Only a request for a DIFFERENT destination may interrupt a
	// transition.
	if (!s_bLevelReady || nNewIntensity <= 0 ||
	    nNewIntensity == s_nCurIntensity || nNewIntensity == s_nPendingIntensity)
		return;

	// A transition is itself a segment. When one is authored for this pair we
	// play it now and queue the destination behind it; otherwise we cut across
	// immediately. ⚠️ An authored transition with an EMPTY segment name means
	// "switch with no bridge" and must not be treated as a missing entry.
	std::map<uint32, std::string>::iterator it =
		s_Transitions.find((uint32)s_nCurIntensity * 4096u + (uint32)nNewIntensity);

	if (s_bPlaying && it != s_Transitions.end() && !it->second.empty())
	{
		mus_StopAllVoices(false);
		float fLen = mus_StartSegment(it->second, false);
		if (fLen > 0.0f)
		{
			s_nPendingIntensity = nNewIntensity;
			s_fSegmentEndsAt    = time_GetTime() + fLen;
			if (mus_Trace())
				fprintf(stderr, "[music] intensity %d -> %d via '%s'\n",
				        s_nCurIntensity, nNewIntensity, it->second.c_str());
			return;
		}
	}

	if (mus_Trace())
		fprintf(stderr, "[music] intensity %d -> %d (direct)\n", s_nCurIntensity, nNewIntensity);
	s_nPendingIntensity = -1;
	if (s_bPlaying)
	{
		mus_StopAllVoices(false);
		mus_EnterIntensity(nNewIntensity);
	}
	else
	{
		s_nCurIntensity = nNewIntensity;
	}
}

int LTMusic_GetCurIntensity()     { return s_nCurIntensity; }
int LTMusic_GetNumIntensities()   { return s_nNumIntensities; }
int LTMusic_GetInitialIntensity() { return s_nInitialIntensity; }
int LTMusic_GetInitialVolume()    { return s_nInitialVolume; }
int LTMusic_GetVolumeOffset()     { return s_nVolumeOffset; }

void LTMusic_PlaySecondary(const char *sSegment)
{
	if (!s_bLevelReady || !sSegment || !*sSegment)
		return;
	mus_StartSegment(sSegment, true);
}

void LTMusic_StopSecondary(const char * /*sSegment*/)
{
	// Secondary segments are one-shots; retire them all rather than tracking
	// names (nothing in the retail data stops a specific one).
	for (size_t i = 0; i < s_aVoices.size(); )
	{
		if (s_aVoices[i].m_bSecondary)
		{
			if (s_aVoices[i].m_hStream)
			{
				GetSoundSys()->PauseStream(s_aVoices[i].m_hStream, 1);
				GetSoundSys()->CloseStream(s_aVoices[i].m_hStream);
			}
			s_aVoices.erase(s_aVoices.begin() + i);
		}
		else ++i;
	}
}

void LTMusic_Update()
{
	if (!s_bLevelReady || s_bPaused)
		return;

	const float fNow = time_GetTime();

	// 1. Start any voice whose musical time has arrived, and retire finished
	//    ones. ⚠️ A voice is only DONE once it has actually been started —
	//    GetStreamStatus reports LS_DONE for a stream that "has never been
	//    started" too, so reaping on status alone would kill every wave that is
	//    still waiting for its entry.
	for (size_t i = 0; i < s_aVoices.size(); )
	{
		MusicVoice &cVoice = s_aVoices[i];
		if (!cVoice.m_bStarted)
		{
			if (fNow >= cVoice.m_fStartAt)
			{
				GetSoundSys()->StartStream(cVoice.m_hStream);
				GetSoundSys()->SetStreamVolume(cVoice.m_hStream, mus_DriverVolume());
				cVoice.m_bStarted = true;
				if (mus_Trace())
					fprintf(stderr, "[music]   + %s\n", cVoice.m_sName.c_str());
			}
			++i;
			continue;
		}

		uint32 nStatus = GetSoundSys()->GetStreamStatus(cVoice.m_hStream);
		if (nStatus & (LS_DONE | LS_STOPPED))
		{
			GetSoundSys()->CloseStream(cVoice.m_hStream);
			s_aVoices.erase(s_aVoices.begin() + i);
		}
		else ++i;
	}

	// 2. Hand over when the current segment's musical length has elapsed. The
	//    tails of its waves deliberately keep playing underneath the new one —
	//    that is what makes the score continuous.
	if (s_bPlaying && fNow >= s_fSegmentEndsAt)
	{
		if (s_nPendingIntensity > 0)
		{
			int nNext = s_nPendingIntensity;
			s_nPendingIntensity = -1;
			mus_EnterIntensity(nNext);
		}
		else
		{
			std::map<int, MusicIntensity>::iterator it = s_Intensities.find(s_nCurIntensity);
			int nNext = (it != s_Intensities.end()) ? it->second.m_nNext : 0;
			if (nNext > 0)
			{
				// The authored chain (Explore1 -> 1A -> 1B ... -> back to 2) is
				// what gives the score its variation; just following it is the
				// whole mechanism.
				mus_EnterIntensity(nNext);
			}
			else
			{
				s_bPlaying = false;
				if (mus_Trace()) fprintf(stderr, "[music] chain ended at intensity %d\n", s_nCurIntensity);
			}
		}
	}
}
