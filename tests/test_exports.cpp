// tier0 -- export manifest harness.
// Cross-checks every export in tier0.def (314 names, ordinals 1..314):
//   * loaded DLL exports the expected total (314) with strictly sequential
//     ordinals 1..N,
//   * every named export resolves via GetProcAddress by name AND by ordinal,
//     and both resolve to the same address (name-export must map to its own
//     ordinal, including DATA exports and alias "name=target" entries),
//   * a safe free-function subset (Plat_FloatTime / Plat_MSTime) is actually
//     callable through the name-keyed path.
//
// Build (x86 MSVC prompt, repo root):
//   cl /O1 /GS- /nologo tests\test_exports.cpp /Fetests\test_exports.exe /link /SUBSYSTEM:CONSOLE
// Run with build\tier0.dll placed next to test_exports.exe, CWD = repo root:
//   copy build\tier0.dll tests\
//   tests\test_exports.exe

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;
static int g_exports = 0;
static int g_dataExports = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { printf("FAIL: %s\n", msg); g_fails++; } \
	} while (0)

// Ordinals live after the LAST '@' (mangled names contain '@' themselves).
static int ParseOrdinal(const char *line)
{
	const char *at = strrchr(line, '@');
	if (!at)
		return 0;
	return atoi(at + 1);
}

static void ParseName(const char *line, char *name, size_t nameLen)
{
	const char *p = line;
	while (*p == ' ' || *p == '\t')
		++p;
	size_t n = 0;
	while (*p && *p != ' ' && *p != '\t' && n + 1 < nameLen)
		name[n++] = *p++;
	name[n] = 0;
	char *eq = strchr(name, '=');	// alias "name=target": export 'name'
	if (eq)
		*eq = 0;
}

static int HasDataKeyword(const char *line)
{
	const char *at = strrchr(line, '@');
	if (!at)
		return 0;
	return strstr(at, "DATA") != NULL;
}

int main()
{
	HMODULE h = LoadLibraryA("tier0.dll");
	if (!h)
	{
		printf("FAIL: cannot load tier0.dll (place build\\tier0.dll next to exe)\n");
		return 1;
	}

	FILE *f = fopen("tier0.def", "r");
	if (!f)
	{
		printf("FAIL: cannot open tier0.def (run from repo root)\n");
		return 1;
	}

	char line[512];
	int expectOrd = 1;

	while (fgets(line, sizeof(line), f))
	{
		if (!strchr(line, '@'))
			continue;			// LIBRARY/EXPORTS header lines

		int ord = ParseOrdinal(line);
		if (ord <= 0)
		{
			printf("FAIL: cannot parse ordinal: %s", line);
			g_fails++;
			continue;
		}

		if (ord != expectOrd)
		{
			printf("FAIL: def line %d: ordinal %d, expected %d\n", expectOrd, ord, expectOrd);
			g_fails++;
		}
		expectOrd = ord + 1;

		char name[256];
		ParseName(line, name, sizeof(name));
		int isData = HasDataKeyword(line);
		g_exports++;
		if (isData)
			g_dataExports++;

		if (!name[0])
		{
			printf("FAIL: ord %d has no exported name\n", ord);
			g_fails++;
			continue;
		}

		void *byName = (void *)GetProcAddress(h, name);
		void *byOrd = (void *)GetProcAddress(h, (LPCSTR)(UINT_PTR)ord);
		if (!byName)
		{
			printf("FAIL: ord %d name not exported: %s\n", ord, name);
			g_fails++;
			continue;
		}
		if (!byOrd)
		{
			printf("FAIL: ord %d not reachable by ordinal\n", ord);
			g_fails++;
			continue;
		}
		if (byName != byOrd)
		{
			printf("FAIL: ord %d name/ordinal resolve to different addresses\n", ord);
			g_fails++;
			continue;
		}
	}

	fclose(f);

	typedef double (__cdecl *FloatTimeFn)();
	FloatTimeFn plt = (FloatTimeFn)GetProcAddress(h, "Plat_FloatTime");
	if (plt)
	{
		// first call latches the time base (returns ~0, like the original),
		// so probe once, then require a monotonic positive sequence
		double a = plt();
		double b = plt();
		double c = plt();
		CHECK(b >= a && c >= b && c > 0.0, "Plat_FloatTime not finite/monotonic");
	}
	else
	{
		printf("FAIL: Plat_FloatTime export missing (safe-call subset)\n");
		g_fails++;
	}

	typedef unsigned int (__cdecl *MSTimeFn)();
	MSTimeFn mst = (MSTimeFn)GetProcAddress(h, "Plat_MSTime");
	if (mst)
		CHECK(mst() < 0x7FFFFFFFu, "Plat_MSTime implausible");
	else
	{
		printf("FAIL: Plat_MSTime export missing (safe-call subset)\n");
		g_fails++;
	}

	printf("export manifest: %d total (%d DATA), ordinals 1..%d\n",
		g_exports, g_dataExports, g_exports);

	// v1.8 314 base (313 original + CreateInterface) + 2 SBArena filesystem arena helpers
	if (g_exports != 314 && g_exports != 316)
	{
		printf("FAIL: expected 314 or 316 exports, got %d\n", g_exports);
		g_fails++;
	}

	printf(g_fails ? "--- %d FAILURES ---\n" : "--- all ok ---\n", g_fails);
	return g_fails ? 1 : 0;
}