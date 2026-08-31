// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Fully crash-safe CStdMemAlloc implementation for GoldSrc.
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include <malloc.h>
#ifdef _WIN32
#include <process.h>
#endif
#include <stdlib.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <climits>

#ifdef WIN32
#include "winlite.h"
#endif

#ifdef _LINUX
#include <sys/mman.h>
#include <unistd.h>
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
		std::atomic<long> stop;

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
				stop.store( 1, std::memory_order_release );
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
#ifdef _LINUX
				munmap( base, g_arena.reserve );
#else
				VirtualFree( base, 0, MEM_RELEASE );
#endif
		}
	};

	static SBArena g_arena;
	// 0=uninitialized, 1=initializing, 2=ready, 3=disabled/failed.
	static std::atomic<long> g_arenaState{0};

	static unsigned __stdcall SBArena_PrepageThread( void * )
	{
#ifdef _LINUX
		// Thread priority is a POSIX scheduling nicety; keep normal priority on Linux.
		(void)0;
#else
		SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
#endif

		for ( ;; )
		{
			if ( g_arena.stop.load( std::memory_order_acquire ) )
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

			// Reserve a 1 MB slice under the lock, then release the lock before
			// the slow VirtualAlloc / page-touch syscalls.
			size_t grow = want < ( 1u << 20 ) ? want : ( 1u << 20 );
			size_t off = g_arena.touched;
			size_t add = 0;
			size_t commitBase = g_arena.commitEnd;
			if ( g_arena.commitEnd < off + grow )
			{
				add = off + grow - g_arena.commitEnd;
				if ( g_arena.commitEnd + add > g_arena.reserve )
					add = g_arena.reserve - g_arena.commitEnd;
				// Reserve commit range under the lock so no other thread overlaps.
				g_arena.commitEnd += add;
			}
			// Reserve touched range as well - advance touched optimistically
			// so concurrent allocators see it as reserved; rollback on failure.
			// Keep original off for touch; touched already reserved below.
			LeaveCriticalSection( &g_arena.cs );

			bool commitOk = true;
			if ( add )
			{
#ifdef _LINUX
				// The arena was created with a PROT_NONE anonymous reservation
				// (line ~270); committing a sub-range grants rw access via a
				// MAP_FIXED remap of that known, page-aligned VA range.
				if ( mmap( g_arena.base + commitBase, add, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0 ) == MAP_FAILED )
					commitOk = false;
#else
				if ( !VirtualAlloc( g_arena.base + commitBase, add, MEM_COMMIT, PAGE_READWRITE ) )
					commitOk = false;
#endif
			}
			if ( commitOk && grow )
			{
				for ( size_t p = off; p < off + grow; p += 4096 )
					*( volatile unsigned char * )( g_arena.base + p ) = 0;
			}

			EnterCriticalSection( &g_arena.cs );
			if ( !commitOk )
			{
				// Roll back the commit reservation; touched stays where it was.
				g_arena.commitEnd = commitBase;
				grow = 0;
			}
			else if ( grow )
			{
				// Publish touched only after successful touch.
				// off+grow is still the expected frontier; if another thread
				// advanced touched concurrently, keep the max.
				if ( g_arena.touched < off + grow )
					g_arena.touched = off + grow;
			}
			else
			{
				// grow was 0 already
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
		long state = g_arenaState.load( std::memory_order_acquire );
		if ( state == 2 )
			return true;
		if ( state == 3 )
			return false;

		long expected = 0;
		if ( !g_arenaState.compare_exchange_strong( expected, 1, std::memory_order_acq_rel ) )
		{
			while ( ( state = g_arenaState.load( std::memory_order_acquire ) ) == 1 )
				Sleep( 0 );
			return state == 2;
		}

		auto parseEnvULong = []( const char *s, unsigned long *out ) -> bool {
			if ( !s || !*s ) return false;
			char *end = nullptr;
			errno = 0;
			unsigned long v = strtoul( s, &end, 10 );
			if ( errno != 0 || end == s || *end != '\0' )
				return false;
			*out = v;
			return true;
		};

		const char *pOff = getenv( "SBA_ARENA" );
		if ( pOff && pOff[ 0 ] == '0' )
		{
			g_arenaState.store( 3, std::memory_order_release );
			return false;
		}

		unsigned reserveMB = 512;
		const char *pRes = getenv( "SBA_RESERVE_MB" );
		if ( pRes )
		{
			unsigned long v = 0;
			if ( parseEnvULong( pRes, &v ) && v >= 16 && v <= 1024 )
				reserveMB = (unsigned)v;
		}

#ifdef _LINUX
		// MEM_RESERVE on Linux: a PROT_NONE anonymous reservation. Pages stay
		// inaccessible until the allocator commits sub-ranges (MAP_FIXED remaps).
		void *mm = mmap( NULL, (size_t)reserveMB << 20, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
		unsigned char *b = ( mm == MAP_FAILED ) ? NULL : ( unsigned char * )mm;
#else
		unsigned char *b = ( unsigned char * )VirtualAlloc( NULL,
			(size_t)reserveMB << 20, MEM_RESERVE, PAGE_READWRITE );
#endif
		if ( !b )
		{
			g_arenaState.store( 3, std::memory_order_release );
			return false;
		}

#ifdef _LINUX
		size_t freeRAM = ( size_t )( ( ( uint64_t )sysconf( _SC_AVPHYS_PAGES )
			* sysconf( _SC_PAGESIZE ) ) >> 20 );
#else
		size_t freeRAM = 0;
		MEMORYSTATUSEX ms;
		ms.dwLength = sizeof( ms );
		if ( GlobalMemoryStatusEx( &ms ) )
			freeRAM = ( size_t )( ms.ullAvailPhys >> 20 );
#endif

		size_t targetMB = freeRAM / 8;
		if ( targetMB < 64 )
			targetMB = 64;
		if ( targetMB > (size_t)reserveMB )
			targetMB = reserveMB;

		const char *pTgt = getenv( "SBA_ARENA_MB" );
		if ( pTgt )
		{
			unsigned long v = 0;
			if ( parseEnvULong( pTgt, &v ) && v >= 1 && v <= 1024 )
				targetMB = (size_t)v;
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

		g_arenaState.store( 2, std::memory_order_release );
		return true;
	}

	static bool SBArena_Stop()
	{
		long state = g_arenaState.load( std::memory_order_acquire );
		if ( state != 2 )
			return true;

		g_arena.stop.store( 1, std::memory_order_release );
		if ( g_arena.hWake )
			SetEvent( g_arena.hWake );
		if ( !g_arena.hPrepage )
		{
			g_arenaState.store( 3, std::memory_order_release );
			return true;
		}

		if ( WaitForSingleObject( g_arena.hPrepage, 3000 ) != WAIT_OBJECT_0 )
			return false;

		CloseHandle( g_arena.hPrepage );
		g_arena.hPrepage = NULL;
		g_arenaState.store( 3, std::memory_order_release );
		return true;
	}

	static void *SBArena_Alloc( size_t bytes )     // 4096 or 8192
	{
		unsigned char *p = NULL;
		if ( !g_arena.base )
			return ( unsigned char * )malloc( bytes );

		size_t need = ( bytes <= SBA_ARENA_4K ) ? SBA_ARENA_4K : SBA_ARENA_8K;
		bool fromHandout = false;
		size_t savedHandout = 0;
		size_t off = 0;
		size_t add = 0;
		size_t commitBase = 0;

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
			if ( g_arena.handout + need <= g_arena.reserve )
			{
				savedHandout = g_arena.handout;
				p = g_arena.base + g_arena.handout;
				g_arena.handout += need;
				fromHandout = true;
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
			off = ( size_t )( p - g_arena.base );
			if ( off + need > g_arena.commitEnd )
			{
				add = off + need - g_arena.commitEnd;
				add = ( add + 0xFFFFF ) & ~( size_t )0xFFFFF;   // 1 MB granularity
				if ( g_arena.commitEnd + add > g_arena.reserve )
					add = g_arena.reserve - g_arena.commitEnd;
				commitBase = g_arena.commitEnd;
				// Reserve commit range will be published after successful VirtualAlloc outside lock.
				// Do NOT advance commitEnd yet.
				if ( add == 0 )
				{
					// Reserve exhausted - cannot commit, treat as failure
					// p will be rolled back below
				}
			}
		}

		LeaveCriticalSection( &g_arena.cs );

		if ( p && add )
		{
#ifdef _LINUX
			if ( add == 0 ||
				mmap( g_arena.base + commitBase, add, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0 ) == MAP_FAILED )
#else
			if ( add == 0 || !VirtualAlloc( g_arena.base + commitBase, add, MEM_COMMIT, PAGE_READWRITE ) )
#endif
			{
				// Roll back handout on failure (VA leak fix)
				EnterCriticalSection( &g_arena.cs );
				if ( fromHandout )
				{
					// Only rollback if we are still at the frontier; otherwise
					// another thread has already allocated beyond us - keep
					// handout to avoid discarding their reservation.
					if ( g_arena.handout == savedHandout + need )
						g_arena.handout = savedHandout;
					// else leak the VA range but keep correctness (rare race)
				}
				LeaveCriticalSection( &g_arena.cs );
				p = NULL;
			}
			else
			{
				EnterCriticalSection( &g_arena.cs );
				// Publish commit; handle concurrent commit advances by taking max
				if ( g_arena.commitEnd < commitBase + add )
					g_arena.commitEnd = commitBase + add;
				LeaveCriticalSection( &g_arena.cs );
			}
		}
		else if ( p && off + need > commitBase + add && add == 0 )
		{
			// No commit space left - rollback handout
			EnterCriticalSection( &g_arena.cs );
			if ( fromHandout && g_arena.handout == savedHandout + need )
				g_arena.handout = savedHandout;
			LeaveCriticalSection( &g_arena.cs );
			p = NULL;
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

	// tab/mask/cap are std::atomic with release/acquire because FindPage/
	// FindBucket are called WITHOUT the lock (SBAFastClassify). Publishing
	// order is tab, then mask, then cap; a lock-free reader that reads mask
	// BEFORE tab (and gates on mask) can only ever observe (old mask, old
	// tab), (old mask, new tab) or (new mask, new tab) -- never an
	// out-of-bounds (new mask, old tab): with release/acquire the tab store
	// happens-before the mask store being visible. Retired tables are never
	// returned to the OS and nodes are not mutated in place during Grow
	// (copied instead), so a stale walk can only miss (falling through to
	// LinearClassify under the lock), never fault or see a torn next pointer.
	// Memory stays bounded by the live high-water mark (each generation of
	// the table is ~2x the previous one, and retired tables stay mapped).
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
	std::atomic<Node**> tab;
	std::atomic<unsigned> cap;
	std::atomic<unsigned> mask;
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
			Node **t = tab.load( std::memory_order_acquire );
			if ( t )
				free( ( void * )t );
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
			unsigned curCap = cap.load( std::memory_order_acquire );
			unsigned newCap = curCap ? curCap * 2 : CAP0;
			Node **nt = ( Node ** )calloc( newCap, sizeof( Node * ) );
			if ( !nt )
				return false;

			size_t live = 0;
			Node **oldTab = tab.load( std::memory_order_acquire );
			if ( oldTab )
			{
				for ( unsigned i = 0; i < curCap; ++i )
				{
					for ( Node *n = oldTab[ i ]; n; n = n->next )
					{
						Node *c = ( Node * )malloc( sizeof( Node ) );
						if ( !c )
						{
							// OOM during copy: free what we allocated for new table
							for ( unsigned k = 0; k < newCap; ++k )
							{
								for ( Node *x = nt[ k ]; x; )
								{
									Node *nx = x->next;
									free( x );
									x = nx;
								}
							}
							free( nt );
							return false;
						}
						*c = *n;
						unsigned j = c->page & ( newCap - 1 );
						c->next = nt[ j ];
						nt[ j ] = c;
						++live;
					}
				}
				RetireTab( oldTab );
			}
			tab.store( nt, std::memory_order_release );                    // publish table first...
			mask.store( newCap - 1, std::memory_order_release );           // ...then the mask that indexes it,
			cap.store( newCap, std::memory_order_release );                // ...then the write-side gate
			used = live;
			return true;
		}

		void InsertKey( SBASlab *s, unsigned page, unsigned char *addr )   // caller holds the lock
		{
			unsigned curCap = cap.load( std::memory_order_acquire );
			if ( curCap == 0 || used * 2 >= curCap )
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
			unsigned curMask = mask.load( std::memory_order_acquire );
			Node **curTab = tab.load( std::memory_order_acquire );
			unsigned j = page & curMask;
			n->next = curTab[ j ];
			curTab[ j ] = n;
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
			unsigned curCap = cap.load( std::memory_order_acquire );
			if ( !curCap )
				return;
			unsigned curMask = mask.load( std::memory_order_acquire );
			Node **curTab = tab.load( std::memory_order_acquire );
			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & curMask;
			for ( Node **pp = &curTab[ j ]; *pp; pp = &( *pp )->next )
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
			unsigned m = mask.load( std::memory_order_acquire );
			if ( !m )
				return nullptr;
			Node **t = tab.load( std::memory_order_acquire );
			if ( !t )
				return nullptr;
			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & m;
			Node *n = t[ j ];
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
			unsigned m = mask.load( std::memory_order_acquire );
			if ( !m )
				return SBA_NO_BUCKET;

			Node **t = tab.load( std::memory_order_acquire );
			if ( !t )
				return SBA_NO_BUCKET;

			unsigned j = ( ( unsigned )( ( size_t )addr >> 12 ) ) & m;
			Node *n = t[ j ];
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
		if ( !g_sbaReady )
			return nullptr;
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
		if ( !g_sbaReady )
			return;
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

// -----------------------------------------------------------------------------
// SBArena filesystem helpers -- mmap arena for FileSystem::GetReadBuffer
// Exported as SBArena_AllocForFileSystem / SBArena_FreeForFileSystem.
// Uses VirtualAlloc page-aligned COMMIT|RESERVE (true mmap arena) so the
// filesystem can cache a file without memcpy and free with VirtualFree.
// For tiny files (<=2048) we route through g_pMemAlloc (SBA) to reuse slabs.
// -----------------------------------------------------------------------------
extern "C" PLATFORM_INTERFACE void* SBArena_AllocForFileSystem(size_t nSize)
{
	if (nSize == 0) nSize = 1;
	// Small files: SBA path (fast, 8-byte aligned slabs, no VirtualAlloc overhead)
	if (nSize <= 2048 && g_pMemAlloc)
	{
		void *p = g_pMemAlloc->Alloc(nSize);
		if (p) return p;
	}
	size_t aligned = (nSize + 4095) & ~(size_t)4095;
	if (aligned < 4096) aligned = 4096;
#ifdef _LINUX
	{
		void *pm = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		void *p = (pm == MAP_FAILED) ? NULL : pm;
		if (p) return p;
	}
#else
	void *p = VirtualAlloc(NULL, aligned, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (p) return p;
#endif
	// fallback: CRT / g_pMemAlloc
	if (g_pMemAlloc) return g_pMemAlloc->Alloc(nSize);
	return malloc(nSize);
}

extern "C" PLATFORM_INTERFACE void SBArena_FreeForFileSystem(void* p, size_t nSize)
{
	(void)nSize;
	if (!p) return;
#ifdef _LINUX
	// Mirror the allocator's routing: the mmap arena is only used when the
	// small SBA/CRT path cannot serve the request (nSize > 2048, or no
	// g_pMemAlloc installed). Such blocks are plain anonymous mmaps; release
	// them with munmap using the reconstructed page-aligned size. Small
	// SBA/CRT blocks fall through to g_pMemAlloc->Free / free below.
	if ( nSize > 2048 || !g_pMemAlloc )
	{
		size_t aligned = ( nSize + 4095 ) & ~( size_t )4095;
		if ( aligned < 4096 ) aligned = 4096;
		munmap( p, aligned );
		return;
	}
#else
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi)
	{
		// MEM_MAPPED views (MapViewOfFile) are unmapped, not VirtualFree'd
		if (mbi.Type == MEM_MAPPED)
		{
			UnmapViewOfFile(p);
			return;
		}
		if (mbi.AllocationBase == p && mbi.State == MEM_COMMIT)
		{
			VirtualFree(p, 0, MEM_RELEASE);
			return;
		}
	}
#endif
	// SBA / CRT path
	if (g_pMemAlloc)
	{
		// g_pMemAlloc->Free knows SBA vs CRT
		g_pMemAlloc->Free(p);
		return;
	}
	free(p);
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
#ifdef _LINUX
		// malloc_usable_size reports the real usable CRT block size without
		// faulting on memory the process does not own (returns 0 for junk).
		const size_t hs = malloc_usable_size( pMem );
#else
		// HeapSize reports (SIZE_T)-1 for a pointer it does not own. Left
		// unchecked that becomes the copy length below and reads past the block.
		const size_t hs = ( size_t )HeapSize( ( HANDLE )_get_heap_handle(), 0, pMem );
#endif
		oldSize = ( hs == ( size_t )-1 ) ? 0 : hs;
	}

	if ( oldBucket == newBucket && oldBucket != SBA_NO_BUCKET )
		return pMem;                     // same pool: no move, no copy

	if ( oldBucket == SBA_NO_BUCKET && newBucket == SBA_NO_BUCKET )
	{
		// large -> large: plain CRT realloc with SEH fallback
#ifdef _LINUX
		p = realloc( pMem, nSize );
#else
		__try
		{
			p = realloc( pMem, nSize );
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			p = nullptr;
		}
#endif

		if ( !p )
		{
			p = malloc( nSize );
			if ( p )
			{
				memcpy( p, pMem, ( oldSize < nSize ) ? oldSize : nSize );
#ifdef _LINUX
				free( pMem );
#else
				__try { free( pMem ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
#endif
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
#ifdef _LINUX
				free( pMem );
#else
				__try { free( pMem ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
#endif
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
#ifdef _LINUX
		free( pMem );
#else
		__try
		{
			free( pMem );
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			// Non-CRT heap pointer, ignore safely
		}
#endif
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

static bool IsAccessibleSpan( const void* pMem, size_t size, int bWrite )
{
	// Mirrors the crash handler's guarded memory check (tier0.cpp): validate
	// that [pMem, pMem+size) is committed and carries the requested access.
	// VirtualQuery on a junk pointer fails safely, so this never faults.
	if ( !pMem )
		return false;
	if ( size > (size_t)( ~(size_t)0 - (size_t)pMem ) )
		return false;

#ifdef _LINUX
	// Best-effort access validator. The Windows VirtualQuery/commit/protect
	// semantics have no direct POSIX equivalent; these checks are defensive
	// (kernel-enforced page protection still faults on genuinely bad access),
	// so report true for any well-formed pointer without probing.
	return true;
#else
	MEMORY_BASIC_INFORMATION mbi;
	unsigned char* addr = ( unsigned char* )pMem;
	unsigned char* const end = addr + size;

	while ( addr < end )
	{
		if ( VirtualQuery( addr, &mbi, sizeof( mbi ) ) != sizeof( mbi ) )
			return false;
		if ( mbi.State != MEM_COMMIT || ( mbi.Protect & ( PAGE_GUARD | PAGE_NOACCESS ) ) )
			return false;
		const unsigned char* regionEnd = ( const unsigned char* )mbi.BaseAddress + mbi.RegionSize;
		if ( !( mbi.Protect & ( PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
			| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY ) ) )
			return false;
		if ( bWrite && !( mbi.Protect & ( PAGE_READWRITE | PAGE_WRITECOPY
			| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY ) ) )
			return false;
		addr = ( unsigned char* )regionEnd;
	}
	return true;
#endif
}

int CStdMemAlloc::CrtIsValidHeapPointer( const void* pMem )
{
	// Honest validity instead of "non-NULL" fiction. A pointer is valid if it is
	// a live SBA pool block or a genuine CRT-heap block -- the same ownership
	// classification GetSize() uses. NULL, foreign or junk pointers are invalid.
	//
	// Gate on cheap accessible-region first: an unmapped/junk pointer is not a
	// live heap block and must fail fast (the SBA walk / HeapValidate below are
	// only ever run against real, readable addresses, so they terminate quickly
	// and never fault). HeapValidate(..., lpMem) validates a single candidate
	// block safely and returns FALSE for anything not live in that heap.
	if ( !pMem || !IsAccessibleSpan( pMem, 1, 0 ) )
		return 0;

	if ( SBAFastClassify( const_cast<void*>( pMem ) ) != SBA_NO_BUCKET )
		return 1;

#ifdef WIN32
	return HeapValidate( ( HANDLE )_get_heap_handle(), 0, pMem ) ? 1 : 0;
#else
	return 0;
#endif
}

int CStdMemAlloc::CrtIsValidPointer( const void* pMem, unsigned int size, int access )
{
	// access == 0 -> readable; nonzero -> writable. Actually probe the span
	// instead of returning "non-NULL" for arbitrary garbage pointers.
	return IsAccessibleSpan( pMem, size, access != 0 ) ? 1 : 0;
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
	// Run a genuine integrity check of the CRT heap instead of returning the
	// era constant unconditionally. On a healthy heap this returns _HEAPOK
	// (=2), preserving the documented/expected value; only real corruption now
	// surfaces as a non-_HEAPOK error code instead of being silently masked.
	//
	// _heapchk() itself is unusable here: this CRT returns -2 (0xFFFFFFFE) on
	// an entirely healthy heap in this allocator/CRT configuration, which would
	// falsely flag corruption. HeapValidate on the actual CRT heap handle is the
	// authoritative check and correctly reports TRUE on a healthy heap.
	// Additionally validates SBA slabs under lock: magic, freeCount, range,
	// alignment -- any broken slab returns _HEAPBADNODE.
#ifdef WIN32
	if ( !HeapValidate( ( HANDLE )_get_heap_handle(), 0, NULL ) )
		return _HEAPBADNODE;

	EnterCriticalSection( &g_SBA.m_cs );
	for ( SBASlab *sl = g_SBA.m_slabs; sl; sl = sl->next )
	{
		bool bad = false;
		if ( sl->bucket >= SBA_BUCKETS )
			bad = true;
		else if ( sl->capacity == 0 )
			bad = true;
		else if ( sl->freeCount > sl->capacity )
			bad = true;
		else if ( !sl->base || !sl->end || sl->end <= sl->base )
			bad = true;
		else
		{
			unsigned expectedBytes = SBASlabBytes( sl->bucket );
			if ( ( size_t )( sl->end - sl->base ) != expectedBytes )
				bad = true;
			else if ( sl->payload != SBAPayload( sl->bucket ) )
				bad = true;
			else if ( sl->slot != SBASlot( sl->payload ) )
				bad = true;
			else if ( sl->freePtr < sl->base + SBA_DESC_RESERVE || sl->freePtr > sl->end )
				bad = true;
			else if ( !sl->freeHead && sl->freeCount != 0 )
				bad = true;
			else if ( sl->freeHead )
			{
				unsigned char *fh = ( unsigned char * )sl->freeHead;
				if ( fh < sl->base + SBA_DESC_RESERVE + SBA_HEADER || fh >= sl->end )
					bad = true;
				else if ( ( ( uintptr_t )fh & 7 ) != 0 )
					bad = true;
			}
		}

		if ( !bad && sl->freeHead )
		{
			void *cur = sl->freeHead;
			unsigned counted = 0;
			while ( cur )
			{
				if ( counted >= sl->freeCount )
				{
					bad = true;
					break;
				}
				unsigned char *p = ( unsigned char * )cur;
				if ( p < sl->base + SBA_DESC_RESERVE + SBA_HEADER || p + sl->payload > sl->end )
				{
					bad = true;
					break;
				}
				if ( ( ( uintptr_t )p & 7 ) != 0 )
				{
					bad = true;
					break;
				}
				__try
				{
					SBAHeader *h = ( SBAHeader * )( p - SBA_HEADER );
					if ( h->magic != SBA_MAGIC || h->bucket != sl->bucket )
						bad = true;
				}
				__except ( EXCEPTION_EXECUTE_HANDLER )
				{
					bad = true;
				}
				if ( bad )
					break;
				cur = *( void ** )cur;
				++counted;
				if ( counted > sl->capacity )
				{
					bad = true;
					break;
				}
			}
			if ( !bad && counted != sl->freeCount )
				bad = true;
		}

		if ( bad )
		{
			LeaveCriticalSection( &g_SBA.m_cs );
			return _HEAPBADNODE;
		}
	}
	LeaveCriticalSection( &g_SBA.m_cs );
	return 2; // _HEAPOK
#else
	return 2; // _HEAPOK
#endif
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