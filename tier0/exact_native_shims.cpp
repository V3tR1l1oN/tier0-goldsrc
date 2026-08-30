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

// 4. CThreadFullMutex methods
// NOTE: Original ship tier0.dll exports all three AssertOwnedByCurrentThread() at
// RVA 0x14C0 = `mov al,1; ret` (always TRUE, no real ownership check).
bool CThreadFullMutex::AssertOwnedByCurrentThread() { return true; }
void CThreadFullMutex::SetTrace( bool b ) { UNREFERENCED_PARAMETER( b ); }
void CThreadFullMutex::Lock( unsigned timeout ) { Wait( timeout ); }
void CThreadFullMutex::Lock() { Wait( TT_INFINITE ); }
void CThreadFullMutex::Unlock() { Release(); }

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
