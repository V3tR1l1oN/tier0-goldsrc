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