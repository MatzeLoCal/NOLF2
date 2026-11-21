// ----------------------------------------------------------------------- //
//
// MODULE  : DebugLine.cpp
//
// PURPOSE : 
//
// CREATED : 3/29/00
//
// ----------------------------------------------------------------------- //

#ifdef __clang__	// "stdafx.h" is StdAfx.h in the client module and Stdafx.h in the server one
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonportable-include-path"
#endif
#include "stdafx.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "DebugLine.h"
#include "iltmessage.h"

LTRGB DebugLine::null_rgba = {255,255,255,255};

ILTMessage_Write & operator<<(ILTMessage_Write & out, const DebugLine & line)
{
	out.WriteLTVector( const_cast<LTVector&>(line.vSource) );
	out.WriteLTVector( const_cast<LTVector&>(line.vDest) );
	out.Writeuint8( const_cast<uint8&>( line.rgba.r ) );
	out.Writeuint8( const_cast<uint8&>( line.rgba.g ) );
	out.Writeuint8( const_cast<uint8&>( line.rgba.b ) );
	out.Writeuint8( const_cast<uint8&>( line.rgba.a ) );

	return out;
}

ILTMessage_Read & operator>>(ILTMessage_Read & in, DebugLine & line)
{
	line.vSource = in.ReadLTVector();
	line.vDest   = in.ReadLTVector();
	line.rgba.r	 = in.Readuint8();
	line.rgba.g	 = in.Readuint8();
	line.rgba.b	 = in.Readuint8();
	line.rgba.a	 = in.Readuint8();

	return in;
}
