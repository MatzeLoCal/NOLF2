// ----------------------------------------------------------------------- //
//
// MODULE  : gl_convar.h
//
// PURPOSE : Registration of the renderer console variables declared in
//           rendererconsolevars.h. See gl_convar.cpp for why this matters —
//           without it those variables do not exist in the engine console, so
//           the game's writes to them (authored fog, far-Z, sky pan, shadow
//           settings from WorldProperties) are silently dropped.
//
// ----------------------------------------------------------------------- //
#ifndef __GL_CONVAR_H__
#define __GL_CONVAR_H__

struct RenderStruct;

// Call once at renderer init, before anything reads a console variable.
// Existing variables (set by autoexec.cfg / display.cfg) keep their values;
// missing ones are created with the renderer's declared default.
void GLConVar_Create(RenderStruct *pStruct);

// Refresh the ConVar objects from the engine console
// (RenderStruct::ReadConsoleVariables).
void GLConVar_Read(RenderStruct *pStruct);

#endif // __GL_CONVAR_H__
