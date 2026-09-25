// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_texture.mm
//
// PURPOSE : See mtl_texture.h. DTX -> MTLTexture, mirroring gl_texture.cpp's
//           decode rules (which are already verified against the retail art):
//           BPP_32 = ARGB in a little-endian uint32, i.e. bytes B,G,R,A, which
//           IS MTLPixelFormatBGRA8Unorm; BPP_16 is expanded to BGRA8 on the
//           CPU; DXT1/3/5 map to BC1/BC2/BC3.
//
//           ⚠️ NO sRGB FORMATS. The GL path uploaded GL_RGBA8 (linear) and all
//           the authored colours, lightmaps and vertex lighting were tuned
//           against that. An sRGB view here would silently brighten the whole
//           game and every A/B against gl_reference/ would be meaningless.
//
// ----------------------------------------------------------------------- //

#import <Metal/Metal.h>

#include "bdefs.h"
#include "de_world.h"      // SharedTexture
#include "dtxmgr.h"        // TextureData / DtxHeader
#include "renderstruct.h"
#include "mtl_texture.h"
#include "mtl_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Lives on SharedTexture::m_pRenderData while bound. A NULL m_Tex marks a
// texture that failed to convert -- cached, so the draw path does not retry the
// load every frame.
//
// ⚠️ The Metal objects are held as CFTypeRef, not id: this struct is malloc'd
// C memory, which ARC will not trace. CFBridgingRetain/Release makes the
// ownership explicit and correct.
struct MTLTexEntry
{
	CFTypeRef m_Tex;          // id<MTLTexture>
	CFTypeRef m_Sampler;      // id<MTLSamplerState>
	uint32    m_nBytes;
	uint32    m_nWidth, m_nHeight;
	uint16    m_nAlphaRef;    // authored alpha-test reference, 0 = ALPHAREF_NONE
	uint8     m_bCubeMap;     // DTX_CUBEMAP -- decides the env-map transform
	// DTX_FULLBRITE. Only the DUAL-TEXTURE world shader reads it, and there it
	// selects the blend: a fullbrite slot-1 texture is ADDED to the base
	// (MODULATEALPHA_ADDCOLOR) instead of being cross-faded with it
	// (BLENDCURRENTALPHA) -- d3d_rendershader_gouraud.cpp's FlushChangeSection.
	uint8     m_bFullbrite;
	// Authored DETAIL-texture placement, read from THIS (base) texture's DTX
	// header. The detail texture itself carries none of it.
	float     m_fDetailScale;
	float     m_fDetailCos, m_fDetailSin;
};

static uint32 g_nTexCount = 0, g_nTexBytes = 0;

// --------------------------------------------------------------------------
// THE AUTHORITATIVE ALPHA-TEST RULE (identical to gl_texture.cpp's, and for the
// same reason): a texture is alpha-tested if and only if its DTX command string
// contains "AlphaRef <n>", and <n> IS the reference. Content-based guessing was
// removed from this renderer in §13 -- whether a surface is a cutout is an
// AUTHORING decision, not a property of its alpha histogram.
// --------------------------------------------------------------------------
static uint16 mtltex_ParseAlphaRef(const TextureData *pData)
{
	if (!pData)
		return 0;
	ConParse cParse;
	cParse.Init((char*)pData->m_Header.m_CommandString);
	if (cParse.ParseFind("AlphaRef", LTFALSE, 1) && cParse.m_nArgs >= 2)
	{
		int nRef = atoi(cParse.m_Args[1]);
		if (nRef < 0)   nRef = 0;
		if (nRef > 255) nRef = 255;
		return (uint16)nRef;
	}
	return 0;   // ALPHAREF_NONE -> no alpha test
}

static const char *mtltex_Name(SharedTexture *pTexture)
{
	if (g_pRenderStruct && g_pRenderStruct->GetTextureName)
	{
		const char *pName = g_pRenderStruct->GetTextureName(pTexture);
		if (pName)
			return pName;
	}
	return "<unknown>";
}

// --------------------------------------------------------------------------
// 16-bit -> BGRA8 expansion.
//
// Metal's packed 16-bit formats (ABGR4Unorm, BGR5A1Unorm, B5G6R5Unorm) are
// Apple-GPU-family only and their component order does not match the DTX masks
// anyway. Expanding on the CPU costs one small allocation per texture, once, at
// load -- and these are the UI font atlases and DTX_PREFER16BIT art, not the
// world. Correctness over a byte of bandwidth.
// Returns a malloc'd BGRA8 buffer the caller must free, or NULL.
// --------------------------------------------------------------------------
enum EMtl16Fmt { MTL16_NONE, MTL16_ARGB4444, MTL16_ARGB1555, MTL16_RGB565 };

static EMtl16Fmt mtltex_Classify16(const PFormat &cFmt)
{
	uint32 nA = cFmt.m_Masks[CP_ALPHA], nR = cFmt.m_Masks[CP_RED];
	uint32 nG = cFmt.m_Masks[CP_GREEN], nB = cFmt.m_Masks[CP_BLUE];

	if (nA == 0xF000 && nR == 0x0F00 && nG == 0x00F0 && nB == 0x000F)
		return MTL16_ARGB4444;
	if (nA == 0x8000 && nR == 0x7C00 && nG == 0x03E0 && nB == 0x001F)
		return MTL16_ARGB1555;
	if (nR == 0xF800 && nG == 0x07E0 && nB == 0x001F)
		return MTL16_RGB565;
	return MTL16_NONE;
}

static uint8 *mtltex_Expand16(const uint16 *pSrc, uint32 nPixels, EMtl16Fmt eFmt)
{
	uint8 *pOut = (uint8*)malloc((size_t)nPixels * 4);
	if (!pOut)
		return NULL;
	for (uint32 i = 0; i < nPixels; ++i)
	{
		uint32 v = pSrc[i];
		uint32 r, g, b, a;
		switch (eFmt)
		{
			case MTL16_ARGB4444:
				a = ((v >> 12) & 0xF) * 17; r = ((v >> 8) & 0xF) * 17;
				g = ((v >> 4)  & 0xF) * 17; b = ( v       & 0xF) * 17;
				break;
			case MTL16_ARGB1555:
				a = (v & 0x8000) ? 255 : 0;
				r = ((v >> 10) & 0x1F); r = (r << 3) | (r >> 2);
				g = ((v >> 5)  & 0x1F); g = (g << 3) | (g >> 2);
				b = ( v        & 0x1F); b = (b << 3) | (b >> 2);
				break;
			default:   // MTL16_RGB565
				a = 255;
				r = ((v >> 11) & 0x1F); r = (r << 3) | (r >> 2);
				g = ((v >> 5)  & 0x3F); g = (g << 2) | (g >> 4);
				b = ( v        & 0x1F); b = (b << 3) | (b >> 2);
				break;
		}
		pOut[i * 4 + 0] = (uint8)b;   // BGRA8Unorm byte order
		pOut[i * 4 + 1] = (uint8)g;
		pOut[i * 4 + 2] = (uint8)r;
		pOut[i * 4 + 3] = (uint8)a;
	}
	return pOut;
}

// --------------------------------------------------------------------------
// Sampler states. Two are enough for everything the DTX path produces: 2D art
// repeats, a cube map must clamp on all three axes or the seams sample across
// faces. Created once and shared -- a sampler is immutable state, not per
// texture.
// --------------------------------------------------------------------------
static id<MTLSamplerState> g_Sampler2D   = nil;
static id<MTLSamplerState> g_SamplerCube = nil;

static void mtltex_EnsureSamplers(id<MTLDevice> dev)
{
	if (g_Sampler2D)
		return;

	MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
	d.minFilter    = MTLSamplerMinMagFilterLinear;
	d.magFilter    = MTLSamplerMinMagFilterLinear;
	d.mipFilter    = MTLSamplerMipFilterLinear;
	d.sAddressMode = MTLSamplerAddressModeRepeat;
	d.tAddressMode = MTLSamplerAddressModeRepeat;
	g_Sampler2D = [dev newSamplerStateWithDescriptor:d];

	d.sAddressMode = MTLSamplerAddressModeClampToEdge;
	d.tAddressMode = MTLSamplerAddressModeClampToEdge;
	d.rAddressMode = MTLSamplerAddressModeClampToEdge;
	g_SamplerCube = [dev newSamplerStateWithDescriptor:d];
}

// --------------------------------------------------------------------------
// Creates the MTLTexture; returns nil on failure.
// --------------------------------------------------------------------------
static id<MTLTexture> mtltex_Create(SharedTexture *pTexture, uint32 &nOutBytes,
                                    uint32 &nOutW, uint32 &nOutH, bool &bOutCube)
{
	nOutBytes = 0; nOutW = 0; nOutH = 0; bOutCube = false;

	id<MTLDevice> dev = (__bridge id<MTLDevice>)MTLDev_Device();
	if (!dev || !g_pRenderStruct)
		return nil;

	// Pull the DTX through the engine (loads + caches it engine-side).
	TextureData *pData = g_pRenderStruct->GetTexture(pTexture);
	if (!pData)
	{
		fprintf(stderr, "[mtltex] load failed: %s\n", mtltex_Name(pTexture));
		return nil;
	}

	const BPPIdent eBPP  = pData->m_Header.GetBPPIdent();
	const uint32   nMips = pData->m_Header.m_nMipmaps;
	if (nMips == 0 || nMips > MAX_DTX_MIPMAPS)
		return nil;

	MTLPixelFormat eFormat   = MTLPixelFormatBGRA8Unorm;
	uint32         nBlockDim = 1;    // 4 for the BC formats
	uint32         nBlockSz  = 4;    // bytes per block (or per pixel when dim==1)
	EMtl16Fmt      e16       = MTL16_NONE;

	switch (eBPP)
	{
		case BPP_32:
			break;
		case BPP_16:
			e16 = mtltex_Classify16(pData->m_PFormat);
			if (e16 == MTL16_NONE)
			{
				const PFormat &c = pData->m_PFormat;
				fprintf(stderr, "[mtltex] unsupported 16-bit masks a=%X r=%X g=%X b=%X: %s\n",
				        c.m_Masks[CP_ALPHA], c.m_Masks[CP_RED],
				        c.m_Masks[CP_GREEN], c.m_Masks[CP_BLUE],
				        mtltex_Name(pTexture));
				return nil;
			}
			break;
		case BPP_S3TC_DXT1: eFormat = MTLPixelFormatBC1_RGBA; nBlockDim = 4; nBlockSz = 8;  break;
		case BPP_S3TC_DXT3: eFormat = MTLPixelFormatBC2_RGBA; nBlockDim = 4; nBlockSz = 16; break;
		case BPP_S3TC_DXT5: eFormat = MTLPixelFormatBC3_RGBA; nBlockDim = 4; nBlockSz = 16; break;
		default:
			fprintf(stderr, "[mtltex] unsupported DTX format %d: %s\n",
			        (int)eBPP, mtltex_Name(pTexture));
			return nil;
	}

	// ⚠️ BC (S3TC) support is NOT universal on Metal: it is a Mac-family
	// feature and absent on Apple-GPU iOS-family devices. Every Apple silicon
	// Mac has it, but check rather than assume -- and say so loudly, because
	// the symptom otherwise is "most of the art is untextured" with no clue.
	if (nBlockDim == 4 && !dev.supportsBCTextureCompression)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			fprintf(stderr, "[mtltex] device does not support BC texture compression"
			                " -- DXT textures will draw untextured\n");
		}
		return nil;
	}

	// ★ CUBE ENVIRONMENT MAPS (TexFX\Cubic\*). dtx_Create lays the six faces end
	// to end inside each mip level, m_dataSize apart, in D3DCUBEMAP_FACES order
	// (+X, -X, +Y, -Y, +Z, -Z) -- which is also Metal's slice order, so the
	// faces map across 1:1.
	const bool   bCube  = (pData->m_Header.m_IFlags & DTX_CUBEMAP) != 0;
	const uint32 nFaces = bCube ? 6 : 1;
	bOutCube = bCube;

	// Metal wants the mip count up front, so find the usable chain first. The
	// rules are gl_texture.cpp's: stop at the first missing/empty level, and for
	// a compressed texture also at the first level that is not block-aligned --
	// CalcImageSize stores DXT mips as w*h/2 (DXT1) or w*h bytes with no 4x4
	// rounding, so a non-aligned mip is truncated in the file and is not valid
	// block data.
	uint32 nUsable = 0;
	for (uint32 nMip = 0; nMip < nMips; ++nMip)
	{
		const TextureMipData &cMip = pData->m_Mips[nMip];
		if (!cMip.m_Data || cMip.m_Width == 0 || cMip.m_Height == 0)
			break;
		if (nBlockDim == 4 && ((cMip.m_Width & 3) || (cMip.m_Height & 3)))
			break;
		++nUsable;
	}
	if (getenv("LT_TRACE_MIPS"))
		fprintf(stderr, "[mips] MTL %-44s %ux%u mips=%u usable=%u\n",
		        mtltex_Name(pTexture),
		        pData->m_Mips[0].m_Width, pData->m_Mips[0].m_Height, nMips, nUsable);

	if (nUsable == 0)
	{
		fprintf(stderr, "[mtltex] no usable mip: %s (%ux%u fmt %d%s)\n",
		        mtltex_Name(pTexture), (uint32)pData->m_Header.m_BaseWidth,
		        (uint32)pData->m_Header.m_BaseHeight, (int)eBPP, bCube ? " CUBE" : "");
		return nil;
	}

	const uint32 nW0 = pData->m_Mips[0].m_Width;
	const uint32 nH0 = pData->m_Mips[0].m_Height;

	MTLTextureDescriptor *desc;
	if (bCube)
	{
		desc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:eFormat
		                                                             size:nW0
		                                                        mipmapped:(nUsable > 1)];
	}
	else
	{
		desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:eFormat
		                                                          width:nW0
		                                                         height:nH0
		                                                      mipmapped:(nUsable > 1)];
	}
	desc.mipmapLevelCount = nUsable;
	desc.usage            = MTLTextureUsageShaderRead;
	desc.storageMode      = MTLStorageModeShared;

	id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];
	if (!tex)
		return nil;

	uint32 nBytes = 0;
	for (uint32 nMip = 0; nMip < nUsable; ++nMip)
	{
		const TextureMipData &cMip = pData->m_Mips[nMip];
		const uint32 nBlocksW  = (cMip.m_Width  + nBlockDim - 1) / nBlockDim;
		const uint32 nBlocksH  = (cMip.m_Height + nBlockDim - 1) / nBlockDim;
		const uint32 nRowBytes = (nBlockDim == 4) ? nBlocksW * nBlockSz
		                                          : cMip.m_Width * 4;

		for (uint32 nFace = 0; nFace < nFaces; ++nFace)
		{
			const uint8 *pFace = (const uint8*)cMip.m_Data
			                   + (size_t)cMip.m_dataSize * nFace;
			uint8 *pTemp = NULL;
			if (e16 != MTL16_NONE)
			{
				pTemp = mtltex_Expand16((const uint16*)pFace,
				                        cMip.m_Width * cMip.m_Height, e16);
				if (!pTemp)
					continue;
				pFace = pTemp;
			}

			MTLRegion region = MTLRegionMake2D(0, 0, cMip.m_Width, cMip.m_Height);
			if (bCube)
			{
				[tex replaceRegion:region
				       mipmapLevel:nMip
				             slice:nFace
				         withBytes:pFace
				       bytesPerRow:nRowBytes
				     bytesPerImage:(NSUInteger)nRowBytes * nBlocksH];
			}
			else
			{
				[tex replaceRegion:region
				       mipmapLevel:nMip
				         withBytes:pFace
				       bytesPerRow:nRowBytes];
			}

			if (pTemp)
				free(pTemp);
		}
		nBytes += (uint32)cMip.m_dataSize * nFaces;
	}

	mtltex_EnsureSamplers(dev);

	++g_nTexCount;
	g_nTexBytes += nBytes;
	nOutBytes = nBytes;
	nOutW = nW0;
	nOutH = nH0;
	if (g_pRenderStruct)
		g_pRenderStruct->m_SystemTextureMemory += nBytes;
	// The same running tally the GL path prints. ⚠️ THIS IS A DIAGNOSTIC TELL,
	// not decoration: C01S01 must report 129 textures / 9.3 MB. Much less means
	// a mip-offset/detail console setting got degraded (§5), not a renderer bug.
	if ((g_nTexCount & 63) == 1)
		fprintf(stderr, "[mtltex] %u textures uploaded (%.1f MB)\n",
		        g_nTexCount, g_nTexBytes / (1024.0 * 1024.0));
	return tex;
}

// --------------------------------------------------------------------------
static void mtltex_ReleaseTex(MTLTexEntry *pEntry)
{
	if (!pEntry || !pEntry->m_Tex)
		return;
	CFBridgingRelease(pEntry->m_Tex);
	pEntry->m_Tex = NULL;
	if (g_nTexCount) --g_nTexCount;
	g_nTexBytes -= pEntry->m_nBytes;
	if (g_pRenderStruct)
		g_pRenderStruct->m_SystemTextureMemory -= pEntry->m_nBytes;
	pEntry->m_nBytes = 0;
}

void MTLTex_Bind(SharedTexture *pTexture, bool bTextureChanged)
{
	if (!pTexture)
		return;

	MTLTexEntry *pEntry = (MTLTexEntry*)pTexture->m_pRenderData;
	if (pEntry)
	{
		if (!bTextureChanged)
			return;
		mtltex_ReleaseTex(pEntry);
	}
	else
	{
		pEntry = new MTLTexEntry;
		memset(pEntry, 0, sizeof(*pEntry));
		pTexture->m_pRenderData = pEntry;
	}

	@autoreleasepool {
		bool bCube = false;
		id<MTLTexture> tex = mtltex_Create(pTexture, pEntry->m_nBytes,
		                                   pEntry->m_nWidth, pEntry->m_nHeight,
		                                   bCube);
		pEntry->m_Tex      = tex ? CFBridgingRetain(tex) : NULL;
		pEntry->m_bCubeMap = bCube ? 1 : 0;
		pEntry->m_Sampler  = (__bridge CFTypeRef)(bCube ? g_SamplerCube : g_Sampler2D);

		TextureData *pTD = g_pRenderStruct->GetTexture ? g_pRenderStruct->GetTexture(pTexture) : 0;
		pEntry->m_nAlphaRef = mtltex_ParseAlphaRef(pTD);
		pEntry->m_bFullbrite =
			(pTD && (pTD->m_Header.m_IFlags & DTX_FULLBRITE)) ? 1 : 0;

		// ⚠️ These live in the DtxHeader's `Extra` bytes, NOT in the command
		// string: GetDetailTextureScale() is `*(float*)&m_Extra[6] + 1.0f` and
		// GetDetailTextureAngle() is an int16 of DEGREES at m_Extra[10..11].
		// The +1.0f bias means an all-zero Extra block reads as scale 1.0.
		pEntry->m_fDetailScale = 1.0f;
		pEntry->m_fDetailCos   = 1.0f;
		pEntry->m_fDetailSin   = 0.0f;
		if (pTD)
		{
			pEntry->m_fDetailScale = pTD->m_Header.GetDetailTextureScale();
			float fAngle = MATH_DEGREES_TO_RADIANS((float)pTD->m_Header.GetDetailTextureAngle());
			pEntry->m_fDetailCos = cosf(fAngle);
			pEntry->m_fDetailSin = sinf(fAngle);
		}

		if (getenv("LT_TRACE_ALPHAREF"))
			fprintf(stderr, "[alpharef] %-44s ref=%u cmd='%s'\n",
			        mtltex_Name(pTexture), (unsigned)pEntry->m_nAlphaRef,
			        pTD ? pTD->m_Header.m_CommandString : "<no data>");
	}
}

void MTLTex_Unbind(SharedTexture *pTexture)
{
	if (!pTexture || !pTexture->m_pRenderData)
		return;
	MTLTexEntry *pEntry = (MTLTexEntry*)pTexture->m_pRenderData;
	mtltex_ReleaseTex(pEntry);
	// m_Sampler is a shared, unretained reference -- nothing to release.
	delete pEntry;
	pTexture->m_pRenderData = 0;
}

static MTLTexEntry *mtltex_Entry(SharedTexture *pTexture)
{
	if (!pTexture)
		return NULL;
	if (!pTexture->m_pRenderData)
		MTLTex_Bind(pTexture, false);   // lazy path (bound before renderer init)
	return (MTLTexEntry*)pTexture->m_pRenderData;
}

void *MTLTex_Get(SharedTexture *pTexture)
{
	MTLTexEntry *pEntry = mtltex_Entry(pTexture);
	return pEntry ? (void*)pEntry->m_Tex : NULL;
}

void *MTLTex_Sampler(SharedTexture *pTexture)
{
	MTLTexEntry *pEntry = mtltex_Entry(pTexture);
	if (!pEntry || !pEntry->m_Tex)
		return (__bridge void*)g_Sampler2D;
	return (void*)pEntry->m_Sampler;
}

bool MTLTex_GetDims(SharedTexture *pTexture, uint32 &nWidth, uint32 &nHeight)
{
	MTLTexEntry *pEntry = mtltex_Entry(pTexture);
	if (!pEntry || !pEntry->m_Tex)
		return false;
	nWidth  = pEntry->m_nWidth;
	nHeight = pEntry->m_nHeight;
	return nWidth > 0 && nHeight > 0;
}

unsigned int MTLTex_GetAlphaRef(SharedTexture *pTexture)
{
	if (!pTexture || !pTexture->m_pRenderData)
		return 0;
	return ((MTLTexEntry*)pTexture->m_pRenderData)->m_nAlphaRef;
}

bool MTLTex_IsCubeMap(SharedTexture *pTexture)
{
	MTLTexEntry *pEntry = mtltex_Entry(pTexture);
	return pEntry && pEntry->m_bCubeMap != 0;
}

bool MTLTex_IsFullbrite(SharedTexture *pTexture)
{
	MTLTexEntry *pEntry = mtltex_Entry(pTexture);
	return pEntry && pEntry->m_bFullbrite != 0;
}

bool MTLTex_GetDetailParams(SharedTexture *pTexture, float &fScale,
                            float &fCos, float &fSin)
{
	fScale = 1.0f; fCos = 1.0f; fSin = 0.0f;
	MTLTexEntry *pEntry = mtltex_Entry(pTexture);
	if (!pEntry)
		return false;
	fScale = pEntry->m_fDetailScale;
	fCos   = pEntry->m_fDetailCos;
	fSin   = pEntry->m_fDetailSin;
	return true;
}

const char *MTLTex_GetTexName(SharedTexture *pTexture)
{
	return mtltex_Name(pTexture);
}
