// Minimal stub of MSVC <direct.h> (directory functions) for the macOS build.
#ifndef __LT_COMPAT_DIRECT_H__
#define __LT_COMPAT_DIRECT_H__

#include <unistd.h>
#include <sys/stat.h>

// MSVC _mkdir takes no mode; POSIX mkdir does.
static inline int lt_mkdir_(const char* path) { return mkdir(path, 0755); }
#ifndef _mkdir
#define _mkdir lt_mkdir_
#endif
#ifndef _rmdir
#define _rmdir rmdir
#endif
#ifndef _chdir
#define _chdir chdir
#endif
#ifndef _getcwd
#define _getcwd getcwd
#endif

#endif // __LT_COMPAT_DIRECT_H__
