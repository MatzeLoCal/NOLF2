// macOS compat: mbstring.h (MSVC multibyte-string runtime)
//
// NOLF2's asset text is single-byte (or the -fshort-wchar wide path); the few
// _mbs* calls in the client UI operate on plain char buffers. Map them to the
// byte-oriented C runtime — behavior is identical for single-byte encodings.
#ifndef __MBSTRING_H_COMPAT__
#define __MBSTRING_H_COMPAT__

#include <string.h>
#include <strings.h>   // strcasecmp

static inline size_t _mbstrlen(const char *s) { return s ? strlen(s) : 0; }

static inline unsigned char *_mbsncpy(unsigned char *dst, const unsigned char *src, size_t n)
{
	return (unsigned char *)strncpy((char *)dst, (const char *)src, n);
}

static inline int _mbsicmp(const unsigned char *a, const unsigned char *b)
{
	return strcasecmp((const char *)a, (const char *)b);
}

static inline int _mbscmp(const unsigned char *a, const unsigned char *b)
{
	return strcmp((const char *)a, (const char *)b);
}

// Advance one "character" — one byte for single-byte encodings.
static inline unsigned char *_mbsinc(const unsigned char *s)
{
	return (unsigned char *)(s + (*s ? 1 : 0));
}

// Byte count of the first n characters — n bytes for single-byte encodings.
static inline size_t _mbsnbcnt(const unsigned char *s, size_t n)
{
	size_t nBytes = 0;
	while (n-- && *s) { ++s; ++nBytes; }
	return nBytes;
}

#endif // __MBSTRING_H_COMPAT__
