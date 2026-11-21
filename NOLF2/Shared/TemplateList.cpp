#ifdef __clang__	// "stdafx.h" is StdAfx.h in the client module and Stdafx.h in the server one
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonportable-include-path"
#endif
#include "Stdafx.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "TemplateList.h"

int s_cLTLinkBankRefCount = 0;
CBankedList<LTLink>* s_pLTLinkBank = LTNULL;
