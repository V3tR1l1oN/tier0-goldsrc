// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Public interface for tier0's memory allocator (GoldSrc layout).
//
//=============================================================================//

#ifndef MEMALLOC_H
#define MEMALLOC_H

#include <stddef.h>
#include "platform.h"

struct _CrtMemState;

typedef void *(*MemAllocFailHandler_t)( size_t );

#define MEMALLOC_VERSION 1

abstract_class IMemAlloc
{
public:
	// Slot 0: Alloc_Debug
	virtual void *Alloc_Debug( size_t nSize, const char *pFileName, int nLine, int unknown = 0 ) = 0;
	// Slot 1: Alloc
	virtual void *Alloc( size_t nSize ) = 0;
	// Slot 2: Realloc_Debug
	virtual void *Realloc_Debug( void *pMem, size_t nSize, const char *pFileName, int nLine ) = 0;
	// Slot 3: Realloc
	virtual void *Realloc( void *pMem, size_t nSize ) = 0;
	// Slot 4: Free_Debug
	virtual void  Free_Debug( void *pMem, const char *pFileName, int nLine, int unknown = 0 ) = 0;
	// Slot 5: Free
	virtual void Free( void *pMem, int unknown = 0 ) = 0;
	// Slot 6: Expand_NoLongerSupported_Debug
	virtual void *Expand_NoLongerSupported_Debug( void *pMem, size_t nSize, const char *pFileName, int nLine, int unknown = 0 ) = 0;
	// Slot 7: Expand_NoLongerSupported
	virtual void *Expand_NoLongerSupported( void *pMem, size_t nSize ) = 0;

	// Slot 8: GetSize
	virtual size_t GetSize( void *pMem ) = 0;

	// Slot 9: PushAllocDbgInfo
	virtual void PushAllocDbgInfo( const char *pFileName, int nLine ) = 0;
	// Slot 10: PopAllocDbgInfo
	virtual void PopAllocDbgInfo() = 0;

	// Slot 11-17: CRT heap debug
	virtual long CrtSetBreakAlloc( long lNewBreakAlloc ) = 0;
	virtual int CrtSetReportMode( int nReportType, int nReportMode ) = 0;
	virtual int CrtIsValidHeapPointer( const void *pMem ) = 0;
	virtual int CrtIsValidPointer( const void *pMem, unsigned int size, int access ) = 0;
	virtual int CrtCheckMemory() = 0;
	virtual int CrtSetDbgFlag( int nNewFlag ) = 0;
	virtual void CrtMemCheckpoint( _CrtMemState *pState ) = 0;

	// Slot 18: DumpStats
	virtual void DumpStats() = 0;

	// Slot 19-21: Reports
	virtual void *CrtSetReportFile( int nRptType, void *hFile ) = 0;
	virtual void *CrtSetReportHook( void *pfnNewHook ) = 0;
	virtual int CrtDbgReport( int nRptType, const char *szFile,
			int nLine, const char *szModule, const char *pMsg ) = 0;

	// Slot 22: heapchk
	virtual int heapchk() = 0;

	// Slot 23: IsDebugHeap
	virtual bool IsDebugHeap() = 0;

	// Slot 24-26: Allocation tracking
	virtual void GetActualDbgInfo( const char *& pFileName, int & nLine ) = 0;
	virtual void RegisterAllocation( const char *pFileName, int nLine, int nLogicalSize, int nActualSize, unsigned nTime ) = 0;
	virtual void RegisterDeallocation( const char *pFileName, int nLine, int nLogicalSize, int nActualSize, unsigned nTime ) = 0;

	// Slot 27: GetVersion
	virtual int GetVersion() = 0;
	// Slot 28: CompactHeap
	virtual void CompactHeap() = 0;
	// Slot 29: SetAllocFailHandler
	virtual MemAllocFailHandler_t SetAllocFailHandler( MemAllocFailHandler_t pfnMemAllocFailHandler ) = 0;
};

extern "C" PLATFORM_INTERFACE IMemAlloc *g_pMemAlloc;

// SBArena filesystem helpers (mmap arena, zero-copy backing for FileSystem::GetReadBuffer)
// Allocated via VirtualAlloc (page-aligned, MEM_COMMIT|MEM_RESERVE) so Release is VirtualFree.
// Exported from tier0.dll for filesystem.cpp and for external callers.
extern "C" PLATFORM_INTERFACE void* SBArena_AllocForFileSystem(size_t nSize);
extern "C" PLATFORM_INTERFACE void  SBArena_FreeForFileSystem(void* p, size_t nSize);

#endif // MEMALLOC_H
