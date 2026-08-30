// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: win32 dependant ASM code for CPU capability detection
//
// $Workfile:     $
// $NoKeywords: $
//=============================================================================//
#include "platform.h"

#ifdef POSIX
#include "cpu_posix.cpp"
#elif _WIN32

#undef ARRAYSIZE
#include "winlite.h"
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof(p) / sizeof((p)[0]) )
#endif

#pragma optimize( "", off )
#pragma warning( disable: 4800 ) //'int' : forcing value to bool 'true' or 'false' (performance warning)

// --------------------------------------------------------------------------
bool CheckMMXTechnology( void )
{
	int retval = true;
	unsigned int RegEDX = 0;

#ifdef CPUID
	_asm pushad;
#endif

	__try
	{
		_asm
		{
#ifdef CPUID
			xor edx, edx	// Clue the compiler that EDX is about to be used.
#endif
			mov eax, 1      // set up CPUID to return processor version and features
							//      0 = vendor string, 1 = version info, 2 = cache info
							CPUID           // code bytes = 0fh,  0a2h
							mov RegEDX, edx // features returned in edx
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		retval = false;
	}

	// If CPUID not supported, then certainly no MMX extensions.
	if( retval )
	{
		if( RegEDX & 0x800000 )          // bit 23 is set for MMX technology
		{
			__try
			{
				// try executing the MMX instruction "emms"
				_asm EMMS
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				retval = false;
			}
		}

		else
			retval = false;           // processor supports CPUID but does not support MMX technology

									  // if retval == 0 here, it means the processor has MMX technology but
									  // floating-point emulation is on; so MMX technology is unavailable
	}

#ifdef CPUID
	_asm popad;
#endif

	return retval;
}
// --------------------------------------------------------------------------
bool CheckSSETechnology( void )
{
	int retval = true;
	unsigned int RegEDX = 0;

#ifdef CPUID
	_asm pushad;
#endif

	// Do we have support for the CPUID function?
	__try
	{
		_asm
		{
#ifdef CPUID
			xor edx, edx			// Clue the compiler that EDX is about to be used.
#endif
			mov eax, 1				// set up CPUID to return processor version and features
									//      0 = vendor string, 1 = version info, 2 = cache info
									CPUID					// code bytes = 0fh,  0a2h
									mov RegEDX, edx			// features returned in edx
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		retval = false;
	}

	// If CPUID not supported, then certainly no SSE extensions.
	if( retval )
	{
		// Do we have support for SSE in this processor?
		if( RegEDX & 0x2000000L )		// bit 25 is set for SSE technology
		{
			// Make sure that SSE is supported by executing an inline SSE instruction

			// BUGBUG, FIXME - Visual C Version 6.0 does not support SSE inline code YET (No macros from Intel either)
			// Fix this if VC7 supports inline SSE instructinons like "xorps" as shown below.
#if 1
			__try
			{
				_asm
				{
					// Attempt execution of a SSE instruction to make sure OS supports SSE FPU context switches
					xorps xmm0, xmm0
					// This will work on Win2k+ (Including masking SSE FPU exception to "normalized" values)
					// This will work on Win98+ (But no "masking" of FPU exceptions provided)
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
#endif

			{
				retval = false;
			}
		}
		else
			retval = false;
	}
#ifdef CPUID
	_asm popad;
#endif

	return retval;
}
bool CheckSSE2Technology( void )
{
	int retval = true;
	unsigned int RegEDX = 0;

#ifdef CPUID
	_asm pushad;
#endif

	// Do we have support for the CPUID function?
	__try
	{
		_asm
		{
#ifdef CPUID
			xor edx, edx			// Clue the compiler that EDX is about to be used.
#endif
			mov eax, 1				// set up CPUID to return processor version and features
									//      0 = vendor string, 1 = version info, 2 = cache info
									CPUID					// code bytes = 0fh,  0a2h
									mov RegEDX, edx			// features returned in edx
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		retval = false;
	}

	// If CPUID not supported, then certainly no SSE extensions.
	if( retval )
	{
		// Do we have support for SSE in this processor?
		if( RegEDX & 0x04000000 )		// bit 26 is set for SSE2 technology
		{
			// Make sure that SSE is supported by executing an inline SSE instruction

			__try
			{
				_asm
				{
					// Attempt execution of a SSE2 instruction to make sure OS supports SSE FPU context switches
					xorpd xmm0, xmm0
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )

			{
				retval = false;
			}
		}
		else
			retval = false;
	}
#ifdef CPUID
	_asm popad;
#endif

	return retval;
}

// --------------------------------------------------------------------------
bool Check3DNowTechnology( void )
{
	int retval = true;
	unsigned int RegEAX = 0;

#ifdef CPUID
	_asm pushad;
#endif

	// First see if we can execute CPUID at all
	__try
	{
		_asm
		{
#ifdef CPUID
			//			xor edx, edx			// Clue the compiler that EDX is about to be used.
#endif
			mov eax, 0x80000000     // setup CPUID to return whether AMD >0x80000000 function are supported.
									// 0x80000000 = Highest 0x80000000+ function, 0x80000001 = 3DNow support
									CPUID					// code bytes = 0fh,  0a2h
									mov RegEAX, eax			// result returned in eax
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		retval = false;
	}

	// If CPUID not supported, then there is definitely no 3DNow support
	if( retval )
	{
		// Are there any "higher" AMD CPUID functions?
		if( RegEAX > 0x80000000L )
		{
			__try
			{
				_asm
				{
					mov			eax, 0x80000001		// setup to test for CPU features
					CPUID							// code bytes = 0fh,  0a2h
					shr			edx, 31				// If bit 31 is set, we have 3DNow support!
					mov			retval, edx			// Save the return value for end of function
				}
			}
			__except( EXCEPTION_EXECUTE_HANDLER )
			{
				retval = false;
			}
		}
		else
		{
			// processor supports CPUID but does not support AMD CPUID functions
			retval = false;
		}
	}

#ifdef CPUID
	_asm popad;
#endif

	return retval;
}

/**
*	Wrapper around CPUID.
*/
bool cpuid( unsigned int instruction, unsigned int& outEax, unsigned int& outEbx, unsigned int& outEcx, unsigned int& outEdx )
{
	int retval = true;
	unsigned int RegEAX = 0;
	unsigned int RegEBX = 0;
	unsigned int RegECX = 0;
	unsigned int RegEDX = 0;

#ifdef CPUID
	_asm pushad;
#endif

	// First see if we can execute CPUID at all
	__try
	{
		_asm
		{
#ifdef CPUID
			//			xor edx, edx			// Clue the compiler that EDX is about to be used.
#endif
			mov eax, instruction

			CPUID
			mov RegEAX, eax
			mov RegEBX, ebx
			mov RegECX, ecx
			mov RegEDX, edx
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		retval = false;
	}

#ifdef CPUID
	_asm popad;
#endif

	outEax = RegEAX;
	outEbx = RegEBX;
	outEcx = RegECX;
	outEdx = RegEDX;

	return retval;
}

tchar* GetProcessorVendorId()
{
	static tchar VendorID[ 13 ];
	static volatile LONG s_InitState = 0;

	if( InterlockedCompareExchange( &s_InitState, 1, 0 ) == 0 )
	{
		unsigned int unused = 0;
		unsigned int VendorIDSegment[ 3 ] = {};

		memset( VendorID, 0, sizeof( VendorID ) );
		if( cpuid( 0, unused, VendorIDSegment[ 0 ], VendorIDSegment[ 2 ], VendorIDSegment[ 1 ] ) )
		{
			*reinterpret_cast<unsigned int*>( VendorID ) = VendorIDSegment[ 0 ];
			*( reinterpret_cast<unsigned int*>( VendorID ) + 1 ) = VendorIDSegment[ 1 ];
			*( reinterpret_cast<unsigned int*>( VendorID ) + 2 ) = VendorIDSegment[ 2 ];
		}
		else
		{
			_tcscpy( VendorID, _T( "Generic_x86" ) );
		}

		InterlockedExchange( &s_InitState, 2 );
	}
	else
	{
		while( InterlockedCompareExchange( &s_InitState, 0, 0 ) != 2 )
			Sleep( 0 );
	}

	return VendorID;
}

bool CheckGenuineIntel()
{
	bool result = false;
	unsigned int v2;
	unsigned int v3 = 0;
	unsigned int v4 = 0;
	unsigned int v5 = 0;
	unsigned int v6 = 0;
	unsigned int v7 = 0;

	if( cpuid( 0, v2, v5, v7, v6 )
		&& cpuid( 1, v3, v2, v2, v4 )
		&& ( ( v3 & 0xF00 ) == 0xF00 || v3 & 0xF00000 )
		&& v5 == 'uneG'
		&& v6 == 'Ieni'
		&& v7 == 'letn' )
		result = ( v4 >> 28 ) & 1;
	else
		result = 0;

	return result;
}

uint64 CalculateCPUFreq();

static unsigned int PopCount( unsigned int x )
{
	unsigned int c = 0;
	while ( x )
	{
		c += x & 1;
		x >>= 1;
	}
	return c;
}

static void FallbackProcessorCounts( uint32_t& nLogical, uint32_t& nPhysical,
	unsigned long dwProcAff, bool bRestricted )
{
	nLogical = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
	if( bRestricted )
	{
		const unsigned int allowed = PopCount( ( unsigned int )dwProcAff );
		if( allowed != 0 )
			nLogical = allowed;
	}
	nPhysical = nLogical;
}

extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall GetProcessAffinityMask(void *, unsigned long *, unsigned long *);

// Counts the total logical processors (threads) and physical cores via the
// modern Windows API (Win7+). Falls back to GetActiveProcessorCount, then to
// the legacy GetSystemInfo count, so the numbers stay sane on any OS.
//
// The engine consumes these counts to size its worker/loader thread pools, so
// they must reflect the CPU set this process is actually allowed to run on.
// Over-reporting the full machine when a process is pinned (affinity: launch
// options, hosting services, 'start /affinity') oversubscribes the pool and
// causes scheduling jitter. Each core/thread mask is therefore intersected
// with the process affinity mask; cores the process cannot use are dropped
// from both counts. Group 0 covers every core on <=64-logical machines; on
// larger systems the flat affinity mask only describes group 0 (noted below).
static void DetectProcessorCounts( uint32_t& nLogical, uint32_t& nPhysical )
{
	nLogical = 0;
	nPhysical = 0;

	unsigned long dwProcAff = 0, dwSysAff = 0;
	const int bRestricted = GetProcessAffinityMask( GetCurrentProcess(), &dwProcAff, &dwSysAff )
		&& dwProcAff != ( unsigned long )~0; // process pinned to a subset of CPUs

	DWORD dwSize = 0;
	GetLogicalProcessorInformationEx( RelationAll, nullptr, &dwSize ); // probe: required buffer size

	if( dwSize == 0 )
	{
		// The API can be unavailable or fail its size probe. Keep this fallback
		// affinity-aware just like the normal RelationAll path.
		FallbackProcessorCounts( nLogical, nPhysical, dwProcAff, bRestricted );
		return;
	}

	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pBuf
		= ( SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* ) malloc( dwSize );

	if( !pBuf )
	{
		// Allocation failure must not silently undo the process-affinity
		// contract. The old path reported every machine CPU here.
		FallbackProcessorCounts( nLogical, nPhysical, dwProcAff, bRestricted );
		return;
	}

	const BOOL rcAll = GetLogicalProcessorInformationEx( RelationAll, pBuf, &dwSize );
	if( rcAll )
	{
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* p = pBuf;
		size_t off = 0;

		while( off < dwSize )
		{
			// A zero-length record would leave `off` unchanged and spin forever.
			if( p->Size == 0 )
				break;

			switch( p->Relationship )
			{
				case RelationProcessorCore:
				{
					unsigned long coreMask = 0;
					for( unsigned int k = 0; k < p->Processor.GroupCount; ++k )
						coreMask |= ( unsigned long ) p->Processor.GroupMask[ k ].Mask;

					if( bRestricted )
						coreMask &= dwProcAff;
					if( coreMask == 0 )
						break; // core outside this process's affinity -- ignore entirely

					++nPhysical;
					nLogical += PopCount( coreMask );
					break;
				}
				default:
					break;
			}

			off += p->Size;
			p = ( SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* )( ( char* ) p + p->Size );
		}
	}

	free( pBuf );

	if( nLogical == 0 || nPhysical == 0 )
	{
		uint32_t fallbackLogical = 0;
		uint32_t fallbackPhysical = 0;
		FallbackProcessorCounts( fallbackLogical, fallbackPhysical, dwProcAff, bRestricted );
		if( nLogical == 0 )
			nLogical = fallbackLogical;
		if( nPhysical == 0 )
			nPhysical = fallbackPhysical;
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
		pi.m_szProcessorID = GetProcessorVendorId();

		unsigned int nEax = 0, nEbx = 0, nEcx = 0, nEdx = 0;
		unsigned int maxBasicLeaf = 0;

		if( cpuid( 0, maxBasicLeaf, nEbx, nEcx, nEdx )
			&& maxBasicLeaf >= 1
			&& cpuid( 1, nEax, nEbx, nEcx, nEdx ) )
		{
			pi.m_bFPU   = ( nEdx & ( 1u <<  0 ) ) != 0;
			pi.m_bRDTSC = ( nEdx & ( 1u <<  4 ) ) != 0;
			pi.m_bCMOV  = ( nEdx & ( 1u << 15 ) ) != 0;
			pi.m_bFCMOV = ( nEdx & ( 1u << 16 ) ) != 0;
			pi.m_bMMX   = ( nEdx & ( 1u << 23 ) ) != 0;
			pi.m_bSSE   = ( nEdx & ( 1u << 25 ) ) != 0;
			pi.m_bSSE2  = ( nEdx & ( 1u << 26 ) ) != 0;

			pi.m_bSSE3   = ( nEcx & ( 1u <<  0 ) ) != 0;
			pi.m_bSSSE3  = ( nEcx & ( 1u <<  9 ) ) != 0;
			pi.m_bSSE41  = ( nEcx & ( 1u << 19 ) ) != 0;
			pi.m_bSSE42  = ( nEcx & ( 1u << 20 ) ) != 0;

			// AVX additionally requires OS-level XSAVE support (OSXSAVE = ECX bit 27).
			pi.m_bAVX = ( nEcx & ( 1u << 28 ) ) != 0 && ( nEcx & ( 1u << 27 ) ) != 0;
		}

		unsigned int nEax2 = 0, nEbx2 = 0, nEcx2 = 0, nEdx2 = 0;
		unsigned int maxExtendedLeaf = 0;

		if( cpuid( 0x80000000, maxExtendedLeaf, nEbx2, nEcx2, nEdx2 )
			&& maxExtendedLeaf >= 0x80000001
			&& cpuid( 0x80000001, nEax2, nEbx2, nEcx2, nEdx2 ) )
		{
			pi.m_b3DNow = ( nEdx2 & ( 1u << 31 ) ) != 0;
			pi.m_bSSE4A = ( nEcx2 & ( 1u <<  6 ) ) != 0;
		}

		DetectProcessorCounts( pi.m_nLogicalProcessors, pi.m_nPhysicalProcessors );

		if( !pi.m_nLogicalProcessors )
			pi.m_nLogicalProcessors = 1;
		if( !pi.m_nPhysicalProcessors )
			pi.m_nPhysicalProcessors = pi.m_nLogicalProcessors;

		// Publish the fully populated structure only after every field is ready.
		InterlockedExchange( &s_InitState, 2 );
	}
	else
	{
		while( InterlockedCompareExchange( &s_InitState, 0, 0 ) != 2 )
			Sleep( 0 );
	}

	return pi;
}

#pragma optimize( "", on )

#endif // _WIN32
