// ----------------------------------------------------------------------- //
//
// MODULE  : mtl_matrix.h
//
// PURPOSE : The handful of matrices the Metal renderer builds, shared by every
//           pass. Column-major, exactly like glLoadMatrixf — Metal's float4x4
//           reads a buffer as columns, so the layout carries straight across
//           from the GL code these were derived from.
//
//           ⚠️ TWO THINGS DIFFER FROM GL AND BOTH ARE SILENT WHEN WRONG:
//           * Metal's NDC z is 0..1, GL's is -1..1. A GL projection matrix used
//             unchanged puts the whole near half of the scene behind the near
//             plane.
//           * Metal's framebuffer origin is TOP-LEFT, GL's viewport origin is
//             bottom-left. That does not change these matrices, but it does
//             invert which winding counts as front-facing — see the cull notes
//             at the draw sites.
//
// ----------------------------------------------------------------------- //
#ifndef __MTL_MATRIX_H__
#define __MTL_MATRIX_H__

#include <string.h>

static inline void mtl_Identity(float *m)
{
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (both column-major). Safe when out aliases a or b.
static inline void mtl_Mul(float *out, const float *a, const float *b)
{
	float t[16];
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0]
			             + a[1 * 4 + r] * b[c * 4 + 1]
			             + a[2 * 4 + r] * b[c * 4 + 2]
			             + a[3 * 4 + r] * b[c * 4 + 3];
	memcpy(out, t, sizeof(t));
}

// Engine screen space (top-left origin, pixels) -> Metal clip space.
static inline void mtl_Ortho(float *m, float fW, float fH)
{
	memset(m, 0, sizeof(float) * 16);
	m[0]  =  2.0f / fW;
	m[5]  = -2.0f / fH;      // top-left origin
	m[10] =  0.5f;           // z -1..1 -> 0..1
	m[12] = -1.0f;
	m[13] =  1.0f;
	m[14] =  0.5f;
	m[15] =  1.0f;
}

// Symmetric perspective, right-handed eye space looking down -Z, Metal z 0..1.
// fRight/fTop are the half-extents of the near plane, as glFrustum takes them.
static inline void mtl_Frustum(float *m, float fRight, float fTop,
                               float fNear, float fFar)
{
	memset(m, 0, sizeof(float) * 16);
	m[0]  = fNear / fRight;
	m[5]  = fNear / fTop;
	m[10] = -fFar / (fFar - fNear);
	m[11] = -1.0f;
	m[14] = -(fFar * fNear) / (fFar - fNear);
}

#endif // __MTL_MATRIX_H__
