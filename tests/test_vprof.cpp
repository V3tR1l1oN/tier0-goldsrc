// Standalone functional test for the reimplemented CVProfile named-node
// EnterScope path. Loads the built tier0.dll directly.
#include <windows.h>
#include <stdio.h>

typedef void *TP_FN;
static FARPROC s_fn;

// Naked __thiscall emulators. g_VProfCurrentProfile is an exported data item,
// so GetProcAddress returns the address of the object itself.

struct CVProfNode;
struct CVProfile;

__declspec(naked) static void Call0( void *self )
{
	__asm {
		mov ecx, [esp+4]
		mov eax, s_fn
		call eax
		ret
	}
}

__declspec(naked) static void Call1( void *self, int a )
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

__declspec(naked) static void Call4( void *self, const char *n, int d, const char *g, int b )
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

__declspec(naked) static void *CallR0( void *self )
{
	__asm {
		mov ecx, [esp+4]
		mov eax, s_fn
		call eax
		ret
	}
}

static const char *GetName( CVProfNode *self )
{
	s_fn = GetProcAddress( LoadLibraryA( "tier0.dll" ), "?GetName@CVProfNode@@QAEPBDXZ" );
	return ( const char * )CallR0( self );
}

static void PrintTree( CVProfNode *pNode, int indent )
{
	for ( int i = 0; i < indent; ++i ) printf( "  " );

	if ( !pNode )
		return;

	s_fn = GetProcAddress( LoadLibraryA( "tier0.dll" ), "?GetName@CVProfNode@@QAEPBDXZ" );
	const char *name = ( const char * )CallR0( pNode );

	s_fn = GetProcAddress( LoadLibraryA( "tier0.dll" ), "?GetBudgetGroupID@CVProfNode@@QAEHXZ" );
	int bid = ( int )( intptr_t )CallR0( pNode );

	s_fn = GetProcAddress( LoadLibraryA( "tier0.dll" ), "?GetCurCalls@CVProfNode@@QAEHXZ" );
	int calls = ( int )( intptr_t )CallR0( pNode );

	printf( "\"%s\" (budget=%d calls=%d)\n", name ? name : "(null)", bid, calls );

	s_fn = GetProcAddress( LoadLibraryA( "tier0.dll" ), "?GetChild@CVProfNode@@QAEPAV1@XZ" );
	PrintTree( ( CVProfNode * )CallR0( pNode ), indent + 1 );

	s_fn = GetProcAddress( LoadLibraryA( "tier0.dll" ), "?GetSibling@CVProfNode@@QAEPAV1@XZ" );
	CVProfNode *sib = ( CVProfNode * )CallR0( pNode );
	PrintTree( sib, indent );
}

int main()
{
	puts( "start" );
	fflush( stdout );

	HMODULE h = LoadLibraryA( "tier0.dll" );
	if ( !h ) { printf( "load tier0.dll FAILED\n" ); return 1; }

	CVProfile *prof = ( CVProfile * )GetProcAddress( h, "g_VProfCurrentProfile" );
	if ( !prof ) { printf( "no g_VProfCurrentProfile\n" ); return 1; }

	printf( "prof object @ %p\n", prof );

	// Start profiling.
	s_fn = GetProcAddress( h, "?Start@CVProfile@@QAEXXZ" );
	Call0( prof );

	// Build a small named tree: Frame -> [Update, Phys], [Render]
	s_fn = GetProcAddress( h, "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	Call4( prof, "Root_Frame", 0, "Frame", 0 );
	Call4( prof, "Update", 1, "TIME", 0 );
	Call4( prof, "Physics", 2, "TIME", 0 );
	s_fn = GetProcAddress( h, "?ExitScope@CVProfile@@QAEXXZ" );
	Call0( prof );
	Call0( prof );

	s_fn = GetProcAddress( h, "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	Call4( prof, "Render", 1, "DRAW", 0 );
	s_fn = GetProcAddress( h, "?ExitScope@CVProfile@@QAEXXZ" );
	Call0( prof );

	// Second frame: Update should reuse the existing node, not recreate it.
	s_fn = GetProcAddress( h, "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	Call4( prof, "Update", 1, "TIME", 0 );
	s_fn = GetProcAddress( h, "?ExitScope@CVProfile@@QAEXXZ" );
	Call0( prof );

	s_fn = GetProcAddress( h, "?GetRoot@CVProfile@@QAEPAVCVProfNode@@XZ" );
	CVProfNode *root = ( CVProfNode * )CallR0( prof );

	printf( "--- node tree ---\n" );
	s_fn = GetProcAddress( h, "?GetChild@CVProfNode@@QAEPAV1@XZ" );
	PrintTree( ( CVProfNode * )CallR0( root ), 0 );

	// Budget group registry check.
	s_fn = GetProcAddress( h, "?GetNumBudgetGroups@CVProfile@@QAEHXZ" );
	int nGroups = ( int )( intptr_t )CallR0( prof );
	printf( "budget groups registered: %d\n", nGroups );

	s_fn = GetProcAddress( h, "?MarkFrame@CVProfile@@QAEXXZ" );
	Call0( prof );

	s_fn = GetProcAddress( h, "?Stop@CVProfile@@QAEXXZ" );
	Call0( prof );

	printf( "--- all ok ---\n" );
	FreeLibrary( h );
	return 0;
}