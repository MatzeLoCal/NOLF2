// macOS stub for MSVC <tchar.h> — ANSI builds, so the _t* names map to the
// narrow C string functions. TCHAR/_T are provided by the windows.h shim.
#ifndef __LT_COMPAT_TCHAR_H__
#define __LT_COMPAT_TCHAR_H__
#include <string.h>
#include <stdio.h>
#ifndef _tcscpy
#define _tcscpy   strcpy
#define _tcsncpy  strncpy
#define _tcscat   strcat
#define _tcslen   strlen
#define _tcscmp   strcmp
#define _tcsicmp  strcasecmp
#define _tcschr   strchr
#define _tcsrchr  strrchr
#define _tcsstr   strstr
#define _stprintf sprintf
#define _tprintf  printf
#define _tcsftime strftime
#endif
#ifndef _TCHAR_DEFINED
#define _TCHAR_DEFINED
typedef char _TCHAR;
#endif
#endif
