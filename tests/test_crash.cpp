// tier0 -- crash-log integration test.
// Loads tier0.dll (installs Tier0_VectoredHandler), then raises an unhandled
// exception so the handler must dump it into <dir-of-tier0.dll>\crash.log.
//   test_crash.exe heap   -> 0xC0000374  (HEAP_CORRUPTION)
//   test_crash.exe brk    -> 0x80000003  (BREAKPOINT)
// The process is expected to die with the exception code; the caller then
// greps crash.log for the Reason= line.
//
// Build (x86 MSVC prompt):
//   cl /O1 /GS- /nologo tests\test_crash.cpp /Fetests\test_crash.exe /link /SUBSYSTEM:CONSOLE

#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	HMODULE h = LoadLibraryA("tier0.dll");
	if (!h)
	{
		printf("FAIL: cannot load tier0.dll\n");
		return 2;
	}

	DWORD code = (argc > 1 && argv[1][0] == 'b') ? 0x80000003 : 0xC0000374;
	RaiseException(code, 0, 0, NULL);

	// Unhandled exceptions terminate the process -- reaching here means the
	// exception was not dispatched (e.g. a debugger swallowed it).
	printf("UNEXPECTED: exception 0x%08X was handled, process survived\n", code);
	return 1;
}