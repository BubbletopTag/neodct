/* nd_cpufreq.h -- what speed the CPU is allowed to run at, and how to change
 * it from an app.
 *
 * The engineering app Sleepy is the only consumer today. The decisions live
 * here rather than in the app for the reason nd_mic.h and nd_remoteshell.h
 * both give: what a test can reach is what gets tested, and an app's .so is
 * the awkward half.
 *
 * ============ WHY THIS EXISTS AT ALL ============
 *
 * NeoDCT's "sleep" is not a kernel suspend and is not going to be one. The
 * RV1103 has no suspend path that keeps the modem alive, and a feature phone
 * that stops answering calls when the screen goes off is not a feature phone.
 * So sleep here means: drop the CPU to its lowest operating point, blank the
 * panel, stop everything that can be stopped, and keep polling i2c and the
 * modem. Two of those four are hardware pokes nobody had written yet -- the
 * downclock and the blank -- and this header is the first of them.
 *
 * ============ SETTING A FREQUENCY MEANS PINNING THE RANGE ============
 *
 * There is no "run at exactly this" file that works on a stock kernel.
 * scaling_setspeed exists only under the userspace governor, which the SDK
 * kernel does not build, so the portable way to hold a frequency is to write
 * the SAME value to scaling_min_freq and scaling_max_freq: the governor is
 * then free to choose, and has one choice.
 *
 * That has a consequence worth stating out loud rather than discovering:
 * pinning is sticky. Nothing puts the range back when the app exits. That is
 * deliberate -- an engineering app whose effect vanished the moment you left
 * it could not be used to measure anything -- but it means the phone stays
 * where it was left until something sets it again.
 *
 * ============ AND THE WRITE ORDER IS NOT ARBITRARY ============
 *
 * min and max are clamped against each other, so writing them in the wrong
 * order writes a value that is silently thrown away:
 *
 *   raising  (816 -> 1200):  min stays 816 until max moves, so MAX FIRST.
 *   lowering (1200 -> 408):  max cannot go under min, so MIN FIRST.
 *
 * Get it backwards and half the write is dropped, leaving the CPU pinned to
 * the OLD frequency with sysfs reporting the new one on one of the two files.
 * nd_cpufreq_max_first() is that rule, on its own, where a test can check it.
 */

#ifndef ND_CPUFREQ_H_INCLUDED
#define ND_CPUFREQ_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cpu0, not policy0. Both exist on this kernel and cpu0/cpufreq is a symlink
 * to whichever policy owns the core -- which is the one that stays right on a
 * chip that grows a second cluster. The RV1103 is single-core, so today they
 * are the same directory. */
#define ND_CPUFREQ_DIR "/sys/devices/system/cpu/cpu0/cpufreq"

#define ND_CPUFREQ_AVAILABLE ND_CPUFREQ_DIR "/scaling_available_frequencies"
#define ND_CPUFREQ_CUR       ND_CPUFREQ_DIR "/scaling_cur_freq"
#define ND_CPUFREQ_MIN       ND_CPUFREQ_DIR "/scaling_min_freq"
#define ND_CPUFREQ_MAX       ND_CPUFREQ_DIR "/scaling_max_freq"
#define ND_CPUFREQ_GOVERNOR  ND_CPUFREQ_DIR "/scaling_governor"

/* The RV1103 OPP table is five entries (408, 600, 816, 1008, 1200 MHz). 16 is
 * slack for a chip with a denser table, and a fixed cap because
 * CODING-STANDARDS.md section 4 puts nothing sized by input on the stack. */
#define ND_CPUFREQ_MAX_STEPS 16

/* "schedutil" is 9, "conservative" is 12. */
#define ND_CPUFREQ_GOV_MAX 32

/* Frequencies are kHz throughout, because that is the unit every file in the
 * cpufreq directory is written in. Converting on the way in would mean
 * converting back on the way out and rounding twice. */
typedef struct {
    int32_t khz[ND_CPUFREQ_MAX_STEPS];
    size_t n;
} nd_cpufreq_table;

typedef struct {
    int32_t cur_khz; /* -1 when the kernel will not say */
    int32_t min_khz; /* -1 when unreadable */
    int32_t max_khz; /* -1 when unreadable */
    char governor[ND_CPUFREQ_GOV_MAX];
} nd_cpufreq_state;

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

/* The operating points the kernel is offering, ASCENDING.
 *
 * scaling_available_frequencies is whitespace-separated kHz, and it is NOT
 * sorted: the rockchip driver emits it in OPP-table order, which on this chip
 * happens to be ascending and on others is not. Sorting here means the menu
 * is in a sensible order without the app knowing why.
 *
 * Returns ND_ERR_NOTFOUND when the file is absent, which is what a kernel
 * built without CONFIG_CPU_FREQ looks like from here -- QEMU, every time.
 * That is a fact to report, not a failure to hide. */
nd_err nd_cpufreq_read_table(nd_cpufreq_table *out);

/* Current frequency, the pinned range and the governor's name. Unreadable
 * fields come back as -1 and "" rather than failing the whole read: a kernel
 * that publishes scaling_cur_freq but not scaling_governor is still worth
 * showing. Fails only when the cpufreq directory is not there at all. */
nd_err nd_cpufreq_read_state(nd_cpufreq_state *out);

/* ------------------------------------------------------------------ *
 * Writing
 * ------------------------------------------------------------------ */

/* Pin the CPU to `khz` by writing it to both ends of the range, in the order
 * nd_cpufreq_max_first() gives. Returns ND_ERR_IO when either write fails,
 * which on a phone means the process is not root or the kernel has no
 * cpufreq -- both of which the caller should say out loud rather than
 * retry. */
nd_err nd_cpufreq_set(int32_t khz);

/* True when scaling_max_freq must be written BEFORE scaling_min_freq. See the
 * block at the top: the answer is "when the target is above the current max",
 * and it is a function rather than an inline comparison because getting it
 * wrong is invisible -- the write succeeds and the frequency does not
 * change. */
bool nd_cpufreq_max_first(int32_t target_khz, int32_t current_max_khz);

/* ------------------------------------------------------------------ *
 * Pieces, exposed because the unit test checks each one separately
 * ------------------------------------------------------------------ */

/* Whitespace-separated kHz into `out`, ascending, duplicates dropped.
 * Returns how many were written. Stops at `max` rather than overflowing, and
 * ignores any token that is not a positive integer -- the file has a trailing
 * newline and some drivers pad it with spaces. */
size_t nd_cpufreq_parse_table(const char *text, int32_t *out, size_t max);

/* kHz as a person reads it: "408 MHz", "1.20 GHz". Two decimals above a
 * gigahertz and none below, which is what fits the menu row and what the
 * datasheet uses. Always NUL-terminates. */
void nd_cpufreq_format(char *out, size_t out_sz, int32_t khz);

#ifdef __cplusplus
}
#endif

#endif /* ND_CPUFREQ_H_INCLUDED */
