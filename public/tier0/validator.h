// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (MIT).
//
// Purpose: Memory validator (DBGFLAG_VALIDATE support).
//			Class layout mirrors the released tier0.dll
//			( CValidator size 0x830, CValObject 0x2C, verified via exports ).
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef VALIDATOR_H
#define VALIDATOR_H

#include "platform.h"
#include "threadtools.h"

class CValidator;
PLATFORM_INTERFACE void ValidateGlobals_internal( CValidator &validator );


class CValObject
{
public:
	void Init( tchar *pchType, void *pvObj, tchar *pchName, CValObject *pValObjectParent, CValObject *pValObjectPrev );
	~CValObject();

	void ClaimMemoryBlock( void *pvMem );
	void ClaimChildMemoryBlock( int cubUser );

	tchar			m_pchType[32];
	int				m_nTypeLength;	// offset 0x20 in binary
	void			*m_pvObj;		// 0x24
	tchar			*m_pchName;		// 0x28
	CValObject		*m_pValObjectParent;	// 0x2C
	CValObject		*m_pValObjectNext;		// 0x30
	CValObject		*m_pValObjectChild;		// 0x34
	void			*m_pvMem;				// 0x38
	int				m_cubMem;				// 0x3C
	bool			m_bMemClaimsOwnership;	// 0x40
};

class PLATFORM_CLASS CValidator
{
public:
	CValidator();
	explicit CValidator( int nParam );
	~CValidator();
	CValidator& operator=( const CValidator& other );

	void Push( const tchar *pchType, void *pvObj, const tchar *pchName );
	void Pop();
	void ClaimMemory( void *pvMem );
	void ClaimArrayMemory( void *pvMem );
	void Finalize();
	void RenderObjects( int cubThreshold );
	void RenderLeaks();
	bool BMemLeaks();
	CValObject *PValObjectFirst();
	CValObject *FindObject( void *pvObj );
	void DiffAgainst( CValidator *pOtherValidator );	// Removes any entries from this validator that are also present in the other.
	void Validate( CValidator &validator, tchar *pchName );

	void AddValidationLock( CThreadMutex *pMutex );
	void UnlockValidationLocks();

protected:
	void InternalClaimBlock( void *pvMem, int cubUser = -1 );

private:
	bool BExcludeAllocationFromTracking( const char *pchModuleName, int nSize );

private:
	CThreadMutex m_SharedMutex;

	// New objects created during validate session are stored linearly.
	CValObject		*m_pValObjectFirst;
	CValObject		*m_pValObjectLast;
	CValObject		*m_pValObjectCur;

	long long		m_cpvOwned;
	long long		m_cubLeaked;
	long long		m_cpubLeaked;

	bool			m_bMemLeaks;

	unsigned long	m_cBlocksSinceMinidump;

	HANDLE			m_hHeap;

	int				m_nExcludeAllocations;

	char			***m_rgpvptrExcludedMemoryBlocks;
	int				*m_rgbTouched;
	byte			**m_rgb;

	// Tracking validation locks
	static const int s_cMaxThreadMutexValidationLocks = 8;
	int				m_iThreadMutexValidationLock;
	CThreadMutex	*m_threadMutexValidationLock[s_cMaxThreadMutexValidationLocks];

	CThreadFastMutex m_fastMutex;
};

PLATFORM_INTERFACE void ValidateClass( const char *pchClassName, bool bTyped, ... );

#define VALIDATE_SCOPE_( pchType, pvObj )									\
	{																		\
		const char *pchObjectName = #pvObj;									\
		static CNamedVerifier( pchType, #pvObj ) ;							\
		validator.Push( pchType, &( pvObj ), pchObjectName )

#define VALIDATE_SCOPE				
#define VALIDATE_SCOPE_SAFE



#endif // VALIDATOR_H
