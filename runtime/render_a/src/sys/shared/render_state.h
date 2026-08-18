// ----------------------------------------------------------------------- //
//
// MODULE  : render_state.h
//
// PURPOSE : The BACKEND-NEUTRAL vocabulary for a draw's blend and alpha-test
//           state — what `REmitState` and its siblings carry from the code
//           that RESOLVES authored render styles to the code that DRAWS.
//
//           ★ WHY THIS EXISTS: those descriptions used to be written in GL's
//           vocabulary — `m_nSrcBlend = GL_SRC_ALPHA`, `m_nAlphaFunc =
//           GL_GEQUAL` — and mtl_model.mm switched on the same GL constants to
//           map them to MTLBlendFactor. So the SHARED description of a draw
//           depended on a GL header, and deleting GL would have broken Metal.
//
//           ⚠️⚠️ THE VALUES ARE DELIBERATELY IDENTICAL TO THE GL CONSTANTS
//           THEY REPLACE (verified against the MacOSX SDK's gl.h). That is not
//           laziness — it makes the migration PROVABLY a no-op numerically, and
//           that mattered here: the null-shell harness draws no sprites, no
//           particles and no polygrids, so three of the four producers of these
//           values are NOT covered by the frame-diff contract. A renumbering
//           would have been unverifiable by the only instrument available.
//           Renumber later if it ever earns its keep; the names are the point.
//
// ----------------------------------------------------------------------- //
#ifndef __RENDER_STATE_H__
#define __RENDER_STATE_H__

// Blend factors. Values match GL_ZERO/GL_ONE/GL_SRC_COLOR/... exactly.
enum ERBlendFactor
{
	kRBlend_Zero             = 0,        // GL_ZERO
	kRBlend_One              = 1,        // GL_ONE
	kRBlend_SrcColor         = 0x0300,   // GL_SRC_COLOR
	kRBlend_InvSrcColor      = 0x0301,   // GL_ONE_MINUS_SRC_COLOR
	kRBlend_SrcAlpha         = 0x0302,   // GL_SRC_ALPHA
	kRBlend_InvSrcAlpha      = 0x0303,   // GL_ONE_MINUS_SRC_ALPHA
	kRBlend_DstAlpha         = 0x0304,   // GL_DST_ALPHA
	kRBlend_InvDstAlpha      = 0x0305,   // GL_ONE_MINUS_DST_ALPHA
	kRBlend_DstColor         = 0x0306,   // GL_DST_COLOR
	kRBlend_InvDstColor      = 0x0307,   // GL_ONE_MINUS_DST_COLOR
	kRBlend_SrcAlphaSaturate = 0x0308,   // GL_SRC_ALPHA_SATURATE
};

// Alpha-test comparisons. Values match GL_NEVER/GL_LESS/... exactly.
enum ERAlphaFunc
{
	kRAlpha_Never    = 0x0200,   // GL_NEVER
	kRAlpha_Less     = 0x0201,   // GL_LESS
	kRAlpha_Equal    = 0x0202,   // GL_EQUAL
	kRAlpha_LEqual   = 0x0203,   // GL_LEQUAL
	kRAlpha_Greater  = 0x0204,   // GL_GREATER
	kRAlpha_NotEqual = 0x0205,   // GL_NOTEQUAL
	kRAlpha_GEqual   = 0x0206,   // GL_GEQUAL
	kRAlpha_Always   = 0x0207,   // GL_ALWAYS
};

#endif  // __RENDER_STATE_H__
