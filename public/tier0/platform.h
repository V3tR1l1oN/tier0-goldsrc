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
#if defined(_LINUX) || defined(__linux__) || defined(linux) || defined(POSIX)
	// _LINUX takes precedence over _WIN32 (allows cross-compile with -D_LINUX on MSVC/MinGW)
	#ifdef _WIN32
	#undef _WIN32
	#endif
	#ifdef WIN32
	#undef WIN32
	#endif
	#ifdef _WINDOWS
	#undef _WINDOWS
	#endif
	#ifndef _LINUX
	#define _LINUX 1
	#endif
	#ifndef POSIX
	#define POSIX 1
	#endif
	// On MinGW, POSIX headers are Windows wrappers that require _WIN32; skip them in _LINUX mode.
	#if defined(__MINGW32__) || defined(__MINGW64__)
	#define _TIER0_MINGW_LINUX 1
	#endif
	// Native GCC/Clang x86 intrinsics (__rdtsc/_mm_pause) when available; the
	// inline-asm shims below are only used as a fallback.
	#if !defined(_TIER0_MINGW_LINUX) && !defined(_WIN32) && !defined(_MSC_VER)
	#if defined(__has_include)
	#  if __has_include(<x86intrin.h>)
	#    include <x86intrin.h>
	#    define _TIER0_X86INTRIN 1
	#  endif
	#endif
	#endif
	// Valve common/port.h -- Linux shims
	#ifndef _TIER0_MINGW_LINUX
	#if defined(__has_include)
	#  if __has_include(<dlfcn.h>)
	#    include <dlfcn.h>
	#  endif
	#  if __has_include(<unistd.h>)
	#    include <unistd.h>
	#  endif
	#  if __has_include(<sys/time.h>)
	#    include <sys/time.h>
	#  endif
	#  if __has_include(<sys/types.h>)
	#    include <sys/types.h>
	#  endif
	#  if __has_include(<time.h>)
	#    include <time.h>
	#  endif
	#  if __has_include(<strings.h>)
	#    include <strings.h>
	#  endif
	#  if __has_include(<limits.h>)
	#    include <limits.h>
	#  endif
	#else
	#  include <dlfcn.h>
	#  include <unistd.h>
	#  include <sys/time.h>
	#  include <sys/types.h>
	#  include <time.h>
	#  include <strings.h>
	#  include <limits.h>
	#endif
	#endif // _TIER0_MINGW_LINUX
	// Fallback definitions when POSIX headers not available (MSVC cross-compile check)
	#ifndef RTLD_NOW
	#define RTLD_NOW 0
	#define RTLD_NOLOAD 0
	#endif
	#ifndef RTLD_NOLOAD
	#define RTLD_NOLOAD 0
	#endif
	#if defined(_MSC_VER) && !defined(__has_include)
	// Provide dummy dlopen/dlsym/dlclose for MSVC _LINUX probe
	extern "C" inline void* dlopen(const char*, int) { return nullptr; }
	extern "C" inline void* dlsym(void*, const char*) { return nullptr; }
	extern "C" inline int dlclose(void*) { return 0; }
	// Dummy getpid/gettimeofday/usleep for MSVC probe
	extern "C" inline int getpid() { return 1; }
	struct timeval; // forward
	extern "C" inline int gettimeofday(struct timeval*, void*) { return 0; }
	extern "C" inline int usleep(unsigned int) { return 0; }
	extern "C" inline int strcasecmp(const char*, const char*) { return 0; }
	extern "C" inline int strncasecmp(const char*, const char*, size_t) { return 0; }
	#endif

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

	// string / snprintf -- Valve: _snprintf -> snprintf (skip on MinGW where UCRT already provides them)
	#ifndef _TIER0_MINGW_LINUX
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
	#endif // _TIER0_MINGW_LINUX
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

	// --- Win32 integer types missing on Linux ---
	#ifndef LONG_DEFINED
	#define LONG_DEFINED
	typedef long LONG;
	typedef long* PLONG;
	typedef long* LPLONG;
	typedef unsigned long* LPDWORD;
	typedef long LONG_PTR;
	typedef unsigned long ULONG_PTR;
	typedef unsigned long DWORD_PTR;
	typedef unsigned short WORD;
	typedef unsigned char BYTE;
	typedef unsigned long SIZE_T;
	typedef long HRESULT;
	typedef void* LPSECURITY_ATTRIBUTES;
	typedef void* LPLONG_PTR;
	#endif

	// pthread for CRITICAL_SECTION (Valve Linux shim) -- skip on MinGW where it requires _WIN32
	#ifndef _TIER0_MINGW_LINUX
	#if defined(__has_include)
	#  if __has_include(<pthread.h>)
	#    include <pthread.h>
	#    define _TIER0_HAS_PTHREAD 1
	#  endif
	#elif defined(__linux__) || defined(_LINUX)
	#  include <pthread.h>
	#  define _TIER0_HAS_PTHREAD 1
	#endif
	#endif // _TIER0_MINGW_LINUX

	// CRITICAL_SECTION -- Valve port.h: emulate Win32 CS with pthread recursive mutex
	#ifndef _CRITICAL_SECTION_DEFINED
	#define _CRITICAL_SECTION_DEFINED
	#if defined(_TIER0_HAS_PTHREAD)
	struct CRITICAL_SECTION
	{
		pthread_mutex_t mutex;
		long RecursionCount;
		void* OwningThread;
		long LockCount;
	};
	#else
	struct CRITICAL_SECTION
	{
		int dummy;
		long RecursionCount;
		void* OwningThread;
		long LockCount;
	};
	#endif
	typedef struct CRITICAL_SECTION CRITICAL_SECTION;
	typedef struct CRITICAL_SECTION* LPCRITICAL_SECTION;
	typedef struct CRITICAL_SECTION* PCRITICAL_SECTION;
	// extra Win32 CRITICAL_SECTION alias used by some code
	typedef CRITICAL_SECTION* LP_CRITICAL_SECTION;
	#endif

	// CRITICAL_SECTION helpers -- inline, no Windows dependency
	#if defined(_TIER0_HAS_PTHREAD)
	inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
	{
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&cs->mutex, &attr);
		pthread_mutexattr_destroy(&attr);
		cs->RecursionCount = 0;
		cs->OwningThread = nullptr;
		cs->LockCount = 0;
	}
	inline void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD) { InitializeCriticalSection(cs); }
	inline void DeleteCriticalSection(CRITICAL_SECTION* cs)
	{
		pthread_mutex_destroy(&cs->mutex);
		cs->RecursionCount = 0;
		cs->OwningThread = nullptr;
	}
	inline void EnterCriticalSection(CRITICAL_SECTION* cs)
	{
		pthread_mutex_lock(&cs->mutex);
		cs->RecursionCount++;
		cs->OwningThread = (void*)(DWORD_PTR)pthread_self();
	}
	inline void LeaveCriticalSection(CRITICAL_SECTION* cs)
	{
		if (cs->RecursionCount > 0) cs->RecursionCount--;
		if (cs->RecursionCount == 0) cs->OwningThread = nullptr;
		pthread_mutex_unlock(&cs->mutex);
	}
	inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs)
	{
		if (pthread_mutex_trylock(&cs->mutex) == 0)
		{
			cs->RecursionCount++;
			cs->OwningThread = (void*)(DWORD_PTR)pthread_self();
			return TRUE;
		}
		return FALSE;
	}
	#else
	inline void InitializeCriticalSection(CRITICAL_SECTION* cs) { cs->RecursionCount = 0; cs->OwningThread = nullptr; cs->LockCount = 0; cs->dummy = 0; }
	inline void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD) { InitializeCriticalSection(cs); }
	inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { cs->RecursionCount = 0; cs->OwningThread = nullptr; }
	inline void EnterCriticalSection(CRITICAL_SECTION* cs) { cs->RecursionCount++; }
	inline void LeaveCriticalSection(CRITICAL_SECTION* cs) { if (cs->RecursionCount > 0) cs->RecursionCount--; if (cs->RecursionCount==0) cs->OwningThread=nullptr; }
	inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { cs->RecursionCount++; return TRUE; }
	#endif

	// IsDebuggerPresent -- Linux stub (always false, matches Valve dedicated server)
	inline BOOL IsDebuggerPresent() { return FALSE; }

	// Additional Win32 API stubs used by threadtools.cpp / assert_dialog.cpp on Linux
	// These are minimal no-op / pthread-based shims to allow compilation with -D_LINUX.
	// Real implementations would map to pthread, but for header compilation they just need to exist.
	#ifndef _LINUX_SHIMS_DEFINED
	#define _LINUX_SHIMS_DEFINED
	// GetCurrentThreadId / GetCurrentThread / GetCurrentProcessId shims
	#if defined(_TIER0_HAS_PTHREAD)
	inline DWORD GetCurrentThreadId() { return (DWORD)(DWORD_PTR)pthread_self(); }
	#else
	inline DWORD GetCurrentThreadId() { return (DWORD)1; }
	#endif
	inline HANDLE GetCurrentThread() { return (HANDLE)(DWORD_PTR)GetCurrentThreadId(); }
	#if defined(_MSC_VER) || defined(_TIER0_MINGW_LINUX)
	inline DWORD GetCurrentProcessId() { return (DWORD)1; }
	#else
	inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
	#endif
	#ifdef GetCurrentProcess
	#undef GetCurrentProcess
	#endif
	inline HANDLE GetCurrentProcess() { return (HANDLE)(DWORD_PTR)GetCurrentProcessId(); }
	#if defined(_MSC_VER) || defined(_TIER0_MINGW_LINUX)
	inline DWORD GetTickCount() { return (DWORD)0; }
	inline void Sleep(DWORD) {}
	#else
	inline DWORD GetTickCount() { struct timeval tv; gettimeofday(&tv, nullptr); return (DWORD)(tv.tv_sec*1000 + tv.tv_usec/1000); }
	inline void Sleep(DWORD ms) { usleep(ms*1000); }
	#endif
	#if defined(_WIN32) && defined(_MSC_VER)
	inline void __debugbreak() { __debugbreak(); }
	#else
	#include <signal.h>
	inline void __debugbreak() { raise( SIGTRAP ); }
	#endif
	inline DWORD GetEnvironmentVariableA(LPCSTR, LPSTR, DWORD) { return 0; }
	inline BOOL CloseHandle(HANDLE) { return TRUE; }
	inline DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; } // WAIT_OBJECT_0
	inline DWORD WaitForMultipleObjects(DWORD, const HANDLE*, BOOL, DWORD) { return 0; }
	#ifndef WAIT_OBJECT_0
	#define WAIT_OBJECT_0 0
	#define WAIT_TIMEOUT 258
	#define WAIT_FAILED 0xFFFFFFFF
	#define WAIT_ABANDONED_0 0x80
	#define INFINITE 0xFFFFFFFF
	#endif
	#ifndef WAIT_ABANDONED
	#define WAIT_ABANDONED WAIT_ABANDONED_0
	#endif
	inline HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR) { return (HANDLE)1; }
	inline HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR) { return (HANDLE)1; }
	inline HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR) { return (HANDLE)1; }
	inline BOOL ReleaseMutex(HANDLE) { return TRUE; }
	inline BOOL SetEvent(HANDLE) { return TRUE; }
	inline BOOL ResetEvent(HANDLE) { return TRUE; }
	inline BOOL ReleaseSemaphore(HANDLE, LONG, LPLONG) { return TRUE; }
	inline BOOL GetExitCodeThread(HANDLE, LPDWORD lp) { if(lp) *lp = STILL_ACTIVE; return TRUE; }
	inline BOOL GetExitCodeProcess(HANDLE, LPDWORD lp) { if(lp) *lp = STILL_ACTIVE; return TRUE; }
	inline HANDLE OpenProcess(DWORD, BOOL, DWORD) { return (HANDLE)0; }
	inline int GetThreadPriority(HANDLE) { return 0; }
	inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
	inline DWORD SuspendThread(HANDLE) { return 0; }
	inline DWORD ResumeThread(HANDLE) { return 1; }
	inline BOOL TerminateThread(HANDLE, DWORD) { return TRUE; }
	inline DWORD TlsAlloc() { return 0; }
	inline BOOL TlsFree(DWORD) { return TRUE; }
	inline LPVOID TlsGetValue(DWORD) { return nullptr; }
	inline BOOL TlsSetValue(DWORD, LPVOID) { return TRUE; }
	inline DWORD GetLastError() { return 0; }
	inline void OutputDebugStringA(LPCSTR) {}
	#if defined(_MSC_VER)
	// MSVC does not have __sync_*; provide simple non-atomic fallback (probe only)
	inline LONG InterlockedCompareExchange(volatile LONG* dest, LONG exch, LONG comp) { LONG old = *dest; if (*dest == comp) *dest = exch; return old; }
	inline LONG InterlockedExchange(volatile LONG* dest, LONG val) { LONG old = *dest; *dest = val; return old; }
	inline LONG InterlockedIncrement(volatile LONG* dest) { return ++(*dest); }
	inline LONG InterlockedDecrement(volatile LONG* dest) { return --(*dest); }
	inline LONG InterlockedExchangeAdd(volatile LONG* dest, LONG val) { LONG old = *dest; *dest += val; return old; }
	inline void* InterlockedExchangePointer(void* volatile* dest, void* val) { void* old = *dest; *dest = val; return old; }
	inline void* InterlockedCompareExchangePointer(void* volatile* dest, void* exch, void* comp) { void* old = *dest; if (*dest == comp) *dest = exch; return old; }
	inline LONG InterlockedAnd(volatile LONG* dest, LONG val) { LONG old = *dest; *dest &= val; return old; }
	inline long long _InterlockedCompareExchange64(volatile long long* dest, long long exch, long long comp) { long long old = *dest; if (*dest == comp) *dest = exch; return old; }
	#else
	inline LONG InterlockedCompareExchange(volatile LONG* dest, LONG exch, LONG comp) { return __sync_val_compare_and_swap(dest, comp, exch); }
	inline LONG InterlockedExchange(volatile LONG* dest, LONG val) { return __sync_lock_test_and_set(dest, val); }
	inline LONG InterlockedIncrement(volatile LONG* dest) { return __sync_add_and_fetch(dest, 1); }
	inline LONG InterlockedDecrement(volatile LONG* dest) { return __sync_sub_and_fetch(dest, 1); }
	inline LONG InterlockedExchangeAdd(volatile LONG* dest, LONG val) { return __sync_fetch_and_add(dest, val); }
	inline void* InterlockedExchangePointer(void* volatile* dest, void* val) { return __sync_lock_test_and_set(dest, val); }
	inline void* InterlockedCompareExchangePointer(void* volatile* dest, void* exch, void* comp) { return __sync_val_compare_and_swap(dest, comp, exch); }
	inline LONG InterlockedAnd(volatile LONG* dest, LONG val) { return __sync_fetch_and_and(dest, val); }
	// _InterlockedCompareExchange64 as macro to avoid int64 forward-decl issue (works for any 64-bit type)
	#ifndef _InterlockedCompareExchange64
	#define _InterlockedCompareExchange64(dest, exch, comp) __sync_val_compare_and_swap((dest), (comp), (exch))
	#endif
	#endif
	inline DWORD timeBeginPeriod(unsigned int) { return 0; }
	inline DWORD timeEndPeriod(unsigned int) { return 0; }
	// _beginthreadex shim (Windows process.h) -> pthread_create minimal stub
	#if defined(_MSC_VER)
	inline uintptr_t _beginthreadex(void*, unsigned, unsigned (*)(void*), void*, unsigned, unsigned*) { return 0; }
	#else
	inline unsigned long _beginthreadex(void*, unsigned, unsigned long (*)(void*), void*, unsigned, unsigned*) { return 0; }
	#endif
	// _mm_pause shim for Linux (avoid redefinition on MSVC or where GCC provides
	// it natively via <x86intrin.h>/<immintrin.h>)
	#if !defined(_MM_PAUSE_DEFINED) && !defined(_WIN32) && !defined(_MSC_VER) && !defined(_TIER0_X86INTRIN)
	inline void _mm_pause() { __asm__ __volatile__("pause"); }
	#define _MM_PAUSE_DEFINED
	#endif
	#endif // _LINUX_SHIMS_DEFINED
#elif defined(_WIN32) || defined(WIN32) || defined(_WINDOWS)
	#ifndef _WIN32
	#define _WIN32 1
	#endif
	#define WIN32 1
	#ifndef _WIN32_WINNT
	#define _WIN32_WINNT 0x0601
	#endif
	#include <windows.h>
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
	#ifndef _TIER0_MINGW_LINUX
	#if defined(__has_include)
	#  if __has_include(<strings.h>)
	#    include <strings.h>
	#  endif
	#else
	#  include <strings.h>
	#endif
	#endif // _TIER0_MINGW_LINUX
	// Provide __rdtsc fallback for CCycleCount when the native header is absent.
	#if !defined(_WIN32) && !defined(_MSC_VER)
		#if !defined(_TIER0_X86INTRIN) && !defined(__rdtsc) && !defined(__RDTSC_DEFINED)
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
#define ALIGN16( def )		__declspec( align( 16 ) ) def
#else
#define NOINLINE			__attribute__((noinline))
#define ALIGN16( def )		def __attribute__((aligned(16)))
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
