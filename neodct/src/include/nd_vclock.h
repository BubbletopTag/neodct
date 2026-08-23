/* nd_vclock.h -- the one clock the system asks the time of, and the pinned
 * PRNG that sits beside it.
 *
 * ADDITION to the frozen header set. Nothing in include/ declared a "what
 * time is it" entry point, and the golden-frame oracle cannot exist without
 * one: goldenframe.py replaces nine attributes of Python's `time` module so
 * that a captured frame is the same bytes on every machine. C has no monkey
 * patching, so the substitution has to be a function everyone already calls.
 *
 * Call nd_time_now() instead of time(), nd_time_monotonic() instead of
 * clock_gettime(CLOCK_MONOTONIC), and nd_time_localtime() instead of
 * localtime_r(). In production every one of them is a direct forward and
 * costs a branch on a cached bool. Under capture they all read the virtual
 * clock, and the frames come out identical in Dublin and in CI.
 *
 * ============ WHY THE CLOCK IS VIRTUAL AND NOT FROZEN ============
 *
 * goldenframe.py's docstring makes the argument and it is worth keeping:
 * freezing time outright deadlocks everything that waits for time to pass --
 * the +CLIP grace period in poll_modem, the cursor blink, the modem retry
 * backoff. So the clock starts at a fixed epoch and advances exactly one UI
 * tick each time a frame is committed. That is deterministic, monotonic, and
 * frame-aligned all at once: a screen composed of several draw calls cannot
 * tear across a tick boundary, because within one frame time does not move.
 *
 * ============ THE CONSTANTS ARE FIXED FOREVER ============
 *
 * Changing any of the three invalidates all 49 stored reference frames. They
 * are goldenframe.EPOCH, goldenframe.TICK and goldenframe.SEED, verbatim.
 */

#ifndef ND_VCLOCK_H_INCLUDED
#define ND_VCLOCK_H_INCLUDED

#include <time.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2024-01-01 12:34:56 UTC. Arbitrary, but fixed forever. */
#define ND_VCLOCK_EPOCH 1704112496.0

/* One UI tick, matching the 0.1 s read_keypress timeout in the main loop. */
#define ND_VCLOCK_TICK 0.1

/* random.seed() value. */
#define ND_VCLOCK_SEED 20240101u

/* ------------------------------------------------------------------ *
 * Asking the time
 * ------------------------------------------------------------------ */

/* Seconds since the Unix epoch -- Python's time.time(). */
double nd_time_now(void);

/* A monotonic reading for measuring intervals -- Python's time.monotonic()
 * and time.perf_counter(), which goldenframe.py maps onto the same source.
 * Under capture this is nd_time_now(); it is NOT comparable with the
 * production reading, so never persist one. */
double nd_time_monotonic(void);

/* gmtime_r() on a double. */
void nd_time_gmtime(double t, struct tm *out);

/* localtime_r() on a double -- EXCEPT under capture, where it is aliased to
 * gmtime exactly as _Frozen does, because the status bar draws
 * strftime("%H:%M") and a reference rendered in one zone has to match one
 * rendered in another. */
void nd_time_localtime(double t, struct tm *out);

/* ------------------------------------------------------------------ *
 * Capture mode
 * ------------------------------------------------------------------ */

/* Pin the clock: virtual time from ND_VCLOCK_EPOCH, frame counter zeroed,
 * TZ forced to UTC (setenv + tzset, both, as _Frozen does), PRNG seeded with
 * ND_VCLOCK_SEED. Idempotent -- calling it again restarts from frame 0. */
void nd_vclock_enable(void);

/* Back to the real clock. TZ is restored to whatever it was. */
void nd_vclock_disable(void);

bool nd_vclock_enabled(void);

/* One tick. The frame-commit path calls this AFTER the frame is recorded,
 * which is where goldenframe.instrument() puts it -- a frame that was refused
 * (budget exhausted) must not advance time. No effect when disabled. */
void nd_vclock_advance(void);

/* Frames committed since nd_vclock_enable(). now == EPOCH + frame * TICK. */
uint64_t nd_vclock_frame(void);

/* ------------------------------------------------------------------ *
 * The pinned PRNG
 * ------------------------------------------------------------------ */

/* OPEN-QUESTIONS.md question 4, answered: do NOT reimplement CPython's
 * MT19937. Snake's food placement and Memory's shuffle are the only randomness
 * that reaches a pixel, and those two reference frames get re-cut from the C
 * build. What matters is that the sequence is identical from one C run to the
 * next, which this is: a 64-bit LCG, seeded through SplitMix64 so that a
 * small seed still produces a well-mixed state.
 *
 * One global stream, because that is what Python's module-level `random` is
 * and the games use it the same way. */
void nd_rand_seed(uint32_t seed);
uint32_t nd_rand_u32(void);

/* [0, n) -- rejection-sampled, so the distribution is flat rather than
 * modulo-biased. Returns 0 for n <= 0. */
int32_t nd_rand_below(int32_t n);

/* random.randint(lo, hi) -- INCLUSIVE of both ends. Returns lo when hi < lo. */
int32_t nd_rand_range(int32_t lo, int32_t hi);

/* [0.0, 1.0). */
double nd_rand_double(void);

/* random.shuffle() -- Fisher-Yates from the top down, in place, no scratch
 * allocation and no VLA. */
void nd_rand_shuffle(void *base, size_t n, size_t elem_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_VCLOCK_H_INCLUDED */
