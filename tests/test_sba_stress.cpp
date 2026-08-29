// tier0 -- multithreaded allocator stress.
// N threads each churn mixed-size blocks (SBA pools + large CRT) through
// g_pMemAlloc concurrently. The SBA keeps one critical section and recycles
// slabs, so this exercises carving, the free-list, slab destruction and the
// per-page slab map under contention. Contents must survive untouched.
//
// Build (x86 MSVC prompt):
//   cl /O1 /GS- /nologo tests\test_sba_stress.cpp /Fetests\test_sba_stress.exe /link /SUBSYSTEM:CONSOLE
// Run: tests\test_sba_stress.exe [threads=8] [rounds=300]
// tier0.dll must sit next to the exe.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

class IMemAllocB
{
public:
	virtual void *Alloc_Debug(size_t, const char *, int, int = 0) = 0;
	virtual void *Alloc(size_t) = 0;
	virtual void *Realloc_Debug(void *, size_t, const char *, int) = 0;
	virtual void *Realloc(void *, size_t) = 0;
	virtual void Free_Debug(void *, const char *, int, int = 0) = 0;
	virtual void Free(void *, int = 0) = 0;
	virtual void *Expand_Debug(void *, size_t, const char *, int, int = 0) = 0;
	virtual void *Expand(void *, size_t) = 0;
	virtual size_t GetSize(void *) = 0;
	virtual void PushAllocDbgInfo(const char *, int) = 0;
	virtual void PopAllocDbgInfo() = 0;
	virtual long CrtSetBreakAlloc(long) = 0;
	virtual int CrtSetReportMode(int, int) = 0;
	virtual int CrtIsValidHeapPointer(const void *) = 0;
	virtual int CrtIsValidPointer(const void *, unsigned int, int) = 0;
	virtual int CrtCheckMemory() = 0;
	virtual int CrtSetDbgFlag(int) = 0;
	virtual void CrtMemCheckpoint(void *) = 0;
	virtual void DumpStats() = 0;
	virtual void *CrtSetReportFile(int, void *) = 0;
	virtual void *CrtSetReportHook(void *) = 0;
	virtual int CrtDbgReport(int, const char *, int, const char *, const char *) = 0;
	virtual int heapchk() = 0;
	virtual bool IsDebugHeap() = 0;
	virtual void GetActualDbgInfo(const char *&, int &) = 0;
	virtual void RegisterAllocation(const char *, int, int, int, unsigned) = 0;
	virtual void RegisterDeallocation(const char *, int, int, int, unsigned) = 0;
	virtual int GetVersion() = 0;
	virtual void CompactHeap() = 0;
	virtual void *SetAllocFailHandler(void *(*)(size_t)) = 0;
};

static IMemAllocB *g_pa;
static unsigned g_threads;
static unsigned g_rounds;
static volatile LONG g_failures = 0;

enum { POOL = 512 };

static unsigned __stdcall Worker(void *arg)
{
	unsigned tid = (unsigned)(unsigned long)arg;
	// Per-thread static mix table (deterministic, no shared RNG state)
	static const size_t mix[] = { 4, 8, 16, 32, 64, 96, 97, 128, 512, 2048, 2049, 65536 };
	unsigned char *blocks[POOL];
	size_t caps[POOL];
	size_t sizes[POOL];

	for (unsigned round = 0; round < g_rounds; ++round)
	{
		for (int i = 0; i < POOL; ++i)
		{
			size_t sz = mix[(tid * 7 + i + round) % 12];
			blocks[i] = (unsigned char *)g_pa->Alloc(sz);
			if (!blocks[i])
			{
				InterlockedIncrement(&g_failures);
				continue;
			}
			sizes[i] = sz;
			caps[i] = (sz <= 2048) ? ((sz < 97) ? (sz + 3) & ~(size_t)3 : (sz + 7) & ~(size_t)7) : sz;
			for (size_t k = 0; k < caps[i]; ++k)
				blocks[i][k] = (unsigned char)(i + (int)k + (int)tid);
		}
		// verify every allocation (read-after-write under concurrency)
		for (int i = 0; i < POOL; ++i)
		{
			if (!blocks[i])
				continue;
			for (size_t k = 0; k < caps[i]; ++k)
			{
				if (blocks[i][k] != (unsigned char)(i + (int)k + (int)tid))
				{
					InterlockedIncrement(&g_failures);
					goto verify_done;
				}
			}
verify_done: ;
		}
		// free in reverse order
		for (int i = POOL - 1; i >= 0; --i)
		{
			if (blocks[i])
				g_pa->Free(blocks[i], 0);
		}
	}
	return 0;
}

// Cross-ownership phase: every worker thread allocates a batch and hands it to
// the main thread, which verifies contents + GetSize and frees everything on a
// different thread than the one that allocated it. This is the engine's "many
// workers allocate, main thread frees" shape and it pounds the TLS-cache spill
// paths plus slab destruction under cross-thread ownership.
enum { HO = 512 };
static void *g_handoff[64][HO];
static size_t g_hcap[64][HO];

static unsigned __stdcall CrossWorker(void *arg)
{
	unsigned tid = (unsigned)(unsigned long)arg;   // 1..g_threads
	unsigned row = tid - 1;
	static const size_t mix[] = { 4, 16, 96, 97, 128, 1024, 2048, 8192 };
	for (int i = 0; i < HO; ++i)
	{
		size_t sz = mix[(tid + i) % 8];
		unsigned char *b = (unsigned char *)g_pa->Alloc(sz);
		if (!b)
		{
			InterlockedIncrement(&g_failures);
			g_handoff[row][i] = NULL;
			continue;
		}
		g_handoff[row][i] = b;
		g_hcap[row][i] = (sz <= 2048) ? ((sz < 97) ? (sz + 3) & ~(size_t)3 : (sz + 7) & ~(size_t)7) : sz;
		for (size_t k = 0; k < g_hcap[row][i]; ++k)
			b[k] = (unsigned char)(i + (int)k + (int)tid);
	}
	return 0;
}

static int RunHandoffPhase(void)
{
	HANDLE *threads = (HANDLE *)calloc(g_threads, sizeof(HANDLE));
	for (unsigned t = 0; t < g_threads; ++t)
		threads[t] = (HANDLE)_beginthreadex(NULL, 0, CrossWorker, (void *)(unsigned long)(t + 1), 0, NULL);
	for (unsigned t = 0; t < g_threads; ++t)
	{
		if (threads[t])
			WaitForSingleObject(threads[t], INFINITE);
	}
	free(threads);

	int bad = 0;
	for (unsigned t = 0; t < g_threads; ++t)
	{
		for (int i = 0; i < HO; ++i)
		{
			unsigned char *b = (unsigned char *)g_handoff[t][i];
			if (!b)
				continue;
			size_t cap = g_hcap[t][i];
			if (g_pa->GetSize(b) < cap)
			{
				printf("handoff GetSize fail t=%u i=%d got=%u want>=%u\n", t + 1, i, (unsigned)g_pa->GetSize(b), (unsigned)cap);
				bad = 1;
			}
			for (size_t k = 0; k < cap; ++k)
			{
				if (b[k] != (unsigned char)(i + (int)k + (int)(t + 1)))
				{
					printf("handoff content fail t=%u i=%d k=%u\n", t + 1, i, (unsigned)k);
					bad = 1;
					break;
				}
			}
			g_pa->Free(b, 0);
		}
	}
	return bad;
}

int main(int argc, char **argv)
{
	g_threads = (argc > 1) ? (unsigned)atoi(argv[1]) : 8;
	g_rounds = (argc > 2) ? (unsigned)atoi(argv[2]) : 300;
	if (g_threads < 1 || g_threads > 64) g_threads = 8;
	if (g_rounds < 1) g_rounds = 300;

	HMODULE h = LoadLibraryA("tier0.dll");
	if (!h)
	{
		printf("FAIL: cannot load tier0.dll (place build\\tier0.dll next to exe)\n");
		return 1;
	}

	size_t *gp = (size_t *)GetProcAddress(h, "g_pMemAlloc");
	if (!gp || !*gp)
	{
		printf("FAIL: g_pMemAlloc export missing\n");
		return 1;
	}
	g_pa = (IMemAllocB *)(*gp);

	HANDLE *threads = (HANDLE *)calloc(g_threads, sizeof(HANDLE));
	for (unsigned t = 0; t < g_threads; ++t)
		threads[t] = (HANDLE)_beginthreadex(NULL, 0, Worker, (void *)(unsigned long)(t + 1), 0, NULL);
	for (unsigned t = 0; t < g_threads; ++t)
	{
		if (threads[t])
			WaitForSingleObject(threads[t], INFINITE);
	}
	free(threads);

	int handoffFail = RunHandoffPhase();
	InterlockedExchangeAdd(&g_failures, handoffFail);

	if (g_failures)
	{
		printf("--- %d FAILURES (threads=%u rounds=%u) ---\n", (int)g_failures, g_threads, g_rounds);
		return 1;
	}
	printf("--- sba stress ok (%u threads x %u rounds x %d blocks + cross-thread handoff %d x %d) ---\n",
		g_threads, g_rounds, POOL, g_threads, HO);
	return 0;
}