// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#include "platform.h"

#undef ARRAYSIZE
#include "winlite.h"
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof(p) / sizeof((p)[0]) )
#endif

using VTuneFunc = int ( * )();

static VTuneFunc VTResumeFn = nullptr;
static VTuneFunc VTPauseFn = nullptr;

bool vtune( bool resume )
{
#ifdef WIN32
	static bool bLoaded = false;

	if( !bLoaded )
	{
		bLoaded = true;

		auto hLib = LoadLibraryA( "vtuneapi.dll" );

		if( hLib )
		{
			VTResumeFn = reinterpret_cast<decltype( VTResumeFn )>( GetProcAddress( hLib, "VTResume" ) );
			VTPauseFn = reinterpret_cast<decltype( VTPauseFn )>( GetProcAddress( hLib, "VTPause" ) );
		}
	}

	auto fn = resume ? VTResumeFn : VTPauseFn;

	if( !fn )
		return false;

	fn();

	return true;
#else
	return true;
#endif
}

PLATFORM_INTERFACE const tchar *Plat_GetCommandLine()
{
#ifdef TCHAR_IS_CHAR
	return GetCommandLineA();
#else
	return GetCommandLineW();
#endif
}

#ifdef WIN32
bool Plat_IsInDebugSession()
{
	return IsDebuggerPresent() != 0;
}
#endif

#ifdef WIN32
static LARGE_INTEGER g_Frequency = {};
static double g_Scale = 0.0;
static LARGE_INTEGER g_PerfCount = {};
static DWORD g_TickBase = 0;
static bool g_bUseQPC = false;
static volatile LONG g_TimerInitState = 0;

static void InitPlatTimer()
{
	if( InterlockedCompareExchange( &g_TimerInitState, 1, 0 ) == 0 )
	{
		LARGE_INTEGER frequency = {};
		LARGE_INTEGER base = {};
		const BOOL bHaveQPC = QueryPerformanceFrequency( &frequency )
			&& frequency.QuadPart != 0
			&& QueryPerformanceCounter( &base );

		g_TickBase = GetTickCount();
		if( bHaveQPC )
		{
			g_Frequency = frequency;
			g_Scale = 1.0 / ( double )frequency.QuadPart;
			g_PerfCount = base;
			g_bUseQPC = true;
		}
		else
		{
			// QueryPerformanceCounter is present on supported Windows versions,
			// but a broken/virtualized platform must still get a monotonic-ish
			// millisecond clock instead of dividing by an uninitialized frequency.
			g_Frequency.QuadPart = 1000;
			g_Scale = 1.0 / 1000.0;
			g_PerfCount.QuadPart = 0;
			g_bUseQPC = false;
		}

		// Publish all timer fields only after initialization is complete.
		InterlockedExchange( &g_TimerInitState, 2 );
	}
	else
	{
		while( InterlockedCompareExchange( &g_TimerInitState, 0, 0 ) != 2 )
			Sleep( 0 );
	}
}
#endif

double Plat_FloatTime()
{
#ifdef WIN32
	InitPlatTimer();

	LARGE_INTEGER PerformanceCount;
	if( g_bUseQPC && QueryPerformanceCounter( &PerformanceCount ) )
		return ( double )( PerformanceCount.QuadPart - g_PerfCount.QuadPart ) * g_Scale;

	return ( double )( GetTickCount() - g_TickBase ) * 0.001;
#else
	static int secbase = 0;

	long double result;
	timeval tp;

	gettimeofday( &tp, 0 );
	if( secbase )
	{
		result = ( long double ) ( tp.tv_sec - secbase ) + ( long double ) tp.tv_usec / 1000000.0;
		if( g_VCRMode )
			g_pVCR->Hook_Sys_FloatTime( result );
	}
	else
	{
		result = ( long double ) tp.tv_usec / 1000000.0;
		secbase = tp.tv_sec;
	}
	return result;
#endif
}