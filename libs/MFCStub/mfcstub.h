// mfcstub.h - Stub header file for duplicating the needed functionality to 
//		remove MFC from a project with as minimal an amount of change as possible

#ifndef __MFCSTUB_H__
#define __MFCSTUB_H__

// stdlith dependency
#include "stdlith.h"

// Some headers that MFC automatically includes for you
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif

#include "mfcs_types.h"
#include "mfcs_point.h"
#include "mfcs_rect.h"
#include "mfcs_string.h"
#include "mfcs_misc.h"

#endif // __MFCSTUB_H__