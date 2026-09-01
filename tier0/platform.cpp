// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#include "platform.h"

#if defined(_WIN32) || defined(WIN32)
#undef ARRAYSIZE
#include "winlite.h"
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof(p) / sizeof((p)[0]) )
#endif
#else
// Linux: need time headers for Plat_FloatTime (gettimeofday / clock_gettime)
// PreciseSleep on Linux uses clock_nanosleep(CLOCK_MONOTONIC,0,&ts,NULL) for 10.02ms QPC-parity (replaces usleep/Sleep)
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof(p) / sizeof((p)[0]) )
#endif
#endif

using VTuneFunc = int ( * )();

static VTuneFunc VTResumeFn = nullptr;
static VTuneFunc VTPauseFn = nullptr;

bool vtune( bool resume )
{
#if defined(WIN32) || defined(_WIN32)
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
	// Linux: no VTune
	(void)resume;
	return true;
#endif
}

PLATFORM_INTERFACE const tchar *Plat_GetCommandLine()
{
#if defined(_WIN32) || defined(WIN32)
#ifdef TCHAR_IS_CHAR
	return GetCommandLineA();
#else
	return GetCommandLineW();
#endif
#elif defined(_LINUX)
	// Linux: GoldSrc dedicated server passes command line via Plat_GetCommandLine
	// Keep a static buffer filled from /proc/self/cmdline if needed; fallback to empty.
	static char s_szCmdLine[ 2048 ] = {0};
	static bool s_bInit = false;
	if( !s_bInit )
	{
		s_bInit = true;
		// try to read /proc/self/cmdline for completeness
		FILE *f = fopen( "/proc/self/cmdline", "rb" );
		if( f )
		{
			size_t n = fread( s_szCmdLine, 1, sizeof(s_szCmdLine)-1, f );
			for( size_t i = 0; i < n; ++i )
				if( s_szCmdLine[i] == '\0' ) s_szCmdLine[i] = ' ';
			fclose( f );
		}
	}
	return s_szCmdLine;
#else
	return "";
#endif
}

#if defined(WIN32) || defined(_WIN32)
bool Plat_IsInDebugSession()
{
	return IsDebuggerPresent() != 0;
}
#else
// Linux stub -- no debugger detection via IsDebuggerPresent()
bool Plat_IsInDebugSession()
{
	return false;
}
#endif

#if defined(WIN32) || defined(_WIN32)
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
#if defined(WIN32) || defined(_WIN32)
	InitPlatTimer();

	LARGE_INTEGER PerformanceCount;
	if( g_bUseQPC && QueryPerformanceCounter( &PerformanceCount ) )
		return ( double )( PerformanceCount.QuadPart - g_PerfCount.QuadPart ) * g_Scale;

	return ( double )( GetTickCount() - g_TickBase ) * 0.001;
#else
	// Linux -- Valve's common/port.h style: monotonic clock.
	// Prefer clock_gettime(CLOCK_MONOTONIC) (nsec, not affected by NTP),
	// fallback to gettimeofday for very old toolchains.
	static bool s_bInit = false;
	static struct timespec s_tsBase = {0,0};
	static time_t s_secBase = 0;

	struct timespec ts;
	if( clock_gettime( CLOCK_MONOTONIC, &ts ) == 0 )
	{
		if( !s_bInit )
		{
			s_tsBase = ts;
			s_bInit = true;
			return 0.0;
		}
		double result = (double)( ts.tv_sec - s_tsBase.tv_sec ) + (double)ts.tv_nsec / 1e9;
		return result;
	}

	// fallback: gettimeofday (wall clock)
	struct timeval tp;
	gettimeofday( &tp, NULL );
	if( s_bInit && s_secBase != 0 )
	{
		return (double)( tp.tv_sec - s_secBase ) + (double)tp.tv_usec / 1e6;
	}
	else
	{
		// first call -- init base, return fractional part only like original Valve code
		if( !s_bInit )
		{
			s_bInit = true;
			s_secBase = tp.tv_sec;
			// keep monotonic base in sync if we later switch
			s_tsBase.tv_sec = tp.tv_sec;
			s_tsBase.tv_nsec = tp.tv_usec * 1000;
		}
		return (double)tp.tv_usec / 1e6;
	}
#endif
}

#if defined(_LINUX) || defined(POSIX)
// PreciseSleep helper for Linux parity with Windows QPC spin (10.02ms).
// Replaces Sleep/usleep with clock_nanosleep(CLOCK_MONOTONIC,0,&ts,NULL) for monotonic precise sleep.
PLATFORM_INTERFACE void Plat_PreciseSleep( unsigned ms )
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)( (ms % 1000) * 1000000L );
	while ( clock_nanosleep( CLOCK_MONOTONIC, 0, &ts, NULL ) == -1 && errno == EINTR ) {}
}
#endif
