// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Native emission of the 8 function-local fDumped static variables.
//
//=============================================================================//

#include "platform.h"
#include "vprof.h"

// Emit fDumped static in CVProfNode::GetName
// Mangled name: ?fDumped@?BA@??GetName@CVProfNode@@QAEPBDXZ@4_NA (or current compiler equivalent)
void Dummy_FDumped_NodeGetName()
{
	static bool fDumped = false;
	UNREFERENCED_PARAMETER( fDumped );
}
