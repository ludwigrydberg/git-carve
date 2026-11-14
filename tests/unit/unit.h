/* SPDX-License-Identifier: MIT */
/* A tiny unit test harness, included once per test program. */
#ifndef CARVE_UNIT_H
#define CARVE_UNIT_H

#include <stdio.h>
#include <string.h>

static int unit_failures;
static int unit_checks;

#define CHECK(cond)                                                            \
	do {                                                                   \
		unit_checks++;                                                 \
		if (!(cond)) {                                                 \
			unit_failures++;                                       \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
				__LINE__, #cond);                              \
		}                                                              \
	} while (0)

#define CHECK_INT(a, b)                                                       \
	do {                                                                  \
		long long _a = (long long)(a), _b = (long long)(b);           \
		unit_checks++;                                                \
		if (_a != _b) {                                               \
			unit_failures++;                                      \
			fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", \
				__FILE__, __LINE__, #a, _a, _b);              \
		}                                                             \
	} while (0)

#define CHECK_STR(a, b)                                                     \
	do {                                                                \
		const char *_a = (a), *_b = (b);                            \
		unit_checks++;                                              \
		if (!_a || !_b || strcmp(_a, _b)) {                         \
			unit_failures++;                                    \
			fprintf(stderr,                                     \
				"%s:%d: %s mismatch\n--- got:\n%s\n"        \
				"--- expected:\n%s\n---\n",                 \
				__FILE__, __LINE__, #a, _a ? _a : "(null)", \
				_b ? _b : "(null)");                        \
		}                                                           \
	} while (0)

#define RUN(fn)                                            \
	do {                                               \
		int _before = unit_failures;               \
		fn();                                      \
		if (unit_failures != _before)              \
			fprintf(stderr, "  in %s\n", #fn); \
	} while (0)

static inline int unit_done(const char *name)
{
	printf("%s: %d checks, %d failures\n", name, unit_checks,
	       unit_failures);
	return unit_failures ? 1 : 0;
}

#endif
