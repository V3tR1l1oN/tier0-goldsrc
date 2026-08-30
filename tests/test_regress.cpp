// Regression harness for the defects fixed in tier0-goldsrc 1.4.0.
//
// Every check below is a use-after-free, double-free, state-corrupting or leak
// bug that the shipped source had. The harness drives the rebuilt tier0.dll
// through GetProcAddress + naked __thiscall thunks (same technique as the
// existing tests/ harnesses) so it exercises the real export table.
//
//   T1  CVProfile::Term() used to leave m_pCurNodeCache pointing at freed
//       nodes, so the next EnterScope() was a use-after-free.
//   T2  An unmatched CVProfile::Stop() drove m_Enabled below zero, which left
//       the profiler silently switched off after the next Start().
//   T3  ~CVProfNode() freed only the first child, so every sibling below the
//       top level leaked (measured here as process private bytes).
//   T4  CValidator::operator= (ordinal 34) shallow-copied the CValObject list,
//       so two validators owned one list and the second destructor to run
//       freed every node a second time.
//
// Runs from anywhere: the .exe sits next to the tier0.dll it tests.

#include <windows.h>
#include <stdio.h>
#include <psapi.h>

#pragma comment( lib, "psapi.lib" )

static HMODULE g_hDll;
static FARPROC s_fn;
static int	   g_fail = 0;

#define CHECK( cond, what )									\
	do {													\
		if ( !( cond ) )									\
		{													\
			printf( "FAIL: %s\n", what );					\
			++g_fail;										\
		}													\
		else												\
		{													\
			printf( "  ok: %s\n", what );					\
		}													\
	} while ( 0 )

//-----------------------------------------------------------------------------
// __thiscall thunks
//-----------------------------------------------------------------------------

__declspec( naked ) static void Call0( void *self )
{
	__asm {
		mov ecx, [esp+4]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec( naked ) static void Call1( void *self, int a )
{
	__asm {
		mov eax, [esp+8]
		push eax
		mov ecx, [esp+8]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec( naked ) static void Call1p( void *self, void *a )
{
	__asm {
		mov eax, [esp+8]
		push eax
		mov ecx, [esp+8]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec( naked ) static void Call3p( void *self, const char *a, void *b, const char *c )
{
	__asm {
		mov eax, [esp+16]
		push eax
		mov eax, [esp+16]
		push eax
		mov eax, [esp+16]
		push eax
		mov ecx, [esp+16]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec( naked ) static void Call4( void *self, const char *n, int d, const char *g, int b )
{
	__asm {
		mov eax, [esp+20]
		push eax
		mov eax, [esp+20]
		push eax
		mov eax, [esp+20]
		push eax
		mov eax, [esp+20]
		push eax
		mov ecx, [esp+20]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec( naked ) static void *CallR0( void *self )
{
	__asm {
		mov ecx, [esp+4]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec( naked ) static void *CallR1p( void *self, void *a )
{
	__asm {
		mov eax, [esp+8]
		push eax
		mov ecx, [esp+8]
		mov eax, s_fn
		call eax
		ret
	}
}

//-----------------------------------------------------------------------------

static FARPROC Fn( const char *name )
{
	FARPROC p = GetProcAddress( g_hDll, name );
	if ( !p ) { printf( "FAIL: missing export %s\n", name ); ++g_fail; }
	return p;
}

//-----------------------------------------------------------------------------
// CVProfile helpers
//-----------------------------------------------------------------------------

static void  *g_prof;

static FARPROC g_fnStart, g_fnStop, g_fnTerm, g_fnIsEnabled, g_fnEnter, g_fnExit,
			   g_fnGetRoot, g_fnGetChild, g_fnGetSibling, g_fnGetName;

static void ProfStart()							{ s_fn = g_fnStart;	  Call0( g_prof ); }
static void ProfStop()							{ s_fn = g_fnStop;	  Call0( g_prof ); }
static void ProfTerm()							{ s_fn = g_fnTerm;	  Call0( g_prof ); }
static void ProfEnter( const char *n, int d, const char *g ) { s_fn = g_fnEnter; Call4( g_prof, n, d, g, 0 ); }
static void ProfExit()							{ s_fn = g_fnExit;	  Call0( g_prof ); }
// bool comes back in AL only -- the upper bits of EAX are garbage, so mask.
static bool ProfIsEnabled()
{
	s_fn = g_fnIsEnabled;
	return ( ( uintptr_t )CallR0( g_prof ) & 0xFF ) != 0;
}

static void *ProfRootChild()
{
	s_fn = g_fnGetRoot;
	void *root = CallR0( g_prof );
	if ( !root ) return NULL;
	s_fn = g_fnGetChild;
	return CallR0( root );
}

static const char *NodeName( void *node )
{
	s_fn = g_fnGetName;
	return ( const char * )CallR0( node );
}

static void *NodeChild( void *node )	{ s_fn = g_fnGetChild;	 return CallR0( node ); }
static void *NodeSibling( void *node )	{ s_fn = g_fnGetSibling; return CallR0( node ); }

static int CountSiblings( void *node )
{
	int n = 0;
	for ( void *p = node; p; p = NodeSibling( p ) )
		++n;
	return n;
}

//-----------------------------------------------------------------------------
// T1: Term() must not leave m_pCurNodeCache dangling (use-after-free).
//-----------------------------------------------------------------------------

static void Test_VProfTermThenReenter()
{
	printf( "T1 vprof: Term() then re-enter\n" );

	ProfStart();
	ProfEnter( "Alpha", 0, "TIME" );
	ProfEnter( "Beta", 0, "TIME" );
	ProfExit();
	ProfExit();
	ProfTerm();

	// Before the fix m_pCurNodeCache still pointed at the freed "Alpha" node,
	// so the next EnterScope() searched freed memory.
	ProfStart();
	ProfEnter( "Alpha", 0, "TIME" );
	ProfExit();

	void *child = ProfRootChild();
	CHECK( child != NULL, "tree rebuilt after Term()" );

	if ( child )
	{
		const char *nm = NodeName( child );
		CHECK( nm && strcmp( nm, "Alpha" ) == 0, "rebuilt first child is \"Alpha\"" );
		CHECK( NodeChild( child ) == NULL, "rebuilt node has no stale children" );
	}

	ProfStop();
	ProfTerm();
}

//-----------------------------------------------------------------------------
// T2: an unmatched Stop() must not drive m_Enabled negative.
//-----------------------------------------------------------------------------

static void Test_VProfUnmatchedStop()
{
	printf( "T2 vprof: unmatched Stop()\n" );

	ProfTerm();
	ProfStop();							// no matching Start(): must be a no-op
	CHECK( !ProfIsEnabled(), "Stop() with profiling off stays off" );

	ProfStart();
	// Before the fix the stray Stop() had already taken m_Enabled to -1, so
	// this Start() only reached 0 and the profiler stayed silently disabled.
	CHECK( ProfIsEnabled(), "Start() after a stray Stop() really enables profiling" );

	ProfEnter( "Gamma", 0, "TIME" );
	ProfExit();
	ProfStop();
	CHECK( !ProfIsEnabled(), "balanced Stop() turns profiling off" );

	// A second, unbalanced Stop() must not push the recursion count negative.
	ProfStop();
	ProfStart();
	CHECK( ProfIsEnabled(), "re-Start() after an extra Stop() works" );
	ProfEnter( "Delta", 0, "TIME" );
	ProfExit();
	ProfStop();
	ProfTerm();
}

//-----------------------------------------------------------------------------
// T3: ~CVProfNode must free every sibling, not just the first child.
//     Root -> L1 -> { C1 C2 C3 C4 }: the old destructor lost C2..C4.
//-----------------------------------------------------------------------------

static void BuildWideTree()
{
	ProfStart();
	ProfEnter( "L1", 0, "TIME" );
	ProfEnter( "C1", 0, "TIME" ); ProfExit();
	ProfEnter( "C2", 0, "TIME" ); ProfExit();
	ProfEnter( "C3", 0, "TIME" ); ProfExit();
	ProfEnter( "C4", 0, "TIME" ); ProfExit();
	ProfExit();
	ProfStop();
}

static SIZE_T PrivateBytes()
{
	PROCESS_MEMORY_COUNTERS_EX pmc;
	pmc.cb = sizeof( pmc );
	if ( !GetProcessMemoryInfo( GetCurrentProcess(), ( PROCESS_MEMORY_COUNTERS * )&pmc, sizeof( pmc ) ) )
		return 0;
	return pmc.PrivateUsage;
}

static void Test_VProfSiblingLeak()
{
	printf( "T3 vprof: sibling subtree leak across Term()\n" );

	// Sanity: the tree shape this test relies on.
	BuildWideTree();
	void *l1 = ProfRootChild();
	CHECK( l1 != NULL, "tree has an L1 node" );
	if ( l1 )
		CHECK( CountSiblings( NodeChild( l1 ) ) == 4, "L1 has 4 children" );
	ProfTerm();

	for ( int i = 0; i < 200; ++i )		// warm-up: settle the heap
	{
		BuildWideTree();
		ProfTerm();
	}

	const SIZE_T before = PrivateBytes();

	for ( int i = 0; i < 5000; ++i )
	{
		BuildWideTree();
		ProfTerm();
	}

	const SIZE_T after = PrivateBytes();
	const long long grew = ( long long )after - ( long long )before;

	printf( "     private bytes: %lld -> %lld (delta %+lld over 5000 cycles)\n",
			( long long )before, ( long long )after, grew );

	// 3 leaked nodes per cycle at ~200 bytes would be ~3 MB here. Anything
	// under 256 KB is noise from the CRT heap growing normally.
	CHECK( grew < 262144, "no per-cycle node leak (growth < 256 KB)" );

	CHECK( ProfRootChild() == NULL, "Term() clears the root child pointer" );
}

//-----------------------------------------------------------------------------
// T4: CValidator::operator= must not alias the CValObject list.
//-----------------------------------------------------------------------------

static void Test_ValidatorAssign()
{
	printf( "T4 validator: operator= deep-copies the object list\n" );

	FARPROC fnCtor	= Fn( "??0CValidator@@QAE@H@Z" );
	FARPROC fnDtor	= Fn( "??1CValidator@@QAE@XZ" );
	FARPROC fnAssign = Fn( "??4CValidator@@QAEAAV0@ABV0@@Z" );
	FARPROC fnPush	= Fn( "?Push@CValidator@@QAEXPBDPAX0@Z" );
	FARPROC fnFirst	= Fn( "?PValObjectFirst@CValidator@@QAEPAVCValObject@@XZ" );
	FARPROC fnFind	= Fn( "?FindObject@CValidator@@QAEPAVCValObject@@PAX@Z" );

	if ( !fnCtor || !fnDtor || !fnAssign || !fnPush || !fnFirst || !fnFind )
		return;

	// 0x830 is the verified sizeof(CValidator); over-allocate generously so a
	// layout surprise can never turn into heap corruption inside the test.
	void *v1 = calloc( 1, 0x2000 );
	void *v2 = calloc( 1, 0x2000 );
	if ( !v1 || !v2 ) { printf( "FAIL: out of memory\n" ); ++g_fail; return; }

	s_fn = fnCtor; Call1( v1, 0 );
	s_fn = fnCtor; Call1( v2, 0 );

	s_fn = fnPush; Call3p( v1, "TypeA", ( void * )0x1111, "objA" );
	s_fn = fnPush; Call3p( v1, "TypeB", ( void * )0x2222, "objB" );

	s_fn = fnFirst;
	void *head1 = CallR0( v1 );
	CHECK( head1 != NULL, "source validator has a record list" );

	// v2 = v1
	s_fn = fnAssign;
	Call1p( v2, v1 );

	s_fn = fnFirst; void *head1After = CallR0( v1 );
	s_fn = fnFirst; void *head2	  = CallR0( v2 );

	CHECK( head1After == head1, "assignment leaves the source list intact" );
	CHECK( head2 != NULL, "copy has a record list" );
	CHECK( head2 != head1, "copy does NOT alias the source list (no double free)" );

	s_fn = fnFind; void *f1 = CallR1p( v2, ( void * )0x1111 );
	s_fn = fnFind; void *f2 = CallR1p( v2, ( void * )0x2222 );
	CHECK( f1 != NULL, "copy resolves objA" );
	CHECK( f2 != NULL, "copy resolves objB" );
	CHECK( f1 != f2, "copy holds two distinct records" );

	// Destroying both must be clean. With the old shallow copy the second
	// destructor freed the same nodes again.
	s_fn = fnDtor; Call0( v1 );
	s_fn = fnFirst;
	CHECK( CallR0( v1 ) == NULL, "source list emptied by destructor" );

	s_fn = fnFirst;
	void *head2AfterDtor1 = CallR0( v2 );
	CHECK( head2AfterDtor1 != NULL, "copy survives the source destructor" );

	s_fn = fnDtor; Call0( v2 );
	s_fn = fnFirst;
	CHECK( CallR0( v2 ) == NULL, "copy list emptied by its own destructor (no double free)" );

	free( v1 );
	free( v2 );
}

//-----------------------------------------------------------------------------

int main()
{
	setvbuf( stdout, NULL, _IONBF, 0 );	// unbuffered: a crash must not swallow progress

	printf( "regression harness for tier0-goldsrc 1.4.0 fixes\n" );
	printf( "loading tier0.dll ...\n" );

	g_hDll = LoadLibraryA( "tier0.dll" );
	if ( !g_hDll )
	{
		printf( "FAIL: cannot load tier0.dll (run from the repo root)\n" );
		return 1;
	}

	g_prof = GetProcAddress( g_hDll, "g_VProfCurrentProfile" );
	if ( !g_prof ) { printf( "FAIL: no g_VProfCurrentProfile\n" ); return 1; }

	g_fnStart	  = Fn( "?Start@CVProfile@@QAEXXZ" );
	g_fnStop	  = Fn( "?Stop@CVProfile@@QAEXXZ" );
	g_fnTerm	  = Fn( "?Term@CVProfile@@QAEXXZ" );
	g_fnIsEnabled = Fn( "?IsEnabled@CVProfile@@QBE_NXZ" );
	g_fnEnter	  = Fn( "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	g_fnExit	  = Fn( "?ExitScope@CVProfile@@QAEXXZ" );
	g_fnGetRoot	  = Fn( "?GetRoot@CVProfile@@QAEPAVCVProfNode@@XZ" );
	g_fnGetChild  = Fn( "?GetChild@CVProfNode@@QAEPAV1@XZ" );
	g_fnGetSibling = Fn( "?GetSibling@CVProfNode@@QAEPAV1@XZ" );
	g_fnGetName	  = Fn( "?GetName@CVProfNode@@QAEPBDXZ" );

	printf( "exports resolved, running checks\n" );

	Test_VProfTermThenReenter();
	printf( "T1 done\n" );
	Test_VProfUnmatchedStop();
	printf( "T2 done\n" );
	Test_VProfSiblingLeak();
	printf( "T3 done\n" );
	Test_ValidatorAssign();
	printf( "T4 done\n" );
	printf( "\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "--- all ok ---",
			g_fail, g_fail == 1 ? "" : "s" );

	FreeLibrary( g_hDll );
	return g_fail ? 1 : 0;
}
