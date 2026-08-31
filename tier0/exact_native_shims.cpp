// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Native implementations for remaining mangled exports.
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include "threadtools.h"
#include "vprof.h"
#include "validator.h"

#ifdef WIN32
#include "winlite.h"
#endif

// 1. volatile bool g_bInException
volatile bool g_bInException = false;

// 2. Default constructor closures: force compiler to emit real ??_F symbols
void ForceDefaultConstructorClosuresEmission()
{
	static CThreadEvent dummyEvents[2];
	static CThreadFullMutex dummyMutexes[2];
	dummyEvents[0].Set();
	dummyMutexes[0].Release();
}

// 3. CThreadFastMutex move / cv methods
// A move has to leave the source empty: copying the ownership word without
// clearing it made the moved-from mutex report GetOwnerId() == the old owner
// and reject every TryLock(), while two objects claimed the same lock.
CThreadFastMutex::CThreadFastMutex( CThreadFastMutex &&other )
{
	m_nOwnership = other.m_nOwnership;
	m_nCount	 = other.m_nCount;

	other.m_nOwnership	= 0;
	other.m_nCount		= 0;
}

CThreadFastMutex& CThreadFastMutex::operator=( CThreadFastMutex &&other )
{
	if ( this != &other )
	{
		m_nOwnership = other.m_nOwnership;
		m_nCount	 = other.m_nCount;

		other.m_nOwnership	= 0;
		other.m_nCount		= 0;
	}
	return *this;
}

// NOTE: the forwarding overloads below must cast to `const volatile`.
// Casting to a plain (non-volatile) pointer made overload resolution pick the
// `volatile` overload again -- the shim called itself and blew the stack
// (warning C4717). Only the `const volatile` members hold the real
// implementation, so that is where the call has to land.
void CThreadFastMutex::Lock( unsigned timeout ) volatile
{
	const_cast<const volatile CThreadFastMutex *>( this )->Lock( timeout );
}

bool CThreadFastMutex::TryLock() volatile
{
	return const_cast<CThreadFastMutex *>( (CThreadFastMutex *)this )->TryLock( 0 );
}

bool CThreadFastMutex::TryLock() const volatile
{
	return const_cast<CThreadFastMutex *>( (const CThreadFastMutex *)this )->TryLock( 0 );
}

void CThreadFastMutex::Unlock() volatile
{
	const_cast<const volatile CThreadFastMutex *>( this )->Unlock();
}

void CThreadFastMutex::SetTrace( bool b ) { UNREFERENCED_PARAMETER( b ); }

// 4. CThreadFullMutex methods – fixed to track ownership.
// Original ship tier0.dll: all three AssertOwnedByCurrentThread() were `mov al,1; ret`.
// Now we maintain a lock-protected table keyed by HANDLE to provide real checks.

// Lightweight owner table for kernel mutexes (no per-object storage to keep ABI).
static CRITICAL_SECTION s_FullMutexMapCS;
static bool s_FullMutexMapInit = false;
static struct FullMutexOwner_t { void* h; DWORD owner; int count; } s_FullOwners[64];

static void FullMutexMapEnsureInit()
{
	if ( !s_FullMutexMapInit )
	{
		InitializeCriticalSection( &s_FullMutexMapCS );
		memset( s_FullOwners, 0, sizeof(s_FullOwners) );
		s_FullMutexMapInit = true;
	}
}
static void FullMutexTrackAcquire( void* h, DWORD tid )
{
	FullMutexMapEnsureInit();
	EnterCriticalSection( &s_FullMutexMapCS );
	for ( int i = 0; i < 64; ++i )
	{
		if ( s_FullOwners[i].h == h )
		{
			// Re-entrant acquire by same thread
			if ( s_FullOwners[i].owner == tid )
				s_FullOwners[i].count++;
			LeaveCriticalSection( &s_FullMutexMapCS );
			return;
		}
	}
	for ( int i = 0; i < 64; ++i )
	{
		if ( s_FullOwners[i].h == nullptr )
		{
			s_FullOwners[i].h = h;
			s_FullOwners[i].owner = tid;
			s_FullOwners[i].count = 1;
			break;
		}
	}
	LeaveCriticalSection( &s_FullMutexMapCS );
}
static void FullMutexTrackRelease( void* h, DWORD tid )
{
	if ( !s_FullMutexMapInit || !h )
		return;
	EnterCriticalSection( &s_FullMutexMapCS );
	for ( int i = 0; i < 64; ++i )
	{
		if ( s_FullOwners[i].h == h && s_FullOwners[i].owner == tid )
		{
			if ( --s_FullOwners[i].count <= 0 )
			{
				s_FullOwners[i].h = nullptr;
				s_FullOwners[i].owner = 0;
				s_FullOwners[i].count = 0;
			}
			break;
		}
	}
	LeaveCriticalSection( &s_FullMutexMapCS );
}
static bool FullMutexIsOwnedByCurrent( void* h )
{
	if ( !s_FullMutexMapInit || !h )
		return false;
	DWORD tid = GetCurrentThreadId();
	bool owned = false;
	EnterCriticalSection( &s_FullMutexMapCS );
	for ( int i = 0; i < 64; ++i )
	{
		if ( s_FullOwners[i].h == h && s_FullOwners[i].owner == tid && s_FullOwners[i].count > 0 )
		{
			owned = true;
			break;
		}
	}
	LeaveCriticalSection( &s_FullMutexMapCS );
	return owned;
}

bool CThreadFullMutex::AssertOwnedByCurrentThread()
{
	if ( !m_hSyncObject )
		return false;
	return FullMutexIsOwnedByCurrent( m_hSyncObject );
}
void CThreadFullMutex::SetTrace( bool b ) { UNREFERENCED_PARAMETER( b ); }
void CThreadFullMutex::Lock( unsigned timeout )
{
	if ( Wait( timeout ) )
		FullMutexTrackAcquire( m_hSyncObject, GetCurrentThreadId() );
}
void CThreadFullMutex::Lock()
{
	if ( Wait( TT_INFINITE ) )
		FullMutexTrackAcquire( m_hSyncObject, GetCurrentThreadId() );
}
void CThreadFullMutex::Unlock()
{
	DWORD tid = GetCurrentThreadId();
	// Only release tracking if we actually own it; ReleaseMutex fails if not owner
	if ( FullMutexIsOwnedByCurrent( m_hSyncObject ) )
	{
		if ( Release() )
			FullMutexTrackRelease( m_hSyncObject, tid );
	}
	else
	{
		// Not owned – still try Release but don't corrupt table
		Release();
	}
}

// 5. CThreadMutex const Lock / Unlock
void CThreadMutex::Lock() const
{
	EnterCriticalSection( const_cast<LPCRITICAL_SECTION>( (LPCRITICAL_SECTION)&m_CriticalSection ) );
}

void CThreadMutex::Unlock() const
{
	LeaveCriticalSection( const_cast<LPCRITICAL_SECTION>( (LPCRITICAL_SECTION)&m_CriticalSection ) );
}

// 6. CWorkerThread::BoostPriority() / Call (Call() lives in threadtools.cpp)
int CWorkerThread::BoostPriority()
{
	return BoostPriority( 1 );
}

// 7. CVProfNode::GetOrigNameAddress -> const void*
const void *CVProfNode::GetOrigNameAddress()
{
	static volatile bool fDumped = false;
	if ( fDumped ) Msg( "" );
	AssertMsgOnce( m_pvOrigNameAddress, "m_pvOrigNameAddress" );
	return ( const void * )m_pvOrigNameAddress;
}

// 8. CVProfile::GetBudgetGroupColor (5 args)
void CVProfile::GetBudgetGroupColor( int budgetGroupID, int &r, int &g, int &b, int &a )
{
	float fr = 1.0f, fg = 1.0f, fb = 1.0f;
	GetBudgetGroupColor( budgetGroupID, fr, fg, fb );
	r = ( int )( fr * 255.0f );
	g = ( int )( fg * 255.0f );
	b = ( int )( fb * 255.0f );
	a = 255;
}
