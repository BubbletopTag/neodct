/* nd_panic.c -- composing the core-crash screen, and reading a wait status.
 *
 * Everything here is a pure function of its arguments plus two files that
 * live on the read-only, dm-verity-protected squashfs (CRASH.jpg and
 * font.ttf). It opens no device, starts no thread, allocates once for the
 * artwork and never in the frame loop. That is the whole point: this code
 * runs in the aftermath of nd-core dying, so the less of the system it needs
 * the more likely it is to work -- see the ladder in nd_panic.h.
 *
 * The binary that drives it is tools/nd_panic.c; the policy that decides when
 * to call it is /bin/nd-crashguard.sh.
 *
 * ============ WHY THE ROW POSITIONS ARE CONSTANTS ============
 *
 * The house rule is to position text by its ink extents rather than by the
 * font's line height, because "17" and "8" genuinely do not sit on the same
 * pixel here. That rule is about placing ONE row. A stacked block of rows
 * needs the opposite as well, or "Core System" (which has a descender in the
 * y) and "Crashed!" (which has none) end up at different pitches and the
 * title looks kerned by accident.
 *
 * So both apply: the ROW is at a fixed y, and within the row the string's INK
 * TOP is pinned to that y. Every line lands where the layout says and no line
 * moves because of which letters are in it.
 */

#include <stdio.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_panic.h"
#include "nd_paths.h"
#include "nd_types.h"

/* ------------------------------------------------------------------ *
 * The layout
 * ------------------------------------------------------------------ */

/* Ink-top rows on a 175-row panel. The headline rows are 22 apart (the 18 px
 * face plus four rows of air) and the small rows 20 apart (the 14 px face
 * plus six), which gives the cause and the attempt count enough separation to
 * read as two facts rather than one wrapped sentence.
 *
 * MEASURED, against this font at these sizes, in a 150 px column:
 *
 *   18 px  "Core System" 137   "Crashed!"          97
 *   14 px  "SIGSEGV (11)" 100  "Not restarting."  128
 *          "Restarting in" 110 "Power off and on" 147
 *          "exited cleanly" 121 "try 1 of 3"       77
 *   24 px  "3..." 45
 *
 * The last of those is why the headline is 18 and not 20: "Core System" sets
 * 153 at 20 px and loses its final letter off the right edge of the panel.
 * Everything here fits with room to spare except "Power off and on", which
 * fits by three pixels and lost its full stop to get there. */
#define ROW_TITLE_1 12
#define ROW_TITLE_2 34
#define ROW_CAUSE   70
#define ROW_TRY     90
#define ROW_LEAD    120
#define ROW_BIG     140
#define ROW_FOOT_2  140

/* The countdown's digit is drawn in the 24 px face and the words above it in
 * the 14 px one. One number changing three times, large, is what makes the
 * screen read as a live countdown rather than a still picture with a number
 * in it. */
static const char *const TITLE_1 = "Core System";
static const char *const TITLE_2 = "Crashed!";
static const char *const LEAD_RESTART = "Restarting in";
/* The clock has run out and the digit is gone, so the lead line has to carry
 * the sentence on its own. "Restarting in" with nothing under it would read
 * as a screen that had stopped updating. */
static const char *const LEAD_NOW = "Restarting...";

/* HALT mode's two lines. "Power off and on" and not "press the power key",
 * because there is no power key: the sixteen are NaviKey, C, Up, Down, 0-9,
 * * and #. Cutting the power is genuinely the only move the owner has left,
 * and it is also a real repair for a real class of fault, which is why the
 * count does not survive it.
 *
 * The serial getty is still respawning on ttyFIQ0/ttyAMA0 behind this screen,
 * so a developer looking at a halted phone has a shell and a crash log; the
 * owner has a sentence. Both are better than a frozen home screen. */
static const char *const HALT_1 = "Not restarting.";
static const char *const HALT_2 = "Power off and on";

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

size_t nd_panic_status_text(int32_t status, char *out, size_t out_sz)
{
    char full[64];

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';

    if (status > 128 && status < 128 + 64) {
        int signo = (int)(status - 128);
        const char *name = nd_crash_signal_name(signo);

        /* nd_crash_signal_name() answers "signal" for one it has no name for,
         * and "signal (37)" reads like a typo. */
        if (strcmp(name, "signal") == 0)
            (void)nd_snprintf(full, sizeof full, "signal %d", signo);
        else
            (void)nd_snprintf(full, sizeof full, "%s (%d)", name, signo);
    } else if (status == 0) {
        (void)nd_strlcpy(full, "exited cleanly", sizeof full);
    } else {
        (void)nd_snprintf(full, sizeof full, "exit code %d", (int)status);
    }
    return nd_strlcpy(out, full, out_sz);
}

void nd_panic_status_info(int32_t status, nd_crash_info *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof *out);
    if (status > 128 && status < 128 + 64) {
        out->from_signal = true;
        out->signo = (int)(status - 128);
    } else {
        out->exit_status = (int)status;
    }
    /* The detail line is what nd_crash_summary() puts on the screen and in
     * the log. An app's comes from its own signal handler over a pipe; the
     * core has no such channel -- it IS the reader -- so this is assembled
     * from the only fact that survived, and says so. */
    {
        char cause[64];

        (void)nd_panic_status_text(status, cause, sizeof cause);
        (void)nd_snprintf(out->detail, sizeof out->detail, "CoreExit: nd-core died with %s", cause);
    }
}

size_t nd_panic_countdown_text(int32_t remaining, char *out, size_t out_sz)
{
    char full[32];

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';

    if (remaining <= 0)
        return 0u;
    (void)nd_snprintf(full, sizeof full, "%d...", (int)remaining);
    return nd_strlcpy(out, full, out_sz);
}

/* ------------------------------------------------------------------ *
 * The artwork
 * ------------------------------------------------------------------ */

nd_image *nd_panic_load_art(void)
{
    nd_image *full;
    nd_image *crop;

    /* NOT through the image cache, for the same reason nd_crash.c opens it
     * directly: there is no cache in this process and no reason to build one
     * for a picture shown once. */
    full = nd_image_open(ND_PATH_CRASH_IMAGE);
    if (full == NULL)
        return NULL;

    /* crop_zeropad rather than crop: a replacement CRASH.jpg smaller than the
     * measured box then yields a correctly sized, partly black tile instead
     * of nothing at all. The screen degrades; it does not disappear. */
    crop = nd_image_crop_zeropad(
        full, ND_RECT(ND_PANIC_ART_X0, ND_PANIC_ART_Y0, ND_PANIC_ART_X1, ND_PANIC_ART_Y1));
    nd_image_free(full);
    if (crop == NULL)
        return NULL;

    if (crop->fmt != ND_PIXFMT_RGB888) {
        nd_image *rgb = nd_image_convert(crop, ND_PIXFMT_RGB888);

        nd_image_free(crop);
        crop = rgb;
    }
    /* owned by the caller; free with nd_image_free() */
    return crop;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

/* Draw so that the string's INK TOP lands on ink_y. See the header comment. */
static void text_at_ink(nd_draw *d, int32_t x, int32_t ink_y, const char *s, const nd_font *f,
                        nd_color c)
{
    nd_rect bb;

    if (f == NULL || s == NULL || s[0] == '\0')
        return;
    nd_text_bbox(f, s, &bb);
    (void)nd_draw_text(d, x, ink_y - bb.y0, s, f, c);
}

/* Whichever of the three sizes is actually loaded, preferring the one the row
 * was designed for. Returns NULL only when nothing loaded at all. */
static const nd_font *pick(const nd_font *want, const nd_panic_fonts *f)
{
    if (want != NULL)
        return want;
    if (f->body != NULL)
        return f->body;
    if (f->title != NULL)
        return f->title;
    return f->count;
}

nd_err nd_panic_draw(nd_image *canvas, const nd_panic_fonts *fonts, const nd_image *art,
                     const nd_panic_state *st)
{
    static const nd_panic_fonts NO_FONTS = {NULL, NULL, NULL};
    const nd_panic_fonts *f = fonts != NULL ? fonts : &NO_FONTS;
    nd_draw d;
    int32_t x;
    char buf[64];

    if (canvas == NULL || st == NULL)
        return ND_ERR_INVAL;
    if (nd_draw_bind(&d, canvas) != ND_OK)
        return ND_ERR_INVAL;

    /* No wallpaper, and no nd_ui_paint_chrome_* -- this is not chrome. It is
     * the same judgement nd_crash.c makes for the app crash screen: a
     * photograph behind 14 px type on the one screen whose entire job is to
     * be legible is a legibility problem. */
    (void)nd_image_fill(canvas, ND_BLACK);

    /* No font at all means the rootfs is unreadable, and there is nothing
     * left to say it with. A solid red panel is still a message: it is not
     * black, it is not the UI, and it is visibly not a hang. */
    if (pick(NULL, f) == NULL) {
        (void)nd_image_fill(canvas, ND_RGB(255, 0, 0));
        return ND_OK;
    }

    x = ND_PANIC_TEXT_X_BARE;
    if (art != NULL && nd_image_blit(canvas, art, 0, 0) == ND_OK)
        x = ND_PANIC_TEXT_X;

    text_at_ink(&d, x, ROW_TITLE_1, TITLE_1, pick(f->title, f), ND_WHITE);
    text_at_ink(&d, x, ROW_TITLE_2, TITLE_2, pick(f->title, f), ND_WHITE);

    (void)nd_panic_status_text(st->status, buf, sizeof buf);
    text_at_ink(&d, x, ROW_CAUSE, buf, pick(f->body, f), ND_GRAY);

    /* "try 2 of 3" is the only place the guard is visible to the owner, and
     * it is what turns two identical-looking crash screens into a countdown
     * of a different kind. Suppressed when the caller did not supply the
     * numbers rather than printed as "try 0 of 0". */
    if (st->crash > 0 && st->limit > 0) {
        (void)nd_snprintf(buf, sizeof buf, "try %d of %d", (int)st->crash, (int)st->limit);
        text_at_ink(&d, x, ROW_TRY, buf, pick(f->body, f), ND_GRAY);
    }

    if (st->mode == ND_PANIC_HALT) {
        text_at_ink(&d, x, ROW_LEAD, HALT_1, pick(f->body, f), ND_WHITE);
        text_at_ink(&d, x, ROW_FOOT_2, HALT_2, pick(f->body, f), ND_WHITE);
        return ND_OK;
    }

    if (nd_panic_countdown_text(st->remaining, buf, sizeof buf) == 0u) {
        text_at_ink(&d, x, ROW_LEAD, LEAD_NOW, pick(f->body, f), ND_WHITE);
        return ND_OK;
    }
    text_at_ink(&d, x, ROW_LEAD, LEAD_RESTART, pick(f->body, f), ND_WHITE);
    text_at_ink(&d, x, ROW_BIG, buf, pick(f->count, f), ND_WHITE);
    return ND_OK;
}
