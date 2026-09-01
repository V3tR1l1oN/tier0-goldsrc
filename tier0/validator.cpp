// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Memory validator implementation (restored from tier0.dll).
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "validator.h"
#include "threadtools.h"

#include <malloc.h>
#include <cstring>
#include <cstdio>

#ifdef _LINUX
#ifndef _T
#define _T(x) x
#endif
#ifndef _sntprintf
#define _sntprintf snprintf
#endif
#endif

//=============================================================================
// CValObject
//=============================================================================

CValObject::~CValObject()
{
}

void CValObject::Init( tchar *pchType, void *pvObj, tchar *pchName,
					   CValObject *pValObjectParent, CValObject *pValObjectPrev )
{
#ifdef _LINUX
	strncpy( m_pchType, pchType ? pchType : "", sizeof( m_pchType ) - 1 );
#else
	strncpy_s( m_pchType, pchType ? pchType : _T( "" ), sizeof( m_pchType ) - 1 );
#endif
	m_pchType[ sizeof( m_pchType ) - 1 ] = '\0';

	m_nTypeLength			= 0;
	m_pvObj					= pvObj;
	m_pchName				= pchName;
	m_pValObjectParent		= pValObjectParent;
	m_pValObjectNext		= NULL;
	m_pValObjectChild		= NULL;
	m_pvMem					= NULL;
	m_cubMem				= 0;
	m_bMemClaimsOwnership	= false;

	if ( pValObjectPrev )
	{
#ifdef _DEBUG
		AssertMsg2( pValObjectPrev->m_pValObjectNext == NULL, "NULL != ... (%s/%s)", "", "" );
#endif
		pValObjectPrev->m_pValObjectNext = this;
	}
}

void CValObject::ClaimMemoryBlock( void *pvMem )
{
	m_pvMem = pvMem;
}

void CValObject::ClaimChildMemoryBlock( int cubUser )
{
	UNREFERENCED_PARAMETER( cubUser );
}

//=============================================================================
// CValidator
//=============================================================================

CValidator::CValidator()
	: m_SharedMutex()
	, m_pValObjectFirst( NULL )
	, m_pValObjectLast( NULL )
	, m_pValObjectCur( NULL )
	, m_cpvOwned( 0 )
	, m_cubLeaked( 0 )
	, m_cpubLeaked( 0 )
	, m_bMemLeaks( false )
	, m_cBlocksSinceMinidump( 0 )
	, m_hHeap( NULL )
	, m_nExcludeAllocations( 0 )
	, m_rgpvptrExcludedMemoryBlocks( NULL )
	, m_rgbTouched( NULL )
	, m_rgb( NULL )
	, m_iThreadMutexValidationLock( -1 )
	, m_fastMutex()
{
	memset( m_threadMutexValidationLock, 0, sizeof( m_threadMutexValidationLock ) );
}

CValidator::~CValidator()
{
	// Free the object list.
	CValObject *pObj = m_pValObjectFirst;

	while ( pObj )
	{
		CValObject *pNext = pObj->m_pValObjectNext;
		free( pObj );
		pObj = pNext;
	}

	m_pValObjectFirst	= NULL;
	m_pValObjectLast	= NULL;
	m_pValObjectCur		= NULL;
}

void CValidator::Push( const tchar *pchType, void *pvObj, const tchar *pchName )
{
	CValObject *pNewObject = ( CValObject * )malloc( sizeof( CValObject ) );

	if ( !pNewObject )
		return;

	pNewObject->Init(
		( tchar * )pchType,
		pvObj,
		( tchar * )pchName,
		NULL,						// parent fixed below
		m_pValObjectCur );

	pNewObject->m_pValObjectParent = m_pValObjectCur;

	if ( !m_pValObjectFirst )
		m_pValObjectFirst = pNewObject;

	if ( m_pValObjectLast && !m_pValObjectLast->m_pValObjectNext && !m_pValObjectCur )
		m_pValObjectLast->m_pValObjectNext = pNewObject;

	m_pValObjectLast	= pNewObject;
	m_pValObjectCur		= pNewObject;

	++m_cpvOwned;
}

void CValidator::Pop()
{
	if ( m_pValObjectCur )
		m_pValObjectCur = m_pValObjectCur->m_pValObjectParent;
}

void CValidator::ClaimMemory( void *pvMem )
{
	if ( !pvMem || !m_pValObjectCur )
		return;

	m_pValObjectCur->ClaimMemoryBlock( pvMem );
	++m_cpubLeaked;
}

void CValidator::ClaimArrayMemory( void *pvMem )
{
	ClaimMemory( pvMem );
}

void CValidator::Finalize()
{
	m_bMemLeaks = ( m_cpvOwned != 0 );
}

void CValidator::RenderObjects( int cubThreshold )
{
	UNREFERENCED_PARAMETER( cubThreshold );

	for ( CValObject *pObj = m_pValObjectFirst; pObj; pObj = pObj->m_pValObjectNext )
	{
		Msg( "%-30s obj(%p) mem(%p,%d)\n", pObj->m_pchType, pObj->m_pvObj, pObj->m_pvMem, pObj->m_cubMem );
	}
}

void CValidator::RenderLeaks()
{
	long long cubLeaked = 0;

	for ( CValObject *pObj = m_pValObjectFirst; pObj; pObj = pObj->m_pValObjectNext )
		cubLeaked += pObj->m_cubMem;

	Msg( "Bytes leaked: %ld\n", ( long )cubLeaked );
}

CValObject *CValidator::FindObject( void *pvObj )
{
	for ( CValObject *pObj = m_pValObjectFirst; pObj; pObj = pObj->m_pValObjectNext )
	{
		if ( pObj->m_pvObj == pvObj )
			return pObj;
	}

	return NULL;
}

void CValidator::DiffAgainst( CValidator *pOtherValidator )
{
	if ( !pOtherValidator )
		return;

	// Remove entries present in both trees from ours: simple linear pass.
	for ( CValObject **ppLink = &m_pValObjectFirst; *ppLink; )
	{
		CValObject *pCur = *ppLink;

		if ( pOtherValidator->FindObject( pCur->m_pvObj ) )
		{
			*ppLink = pCur->m_pValObjectNext;
			free( pCur );
			continue;
		}

		ppLink = &pCur->m_pValObjectNext;
	}
}

void CValidator::Validate( CValidator &validator, char *pchName )
{
	UNREFERENCED_PARAMETER( pchName );

	validator.Push( "CValidator", this, "CValidator" );

	// Chain all object records for validation purposes.
	for ( CValObject *pObj = m_pValObjectFirst; pObj; pObj = pObj->m_pValObjectNext )
		validator.ClaimMemory( pObj );

	validator.Pop();
}

void CValidator::AddValidationLock( CThreadMutex *pMutex )
{
	if ( m_iThreadMutexValidationLock >= s_cMaxThreadMutexValidationLocks - 1 )
	{
		AssertMsg1( false,
			"m_iThreadMutexValidationLock < ( sizeof(m_threadMutexValidationLock)/sizeof(m_threadMutexValidationLock[0]) ) (%d)",
			s_cMaxThreadMutexValidationLocks );
		return;
	}

	++m_iThreadMutexValidationLock;
	m_threadMutexValidationLock[m_iThreadMutexValidationLock] = pMutex;
}

void CValidator::UnlockValidationLocks()
{
	while ( m_iThreadMutexValidationLock >= 0 )
	{
		if ( m_threadMutexValidationLock[m_iThreadMutexValidationLock] )
		{
			m_threadMutexValidationLock[m_iThreadMutexValidationLock]->Unlock();
			m_threadMutexValidationLock[m_iThreadMutexValidationLock] = NULL;
		}

		--m_iThreadMutexValidationLock;
	}
}

void CValidator::InternalClaimBlock( void *pvMem, int cubUser /* = -1 */ )
{
	if ( !m_pValObjectCur )
		return;

	m_pValObjectCur->ClaimMemoryBlock( pvMem );

	if ( cubUser >= 0 )
	{
		m_pValObjectCur->m_cubMem = cubUser;
	}
}

bool CValidator::BExcludeAllocationFromTracking( const char *pchModuleName, int nSize )
{
	// Allocation-source exclusions compiled into the released tier0.dll.
	// Allocations tagged with these sources (CRT internals, server.dll,
	// steamclient.dll, gsoapwrapper.dll and the two crypto-lib headers) are
	// left out of leak tracking; the last two entries are size-qualified.
	struct Exclude_t
	{
		const char *pchModuleName;
		int nSize;
	};

	static const Exclude_t s_Exclusions[] =
	{
		{ "C-runtime internal", 0x00 },
		{ "server.dll",		    0x00 },
		{ "steamclient.dll",	0x00 },
		{ "gsoapwrapper.dll",	0x00 },
		{ "misc.h",			    0x43 },
		{ "secblock.h",		    0x57 },
	};

	if ( !pchModuleName )
		return false;

	for ( int i = 0; i < ARRAYSIZE( s_Exclusions ); ++i )
	{
		if ( _stricmp( pchModuleName, s_Exclusions[i].pchModuleName ) == 0
			&& nSize == s_Exclusions[i].nSize )
		{
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------

// Additional entry points required by the fixed-ordinal export table.

CValidator::CValidator( int nParam )
	: m_SharedMutex()
	, m_pValObjectFirst( NULL )
	, m_pValObjectLast( NULL )
	, m_pValObjectCur( NULL )
	, m_cpvOwned( 0 )
	, m_cubLeaked( 0 )
	, m_cpubLeaked( 0 )
	, m_bMemLeaks( false )
	, m_cBlocksSinceMinidump( 0 )
	, m_hHeap( NULL )
	, m_nExcludeAllocations( 0 )
	, m_rgpvptrExcludedMemoryBlocks( NULL )
	, m_rgbTouched( NULL )
	, m_rgb( NULL )
	, m_iThreadMutexValidationLock( -1 )
	, m_fastMutex()
{
	memset( m_threadMutexValidationLock, 0, sizeof( m_threadMutexValidationLock ) );
	UNREFERENCED_PARAMETER( nParam );
}

bool CValidator::BMemLeaks()
{
	return m_bMemLeaks;
}

CValidator& CValidator::operator=( const CValidator& other )
{
	if ( this == &other )
		return *this;

	// Release our own records first. Simply taking over the source pointers
	// (the original behaviour) left two CValidator objects owning one list,
	// and the second destructor to run freed every node a second time.
	CValObject *pObj = m_pValObjectFirst;

	while ( pObj )
	{
		CValObject *pNext = pObj->m_pValObjectNext;
		free( pObj );
		pObj = pNext;
	}

	m_pValObjectFirst	= NULL;
	m_pValObjectLast	= NULL;
	m_pValObjectCur		= NULL;

	// Deep-copy the source list. m_pchName / m_pvObj / m_pvMem are borrowed
	// pointers owned by whoever pushed the record, and m_pchType is an inline
	// array, so copying the node is safe -- only the links that point back
	// into the tree have to be re-pointed at the copies.
	CValObject *pPrevCopy = NULL;

	for ( const CValObject *pSrc = other.m_pValObjectFirst; pSrc; pSrc = pSrc->m_pValObjectNext )
	{
		CValObject *pCopy = ( CValObject * )malloc( sizeof( CValObject ) );

		if ( !pCopy )
			break;	// out of memory: keep the partial list, it stays self-consistent

		*pCopy = *pSrc;

		pCopy->m_pValObjectNext		= NULL;
		pCopy->m_pValObjectChild	= NULL;	// never populated by Push(); do not inherit a stale link
		pCopy->m_pValObjectParent	= NULL;

		// A parent is always an earlier node of the same list, so its copy has
		// already been made by the time we need it.
		if ( pSrc->m_pValObjectParent )
		{
			const CValObject *pSrcWalk	= other.m_pValObjectFirst;
			CValObject		 *pDstWalk	= m_pValObjectFirst;

			while ( pSrcWalk && pSrcWalk != pSrc->m_pValObjectParent )
			{
				pSrcWalk = pSrcWalk->m_pValObjectNext;
				pDstWalk = pDstWalk ? pDstWalk->m_pValObjectNext : NULL;
			}

			pCopy->m_pValObjectParent = pDstWalk;
		}

		if ( pPrevCopy )
			pPrevCopy->m_pValObjectNext = pCopy;
		else
			m_pValObjectFirst = pCopy;

		pPrevCopy		= pCopy;
		m_pValObjectLast = pCopy;

		if ( other.m_pValObjectCur == pSrc )
			m_pValObjectCur = pCopy;
	}

	m_bMemLeaks	= other.m_bMemLeaks;
	m_cpvOwned	= other.m_cpvOwned;
	m_cubLeaked	= other.m_cubLeaked;
	m_cpubLeaked = other.m_cpubLeaked;

	return *this;
}

CValObject *CValidator::PValObjectFirst()
{
	return m_pValObjectFirst;
}
