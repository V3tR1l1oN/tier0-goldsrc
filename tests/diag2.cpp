// Step-by-step pixel-equivalent of the full EnterScope test with direct file logging.
#include <windows.h>
#include <stdio.h>

static FARPROC g_fp = 0;
static void *g_this = 0;
static const char *g_s1 = 0;
static int g_i1 = 0;
static const char *g_s2 = 0;
static int g_i2 = 0;

__declspec(naked) static void Call0()
{
	__asm {
		mov ecx, g_this
		mov eax, g_fp
		call eax
		ret
	}
}

__declspec(naked) static void Call1()
{
	__asm {
		mov eax, g_i1
		push eax
		mov ecx, g_this
		mov eax, g_fp
		call eax
		add esp, 4
		ret
	}
}

__declspec(naked) static void Call4()
{
	__asm {
		mov eax, g_i2
		push eax
		mov eax, g_s2
		push eax
		mov eax, g_i1
		push eax
		mov eax, g_s1
		push eax
		mov ecx, g_this
		mov eax, g_fp
		call eax
		ret
	}
}

__declspec(naked) static void *CallR0()
{
	__asm {
		mov ecx, g_this
		mov eax, g_fp
		call eax
		ret
	}
}

static void Log( HANDLE hf, const char *msg )
{
	DWORD w = 0;
	WriteFile( hf, msg, ( DWORD )strlen( msg ), &w, NULL );
	WriteFile( hf, "\r\n", 2, &w, NULL );
}

#define STEP( msg ) Log( hf, msg )

int main()
{
	HANDLE hf = CreateFileA( "trace2.log", FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	char tmp[128];

	HMODULE h = LoadLibraryA( "tier0.dll" );
	if ( !h ) { STEP( "LOAD FAIL" ); return 1; }
	STEP( "loaded" );

	void *prof = ( void * )GetProcAddress( h, "g_VProfCurrentProfile" );
	STEP( "got prof" );

	#define FP( name ) GetProcAddress( h, name )

	g_this = prof;

	g_fp = FP( "?Start@CVProfile@@QAEXXZ" );
	if ( !g_fp ) { STEP( "no Start" ); return 1; }
	Call0();
	STEP( "Start ok" );

	g_fp = FP( "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	g_s1 = "Root_Frame"; g_i1 = 0; g_s2 = "Frame"; g_i2 = 0;
	Call4();
	STEP( "EnterScope Root_Frame ok" );

	g_s1 = "Update"; g_i1 = 1; g_s2 = "TIME"; g_i2 = 0;
	Call4();
	STEP( "EnterScope Update ok" );

	g_s1 = "Physics"; g_i1 = 2; g_s2 = "TIME"; g_i2 = 0;
	Call4();
	STEP( "EnterScope Physics ok" );

	g_fp = FP( "?ExitScope@CVProfile@@QAEXXZ" );
	Call0(); STEP( "ExitScope x1" );
	Call0(); STEP( "ExitScope x2" );

	g_fp = FP( "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	g_s1 = "Render"; g_i1 = 1; g_s2 = "DRAW"; g_i2 = 0;
	Call4();
	STEP( "EnterScope Render ok" );

	g_fp = FP( "?ExitScope@CVProfile@@QAEXXZ" );
	Call0(); STEP( "ExitScope x3" );

	g_fp = FP( "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	g_s1 = "Update"; g_i1 = 1; g_s2 = "TIME"; g_i2 = 0;
	Call4();
	STEP( "EnterScope Update (2nd) ok" );

	g_fp = FP( "?ExitScope@CVProfile@@QAEXXZ" );
	Call0();
	STEP( "ExitScope x4" );

	g_fp = FP( "?GetRoot@CVProfile@@QAEPAVCVProfNode@@XZ" );
	void *root = CallR0();
	wsprintfA( tmp, "GetRoot -> %p", root );
	STEP( tmp );

	g_fp = FP( "?GetChild@CVProfNode@@QAEPAV1@XZ" );
	g_this = root;
	void *child = CallR0();
	wsprintfA( tmp, "Root child -> %p", child );
	STEP( tmp );

	if ( child )
	{
		g_this = child;
		g_fp = FP( "?GetName@CVProfNode@@QAEPBDXZ" );
		const char *nm = ( const char * )CallR0();
		wsprintfA( tmp, "  name=%s", nm ? nm : "(null)" );
		STEP( tmp );

		g_fp = FP( "?GetBudgetGroupID@CVProfNode@@QAEHXZ" );
		int bid = ( int )( intptr_t )CallR0();
		wsprintfA( tmp, "  budget=%d", bid );
		STEP( tmp );

		g_fp = FP( "?GetCurCalls@CVProfNode@@QAEHXZ" );
		int calls = ( int )( intptr_t )CallR0();
		wsprintfA( tmp, "  calls=%d", calls );
		STEP( tmp );
	}

	g_this = prof;
	g_fp = FP( "?GetNumBudgetGroups@CVProfile@@QAEHXZ" );
	int ng = ( int )( intptr_t )CallR0();
	wsprintfA( tmp, "numBudgetGroups=%d", ng );
	STEP( tmp );

	g_fp = FP( "?MarkFrame@CVProfile@@QAEXXZ" );
	Call0();
	STEP( "MarkFrame ok" );

	g_fp = FP( "?Stop@CVProfile@@QAEXXZ" );
	Call0();
	STEP( "Stop ok" );

	STEP( "DONE" );
	CloseHandle( hf );
	FreeLibrary( h );
	return 0;
}