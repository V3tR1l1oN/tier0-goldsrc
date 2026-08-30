// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#include "platform.h"
#include "dbg.h"
#include "strtools.h"
#include "threadtools.h"
#include "testthread.h"

#ifdef WIN32
#undef ARRAYSIZE
#include "winlite.h"
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof(p) / sizeof((p)[0]) )
#endif
#endif

//Disable "'typedef ': ignored on left of '' when no variable is declared"
#pragma warning( push )
#pragma warning( disable: 4091 )
#include "minidump.h"
#pragma warning( pop )

AssertFailedNotifyFunc_t s_AssertFailedNotifyFunc = nullptr;

// Assert-dialog plumbing lives in assert_dialog.cpp (no public header).
PLATFORM_INTERFACE bool IsInAssert();
PLATFORM_INTERFACE void SetInAssert( bool bState );
PLATFORM_INTERFACE bool DoNewAssertDialog( const tchar *pFilename, int line, const tchar *pExpression );

void CallAssertFailedNotifyFunc()
{
	if( s_AssertFailedNotifyFunc )
		s_AssertFailedNotifyFunc();
}

void SetAssertFailedNotifyFunc( AssertFailedNotifyFunc_t func )
{
	s_AssertFailedNotifyFunc = func;
}

FlushLogFunc_t s_FlushLogFunc = nullptr;

void CallFlushLogFunc()
{
	if( s_FlushLogFunc )
		s_FlushLogFunc();
}

void SetFlushLogFunc( FlushLogFunc_t func )
{
	s_FlushLogFunc = func;
}

float CrackSmokingCompiler( float a )
{
	return fabs( a );
}

struct SpewGroup_t
{
	char m_GroupName[ 48 ];
	int m_Level;
	int m_LogLevel;
};

static int s_GroupCount = 0;
static int s_DefaultLevel = 0;
static int s_DefaultLogLevel = 0;

static SpewGroup_t* s_pSpewGroups = nullptr;

static const char* s_pFileName = nullptr;
static int s_Line = 0;
static SpewType_t s_SpewType = SPEW_MESSAGE;

SpewRetval_t DefaultSpewFunc( SpewType_t type, const tchar* pMsg )
{
	// Original: DefaultSpewFunc prints the message to stdout, then
	// returns: ASSERT -> SPEW_CONTINUE, ERROR -> SPEW_ABORT, else SPEW_DEBUGGER.
	printf( "%s", pMsg );

	if( type == SPEW_ASSERT )
		return SPEW_CONTINUE;

	if( type == SPEW_ERROR )
		return SPEW_ABORT;

	return SPEW_DEBUGGER;
}

static SpewOutputFunc_t s_SpewOutputFunc = &DefaultSpewFunc;

SpewOutputFunc_t GetSpewOutputFunc()
{
	if( s_SpewOutputFunc )
		return s_SpewOutputFunc;

	return &DefaultSpewFunc;
}

void SpewOutputFunc( SpewOutputFunc_t func )
{
	if( !func )
		func = &DefaultSpewFunc;

	s_SpewOutputFunc = func;
}

bool FindSpewGroup( const tchar* pGroupName, int* pInd )
{
	if( s_GroupCount > 0 )
	{
		int start = 0, end = s_GroupCount - 1;

		while( true )
		{
			const int index = ( start + end ) / 2;

			const int bias = _tcsicmp( pGroupName, s_pSpewGroups[ index ].m_GroupName );

			if( !bias )
			{
				*pInd = index;
				return true;
			}

			if( bias >= 0 )
			{
				start = index + 1;
			}
			else
			{
				end = index - 1;
			}

			if( end < start )
			{
				// Not found. `start` is the insertion point that keeps the array
				// sorted (it is where the name belongs); `index` is the last
				// probe, which is one slot too low whenever the name sorts after
				// it. Return from here directly -- falling through to the
				// `*pInd = 0` below would throw the insertion point away.
				*pInd = start;
				return false;
			}
		}
	}

	*pInd = 0;

	return false;
}

void SpewAndLogActivate( const tchar *pGroupName, int level, int logLevel )
{
	if( !pGroupName || !pGroupName[ 0 ] )
		return;

	if( *pGroupName != '*' || pGroupName[ 1 ] )
	{
		int index;

SpewGroup_t* pGroup;

		if( FindSpewGroup( pGroupName, &index ) )
		{
			pGroup = &s_pSpewGroups[ index ];
		}
		else
		{
			// FindSpewGroup's binary-search probe does not yield a reliable
			// insertion point when the new name sorts after the last probed
			// entry, so (re)locate the position with a linear pass. Inserting
			// out of order would silently make the search fail forever and
			// every repeated SpewActivate would append a duplicate group.
			index = 0;
			while( index < s_GroupCount && _tcsicmp( pGroupName, s_pSpewGroups[ index ].m_GroupName ) > 0 )
				++index;

			SpewGroup_t *pNew = ( SpewGroup_t* ) realloc( s_pSpewGroups, sizeof( SpewGroup_t ) * ( s_GroupCount + 1 ) );
			if( !pNew )
				return;
			s_pSpewGroups = pNew;

			if( index < s_GroupCount )
			{
				memmove( &s_pSpewGroups[ index + 1 ], &s_pSpewGroups[ index ],
					sizeof( SpewGroup_t ) * ( s_GroupCount - index ) );
			}

			pGroup = &s_pSpewGroups[ index ];

			// Bounded copy: m_GroupName is a fixed 48-byte buffer and an
			// over-long caller name would run off the end of the record.
			_tcsncpy( pGroup->m_GroupName, pGroupName, ARRAYSIZE( pGroup->m_GroupName ) - 1 );
			pGroup->m_GroupName[ ARRAYSIZE( pGroup->m_GroupName ) - 1 ] = _T( '\0' );
			++s_GroupCount;
		}

		pGroup->m_Level = level;
		pGroup->m_LogLevel = logLevel;
	}
	else
	{
		s_DefaultLevel = level;
		s_DefaultLogLevel = logLevel;
	}
}

void SpewAndLogChangeIfStillDefault( const tchar *pGroupName, int level, int leveldefault, int logLevel, int logLevelDefault )
{
	int index;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		auto pGroup = &s_pSpewGroups[ index ];

		if( pGroup->m_Level == leveldefault && pGroup->m_LogLevel == logLevelDefault )
			SpewAndLogActivate( pGroupName, level, logLevel );
	}
}

void SpewChangeIfStillDefault( const tchar *pGroupName, int level, int leveldefault )
{
	int index;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		auto pGroup = &s_pSpewGroups[ index ];

		if( pGroup->m_Level == leveldefault )
			SpewAndLogActivate( pGroupName, level, level );
	}
}

void SpewActivate( tchar const* pGroupName, int level )
{
	SpewAndLogActivate( pGroupName, level, level );
}

bool IsLogActive( const tchar *pGroupName, int iLogLevel )
{
	int iLogLevelRequired = s_DefaultLogLevel;

	int index;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		iLogLevelRequired = s_pSpewGroups[ index ].m_LogLevel;
	}

	return iLogLevelRequired >= iLogLevel;
}

bool IsSpewActive( tchar const* pGroupName, int level )
{
	int index;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		return level <= s_pSpewGroups[ index ].m_Level;
	}
	else
	{
		return level <= s_DefaultLevel;
	}
}

void _SpewInfo( SpewType_t type, const tchar* pFile, int line )
{
	auto pszName = pFile;
	auto pszLastSlash = _tcsrchr( pFile, '\\' );
	auto pszLastSlash2 = _tcsrchr( pFile, '/' );

	if( pszLastSlash2 >= pszLastSlash )
		pszLastSlash = pszLastSlash2;

	if( pszLastSlash )
		pszName = pszLastSlash + 1;

	s_pFileName = pszName;
	s_Line = line;
	s_SpewType = type;
}

SpewRetval_t SpewMessageType( SpewType_t spewType, const tchar* pMsgFormat, va_list args )
{
	tchar pTempBuffer[ 5020 ];

	static CThreadMutex autoMutex;

	autoMutex.Lock();

	if( spewType == SPEW_ASSERT )
		Test_SetFailed();

	size_t uiLeft;
	int iWritten;

	if( spewType != SPEW_ASSERT )
	{
		uiLeft = sizeof( pTempBuffer ) - 1;
		iWritten = 0;
	}
	else
	{
		Test_SetFailed();

		iWritten = _sntprintf( pTempBuffer, ARRAYSIZE( pTempBuffer ) - 1, _T( "%s (%d) : " ),
			s_pFileName ? s_pFileName : _T( "unknown" ), s_Line );

		// _sntprintf reports truncation as a negative count on MSVC, but a
		// truncated result is still NUL-terminated -- clamp and keep going.
		if( iWritten < 0 || (size_t)iWritten >= ARRAYSIZE( pTempBuffer ) - 1 )
			iWritten = ( int )( ARRAYSIZE( pTempBuffer ) - 1 ) - 1;

		pTempBuffer[ iWritten ] = _T( '\0' );

		uiLeft = ( sizeof( pTempBuffer ) - 1 ) - iWritten;
	}

	int iAppendedWritten = vsnprintf( &pTempBuffer[ iWritten ], uiLeft, pMsgFormat, args );

	// vsnprintf is C99 on modern MSVC: on truncation it returns the length that
	// WOULD have been written, not -1. Treating -1 as the only failure let that
	// oversized value reach the index below and write far past the buffer.
	// Clamp to what actually fit (the text is already NUL-terminated).
	if( iAppendedWritten < 0 || (size_t)iAppendedWritten >= uiLeft )
		iAppendedWritten = ( int )uiLeft - 1;

	if( spewType == SPEW_ASSERT )
	{
		// Overwrites vsnprintf's terminator, so re-terminate after the newline.
		pTempBuffer[ iWritten + iAppendedWritten ] = _T( '\n' );
		pTempBuffer[ iWritten + iAppendedWritten + 1 ] = _T( '\0' );
	}

	SpewRetval_t retval = s_SpewOutputFunc( spewType, pTempBuffer );

	if( retval != SPEW_DEBUGGER )
	{
		if( retval == SPEW_ABORT )
		{
			DMsg( _T( "console" ), 1, _T( "Exiting on SPEW_ABORT\n" ) );
			exit( 0 );
		}
	}
	else if( spewType != SPEW_ASSERT )
	{
		__debugbreak();
	}

	autoMutex.Unlock();

	return retval;
}

SpewRetval_t _SpewMessage( const tchar* pMsgFormat, ... )
{
	va_list va;

	va_start( va, pMsgFormat );
	const auto retval = SpewMessageType( s_SpewType, pMsgFormat, va );
	va_end( va );

	return retval;
}

SpewRetval_t _DSpewMessage( tchar const *pGroupName, int level, tchar const* pMsg, ... )
{
	int index;

	bool bShouldLog;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	}
	else
	{
		bShouldLog = level <= s_DefaultLevel;
	}

	SpewRetval_t result = SPEW_CONTINUE;

	if( bShouldLog )
	{
		va_list va;

		va_start( va, pMsg );
		result = SpewMessageType( s_SpewType, pMsg, va );
		va_end( va );
	}

	return result;
}

void _ExitOnFatalAssert( tchar const* pFile, int line )
{
	_SpewMessage( "Fatal assert failed: %s, line %d.  Application exiting.\n", pFile, line );
	WriteMiniDump();
	DMsg( "console", 1, "_ExitOnFatalAssert\n" );
	exit( 1 );
}

void Msg( tchar const* pMsg, ... )
{
	va_list va;

	va_start( va, pMsg );
	SpewMessageType( SPEW_MESSAGE, pMsg, va );
	va_end( va );
}

void DMsg( tchar const* pGroupName, int level, tchar const* pMsg, ... )
{
	int index;

	bool bShouldLog;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	}
	else
	{
		bShouldLog = level <= s_DefaultLevel;
	}

	if( bShouldLog )
	{
		va_list va;

		va_start( va, pMsg );
		SpewMessageType( SPEW_MESSAGE, pMsg, va );
		va_end( va );
	}
}

//TODO: temporary until linker issues with vstdlib are resolved - Solokiller
DBG_INTERFACE void _DMsg( tchar const* pGroupName, int level, tchar const* pMsg, ... )
{
	int index;

	bool bShouldLog;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	}
	else
	{
		bShouldLog = level <= s_DefaultLevel;
	}

	if( bShouldLog )
	{
		va_list va;

		va_start( va, pMsg );
		SpewMessageType( SPEW_MESSAGE, pMsg, va );
		va_end( va );
	}
}

void Warning( tchar const *pMsg, ... )
{
	va_list va;

	va_start( va, pMsg );
	SpewMessageType( SPEW_WARNING, pMsg, va );
	va_end( va );
}

void DWarning( const tchar* pGroupName, int level, const tchar* pMsgFormat, ... )
{
	int index;

	bool bShouldLog;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	}
	else
	{
		bShouldLog = level <= s_DefaultLevel;
	}

	if( bShouldLog )
	{
		va_list va;

		va_start( va, pMsgFormat );
		SpewMessageType( SPEW_WARNING, pMsgFormat, va );
		va_end( va );
	}
}

void Log( tchar const *pMsg, ... )
{
	va_list va;

	va_start( va, pMsg );
	SpewMessageType( SPEW_LOG, pMsg, va );
	va_end( va );
}

void DLog( const tchar* pGroupName, int level, const tchar* pMsgFormat, ... )
{
	int index;

	bool bShouldLog;

	if( FindSpewGroup( pGroupName, &index ) )
	{
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	}
	else
	{
		bShouldLog = level <= s_DefaultLevel;
	}

	if( bShouldLog )
	{
		va_list va;

		va_start( va, pMsgFormat );
		SpewMessageType( SPEW_LOG, pMsgFormat, va );
		va_end( va );
	}
}

void Error( tchar const* pMsg, ... )
{
	va_list va;

	va_start( va, pMsg );
	SpewMessageType( SPEW_ERROR, pMsg, va );
	va_end( va );
}

// Original tier0.dll (exports @295-298) returns `bool` and gates every helper on
// IsBadReadPtr/IsBadWritePtr/IsBadStringPtrA before running the assert machinery:
//   10002829 call ds:IsBadReadPtr; test eax,eax; je return     <- valid -> return true
//   10002837 call IsInAssert; test al,al; jne return            <- in assert -> return false
//   10002848 call SetInAssert(1); ... assert path ...           <- show assert, return false
// We mirror that exactly; DoNewAssertDialog is non-blocking in this build.

bool _AssertValidReadPtr( void* ptr, int count )
{
	if ( !IsBadReadPtr( ptr, ( UINT )count ) )
		return true;

	if ( IsInAssert() )
		return false;

	SetInAssert( true );
	{
		char szExpr[ 64 ];
		_snprintf( szExpr, sizeof( szExpr ), "Invalid read pointer: 0x%p (%d byte%s)", ptr, count, count == 1 ? "" : "s" );
		DoNewAssertDialog( _T( "tier0" ), -1, szExpr );
	}
	SetInAssert( false );

	return false;
}

bool _AssertValidWritePtr( void* ptr, int count )
{
	if ( !IsBadWritePtr( ptr, ( UINT )count ) )
		return true;

	if ( IsInAssert() )
		return false;

	SetInAssert( true );
	{
		char szExpr[ 64 ];
		_snprintf( szExpr, sizeof( szExpr ), "Invalid write pointer: 0x%p (%d byte%s)", ptr, count, count == 1 ? "" : "s" );
		DoNewAssertDialog( _T( "tier0" ), -1, szExpr );
	}
	SetInAssert( false );

	return false;
}

bool _AssertValidReadWritePtr( void* ptr, int count )
{
	// Original: checks WRITE first, then READ (1000293D/10002949).
	if ( !IsBadWritePtr( ptr, ( UINT )count ) && !IsBadReadPtr( ptr, ( UINT )count ) )
		return true;

	if ( IsInAssert() )
		return false;

	SetInAssert( true );
	{
		char szExpr[ 64 ];
		_snprintf( szExpr, sizeof( szExpr ), "Invalid read/write pointer: 0x%p (%d byte%s)", ptr, count, count == 1 ? "" : "s" );
		DoNewAssertDialog( _T( "tier0" ), -1, szExpr );
	}
	SetInAssert( false );

	return false;
}

bool AssertValidStringPtr( const tchar* ptr, int maxchar )
{
	// Original uses IsBadStringPtrA (1001003C), i.e. ANSI strings even on wide builds.
	if ( !IsBadStringPtrA( ( LPCSTR )ptr, ( UINT )maxchar ) )
		return true;

	if ( IsInAssert() )
		return false;

	SetInAssert( true );
	{
		char szExpr[ 64 ];
		_snprintf( szExpr, sizeof( szExpr ), "Invalid string pointer: 0x%p (max %d chars)", ptr, maxchar );
		DoNewAssertDialog( _T( "tier0" ), -1, szExpr );
	}
	SetInAssert( false );

	return false;
}

void* Plat_SimpleLog( const tchar* file, int line )
{
	FILE* pFile = fopen( "simple.log", "at+" );

	// fopen fails when the CWD is not writable; _ftprintf(NULL, ...) would
	// have dereferenced a null FILE*.
	if( !pFile )
		return nullptr;

	_ftprintf( pFile, _T( "%s:%i\n" ), file, line );
	fclose( pFile );

	return nullptr;
}
