// Minimal stub of MSVC <io.h> (low-level file access) for the macOS build.
// (Also serves `#include <IO.h>` — APFS is case-insensitive.)
#ifndef __LT_COMPAT_IO_H__
#define __LT_COMPAT_IO_H__

#include <unistd.h>
#include <fcntl.h>

#ifndef _access
#define _access access
#endif
#ifndef _open
#define _open open
#endif
#ifndef _close
#define _close close
#endif
#ifndef _read
#define _read read
#endif
#ifndef _write
#define _write write
#endif
#ifndef _lseek
#define _lseek lseek
#endif
#ifndef _unlink
#define _unlink unlink
#endif

// --- _findfirst/_findnext/_findclose (MSVC file enumeration) ---------------
// Callers pass a glob like "dir\subdir\*.ext" and iterate matches. Implemented
// with opendir/readdir + fnmatch; both path separators are tolerated. Handles
// are heap pointers cast to long (LP64: pointers fit).
#ifdef __cplusplus

#include <dirent.h>
#include <fnmatch.h>
#include <string.h>
#include <stdlib.h>

#define _A_NORMAL 0x00
#define _A_SUBDIR 0x10

// MSVC spelling of the stat directory mode bit.
#include <sys/stat.h>
#ifndef _S_IFDIR
#define _S_IFDIR S_IFDIR
#endif

struct _finddata_t {
    unsigned    attrib;
    long        time_create, time_access, time_write;
    unsigned long size;
    char        name[260];
};

struct lt_findhandle_ {
    DIR* dir;
    char pattern[260];
    char dirpath[1024];
};

static inline int lt_find_fill_(lt_findhandle_* h, struct _finddata_t* fd) {
    struct dirent* e;
    while ((e = readdir(h->dir)) != NULL) {
        if (fnmatch(h->pattern, e->d_name, 0) == 0) {
            strncpy(fd->name, e->d_name, sizeof(fd->name) - 1);
            fd->name[sizeof(fd->name) - 1] = 0;
            fd->attrib = (e->d_type == DT_DIR) ? _A_SUBDIR : _A_NORMAL;
            fd->size = 0; fd->time_create = fd->time_access = fd->time_write = 0;
            return 0;
        }
    }
    return -1;
}

static inline long _findfirst(const char* spec, struct _finddata_t* fd) {
    if (!spec || !fd) return -1;
    lt_findhandle_* h = (lt_findhandle_*)calloc(1, sizeof(lt_findhandle_));
    if (!h) return -1;
    // split spec into directory + pattern, tolerating both separators
    char buf[1024];
    strncpy(buf, spec, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char* p = buf; *p; ++p) if (*p == '\\') *p = '/';
    const char* slash = strrchr(buf, '/');
    if (slash) {
        size_t n = (size_t)(slash - buf);
        memcpy(h->dirpath, buf, n); h->dirpath[n] = 0;
        strncpy(h->pattern, slash + 1, sizeof(h->pattern) - 1);
    } else {
        strcpy(h->dirpath, ".");
        strncpy(h->pattern, buf, sizeof(h->pattern) - 1);
    }
    h->dir = opendir(h->dirpath);
    if (!h->dir || lt_find_fill_(h, fd) != 0) {
        if (h->dir) closedir(h->dir);
        free(h);
        return -1;
    }
    return (long)(uintptr_t)h;
}

// NOTE: _findfirst returns -1 on failure, and callers routinely pass that
// straight back to _findnext/_findclose without checking (MSVC tolerates it).
// Reject -1 explicitly — treating it as a pointer dereferences 0xFFFF...FF.
static inline int _findnext(long handle, struct _finddata_t* fd) {
    if (handle == -1) return -1;
    lt_findhandle_* h = (lt_findhandle_*)(uintptr_t)handle;
    if (!h || !fd) return -1;
    return lt_find_fill_(h, fd);
}

static inline int _findclose(long handle) {
    if (handle == -1) return -1;
    lt_findhandle_* h = (lt_findhandle_*)(uintptr_t)handle;
    if (!h) return -1;
    if (h->dir) closedir(h->dir);
    free(h);
    return 0;
}

#endif // __cplusplus

#endif // __LT_COMPAT_IO_H__
