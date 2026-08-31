// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: MiniDump API (wraps DbgHelp.dll)
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef MINIDUMP_H
#define MINIDUMP_H

#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <eh.h>
#else
enum _EXCEPTION_DISPOSITION { ExceptionContinueExecution = 0, ExceptionContinueSearch = 1, ExceptionNestedException = 2, ExceptionCollidedUnwind = 3 };
struct EXCEPTION_POINTERS;
#define NULL 0
#endif

typedef void (*FnWMain)( int, tchar ** );
typedef void (*FnVoidPtrFn)( void * );
typedef void (*FnMiniDump)( unsigned int nExceptionCode, struct _EXCEPTION_POINTERS *pExceptionPointers );

PLATFORM_INTERFACE void WriteMiniDump();
PLATFORM_INTERFACE void CatchAndWriteMiniDump( FnWMain pfn, int argc, tchar *argv[] );
PLATFORM_INTERFACE void CatchAndWriteMiniDumpForVoidPtrFn( FnVoidPtrFn pvFn, FnMiniDump pfnMiniDump = NULL, bool bExitQuietly = false );

// Works from the vectored crash handler: writes a timestamped *.mdmp with the
// real exception pointers when the environment variable TIER0_MD=1 is set.
// Off by default so normal play stays lean; enable it only to debug a crash.
void WriteMiniDumpForException( unsigned int nExceptionCode, struct _EXCEPTION_POINTERS *pExceptionPointers );

bool BGetMiniDumpLock();
int MiniDumpUnlock();

#endif // MINIDUMP_H
