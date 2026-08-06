// ------------------------------------------------------------------------- //
//
// FILE      : U N D A T A . H
//
// CREATED   : 10/29/99
//
// AUTHOR    : Matthew Scott
//
// COPYRIGHT : Monolith Productions Inc.
//
// ORIGN     : Loosly based upon struct banks and moArray's
//
// ------------------------------------------------------------------------- //

#ifndef __UNDATA_H__
#define __UNDATA_H__

// ------------------------------------------------------------------------- //
// Includes this module depends on
// ------------------------------------------------------------------------- //
#include "ltbasetypes.h"
#include "stdlterror.h"
#include "ltassert.h"

// ------------------------------------------------------------------------- //
// Typedefs
// ------------------------------------------------------------------------- //

// ------------------------------------------------------------------------- //
// Externs (Public C data)
// ------------------------------------------------------------------------- //
extern class CGenUnboundData *g_pUnboundData;

// ------------------------------------------------------------------------- //
// Prototypes (Public C methods)
// ------------------------------------------------------------------------- //

// ------------------------------------------------------------------------- //
// Class Definitions
// ------------------------------------------------------------------------- //

class CGenUnboundData {
	// The heap
	uint32* heapStart;
	uint32* heapEnd;

	// The free list
	uint32* FreeListHead;
	uint32 minFreeBlocks;
	uint32 CurBlockSize;
	uint32 FreeBlocks;
	uint32 GetCnt;
	uint32 RetCnt;

	public:
		// Set it all up called from derived class constructor
		ESTDLTResults Init();

		uint32  GetBlockSize    ();

		// get and return of memory blocks
		uint32* GetNewBlock     ();
		void   ReturnOldBlock  (uint32* block);

		// platform specific routine for seting up initial memory segment
		virtual ESTDLTResults
		initUnboundHeap (uint32 blockSize, uint32** begin, uint32** end) = 0;

		// print debug info
		void PrintUndataStats ();
};

inline uint32*
CGenUnboundData::GetNewBlock () {
	uint32 *retVal;

	if (FreeListHead == NULL) {
		ASSERT (false);
		return (NULL);
	}

	FreeBlocks--;
 
	if (FreeBlocks < minFreeBlocks)
		minFreeBlocks = FreeBlocks;
	ASSERT (minFreeBlocks > 0);

	GetCnt++;

	retVal = FreeListHead;
	// The free block's first word holds the next-link at pointer width (a 32-bit
	// slot cannot hold a 64-bit pointer on LP64).
	FreeListHead = (uint32 *)(*(uintptr_t *)FreeListHead);
	return (retVal);
}

inline uint32
CGenUnboundData::GetBlockSize () {
	return (CurBlockSize);
}

inline void
CGenUnboundData::ReturnOldBlock (uint32* block) {
	// Error checking
	if (block == NULL) {
		CRITICAL_ERROR ("undata.h", "Tried to free a NULL block");
	}

	// 1st word of block (next pointer) gets what FreeListHead was pointing at.
	// Stored at pointer width: a uint32 slot would truncate a 64-bit pointer.
	*(uintptr_t *)block = (uintptr_t) FreeListHead;

	FreeListHead = block;
	FreeBlocks++;
	RetCnt++;
}

#endif  // __UNDATA_H__

