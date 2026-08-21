//------------------------------------------------------------------
//
//	FILE	  : Console.cpp
//
//	PURPOSE	  : Implements the console.
//
//	CREATED	  : May 7 1997
//
//	COPYRIGHT : Microsoft 1997 All Rights Reserved
//
// ------------------------------------------------------------------ //

#include "bdefs.h"

#include "sysconsole_impl.h"
#include "ltpvalue.h"


// ------------------------------------------------------------------ //
// Console interface functions.
// ------------------------------------------------------------------ //

void con_Term(bool bDeleteTextLines)
{
	GETCONSOLE()->Term( bDeleteTextLines );
}

bool con_InitBare()
{
	return GETCONSOLE()->InitBare();
}

LTRESULT con_LoadBackground()
{
	return GETCONSOLE()->LoadBackground();
}

bool con_Init(LTRect *pRect, CommandHandler handler, RenderStruct *pStruct)
{
	return GETCONSOLE()->Init( pRect, handler, pStruct );
}

void con_SetErrorLog(ErrorLogFn fn)
{
	GETCONSOLE()->SetErrorLogFn( fn );
}

void con_CycleCommandsBack()
{
	GETCONSOLE()->CycleCommands( -1 );
}

void con_CycleCommandsForward()
{
	GETCONSOLE()->CycleCommands( 1 );
}

void con_Draw()
{
	if (dsi_IsConsoleEnabled () == TRUE)
		GETCONSOLE()->Draw();
}

void con_DrawSmall(int nLines)
{
	if (dsi_IsConsoleEnabled () == TRUE)
		GETCONSOLE()->DrawSmall( nLines );
}

void con_PrintString(CONCOLOR theColor, int filterLevel, const char *pMsg)
{
	// ⚠️ Second half of LT_TRACE_CONSOLE (see dsi_PrintToConsole in linuxdsys.cpp).
	// There are TWO separate console paths and mirroring only one is a trap:
	//   dsi_ConsolePrint -> dsi_PrintToConsole   ENGINE-side diagnostics
	//   g_pLTClient->CPrint -> con_PrintString   GAME-DLL diagnostics (this one)
	// The game DLLs (ObjectDLL / ClientShellDLL) use CPrint exclusively, so
	// without this every NOLF2-side diagnostic goes only to the in-game console
	// overlay — invisible to a headless or logged run, and its absence from a log
	// reads as "this code never ran".
	static int s_nTraceConsole = -1;
	if (s_nTraceConsole < 0)
		s_nTraceConsole = getenv("LT_TRACE_CONSOLE") ? 1 : 0;

	if (s_nTraceConsole && pMsg)
	{
		// CPrint appends its own '\n'; don't double it up.
		size_t nLen = strlen(pMsg);
		if (nLen && pMsg[nLen - 1] == '\n')
			printf("[con] %.*s\n", (int)(nLen - 1), pMsg);
		else
			printf("[con] %s\n", pMsg);
		fflush(stdout);
	}

	GETCONSOLE()->PrintString( theColor, filterLevel, pMsg );
}

void con_Printf(CONCOLOR theColor, int filterLevel, const char *pMsg, ...)
{
	va_list	marker;
	va_start( marker, pMsg );
	GETCONSOLE()->vPrintf( theColor, filterLevel, pMsg, marker );
	va_end( marker );
}

void con_WhitePrintf(const char *pMsg, ...)
{
	va_list		marker;
	va_start( marker, pMsg );
	GETCONSOLE()->vPrintf( CONRGB(255,255,255), 0, pMsg, marker );
	va_end( marker );
}

void con_OnKeyPress(uint32 key)
{
	if (dsi_IsConsoleEnabled () == TRUE)
		GETCONSOLE()->OnKeyPress( key );
}
