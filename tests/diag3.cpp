// SEH-instrumented probe of EnterScope.
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

int HasFault( void )
{
	return 1;
}

int main()
{
	HANDLE hf = CreateFileA( "trace3.log", FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	char tmp[160];

	HMODULE h = LoadLibraryA( "tier0.dll" );
	if ( !h ) { Log( hf, "LOAD FAIL" ); return 1; }

	void *prof = ( void * )GetProcAddress( h, "g_VProfCurrentProfile" );
	g_this = prof;

	// Run 1: EnterScope with profiling DISABLED (should no-op).
	g_fp = GetProcAddress( h, "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
	g_s1 = "Root_Frame"; g_i1 = 0; g_s2 = "Frame"; g_i2 = 0;
	__try
	{
		Call4();
		Log( hf, "R1 EnterScope(disabled) OK" );
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		wsprintfA( tmp, "R1 FAULT code=%08X", ( unsigned )GetExceptionCode() );
		Log( hf, tmp );
	}

	// Start.
	g_fp = GetProcAddress( h, "?Start@CVProfile@@QAEXXZ" );
	Call0();
	Log( hf, "Start OK" );

	// Run 2: EnterScope with profiling enabled.
	__try
	{
		g_fp = GetProcAddress( h, "?EnterScope@CVProfile@@QAEXPBDH0_N@Z" );
		g_s1 = "Root_Frame"; g_i1 = 0; g_s2 = "Frame"; g_i2 = 0;
		Call4();
		Log( hf, "R2 EnterScope(enabled) OK" );
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		wsprintfA( tmp, "R2 FAULT code=%08X", ( unsigned )GetExceptionCode() );
		Log( hf, tmp );
	}

	// Run 3: get child name.
	__try
	{
		g_fp = GetProcAddress( h, "?GetRoot@CVProfile@@QAEPAVCVProfNode@@XZ" );
		g_this = prof;
		void *root = CallR0();
		wsprintfA( tmp, "R3 root=%p", root );
		Log( hf, tmp );

		g_fp = GetProcAddress( h, "?GetChild@CVProfNode@@QAEPAV1@XZ" );
		g_this = root;
		void *child = CallR0();
		g_this = child;
		g_fp = GetProcAddress( h, "?GetName@CVProfNode@@QAEPBDXZ" );
		const char *nm = ( const char * )CallR0();
		wsprintfA( tmp, "R3 child=%p name=%s", child, nm ? nm : "(null)" );
		Log( hf, tmp );
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		wsprintfA( tmp, "R3 FAULT code=%08X", ( unsigned )GetExceptionCode() );
		Log( hf, tmp );
	}

	Log( hf, "DONE" );
	CloseHandle( hf );
	FreeLibrary( h );
	return 0;
}
