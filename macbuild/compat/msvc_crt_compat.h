// ----------------------------------------------------------------------- //
// MSVC C-runtime compatibility shim (POSIX / macOS).
//
// Force-included into every translation unit by the macOS build (see
// macbuild/CMakeLists.txt). Provides the MSVC-specific CRT helpers the engine
// uses but which are absent or differently-named on POSIX. C and C++ safe.
// ----------------------------------------------------------------------- //
#ifndef __LT_MSVC_CRT_COMPAT_H__
#define __LT_MSVC_CRT_COMPAT_H__

#if !defined(_WIN32)

#include <string.h>
#include <strings.h>   // strcasecmp / strncasecmp
#include <ctype.h>
#include <wctype.h>    // iswspace/iswalpha/... (wide ctype; codepoint-based, ok with -fshort-wchar)
#include <stddef.h>
#ifdef __cplusplus
#include <new>         // placement new (MSVC pulled it in implicitly; clang needs it)
#endif

// ----------------------------------------------------------------------- //
// ⚠️ _MAX_PATH IS PART OF THE SHARED ABI -- ONE VALUE, EVERY TRANSLATION UNIT.
//
// It sizes character arrays INSIDE structs that the engine and the game modules
// both touch: PlaySoundInfo::m_szSoundName, NetHost::m_szProvider, ... If two
// TUs disagree, every member after the array sits at a different offset and the
// two sides silently read and write different fields of the "same" struct.
//
// The tree used to define it twice with different values -- 260 in
// compat/windows.h (MSVC's value) and 256 in sdk/inc/ltbasedefs.h's __LINUX
// branch -- so a TU got whichever header it reached first. The engine wrote
// PlaySoundInfo::m_hSound 8 bytes away from where the game read it, and
// SoundFX::PlaySound got the float pair (0.0f, m_fPitchShift == 1.0f) back as
// its HLTSOUND: the wild pointer 0x3f80000000000000 that killed the first
// mission. Defining it HERE -- this header is force-included into every TU
// before anything else -- makes the value unconditional; both former definition
// sites now carry an #error guard against re-divergence.
#define _MAX_PATH 260
#define MAX_PATH  260

// printf family (MSVC underscore names)
#ifndef _snprintf
#define _snprintf  snprintf
#endif
#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif

// case-insensitive compare
#ifndef stricmp
#define stricmp   strcasecmp
#endif
#ifndef strnicmp
#define strnicmp  strncasecmp
#endif
#ifndef _stricmp
#define _stricmp  strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif

// in-place case / reverse helpers (MSVC _str* family)
static inline char* lt_strupr_(char* s) {
    if (s) for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}
static inline char* lt_strlwr_(char* s) {
    if (s) for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}
static inline char* lt_strrev_(char* s) {
    if (s) {
        size_t n = strlen(s);
        for (size_t i = 0; i < n / 2; ++i) { char t = s[i]; s[i] = s[n-1-i]; s[n-1-i] = t; }
    }
    return s;
}
// width-agnostic wide-string helpers. The engine uses wchar_t wide strings and
// we build with -fshort-wchar (16-bit, matching Windows/assets); the system
// wcs* assume 32-bit wchar_t and would misread our strings, so use these.
#include <stddef.h>
static inline size_t   lt_wcslen(const wchar_t* s){ const wchar_t* p=s; while(p&&*p)++p; return s?(size_t)(p-s):0; }
static inline wchar_t* lt_wcscpy(wchar_t* d, const wchar_t* s){ wchar_t* r=d; while((*d++=*s++)){} return r; }
static inline wchar_t* lt_wcsncpy(wchar_t* d, const wchar_t* s, size_t n){ wchar_t* r=d; while(n&&(*d++=*s++))--n; while(n--)*d++=0; return r; }
static inline wchar_t* lt_wcsncat(wchar_t* d, const wchar_t* s, size_t n){ wchar_t* r=d; while(*d)++d; while(n&&(*d=*s)){++d;++s;--n;} *d=0; return r; }
static inline int      lt_wcscmp(const wchar_t* a, const wchar_t* b){ while(*a&&*a==*b){++a;++b;} return (int)(*a-*b); }

#ifndef _strupr
#define _strupr lt_strupr_
#endif
#ifndef _strlwr
#define _strlwr lt_strlwr_
#endif
#ifndef _strrev
#define _strrev lt_strrev_
#endif
// NOTE: the no-underscore strupr/strlwr are intentionally NOT defined here.
// sdk/inc/ltbasedefs.h already provides strupr() as a function on the POSIX
// (__LINUX) path; defining a macro for it would rewrite that definition.

// --- game-DLL (NOLF2/) additions ---------------------------------------
#include <strings.h>   // strcasecmp (for the strcmpi spellings below)
#include <stdio.h>     // sprintf (for lt_ltoa_)
// MSVC crtdbg assert macros, used bare throughout the game code (crtdbg.h is
// not included there). No-ops, matching compat/crtdbg.h.
#ifndef _ASSERT
#define _ASSERT(expr)  ((void)0)
#endif
#ifndef _ASSERTE
#define _ASSERTE(expr) ((void)0)
#endif

// case-insensitive compare spellings
#ifndef strcmpi
#define strcmpi  strcasecmp
#endif
#ifndef _strcmpi
#define _strcmpi strcasecmp
#endif

// MSVC path-component limits (stdlib.h on Windows)
#ifndef _MAX_DRIVE
#define _MAX_DRIVE 3
#endif
#ifndef _MAX_DIR
#define _MAX_DIR   256
#endif
#ifndef _MAX_FNAME
#define _MAX_FNAME 256
#endif
#ifndef _MAX_EXT
#define _MAX_EXT   256
#endif

// _ltoa(value, buf, radix) -- game only uses radix 10
static inline char* lt_ltoa_(long v, char* buf, int radix) {
    if (radix == 16) { sprintf(buf, "%lx", v); } else { sprintf(buf, "%ld", v); }
    return buf;
}
#ifndef _ltoa
#define _ltoa lt_ltoa_
#endif

#ifndef _UI8_MAX
#define _UI8_MAX 0xffu
#endif

// _splitpath(path, drive, dir, fname, ext) -- MSVC path splitter. No drive
// letters on macOS (drive is always ""); dir keeps its trailing slash, ext
// keeps its leading dot, matching MSVC semantics.
#include <string.h>
static inline void lt_splitpath_(const char* path, char* drive, char* dir,
                                 char* fname, char* ext) {
    if (drive) drive[0] = 0;
    if (!path) { if (dir) dir[0]=0; if (fname) fname[0]=0; if (ext) ext[0]=0; return; }
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    const char* base = slash ? slash + 1 : path;
    if (dir) { size_t n = (size_t)(base - path); memcpy(dir, path, n); dir[n] = 0; }
    const char* dot = strrchr(base, '.');
    if (fname) { size_t n = dot ? (size_t)(dot - base) : strlen(base); memcpy(fname, base, n); fname[n] = 0; }
    if (ext) { if (dot) strcpy(ext, dot); else ext[0] = 0; }
}
#ifndef _splitpath
#define _splitpath lt_splitpath_
#endif

// MSVC crt debug report hook (AssertMgr): accept and ignore.
typedef int (*_CRT_REPORT_HOOK)(int, char*, int*);
static inline _CRT_REPORT_HOOK _CrtSetReportHook(_CRT_REPORT_HOOK) { return 0; }

// _ftime / _timeb (millisecond wall clock — GameClientShell frame timing).
#include <sys/time.h>
#ifndef _LT_TIMEB_DEFINED
#define _LT_TIMEB_DEFINED
struct _timeb
{
    long           time;      // seconds since epoch
    unsigned short millitm;   // milliseconds
    short          timezone;
    short          dstflag;
};
static inline void _ftime(struct _timeb *tb)
{
    if (!tb) return;
    struct timeval tv;
    gettimeofday(&tv, 0);
    tb->time     = (long)tv.tv_sec;
    tb->millitm  = (unsigned short)(tv.tv_usec / 1000);
    tb->timezone = 0;
    tb->dstflag  = 0;
}
#endif

#endif // !_WIN32
#endif // __LT_MSVC_CRT_COMPAT_H__
