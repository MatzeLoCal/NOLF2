// ----------------------------------------------------------------------- //
//
// MODULE  : FullScreenTint.cpp
//
// PURPOSE : Implementation of FullScreenTint class
//
// CREATED : 6/22/02
//
// (c) 2002 Monolith Productions, Inc.  All Rights Reserved
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "FullScreenTint.h"

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CFullScreenTint::CFullScreenTint()
//
//	PURPOSE:	Initialize object
//
// ----------------------------------------------------------------------- //

CFullScreenTint::CFullScreenTint()
{
	m_fAlpha = 1.0f;
	m_bOn = false;
	m_hSurface = LTNULL;
	m_rcSrc.Init(0, 0, 2, 2);
    m_hTransColor = LTNULL;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CFullScreenTint::~CFullScreenTint()
//
//	PURPOSE:	Handle object destruction
//
// ----------------------------------------------------------------------- //

CFullScreenTint::~CFullScreenTint()
{
	Term();
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CFullScreenTint::SetAlpha()
//
//	PURPOSE:	Set the surface's alpha value
//
// ----------------------------------------------------------------------- //

void CFullScreenTint::SetAlpha(float fAlpha)
{
	m_fAlpha = (fAlpha < 0.0f ? 0.0f : (fAlpha > 1.0 ? 1.0f : fAlpha));

	if (m_hSurface)
	{
	    g_pLTClient->SetSurfaceAlpha(m_hSurface, m_fAlpha);
	}
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CFullScreenTint::Init()
//
//	PURPOSE:	Create the tint surface - black only (for now)
//
// ----------------------------------------------------------------------- //

void CFullScreenTint::Init()
{
    if (!m_hTransColor)
	{
		m_hTransColor = g_pLTClient->SetupColor1(1.0f, 1.0f, 1.0f, LTTRUE);
	}

	if (!m_hSurface)
	{
 		m_hSurface = g_pLTClient->CreateSurface(2, 2);
		if (m_hSurface)
		{
	        g_pLTClient->SetSurfaceAlpha(m_hSurface, m_fAlpha);
		    g_pLTClient->FillRect(m_hSurface, &m_rcSrc, kBlack);
			g_pLTClient->OptimizeSurface(m_hSurface, m_hTransColor);
		}
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CFullScreenTint::Term()
//
//	PURPOSE:	clean up 
//
// ----------------------------------------------------------------------- //

void CFullScreenTint::Term()
{
	if (m_hSurface)
	{
		g_pLTClient->DeleteSurface(m_hSurface);
		m_hSurface = LTNULL;
	}
}


// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CFullScreenTint::Draw()
//
//	PURPOSE:	Handle drawing the tint
//
// ----------------------------------------------------------------------- //

void CFullScreenTint::Draw(HSURFACE hScreen)
{
	if (!m_bOn || !hScreen || !m_hSurface) return;

    uint32 dwWidth = 640, dwHeight = 480;
    g_pLTClient->GetSurfaceDims(hScreen, &dwWidth, &dwHeight);
    
	LTRect rcDest;
	rcDest.Init(0, 0, dwWidth, dwHeight);

	// LT_TRACE_OVERLAY=1 -- what the full-screen tint thinks it is covering.
	// Reports on CHANGE, not once: the screen dims arrive late and the tint is
	// created before a world exists, so a one-shot line would only ever describe
	// the startup state (the §28/§70 "a diagnostic that can only fire once"
	// trap). If the visible tint does not reach the window edges while this
	// prints the full drawable size, the mis-sizing is BELOW this call -- in the
	// surface blit -- not in the rect handed to it.
	if (getenv("LT_TRACE_OVERLAY"))
	{
		static uint32 s_nLastW = 0, s_nLastH = 0;
		if (s_nLastW != dwWidth || s_nLastH != dwHeight)
		{
			s_nLastW = dwWidth; s_nLastH = dwHeight;
			fprintf(stderr, "[tint] full-screen tint dest 0,0..%ux%u  src %d,%d..%d,%d\n",
			        dwWidth, dwHeight,
			        (int)m_rcSrc.left, (int)m_rcSrc.top,
			        (int)m_rcSrc.right, (int)m_rcSrc.bottom);
		}
	}

    g_pLTClient->ScaleSurfaceToSurfaceTransparent(hScreen, m_hSurface,
		&rcDest, &m_rcSrc, m_hTransColor);
}


