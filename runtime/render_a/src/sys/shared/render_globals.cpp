// ----------------------------------------------------------------------- //
//
// MODULE  : render_globals.cpp
//
// PURPOSE : Storage for the backend-neutral renderer globals. See the header
//           for why these do not live in a gl_* or mtl_* translation unit.
//
// ----------------------------------------------------------------------- //

#include "render_globals.h"

// Assigned by rdll_RenderDLLSetup (nullrender.cpp) before any draw can happen.
RenderStruct *g_pRenderStruct = 0;
