// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Fast timer and CPU clock speed calculation.
//
//=============================================================================//

#include "platform.h"
#include "fasttimer.h"

#ifdef WIN32
#undef ARRAYSIZE
#include "winlite.h"
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof(p) / sizeof((p)[0]) )
#endif

PLATFORM_INTERFACE int64 g_ClockSpeed = 0;
PLATFORM_INTERFACE unsigned long g_dwClockSpeed = 0;

PLATFORM_INTERFACE double g_ClockSpeedMicrosecondsMultiplier = 0;
PLATFORM_INTERFACE double g_ClockSpeedMillisecondsMultiplier = 0;
PLATFORM_INTERFACE double g_ClockSpeedSecondsMultiplier = 0;

PLATFORM_INTERFACE int64 g_ulLastCycleSample = 0;
PLATFORM_INTERFACE int g_cBadCycleCountReceived = 0;

uint64 CalculateCPUFreq()
{
	LARGE_INTEGER PerformanceCount;
	LARGE_INTEGER Frequency;

	HANDLE hThisThread = GetCurrentThread();

	DWORD_PTR dwThreadAffinityMask = SetThreadAffinityMask( hThisThread, 1u );

	QueryPerformanceFrequency( &Frequency );
	const LONGLONG interval = Frequency.QuadPart >> 5;
	QueryPerformanceCounter( &PerformanceCount );

	uint64 cpuTimestamp = __rdtsc();

	if( ( int64 ) cpuTimestamp >= g_ulLastCycleSample || ( ++g_cBadCycleCountReceived, g_cBadCycleCountReceived >= 1000 ) )
	{
		g_ulLastCycleSample = cpuTimestamp;
		g_cBadCycleCountReceived = 0;
	}
	else
	{
		cpuTimestamp = g_ulLastCycleSample;
	}

	LARGE_INTEGER count;

	do
	{
		QueryPerformanceCounter( &count );
	}
	while( ( count.QuadPart - PerformanceCount.QuadPart ) < interval );

	uint64 endCPUTimestamp = __rdtsc();

	if( ( int64 ) endCPUTimestamp >= g_ulLastCycleSample || ( ++g_cBadCycleCountReceived, g_cBadCycleCountReceived >= 1000 ) )
	{
		g_ulLastCycleSample = endCPUTimestamp;
		g_cBadCycleCountReceived = 0;
	}
	else
	{
		endCPUTimestamp = g_ulLastCycleSample;
	}

	SetThreadAffinityMask( hThisThread, dwThreadAffinityMask );

	endCPUTimestamp -= cpuTimestamp;
	PerformanceCount.QuadPart = ( count.QuadPart - PerformanceCount.QuadPart );

	if ( PerformanceCount.QuadPart == 0 )
		return 2000000000ULL;

	return ( uint64 ) ( ( double ) ( int64 ) endCPUTimestamp / ( ( double ) PerformanceCount.QuadPart / ( double ) Frequency.QuadPart ) );
}

void InitFastTimer()
{
	if ( g_ClockSpeed == 0 )
	{
		g_ClockSpeed = (int64)CalculateCPUFreq();
		if ( g_ClockSpeed <= 0 )
			g_ClockSpeed = 2000000000LL;

		g_dwClockSpeed = (unsigned long)g_ClockSpeed;

		g_ClockSpeedMicrosecondsMultiplier = 1000000.0 / (double)g_ClockSpeed;
		g_ClockSpeedMillisecondsMultiplier = 1000.0 / (double)g_ClockSpeed;
		g_ClockSpeedSecondsMultiplier = 1.0 / (double)g_ClockSpeed;
	}
}

// ??????? ?????????????: ?????????? ??? ?????? ????????? ? ??????????
// (??????????????? ????? ????????? ??????? C++11)
static void EnsureFastTimerInit()
{
	static bool s_bInitialized = false;
	if ( !s_bInitialized )
	{
		InitFastTimer();
		s_bInitialized = true;
	}
}

#endif
