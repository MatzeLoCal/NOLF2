// Minimal macOS stub for <d3dx9.h> (D3DX helper math/texture lib). The null
// renderer never uses these; just enough types/functions for the shared render
// headers to parse. Real math goes through the engine's own LTMatrix/LTVector.
#ifndef __LT_COMPAT_D3DX9_H__
#define __LT_COMPAT_D3DX9_H__

#include <windows.h>
#include <d3d9.h>
#include <math.h>

// --- D3DX vector / matrix types ---------------------------------------------
typedef struct D3DXVECTOR2 { float x, y; } D3DXVECTOR2;
typedef D3DVECTOR D3DXVECTOR3;
typedef struct D3DXVECTOR4 { float x, y, z, w; } D3DXVECTOR4;
typedef D3DMATRIX  D3DXMATRIX,  *LPD3DXMATRIX;
typedef D3DXVECTOR3 *LPD3DXVECTOR3;
typedef struct D3DXQUATERNION { float x, y, z, w; } D3DXQUATERNION;
typedef struct D3DXPLANE { float a, b, c, d; } D3DXPLANE;
typedef DWORD D3DXCOLOR;

// --- D3DX effect/shader framework (opaque; held only as pointers) -----------
struct ID3DXEffect;
struct ID3DXEffectPool;
struct ID3DXBuffer;
struct ID3DXConstantTable;
typedef ID3DXEffect*        LPD3DXEFFECT;
typedef ID3DXEffectPool*    LPD3DXEFFECTPOOL;
typedef ID3DXBuffer*        LPD3DXBUFFER;
typedef ID3DXConstantTable* LPD3DXCONSTANTTABLE;
typedef DWORD               D3DXHANDLE;

// --- the handful of D3DX entry points referenced (no-op/identity stubs) -----
static inline D3DXMATRIX* D3DXMatrixIdentity(D3DXMATRIX* p) {
    if (p) { memset(p, 0, sizeof(*p)); p->_11 = p->_22 = p->_33 = p->_44 = 1.0f; }
    return p;
}
static inline D3DXMATRIX* D3DXMatrixMultiply(D3DXMATRIX* out, const D3DXMATRIX*, const D3DXMATRIX*) {
    return D3DXMatrixIdentity(out);
}
static inline D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* out, float*, const D3DXMATRIX*) {
    return D3DXMatrixIdentity(out);
}
static inline D3DXMATRIX* D3DXMatrixTranspose(D3DXMATRIX* out, const D3DXMATRIX* in) {
    if (out && in) *out = *in; return out;
}
static inline float D3DXVec3Length(const D3DXVECTOR3* v) {
    return v ? sqrtf(v->x*v->x + v->y*v->y + v->z*v->z) : 0.0f;
}

#endif // __LT_COMPAT_D3DX9_H__
