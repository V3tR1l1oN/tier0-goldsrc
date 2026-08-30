// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Threading primitives implementation (restored from tier0.dll).
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include "threadtools.h"

#include <process.h>
#include <intrin.h>
#include <stdlib.h>

#ifdef WIN32
static DWORD g_ThreadMainThreadID = GetCurrentThreadId();
#endif

static CThreadLocalPtr<CThread> g_pCurThread;

//=============================================================================
// 64-bit atomics: x86 has no intrinsic forms of these, so they are built on
// a single-shot lock cmpxchg8b CAS loop exactly like the shipped binary.
//=============================================================================

int64 ThreadInterlockedEmu_CompareExchange64( int64 volatile *pDest, int64 value, int64 comperand )
{
#if defined( _M_IX86 )
	unsigned long *pd	= (unsigned long *)pDest;
	unsigned long *pv	= (unsigned long *)&value;
	unsigned long *pc	= (unsigned long *)&comperand;
	unsigned long retLo, retHi;

	// cmpxchg8b [esi] compares EDX:EAX against the destination and, on a
	// match, stores ECX:EBX into it. BOTH halves of the store pair must come
	// from `value` and BOTH halves of the compare pair from `comperand`.
	// Loading only EAX/EDX from the destination (instead of from `comperand`)
	// made every CAS "match", and leaving ECX/EBX uninitialised stored
	// whatever those registers happened to hold -- silently trashing the
	// destination and breaking every helper layered on top of this one.
	__asm
	{
		push	edi
		push	ebx

		mov		esi, pd

		mov		edi, pv
		mov		ebx, [edi]      // low  half of the value to store
		mov		ecx, [edi+4]    // high half of the value to store

		mov		edi, pc
		mov		eax, [edi]      // low  half of the comparand
		mov		edx, [edi+4]    // high half of the comparand

		lock	cmpxchg8b qword ptr [esi]

		mov		retLo, eax
		mov		retHi, edx

		pop		ebx
		pop		edi
	}

	return ( int64 )( ( ( unsigned __int64 )retHi << 32 ) | retLo );
#else
	return _InterlockedCompareExchange64( pDest, value, comperand );
#endif
}

int64 ThreadInterlockedEmu_Increment64( volatile int64 *pDest )
{
	int64 Old;
	do { Old = ThreadInterlockedEmu_CompareExchange64( pDest, 0, 0 ); }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, Old + 1, Old ) != Old );
	return Old + 1;
}

int64 ThreadInterlockedEmu_Decrement64( volatile int64 *pDest )
{
	int64 Old;
	do { Old = ThreadInterlockedEmu_CompareExchange64( pDest, 0, 0 ); }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, Old - 1, Old ) != Old );
	return Old - 1;
}

int64 ThreadInterlockedEmu_Exchange64( volatile int64 *pDest, int64 value )
{
	int64 Old;
	do { Old = ThreadInterlockedEmu_CompareExchange64( pDest, 0, 0 ); }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, value, Old ) != Old );
	return Old;
}

int64 ThreadInterlockedEmu_ExchangeAdd64( volatile int64 *pDest, int64 value )
{
	int64 Old;
	do { Old = ThreadInterlockedEmu_CompareExchange64( pDest, 0, 0 ); }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, Old + value, Old ) != Old );
	return Old;
}

//=============================================================================
// Thread API
//=============================================================================

//-----------------------------------------------------------------------------
// Precise sleep. Bare ::Sleep() on Windows quantizes to the system timer
// (~15.625 ms), so a 100/128 tick server thread that sleeps 10/7.8 ms actually
// wakes every ~15.6 ms and its tick rate collapses toward 64 -- the engine's
// clock lags the simulation and client-side interpolation gets stretchy/jerky.
// Hybrid: ::Sleep() for the coarse part, then busy-wait the last millisecond
// against QPC with pause (no extra system timer resolution, no wakeups).
// Set TIER0_PRECISE_SLEEP=0 to restore legacy coarse behavior.
//-----------------------------------------------------------------------------

bool BPreciseSleepEnabled()
{
	// Environment lookup is lazy, but this function is commonly called from
	// several engine threads during startup. Publish the result atomically so
	// no caller observes a partially initialized mode.
	static volatile LONG s_mode = -1;
	LONG mode = InterlockedCompareExchange( &s_mode, -1, -1 );
	if( mode == -1 )
	{
		if( InterlockedCompareExchange( &s_mode, -2, -1 ) == -1 )
		{
			char buf[ 8 ] = { 0 };
			DWORD len = GetEnvironmentVariableA( "TIER0_PRECISE_SLEEP", buf, sizeof( buf ) );
			InterlockedExchange( &s_mode, ( len && buf[ 0 ] == '0' ) ? 0 : 1 );
		}
		mode = InterlockedCompareExchange( &s_mode, -1, -1 );
	}
	while( mode == -2 )
	{
		Sleep( 0 );
		mode = InterlockedCompareExchange( &s_mode, -1, -1 );
	}
	return mode != 0;
}

// timeBeginPeriod() comes from the SDK header (timeapi.h). It used to be
// re-declared here without dllimport, which made every inclusion of
// <windows.h> report C4273 "inconsistent dll linkage".

static double SlpNow()
{
	// Reuse the process-wide, once-initialized timer. The previous local
	// frequency/base pair was initialized with a plain bool, so concurrent
	// PreciseSleep callers could divide by zero or observe a half-written base.
	return Plat_FloatTime();
}

//-----------------------------------------------------------------------------
// High-resolution system timer. Raising the global timer resolution with
// timeBeginPeriod(1) is a process-wide, machine-wide effect -- it makes every
// process's Sleep/timers tick at 1 ms granularity and keeps background
// wakeups/power draw elevated for as long as the call is outstanding, which is
// also true for the other processes on the box. We raise it once when precise
// sleep first needs it, and pair it with timeEndPeriod(1) on DLL unload so we
// do not leak a system-wide side effect for the lifetime of the process.
//
// Success is a strict superset of what precise pacing needs: with the coarse
// ::Sleep quantized to the ~15.6 ms system timer, a 10 ms tick wakes at 15.6 ms
// no matter how long the QPC tail spins. Set TIER0_HIGHRES_TIMER=0 to opt out
// of touching the global timer resolution entirely (the pace degrades back
// toward the legacy behavior; users who need that are normally also setting
// TIER0_PRECISE_SLEEP=0).
//-----------------------------------------------------------------------------

static bool BHighResTimerEnabled()
{
	static volatile LONG s_mode = -1;
	LONG mode = InterlockedCompareExchange( &s_mode, -1, -1 );
	if( mode == -1 )
	{
		if( InterlockedCompareExchange( &s_mode, -2, -1 ) == -1 )
		{
			char buf[ 8 ] = { 0 };
			DWORD len = GetEnvironmentVariableA( "TIER0_HIGHRES_TIMER", buf, sizeof( buf ) );
			InterlockedExchange( &s_mode, ( len && buf[ 0 ] == '0' ) ? 0 : 1 );
		}
		mode = InterlockedCompareExchange( &s_mode, -1, -1 );
	}
	while( mode == -2 )
	{
		Sleep( 0 );
		mode = InterlockedCompareExchange( &s_mode, -1, -1 );
	}
	return mode != 0;
}

// timeBeginPeriod state machine, file-scope so teardown can pair the raise:
//   0 = not raised yet, 1 = raise in progress, 2 = raised (owned),
//   3 = raise failed, 4 = released via timeEndPeriod.
static volatile LONG s_HighResState = 0;

void Tier0ShutdownHighResTimer()
{
	// Pair the raise. Only the thread that owns the raise (state 2) may release
	// it; the CAS from 2 -> 4 claims that ownership exactly once, so a detach
	// racing a late PreciseSleep caller can never release someone else's raise
	// or release twice. If state is 1 (raise in progress) wait briefly then
	// release if it became 2; state 3 (failed) just resets to 4.
	LONG s = InterlockedCompareExchange( &s_HighResState, 0, 0 );
	if ( s == 2 )
	{
		if( InterlockedCompareExchange( &s_HighResState, 4, 2 ) == 2 )
			timeEndPeriod( 1 );
	}
	else if ( s == 1 )
	{
		DWORD t0 = GetTickCount();
		while ( ( s = InterlockedCompareExchange( &s_HighResState, 0, 0 ) ) == 1 )
		{
			if ( GetTickCount() - t0 > 100 )
				break;
			Sleep( 0 );
		}
		if( InterlockedCompareExchange( &s_HighResState, 4, 2 ) == 2 )
			timeEndPeriod( 1 );
		else if ( s == 3 )
			InterlockedCompareExchange( &s_HighResState, 4, 3 );
	}
	else if ( s == 3 )
	{
		InterlockedCompareExchange( &s_HighResState, 4, 3 );
	}
}

static void PreciseSleepRaiseTimer()
{
	if( !BHighResTimerEnabled() )
		return;

	if( InterlockedCompareExchange( &s_HighResState, 1, 0 ) == 0 )
	{
		const LONG state = ( timeBeginPeriod( 1 ) == 0 ) ? 2 : 3;
		InterlockedExchange( &s_HighResState, state );
	}
	else
	{
		// If the thread that claimed 1 died, don't spin forever - timeout ~100ms then reset.
		DWORD t0 = GetTickCount();
		while( InterlockedCompareExchange( &s_HighResState, 0, 0 ) == 1 )
		{
			if ( GetTickCount() - t0 > 100 )
			{
				// Try to recover: if still 1, reset to 0 and let next caller retry.
				InterlockedCompareExchange( &s_HighResState, 0, 1 );
				break;
			}
			Sleep( 0 );
		}
	}
}

void PreciseSleep( unsigned duration )
{
	if ( duration == 0 )
	{
		::Sleep( 0 );
		return;
	}

	if ( !BPreciseSleepEnabled() )
	{
		::Sleep( duration );
		return;
	}

	// Short waits spin too much CPU for nothing, long waits are dominated by
	// ::Sleep's own accuracy anyway. For the tick-pacing sweet spot (3..50 ms)
	// first raise the system timer to 1 ms (once, for process lifetime -- the
	// standard game practice), then sleep most of the duration and spin the
	// remaining ~1 ms against QPC.
	const unsigned kSpinHeadroom = 1;
	if ( duration < 3 || duration > 50 )
	{
		::Sleep( duration );
		return;
	}

	PreciseSleepRaiseTimer();

	const double target = ( double )duration / 1000.0;
	const double tStart = SlpNow();
	::Sleep( duration > kSpinHeadroom + 1 ? duration - kSpinHeadroom : 1 );

	const double spinLimit = target + 0.005; // +5ms cap against clock skew
	for ( ;; )
	{
		double elapsed = SlpNow() - tStart;
		if ( elapsed >= target || elapsed >= spinLimit || elapsed < -0.001 )
			break;
		_mm_pause();
		if ( elapsed > 0.002 )
			Sleep( 0 ); // yield after 2ms spin to avoid 100% CPU on clock jump
	}
}

void ThreadSleep( unsigned duration )
{
	PreciseSleep( duration );
}

uint ThreadGetCurrentId()
{
	return GetCurrentThreadId();
}

uint ThreadGetCurrentProcessId()
{
	return GetCurrentProcessId();
}

int ThreadGetPriority( ThreadHandle_t hThread )
{
	HANDLE hTargetThread = hThread;

	if ( !hThread )
		hTargetThread = GetCurrentThread();

	return GetThreadPriority( hTargetThread );
}

bool ThreadInMainThread()
{
	return GetCurrentThreadId() == g_ThreadMainThreadID;
}

long ThreadInterlockedIncrement( long volatile * pDest )			{ return InterlockedIncrement( pDest ); }
long ThreadInterlockedDecrement( long volatile * pDest )			{ return InterlockedDecrement( pDest ); }
long ThreadInterlockedExchange( long volatile * pDest, long value )	{ return InterlockedExchange( pDest, value ); }
long ThreadInterlockedExchangeAdd( long volatile * pDest, long value ){ return InterlockedExchangeAdd( pDest, value ); }
long ThreadInterlockedCompareExchange( long volatile * pDest, long value, long comperand ) { return InterlockedCompareExchange( pDest, value, comperand ); }

void *ThreadInterlockedExchangePointer( void * volatile * pDest, void *value )
{
	return InterlockedExchangePointer( pDest, value );
}

void *ThreadInterlockedCompareExchangePointer( void * volatile * pDest, void *value, void *comperand )
{
	return InterlockedCompareExchangePointer( pDest, value, comperand );
}

int64 ThreadInterlockedIncrement64( int64 volatile *pDest )				{ return ThreadInterlockedEmu_Increment64( pDest ); }
int64 ThreadInterlockedDecrement64( int64 volatile *pDest )				{ return ThreadInterlockedEmu_Decrement64( pDest ); }
int64 ThreadInterlockedCompareExchange64( int64 volatile *pDest, int64 v, int64 c ){ return ThreadInterlockedEmu_CompareExchange64( pDest, v, c ); }
int64 ThreadInterlockedExchange64( int64 volatile *pDest, int64 v )		{ return ThreadInterlockedEmu_Exchange64( pDest, v ); }
int64 ThreadInterlockedExchangeAdd64( int64 volatile *pDest, int64 v )	{ return ThreadInterlockedEmu_ExchangeAdd64( pDest, v ); }

int ThreadIsProcessActive( int procID )
{
	// Original (10009AE0): OpenProcess(PROCESS_QUERY_INFORMATION) + GetExitCodeProcess
	// == STILL_ACTIVE (0x103). Invalid/limited handles -> FALSE.
	HANDLE hProc = OpenProcess( PROCESS_QUERY_INFORMATION, FALSE, ( DWORD )procID );

	if ( hProc )
	{
		DWORD dwExitCode = 0;

		if ( GetExitCodeProcess( hProc, &dwExitCode ) && dwExitCode == STILL_ACTIVE )
		{
			CloseHandle( hProc );
			return 1;
		}

		CloseHandle( hProc );
	}

	return 0;
}

int WaitForMultipleEvents( int nEvents, const void **pHandles, bool bWaitAll, unsigned timeout )
{
	// Original (10009B40): capped at MAXIMUM_WAIT_OBJECTS, each entry is a POINTER TO a
	// HANDLE (dereferenced into a local array), returns the raw WaitForMultipleObjects
	// result, or -1 for FAILED / TIMEOUT / WAIT_ABANDONED_0+N.
	if( nEvents <= 0 || pHandles == NULL )
		return -1;

	if ( nEvents > MAXIMUM_WAIT_OBJECTS )
		return -1;

	int count = nEvents;

	HANDLE handles[ MAXIMUM_WAIT_OBJECTS ];

	for ( int i = 0; i < count; ++ i )
	{
		if( pHandles[ i ] == NULL || *( const HANDLE * )pHandles[ i ] == NULL )
			return -1;
		handles[ i ] = *( const HANDLE * )pHandles[ i ];
	}

	DWORD dwResult = WaitForMultipleObjects( ( DWORD )count, handles, bWaitAll, ( DWORD )timeout );

	if ( dwResult == WAIT_FAILED || dwResult == WAIT_TIMEOUT )
		return -1;

	// Valid abandoned range is [WAIT_ABANDONED_0, WAIT_ABANDONED_0 + count - 1];
	// `<=` would also swallow the first index beyond it.
	if ( dwResult >= WAIT_ABANDONED_0 && dwResult < WAIT_ABANDONED_0 + ( DWORD )count )
		return -1;

	return ( int )dwResult;
}

unsigned __stdcall SimpleThreadThreadProc( void *pvParam );

struct SimpleThreadInit_t
{
	ThreadWorkerFunction_t	pfnUserFunc;
	void*					pvParam;
};

unsigned __stdcall SimpleThreadThreadProc( void *pvParam )
{
	SimpleThreadInit_t *pData = ( SimpleThreadInit_t * )pvParam;
	unsigned retVal = 0;
	__try
	{
		retVal = pData->pfnUserFunc( pData->pvParam );
	}
	__finally
	{
		delete pData;
	}
	return retVal;
}

ThreadHandle_t CreateSimpleThread( ThreadWorkerFunction_t pfnThread, void *pvParam, ThreadId_t *pOutThreadId, unsigned nBytesStack )
{
	SimpleThreadInit_t *pInitData = new SimpleThreadInit_t;

	pInitData->pfnUserFunc	= pfnThread;
	pInitData->pvParam		= pvParam;

	DWORD threadID	= 0;
	HANDLE hThread	= ( HANDLE )_beginthreadex( NULL, nBytesStack, SimpleThreadThreadProc, pInitData, 0, ( unsigned * )&threadID );

	if ( !hThread )
	{
		delete pInitData;            // no thread was created; do not leak the init record
		if ( pOutThreadId )
			*pOutThreadId = 0;
		return NULL;
	}

	if ( pOutThreadId )
		*pOutThreadId = ( ThreadId_t )( uintptr_t )threadID;

	return hThread;
}

void DeclareCurrentThreadIsMainThread()		{ g_ThreadMainThreadID = GetCurrentThreadId(); }

// Thread-id used by Plat_GetCurrentThreadID is kept per-thread, exactly like the
// original shipping tier0.dll which stores it in a TLS slot (+4) at Plat_GetCurrentThreadID
// (100072B0) / Plat_RegisterPrimaryThread (100072D0) / Plat_RegisterThread (10007300).
static __declspec( thread ) unsigned s_RegThreadID = 0;

// DATA export defined in exports_fillins.cpp.
extern "C" unsigned long Plat_PrimaryThreadID;

void Plat_RegisterPrimaryThread()
{
	const unsigned id = ( unsigned )GetCurrentThreadId();

	s_RegThreadID = id;
	Plat_PrimaryThreadID = ( unsigned long )id;
	g_ThreadMainThreadID = id;
}

void Plat_RegisterThread( unsigned nIndex )
{
	UNREFERENCED_PARAMETER( nIndex );
	s_RegThreadID = ( unsigned )GetCurrentThreadId();
}

unsigned Plat_GetCurrentThreadID()
{
	return s_RegThreadID;
}

//=============================================================================
// CThreadLocalBase
//=============================================================================

CThreadLocalBase::CThreadLocalBase()
{
	m_index = TlsAlloc();

	AssertMsg1( m_index != 0xFFFFFFFF, "Bad thread local (%d)", GetLastError() );
}

CThreadLocalBase::~CThreadLocalBase()
{
	if ( m_index != 0xFFFFFFFF )
		TlsFree( m_index );

	m_index = 0xFFFFFFFF;
}

void *CThreadLocalBase::Get() const
{
	AssertMsg1( m_index != 0xFFFFFFFF, "Bad thread local (%d)", m_index );

	return TlsGetValue( m_index );
}

void CThreadLocalBase::Set( void *value )
{
	if ( m_index == 0xFFFFFFFF )
	{
		AssertMsg( false, "Bad thread local" );
	}
	else
	{
		TlsSetValue( m_index, value );
	}
}

//=============================================================================
// CThreadMutex
//=============================================================================

CThreadMutex::CThreadMutex()
{
	InitializeCriticalSection( &m_CriticalSection );
}

CThreadMutex::~CThreadMutex()
{
	DeleteCriticalSection( &m_CriticalSection );
}

void CThreadMutex::Lock()						{ EnterCriticalSection( reinterpret_cast<LPCRITICAL_SECTION>( &m_CriticalSection ) ); }
void CThreadMutex::Unlock()						{ LeaveCriticalSection( reinterpret_cast<LPCRITICAL_SECTION>( &m_CriticalSection ) ); }
void CThreadMutex::Lock() const volatile		{ EnterCriticalSection( reinterpret_cast<LPCRITICAL_SECTION>( const_cast<CRITICAL_SECTION *>( &m_CriticalSection ) ) ); }
void CThreadMutex::Unlock() const volatile		{ LeaveCriticalSection( reinterpret_cast<LPCRITICAL_SECTION>( const_cast<CRITICAL_SECTION *>( &m_CriticalSection ) ) ); }
bool CThreadMutex::TryLock()
{
	return TryEnterCriticalSection( reinterpret_cast<LPCRITICAL_SECTION>( &m_CriticalSection ) ) != FALSE;
}

//=============================================================================
// CThreadSyncObject
//=============================================================================

CThreadSyncObject::CThreadSyncObject()
{
	m_hSyncObject	= NULL;
	m_bInitalized	= true;
}

CThreadSyncObject::~CThreadSyncObject()
{
	if ( m_hSyncObject && m_bInitalized )
	{
		BOOL bResult = CloseHandle( m_hSyncObject );
		AssertOnce( bResult );
	}
}

bool CThreadSyncObject::operator!() const
{
	return m_hSyncObject == NULL;
}

CThreadSyncObject::operator void *()
{
	return m_hSyncObject;
}

void *CThreadSyncObject::Handle()
{
	return m_hSyncObject;
}

void CThreadSyncObject::AssertUseable()
{
#ifdef DEBUG
	AssertMsg( m_hSyncObject, "Thread synchronization object is unuseable" );
#endif
}

bool CThreadSyncObject::Wait( unsigned timeout )
{
	AssertUseable();

	return WaitForSingleObject( m_hSyncObject, timeout ) == WAIT_OBJECT_0;
}

//=============================================================================
// CThreadFullMutex
//=============================================================================

CThreadFullMutex::CThreadFullMutex( bool bEstablishInitialOwnership, const char *pszName )
{
	m_bInitalized = true;

	m_hSyncObject = CreateMutexA( NULL, bEstablishInitialOwnership ? TRUE : FALSE, pszName );

	AssertMsg1( m_hSyncObject, "Failed to create mutex (error 0x%x)", GetLastError() );
}

CThreadFullMutex::~CThreadFullMutex()
{
}

bool CThreadFullMutex::Release()
{
	return ReleaseMutex( m_hSyncObject ) != 0;
}

//=============================================================================
// CThreadSemaphore
//=============================================================================

CThreadSemaphore::CThreadSemaphore( long initialValue, long maxValue )
{
	m_bInitalized = true;

	m_hSyncObject = CreateSemaphoreA( NULL, initialValue, maxValue, NULL );

	AssertMsg1( m_hSyncObject, "Failed to create semaphore (error 0x%x)", GetLastError() );
}

CThreadSemaphore::~CThreadSemaphore()
{
}

bool CThreadSemaphore::Release( long releaseCount, long *pPreviousCount )
{
	AssertUseable();

	return ReleaseSemaphore( m_hSyncObject, releaseCount, ( LPLONG )pPreviousCount ) != 0;
}

//=============================================================================
// CThreadEvent
//=============================================================================

CThreadEvent::CThreadEvent( bool fManualReset )
{
	m_bInitalized = true;

	m_hSyncObject = CreateEventA( NULL, fManualReset ? TRUE : FALSE, FALSE, NULL );

	AssertMsg1( m_hSyncObject != NULL, "Failed to create event (error 0x%x)", GetLastError() );
}

CThreadEvent::CThreadEvent( void *pEvent, bool fManualReset )
{
	UNREFERENCED_PARAMETER( fManualReset );

	m_hSyncObject	= pEvent;
	m_bInitalized	= false;

	AssertFatalMsg1( m_hSyncObject != NULL, "Bad Event handle (handle 0x%p)", m_hSyncObject );
}

bool CThreadEvent::Set()
{
	AssertUseable();

	return SetEvent( m_hSyncObject ) != 0;
}

bool CThreadEvent::Reset()
{
	AssertUseable();

	return ResetEvent( m_hSyncObject ) != 0;
}

bool CThreadEvent::Pulse()
{
	AssertUseable();

	// PulseEvent is unreliable (loses signal if no waiter). Use Set for correctness.
	return SetEvent( m_hSyncObject ) != 0;
}

bool CThreadEvent::Check()
{
	return Wait( 0 );
}

//=============================================================================
// CThreadFastMutex
//=============================================================================

CThreadFastMutex::CThreadFastMutex()
{
	m_nOwnership = 0;
	m_nCount = 0;
}

CThreadFastMutex::CThreadFastMutex( const CThreadFastMutex& other )
{
	UNREFERENCED_PARAMETER( other );
	m_nOwnership = 0;
	m_nCount = 0;
}

CThreadFastMutex& CThreadFastMutex::operator=( const CThreadFastMutex& other )
{
	UNREFERENCED_PARAMETER( other );
	if ( this != &other )
	{
		m_nOwnership = 0;
		m_nCount = 0;
	}

	return *this;
}

static uint32 FM_CurrentThreadId()
{
	return ( uint32 )GetCurrentThreadId();
}

void CThreadFastMutex::Lock( unsigned timeout ) const volatile
{
	volatile uint32 *pOwner	= (volatile uint32 *)&m_nOwnership;
	volatile int *pCount	= (volatile int *)&m_nCount;

	const uint32 self = FM_CurrentThreadId();

	const DWORD tStart = GetTickCount();

	while ( true )
	{
		if ( *pOwner == self )
		{
			++(*pCount);
			return;
		}

		if ( *pOwner == 0 && InterlockedCompareExchange( ( long * )pOwner, self, 0 ) == 0 )
		{
			*pCount = 1;
			return;
		}

		if ( timeout != TT_INFINITE && ( GetTickCount() - tStart ) >= timeout )
		{
			// Bounded wait: give up without acquiring rather than spinning
			// forever once the budget is exhausted.
			return;
		}

		// Backoff: a few pause-stall cycles keep the cache line in the exclusive
		// state without burning a syscall, then yield to the scheduler.
		for ( int i = 0; i < 64; ++i )
			_mm_pause();

		Sleep( 0 );
	}
}

bool CThreadFastMutex::TryLock( const uint32 threadId ) const volatile
{
	volatile uint32 *pOwner	= (volatile uint32 *)&m_nOwnership;
	volatile int *pCount	= (volatile int *)&m_nCount;

	uint32 self = threadId;
	if ( self == 0 )
		self = FM_CurrentThreadId();

	if ( *pOwner == self )
	{
		++(*pCount);
		return true;
	}

	if ( *pOwner == 0 && InterlockedCompareExchange( ( long * )pOwner, self, 0 ) == 0 )
	{
		*pCount = 1;
		return true;
	}

	return false;
}

void CThreadFastMutex::Unlock() const volatile
{
	volatile uint32 *pOwner	= (volatile uint32 *)&m_nOwnership;
	volatile int *pCount	= (volatile int *)&m_nCount;

	const uint32 self = FM_CurrentThreadId();

	if ( *pOwner == self )
	{
		if ( --(*pCount) <= 0 )
		{
			*pCount = 0;
			InterlockedExchange( ( long * )pOwner, 0 );
		}
	}
	else if ( *pOwner == 0 && *pCount > 0 )
	{
		*pCount = 0;
	}
}

bool CThreadFastMutex::AssertOwnedByCurrentThread()
{
	// Original ship tier0.dll: this export is `mov al,1; ret` (always true).
	return true;
}

//=============================================================================
// CThread
//=============================================================================

CThread::CThread()
{
	m_hThread	= NULL;
	m_threadId	= 0;
	m_result	= 0;
	m_szName[0]	= '\0';
}


const char *CThread::GetName()
{


	if ( !*( m_szName ) )
	{
		_snprintf( m_szName, ARRAYSIZE( m_szName ), "Thread(0x%p/0x%x)",
			this, m_threadId );
		m_szName[ ARRAYSIZE( m_szName ) - 1 ] = '\0';
	}


	return m_szName;
}

void CThread::SetName( const char *pszName )
{
	strncpy( m_szName, pszName, ARRAYSIZE( m_szName ) - 1 );
	m_szName[ ARRAYSIZE( m_szName ) - 1 ] = '\0';
}

// Per-thread init structure exchanged through a small event handshake.
struct ThreadInit_t
{
	CThread			*pThread;
	CThreadEvent	*pInitCompleteEvent;
	bool			*pfInitSuccess;
};

bool CThread::Start( unsigned nBytesStack )
{
	CThreadEvent initComplete( false );
	bool bInitSuccess = false;

	ThreadInit_t *pInit = new ThreadInit_t;
	pInit->pThread = this;
	pInit->pInitCompleteEvent = &initComplete;
	pInit->pfInitSuccess = &bInitSuccess;

	HANDLE hNewThread = ( HANDLE )_beginthreadex(
		NULL,
		nBytesStack,
		( unsigned (__stdcall *)( void * ) )GetThreadProc(),
		pInit,
		0,
		( unsigned * )&m_threadId );

	if ( hNewThread == NULL )
	{
		delete pInit;
		AssertMsg1( false, "Failed to create thread (error 0x%x)", GetLastError() );
		return false;
	}

	m_hThread = hNewThread;

	if ( !WaitForCreateComplete( &initComplete ) )
	{
		Msg( "Thread failed to initialize\n" );
		return false;
	}

	if ( !bInitSuccess )
	{
		Msg( "Thread failed to initialize (2)\n" );
		return false;
	}

	return true;
}

bool CThread::IsAlive()
{
	DWORD exitCode;
	return m_hThread && GetExitCodeThread( m_hThread, &exitCode ) && exitCode == STILL_ACTIVE;
}

bool CThread::Join( unsigned timeout )
{
	if ( m_hThread == NULL )
		return true;

	// thread cannot be joined with self
	const DWORD dwWait = WaitForSingleObject( m_hThread, timeout );

	if ( dwWait == WAIT_TIMEOUT )
		return false;

	if ( dwWait == WAIT_FAILED )
	{
		AssertMsg( !"\"CThread::Join WAIT_FAILED\"", "" );
		return false;
	}

	if ( dwWait != WAIT_OBJECT_0 )
		return false;

	DWORD exitCode = 0;
	if ( GetExitCodeThread( m_hThread, &exitCode ) )
		m_result = ( int )exitCode;

	CloseHandle( m_hThread );
	m_hThread = NULL;
	m_threadId = 0;
	return true;
}

void *CThread::GetThreadHandle()			{ return m_hThread; }
uint CThread::GetThreadId()					{ return m_threadId; }
int CThread::GetResult()					{ return m_result; }

void CThread::Stop( int timeout )
{
	DWORD ExitCode;

	// If no thread, or it has already finished, there is nothing to stop.
	if ( !m_hThread || !GetExitCodeThread( m_hThread, &ExitCode ) )
		return;

	if ( ExitCode == STILL_ACTIVE )
	{
		// Negative timeout is the public convention for an infinite wait.
		const DWORD waitTimeout = timeout < 0 ? INFINITE : ( DWORD )timeout;
		WaitForSingleObject( m_hThread, waitTimeout );
	}
}

unsigned CThread::Suspend()
{
	if ( m_hThread )
		return SuspendThread( m_hThread );

	return ( unsigned )-1;
}

unsigned CThread::Resume()
{
	if ( m_hThread )
		return ResumeThread( m_hThread );

	return ( unsigned )-1;
}

bool CThread::Terminate( int result )
{
	if ( !m_hThread )
		return false;

	// Terminating the calling thread cannot return to update the object safely.
	if ( m_threadId != 0 && m_threadId == GetCurrentThreadId() )
		return false;

	DWORD exitCode = STILL_ACTIVE;
	if ( GetExitCodeThread( m_hThread, &exitCode ) && exitCode != STILL_ACTIVE )
	{
		m_result = ( int )exitCode;
		return true;
	}

	if ( !TerminateThread( m_hThread, result ) )
		return false;

	// Keep the handle owned by CThread until Join() or the destructor confirms
	// that the kernel object is signaled. This makes a subsequent Join real
	// synchronization instead of a vacuous success on a NULL handle.
	if ( WaitForSingleObject( m_hThread, 3000 ) != WAIT_OBJECT_0 )
		return false;

	m_result = result;
	return true;
}

int CThread::GetPriority() const
{
	return GetThreadPriority( m_hThread ? m_hThread : GetCurrentThread() );
}

bool CThread::SetPriority( int priority )
{
	return m_hThread ? SetThreadPriority( m_hThread, priority ) != FALSE : false;
}

void CThread::Sleep( unsigned duration )
{
	PreciseSleep( duration );
}

void (CThread::Yield)()
{
#ifdef WIN32
	Sleep( 0 );
#endif
}

CThread *CThread::GetCurrentCThread()
{
	return g_pCurThread;
}

bool CThread::Init()					{ return true; }
void CThread::OnExit()					{}
int CThread::Run()						{ return 0; }

bool CThread::WaitForCreateComplete( CThreadEvent *pEvent )
{
	// Use a timeout to avoid deadlock
	if ( !pEvent->Wait( 60000 ) )
	{
		AssertMsg( false, "Probably deadlock or failure waiting for thread to initialize." );
		return false;
	}

	return true;
}

CThread::ThreadProc_t CThread::GetThreadProc()
{
	return &CThread::ThreadProc;
}

unsigned __stdcall CThread::ThreadProc( void * pv )
{
	ThreadInit_t *pData = static_cast<ThreadInit_t *>( pv );

	// `pData` was heap-allocated in CThread::Start() and owned by this thread.
	// Cache everything needed before signalling, then Set() and delete.
	CThread *pThread = pData->pThread;
	CThreadEvent *pEvent = pData->pInitCompleteEvent;
	bool *pfInitSuccess = pData->pfInitSuccess;

	g_pCurThread = pThread;

	if ( pfInitSuccess )
		*pfInitSuccess = false;

	const bool bInitSuccess = pThread->Init();

	if ( pfInitSuccess )
		*pfInitSuccess = bInitSuccess;

	pEvent->Set();
	delete pData;

	// ---- pData is off limits from here on ----

	unsigned result = 0;

	if ( bInitSuccess )
	{
		result = pThread->Run();

		pThread->OnExit();

		g_pCurThread = ( CThread * )NULL;

		pThread->m_threadId = 0;
	}

	return result;
}

//=============================================================================
// CWorkerThread
//=============================================================================

static unsigned WorkerCallTimeout()
{
	static volatile LONG s_timeout = -1;
	LONG timeout = InterlockedCompareExchange( &s_timeout, -1, -1 );
	if( timeout == -1 )
	{
		if( InterlockedCompareExchange( &s_timeout, -2, -1 ) == -1 )
		{
			unsigned value = 30 * 1000;
			const char *psz = getenv( "TIER0_WORKER_TIMEOUT_MS" );
			if( psz && psz[ 0 ] )
			{
				int parsed = atoi( psz );
				if( parsed >= 0 && parsed <= 10 * 60 * 1000 )
					value = ( unsigned )parsed;
			}
			InterlockedExchange( &s_timeout, ( LONG )value );
		}
		timeout = InterlockedCompareExchange( &s_timeout, -1, -1 );
	}
	while( timeout == -2 )
	{
		Sleep( 0 );
		timeout = InterlockedCompareExchange( &s_timeout, -1, -1 );
	}
	return ( unsigned )timeout;
}

CWorkerThread::CWorkerThread()
{
	memset( &m_Call, 0, sizeof( m_Call ) );
}

int CWorkerThread::BoostPriority( int newPriority )
{
	if ( !IsAlive() )
		return -1;

	int oldPriority = GetPriority();
	SetPriority( newPriority );

	return oldPriority;
}



int CWorkerThread::CallMaster( unsigned callFlags, unsigned callParam )
{
	// Master side of the handshake: a call posted from the primary thread is
	// always synchronous, so wait for the worker's reply before returning.
	return Call( callFlags, callParam, true, NULL );
}

int CWorkerThread::CallWorker( unsigned flags, unsigned callParam /* = 0 */, bool fWaitForReply /* = false */ )
{
	return Call( flags, callParam, fWaitForReply, NULL );
}

int CWorkerThread::Call( unsigned flags, unsigned callParam, bool fWaitForReply,
						 unsigned ( __stdcall *pfn )( unsigned, void * const *, int, unsigned ) )
{
	UNREFERENCED_PARAMETER( pfn );

	if ( !IsAlive() )
		return -1;

	// Single-slot queue: reserve the slot atomically before publishing the
	// payload. The previous read-then-write sequence allowed two callers to
	// overwrite m_Call, and WaitForCall() cleared WT_PENDING before the worker
	// had produced its reply.
	volatile LONG *pFlags = ( volatile LONG * )&m_Call.m_flags;
	const LONG scheduledFlags = ( LONG )( flags & ~WT_PENDING ) | WT_SCHEDULED;
	if( InterlockedCompareExchange( pFlags, scheduledFlags, 0 ) != 0 )
		return -1;

	m_Call.m_param = callParam;
	InterlockedExchange( pFlags, ( LONG )( flags & ~WT_PENDING ) | WT_PENDING );

	// An asynchronous call may leave the auto-reset completion event signaled.
	// Clear that stale signal before publishing the next request, otherwise a
	// later synchronous call can return before its own worker reply arrives.
	m_EventComplete.Reset();
	m_EventSend.Set();

	if ( fWaitForReply )
	{
		if ( !WaitForReply( WorkerCallTimeout() ) )
		{
			// A timed-out one-slot request cannot be safely reused: the worker
			// could still publish its late reply into the shared m_Call payload.
			// Fail closed by terminating the worker and leave the slot poisoned;
			// callers must recreate the worker before issuing another request.
			Terminate( -1 );
			return -1;
		}
	}

	// The worker's Reply() stores the result in the shared call slot.
	return ( int )m_Call.m_param;
}

void *CWorkerThread::GetCallHandle()
{
	return m_hThread;
}

unsigned CWorkerThread::GetCallParam() const
{
	return m_Call.m_param;
}

bool CWorkerThread::PeekCall( unsigned *pParam )
{
	if ( WaitForSingleObject( m_EventSend.Handle(), 0 ) == WAIT_OBJECT_0 )
	{
		if ( pParam )
			*pParam = m_Call.m_param;

		// Keep WT_PENDING set until Reply(). The single slot is still occupied
		// while the worker processes this request; clearing it here lets a new
		// caller overwrite m_Call before the reply is stored.
		return true;
	}

	if ( pParam )
		*pParam = 0;

	return false;
}

bool CWorkerThread::WaitForCall( unsigned dwTimeout, unsigned *pResult )
{
	if ( !WaitForSingleObject( m_EventSend.Handle(), dwTimeout ) )
	{
		if ( pResult )
			*pResult = m_Call.m_param;

		// The request remains pending until Reply() publishes the result.
		// This prevents a second producer from reusing the one-slot queue while
		// the worker is still processing the current request.
		return true;
	}

	if ( pResult )
		*pResult = 0;

	return false;
}

bool CWorkerThread::WaitForCall( unsigned *pResult )
{
	return WaitForCall( TT_INFINITE, pResult );
}

void CWorkerThread::Reply( unsigned result )
{
	// Store the reply payload where the caller can pick it up, then release
	// the waiters. The event handle re-arms on the next Send.
	m_Call.m_param = result;
	InterlockedAnd( ( volatile LONG * )&m_Call.m_flags,
		~( LONG )( WT_PENDING | WT_SCHEDULED ) );

	m_EventComplete.Set();
}

bool CWorkerThread::WaitForReply( unsigned timeout )
{
	const DWORD waitResult = WaitForSingleObject( m_EventComplete.Handle(), timeout );

	if ( waitResult != WAIT_OBJECT_0 )
	{
		AssertMsg2( !"Timed out waiting for reply", "", "", "" );
	}

	return waitResult == WAIT_OBJECT_0;
}

int CWorkerThread::Run()
{
	unsigned result;

	// Base dispatcher: wait for a call, hand its parameter to derived Run()
	// processing, and ship the result back through Reply().
	while ( WaitForCall( &result ) )
	{
		Reply( result );
	}

	return 0;
}

//-----------------------------------------------------------------------------

// Out-of-line definitions required by the fixed-ordinal export table.

bool CThreadEvent::Wait( unsigned timeout )
{
	return CThreadSyncObject::Wait( timeout );
}

bool CThreadMutex::AssertOwnedByCurrentThread()
{
	return true;
}

void CThreadMutex::SetTrace( bool )
{
}

//-----------------------------------------------------------------------------

// Extra entry points required verbatim by the export table.

// CThreadLocalBase copy assignment (shares the TLS slot, matching ship ABI)
CThreadLocalBase& CThreadLocalBase::operator=( const CThreadLocalBase& other )
{
	if ( this != &other )
	{
		if ( m_index != 0xFFFFFFFF )
			TlsFree( m_index );

		m_index = other.m_index;
	}

	return *this;
}

// CWorkerThread::Call queue flags type kept ABI-shaped; helper completes the
// exported Call() path without external state.
void CWorkerThread_LocalHelperMarker( void ) {}
