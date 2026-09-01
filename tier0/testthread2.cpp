// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Test-thread harness + spew validation helpers restored from
//			the shipped tier0.dll.
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include "testthread.h"
#include "threadtools.h"

#ifdef _LINUX
#include <string.h>
#include <stdio.h>
#ifndef _T
#define _T(x) x
#endif
#ifndef _sntprintf
#define _sntprintf snprintf
#endif
#ifndef _vsntprintf
#define _vsntprintf vsnprintf
#endif
#ifndef strncpy_s
// Valve _TRUNCATE uses destSize; emulate via strncpy + NUL-terminate. Returns 0 on success.
#define strncpy_s(dst, dstSize, src, count) (strncpy((dst), (src), ((count)==_TRUNCATE ? (dstSize)-1 : (count))), (dst)[(dstSize)-1]='\0', 0)
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
// Minimal CreateThread shim for -D_LINUX: test harness is no-op on Linux (see Test_RunTest below)
// Real pthread_create mapping would require thread proc conversion; stub is sufficient for compilation and 1:1 inclusion.
#ifndef CreateThread
inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, DWORD (WINAPI *)(LPVOID), LPVOID, DWORD, LPDWORD) { return (HANDLE)1; }
#endif
#endif

#ifdef WIN32
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof( p ) / sizeof( (p)[0] ) )
#endif
#include "winlite.h"
#endif

//=============================================================================
// Spew validation
//=============================================================================

void ValidateSpew()
{
	SpewRetval_t retval;
	_SpewInfo( SPEW_MESSAGE, __FILE__, __LINE__ );

	retval = _SpewMessage( "%s", "" );
	AssertOnce( retval == SPEW_CONTINUE );

	_SpewInfo( SPEW_WARNING, __FILE__, __LINE__ );
	retval = _SpewMessage( "%s", "" );
	AssertOnce( retval == SPEW_CONTINUE );
}

SpewRetval_t _SpewMessageType( SpewType_t spewType, const tchar* pMsgFormat, va_list args )
{
	tchar pTempBuffer[ 5020 ];

	// NOTE: no mutex - static init order issues



	if ( spewType == SPEW_ASSERT )
		Test_SetFailed();

	size_t uiLeft;
	int iWritten;

	if ( spewType != SPEW_ASSERT )
	{
		uiLeft = sizeof( pTempBuffer ) - 1;
		iWritten = 0;
	}
	else
	{
		const tchar* s_pFileName = _T( "unknown" );
		UNREFERENCED_PARAMETER( s_pFileName );
		int s_Line = 0;

		iWritten = _sntprintf( pTempBuffer, ARRAYSIZE( pTempBuffer ) - 1, _T( "%s (%d) : " ), _T( "unknown" ), s_Line );

		if ( iWritten < 0 || (size_t)iWritten >= ARRAYSIZE( pTempBuffer ) - 1 )
			iWritten = ( int )( ARRAYSIZE( pTempBuffer ) - 1 ) - 1;

		pTempBuffer[ iWritten ] = _T( '\0' );

		uiLeft = ( sizeof( pTempBuffer ) - 1 ) - iWritten;
	}

	int iAppendedWritten = _vsntprintf( &pTempBuffer[ iWritten ], uiLeft, pMsgFormat, args );

	// _vsntprintf returns the would-be length on truncation (not -1) on modern
	// MSVC; clamping keeps that oversized value from indexing past the buffer.
	if ( iAppendedWritten < 0 || (size_t)iAppendedWritten >= uiLeft )
		iAppendedWritten = ( int )uiLeft - 1;

	if ( spewType == SPEW_ASSERT )
	{
		pTempBuffer[ iWritten + iAppendedWritten ] = '\n';
		pTempBuffer[ iWritten + iAppendedWritten + 1 ] = _T( '\0' );
	}

	SpewRetval_t retval = GetSpewOutputFunc()( spewType, pTempBuffer );

	if ( retval != SPEW_DEBUGGER )
	{
		if ( retval == SPEW_ABORT )
		{
			DMsg( _T( "console" ), 1, _T( "Exiting on SPEW_ABORT\n" ) );
			exit( 0 );
		}
	}
	else if ( spewType != SPEW_ASSERT )
	{
#ifdef WIN32
		DebugBreak();
#endif
	}



	return retval;
}

//=============================================================================
// Test harness
//=============================================================================

struct CTestHarness
{
	HANDLE					m_hThreadTestDriver;
	bool					m_bTestActive;
	bool					m_bTestThreadRunning;
	bool					m_bStopTestThread;
	bool					m_bLetTestThreadRun;
	bool					m_bTestFailed;

	ThreadId_t				m_ulTestThreadID;
	ThreadId_t				m_ulMainThreadID;

	void					*m_pvTestThreadArg;
	TestFunc				m_pTestFunc;
};

static CTestHarness g_TestHarness =
{
	NULL,
	false,
	false,
	false,
	true,
	false,
	NULL,
	NULL,
	NULL,
	NULL
};

struct TestThreadInfo_t
{
	CRITICAL_SECTION	cs;
	HANDLE				hRunTestThread;
	HANDLE				hRunMainThread;
};

static TestThreadInfo_t g_TestThreadInfo =
{
	{ 0 },
	NULL,
	NULL
};

static DWORD WINAPI TestThreadProc( LPVOID pvArg )
{
#ifdef _LINUX
	UNREFERENCED_PARAMETER( pvArg );
	return 0;
#else
	g_TestHarness.m_bTestThreadRunning = true;

	while ( !g_TestHarness.m_bStopTestThread )
	{
		WaitForSingleObject( g_TestThreadInfo.hRunTestThread, INFINITE );

		g_TestHarness.m_bTestFailed = false;

		if ( g_TestHarness.m_bLetTestThreadRun && g_TestHarness.m_pTestFunc )
		{
			g_TestHarness.m_pTestFunc( g_TestHarness.m_pvTestThreadArg );
		}

		SetEvent( g_TestThreadInfo.hRunMainThread );
	}

	g_TestHarness.m_bTestThreadRunning = false;

	return 0;
#endif
}

void Test_RunTest( TestFunc func, void *pvArg )
{
#ifdef _LINUX
	// Test harness is no-op on Linux; stub to allow g++ -D_LINUX compilation and 1:1 inclusion in tier0.so
	UNREFERENCED_PARAMETER( func );
	UNREFERENCED_PARAMETER( pvArg );
	return;
#else
	// One-time setup: a second Test_RunTest() used to re-init the critical
	// section while the driver thread might be inside it and to overwrite (and
	// leak) both event handles.
	if ( !g_TestThreadInfo.hRunTestThread )
	{
		InitializeCriticalSection( &g_TestThreadInfo.cs );
		g_TestThreadInfo.hRunTestThread	= CreateEventA( NULL, FALSE, FALSE, NULL );
		g_TestThreadInfo.hRunMainThread	= CreateEventA( NULL, FALSE, FALSE, NULL );
	}

	if ( !g_TestThreadInfo.hRunTestThread || !g_TestThreadInfo.hRunMainThread )
		return;

	g_TestHarness.m_ulMainThreadID	= ( ThreadId_t )( uintptr_t )GetCurrentThreadId();
	g_TestHarness.m_bTestActive		= true;
	g_TestHarness.m_bTestFailed		= false;
	g_TestHarness.m_pTestFunc		= func;
	g_TestHarness.m_pvTestThreadArg	= pvArg;

	if ( !g_TestHarness.m_hThreadTestDriver )
	{
		g_TestHarness.m_hThreadTestDriver = CreateThread( NULL, 0, TestThreadProc, NULL, 0, ( LPDWORD )&g_TestHarness.m_ulTestThreadID );
	}
	else
	{
		SetEvent( g_TestThreadInfo.hRunMainThread );
	}
#endif
}

void Test_RunFrame()
{
#ifdef _LINUX
	return;
#else
	AssertMsgOnce( GetCurrentThreadId() == ( DWORD )( uintptr_t )g_TestHarness.m_ulMainThreadID, "Main thread only call!" );

	SetEvent( g_TestThreadInfo.hRunTestThread );
	WaitForSingleObject( g_TestThreadInfo.hRunMainThread, INFINITE );
#endif
}

bool Test_IsActive()			{ return g_TestHarness.m_bTestActive; }
void Test_SetFailed()			{ g_TestHarness.m_bTestFailed = true; }
bool Test_HasFailed()			{ return g_TestHarness.m_bTestFailed; }

bool Test_HasFinished()
{
	return !g_TestHarness.m_bTestThreadRunning ? g_TestHarness.m_bTestActive : false;
}

void Test_TerminateThread()
{
#ifdef _LINUX
	return;
#else
	if ( g_TestHarness.m_bTestActive )
	{
		AssertMsgOnce( g_TestHarness.m_bTestThreadRunning || g_TestHarness.m_hThreadTestDriver != NULL, "No test thread to terminate!" );

		g_TestHarness.m_bStopTestThread = true;
		SetEvent( g_TestThreadInfo.hRunTestThread );

		if ( g_TestHarness.m_hThreadTestDriver )
		{
			WaitForSingleObject( g_TestHarness.m_hThreadTestDriver, INFINITE );
			CloseHandle( g_TestHarness.m_hThreadTestDriver );
			g_TestHarness.m_hThreadTestDriver = NULL;
		}

		DeleteCriticalSection( &g_TestThreadInfo.cs );
		CloseHandle( g_TestThreadInfo.hRunTestThread );
		CloseHandle( g_TestThreadInfo.hRunMainThread );

		g_TestHarness.m_bTestActive	= false;
		g_TestHarness.m_bTestFailed	= false;
	}
#endif
}

void TestThread_Yield()
{
	Sleep( 0 );
}
