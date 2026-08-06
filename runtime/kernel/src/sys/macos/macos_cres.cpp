// ----------------------------------------------------------------------- //
//
// MODULE  : macos_cres.cpp
//
// PURPOSE : Reads the string tables out of a Win32 resource DLL (cres.dll) so
//           the game's LoadString()/FormatString() work on macOS. See
//           macos_cres.h for why this exists and how the handle reaches the
//           game.
//
//           PE layout walked here (all little-endian, same as arm64, but every
//           multi-byte field is memcpy'd out to stay alignment-safe):
//             MZ header .e_lfanew -> "PE\0\0" -> COFF header -> optional header
//             (PE32 magic 0x10B / PE32+ 0x20B decides where the data
//             directories start) -> DataDirectory[2] = RESOURCE RVA
//             -> section table maps that RVA to a file offset.
//           The resource tree is 3 levels: Type -> Name/Id -> Language, each an
//           IMAGE_RESOURCE_DIRECTORY followed by 8-byte entries; leaves are
//           IMAGE_RESOURCE_DATA_ENTRY whose OffsetToData is an RVA.
//
//           RT_STRING (type 6) stores strings in blocks of 16: the resource
//           whose id is (stringID / 16) + 1 holds 16 consecutive
//           uint16-length-prefixed UTF-16 strings; stringID % 16 selects one.
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "macos_cres.h"
#include "iltclient.h"
#include "iltstream.h"

#include <stdio.h>
#include <string.h>
#include <vector>

static ILTClient *ilt_client_cres;
define_holder(ILTClient, ilt_client_cres);

#define LTCRES_RT_STRING 6

namespace
{

// A parsed string table: id -> string. Built once at load.
class CMacCResData
{
public:
	// stringID -> text (already narrowed from UTF-16)
	std::vector<std::pair<uint32, std::string> > m_aStrings;

	const char *Find(uint32 nID) const
	{
		// Small table (a few thousand entries), only hit during UI setup.
		for (size_t i = 0; i < m_aStrings.size(); ++i)
		{
			if (m_aStrings[i].first == nID)
				return m_aStrings[i].second.c_str();
		}
		return 0;
	}
};

// --- alignment-safe little-endian readers ---------------------------------
inline bool cres_Read16(const std::vector<uint8> &aData, size_t nOff, uint16 &nOut)
{
	if (nOff + 2 > aData.size()) return false;
	memcpy(&nOut, &aData[nOff], 2);
	return true;
}
inline bool cres_Read32(const std::vector<uint8> &aData, size_t nOff, uint32 &nOut)
{
	if (nOff + 4 > aData.size()) return false;
	memcpy(&nOut, &aData[nOff], 4);
	return true;
}

struct CResSection
{
	uint32 m_nVirtualAddress;
	uint32 m_nSizeOfRawData;
	uint32 m_nPointerToRawData;
};

// Map an RVA to a file offset using the section table.
bool cres_RVAToOffset(const std::vector<CResSection> &aSections, uint32 nRVA, size_t &nOut)
{
	for (size_t i = 0; i < aSections.size(); ++i)
	{
		const CResSection &cSec = aSections[i];
		if (nRVA >= cSec.m_nVirtualAddress &&
		    nRVA <  cSec.m_nVirtualAddress + cSec.m_nSizeOfRawData)
		{
			nOut = (size_t)(nRVA - cSec.m_nVirtualAddress + cSec.m_nPointerToRawData);
			return true;
		}
	}
	return false;
}

// Narrow a UTF-16 run to char. The game's text is Latin-1; anything outside
// that becomes '?' rather than silently truncating to a bogus byte.
void cres_NarrowUTF16(const std::vector<uint8> &aData, size_t nOff, uint16 nChars, std::string &sOut)
{
	sOut.clear();
	sOut.reserve(nChars);
	for (uint16 i = 0; i < nChars; ++i)
	{
		uint16 nCh = 0;
		if (!cres_Read16(aData, nOff + (size_t)i * 2, nCh))
			break;
		sOut.push_back((nCh && nCh < 0x100) ? (char)nCh : (nCh ? '?' : ' '));
	}
}

// Walk one RT_STRING leaf: 16 length-prefixed UTF-16 strings.
void cres_ParseStringBlock(const std::vector<uint8> &aData, size_t nOff, uint32 nSize,
                           uint32 nBlockID, CMacCResData &cOut)
{
	size_t nEnd = nOff + nSize;
	// Ids in this block start at (blockID - 1) * 16.
	uint32 nBaseID = (nBlockID - 1) * 16;

	for (uint32 i = 0; i < 16 && nOff + 2 <= nEnd; ++i)
	{
		uint16 nLen = 0;
		if (!cres_Read16(aData, nOff, nLen))
			break;
		nOff += 2;

		if (nLen)
		{
			if (nOff + (size_t)nLen * 2 > nEnd)
				break;
			std::string sText;
			cres_NarrowUTF16(aData, nOff, nLen, sText);
			cOut.m_aStrings.push_back(std::make_pair(nBaseID + i, sText));
			nOff += (size_t)nLen * 2;
		}
		// zero-length entries are simply "no string at this id"
	}
}

// Recursively walk the resource tree. nLevel: 0=Type, 1=Name/Id, 2=Language.
void cres_WalkResourceDir(const std::vector<uint8> &aData,
                          const std::vector<CResSection> &aSections,
                          size_t nResRoot, size_t nDirOff, int nLevel,
                          bool bInStringType, uint32 nBlockID,
                          CMacCResData &cOut)
{
	uint16 nNamed = 0, nIds = 0;
	if (!cres_Read16(aData, nDirOff + 12, nNamed)) return;
	if (!cres_Read16(aData, nDirOff + 14, nIds)) return;

	size_t nEntry = nDirOff + 16;
	uint32 nTotal = (uint32)nNamed + (uint32)nIds;

	for (uint32 i = 0; i < nTotal; ++i, nEntry += 8)
	{
		uint32 nName = 0, nOffsetToData = 0;
		if (!cres_Read32(aData, nEntry, nName)) return;
		if (!cres_Read32(aData, nEntry + 4, nOffsetToData)) return;

		bool bIsNamed = (nName & 0x80000000u) != 0;
		uint32 nID = bIsNamed ? 0 : nName;

		if (nLevel == 0)
		{
			// Type level: only RT_STRING interests us (named types can't be it).
			if (bIsNamed || nID != LTCRES_RT_STRING)
				continue;
		}
		else if (nLevel == 1 && !bIsNamed)
		{
			nBlockID = nID;   // the string-table block number
		}

		if (nOffsetToData & 0x80000000u)
		{
			// Subdirectory.
			size_t nSub = nResRoot + (nOffsetToData & 0x7FFFFFFFu);
			cres_WalkResourceDir(aData, aSections, nResRoot, nSub, nLevel + 1,
			                     bInStringType || nLevel == 0, nBlockID, cOut);
		}
		else if (nLevel >= 1 && nBlockID)
		{
			// Leaf: IMAGE_RESOURCE_DATA_ENTRY { RVA, Size, CodePage, Reserved }
			size_t nDataEntry = nResRoot + nOffsetToData;
			uint32 nDataRVA = 0, nDataSize = 0;
			if (!cres_Read32(aData, nDataEntry, nDataRVA)) continue;
			if (!cres_Read32(aData, nDataEntry + 4, nDataSize)) continue;

			size_t nDataOff = 0;
			if (!cres_RVAToOffset(aSections, nDataRVA, nDataOff)) continue;
			if (nDataOff + nDataSize > aData.size()) continue;

			cres_ParseStringBlock(aData, nDataOff, nDataSize, nBlockID, cOut);
		}
	}
}

// Parse the whole PE and pull out every RT_STRING.
bool cres_ParsePE(const std::vector<uint8> &aData, CMacCResData &cOut)
{
	uint16 nMZ = 0;
	if (!cres_Read16(aData, 0, nMZ) || nMZ != 0x5A4D)   // 'MZ'
		return false;

	uint32 nPEOff = 0;
	if (!cres_Read32(aData, 0x3C, nPEOff))
		return false;

	uint32 nPESig = 0;
	if (!cres_Read32(aData, nPEOff, nPESig) || nPESig != 0x00004550)   // 'PE\0\0'
		return false;

	size_t nCoff = nPEOff + 4;
	uint16 nNumSections = 0, nSizeOfOptional = 0;
	if (!cres_Read16(aData, nCoff + 2, nNumSections)) return false;
	if (!cres_Read16(aData, nCoff + 16, nSizeOfOptional)) return false;

	size_t nOptional = nCoff + 20;
	uint16 nOptMagic = 0;
	if (!cres_Read16(aData, nOptional, nOptMagic)) return false;

	// Data directories sit at a different offset for PE32 vs PE32+.
	size_t nDataDirs;
	if (nOptMagic == 0x010B)      nDataDirs = nOptional + 96;    // PE32
	else if (nOptMagic == 0x020B) nDataDirs = nOptional + 112;   // PE32+
	else return false;

	// DataDirectory[2] == IMAGE_DIRECTORY_ENTRY_RESOURCE
	uint32 nResRVA = 0, nResSize = 0;
	if (!cres_Read32(aData, nDataDirs + 2 * 8, nResRVA)) return false;
	if (!cres_Read32(aData, nDataDirs + 2 * 8 + 4, nResSize)) return false;
	if (!nResRVA || !nResSize)
		return false;   // no resources at all

	// Section table follows the optional header.
	size_t nSecTable = nOptional + nSizeOfOptional;
	std::vector<CResSection> aSections;
	aSections.reserve(nNumSections);
	for (uint16 i = 0; i < nNumSections; ++i)
	{
		size_t nSec = nSecTable + (size_t)i * 40;
		CResSection cSec;
		if (!cres_Read32(aData, nSec + 12, cSec.m_nVirtualAddress)) return false;
		if (!cres_Read32(aData, nSec + 16, cSec.m_nSizeOfRawData)) return false;
		if (!cres_Read32(aData, nSec + 20, cSec.m_nPointerToRawData)) return false;
		aSections.push_back(cSec);
	}

	size_t nResRoot = 0;
	if (!cres_RVAToOffset(aSections, nResRVA, nResRoot))
		return false;

	cres_WalkResourceDir(aData, aSections, nResRoot, nResRoot, 0, false, 0, cOut);
	return !cOut.m_aStrings.empty();
}

// --- the module handle handed to the game ---------------------------------

int cres_LoadStringImpl(LTMacResModule *pSelf, unsigned int nID, char *pBuf, int nBufLen)
{
	if (!pBuf || nBufLen <= 0)
		return 0;
	pBuf[0] = 0;

	if (!pSelf || !pSelf->m_pImpl)
		return 0;

	CMacCResData *pData = (CMacCResData*)pSelf->m_pImpl;
	const char *pStr = pData->Find(nID);
	if (!pStr)
		return 0;

	strncpy(pBuf, pStr, (size_t)nBufLen - 1);
	pBuf[nBufLen - 1] = 0;
	return (int)strlen(pBuf);
}

LTMacResModule *g_pCResModule = 0;
bool            g_bCResTried  = false;

} // anonymous namespace

LTMacResModule *LTMacCRes_GetDefault()
{
	if (g_bCResTried)
		return g_pCResModule;
	g_bCResTried = true;

	if (!ilt_client_cres)
		return 0;

	// cres.dll ships inside the rez trees; the engine FS finds it there or as
	// a loose file.
	ILTStream *pStream = 0;
	if (ilt_client_cres->OpenFile("cres.dll", &pStream) != LT_OK || !pStream)
	{
		fprintf(stderr, "[cres] cres.dll not found — game UI strings will be empty\n");
		return 0;
	}

	uint32 nLen = 0;
	pStream->GetLen(&nLen);
	std::vector<uint8> aData(nLen);
	LTRESULT nRead = (nLen > 0) ? pStream->Read(&aData[0], nLen) : LT_ERROR;
	pStream->Release();

	if (nRead != LT_OK)
	{
		fprintf(stderr, "[cres] couldn't read cres.dll (%u bytes)\n", nLen);
		return 0;
	}

	CMacCResData *pParsed = new CMacCResData;
	if (!cres_ParsePE(aData, *pParsed))
	{
		fprintf(stderr, "[cres] cres.dll has no parsable string resources\n");
		delete pParsed;
		return 0;
	}

	g_pCResModule = new LTMacResModule;
	g_pCResModule->m_nMagic        = LTMACRESMODULE_MAGIC;
	g_pCResModule->m_pfnLoadString = cres_LoadStringImpl;
	g_pCResModule->m_pImpl         = pParsed;

	fprintf(stderr, "[cres] cres.dll: %u strings loaded\n",
	        (uint32)pParsed->m_aStrings.size());
	return g_pCResModule;
}

void LTMacCRes_Term()
{
	if (g_pCResModule)
	{
		delete (CMacCResData*)g_pCResModule->m_pImpl;
		delete g_pCResModule;
		g_pCResModule = 0;
	}
	g_bCResTried = false;
}
