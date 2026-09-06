/* nd_crash.h -- what you see, and what gets written down, when something
 * breaks.
 *
 * ============ THE HONEST LIMITATION ============
 *
 * A Python traceback names the file, the line and the value of everything on
 * the way down. A C crash gives a signal number and an address. Process
 * isolation buys most of it back -- the core survives, and the crash screen
 * can say what died and how -- but the detail is genuinely reduced, and this
 * is the one place in the port where the crash LOG cannot be 1:1. The screen
 * can be, and is.
 *
 * ============ WHO WRITES WHAT ============
 *
 * The CHILD (nd-apprun) installs handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE
 * and SIGABRT. Each writes si_signo, si_code, si_addr and a backtrace to the
 * inherited crash-report descriptor USING ONLY async-signal-safe calls, then
 * restores the default disposition and re-raises -- so waitpid() in the core
 * still reports the real signal rather than a synthetic exit code.
 *
 * The CORE reads that report, fills in nd_crash_info, logs it and draws the
 * screen.
 *
 * ============ THE LOG ============
 *
 * /NeoDCT/User/logs/crash.log, rotated to crash.log.1 at 64 KiB, so the total
 * on disk is capped at 128 KiB. CrashHandler is the ONE writer in the project
 * that fsyncs the parent directory as well as the file -- a crash is exactly
 * the moment the power might go.
 */

#ifndef ND_CRASH_H_INCLUDED
#define ND_CRASH_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_CRASH_DEFAULT_NOTICE "An application has crashed."
#define ND_CRASH_LOG_MAX_BYTES  (64 * 1024)

/* BACKSPACE, ENTER, C, M, KP_ENTER. Any of them dismisses the crash screen. */
#define ND_CRASH_CONTINUE_KEY_COUNT 5

/* How long the core waits for a dying app's report before drawing the crash
 * screen without it. The write end of that pipe belongs to the app, and an
 * app that forks a helper leaves it open after it dies -- so "read until EOF"
 * is a phone an untrusted app can stop. Long enough for a signal handler that
 * has already run to have written one 88-byte record; short enough that
 * nobody waits for it. */
#define ND_CRASH_REPORT_WAIT_MS 250
extern const int32_t ND_CRASH_CONTINUE_KEYS[ND_CRASH_CONTINUE_KEY_COUNT];

#define ND_CRASH_DETAIL_MAX 512

/* Stands in for Python's sys.exc_info(). */
typedef struct {
    bool from_signal;
    int signo; /* WTERMSIG when from_signal   */
    int si_code;
    int exit_status;                  /* WEXITSTATUS otherwise       */
    void *fault_addr;                 /* si_addr when known, else NULL */
    char detail[ND_CRASH_DETAIL_MAX]; /* the child's own backtrace */
} nd_crash_info;

/* True when the phone is running without hardware -- the crash screen is
 * chattier there, because a developer is looking at it. */
bool nd_crash_is_simulation(void);

/* Append to the log, rotating first when needed. Returns the log path, or
 * NULL when nothing could be written. NEVER FAILS INTO THE CALLER: a crash
 * handler that can itself fail is worse than useless. */
const char *nd_crash_log(const char *source, const nd_crash_info *info, const char *note);

/* The crash screen: CRASH.jpg behind, the app's name, the signal, and a
 * softkey. Blocks until one of ND_CRASH_CONTINUE_KEYS. Pass NULL for message
 * to get ND_CRASH_DEFAULT_NOTICE. */
struct nd_ui;
void nd_crash_show_app(struct nd_ui *ui, const char *message, const char *app_name,
                       const nd_crash_info *info);

/* ------------------------------------------------------------------ *
 * The child half, in nd-apprun
 * ------------------------------------------------------------------ */

/* Install the fatal-signal handlers, writing reports to report_fd.
 * Async-signal-safe from that point on. */
nd_err nd_crash_install_child(int report_fd);

/* Read one report the child wrote. Returns false when the pipe was empty,
 * which is the normal case for a clean exit. */
bool nd_crash_read_report(int report_fd, nd_crash_info *out);

/* Name the entry point that is about to run, so a report says which one was
 * on the stack. Copied into the pre-built record; safe to call before the app
 * is loaded and never from a handler. */
void nd_crash_set_entry(const char *entry);

/* ---- ADDITIVE: three helpers the Python has as separate functions ----
 *
 * spec-build-test.md section 3.6's crash-screen recipe calls
 * CrashHandler._draw_engineering_crash_screen() DIRECTLY, without the wait --
 * so the C has to expose the same split or nd-shoot cannot reproduce the
 * frame. Nothing above changed. See the P-section of OPEN-QUESTIONS.md.
 */

/* _draw_engineering_crash_screen(ui, summary): CRASH.jpg resized to the whole
 * screen, a black strip with a one-line summary at (2,2) in font_s, a
 * "Continue" softkey, and exactly one present. Does not wait. */
void nd_crash_draw_engineering(struct nd_ui *ui, const char *summary);

/* The screen the core draws when its keypad never opened: nd_panic's LOOK --
 * the sick Nokia cropped to just the phone and pinned left, a text column on
 * black in the OS's own faces -- rather than the app crash screen. The words
 * are the difference: "Input failed to initialize", and no countdown, because
 * nothing is about to restart and fix it. reason == NULL draws the headline
 * alone (the first of nd_main.c's two frames); a second call with the reason
 * wraps it in below. Does not wait; nobody is coming to press a key. */
void nd_crash_draw_input_failure(struct nd_ui *ui, const char *reason);

/* _exc_summary()'s C equivalent: the child's detail when there is one, else a
 * line built from the signal or the exit status. Capped at 90 characters with
 * "..." at 87, exactly as the Python caps it. Returns the wanted length. */
size_t nd_crash_summary(const nd_crash_info *info, char *out, size_t out_sz);

/* "SIGSEGV", "SIGBUS", ... A string literal; never NULL. */
const char *nd_crash_signal_name(int signo);

#ifdef __cplusplus
}
#endif

#endif /* ND_CRASH_H_INCLUDED */
