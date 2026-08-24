/* nd_bench.c -- see nd_bench.h. */

#include "nd_bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_on = -1; /* -1 = not yet asked */
static double g_last;

static double now_ms(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0.0;
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

bool nd_bench_on(void)
{
    if (g_on < 0) {
        const char *v = getenv("NEODCT_BENCH");

        g_on = (v != NULL && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0')) ? 1 : 0;
        if (g_on == 1)
            g_last = now_ms();
    }
    return g_on == 1;
}

void nd_bench_mark(const char *label)
{
    double n;

    if (!nd_bench_on())
        return;
    n = now_ms();
    (void)fprintf(stderr, "[BENCH] %28s %9.3f ms\n", label != NULL ? label : "?", n - g_last);
    (void)fflush(stderr);
    /* Read the clock again: the fprintf and the flush are the measurement's
     * own cost, and on a serial console at 115200 baud that is not small. */
    g_last = now_ms();
}
