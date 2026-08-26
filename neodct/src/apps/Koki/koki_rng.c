/* koki_rng.c -- CPython's Mersenne Twister, bit for bit.
 *
 * engine.py holds `self.random = random.Random()` -- a PRIVATE generator, not
 * the `random` module -- and every `R(a, b)` in game.py draws from it. The
 * port reproduces CPython's algorithm rather than substituting the project's
 * nd_rand_*, and the reason is narrow but real: nd_vclock.h's PRNG note
 * ("do NOT reimplement CPython's MT19937") is about Snake's food placement
 * and Memory's shuffle, whose two reference frames are allowed to be re-cut
 * from the C build. Koki's are not -- app-koki is a stored reference that
 * must match -- and more to the point, a bit-identical generator is what
 * makes a C run and a Python run of the SAME LEVEL comparable frame by frame,
 * which is how the rest of this port was checked. That oracle is worth 60
 * lines.
 *
 * Three pieces, all CPython's:
 *
 *   init_by_array   _randommodule.c random_seed() splits abs(n) into 32-bit
 *                   little-endian words and seeds with them. For n < 2^32
 *                   that is a one-word key, which is what the spec's test
 *                   vectors were taken with.
 *   getrandbits(k)  k <= 32 is genrand_uint32() >> (32 - k). k == 0 is 0.
 *   _randbelow(n)   k = bit_length(n); retry getrandbits(k) until r < n.
 *
 * randint(a, b) is then a + _randbelow(b - a + 1), with a and b order-
 * normalised first, exactly as Engine.randint does.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_types.h"
#include "nd_vclock.h"

#include "koki.h"

#define MT_N          624
#define MT_M          397
#define MT_MATRIX_A   0x9908b0dfu
#define MT_UPPER_MASK 0x80000000u
#define MT_LOWER_MASK 0x7fffffffu

static void init_genrand(koki_rng *r, uint32_t s)
{
    int32_t i;

    r->mt[0] = s;
    for (i = 1; i < MT_N; i++) {
        uint32_t prev = r->mt[i - 1];

        r->mt[i] = 1812433253u * (prev ^ (prev >> 30)) + (uint32_t)i;
    }
    r->index = MT_N;
}

static void init_by_array(koki_rng *r, const uint32_t *key, size_t key_len)
{
    size_t i = 1u;
    size_t j = 0u;
    size_t k;

    init_genrand(r, 19650218u);
    k = (MT_N > key_len) ? (size_t)MT_N : key_len;
    for (; k > 0u; k--) {
        uint32_t prev = r->mt[i - 1u];

        r->mt[i] = (r->mt[i] ^ ((prev ^ (prev >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
        i++;
        j++;
        if (i >= (size_t)MT_N) {
            r->mt[0] = r->mt[MT_N - 1];
            i = 1u;
        }
        if (j >= key_len)
            j = 0u;
    }
    for (k = (size_t)MT_N - 1u; k > 0u; k--) {
        uint32_t prev = r->mt[i - 1u];

        r->mt[i] = (r->mt[i] ^ ((prev ^ (prev >> 30)) * 1566083941u)) - (uint32_t)i;
        i++;
        if (i >= (size_t)MT_N) {
            r->mt[0] = r->mt[MT_N - 1];
            i = 1u;
        }
    }
    /* MSB is 1, assuring a non-zero initial array. */
    r->mt[0] = MT_UPPER_MASK;
    r->index = MT_N;
}

void koki_rng_seed(koki_rng *r, uint32_t seed)
{
    uint32_t key[1];

    if (r == NULL)
        return;
    /* random_seed(): keyused is 1 for any n that fits in 32 bits, including
     * 0 -- bit_length 0 still uses one word. */
    key[0] = seed;
    init_by_array(r, key, 1u);
}

/* random.Random() with no argument seeds from os.urandom(32). Eight words is
 * the same shape; a short read falls back to the clock, which is worse
 * randomness and still perfectly good for deciding which way a cannonball
 * flies. */
static void seed_from_entropy(koki_rng *r)
{
    uint32_t key[8];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    bool ok = false;

    if (fd >= 0) {
        ssize_t n = read(fd, key, sizeof key);

        ok = (n == (ssize_t)sizeof key);
        (void)close(fd);
    }
    if (!ok) {
        size_t i;
        uint32_t t = (uint32_t)(nd_time_now() * 1000.0);

        for (i = 0u; i < ND_ARRAY_LEN(key); i++)
            key[i] = t + (uint32_t)i * 2654435761u;
    }
    init_by_array(r, key, ND_ARRAY_LEN(key));
}

void koki_rng_init(koki_rng *r)
{
    if (r == NULL)
        return;
    memset(r, 0, sizeof *r);
    seed_from_entropy(r);
}

uint32_t koki_rng_u32(koki_rng *r)
{
    uint32_t y;

    if (r == NULL)
        return 0u;
    if (r->index >= MT_N) {
        int32_t i;

        for (i = 0; i < MT_N; i++) {
            uint32_t x = (r->mt[i] & MT_UPPER_MASK) | (r->mt[(i + 1) % MT_N] & MT_LOWER_MASK);

            r->mt[i] = r->mt[(i + MT_M) % MT_N] ^ (x >> 1);
            if ((x & 1u) != 0u)
                r->mt[i] ^= MT_MATRIX_A;
        }
        r->index = 0;
    }

    y = r->mt[r->index++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
}

uint32_t koki_rng_getrandbits(koki_rng *r, int32_t k)
{
    if (k <= 0)
        return 0u;
    if (k >= 32)
        return koki_rng_u32(r);
    return koki_rng_u32(r) >> (32 - k);
}

uint32_t koki_rng_below(koki_rng *r, uint32_t n)
{
    int32_t k = 0;
    uint32_t v = n;
    uint32_t got;

    if (n == 0u)
        return 0u;
    while (v != 0u) { /* int.bit_length() */
        k++;
        v >>= 1;
    }
    do {
        got = koki_rng_getrandbits(r, k);
    } while (got >= n);
    return got;
}

int32_t koki_randint(koki_engine *eng, int32_t a, int32_t b)
{
    int32_t lo = (a <= b) ? a : b;
    int32_t hi = (a <= b) ? b : a;
    uint32_t width;

    if (eng == NULL)
        return lo;
    /* hi - lo cannot overflow int32 for any value game.py passes (the widest
     * is randint(-240, 240)), but the subtraction is done in uint32 anyway so
     * that a future caller cannot make it undefined. */
    width = (uint32_t)hi - (uint32_t)lo + 1u;
    return (int32_t)((uint32_t)lo + koki_rng_below(&eng->rng, width));
}
