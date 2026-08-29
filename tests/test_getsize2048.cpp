// tier0 -- regression: 2048 is the bucket that gets 8192-byte slabs.
// Allocates 2000 live 2048-byte blocks and asserts GetSize()==2048 for each,
// then Realloc(64)->2048 and checks GetSize of the large block.
// Loads tier0.dll via g_pMemAlloc.
#include <stdio.h>
#include <windows.h>

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

#define SBA_HEADER 8

int main()
{
	HMODULE h = LoadLibraryA("tier0.dll");
	if (!h)
	{
		printf("FAIL: cannot load tier0.dll\n");
		return 1;
	}
	size_t *gp = (size_t *)GetProcAddress(h, "g_pMemAlloc");
	IMemAllocB *p = (IMemAllocB *)(*gp);

	const unsigned N = 2000;
	void *ptrs[2000] = { 0 };
	for (unsigned i = 0; i < 5; ++i)
	{
		ptrs[i] = p->Alloc(2048);
		unsigned *h = (unsigned *)((unsigned char *)ptrs[i] - 8);
		fprintf(stderr, "alloc[%u]=%p hdrBucket=%u\n", i, ptrs[i], h[1]);
	}
	for (unsigned i = 0; i < N; ++i)
	{
		ptrs[i] = p->Alloc(2048);
		if ((i % 200) == 0)
			fprintf(stderr, "alloc %u ptr=%p\n", i, ptrs[i]);
	}

	int bad = 0;
	for (unsigned i = 0; i < N; ++i)
	{
		unsigned char *b = (unsigned char *)ptrs[i];
		unsigned *hdr = (unsigned *)(b - SBA_HEADER);
		size_t sz = p->GetSize(ptrs[i]);
		if (sz != 2048)
		{
			if (bad < 20)
				fprintf(stderr, "MISMATCH at %u: GetSize=%u magic=0x%08X hdrBucket=%u ptr=%p\n",
					i, (unsigned)sz, hdr[0], hdr[1], ptrs[i]);
			++bad;
		}
	}
	printf("Alloc(2048) x%d: %d/%d GetSize==2048\n", N, N - bad, N);

	void *re = p->Realloc(p->Alloc(64), 2048);
	size_t reSz = p->GetSize(re);
	printf("Realloc(64->2048): GetSize=%u %s\n",
		(unsigned)reSz, reSz == 2048 ? "OK" : "MISMATCH");
	p->Free(re, 0);

	// Large non-SBA blocks go through the CRT heap: GetSize must be exact and
	// survive a Realloc that changes size class 256 <-> 20480 (CRT stays put)
	// and 20480 <-> 65536 (CRT realloc may move the block).
	struct { unsigned sz; } large[] = { { 20480 }, { 65536 } };
	for (int li = 0; li < 2; ++li)
	{
		const unsigned SZ = large[li].sz;
		void *lp = p->Alloc(SZ);
		unsigned char *b = (unsigned char *)lp;
		memset(b, 0xAB, SZ);
		size_t got = p->GetSize(lp);
		if (got != SZ)
		{
			fprintf(stderr, "LARGE MISMATCH: Alloc(%u) GetSize=%u ptr=%p\n", SZ, (unsigned)got, lp);
			bad++;
		}
		else
		{
			printf("Alloc(%u): GetSize=%u OK\n", SZ, (unsigned)got);
		}

		// realloc 256 -> SZ preserves prefix and content (free after)
		void *rB = p->Realloc(p->Alloc(256), SZ);
		memset(rB, 0xCD, SZ);
		size_t rGot = p->GetSize(rB);
		if (rGot != SZ)
		{
			fprintf(stderr, "LARGE REALLOC MISMATCH: 256->%u GetSize=%u\n", SZ, (unsigned)rGot);
			bad++;
		}
		else
		{
			printf("Realloc(256->%u): GetSize=%u OK\n", SZ, (unsigned)rGot);
		}
		p->Free(rB, 0);

		// realloc SZ -> 2048 (drop to SBA) then back up to SZ
		void *rS = p->Realloc(lp, 2048);
		memset(rS, 0xEF, 2048);
		size_t sGot = p->GetSize(rS);
		if (sGot < 2048)
		{
			fprintf(stderr, "realloc(%u->2048): GetSize=%u MISMATCH\n", SZ, (unsigned)sGot);
			bad++;
		}
		void *rUp = p->Realloc(rS, SZ);
		size_t uGot = p->GetSize(rUp);
		if (uGot != SZ)
		{
			fprintf(stderr, "realloc(2048->%u): GetSize=%u MISMATCH\n", SZ, (unsigned)uGot);
			bad++;
		}
		else
		{
			printf("Realloc(%u->2048->%u): GetSize %u->%u->%u OK\n", SZ, SZ,
				(unsigned)sGot, (unsigned)2048, (unsigned)uGot);
		}
		p->Free(rUp, 0);
	}

	for (unsigned i = 0; i < N; ++i)
		p->Free(ptrs[i], 0);

	FreeLibrary(h);
	if (bad || reSz != 2048)
	{
		printf("--- FAIL ---\n");
		return 1;
	}
	printf("--- ok ---\n");
	return 0;
}