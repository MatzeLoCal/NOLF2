#include "bdefs.h"

#include "musicmgr.h"
#include "console.h"
#include "ltpvalue.h"
#include "musicdriver.h"


void music_ConsolePrint( const char *pMsg, ... )
{
	va_list		marker;
	char		str[500];

	va_start(marker, pMsg);
	LTVSNPrintF(str, sizeof(str), pMsg, marker);
	va_end(marker);

	con_PrintString(CONRGB(100,255,100), 0, str);
}

#ifdef LT_MACOS

// DirectMusic is a Windows-only API and the music DLL it loads does not exist on
// macOS, so the driver is disabled here.  These stubs report "no music driver"
// cleanly; game audio is a later (Phase 4) concern, to be revisited via
// CoreAudio / OpenAL.
musicdriver_status music_InitDriver( char *pMusicDLLName, SMusicMgr *pMusicMgr )
{
	if( pMusicMgr )
		pMusicMgr->m_bValid = 0;
	return MUSICDRIVER_CANTLOADLIBRARY;
}

void music_TermDriver()
{
}

#else // !LT_MACOS

static HINSTANCE g_hMusicDLL = 0;
static SMusicMgr *g_pMusicMgr = LTNULL;

musicdriver_status music_InitDriver( char *pMusicDLLName, SMusicMgr *pMusicMgr )
{
	MusicDLLSetupFn pSetupFn;

	ASSERT( pMusicMgr );
	ASSERT( pMusicDLLName );
	ASSERT( strlen( pMusicDLLName ));
	if( !pMusicMgr || !pMusicDLLName )
		return MUSICDRIVER_INVALIDOPTIONS;

	music_TermDriver( );
	pMusicMgr->m_bValid = 0;

	g_hMusicDLL = LoadLibrary( pMusicDLLName );
	if( !g_hMusicDLL )
	{
		DWORD error;
		error = GetLastError( );
		return MUSICDRIVER_CANTLOADLIBRARY;
	}

	// Have the driver setup all the function pointers.
	pSetupFn = (MusicDLLSetupFn)GetProcAddress(g_hMusicDLL, "MusicDLLSetup");
	if(!pSetupFn)
	{
		FreeLibrary(g_hMusicDLL);
		g_hMusicDLL = LTNULL;
		return MUSICDRIVER_INVALIDDLL;
	}

	pSetupFn( pMusicMgr );
	pMusicMgr->ConsolePrint = music_ConsolePrint;

	if( !pMusicMgr->Init( pMusicMgr ))
	{
		FreeLibrary(g_hMusicDLL);
		g_hMusicDLL = LTNULL;
		return MUSICDRIVER_INVALIDOPTIONS;
	}

	g_pMusicMgr = pMusicMgr;

// {BP 1/2/98}  Commented this out becuase this is supposed to be in DLL
//	pMusicMgr->m_bValid = 1;

	return MUSICDRIVER_OK;
}


void music_TermDriver()
{
	if( g_pMusicMgr )
	{
		g_pMusicMgr->Term( );
		g_pMusicMgr->m_bValid = 0;
		g_pMusicMgr = LTNULL;
	}

	if( g_hMusicDLL )
	{
		FreeLibrary( g_hMusicDLL );
		g_hMusicDLL = LTNULL;
	}
}

#endif // LT_MACOS
