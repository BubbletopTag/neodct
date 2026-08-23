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

#ifdef __cplusplus
}
#endif

#endif /* ND_CRASH_H_INCLUDED */
