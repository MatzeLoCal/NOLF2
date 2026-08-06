// ----------------------------------------------------------------------- //
//
// MODULE  : gl_convar.cpp
//
// PURPOSE : Register the RENDERER CONSOLE VARIABLES with the engine console.
//
//           ★ WHY THIS EXISTS (found while implementing fog, 2026-07-25):
//           `rendererconsolevars.h` declares 178 console variables that ARE
//           the renderer's published contract with the rest of the engine --
//           FogEnable/FogR/FogG/FogB/FogNearZ/FogFarZ, FarZ, ClampFarZ,
//           reallyclose_near/far, PVModelFOV, DetailTextures, EnvMap*,
//           ModelShadow_*, Saturate, ... The GAME reads and writes many of
//           them BY NAME: WorldProperties pushes the level's authored fog,
//           far-Z, sky-pan and shadow settings into them at load
//           (WorldProperties.cpp:390-500), and VolumeBrushes override the fog
//           ones while the player is inside a volume.
//
//           The D3D renderer creates every one of them at init
//           (common_init.cpp:120 -> d3d_CreateConsoleVariables ->
//           d3d_MaybeCreateCVar, which RUNS "<name> <default>" through the
//           console when the variable does not exist yet). Our GL renderer
//           never did, so those variables simply DID NOT EXIST: every one of
//           the game's SetGameConVar writes went nowhere, and the renderer's
//           own GetParameter lookups all missed and fell back to hardcoded
//           defaults. Symptom that exposed it: fog rendered as pure white at
//           range 0..2000 (the bare defaults) in every level, because
//           FogR/G/B/NearZ/FarZ were MISSING while FogEnable -- the one fog
//           variable retail's autoexec.cfg happens to set -- was found.
//
//           This file is the GL twin of d3d_convar/common_stuff's half:
//           instantiate the variables and mirror the create/read walks.
//           Nothing else in the GL renderer reads the ConVar objects yet (the
//           gl_* modules query the engine console directly by name), but
//           registering them is what makes those queries -- and the game's
//           writes -- resolve at all.
//
// ----------------------------------------------------------------------- //

#include <windows.h>
#include "ltbasedefs.h"
#include "renderstruct.h"
#include <stdio.h>

// The linked list every ConVar's constructor pushes itself onto (d3d_convar.h).
// The D3D build defines this in common_stuff.cpp, which we do not build.
class BaseConVar;
BaseConVar* g_pConVars = NULL;

#define INSTANTIATE_RENDERER_CONSOLE_VARS
#include "rendererconsolevars.h"

#include "gl_convar.h"

// Mirrors d3d_MaybeCreateCVar (common_stuff.cpp:43): use the engine's variable
// if it already exists (autoexec.cfg / display.cfg have priority -- that is how
// retail's "Saturate" "1" and "FogEnable" "1" survive), otherwise create it by
// running a console assignment with the renderer's default.
static HLTPARAM glcv_MaybeCreate(RenderStruct *pStruct, const char *pName, float fDefault)
{
	HLTPARAM hRet = pStruct->GetParameter(const_cast<char*>(pName));
	if (hRet)
		return hRet;

	char str[256];
	snprintf(str, sizeof(str), "%s %f", pName, fDefault);
	pStruct->RunConsoleString(str);
	return pStruct->GetParameter(const_cast<char*>(pName));
}

void GLConVar_Create(RenderStruct *pStruct)
{
	if (!pStruct || !pStruct->GetParameter || !pStruct->RunConsoleString)
		return;

	uint32 nCreated = 0, nExisting = 0;
	for (BaseConVar *pCur = g_pConVars; pCur; pCur = pCur->m_pNext)
	{
		bool bExisted = (pStruct->GetParameter(const_cast<char*>(pCur->m_pName)) != NULL);
		pCur->m_hParam = glcv_MaybeCreate(pStruct, pCur->m_pName, pCur->m_DefaultVal);
		if (bExisted) ++nExisting; else ++nCreated;
	}

	fprintf(stderr, "[glcv] %u renderer console vars created, %u already set by config\n",
	        nCreated, nExisting);
}

void GLConVar_Read(RenderStruct *pStruct)
{
	if (!pStruct || !pStruct->GetParameterValueFloat)
		return;

	for (BaseConVar *pCur = g_pConVars; pCur; pCur = pCur->m_pNext)
	{
		if (pCur->m_hParam)
			pCur->SetFloat(pStruct->GetParameterValueFloat(pCur->m_hParam));
	}
}
