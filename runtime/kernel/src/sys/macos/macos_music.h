// ----------------------------------------------------------------------- //
//
// MODULE  : macos_music.h
//
// PURPOSE : NOLF2's interactive music, natively — the real thing, not a stub.
//
//           ★ WHY THIS IS NOT A DIRECTMUSIC REIMPLEMENTATION.
//           §37.D assumed the 1542 .SGT files were DirectMusic MIDI segments
//           and that playing them needed a DLS synthesiser, styles, bands and
//           chordmaps. Reading the data says otherwise: a .SGT here averages
//           817 BYTES and is a `wavt` WAVE TRACK — a playlist of `DMRF`
//           references naming external .wav files with musical timings. Those
//           1643 waves are 155 MB of MP3-in-WAV, which is exactly the format
//           §23 already decodes for dialogue. Across all 12 control files there
//           are ZERO DLS banks, ZERO chordmaps and ZERO bands; the only style
//           is one "Fade" motif. So the synthesiser — the expensive part — is
//           not used by this game at all.
//
//           What playback actually needs:
//             * a text parser for the level control file (INTENSITY /
//               TRANSITION lines);
//             * a small RIFF reader for the .SGT wave track;
//             * a few CONCURRENT streams, because a segment's waves OVERLAP
//               (JAPAN/COMBAT1 layers three with 3.5 s and 3.6 s overlaps, and
//               Siberia chains 5.6 s segments built from 11.3 s waves). This is
//               why "loop one .sgt per mood" sounds wrong: it cuts every
//               crossfade the music is written around.
//           The CoreAudio driver already offers 32 stream voices; music uses a
//           handful.
//
// ----------------------------------------------------------------------- //
#ifndef __MACOS_MUSIC_H__
#define __MACOS_MUSIC_H__

// Lifetime.
bool LTMusic_Init();
void LTMusic_Term();

// A level's music set. sWorkingDir is a game-relative directory ("MUSIC\JAPAN")
// and sControlFile the .txt inside it. Returns false if either cannot be read.
bool LTMusic_InitLevel(const char *sWorkingDir, const char *sControlFile);
void LTMusic_TermLevel();

// Transport.
void LTMusic_Play();
void LTMusic_Stop();
void LTMusic_Pause(bool bPause);

// ⚠️ A fixed music HEADROOM is applied on top of the authored numbers, because
// the shipped profile's `musicvolume 1250` exactly cancels every control file's
// `VOLUMEOFFSET -1250` and retail's synth had headroom we do not. Trim it by ear
// with `LT_MUSIC_GAIN=<dB>` — see macos_music.cpp.
//
// Volume in HUNDREDTHS OF A DECIBEL (the control file's own unit — Japan ships
// VOLUMEOFFSET -1250, i.e. -12.5 dB), not a 0..100 percentage.
void LTMusic_SetVolume(long nVolume);

// The mood system. Intensities are numbered from 1 and chain to each other
// through their control-file `next` field, which is what gives the score its
// variation without any extra logic here.
void LTMusic_ChangeIntensity(int nNewIntensity);
int  LTMusic_GetCurIntensity();
int  LTMusic_GetNumIntensities();
int  LTMusic_GetInitialIntensity();
int  LTMusic_GetInitialVolume();
int  LTMusic_GetVolumeOffset();

// One-shot segment layered over the current music.
void LTMusic_PlaySecondary(const char *sSegment);
void LTMusic_StopSecondary(const char *sSegment);

// ⚠️ MUST BE CALLED EVERY FRAME. ILTDirectMusicMgr has no Update() of its own,
// so this is driven from CClientMgr::Update alongside the sound manager. All
// scheduling (starting a wave when its musical time arrives, retiring finished
// streams, advancing to the next intensity when a segment ends) happens here;
// without the tick the first segment plays and the music then stops dead.
void LTMusic_Update();

#endif // __MACOS_MUSIC_H__
