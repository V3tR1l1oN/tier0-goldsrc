// SBA arena lifecycle smoke test.
// It forces the background prepage thread to start, exercises the arena, and
// exits cleanly. A shutdown path that destroys the arena before joining the
// helper can fail here under Application Verifier or a short CI timeout.
#include <windows.h>
#include <stdio.h>
#include <stddef.h>

struct IMemAllocView
{
    void *vtable;
};

typedef void *(__thiscall *AllocFn)(void *, size_t);
typedef void (__thiscall *FreeFn)(void *, void *, int);

int main()
{
    SetEnvironmentVariableA("SBA_PREPAGE", "1");
    SetEnvironmentVariableA("SBA_ARENA_MB", "16");

    HMODULE dll = LoadLibraryA("tier0.dll");
    if (!dll)
    {
        printf("FAIL: cannot load tier0.dll\n");
        return 1;
    }

    void **ppAlloc = (void **)GetProcAddress(dll, "g_pMemAlloc");
    if (!ppAlloc || !*ppAlloc)
    {
        printf("FAIL: g_pMemAlloc export missing\n");
        return 1;
    }

    void *allocator = *ppAlloc;
    void **vtable = *(void ***)allocator;
    AllocFn alloc = (AllocFn)vtable[1];
    FreeFn freeFn = (FreeFn)vtable[5];
    if (!alloc || !freeFn)
    {
        printf("FAIL: allocator vtable incomplete\n");
        return 1;
    }

    void *blocks[256] = {};
    for (int round = 0; round < 8; ++round)
    {
        for (int i = 0; i < 256; ++i)
        {
            blocks[i] = alloc(allocator, 2048);
            if (!blocks[i])
            {
                printf("FAIL: arena allocation failed at %d/%d\n", round, i);
                return 1;
            }
            ((unsigned char *)blocks[i])[0] = (unsigned char)i;
        }
        for (int i = 0; i < 256; ++i)
            freeFn(allocator, blocks[i], 0);
    }

    Sleep(25);
    printf("test_sba_shutdown: OK\n");
    return 0;
}
