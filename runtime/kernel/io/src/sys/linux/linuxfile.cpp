// ------------------------------------------------------------------ //
// Includes..
// ------------------------------------------------------------------ //

#include <sys/stat.h>

#include "bdefs.h"
#include <string.h>
//#include <io.h>
#include <stdio.h>
#include <dirent.h>
#include "sysfile.h"
#include "de_memory.h"
#include "dutil.h"
//#include "dsys_interface.h"
#include "syslthread.h"
#include "syscounter.h"
#include "rezmgr.h"
#include "genltstream.h"

// console output of file access
// 0 - no output (default)
// 1 - display file open calls
// 2 - display file open and close calls
// 3 - display file open and close calls
// 4 - display file open and close and read calls
extern int32 g_CV_ShowFileAccess;

// PlayDemo profile info.
uint32 g_PD_FOpen=0;


typedef struct FileTree_t
{
	TreeType	m_TreeType;
	CRezMgr*	m_pRezMgr;
	char		m_BaseName[1];
} FileTree;


class BaseFileStream : public CGenLTStream
{
public:

		BaseFileStream()
		{
			m_FileLen = 0;
			m_SeekOffset = 0;
			m_pFile = LTNULL;
			m_pTree = LTNULL;
			m_ErrorStatus = 0;
			m_nNumReadCalls = 0;
			m_nTotalBytesRead = 0;
		}

	virtual	~BaseFileStream()
	{
		if (g_CV_ShowFileAccess >= 2)
		{
			dsi_ConsolePrint("close stream %p num reads = %u bytes read = %u", 
				this, m_nNumReadCalls, m_nTotalBytesRead);
		}

		if(m_pTree->m_TreeType == UnixTree && m_pFile)
		{
			fclose(m_pFile);
		}
	}

	LTRESULT ErrorStatus()
	{
		return m_ErrorStatus ? LT_ERROR : LT_OK;
	}

	LTRESULT GetLen(uint32 *len)
	{
		*len = m_FileLen;
		return LT_OK;
	}

	LTRESULT Write(const void *pData, uint32 dataLen)
	{
		// Can't write to these streams.
		return LT_ERROR;
	}

	unsigned long	m_FileLen;		// Stored when the file is opened.
	unsigned long	m_SeekOffset;	// Seek offset (used in rezfiles) (NOTE: in a rezmgr rez file this is the current position inside the resource).
	FILE			*m_pFile;
	struct FileTree_t	*m_pTree;
	int				m_ErrorStatus;
	unsigned long	m_nNumReadCalls;
	unsigned long	m_nTotalBytesRead;
};


// The engine and game address files with Windows-style backslash paths
// ("Attributes\ModelButes.txt"); POSIX file APIs need forward slashes.
static void df_NormalizeSeparators(char *pPath)
{
	for (; *pPath; ++pPath)
		if (*pPath == '\\') *pPath = '/';
}

class UnixFileStream : public BaseFileStream
{
public:

	void	Release();

	LTRESULT GetPos(uint32 *pos)
	{
		*pos = (uint32)(ftell(m_pFile) - m_SeekOffset);
		return LT_OK;
	}
	
	LTRESULT SeekTo(uint32 offset)
	{
		long ret;

		ret = fseek(m_pFile, offset + m_SeekOffset, SEEK_SET);
		if(ret == 0)
		{
			return LT_OK;
		}
		else
		{
			m_ErrorStatus = 1;
			return LT_ERROR;
		}
	}

	LTRESULT Read(void *pData, uint32 size)
	{
		size_t sizeRead;

		if (g_CV_ShowFileAccess >= 4)
		{
			dsi_ConsolePrint("read %u bytes from stream %p",size, this);
		}

		if(m_ErrorStatus == 1)
		{
			memset(pData, 0, size);
			return LT_ERROR;
		}

		if(size != 0)
		{
			sizeRead = fread(pData, 1, size, m_pFile);
			if(sizeRead == size)
			{
				m_nNumReadCalls++;
				m_nTotalBytesRead += sizeRead;
				return LT_OK;
			}
			else
			{			
				memset(pData, 0, size);
				m_ErrorStatus = 1;
				return LT_ERROR;
			}
		}

		return LT_OK;
	}
};

static ObjectBank<UnixFileStream> g_UnixFileStreamBank(8, 8);


void UnixFileStream::Release()
{
	g_UnixFileStreamBank.Free(this);
}


// Stream over an item inside a .rez archive (mirrors the Win32 RezFileStream:
// m_SeekOffset is the current position inside the resource).
class UnixRezFileStream : public BaseFileStream
{
public:

	UnixRezFileStream() : m_pRezItm(LTNULL) {}

	void	Release();

	LTRESULT GetPos(uint32 *pos)
	{
		*pos = (uint32)m_SeekOffset;
		return LT_OK;
	}

	LTRESULT SeekTo(uint32 offset)
	{
		if (m_pRezItm && m_pRezItm->Seek(offset))
		{
			m_SeekOffset = offset;
			return LT_OK;
		}
		m_ErrorStatus = 1;
		return LT_ERROR;
	}

	LTRESULT Read(void *pData, uint32 size)
	{
		if (m_ErrorStatus == 1)
		{
			memset(pData, 0, size);
			return LT_ERROR;
		}

		if (size != 0)
		{
			DWORD sizeRead = m_pRezItm->Read(pData, size, (DWORD)m_SeekOffset);
			m_SeekOffset += sizeRead;
			if (sizeRead != size)
			{
				memset(pData, 0, size);
				m_ErrorStatus = 1;
				return LT_ERROR;
			}
			m_nNumReadCalls++;
			m_nTotalBytesRead += sizeRead;
		}

		return LT_OK;
	}

	CRezItm	*m_pRezItm;
};

static ObjectBank<UnixRezFileStream> g_UnixRezFileStreamBank(8, 8);

void UnixRezFileStream::Release()
{
	g_UnixRezFileStreamBank.Free(this);
}

// LTFindInfo::m_pInternal — see df_FindNext (defined there).



// ------------------------------------------------------------------ //
// Interface functions.
// ------------------------------------------------------------------ //

void df_Init()
{
}

void df_Term()
{
}

int df_OpenTree(const char *pName, HLTFileTree *&pTreePointer)
{
	long allocSize;
	FileTree *pTree;
	struct stat info;

	pTreePointer = NULL;

	// See if it exists..
	if (stat(pName,&info) != 0)
		return -1;

	allocSize = sizeof(FileTree) + strlen(pName);

	if (S_ISDIR(info.st_mode))
	{
		// A directory: plain filesystem tree.
		pTree = (FileTree*)dalloc_z(allocSize);
		pTree->m_TreeType = UnixTree;
	}
	else
	{
		// A file: .rez archive (mirrors the Win32 df_OpenTree).
		pTree = (FileTree*)dalloc_z(allocSize);
		pTree->m_TreeType = RezFileTree;

		LT_MEM_TRACK_ALLOC(pTree->m_pRezMgr = new CRezMgr, LT_MEM_TYPE_FILE);
		if (pTree->m_pRezMgr == LTNULL)
		{
			dfree(pTree);
			return -2;
		}

		if (!pTree->m_pRezMgr->Open(pName))
		{
			delete pTree->m_pRezMgr;
			dfree(pTree);
			return -2;
		}

		// Lookups arrive with either separator.
		pTree->m_pRezMgr->SetDirSeparators("\\/");
	}

	strcpy(pTree->m_BaseName, pName);
	pTreePointer = (HLTFileTree*)pTree;
	return 0;
}


void df_CloseTree(HLTFileTree* hTree)
{
	FileTree *pTree;

	pTree = (FileTree*)hTree;
	if(!pTree)
		return;

	if (pTree->m_pRezMgr)
	{
		pTree->m_pRezMgr->Close();
		delete pTree->m_pRezMgr;
		pTree->m_pRezMgr = LTNULL;
	}

	dfree(pTree);
}


TreeType df_GetTreeType(HLTFileTree* hTree)
{
	FileTree *pTree;

	pTree = (FileTree*)hTree;
	if(!pTree)
		return UnixTree;

	return pTree->m_TreeType;
}


void df_BuildName(char *pPart1, char *pPart2, char *pOut)
{
	if(pPart1[0] == 0)
	{
		strcpy(pOut, pPart2);
	}
	else
	{
		sprintf(pOut, "%s/%s", pPart1, pPart2);
	}
	df_NormalizeSeparators(pOut);
}


int df_GetFileInfo(HLTFileTree* hTree, const char *pName, LTFindInfo *pInfo)
{
	FileTree *pTree;
	long handle, curRet;
	char fullName[500];
	CRezItm* pRezItm;

	pTree = (FileTree*)hTree;
	if(!pTree)
		return false;

	if(pTree->m_TreeType == UnixTree)
	{
		sprintf(fullName, "%s/%s", pTree->m_BaseName, pName);
		df_NormalizeSeparators(fullName);
		char* pLastSlash = strrchr(fullName, '/');
		*(pLastSlash++) = '\0';
		
		int numMatches;
		dirent** namelist;
		if ((numMatches = scandir(fullName, &namelist, 0, alphasort)) > 0)
		{
		        for (int iMatch = 0; iMatch < numMatches; ++iMatch)
			{
				if (namelist[iMatch]->d_type != DT_DIR) 
				{
					strncpy(pInfo->m_Name, namelist[iMatch]->d_name, 255);
					pInfo->m_Type = FILE_TYPE;
					//pInfo->m_Size = data.size;
					//memcpy(&pInfo->m_Date, &data.time_write, 4);
					return true;
				}
			}
		}

		// Didn't find any files...
		return false;
	}
	else
	{
		pRezItm = pTree->m_pRezMgr->GetRezFromDosPath(pName);
		if (pRezItm != LTNULL)
		{
			strncpy(pInfo->m_Name, pRezItm->GetName(), sizeof(pInfo->m_Name)-1);
			pInfo->m_Type = FILE_TYPE;
			pInfo->m_Size = pRezItm->GetSize();
			pInfo->m_Date = pRezItm->GetTime();
			return true;
		}
		else
		{
			return false;
		}
	}
}


int df_GetDirInfo(HLTFileTree hTree, char *pName)
{
	FileTree *pTree;
	char fullName[500];
	struct stat info;

	pTree = (FileTree*)hTree;
	if(!pTree) return 0;

	sprintf(fullName, "%s/%s", pTree->m_BaseName, pName);
	df_NormalizeSeparators(fullName);
	if (stat(fullName,&info) != 0) return 0;
	return (S_ISDIR(info.st_mode));
}


int df_GetFullFilename(HLTFileTree hTree, char *pName, char *pOutName, int maxLen)
{
	FileTree *pTree;

	pTree = (FileTree*)hTree;
	if(!pTree)
		return 0;

	if(pTree->m_TreeType != UnixTree)
		return 0;
	
	sprintf(pOutName, "%s/%s", pTree->m_BaseName, pName);
	df_NormalizeSeparators(pOutName);
	return 1;
}


ILTStream* df_Open(HLTFileTree* hTree, const char *pName, int openMode)
{
	char fullName[500];
	FileTree *pTree;
	FILE *fp;
	UnixFileStream *pUnixStream;
	unsigned long seekOffset, fileLen;

	pTree = (FileTree*)hTree;
	if(!pTree)
		return NULL;

	// Temporary bring-up diagnostics: LT_TRACE_FILES=1 logs every open attempt.
	static int s_nTraceFiles = -1;
	if (s_nTraceFiles < 0)
		s_nTraceFiles = (getenv("LT_TRACE_FILES") != NULL) ? 1 : 0;

	if(pTree->m_TreeType == RezFileTree)
	{
		// Open from inside the .rez archive.
		CRezItm *pRezItm = pTree->m_pRezMgr->GetRezFromDosPath(pName);
		if (s_nTraceFiles)
			fprintf(stderr, "[df] open rez('%s') name='%s' -> %s\n",
				pTree->m_BaseName, pName, pRezItm ? "HIT" : "miss");
		if (!pRezItm)
			return NULL;

		UnixRezFileStream *pRezStream = g_UnixRezFileStreamBank.Allocate();
		pRezStream->m_pRezItm    = pRezItm;
		pRezStream->m_pTree      = pTree;
		pRezStream->m_pFile      = LTNULL;
		pRezStream->m_FileLen    = pRezItm->GetSize();
		pRezStream->m_SeekOffset = 0;
		pRezStream->m_ErrorStatus = 0;
		pRezItm->Seek(0);
		return pRezStream;
	}

	if(pTree->m_TreeType != UnixTree) {
		return NULL;
	}

	sprintf(fullName, "%s/%s", pTree->m_BaseName, pName);
	df_NormalizeSeparators(fullName);
	CountAdder cntAdd(&g_PD_FOpen);
	fp = fopen(fullName, "rb");
	if (s_nTraceFiles)
		fprintf(stderr, "[df] open dir '%s' -> %s\n", fullName, fp ? "HIT" : "miss");
	if (!fp)
		return NULL;

	fseek(fp, 0, SEEK_END);
	fileLen = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	seekOffset = 0;

	// Use fp to setup the stream.
	pUnixStream = g_UnixFileStreamBank.Allocate();
	pUnixStream->m_pFile = fp;
	pUnixStream->m_pTree = pTree;
	pUnixStream->m_FileLen = fileLen;
	pUnixStream->m_SeekOffset = seekOffset;

	if (g_CV_ShowFileAccess >= 1)
	{
		dsi_ConsolePrint("stream %p open file %s size = %u",pUnixStream,pName,fileLen);
	}

	return pUnixStream;
}


// ---------------------------------------------------------------------------
// Directory enumeration (df_FindNext / df_FindClose)
//
// Mirrors the Win32 de_file.cpp implementation for both tree kinds. This was a
// `return 0` stub through Phase 0-2 (the null client shell never enumerated
// anything); the real game shell needs it — e.g. CClientFXDB::Init calls
// GetFileList("ClientFX") and treats an empty list as a fatal error.
//
// Contract (see impl_common.cpp ic_GetFileList): the caller zeroes
// pInfo->m_pInternal before the first call; we allocate iteration state there
// and free it when the walk ends (returning 0). df_FindClose releases it if
// the caller abandons the walk early.
// ---------------------------------------------------------------------------

typedef struct LTFindData
{
	FileTree	*m_pTree;

	// UnixTree state
	DIR			*m_pDir;
	char		 m_sDirPath[_MAX_PATH + 1];

	// RezFileTree state
	CRezDir		*m_pCurDir;
	CRezTyp		*m_pCurTyp;
	CRezItm		*m_pCurItm;
	CRezDir		*m_pCurSubDir;
} LTFindData;

static void df_FreeFindData(LTFindInfo *pInfo)
{
	LTFindData *pFindData = (LTFindData*)pInfo->m_pInternal;
	if (!pFindData)
		return;
	if (pFindData->m_pDir)
		closedir(pFindData->m_pDir);
	dfree(pFindData);
	pInfo->m_pInternal = LTNULL;
}

int df_FindNext(HLTFileTree* hTree, const char *pDirName, LTFindInfo *pInfo)
{
	if (!hTree || !pInfo)
		return 0;

	FileTree *pTree = (FileTree*)hTree;
	LTFindData *pFindData = (LTFindData*)pInfo->m_pInternal;

	if (pTree->m_TreeType == UnixTree)
	{
		// First call: open the directory.
		if (!pFindData)
		{
			LT_MEM_TRACK_ALLOC(pFindData = (LTFindData*)dalloc(sizeof(LTFindData)), LT_MEM_TYPE_FILE);
			if (!pFindData)
				return 0;
			memset(pFindData, 0, sizeof(*pFindData));
			pFindData->m_pTree = pTree;

			if (pDirName && pDirName[0])
				LTSNPrintF(pFindData->m_sDirPath, sizeof(pFindData->m_sDirPath), "%s/%s",
				           pTree->m_BaseName, pDirName);
			else
				LTStrCpy(pFindData->m_sDirPath, pTree->m_BaseName, sizeof(pFindData->m_sDirPath));
			df_NormalizeSeparators(pFindData->m_sDirPath);

			pFindData->m_pDir = opendir(pFindData->m_sDirPath);
			pInfo->m_pInternal = pFindData;

			if (!pFindData->m_pDir)
			{
				df_FreeFindData(pInfo);
				return 0;
			}
		}

		// Return the next non-dot entry.
		struct dirent *pEnt;
		while ((pEnt = readdir(pFindData->m_pDir)) != NULL)
		{
			if (pEnt->d_name[0] == '.')
				continue;   // skip ".", ".." and dotfiles (Win32 skips '.' names too)

			char sFullPath[_MAX_PATH + 1];
			LTSNPrintF(sFullPath, sizeof(sFullPath), "%s/%s", pFindData->m_sDirPath, pEnt->d_name);

			struct stat cStat;
			if (stat(sFullPath, &cStat) != 0)
				continue;

			LTStrCpy(pInfo->m_Name, pEnt->d_name, sizeof(pInfo->m_Name));
			pInfo->m_Type = S_ISDIR(cStat.st_mode) ? DIRECTORY_TYPE : FILE_TYPE;
			pInfo->m_Date = (unsigned long)cStat.st_mtime;
			pInfo->m_Size = (unsigned long)cStat.st_size;
			return 1;
		}

		// Exhausted.
		df_FreeFindData(pInfo);
		return 0;
	}

	// --- RezFileTree ---
	if (!pTree->m_pRezMgr)
		return 0;

	// First call: locate the directory and prime the item/sub-dir cursors.
	if (!pFindData)
	{
		CRezDir *pRezDir = pTree->m_pRezMgr->GetDirFromPath(pDirName);
		if (!pRezDir)
			return 0;

		CRezTyp *pRezTyp = pRezDir->GetFirstType();
		CRezItm *pRezItm = pRezTyp ? pRezDir->GetFirstItem(pRezTyp) : LTNULL;
		CRezDir *pRezSubDir = LTNULL;

		// No files? Fall back to enumerating sub-directories.
		if (!pRezItm)
		{
			pRezSubDir = pRezDir->GetFirstSubDir();
			if (!pRezSubDir)
				return 0;
		}

		LT_MEM_TRACK_ALLOC(pFindData = (LTFindData*)dalloc(sizeof(LTFindData)), LT_MEM_TYPE_FILE);
		if (!pFindData)
			return 0;
		memset(pFindData, 0, sizeof(*pFindData));
		pFindData->m_pTree     = pTree;
		pFindData->m_pCurDir   = pRezDir;
		pFindData->m_pCurTyp   = pRezTyp;
		pFindData->m_pCurItm   = pRezItm;
		pFindData->m_pCurSubDir = pRezSubDir;
		pInfo->m_pInternal = pFindData;
	}
	else
	{
		// Advance: items (grouped by type) first, then sub-directories.
		if (pFindData->m_pCurItm)
		{
			pFindData->m_pCurItm = pFindData->m_pCurDir->GetNextItem(pFindData->m_pCurItm);

			while (!pFindData->m_pCurItm)
			{
				pFindData->m_pCurTyp = pFindData->m_pCurDir->GetNextType(pFindData->m_pCurTyp);
				if (!pFindData->m_pCurTyp)
				{
					// Done with files — switch to sub-directories.
					pFindData->m_pCurSubDir = pFindData->m_pCurDir->GetFirstSubDir();
					if (!pFindData->m_pCurSubDir)
					{
						df_FreeFindData(pInfo);
						return 0;
					}
					break;
				}
				pFindData->m_pCurItm = pFindData->m_pCurDir->GetFirstItem(pFindData->m_pCurTyp);
			}
		}
		else
		{
			pFindData->m_pCurSubDir = pFindData->m_pCurDir->GetNextSubDir(pFindData->m_pCurSubDir);
			if (!pFindData->m_pCurSubDir)
			{
				df_FreeFindData(pInfo);
				return 0;
			}
		}
	}

	// Fill in the current entry.
	if (pFindData->m_pCurItm)
	{
		char sItemType[5];
		LTStrCpy(pInfo->m_Name, pFindData->m_pCurItm->GetName(), sizeof(pInfo->m_Name) - 4);
		pTree->m_pRezMgr->TypeToStr(pFindData->m_pCurItm->GetType(), sItemType);
		LTStrCat(pInfo->m_Name, ".", sizeof(pInfo->m_Name));
		LTStrCat(pInfo->m_Name, sItemType, sizeof(pInfo->m_Name));
		pInfo->m_Date = pFindData->m_pCurItm->GetTime();
		pInfo->m_Type = FILE_TYPE;
		pInfo->m_Size = pFindData->m_pCurItm->GetSize();
		return 1;
	}

	if (pFindData->m_pCurSubDir)
	{
		LTStrCpy(pInfo->m_Name, pFindData->m_pCurSubDir->GetDirName(), sizeof(pInfo->m_Name));
		pInfo->m_Date = pFindData->m_pCurSubDir->GetTime();
		pInfo->m_Type = DIRECTORY_TYPE;
		pInfo->m_Size = 0;
		return 1;
	}

	df_FreeFindData(pInfo);
	return 0;
}


void df_FindClose(LTFindInfo *pInfo)
{
	if (pInfo)
		df_FreeFindData(pInfo);
}


#define SAVEBUFSIZE 1024*16

// Save the contents of a steam to a file
// returns 1 if successful 0 if an error occured
int df_Save(ILTStream *hFile, const char* pName)
{
	return 0;
}


//----------------------------------------------------------------------------
// df_GetRawInfo -- where does this file physically live?
//
// Returns the on-disk filename plus the byte OFFSET and SIZE of the resource
// within it, so a caller can open and read it directly instead of going
// through the file-tree abstraction. Streaming sound is the only user: the
// sound driver needs a plain file handle it can seek in, not an ILTStream.
//
// This was the missing piece behind "lips move but there is no voice" --
// CSoundInstance::AcquireStream was #ifdef'd out on macOS because this
// function did not exist here, so every STREAMED sound (all NOLF2 dialogue)
// failed to open. Port of the Win32 de_file.cpp implementation; the macOS
// FileTree carries the same m_TreeType / m_pRezMgr pair, so it maps directly.
//----------------------------------------------------------------------------
int df_GetRawInfo(HLTFileTree *hTree, const char *pName, char* sFileName,
                  unsigned int nMaxFileName, uint32* nPos, uint32* nSize)
{
	FileTree *pTree = (FileTree*)hTree;
	if (!pTree || !pName || !sFileName || !nPos || !nSize)
		return 0;

	if (pTree->m_TreeType == UnixTree)
	{
		// Loose file on disk: the whole file, from offset 0. Note the '/'
		// separator -- the tree's base name plus a resource path that may well
		// have arrived with backslashes (the io layer normalizes on open).
		char fullName[500];
		LTSNPrintF(fullName, sizeof(fullName), "%s/%s", pTree->m_BaseName, pName);
		for (char *p = fullName; *p; ++p)
			if (*p == '\\') *p = '/';

		FILE *fp = fopen(fullName, "rb");
		if (!fp)
			return 0;
		fseek(fp, 0, SEEK_END);
		*nSize = (uint32)ftell(fp);
		fclose(fp);

		*nPos = 0;
		if (nMaxFileName <= strlen(fullName))
			return 0;
		LTStrCpy(sFileName, fullName, nMaxFileName);
		return 1;
	}

	// Inside a .rez archive: hand back the archive's path and the item's
	// offset/size within it.
	if (!pTree->m_pRezMgr)
		return 0;

	CRezItm* pRezItm = pTree->m_pRezMgr->GetRezFromDosPath(pName);
	if (!pRezItm)
		return 0;

	const char* pRezName = pRezItm->DirectRead_GetFullRezName();
	if (!pRezName)
		return 0;
	if (nMaxFileName <= strlen(pRezName))
		return 0;

	LTStrCpy(sFileName, pRezName, nMaxFileName);
	*nPos  = (uint32)pRezItm->DirectRead_GetFileOffset();
	*nSize = (uint32)pRezItm->GetSize();
	return 1;
}
