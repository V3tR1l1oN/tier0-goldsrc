// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef DBG_H
#define DBG_H

#include "platform.h"

#ifdef _WIN32
#define DBG_INTERFACE_US
#endif

// Spew templates
enum SpewType_t
{
	SPEW_MESSAGE		= 0,
	SPEW_WARNING,
	SPEW_ASSERT,
	SPEW_ERROR,
	SPEW_LOG,

	SPEW_TYPE_COUNT
};

enum SpewRetval_t
{
	SPEW_CONTINUE		= 0,
	SPEW_DEBUGGER,
	SPEW_ABORT
};

typedef SpewRetval_t (*SpewOutputFunc_t)( SpewType_t spewType, const tchar *pMsg );
typedef void (*AssertFailedNotifyFunc_t)();
typedef void (*FlushLogFunc_t)();

PLATFORM_INTERFACE void   _SpewInfo( SpewType_t type, const tchar* pFile, int line );
PLATFORM_INTERFACE SpewRetval_t _SpewMessage( const tchar* pMsgFormat, ... );
PLATFORM_INTERFACE SpewRetval_t _DSpewMessage( tchar const* pGroupName, int level, tchar const* pMsgFormat, ... );
PLATFORM_INTERFACE SpewRetval_t _SpewMessageType( SpewType_t spewType, const tchar* pMsgFormat, va_list args );

DBG_INTERFACE void DMsg( tchar const* pGroupName, int level, tchar const* pMsgFormat, ... );
DBG_INTERFACE void _DMsg( tchar const* pGroupName, int level, tchar const* pMsgFormat, ... );

PLATFORM_INTERFACE void Warning( tchar const* pMsgFormat, ... );
PLATFORM_INTERFACE void DWarning( const tchar* pGroupName, int level, const tchar* pMsgFormat, ... );
PLATFORM_INTERFACE void Log( tchar const* pMsg, ... );
PLATFORM_INTERFACE void DLog( const tchar* pGroupName, int level, const tchar* pMsgFormat, ... );
PLATFORM_INTERFACE void Error( tchar const* pMsg, ... );
PLATFORM_INTERFACE void Msg( tchar const* pMsg, ... );

PLATFORM_INTERFACE void SpewActivate( tchar const* pGroupName, int level );
PLATFORM_INTERFACE bool IsLogActive( tchar const* pGroupName, int level );
PLATFORM_INTERFACE bool IsSpewActive( tchar const* pGroupName, int level );
PLATFORM_INTERFACE SpewOutputFunc_t GetSpewOutputFunc();
PLATFORM_INTERFACE void SpewOutputFunc( SpewOutputFunc_t func );

PLATFORM_INTERFACE void _ExitOnFatalAssert( tchar const* pFile, int line );
PLATFORM_INTERFACE void SetAssertFailedNotifyFunc( AssertFailedNotifyFunc_t func );
PLATFORM_INTERFACE void CallAssertFailedNotifyFunc();
PLATFORM_INTERFACE void SetFlushLogFunc( FlushLogFunc_t func );
PLATFORM_INTERFACE void CallFlushLogFunc();
PLATFORM_INTERFACE float CrackSmokingCompiler( float a ); // fabsf wrapper

PLATFORM_INTERFACE bool _AssertValidReadPtr( void* ptr, int count = 0 );
PLATFORM_INTERFACE bool _AssertValidWritePtr( void* ptr, int count = 0 );
PLATFORM_INTERFACE bool _AssertValidReadWritePtr( void* ptr, int count = 0 );
PLATFORM_INTERFACE bool AssertValidStringPtr( const tchar* ptr, int maxchar = 0xFFFFFF );

PLATFORM_INTERFACE bool ShouldUseNewAssertDialog();

extern "C" PLATFORM_INTERFACE int _printfExxx;

// Validate the spew system integrity.
DBG_INTERFACE void ValidateSpew();

#endif // DBG_H
