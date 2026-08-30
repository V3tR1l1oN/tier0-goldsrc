// Focused ABI regression tests for the CWorkerThread one-slot handshake and
// WaitForMultipleEvents argument validation. The class is driven through the
// real exports, matching the way an engine loads tier0.dll.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int s_failures = 0;
#define CHECK(cond, text) \
    do { if (!(cond)) { printf("FAIL: %s\n", text); ++s_failures; } \
         else { printf("  ok: %s\n", text); } } while (0)

typedef void (__thiscall *CtorFn)(void *);
typedef void (__thiscall *DtorFn)(void *);
typedef bool (__thiscall *StartFn)(void *, unsigned);
typedef bool (__thiscall *AliveFn)(void *);
typedef bool (__thiscall *JoinFn)(void *, unsigned);
typedef bool (__thiscall *TerminateFn)(void *, int);
typedef void *(__thiscall *HandleFn)(void *);
typedef int (__thiscall *CallWorkerFn)(void *, unsigned, unsigned, bool);
typedef int (__cdecl *WaitEventsFn)(int, const void **, bool, unsigned);

template <class T>
static T Resolve(HMODULE dll, const char *name)
{
    return (T)GetProcAddress(dll, name);
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("threadtools ABI regression\n");

    HMODULE dll = LoadLibraryA("tier0.dll");
    CHECK(dll != NULL, "tier0.dll loads");
    if (!dll)
        return 1;

    CtorFn ctor = Resolve<CtorFn>(dll, "??0CWorkerThread@@QAE@XZ");
    DtorFn dtor = Resolve<DtorFn>(dll, "??1CWorkerThread@@UAE@XZ");
    StartFn start = Resolve<StartFn>(dll, "?Start@CThread@@UAE_NI@Z");
    AliveFn alive = Resolve<AliveFn>(dll, "?IsAlive@CThread@@QAE_NXZ");
    JoinFn join = Resolve<JoinFn>(dll, "?Join@CThread@@QAE_NI@Z");
    TerminateFn terminate = Resolve<TerminateFn>(dll, "?Terminate@CThread@@QAE_NH@Z");
    HandleFn handle = Resolve<HandleFn>(dll, "?GetThreadHandle@CThread@@QAEPAXXZ");
    CallWorkerFn call = Resolve<CallWorkerFn>(dll, "?CallWorker@CWorkerThread@@QAEHII_N@Z");
    WaitEventsFn waitEvents = Resolve<WaitEventsFn>(dll, "WaitForMultipleEvents");

    CHECK(ctor && dtor && start && alive && join && terminate && handle && call && waitEvents,
          "threadtools exports resolve");
    if (!(ctor && dtor && start && alive && join && terminate && handle && call && waitEvents))
    {
        FreeLibrary(dll);
        return 1;
    }

    void *worker = calloc(1, 512);
    CHECK(worker != NULL, "worker storage allocated");
    if (!worker)
    {
        FreeLibrary(dll);
        return 1;
    }

    ctor(worker);
    CHECK(start(worker, 0), "worker starts");
    CHECK(alive(worker), "worker reports alive");

    CHECK(call(worker, 0, 100, false) == 100,
          "asynchronous call returns submitted parameter");

    // The base CWorkerThread replies with the submitted parameter. Repeated
    // synchronous calls catch stale completion-event signals and slots that
    // never become reusable after Reply().
    Sleep(10);
    CHECK(call(worker, 0, 200, true) == 200,
          "synchronous call receives its own reply after async call");

    bool sequenceOk = true;
    for (unsigned i = 0; i < 100; ++i)
    {
        const unsigned value = 1000 + i;
        if (call(worker, 0, value, true) != (int)value)
        {
            sequenceOk = false;
            break;
        }
    }
    CHECK(sequenceOk, "one-slot worker can be reused for 100 synchronous calls");

    void *threadHandle = handle(worker);
    CHECK(threadHandle != NULL, "worker exposes a valid thread handle");
    if (threadHandle)
    {
        CHECK(terminate(worker, -1), "worker terminates cleanly after the request sequence");
        CHECK(join(worker, 2000), "terminated worker is joinable");
    }

    dtor(worker);
    free(worker);

    HANDLE event = CreateEventA(NULL, FALSE, TRUE, NULL);
    const void *eventPtr = &event;
    CHECK(waitEvents(0, NULL, false, 0) == -1,
          "WaitForMultipleEvents rejects an empty handle list");
    CHECK(waitEvents(1, NULL, false, 0) == -1,
          "WaitForMultipleEvents rejects a null handle list");
    CHECK(waitEvents(1, &eventPtr, false, 0) >= WAIT_OBJECT_0,
          "WaitForMultipleEvents accepts a valid handle pointer");
    CloseHandle(event);

    FreeLibrary(dll);
    if (s_failures)
    {
        printf("test_threadtools: %d FAILURES\n", s_failures);
        return 1;
    }

    printf("test_threadtools: OK\n");
    return 0;
}
