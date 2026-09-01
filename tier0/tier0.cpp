#include "platform.h"
#include "minidump.h"
#ifdef _WIN32
#include <tlhelp32.h>
#endif
#include <stdint.h>

#ifdef WIN32
#include "winlite.h"
#include "../public/tier1/interface.h"

// Defined in threadtools.cpp: pairs the one-time timeBeginPeriod(1) timer
// raise (PreciseSleep) with timeEndPeriod(1) so we do not leak a global
// system-wide timer-resolution side effect for the lifetime of the process.
void Tier0ShutdownHighResTimer();

// AddVectoredExceptionHandler() comes from the SDK header (errhandlingapi.h);
// re-declaring it here without dllimport produced C4273 "inconsistent dll
// linkage" and risked binding to the wrong symbol.

static HINSTANCE g_hInst = NULL;
static PVOID g_hVeh = nullptr;
static char g_szCrashLogPath[MAX_PATH];
static volatile LONG g_CrashLogInitState = 0;
static volatile LONG g_CrashInProgress = 0;

// Resolves the crash log path once: <directory-of-tier0.dll>\crash.log.
// Falls back to a relative "crash.log" if the module path can't be queried,
// so crash reporting works regardless of where the game is installed.
static const char *GetCrashLogPath()
{
	if( InterlockedCompareExchange( &g_CrashLogInitState, 1, 0 ) == 0 )
	{
		DWORD len = GetModuleFileNameA( g_hInst, g_szCrashLogPath, MAX_PATH );
		if( len > 0 && len < MAX_PATH )
		{
			char *slash = strrchr( g_szCrashLogPath, '\\' );
			if( slash )
			{
				*slash = '\0';
				const size_t dirLen = strlen( g_szCrashLogPath );
				const int n = _snprintf( g_szCrashLogPath + dirLen,
					MAX_PATH - ( int )dirLen, "\\crash.log" );
				if( n < 0 || dirLen + ( size_t )n >= MAX_PATH )
					strcpy( g_szCrashLogPath, "crash.log" );
			}
			else
			{
				// A module name without a directory is not a valid log path. Do
				// not accidentally open/overwrite the DLL itself.
				strcpy( g_szCrashLogPath, "crash.log" );
			}
		}
		else
		{
			strcpy( g_szCrashLogPath, "crash.log" );
		}

		InterlockedExchange( &g_CrashLogInitState, 2 );
		return g_szCrashLogPath;
	}

	if( InterlockedCompareExchange( &g_CrashLogInitState, 0, 0 ) == 2 )
		return g_szCrashLogPath;

	// Avoid waiting from an exception handler. A concurrent first-use call can
	// use the safe relative fallback until the primary path is published.
	return "crash.log";
}

// Keeps crash.log bounded: once it grows past 8 MB the old content is rotated
// to crash_prev.log (kept as the previous crash, overwritten by the next
// rotation). Long-running play sessions with many crashes no longer grind out a
// multi-hundred-MB log file.
static const unsigned long CRASH_LOG_ROTATE_BYTES = 8u << 20;

static void RotateCrashLogIfNeeded()
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if ( !GetFileAttributesExA( GetCrashLogPath(), GetFileExInfoStandard, &fad ) )
		return;                 // no log yet

	ULARGE_INTEGER size;
	size.LowPart = fad.nFileSizeLow;
	size.HighPart = fad.nFileSizeHigh;

	if ( size.QuadPart < CRASH_LOG_ROTATE_BYTES )
		return;

	const char *pLogPath = GetCrashLogPath();
	const size_t logLen = strlen( pLogPath );

	// Need room for path without ".log" plus "_prev.log" plus the terminator.
	if ( logLen > 4 && logLen + 6 < MAX_PATH )
	{
		char prevPath[MAX_PATH];
		memcpy( prevPath, pLogPath, logLen - 4 );
		memcpy( prevPath + logLen - 4, "_prev.log", sizeof( "_prev.log" ) );
		MoveFileExA( pLogPath, prevPath, MOVEFILE_REPLACE_EXISTING );
	}
}

// _snprintf signals truncation with a negative count; passing that straight to
// WriteFile as a DWORD would ask it to write ~4 GB out of a stack buffer.
static int ClampSnprintf(int n, int bufSize)
{
    if (n < 0 || n >= bufSize)
        return bufSize - 1;
    return n;
}

static void WriteLog(const char *text, int len)
{
    if (len <= 0)
        return;

    HANDLE hFile = CreateFileA(GetCrashLogPath(),
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(hFile, text, (DWORD)len, &written, NULL);
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    }
}

static void LogAddr(void *addr, HANDLE hSnap)
{
    if (hSnap != INVALID_HANDLE_VALUE && hSnap != NULL)
    {
        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        uintptr_t uaddr = (uintptr_t)addr;
        if (Module32First(hSnap, &me)) {
            do {
                uintptr_t base = (uintptr_t)me.modBaseAddr;
                if (uaddr >= base && uaddr < base + (uintptr_t)me.modBaseSize) {
                    char buf[512];
                    int len = _snprintf(buf, sizeof(buf) - 1, "  %s+%X\r\n", me.szModule, (unsigned int)(uaddr - base));
                    WriteLog(buf, ClampSnprintf(len, (int)sizeof(buf)));
                    return;
                }
            } while (Module32Next(hSnap, &me));
        }
    }
    char buf[128];
    int len = _snprintf(buf, sizeof(buf) - 1, "  unknown(%p)\r\n", addr);
    WriteLog(buf, ClampSnprintf(len, (int)sizeof(buf)));
}

// Backward-compatible wrapper (creates temporary snapshot) — kept to avoid breaking internal callers
static void LogAddr(void *addr)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        char buf[128];
        int l = _snprintf(buf, sizeof(buf) - 1, "  unknown(%p)\r\n", addr);
        WriteLog(buf, ClampSnprintf(l, (int)sizeof(buf)));
        return;
    }
    LogAddr(addr, snap);
    CloseHandle(snap);
}

static bool IsReadableMemory( const MEMORY_BASIC_INFORMATION& mbi, const BYTE *address, SIZE_T bytes )
{
	if( mbi.State != MEM_COMMIT || ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) )
		return false;

	const BYTE *regionEnd = ( const BYTE * )mbi.BaseAddress + mbi.RegionSize;
	if( address < ( const BYTE * )mbi.BaseAddress || bytes > ( SIZE_T )( regionEnd - address ) )
		return false;

	const DWORD protection = mbi.Protect & 0xFF;
	return protection == PAGE_READONLY || protection == PAGE_READWRITE
		|| protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ
		|| protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

static void LogStackSweep(DWORD esp, HANDLE hSnap)
{
    bool bOwnSnap = false;
    HANDLE snap = hSnap;
    if (snap == INVALID_HANDLE_VALUE || snap == NULL)
    {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) return;
        bOwnSnap = true;
    }

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);

    WriteLog("  Stack hits at ESP..ESP+0x6000 (dwords pointing into code):\r\n", 55);
    int logged = 0;
    uintptr_t last = (uintptr_t)-1;
    char buf[512];

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = (uintptr_t)esp - 4;
    uintptr_t end = (uintptr_t)esp + 0x6000;
    for (; a < end && logged < 80; a += 4)
    {
        if (VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof(mbi)) != sizeof(mbi)) continue;
        if( !IsReadableMemory( mbi, ( const BYTE * )(uintptr_t)a, sizeof( DWORD ) ) ) continue;

        DWORD v = 0;
        __try { v = *(volatile DWORD *)(uintptr_t)a; } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (v < 0x10000) continue;
        uintptr_t uv = (uintptr_t)v;
        if (Module32First(snap, &me))
        {
            do
            {
                uintptr_t base = (uintptr_t)me.modBaseAddr;
                if (uv >= base && uv < base + (uintptr_t)me.modBaseSize)
                {
                    if (uv != last)
                    {
                        int len = _snprintf(buf, sizeof(buf) - 1, "  %08X: %s+%X\r\n", (DWORD)a, me.szModule, (unsigned int)(uv - base));
                        WriteLog(buf, ClampSnprintf(len, (int)sizeof(buf)));
                        logged++;
                        last = uv;
                    }
                    break;
                }
            } while (Module32Next(snap, &me));
        }
    }
    if (bOwnSnap) CloseHandle(snap);
}

static void LogStackSweep(DWORD esp)
{
    LogStackSweep(esp, INVALID_HANDLE_VALUE);
}

static LONG WINAPI Tier0_VectoredHandler( EXCEPTION_POINTERS *pExceptionInfo )
{
    if( !pExceptionInfo || !pExceptionInfo->ExceptionRecord || !pExceptionInfo->ContextRecord )
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    const char *reason = NULL;
    if ( code == 0xC0000005 )
        reason = "ACCESS_VIOLATION";
    else if ( code == 0xC0000374 )
        reason = "HEAP_CORRUPTION";
    else if ( code == 0x80000003 )
        reason = "BREAKPOINT";
    else if ( code == 0xC00000FD )
        reason = "STACK_OVERFLOW";
    else if ( (code & 0xC0000000) == 0xC0000000 || (pExceptionInfo->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) )
        reason = "FATAL_EXCEPTION";

    if ( reason )
    {
        // Crash logging itself touches files, module snapshots and stack
        // memory. Do not recursively enter it if one of those operations faults.
        if( InterlockedCompareExchange( &g_CrashInProgress, 1, 0 ) != 0 )
            return EXCEPTION_CONTINUE_SEARCH;

        RotateCrashLogIfNeeded();

        // Create module snapshot once and reuse for all LogAddr calls
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

        HANDLE hFile = CreateFileA(GetCrashLogPath(),
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            char buf[1024];
            void *excAddr = pExceptionInfo->ExceptionRecord->ExceptionAddress;
            void *targetAddr = NULL;
            DWORD numPar = pExceptionInfo->ExceptionRecord->NumberParameters;
            if ( numPar > 1 )
                targetAddr = (void *)pExceptionInfo->ExceptionRecord->ExceptionInformation[1];

            int len = _snprintf(buf, sizeof(buf) - 1,
                "=== CRASH ===\r\nReason=%s Code=0x%08X Target=%p\r\nEIP:\r\n", reason, code, targetAddr);
            DWORD written;
            WriteFile(hFile, buf, (DWORD)ClampSnprintf(len, (int)sizeof(buf)), &written, NULL);
            FlushFileBuffers(hFile);
            CloseHandle(hFile);

            LogAddr(excAddr, hSnap);

            // EBP Chain Stack Trace (reads guarded with VirtualQuery + __try/__except so a nested fault
            // inside the handler cannot raise a second, uncaught access violation)
            DWORD *ebp = (DWORD*)(uintptr_t)pExceptionInfo->ContextRecord->Ebp;
            {
                PEXCEPTION_RECORD er = pExceptionInfo->ExceptionRecord;
                char rbuf[1024];
                int rlen = _snprintf(rbuf, sizeof(rbuf) - 1,
                    "Registers:\r\n  EIP=%08X EBP=%08X ESP=%08X EFL=%08X\r\n  EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\r\n  FaultInfo[0]=%08X [1]=%08X\r\n",
                    pExceptionInfo->ContextRecord->Eip, pExceptionInfo->ContextRecord->Ebp,
                    pExceptionInfo->ContextRecord->Esp, pExceptionInfo->ContextRecord->EFlags,
                    pExceptionInfo->ContextRecord->Eax, pExceptionInfo->ContextRecord->Ebx,
                    pExceptionInfo->ContextRecord->Ecx, pExceptionInfo->ContextRecord->Edx,
                    pExceptionInfo->ContextRecord->Esi, pExceptionInfo->ContextRecord->Edi,
                    (er->NumberParameters > 0) ? (DWORD)er->ExceptionInformation[0] : 0,
                    (er->NumberParameters > 1) ? (DWORD)er->ExceptionInformation[1] : 0);
                WriteLog(rbuf, ClampSnprintf(rlen, (int)sizeof(rbuf)));
                hFile = CreateFileA(GetCrashLogPath(),
                    FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    DWORD w2;
                    WriteFile(hFile, "EBP chain:\r\n", 12, &w2, NULL);
                    FlushFileBuffers(hFile);
                    CloseHandle(hFile);
                }
            }
            hFile = CreateFileA(GetCrashLogPath(),
                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD w2;
                WriteFile(hFile, "Stack:\r\n", 8, &w2, NULL);
                FlushFileBuffers(hFile);
                CloseHandle(hFile);
            }
            MEMORY_BASIC_INFORMATION mbi;
            for (int i = 0; i < 20; i++) {
                uintptr_t cur = (uintptr_t)ebp;
                if (cur < 0x10000 || cur > 0xFFFFFFF0) break;
                if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
                if( !IsReadableMemory( mbi, ( const BYTE * )cur, sizeof( DWORD ) * 2 ) ) break;
                DWORD retAddr = 0;
                DWORD nextEbpRaw = 0;
                __try { retAddr = ebp[1]; nextEbpRaw = ebp[0]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                if (retAddr == 0) break;
                LogAddr((void*)(uintptr_t)retAddr, hSnap);
                uintptr_t next = (uintptr_t)nextEbpRaw;
                if (next <= cur || next - cur > 0x10000) break;
                ebp = (DWORD*)next;
            }

            LogStackSweep(pExceptionInfo->ContextRecord->Esp, hSnap);

            hFile = CreateFileA(GetCrashLogPath(),
                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD w3;
                WriteFile(hFile, "=== END ===\r\n\r\n", 15, &w3, NULL);
                FlushFileBuffers(hFile);
                CloseHandle(hFile);
            }
        }

        if (hSnap != INVALID_HANDLE_VALUE && hSnap != NULL)
            CloseHandle(hSnap);

        WriteMiniDumpForException(code, pExceptionInfo);
        InterlockedExchange( &g_CrashInProgress, 0 );
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" BOOL WINAPI DllMain( HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved )
{
	UNREFERENCED_PARAMETER( lpvReserved );

	if ( fdwReason == DLL_PROCESS_ATTACH )
	{
		g_hInst = hinstDLL;
		GetCrashLogPath();

		DisableThreadLibraryCalls( hinstDLL );

		// Pin DLL in memory (chromehtml.dll may FreeLibrary us)
		HMODULE hPinned = NULL;
		if ( !GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			(LPCSTR)DllMain,
			&hPinned ) )
		{
			// Pinning failed — still continue, handler will be removed on detach if installed
		}

		// Install vectored exception handler for crash logging
		g_hVeh = AddVectoredExceptionHandler( 1, Tier0_VectoredHandler );

		// Ensure InterfaceReg factory (CreateInterface @314) is linked and
		// available via Sys_GetFactoryThis. Static InterfaceReg instances
		// self-register through their constructors before DllMain.
		{
			CreateInterfaceFn pFactory = Sys_GetFactoryThis();
			(void)pFactory;
		}
	}
	else if ( fdwReason == DLL_PROCESS_DETACH )
	{
		if ( g_hVeh )
		{
			RemoveVectoredExceptionHandler( g_hVeh );
			g_hVeh = nullptr;
		}
		// Release the process-wide high-resolution timer raised by PreciseSleep
		// so the global timer resolution is not left elevated after we unload.
		Tier0ShutdownHighResTimer();
	}
	return TRUE;
}

#endif
