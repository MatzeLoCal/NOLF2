// macOS stub for ServerDir.
//
// The WON / Titan online matchmaking stack (wonapi, wondir/*, wonauth/*) is not
// ported to macOS. Returning NULL disables online matchmaking while letting
// ObjectDLL / ClientShellDLL link without the WON SDK. Single-player never uses
// the server directory at all. (A real port can replace this later.)
//
// ⚠️ THIS COMMENT USED TO CLAIM "every NOLF2 (TO2) caller null-checks the factory
// result". THAT IS FALSE and it cost two debugging sessions. It is true only of
// the two FACTORY call sites (TO2GameServerShell, ClientMultiplayerMgr). The
// callers of the plain ACCESSOR ClientMultiplayerMgr::GetServerDir() mostly do
// NOT check, and several dereference it inline. Crashes found and fixed so far:
//   ScreenJoin.cpp:925    CScreenJoin::Update           (opening the join screen)
//   ScreenMulti.cpp:476   CScreenMulti::Update          (opening the multi screen)
//   ScreenMulti.cpp:685   CScreenMulti::RequestMOTD
//   ScreenPreload.cpp     CD-key validation state machine (a direct-IP join)
// ⇒ If you enable a new multiplayer path and it faults on address 0x0, look for
// an unguarded GetServerDir() before looking anywhere else.
//
// ★ Note that LAN / direct-IP play does NOT need this at all: the server side
// early-returns on both a NULL directory and on m_bLANOnly
// (TO2GameServerShell.cpp:173), so only internet server BROWSING is lost here.

#include <windows.h>        // HMODULE
#define SERVERDIR_EXPORTS   // so SERVERDIR_API resolves to the export attribute
#include "IServerDir.h"

SERVERDIR_API IServerDirectory *Factory_Create_IServerDirectory_Titan(
    bool /*bClientSide*/, ILTCSBase & /*ltCSBase*/, HMODULE /*hResourceModule*/ )
{
    return 0;
}
