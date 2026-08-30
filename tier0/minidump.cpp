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

void MiniDumpWriter( unsigned int uExceptionCode, struct _EXCEPTION_POINTERS *pExceptionInfo );

static bool g_bWritingMiniDump = false;
static FnMiniDump g_MiniDumpFn = &MiniDumpWriter;
static volatile LONG g_CurrentMiniDumpThread = 0;
static int g_MiniDumpRecursionLevel = 0;
static int g_MiniDumpCount = 0;
static DWORD g_dwMiniDumpThreadId = 0;

PLATFORM_INTERFACE void WriteMiniDump()
{
	// CRITICAL FIX: NEVER raise an exception from WriteMiniDump!
	// The original raises a NONCONTINUABLE exception which is caught by
	// _set_se_translator from CatchAndWriteMiniDump. But when called from
	// other threads (hw.dll asserts, etc.) no translator exists and the
	// process is killed. Write the dump directly instead.

	g_bWritingMiniDump = true;

	EXCEPTION_POINTERS excPtrs;
	CONTEXT ctx;
	memset( &ctx, 0, sizeof( ctx ) );
	excPtrs.ExceptionRecord = nullptr;
	excPtrs.ContextRecord = &ctx;

	MiniDumpWriter( 0, &excPtrs );

	g_bWritingMiniDump = false;
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
	bool result;

	DWORD hThisThread = GetCurrentThreadId();

	if ( hThisThread != g_CurrentMiniDumpThread && InterlockedCompareExchange( &g_CurrentMiniDumpThread, hThisThread, 0 ) )
	{
		result = false;
	}
	else
	{
		result = true;
		++g_MiniDumpRecursionLevel;
	}

	return result;
}

int MiniDumpUnlock()
{
	if ( g_MiniDumpRecursionLevel-- == 1 )
		g_CurrentMiniDumpThread = 0;

	return g_MiniDumpRecursionLevel;
}

void MiniDumpWriter( unsigned int uExceptionCode, struct _EXCEPTION_POINTERS *pExceptionInfo )
{
	if ( !BGetMiniDumpLock() )
		return;

	HMODULE hDbgHelp = LoadLibraryA( "DbgHelp.dll" );

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

	if ( pWriteDump )
	{
		__time64_t Time = _time64( 0 );
		struct tm tmBuf;
		memset( &tmBuf, 0, sizeof( tmBuf ) );
		struct tm* v6 = _localtime64( &Time );
		++g_MiniDumpCount;
		struct tm * v7 = v6 ? v6 : &tmBuf;

		char Filename[ MAX_PATH ];

		Filename[ 0 ] = '\0';

		if ( GetModuleFileNameA( NULL, Filename, sizeof( Filename ) ) )
		{
			char* pszExt = strrchr( Filename, '.' );

			if ( pszExt )
				*pszExt = '\0';

			char* pszDirEnd = strrchr( Filename, '\\' );

			const char* pszFilename = pszDirEnd ? pszDirEnd + 1 : "unknown";

			const char* pszType = g_bWritingMiniDump ? "assert" : "crash";

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
				g_MiniDumpCount );

			// A truncated _snprintf leaves the buffer unterminated; CreateFileA
			// would then read past it. Force a terminator and bail out.
			if ( nWritten < 0 || (size_t)nWritten >= sizeof( FileName ) )
			{
				FreeLibrary( hDbgHelp );
				MiniDumpUnlock();
				return;
			}

			HANDLE hFile = CreateFileA( FileName, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );

			bool bSuccess = false;

			if ( NULL != hFile )
			{
				MINIDUMP_EXCEPTION_INFORMATION except;

				except.ThreadId = GetCurrentThreadId();
				except.ExceptionPointers = pExceptionInfo;
				except.ClientPointers = TRUE;

				bSuccess = pWriteDump( GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpWithDataSegs, &except, nullptr, nullptr ) != FALSE;
				CloseHandle( hFile );
			}

			if ( !bSuccess )
			{
				char szRenamed[ MAX_PATH ];

				// "(failed)" needs 8 more bytes than the name itself; if it does
				// not fit, leave the file under its original name rather than
				// producing an unterminated path.
				const int nRenamed = _snprintf( szRenamed, sizeof( szRenamed ), "(failed)%s", FileName );

				if ( nRenamed > 0 && (size_t)nRenamed < sizeof( szRenamed ) )
					rename( FileName, szRenamed );
			}

			CallFlushLogFunc();
		}
	}

	FreeLibrary( hDbgHelp );
	MiniDumpUnlock();
}

void WriteMiniDumpForException( unsigned int uExceptionCode, struct _EXCEPTION_POINTERS *pExceptionInfo )
{
	// Opt-in crash dump: only when TIER0_MD=1. Off by default so crash logging
	// stays lightweight for normal play; turn it on to get a real .mdmp with
	// the crashing registers alongside crash.log.
	if ( !uExceptionCode || !pExceptionInfo || !pExceptionInfo->ExceptionRecord )
		return;

	if ( !getenv( "TIER0_MD" ) || getenv( "TIER0_MD" )[ 0 ] != '1' )
		return;

	MiniDumpWriter( uExceptionCode, pExceptionInfo );
}

#else
PLATFORM_INTERFACE void WriteMiniDump()
{
}
#endif
