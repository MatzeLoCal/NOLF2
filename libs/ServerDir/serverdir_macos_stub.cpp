// macOS stub for ServerDir.
//
// The WON / Titan online matchmaking stack (wonapi, wondir/*, wonauth/*) is not
// ported to macOS. Single-player never uses the server directory, and every
// NOLF2 (TO2) caller null-checks the factory result — e.g. TO2GameServerShell
// and ClientMultiplayerMgr both do `if (!pServerDir) ...`. So returning NULL
// cleanly disables online play while letting ObjectDLL / ClientShellDLL link
// without the WON SDK. (A real port can replace this later.)

#include <windows.h>        // HMODULE
#define SERVERDIR_EXPORTS   // so SERVERDIR_API resolves to the export attribute
#include "IServerDir.h"

SERVERDIR_API IServerDirectory *Factory_Create_IServerDirectory_Titan(
    bool /*bClientSide*/, ILTCSBase & /*ltCSBase*/, HMODULE /*hResourceModule*/ )
{
    return 0;
}
