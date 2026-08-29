// tier0 -- functional benchmark for internal hot paths (plat time + allocs).
// Loads the built tier0.dll via LoadLibrary and measures costs of the
// per-frame timer and the memory allocator without running the game.
//
// Build (x86 MSVC prompt, as with the other tests):
//   cl /O1 /GS- /nologo tests\bench.cpp /Fetests\bench.exe /link /SUBSYSTEM:CONSOLE
// Run with build\tier0.dll placed next to bench.exe.

#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <process.h>

// Minimal mirror of IMemAlloc: vtable layout must match
// public/tier0/memalloc.h (declaration order from slot 0).
class IMemAllocB
{
public:
	virtual void *Alloc_Debug(size_t, const char *, int, int = 0) = 0;	// slot 0
	virtual void *Alloc(size_t) = 0;									// slot 1
	virtual void *Realloc_Debug(void *, size_t, const char *, int) = 0;	// slot 2
	virtual void *Realloc(void *, size_t) = 0;							// slot 3
	virtual void Free_Debug(void *, const char *, int, int = 0) = 0;	// slot 4
	virtual void Free(void *, int = 0) = 0;								// slot 5
	virtual void *Expand_Debug(void *, size_t, const char *, int, int = 0) = 0;	// slot 6
	virtual void *Expand(void *, size_t) = 0;							// slot 7
	virtual size_t GetSize(void *) = 0;									// slot 8
};

typedef double (__cdecl *Plat_FloatTimeFn)();
typedef unsigned int (__cdecl *Plat_MSTimeFn)();
typedef unsigned (__stdcall *SimpleWorkerFn)(void *);
typedef void *(__cdecl *CreateSimpleThreadFn)(SimpleWorkerFn, void *, unsigned *, unsigned);

static double NowSec()
{
	static LARGE_INTEGER freq = {};
	static LARGE_INTEGER base = {};
	if (!freq.QuadPart)
	{
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&base);
	}
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)(c.QuadPart - base.QuadPart) / (double)freq.QuadPart;
}

static double BenchPlatFloatTime(Plat_FloatTimeFn fn, unsigned int iters)
{
	double last = fn();	// warmup / init
	double t0 = NowSec();
	double prev = last;
	int monotonic = 1;
	for (unsigned int i = 0; i < iters; ++i)
	{
		double v = fn();
		if (v < prev)
			monotonic = 0;
		prev = v;
	}
	double dt = NowSec() - t0;
	printf("Plat_FloatTime         : %8.2f ns/call (%u calls in %.3f s)\n",
		monotonic ? dt * 1e9 / iters : -1.0, iters, dt);
	return monotonic ? dt * 1e9 / iters : -1.0;
}

static size_t ExpectedRound(size_t n)
{
	// mirrors CStdMemAlloc small-block rounding (<=2048)
	if (n <= 2048)
		return n < 97 ? (n + 3) & ~(size_t)3 : (n + 7) & ~(size_t)7;
	return n;
}

static void BenchAllocFree(IMemAllocB *pAlloc, size_t size, unsigned int iters)
{
	void **ptrs = new void *[iters];
	for (unsigned int i = 0; i < iters; ++i)
		ptrs[i] = NULL;

	double t0 = NowSec();
	for (unsigned int i = 0; i < iters; ++i)
		ptrs[i] = pAlloc->Alloc(size);
	double tMid = NowSec();
	for (unsigned int i = 0; i < iters; ++i)
		pAlloc->Free(ptrs[i], 0);
	double tEnd = NowSec();

	size_t expectedLog = ExpectedRound(size);

	// verify GetSize reports the real usable size on a LIVE block
	void *live = pAlloc->Alloc(size);
	size_t liveSize = pAlloc->GetSize(live);
	pAlloc->Free(live, 0);
	delete[] ptrs;

	int ok = (liveSize >= expectedLog) ? 1 : 0;
	printf("Alloc/Free size %-6u   : %8.2f ns/op (alloc+free), GetSize=%u expect>=%u %s\n",
		(unsigned int)size, (tEnd - t0) * 1e9 / (2.0 * iters),
		(unsigned int)liveSize, (unsigned int)expectedLog, ok ? "OK" : "MISMATCH");
}

static void BenchRealloc(IMemAllocB *pAlloc, unsigned int iters)
{
	void *p = pAlloc->Alloc(64);
	double t0 = NowSec();
	for (unsigned int i = 0; i < iters; ++i)
		p = pAlloc->Realloc(p, 2048);
	double dt = NowSec() - t0;
	size_t liveSize = pAlloc->GetSize(p);
	pAlloc->Free(p, 0);
	printf("Realloc 64->2048      : %8.2f ns/op, final GetSize=%u %s\n",
		dt * 1e9 / iters, (unsigned int)liveSize, liveSize >= 2048 ? "OK" : "MISMATCH");
}

// Clock stability: Plat_FloatTime is the engine's frametime source, so any
// glitch between consecutive reads becomes a visible hitch (jitter). Measure
// read-to-read continuity in a clean pass and again while a noise thread
// churns allocations (battle-like contention). QPC deltas "should" be ~100ns,
// huge gaps mean the clock path stalls or a preemption slipped through.
struct JitterShared
{
	IMemAllocB *p;
	volatile bool stop;
};

static unsigned __stdcall JitterNoise(void *pv)
{
	JitterShared *s = (JitterShared *)pv;
	while (!s->stop)
	{
		void *q = s->p->Alloc(256);
		s->p->Free(q, 0);
	}
	return 0;
}

static void BenchClockJitter(Plat_FloatTimeFn pfnFloat, IMemAllocB *pAlloc,
	CreateSimpleThreadFn fnThread, unsigned int iters, int noise)
{
	JitterShared s;
	s.p = pAlloc;
	s.stop = false;

	void *h = NULL;
	if (noise)
		h = fnThread(JitterNoise, &s, NULL, 0);

	pfnFloat();                    // warmup
	double prev = pfnFloat();
	double maxGap = 0.0;
	unsigned long long bigSpikes = 0;   // > 5 us
	unsigned long long medSpikes = 0;   // > 1 us
	for (unsigned int i = 0; i < iters; ++i)
	{
		double v = pfnFloat();
		double gap = v - prev;
		prev = v;
		if (gap > maxGap)
			maxGap = gap;
		if (gap > 5e-6)
			bigSpikes++;
		else if (gap > 1e-6)
			medSpikes++;
	}

	if (noise)
	{
		s.stop = true;
		WaitForSingleObject((HANDLE)h, INFINITE);
		CloseHandle((HANDLE)h);
	}

	printf("Clock stability     : %s pass max gap %7.2f us, spikes 1-5us %llu / >5us %llu (%9.5f%% >1us)\n",
		noise ? "noise-t" : "clean  ",
		maxGap * 1e6, medSpikes, bigSpikes, 100.0 * (double)(medSpikes + bigSpikes) / (double)iters);
}

// GetSize on LIVE large (non-SBA) blocks: these go through HeapSize() which
// takes the CRT heap lock -- worth measuring so the number is on record.
static void BenchGetSizeLive(IMemAllocB *pAlloc, size_t size, unsigned int count)
{
	void **ptrs = new void *[count];
	for (unsigned int i = 0; i < count; ++i)
		ptrs[i] = pAlloc->Alloc(size);

	double t0 = NowSec();
	size_t acc = 0;
	for (unsigned int r = 0; r < 30; ++r)
		for (unsigned int i = 0; i < count; ++i)
			acc += pAlloc->GetSize(ptrs[i]);
	double dt = NowSec() - t0;

	size_t ok = (acc == count * 30u * size) ? 1 : 0;   // HeapSize must be exact
	printf("GetSize live %-6u    : %8.2f ns/call (count %u, %s)\n",
		(unsigned int)size, dt * 1e9 / (count * 30u), count, ok ? "exact" : "MISMATCH");

	for (unsigned int i = 0; i < count; ++i)
		pAlloc->Free(ptrs[i], 0);
	delete[] ptrs;
}

static unsigned __stdcall NoopWorker(void *) { return 0; }

static void BenchThreadCreateJoin(CreateSimpleThreadFn fn, unsigned int iters)
{
	// warm one spawn (heap + OS thread setup)
	void *h = fn(NoopWorker, NULL, NULL, 0);
	if (h)
	{
		WaitForSingleObject((HANDLE)h, INFINITE);
		CloseHandle((HANDLE)h);
	}

	double t0 = NowSec();
	for (unsigned int i = 0; i < iters; ++i)
	{
		h = fn(NoopWorker, NULL, NULL, 0);
		WaitForSingleObject((HANDLE)h, INFINITE);
		CloseHandle((HANDLE)h);
	}
	double dt = NowSec() - t0;
	printf("Thread create+join    : %8.2f us/spawn (%u spawns)\n",
		dt * 1e6 / iters, iters);
}

// Producers allocate blocks of one size on their own threads; the main thread
// frees them all. Freeing on a different thread than allocation walks the
// calling thread's TLS caches and spills to the global pool past the cap --
// this is the engine's "many worker threads allocate, main thread frees" shape.
struct CrossChurnShared
{
	IMemAllocB *p;
	void **ptrs;
	volatile long head;   // next free slot (locked increment)
	unsigned int perThread;
};

static unsigned __stdcall CrossProducer(void *pv)
{
	CrossChurnShared *c = (CrossChurnShared *)pv;
	for (unsigned int i = 0; i < c->perThread; ++i)
	{
		long idx = InterlockedIncrement(&c->head) - 1;
		c->ptrs[idx] = c->p->Alloc(256);
	}
	return 0;
}

static void BenchCrossThreadChurn(IMemAllocB *pAlloc, CreateSimpleThreadFn fnThread,
	unsigned int threads, unsigned int perThread, size_t size)
{
	unsigned int total = threads * perThread;
	CrossChurnShared c;
	c.p = pAlloc;
	c.ptrs = new void *[total];
	c.head = 0;
	c.perThread = perThread;

	void **threadsH = new void *[threads];
	double t0 = NowSec();
	for (unsigned int t = 0; t < threads; ++t)
		threadsH[t] = fnThread(CrossProducer, &c, NULL, 0);
	for (unsigned int t = 0; t < threads; ++t)
	{
		WaitForSingleObject((HANDLE)threadsH[t], INFINITE);
		CloseHandle((HANDLE)threadsH[t]);
	}
	double tMid = NowSec();
	for (unsigned int i = 0; i < total; ++i)
		pAlloc->Free(c.ptrs[i], 0);
	double tEnd = NowSec();

	printf("XThread alloc %-6u   : %8.2f ns/alloc (%u threads), free-main %8.2f ns/op\n",
		(unsigned int)size, (tMid - t0) * 1e9 / total, threads, (tEnd - tMid) * 1e9 / total);
	delete[] c.ptrs;
	delete[] threadsH;
}

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	HMODULE h = LoadLibraryA("tier0.dll");
	if (!h)
	{
		printf("FAIL: cannot load tier0.dll (place build\\tier0.dll next to exe)\n");
		return 1;
	}

	Plat_FloatTimeFn pfnFloatTime = (Plat_FloatTimeFn)GetProcAddress(h, "Plat_FloatTime");
	Plat_MSTimeFn pfnMSTime = (Plat_MSTimeFn)GetProcAddress(h, "Plat_MSTime");
	CreateSimpleThreadFn pfnThread = (CreateSimpleThreadFn)GetProcAddress(h, "CreateSimpleThread");
	size_t *pGpa = (size_t *)GetProcAddress(h, "g_pMemAlloc");
	IMemAllocB **ppAlloc = (IMemAllocB **)pGpa;
	IMemAllocB *pAlloc = ppAlloc ? *ppAlloc : NULL;

	if (!pfnFloatTime || !pfnMSTime || !pfnThread || !pAlloc)
	{
		printf("FAIL: missing exports (Plat_FloatTime=@244, g_pMemAlloc=@311)\n");
		return 1;
	}

	printf("tier0 hot-path benchmark\n");
	double ns = BenchPlatFloatTime(pfnFloatTime, 5000000);
	(void)ns;
	printf("\n");

	// Plat_MSTime cost
	{
		unsigned int warm = pfnMSTime();
		double t0 = NowSec();
		unsigned int prev = warm;
		int monotonic = 1;
		for (unsigned int i = 0; i < 3000000; ++i)
		{
			unsigned int v = pfnMSTime();
			if (v < prev)
				monotonic = 0;
			prev = v;
		}
		double dt = NowSec() - t0;
		printf("Plat_MSTime           : %8.2f ns/call (%s)\n",
			dt * 1e9 / 3000000, monotonic ? "monotonic" : "warning: wrapped");
		printf("\n");
	}

	printf("-- clock stability (frametime source)\n");
	BenchClockJitter(pfnFloatTime, pAlloc, pfnThread, 2000000, 0);
	BenchClockJitter(pfnFloatTime, pAlloc, pfnThread, 2000000, 1);
	printf("\n");

	size_t sizes[] = { 4, 64, 256, 1024, 2048, 8192, 65536 };
	unsigned int phaseIters[] = { 200000, 200000, 200000, 50000, 20000, 2000, 128 };
	for (int i = 0; i < 7; ++i)
	{
		printf("-- phase %d/7 size %u\n", i + 1, (unsigned int)sizes[i]);
		BenchAllocFree(pAlloc, sizes[i], phaseIters[i]);
	}

	printf("-- phase realloc 64->2048\n");
	BenchRealloc(pAlloc, 50000);

	printf("\n");
	printf("-- getsize live large blocks\n");
	BenchGetSizeLive(pAlloc, 8192, 2000);
	BenchGetSizeLive(pAlloc, 65536, 256);

	printf("\n");
	printf("-- thread create/join\n");
	BenchThreadCreateJoin(pfnThread, 200);

	printf("-- cross-thread churn (alloc 4thr -> free main)\n");
	BenchCrossThreadChurn(pAlloc, pfnThread, 4, 50000, 256);

	printf("--- bench done ---\n");
	return 0;
}