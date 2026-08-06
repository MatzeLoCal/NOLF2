// Stub of <dsound.h> (DirectSound) for the macOS build.
//
// DirectSound is Windows-only.  The engine's sound headers (soundmgr.h,
// ltdirectsound.h, soundbuffer.h ...) name the DirectSound interfaces, buffer
// descriptors and capability/error constants in their API, but the actual
// DirectSound calls live in a separate Windows sound driver that is NOT built on
// macOS.  So this only needs to provide the named types/structs/constants so the
// shared headers parse; no method is ever invoked.  Audio is a later (Phase 4)
// concern, to be implemented via OpenAL / CoreAudio.
#ifndef __LT_COMPAT_DSOUND_H__
#define __LT_COMPAT_DSOUND_H__

#include <windows.h>     // WAVEFORMATEX, GUID, DWORD, HRESULT, ...
#include <unknwn.h>

// --- Interfaces (opaque; only ever held as pointers) ------------------------
struct IDirectSound;
struct IDirectSound8;
struct IDirectSoundBuffer;
struct IDirectSoundBuffer8;
struct IDirectSound3DListener;
struct IDirectSound3DListener8;
struct IDirectSound3DBuffer;
struct IDirectSound3DBuffer8;
struct IDirectSoundFXChorus;
struct IDirectSoundFXCompressor;
struct IDirectSoundFXDistortion;
struct IDirectSoundFXEcho;
struct IDirectSoundFXFlanger;
struct IDirectSoundFXParamEq;
struct IDirectSoundFXI3DL2Reverb8;

typedef IDirectSound*              LPDIRECTSOUND;
typedef IDirectSound8*             LPDIRECTSOUND8;
typedef IDirectSoundBuffer*        LPDIRECTSOUNDBUFFER;
typedef IDirectSoundBuffer8*       LPDIRECTSOUNDBUFFER8;
typedef IDirectSound3DListener*    LPDIRECTSOUND3DLISTENER;
typedef IDirectSound3DListener8*   LPDIRECTSOUND3DLISTENER8;
typedef IDirectSound3DBuffer*      LPDIRECTSOUND3DBUFFER;
typedef IDirectSound3DBuffer8*     LPDIRECTSOUND3DBUFFER8;
typedef IDirectSoundFXChorus*      LPDIRECTSOUNDFXCHORUS;
typedef IDirectSoundFXCompressor*  LPDIRECTSOUNDFXCOMPRESSOR;
typedef IDirectSoundFXDistortion*  LPDIRECTSOUNDFXDISTORTION;
typedef IDirectSoundFXEcho*        LPDIRECTSOUNDFXECHO;
typedef IDirectSoundFXFlanger*     LPDIRECTSOUNDFXFLANGER;
typedef IDirectSoundFXParamEq*     LPDIRECTSOUNDFXPARAMEQ;
typedef IDirectSoundFXI3DL2Reverb8* LPDIRECTSOUNDFXI3DL2REVERB8;

// --- Descriptors / capability structs ---------------------------------------
typedef struct _DSCAPS {
    DWORD dwSize, dwFlags, dwMinSecondarySampleRate, dwMaxSecondarySampleRate;
    DWORD dwPrimaryBuffers, dwMaxHwMixingAllBuffers, dwMaxHw3DAllBuffers;
    DWORD dwFreeHwMixingAllBuffers, dwFreeHw3DAllBuffers, dwTotalHwMemBytes;
    DWORD dwFreeHwMemBytes, dwMaxContigFreeHwMemBytes;
} DSCAPS, *LPDSCAPS;

typedef struct _DSBCAPS {
    DWORD dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead;
} DSBCAPS, *LPDSBCAPS;

typedef struct _DSBUFFERDESC {
    DWORD dwSize, dwFlags, dwBufferBytes, dwReserved;
    WAVEFORMATEX* lpwfxFormat;
    GUID guid3DAlgorithm;
} DSBUFFERDESC, *LPDSBUFFERDESC;

// --- Result code & capability/status/scope flags ----------------------------
#ifndef DS_OK
#define DS_OK            ((HRESULT)0)
#endif
#define DSERR_GENERIC            ((HRESULT)0x88780000)
#define DSERR_ALLOCATED          ((HRESULT)0x8878000A)
#define DSERR_ALREADYINITIALIZED ((HRESULT)0x88780052)
#define DSERR_BADFORMAT          ((HRESULT)0x88780064)
#define DSERR_BUFFERLOST         ((HRESULT)0x88780096)
#define DSERR_CONTROLUNAVAIL     ((HRESULT)0x8878001E)
#define DSERR_INVALIDCALL        ((HRESULT)0x88780032)
#define DSERR_INVALIDPARAM       ((HRESULT)0x80070057)
#define DSERR_NOAGGREGATION      ((HRESULT)0x80040110)
#define DSERR_NODRIVER           ((HRESULT)0x88780078)
#define DSERR_NOINTERFACE        ((HRESULT)0x80004002)
#define DSERR_OTHERAPPHASPRIO    ((HRESULT)0x887800A0)
#define DSERR_OUTOFMEMORY        ((HRESULT)0x8007000E)
#define DSERR_PRIOLEVELNEEDED    ((HRESULT)0x88780046)
#define DSERR_UNINITIALIZED      ((HRESULT)0x887800AA)
#define DSERR_UNSUPPORTED        ((HRESULT)0x80004001)

#define DSCAPS_PRIMARYBUFFER     0x00000001

#define DSBCAPS_PRIMARYBUFFER         0x00000001
#define DSBCAPS_STATIC                0x00000002
#define DSBCAPS_LOCHARDWARE           0x00000004
#define DSBCAPS_LOCSOFTWARE           0x00000008
#define DSBCAPS_CTRL3D                0x00000010
#define DSBCAPS_CTRLFREQUENCY         0x00000020
#define DSBCAPS_CTRLPAN               0x00000040
#define DSBCAPS_CTRLVOLUME            0x00000080
#define DSBCAPS_CTRLFX                0x00000200
#define DSBCAPS_MUTE3DATMAXDISTANCE   0x00020000

#define DSBPLAY_LOOPING          0x00000001
#define DSBSTATUS_PLAYING        0x00000001
#define DSBSTATUS_BUFFERLOST     0x00000002

#define DSSCL_NORMAL             0x00000001
#define DSSCL_PRIORITY           0x00000002

#endif // __LT_COMPAT_DSOUND_H__
