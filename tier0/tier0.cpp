#include "platform.h"
#include "minidump.h"
#include <tlhelp32.h>

#ifdef WIN32
#include "winlite.h"

// AddVectoredExceptionHandler() comes from the SDK header (errhandlingapi.h);
// re-declaring it here without dllimport produced C4273 "inconsistent dll
// linkage" and risked binding to the wrong symbol.

static HINSTANCE g_hInst = NULL;
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
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(hFile, text, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }
}

static void LogAddr(void *addr)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);

    if (Module32First(snap, &me)) {
        do {
            if ((DWORD)addr >= (DWORD)me.modBaseAddr && (DWORD)addr < (DWORD)(me.modBaseAddr + me.modBaseSize)) {
                char buf[512];
                int len = _snprintf(buf, sizeof(buf) - 1, "  %s+%X\r\n", me.szModule, (DWORD)addr - (DWORD)me.modBaseAddr);
                WriteLog(buf, ClampSnprintf(len, (int)sizeof(buf)));
                CloseHandle(snap);
                return;
            }
        } while (Module32Next(snap, &me));
    }

    char buf[128];
    int len = _snprintf(buf, sizeof(buf) - 1, "  unknown(%p)\r\n", addr);
    WriteLog(buf, ClampSnprintf(len, (int)sizeof(buf)));
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

static void LogStackSweep(DWORD esp)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);

    WriteLog("  Stack hits at ESP..ESP+0x6000 (dwords pointing into code):\r\n", 55);
    int logged = 0;
    DWORD last = 0xFFFFFFFF;
    char buf[512];

    MEMORY_BASIC_INFORMATION mbi;
    DWORD a = esp - 4;
    DWORD end = esp + 0x6000;
    for (; a < end && logged < 80; a += 4)
    {
        if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) != sizeof(mbi)) continue;
        if( !IsReadableMemory( mbi, ( const BYTE * )a, sizeof( DWORD ) ) ) continue;

        DWORD v = *(volatile DWORD *)a;
        if (v < 0x10000) continue;
        if (Module32First(snap, &me))
        {
            do
            {
                if (v >= (DWORD)me.modBaseAddr && v < (DWORD)(me.modBaseAddr + me.modBaseSize))
                {
                    if (v != last)
                    {
                        int len = _snprintf(buf, sizeof(buf) - 1, "  %08X: %s+%X\r\n", a, me.szModule, v - (DWORD)me.modBaseAddr);
                        WriteLog(buf, ClampSnprintf(len, (int)sizeof(buf)));
                        logged++;
                        last = v;
                    }
                    break;
                }
            } while (Module32Next(snap, &me));
        }
    }
    CloseHandle(snap);
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

    if ( reason )
    {
        // Crash logging itself touches files, module snapshots and stack
        // memory. Do not recursively enter it if one of those operations faults.
        if( InterlockedCompareExchange( &g_CrashInProgress, 1, 0 ) != 0 )
            return EXCEPTION_CONTINUE_SEARCH;

        RotateCrashLogIfNeeded();

        HANDLE hFile = CreateFileA(GetCrashLogPath(),
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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
            CloseHandle(hFile);

            LogAddr(excAddr);

            // EBP Chain Stack Trace (reads guarded with VirtualQuery so a nested fault
            // inside the handler cannot raise a second, uncaught access violation)
            DWORD *ebp = (DWORD*)pExceptionInfo->ContextRecord->Ebp;
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
                    FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    DWORD w2;
                    WriteFile(hFile, "EBP chain:\r\n", 12, &w2, NULL);
                    CloseHandle(hFile);
                }
            }
            hFile = CreateFileA(GetCrashLogPath(),
                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD w2;
                WriteFile(hFile, "Stack:\r\n", 8, &w2, NULL);
                CloseHandle(hFile);
            }
            MEMORY_BASIC_INFORMATION mbi;
            for (int i = 0; i < 20; i++) {
                if ((DWORD)ebp < 0x10000 || (DWORD)ebp > 0x7FFFFFFF) break;
                if (VirtualQuery((LPCVOID)ebp, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
                if( !IsReadableMemory( mbi, ( const BYTE * )ebp, sizeof( DWORD ) * 2 ) ) break;
                DWORD retAddr = ebp[1];
                if (retAddr == 0) break;
                LogAddr((void*)retAddr);
                ebp = (DWORD*)ebp[0];
            }

            LogStackSweep(pExceptionInfo->ContextRecord->Esp);

            hFile = CreateFileA(GetCrashLogPath(),
                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD w3;
                WriteFile(hFile, "=== END ===\r\n\r\n", 15, &w3, NULL);
                CloseHandle(hFile);
            }
        }

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
		HMODULE hPinned;
		GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			(LPCSTR)DllMain,
			&hPinned );

		// Install vectored exception handler for crash logging
		AddVectoredExceptionHandler( 1, Tier0_VectoredHandler );
	}
	return TRUE;
}

#endif
