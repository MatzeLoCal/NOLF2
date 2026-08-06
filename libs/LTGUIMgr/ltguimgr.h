// ----------------------------------------------------------------------- //
//
// MODULE  : LTGUIMgr.h
//
// PURPOSE : Shared header for the library of LTGUI controls
//
// (c) 1997-2001 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //


#if !defined(_LTGUIMGR_H_)
#define _LTGUIMGR_H_


#include "lithtech.h"
#include "iltfontmanager.h"

//TO2 specific 
#include "../../NOLF2/Shared/DebugNew.h"

#pragma warning( disable : 4786 )
#include <vector>

typedef std::vector<CUIPolyString*> PStringArray;
typedef std::vector<CUIFormattedPolyString*> FPStringArray;

//base classes
#include "ltguicommandhandler.h"
#include "ltguictrl.h"
typedef std::vector<CLTGUICtrl*> ControlArray;

//basic control classes
#include "ltguitextitemctrl.h"
#include "ltguibutton.h"
#include "ltguicyclectrl.h"
#include "ltguitoggle.h"
#include "ltguislider.h"
#include "ltguicolumnctrl.h"
#include "ltguiframe.h"
#include "ltguieditctrl.h"
#include "ltguilargetext.h"

//container control classes
#include "ltguiwindow.h"
#include "ltguilistctrl.h"


//these are defined in the module that links
extern ILTDrawPrim*		g_pDrawPrim;
extern ILTFontManager*	g_pFontManager;
extern ILTTexInterface*	g_pTexInterface;

//utility functions for handling UV coordinates
#include "ltquaduvutils.h"

#endif // _LTGUIMGR_H_
