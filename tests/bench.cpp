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
	size_t *pGpa = (size_t *)GetProcAddress(h, "g_pMemAlloc");
	IMemAllocB **ppAlloc = (IMemAllocB **)pGpa;
	IMemAllocB *pAlloc = ppAlloc ? *ppAlloc : NULL;

	if (!pfnFloatTime || !pAlloc)
	{
		printf("FAIL: missing exports (Plat_FloatTime=@244, g_pMemAlloc=@311)\n");
		return 1;
	}

	printf("tier0 hot-path benchmark\n");
	double ns = BenchPlatFloatTime(pfnFloatTime, 5000000);
	(void)ns;
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

	printf("--- bench done ---\n");
	return 0;
}