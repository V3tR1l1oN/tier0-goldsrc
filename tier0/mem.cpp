// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Fully crash-safe CStdMemAlloc implementation for GoldSrc.
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include <malloc.h>

#ifdef WIN32
#include "winlite.h"
#endif

#undef NO_MALLOC_OVERRIDE
#include "memalloc.h"

// -----------------------------------------------------------------------------
// Small Block Allocator (SBA)
//
// Faithful to the original CStdMemAlloc pool behavior observed in disasm:
//   * sizes <= 2048 are rounded -- size < 97 -> multiple of 4, else -> multiple
//     of 8 -- and are served from fixed-size pools;
//   * the pool block size equals the exact rounded size, which GetSize reports;
//   * user pointers are 8-byte aligned, carved from CRT-allocated slabs;
//   * larger requests bypass the SBA and use the CRT heap directly.
//
// The whole SBA is guarded by one critical section; CRT heap calls stay out of
// it where practical so the fail handler never recurses into the lock.
// -----------------------------------------------------------------------------

namespace
{
	const unsigned SBA_MAX = 2048;
	const unsigned SBA_SMALL_ROUND = 97;   // first size rounded to the next 8
	const unsigned SBA_BUCKETS = 11 + ( 2048 + 15 ) / 8;   // 24 (4..96) + 244 (104..2048) = 268
	const unsigned SBA_HEADER = 8;
	const unsigned SBA_SLAB_SIZE = 4096;
	const unsigned SBA_DESC_RESERVE = 48;   // slab descriptor (40 B) + safe pad
	const unsigned SBA_MAGIC = 0x54414253;               // 'SBAT'
	const unsigned SBA_NO_BUCKET = 0xFFFFFFFF;

	unsigned SBARound( size_t size )
	{
		if ( size == 0 )
			size = 1;

		if ( size < SBA_SMALL_ROUND )
			return ( unsigned )( ( size + 3 ) & ~( size_t )3 );

		return ( unsigned )( ( size + 7 ) & ~( size_t )7 );
	}

	unsigned SBABucket( unsigned roundSize )             // roundSize <= 2048
	{
		if ( roundSize <= 96 )
			return roundSize / 4 - 1;                    // 0..23 -> payload 4..96

		return 11 + roundSize / 8;                       // 24..267 -> payload 104..2048
	}

	unsigned SBAPayload( unsigned bucket )
	{
		if ( bucket < 24 )
			return ( bucket + 1 ) * 4;

		return ( bucket - 11 ) * 8;
	}

	unsigned SBASlot( unsigned payload )
	{
		// Block footprint is header(8) + payload; stride must keep the next
		// block's header clear of the previous block's data. When payload is a
		// multiple of 16 the naive 16-rounding of payload would put the next
		// header inside the previous data block -- so round payload+header.
		return ( payload + SBA_HEADER + 15 ) & ~15u;
	}

	unsigned SBASlabBytesForSlot( unsigned slot )
	{
		// Buckets whose slots fit fewer than two blocks per 4096-byte slab
		// (payload >= 2016; the 2048 bucket holds exactly one block and churns a
		// full slab per alloc/free) get a 8192-byte slab instead: same carve cost
		// amortised over 4 blocks and ~2x lower memory waste.
		return ( ( SBA_SLAB_SIZE - SBA_DESC_RESERVE ) / slot < 2 )
			? SBA_SLAB_SIZE * 2 : SBA_SLAB_SIZE;
	}

unsigned SBASlabBytes( unsigned bucket )
	{
		return SBASlabBytesForSlot( SBASlot( SBAPayload( bucket ) ) );
	}

	struct SBAHeader
	{
		unsigned magic;
		unsigned bucket;
	};

struct SBASlab
	{
		SBASlab *next;             // global slab list (doubly linked: prevLink)
		SBASlab *prevLink;
		SBASlab *freeNext;         // per-bucket list of slabs with free blocks
		unsigned bucket;
		unsigned payload;
		unsigned slot;             // block stride
		unsigned capacity;         // blocks the slab can hold
		unsigned freeCount;        // free blocks chained in freeHead
		void *freeHead;            // intrusive free chain of user pointers
		unsigned char *freePtr;    // next block to carve
		unsigned char *end;
		unsigned char *base;
	};

	// Slab lookup by 4K page. CRT returns 4096-byte slabs that are only
	// 16-byte aligned, so a slab routinely straddles TWO pages.  A page can be
	// owned by at most two slabs: an unaligned slab's tail page is shared with
	// the base page of the slab carved 4096 bytes later, and within a shared
	// page the two owners' byte ranges partition it.
	// Separate chaining: bucket = page mod cap; chains are short (a page alone
	// has <= 2 owners; only distinct pages colliding into one bucket lengthen
	// a chain), deletes are O(1) and leave NO tombstones, and rehashing simply
	// re-links live nodes so Grow never fails.
	struct SBASlabMap
	{
		static const unsigned CAP0 = 1024;

		struct Node
		{
			SBASlab *sl;
			unsigned char *base;
			unsigned page;
			Node *next;
		};

		Node **tab;
		unsigned cap;
		unsigned mask;
		size_t used;

		SBASlabMap() : tab( nullptr ), cap( 0 ), mask( 0 ), used( 0 ) {}

		~SBASlabMap()
		{
			if ( tab )
			{
				for ( unsigned i = 0; i < cap; ++i )
				{
					for ( Node *n = tab[ i ]; n; )
					{
						Node *nx = n->next;
						free( n );
						n = nx;
					}
				}
				free( tab );
			}
		}

		bool Grow()
		{
			unsigned newCap = cap ? cap * 2 : CAP0;
			Node **nt = ( Node ** )calloc( newCap, sizeof( Node * ) );
			if ( !nt )
				return false;
			size_t live = 0;
			if ( tab )
			{
				for ( unsigned i = 0; i < cap; ++i )
				{
					for ( Node *n = tab[ i ]; n; )
					{
						Node *nx = n->next;
						unsigned j = n->page & ( newCap - 1 );
						n->next = nt[ j ];
						nt[ j ] = n;
						++live;
						n = nx;
					}
				}
				free( tab );
			}
			tab = nt;
			cap = newCap;
			mask = newCap - 1;
			used = live;
			return true;
		}

		void InsertKey( SBASlab *s, unsigned page, unsigned char *addr )
		{
			if ( cap == 0 || used * 2 >= cap )
			{
				if ( !Grow() )
					return;            // not indexed; linear scan covers it
			}
			Node *n = ( Node * )malloc( sizeof( Node ) );
			if ( !n )
				return;
			n->sl = s;
			n->base = addr;
			n->page = page;
			unsigned j = page & mask;
			n->next = tab[ j ];
			tab[ j ] = n;
			++used;
		}

		void Insert( SBASlab *s )
		{
			unsigned p1 = ( unsigned )( ( size_t )s->base >> 12 );
			unsigned pLast = ( unsigned )( ( size_t )( s->base + SBASlabBytes( s->bucket ) - 1 ) >> 12 );
			for ( unsigned p = p1; ; ++p )
			{
				InsertKey( s, p, s->base );
				if ( p == pLast )
					break;
			}
		}

		void Remove( SBASlab *sl, unsigned char *addr )
		{
			if ( !cap )
				return;
			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & mask;
			for ( Node **pp = &tab[ j ]; *pp; pp = &( *pp )->next )
			{
				if ( ( *pp )->sl == sl )
				{
					Node *n = *pp;
					*pp = n->next;
					free( n );
					--used;
					return;
				}
			}
		}

		SBASlab *FindPage( unsigned char *addr )
		{
			if ( !cap )
				return nullptr;
			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & mask;
			for ( Node *n = tab[ j ]; n; n = n->next )
			{
				if ( addr >= n->base && addr < n->base + SBASlabBytes( n->sl->bucket ) )
					return n->sl;      // shared page: the other owner holds the other range
			}
			return nullptr;
		}
	};

	class CSmallBlockAlloc
	{
	public:
		CRITICAL_SECTION m_cs;
		SBASlab *m_slabs;
		SBASlab *m_cur[ SBA_BUCKETS ];
		SBASlab *m_partial[ SBA_BUCKETS ];   // slabs having free blocks
		SBASlabMap m_map;
		unsigned short m_bucketOf[ SBA_MAX + 1 ];   // size -> bucket (Alloc hot path), 0..267
		unsigned short m_payload[ SBA_BUCKETS ];   // bucket -> block payload
		unsigned short m_slot[ SBA_BUCKETS ];      // bucket -> block stride

		CSmallBlockAlloc()
		{
			InitializeCriticalSectionAndSpinCount( &m_cs, 4000 );
			m_slabs = nullptr;
			for ( unsigned i = 0; i < SBA_BUCKETS; ++i )
			{
				m_cur[ i ] = nullptr;
				m_partial[ i ] = nullptr;
				unsigned payload = SBAPayload( i );
				m_payload[ i ] = ( unsigned short )payload;
				m_slot[ i ] = ( unsigned short )SBASlot( payload );
			}
			for ( unsigned s = 0; s <= SBA_MAX; ++s )
				m_bucketOf[ s ] = ( unsigned short )SBABucket( SBARound( s ) );
		}

		~CSmallBlockAlloc()
		{
			EnterCriticalSection( &m_cs );
			while ( m_slabs )
			{
				SBASlab *next = m_slabs->next;
				free( m_slabs->base );
				m_slabs = next;
			}
			LeaveCriticalSection( &m_cs );
			DeleteCriticalSection( &m_cs );
		}

		unsigned Classify( void *p )                     // caller holds the lock
		{
			unsigned char *addr = ( unsigned char * )p;

			// Fast path: hash by the containing 4K page; FindPage verifies the owner
			// by range so a page shared by two slabs still resolves correctly.
			SBASlab *sl = m_map.FindPage( addr );
			unsigned bucket = SBA_NO_BUCKET;
			if ( sl )
			{
				size_t off = ( size_t )( addr - sl->base );
				if ( off >= SBA_HEADER + SBA_DESC_RESERVE )
				{
					SBAHeader *h = ( SBAHeader * )( addr - SBA_HEADER );
					if ( h->magic == SBA_MAGIC && h->bucket == sl->bucket )
						bucket = sl->bucket;
					else
						bucket = LinearClassify( addr );
				}
			}
			else
				bucket = LinearClassify( addr );

			return bucket;
		}

		unsigned LinearClassify( unsigned char *addr )   // caller holds the lock
		{
			for ( SBASlab *sl = m_slabs; sl; sl = sl->next )
			{
				if ( addr >= sl->base && addr < sl->end )
				{
					if ( ( size_t )( addr - sl->base ) < SBA_HEADER + SBA_DESC_RESERVE )
						break;                           // slab descriptor / guard area

					SBAHeader *h = ( SBAHeader * )( addr - SBA_HEADER );
					if ( h->magic == SBA_MAGIC && h->bucket == sl->bucket )
						return sl->bucket;

					break;
				}
			}

			return SBA_NO_BUCKET;
		}

		void *PopOrCarve( unsigned bucket )              // caller holds the lock
		{
			SBASlab *sl = m_partial[ bucket ];
			if ( sl )
			{
				void *p = sl->freeHead;
				sl->freeHead = *( void ** )p;
				if ( --sl->freeCount == 0 )
					UnlinkPartial( sl, bucket );
				return p;
			}

			unsigned slot = m_slot[ bucket ];
			sl = m_cur[ bucket ];
			if ( !sl || sl->freePtr + slot > sl->end )
				sl = CarveSlab( bucket );
			if ( !sl )
				return nullptr;

			unsigned char *block = sl->freePtr;
			sl->freePtr += slot;
			SBAHeader *h = ( SBAHeader * )block;
			h->magic = SBA_MAGIC;
			h->bucket = bucket;
			return block + SBA_HEADER;
		}

		void Push( unsigned bucket, void *p )            // caller holds the lock
		{
			SBASlab *sl = m_map.FindPage( ( unsigned char * )p );
			if ( !sl )
				sl = LinearFindSlab( ( unsigned char * )p );
			if ( !sl )
				return;                              // leak the block rather than crash

			if ( sl->freeCount == 0 )
			{
				sl->freeNext = m_partial[ bucket ];
				m_partial[ bucket ] = sl;
			}

			*( void ** )p = sl->freeHead;            // intrusive chain in the payload
			sl->freeHead = p;
			++sl->freeCount;

			// Frozen slab: all its blocks freed and no room left to carve.
			if ( sl->freeCount == sl->capacity && sl->freePtr + sl->slot > sl->end )
				DestroySlab( sl, bucket );
		}

		void UnlinkPartial( SBASlab *target, unsigned bucket )
		{
			SBASlab **pp = &m_partial[ bucket ];
			while ( *pp )
			{
				if ( *pp == target )
				{
					*pp = target->freeNext;
					return;
				}
				pp = &( *pp )->freeNext;
			}
		}

		SBASlab *CarveSlab( unsigned bucket )            // caller holds the lock
		{
			unsigned payload = SBAPayload( bucket );
			unsigned slot = SBASlot( payload );
			unsigned slabBytes = SBASlabBytesForSlot( slot );
			unsigned char *mem = ( unsigned char * )malloc( slabBytes );
			if ( !mem )
				return nullptr;

			SBASlab *sl = ( SBASlab * )mem;
			sl->next = m_slabs;
			sl->prevLink = nullptr;
			sl->freeNext = nullptr;
			sl->bucket = bucket;
			sl->payload = payload;
			sl->slot = slot;
			sl->capacity = ( slabBytes - SBA_DESC_RESERVE ) / slot;
			sl->freeCount = 0;
			sl->freeHead = nullptr;
			sl->base = mem;
			sl->end = mem + slabBytes;
			sl->freePtr = mem + SBA_DESC_RESERVE;
			if ( m_slabs )
				m_slabs->prevLink = sl;
			m_slabs = sl;
			m_cur[ bucket ] = sl;
			m_map.Insert( sl );
			return sl;
		}

		void DestroySlab( SBASlab *sl, unsigned bucket )
		{
			UnlinkPartial( sl, bucket );
			if ( m_cur[ bucket ] == sl )
				m_cur[ bucket ] = nullptr;

			unsigned char *base = sl->base;
			unsigned lastPage = ( unsigned )( ( size_t )( base + SBASlabBytes( sl->bucket ) - 1 ) >> 12 );
			for ( unsigned p = ( unsigned )( ( size_t )base >> 12 ); ; ++p )
			{
				m_map.Remove( sl, ( unsigned char * )( ( size_t )p << 12 ) );
				if ( p == lastPage )
					break;
			}

			if ( sl->prevLink )
				sl->prevLink->next = sl->next;
			else
				m_slabs = sl->next;
			if ( sl->next )
				sl->next->prevLink = sl->prevLink;
			free( sl->base );
		}

		SBASlab *LinearFindSlab( unsigned char *addr )   // caller holds the lock
		{
			for ( SBASlab *sl = m_slabs; sl; sl = sl->next )
			{
				if ( addr >= sl->base && addr < sl->end )
					return sl;
			}
			return nullptr;
		}
	};

	CSmallBlockAlloc g_SBA;
}

// --------------------------------------------------------------------------------
// Crash-safe memory allocator wrapping CRT heap with SEH guards
// --------------------------------------------------------------------------------
class CStdMemAlloc : public IMemAlloc
{
public:
	// Slot 0
	void* Alloc_Debug( size_t nSize, const char* pFileName, int nLine, int unknown = 0 ) override;
	// Slot 1
	void* Alloc( size_t nSize ) override;
	// Slot 2
	void* Realloc_Debug( void* pMem, size_t nSize, const char* pFileName, int nLine ) override;
	// Slot 3
	void* Realloc( void *pMem, size_t nSize ) override;
	// Slot 4
	void  Free_Debug( void* pMem, const char* pFileName, int nLine, int unknown = 0 ) override;
	// Slot 5
	void Free( void *pMem, int unknown = 0 ) override;
	// Slot 6
	void* Expand_NoLongerSupported_Debug( void* pMem, size_t nSize, const char* pFileName, int nLine, int unknown = 0 ) override;
	// Slot 7
	void* Expand_NoLongerSupported( void* pMem, size_t nSize ) override;

	// Slot 8
	size_t GetSize( void* pMem ) override;

	// Slot 9
	void PushAllocDbgInfo( const char* pFileName, int nLine ) override;
	// Slot 10
	void PopAllocDbgInfo() override;

	// Slot 11-17
	long CrtSetBreakAlloc( long lNewBreakAlloc ) override;
	int CrtSetReportMode( int nReportType, int nReportMode ) override;
	int CrtIsValidHeapPointer( const void *pMem ) override;
	int CrtIsValidPointer( const void* pMem, unsigned int size, int access ) override;
	int CrtCheckMemory() override;
	int CrtSetDbgFlag( int nNewFlag ) override;
	void CrtMemCheckpoint( _CrtMemState* pState ) override;

	// Slot 18
	void DumpStats() override;

	// Slot 19-21
	void* CrtSetReportFile( int nRptType, void* hFile ) override;
	void* CrtSetReportHook( void* pfnNewHook ) override;
	int CrtDbgReport( int nRptType, const char* szFile,
					  int nLine, const char* szModule, const char* pMsg ) override;

	// Slot 22
	int heapchk() override;

	// Slot 23
	bool IsDebugHeap() override;

	// Slot 24-26
	void GetActualDbgInfo( const char *& pFileName, int & nLine ) override;
	void RegisterAllocation( const char *pFileName, int nLine, int nLogicalSize, int nActualSize, unsigned nTime ) override;
	void RegisterDeallocation( const char *pFileName, int nLine, int nLogicalSize, int nActualSize, unsigned nTime ) override;

	// Slot 27
	int GetVersion() override;
	// Slot 28
	void CompactHeap() override;
	// Slot 29
	MemAllocFailHandler_t SetAllocFailHandler( MemAllocFailHandler_t pfnMemAllocFailHandler ) override;
};

static CStdMemAlloc g_MemAlloc;
IMemAlloc* g_pMemAlloc = &g_MemAlloc;

static MemAllocFailHandler_t g_pfnFailHandler = nullptr;

// SBA fast path: returns a pool block for nSize <= SBA_MAX, or NULL.
static void* AllocSBA( size_t nSize )
{
	unsigned bucket = g_SBA.m_bucketOf[ nSize ];

	EnterCriticalSection( &g_SBA.m_cs );
	void* p = g_SBA.PopOrCarve( bucket );
	LeaveCriticalSection( &g_SBA.m_cs );

	return p;
}

void* CStdMemAlloc::Alloc_Debug( size_t nSize, const char* pFileName, int nLine, int unknown )
{
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
	UNREFERENCED_PARAMETER( unknown );
	return Alloc( nSize );
}

void* CStdMemAlloc::Alloc( size_t nSize )
{
	if ( nSize == 0 )
		nSize = 1;

	if ( nSize <= SBA_MAX )
	{
		void *p = AllocSBA( nSize );
		if ( !p && g_pfnFailHandler )
			p = g_pfnFailHandler( nSize );
		return p;
	}

	void *p = malloc( nSize );
	if ( !p && g_pfnFailHandler )
		p = g_pfnFailHandler( nSize );
	return p;
}

void* CStdMemAlloc::Realloc_Debug( void* pMem, size_t nSize, const char* pFileName, int nLine )
{
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
	return Realloc( pMem, nSize );
}

void* CStdMemAlloc::Realloc( void* pMem, size_t nSize )
{
	// CRITICAL: Original tier0.dll does NOT free memory when newSize == 0!
	// Instead it allocates a new 1-byte block (see disasm at 0x100044EC: mov ebx,1; cmovne ebx,edx).
	// realloc(p, 0) would FREE the block and return NULL, which caused
	// use-after-free crashes in the past -- so normalize 0 -> 1 first,
	// exactly like Alloc() does.
	if ( nSize == 0 )
		nSize = 1;

	if ( !pMem )
		return Alloc( nSize );

	unsigned newBucket = ( nSize <= SBA_MAX ) ? g_SBA.m_bucketOf[ nSize ] : SBA_NO_BUCKET;
	void *p = nullptr;

	EnterCriticalSection( &g_SBA.m_cs );
	unsigned oldBucket = g_SBA.Classify( pMem );
	size_t oldSize = ( oldBucket != SBA_NO_BUCKET )
		? g_SBA.m_payload[ oldBucket ]
		: ( size_t )HeapSize( ( HANDLE )_get_heap_handle(), 0, pMem );

	if ( oldBucket == newBucket && oldBucket != SBA_NO_BUCKET )
	{
		LeaveCriticalSection( &g_SBA.m_cs );
		return pMem;                     // same pool: no move, no copy
	}

	if ( oldBucket == SBA_NO_BUCKET && newBucket == SBA_NO_BUCKET )
	{
		// large -> large: plain CRT realloc with SEH fallback
		LeaveCriticalSection( &g_SBA.m_cs );

		void *q = nullptr;
		__try
		{
			q = realloc( pMem, nSize );
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			q = nullptr;
		}

		if ( !q )
		{
			q = malloc( nSize );
			if ( q )
			{
				memcpy( q, pMem, ( oldSize < nSize ) ? oldSize : nSize );
				__try { free( pMem ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
			}
		}

		if ( !q && g_pfnFailHandler )
			q = g_pfnFailHandler( nSize );
		return q;
	}

	if ( newBucket != SBA_NO_BUCKET )
	{
		// move into an SBA pool: copy, then retire the old block
		p = g_SBA.PopOrCarve( newBucket );
		if ( p )
			memcpy( p, pMem, ( oldSize < nSize ) ? oldSize : nSize );

		if ( oldBucket != SBA_NO_BUCKET )
		{
			g_SBA.Push( oldBucket, pMem );
			LeaveCriticalSection( &g_SBA.m_cs );
		}
		else
		{
			LeaveCriticalSection( &g_SBA.m_cs );
			if ( p )
			{
				// old CRT block retired outside the lock
				__try { free( pMem ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
			}
		}

		if ( !p && g_pfnFailHandler )
			p = g_pfnFailHandler( nSize );
		return p;
	}

	// small -> large: move out of the SBA into the CRT heap
	LeaveCriticalSection( &g_SBA.m_cs );
	p = malloc( nSize );
	if ( p )
	{
		memcpy( p, pMem, ( oldSize < nSize ) ? oldSize : nSize );
		EnterCriticalSection( &g_SBA.m_cs );
		g_SBA.Push( oldBucket, pMem );
		LeaveCriticalSection( &g_SBA.m_cs );
	}

	if ( !p && g_pfnFailHandler )
		p = g_pfnFailHandler( nSize );
	return p;
}

void CStdMemAlloc::Free_Debug( void* pMem, const char* pFileName, int nLine, int unknown )
{
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
	UNREFERENCED_PARAMETER( unknown );
	Free( pMem, 0 );
}

void CStdMemAlloc::Free( void* pMem, int unknown )
{
	UNREFERENCED_PARAMETER( unknown );
	if ( !pMem )
		return;

	unsigned bucket;

	EnterCriticalSection( &g_SBA.m_cs );
	bucket = g_SBA.Classify( pMem );
	if ( bucket != SBA_NO_BUCKET )
		g_SBA.Push( bucket, pMem );
	LeaveCriticalSection( &g_SBA.m_cs );

	if ( bucket == SBA_NO_BUCKET )
	{
		__try
		{
			free( pMem );
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			// Non-CRT heap pointer, ignore safely
		}
	}
}

void* CStdMemAlloc::Expand_NoLongerSupported_Debug( void* pMem, size_t nSize, const char* pFileName, int nLine, int unknown )
{
	UNREFERENCED_PARAMETER( pMem );
	UNREFERENCED_PARAMETER( nSize );
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
	UNREFERENCED_PARAMETER( unknown );
	return nullptr;
}

void* CStdMemAlloc::Expand_NoLongerSupported( void* pMem, size_t nSize )
{
	UNREFERENCED_PARAMETER( pMem );
	UNREFERENCED_PARAMETER( nSize );
	return nullptr;
}

size_t CStdMemAlloc::GetSize( void* pMem )
{
	if ( !pMem )
		return 0;

	EnterCriticalSection( &g_SBA.m_cs );
	unsigned bucket = g_SBA.Classify( pMem );
	if ( bucket != SBA_NO_BUCKET )
	{
		size_t s = g_SBA.m_payload[ bucket ];       // exact SBA pool size
		LeaveCriticalSection( &g_SBA.m_cs );
		return s;
	}
	LeaveCriticalSection( &g_SBA.m_cs );

	// Large CRT blocks: report the real usable size via the actual CRT heap.
#ifdef WIN32
	return ( size_t )HeapSize( ( HANDLE )_get_heap_handle(), 0, pMem );
#else
	return 0;
#endif
}

void CStdMemAlloc::PushAllocDbgInfo( const char* pFileName, int nLine )
{
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
}

void CStdMemAlloc::PopAllocDbgInfo()
{
}

long CStdMemAlloc::CrtSetBreakAlloc( long lNewBreakAlloc )
{
	UNREFERENCED_PARAMETER( lNewBreakAlloc );
	return 0;
}

int CStdMemAlloc::CrtSetReportMode( int nReportType, int nReportMode )
{
	UNREFERENCED_PARAMETER( nReportType );
	UNREFERENCED_PARAMETER( nReportMode );
	return 0;
}

int CStdMemAlloc::CrtIsValidHeapPointer( const void* pMem )
{
	return pMem != nullptr;
}

int CStdMemAlloc::CrtIsValidPointer( const void* pMem, unsigned int size, int access )
{
	UNREFERENCED_PARAMETER( size );
	UNREFERENCED_PARAMETER( access );
	return pMem != nullptr;
}

int CStdMemAlloc::CrtCheckMemory()
{
	return 1;
}

int CStdMemAlloc::CrtSetDbgFlag( int nNewFlag )
{
	UNREFERENCED_PARAMETER( nNewFlag );
	return 0;
}

void CStdMemAlloc::CrtMemCheckpoint( _CrtMemState* pState )
{
	UNREFERENCED_PARAMETER( pState );
}

void CStdMemAlloc::DumpStats()
{
	size_t slabs = 0;
	size_t freeBlocks = 0;

	EnterCriticalSection( &g_SBA.m_cs );
	for ( SBASlab *sl = g_SBA.m_slabs; sl; sl = sl->next )
	{
		++slabs;
		freeBlocks += sl->freeCount;
	}
	LeaveCriticalSection( &g_SBA.m_cs );

	Msg( "SBA: %u slabs, %u free blocks\n",
		(unsigned)slabs, (unsigned)freeBlocks );
}

void* CStdMemAlloc::CrtSetReportFile( int nRptType, void* hFile )
{
	UNREFERENCED_PARAMETER( nRptType );
	UNREFERENCED_PARAMETER( hFile );
	return nullptr;
}

void* CStdMemAlloc::CrtSetReportHook( void* pfnNewHook )
{
	UNREFERENCED_PARAMETER( pfnNewHook );
	return nullptr;
}

int CStdMemAlloc::CrtDbgReport( int nRptType, const char* szFile,
			  int nLine, const char* szModule, const char* pMsg )
{
	UNREFERENCED_PARAMETER( nRptType );
	UNREFERENCED_PARAMETER( szFile );
	UNREFERENCED_PARAMETER( nLine );
	UNREFERENCED_PARAMETER( szModule );
	UNREFERENCED_PARAMETER( pMsg );
	return 0;
}

int CStdMemAlloc::heapchk()
{
	return -2; // _HEAPOK in original MSVC CRT (not UCRT's 0)
}

bool CStdMemAlloc::IsDebugHeap()
{
	return false;
}

void CStdMemAlloc::GetActualDbgInfo( const char*& pFileName, int& nLine )
{
	pFileName = "";
	nLine = 0;
}

void CStdMemAlloc::RegisterAllocation( const char* pFileName, int nLine, int nLogicalSize, int nActualSize, unsigned nTime )
{
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
	UNREFERENCED_PARAMETER( nLogicalSize );
	UNREFERENCED_PARAMETER( nActualSize );
	UNREFERENCED_PARAMETER( nTime );
}

void CStdMemAlloc::RegisterDeallocation( const char* pFileName, int nLine, int nLogicalSize, int nActualSize, unsigned nTime )
{
	UNREFERENCED_PARAMETER( pFileName );
	UNREFERENCED_PARAMETER( nLine );
	UNREFERENCED_PARAMETER( nLogicalSize );
	UNREFERENCED_PARAMETER( nActualSize );
	UNREFERENCED_PARAMETER( nTime );
}

int CStdMemAlloc::GetVersion()
{
	return 0;
}

void CStdMemAlloc::CompactHeap()
{
#ifdef WIN32
	_heapmin();
#endif
}

MemAllocFailHandler_t CStdMemAlloc::SetAllocFailHandler( MemAllocFailHandler_t pfnMemAllocFailHandler )
{
	MemAllocFailHandler_t prev = g_pfnFailHandler;
	g_pfnFailHandler = pfnMemAllocFailHandler;
	return prev;
}