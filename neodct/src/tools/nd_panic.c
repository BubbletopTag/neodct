/* nd_panic.c -- the binary that draws the screen when nd-core is gone.
 *
 *     nd-panic --status 139 --crash 1 --limit 3
 *     nd-panic --status 139 --crash 3 --limit 3 --halt
 *     nd-panic --status 139 --out /tmp/frames --no-wait   (PNGs, no device)
 *
 * /bin/run_neodct.sh runs this after every death of nd-core. It draws the
 * countdown itself, one frame a second, and returns when the clock reaches
 * zero -- so the shell's restart is simply "the next statement" and there is
 * no sleep to keep in step with a picture.
 *
 * nd_panic.h explains at length why this is a separate program rather than
 * shell code or a second call into nd_crash.c. What is worth repeating here
 * is the part that constrains this file: it must not do anything nd-core
 * does. The whole of main() is two ioctls and an mmap (nd_fb_open), two
 * FT_New_Face calls, one JPEG decode, and a loop of memsets. No nd_ui, no
 * settings, no database, no input device, no thread. If any of it fails the
 * next line down still runs, and a non-zero exit tells the shell to fall back
 * to its ANSI banner.
 *
 * ============ THE EXIT STATUS IS A CONTRACT ============
 *
 * 0  something was put on the panel, and for a restart the countdown has
 *    already been waited out. The shell restarts immediately.
 * 1  nothing reached the panel. The shell prints its banner and does the
 *    waiting itself.
 *
 * So the shell's fallback must sleep and this must not tell it to when the
 * sleep already happened. Getting that backwards gives a six-second countdown
 * that says three.
 *
 * ============ WHY IT TURNS THE BACKLIGHT ON ============
 *
 * The idle blanker and the Sleepy app both drive the panel dark, and a core
 * that crashes while the screen is off would otherwise draw this beautifully
 * into a black panel. nd_backlight_on() is a handful of sysfs writes and is a
 * documented no-op where there is no backlight to find, which is every
 * development host and QEMU.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_crash.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_log.h"
#include "nd_panic.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"

typedef struct {
    nd_panic_state st;
    int32_t seconds; /* countdown length; 0 draws a single frame */
    const char *fb;  /* framebuffer device, NULL for ND_PATH_FB  */
    const char *out; /* PNG directory instead of a device        */
    bool log;        /* append to the crash log                  */
    bool no_wait;    /* draw every frame at once, for a harness  */
} panic_opts;

static void usage(void)
{
    (void)fprintf(stderr, "usage: nd-panic [--status N] [--crash N] [--limit N]\n"
                          "                [--seconds N] [--halt] [--no-log] [--no-wait]\n"
                          "                [--fb DEVICE] [--out DIR]\n");
}

/* One argument that must be a number. Returns false on anything else, so a
 * typo in the boot script becomes a usage error rather than a zero. */
static bool arg_int(const char *s, int32_t *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0')
        return false;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || v < -1000000L || v > 1000000L)
        return false;
    *out = (int32_t)v;
    return true;
}

static bool parse_args(int argc, char **argv, panic_opts *o)
{
    int i;

    memset(o, 0, sizeof *o);
    o->st.mode = ND_PANIC_RESTART;
    o->st.limit = ND_PANIC_MAX_RESTARTS;
    o->seconds = ND_PANIC_COUNTDOWN;
    o->log = true;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool needs_value = strcmp(a, "--status") == 0 || strcmp(a, "--crash") == 0 ||
                           strcmp(a, "--limit") == 0 || strcmp(a, "--seconds") == 0 ||
                           strcmp(a, "--fb") == 0 || strcmp(a, "--out") == 0;

        if (needs_value && i + 1 >= argc)
            return false;

        if (strcmp(a, "--halt") == 0) {
            o->st.mode = ND_PANIC_HALT;
        } else if (strcmp(a, "--no-log") == 0) {
            o->log = false;
        } else if (strcmp(a, "--no-wait") == 0) {
            o->no_wait = true;
        } else if (strcmp(a, "--status") == 0) {
            if (!arg_int(argv[++i], &o->st.status))
                return false;
        } else if (strcmp(a, "--crash") == 0) {
            if (!arg_int(argv[++i], &o->st.crash))
                return false;
        } else if (strcmp(a, "--limit") == 0) {
            if (!arg_int(argv[++i], &o->st.limit))
                return false;
        } else if (strcmp(a, "--seconds") == 0) {
            if (!arg_int(argv[++i], &o->seconds))
                return false;
        } else if (strcmp(a, "--fb") == 0) {
            o->fb = argv[++i];
        } else if (strcmp(a, "--out") == 0) {
            o->out = argv[++i];
        } else {
            return false;
        }
    }
    if (o->seconds < 0 || o->seconds > 60)
        return false;
    return true;
}

/* nanosleep rather than sleep(3): sleep() is specified to interact with
 * SIGALRM, and this process may be running under an init that uses one. */
static void wait_a_second(void)
{
    struct timespec ts;

    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* ------------------------------------------------------------------ *
 * The two sinks
 * ------------------------------------------------------------------ */

/* The PNG sink is the cheapest harness that produces a real picture: the same
 * nd_panic_draw() over the same canvas, saved instead of packed. It is how
 * the layout was reviewed and it is what test_panic.c drives, so the frames a
 * developer looks at are the frames the phone gets. */
static nd_err save_png(const char *dir, const nd_image *canvas, const nd_panic_state *st)
{
    char path[ND_PATH_MAX];

    if (st->mode == ND_PANIC_HALT)
        (void)nd_snprintf(path, sizeof path, "%s/panic-halt.png", dir);
    else
        (void)nd_snprintf(path, sizeof path, "%s/panic-%d.png", dir, (int)st->remaining);
    return nd_image_save_png(canvas, path);
}

int main(int argc, char **argv)
{
    panic_opts o;
    nd_fb *fb = NULL;
    nd_image *canvas = NULL;
    nd_image *art = NULL;
    nd_font *title = NULL;
    nd_font *body = NULL;
    nd_font *count = NULL;
    nd_panic_fonts fonts;
    char fontpath[ND_PATH_MAX];
    int32_t tick;
    bool drew = false;
    int rc = 1;

    if (!parse_args(argc, argv, &o)) {
        usage();
        return 2;
    }

    /* The log first, because it is the half that survives a panel that will
     * not open. nd_crash_log() never fails into its caller. */
    if (o.log) {
        nd_crash_info info;
        char note[64];

        nd_panic_status_info(o.st.status, &info);
        if (o.st.crash > 0 && o.st.limit > 0)
            (void)nd_snprintf(note, sizeof note, "consecutive crash %d of %d%s", (int)o.st.crash,
                              (int)o.st.limit,
                              o.st.mode == ND_PANIC_HALT ? " -- not restarting" : "");
        else
            note[0] = '\0';
        (void)nd_crash_log("nd-core", &info, note[0] != '\0' ? note : NULL);
    }

    if (o.out == NULL) {
        /* A screen nobody can see is not a screen. See the header. */
        (void)nd_backlight_on(100);

        if (nd_fb_open(&fb, o.fb) != ND_OK) {
            nd_log_err(ND_LOG_CRASH, "nd-panic: no framebuffer; the shell will draw instead");
            goto done;
        }
    } else if (nd_mkdir_p(o.out, 0755u) != ND_OK) {
        nd_log_err(ND_LOG_CRASH, "nd-panic: cannot create %s", o.out);
        goto done;
    }

    /* 240 x 175 RGB888 == 126,000 bytes, the same frame the UI uses. Drawn
     * once per tick and reused; nothing is allocated inside the loop. */
    canvas = nd_image_new(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888);
    if (canvas == NULL) {
        nd_log_err(ND_LOG_CRASH, "nd-panic: cannot allocate a %dx%d canvas", ND_UI_W, ND_UI_H);
        goto done;
    }

    /* nd_font_load() takes a REAL filesystem path; the ND_ROOT hook belongs to
     * whoever owns the constant, exactly as ui_load_fonts() does it. */
    if (nd_path_resolve(fontpath, sizeof fontpath, ND_PATH_FONT) != ND_OK)
        fontpath[0] = '\0';
    title = nd_font_load(fontpath, ND_FONT_PX_MD);
    body = nd_font_load(fontpath, ND_FONT_PX_S);
    count = nd_font_load(fontpath, ND_FONT_PX_XL);
    fonts.title = title;
    fonts.body = body;
    fonts.count = count;
    art = nd_panic_load_art();

    /* HALT and a zero-length countdown are one still frame each. A real
     * countdown is exactly `seconds` frames -- N down to 1, a second of
     * looking at each -- so the call takes precisely as long as the number on
     * the screen said it would, and the shell can restart on the next line
     * with nothing to keep in step. There is deliberately no "0..." frame;
     * see nd_panic_countdown_text(). */
    tick = o.st.mode == ND_PANIC_RESTART ? o.seconds : 0;
    do {
        nd_err rr;

        o.st.remaining = tick;
        if (nd_panic_draw(canvas, &fonts, art, &o.st) != ND_OK)
            break;

        rr = o.out != NULL ? save_png(o.out, canvas, &o.st) : nd_fb_update(fb, canvas);
        if (rr != ND_OK) {
            nd_log_err(ND_LOG_CRASH, "nd-panic: frame not delivered: %s", nd_strerror(rr));
            break;
        }
        drew = true;

        if (tick <= 0)
            break;
        /* The pause is NOT skipped just because the frames are going to PNGs:
         * `nd-panic --out DIR --seconds 2` taking two seconds is the only way
         * a host with no framebuffer can check that the countdown is paced at
         * all. --no-wait is how a harness that only wants the pictures asks
         * to skip it. */
        if (!o.no_wait)
            wait_a_second();
    } while (--tick >= 1);

    if (drew)
        rc = 0;

done:
    /* Freed even though the process is about to exit, per
     * CODING-STANDARDS.md 1.7 -- this binary runs under ASan in the test
     * suite and a leak here would hide a real one. */
    nd_image_free(art);
    nd_image_free(canvas);
    nd_font_free(title);
    nd_font_free(body);
    nd_font_free(count);
    if (fb != NULL)
        nd_fb_close(fb);
    return rc;
}
