// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Fully crash-safe CStdMemAlloc implementation for GoldSrc.
//
//=============================================================================//

#include "platform.h"
#include <malloc.h>

#ifdef WIN32
#include "winlite.h"
#endif

#undef NO_MALLOC_OVERRIDE
#include "memalloc.h"





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

	// Small Block Allocator emulation:
	// Original rounds sizes <= 2048:
	//   size < 97  -> round up to multiple of 4
	//   size >= 97 -> round up to multiple of 8
	// (matches the original's pool-based allocator exactly:
	//  Alloc(1..4) -> 4, Alloc(5..8) -> 8, Alloc(9..12) -> 12,
	//  Alloc(97..104) -> 104, Alloc(2041..2048) -> 2048)
	if ( nSize <= 2048 )
	{
		if ( nSize < 97 )
			nSize = ( nSize + 3 ) & ~(size_t)3;
		else
			nSize = ( nSize + 7 ) & ~(size_t)7;
	}

	void *p = malloc( nSize );
	if ( !p && g_pfnFailHandler )
	{
		p = g_pfnFailHandler( nSize );
	}
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

	// Same rounding as Alloc for small blocks
	size_t nRounded = nSize;
	if ( nRounded <= 2048 )
	{
		if ( nRounded < 97 )
			nRounded = ( nRounded + 3 ) & ~(size_t)3;
		else
			nRounded = ( nRounded + 7 ) & ~(size_t)7;
	}

	void *p = nullptr;
	__try
	{
		p = realloc( pMem, nRounded );
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		size_t oldSize = GetSize( pMem );
		p = Alloc( nSize );
		if ( p && oldSize > 0 )
		{
			size_t copySize = oldSize < nSize ? oldSize : nSize;
			memcpy( p, pMem, copySize );
		}
	}

	if ( !p && g_pfnFailHandler )
	{
		p = g_pfnFailHandler( nSize );
	}
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

	__try
	{
		free( pMem );
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		// Non-CRT heap pointer, ignore safely
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

	// Use HeapSize over the actual CRT heap instead of _msize:
	// _msize calls _fastfail on invalid pointers which cannot be caught by SEH,
	// while HeapSize safely returns 0 for pointers that don't belong to the heap.
	// _get_heap_handle() returns the exact heap the modern UCRT allocates from
	// (typically the process heap), which makes GetSize report the REAL usable
	// size of CRT blocks -- matching the original tier0 behavior.
#ifdef WIN32
	return (size_t)HeapSize( (HANDLE)_get_heap_handle(), 0, pMem );
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
