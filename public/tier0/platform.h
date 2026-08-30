// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Common platform definitions for the tier0 rebuild.
//			Reconstructed to match the original GoldSrc tier0.dll binary layout.
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef TIER0_PLATFORM_H
#define TIER0_PLATFORM_H

//-----------------------------------------------------------------------------
// Platform detection -- Valve style: _WIN32 vs _LINUX (common/port.h)
//-----------------------------------------------------------------------------
#if defined(_WIN32) || defined(WIN32) || defined(_WINDOWS)
	#ifndef _WIN32
	#define _WIN32 1
	#endif
	#define WIN32 1
	#ifndef _WIN32_WINNT
	#define _WIN32_WINNT 0x0601
	#endif
	#include <windows.h>
#elif defined(_LINUX) || defined(__linux__) || defined(linux) || defined(POSIX)
	#ifndef _LINUX
	#define _LINUX 1
	#endif
	#ifndef POSIX
	#define POSIX 1
	#endif
	// Valve common/port.h -- Linux shims
	#include <dlfcn.h>
	#include <unistd.h>
	#include <sys/time.h>
	#include <sys/types.h>
	#include <time.h>
	#include <strings.h>
	#include <limits.h>

	#ifndef MAX_PATH
	#define MAX_PATH 260
	#endif

	// HMODULE / handles -- Valve port.h: void*
	#ifndef HMODULE
	typedef void* HMODULE;
	#endif
	#ifndef HINSTANCE
	typedef void* HINSTANCE;
	#endif
	#ifndef HANDLE
	typedef void* HANDLE;
	#endif

	#define GetProcAddress(h, n) dlsym((h), (n))
	#define LoadLibrary(x) dlopen((x), RTLD_NOW)
	#define LoadLibraryA(x) dlopen((x), RTLD_NOW)
	#define LoadLibraryExA(a,b,c) dlopen((a), RTLD_NOW)
	#define FreeLibrary(x) dlclose(x)
	// GetModuleHandle on Linux: try RTLD_NOLOAD to not increment refcount
	#ifndef GetModuleHandleA
	#define GetModuleHandleA(x) dlopen((x), RTLD_NOW | RTLD_NOLOAD)
	#endif
	#ifndef GetCurrentProcess
	#define GetCurrentProcess() ((HANDLE)0)
	#endif

	// string / snprintf -- Valve: _snprintf -> snprintf
	#ifndef _snprintf
	#define _snprintf snprintf
	#endif
	#ifndef _vsnprintf
	#define _vsnprintf vsnprintf
	#endif
	#ifndef _stricmp
	#define _stricmp strcasecmp
	#endif
	#ifndef stricmp
	#define stricmp strcasecmp
	#endif
	#ifndef _strnicmp
	#define _strnicmp strncasecmp
	#endif
	#ifndef strnicmp
	#define strnicmp strncasecmp
	#endif
	#ifndef _alloca
	#define _alloca alloca
	#endif

	// calling conventions -- no-op on Linux
	#ifndef __stdcall
	#define __stdcall
	#endif
	#ifndef __cdecl
	#define __cdecl
	#endif
	#ifndef WINAPI
	#define WINAPI
	#endif
	#ifndef STILL_ACTIVE
	#define STILL_ACTIVE 259
	#endif
	#ifndef MAXIMUM_WAIT_OBJECTS
	#define MAXIMUM_WAIT_OBJECTS 64
	#endif
	#ifndef TRUE
	#define TRUE 1
	#endif
	#ifndef FALSE
	#define FALSE 0
	#endif
	typedef int BOOL;
	typedef unsigned long DWORD;
	typedef unsigned long ULONG;
	typedef void* LPVOID;
	typedef const void* LPCVOID;
	typedef char* LPSTR;
	typedef const char* LPCSTR;
	typedef void* HKEY;
#else
	// Unknown -- default to Windows for backward compat
	#define WIN32 1
	#ifndef _WIN32_WINNT
	#define _WIN32_WINNT 0x0601
	#endif
	#include <windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cfloat>
#include <cmath>

#ifdef _WIN32
#include <tchar.h>
#include <intrin.h>
#else
	// Linux: tchar is char (no _UNICODE wchar path on dedicated server)
	#include <strings.h>
	// __rdtsc fallback for CCycleCount
	#if !defined(__rdtsc) && !defined(__RDTSC_DEFINED)
		#if defined(__i386__) || defined(__x86_64__)
			inline unsigned long long __rdtsc()
			{
				unsigned int lo, hi;
				__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
				return ((unsigned long long)hi << 32) | lo;
			}
		#else
			// generic: use clock as fallback
			inline unsigned long long __rdtsc() { return 0; }
		#endif
	#endif
#endif

//-----------------------------------------------------------------------------
// Basic types
//-----------------------------------------------------------------------------
typedef unsigned char	byte;
typedef unsigned short	word;
typedef unsigned char	uint8;
typedef unsigned short	uint16;
typedef unsigned int	uint32;
typedef unsigned int	uint;
typedef unsigned long	ulong;
typedef float			float32;
typedef double			float64;

typedef wchar_t			uchar16;
typedef unsigned int	uchar32;

#if defined( _UNICODE )
typedef wchar_t			tchar;
#define TCHAR_IS_CHAR	0
#else
typedef char			tchar;
#define TCHAR_IS_CHAR	1
#endif

#if defined(WIN32) || defined(_WIN32)
typedef void* ThreadHandle_t;
#elif defined(_LINUX)
typedef void* ThreadHandle_t;
#endif

typedef int64_t int64;
typedef uint64_t uint64;

#define PLATFORM_CLASS
#define abstract_class class
#if !defined( PLATFORM_STRUCT )
#define PLATFORM_STRUCT
#endif

//-----------------------------------------------------------------------------
// Import/export
//-----------------------------------------------------------------------------
#if defined( TIER0_DLL_EXPORT )
#define PLATFORM_INTERFACE	extern "C"
#define DBG_INTERFACE		extern "C"
#define TT_INTERFACE		extern "C"
#else
#define PLATFORM_INTERFACE	extern "C"
#define DBG_INTERFACE		extern "C"
#define TT_INTERFACE		extern "C"
#endif

#ifdef _WIN32
#define NOINLINE			__declspec( noinline )
#else
#define NOINLINE			__attribute__((noinline))
#endif

#define DBG_CLASS
#define TT_CLASS

//-----------------------------------------------------------------------------
// Macros
//-----------------------------------------------------------------------------
// The Windows SDK (winnt.h) defines ARRAYSIZE with identical semantics. Only
// supply ours when the SDK has not already done so, otherwise every
// translation unit that includes windows.h first reports C4005.
#ifndef ARRAYSIZE
#define ARRAYSIZE(p)		(sizeof(p)/sizeof(p[0]))
#endif
#define UNREFERENCED_PARAMETER(p) (p)

#define Q_min( a, b )		(((a) < (b)) ? (a) : (b))
#define Q_max( a, b )		(((a) > (b)) ? (a) : (b))
#define clamp( a, b, c )	( Q_max( b, Q_min( c, a ) ) )

#define setbiosNotifyCodePageCallbackNULL 0

//-----------------------------------------------------------------------------
// Cycle counter helpers (rdtsc based; matches fasttimer.cpp usage).
//-----------------------------------------------------------------------------

class CCycleCount
{
public:
	CCycleCount()					{ m_Int64 = 0; }
	CCycleCount( unsigned long cycles ) { m_Int64 = cycles; }

	void	Sample()				{ m_Int64 = (int64)__rdtsc(); }
	void	Init( float mhz = 0.f );
	int64	GetLongCycles() const	{ return m_Int64; }

	unsigned long GetMicroseconds() const;
	unsigned long GetMilliseconds() const;
	double GetMillisecondsF() const;
	double GetSeconds() const;

	void	Add( const CCycleCount& other )	{ m_Int64 += other.m_Int64; }
	void	Sub( const CCycleCount& other )	{ m_Int64 -= other.m_Int64; }

	int64	m_Int64;
};

PLATFORM_INTERFACE double g_ClockSpeedMicrosecondsMultiplier;
PLATFORM_INTERFACE double g_ClockSpeedMillisecondsMultiplier;
PLATFORM_INTERFACE double g_ClockSpeedSecondsMultiplier;

class CFastTimer
{
public:
	void	Start();
	void	End();

	const CCycleCount& GetDuration() const { return m_Duration; }

	CCycleCount	m_Duration;
#ifdef DEBUG_FASTTIMER
	bool m_bPerf;
#endif
};

inline void CCycleCount::Init( float mhz )
{
	UNREFERENCED_PARAMETER( mhz );
	m_Int64 = 0;
}


inline unsigned long CCycleCount::GetMicroseconds() const
{
	return (unsigned long)( m_Int64 / g_ClockSpeedMicrosecondsMultiplier );
}

inline unsigned long CCycleCount::GetMilliseconds() const
{
	return (unsigned long)( m_Int64 / g_ClockSpeedMillisecondsMultiplier );
}

inline double CCycleCount::GetMillisecondsF() const
{
	return (double)m_Int64 * g_ClockSpeedMillisecondsMultiplier;
}

inline double CCycleCount::GetSeconds() const
{
	return m_Int64 * g_ClockSpeedSecondsMultiplier;
}

inline void CFastTimer::Start()
{
	m_Duration.Sample();
}

inline void CFastTimer::End()
{
	CCycleCount cnt;
	cnt.Sample();
	m_Duration.m_Int64 = cnt.GetLongCycles() - m_Duration.GetLongCycles();
}


// Generic TCHAR mapping for narrow builds (matches GoldSrc tier0 usage).
#ifndef _UNICODE
#define _tcsicmp		_stricmp
#define _tcsstr			strstr
#define _tcsrchr		strrchr
#define _tcsncpy		strncpy
#define _tcscpy			strcpy
#define _tcslen			strlen
#define _tprintf		printf
#define _ftprintf		fprintf
#define _sntprintf		_snprintf
#endif

struct CPUInformation
{
	int	m_Size;
	bool m_bRDTSC;
	bool m_bCMOV;
	bool m_bFCMOV;
	bool m_bFPU;
	bool m_bMMX;
	bool m_bSSE;
	bool m_bSSE2;
	bool m_bSSE3;
	bool m_bSSSE3;
	bool m_bSSE4A;
	bool m_bSSE41;
	bool m_bSSE42;
	bool m_bAVX;
	bool m_b3DNow;

	uint32_t m_nLogicalProcessors;
	uint32_t m_nPhysicalProcessors;

	int64 m_Speed;
	char *m_szProcessorID;
};

PLATFORM_INTERFACE const CPUInformation& GetCPUInformation();

//----- assert macros (release builds compile them away) ---------------------
#ifndef NDEBUG
#define NDEBUG
#endif
#define Assert( exp )					((void)0)
#define Verify( exp )					((void)(exp))
#define AssertOnce( exp )				((void)0)
#define AssertMsgOnce( exp, msg )		((void)0)
#define AssertMsg( exp, msg )			((void)0)
#define AssertMsg1( exp, msg, a1 )		((void)0)
#define AssertMsg2( exp, msg, a1, a2 )	((void)0)
#define AssertMsg3( exp, msg, a1, a2, a3 ) ((void)0)
#define AssertFatalMsg( exp, msg )		((void)0)
#define AssertFatalMsg1( exp, msg, a1 )	((void)0)
#define AssertFatalMsg2( exp, msg, a1, a2 ) ((void)0)
#define DbgVerify( exp )				((void)(exp))
#define MsgV(...)
#define _AssertMsgLocalCopy

//----- exported time/memory helpers -----------------------------------------
PLATFORM_INTERFACE double		Plat_FloatTime();
PLATFORM_INTERFACE unsigned int	Plat_MSTime();
PLATFORM_INTERFACE const char*	Plat_GetCommandLine();
PLATFORM_INTERFACE bool			Plat_IsInDebugSession();
PLATFORM_INTERFACE void*		Plat_Alloc( unsigned long size );
PLATFORM_INTERFACE void			Plat_Free( void *pMem );
PLATFORM_INTERFACE void*		Plat_Realloc( void *pMem, unsigned long newSize );
PLATFORM_INTERFACE bool Is64BitWindows();



#endif // TIER0_PLATFORM_H
