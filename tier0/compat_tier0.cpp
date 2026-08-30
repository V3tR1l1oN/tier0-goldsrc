// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: CTier0 global-registry holder + fDumped globals used by the
//			shipped binary's budget-group dump logic.
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "validator.h"
#include "vprof.h"

//=============================================================================
// fDumped function-local tracking flags.
// In the original these live as function-statics; their mangled names are
// forced onto plain globals through .def DATA aliases so the exported
// addresses stay stable for tooling.
//=============================================================================

extern "C"
{
	bool g_fdump_pushgroup_a			= false;
	bool g_fdump_pushgroup_b			= false;
	bool g_fdump_popgroup				= false;
	bool g_fdump_getparent				= false;
	bool g_fdump_getname				= false;
	bool g_fdump_getorignameaddress		= false;
	bool g_fdump_getbudgetgroupname		= false;
	bool g_fdump_getbudgetgroupflags	= false;

	// PME plumbing shared across tier0.
}

//=============================================================================
// CTier0
//=============================================================================

class CTier0
{
public:
	CTier0() {}

	CTier0( const CTier0& other )					{ *this = other; }
	CTier0& operator=( const CTier0& other );
	CTier0& operator=( CTier0&& other );

	static void ValidateGlobals( CValidator &validator );
};

CTier0& CTier0::operator=( const CTier0& /*other*/ )
{
	return *this;
}

CTier0& CTier0::operator=( CTier0&& /*other*/ )
{
	return *this;
}

void CTier0::ValidateGlobals( CValidator &validator )
{
#ifdef DBGFLAG_VALIDATE
	ValidateGlobals_internal( validator );
#else
	UNREFERENCED_PARAMETER( validator );
#endif
}

// Private helper expected by the class entry point; declared in validator.h.
void ValidateGlobals_internal( CValidator &validator )
{
	validator.Push( "CTier0", NULL, "globals" );
	g_VProfCurrentProfile.Validate( validator, "g_VProfCurrentProfile" );
	validator.Pop();
}

// These are plain C++ globals (not extern "C") so their mangled forms match
// the exported names of the shipped binary.
bool g_bPMELoaded		= false;
bool g_bPMEInitialized	= false;
bool g_bPMEEnabled		= false;

// NOTE: there is deliberately no `g_bInException` definition here any more.
// The flag that the export table ships ( ?g_bInException@@3_NC ) is the
// `volatile bool` in exact_native_shims.cpp. Keeping a second, plain `bool`
// with the same source name created a *different* symbol
// ( ?g_bInException@@3_NA ) that nothing referenced: any future translation
// unit declaring `extern bool g_bInException;` would silently have read and
// written a flag unrelated to the one the exception handler sets.
