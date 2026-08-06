// ----------------------------------------------------------------------- //
//
// MODULE  : gl_texture.cpp
//
// PURPOSE : DTX -> GL texture upload. The engine's dtx_Create (non-__D3D
//           path) delivers TextureData mips in the file's native format:
//           BPP_32 = ARGB in uint32 (little-endian bytes B,G,R,A) or
//           BPP_S3TC_DXT1/3/5 = raw block data. 32-bit uploads via
//           GL_BGRA + UNSIGNED_INT_8_8_8_8_REV; DXT via
//           glCompressedTexImage2D (GL_EXT_texture_compression_s3tc).
//
// ----------------------------------------------------------------------- //

#include "bdefs.h"
#include "de_world.h"      // SharedTexture
#include "dtxmgr.h"        // TextureData / DtxHeader
#include "renderstruct.h"
#include "gl_texture.h"

#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <string.h>

RenderStruct *g_pGLStruct = 0;

// Lives on SharedTexture::m_pRenderData while bound. m_nName == 0 marks a
// texture that failed to convert (cached so the draw path doesn't retry the
// load every frame; AddSharedTexture also stops re-binding it).
struct GLTexEntry
{
	GLuint m_nName;
	uint32 m_nBytes;
	uint8  m_nAlphaClass;   // GLTEX_ALPHA_* — how this texture's alpha must be used
	uint16 m_nAlphaRef;     // authored alpha-test reference, 0 = ALPHAREF_NONE
	uint8  m_bCubeMap;      // DTX_CUBEMAP — decides the env-map texture transform
	// Authored DETAIL-texture placement, read from THIS (base) texture's DTX
	// header. The detail texture itself carries none of it: the surface decides
	// how its own detail layer is scaled and rotated (d3d_texture.cpp:495).
	float  m_fDetailScale;
	float  m_fDetailCos, m_fDetailSin;
};

// THE AUTHORITATIVE ALPHA-TEST RULE, straight from the D3D renderer.
//
// A texture is alpha-tested if and only if its DTX command string contains
// "AlphaRef <n>", and <n> IS the reference value. d3d_texture.cpp parses exactly
// this into CRenderTexture::m_AlphaRef (ALPHAREF_NONE = 0 when absent), and
// every render shader then does
//     bAlphaTest = (pRenderTexture->m_AlphaRef != ALPHAREF_NONE)
// with D3DRS_ALPHAREF set to the value.
//
// This replaces guessing from pixel content, which could never work: whether a
// surface is a cutout is an AUTHORING decision, not a property of its alpha
// histogram. Testing every textured surface at a hardcoded 0.5 is what made the
// rails, lamp poles and shoji screens vanish (§12/§13).
static uint16 gltex_ParseAlphaRef(const TextureData *pData)
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

// Classify a texture's alpha channel at upload time. This decides how the world
// draw treats the surface, and getting it wrong makes geometry VANISH: a flat
// 0.5 alpha-test on every textured surface discards EVERY texel of a uniformly
// semi-transparent texture (shoji paper screens, lamp-post glass, railings),
// which looked exactly like missing geometry.
// --- DXT alpha classification -------------------------------------------
// Most NOLF2 world textures are DXT-compressed, so the alpha channel cannot be
// read without decoding it -- and without a class they fall back to "alpha test
// everything", which is what makes railings, lamp poles and shoji screens
// disappear (§12). Only the ALPHA part of each block is decoded; the colour
// half is irrelevant here.
//
// Counts are accumulated as (clear, opaque, total) so the same hole-based rule
// used for uncompressed textures decides the class.
struct GLTexAlphaStats { uint32 m_nClear, m_nOpaque, m_nPass, m_nTotal; };

// DXT1 (BC1): 8 bytes/block. Alpha exists ONLY when color0 <= color1, in which
// case index 3 means "transparent black" -- 1-bit punch-through.
static void gltex_ScanDXT1Alpha(const uint8 *pData, uint32 nBlocks, GLTexAlphaStats &cS)
{
	for (uint32 b = 0; b < nBlocks; ++b, pData += 8)
	{
		uint16 nC0 = (uint16)(pData[0] | (pData[1] << 8));
		uint16 nC1 = (uint16)(pData[2] | (pData[3] << 8));
		cS.m_nTotal += 16;
		if (nC0 > nC1)
		{
			cS.m_nOpaque += 16;      // opaque block, no alpha at all
			cS.m_nPass   += 16;
			continue;
		}
		for (uint32 t = 0; t < 16; ++t)
		{
			uint32 nIdx = (pData[4 + (t >> 2)] >> ((t & 3) * 2)) & 3;
			if (nIdx == 3) ++cS.m_nClear; else { ++cS.m_nOpaque; ++cS.m_nPass; }
		}
	}
}

// DXT3 (BC2): 16 bytes/block, first 8 = explicit 4-bit alpha per texel.
static void gltex_ScanDXT3Alpha(const uint8 *pData, uint32 nBlocks, GLTexAlphaStats &cS)
{
	for (uint32 b = 0; b < nBlocks; ++b, pData += 16)
	{
		for (uint32 t = 0; t < 16; ++t)
		{
			uint32 nNib = (pData[t >> 1] >> ((t & 1) * 4)) & 0xF;
			uint32 nA = nNib * 17;   // 0..15 -> 0..255
			++cS.m_nTotal;
			if (nA <= 16)       ++cS.m_nClear;
			else if (nA >= 250) ++cS.m_nOpaque;
			if (nA >= 128)      ++cS.m_nPass;   // would survive the 0.5 test
		}
	}
}

// DXT5 (BC3): 16 bytes/block, first 8 = two alpha endpoints + 16 3-bit indices.
static void gltex_ScanDXT5Alpha(const uint8 *pData, uint32 nBlocks, GLTexAlphaStats &cS)
{
	for (uint32 b = 0; b < nBlocks; ++b, pData += 16)
	{
		uint32 a0 = pData[0], a1 = pData[1];
		uint32 aPalette[8];
		aPalette[0] = a0;
		aPalette[1] = a1;
		if (a0 > a1)
		{
			for (uint32 i = 0; i < 6; ++i)
				aPalette[2 + i] = ((6 - i) * a0 + (1 + i) * a1) / 7;
		}
		else
		{
			for (uint32 i = 0; i < 4; ++i)
				aPalette[2 + i] = ((4 - i) * a0 + (1 + i) * a1) / 5;
			aPalette[6] = 0;
			aPalette[7] = 255;
		}

		// 16 three-bit indices packed into 6 bytes (little-endian bit stream).
		uint64 nBits = 0;
		for (uint32 i = 0; i < 6; ++i)
			nBits |= ((uint64)pData[2 + i]) << (8 * i);

		for (uint32 t = 0; t < 16; ++t)
		{
			uint32 nA = (uint32)aPalette[(nBits >> (3 * t)) & 7];
			++cS.m_nTotal;
			if (nA <= 16)       ++cS.m_nClear;
			else if (nA >= 250) ++cS.m_nOpaque;
			if (nA >= 128)      ++cS.m_nPass;   // would survive the 0.5 test
		}
	}
}

static const char *gltex_ClassName(uint8 n)
{
	return (n == GLTEX_ALPHA_OPAQUE) ? "OPAQUE"
	     : (n == GLTEX_ALPHA_MASKED) ? "MASKED" : "TRANSLUCENT";
}

static uint8 gltex_ClassifyFromStats(const GLTexAlphaStats &cS)
{
	if (!cS.m_nTotal)
		return GLTEX_ALPHA_MASKED;
	if (cS.m_nOpaque == cS.m_nTotal)
		return GLTEX_ALPHA_OPAQUE;

	// Ask the question that actually matters: would a 0.5 alpha TEST destroy
	// this surface? A texture can have BOTH holes and large partial-alpha areas
	// (a paper screen inside a cut-out frame), and testing it erases the partial
	// part -- which is exactly how the shoji screens disappeared.
	// Be CONSERVATIVE: only call something translucent when the 0.5 test would
	// destroy most of it. Foliage has plenty of solid texels and must stay a
	// cutout -- blending leaves instead smears them over everything behind.
	// A uniformly semi-transparent surface (shoji paper, glass) has almost
	// nothing above 0.5 and is the case the test genuinely breaks.
	if (cS.m_nPass * 4 < cS.m_nTotal)
		return GLTEX_ALPHA_TRANSLUCENT;

	return GLTEX_ALPHA_MASKED;
}

static uint8 gltex_ClassifyAlpha(const uint8 *pBGRA, uint32 nPixels)
{
	if (!pBGRA || !nPixels)
		return GLTEX_ALPHA_MASKED;

	uint32 nOpaque = 0, nClear = 0, nPass = 0;
	for (uint32 i = 0; i < nPixels; ++i)
	{
		uint8 a = pBGRA[i * 4 + 3];          // BGRA byte order
		if (a >= 250)     ++nOpaque;
		else if (a <= 16) ++nClear;
		if (a >= 128)     ++nPass;
	}

	GLTexAlphaStats cS;
	cS.m_nClear = nClear; cS.m_nOpaque = nOpaque; cS.m_nPass = nPass; cS.m_nTotal = nPixels;
	return gltex_ClassifyFromStats(cS);
}

static uint32 g_nTexCount = 0, g_nTexBytes = 0;   // upload stats (logged once in a while)

static bool gltex_S3TCSupported()
{
	static int s_nSupported = -1;
	if (s_nSupported < 0)
	{
		const char *pExt = (const char*)glGetString(GL_EXTENSIONS);
		s_nSupported = (pExt && strstr(pExt, "GL_EXT_texture_compression_s3tc")) ? 1 : 0;
		if (!s_nSupported)
			fprintf(stderr, "[gltex] GL_EXT_texture_compression_s3tc NOT available — DXT textures will draw untextured\n");
	}
	return s_nSupported == 1;
}

static const char *gltex_Name(SharedTexture *pTexture)
{
	if (g_pGLStruct && g_pGLStruct->GetTextureName)
	{
		const char *pName = g_pGLStruct->GetTextureName(pTexture);
		if (pName)
			return pName;
	}
	return "<unknown>";
}

// Creates the GL texture object; returns 0 on failure.
static GLuint gltex_Create(SharedTexture *pTexture, uint32 &nOutBytes, uint8 &nAlphaClass)
{
	nOutBytes = 0;
	if (!g_pGLStruct)
		return 0;

	// Pull the DTX through the engine (loads + caches it engine-side).
	TextureData *pData = g_pGLStruct->GetTexture(pTexture);
	if (!pData)
	{
		fprintf(stderr, "[gltex] load failed: %s\n", gltex_Name(pTexture));
		return 0;
	}

	BPPIdent eBPP  = pData->m_Header.GetBPPIdent();
	uint32   nMips = pData->m_Header.m_nMipmaps;
	if (nMips == 0 || nMips > MAX_DTX_MIPMAPS)
		return 0;

	// Uncompressed uploads default to the 32-bit ARGB path; 16-bit formats pick
	// a packed type from the PFormat masks below.
	GLenum eCompressed = 0;
	// Default to MASKED = the behaviour before alpha classification existed
	// (alpha-test every textured surface). Only formats we can actually inspect
	// get reclassified. DXT-compressed textures -- which is most of the foliage
	// -- cannot be sampled here without decoding, and defaulting THOSE to
	// OPAQUE silently removed the cutout and turned every tree into a solid
	// grey quad. Unknown must fall back to the old behaviour, never to "opaque".
	nAlphaClass = GLTEX_ALPHA_MASKED;
	GLenum eFormat = GL_BGRA, eType = GL_UNSIGNED_INT_8_8_8_8_REV;
	switch (eBPP)
	{
		case BPP_32:        break;
		case BPP_16:
		{
			// 16bpp comes from ILTTexInterface::CreateTextureFromData (the UI
			// font atlases are ARGB4444) as well as DTX_PREFER16BIT art.
			// Component order is decided by the mask layout; the _REV packed
			// types put the FIRST component in the LOW bits, which is what a
			// little-endian uint16 of 0xARGB gives us with GL_BGRA.
			const PFormat &cFmt = pData->m_PFormat;
			uint32 nA = cFmt.m_Masks[CP_ALPHA], nR = cFmt.m_Masks[CP_RED];
			uint32 nG = cFmt.m_Masks[CP_GREEN], nB = cFmt.m_Masks[CP_BLUE];

			if (nA == 0xF000 && nR == 0x0F00 && nG == 0x00F0 && nB == 0x000F)
			{
				eFormat = GL_BGRA; eType = GL_UNSIGNED_SHORT_4_4_4_4_REV;
			}
			else if (nA == 0x8000 && nR == 0x7C00 && nG == 0x03E0 && nB == 0x001F)
			{
				eFormat = GL_BGRA; eType = GL_UNSIGNED_SHORT_1_5_5_5_REV;
			}
			else if (nR == 0xF800 && nG == 0x07E0 && nB == 0x001F)
			{
				eFormat = GL_RGB;  eType = GL_UNSIGNED_SHORT_5_6_5;
			}
			else
			{
				fprintf(stderr, "[gltex] unsupported 16-bit masks a=%X r=%X g=%X b=%X: %s\n",
				        nA, nR, nG, nB, gltex_Name(pTexture));
				return 0;
			}
			break;
		}
		case BPP_S3TC_DXT1: eCompressed = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
		case BPP_S3TC_DXT3: eCompressed = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
		case BPP_S3TC_DXT5: eCompressed = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
		default:
			fprintf(stderr, "[gltex] unsupported DTX format %d: %s\n",
			        (int)eBPP, gltex_Name(pTexture));
			return 0;
	}
	if (eCompressed && !gltex_S3TCSupported())
		return 0;

	while (glGetError() != GL_NO_ERROR) {}   // drain stale errors

	// ★ CUBE ENVIRONMENT MAPS (TexFX\Cubic\*). dtx_Create lays the six faces end
	// to end inside each mip level, m_dataSize apart, in D3DCUBEMAP_FACES order
	// (+X, -X, +Y, -Y, +Z, -Z) — which is also GL's
	// GL_TEXTURE_CUBE_MAP_POSITIVE_X + n order, so the faces map across 1:1.
	const bool   bCube    = (pData->m_Header.m_IFlags & DTX_CUBEMAP) != 0;
	const GLenum eTarget  = bCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
	const uint32 nFaces   = bCube ? 6 : 1;

	GLuint nName = 0;
	glGenTextures(1, &nName);
	if (!nName)
		return 0;
	glBindTexture(eTarget, nName);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	int nLastLevel = -1;
	uint32 nBytes = 0;
	for (uint32 nMip = 0; nMip < nMips; ++nMip)
	{
		const TextureMipData &cMip = pData->m_Mips[nMip];
		if (!cMip.m_Data || cMip.m_Width == 0 || cMip.m_Height == 0)
			break;

		bool bMipFailed = false;
		for (uint32 nFace = 0; nFace < nFaces && !bMipFailed; ++nFace)
		{
			const GLenum eFaceTarget = bCube
				? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + nFace) : GL_TEXTURE_2D;
			const uint8 *pFace = (const uint8*)cMip.m_Data + (size_t)cMip.m_dataSize * nFace;

		if (eCompressed)
		{
			// CalcImageSize stores DXT mips as w*h/2 (DXT1) or w*h bytes with
			// no 4x4-block rounding, so mips not block-aligned are truncated
			// in the file — not valid DXT data. Stop the chain there.
			if ((cMip.m_Width & 3) || (cMip.m_Height & 3))
			{
				bMipFailed = true;
				break;
			}
			glCompressedTexImage2D(eFaceTarget, nMip, eCompressed,
			                       cMip.m_Width, cMip.m_Height, 0,
			                       cMip.m_dataSize, pFace);
			// Classify alpha from the top mip's compressed blocks (face 0 only).
			if (nMip == 0 && nFace == 0)
			{
				GLTexAlphaStats cStats = { 0, 0, 0, 0 };
				uint32 nBlocks = ((cMip.m_Width + 3) / 4) * ((cMip.m_Height + 3) / 4);
				if (eBPP == BPP_S3TC_DXT1)
					gltex_ScanDXT1Alpha((const uint8*)cMip.m_Data, nBlocks, cStats);
				else if (eBPP == BPP_S3TC_DXT3)
					gltex_ScanDXT3Alpha((const uint8*)cMip.m_Data, nBlocks, cStats);
				else
					gltex_ScanDXT5Alpha((const uint8*)cMip.m_Data, nBlocks, cStats);
				nAlphaClass = gltex_ClassifyFromStats(cStats);
				if (getenv("LT_TRACE_ALPHACLASS"))
					fprintf(stderr, "[alpha] %-44s DXT%d %s clear=%u%% opaque=%u%%  (class is diagnostic only; AlphaRef decides)\n",
					        gltex_Name(pTexture),
					        (eBPP == BPP_S3TC_DXT1) ? 1 : ((eBPP == BPP_S3TC_DXT3) ? 3 : 5),
					        gltex_ClassName(nAlphaClass),
					        cStats.m_nTotal ? cStats.m_nClear  * 100 / cStats.m_nTotal : 0,
					        cStats.m_nTotal ? cStats.m_nOpaque * 100 / cStats.m_nTotal : 0);
			}
		}
		else
		{
			glTexImage2D(eFaceTarget, nMip, GL_RGBA8,
			             cMip.m_Width, cMip.m_Height, 0,
			             eFormat, eType, pFace);
			// Classify from the top mip only (BPP_32 = BGRA bytes), face 0.
			if (nMip == 0 && nFace == 0 && pData->m_Header.GetBPPIdent() == BPP_32)
			{
				nAlphaClass = gltex_ClassifyAlpha((const uint8*)cMip.m_Data,
				                                  cMip.m_Width * cMip.m_Height);
				if (getenv("LT_TRACE_ALPHACLASS"))
					fprintf(stderr, "[alpha] %-44s BPP32 %s\n",
					        gltex_Name(pTexture), gltex_ClassName(nAlphaClass));
			}
		}

			if (glGetError() != GL_NO_ERROR)
				bMipFailed = true;
		}   // faces

		if (bMipFailed)
			break;
		nLastLevel = (int)nMip;
		nBytes += (uint32)cMip.m_dataSize * nFaces;
	}

	if (nLastLevel < 0)
	{
		fprintf(stderr, "[gltex] upload failed: %s (%ux%u fmt %d%s)\n",
		        gltex_Name(pTexture), (uint32)pData->m_Header.m_BaseWidth,
		        (uint32)pData->m_Header.m_BaseHeight, (int)eBPP,
		        bCube ? " CUBE" : "");
		glDeleteTextures(1, &nName);
		return 0;
	}

	glTexParameteri(eTarget, GL_TEXTURE_MAX_LEVEL, nLastLevel);
	glTexParameteri(eTarget, GL_TEXTURE_MIN_FILTER,
	                nLastLevel > 0 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	glTexParameteri(eTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	// A cube map must clamp on all three axes or the seams sample across faces.
	glTexParameteri(eTarget, GL_TEXTURE_WRAP_S, bCube ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glTexParameteri(eTarget, GL_TEXTURE_WRAP_T, bCube ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	if (bCube)
		glTexParameteri(eTarget, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	++g_nTexCount;
	g_nTexBytes += nBytes;
	nOutBytes = nBytes;
	if (g_pGLStruct)
		g_pGLStruct->m_SystemTextureMemory += nBytes;
	if ((g_nTexCount & 63) == 1)
		fprintf(stderr, "[gltex] %u textures uploaded (%.1f MB)\n",
		        g_nTexCount, g_nTexBytes / (1024.0 * 1024.0));
	return nName;
}

void GLTex_Bind(SharedTexture *pTexture, bool bTextureChanged)
{
	if (!pTexture)
		return;

	GLTexEntry *pEntry = (GLTexEntry*)pTexture->m_pRenderData;
	if (pEntry)
	{
		if (!bTextureChanged)
			return;
		if (pEntry->m_nName)
		{
			glDeleteTextures(1, &pEntry->m_nName);
			pEntry->m_nName = 0;
			if (g_nTexCount) --g_nTexCount;
			g_nTexBytes -= pEntry->m_nBytes;
			if (g_pGLStruct)
				g_pGLStruct->m_SystemTextureMemory -= pEntry->m_nBytes;
			pEntry->m_nBytes = 0;
		}
	}
	else
	{
		pEntry = new GLTexEntry;
		pEntry->m_nName = 0;
		pEntry->m_nBytes = 0;
		pTexture->m_pRenderData = pEntry;
	}

	pEntry->m_nAlphaClass = GLTEX_ALPHA_MASKED;
	pEntry->m_nName = gltex_Create(pTexture, pEntry->m_nBytes, pEntry->m_nAlphaClass);
	{
		TextureData *pTD = (g_pGLStruct && g_pGLStruct->GetTexture)
		                 ? g_pGLStruct->GetTexture(pTexture) : 0;
		pEntry->m_nAlphaRef = gltex_ParseAlphaRef(pTD);
		pEntry->m_bCubeMap  = (pTD && (pTD->m_Header.m_IFlags & DTX_CUBEMAP)) ? 1 : 0;

		// ⚠️ These live in the DtxHeader's `Extra` bytes, NOT in the command
		// string: GetDetailTextureScale() is `*(float*)&m_Extra[6] + 1.0f` and
		// GetDetailTextureAngle() is an int16 of DEGREES at m_Extra[10..11]
		// (dtxmgr.h:87). The +1.0f bias means an all-zero Extra block reads as
		// scale 1.0, which is why a texture with no authored detail placement
		// still tiles sanely.
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
	}
	if (getenv("LT_TRACE_ALPHAREF"))
	{
		TextureData *pTD = (g_pGLStruct && g_pGLStruct->GetTexture)
		                 ? g_pGLStruct->GetTexture(pTexture) : 0;
		fprintf(stderr, "[alpharef] %-44s ref=%u cmd='%s'\n",
		        gltex_Name(pTexture), (unsigned)pEntry->m_nAlphaRef,
		        pTD ? pTD->m_Header.m_CommandString : "<no data>");
	}
}

void GLTex_Unbind(SharedTexture *pTexture)
{
	if (!pTexture || !pTexture->m_pRenderData)
		return;

	GLTexEntry *pEntry = (GLTexEntry*)pTexture->m_pRenderData;
	if (pEntry->m_nName)
	{
		glDeleteTextures(1, &pEntry->m_nName);
		if (g_nTexCount) --g_nTexCount;
		g_nTexBytes -= pEntry->m_nBytes;
		if (g_pGLStruct)
			g_pGLStruct->m_SystemTextureMemory -= pEntry->m_nBytes;
	}
	delete pEntry;
	pTexture->m_pRenderData = 0;
}

unsigned int GLTex_GetName(SharedTexture *pTexture)
{
	if (!pTexture)
		return 0;
	if (!pTexture->m_pRenderData)
		GLTex_Bind(pTexture, false);   // lazy path (bound before renderer init)
	GLTexEntry *pEntry = (GLTexEntry*)pTexture->m_pRenderData;
	return pEntry ? pEntry->m_nName : 0;
}

bool GLTex_GetDims(SharedTexture *pTexture, uint32 &nWidth, uint32 &nHeight)
{
	GLuint nName = GLTex_GetName(pTexture);
	if (!nName)
		return false;
	GLint nW = 0, nH = 0;
	// A cube map has no GL_TEXTURE_2D level to query — ask one of its faces.
	const bool bCube = GLTex_IsCubeMap(pTexture);
	const GLenum eTarget = bCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
	const GLenum eQuery  = bCube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : GL_TEXTURE_2D;
	glBindTexture(eTarget, nName);
	glGetTexLevelParameteriv(eQuery, 0, GL_TEXTURE_WIDTH, &nW);
	glGetTexLevelParameteriv(eQuery, 0, GL_TEXTURE_HEIGHT, &nH);
	nWidth  = (uint32)nW;
	nHeight = (uint32)nH;
	return nW > 0 && nH > 0;
}

unsigned int GLTex_GetAlphaClass(SharedTexture *pTexture)
{
	if (!pTexture || !pTexture->m_pRenderData)
		return GLTEX_ALPHA_OPAQUE;
	return ((GLTexEntry*)pTexture->m_pRenderData)->m_nAlphaClass;
}

unsigned int GLTex_GetAlphaRef(SharedTexture *pTexture)
{
	if (!pTexture || !pTexture->m_pRenderData)
		return 0;
	return ((GLTexEntry*)pTexture->m_pRenderData)->m_nAlphaRef;
}

bool GLTex_GetDetailParams(SharedTexture *pTexture, float &fScale, float &fCos, float &fSin)
{
	fScale = 1.0f; fCos = 1.0f; fSin = 0.0f;
	if (!pTexture)
		return false;
	if (!pTexture->m_pRenderData)
		GLTex_Bind(pTexture, false);
	GLTexEntry *pEntry = (GLTexEntry*)pTexture->m_pRenderData;
	if (!pEntry)
		return false;
	fScale = pEntry->m_fDetailScale;
	fCos   = pEntry->m_fDetailCos;
	fSin   = pEntry->m_fDetailSin;
	return true;
}

bool GLTex_IsCubeMap(SharedTexture *pTexture)
{
	if (!pTexture)
		return false;
	if (!pTexture->m_pRenderData)
		GLTex_Bind(pTexture, false);
	GLTexEntry *pEntry = (GLTexEntry*)pTexture->m_pRenderData;
	return pEntry && pEntry->m_bCubeMap != 0;
}


// Public accessor for the diagnostics — identifying WHICH texture a
// particle system or sprite is using is otherwise guesswork from positions.
const char *GLTex_GetTexName(SharedTexture *pTexture)
{
	return gltex_Name(pTexture);
}
