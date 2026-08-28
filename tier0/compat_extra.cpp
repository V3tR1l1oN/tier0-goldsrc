// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Legacy compatibility shims kept from earlier reconstruction passes.
//			Everything needed by the shipped export table now lives in
//			threadtools.cpp / validator.cpp; this file only supplies small
//			helpers that reference tree-wide state.
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include "threadtools.h"
#include "vprof.h"

#ifdef WIN32
#include "winlite.h"
#endif
