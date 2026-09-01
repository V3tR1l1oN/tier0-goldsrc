// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Companion test-thread harness (tier0/testthread.cpp) -- companion to
//          testthread2.cpp. Original shipped binary contained both TUs; this
//          file is retained for 100% 1:1 tier0.so inclusion. On Linux it is a
//          no-op stub that compiles with g++ -D_LINUX.
//
// $NoKeywords: $
//=============================================================================//

#include "platform.h"
#include "dbg.h"
#include "testthread.h"
#include "threadtools.h"

#ifdef _LINUX
#include <string.h>
#include <stdio.h>
#ifndef _T
#define _T(x) x
#endif
#ifndef _sntprintf
#define _sntprintf snprintf
#endif
#ifndef _vsntprintf
#define _vsntprintf vsnprintf
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#ifndef strncpy_s
// Emulate MSVC strncpy_s(dst,dstSize,src,count) via strncpy + NUL-terminate. Handles _TRUNCATE.
#define strncpy_s(dst, dstSize, src, count) (strncpy((dst), (src), ((count)==_TRUNCATE ? (dstSize)-1 : (count))), (dst)[(dstSize)-1]='\0', 0)
#endif
#ifndef CreateThread
// Minimal CreateThread shim for -D_LINUX compilation. Test harness is no-op on Linux;
// pthread_create mapping would require proc-type conversion, stub is sufficient for 1:1 inclusion.
inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, DWORD (WINAPI *)(LPVOID), LPVOID, DWORD, LPDWORD) { return (HANDLE)1; }
#endif

// Linux: all exported test-harness symbols are provided by testthread2.cpp.
// This TU remains as a no-op placeholder so tier0.so can include it 1:1 without
// duplicate-symbol link errors, while still exercising _T/strncpy_s/CreateThread shims.
void __TestThread_CPP_LinuxPlaceholder()
{
	const tchar* s = _T("testthread.cpp placeholder");
	UNREFERENCED_PARAMETER(s);
	char buf[32];
	strncpy_s(buf, sizeof(buf), "placeholder", _TRUNCATE);
	HANDLE h = CreateThread(NULL, 0, NULL, NULL, 0, NULL);
	(void)h;
	(void)buf;
}

#else // WIN32

#ifdef WIN32
#ifndef ARRAYSIZE
#define ARRAYSIZE(p) (sizeof(p)/sizeof((p)[0]))
#endif
#include "winlite.h"
#endif

// Windows: original harness logic is consolidated in testthread2.cpp; this
// companion TU provides a thin wrapper to keep the 1:1 file set link-clean.
void __TestThread_CPP_WinPlaceholder()
{
	// Intentionally empty -- see testthread2.cpp for full harness.
}

#endif // _LINUX
