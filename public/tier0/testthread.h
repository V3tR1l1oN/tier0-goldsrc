// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (MIT).
//
// Purpose: Test harness multithreaded tools.
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef TESTTHREAD_H
#define TESTTHREAD_H

#include "platform.h"

typedef void ( *TestFunc )( void *pvArgs );

PLATFORM_INTERFACE void Test_RunTest( TestFunc func, void *pvArg );
PLATFORM_INTERFACE bool Test_IsActive();
PLATFORM_INTERFACE bool Test_HasFailed();
PLATFORM_INTERFACE bool Test_HasFinished();
PLATFORM_INTERFACE void Test_SetFailed();
PLATFORM_INTERFACE void Test_RunFrame();
PLATFORM_INTERFACE void Test_TerminateThread();
PLATFORM_INTERFACE void TestThread_Yield();

#endif // TESTTHREAD_H
