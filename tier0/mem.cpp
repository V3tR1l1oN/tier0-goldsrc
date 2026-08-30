// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Fully crash-safe CStdMemAlloc implementation for GoldSrc.
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include <malloc.h>
#include <process.h>
#include <stdlib.h>

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

	bool g_sbaReady = false;    // flushes on thread detach must not touch a torn-down SBA

	// -----------------------------------------------------------------------------
	// Slab memory arena. Slab backing comes from one big virtual reservation that
	// is committed and prefaulted ahead of demand by a low-priority helper thread,
	// so map/resource loads hit warm pages instead of cold page-fault storms and
	// fresh-slab carve never pays lazy-commit syscalls on the critical path.
	// Frozen (fully returned) slabs recycle per size class without touching the OS.
	// Falls back to CRT malloc if the reservation fails. Tuning env vars:
	//   SBA_ARENA=0       disable the arena entirely
	//   SBA_RESERVE_MB=n  reservation size (default 512)
	//   SBA_ARENA_MB=n    prefault target (default min(reserve, max(64, freeRAM/8)))
	// -----------------------------------------------------------------------------

	enum { SBA_ARENA_4K = 4096, SBA_ARENA_8K = 8192 };

	struct SBArena
	{
		unsigned char *base;       // reserved VA block; NULL = off
		size_t reserve;            // reservation size
		size_t commitEnd;          // committed frontier (page-aligned)
		size_t handout;            // next fresh 4K chunk (page-aligned)
		size_t touched;            // prefaulted frontier (page-aligned)
		size_t target;             // prefault target bytes
		void *fl4k;                // recycled 4K chunks
		void *fl8k;                // recycled 8K chunks
		CRITICAL_SECTION cs;
		HANDLE hPrepage;
		HANDLE hWake;
		volatile long stop;

		SBArena()
			: base( NULL ), reserve( 0 ), commitEnd( 0 ), handout( 0 ), touched( 0 ),
			  target( 0 ), fl4k( NULL ), fl8k( NULL ), hPrepage( NULL ),
			  hWake( CreateEventA( NULL, TRUE, FALSE, NULL ) ), stop( 0 )
		{
			InitializeCriticalSection( &cs );
		}

		~SBArena()
		{
			if ( hPrepage )
			{
				InterlockedExchange( &stop, 1 );
				if ( hWake )
					SetEvent( hWake );

				// Never destroy the critical section or release the arena while
				// the helper can still touch them. If an unexpected shutdown bug
				// prevents the thread from exiting, leak the process-lifetime
				// resources rather than create a use-after-free during teardown.
				if ( WaitForSingleObject( hPrepage, 3000 ) != WAIT_OBJECT_0 )
					return;

				CloseHandle( hPrepage );
				hPrepage = NULL;
			}
			if ( hWake )
			{
				CloseHandle( hWake );
				hWake = NULL;
			}
			DeleteCriticalSection( &cs );
			if ( base )
				VirtualFree( base, 0, MEM_RELEASE );
		}
	};

	static SBArena g_arena;
	// 0=uninitialized, 1=initializing, 2=ready, 3=disabled/failed.
	static volatile long g_arenaState = 0;

	static unsigned __stdcall SBArena_PrepageThread( void * )
	{
		SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );

		for ( ;; )
		{
			if ( g_arena.stop )
				break;

			EnterCriticalSection( &g_arena.cs );

			size_t want = 0;
			if ( g_arena.touched < g_arena.target )
			{
				// Pages BELOW the handout frontier are live slabs (or committed
				// for allocation); only prefault the still-free region above it.
				// We hold cs here, so handout cannot move during the slice.
				if ( g_arena.touched < g_arena.handout )
					g_arena.touched = g_arena.handout;
				size_t limit = g_arena.target;
				if ( limit > g_arena.reserve )
					limit = g_arena.reserve;
				if ( limit > g_arena.touched )
					want = limit - g_arena.touched;
			}

			if ( want == 0 )
			{
				LeaveCriticalSection( &g_arena.cs );
				if ( g_arena.hWake )
				{
					if ( WaitForSingleObject( g_arena.hWake, 8 ) == WAIT_OBJECT_0 )
						break;
				}
				else
				{
					Sleep( 8 );
				}
				continue;
			}

			// Commit ahead of the touch in 1 MB slices, then touch under the
			// lock so the slice never overlaps a page handed out meanwhile.
			size_t grow = want < ( 1u << 20 ) ? want : ( 1u << 20 );
			size_t off = g_arena.touched;
			if ( g_arena.commitEnd < off + grow )
			{
				size_t add = off + grow - g_arena.commitEnd;
				if ( g_arena.commitEnd + add > g_arena.reserve )
					add = g_arena.reserve - g_arena.commitEnd;
				if ( add && !VirtualAlloc( g_arena.base + g_arena.commitEnd, add,
						MEM_COMMIT, PAGE_READWRITE ) )
					add = 0;
				g_arena.commitEnd += add;
				if ( add == 0 )
					grow = 0;
			}
			if ( grow )
			{
				for ( size_t p = off; p < off + grow; p += 4096 )
					*( volatile unsigned char * )( g_arena.base + p ) = 0;
				g_arena.touched = off + grow;
			}
			LeaveCriticalSection( &g_arena.cs );

			// If VirtualAlloc refused to commit, `grow` collapses to 0 and the
			// loop would re-request the same slice forever, pegging a core at
			// 100%. Back off before retrying so a low-memory condition cannot
			// turn into a permanent busy spin.
			if ( !grow )
				Sleep( 50 );
		}

		return 0;
	}

	static bool SBArena_Start()     // one-shot; may be called from any thread, not under any lock
	{
		LONG state = InterlockedCompareExchange( &g_arenaState, 0, 0 );
		if ( state == 2 )
			return true;
		if ( state == 3 )
			return false;

		if ( InterlockedCompareExchange( &g_arenaState, 1, 0 ) != 0 )
		{
			while ( ( state = InterlockedCompareExchange( &g_arenaState, 0, 0 ) ) == 1 )
				Sleep( 0 );
			return state == 2;
		}

		const char *pOff = getenv( "SBA_ARENA" );
		if ( pOff && pOff[ 0 ] == '0' )
		{
			InterlockedExchange( &g_arenaState, 3 );
			return false;
		}

		unsigned reserveMB = 512;
		const char *pRes = getenv( "SBA_RESERVE_MB" );
		if ( pRes )
		{
			unsigned v = ( unsigned )atoi( pRes );
			if ( v >= 16 && v <= 1024 )
				reserveMB = v;
		}

		unsigned char *b = ( unsigned char * )VirtualAlloc( NULL,
			(size_t)reserveMB << 20, MEM_RESERVE, PAGE_READWRITE );
		if ( !b )
		{
			InterlockedExchange( &g_arenaState, 3 );
			return false;
		}

		size_t freeRAM = 0;
		MEMORYSTATUSEX ms;
		ms.dwLength = sizeof( ms );
		if ( GlobalMemoryStatusEx( &ms ) )
			freeRAM = ( size_t )( ms.ullAvailPhys >> 20 );

		size_t targetMB = freeRAM / 8;
		if ( targetMB < 64 )
			targetMB = 64;
		if ( targetMB > (size_t)reserveMB )
			targetMB = reserveMB;

		const char *pTgt = getenv( "SBA_ARENA_MB" );
		if ( pTgt )
		{
			unsigned v = ( unsigned )atoi( pTgt );
			if ( v >= 1 && v <= 1024 )
				targetMB = v;
		}

		g_arena.base = b;
		g_arena.reserve = (size_t)reserveMB << 20;
		g_arena.target = targetMB << 20;

		const char *pPg = getenv( "SBA_PREPAGE" );
		if ( !pPg || pPg[ 0 ] != '0' )
		{
			unsigned threadID = 0;
			g_arena.hPrepage = ( HANDLE )_beginthreadex( NULL, 0, SBArena_PrepageThread,
				NULL, 0, ( unsigned * )&threadID );
		}
		if ( !g_arena.hPrepage )
			g_arena.hPrepage = NULL;

		InterlockedExchange( &g_arenaState, 2 );
		return true;
	}

	static bool SBArena_Stop()
	{
		LONG state = InterlockedCompareExchange( &g_arenaState, 0, 0 );
		if ( state != 2 )
			return true;

		InterlockedExchange( &g_arena.stop, 1 );
		if ( g_arena.hWake )
			SetEvent( g_arena.hWake );
		if ( !g_arena.hPrepage )
		{
			InterlockedExchange( &g_arenaState, 3 );
			return true;
		}

		if ( WaitForSingleObject( g_arena.hPrepage, 3000 ) != WAIT_OBJECT_0 )
			return false;

		CloseHandle( g_arena.hPrepage );
		g_arena.hPrepage = NULL;
		InterlockedExchange( &g_arenaState, 3 );
		return true;
	}

	static void *SBArena_Alloc( size_t bytes )     // 4096 or 8192
	{
		unsigned char *p = NULL;

		if ( g_arena.base )
		{
			EnterCriticalSection( &g_arena.cs );

			if ( bytes <= SBA_ARENA_4K )
			{
				if ( g_arena.fl4k ) { p = ( unsigned char * )g_arena.fl4k; g_arena.fl4k = *( void ** )p; }
			}
			else
			{
				if ( g_arena.fl8k ) { p = ( unsigned char * )g_arena.fl8k; g_arena.fl8k = *( void ** )p; }
			}

			if ( !p )
			{
				size_t need = ( bytes <= SBA_ARENA_4K ) ? SBA_ARENA_4K : SBA_ARENA_8K;
				if ( g_arena.handout + need <= g_arena.reserve )
				{
					p = g_arena.base + g_arena.handout;
					g_arena.handout += need;
				}
			}

			// Keep a warm cushion ahead of handout: when the engine allocates
			// faster than the prefault thread (map/resource load bursts), raise
			// the prefault target adaptively so warm pages keep arriving ahead
			// of the burst (capped at the reservation).
			if ( g_arena.touched < g_arena.handout + ( 8u << 20 ) &&
				 g_arena.target < g_arena.reserve )
			{
				size_t want = g_arena.handout + ( 16u << 20 );
				g_arena.target = want < g_arena.reserve ? want : g_arena.reserve;
			}

			if ( p )
			{
				size_t off = ( size_t )( p - g_arena.base );
				if ( off + bytes > g_arena.commitEnd )
				{
					size_t add = off + bytes - g_arena.commitEnd;
					add = ( add + 0xFFFFF ) & ~( size_t )0xFFFFF;   // 1 MB granularity
					if ( g_arena.commitEnd + add > g_arena.reserve )
						add = g_arena.reserve - g_arena.commitEnd;
					if ( !VirtualAlloc( g_arena.base + g_arena.commitEnd, add,
							MEM_COMMIT, PAGE_READWRITE ) )
					{
						// Rare out-of-RAM case: drop the fresh chunk (never touch
						// its uncommitted pages) and fall back to the CRT heap.
						p = NULL;
					}
					else
						g_arena.commitEnd += add;
				}
			}

			LeaveCriticalSection( &g_arena.cs );
		}

		return p ? p : ( unsigned char * )malloc( bytes );
	}

	static void SBArena_Free( void *pv, size_t bytes )     // 4096 or 8192
	{
		unsigned char *p = ( unsigned char * )pv;
		if ( !g_arena.base || p < g_arena.base || p >= g_arena.base + g_arena.reserve )
		{
			free( pv );          // allocated on the malloc fallback path
			return;
		}

		EnterCriticalSection( &g_arena.cs );
		if ( bytes <= SBA_ARENA_4K ) { *( void ** )p = g_arena.fl4k; g_arena.fl4k = p; }
		else { *( void ** )p = g_arena.fl8k; g_arena.fl8k = p; }
		LeaveCriticalSection( &g_arena.cs );
	}

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

	unsigned SBACapForSlot( unsigned slot )
	{
		// How many blocks a thread may keep in its own need-not-lock cache for
		// one bucket: bounded by bytes (~8 KB/bucket/thread) and by the batch
		// array size used on refill. Large slots keep a floor of 16 so a block
		// churn (alloc/free of the same big bucket) recycles within the thread
		// cache instead of tearing down and re-carving an 8K slab each cycle.
		unsigned cap = 8192 / slot;
		if ( cap < 16 )
			cap = 16;
		return ( cap > 192 ) ? 192 : cap;
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
		static const unsigned MAX_WALK = 64;

		struct Node
		{
			SBASlab *sl;
			unsigned char *base;
			unsigned char *end;      // slab extent, cached: the lock-free walk
			                         // must not read it through sl (see above)
			unsigned page;
			unsigned bucket;         // sl->bucket, cached for the same reason
			Node *next;
		};

		// tab/mask/cap are volatile because FindPage is called WITHOUT the lock
		// (SBAFastClassify). Publishing order is tab, then mask, then cap; a
		// lock-free reader that reads mask BEFORE tab (and gates on mask) can
		// only ever observe (old mask, old tab), (old mask, new tab) or
		// (new mask, new tab) -- never an out-of-bounds (new mask, old tab):
		// on x86/TSO the tab store precedes the mask store, so the moment the
		// mask store is visible to a reader the tab store is visible too.
		// Retired tables and retired nodes are never returned to the OS: slot
		// drops/chains that a concurrent reader is mid-walk stay mapped forever,
		// so a stale walk can only miss (falling through to LinearClassify under
		// the lock), never fault. Memory stays bounded by the live high-water
		// mark (each generation of the table is ~2x the previous one, and the
		// node pool peaks at the largest live slab count).
		//
		// CRITICAL: the *slab* is NOT covered by that guarantee. A concurrent
		// DestroySlab() frees the slab backing while a lock-free reader may be
		// walking a chain that still contains a node pointing at it -- so the
		// walk must never dereference Node::sl. Slab extent and bucket are
		// therefore cached in the node itself (see Node::end / Node::bucket).
		// With the arena on, SBArena_Free only recycles the chunk into a free
		// list, so a stale read returns garbage instead of faulting; with
		// SBA_ARENA=0 the slab goes to free() and the same read is a genuine
		// use-after-free.
		Node ** volatile tab;
		volatile unsigned cap;
		volatile unsigned mask;
		size_t used;
		Node *pool;              // retired nodes (never freed, reused on insert)
		Node ***retired;         // retired tab arrays (never freed)
		size_t retiredCount;

		SBASlabMap() : tab( nullptr ), cap( 0 ), mask( 0 ), used( 0 ),
			pool( nullptr ), retired( nullptr ), retiredCount( 0 ) {}

		~SBASlabMap()
		{
			for ( size_t i = 0; i < retiredCount; ++i )
				free( ( void * )retired[ i ] );
			free( ( void * )retired );
			if ( tab )
				free( ( void * )tab );
			for ( Node *n = pool; n; )
			{
				Node *nx = n->next;
				free( n );
				n = nx;
			}
		}

		void RetireTab( Node **t )       // caller holds the lock
		{
			Node ***nr = ( Node *** )realloc( retired, ( retiredCount + 1 ) * sizeof( Node ** ) );
			if ( !nr )
			{
				free( t );
				return;
			}
			retired = nr;
			retired[ retiredCount++ ] = t;
		}

		Node *GetNode()                  // caller holds the lock
		{
			if ( pool )
			{
				Node *n = pool;
				pool = n->next;
				return n;
			}
			return ( Node * )malloc( sizeof( Node ) );
		}

		void PutNode( Node *n )          // caller holds the lock
		{
			n->next = pool;
			pool = n;
		}

		bool Grow()                      // caller holds the lock
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
				RetireTab( ( Node ** )tab );
			}
			tab = nt;                    // publish table first...
			mask = newCap - 1;           // ...then the mask that indexes it,
			cap = newCap;                // ...then the write-side gate
			used = live;
			return true;
		}

		void InsertKey( SBASlab *s, unsigned page, unsigned char *addr )   // caller holds the lock
		{
			if ( cap == 0 || used * 2 >= cap )
			{
				if ( !Grow() )
					return;            // not indexed; linear scan covers it
			}
			Node *n = GetNode();
			if ( !n )
				return;
		n->sl = s;
		n->base = addr;
		n->end = addr + SBASlabBytes( s->bucket );
		n->page = page;
		n->bucket = s->bucket;
		unsigned j = page & mask;
			n->next = tab[ j ];
			tab[ j ] = n;
			++used;
		}

		void Insert( SBASlab *s )                          // caller holds the lock
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

		void Remove( SBASlab *sl, unsigned char *addr )                      // caller holds the lock
		{
			if ( !cap )
				return;
			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & mask;
			for ( Node **pp = ( Node ** )&tab[ j ]; *pp; pp = &( *pp )->next )
			{
				if ( ( *pp )->sl == sl )
				{
					Node *n = *pp;
					*pp = n->next;
					PutNode( n );
					--used;
					return;
				}
			}
		}

		SBASlab *FindPage( unsigned char *addr )        // may run WITHOUT the lock
		{
			unsigned m = mask;
			if ( !m )
				return nullptr;
			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & m;
			Node *n = ( Node * )tab[ j ];
			for ( unsigned hops = 0; n && hops < MAX_WALK; ++hops )
			{
				// Uses the cached node extent, NOT n->base + SBASlabBytes(
				// n->sl->bucket ): a concurrent DestroySlab may already have
				// freed the slab this node points at.
				if ( addr >= n->base && addr < n->end )
					return n->sl;      // shared page: the other owner holds the other range
				n = n->next;
			}
			return nullptr;
		}

		// Lock-free variant for SBAFastClassify. Same walk, but it hands back
		// the cached bucket and slab base instead of the SBASlab *, so the
		// caller can finish classifying without dereferencing slab memory at
		// all. Nodes are pooled and never returned to the OS, so reading node
		// fields during a raced walk is safe -- it can only MISS (the caller
		// then retries under the lock), never fault.
		unsigned FindBucket( unsigned char *addr, unsigned char **pSlabBase )
		{
			unsigned m = mask;
			if ( !m )
				return SBA_NO_BUCKET;

			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & m;
			Node *n = ( Node * )tab[ j ];
			for ( unsigned hops = 0; n && hops < MAX_WALK; ++hops )
			{
				if ( addr >= n->base && addr < n->end )
				{
					*pSlabBase = n->base;
					return n->bucket;
				}
				n = n->next;
			}
			return SBA_NO_BUCKET;
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
			g_sbaReady = true;
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
			g_sbaReady = false;
			// Stop the arena helper before returning slab backing. If it cannot
			// be joined, keep all SBA memory alive for process teardown instead of
			// freeing storage that the helper may still access.
			if ( !SBArena_Stop() )
				return;

			EnterCriticalSection( &m_cs );
			while ( m_slabs )
			{
				SBASlab *next = m_slabs->next;
				SBArena_Free( m_slabs->base, SBASlabBytes( m_slabs->bucket ) );
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

		unsigned PopBatch( unsigned bucket, unsigned want, void **out )   // caller holds the lock
		{
			// Drains the bucket's free blocks (and carves fresh slabs) into the
			// caller's array. One carve can fill the whole batch, so an alloc-only
			// workload pays for a slab once every (capacity) blocks instead of
			// every block.
			unsigned got = 0;
			unsigned slot = m_slot[ bucket ];

			while ( got < want )
			{
				SBASlab *sl = m_partial[ bucket ];
				while ( sl && sl->freeCount && got < want )
				{
					void *p = sl->freeHead;
					sl->freeHead = *( void ** )p;
					out[ got++ ] = p;
					if ( --sl->freeCount == 0 )
					{
						UnlinkPartial( sl, bucket );
						break;
					}
				}
				if ( got >= want )
					break;

				sl = m_cur[ bucket ];
				if ( !sl || sl->freePtr + slot > sl->end )
					sl = CarveSlab( bucket );
				if ( !sl )
					break;

				while ( sl->freePtr + slot <= sl->end && got < want )
				{
					unsigned char *block = sl->freePtr;
					sl->freePtr += slot;
					SBAHeader *h = ( SBAHeader * )block;
					h->magic = SBA_MAGIC;
					h->bucket = bucket;
					out[ got++ ] = block + SBA_HEADER;
				}
			}

			return got;
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
			SBArena_Start();
			unsigned char *mem = ( unsigned char * )SBArena_Alloc( slabBytes );
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
			SBArena_Free( sl->base, SBASlabBytes( sl->bucket ) );
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

	// -----------------------------------------------------------------------------
	// Per-thread block caches (fronting the global locked SBA).
	//
	// Alloc pops from the thread's own cache (no lock); when the cache is empty a
	// whole batch is grabbed from the global pool in one lock hold. Free pushes
	// to the thread's cache; when a bucket holds more than its cap, half of it is
	// returned to the global pool. Lock traffic drops from "every operation" to
	// "every cap blocks", and GetSize/Free of a live SBA block need no lock at
	// all. A block held on a thread keeps its slab alive (it is an outstanding
	// allocation there), so slabs are never destroyed early; the destructor
	// flushes all caches back to the global pool at thread detach.
	// -----------------------------------------------------------------------------
	struct TLSFreeBucket
	{
		void *head;          // intrusive free chain threaded through payloads
		unsigned count;
	};

	thread_local struct SBAThreadCaches
	{
		TLSFreeBucket b[ SBA_BUCKETS ];

		~SBAThreadCaches()
		{
			if ( !g_sbaReady )
				return;
			EnterCriticalSection( &g_SBA.m_cs );
			for ( unsigned k = 0; k < SBA_BUCKETS; ++k )
			{
				TLSFreeBucket &c = b[ k ];
				while ( c.head )
				{
					void *p = c.head;
					c.head = *( void ** )p;
					--c.count;
					g_SBA.Push( k, p );
				}
			}
			LeaveCriticalSection( &g_SBA.m_cs );
		}
	} g_tlCaches;

	// Classify a pointer without the lock. Resolves the address through the page
	// map first, validates it against the containing slab, and only then reads
	// the block header. A raced/destroyed slab simply fails the lookup and the
	// caller falls back to the locked path, so a genuine CRT pointer can never
	// be mistaken for a pool block.
	unsigned SBAFastClassify( void *p )
	{
		unsigned char *addr = ( unsigned char * )p;
		if ( !addr )
			return SBA_NO_BUCKET;

		// Validate BEFORE dereferencing anything derived from the pointer.
		// Free()/Realloc()/GetSize() are handed arbitrary pointers -- CRT
		// blocks, static data, sometimes junk -- and the block header sits
		// immediately BELOW the payload, so reading it first can fault on an
		// address that is not mapped at all.
		//
		// The page map is the right first step: it never touches the caller's
		// pointer (hashes the page number, masks into a bounded table, walks a
		// bounded chain of long-lived nodes, range-checks). It therefore proves
		// "this address is inside a slab known to the map" before we look at any
		// header, and it is safe for any bit pattern.
		//
		// NOTE: the arena range is deliberately NOT used as a pre-filter here.
		// The arena is only a *slab backing source*; when it is disabled
		// (SBA_ARENA=0) or the reservation fails, slabs come from malloc() and
		// live pool blocks sit on the CRT heap, entirely outside the arena.
		// Rejecting them would make Free() hand a pool block to free() and
		// corrupt the heap.
		//
		// A slab that was never indexed (map growth OOM) simply is not found;
		// the caller retries under the lock and LinearClassify covers it. That
		// is a miss, never a misclassification.
		unsigned char *slabBase = nullptr;
		const unsigned bucket = g_SBA.m_map.FindBucket( addr, &slabBase );
		if ( bucket == SBA_NO_BUCKET )
			return SBA_NO_BUCKET;

		// Inside a live slab, but still within the descriptor/guard area:
		// that is not a block payload, so there is no header behind it.
		if ( ( size_t )( addr - slabBase ) < SBA_HEADER + SBA_DESC_RESERVE )
			return SBA_NO_BUCKET;

		SBAHeader *h = ( SBAHeader * )( addr - SBA_HEADER );
		if ( h->magic != SBA_MAGIC || h->bucket != bucket )
			return SBA_NO_BUCKET;   // caller retries under the lock

		return h->bucket;
	}

	static void* AllocSBA( size_t nSize )
	{
		unsigned bucket = g_SBA.m_bucketOf[ nSize ];

		TLSFreeBucket &c = g_tlCaches.b[ bucket ];
		if ( c.count )
		{
			void *p = c.head;
			c.head = c.count > 1 ? *( void ** )p : nullptr;
			--c.count;
			return p;
		}

		unsigned want = SBACapForSlot( g_SBA.m_slot[ bucket ] );
		if ( want > 96 )
			want = 96;
		void *tmp[ 96 ];
		unsigned got;

		EnterCriticalSection( &g_SBA.m_cs );
		got = g_SBA.PopBatch( bucket, want, tmp );
		LeaveCriticalSection( &g_SBA.m_cs );

		if ( !got )
			return nullptr;

		if ( got > 1 )
		{
			for ( unsigned i = 0; i + 1 < got; ++i )
				*( void ** )tmp[ i ] = tmp[ i + 1 ];
			*( void ** )tmp[ got - 1 ] = nullptr;
		}
		c.head = ( got > 1 ) ? tmp[ 1 ] : nullptr;
		c.count = got - 1;
		return tmp[ 0 ];
	}

	static void TLSFree( unsigned bucket, void *p )
	{
		TLSFreeBucket &c = g_tlCaches.b[ bucket ];
		*( void ** )p = c.head;
		c.head = p;

		unsigned cap = SBACapForSlot( g_SBA.m_slot[ bucket ] );
		if ( ++c.count > cap )
		{
			unsigned leave = cap / 2;
			unsigned n = c.count - leave;
			EnterCriticalSection( &g_SBA.m_cs );
			while ( n-- && c.head )
			{
				void *q = c.head;
				c.head = *( void ** )q;
				--c.count;
				g_SBA.Push( bucket, q );
			}
			LeaveCriticalSection( &g_SBA.m_cs );
		}
	}
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

	// Classify without the lock when possible: same-pool realloc is the hot case.
	unsigned oldBucket = SBAFastClassify( pMem );
	if ( oldBucket == SBA_NO_BUCKET )
	{
		EnterCriticalSection( &g_SBA.m_cs );
		oldBucket = g_SBA.Classify( pMem );
		LeaveCriticalSection( &g_SBA.m_cs );
	}
	size_t oldSize;

	if ( oldBucket != SBA_NO_BUCKET )
	{
		oldSize = g_SBA.m_payload[ oldBucket ];
	}
	else
	{
		// HeapSize reports (SIZE_T)-1 for a pointer it does not own. Left
		// unchecked that becomes the copy length below and reads past the block.
		const size_t hs = ( size_t )HeapSize( ( HANDLE )_get_heap_handle(), 0, pMem );
		oldSize = ( hs == ( size_t )-1 ) ? 0 : hs;
	}

	if ( oldBucket == newBucket && oldBucket != SBA_NO_BUCKET )
		return pMem;                     // same pool: no move, no copy

	if ( oldBucket == SBA_NO_BUCKET && newBucket == SBA_NO_BUCKET )
	{
		// large -> large: plain CRT realloc with SEH fallback
		__try
		{
			p = realloc( pMem, nSize );
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			p = nullptr;
		}

		if ( !p )
		{
			p = malloc( nSize );
			if ( p )
			{
				memcpy( p, pMem, ( oldSize < nSize ) ? oldSize : nSize );
				__try { free( pMem ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
			}
		}

		if ( !p && g_pfnFailHandler )
			p = g_pfnFailHandler( nSize );
		return p;
	}

	if ( newBucket != SBA_NO_BUCKET )
	{
		// move into an SBA pool: copy, then retire the old block
		p = AllocSBA( nSize );
		if ( p )
			memcpy( p, pMem, ( oldSize < nSize ) ? oldSize : nSize );

		if ( oldBucket != SBA_NO_BUCKET )
		{
			// Only retire the old block once the replacement exists. Releasing it
			// unconditionally meant a failed allocation left the caller holding a
			// block that was already on the free list -- data loss plus a double
			// free the next time the caller frees or reallocs it.
			if ( p )
			{
				EnterCriticalSection( &g_SBA.m_cs );
				g_SBA.Push( oldBucket, pMem );
				LeaveCriticalSection( &g_SBA.m_cs );
			}
		}
		else
		{
			// old CRT block retired outside the lock
			if ( p )
				__try { free( pMem ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
		}

		if ( !p && g_pfnFailHandler )
			p = g_pfnFailHandler( nSize );
		return p;
	}

	// small -> large: move out of the SBA into the CRT heap
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

	// Fast path: live SBA block -> the thread's own cache, no lock at all.
	unsigned bucket = SBAFastClassify( pMem );
	if ( bucket != SBA_NO_BUCKET )
	{
		TLSFree( bucket, pMem );
		return;
	}

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

	// Fast path, no lock: a live SBA block carries its bucket in the header; the
	// page map confirms the block is still owned by a live slab. Genuine CRT
	// pointers are never inside a live slab range, so they fall through to
	// HeapSize below (matching the original behavior, minus the lock).
	unsigned bucket = SBAFastClassify( pMem );
	if ( bucket != SBA_NO_BUCKET )
		return g_SBA.m_payload[ bucket ];      // exact SBA pool size

	// Large CRT blocks: report the real usable size via the actual CRT heap.
#ifdef WIN32
	// HeapSize reports (SIZE_T)-1 for a pointer it does not own -- including
	// memory from another module's heap, a debug heap with an unknown block
	// type, or plain junk. Callers do arithmetic with GetSize() (copy
	// lengths, loop bounds), so a bogus SIZE_MAX is far worse than a
	// conservative 0. Same guard as Realloc().
	const size_t hs = ( size_t )HeapSize( ( HANDLE )_get_heap_handle(), 0, pMem );
	return ( hs == ( size_t )-1 ) ? 0 : hs;
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

	if ( g_arena.base )
	{
		size_t recycled4 = 0;
		size_t recycled8 = 0;

		EnterCriticalSection( &g_arena.cs );
		for ( void *q = g_arena.fl4k; q; q = *( void ** )q )
			++recycled4;
		for ( void *q = g_arena.fl8k; q; q = *( void ** )q )
			++recycled8;
		LeaveCriticalSection( &g_arena.cs );

		Msg( "SBA arena: reserve=%uMB commit=%uMB handout=%uMB "
			 "touched=%uMB target=%uMB recycled4K=%u recycled8K=%u\n",
			( unsigned )( g_arena.reserve >> 20 ),
			( unsigned )( g_arena.commitEnd >> 20 ),
			( unsigned )( g_arena.handout >> 20 ),
			( unsigned )( g_arena.touched >> 20 ),
			( unsigned )( g_arena.target >> 20 ),
			( unsigned )recycled4, ( unsigned )recycled8 );
	}
	else
	{
		Msg( "SBA arena: off (SBA_ARENA=0 or reservation failed)\n" );
	}

	Msg( "SBA: %u slabs, %u free blocks\n",
		( unsigned )slabs, ( unsigned )freeBlocks );
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
	return 2; // _HEAPOK (legacy MSVC CRT value, matches the era of the original DLL)
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