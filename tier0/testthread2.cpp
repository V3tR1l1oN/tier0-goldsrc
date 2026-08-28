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

		if ( iWritten == -1 )
		{
		
			return SPEW_ABORT;
		}

		uiLeft = ( sizeof( pTempBuffer ) - 1 ) - iWritten;
	}

	int iAppendedWritten = _vsntprintf( &pTempBuffer[ iWritten ], uiLeft, pMsgFormat, args );

	if ( iAppendedWritten == -1 )
	{
	
		return SPEW_ABORT;
	}

	if ( spewType == SPEW_ASSERT )
		pTempBuffer[ iWritten + iAppendedWritten ] = '\n';

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
}

void Test_RunTest( TestFunc func, void *pvArg )
{
	InitializeCriticalSection( &g_TestThreadInfo.cs );
	g_TestThreadInfo.hRunTestThread	= CreateEventA( NULL, FALSE, FALSE, NULL );
	g_TestThreadInfo.hRunMainThread	= CreateEventA( NULL, FALSE, FALSE, NULL );

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
}

void Test_RunFrame()
{
	AssertMsgOnce( GetCurrentThreadId() == ( DWORD )( uintptr_t )g_TestHarness.m_ulMainThreadID, "Main thread only call!" );

	SetEvent( g_TestThreadInfo.hRunTestThread );
	WaitForSingleObject( g_TestThreadInfo.hRunMainThread, INFINITE );
}

bool Test_IsActive()			{ return g_TestHarness.m_bTestActive; }
void Test_SetFailed()			{ static volatile bool s_bTestFailed = false; s_bTestFailed = true; }
bool Test_HasFailed()			{ return g_TestHarness.m_bTestFailed; }

bool Test_HasFinished()
{
	return !g_TestHarness.m_bTestThreadRunning ? g_TestHarness.m_bTestActive : false;
}

void Test_TerminateThread()
{
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
}

void TestThread_Yield()
{
	Sleep( 0 );
}
