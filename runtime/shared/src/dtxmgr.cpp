
#include "bdefs.h"
#include "dtxmgr.h"
#include "render.h"

#ifdef __D3D
extern FormatMgr g_FormatMgr;
#endif

int g_dtxInMemSize = 0;

//Texture groups, used to control offsetting of Mip Maps when they are loaded, thus allowing
//us to not waste any memory loading mips higher than those that we are going to use. Note that
//this define needs to match the one in the engine vars code. This is ugly, but there is no
//shared header.
#define MAX_TEXTURE_GROUPS 4
extern int32	g_CV_TextureMipMapOffset;
extern int32	g_CV_TextureGroupOffset[MAX_TEXTURE_GROUPS];


// --------------------------------------------------------------------------------- //
// TextureMipData.
// --------------------------------------------------------------------------------- //

TextureMipData::TextureMipData()
{
	m_Width = 0;
	m_Height = 0;
	m_Pitch = 0;
	m_Data = LTNULL;
}


// --------------------------------------------------------------------------------- //
// TextureData functions.
// --------------------------------------------------------------------------------- //

TextureData::TextureData()
{
	m_pSharedTexture	= NULL;
	m_pDataBuffer		= NULL;
    m_bufSize			= 0;
}


TextureData::~TextureData()
{
	if(m_Header.m_IFlags & DTX_MIPSALLOCED)
	{
		for(uint32 i=0; i < m_Header.m_nMipmaps; i++)
		{
            if(m_Mips[i].m_Data) 
			{
                g_dtxInMemSize -= m_Mips[i].m_dataSize;
				dfree(m_Mips[i].m_Data);
            }
		}
	}

    if(m_pDataBuffer) 
	{
		delete[] m_pDataBuffer;
        g_dtxInMemSize -= m_bufSize;
        m_bufSize = 0;
    }
}


// --------------------------------------------------------------------------------- //
// dtx_ functions.
// --------------------------------------------------------------------------------- //

// Based on the value of bSkip, it either memset()s the memory to 0 or reads it from the file.
static void dtx_ReadOrSkip(
	LTBOOL bSkip,
	ILTStream *pStream,
	void *pData,
	uint32 dataLen)
{
	if(bSkip)
	{
		pStream->SeekTo(pStream->GetPos() + dataLen);
	}
	else
	{
		pStream->Read(pData, dataLen);
	}
}

TextureData* dtx_Alloc(BPPIdent bpp, uint32 baseWidth, uint32 baseHeight, uint32 nMipmaps,
	uint32 *pAllocSize, uint32 *pTextureDataSize, uint32 iFlags)
{
	TextureData *pRet;
	TextureMipData *pMip;
	uint32 i, size, width, height;
	uint32 textureDataSize;
	uint8 *pOutData;

	textureDataSize = 0;
	width = baseWidth;
	height = baseHeight;

	uint32 dataHeaderSize = 0;

	for (i=0; i < nMipmaps; i++) 
	{
		size = CalcImageSize(bpp, width, height) + dataHeaderSize;;
		if (iFlags & DTX_CUBEMAP) 
		{ 
			size *= 6; 
        }

		textureDataSize += size;
		width  >>= 1;
		height >>= 1; 
	}

	LT_MEM_TRACK_ALLOC(pRet = new TextureData,LT_MEM_TYPE_TEXTURE);
	if (!pRet) 
		return LTNULL;

	// ⚠️ TIE THE MRU LINK OFF AT BIRTH. LTLink's default constructor is
	// `force_inline LTLink() {}` -- it initialises NOTHING -- so a freshly
	// `new`ed TextureData carries a garbage m_Link until r_LoadSystemTexture
	// reaches its dl_AddHead. r_UnloadSystemTexture then calls dl_RemoveAt
	// unconditionally, and CheapLTLink::Remove is `m_pPrev->m_pNext = m_pNext`
	// with no NULL check (ltlink.h): on a zeroed heap page that writes through
	// NULL+8 and gives SIGSEGV at 0x8 -- the reported fault, exactly.
	//
	// A TIED-OFF LINK REMOVES ITSELF HARMLESSLY (TieOff points both ends at the
	// link itself), so this makes the unload safe on every path, including any
	// that frees a texture that was never added to g_SysCache.m_List. Same bug
	// and same remedy as the sprite crash in §41, which used Init2 for the same
	// reason: `dl_Remove` on a link that was never inserted.
	pRet->m_Link.Init2(pRet);

	LT_MEM_TRACK_ALLOC(pRet->m_pDataBuffer = new uint8[textureDataSize],LT_MEM_TYPE_TEXTURE);
	if (!pRet->m_pDataBuffer) 
	{
		delete pRet; 
		return LTNULL; 
	}

    pRet->m_bufSize = textureDataSize;
    g_dtxInMemSize += textureDataSize;

	pRet->m_ResHeader.m_Type = LT_RESTYPE_DTX;
	pRet->m_Header.m_ResType = LT_RESTYPE_DTX;
	pRet->m_Header.m_IFlags = DTX_SECTIONSFIXED;
	pRet->m_Header.m_UserFlags = 0;
	pRet->m_Header.m_nMipmaps = (uint16)nMipmaps;
	pRet->m_Header.m_BaseWidth = (uint16)baseWidth;
	pRet->m_Header.m_BaseHeight = (uint16)baseHeight;
	pRet->m_Header.m_Version = CURRENT_DTX_VERSION;
	pRet->m_Header.m_CommandString[0] = '\0';

	pRet->m_AllocSize = sizeof(TextureData) + textureDataSize;
	pRet->m_Link.m_pData = pRet;
	pRet->m_Header.m_nSections = 0;
	pRet->m_Header.m_ExtraLong[0] = pRet->m_Header.m_ExtraLong[1] = pRet->m_Header.m_ExtraLong[2] = 0;
	pRet->m_Header.SetBPPIdent(bpp);	//! moved here by jyl, this line used to be located
										//  before the line that set's m_ExtraLong[...] to 0
										//  which nulls out the call to SetBPPIdent.
	pRet->m_pSharedTexture = LTNULL;
	pRet->m_Flags = 0;

	// Setup the mipmap structures.
	pOutData = pRet->m_pDataBuffer;

	width = baseWidth;
	height = baseHeight;
	for (i=0; i < nMipmaps; i++) 
	{
		pMip = &pRet->m_Mips[i];

		pMip->m_Width = width;
		pMip->m_Height = height;
		pMip->m_DataHeader = pOutData;
		pMip->m_Data = pOutData + dataHeaderSize;
		pMip->m_dataSize = CalcImageSize(bpp, width, height);

		switch( bpp ) {
		case BPP_32:
		  pMip->m_Pitch = (int32)width * sizeof(uint32);
		  break;
		case BPP_32P:
		  pMip->m_Pitch = width;	//  BPP_32P: texture w/ true color pallete
		  break;
		case BPP_16:
		  pMip->m_Pitch = (int32)width * sizeof(uint16);
		  break;
		default:
		  pMip->m_Pitch = 0;
		  break;
		}

		size = pMip->m_dataSize + dataHeaderSize;
		if (iFlags & DTX_CUBEMAP) 
		{ 
			size *= 6; 
		}

		pOutData += size;

		width >>= 1; height >>= 1; 
	}

	if (pAllocSize) 
		*pAllocSize = pRet->m_AllocSize;

	if (pTextureDataSize) 
		*pTextureDataSize = textureDataSize;

	return pRet;
}

LTRESULT dtx_Create(ILTStream *pStream, TextureData **ppOut, uint32& nBaseWidth, uint32& nBaseHeight)
{
	TextureData *pRet;
	uint32 allocSize, textureDataSize;
	

	DtxHeader hdr;
	STREAM_READ(hdr);
	if (hdr.m_ResType != LT_RESTYPE_DTX) 
		RETURN_ERROR(1, dtx_Create, LT_INVALIDDATA);

	// Correct version and valid data?
	if (hdr.m_Version != CURRENT_DTX_VERSION) 
		RETURN_ERROR(1, dtx_Create, LT_INVALIDVERSION);

	if (hdr.m_nMipmaps == 0 || hdr.m_nMipmaps > MAX_DTX_MIPMAPS) 
		RETURN_ERROR(1, dtx_Create, LT_INVALIDDATA);

	
	//figure out the dimensions that we are going to want our texture to start out at
	uint32 nMipOffset = (uint32)LTMAX(0, g_CV_TextureMipMapOffset);
	if(hdr.GetTextureGroup() < MAX_TEXTURE_GROUPS)
	{
		nMipOffset = (uint32)LTMAX(0, g_CV_TextureMipMapOffset + g_CV_TextureGroupOffset[hdr.GetTextureGroup()]);
	}

	//clamp our mipmap offset to the number of mipmaps that we actually have
	nMipOffset = LTMIN(hdr.m_nMipmaps - 1, nMipOffset);

	nBaseWidth  = hdr.m_BaseWidth;
	nBaseHeight = hdr.m_BaseHeight;

	//now we need to determine our dimensions
	uint32 nTexWidth  = hdr.m_BaseWidth  / (1 << nMipOffset);
	uint32 nTexHeight = hdr.m_BaseHeight / (1 << nMipOffset);
	uint32 nNumMips	  = hdr.m_nMipmaps - nMipOffset;

	// For D3D I'm supporting converts to DD texture formats (the sys mem copy will be in DD format - so it's fast copy & minimal mem storage).
#ifdef __D3D

	PFormat DstFormat;

	BPPIdent TexFormat = hdr.GetBPPIdent();

	bool bConvertToDDFormat = true;

	// Need to get the DD format (we're going to store it in memory in the DD format)...
	if(!r_GetRenderStruct()->GetTextureDDFormat2(hdr.GetBPPIdent(),hdr.m_IFlags,&DstFormat))
		RETURN_ERROR(1, dtx_Create, LT_INVALIDFILE); 

	// If they're the same, just blip on out...
	BPPIdent SrcBppIdent = hdr.GetBPPIdent();
	if (DstFormat.GetType() == SrcBppIdent) 
		bConvertToDDFormat = false;
	if (hdr.m_IFlags & DTX_NOSYSCACHE) 
		bConvertToDDFormat = false;
	if (hdr.m_IFlags & DTX_32BITSYSCOPY) 
		bConvertToDDFormat = false;

	//determine what format we are going to be using
	if(bConvertToDDFormat)
		TexFormat = DstFormat.GetType();

	pRet = dtx_Alloc(TexFormat, nTexWidth, nTexHeight, nNumMips, &allocSize, &textureDataSize,hdr.m_IFlags);
	if (!pRet) 
		RETURN_ERROR(1, dtx_Create, LT_OUTOFMEMORY); 


	if (bConvertToDDFormat) 
	{ 
		pRet->m_PFormat = DstFormat; 
	}
	else 
	{ 
		dtx_SetupDTXFormat2(pRet->m_Header.GetBPPIdent(), &pRet->m_PFormat); 
	}

	pRet->m_pSharedTexture = LTNULL;
	uint8 iBppBefore = pRet->m_Header.m_Extra[2];
	memcpy(&pRet->m_Header, &hdr, sizeof(DtxHeader));
	if (bConvertToDDFormat) 
		pRet->m_Header.m_Extra[2] = iBppBefore;	// Restore the Bpps (because we might have asked for a different one)...

	//restore the width, height, and mipmaps
	pRet->m_Header.m_BaseWidth  = nTexWidth;
	pRet->m_Header.m_BaseHeight = nTexHeight;
	pRet->m_Header.m_nMipmaps	= nNumMips;

	// Alloc a tmp buffer to read in the source image...
	uint8* pTmpBuffer = NULL; 
	TextureMipData *pMip = &pRet->m_Mips[0];

	if (bConvertToDDFormat) 
	{
		uint32 size = CalcImageSize(hdr.GetBPPIdent(), pMip->m_Width, pMip->m_Height);
		LT_MEM_TRACK_ALLOC(pTmpBuffer = new uint8[size],LT_MEM_TYPE_TEXTURE);
		
		if (!pTmpBuffer) 
			return false; 
	}

	// Read in mipmap data....
	uint32 iTexturesInRow = 1;
	if (hdr.m_IFlags & DTX_CUBEMAP) 
		iTexturesInRow = 6;

	for (uint32 iTex = 0; iTex < iTexturesInRow; ++iTex) 
	{
		uint32 nWidth  = hdr.m_BaseWidth;
		uint32 nHeight = hdr.m_BaseHeight;

		for (uint32 iMipmap=0; iMipmap < hdr.m_nMipmaps; iMipmap++) 
		{
			//determine whether or not we want to read in this mip map or not
			bool bSkipImageData = true;

			uint8* pTmpBufferPtr = pTmpBuffer;
			TextureMipData *pMip = NULL;
			uint8* pMipData		 = NULL;

			//see if we want to actually read in this mipmap
			if(iMipmap >= nMipOffset)
			{
				pMip			= &pRet->m_Mips[iMipmap - nMipOffset];
				pMipData		= pMip->m_Data;
				bSkipImageData	= false;
			}

			pMipData += CalcImageSize(TexFormat, nWidth, nHeight) * iTex; 

			uint32 size = CalcImageSize(hdr.GetBPPIdent(), nWidth, nHeight);
			if (bConvertToDDFormat) 
			{
				dtx_ReadOrSkip(bSkipImageData, pStream, pTmpBuffer, size); 
			}
			else 
			{
				dtx_ReadOrSkip(bSkipImageData, pStream, pMipData, size); 
			}

			if (bConvertToDDFormat && !bSkipImageData) 
			{
				// Convert the mip to DD format...
				PFormat SrcFormat; 
				if (hdr.GetBPPIdent() == BPP_32) 
				{ 
					SrcFormat.Init(hdr.GetBPPIdent(),0xFF000000,0x00FF0000,0x0000FF00,0x000000FF); 
				}
				else 
				{ 
					SrcFormat.Init(hdr.GetBPPIdent(),0x0,0x0,0x0,0x0); 
				}

				r_GetRenderStruct()->ConvertTexDataToDD(pTmpBuffer, &SrcFormat, nWidth, nHeight,
														pMipData,&DstFormat,hdr.GetBPPIdent(), hdr.m_IFlags, nWidth, nHeight);

				pTmpBufferPtr = pTmpBuffer; 
			} 

			nWidth /= 2;
			nHeight /= 2;
		} 
		if ((hdr.m_IFlags & DTX_CUBEMAP) && (iTex==0)) 
		{
			DtxSection DtxSectHeader;
			pStream->Read(&DtxSectHeader, sizeof(SectionHeader)); 
		} 
	} 

	if (pTmpBuffer) 
	{ 
		delete[] pTmpBuffer; 
		pTmpBuffer = NULL; 
	}
#else

	// (declared in the #if branch above; this branch needs its own copies)
	TextureMipData *pMip;
	uint32 y;
	uint32 size;

	// ★ CUBE MAPS. `iFlags` was omitted here, so a DTX_CUBEMAP texture was
	// allocated (and read) as a SINGLE face -- the +X one -- and the five faces
	// stored after it were left in the stream. Every reflective world surface in
	// the game points at one of these (TexFX\Cubic\*), so world env mapping
	// cannot work without it. dtx_Alloc sizes each mip level 6x when told.
	pRet = dtx_Alloc(hdr.GetBPPIdent(), nTexWidth, nTexHeight, nNumMips, &allocSize, &textureDataSize, hdr.m_IFlags);
	if (!pRet) RETURN_ERROR(1, dtx_Create, LT_OUTOFMEMORY);

	dtx_SetupDTXFormat2(pRet->m_Header.GetBPPIdent(), &pRet->m_PFormat);

	pRet->m_pSharedTexture = LTNULL;
	memcpy(&pRet->m_Header, &hdr, sizeof(DtxHeader));

	// The on-disk layout, VERIFIED byte-exact against five retail cube DTXs
	// (GLUE1 64x64 DXT1, GLOBAL01 128x128 BPP_32, UW_LAB1 / UW_SUBBAY 512x512
	// DXT1, FALLOFF1 64x64 BPP_32) -- in every one the residual after
	// 6 x <face mip chain> was exactly 32 bytes:
	//
	//     DtxHeader(164) | face0 mip chain | SectionHeader(32) | faces 1..5
	//
	// i.e. ONE section header total, not one per face, which is what
	// UploadRTexture's `iTex==0` guard (dtxmgr.cpp:364) already implied.
	// Face order is D3DCUBEMAP_FACES: +X, -X, +Y, -Y, +Z, -Z.
	const bool bCubeMap    = (hdr.m_IFlags & DTX_CUBEMAP) != 0;
	const uint32 nNumFaces = bCubeMap ? 6 : 1;

	for (uint32 iFace = 0; iFace < nNumFaces; ++iFace)
	{
		uint32 nSrcWidth  = hdr.m_BaseWidth;
		uint32 nSrcHeight = hdr.m_BaseHeight;

		for (uint32 iMipmap=0; iMipmap < hdr.m_nMipmaps; iMipmap++)
		{
			// ⚠️ The mip levels BELOW nMipOffset are present in the file but have
			// no slot in pRet (which was allocated with nNumMips), so they must be
			// consumed and dropped. This loop used to index m_Mips[iMipmap]
			// unconditionally while allocating only nNumMips levels -- a heap
			// overrun the moment a config set GroupOffset non-zero, which is
			// exactly what §5's autoexec.cfg corruption did.
			const bool bWantMip = (iMipmap >= nMipOffset);
			pMip = bWantMip ? &pRet->m_Mips[iMipmap - nMipOffset] : NULL;

			// Each face occupies m_dataSize bytes at this level, laid end to end.
			uint8 *pFaceData = pMip ? (pMip->m_Data + pMip->m_dataSize * iFace) : NULL;

			if (hdr.GetBPPIdent() == BPP_32)
			{
				for (y=0; y < nSrcHeight; y++)
				{
					dtx_ReadOrSkip(!pMip, pStream,
						pFaceData ? &pFaceData[y*pMip->m_Pitch] : NULL,
						nSrcWidth * sizeof(uint32));
				}
			}
			else
			{
				size = CalcImageSize(hdr.GetBPPIdent(), nSrcWidth, nSrcHeight);
				dtx_ReadOrSkip(!pMip, pStream, pFaceData, size);
			}

			nSrcWidth  >>= 1;
			nSrcHeight >>= 1;
		}

		// The lone section header, between face 0 and face 1.
		if (bCubeMap && iFace == 0)
		{
			SectionHeader cIgnored;
			pStream->Read(&cIgnored, sizeof(SectionHeader));
		}
	}
#endif
	
	//don't bother loading in the sections
	pRet->m_Header.m_nSections = 0;

	// Check the error status.
	if (pStream->ErrorStatus() != LT_OK) 
	{
		dtx_Destroy(pRet);
		RETURN_ERROR(1, dtx_Create, LT_INVALIDDATA); 
	}

	*ppOut = pRet;
	return LT_OK;
}


void dtx_Destroy(TextureData *pTextureData)
{
	if(pTextureData)
	{
		delete pTextureData;
	}
}

// Warning: This isn't very reliable - assumes either 32 bit or compressed...
void dtx_SetupDTXFormat2(BPPIdent bpp, PFormat *pFormat)
{
	if (bpp == BPP_32) 
	{
		pFormat->InitPValueFormat(); 
	}
	else 
	{
		pFormat->Init(bpp, 0, 0, 0, 0); 
	}
}

