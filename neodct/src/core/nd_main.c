/* nd_main.c -- nd-core's entry point: the boot sequence and the main loop.
 *
 * This is launcher.py's main() followed by System/core/main.py's run(), in
 * that order, because that is the order the phone boots in and every step
 * depends on the one before it.
 *
 * ============ THE ORDER IS LOAD-BEARING ============
 *
 *   1. nd_log_redirect_serial()  -- before anything can print. Colour goes on
 *      AFTER the redirect, never before: it has to wrap the serial stream, not
 *      the one that was replaced a line ago.
 *   2. nd_clock_start()          -- before anything can reach the network. A
 *      phone with no battery-backed RTC boots at the epoch, and every TLS
 *      certificate has a "not valid before" date, so a 1970 clock fails
 *      validation on every HTTPS site at once.
 *   3. nd_rs_start_if_enabled()  -- the remote shell, if it was left on.
 *   4. "[Launcher] Initializing Hardware...", then the framebuffer.
 *   5. the boot splash, then EXACTLY one second.
 *   6. "[Launcher] Starting UI...", then the loop.
 *
 * EVERY STEP'S FAILURE IS CAUGHT AND BOOT CONTINUES. A phone that will not
 * boot because NTP was unreachable is worse than a phone with the wrong time,
 * and the Python is careful about this in a way that is easy to lose in a
 * port -- each of those try/excepts is there because it fired once.
 *
 * ============ WHAT THE C LOOP CANNOT REPRODUCE, AND WHY IT DOES NOT MATTER ==
 *
 * The Python loop wraps its whole body in `except BaseException:` and carries
 * on. Almost everything that exception ever caught was AN APP CRASHING, because
 * an app was exec_module'd straight into the core process. Apps are now
 * separate processes: a null dereference in one kills that process and the
 * core reads the report and draws the crash screen. The failure the Python
 * papered over is the failure this design removes, which is a better answer
 * than reproducing the paper.
 *
 * What is genuinely gone is resuming the core from a fault in the CORE'S OWN
 * code. C cannot do that -- there is no consistent state to resume into. The
 * handler below writes one async-signal-safe line and re-raises, so the death
 * is visible on the serial console instead of silent.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_clock.h"
#include "nd_crash.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_ui.h"

/* ------------------------------------------------------------------ *
 * Subsystems that belong to other work packages
 * ------------------------------------------------------------------ *
 *
 * ClockService and RemoteShell are spec-core-services.md's, not this one's,
 * and neither exists in neodct/src yet. Weak, exactly as nd_ui.c does it for
 * the modem, the battery and the notify service: when the real module lands
 * the reference resolves and the branch simply starts being taken, and
 * nothing here needs editing. Both are inside a "boot continues" try/except in
 * the Python, so "not linked" is a case the Python already had a message for.
 */
#pragma weak nd_clock_start
/* The server list is DATA the same module owns, so it needs the same
 * treatment -- a weak object reference is NULL when nothing defines it. */
#pragma weak ND_NTP_SERVERS

void nd_rs_start_if_enabled(void);
#pragma weak nd_rs_start_if_enabled

#define BANNER_RULE_WIDTH 72
#define BANNER_RULE_CHAR  '='
/* the green the CORE tag uses, so the banner and the OS that follows it read
 * as one voice */
#define BANNER_COLOUR 46

/* launcher.py: DejaVu, NOT the NeoDCT font. The splash predates the UI's own
 * face and never moved. */
#define SPLASH_FONT_BOLD    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
#define SPLASH_FONT_REGULAR "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define SPLASH_TITLE        "Starting NeoDCT..."
#define SPLASH_SECONDS      1.0

static volatile sig_atomic_t g_quit;

/* ------------------------------------------------------------------ *
 * Signals
 * ------------------------------------------------------------------ */

static void on_quit(int signo)
{
    ND_UNUSED(signo);
    g_quit = 1;
}

static void on_fatal(int signo)
{
    static const char msg[] = "[CRASH] nd-core faulted; see the serial log\n";

    /* write(2) and raise(3) are async-signal-safe; nothing else here would be.
     * SA_RESETHAND has already restored the default, so the re-raise is the
     * real death and the exit status names the real signal. */
    (void)!write(2, msg, sizeof msg - 1u);
    (void)raise(signo);
    _exit(128 + signo);
}

static void install_signals(void)
{
    static const int FATAL[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = (int)((unsigned int)SA_RESETHAND);
    for (i = 0u; i < ND_ARRAY_LEN(FATAL); i++)
        (void)sigaction(FATAL[i], &sa, NULL);

    /* A write to a dead child's key channel must be an error, not a death. */
    (void)signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------------ *
 * The boot banner
 * ------------------------------------------------------------------ */

static void print_banner(void)
{
    char line[256];
    char banner[24][ND_LOG_BANNER_COLS];
    size_t n;
    size_t i;

    (void)nd_log_rule(line, sizeof line, BANNER_RULE_CHAR, BANNER_RULE_WIDTH, BANNER_COLOUR);
    (void)printf("\n%s\n", line);

    /* Pre-rendered at build time by post-build-system-metadata.sh from the
     * same VERSION_ID the rest of the image uses, so nothing here parses a
     * version and nothing here can disagree with one. An image without the
     * file simply has no art. */
    n = nd_log_banner_lines(ND_PATH_BANNER, banner, ND_ARRAY_LEN(banner));
    for (i = 0u; i < n; i++) {
        (void)nd_log_paint(line, sizeof line, banner[i], BANNER_COLOUR, true);
        (void)printf("%s\n", line);
    }

    (void)nd_log_rule(line, sizeof line, BANNER_RULE_CHAR, BANNER_RULE_WIDTH, BANNER_COLOUR);
    (void)printf("%s\n\n", line);
}

/* ------------------------------------------------------------------ *
 * The boot splash (launcher.py:show_boot_logo)
 * ------------------------------------------------------------------ */

/* "System v" + system.os.versionnumber, or "System v?" when it is empty.
 * Read from the image's own version.prop rather than typed in here: this line
 * spent a release showing the version before the one it was running. */
static void splash_version(char *out, size_t out_sz)
{
    const char *v = nd_settings_get(ND_SET_OS_VERSIONNUMBER, "");

    if (v == NULL || v[0] == '\0')
        v = "?";
    (void)nd_snprintf(out, out_sz, "System v%s", v);
}

static void centre_text(nd_draw *d, int32_t screen_w, int32_t y, const char *text,
                        const nd_font *f, nd_color colour)
{
    int32_t w = 0;
    int32_t h = 0;

    if (f == NULL)
        return;
    nd_text_size(f, text, &w, &h);
    (void)nd_draw_text(d, (screen_w - w) / 2, y, text, f, colour);
}

static void show_boot_logo(nd_fb *fb)
{
    const int32_t screen_w = ND_UI_W;
    const int32_t screen_h = ND_UI_H;
    nd_image *canvas;
    nd_draw draw;
    nd_font *bold;
    nd_font *regular;
    nd_font *fallback = NULL;
    char ver[64];
    int32_t title_y;

    canvas = nd_image_new_filled(screen_w, screen_h, ND_PIXFMT_RGB888, ND_BLACK);
    if (canvas == NULL)
        return;
    if (nd_draw_bind(&draw, canvas) != ND_OK) {
        nd_image_free(canvas);
        return;
    }

    /* nd_font_load() takes a REAL path and does not resolve ND_ROOT -- these
     * two live in the rootfs proper, not under /NeoDCT. */
    bold = nd_font_load(SPLASH_FONT_BOLD, 20);
    regular = nd_font_load(SPLASH_FONT_REGULAR, 14);
    if (bold == NULL || regular == NULL) {
        /* Python falls back to ImageFont.load_default(), a bundled bitmap face
         * C has no equivalent of. The NeoDCT font is the nearest honest
         * substitute; the splash is not a golden frame. See U-2 / P-6. */
        char font_path[ND_PATH_MAX];

        if (nd_path_resolve(font_path, sizeof font_path, ND_PATH_FONT) == ND_OK)
            fallback = nd_font_load(font_path, 20);
        nd_log(ND_LOG_LAUNCHER, "boot splash: DejaVu unavailable, using the UI face");
    }

    title_y = nd_max32(20, nd_trunc32((double)screen_h * 0.35)); /* = 61 */
    centre_text(&draw, screen_w, title_y, SPLASH_TITLE, bold != NULL ? bold : fallback, ND_WHITE);

    splash_version(ver, sizeof ver);
    centre_text(&draw, screen_w, title_y + 30, ver, regular != NULL ? regular : fallback,
                ND_GRAY);

    if (fb != NULL)
        (void)nd_fb_update(fb, canvas);

    nd_font_free(bold);
    nd_font_free(regular);
    nd_font_free(fallback);
    nd_image_free(canvas);
}

static void nap(double seconds)
{
    struct timespec ts;

    if (seconds <= 0.0)
        return;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* ------------------------------------------------------------------ *
 * The main loop (main.py:run)
 * ------------------------------------------------------------------ */

static void core_run(nd_fb *fb, bool idle_measure)
{
    nd_ui ui;

    /* First boot with an i2c keypad but no keymap runs the on-screen setup
     * wizard here, which exec-restarts the UI and may never return. It is
     * System/hw's, not this package's; when it lands it goes on this line. */

    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_CORE, "UI initialisation failed; nothing to run");
        return;
    }

    if (idle_measure) {
        /* Everything a booted phone has is now allocated: the canvas, the
         * scratch column, four faces, the wallpaper, the image cache and the
         * app list. This is the moment the RSS number means something. */
        (void)printf("[CORE] idle: initialised, %zu apps, holding for measurement\n", ui.n_apps);
        (void)fflush(stdout);
        while (g_quit == 0)
            nap(0.1);
        nd_ui_teardown(&ui);
        return;
    }

    nd_log(ND_LOG_CORE, "Entering Main Loop...");

    while (g_quit == 0) {
        int32_t key;

        nd_ui_update(&ui);
        nd_ui_show_pending_battery_warning(&ui);
        key = nd_ui_read_keypress(&ui, 0.1);
        if (key == ND_KEY_INCOMING_CALL) {
            /* Where the Python raised IncomingCall from inside read_keypress
             * and let it unwind to here. */
            nd_ui_handle_incoming_call(&ui, NULL);
        } else if (key != ND_KEY_NONE) {
            nd_log(ND_LOG_INPUT, "Code: %d", key);
            nd_ui_handle_input(&ui, key);
        }
    }

    nd_log(ND_LOG_CORE, "Leaving the main loop.");
    nd_ui_teardown(&ui);
}

/* ------------------------------------------------------------------ *
 * Entry
 * ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    (void)fprintf(out, "usage: nd-core [--headless] [--idle-measure] [--no-splash]\n"
                       "  --headless      do not open /dev/fb0; render into memory only\n"
                       "  --idle-measure  boot, report readiness, then hold still so a\n"
                       "                  caller can read /proc/<pid>/smaps_rollup\n"
                       "  --no-splash     skip the one-second boot splash\n");
}

int main(int argc, char **argv)
{
    bool headless = false;
    bool idle_measure = false;
    bool splash = true;
    nd_fb *fb = NULL;
    char serial[64];
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--idle-measure") == 0) {
            idle_measure = true;
            splash = false;
        } else if (strcmp(argv[i], "--no-splash") == 0) {
            splash = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            (void)fprintf(stderr, "nd-core: unknown option %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    /* 1. The serial console, before anything can print. Failure is logged and
     *    boot continues on whatever stdout already was. */
    (void)nd_log_redirect_serial(serial, sizeof serial);

    print_banner();
    install_signals();
    (void)nd_proc_reaper_start();

    /* 2. The clock floor, before anything can reach the network. */
    if (nd_clock_start != NULL)
        nd_clock_start(true, ND_NTP_SERVERS,
                       ND_NTP_SERVERS != NULL ? (size_t)ND_NTP_SERVER_COUNT : 0u);
    else
        nd_log(ND_LOG_CLOCK, "clock service unavailable: not linked in this build");

    /* 3. The remote shell, if somebody left it on. It comes up here rather
     *    than waiting for a route: the tunnel is a retry loop, and mobile data
     *    on this phone can take a minute to attach or never attach at all. */
    if (nd_rs_start_if_enabled != NULL)
        nd_rs_start_if_enabled();
    else
        nd_log(ND_LOG_RSHELL, "remote shell unavailable: not linked in this build");

    /* 4. Hardware. */
    nd_log(ND_LOG_LAUNCHER, "Initializing Hardware...");
    if (!headless) {
        if (nd_fb_open(&fb, ND_PATH_FB) != ND_OK) {
            nd_log_err(ND_LOG_FB, "no framebuffer; continuing without a panel");
            fb = NULL;
        }
    } else {
        nd_log(ND_LOG_FB, "headless: no panel will be written");
    }

    /* 5. The splash, then exactly one second. */
    if (splash) {
        show_boot_logo(fb);
        nap(SPLASH_SECONDS);
    }

    /* 6. The UI. */
    nd_log(ND_LOG_LAUNCHER, "Starting UI...");
    core_run(fb, idle_measure);

    nd_proc_reaper_stop();
    if (fb != NULL)
        nd_fb_close(fb);
    return 0;
}
