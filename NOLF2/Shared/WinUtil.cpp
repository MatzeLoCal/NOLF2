#include "stdafx.h"
#include "stdio.h"
#include "sys/stat.h"
#include "winutil.h"
#include <time.h>
#include <direct.h>
#include <IO.h>
#include "ltbasedefs.h"

#ifdef LT_MACOS
// ------------------------------------------------------------------------
// POSIX implementation of CWinUtil (the game's Win32 filesystem/utility
// wrapper). Semantics mirror the Win32 original below: DirExist is a loose
// stat (matches files too), EmptyDir removes plain entries only, CopyDir
// copies the regular files in a directory (not recursive).
// ------------------------------------------------------------------------
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <copyfile.h>
#include <string.h>

BOOL CWinUtil::GetMoviesPath (char* strPath)
{
	// No CD-drive scan on macOS; honour the working-directory case only.
	strPath[0] = '\0';

	char strTemp[256];
	if (_getcwd (strTemp, 255))
	{
		char strFile[270];
		if (strTemp[strlen(strTemp) - 1] != '/') strcat (strTemp, "/");
		SAFE_STRCPY(strFile, strTemp);
		strcat (strFile, "intro.smk");

		if (FileExist (strFile))
		{
			SAFE_STRCPY(strPath, strTemp);
			return TRUE;
		}
	}

	return FALSE;
}

// Both separators tolerated everywhere: the game builds paths with '\\'.
static void lt_NormalizePath(char* p)
{
	for (; *p; ++p)
		if (*p == '\\') *p = '/';
}

BOOL CWinUtil::DirExist (char const* strPath)
{
	if (!strPath || !*strPath) return FALSE;

	char szPath[MAX_PATH];
	SAFE_STRCPY( szPath, strPath );
	lt_NormalizePath( szPath );

	size_t nLen = strlen(szPath);
	if (nLen > 1 && szPath[nLen - 1] == '/')
		szPath[nLen - 1] = '\0';

	struct stat statbuf;
	return (stat (szPath, &statbuf) != -1) ? TRUE : FALSE;
}

BOOL CWinUtil::CreateDir (char const* strPath)
{
	if (DirExist (strPath)) return TRUE;

	char szPath[MAX_PATH];
	SAFE_STRCPY( szPath, strPath );
	lt_NormalizePath( szPath );

	char strPartialPath[MAX_PATH];
	strPartialPath[0] = '\0';
	if (szPath[0] == '/') strcat (strPartialPath, "/");

	char* token = strtok (szPath, "/");
	while (token)
	{
		strcat (strPartialPath, token);
		if (!DirExist (strPartialPath))
		{
			if (mkdir (strPartialPath, 0755) != 0) return FALSE;
		}
		strcat (strPartialPath, "/");
		token = strtok (NULL, "/");
	}

	return TRUE;
}

BOOL CWinUtil::FileExist (char const* strPath)
{
	if (!strPath || !*strPath) return FALSE;

	char szPath[MAX_PATH];
	SAFE_STRCPY( szPath, strPath );
	lt_NormalizePath( szPath );

	struct stat statbuf;
	if (stat (szPath, &statbuf) != 0) return FALSE;
	return S_ISREG(statbuf.st_mode) ? TRUE : FALSE;
}

BOOL CWinUtil::CopyDir( char const* pSrc, char const* pDest )
{
	if( !pSrc || !pDest ) return FALSE;
	if( !DirExist( pSrc )) return FALSE;

	if( !DirExist( pDest ))
		CreateDir( pDest );

	char szSrc[MAX_PATH];
	SAFE_STRCPY( szSrc, pSrc );
	lt_NormalizePath( szSrc );

	DIR* pDir = opendir( szSrc );
	if( !pDir ) return FALSE;

	BOOL bOk = TRUE;
	struct dirent* pEntry;
	char szSrcFile[MAX_PATH], szDestFile[MAX_PATH];
	while( (pEntry = readdir( pDir )) != NULL )
	{
		snprintf( szSrcFile, sizeof(szSrcFile), "%s/%s", szSrc, pEntry->d_name );
		if( !FileExist( szSrcFile ))   // regular files only, like the original
			continue;

		snprintf( szDestFile, sizeof(szDestFile), "%s/%s", pDest, pEntry->d_name );
		lt_NormalizePath( szDestFile );
		if( copyfile( szSrcFile, szDestFile, NULL, COPYFILE_ALL ) != 0 )
		{
			bOk = FALSE;
			break;
		}
	}
	closedir( pDir );

	return bOk;
}

BOOL CWinUtil::EmptyDir( char const* pDir )
{
	if( !pDir ) return FALSE;
	if( !DirExist( pDir )) return FALSE;

	char szDir[MAX_PATH];
	SAFE_STRCPY( szDir, pDir );
	lt_NormalizePath( szDir );

	DIR* pD = opendir( szDir );
	if( !pD ) return FALSE;

	struct dirent* pEntry;
	char szDelFile[MAX_PATH];
	while( (pEntry = readdir( pD )) != NULL )
	{
		if( strcmp( pEntry->d_name, "." ) == 0 || strcmp( pEntry->d_name, ".." ) == 0 )
			continue;
		snprintf( szDelFile, sizeof(szDelFile), "%s/%s", szDir, pEntry->d_name );
		remove( szDelFile );   // plain entries only, like the original
	}
	closedir( pD );

	return TRUE;
}

BOOL CWinUtil::RemoveDir( char const* pDir )
{
	if( !pDir ) return FALSE;
	if( !DirExist( pDir )) return FALSE;
	if( !EmptyDir( pDir )) return FALSE;

	_rmdir( pDir );

	return TRUE;
}

// --- minimal .ini reader/writer (GetPrivateProfileString semantics) --------
// Format handled: [Section] lines and Key=Value lines; comparisons are
// case-insensitive like the Win32 originals.

static bool lt_IniSectionMatches(const char* line, const char* section)
{
	if (*line != '[') return false;
	const char* end = strchr(line, ']');
	if (!end) return false;
	size_t n = (size_t)(end - line) - 1;
	return (strlen(section) == n) && (strncasecmp(line + 1, section, n) == 0);
}

DWORD CWinUtil::WinGetPrivateProfileString (const char* lpAppName, const char* lpKeyName, const char* lpDefault, char* lpReturnedString, DWORD nSize, const char* lpFileName)
{
	if (!lpReturnedString || nSize == 0) return 0;
	lpReturnedString[0] = '\0';
	if (lpDefault)
	{
		strncpy(lpReturnedString, lpDefault, nSize - 1);
		lpReturnedString[nSize - 1] = '\0';
	}

	char szFile[MAX_PATH];
	SAFE_STRCPY( szFile, lpFileName ? lpFileName : "" );
	lt_NormalizePath( szFile );

	FILE* pFile = fopen(szFile, "r");
	if (!pFile) return (DWORD)strlen(lpReturnedString);

	bool bInSection = false;
	char line[1024];
	size_t nKeyLen = lpKeyName ? strlen(lpKeyName) : 0;
	while (fgets(line, sizeof(line), pFile))
	{
		// strip leading whitespace + trailing newline
		char* p = line;
		while (*p == ' ' || *p == '\t') ++p;
		p[strcspn(p, "\r\n")] = '\0';

		if (*p == '[')
		{
			bInSection = lpAppName ? lt_IniSectionMatches(p, lpAppName) : false;
			continue;
		}
		if (!bInSection || !lpKeyName) continue;

		if (strncasecmp(p, lpKeyName, nKeyLen) == 0)
		{
			char* eq = p + nKeyLen;
			while (*eq == ' ' || *eq == '\t') ++eq;
			if (*eq != '=') continue;
			++eq;
			while (*eq == ' ' || *eq == '\t') ++eq;
			strncpy(lpReturnedString, eq, nSize - 1);
			lpReturnedString[nSize - 1] = '\0';
			break;
		}
	}
	fclose(pFile);

	return (DWORD)strlen(lpReturnedString);
}

DWORD CWinUtil::WinWritePrivateProfileString (const char* lpAppName, const char* lpKeyName, const char* lpString, const char* lpFileName)
{
	if (!lpAppName || !lpFileName) return 0;

	char szFile[MAX_PATH];
	SAFE_STRCPY( szFile, lpFileName );
	lt_NormalizePath( szFile );

	// Read the whole file, then rewrite with the key set/replaced/removed.
	std::string sOut;
	bool bInSection = false, bWroteKey = false, bSawSection = false;
	size_t nKeyLen = lpKeyName ? strlen(lpKeyName) : 0;

	FILE* pFile = fopen(szFile, "r");
	if (pFile)
	{
		char line[1024];
		while (fgets(line, sizeof(line), pFile))
		{
			char clean[1024];
			strncpy(clean, line, sizeof(clean) - 1); clean[sizeof(clean) - 1] = '\0';
			clean[strcspn(clean, "\r\n")] = '\0';

			if (clean[0] == '[')
			{
				// leaving the target section without having written the key?
				if (bInSection && !bWroteKey && lpKeyName && lpString)
				{
					sOut += lpKeyName; sOut += "="; sOut += lpString; sOut += "\n";
					bWroteKey = true;
				}
				bInSection = lt_IniSectionMatches(clean, lpAppName);
				if (bInSection) bSawSection = true;
				sOut += clean; sOut += "\n";
				continue;
			}

			if (bInSection && lpKeyName && nKeyLen &&
			    strncasecmp(clean, lpKeyName, nKeyLen) == 0)
			{
				const char* eq = clean + nKeyLen;
				while (*eq == ' ' || *eq == '\t') ++eq;
				if (*eq == '=')
				{
					// replace (or drop when lpString is NULL)
					if (lpString && !bWroteKey)
					{
						sOut += lpKeyName; sOut += "="; sOut += lpString; sOut += "\n";
						bWroteKey = true;
					}
					continue;
				}
			}

			sOut += clean; sOut += "\n";
		}
		fclose(pFile);
	}

	if (!bWroteKey && lpKeyName && lpString)
	{
		if (!bSawSection)
		{
			sOut += "["; sOut += lpAppName; sOut += "]\n";
		}
		else if (bInSection)
		{
			// section was the last one in the file; key just appends
		}
		else
		{
			sOut += "["; sOut += lpAppName; sOut += "]\n";
		}
		sOut += lpKeyName; sOut += "="; sOut += lpString; sOut += "\n";
	}

	pFile = fopen(szFile, "w");
	if (!pFile) return 0;
	fwrite(sOut.c_str(), 1, sOut.size(), pFile);
	fclose(pFile);

	return 1;
}

void CWinUtil::DebugOut (char const* str)
{
	OutputDebugString (str);
}

void CWinUtil::DebugBreak()
{
	::DebugBreak();
}

float CWinUtil::GetTime()
{
	return (float)GetTickCount() / 1000.0f;
}

char* CWinUtil::GetFocusWindow()
{
	return NULL;   // no Win32 window focus concept; callers null-check
}

void CWinUtil::WriteToDebugFile (char const* strText)
{
	FILE* pFile = fopen ("/tmp/shodebug.txt", "a+");
	if (!pFile) return;

	time_t seconds;
	time (&seconds);
	struct tm* timedate = localtime (&seconds);
	if (!timedate) { fclose(pFile); return; }

	char strTimeDate[128];
	sprintf (strTimeDate, "[%02d/%02d/%02d %02d:%02d:%02d]  ", timedate->tm_mon + 1, timedate->tm_mday, (timedate->tm_year + 1900) % 100, timedate->tm_hour, timedate->tm_min, timedate->tm_sec);
	fwrite (strTimeDate, strlen(strTimeDate), 1, pFile);

	fwrite (strText, strlen(strText), 1, pFile);
	fwrite ("\n", 1, 1, pFile);

	fclose (pFile);
}
// (macOS implementation ends at the #else below; original Win32 follows.)

#else // !LT_MACOS -- original Win32 implementation

BOOL CWinUtil::GetMoviesPath (char* strPath)
{
	char chDrive   = 'A';
	strPath[0] = '\0';

	char strTemp[256];
	if (_getcwd (strTemp, 255))
	{
		char strFile[270];
		if (strTemp[strlen(strTemp) - 1] != '\\') strcat (strTemp, "\\");
		SAFE_STRCPY(strFile, strTemp);
		strcat (strFile, "intro.smk");

		if (FileExist (strFile))
		{
			SAFE_STRCPY(strPath, strTemp);
			return TRUE;
		}
	}

	while (chDrive <= 'Z')
	{
		sprintf(strPath, "%c:\\", chDrive);

		if (GetDriveType(strPath) == DRIVE_CDROM)
		{
			strcat(strPath, "Movies\\");
			if (DirExist (strPath)) return TRUE;
		}

		chDrive++;
	}

	strPath[0] = '\0';

	return FALSE;
}

BOOL CWinUtil::DirExist (char const* strPath)
{
	if (!strPath || !*strPath) return FALSE;

	BOOL bDirExists = FALSE;

	char szPath[MAX_PATH];
	SAFE_STRCPY( szPath, strPath );

	if (szPath[strlen(szPath) - 1] == '\\')
	{
		szPath[strlen(szPath) - 1] = '\0';
	}

	UINT oldErrorMode = SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	struct stat statbuf;
	int error = stat (szPath, &statbuf);
	SetErrorMode (oldErrorMode);
	if (error != -1) bDirExists = TRUE;

	return bDirExists;
}

BOOL CWinUtil::CreateDir (char const* strPath)
{
	if (DirExist (strPath)) return TRUE;
	if (strPath[strlen(strPath) - 1] == ':') return FALSE;		// special case

	char strPartialPath[MAX_PATH];
	strPartialPath[0] = '\0';

	// Make a copy of the input since we're going to change it.
	char szPath[MAX_PATH];
	SAFE_STRCPY( szPath, strPath );

	char* token = strtok (szPath, "\\");
	while (token)
	{
		strcat (strPartialPath, token);
		if (!DirExist (strPartialPath) && strPartialPath[strlen(strPartialPath) - 1] != ':')
		{
			if (!CreateDirectory (strPartialPath, NULL)) return FALSE;
		}
		strcat (strPartialPath, "\\");
		token = strtok (NULL, "\\");
	}

	return TRUE;
}

BOOL CWinUtil::FileExist (char const* strPath)
{
	OFSTRUCT ofs;
	HFILE hFile = OpenFile (strPath, &ofs, OF_EXIST);
	if (hFile == HFILE_ERROR) return FALSE;

	return TRUE;
}

BOOL CWinUtil::CopyDir( char const* pSrc, char const* pDest )
{
	if( !pSrc || !pDest ) return FALSE;

	if( !DirExist( pSrc )) return FALSE;

	// CreateDir does not preserve the path...
	char szDir[MAX_PATH] = {0};
	strcpy( szDir, pDest );

	if( !DirExist( szDir ))
		CreateDir( szDir );

	char szDestFile[MAX_PATH] = {0};
	char szFiles[MAX_PATH] = {0};
	sprintf(szFiles, "%s\\*.*", pSrc);

	struct _finddata_t file;
	long hFile;
	StringSet fileList;

	// find first file
	if((hFile = _findfirst(szFiles, &file)) != -1L)
	{
		do
		{
			sprintf( szFiles, "%s\\%s", pSrc, file.name );
			if( FileExist( szFiles ))
				fileList.insert(file.name);
		}
		while(_findnext(hFile, &file) == 0);
	}
	_findclose(hFile);

	StringSet::iterator iter = fileList.begin();
	while (iter != fileList.end())
	{
		sprintf(szFiles,"%s\\%s",pSrc,iter->c_str());
		sprintf(szDestFile, "%s\\%s", pDest, iter->c_str());
		
		if( !CopyFile( szFiles, szDestFile, FALSE ))
			return FALSE;

		iter++;
	}
	
	return TRUE;
}

BOOL CWinUtil::EmptyDir( char const* pDir )
{
	if( !pDir ) return FALSE;

	if( !DirExist( pDir )) return FALSE;

	char szDelFile[MAX_PATH] = {0};
	char szFiles[MAX_PATH] = {0};
	sprintf( szFiles, "%s\\*.*", pDir);

	struct _finddata_t file;
	long hFile;
	StringSet fileList;

	// find first file...
	if( (hFile = _findfirst( szFiles, &file )) != -1L)
	{
		do
		{
			sprintf( szFiles, "%s\\%s", pDir, file.name );
			fileList.insert( szFiles );
		}
		while (_findnext( hFile, &file ) == 0 );
	}
	_findclose( hFile );

	// Remove each file...

	StringSet::iterator iter = fileList.begin();
	while( iter != fileList.end() )
	{
		remove( iter->c_str() );
		iter++;
	}

	return TRUE;
}

BOOL CWinUtil::RemoveDir( char const* pDir )
{
	if( !pDir ) return FALSE;

	// Make sure it exists and is empty...

	if( !DirExist( pDir )) return FALSE;
	if( !EmptyDir( pDir )) return FALSE;

	// Ok, delete it...

	_rmdir( pDir );

	return TRUE;
}

DWORD CWinUtil::WinGetPrivateProfileString (const char* lpAppName, const char* lpKeyName, const char* lpDefault, char* lpReturnedString, DWORD nSize, const char* lpFileName)
{
	return GetPrivateProfileString (lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName);
}

DWORD CWinUtil::WinWritePrivateProfileString (const char* lpAppName, const char* lpKeyName, const char* lpString, const char* lpFileName)
{
	return WritePrivateProfileString (lpAppName, lpKeyName, lpString, lpFileName);
}

void CWinUtil::DebugOut (char const* str)
{
	OutputDebugString (str);
}

void CWinUtil::DebugBreak()
{
	::DebugBreak();
}

float CWinUtil::GetTime()
{
	return (float)GetTickCount() / 1000.0f;
}

char* CWinUtil::GetFocusWindow()
{
	static char strText[128];

	HWND hWnd = GetFocus();
	if (!hWnd)
	{
		hWnd = GetForegroundWindow();
		if (!hWnd) return NULL;
	}

	GetWindowText (hWnd, strText, 127);
	return strText;
}

void CWinUtil::WriteToDebugFile (char const* strText)
{
	FILE* pFile = fopen ("c:\\shodebug.txt", "a+t");
	if (!pFile) return;

	time_t seconds;
	time (&seconds);
	struct tm* timedate = localtime (&seconds);
	if (!timedate) return;

	char strTimeDate[128];
	sprintf (strTimeDate, "[%02d/%02d/%02d %02d:%02d:%02d]  ", timedate->tm_mon + 1, timedate->tm_mday, (timedate->tm_year + 1900) % 100, timedate->tm_hour, timedate->tm_min, timedate->tm_sec);
	fwrite (strTimeDate, strlen(strTimeDate), 1, pFile);

	fwrite (strText, strlen(strText), 1, pFile);
	fwrite ("\n", 1, 1, pFile);

	fclose (pFile);
}

#endif // LT_MACOS
