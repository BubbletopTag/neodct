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
 *   5. "[Launcher] Starting UI...", then the loop.
 *
 * launcher.py had a step between 4 and 5: a "Starting NeoDCT..." splash and
 * then a sleep of exactly one second so it could be read. It is gone. The
 * initramfs draws its own boot screen now, so the splash was a second screen
 * saying less, and the sleep was a second of deliberately doing nothing.
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

#include "nd_broker.h"
#include "nd_clock.h"
#include "nd_crash.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypadsetup.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

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

/* ============ HOW LONG A LATE KEYPAD IS GIVEN ============
 *
 * A keypad is late far more often than it is absent. The i2c node registers
 * after userspace has started, udev applies its group in a subshell rcS
 * deliberately backgrounds, and the expander's rail takes tens of
 * milliseconds to come up -- so the honest answer to "is there a keypad" a
 * quarter of a second into the UI is "ask again". Twenty seconds is far past
 * every one of those (the background coldplug measures ~2.7 s) and still
 * short enough that a phone which genuinely has no keypad says so while the
 * owner is still holding it.
 *
 * Nothing is drawn during the grace window on purpose. Whatever the boot put
 * on the panel stays there, so a phone that recovers in two seconds never
 * showed an error at all -- which is the entire complaint this is answering.
 */
#define ND_CORE_INPUT_GRACE_S 20.0
#define ND_CORE_INPUT_RETRY_S 1.0

/* And how long the failure screen is held before nd-core gives the problem
 * back to the supervisor. Long enough to read and photograph; short enough
 * that a phone whose keypad needs the whole privileged boot re-run is not
 * sitting on a dead screen for a minute. The retry keeps running underneath
 * it, so a keypad that turns up during the hold is used, not ignored. */
#define ND_CORE_INPUT_HOLD_S 20.0

/* ============ WHY THE KEYLESS PHONE EXITS ============
 *
 * It used to hold: `while (g_quit == 0) nap(0.2);`, for ever. That looks like
 * patience and is actually a dead end. nd-core never returns, so
 * nd-crashguard.sh -- which is still ROOT, and which restarting would re-run
 * the entire privileged boot including the keypad bring-up and the first-boot
 * wizard -- never gets control back; and inittab's `::once:` will not respawn
 * run_neodct.sh either. The one screen that most needs the recovery machinery
 * was the one screen that could not reach it, and the owner's only way out
 * was the battery.
 *
 * So it exits, with a status that is not zero (which the guard reads as a
 * requested shutdown) and not a signal. The guard restarts nd-core as root,
 * up to NDGUARD_MAX times, and then draws its own halt screen -- a bounded
 * sequence that ends somewhere a person can act on, which the hold never did.
 *
 * The wizard has always done exactly this: nd_keypadsetup.c's restart_ui()
 * calls _exit(0) precisely so the root supervisor performs the restart. This
 * is the same decision for the same reason. */
#define ND_CORE_EXIT_NO_INPUT 90
/* the green the CORE tag uses, so the banner and the OS that follows it read
 * as one voice */
#define BANNER_COLOUR 46

static volatile sig_atomic_t g_quit;

/* ------------------------------------------------------------------ *
 * Signals
 * ------------------------------------------------------------------ */

/* ============ WHY A SIGNAL SETS A FLAG *AND* ARMS AN ALARM ============
 *
 * In Python a SIGINT raises KeyboardInterrupt, which unwinds from wherever the
 * process happens to be -- including from inside a modal dialog's key wait --
 * and `run()` re-raises it, so the UI process always dies. C has no unwinding,
 * and the flag below is only read by the two loops in core_run(). A core
 * sitting in the first-boot notice, or in any blocking widget, would ignore a
 * SIGTERM forever and have to be SIGKILLed, which is not something an init
 * system should have to do to shut a phone down.
 *
 * So the signal does both: it asks for a graceful exit, and it arms a two
 * second alarm that takes the process down if nobody noticed. A clean shutdown
 * finishes long before the alarm; a wedged one still goes. */
static void on_quit(int signo)
{
    ND_UNUSED(signo);
    g_quit = 1;
    (void)alarm(2);
}

static void on_alarm(int signo)
{
    static const char msg[] = "[CORE] shutdown flag ignored; exiting anyway\n";

    ND_UNUSED(signo);
    (void)!write(2, msg, sizeof msg - 1u);
    _exit(0);
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
    sa.sa_handler = on_alarm;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGALRM, &sa, NULL);

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

/* Construction step 13 draws a BLOCKING modal on a phone that has never been
 * booted, and waits for a key. That is correct, and it is also the end of any
 * unattended measurement: with no keypad attached nothing ever dismisses it.
 *
 * --idle-measure is explicitly a measurement mode with no user, and the state
 * worth measuring is a phone that HAS been booted before -- every byte the
 * notice would account for is freed again the moment it is dismissed. So the
 * acknowledgement is written first, exactly as nd_ui.c writes it, and the run
 * says out loud that it did.
 *
 * It is written into whatever ND_ROOT is in force, so a staged root stays
 * inside itself. */
static void ack_security_notice_for_measurement(void)
{
    char resolved[ND_PATH_MAX];
    FILE *f;

    if (nd_path_exists(ND_PATH_ACK_SECURITY))
        return;
    nd_log(ND_LOG_CORE, "idle-measure: acknowledging the first-boot notice so the "
                        "measurement does not sit on a modal");
    (void)nd_mkdir_p(ND_PATH_USER, 0755u);
    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_ACK_SECURITY) != ND_OK)
        return;
    f = fopen(resolved, "w");
    if (f != NULL) {
        (void)fputs("0", f);
        (void)fclose(f);
    }
}

/* nd_ui.c's t9_active(), which is static there and belongs to another work
 * package. The duplicate is deliberate and small: when a keypad turns up
 * during the grace window below, ui.has_matrix_keypad has already been
 * decided -- as false -- and a phone that came alive with an i2c matrix and a
 * false T9 flag would put every text field on the QWERTY path and type
 * gibberish. Getting the override wrong in the other direction would be just
 * as bad, so the rule is copied rather than approximated. */
static bool t9_active_for_recovery(bool detected)
{
    const char *env = getenv("NEODCT_T9");

    if (env == NULL || env[0] == '\0')
        return detected;
    return !(env[0] == '0' && env[1] == '\0');
}

/* Sit on a frame for `seconds`, or until a signal asks for a shutdown. Used
 * only by the forced-failure developer flag, which has nothing to retry. */
static void hold_still(double seconds)
{
    double held = 0.0;

    while (g_quit == 0 && held < seconds) {
        nap(0.2);
        held += 0.2;
    }
}

/* Ask the input layer, once a second for `seconds`, whether a backend can be
 * opened now that could not be a moment ago. True as soon as one is.
 *
 * This is the call 0.5.7b's self-heal was missing. The retry it added lived
 * inside nd_input_read_event(), and a core with no backend at all never
 * reaches its read loop -- so on a Luckfox, the only phone without a spare
 * evdev keyboard to fall back on, the recovery written for the udev race
 * could not execute even once. */
static bool wait_for_input_backend(nd_ui *ui, double seconds)
{
    double waited = 0.0;

    while (g_quit == 0 && waited < seconds) {
        nap(ND_CORE_INPUT_RETRY_S);
        waited += ND_CORE_INPUT_RETRY_S;
        if (!nd_input_retry_backend(ui->input))
            continue;

        nd_log(ND_LOG_INPUT, "Input recovered after %.0fs: %s", waited,
               nd_input_has_matrix(ui->input) ? "i2c keypad matrix" : "evdev");
        /* Both were decided in nd_ui_init() from an input that had nothing
         * behind it, and both are wrong now. */
        ui->keypad_fd = nd_input_fd(ui->input);
        if (nd_input_has_matrix(ui->input))
            ui->has_matrix_keypad = t9_active_for_recovery(true);
        return true;
    }
    return false;
}

static int core_run(nd_fb *fb, bool idle_measure)
{
    nd_ui ui;

    if (idle_measure)
        ack_security_notice_for_measurement();

    /* First-boot keypad setup used to run here. It now runs in main(), BEFORE
     * the privilege drop, so its i2c probe happens as root -- see "4a-bis".
     * By the time core_run() is reached the UI is ndusr, which is exactly why
     * the wizard could not do its job from here. */

    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_CORE, "UI initialisation failed; nothing to run");
        return 0;
    }

    if (idle_measure) {
        /* Everything a booted phone has is now allocated: the canvas, the
         * scratch column, four faces, the wallpaper, the image cache and the
         * app list. This is the moment the RSS number means something.
         *
         * The last three are lazy now (nd_ui.h, "Lazy home state"), and a
         * phone sitting on its home screen has certainly loaded them -- so
         * ask for them here rather than reporting an idle figure no real
         * device would ever show. */
        size_t n_apps = 0u;

        (void)nd_ui_wallpaper(&ui);
        (void)nd_ui_home_layout(&ui);
        (void)nd_ui_app_list(&ui, &n_apps);
        (void)printf("[CORE] idle: initialised, %zu apps, holding for measurement\n", n_apps);
        (void)fflush(stdout);
        while (g_quit == 0)
            nap(0.1);
        nd_ui_teardown(&ui);
        return 0;
    }

    /* ============ A LATE KEYPAD IS NOT AN ABSENT ONE ============
     *
     * A phone whose keypad never came up cannot be driven, and a home screen
     * that ignores every key looks like a freeze -- so there is a screen for
     * it: the sick-Nokia panic with "Input failed to initialize" and the
     * reason underneath. The screen is right. WHEN it was drawn was the bug.
     *
     * It was drawn from a single verdict taken at the first instant the UI
     * wanted a key, from an nd_input that had had exactly one attempt at each
     * backend. On a Luckfox nearly everything that can go wrong in that
     * instant is a race that resolves by itself a second or two later -- the
     * i2c node's group, the expander's rail, i2c-dev registering -- and every
     * one of them put this screen in front of the owner of a working phone.
     * Three releases of point fixes did not close it because each one made
     * the single verdict slightly more likely to be right, rather than making
     * it stop being a single verdict.
     *
     * So: ask again, once a second, for twenty seconds, drawing nothing (the
     * boot screen stays up and the phone looks like it is still starting,
     * which it is). Only a phone that is still keyless after all of that gets
     * told it has no keypad -- and even then the retry keeps running behind
     * the screen, and even then the screen is not the end: see
     * ND_CORE_EXIT_NO_INPUT.
     *
     * NEODCT_FORCE_NO_INPUT=1 forces the screen for a screenshot. It skips
     * the RETRY deliberately -- it is a static flag and nothing could ever
     * clear it, so asking would only make the developer wait -- but it still
     * holds the screen and still exits the same way, because a developer
     * looking at this screen wants to see what the phone does, not a
     * simulation of it. */
    if (!nd_input_has_backend(ui.input) || getenv("NEODCT_FORCE_NO_INPUT") != NULL) {
        bool forced = getenv("NEODCT_FORCE_NO_INPUT") != NULL;
        bool recovered = false;
        const char *why;

        if (!forced) {
            nd_log_err(ND_LOG_INPUT,
                       "no input backend at UI start (%s); waiting up to %.0fs "
                       "for a late keypad before saying so",
                       nd_input_no_backend_reason(ui.input), ND_CORE_INPUT_GRACE_S);
            recovered = wait_for_input_backend(&ui, ND_CORE_INPUT_GRACE_S);
        }

        if (!recovered && g_quit == 0) {
            why = nd_input_no_backend_reason(ui.input);
            if (why == NULL || why[0] == '\0')
                why = "No keypad and no keyboard were found.";
            nd_log_err(ND_LOG_INPUT, "no input backend after %.0fs: %s", ND_CORE_INPUT_GRACE_S,
                       why);

            /* The panic-styled screen, in two frames sharing one picture: the
             * headline and the cropped sick Nokia first, then -- once the
             * owner has had a moment to read it -- the same picture with the
             * reason wrapped in below. Neither waits; nobody can dismiss a
             * keyless phone. */
            nd_crash_draw_input_failure(&ui, NULL);
            nap(3.0);
            nd_crash_draw_input_failure(&ui, why);

            /* THE SCREEN IS NOT A DEAD END. The retry runs underneath it for
             * the whole hold, so a keypad that finally answers is used and
             * the phone walks straight out of the panic and into its home
             * screen. */
            if (forced)
                hold_still(ND_CORE_INPUT_HOLD_S);
            else
                recovered = wait_for_input_backend(&ui, ND_CORE_INPUT_HOLD_S);
        }

        if (!recovered) {
            if (g_quit != 0) {
                /* A poweroff arrived while we were waiting. That is a clean
                 * exit, not a keypad verdict. */
                nd_ui_teardown(&ui);
                return 0;
            }
            nd_log_err(ND_LOG_CORE,
                       "exiting %d so the supervisor re-runs the privileged boot; the "
                       "keypad cannot be repaired from inside a UI that is not root",
                       ND_CORE_EXIT_NO_INPUT);
            nd_ui_teardown(&ui);
            return ND_CORE_EXIT_NO_INPUT;
        }
    }

    nd_log(ND_LOG_CORE, "Entering Main Loop...");

    while (g_quit == 0) {
        int32_t key;

        nd_ui_update(&ui);
        nd_ui_show_pending_battery_warning(&ui);
        nd_ui_show_pending_modem_fault(&ui);
        /* 0.1 s unless an animated wallpaper owes a frame sooner. A still
         * wallpaper, or none, gets exactly the poll this loop always had. */
        key = nd_ui_read_keypress(&ui, nd_ui_frame_timeout(&ui, 0.1));
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
    return 0;
}

/* ------------------------------------------------------------------ *
 * Entry
 * ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    (void)fprintf(out, "usage: nd-core [--headless] [--idle-measure]\n"
                       "  --headless      do not open /dev/fb0; render into memory only\n"
                       "  --idle-measure  boot, report readiness, then hold still so a\n"
                       "                  caller can read /proc/<pid>/smaps_rollup\n");
}

int main(int argc, char **argv)
{
    nd_broker *broker = NULL;
    bool headless = false;
    bool idle_measure = false;
    nd_fb *fb = NULL;
    char serial[64];
    int status;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--idle-measure") == 0) {
            idle_measure = true;
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

    /* 1b. The broker, BEFORE any thread exists.
     *
     *     Forking a process that has threads gives the child locks no thread
     *     will ever release -- malloc's among them -- and the broker
     *     allocates. The clock service below starts a thread, so the fork
     *     goes above it, not beside the drop it enables. */
    broker = nd_broker_start();
    /* Installed as the default only if the drop below actually happens. A
     * core that is STILL ROOT gains nothing by asking a root broker to fork
     * for it, and it loses something: the broker refuses a spawn whose user
     * cannot be resolved (nd_broker.c), which on an image with no users table
     * would be every app on the phone. Routing a privileged core straight to
     * nd_proc_spawn keeps that image booting exactly as it did before, loudly
     * logged, while the broker's invariant stays absolute for the case it is
     * actually for. */

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

    /* 4a-bis. First-boot keypad setup, BEFORE the drop, so it runs as root.
     *
     *     The wizard probes /dev/i2c-3 for the keypad expander and writes the
     *     keymap. Setting the i2c bus up and opening it wants privileges the
     *     UI is about to give away, and on a fresh phone the probe as ndusr
     *     was finding nothing -- no keymap, then no keypad at all. As root the
     *     probe just works. If a keymap gets written the wizard exits for the
     *     supervisor to restart nd-core as root, so this returns only when
     *     there is nothing to set up. It needs the framebuffer above; on a
     *     phone with no i2c bus (QEMU) it gates itself off and is silent. */
    if (fb != NULL)
        (void)nd_kpsetup_maybe_run(fb, true);

    /* 4a-ter. AND THE OTHER HALF: bring an ALREADY CONFIGURED keypad up,
     *     still as root, and keep the descriptor.
     *
     *     Step 4a-bis above only does anything on a phone with no keymap. The
     *     instant one exists -- which is every phone after first boot, i.e.
     *     every phone the owner actually uses -- nd_kpsetup_maybe_run()
     *     returns at its HAVE_KEYMAP gate having opened no bus at all, and
     *     the only open of /dev/i2c-3 for the keypad happened afterwards,
     *     inside nd_ui_init(), as ndusr, once, against a node whose group
     *     udev may not have applied yet. That is the race, and no retry
     *     placed after the drop can be sure of winning it.
     *
     *     So the bus is opened HERE, where the process still has the
     *     privilege, with a bounded wait for the node and a bounded probe
     *     that makes the expander answer before anything is decided. The
     *     descriptor is then handed to nd_input, which prefers it over
     *     opening the node itself. open(2) checks permission once, at open
     *     time, and the I2C_SLAVE address is per-descriptor state -- so the
     *     keypad crosses the setuid intact and the steady-state UI never
     *     needs group i2c on that node at all.
     *
     *     It is deliberately BEFORE the drop and deliberately AFTER the
     *     wizard: a fresh phone gets its keymap written first, and on that
     *     boot the wizard exits for the supervisor anyway. */
    {
        nd_kpsetup_bringup keypad;
        int kfd = nd_kpsetup_open_keypad_as_root(&keypad);

        if (kfd >= 0)
            nd_input_provide_keypad_fd(kfd, keypad.bus, keypad.addr);
    }

    /* 4b. AND NOW STOP BEING ROOT.
     *
     *     What nd-core actually needed uid 0 for was measured rather than
     *     reasoned about: it was made to become ndusr at startup and run on a
     *     booted phone. The panel, the keypad, the modem, the battery, the
     *     settings, the fonts and the wallpaper all came up unchanged --
     *     everything the UI touches is group-reachable to ndusr already, by
     *     the layout in 61-neodct-devices.rules. Exactly one thing broke:
     *     launching an app, exit 122, ND_PRIV_STEP_SETGROUPS, because
     *     setgroups() needs CAP_SETGID.
     *
     *     So the privilege that kept the whole UI at uid 0 was one syscall
     *     between fork and execve. The broker forked above holds it. This is
     *     where the rest of the phone gives it up.
     *
     *     ONLY WITH A BROKER. Without one there would be nothing left that
     *     can launch an app, and a phone whose apps do not open is worse than
     *     a phone whose UI is privileged. NEODCT_NO_DROP=1 keeps the old
     *     behaviour for a developer bisecting something. */
    if (broker == NULL) {
        nd_log_err(ND_LOG_CORE, "no broker: the UI stays root, because nothing "
                                "else could launch an app");
    } else if (getenv("NEODCT_NO_DROP") != NULL) {
        nd_log(ND_LOG_CORE, "NEODCT_NO_DROP: staying root by request");
    } else {
        nd_priv_id id;

        if (!nd_priv_lookup(ND_PRIV_USER, &id)) {
            nd_log_err(ND_LOG_CORE, "no " ND_PRIV_USER " in this image; the UI stays root");
        } else {
            int step = nd_priv_become(&id);

            if (step != 0)
                nd_log_err(ND_LOG_CORE,
                           "could not become " ND_PRIV_USER " (step %d: %s); the UI stays root",
                           step, strerror(errno));
            else {
                /* Only here. See the fork above. */
                nd_broker_set_default(broker);
                nd_log(ND_LOG_CORE, "UI is now uid %ld; privilege lives in the broker only",
                       (long)getuid());
            }
        }
    }

    /* 5. The UI. Nothing between the framebuffer and here: see the header
     *    comment for what used to be, and why it is not. */
    nd_log(ND_LOG_LAUNCHER, "Starting UI...");
    status = core_run(fb, idle_measure);

    nd_proc_reaper_stop();
    nd_broker_set_default(NULL);
    nd_broker_stop(broker);
    if (fb != NULL)
        nd_fb_close(fb);
    /* Not always zero any more: ND_CORE_EXIT_NO_INPUT tells nd-crashguard.sh
     * that this boot ended with no keypad, so that it restarts the phone --
     * as root -- rather than leaving it on a screen nothing can dismiss. */
    return status;
}
