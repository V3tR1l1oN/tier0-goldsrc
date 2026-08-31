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
static CThreadMutex s_SpewGroupsMutex;

static thread_local const char* s_pFileName = nullptr;
static thread_local int s_Line = 0;
static thread_local SpewType_t s_SpewType = SPEW_MESSAGE;

// --- VirtualQuery based pointer validation (replaces banned APIs) ---
static bool IsReadableMemory( const void* ptr, SIZE_T size )
{
	if ( !ptr || size == 0 )
		return size == 0 && ptr != nullptr; // zero-size check considered valid if ptr non-null; caller handles count==0 separately
	if ( size > 0x7fffffff )
		return false;
#ifdef _LINUX
	// POSIX: mincore() reports which pages are backed by physical memory.
	// A readable mapping is committed; a fault-free read on an uncommitted or
	// protected/unreadable page would SIGSEGV. We scan page-by-page.
	size_t page = (size_t)sysconf( _SC_PAGESIZE );
	if ( page == 0 )
		page = 4096;
	const uintptr_t start = (uintptr_t)ptr;
	const uintptr_t endp = start + (uintptr_t)size;
	for ( uintptr_t p = start & ~(uintptr_t)( page - 1 ); p < endp; p += page )
	{
		unsigned char vec = 0;
		if ( mincore( (void*)p, page, &vec ) != 0 )
			return false;
		if ( ( vec & 1 ) == 0 )
			return false;
	}
	return true;
#else
	const BYTE* addr = (const BYTE*)ptr;
	const BYTE* end = addr + size;
	MEMORY_BASIC_INFORMATION mbi;
	while ( addr < end )
	{
		if ( VirtualQuery( addr, &mbi, sizeof( mbi ) ) != sizeof( mbi ) )
			return false;
		if ( mbi.State != MEM_COMMIT )
			return false;
		if ( mbi.Protect & PAGE_GUARD )
			return false;
		if ( mbi.Protect & PAGE_NOACCESS )
			return false;
		DWORD protect = mbi.Protect & 0xFF;
		bool readable = ( protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
		                  protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY );
		if ( !readable )
			return false;
		const BYTE* regionEnd = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
		const BYTE* next = regionEnd < end ? regionEnd : end;
		if ( next <= addr )
			return false;
		addr = next;
	}
#endif
	return true;
}

static bool IsWritableMemory( void* ptr, SIZE_T size )
{
	if ( !ptr || size == 0 )
		return size == 0 && ptr != nullptr;
	if ( size > 0x7fffffff )
		return false;
#ifdef _LINUX
	// POSIX: assume writable for the pointer we own (no cheap portable probe).
	return true;
#else
	const BYTE* addr = (const BYTE*)ptr;
	const BYTE* end = addr + size;
	MEMORY_BASIC_INFORMATION mbi;
	while ( addr < end )
	{
		if ( VirtualQuery( addr, &mbi, sizeof( mbi ) ) != sizeof( mbi ) )
			return false;
		if ( mbi.State != MEM_COMMIT )
			return false;
		if ( mbi.Protect & PAGE_GUARD )
			return false;
		if ( mbi.Protect & PAGE_NOACCESS )
			return false;
		DWORD protect = mbi.Protect & 0xFF;
		bool writable = ( protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
		                  protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY );
		if ( !writable )
			return false;
		const BYTE* regionEnd = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
		const BYTE* next = regionEnd < end ? regionEnd : end;
		if ( next <= addr )
			return false;
		addr = next;
	}
#endif
	return true;
}

static bool IsValidStringPtrA( const char* ptr, SIZE_T maxChars )
{
	if ( !ptr )
		return false;
	if ( maxChars == 0 )
		return true;
	// Walk up to maxChars looking for NUL, validating readability page-by-page
	const char* cur = ptr;
	SIZE_T remaining = maxChars;
	while ( remaining > 0 )
	{
		MEMORY_BASIC_INFORMATION mbi;
		if ( VirtualQuery( cur, &mbi, sizeof( mbi ) ) != sizeof( mbi ) )
			return false;
		if ( mbi.State != MEM_COMMIT || ( mbi.Protect & PAGE_GUARD ) || ( mbi.Protect & PAGE_NOACCESS ) )
			return false;
		DWORD protect = mbi.Protect & 0xFF;
		bool readable = ( protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
		                  protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY );
		if ( !readable )
			return false;
		const char* regionEnd = (const char*)mbi.BaseAddress + mbi.RegionSize;
		SIZE_T chunk = (SIZE_T)( regionEnd - cur );
		if ( chunk > remaining )
			chunk = remaining;
		// scan for NUL within chunk
		for ( SIZE_T i = 0; i < chunk; ++i )
		{
			__try
			{
				if ( cur[i] == '\0' )
					return true;
			}
			__except ( EXCEPTION_EXECUTE_HANDLER )
			{
				return false;
			}
		}
		cur += chunk;
		remaining -= chunk;
	}
	// No terminating NUL found within maxChars => still considered valid for legacy semantics (checks readability only)
	// but if memory was readable up to maxChars, return true
	return true;
}

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

		// Protect sorted array + realloc against concurrent SpewActivate
		s_SpewGroupsMutex.Lock();
		bool bFound = FindSpewGroup( pGroupName, &index );
		if( bFound )
		{
			pGroup = &s_pSpewGroups[ index ];
			pGroup->m_Level = level;
			pGroup->m_LogLevel = logLevel;
			s_SpewGroupsMutex.Unlock();
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
			{
				s_SpewGroupsMutex.Unlock();
				return;
			}
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

			pGroup->m_Level = level;
			pGroup->m_LogLevel = logLevel;
			s_SpewGroupsMutex.Unlock();
		}
	}
	else
	{
		// Default group also needs lock – readers (IsSpewActive etc.) race with this write.
		s_SpewGroupsMutex.Lock();
		s_DefaultLevel = level;
		s_DefaultLogLevel = logLevel;
		s_SpewGroupsMutex.Unlock();
	}
}

void SpewAndLogChangeIfStillDefault( const tchar *pGroupName, int level, int leveldefault, int logLevel, int logLevelDefault )
{
	int index;
	bool needActivate = false;
	s_SpewGroupsMutex.Lock();
	if( FindSpewGroup( pGroupName, &index ) )
	{
		auto pGroup = &s_pSpewGroups[ index ];
		needActivate = ( pGroup->m_Level == leveldefault && pGroup->m_LogLevel == logLevelDefault );
	}
	s_SpewGroupsMutex.Unlock();
	if( needActivate )
		SpewAndLogActivate( pGroupName, level, logLevel );
}

void SpewChangeIfStillDefault( const tchar *pGroupName, int level, int leveldefault )
{
	int index;
	bool needActivate = false;
	s_SpewGroupsMutex.Lock();
	if( FindSpewGroup( pGroupName, &index ) )
	{
		auto pGroup = &s_pSpewGroups[ index ];
		needActivate = ( pGroup->m_Level == leveldefault );
	}
	s_SpewGroupsMutex.Unlock();
	if( needActivate )
		SpewAndLogActivate( pGroupName, level, level );
}

void SpewActivate( tchar const* pGroupName, int level )
{
	SpewAndLogActivate( pGroupName, level, level );
}

bool IsLogActive( const tchar *pGroupName, int iLogLevel )
{
	int iLogLevelRequired;
	s_SpewGroupsMutex.Lock();
	iLogLevelRequired = s_DefaultLogLevel;
	int index;
	if( FindSpewGroup( pGroupName, &index ) )
	{
		iLogLevelRequired = s_pSpewGroups[ index ].m_LogLevel;
	}
	s_SpewGroupsMutex.Unlock();
	return iLogLevelRequired >= iLogLevel;
}

bool IsSpewActive( tchar const* pGroupName, int level )
{
	int index;
	int active = 0;
	int def = 0;
	bool found = false;
	s_SpewGroupsMutex.Lock();
	found = FindSpewGroup( pGroupName, &index );
	if( found )
		active = s_pSpewGroups[ index ].m_Level;
	def = s_DefaultLevel;
	s_SpewGroupsMutex.Unlock();
	if( found )
		return level <= active;
	else
		return level <= def;
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

// RAII wrapper for CThreadMutex
class CAutoMutexLock
{
public:
	explicit CAutoMutexLock( CThreadMutex &m ) : m_mutex( m ), m_locked( true ) { m_mutex.Lock(); }
	~CAutoMutexLock() { if ( m_locked ) m_mutex.Unlock(); }
	void Unlock() { if ( m_locked ) { m_mutex.Unlock(); m_locked = false; } }
	bool IsLocked() const { return m_locked; }
private:
	CThreadMutex &m_mutex;
	bool m_locked;
	CAutoMutexLock( const CAutoMutexLock& ) = delete;
	CAutoMutexLock& operator=( const CAutoMutexLock& ) = delete;
};

SpewRetval_t SpewMessageType( SpewType_t spewType, const tchar* pMsgFormat, va_list args )
{
	tchar pTempBuffer[ 5020 ];

	static CThreadMutex autoMutex;

	CAutoMutexLock lock( autoMutex );

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

	// Do not hold lock across debugbreak/exit - release RAII lock now
	lock.Unlock();

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
	bool bShouldLog;
	s_SpewGroupsMutex.Lock();
	int index;
	if( FindSpewGroup( pGroupName, &index ) )
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	else
		bShouldLog = level <= s_DefaultLevel;
	s_SpewGroupsMutex.Unlock();

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
	bool bShouldLog;
	s_SpewGroupsMutex.Lock();
	int index;
	if( FindSpewGroup( pGroupName, &index ) )
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	else
		bShouldLog = level <= s_DefaultLevel;
	s_SpewGroupsMutex.Unlock();

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
	bool bShouldLog;
	s_SpewGroupsMutex.Lock();
	int index;
	if( FindSpewGroup( pGroupName, &index ) )
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	else
		bShouldLog = level <= s_DefaultLevel;
	s_SpewGroupsMutex.Unlock();

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
	bool bShouldLog;
	s_SpewGroupsMutex.Lock();
	int index;
	if( FindSpewGroup( pGroupName, &index ) )
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	else
		bShouldLog = level <= s_DefaultLevel;
	s_SpewGroupsMutex.Unlock();

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
	bool bShouldLog;
	s_SpewGroupsMutex.Lock();
	int index;
	if( FindSpewGroup( pGroupName, &index ) )
		bShouldLog = level <= s_pSpewGroups[ index ].m_Level;
	else
		bShouldLog = level <= s_DefaultLevel;
	s_SpewGroupsMutex.Unlock();

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
// VirtualQuery+IsReadableMemory before running the assert machinery
// (replacing deprecated APIs that are racy and trigger false positives).
// We mirror that exactly; DoNewAssertDialog is non-blocking in this build.

bool _AssertValidReadPtr( void* ptr, int count )
{
	bool bValid = ( count == 0 ) ? ( ptr != nullptr ) : IsReadableMemory( ptr, (SIZE_T)count );
	if ( bValid )
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
	bool bValid = ( count == 0 ) ? ( ptr != nullptr ) : IsWritableMemory( ptr, (SIZE_T)count );
	if ( bValid )
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
	bool bValid = false;
	if ( count == 0 )
		bValid = ( ptr != nullptr );
	else
		bValid = IsWritableMemory( ptr, (SIZE_T)count ) && IsReadableMemory( ptr, (SIZE_T)count );
	if ( bValid )
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
	// Original uses ANSI string check (1001003C), i.e. ANSI strings even on wide builds.
	bool bValid = IsValidStringPtrA( (LPCSTR)ptr, (SIZE_T)maxchar );
	if ( bValid )
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
	// Fixed: make atomic via CreateFile+WriteFile with FILE_APPEND_DATA and a process-wide mutex.
	// fopen("at+") used CRT buffering and was not atomic across threads/processes – concurrent
	// callers interleaved or truncated. Now we use Win32 atomic append (FILE_APPEND_DATA guarantees
	// the write lands at EOF atomically) plus a CThreadMutex to serialize formatting.
	static CThreadMutex s_SimpleLogMutex;
	char szBuf[ 512 ];
	// TCHAR is char in narrow builds; handle generic via _sntprintf compatible formatting
	int len = 0;
#if TCHAR_IS_CHAR
	len = _snprintf( szBuf, sizeof(szBuf)-1, "%s:%i\r\n", file ? file : "unknown", line );
#else
	// Wide build fallback – convert to UTF-8 via wide->narrow
	char narrowFile[ 260 ] = {0};
	if( file ) WideCharToMultiByte( CP_UTF8, 0, file, -1, narrowFile, sizeof(narrowFile)-1, NULL, NULL );
	len = _snprintf( szBuf, sizeof(szBuf)-1, "%s:%i\r\n", narrowFile, line );
#endif
	if( len < 0 ) len = (int)sizeof(szBuf)-1;
	if( (size_t)len >= sizeof(szBuf) ) len = (int)sizeof(szBuf)-1;
	szBuf[len] = '\0';

	s_SimpleLogMutex.Lock();
	HANDLE h = CreateFileA( "simple.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if( h == INVALID_HANDLE_VALUE )
	{
		s_SimpleLogMutex.Unlock();
		return nullptr;
	}
	DWORD written = 0;
	// FILE_APPEND_DATA makes the write atomic at the filesystem level (no SetFilePointer needed)
	WriteFile( h, szBuf, (DWORD)len, &written, NULL );
	// Flush to disk atomically so crash right after log still persists (optional, low volume)
	FlushFileBuffers( h );
	CloseHandle( h );
	s_SimpleLogMutex.Unlock();
	return nullptr;
}
