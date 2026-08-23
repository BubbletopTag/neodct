/* nd_vclock.c -- the clock everything reads, real or virtual, and the PRNG.
 *
 * This is the C answer to goldenframe._Frozen, which replaces nine attributes
 * of Python's `time` module for the duration of a capture. C cannot patch
 * libc, so instead every caller in the project asks these four functions and
 * they answer differently under capture. The production cost is one load of a
 * bool and a predictable branch.
 *
 * ============ WHY localtime IS gmtime UNDER CAPTURE ============
 *
 * The status bar draws strftime("%H:%M"), which reads the local zone. A
 * reference rendered in Dublin has to match one rendered in a CI container in
 * UTC, so _Frozen pins TZ AND aliases localtime onto gmtime. Both, not one:
 * TZ alone is not enough because a process that has already called tzset()
 * before we set the variable keeps its cached zone. Doing both means the
 * answer does not depend on who ran first.
 *
 * ============ THE PRNG IS NOT MERSENNE TWISTER ON PURPOSE ============
 *
 * OPEN-QUESTIONS.md question 4 settles it: matching CPython's MT19937 and its
 * exact consumption pattern through randint/shuffle is real work that buys one
 * thing, two reference frames that would otherwise be re-cut. They get re-cut.
 * What this has to be is deterministic and identical between C runs, which a
 * 64-bit LCG with a SplitMix64-seeded state is.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_types.h"
#include "nd_vclock.h"

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

static bool g_virtual;
static uint64_t g_frame;

/* What TZ was before nd_vclock_enable() touched it, so disabling puts the
 * process back the way it found it. Copied, because getenv's storage is not
 * ours to keep. */
static char g_saved_tz[128];
static bool g_had_tz;

static uint64_t g_rand_state;

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */

static double real_clock(clockid_t which)
{
    struct timespec ts;

    if (clock_gettime(which, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double nd_time_now(void)
{
    if (g_virtual)
        return ND_VCLOCK_EPOCH + (double)g_frame * ND_VCLOCK_TICK;
    return real_clock(CLOCK_REALTIME);
}

double nd_time_monotonic(void)
{
    /* One source under capture, exactly as _Frozen maps time.monotonic and
     * time.perf_counter onto clock.now(). CubeBench integrates its rotation
     * over perf_counter deltas, so if this were left real the cube would land
     * at a different angle on a faster machine. */
    if (g_virtual)
        return ND_VCLOCK_EPOCH + (double)g_frame * ND_VCLOCK_TICK;
    return real_clock(CLOCK_MONOTONIC);
}

/* Seconds as a time_t, floored, so a fractional virtual time lands in the
 * second it is inside rather than the one after. */
static time_t as_time_t(double t)
{
    if (t < 0.0)
        return (time_t)0;
    return (time_t)t;
}

void nd_time_gmtime(double t, struct tm *out)
{
    time_t secs = as_time_t(t);

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    (void)gmtime_r(&secs, out);
}

void nd_time_localtime(double t, struct tm *out)
{
    time_t secs = as_time_t(t);

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (g_virtual) {
        (void)gmtime_r(&secs, out);
        return;
    }
    (void)localtime_r(&secs, out);
}

/* ------------------------------------------------------------------ *
 * Capture mode
 * ------------------------------------------------------------------ */

void nd_vclock_enable(void)
{
    const char *tz;

    if (!g_virtual) {
        tz = getenv("TZ");
        g_had_tz = tz != NULL;
        if (g_had_tz)
            (void)nd_strlcpy(g_saved_tz, tz, sizeof g_saved_tz);
        else
            g_saved_tz[0] = '\0';
    }

    (void)setenv("TZ", "UTC", 1);
    tzset();

    g_virtual = true;
    g_frame = 0u;
    nd_rand_seed(ND_VCLOCK_SEED);
}

void nd_vclock_disable(void)
{
    if (!g_virtual)
        return;

    if (g_had_tz)
        (void)setenv("TZ", g_saved_tz, 1);
    else
        (void)unsetenv("TZ");
    tzset();

    g_virtual = false;
    g_frame = 0u;
}

bool nd_vclock_enabled(void)
{
    return g_virtual;
}

void nd_vclock_advance(void)
{
    if (g_virtual)
        g_frame++;
}

uint64_t nd_vclock_frame(void)
{
    return g_frame;
}

/* ------------------------------------------------------------------ *
 * The PRNG
 * ------------------------------------------------------------------ */

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15u);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

void nd_rand_seed(uint32_t seed)
{
    uint64_t s = (uint64_t)seed;

    /* Through SplitMix64 first: a small seed like 20240101 left in a raw LCG
     * state produces a visibly patterned first few outputs, and the games
     * place their first food from exactly those. */
    g_rand_state = splitmix64(&s);
}

uint32_t nd_rand_u32(void)
{
    /* Knuth's LCG constants. The low bits of an LCG are weak, so the result
     * is the top 32 of the 64-bit state and never the bottom. */
    g_rand_state = g_rand_state * 6364136223846793005u + 1442695040888963407u;
    return (uint32_t)(g_rand_state >> 32);
}

int32_t nd_rand_below(int32_t n)
{
    uint32_t bound;
    uint32_t threshold;

    if (n <= 0)
        return 0;

    bound = (uint32_t)n;

    /* 2**32 % bound, computed without a 64-bit divide. Values below this are
     * the ones that would make the modulo uneven, so they are redrawn. */
    threshold = (0u - bound) % bound;

    for (;;) {
        uint32_t r = nd_rand_u32();
        if (r >= threshold)
            return (int32_t)(r % bound);
    }
}

int32_t nd_rand_range(int32_t lo, int32_t hi)
{
    int64_t span;

    if (hi <= lo)
        return lo;

    /* In 64 bits because hi - lo overflows int32 for a full-range request,
     * and randint(INT32_MIN, INT32_MAX) is a legal thing to ask for. */
    span = (int64_t)hi - (int64_t)lo + 1;
    if (span > (int64_t)INT32_MAX)
        return lo + (int32_t)(nd_rand_u32() % (uint32_t)span);

    return lo + nd_rand_below((int32_t)span);
}

double nd_rand_double(void)
{
    /* 53 bits, the same width Python's random() has, assembled from two
     * draws so the mantissa is fully populated. */
    uint32_t a = nd_rand_u32() >> 5; /* 27 bits */
    uint32_t b = nd_rand_u32() >> 6; /* 26 bits */

    return ((double)a * 67108864.0 + (double)b) / 9007199254740992.0;
}

void nd_rand_shuffle(void *base, size_t n, size_t elem_sz)
{
    uint8_t *p = base;
    size_t i;

    if (p == NULL || elem_sz == 0u || n < 2u)
        return;
    /* nd_rand_below takes an int32_t, and nothing in this project shuffles
     * two billion elements. Refusing beats silently shuffling a prefix. */
    if (n > (size_t)INT32_MAX)
        return;

    /* Top down, matching random.shuffle's direction. Swapped byte at a time:
     * a scratch buffer would either be a VLA or an allocation, and both are
     * banned in this codebase for something called from a game loop. */
    for (i = n - 1u; i > 0u; i--) {
        size_t j = (size_t)nd_rand_below((int32_t)(i + 1u));
        uint8_t *a = p + i * elem_sz;
        uint8_t *b = p + j * elem_sz;
        size_t k;

        if (a == b)
            continue;
        for (k = 0u; k < elem_sz; k++) {
            uint8_t t = a[k];
            a[k] = b[k];
            b[k] = t;
        }
    }
}
