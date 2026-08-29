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
	unsigned long retLo, retHi;

	__asm
	{
		push	edi
		push	ebx

		mov		esi, pd
		mov		eax, [esi]
		mov		edx, [esi+4]
		mov		edi, pv

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
	do { Old = *pDest; }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, Old + 1, Old ) != Old );
	return Old + 1;
}

int64 ThreadInterlockedEmu_Decrement64( volatile int64 *pDest )
{
	int64 Old;
	do { Old = *pDest; }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, Old - 1, Old ) != Old );
	return Old - 1;
}

int64 ThreadInterlockedEmu_Exchange64( volatile int64 *pDest, int64 value )
{
	int64 Old;
	do { Old = *pDest; }
	while ( ThreadInterlockedEmu_CompareExchange64( pDest, value, Old ) != Old );
	return Old;
}

int64 ThreadInterlockedEmu_ExchangeAdd64( volatile int64 *pDest, int64 value )
{
	int64 Old;
	do { Old = *pDest; }
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
	static int s_mode = -1;
	if ( s_mode < 0 )
	{
		const char *psz = getenv( "TIER0_PRECISE_SLEEP" );
		s_mode = ( psz && psz[ 0 ] == '0' ) ? 0 : 1;
	}
	return s_mode != 0;
}

extern "C" unsigned int __stdcall timeBeginPeriod( unsigned int uPeriod );

static double SlpNow()
{
	static LARGE_INTEGER s_Frequency = {};
	static LARGE_INTEGER s_Base = {};
	static bool s_bInit = false;

	if ( !s_bInit )
	{
		QueryPerformanceFrequency( &s_Frequency );
		QueryPerformanceCounter( &s_Base );
		s_bInit = true;
	}

	LARGE_INTEGER c;
	QueryPerformanceCounter( &c );
	return ( double )( c.QuadPart - s_Base.QuadPart ) / ( double )s_Frequency.QuadPart;
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

	static bool s_bHighRes = false;
	if ( !s_bHighRes )
		s_bHighRes = ( timeBeginPeriod( 1 ) == 0 );

	const double target = ( double )duration / 1000.0;
	const double tStart = SlpNow();
	::Sleep( duration > kSpinHeadroom + 1 ? duration - kSpinHeadroom : 1 );

	for ( ;; )
	{
		if ( SlpNow() - tStart >= target )
			break;
		_mm_pause();
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
	int count = MAXIMUM_WAIT_OBJECTS;

	if ( nEvents < MAXIMUM_WAIT_OBJECTS )
		count = nEvents;

	if ( count < 0 )
		count = 0;

	HANDLE handles[ MAXIMUM_WAIT_OBJECTS ];

	for ( int i = 0; i < count; ++ i )
		handles[ i ] = *( const HANDLE * )pHandles[ i ];

	DWORD dwResult = WaitForMultipleObjects( ( DWORD )count, handles, bWaitAll, ( DWORD )timeout );

	if ( dwResult == WAIT_FAILED || dwResult == WAIT_TIMEOUT )
		return -1;

	if ( dwResult >= WAIT_ABANDONED_0 && dwResult <= WAIT_ABANDONED_0 + ( DWORD )count )
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
	const unsigned retVal = pData->pfnUserFunc( pData->pvParam );
	delete pData;
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

	return PulseEvent( m_hSyncObject ) != 0;
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
	m_nOwnership = other.m_nOwnership;
	m_nCount = other.m_nCount;
}

CThreadFastMutex& CThreadFastMutex::operator=( const CThreadFastMutex& other )
{
	if ( this != &other )
	{
		m_nOwnership = other.m_nOwnership;
		m_nCount = other.m_nCount;
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
	HANDLE hNewThread;
	CThreadEvent initComplete( false );
	bool bInitSuccess = false;

	ThreadInit_t Init = { this, &initComplete, &bInitSuccess };

	hNewThread = CreateThread(
		NULL,
		nBytesStack,
		( LPTHREAD_START_ROUTINE )( GetThreadProc() ),
		&Init,
		0,
		( LPDWORD )&m_threadId );

	if ( hNewThread == NULL )
	{
		AssertMsg1( false, "Failed to create thread (error 0x%x)", GetLastError() );
		return false;
	}

	m_hThread = hNewThread;

	if ( !WaitForCreateComplete( &initComplete ) )
	{
		Msg( "Thread failed to initialize\n" );
		CloseHandle( m_hThread );
		m_hThread = NULL;
		return false;
	}

	if ( !bInitSuccess )
	{
		Msg( "Thread failed to initialize (2)\n" );
		CloseHandle( m_hThread );
		m_hThread = NULL;
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
		AssertMsg( !"\"CThread::Join WAIT_FAILED\"" );
		return true;
	}

	AssertMsg( dwWait == WAIT_OBJECT_0, "" );

	return dwWait == WAIT_OBJECT_0;
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
		// Give the thread up to the requested time to wind down on its own.
		WaitForSingleObject( m_hThread, timeout );
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
	// Original t=o: after a successful TerminateThread the handle/thread-id are
	// zeroed so a later ~CThread / Join does not touch a dead thread object.
	// Closing the raw handle here is safe: the destructor's CloseHandle(NULL)
	// becomes a no-op, and no runner touches m_hThread afterwards on this path.
	if ( !TerminateThread( m_hThread, result ) )
		return false;

	void *hThread = m_hThread;
	m_hThread = NULL;
	m_threadId = 0;

	if ( hThread )
		CloseHandle( hThread );

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

	g_pCurThread = pData->pThread;

	if ( pData->pfInitSuccess )
		*pData->pfInitSuccess = false;

	const bool bInitSuccess = pData->pThread->Init();

	if ( pData->pfInitSuccess )
		*pData->pfInitSuccess = bInitSuccess;

	pData->pInitCompleteEvent->Set();

	unsigned result = 0;

	if ( bInitSuccess )
	{
		result = pData->pThread->Run();

		pData->pThread->OnExit();

		g_pCurThread = ( CThread * )NULL;

		pData->pThread->m_threadId = 0;
	}

	return result;
}

//=============================================================================
// CWorkerThread
//=============================================================================

#define WTCALL_TIMEOUT	( 30 * 1000 )

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

	// Single-slot queue: reject a second call while the first is being serviced.
	if ( m_Call.m_flags & WT_PENDING )
		return -1;

	m_Call.m_flags = ( flags & ~WT_PENDING ) | WT_PENDING;
	m_Call.m_param = callParam;

	m_EventSend.Set();

	if ( fWaitForReply )
	{
		if ( !WaitForReply( WTCALL_TIMEOUT ) )
			return -1;
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

		m_Call.m_flags &= ~WT_PENDING;
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

		m_Call.m_flags &= ~WT_PENDING;
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
	m_Call.m_flags &= ~WT_PENDING;

	m_EventComplete.Set();
}

bool CWorkerThread::WaitForReply( unsigned timeout )
{
	const DWORD waitResult = WaitForSingleObject( m_EventComplete.Handle(), timeout );

	if ( waitResult != WAIT_OBJECT_0 )
	{
		AssertMsg2( !"Timed out waiting for reply", "" );
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
