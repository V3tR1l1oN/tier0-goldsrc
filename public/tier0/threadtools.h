// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
//
// Purpose: Threading primitives for tier0.
//
//=============================================================================//

#ifndef THREADTOOLS_H
#define THREADTOOLS_H

#include "platform.h"
#include "dbg.h"

#ifdef WIN32
#ifndef ARRAYSIZE
#define ARRAYSIZE( p ) ( sizeof( p ) / sizeof( (p)[0] ) )
#endif
#include "winlite.h"
#endif

#ifndef TT_INFINITE
#define TT_INFINITE		0xFFFFFFFFUL
#endif

typedef void *ThreadId_t;
typedef unsigned (__stdcall *ThreadWorkerFunction_t)( void *pvParam );

class PLATFORM_CLASS CThread;
class PLATFORM_CLASS CWorkerThread;

// C API exports
PLATFORM_INTERFACE void ThreadSleep( unsigned duration );
PLATFORM_INTERFACE uint ThreadGetCurrentId();
PLATFORM_INTERFACE uint ThreadGetCurrentProcessId();
PLATFORM_INTERFACE int ThreadGetPriority( ThreadHandle_t hThread );
PLATFORM_INTERFACE bool ThreadInMainThread();
PLATFORM_INTERFACE void DeclareCurrentThreadIsMainThread();

PLATFORM_INTERFACE long ThreadInterlockedIncrement( long volatile *pDest );
PLATFORM_INTERFACE long ThreadInterlockedDecrement( long volatile *pDest );
PLATFORM_INTERFACE long ThreadInterlockedExchange( long volatile *pDest, long value );
PLATFORM_INTERFACE long ThreadInterlockedExchangeAdd( long volatile *pDest, long value );
PLATFORM_INTERFACE long ThreadInterlockedCompareExchange( long volatile *pDest, long value, long comperand );
PLATFORM_INTERFACE void *ThreadInterlockedExchangePointer( void * volatile *pDest, void *value );
PLATFORM_INTERFACE void *ThreadInterlockedCompareExchangePointer( void * volatile *pDest, void *value, void *comperand );

PLATFORM_INTERFACE int64 ThreadInterlockedIncrement64( int64 volatile *pDest );
PLATFORM_INTERFACE int64 ThreadInterlockedDecrement64( int64 volatile *pDest );
PLATFORM_INTERFACE int64 ThreadInterlockedCompareExchange64( int64 volatile *pDest, int64 value, int64 comperand );
PLATFORM_INTERFACE int64 ThreadInterlockedExchange64( int64 volatile *pDest, int64 value );
PLATFORM_INTERFACE int64 ThreadInterlockedExchangeAdd64( int64 volatile *pDest, int64 value );

PLATFORM_INTERFACE int ThreadIsProcessActive( int procID );
PLATFORM_INTERFACE int WaitForMultipleEvents( int nEvents, const void **pHandles, bool bWaitAll, unsigned timeout );
PLATFORM_INTERFACE ThreadHandle_t CreateSimpleThread( ThreadWorkerFunction_t pfnThread, void *pParam,
													  ThreadId_t *pOutThreadId = NULL, unsigned nBytesStack = 0 );

void Plat_RegisterThread( unsigned nIndex = 0 );
unsigned Plat_GetCurrentThreadID();
void Plat_SetThreadName( const char *pszName );
void Plat_RegisterPrimaryThread();

// TLS
class PLATFORM_CLASS CThreadLocalBase
{
public:
	CThreadLocalBase();
	~CThreadLocalBase();

	void *Get() const;
	void Set( void *value );

	CThreadLocalBase& operator=( const CThreadLocalBase& other );

public:
	uint m_index;
};

template <class T> class CThreadLocal : public CThreadLocalBase
{
public:
	operator T *()					{ return ( T * )Get(); }
	CThreadLocal<T>& operator=( T i ){ Set( ( void * )i ); return *this; }
};

template <class T> class CThreadLocalPtr : private CThreadLocalBase
{
public:
	CThreadLocalPtr() {}
	CThreadLocalPtr( const CThreadLocalPtr<T>& other )	{ Set( other.Get() ); }

	CThreadLocalPtr<T>& operator=( void *pv )	{ Set( pv ); return *this; }
	CThreadLocalPtr<T>& operator=( T *pv )		{ Set( pv ); return *this; }
	CThreadLocalPtr<T>& operator=( const CThreadLocalPtr<T>& other ){ Set( other.Get() ); return *this; }

	operator const void *()	const	{ return Get(); }
	operator void *()				{ return Get(); }
	operator const T *() const		{ return ( T * )Get(); }
	operator T *()					{ return ( T * )Get(); }
	T*	operator->()				{ return ( T * )Get(); }
	T&	operator*()					{ return *( ( T * )Get() ); }

	bool operator!=( void *pv ) const { return Get() != pv; }
	bool operator==( void *pv ) const { return Get() == pv; }
};

// CThreadSyncObject
class PLATFORM_CLASS CThreadSyncObject
{
public:
	~CThreadSyncObject();

	bool operator!() const;
	operator void *();
	void *Handle();

	bool Wait( unsigned timeout = TT_INFINITE );

protected:
	CThreadSyncObject();
	void AssertUseable();

protected:
	void *volatile m_hSyncObject;
	bool m_bInitalized;

private:
	CThreadSyncObject( const CThreadSyncObject& );
	CThreadSyncObject& operator=( const CThreadSyncObject& );
};

// CThreadFullMutex
class PLATFORM_CLASS CThreadFullMutex : public CThreadSyncObject
{
public:
	CThreadFullMutex( bool bEstablishInitialOwnership = false, const char *pszName = NULL );
	~CThreadFullMutex();

	bool Release();

	bool TryLock( unsigned timeout = 0 )			{ return Wait( timeout ); }
	void Lock( unsigned timeout );
	void Lock();
	void Unlock();

	bool AssertOwnedByCurrentThread();
	void SetTrace( bool );

private:
	CThreadFullMutex( const CThreadFullMutex& );
	CThreadFullMutex& operator=( const CThreadFullMutex& );
};

// CThreadEvent
class PLATFORM_CLASS CThreadEvent : public CThreadSyncObject
{
public:
	CThreadEvent( bool fManualReset = false );
	CThreadEvent( void *pEvent, bool fManualReset = false );

	bool Set();
	bool Reset();
	bool Pulse();
	bool Check();
	bool Wait( unsigned timeout = TT_INFINITE );

	void *Handle() { return m_hSyncObject; }

private:
	CThreadEvent( const CThreadEvent& );
	CThreadEvent& operator=( const CThreadEvent& );
};

// CThreadSemaphore
class PLATFORM_CLASS CThreadSemaphore : public CThreadSyncObject
{
public:
	CThreadSemaphore( long initialValue = 0, long maxValue = LONG_MAX );
	~CThreadSemaphore();

	bool Release( long releaseCount = 1, long *pPreviousCount = NULL );

	bool TryLock( unsigned timeout = 0 )		{ return Wait( timeout ); }
	void Lock( unsigned timeout = TT_INFINITE )	{ Wait( timeout ); }
	void Unlock()								{ Release( 1 ); }

private:
	CThreadSemaphore( const CThreadSemaphore& );
	CThreadSemaphore& operator=( const CThreadSemaphore& );
};

// CThreadMutex
class PLATFORM_CLASS CThreadMutex
{
public:
	CThreadMutex();
	~CThreadMutex();

	void Lock();
	void Unlock();
	void Lock() const;
	void Unlock() const;
	void Lock() const volatile;
	void Unlock() const volatile;

	bool TryLock();

	bool AssertOwnedByCurrentThread();
	void SetTrace( bool );

#if defined( _WIN32 )
	operator CRITICAL_SECTION *() { return reinterpret_cast<CRITICAL_SECTION *>( &m_CriticalSection ); }
#endif

protected:
	CRITICAL_SECTION m_CriticalSection;
};

// CThreadFastMutex
class PLATFORM_CLASS CThreadFastMutex
{
public:
	CThreadFastMutex();
	CThreadFastMutex( const CThreadFastMutex& );
	CThreadFastMutex( CThreadFastMutex &&other );
	CThreadFastMutex& operator=( const CThreadFastMutex& );
	CThreadFastMutex& operator=( CThreadFastMutex &&other );

	void Lock( unsigned timeout = 0 ) const volatile;
	void Lock( unsigned timeout = 0 ) volatile;
	void Unlock() const volatile;
	void Unlock() volatile;

	bool TryLock( const uint32 threadId = 0 ) const volatile;
	bool TryLock() volatile;
	bool TryLock() const volatile;

	bool AssertOwnedByCurrentThread();
	void SetTrace( bool );

	uint32 GetOwnerId() const { return m_nOwnership; }

private:
	volatile uint32 m_nOwnership;
	volatile int m_nCount;
};

// CThread
// CRITICAL: vtable layout must match original (old MSVC) exactly!
// VERIFIED against original tier0.dll vtable and real ThreadProc disassembly:
//   [0]=dtor [1]=Start [2]=Init [3]=Run [4]=OnExit [5]=WaitForCreateComplete [6]=GetThreadProc [7]=fn [8]=fn
// Original CThread::ThreadProc calls Init via [eax+8], Run via [eax+0Ch], OnExit via [eax+10h].
// New MSVC creates 2 slots for virtual dtor ? we avoid this by making dtor non-virtual
// and adding stub functions to match positions exactly.
class PLATFORM_CLASS CThread
{
public:
	typedef unsigned ( __stdcall *ThreadProc_t )( void *pv );

	CThread();
	// Non-virtual dtor: exported via .def alias (UAE->QAE).
	// Kept non-inline so the alias "??1CThread@@UAE@XZ"="??1CThread@@QAE@XZ"
	// resolves to a plain external symbol instead of a strippable inline COMDAT.
	~CThread();

	// vtable[0]: stub matching original's virtual destructor slot
	virtual void _DtorStub() { }

	// vtable[1]: Start (matches original position)
	virtual bool Start( unsigned nBytesStack = 0 );

	const char *GetName();
	void SetName( const char *pszName );

	bool IsAlive();

	void *GetThreadHandle();
	uint GetThreadId();

	int GetResult();
	void Stop( int exitCode = 0 );

	unsigned Suspend();
	unsigned Resume();
	bool Terminate( int result );

	int GetPriority() const;
	bool SetPriority( int priority );

	bool Join( unsigned timeout = TT_INFINITE );

	static void Sleep( unsigned duration );
	static void (Yield)();
	static CThread *GetCurrentCThread();

private:
	static unsigned __stdcall ThreadProc( void *pv );

protected:
	// vtable[2]: Init (matches original position)
	virtual bool Init();
	// vtable[3]: Run (matches original position, original calls [eax+0Ch])
	virtual int Run();
	// vtable[4]: OnExit (matches original position, original calls [eax+10h])
	virtual void OnExit();
	// vtable[5]: WaitForCreateComplete (matches original position)
	virtual bool WaitForCreateComplete( CThreadEvent *pEvent );
	// vtable[6]: GetThreadProc (matches original position)
	virtual ThreadProc_t GetThreadProc();
	// vtable[7],[8]: original also carries two trailing virtuals; keep stub slots so
	// the derived layout (CWorkerThread etc.) and our vtable length match.
	virtual void _Slot7Stub() { }
	virtual void _Slot8Stub() { }

protected:
	HANDLE	m_hThread;
	uint	m_threadId;
	int		m_result;
	char	m_szName[32];
};

// CWorkerThread
class PLATFORM_CLASS CWorkerThread : public CThread
{
public:
	CWorkerThread();
	virtual ~CWorkerThread() {}

	int BoostPriority( int newPriority );
	int BoostPriority();

	int CallMaster( unsigned, unsigned );
	int CallWorker( unsigned, unsigned callParam = 0, bool fWaitForReply = false );

	void *GetCallHandle();
	unsigned GetCallParam() const;

	bool PeekCall( unsigned *pParam );
	bool WaitForCall( unsigned timeout, unsigned *pResult );
	bool WaitForCall( unsigned *pResult );
	void Reply( unsigned );

	virtual int Run();

protected:
	int Call( unsigned, unsigned, bool, unsigned (__stdcall *)( unsigned, void * const *, int, unsigned ) );
	virtual bool WaitForReply( unsigned timeout = TT_INFINITE );

protected:
	enum
	{
		WT_MAXCALLS = 1,
		WT_PENDING = 0x1,
		WT_SCHEDULED = 0x2
	};

	struct CallQueue_t
	{
		unsigned m_flags;
		unsigned m_param;
	};

	CallQueue_t		m_Call;
	CThreadEvent	m_EventSend;
	CThreadEvent	m_EventComplete;
};

#endif // THREADTOOLS_H
