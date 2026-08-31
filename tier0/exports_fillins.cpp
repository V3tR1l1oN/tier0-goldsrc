// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Remaining tier0 exports restored from the shipped binary:
//			platform helpers, scratch memory, PME toggles, test harness
//			completion and spew validation.
//
// $NoKeywords: $
//
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include "threadtools.h"
#include "minidump.h"
#include "fasttimer.h"
#include "testthread.h"
#include "vprof.h"
#include "memalloc.h"

#ifdef _LINUX
#include <sys/prctl.h>
#endif

// Force the linker to retain the inline CThread dtor COMDAT. It is exported only
// through the .def alias "??1CThread@@UAE@XZ"="??1CThread@@QAE@XZ", and the alias
// alone does not root the COMDAT, so without this the link fails with LNK2001.
#pragma comment(linker, "/include:??1CThread@@QAE@XZ")

// Non-inline CThread dtor definition (declared in threadtools.h). Provides the
// plain external symbol the .def alias resolves to.
//
// Mirrors the released tier0.dll: if the thread is still running this is a
// programming error (assert-spew), then Stop(0), then Terminate(-1) if it is
// still alive, then CloseHandle.
CThread::~CThread()
{
	if ( m_hThread )
	{
		DWORD code = 0;

		if ( GetExitCodeThread( m_hThread, &code ) && code == STILL_ACTIVE )
		{
			// AssertMsg( exp, msg ) takes two arguments; passing one trips C4003
			// in NDEBUG and is a hard error in a debug build.
			AssertMsg( !"Cannot call ~CThread on a running thread", "" );

			Stop( 0 );

			if ( GetExitCodeThread( m_hThread, &code ) && code == STILL_ACTIVE )
			{
#pragma warning(push)
#pragma warning(disable: 4996)
				Terminate( -1 );
#pragma warning(pop)
			}
		}

		CloseHandle( m_hThread );
		m_hThread = NULL;
	}
}

#ifdef WIN32
#undef ARRAYSIZE
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof( p ) / sizeof( (p)[0] ) )
#endif
#include "winlite.h"
#endif

//=============================================================================
// Platform helpers
//=============================================================================

unsigned int Plat_MSTime()
{
	return ( unsigned int )( Plat_FloatTime() * 1000.0 );
}

bool Is64BitWindows()
{
#if defined( _WIN64 )
	return true;
#elif defined( _LINUX )
	// On a 64-bit Linux binary the process itself is 64-bit.
	return sizeof( void* ) == 8;
#else
	typedef BOOL ( WINAPI *LPFN_ISWOW64PROCESS )( HANDLE, PBOOL );

	LPFN_ISWOW64PROCESS fnIsWow64Process = ( LPFN_ISWOW64PROCESS )GetProcAddress(
		GetModuleHandleA( TEXT( "kernel32" ) ), "IsWow64Process" );

	BOOL bIsWow64 = FALSE;

	if ( NULL != fnIsWow64Process )
	{
		fnIsWow64Process( GetCurrentProcess(), &bIsWow64 );
	}

	return bIsWow64 != 0;
#endif
}

// Plat_Alloc / Free / Realloc MUST go through g_pMemAlloc exactly like the original!
// g_pMemAlloc is already fully thread-safe (per-thread lock-free TLS caches fronting
// the small-block allocator), so no extra per-call mutex is needed -- the original's
// critical section was only serializing calls because its backend was the raw CRT
// heap. Keeping the old lock here would bottleneck every Plat_* call instead of the
// allocator itself.
void *Plat_Alloc( unsigned long size )
{
	return g_pMemAlloc->Alloc( size );
}

void Plat_Free( void *pMem )
{
	g_pMemAlloc->Free( pMem );
}

void *Plat_Realloc( void *pMem, unsigned long newSize )
{
	return g_pMemAlloc->Realloc( pMem, newSize );
}

// Plat_PrimaryThreadID is DATA export in the original!
extern "C" unsigned long Plat_PrimaryThreadID = 0;

struct tagTHREADNAME_INFO
{
	DWORD dwType;
	LPCSTR szName;
	DWORD dwThreadID;
	DWORD dwFlags;
};

void Plat_SetThreadName( const char *pszName )
{
#ifdef _LINUX
	// POSIX: name the current thread via prctl (sets the caller thread's name).
	prctl( PR_SET_NAME, pszName ? pszName : "", 0, 0, 0 );
	return;
#else
	tagTHREADNAME_INFO info;

	info.dwType = 0x1000;
	info.szName = pszName;
	info.dwThreadID = ( DWORD )-1;
	info.dwFlags = 0;

	__try
	{
		RaiseException( 0x406D1388, 0, sizeof( info ) / sizeof( ULONG_PTR ), ( ULONG_PTR * )&info );
	}
	__except ( EXCEPTION_CONTINUE_EXECUTION )
	{
	}
#endif
}

//=============================================================================
// Hardware key stubs
//=============================================================================

void Plat_VerifyHardwareKey() {}
void Plat_VerifyHardwareKeyDriver() {}
void Plat_VerifyHardwareKeyPrompt() {}
int Plat_FastVerifyHardwareKey() { return 1; }

//=============================================================================
// Scratch memory buffers (uses g_pMemAlloc in original!)
//=============================================================================

#define SCRATCH_MAX_DEPTH 32

static void* s_pScratchBuf = NULL;
static size_t s_nScratchBufAllocated = 0;
static size_t s_nScratchBufMax = 0;
static int s_nScratchDepth = 0;
static size_t s_ScratchSizes[ SCRATCH_MAX_DEPTH ];
static CThreadFastMutex s_ScratchMutex;

PLATFORM_INTERFACE void *MemAllocScratch( unsigned long nBytes );
PLATFORM_INTERFACE void MemFreeScratch();

void *MemAllocScratch( unsigned long nBytes )
{
	s_ScratchMutex.Lock();

	// Depth guard: scratch is a shallow LIFO in practice. The old code wrapped
	// the size history through a %SCRATCH_MAX_DEPTH ring, so a 33rd nested
	// allocation silently overwrote an older slot and MemFreeScratch then
	// subtracted a stale size -- corrupting the accounting and handouts. Refuse
	// to grow beyond the depth cap instead of corrupting.
	if ( s_nScratchDepth >= SCRATCH_MAX_DEPTH )
	{
		s_ScratchMutex.Unlock();
		return NULL;
	}

	size_t needed = s_nScratchBufAllocated + nBytes;
	if ( needed > s_nScratchBufMax )
	{
		size_t newMax = needed < 0x100000 ? 0x100000 : needed;

		// Grow through a temporary. Assigning straight into s_pScratchBuf meant
		// a failed Realloc both leaked the old buffer and left NULL behind, and
		// the offset arithmetic at the end then handed out a bogus pointer.
		void *pNew = s_pScratchBuf
			? g_pMemAlloc->Realloc_Debug( s_pScratchBuf, newMax, "MemScratch", 46 )
			: g_pMemAlloc->Alloc_Debug( newMax, "MemScratch", 51, 0 );

		if ( !pNew )
		{
			s_ScratchMutex.Unlock();
			return NULL;
		}

		s_pScratchBuf = pNew;
		s_nScratchBufMax = newMax;
	}

	size_t offset = s_nScratchBufAllocated;
	s_nScratchBufAllocated += nBytes;

	// Index by exact depth (no modulo wrap): pop reads back the size it pushed.
	s_ScratchSizes[ s_nScratchDepth ] = nBytes;
	s_nScratchDepth++;

	void *pResult = ( byte * )s_pScratchBuf + offset;
	s_ScratchMutex.Unlock();
	return pResult;
}

void MemFreeScratch()
{
	s_ScratchMutex.Lock();
	if ( s_nScratchDepth > 0 )
	{
		s_nScratchDepth--;
		s_nScratchBufAllocated -= s_ScratchSizes[ s_nScratchDepth ];
	}
	s_ScratchMutex.Unlock();
}

//=============================================================================
// PME init / shutdown (PME == Pentium M events; stubs in non-PME builds)
//=============================================================================

PLATFORM_INTERFACE void InitPME();
PLATFORM_INTERFACE void ShutdownPME();

extern bool g_bPMELoaded;

void InitPME()
{
	// Original tier0.dll enable-passes only when the (long-gone) PME driver
	// loads AND the CPUID feature gate (bits 0xF00 mask) matches. On any modern
	// CPU that gate fails, so the original leaves g_bPMELoaded == false and
	// never touches process/thread priorities. Keep the same end result.
	g_bPMELoaded = false;
}

void ShutdownPME()
{
	// Original restores NORMAL_PRIORITY_CLASS / THREAD_PRIORITY_NORMAL only when
	// PME was enabled; since InitPME() never enables it here, the flag is cleared.
	g_bPMELoaded = false;
}

//=============================================================================
// MiniDump extra entry point + SetMiniDumpFunction
//=============================================================================

static FnMiniDump g_UserMiniDumpFn = NULL;

#ifdef _WIN32
void SetMiniDumpFunction( FnMiniDump pfn )
{
	g_UserMiniDumpFn = pfn;
}

static void MiniDumpWriterTrampoline( unsigned int uCode, struct _EXCEPTION_POINTERS *pExc )
{
	if ( g_UserMiniDumpFn )
		g_UserMiniDumpFn( uCode, pExc );
	else
		BGetMiniDumpLock(), MiniDumpUnlock();
}

void CatchAndWriteMiniDumpForVoidPtrFn( FnVoidPtrFn pvFn, FnMiniDump pfnMiniDump, bool bExitQuietly )
{
	if ( g_UserMiniDumpFn == NULL && pfnMiniDump != NULL )
		SetMiniDumpFunction( pfnMiniDump );

	__try
	{
		if ( pvFn )
			pvFn( NULL );
	}
	__except ( MiniDumpWriterTrampoline( GetExceptionCode(), GetExceptionInformation() ), EXCEPTION_CONTINUE_SEARCH )
	{
	}

	if ( bExitQuietly )
		exit( -1 );
}
#else
void SetMiniDumpFunction( FnMiniDump pfn )
{
	g_UserMiniDumpFn = pfn;
}
#endif
