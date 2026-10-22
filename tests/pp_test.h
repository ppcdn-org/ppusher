#ifndef PP_TEST_H
#define PP_TEST_H

#include <stdio.h>
#include <string.h>

static int pp_t_checks, pp_t_fails;

#define CHECK(c)                                                                         \
	do {                                                                             \
		pp_t_checks++;                                                           \
		if (!(c)) {                                                              \
			pp_t_fails++;                                                    \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
		}                                                                        \
	} while (0)

#define CHECK_INT(a, b)                                                                  \
	do {                                                                             \
		long long _a = (long long)(a), _b = (long long)(b);                      \
		pp_t_checks++;                                                           \
		if (_a != _b) {                                                          \
			pp_t_fails++;                                                    \
			printf("  FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__, \
			       __LINE__, #a, #b, _a, _b);                                \
		}                                                                        \
	} while (0)

#define CHECK_STR(a, b)                                                                  \
	do {                                                                             \
		const char *_a = (a), *_b = (b);                                         \
		pp_t_checks++;                                                           \
		if (!_a || !_b || strcmp(_a, _b) != 0) {                                 \
			pp_t_fails++;                                                    \
			printf("  FAIL %s:%d: %s == %s (got \"%s\", want \"%s\")\n",     \
			       __FILE__, __LINE__, #a, #b, _a ? _a : "(null)",           \
			       _b ? _b : "(null)");                                      \
		}                                                                        \
	} while (0)

#define RUN(fn)                                                                          \
	do {                                                                             \
		int _before = pp_t_fails;                                                \
		fn();                                                                    \
		printf("%s %s\n", pp_t_fails == _before ? "[ ok ]" : "[FAIL]", #fn);     \
	} while (0)

#define TEST_DONE()                                                                      \
	do {                                                                             \
		printf("%d checks, %d failures\n", pp_t_checks, pp_t_fails);             \
		return pp_t_fails ? 1 : 0;                                               \
	} while (0)

#endif
