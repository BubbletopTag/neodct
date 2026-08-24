/*
 * Minimal assert harness for NeoDCT chrome unit tests.
 * No dependencies; each test binary exits non-zero on any failure.
 */

#ifndef NEODCT_TEST_UTIL_H
#define NEODCT_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int test_failures = 0;
static int test_checks = 0;

#define CHECK(cond) do {						\
		test_checks++;						\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s\n",			\
			       __FILE__, __LINE__, #cond);		\
			test_failures++;				\
		}							\
	} while (0)

#define CHECK_STR(got, want) do {					\
		test_checks++;						\
		const char *tu_g = (got);				\
		const char *tu_w = (want);				\
		if (tu_g == NULL || strcmp(tu_g, tu_w) != 0) {		\
			printf("FAIL %s:%d: got \"%s\" want \"%s\"\n",	\
			       __FILE__, __LINE__,			\
			       tu_g ? tu_g : "(null)", tu_w);		\
			test_failures++;				\
		}							\
	} while (0)

#define CHECK_INT(got, want) do {					\
		test_checks++;						\
		long tu_gi = (long)(got);				\
		long tu_wi = (long)(want);				\
		if (tu_gi != tu_wi) {					\
			printf("FAIL %s:%d: got %ld want %ld\n",	\
			       __FILE__, __LINE__, tu_gi, tu_wi);	\
			test_failures++;				\
		}							\
	} while (0)

#define TEST_EXIT() do {						\
		printf("%s: %d checks, %d failures\n",			\
		       __FILE__, test_checks, test_failures);		\
		return test_failures > 0 ? 1 : 0;			\
	} while (0)

#endif
