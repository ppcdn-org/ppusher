#include "pp_platform.h"

#ifdef _WIN32
#define _CRT_RAND_S
#include <stdlib.h>
#include <windows.h>

int64_t pp_now_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;
	if (freq.QuadPart == 0)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&c);
	return (int64_t)(c.QuadPart * 1000 / freq.QuadPart) + 1;
}

int64_t pp_wall_ms(void)
{
	FILETIME ft;
	ULARGE_INTEGER u;
	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

void pp_sleep_ms(int ms)
{
	Sleep((DWORD)(ms < 0 ? 0 : ms));
}

uint32_t pp_rand32(void)
{
	unsigned int v = 0;
	if (rand_s(&v) != 0)
		v = (unsigned int)GetTickCount();
	return v;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int64_t pp_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + 1;
}

int64_t pp_wall_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pp_sleep_ms(int ms)
{
	struct timespec ts;
	if (ms < 0)
		ms = 0;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

uint32_t pp_rand32(void)
{
	uint32_t v = 0;
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(&v, sizeof v, 1, f) != 1)
			v = (uint32_t)time(NULL) ^ (uint32_t)getpid();
		fclose(f);
	} else {
		v = (uint32_t)time(NULL) ^ (uint32_t)getpid();
	}
	return v;
}
#endif

void pp_rand_bytes(uint8_t *out, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint32_t v = pp_rand32();
		size_t k;
		for (k = 0; k < 4 && i < n; k++, i++)
			out[i] = (uint8_t)(v >> (8 * k));
	}
}
