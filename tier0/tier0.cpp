#include "platform.h"
#include <tlhelp32.h>

#ifdef WIN32
#include "winlite.h"

extern "C" PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);

static void WriteLog(const char *text, int len)
{
    HANDLE hFile = CreateFileA("D:\\SteamLibrary\\steamapps\\common\\Half-Life\\crash.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(hFile, text, len, &written, NULL);
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
                WriteLog(buf, len);
                CloseHandle(snap);
                return;
            }
        } while (Module32Next(snap, &me));
    }

    char buf[128];
    int len = _snprintf(buf, sizeof(buf) - 1, "  unknown(%p)\r\n", addr);
    WriteLog(buf, len);
    CloseHandle(snap);
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
        if (mbi.State != MEM_COMMIT) continue;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;

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
                        WriteLog(buf, len);
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
    if ( pExceptionInfo->ExceptionRecord->ExceptionCode == 0xC0000005 )
    {
        HANDLE hFile = CreateFileA("D:\\SteamLibrary\\steamapps\\common\\Half-Life\\crash.log",
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            char buf[1024];
            DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
            void *excAddr = pExceptionInfo->ExceptionRecord->ExceptionAddress;
            void *targetAddr = (void*)pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
            DWORD op = (DWORD)pExceptionInfo->ExceptionRecord->ExceptionInformation[0];

            int len = _snprintf(buf, sizeof(buf) - 1,
                "=== CRASH ===\r\nCode=0x%08X Op=%d Target=%p\r\nEIP:\r\n", code, op, targetAddr);
            DWORD written;
            WriteFile(hFile, buf, len, &written, NULL);
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
                WriteLog(rbuf, rlen);
                hFile = CreateFileA("D:\\SteamLibrary\\steamapps\\common\\Half-Life\\crash.log",
                    FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    DWORD w2;
                    WriteFile(hFile, "EBP chain:\r\n", 12, &w2, NULL);
                    CloseHandle(hFile);
                }
            }
            hFile = CreateFileA("D:\\SteamLibrary\\steamapps\\common\\Half-Life\\crash.log",
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
                if (mbi.State != MEM_COMMIT) break;
                if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) break;
                DWORD retAddr = ebp[1];
                if (retAddr == 0) break;
                LogAddr((void*)retAddr);
                ebp = (DWORD*)ebp[0];
            }

            LogStackSweep(pExceptionInfo->ContextRecord->Esp);

            hFile = CreateFileA("D:\\SteamLibrary\\steamapps\\common\\Half-Life\\crash.log",
                FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD w3;
                WriteFile(hFile, "=== END ===\r\n\r\n", 15, &w3, NULL);
                CloseHandle(hFile);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" BOOL WINAPI DllMain( HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved )
{
	UNREFERENCED_PARAMETER( lpvReserved );

	if ( fdwReason == DLL_PROCESS_ATTACH )
	{
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
