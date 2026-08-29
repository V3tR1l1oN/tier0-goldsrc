// tier0 -- SBA diagnostic: reproduce bench phase pattern with a knob.
// Loads build\tier0.dll and runs Alloc/Free(size) bursts; detects the first
// iteration count at which the heap gets corrupted (STATUS_HEAP_CORRUPTION
// aborts the process, so we run fewer and fewer counts to bisect).
//
// Build: cl /O1 /GS- /nologo tests\sba_diag.cpp /Fetests\sba_diag.exe /link /SUBSYSTEM:CONSOLE

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <crtdbg.h>
#include <malloc.h>
#include <intrin.h>

static double now_msec(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

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

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 100000;
	unsigned size  = argc > 2 ? (unsigned)atoi(argv[2]) : 64;
	unsigned size2 = argc > 3 ? (unsigned)atoi(argv[3]) : (unsigned)-1;
	unsigned long long t0 = (unsigned long long)now_msec();

	HMODULE h = LoadLibraryA("tier0.dll");
	if (!h) { printf("cannot load tier0.dll\n"); return 2; }
	size_t *gp = (size_t *)GetProcAddress(h, "g_pMemAlloc");
	IMemAllocB *p = (IMemAllocB *)(*gp);

	printf("--- arena stats before ---\n");
	p->DumpStats();

	unsigned sizes[2] = { size, size2 };
	for (int s = 0; s < 2; ++s)
	{
		unsigned sz = sizes[s];
		if (sz == (unsigned)-1)
			break;
		printf("--- burst size=%u iters=%u (t=%.0fms) ---\n", sz, iters, now_msec() - t0);

		void **ptrs = (void **)malloc(sizeof(void *) * iters);
		for (unsigned i = 0; i < iters; ++i)
		{
			ptrs[i] = p->Alloc(sz);
			if ((i % 10000) == 0)
				printf("alloc %u\n", i);
		}
		printf("alloc'd %u (t=%0.0fms)\n", iters, now_msec() - t0);
		for (unsigned i = 0; i < iters; ++i)
		{
			if ((i % 10000) == 0)
				printf("chk %u (t=%0.0fms)\n", i, now_msec() - t0);
			size_t gs = p->GetSize(ptrs[i]);
			if (gs < sz)
			{
				printf("MISMATCH at %u: GetSize=%u expected>=%u ptr=%p\n", i, (unsigned)gs, sz, ptrs[i]);
				return 3;
			}
		}
		printf("classify-check ok (t=%0.0fms)\n", now_msec() - t0);
		for (unsigned i = 0; i < iters; ++i)
		{
			p->Free(ptrs[i], 0);
			if ((i % 10000) == 0)
				printf("free %u (t=%0.0fms)\n", i, now_msec() - t0);
		}
		printf("freed %u (t=%0.0fms)\n", iters, now_msec() - t0);
		free(ptrs);

		unsigned slot = (unsigned)-1;
		void *live = p->Alloc(sz);
		if (live && s == 0)
			slot = (unsigned)p->GetSize(live);   // exercise GetSize fast path
		printf("live GetSize=%u\n", slot);
		if (live)
			p->Free(live, 0);

		// fresh burst (no UAF): alloc-fill again, keep live
		void **keep = (void **)malloc(sizeof(void *) * iters);
		for (unsigned i = 0; i < iters; ++i)
			keep[i] = p->Alloc(sz);
		printf("refill %u (t=%0.0fms)\n", iters, now_msec() - t0);
		for (unsigned i = 0; i < iters; ++i)
			p->Free(keep[i], 0);
		free(keep);

		printf("--- arena stats after burst %d ---\n", s);
		p->DumpStats();
	}
	printf("--- sba_diag done (t=%0.0fms) ---\n", now_msec() - t0);
	return 0;
}