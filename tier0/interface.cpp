// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Interface registry implementation (InterfaceReg linked-list,
//          CreateInterface export, Sys_GetFactory helpers).
//
// $NoKeywords: $
//=============================================================================//

#include "../public/tier1/interface.h"
#include <cstring>

//-----------------------------------------------------------------------------
// InterfaceReg linked-list
//-----------------------------------------------------------------------------
InterfaceReg *InterfaceReg::s_pInterfaceRegs = nullptr;

InterfaceReg::InterfaceReg( InstantiateInterfaceFn fn, const char *pName )
	: m_CreateFn( fn )
	, m_pName( pName )
	, m_pNext( s_pInterfaceRegs )
{
	s_pInterfaceRegs = this;
}

//-----------------------------------------------------------------------------
// CreateInterface -- exported factory (ordinal ord. 314 in tier0.def)
// Walks the linked-list and instantiates the matching interface.
// Returns NULL and sets *pReturnCode to 1 if not found, 0 on success.
//-----------------------------------------------------------------------------
CREATEINTERFACE_API void* CreateInterface( const char *pName, int *pReturnCode )
{
	InterfaceReg *pCur;
	for ( pCur = InterfaceReg::s_pInterfaceRegs; pCur; pCur = pCur->m_pNext )
	{
		if ( pCur->m_pName && pName && std::strcmp( pCur->m_pName, pName ) == 0 )
		{
			if ( pReturnCode )
				*pReturnCode = 0;
			return pCur->m_CreateFn();
		}
	}

	if ( pReturnCode )
		*pReturnCode = 1;

	return nullptr;
}

//-----------------------------------------------------------------------------
// Sys_GetFactory -- retrieve CreateInterface from a loaded module
//-----------------------------------------------------------------------------
CreateInterfaceFn Sys_GetFactory( HMODULE hModule )
{
	if ( !hModule )
		return nullptr;

	return reinterpret_cast<CreateInterfaceFn>( GetProcAddress( hModule, "CreateInterface" ) );
}

CreateInterfaceFn Sys_GetFactory( const char *pModuleName )
{
	if ( !pModuleName || !pModuleName[0] )
		return nullptr;

	HMODULE hMod = GetModuleHandleA( pModuleName );
	if ( !hMod )
	{
		hMod = LoadLibraryA( pModuleName );
		if ( !hMod )
			return nullptr;
	}

	return Sys_GetFactory( hMod );
}

CreateInterfaceFn Sys_GetFactoryThis( void )
{
	return &CreateInterface;
}

//-----------------------------------------------------------------------------
// Self-test interface exposed from tier0.dll (verifies factory works across DLL boundary)
// Used by dumpbin/runtime checks, not required for GoldSrc compatibility.
//-----------------------------------------------------------------------------
class ITier0TestInterface
{
public:
	virtual int GetTestValue() = 0;
};

class CTier0TestImpl : public ITier0TestInterface
{
public:
	virtual int GetTestValue() { return 314; }
};

EXPOSE_INTERFACE( CTier0TestImpl, ITier0TestInterface, "Tier0Test001" )
