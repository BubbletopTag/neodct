/* nd_bootbar.c -- argv for the initramfs install-progress screen.
 *
 * Three modes, all reached from neodct/initramfs/ndsys-panel.sh:
 *
 *   nd-bootbar --step "Installing" --phase 2 --total 53477376
 *       A PIPELINE STAGE. Copies stdin to stdout and draws the bar from the
 *       bytes that actually cross the pipe. Every long phase of the install
 *       is already a pipeline, so it drops in between two existing stages and
 *       measures the real thing rather than guessing at it.
 *
 *   nd-bootbar --step "Checking the update" --phase 1 --total N --at 0
 *       One frame at a fixed percentage, no copying. The 0% frame is what
 *       puts something on the panel within a fraction of a second of the
 *       update starting, rather than after the first megabyte.
 *
 *   nd-bootbar --fail "Update refused" --reason "Not signed by NeoDCT"
 *       The refusal screen. Today every refusal in apply_pending() goes to
 *       /dev/console and the owner sees a logo and then a reboot.
 *
 * ============ IT MUST NEVER FAIL AN INSTALL ============
 *
 * A missing /dev/fb0, a framebuffer whose bit depth nobody expected, a write
 * that fails halfway: all of them mean the copy still happens and the exit
 * status is still 0. A cosmetic feature that can stop an operating system
 * from being installed is worse than no feature, and the shell side agrees --
 * progress_filter() degrades to `exec cat` when this binary is not there.
 *
 * ============ NOT STATIC-ONLY BECAUSE IT COULD NOT BE ============
 *
 * Unlike nd-verify this links nothing but libc: no OpenSSL, no libneodct, no
 * FreeType. Statically against musl that is tens of kilobytes, against the
 * 4.3 MB nd-verify already packed into the same initramfs.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_bootbar.h"
#include "nd_bootfb.h"

/* The panel, as ndsys-panel.sh's PANEL_FB names it. */
#define DEFAULT_FB "/dev/fb0"

/* 42 KB of shadow plus the packed frame pointer: static, not automatic. */
static nd_bootfb fb;

static void usage(void)
{
    (void)fprintf(stderr, "usage: nd-bootbar --step TEXT --phase N --total BYTES [--at PERCENT]\n"
                          "       nd-bootbar --fail HEADLINE --reason TEXT\n"
                          "options: --fb PATH (default " DEFAULT_FB ")\n"
                          "         --geom WxHxBPP[:STRIDE]  geometry for a plain file\n");
}

/* strtoll with the whole string required, because a total that silently
 * became 0 would put the bar at 100% for the entire phase. */
static bool parse_i64(const char *s, int64_t *out)
{
    char *end = NULL;
    long long v;

    if (s == NULL || *s == '\0')
        return false;
    v = strtoll(s, &end, 10);
    if (end == NULL || *end != '\0' || v < 0)
        return false;
    *out = (int64_t)v;
    return true;
}

/* "240x175x32" or "240x175x32:960". Only ever passed by the host tests and by
 * neodct/tools/bootbar_frames.py: a regular file has no FBIOGET_VSCREENINFO
 * to answer, and nd_bootfb refuses to invent an answer of its own. */
static bool parse_geom(const char *s, int32_t *w, int32_t *h, int32_t *bpp, size_t *stride)
{
    long a, b, c, d = 0;
    char *end = NULL;

    a = strtol(s, &end, 10);
    if (end == NULL || *end != 'x')
        return false;
    b = strtol(end + 1, &end, 10);
    if (end == NULL || *end != 'x')
        return false;
    c = strtol(end + 1, &end, 10);
    if (end == NULL)
        return false;
    if (*end == ':') {
        d = strtol(end + 1, &end, 10);
        if (end == NULL)
            return false;
    }
    if (*end != '\0' || a <= 0 || b <= 0 || c <= 0 || d < 0)
        return false;
    *w = (int32_t)a;
    *h = (int32_t)b;
    *bpp = (int32_t)c;
    *stride = (size_t)d;
    return true;
}

int main(int argc, char **argv)
{
    const char *step = "";
    const char *headline = NULL;
    const char *reason = "";
    const char *fb_path = DEFAULT_FB;
    const char *geom = NULL;
    int64_t total = 0;
    int32_t phase = 0;
    int32_t at = -1;
    bool live;
    int i;

    for (i = 1; i < argc; i++) {
        const char *flag = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(flag, "--help") == 0) {
            usage();
            return 0;
        }
        if (value == NULL) {
            usage();
            return 2;
        }
        i++;
        if (strcmp(flag, "--step") == 0) {
            step = value;
        } else if (strcmp(flag, "--fail") == 0) {
            headline = value;
        } else if (strcmp(flag, "--reason") == 0) {
            reason = value;
        } else if (strcmp(flag, "--fb") == 0) {
            fb_path = value;
        } else if (strcmp(flag, "--geom") == 0) {
            geom = value;
        } else if (strcmp(flag, "--phase") == 0) {
            int64_t v;

            if (!parse_i64(value, &v) || v > 3) {
                usage();
                return 2;
            }
            phase = (int32_t)v;
        } else if (strcmp(flag, "--total") == 0) {
            if (!parse_i64(value, &total)) {
                usage();
                return 2;
            }
        } else if (strcmp(flag, "--at") == 0) {
            int64_t v;

            if (!parse_i64(value, &v) || v > 100) {
                usage();
                return 2;
            }
            at = (int32_t)v;
        } else {
            usage();
            return 2;
        }
    }

    if (geom != NULL) {
        int32_t w = 0, h = 0, bpp = 0;
        size_t stride = 0u;

        if (!parse_geom(geom, &w, &h, &bpp, &stride)) {
            usage();
            return 2;
        }
        live = nd_bootfb_open_at(&fb, fb_path, w, h, bpp, stride);
    } else {
        live = nd_bootfb_open(&fb, fb_path);
    }

    /* From here on nothing checks `live` again except to decide whether the
     * filter should bother formatting a frame. Every nd_bootfb call is a
     * no-op on a struct that never opened. */
    if (headline != NULL) {
        nd_bootbar_fail(&fb, headline, reason);
    } else if (at >= 0) {
        nd_bootbar_frame_at(&fb, step, phase, at, total);
    } else {
        bool copied =
            nd_bootbar_filter(live ? &fb : NULL, STDIN_FILENO, STDOUT_FILENO, step, phase, total);

        nd_bootfb_close(&fb);
        /* A short copy is real: it means the stream broke, and the caller's
         * next stage (write_system, or sha256sum) will report it. Say so, but
         * note that in all three call sites this exit status is not the
         * pipeline's -- ash reports the LAST command's, and this is never
         * last. Nothing here can turn a good install into a failed one. */
        return copied ? 0 : 1;
    }

    nd_bootfb_close(&fb);
    return 0;
}
