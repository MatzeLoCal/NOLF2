//////////////////////////////////////////////////////////////////////////////
// Game implementation of the standard allocator

#ifdef __clang__	// "stdafx.h" is StdAfx.h in the client module and Stdafx.h in the server one
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonportable-include-path"
#endif
#include "stdafx.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <stdlib.h>

void* DefStdlithAlloc(uint32 size) 
{
	if(size == 0)
		return NULL;

	return malloc((size_t)size);
}

void DefStdlithFree(void *ptr)
{
	if(ptr)
	{
		free(ptr);
	}
}
