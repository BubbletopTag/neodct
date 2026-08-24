/* nd_bench.h -- "where did the milliseconds go", answerable on the phone.
 *
 * Twice now a startup cost has been chased by hand-patching fprintf()s into
 * nd_ui.c, rebuilding, measuring, and reverting -- and each time the answer
 * on the host disagreed with the answer on the target, because the target
 * reads from a compressed squashfs through an emulated CPU and the host
 * reads from page cache. The host measurement was not wrong, it was
 * answering a different question.
 *
 * So the marks live in the source. They cost one cached getenv() and nothing
 * else when NEODCT_BENCH is unset, which is every boot the owner will ever
 * see; with it set they go to stderr, which is where a phone's serial
 * console already is:
 *
 *     NEODCT_BENCH=1 /NeoDCT/System/bin/nd-apprun /NeoDCT/System/apps/Clock run
 *     [BENCH]                 dlopen    41.207 ms
 *     [BENCH]        ui_init_app:db      2.113 ms
 *     ...
 *
 * Each line is the time since the PREVIOUS mark, so a run reads as a
 * breakdown rather than as a set of timestamps to subtract by hand. The
 * first mark in a process measures from the first nd_bench call.
 *
 * Deliberately not routed through nd_log(): that is the colourful serial log
 * whose bytes are pinned against logref.json, and a debugging aid has no
 * business appearing in an oracle.
 */

#ifndef ND_BENCH_H_INCLUDED
#define ND_BENCH_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when NEODCT_BENCH is set to something other than "" or "0". Cached
 * after the first call, so a mark on a hot path is a load and a branch. */
bool nd_bench_on(void);

/* Print "<label> <ms since the previous mark>" when enabled; otherwise do
 * nothing at all -- not even read the clock. */
void nd_bench_mark(const char *label);

#ifdef __cplusplus
}
#endif

#endif /* ND_BENCH_H_INCLUDED */
