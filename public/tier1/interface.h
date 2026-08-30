// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Public interface registry (CreateInterface factory).
//          Matches Source SDK tier1/interface.h layout: InterfaceReg linked-list,
//          EXPOSE_INTERFACE macros, Sys_GetFactory, CreateInterface export.
//
// $NoKeywords: $
//=============================================================================//

#ifndef TIER1_INTERFACE_H
#define TIER1_INTERFACE_H

#ifdef _WIN32
#pragma once
#endif

#include <windows.h>
#include "tier0/platform.h"

//-----------------------------------------------------------------------------
// CreateInterface function pointer types
//-----------------------------------------------------------------------------
typedef void* (*CreateInterfaceFn)( const char *pName, int *pReturnCode );
typedef void* (*InstantiateInterfaceFn)();

//-----------------------------------------------------------------------------
// InterfaceReg -- linked-list registry for all interfaces exposed via
// CreateInterface. Each EXPOSE_INTERFACE macro creates a static instance
// that inserts itself into s_pInterfaceRegs before main/DllMain.
//-----------------------------------------------------------------------------
class InterfaceReg
{
public:
	InterfaceReg( InstantiateInterfaceFn fn, const char *pName );

	InstantiateInterfaceFn m_CreateFn;
	const char *m_pName;
	InterfaceReg *m_pNext;
	static InterfaceReg *s_pInterfaceRegs;
};

//-----------------------------------------------------------------------------
// Macros to expose interfaces
//-----------------------------------------------------------------------------

// Expose a function-based interface (custom factory function).
#define EXPOSE_INTERFACE_FN( functionName, interfaceName, versionName ) \
	static InterfaceReg __g_Create##interfaceName##_reg( functionName, versionName );

// Expose a class-based interface: allocates new instance via default ctor.
#define EXPOSE_INTERFACE( className, interfaceName, versionName ) \
	static void* __Create##className##_interface() { return static_cast<interfaceName*>( new className ); } \
	static InterfaceReg __g_Create##className##_reg( __Create##className##_interface, versionName );

// Expose a singleton class-based interface (single static instance).
#define EXPOSE_SINGLE_INTERFACE( className, interfaceName, versionName ) \
	static className __g_##className##_singleton; \
	static void* __Create##className##_singleton_interface() { return static_cast<interfaceName*>( &__g_##className##_singleton ); } \
	static InterfaceReg __g_Create##className##_reg( __Create##className##_singleton_interface, versionName );

// Global singleton variant with explicit global variable name.
#define EXPOSE_SINGLE_INTERFACE_GLOBALVAR( className, interfaceName, versionName, globalVarName ) \
	static void* __Create##className##__##globalVarName##_interface() { return static_cast<interfaceName*>( &globalVarName ); } \
	static InterfaceReg __g_Create##className##__##globalVarName##_reg( __Create##className##__##globalVarName##_interface, versionName );

//-----------------------------------------------------------------------------
// Factory helpers
//-----------------------------------------------------------------------------

// CreateInterface is exported from tier0.dll as ordinal @314.
// It walks InterfaceReg::s_pInterfaceRegs and returns the matching interface.
#if defined( TIER0_DLL_EXPORT )
#define CREATEINTERFACE_API extern "C" __declspec(dllexport)
#else
#define CREATEINTERFACE_API extern "C" __declspec(dllimport)
#endif

CREATEINTERFACE_API void* CreateInterface( const char *pName, int *pReturnCode );

// Sys_GetFactory returns CreateInterface function pointer for a given module.
CreateInterfaceFn Sys_GetFactory( HMODULE hModule );
CreateInterfaceFn Sys_GetFactory( const char *pModuleName );
CreateInterfaceFn Sys_GetFactoryThis( void );

// Convenience helper: load module and get interface in one call.
inline void* Sys_GetInterface( const char *pModuleName, const char *pInterfaceName )
{
	CreateInterfaceFn fn = Sys_GetFactory( pModuleName );
	if ( !fn )
		return NULL;
	return fn( pInterfaceName, NULL );
}

#endif // TIER1_INTERFACE_H
