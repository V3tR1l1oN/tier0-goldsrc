// tier0 -- allocator correctness test.
// Verifies CStdMemAlloc semantics against the original tier0 behavior:
//   * Alloc rounds small sizes like the original SBA and never returns NULL
//     for size 0,
//   * Realloc(NULL, n) behaves as Alloc(n),
//   * Realloc(p, 0) does NOT free the block (original allocates a 1-byte
//     block instead; we normalize to 4 bytes after SBA rounding),
//   * shrinking realloc preserves the prefix,
//   * Free(NULL) is safe, GetVersion/IsDebugHeap match original (0 / false).
//
// Build (x86 MSVC prompt):
//   cl /O1 /GS- /nologo tests\test_mem.cpp /Fetests\test_mem.exe /link /SUBSYSTEM:CONSOLE
// Run with build\tier0.dll placed next to test_mem.exe.

#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

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

static int g_fails = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { printf("FAIL: %s\n", msg); g_fails++; } \
	} while (0)

static size_t ExpectedRound(size_t n)
{
	if (n <= 2048)
		return n < 97 ? (n + 3) & ~(size_t)3 : (n + 7) & ~(size_t)7;
	return n;
}

int main()
{
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
	IMemAllocB *p = (IMemAllocB *)(*gp);
	(void)gp;

	// 1) Alloc(0) -> non-null, writable block (normalized to 1 -> rounded to 4)
	void *z = p->Alloc(0);
	CHECK(z != NULL, "Alloc(0) returns NULL");
	if (z)
	{
		memset(z, 0xAB, 4);
		CHECK(p->GetSize(z) >= 4, "GetSize(Alloc(0)) < 4");
		p->Free(z, 0);
	}

	// 2) Small-size rounding: GetSize(live) must be >= the value we round to
	{
		size_t testSizes[] = { 1, 4, 5, 96, 97, 100, 512, 2048, 2049, 65536 };
		for (int i = 0; i < 10; ++i)
		{
			void *q = p->Alloc(testSizes[i]);
			CHECK(q != NULL, "Alloc returned NULL");
			if (q)
			{
				size_t exp = ExpectedRound(testSizes[i]);
				size_t got = p->GetSize(q);
				// GetSize returns the real usable size (may exceed the SBA round)
				char b[128];
				sprintf(b, "GetSize(%u) = %u < expect %u", (unsigned)testSizes[i], (unsigned)got, (unsigned)exp);
				CHECK(got >= exp, b);
				p->Free(q, 0);
			}
		}
	}

	// 3) Realloc(NULL, n) behaves like Alloc(n)
	{
		void *q = p->Realloc(NULL, 64);
		CHECK(q != NULL, "Realloc(NULL, 64) == NULL");
		if (q)
		{
			CHECK(p->GetSize(q) >= 64, "GetSize(Realloc(NULL,64)) < 64");
			p->Free(q, 0);
		}
	}

	// 4) Realloc(p, 0) must NOT free: block stays alive and writable
	{
		void *q = p->Alloc(64);
		CHECK(q != NULL, "setup Alloc(64) failed");
		if (q)
		{
			memset(q, 0x11, 64);
			void *r = p->Realloc(q, 0);
			CHECK(r != NULL, "Realloc(p, 0) freed the block (returned NULL)");
			if (r)
			{
				memset(r, 0x22, 4);		// must be writable after "0-size" realloc
				CHECK(p->GetSize(r) >= 4, "GetSize after Realloc(p,0) < 4");
				p->Free(r, 0);
			}
		}
	}

	// 5) Shrinking realloc preserves prefix
	{
		void *q = p->Alloc(1024);
		CHECK(q != NULL, "setup Alloc(1024) failed");
		if (q)
		{
			memset(q, 0x55, 1024);
			void *r = p->Realloc(q, 32);
			CHECK(r != NULL, "Realloc(p, 32) == NULL");
			if (r)
			{
				unsigned char *bytes = (unsigned char *)r;
				int prefixOk = 1;
				for (int i = 0; i < 32; ++i)
					if (bytes[i] != 0x55) { prefixOk = 0; break; }
				CHECK(prefixOk, "shrink realloc lost prefix bytes");
				CHECK(p->GetSize(r) >= 32, "GetSize after shrink < 32");
				p->Free(r, 0);
			}
		}
	}

	// 6) Free(NULL) is safe, API trivia matches original
	p->Free(NULL, 0);
	CHECK(p->GetVersion() == 0, "GetVersion() != 0 (original returns 0)");
	CHECK(p->IsDebugHeap() == false, "IsDebugHeap() != false");
	p->CompactHeap();

	// 7) GetSize equals the exact SBA round for every 1..96 and sampled 97..2048
	{
		for (size_t n = 1; n <= 96; ++n)
		{
			void *q = p->Alloc(n);
			if (!q) { printf("FAIL: Alloc(%u)\n", (unsigned)n); g_fails++; break; }
			size_t exp = (n + 3) & ~(size_t)3;
			size_t got = p->GetSize(q);
			if (got != exp)
			{
				printf("FAIL: GetSize(%u)=%u != %u\n", (unsigned)n, (unsigned)got, (unsigned)exp);
				g_fails++;
			}
			p->Free(q, 0);
		}
		for (size_t n = 97; n <= 2048; n += 29)
		{
			void *q = p->Alloc(n);
			if (!q) { printf("FAIL: Alloc(%u)\n", (unsigned)n); g_fails++; break; }
			size_t exp = (n + 7) & ~(size_t)7;
			size_t got = p->GetSize(q);
			if (got != exp)
			{
				printf("FAIL: GetSize(%u)=%u != %u\n", (unsigned)n, (unsigned)got, (unsigned)exp);
				g_fails++;
			}
			p->Free(q, 0);
		}
		// Alloc(0) normalizes to 1 -> bucket payload 4
		{
			void *z = p->Alloc(0);
			CHECK(z && p->GetSize(z) == 4, "GetSize(Alloc(0)) != 4");
			if (z) p->Free(z, 0);
		}
	}

	// 8) Cross-bucket Realloc 96 <-> 97 preserves the prefix and changes GetSize
	{
		unsigned char *q1 = (unsigned char *)p->Alloc(96);
		CHECK(q1 != NULL, "setup Alloc(96) failed");
		if (q1)
		{
			CHECK(p->GetSize(q1) == 96, "GetSize(Alloc(96)) != 96");
			memset(q1, 0xC1, 96);
			unsigned char *r1 = (unsigned char *)p->Realloc(q1, 97);
			CHECK(r1 != NULL, "Realloc(96->97) == NULL");
			if (r1)
			{
				CHECK(p->GetSize(r1) == 104, "GetSize(96->97) != 104");
				int ok = 1;
				for (int i = 0; i < 96; ++i)
					if (r1[i] != 0xC1) { ok = 0; break; }
				CHECK(ok, "realloc 96->97 lost prefix");
				p->Free(r1, 0);
			}
		}

		unsigned char *q2 = (unsigned char *)p->Alloc(97);
		CHECK(q2 != NULL, "setup Alloc(97) failed");
		if (q2)
		{
			CHECK(p->GetSize(q2) == 104, "GetSize(Alloc(97)) != 104");
			memset(q2, 0xC2, 97);
			unsigned char *r2 = (unsigned char *)p->Realloc(q2, 96);
			CHECK(r2 != NULL, "Realloc(97->96) == NULL");
			if (r2)
			{
				CHECK(p->GetSize(r2) == 96, "GetSize(97->96) != 96");
				int ok = 1;
				for (int i = 0; i < 96; ++i)
					if (r2[i] != 0xC2) { ok = 0; break; }
				CHECK(ok, "realloc 97->96 lost prefix");
				p->Free(r2, 0);
			}
		}
	}

	// 9) Boundary Realloc 2048 (SBA max pool) <-> 2049 (CRT heap)
	{
		unsigned char *q1 = (unsigned char *)p->Alloc(2048);
		CHECK(q1 != NULL, "setup Alloc(2048) failed");
		if (q1)
		{
			memset(q1, 0xD1, 2048);
			unsigned char *r1 = (unsigned char *)p->Realloc(q1, 2049);
			CHECK(r1 != NULL, "Realloc(2048->2049) == NULL");
			if (r1)
			{
				CHECK(p->GetSize(r1) >= 2049, "GetSize(2048->2049) < 2049");
				int ok = 1;
				for (int i = 0; i < 2048; ++i)
					if (r1[i] != 0xD1) { ok = 0; break; }
				CHECK(ok, "realloc 2048->2049 lost prefix");
				p->Free(r1, 0);
			}
		}

		unsigned char *q2 = (unsigned char *)p->Alloc(2049);
		CHECK(q2 != NULL, "setup Alloc(2049) failed");
		if (q2)
		{
			CHECK(p->GetSize(q2) >= 2049, "GetSize(Alloc(2049)) < 2049");
			memset(q2, 0xD2, 2049);
			unsigned char *r2 = (unsigned char *)p->Realloc(q2, 2048);
			CHECK(r2 != NULL, "Realloc(2049->2048) == NULL");
			if (r2)
			{
				CHECK(p->GetSize(r2) == 2048, "GetSize(2049->2048) != 2048");
				int ok = 1;
				for (int i = 0; i < 2048; ++i)
					if (r2[i] != 0xD2) { ok = 0; break; }
				CHECK(ok, "realloc 2049->2048 lost prefix");
				p->Free(r2, 0);
			}
		}
	}

	// 10) Mixed-size churn: alloc/free cycles exercise carving, recycling and
	//     the slab-destroy path; contents must survive across rounds untouched.
	{
		enum { N = 4096, ROUNDS = 50 };
		static const size_t mix[] = { 4, 5, 16, 17, 96, 97, 512, 2048, 2049 };
		unsigned char *blocks[N];
		size_t caps[N];

		for (int round = 0; round < ROUNDS; ++round)
		{
			for (int i = 0; i < N; ++i)
			{
				size_t sz = mix[i % 9];
				blocks[i] = (unsigned char *)p->Alloc(sz);
				CHECK(blocks[i] != NULL, "churn Alloc == NULL");
				if (blocks[i])
				{
					caps[i] = (sz <= 2048) ? ExpectedRound(sz) : sz;
					for (size_t k = 0; k < caps[i]; ++k)
						blocks[i][k] = (unsigned char)(i + (int)k);
				}
			}
			if (round % 10 == 9)
			{
				for (int i = 0; i < N; ++i)
				{
					if (blocks[i])
					{
						int ok = 1;
						for (size_t k = 0; k < caps[i]; ++k)
							if (blocks[i][k] != (unsigned char)(i + (int)k)) { ok = 0; break; }
						if (!ok)
						{
							printf("FAIL: churn contents at round %d, block %d\n", round, i);
							g_fails++;
						}
					}
				}
			}
			for (int i = N - 1; i >= 0; --i)
				if (blocks[i]) p->Free(blocks[i], 0);
		}
	}

	printf(g_fails ? "--- %d FAILURES ---\n" : "--- all ok ---\n", g_fails);
	return g_fails ? 1 : 0;
}