/* nd_bootbar.h -- the install-progress screen the initramfs draws while
 * ndsys-apply.sh writes the system partition.
 *
 * Split from main() so the layout, the arithmetic and the copy loop can be
 * unit tested on the host (test/unit/test_bootbar.c) without a framebuffer,
 * a pipeline or an argv.
 *
 * ============ THE LAYOUT IS nd_progress'S LAYOUT, HARD-CODED ============
 *
 * lib/nd_progress.c DERIVES its five boxes from nd_ui_width() and
 * nd_ui_content_bottom(). There is no nd_ui here -- no FreeType, no wallpaper,
 * no libneodct at all -- so the numbers are written down instead.
 *
 * Hard-coding is only acceptable with a check, and there is one:
 * test_bootbar.c builds a real 240x175 nd_ui, calls nd_progress_init(), and
 * asserts the five boxes and the divider equal the constants below. Change
 * the panel size or bar_top = trunc(content_bottom * 0.55) and that test
 * fails and names this file, rather than the boot screen quietly drifting
 * away from the screen it is meant to be the same as.
 *
 * ============ THREE BARS, NOT ONE ============
 *
 * The install moves the same 51 MB three times -- hash the package, write the
 * flash, hash it back -- at three very different rates. One bar over 3x the
 * bytes would sprint to 33%, look frozen for most of a minute and then finish
 * in a rush, and people read a bar's RATE as an estimate of the time left. So
 * each phase gets its own sweep with its own label, which is the same idiom
 * nd_progress_set_step() already gives the Update app, and needs no
 * per-device calibration to be honest on the first boot after a flash.
 */

#ifndef ND_BOOTBAR_H_INCLUDED
#define ND_BOOTBAR_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nd_bootfb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The five boxes and the divider, as lib/nd_progress.c computes them on a
 * 240x175 panel. Its own header comment lists the same numbers, because
 * neodct/tests/test_update_ui.py asserts on them too. */
extern const nd_brect nd_bootbar_header_box;
extern const nd_brect nd_bootbar_label_box;
extern const nd_brect nd_bootbar_bar_box;
extern const nd_brect nd_bootbar_status_box;
extern const nd_brect nd_bootbar_hint_box;
extern const int32_t nd_bootbar_divider_y;

/* ND_PROGRESS_INSET: the fill sits two pixels inside the outline. */
#define ND_BOOTBAR_INSET 2

/* The Update app's own strings, so the boot screen and the in-system update
 * screen say the same words. update_app.h's ND_UPDATE_HEADER. */
#define ND_BOOTBAR_HEADER "SOFTWARE UPDATE"
#define ND_BOOTBAR_HINT   "Do not power off"

/* trunc(done * 100 / total), clamped to [0, 100], with total == 0 meaning
 * done. Literally the expression in nd_progress_draw(), so the boot bar and
 * the in-system bar round identically -- test_bootbar.c checks the two
 * against each other over a table of values. */
int32_t nd_bootbar_percent(int64_t done, int64_t total);

/* One frame. `phase` is 1..3 and shows as "1/3" in the header; 0 draws no
 * phase marker. `total` of 0 suppresses the "x of y MB" detail and centres
 * the percentage, exactly as nd_progress_draw() does with no detail
 * function. */
void nd_bootbar_frame(nd_bootfb *fb, const char *step, int32_t phase, int64_t done, int64_t total);

/* The same frame at an exact percentage rather than a byte count: the opening
 * 0% frame drawn before the signature check, and the 100% frame drawn before
 * the sync that follows the write. (A 51 MB sync with the bar sitting at 99%
 * would look like the hang this whole feature exists to remove.) */
void nd_bootbar_frame_at(nd_bootfb *fb, const char *step, int32_t phase, int32_t percent,
                         int64_t total);

/* The refusal screen. `headline` replaces the step label, the bar is drawn as
 * an empty outline, and `reason` is centred where the reading would be.
 *
 * This is the half of the feature that matters most. Every refusal inside
 * apply_pending() logs to /dev/console -- a serial cable the owner does not
 * have -- and then boots the old system, so an update that is not signed by
 * the release key today looks to its owner like an update that did nothing. */
void nd_bootbar_fail(nd_bootfb *fb, const char *headline, const char *reason);

/* Copy stdin to stdout, drawing as the bytes pass.
 *
 * The count is exact rather than sampled: it is the bytes that actually
 * crossed the pipe, at the point they crossed it. `fb` may be an unopened
 * nd_bootfb, in which case this is a plain copy -- which is the property that
 * stops a progress bar from ever failing an install.
 *
 * Returns true when the whole of stdin reached stdout. */
bool nd_bootbar_filter(nd_bootfb *fb, int in_fd, int out_fd, const char *step, int32_t phase,
                       int64_t total);

#ifdef __cplusplus
}
#endif

#endif /* ND_BOOTBAR_H_INCLUDED */
