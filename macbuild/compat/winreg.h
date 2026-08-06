// Minimal stub of <winreg.h> (Windows registry) for the macOS build.
//
// The registry is Windows-only; RegMgr (libs/RegMgr) uses it to persist game /
// profile config. For the macOS bring-up these are no-ops: key "creation"
// succeeds with a sentinel handle, reads report "not found" so callers fall back
// to their defaults, and writes are discarded. A real POSIX/plist-backed config
// store can replace this later without touching RegMgr's callers.
#ifndef __LT_COMPAT_WINREG_H__
#define __LT_COMPAT_WINREG_H__

#include <windows.h>

typedef DWORD REGSAM;

// Predefined root keys (opaque non-null sentinels).
#define HKEY_CLASSES_ROOT   ((HKEY)(uintptr_t)0x80000000ul)
#define HKEY_CURRENT_USER   ((HKEY)(uintptr_t)0x80000001ul)
#define HKEY_LOCAL_MACHINE  ((HKEY)(uintptr_t)0x80000002ul)
#define HKEY_USERS          ((HKEY)(uintptr_t)0x80000003ul)

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS        0L
#endif
#ifndef ERROR_FILE_NOT_FOUND
#define ERROR_FILE_NOT_FOUND 2L
#endif

// Value types.
#define REG_NONE    0
#define REG_SZ      1
#define REG_BINARY  3
#define REG_DWORD   4

// Access rights / create options.
#define KEY_READ                0x20019
#define KEY_WRITE               0x20006
#define KEY_ALL_ACCESS          0xF003F
#define REG_OPTION_NON_VOLATILE 0

// Every "opened" key resolves to this sentinel; it is never dereferenced.
#define LT_REG_FAKEKEY ((HKEY)(uintptr_t)0x1)

static inline LONG RegCreateKeyEx(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
                                  void*, HKEY* phkResult, DWORD* lpdwDisposition)
{
    if (phkResult)       *phkResult = LT_REG_FAKEKEY;
    if (lpdwDisposition) *lpdwDisposition = 0;
    return ERROR_SUCCESS;
}

static inline LONG RegOpenKeyEx(HKEY, LPCSTR, DWORD, REGSAM, HKEY* phkResult)
{
    if (phkResult) *phkResult = LT_REG_FAKEKEY;
    return ERROR_SUCCESS;
}

static inline LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }

static inline LONG RegSetValueEx(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD)
{
    return ERROR_SUCCESS;   // discarded
}

static inline LONG RegQueryValueEx(HKEY, LPCSTR, LPDWORD, LPDWORD lpType,
                                   LPBYTE, LPDWORD lpcbData)
{
    if (lpType)   *lpType = REG_NONE;
    if (lpcbData) *lpcbData = 0;
    return ERROR_FILE_NOT_FOUND;   // caller falls back to its default
}

static inline LONG RegDeleteValue(HKEY, LPCSTR) { return ERROR_SUCCESS; }
static inline LONG RegDeleteKey(HKEY, LPCSTR)   { return ERROR_SUCCESS; }

#endif // __LT_COMPAT_WINREG_H__
