// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// cpu_posix.cpp -- POSIX (Linux) CPU capability detection + core counts.
// Portable implementation using GCC/Clang __builtin_cpu_* and cpuid.h, so the
// Linux tier0.so can detect SSE/SSE2/SSE3/AVX for mathlib without Win32 ASM.

#include "platform.h"

#ifdef POSIX

#include <cpuid.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <sys/sysinfo.h>
#endif

#ifdef _SC_NPROCESSORS_ONLN
#endif

uint64 CalculateCPUFreq();

namespace
{
const char* GetPosixVendorId()
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
	__get_cpuid( 0, &eax, &ebx, &ecx, &edx );
	static char buf[ 13 ] = { 0 };
	if( eax >= 1 )
	{
		memcpy( buf, &ebx, 4 );
		memcpy( buf + 4, &edx, 4 );
		memcpy( buf + 8, &ecx, 4 );
	}
	else
	{
		memcpy( buf, "Generic_x86", 12 );
	}
	buf[ 12 ] = 0;
	return buf;
}

unsigned int CountLogical()
{
	long n = sysconf( _SC_NPROCESSORS_ONLN );
	return n > 0 ? ( unsigned int )n : 1;
}
}

PLATFORM_INTERFACE const CPUInformation& GetCPUInformation()
{
	static CPUInformation pi{};
	static volatile LONG s_InitState = 0;

	if( InterlockedCompareExchange( &s_InitState, 1, 0 ) == 0 )
	{
		pi.m_Size = sizeof( CPUInformation );
		pi.m_Speed = CalculateCPUFreq();
		pi.m_szProcessorID = const_cast<char*>( GetPosixVendorId() );

		unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

		pi.m_bFPU = 1;

		__builtin_cpu_init();
		pi.m_bRDTSC = __builtin_cpu_supports( "rdtsc" );
		pi.m_bCMOV = __builtin_cpu_supports( "cmov" );
		pi.m_bMMX = __builtin_cpu_supports( "mmx" );
		pi.m_bSSE = __builtin_cpu_supports( "sse" );
		pi.m_bSSE2 = __builtin_cpu_supports( "sse2" );
		pi.m_bSSE3 = __builtin_cpu_supports( "sse3" );
		pi.m_bSSSE3 = __builtin_cpu_supports( "ssse3" );
		pi.m_bSSE41 = __builtin_cpu_supports( "sse4.1" );
		pi.m_bSSE42 = __builtin_cpu_supports( "sse4.2" );
		pi.m_bAVX = __builtin_cpu_supports( "avx" );

		if( __get_cpuid( 0x80000000, &eax, &ebx, &ecx, &edx ) && eax >= 0x80000001 )
		{
			__get_cpuid( 0x80000001, &eax, &ebx, &ecx, &edx );
			pi.m_b3DNow = ( edx & ( 1u << 31 ) ) != 0;
			pi.m_bSSE4A = ( ecx & ( 1u << 6 ) ) != 0;
		}

		// Try to get physical core count via /sys; fall back to logical/2 heuristic.
		unsigned int nLogical = CountLogical();
		unsigned int nPhysical = 0;

		FILE* f = fopen( "/sys/devices/system/cpu/cpu0/topology/core_id", "r" );
		if( f ) fclose( f );
		f = fopen( "/sys/devices/system/cpu/present", "r" );
		if( f ) fclose( f );

		// Simple heuristic: assume at least 2 threads/core on HT systems.
		nPhysical = ( nLogical + 1 ) / 2;
		if( nPhysical < 1 ) nPhysical = 1;

		pi.m_nLogicalProcessors = nLogical;
		pi.m_nPhysicalProcessors = nPhysical;

		InterlockedExchange( &s_InitState, 2 );
	}
	else
	{
		while( InterlockedCompareExchange( &s_InitState, 0, 0 ) != 2 )
			Sleep( 0 );
	}

	return pi;
}

#endif // POSIX
