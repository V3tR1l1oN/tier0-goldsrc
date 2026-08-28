// tier0 -- bare-CRT probe with WRITE: carve+keep N 4096-byte blocks, write one
// byte into each (forcing page commit like the SBA descriptor write), read one
// byte of each, then free all. Times the pattern the SBA uses for 2048 slabs.
//
// Build: cl /O1 /GS- /nologo tests\probe_malloc.cpp /Fetests\probe_malloc.exe /link /SUBSYSTEM:CONSOLE

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

static double now_msec(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
	unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 80000;
	double t0 = now_msec();

	void **p = (void **)malloc(sizeof(void *) * n);
	for (unsigned i = 0; i < n; ++i)
		p[i] = malloc(4096);
	printf("alloc %u 4096-blocks      : %.1fms\n", n, now_msec() - t0);

	double tm = now_msec();
	for (unsigned i = 0; i < n; ++i)
		*(volatile unsigned char *)p[i] = 1;
	printf("write 1 byte into each    : %.1fms\n", now_msec() - tm);

	tm = now_msec();
	unsigned long long sum = 0;
	for (unsigned i = 0; i < n; ++i)
		sum += *(volatile unsigned char *)p[i];
	printf("read 1 byte each          : %.1fms (sum=%llu)\n", now_msec() - tm, sum);

	tm = now_msec();
	for (unsigned i = 0; i < n; ++i)
		free(p[i]);
	printf("free all                  : %.1fms\n", now_msec() - tm);
	free(p);
	printf("total                     : %.1fms\n", now_msec() - t0);
	return 0;
}