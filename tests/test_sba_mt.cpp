#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
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
};

static IMemAllocB *s_p;
static unsigned s_itersPerThread;
static unsigned s_sizeMin = 4;
static unsigned s_sizeMax = 2048;
static double s_totalMs;

static double nowMs()
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static unsigned __stdcall worker(void *arg)
{
	unsigned int seed = (unsigned int)(size_t)arg * 7919u + 17u;
	void *p = NULL;
	for (unsigned i = 0; i < s_itersPerThread; ++i)
	{
		unsigned n = s_sizeMin + (unsigned)((seed = seed * 1103515245u + 12345u) % (s_sizeMax - s_sizeMin + 1));
		p = s_p->Alloc(n);
		if (p)
		{
			s_p->Free(p, 0);
		}
	}
	return 0;
}

static void run(unsigned nThreads)
{
	HANDLE *h = (HANDLE *)malloc(nThreads * sizeof(HANDLE));
	double t0 = nowMs();
	for (unsigned i = 0; i < nThreads; ++i)
		h[i] = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)(size_t)(i + 1), 0, NULL);
	WaitForMultipleObjects(nThreads, h, TRUE, INFINITE);
	double dt = nowMs() - t0;
	s_totalMs = dt;
	for (unsigned i = 0; i < nThreads; ++i) CloseHandle(h[i]);
	free(h);
	printf("  %2u threads: %8.2f ms  (%7.1f ns/op)\n",
		nThreads, dt, 1000000.0 * dt / ((double)nThreads * s_itersPerThread));
}

int main(int argc, char **argv)
{
	HMODULE m = LoadLibraryA("tier0.dll");
	if (!m) { printf("cannot load tier0.dll\n"); return 1; }
	size_t *gp = (size_t *)GetProcAddress(m, "g_pMemAlloc");
	if (!gp) { printf("no g_pMemAlloc\n"); return 1; }
	s_p = (IMemAllocB *)(*gp);

	unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 2000000;
	unsigned sizeMin = argc > 2 ? (unsigned)atoi(argv[2]) : 4;
	unsigned sizeMax = argc > 3 ? (unsigned)atoi(argv[3]) : 2048;
	s_itersPerThread = iters / 16 ? iters / 16 : 1000;
	s_sizeMin = sizeMin;
	s_sizeMax = sizeMax;

	printf("contended alloc/free  size %u..%u  per-thread iters=%u\n", sizeMin, sizeMax, s_itersPerThread);
	unsigned threads[3] = { 1, 4, 16 };
	for (int i = 0; i < 3; ++i)
		run(threads[i]);

	printf("ok\n");
	return 0;
}