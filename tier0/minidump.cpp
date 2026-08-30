// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: MiniDump API (wraps DbgHelp.dll)
//
// $NoKeywords: $
//
//=============================================================================//

#include <cstring>
#include <ctime>
#include "dbg.h"

#ifdef WIN32
#undef ARRAYSIZE
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof( p ) / sizeof( (p)[0] ) )
#endif
#include "winlite.h"

// Disable "'typedef ': ignored on left of '' when no variable is declared"
#pragma warning( push )
#pragma warning( disable: 4091 )
#include "minidump.h"
#pragma warning( pop )

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

void MiniDumpWriter( unsigned int uExceptionCode, struct _EXCEPTION_POINTERS *pExceptionInfo );

static volatile LONG g_bWritingMiniDump = 0;
static FnMiniDump g_MiniDumpFn = &MiniDumpWriter;
static volatile LONG g_CurrentMiniDumpThread = 0;
static volatile LONG g_MiniDumpRecursionLevel = 0;
static volatile LONG g_MiniDumpCount = 0;
static thread_local DWORD g_dwMiniDumpThreadId = 0;

// Cached TIER0_MD env flag at startup
static volatile LONG g_Tier0MD_Cached = 0; // 0=not cached,1=caching,2=cached
static volatile LONG g_Tier0MD_Enabled = 0;

static bool IsTier0MDEnabled()
{
	if ( InterlockedCompareExchange( &g_Tier0MD_Cached, 0, 0 ) == 2 )
		return g_Tier0MD_Enabled != 0;
	if ( InterlockedCompareExchange( &g_Tier0MD_Cached, 1, 0 ) == 0 )
	{
		char buf[16];
		DWORD len = GetEnvironmentVariableA( "TIER0_MD", buf, sizeof( buf ) );
		bool enabled = ( len > 0 && len < sizeof( buf ) && buf[0] == '1' );
		InterlockedExchange( &g_Tier0MD_Enabled, enabled ? 1 : 0 );
		InterlockedExchange( &g_Tier0MD_Cached, 2 );
		return enabled;
	}
	// another thread is caching, spin until done
	while ( InterlockedCompareExchange( &g_Tier0MD_Cached, 0, 0 ) != 2 )
		Sleep( 0 );
	return g_Tier0MD_Enabled != 0;
}

// Ensure env is cached at DLL startup (called from DllMain or static init)
struct Tier0MD_CacheInit
{
	Tier0MD_CacheInit() { IsTier0MDEnabled(); }
};
static Tier0MD_CacheInit s_Tier0MDInit;

PLATFORM_INTERFACE void WriteMiniDump()
{
	// CRITICAL FIX: NEVER raise an exception from WriteMiniDump!
	// The original raises a NONCONTINUABLE exception which is caught by
	// _set_se_translator from CatchAndWriteMiniDump. But when called from
	// other threads (hw.dll asserts, etc.) no translator exists and the
	// process is killed. Write the dump directly instead.

	InterlockedExchange( &g_bWritingMiniDump, 1 );

	EXCEPTION_POINTERS excPtrs;
	CONTEXT ctx;
	memset( &ctx, 0, sizeof( ctx ) );
	excPtrs.ExceptionRecord = nullptr;
	excPtrs.ContextRecord = &ctx;

	MiniDumpWriter( 0, &excPtrs );

	InterlockedExchange( &g_bWritingMiniDump, 0 );
}

// Runs inside the SEH filter below. GetExceptionInformation() is only valid
// there, so this cannot be turned into an ordinary call in the handler body.
static LONG MiniDumpFilter( struct _EXCEPTION_POINTERS *pExceptionInfo )
{
	if ( pExceptionInfo && pExceptionInfo->ExceptionRecord )
		g_MiniDumpFn( pExceptionInfo->ExceptionRecord->ExceptionCode, pExceptionInfo );

	// Same contract the old _set_se_translator() hook had: it wrote the dump
	// and returned instead of rethrowing, so execution resumed afterwards.
	return EXCEPTION_EXECUTE_HANDLER;
}

PLATFORM_INTERFACE void CatchAndWriteMiniDump( FnWMain pfn, int argc, tchar *argv[] )
{
	const DWORD dwPrevThreadId = g_dwMiniDumpThreadId;

	g_dwMiniDumpThreadId = GetCurrentThreadId();

	if ( IsDebuggerPresent() )
	{
		// A debugger stops on the fault anyway; dumping from here would just
		// fight with it.
		pfn( argc, argv );
	}
	else
	{
		// NOTE: this used to install _set_se_translator(), but that only turns
		// structured exceptions into C++ exceptions when the module is built
		// with /EHa (warning C4535). This build uses /EHsc, so the translator
		// would never have fired for a hardware fault and no dump was ever
		// written. A real SEH frame works independently of the C++ exception
		// model and needs no translator.
		__try
		{
			pfn( argc, argv );
		}
		__except ( MiniDumpFilter( GetExceptionInformation() ) )
		{
		}
	}

	g_dwMiniDumpThreadId = dwPrevThreadId;
}

bool BGetMiniDumpLock()
{
	DWORD hThisThread = GetCurrentThreadId();

	if ( (DWORD)InterlockedCompareExchange( &g_CurrentMiniDumpThread, 0, 0 ) != hThisThread &&
	     InterlockedCompareExchange( &g_CurrentMiniDumpThread, (LONG)hThisThread, 0 ) != 0 )
	{
		return false;
	}
	else
	{
		InterlockedIncrement( &g_MiniDumpRecursionLevel );
		return true;
	}
}

int MiniDumpUnlock()
{
	LONG lvl = InterlockedDecrement( &g_MiniDumpRecursionLevel );
	if ( lvl == 0 )
		InterlockedExchange( &g_CurrentMiniDumpThread, 0 );

	return (int)lvl;
}

void MiniDumpWriter( unsigned int uExceptionCode, struct _EXCEPTION_POINTERS *pExceptionInfo )
{
	if ( !BGetMiniDumpLock() )
		return;

	HMODULE hDbgHelp = NULL;
	char szSystemDir[MAX_PATH];
	UINT sysLen = GetSystemDirectoryA( szSystemDir, ARRAYSIZE( szSystemDir ) );
	if ( sysLen > 0 && sysLen < ARRAYSIZE( szSystemDir ) )
	{
		char szPath[MAX_PATH];
		_snprintf( szPath, sizeof( szPath ), "%s\\DbgHelp.dll", szSystemDir );
		szPath[ sizeof( szPath ) - 1 ] = '\0';
		hDbgHelp = LoadLibraryExA( szPath, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 );
		if ( !hDbgHelp )
		{
			// Fallback for OS without KB2533623: load via full system path without flag.
			hDbgHelp = LoadLibraryExA( szPath, NULL, 0 );
		}
	}
	else
	{
		// Very unlikely: GetSystemDirectory failed, try secure load from system32
		hDbgHelp = LoadLibraryExA( "DbgHelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 );
		if ( !hDbgHelp )
			hDbgHelp = LoadLibraryExA( "C:\\Windows\\System32\\DbgHelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 );
	}

	if ( hDbgHelp == NULL )
	{
		MiniDumpUnlock();
		return;
	}

	auto pWriteDump = reinterpret_cast<BOOL ( WINAPI * )(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
			PMINIDUMP_EXCEPTION_INFORMATION,
			PMINIDUMP_USER_STREAM_INFORMATION,
			PMINIDUMP_CALLBACK_INFORMATION
		)>( GetProcAddress( hDbgHelp, "MiniDumpWriteDump" ) );

	if ( !pWriteDump )
	{
		FreeLibrary( hDbgHelp );
		MiniDumpUnlock();
		return;
	}

	__try
	{
		__time64_t Time = _time64( 0 );
		struct tm tmBuf;
		memset( &tmBuf, 0, sizeof( tmBuf ) );
		// Use secure version; on failure tmBuf stays zeroed
		_localtime64_s( &tmBuf, &Time );
		LONG cnt = InterlockedIncrement( &g_MiniDumpCount );
		struct tm * v7 = &tmBuf;

		char Filename[ MAX_PATH ];

		Filename[ 0 ] = '\0';

		if ( GetModuleFileNameA( NULL, Filename, sizeof( Filename ) ) )
		{
			char* pszExt = strrchr( Filename, '.' );

			if ( pszExt )
				*pszExt = '\0';

			char* pszDirEnd = strrchr( Filename, '\\' );

			const char* pszFilename = nullptr;
			if ( pszDirEnd )
				pszFilename = pszDirEnd + 1;
			else if ( Filename[ 0 ] != '\0' )
				pszFilename = Filename;
			else
				pszFilename = "unknown";

			const char* pszType = ( InterlockedCompareExchange( &g_bWritingMiniDump, 0, 0 ) != 0 ) ? "assert" : "crash";

			char FileName[ MAX_PATH ];

			const int nWritten = _snprintf(
				FileName,
				sizeof( FileName ),
				"%s_%s_%d%.2d%.2d%.2d%.2d%.2d_%d.mdmp",
				pszFilename,
				pszType,
				v7->tm_year + 1900,
				v7->tm_mon + 1,
				v7->tm_mday,
				v7->tm_hour,
				v7->tm_min,
				v7->tm_sec,
				(int)cnt );

			// A truncated _snprintf leaves the buffer unterminated; CreateFileA
			// would then read past it. Force a terminator and bail out.
			if ( nWritten < 0 || (size_t)nWritten >= sizeof( FileName ) )
			{
				// will be cleaned in __finally
				return;
			}

			HANDLE hFile = CreateFileA( FileName, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );

			bool bSuccess = false;

			if ( NULL != hFile && hFile != INVALID_HANDLE_VALUE )
			{
				MINIDUMP_EXCEPTION_INFORMATION except;

				except.ThreadId = GetCurrentThreadId();
				except.ExceptionPointers = pExceptionInfo;
				except.ClientPointers = FALSE;

				__try
				{
					bSuccess = pWriteDump( GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpWithDataSegs, &except, nullptr, nullptr ) != FALSE;
				}
				__finally
				{
					CloseHandle( hFile );
				}
			}

			if ( !bSuccess )
			{
				char szRenamed[ MAX_PATH ];

				// "(failed)" needs 8 more bytes than the name itself; if it does
				// not fit, leave the file under its original name rather than
				// producing an unterminated path.
				const int nRenamed = _snprintf( szRenamed, sizeof( szRenamed ), "(failed)%s", FileName );

				if ( nRenamed > 0 && (size_t)nRenamed < sizeof( szRenamed ) )
					MoveFileExA( FileName, szRenamed, MOVEFILE_REPLACE_EXISTING );
			}

			CallFlushLogFunc();
		}
	}
	__finally
	{
		FreeLibrary( hDbgHelp );
		MiniDumpUnlock();
	}
}

void WriteMiniDumpForException( unsigned int uExceptionCode, struct _EXCEPTION_POINTERS *pExceptionInfo )
{
	// Opt-in crash dump: only when TIER0_MD=1. Off by default so crash logging
	// stays lightweight for normal play; turn it on to get a real .mdmp with
	// the crashing registers alongside crash.log.
	if ( !uExceptionCode || !pExceptionInfo || !pExceptionInfo->ExceptionRecord )
		return;

	if ( !IsTier0MDEnabled() )
		return;

	MiniDumpWriter( uExceptionCode, pExceptionInfo );
}

#else
PLATFORM_INTERFACE void WriteMiniDump()
{
}
#endif
