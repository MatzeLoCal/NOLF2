// ----------------------------------------------------------------------- //
// macOS / POSIX compatibility shim for <windows.h>
//
// Provides the SUBSET of the Win32 API that the Jupiter EX engine core and
// NOLF2 game code actually use, mapped onto POSIX / pthreads / C stdlib.
// It is placed on the include path only for the macOS build, so the ~51 files
// that `#include <windows.h>` / `#include "windows.h"` pick it up unchanged.
//
// Philosophy: satisfy what is used; error loudly on what is not (so missing
// pieces surface at compile time rather than silently misbehaving). Renderer /
// DirectX / DirectShow files need the REAL Win32 and are gated off on macOS.
//
// CRITICAL: Win32 fixed-width types do NOT match macOS native widths.
//   DWORD/LONG/ULONG are 32-bit on Win32 but `unsigned long` is 64-bit on
//   macOS (LP64). They are defined here via <stdint.h> to preserve on-disk /
//   on-wire / in-struct layout.
// ----------------------------------------------------------------------- //
#ifndef __LT_COMPAT_WINDOWS_H__
#define __LT_COMPAT_WINDOWS_H__

// Canonical "windows.h has been included" sentinel. Lots of engine/MFC-stub
// code guards fallback POINT/RECT/etc. definitions with `#ifndef _WINDOWS_`.
#ifndef _WINDOWS_
#define _WINDOWS_
#endif

#if defined(_WIN32)
#error "compat/windows.h (macOS shim) must not be used on Windows"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>   // FormatMessageA walks the caller's va_list
#include <ctype.h>    // ... and reads each insert's printf conversion char
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>

// --- calling conventions / storage (no-ops off Windows) ------------------
#ifndef WINAPI
#define WINAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef _cdecl
#define _cdecl
#endif

// --- fixed-width / fundamental types (widths match Win32, not macOS) -----
typedef uint8_t        BYTE;
typedef uint16_t       WORD;
typedef uint32_t       DWORD;     // 32-bit on Win32 (NOT `unsigned long`)
typedef int32_t        LONG;      // 32-bit on Win32
typedef uint32_t       ULONG;     // 32-bit on Win32
typedef uint32_t       UINT;
typedef int            INT;
typedef int            BOOL;
typedef int64_t        LONGLONG;
typedef uint64_t       ULONGLONG;
typedef uint64_t       DWORDLONG;
typedef unsigned char  UCHAR;
typedef unsigned short USHORT;
typedef float          FLOAT;

typedef char           CHAR;
typedef char           TCHAR;
typedef void           VOID;
typedef void*          PVOID;
typedef void*          LPVOID;
typedef const void*    LPCVOID;
typedef char*          LPSTR;
typedef const char*    LPCSTR;
typedef char*          LPTSTR;
typedef const char*    LPCTSTR;
typedef BYTE*          LPBYTE;
typedef WORD*          LPWORD;
typedef DWORD*         LPDWORD;
typedef BOOL*          LPBOOL;
typedef LONG*          LPLONG;

// pointer-sized integers (correct on LP64)
typedef intptr_t       INT_PTR;
typedef uintptr_t      UINT_PTR;
typedef intptr_t       LONG_PTR;
typedef uintptr_t      ULONG_PTR;
typedef ULONG_PTR      DWORD_PTR;
typedef uintptr_t      WPARAM;
typedef intptr_t       LPARAM;
typedef intptr_t       LRESULT;
typedef uint16_t       ATOM;

// handles
typedef void*          HANDLE;
typedef void*          HMODULE;
typedef void*          HINSTANCE;
typedef void*          HWND;
typedef void*          HDC;
typedef void*          HBITMAP;
typedef void*          HICON;
typedef void*          HCURSOR;
typedef void*          HMENU;
typedef void*          HKEY;
typedef void*          HGLOBAL;
typedef void*          HLOCAL;
typedef void*          HFONT;
typedef HANDLE*        LPHANDLE;

// HRESULT is a 32-bit status code, not a handle.
typedef LONG           HRESULT;
#ifndef S_OK
#define S_OK    ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL  ((HRESULT)0x80004005L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG  ((HRESULT)0x80070057L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)
#endif

#ifndef _T
#define _T(x) x
#define __T(x) x
#define TEXT(x) x
#endif

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef _MAX_PATH
#define _MAX_PATH 260
#endif
// See msvc_crt_compat.h: _MAX_PATH sizes arrays inside structs shared between
// the engine and the game modules, so a per-TU disagreement is an ABI break.
#if _MAX_PATH != 260
#error "_MAX_PATH must be 260 in every TU -- shared struct layouts depend on it"
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFF
#endif
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif

// --- POINT / RECT / large integer ---------------------------------------
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagRECT  { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct tagSIZE  { LONG cx, cy; } SIZE, *LPSIZE, *PSIZE;

// CONST is a legacy alias for const used pervasively in DirectX headers.
#ifndef CONST
#define CONST const
#endif

// GDI palette / region types referenced by the D3D9 device interface.
typedef struct tagPALETTEENTRY { BYTE peRed, peGreen, peBlue, peFlags; } PALETTEENTRY, *LPPALETTEENTRY;
typedef struct _RGNDATAHEADER {
    DWORD dwSize, iType, nCount, nRgnSize;
    RECT  rcBound;
} RGNDATAHEADER;
typedef struct _RGNDATA { RGNDATAHEADER rdh; char Buffer[1]; } RGNDATA, *LPRGNDATA;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; } u;
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; } u;
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME, *LPFILETIME;

// --- multimedia (mmreg.h) wave format structures (exact Win32 layout) -----
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif
#pragma pack(push, 1)
typedef struct waveformat_tag {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
} WAVEFORMAT, *LPWAVEFORMAT;

typedef struct pcmwaveformat_tag {
    WAVEFORMAT wf;
    WORD       wBitsPerSample;
} PCMWAVEFORMAT, *LPPCMWAVEFORMAT;

typedef struct tWAVEFORMATEX {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
#pragma pack(pop)
typedef DWORD MMRESULT;

// --- memory helpers ------------------------------------------------------
#define ZeroMemory(p,n)      memset((p),0,(n))
#define FillMemory(p,n,v)    memset((p),(v),(n))
#define CopyMemory(d,s,n)    memcpy((d),(s),(n))
#define MoveMemory(d,s,n)    memmove((d),(s),(n))

// wsprintf family -> standard sprintf/snprintf (ANSI builds only)
#ifndef wsprintf
#define wsprintf  sprintf
#endif
#ifndef wsprintfA
#define wsprintfA sprintf
#endif

// --- critical sections (recursive, to match Win32 semantics) -------------
typedef pthread_mutex_t CRITICAL_SECTION;
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;

static inline void InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline void DeleteCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_destroy(cs); }
static inline void EnterCriticalSection(LPCRITICAL_SECTION cs)  { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(LPCRITICAL_SECTION cs)  { pthread_mutex_unlock(cs); }
static inline BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return pthread_mutex_trylock(cs) == 0; }

// --- interlocked atomics -------------------------------------------------
static inline LONG InterlockedIncrement(LONG volatile* p) { return __sync_add_and_fetch(p, 1); }
static inline LONG InterlockedDecrement(LONG volatile* p) { return __sync_sub_and_fetch(p, 1); }
static inline LONG InterlockedExchangeAdd(LONG volatile* p, LONG v) { return __sync_fetch_and_add(p, v); }
static inline LONG InterlockedExchange(LONG volatile* p, LONG v) { return __sync_lock_test_and_set(p, v); }

// --- misc kernel32 surface ----------------------------------------------
static inline void  Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }
// Win32 GetTickCount = milliseconds since BOOT, and callers rely on that scale,
// not just on differences: the engine divides tick values into floats and casts
// them to int32. Returning UNIX EPOCH milliseconds (~1.78e12, i.e. 2.3e9 once
// truncated to 32 bits) overflowed both — it is what pinned every weapon-fire
// timestamp at INT32_MAX (PHASE2_HANDOFF §16). CLOCK_MONOTONIC also cannot be
// stepped backwards by NTP. Kept byte-identical to sys/linux/timemgr.cpp's
// timeGetTime(), because both definitions of the name are in play (this one is
// `static inline`, that one has external linkage) and a TU binds to whichever
// header it saw first.
static inline DWORD GetTickCount(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}
static inline DWORD timeGetTime(void) { return GetTickCount(); }   // winmm
static inline DWORD GetLastError(void) { return 0; }
static inline void  SetLastError(DWORD) {}
static inline DWORD GetCurrentThreadId(void) { return (DWORD)(uintptr_t)pthread_self(); }
static inline HMODULE GetModuleHandle(LPCSTR) { return NULL; }
#define GetModuleHandleA GetModuleHandle
static inline DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n) { if (buf && n) buf[0]=0; return 0; }
#define GetModuleFileName GetModuleFileNameA

// module loading (the engine loads renderer/etc. as DLLs on Windows; on macOS the
// null renderer is linked in, so these return NULL — the loader path is bypassed).
typedef void* FARPROC;
static inline HMODULE LoadLibraryA(LPCSTR) { return NULL; }
#define LoadLibrary LoadLibraryA
static inline BOOL    FreeLibrary(HMODULE) { return TRUE; }
static inline FARPROC GetProcAddress(HMODULE, LPCSTR) { return NULL; }
static inline void    DebugBreak(void) { }

static inline void OutputDebugStringA(LPCSTR s) { if (s) fputs(s, stderr); }
#define OutputDebugString OutputDebugStringA

// MessageBox -> stderr (no GUI dependency in the shim)
#ifndef MB_OK
#define MB_OK 0
#endif
#ifndef MB_ABORTRETRYIGNORE
#define MB_ABORTRETRYIGNORE 0x2
#define MB_ICONERROR        0x10
#define IDABORT             3
#define IDRETRY             4
#define IDIGNORE            5
#endif
static inline int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : "");
    // Abort/Retry/Ignore prompts (AssertMgr): headless -> "Ignore" (continue).
    return (type & MB_ABORTRETRYIGNORE) ? IDIGNORE : 0;
}
#define MessageBox MessageBoxA

// window lookup/teardown used by AssertMgr's assert dialog plumbing
#ifndef SW_MAXIMIZE
#define SW_MAXIMIZE 3
#endif
static inline HWND FindWindowA(LPCSTR, LPCSTR) { return NULL; }
#define FindWindow FindWindowA
static inline BOOL DestroyWindow(HWND) { return 1; }

// --- additional handle / pointer types --------------------------------------
typedef void* HBRUSH;
typedef void* HPEN;
typedef void* HPALETTE;
typedef void* HRGN;
typedef void* HGDIOBJ;
typedef void* HKL;
typedef DWORD* PDWORD;
typedef DWORD_PTR* PDWORD_PTR;
typedef ULONG_PTR* PULONG_PTR;
typedef UINT* PUINT;

// --- lstr* string helpers (map to C stdlib) ---------------------------------
#ifndef lstrcpy
#define lstrcpy   strcpy
#define lstrcpyA  strcpy
#define lstrcpyn  strncpy
#define lstrcat   strcat
#define lstrlen   (int)strlen
#define lstrcmp   strcmp
#define lstrcmpi  strcasecmp
#endif

// --- GetSystemInfo / SYSTEM_INFO --------------------------------------------
typedef struct _SYSTEM_INFO {
    union { DWORD dwOemId; struct { WORD wProcessorArchitecture; WORD wReserved; }; };
    DWORD     dwPageSize;
    LPVOID    lpMinimumApplicationAddress;
    LPVOID    lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
static inline void GetSystemInfo(LPSYSTEM_INFO si) {
    if (si) { memset(si, 0, sizeof(*si)); si->dwPageSize = 4096; si->dwNumberOfProcessors = 1; }
}

// --- virtual-key codes (subset the engine references) -----------------------
#ifndef VK_ESCAPE
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_NUMLOCK 0x90
#endif

// --- GDI / user32 function stubs (null behavior; no windowing in Phase 0) ----
static inline BOOL    DeleteObject(HGDIOBJ) { return TRUE; }
static inline HGDIOBJ SelectObject(HDC, HGDIOBJ) { return NULL; }
static inline int     GetObjectA(HGDIOBJ, int, LPVOID) { return 0; }
#define GetObject GetObjectA
static inline HDC     CreateCompatibleDC(HDC) { return NULL; }
static inline BOOL    DeleteDC(HDC) { return TRUE; }
static inline HDC     GetDC(HWND) { return NULL; }
static inline int     ReleaseDC(HWND, HDC) { return 1; }
#ifndef WHITE_BRUSH
#define WHITE_BRUSH   0
#define LTGRAY_BRUSH  1
#define GRAY_BRUSH    2
#define DKGRAY_BRUSH  3
#define BLACK_BRUSH   4
#define NULL_BRUSH    5
#define HOLLOW_BRUSH  NULL_BRUSH
#define WHITE_PEN     6
#define BLACK_PEN     7
#define NULL_PEN      8
#define SYSTEM_FONT   13
#define DEFAULT_GUI_FONT 17
#endif
static inline HGDIOBJ GetStockObject(int) { return NULL; }
static inline HFONT   CreateFontIndirectA(const void*) { return NULL; }
#define CreateFontIndirect CreateFontIndirectA
typedef DWORD COLORREF;
static inline HBRUSH  CreateSolidBrush(COLORREF) { return NULL; }

static inline short GetAsyncKeyState(int) { return 0; }
static inline short GetKeyState(int) { return 0; }
static inline BOOL  GetCursorPos(LPPOINT p) { if (p) { p->x = 0; p->y = 0; } return TRUE; }
static inline BOOL  SetCursorPos(int, int) { return TRUE; }
static inline BOOL  ClipCursor(const RECT*) { return TRUE; }
/* Win32 semantics: TRUE increments / FALSE decrements a display counter and
   returns the NEW count (visible while count >= 0). Callers spin on the count
   crossing zero — e.g. lt_cursor_impl.cpp PreSetMode does
   `do { t = ShowCursor(FALSE); } while (t >= 0);` — so a constant return value
   hangs the game the first time it hides the cursor. Counter is per-TU (static
   inline), which is fine: every caller's loop converges just like on Win32. */
static inline int   ShowCursor(BOOL bShow) {
    static int s_nShowCursorCount = 0;
    s_nShowCursorCount += (bShow ? 1 : -1);
    return s_nShowCursorCount;
}
static inline HCURSOR SetCursor(HCURSOR) { return NULL; }
static inline HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return NULL; }
#define LoadCursor LoadCursorA

// font charset / family constants (GDI LOGFONT)
#ifndef DEFAULT_CHARSET
#define DEFAULT_CHARSET   1
#define SYMBOL_CHARSET    2
#define OEM_CHARSET       255
#define VARIABLE_PITCH    2
#define FIXED_PITCH       1
#define FF_SWISS          32
#define FF_MODERN         48
#define PROOF_QUALITY     2
#define NONANTIALIASED_QUALITY 3
#endif

// LoadImage + flags
#ifndef IMAGE_BITMAP
#define IMAGE_BITMAP 0
#define IMAGE_ICON   1
#define IMAGE_CURSOR 2
#define LR_LOADFROMFILE     0x0010
#define LR_DEFAULTSIZE      0x0040
#define LR_CREATEDIBSECTION 0x2000
#endif
static inline HANDLE LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return NULL; }
#define LoadImage LoadImageA

// MAKEINTRESOURCE / resource helpers
#ifndef MAKEINTRESOURCEA
#define MAKEINTRESOURCEA(i) ((LPSTR)((uintptr_t)((WORD)(i))))
#define MAKEINTRESOURCE     MAKEINTRESOURCEA
#endif
typedef void* HRSRC;   // (also typedef'd in the GDI section; identical redef is OK)
static inline HRSRC  FindResourceA(HMODULE, LPCSTR, LPCSTR) { return NULL; }
#define FindResource FindResourceA
static inline HGLOBAL LoadResource(HMODULE, HRSRC) { return NULL; }
static inline LPVOID  LockResource(HGLOBAL) { return NULL; }
static inline DWORD   SizeofResource(HMODULE, HRSRC) { return 0; }

// --- window messages / show / misc constants -------------------------------
#ifndef WM_NULL
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_ACTIVATEAPP 0x001C
#define WM_FONTCHANGE 0x001D
#define WM_SETCURSOR 0x0020
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_COMMAND 0x0111
#define WM_USER 0x0400
#endif
#ifndef SW_HIDE
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#endif
#ifndef SMTO_NORMAL
#define SMTO_NORMAL 0
#define SMTO_BLOCK 1
#define SMTO_ABORTIFHUNG 2
#endif
#ifndef HWND_BROADCAST
#define HWND_BROADCAST ((HWND)(uintptr_t)0xffff)
#endif
#ifndef RT_RCDATA
#define RT_RCDATA ((LPCSTR)10)
#define RT_BITMAP ((LPCSTR)2)
#endif
#ifndef SUBLANG_DEFAULT
#define SUBLANG_DEFAULT 0x01
#define LANG_NEUTRAL 0x00
#endif
#ifndef RGB
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r))|(((WORD)((BYTE)(g)))<<8)|(((DWORD)(BYTE)(b))<<16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)(((WORD)(c))>>8))
#define GetBValue(c) ((BYTE)((c)>>16))
#endif

typedef BYTE* PBYTE;
typedef WORD* PWORD;

// --- GDI structs (bitmap / paint / font) ------------------------------------
#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD bfType; DWORD bfSize; WORD bfReserved1; WORD bfReserved2; DWORD bfOffBits;
} BITMAPFILEHEADER, *LPBITMAPFILEHEADER;
#pragma pack(pop)
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize; LONG biWidth; LONG biHeight; WORD biPlanes; WORD biBitCount;
    DWORD biCompression; DWORD biSizeImage; LONG biXPelsPerMeter; LONG biYPelsPerMeter;
    DWORD biClrUsed; DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;
typedef struct tagRGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct tagBITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO, *LPBITMAPINFO, *PBITMAPINFO;
typedef struct tagPAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore; BOOL fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT, *LPPAINTSTRUCT;
typedef void* HRSRC;

#ifndef LF_FACESIZE
#define LF_FACESIZE 32
#endif
typedef struct tagLOGFONTA {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision,
         lfClipPrecision, lfQuality, lfPitchAndFamily;
    CHAR lfFaceName[LF_FACESIZE];
} LOGFONTA, LOGFONT, *PLOGFONTA, *LPLOGFONTA, *LPLOGFONT;
#ifndef FW_NORMAL
#define FW_NORMAL 400
#define FW_BOLD 700
#define ANSI_CHARSET 0
#define OUT_TT_PRECIS 4
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define DEFAULT_QUALITY 0
#define DEFAULT_PITCH 0
#define FF_DONTCARE 0
#endif

// --- GDI / user32 / kernel32 stubs ------------------------------------------
static inline BOOL TextOutA(HDC,int,int,LPCSTR,int){return TRUE;}
#define TextOut TextOutA
static inline BOOL GetTextExtentPoint32A(HDC,LPCSTR,int,LPSIZE sz){if(sz){sz->cx=0;sz->cy=0;}return TRUE;}
#define GetTextExtentPoint32 GetTextExtentPoint32A
static inline int  GetTextCharacterExtra(HDC){return 0;}
static inline HFONT CreateFontA(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCSTR){return NULL;}
#define CreateFont CreateFontA
#ifndef OPAQUE
#define OPAQUE      2
#define TRANSPARENT 1
#endif
#ifndef LR_DEFAULTCOLOR
#define LR_DEFAULTCOLOR 0x0000
#endif
#ifndef WM_SYSCOMMAND
#define WM_SYSCOMMAND 0x0112
#define WM_NCACTIVATE 0x0086
#define SC_SCREENSAVE 0xF140
#define SC_KEYMENU    0xF100
#define SC_MONITORPOWER 0xF170
#endif
static inline BOOL DeleteFileA(LPCSTR p){ return p ? (remove(p)==0) : FALSE; }
#define DeleteFile DeleteFileA
static inline BOOL Rectangle(HDC,int,int,int,int){return TRUE;}
static inline int  SetTextCharacterExtra(HDC,int){return 0;}
static inline COLORREF SetTextColor(HDC,COLORREF){return 0;}
static inline COLORREF SetBkColor(HDC,COLORREF){return 0;}
static inline int  SetBkMode(HDC,int){return 0;}
static inline int  AddFontResourceA(LPCSTR){return 1;}
#define AddFontResource AddFontResourceA
static inline BOOL RemoveFontResourceA(LPCSTR){return TRUE;}
#define RemoveFontResource RemoveFontResourceA
static inline HFONT CreateFontIndirectA_real(const LOGFONTA*){return NULL;}
static inline HDC BeginPaint(HWND,LPPAINTSTRUCT){return NULL;}
static inline BOOL EndPaint(HWND,const PAINTSTRUCT*){return TRUE;}
static inline HWND SetFocus(HWND){return NULL;}
static inline BOOL ShowWindow(HWND,int){return TRUE;}
static inline BOOL UpdateWindow(HWND){return TRUE;}
static inline BOOL InvalidateRect(HWND,const RECT*,BOOL){return TRUE;}
static inline LRESULT SendMessageTimeoutA(HWND,UINT,WPARAM,LPARAM,UINT,UINT,PDWORD_PTR){return 0;}
#define SendMessageTimeout SendMessageTimeoutA
static inline int  ToAscii(UINT,UINT,const BYTE*,WORD*,UINT){return 0;}
static inline BOOL GetKeyboardState(PBYTE){return TRUE;}
static inline DWORD GetTempPathA(DWORD n,LPSTR buf){const char* t="/tmp/"; if(buf&&n)strncpy(buf,t,n); return 5;}
#define GetTempPath GetTempPathA

// --- window styles / SetWindowPos flags / DIB blit (null-DIB renderer) -------
#ifndef WS_OVERLAPPEDWINDOW
#define WS_OVERLAPPED       0x00000000
#define WS_POPUP            0x80000000
#define WS_CHILD            0x40000000
#define WS_VISIBLE          0x10000000
#define WS_CAPTION          0x00C00000
#define WS_SYSMENU          0x00080000
#define WS_OVERLAPPEDWINDOW 0x00CF0000
#endif
#ifndef SWP_NOSIZE
#define SWP_NOSIZE        0x0001
#define SWP_NOMOVE        0x0002
#define SWP_NOZORDER      0x0004
#define SWP_NOACTIVATE    0x0010
#define SWP_SHOWWINDOW    0x0040
#define SWP_NOREPOSITION  0x0200
#endif
#ifndef BI_RGB
#define BI_RGB        0
#define BI_RLE8       1
#define BI_BITFIELDS  3
#endif
#ifndef DIB_RGB_COLORS
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#endif
#ifndef SRCCOPY
#define SRCCOPY (DWORD)0x00CC0020
#endif
static inline HWND GetDesktopWindow(void) { return NULL; }
static inline BOOL SetWindowPos(HWND,HWND,int,int,int,int,UINT) { return TRUE; }
static inline BOOL AdjustWindowRect(LPRECT,DWORD,BOOL) { return TRUE; }
static inline BOOL MoveWindow(HWND,int,int,int,int,BOOL) { return TRUE; }
static inline BOOL GetClientRect(HWND,LPRECT r) { if(r){r->left=r->top=0;r->right=640;r->bottom=480;} return TRUE; }
static inline BOOL GetWindowRect(HWND,LPRECT r) { return GetClientRect(NULL,r); }
static inline BOOL BitBlt(HDC,int,int,int,int,HDC,int,int,DWORD) { return TRUE; }
static inline int  StretchDIBits(HDC,int,int,int,int,int,int,int,int,const void*,const BITMAPINFO*,UINT,DWORD) { return 0; }
static inline HBITMAP CreateDIBSection(HDC,const BITMAPINFO*,UINT,void**ppv,HANDLE,DWORD) { if(ppv)*ppv=0; return NULL; }
static inline UINT GetTempFileNameA(LPCSTR,LPCSTR pre,UINT,LPSTR buf){ if(buf){strcpy(buf,"/tmp/lt_"); if(pre)strncat(buf,pre,3); strcat(buf,"tmp");} return 1; }
#define GetTempFileName GetTempFileNameA

// --- Win32 events + threads on pthreads (game-code UI uses these directly:
//     e.g. LoadingScreen's background progress thread) ------------------------
#ifndef WAIT_OBJECT_0
#define WAIT_OBJECT_0  0x00000000
#define WAIT_TIMEOUT   0x00000102
#define WAIT_FAILED    0xFFFFFFFF
#endif

// A manual/auto-reset event backed by a pthread condition variable.
// Win32 lets one HANDLE space hold events, threads, mutexes... and
// WaitForSingleObject works on any of them (a THREAD handle becomes signaled
// when the thread exits). We can't recover the type from a bare void*, so every
// compat handle starts with a type tag and WaitForSingleObject dispatches on it.
// (Without this, waiting on a thread handle reinterpreted the pthread_t as a
// mutex/cond and blocked forever — which hung the level load in
// CLoadingScreen::Pause -> WaitForSingleObject(m_hThreadHandle, INFINITE).)
enum LTCompatHandleType { LTCOMPAT_HANDLE_EVENT = 0x4C544556, LTCOMPAT_HANDLE_THREAD = 0x4C545448 };

struct LTCompatHandleBase
{
    int m_nType;
};

struct LTCompatEvent : public LTCompatHandleBase
{
    pthread_mutex_t m_Mutex;
    pthread_cond_t  m_Cond;
    bool            m_bManualReset;
    bool            m_bSignaled;
};

// CreateEvent(lpAttr, bManualReset, bInitialState, lpName)
static inline HANDLE CreateEventA(void*, BOOL bManualReset, BOOL bInitial, LPCSTR)
{
    LTCompatEvent *pEvent = new LTCompatEvent;
    pEvent->m_nType = LTCOMPAT_HANDLE_EVENT;
    pthread_mutex_init(&pEvent->m_Mutex, NULL);
    pthread_cond_init(&pEvent->m_Cond, NULL);
    pEvent->m_bManualReset = bManualReset != 0;
    pEvent->m_bSignaled    = bInitial != 0;
    return (HANDLE)pEvent;
}
#define CreateEvent CreateEventA

static inline BOOL SetEvent(HANDLE h)
{
    LTCompatEvent *pEvent = (LTCompatEvent*)h;
    if (!pEvent) return FALSE;
    pthread_mutex_lock(&pEvent->m_Mutex);
    pEvent->m_bSignaled = true;
    pthread_cond_broadcast(&pEvent->m_Cond);
    pthread_mutex_unlock(&pEvent->m_Mutex);
    return TRUE;
}

static inline BOOL ResetEvent(HANDLE h)
{
    LTCompatEvent *pEvent = (LTCompatEvent*)h;
    if (!pEvent) return FALSE;
    pthread_mutex_lock(&pEvent->m_Mutex);
    pEvent->m_bSignaled = false;
    pthread_mutex_unlock(&pEvent->m_Mutex);
    return TRUE;
}

// A thread handle carries its pthread. Waiting on it == joining it (Win32
// semantics: signaled once the thread has exited).
struct LTCompatThread : public LTCompatHandleBase
{
    pthread_t m_Thread;
    bool      m_bJoined;
};

static inline DWORD WaitForSingleObject(HANDLE h, DWORD dwMillis)
{
    LTCompatHandleBase *pBase = (LTCompatHandleBase*)h;
    if (!pBase) return WAIT_FAILED;

    // Thread handle: wait for the thread to exit, i.e. join it.
    if (pBase->m_nType == LTCOMPAT_HANDLE_THREAD)
    {
        LTCompatThread *pThread = (LTCompatThread*)pBase;
        if (!pThread->m_bJoined)
        {
            pthread_join(pThread->m_Thread, NULL);
            pThread->m_bJoined = true;
        }
        return WAIT_OBJECT_0;   // an exited thread stays signaled
    }

    LTCompatEvent *pEvent = (LTCompatEvent*)h;

    pthread_mutex_lock(&pEvent->m_Mutex);
    DWORD nResult = WAIT_OBJECT_0;
    if (!pEvent->m_bSignaled)
    {
        if (dwMillis == 0)
            nResult = WAIT_TIMEOUT;
        else if (dwMillis == INFINITE)
        {
            while (!pEvent->m_bSignaled)
                pthread_cond_wait(&pEvent->m_Cond, &pEvent->m_Mutex);
        }
        else
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += dwMillis / 1000;
            ts.tv_nsec += (long)(dwMillis % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while (!pEvent->m_bSignaled)
                if (pthread_cond_timedwait(&pEvent->m_Cond, &pEvent->m_Mutex, &ts) != 0)
                    { nResult = WAIT_TIMEOUT; break; }
        }
    }
    if (nResult == WAIT_OBJECT_0 && !pEvent->m_bManualReset)
        pEvent->m_bSignaled = false;   // auto-reset
    pthread_mutex_unlock(&pEvent->m_Mutex);
    return nResult;
}

// Win32 LPTHREAD_START_ROUTINE returns DWORD == `unsigned long` on Windows;
// the game declares its thread procs `unsigned long WINAPI`, so match that
// exactly (NOT uint32_t — differs from unsigned long on LP64).
typedef unsigned long (*LTCompatThreadProc)(void*);
struct LTCompatThreadStart { LTCompatThreadProc m_pProc; void *m_pParam; };
static inline void *LTCompatThreadEntry(void *pArg)
{
    LTCompatThreadStart cStart = *(LTCompatThreadStart*)pArg;
    delete (LTCompatThreadStart*)pArg;
    cStart.m_pProc(cStart.m_pParam);
    return NULL;
}

// CreateThread(lpAttr, stack, lpStartAddr, lpParam, flags, lpThreadId).
// lpThreadId is Win32 LPDWORD == `unsigned long*` (the game casts to it); use
// that exact type so the call matches on LP64.
static inline HANDLE CreateThread(void*, size_t, LTCompatThreadProc pProc,
                                  void *pParam, DWORD, unsigned long *pThreadId)
{
    LTCompatThreadStart *pStart = new LTCompatThreadStart;
    pStart->m_pProc = pProc;
    pStart->m_pParam = pParam;
    LTCompatThread *pThread = new LTCompatThread;
    pThread->m_nType = LTCOMPAT_HANDLE_THREAD;
    pThread->m_bJoined = false;
    if (pthread_create(&pThread->m_Thread, NULL, LTCompatThreadEntry, pStart) != 0)
    {
        delete pStart; delete pThread;
        return NULL;
    }
    if (pThreadId) *pThreadId = 0;
    return (HANDLE)pThread;
}

// GUI messaging: no windowing in the Cocoa client — list boxes etc. are no-ops
// (multiplayer join UI won't populate, but single-player is unaffected).
#ifndef LB_ADDSTRING
#define LB_ADDSTRING    0x0180
#define LB_DELETESTRING 0x0182
#define LB_RESETCONTENT 0x0184
#define LB_GETCURSEL    0x0188
#define LB_GETTEXT      0x0189
#define LB_ERR          (-1)
#endif
#ifndef WM_SETREDRAW
#define WM_SETREDRAW    0x000B
#endif
#ifndef WM_MOUSEMOVE
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#endif
#ifndef GWL_STYLE
#define GWL_STYLE       (-16)
#define GWL_EXSTYLE     (-20)
#endif
#ifndef HWND_TOPMOST
#define HWND_TOPMOST    ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST  ((HWND)(intptr_t)-2)
#endif
#ifndef IDCANCEL
#define IDCANCEL 2
#endif
#ifndef MB_OKCANCEL
#define MB_OKCANCEL 0x00000001
#endif

static inline LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM) { return 0; }
#define SendMessage SendMessageA
static inline int  GetWindowTextA(HWND, LPSTR buf, int n) { if(buf&&n)buf[0]=0; return 0; }
#define GetWindowText GetWindowTextA
static inline BOOL SetWindowTextA(HWND, LPCSTR) { return TRUE; }
#define SetWindowText SetWindowTextA
static inline LRESULT CallWindowProcA(void*, HWND, UINT, WPARAM, LPARAM) { return 0; }
#define CallWindowProc CallWindowProcA

// Window proc type + Get/SetWindowLong(Ptr): the game hooks the main window's
// proc on Win32; on macOS these are no-ops (no HWND message pump).
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
#ifndef GWLP_WNDPROC
#define GWLP_WNDPROC (-4)
#define GWL_WNDPROC  (-4)
#endif
#ifndef WM_RBUTTONDOWN
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205
#define WM_RBUTTONDBLCLK 0x0206
#endif
#ifndef SWP_FRAMECHANGED
#define SWP_FRAMECHANGED 0x0020
#endif
static inline LONG_PTR GetWindowLongPtr(HWND, int) { return 0; }
static inline LONG_PTR SetWindowLongPtr(HWND, int, LONG_PTR) { return 0; }
static inline LONG     GetWindowLongA(HWND, int) { return 0; }
static inline LONG     SetWindowLongA(HWND, int, LONG) { return 0; }
#define GetWindowLong GetWindowLongA
#define SetWindowLong SetWindowLongA

// GUID + Win32 process-launch structs (the game references these in a
// process-spawn path that the macOS build never takes).
#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; unsigned char Data4[8]; } GUID;
#endif
#ifndef _STARTUPINFO_DEFINED
#define _STARTUPINFO_DEFINED
typedef struct _STARTUPINFOA {
    DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    unsigned short wShowWindow, cbReserved2; unsigned char *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, STARTUPINFO, *LPSTARTUPINFOA, *LPSTARTUPINFO;
typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#endif
// External-process launch (SierraUp.exe / NOLF2Srv.exe — Win32 multiplayer
// helpers that don't exist on macOS). Fail cleanly; single-player unaffected.
static inline BOOL CreateProcessA(LPCSTR, LPSTR, void*, void*, BOOL, DWORD, void*, LPCSTR,
                                  LPSTARTUPINFOA, LPPROCESS_INFORMATION) { return FALSE; }
#define CreateProcess CreateProcessA
// COM GUID generation (ProfileMgr unique-id). Fill a best-effort GUID.
typedef long HRESULT_LT_COMPAT;   // (HRESULT already typedef'd elsewhere)
static inline long CoCreateGuid(GUID *pGuid)
{
    if (pGuid)
    {
        pGuid->Data1 = (uint32_t)time(0);
        pGuid->Data2 = (uint16_t)clock();
        pGuid->Data3 = 0x4000;
        for (int i = 0; i < 8; ++i) pGuid->Data4[i] = (unsigned char)(rand() & 0xFF);
    }
    return 0;   // S_OK
}

// String-resource + message formatting.
//
// ⚠️ CORRECTION (2026-08-04): the old comment here claimed FormatMessage was
// "used for a few error strings" and that not expanding %1 inserts was
// "acceptable for these paths". BOTH were wrong. FormatString /
// FormatTempString (ClientUtilities.cpp:371, 412) run EVERY inserted UI string
// through it, and CRES.DLL contains 305 uses of `%1!d!` and 40 of `%1!s!` —
// mission rewards, skill points, ammo counts. With a plain copy-through the
// player sees the literal text "You received %1!d! skill points".
#ifndef FORMAT_MESSAGE_FROM_STRING
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_ARGUMENT_ARRAY  0x00002000
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#endif
// LoadString: the HINSTANCE the game passes here comes from the engine hook
// "cres_hinstance", which on macOS is an LTMacResModule* (a parsed cres.dll —
// see runtime/kernel/src/sys/macos/macos_cres.*). Dispatch through the struct's
// function pointer: this is a static inline compiled into libCShell.dylib, and
// going through DATA keeps that dylib free of cross-module link symbols.
// A NULL/foreign handle degrades to the old "no strings" behaviour.
#include "ltmacresmodule.h"
static inline int LoadStringA(HINSTANCE hInst, unsigned int nID, LPSTR buf, int n)
{
    if (buf && n) buf[0] = 0;
    LTMacResModule *pModule = (LTMacResModule*)hInst;
    if (!pModule || pModule->m_nMagic != LTMACRESMODULE_MAGIC || !pModule->m_pfnLoadString)
        return 0;
    return pModule->m_pfnLoadString(pModule, nID, buf, n);
}
#define LoadString LoadStringA
// Win32 FormatMessage's insert syntax, as actually used by this game's strings:
//   %N        insert argument N, default format !s!
//   %N!spec!  insert argument N with the printf spec between the '!'s (d, s, ...)
//   %%        a literal '%'
//   %n %r %t  hard line break / carriage return / tab
//   %0        end the message here
//
// ⚠️ ARGUMENTS ARE READ SEQUENTIALLY. A va_list cannot be indexed portably (on
// arm64 it is an opaque struct), so arguments 1..max are pulled in INDEX ORDER
// and cached before substitution. That is correct whenever the caller passes
// them in order — which every caller here does — and it is why a string may
// safely mention %2 before %1.
// ⚠️ EACH ARGUMENT'S TYPE COMES FROM ITS SPEC: 's' reads a char*, 'f'/'e'/'g' a
// double, everything else an int. Reading them all as one type would walk the
// va_list by the wrong stride and corrupt every later insert.
static inline DWORD FormatMessageA(DWORD dwFlags, const void *pSource, DWORD, DWORD,
                                   LPSTR pBuffer, DWORD nSize, void *pArgs)
{
    if (!pBuffer || !nSize) return 0;
    pBuffer[0] = 0;
    const char *pSrc = (const char*)pSource;
    if (!pSrc) return 0;

    const bool bInserts = pArgs != 0 && (dwFlags & FORMAT_MESSAGE_IGNORE_INSERTS) == 0;
    if (!bInserts)
    {
        strncpy(pBuffer, pSrc, nSize - 1);
        pBuffer[nSize - 1] = 0;
        return (DWORD)strlen(pBuffer);
    }

    // --- pass 1: which arguments are used, and with what spec ---
    enum { kMaxArgs = 10, kSpecLen = 16 };
    char aSpec[kMaxArgs][kSpecLen];
    int  nMaxArg = 0;
    for (int i = 0; i < kMaxArgs; ++i) aSpec[i][0] = 0;

    for (const char *p = pSrc; *p; )
    {
        if (*p != '%') { ++p; continue; }
        ++p;
        if (*p < '1' || *p > '9') { if (*p) ++p; continue; }
        int nIdx = *p++ - '0';
        if (nIdx > nMaxArg) nMaxArg = nIdx;
        if (*p == '!')
        {
            const char *pEnd = strchr(p + 1, '!');
            if (pEnd)
            {
                size_t nLen = (size_t)(pEnd - (p + 1));
                if (nLen >= kSpecLen - 2) nLen = kSpecLen - 3;
                if (!aSpec[nIdx][0])
                {
                    aSpec[nIdx][0] = '%';
                    memcpy(aSpec[nIdx] + 1, p + 1, nLen);
                    aSpec[nIdx][nLen + 1] = 0;
                }
                p = pEnd + 1;
            }
        }
        else if (!aSpec[nIdx][0])
        {
            strcpy(aSpec[nIdx], "%s");   // bare %N defaults to a string
        }
    }

    // --- pass 2: render each argument once, in index order ---
    char aText[kMaxArgs][256];
    for (int i = 0; i < kMaxArgs; ++i) aText[i][0] = 0;
    {
        va_list *pVa = (va_list*)pArgs;
        for (int i = 1; i <= nMaxArg && i < kMaxArgs; ++i)
        {
            const char *pSpec = aSpec[i][0] ? aSpec[i] : "%s";
            char c = 0;
            for (const char *q = pSpec; *q; ++q)
                if (isalpha((unsigned char)*q)) c = *q;   // conversion = last alpha

            if (c == 's')
            {
                const char *pStr = va_arg(*pVa, const char*);
                snprintf(aText[i], sizeof(aText[i]), pSpec, pStr ? pStr : "");
            }
            else if (c == 'f' || c == 'e' || c == 'E' || c == 'g' || c == 'G')
            {
                double f = va_arg(*pVa, double);
                snprintf(aText[i], sizeof(aText[i]), pSpec, f);
            }
            else if (c == 'p')
            {
                void *pv = va_arg(*pVa, void*);
                snprintf(aText[i], sizeof(aText[i]), pSpec, pv);
            }
            else
            {
                int n = va_arg(*pVa, int);
                snprintf(aText[i], sizeof(aText[i]), pSpec, n);
            }
        }
    }

    // --- pass 3: emit ---
    DWORD nOut = 0;
    const DWORD nCap = nSize - 1;
    for (const char *p = pSrc; *p && nOut < nCap; )
    {
        if (*p != '%') { pBuffer[nOut++] = *p++; continue; }
        ++p;
        if (*p >= '1' && *p <= '9')
        {
            int nIdx = *p++ - '0';
            if (*p == '!') { const char *pEnd = strchr(p + 1, '!'); if (pEnd) p = pEnd + 1; }
            const char *pIns = (nIdx < kMaxArgs) ? aText[nIdx] : "";
            while (*pIns && nOut < nCap) pBuffer[nOut++] = *pIns++;
            continue;
        }
        switch (*p)
        {
            case '%': pBuffer[nOut++] = '%';  ++p; break;
            case 'n': pBuffer[nOut++] = '\n'; ++p; break;
            case 'r': pBuffer[nOut++] = '\r'; ++p; break;
            case 't': pBuffer[nOut++] = '\t'; ++p; break;
            case '0': p = "";                      break;   // %0 ends the message
            case 0:                                break;
            default:  pBuffer[nOut++] = *p++;      break;   // %. and %! are literal
        }
    }
    pBuffer[nOut] = 0;
    return nOut;
}
#define FormatMessage FormatMessageA

#endif // __LT_COMPAT_WINDOWS_H__
