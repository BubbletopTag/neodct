/* nd_bootbar_ui.c -- drawing and counting for the boot install screen.
 * nd_bootbar.h says what this is and why the layout is written down here
 * rather than derived.
 *
 * ============ THE PERCENT GATE IS WHY THIS IS FREE ============
 *
 * nd_progress_draw() paints nothing when the whole percentage has not moved,
 * and its header calls that "a performance feature" rather than an
 * optimisation. The same gate is ported here and it is what keeps the bar
 * out of the way of the write: at most 100 frames a phase, so one frame per
 * 512 KB on a 51 MB image, against a phase whose bottleneck is NAND
 * programming.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nd_bootbar.h"
#include "nd_bootfb.h"

/* lib/nd_progress.c on a 240x175 panel. Cross-checked by test_bootbar.c
 * against a real nd_ui, which is the only thing that makes writing them down
 * legitimate. */
const nd_brect nd_bootbar_header_box = {0, 4, 240, 19};
const nd_brect nd_bootbar_label_box = {0, 44, 240, 65};
const nd_brect nd_bootbar_bar_box = {20, 79, 220, 93};
const nd_brect nd_bootbar_status_box = {20, 102, 220, 117};
const nd_brect nd_bootbar_hint_box = {0, 124, 240, 139};
const int32_t nd_bootbar_divider_y = 24;

/* 64 KiB, the pipe's own buffer size. Three phases x 51 MB is ~2400 extra
 * pipe transactions per install, which against a NAND write is noise. */
#define COPY_CHUNK (64u * 1024u)

/* Python's // floors, and a label wider than its box makes the numerator
 * negative -- which is where floor and C's truncation part company.
 * nd_progress.c's floordiv2(), ported so a long headline lands on the same
 * pixel in both. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

/* _centered(): (box.x0 + box.x1 - text_w) // 2, clamped at 0. Note it is
 * left + RIGHT, not left + width -- see the note at the top of nd_progress.c
 * about not porting the coincidence. */
static int32_t centered_x(const char *s, nd_bootfb_size size, nd_brect box)
{
    int32_t w = nd_bootfb_text_w(s, size);
    int32_t x = floordiv2(box.x0 + box.x1 - w);

    return (x > 0) ? x : 0;
}

int32_t nd_bootbar_percent(int64_t done, int64_t total)
{
    int32_t percent;

    /* int(done * 100 / total): Python's true division then int(), which
     * truncates toward zero, and doubles reproduce it exactly. total == 0
     * means "done", not "divide by zero" -- as in nd_progress_draw(). */
    percent = (total != 0) ? (int32_t)((double)done * 100.0 / (double)total) : 100;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    return percent;
}

/* MiB, called MB on screen, and truncated to one decimal exactly as
 * nd_update_format_size() in apps/Update/main.c does it. */
static double megabytes(int64_t count)
{
    return (double)count / 1048576.0;
}

/* Everything above the bar, shared by the progress frame and the failure
 * frame so the two cannot drift apart. */
static void draw_chrome(nd_bootfb *fb, const char *label, int32_t phase)
{
    char marker[8];

    nd_bootfb_clear(fb);

    nd_bootfb_text(fb, 10, nd_bootbar_header_box.y0, ND_BOOTBAR_HEADER, ND_BOOTFB_SMALL);
    if (phase >= 1 && phase <= 3) {
        /* Right-aligned on the header line, where nd_header draws the "1-4"
         * breadcrumb in the running UI. It is what explains the bar going
         * back to zero: the step changed, and the screen says which step. */
        (void)snprintf(marker, sizeof marker, "%d/3", (int)phase);
        nd_bootfb_text(fb,
                       nd_bootbar_header_box.x1 - 10 - nd_bootfb_text_w(marker, ND_BOOTFB_SMALL),
                       nd_bootbar_header_box.y0, marker, ND_BOOTFB_SMALL);
    }
    nd_bootfb_hline(fb, 10, nd_bootbar_header_box.x1 - 10, nd_bootbar_divider_y);

    if (label != NULL && label[0] != '\0') {
        /* room = width - 16, the ladder, then ellipsize -- nd_progress_draw()
         * does all three and in that order. Without the ladder "Checking the
         * update" runs off the right of the panel at 20 px; with it, the boot
         * screen drops to the same size the Update app drops to. */
        int32_t room = nd_bootbar_label_box.x1 - 16;
        nd_bootfb_size size = nd_bootfb_fit(label, room);
        char fitted[ND_BOOTFB_LINE_MAX];

        nd_bootfb_ellipsize(fitted, sizeof fitted, label, size, room);
        nd_bootfb_text(fb, centered_x(fitted, size, nd_bootbar_label_box), nd_bootbar_label_box.y0,
                       fitted, size);
    }
}

static void draw_frame(nd_bootfb *fb, const char *step, int32_t phase, int32_t percent,
                       int64_t done, int64_t total)
{
    int32_t span;
    int32_t filled;
    char reading[16];
    char detail[40];

    draw_chrome(fb, step, phase);

    /* width=1 outline drawn INSIDE the inclusive box, so the frame occupies
     * rows 79 and 93 and columns 20 and 220. */
    nd_bootfb_outline(fb, nd_bootbar_bar_box);
    span = (nd_bootbar_bar_box.x1 - ND_BOOTBAR_INSET) - (nd_bootbar_bar_box.x0 + ND_BOOTBAR_INSET);
    filled = (int32_t)((double)span * (double)percent / 100.0);
    if (filled > 0)
        nd_bootfb_fill(fb,
                       ND_BRECT(nd_bootbar_bar_box.x0 + ND_BOOTBAR_INSET,
                                nd_bootbar_bar_box.y0 + ND_BOOTBAR_INSET,
                                nd_bootbar_bar_box.x0 + ND_BOOTBAR_INSET + filled,
                                nd_bootbar_bar_box.y1 - ND_BOOTBAR_INSET),
                       true);

    (void)snprintf(reading, sizeof reading, "%d%%", (int)percent);
    if (total > 0) {
        /* "24.1 of 51.0 MB", NOT nd_update_size_detail()'s "24.1 MB of
         * 51.0 MB". The unit is said once because it has to be: this status
         * line also carries the percentage left-aligned, and at 14 px in a
         * 200 px box the long form measures 156 px against "100%"'s 41, which
         * leaves three pixels between them and reads as one run-on word. The
         * short form is 126 and the gap is 33. nd_update_size_detail() has
         * the whole width to itself and does not have to make that trade. */
        (void)snprintf(detail, sizeof detail, "%.1f of %.1f MB", megabytes(done), megabytes(total));
        nd_bootfb_text(fb, nd_bootbar_status_box.x0, nd_bootbar_status_box.y0, reading,
                       ND_BOOTFB_SMALL);
        nd_bootfb_text(fb, nd_bootbar_status_box.x1 - nd_bootfb_text_w(detail, ND_BOOTFB_SMALL),
                       nd_bootbar_status_box.y0, detail, ND_BOOTFB_SMALL);
    } else {
        nd_bootfb_text(fb, centered_x(reading, ND_BOOTFB_SMALL, nd_bootbar_status_box),
                       nd_bootbar_status_box.y0, reading, ND_BOOTFB_SMALL);
    }

    nd_bootfb_text(fb, centered_x(ND_BOOTBAR_HINT, ND_BOOTFB_SMALL, nd_bootbar_hint_box),
                   nd_bootbar_hint_box.y0, ND_BOOTBAR_HINT, ND_BOOTFB_SMALL);

    /* Rows 146-174 stay black. nd_progress_draw() ends by clearing the
     * softkey strip; there is nothing to press during a boot install, so this
     * screen simply never draws there. */
    nd_bootfb_present(fb);
}

void nd_bootbar_frame(nd_bootfb *fb, const char *step, int32_t phase, int64_t done, int64_t total)
{
    if (fb == NULL)
        return;
    draw_frame(fb, step, phase, nd_bootbar_percent(done, total), done, total);
}

void nd_bootbar_frame_at(nd_bootfb *fb, const char *step, int32_t phase, int32_t percent,
                         int64_t total)
{
    int64_t done;

    if (fb == NULL)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    /* The reading has to agree with the bar, so derive the byte count from
     * the percentage rather than showing "0.0 of 51.0 MB" under a full bar. */
    done = (total > 0) ? (int64_t)((double)total * (double)percent / 100.0) : 0;
    draw_frame(fb, step, phase, percent, done, total);
}

void nd_bootbar_fail(nd_bootfb *fb, const char *headline, const char *reason)
{
    if (fb == NULL)
        return;

    /* No phase marker: a refusal is not step 2 of 3, it is the end. */
    draw_chrome(fb, headline, 0);

    /* An empty outline rather than no bar at all, so the screen is
     * recognisably the same screen the install was drawing a moment ago and
     * the owner reads it as "this stopped" rather than "this is something
     * else". */
    nd_bootfb_outline(fb, nd_bootbar_bar_box);

    if (reason != NULL && reason[0] != '\0')
        nd_bootfb_text(fb, centered_x(reason, ND_BOOTFB_SMALL, nd_bootbar_status_box),
                       nd_bootbar_status_box.y0, reason, ND_BOOTFB_SMALL);

    /* No "Do not power off": there is nothing left to interrupt. */
    nd_bootfb_present(fb);
}

/* ------------------------------------------------------------------ *
 * The counting filter
 * ------------------------------------------------------------------ */

static bool write_all(int fd, const uint8_t *buf, size_t n)
{
    /* A pipe write can be short whenever the reader is slower than we are,
     * and this one's reader is ubiupdatevol erasing NAND. Dropping the tail
     * of a chunk would truncate the system image by however much was left. */
    while (n > 0u) {
        ssize_t got = write(fd, buf, n);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        buf += (size_t)got;
        n -= (size_t)got;
    }
    return true;
}

bool nd_bootbar_filter(nd_bootfb *fb, int in_fd, int out_fd, const char *step, int32_t phase,
                       int64_t total)
{
    static uint8_t buf[COPY_CHUNK];
    int64_t done = 0;
    int32_t drawn = -1; /* nd_progress's percent == -1: nothing drawn yet */

    if (fb != NULL) {
        nd_bootbar_frame(fb, step, phase, 0, total);
        drawn = nd_bootbar_percent(0, total);
    }

    for (;;) {
        ssize_t got = read(in_fd, buf, sizeof buf);
        int32_t percent;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            break;
        if (!write_all(out_fd, buf, (size_t)got))
            return false;
        done += (int64_t)got;

        if (fb == NULL)
            continue;
        percent = nd_bootbar_percent(done, total);
        if (percent == drawn)
            continue;
        drawn = percent;
        nd_bootbar_frame(fb, step, phase, done, total);
    }
    return true;
}
