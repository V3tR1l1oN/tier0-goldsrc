// tier0 -- regression: the engine-communicates-with-the-PC contract.
// GetCPUInformation() powers engine decisions (SSE code paths, worker/loader
// thread pool sizing, clock speed warnings), so what it reports must match
// reality:
//   * processor counts reflect the CPU set this process may actually use
//     (process affinity), never the full machine when pinned;
//   * feature bits form a credible hierarchy (no SSE41 without SSE2, etc.);
//   * the exported clock data satisfies  speed * multiplier == 1e{3,6,9}.
// Compiles the shipped tier0/cpu.cpp and tier0/fasttimer.cpp into this exe
// (CPUInformation is not an exported symbol), i.e. tests exactly the same
// source the DLL is built from.
#include <stdio.h>
#include <stdlib.h>
#include <process.h>

#include "../tier0/cpu.cpp"
#include "../tier0/fasttimer.cpp"

extern "C" __declspec(dllimport) int __stdcall SetProcessAffinityMask( void *, unsigned long );

static int s_failures = 0;

#define CHECK( cond, fmt ) \
	do { if ( !( cond ) ) { printf( "FAIL: " fmt "\n" ); ++s_failures; } } while ( 0 )

static unsigned int PopCountUL( unsigned long x )
{
	unsigned int c = 0;
	while ( x ) { c += x & 1; x >>= 1; }
	return c;
}

struct CpuProbe_t
{
	HANDLE hStart;
	const CPUInformation *pInfo;
};

static unsigned __stdcall CpuProbeProc( void *pv )
{
	CpuProbe_t *pProbe = ( CpuProbe_t * )pv;
	WaitForSingleObject( pProbe->hStart, INFINITE );
	pProbe->pInfo = &GetCPUInformation();
	return 0;
}

int main( int argc, char **argv )
{
	// Optional: pin this process to N CPUs before detection, proving the
	// affinity-aware path. Build the mask from CPUs the process may already
	// use; sandbox/host restrictions often exclude the low mask bits.
	if ( argc > 1 && atoi( argv[ 1 ] ) > 0 )
	{
		const unsigned int n = ( unsigned int )atoi( argv[ 1 ] );
		unsigned long procAff = 0, systemAff = 0;
		if( GetProcessAffinityMask( GetCurrentProcess(), &procAff, &systemAff ) )
		{
			unsigned long mask = 0;
			unsigned int selected = 0;
			for( unsigned int bit = 0; bit < sizeof( unsigned long ) * 8 && selected < n; ++bit )
			{
				const unsigned long bitMask = 1UL << bit;
				if( procAff & bitMask )
				{
					mask |= bitMask;
					++selected;
				}
			}
			if( selected > 0 )
				SetProcessAffinityMask( ( void * )-1, mask );
		}
	}

	// Force first-use initialization through many callers at once. A plain
	// `m_Size == 0` guard used to let readers observe a partially filled
	// CPUInformation structure during startup.
	const int kProbeCount = 16;
	CpuProbe_t probes[ kProbeCount ] = {};
	HANDLE probeThreads[ kProbeCount ] = {};
	HANDLE hStart = CreateEventA( NULL, TRUE, FALSE, NULL );
	CHECK( hStart != NULL, "failed to create CPU initialization barrier" );
	if( hStart )
	{
		int created = 0;
		for( int i = 0; i < kProbeCount; ++i )
		{
			probes[ i ].hStart = hStart;
			probeThreads[ i ] = ( HANDLE )_beginthreadex( NULL, 0, CpuProbeProc, &probes[ i ], 0, NULL );
			if( probeThreads[ i ] )
				++created;
			else
				CHECK( false, "failed to create CPU probe thread" );
		}

		SetEvent( hStart );
		if( created > 0 )
		{
			HANDLE live[ kProbeCount ];
			int liveCount = 0;
			for( int i = 0; i < kProbeCount; ++i )
				if( probeThreads[ i ] )
					live[ liveCount++ ] = probeThreads[ i ];
			WaitForMultipleObjects( liveCount, live, TRUE, INFINITE );
		}

		for( int i = 0; i < kProbeCount; ++i )
			if( probeThreads[ i ] )
				CloseHandle( probeThreads[ i ] );
		CloseHandle( hStart );
	}

	const CPUInformation &pi = GetCPUInformation();
	for( int i = 0; i < kProbeCount; ++i )
		if( probes[ i ].pInfo )
			CHECK( probes[ i ].pInfo == &pi, "concurrent callers returned different CPUInformation storage" );

	CHECK( pi.m_Size == ( int )sizeof( CPUInformation ), "m_Size must equal sizeof(CPUInformation)" );
	CHECK( pi.m_szProcessorID && pi.m_szProcessorID[ 0 ], "vendor string empty" );
	CHECK( pi.m_nLogicalProcessors >= 1, "logical processors == 0" );
	CHECK( pi.m_nPhysicalProcessors >= 1 && pi.m_nPhysicalProcessors <= pi.m_nLogicalProcessors,
		"physical processors not in [1, logical]" );
	CHECK( pi.m_Speed > 50000000 && pi.m_Speed < 10000000000LL, "speed out of sane range" );

	// Feature hierarchy: well-formed reports only.
	CHECK( pi.m_bFPU && pi.m_bRDTSC, "any x86 CPU has FPU+RDTSC" );
	CHECK( pi.m_bSSE && pi.m_bSSE2, "x86-64 CPU must report SSE+SSE2" );
	CHECK( !pi.m_bSSE3 || pi.m_bSSE2, "SSE3 without SSE2" );
	CHECK( !pi.m_bSSSE3 || pi.m_bSSE3, "SSSE3 without SSE3" );
	CHECK( !pi.m_bSSE41 || pi.m_bSSE42 || pi.m_bSSSE3, "SSE41 without SSE3 family" );
	CHECK( !pi.m_bSSE42 || pi.m_bSSE41, "SSE42 without SSE41" );
	CHECK( !pi.m_bAVX || pi.m_bSSE42, "AVX without SSE42" );
	CHECK( !pi.m_bSSE4A || pi.m_bSSE2, "SSE4A without SSE2" );

	// Affinity contract: the reported logical count is what this process can
	// actually schedule. On <=64-core machines the flat affinity mask is exact
	// for the whole system, so the report must equal its popcount.
	unsigned long dwProcAff = 0, dwSysAff = 0;
	const int bAff = GetProcessAffinityMask( GetCurrentProcess(), &dwProcAff, &dwSysAff );
	if ( bAff )
	{
		const unsigned int machineLogical = PopCountUL( dwSysAff );
		const unsigned int allowedLogical = PopCountUL( dwProcAff );

		CHECK( pi.m_nLogicalProcessors <= machineLogical, "reports more logical CPUs than the machine has" );
		CHECK( pi.m_nPhysicalProcessors <= machineLogical, "reports more physical cores than the machine has" );

		if ( machineLogical <= 64 )
			CHECK( pi.m_nLogicalProcessors == allowedLogical,
				"logical count must match the process affinity mask" );
	}

	// Clock data consistency (same code path the exported g_* globals take).
	InitFastTimer();
	CHECK( g_dwClockSpeed > 0, "g_dwClockSpeed not initialized" );
	CHECK( g_ClockSpeed == ( int64 )g_dwClockSpeed, "g_ClockSpeed/g_dwClockSpeed disagree" );
	CHECK( g_ClockSpeedMillisecondsMultiplier * ( double )g_dwClockSpeed - 1e3 < 1.0,
		"ms multiplier breaks speed*mult == 1000" );
	CHECK( g_ClockSpeedMicrosecondsMultiplier * ( double )g_dwClockSpeed - 1e6 < 1e6,
		"us multiplier breaks speed*mult == 1e6" );

	if ( s_failures )
	{
		printf( "test_cpu: %u FAILURES\n", s_failures );
		return 1;
	}

	printf( "test_cpu: OK  logical=%u physical=%u speed=%dMHz vendor=%.12s\n",
		pi.m_nLogicalProcessors, pi.m_nPhysicalProcessors,
		( int )( pi.m_Speed / 1000000 ), pi.m_szProcessorID );
	return 0;
}