/* apps/LinuxShell/main.c -- hand the screen to fbcon and run /bin/sh, app 999.
 *
 * A one-to-one port of System/engineering/apps/LinuxShell/main.py (120
 * lines). Its docstring is the specification and is reproduced rather than
 * summarised, because every line of it is a constraint somebody arrived at
 * the hard way:
 *
 *     Raw console shell on a real /dev/ttyN, shown on-device.
 *     No VT ioctls, no KDSETMODE, no openvt (avoids common hangs).
 *     Requires:
 *       - fbcon enabled
 *       - /dev/ttyN exists
 *       - chvt available (busybox provides it on many systems)
 *
 * IT IS NOT A TERMINAL EMULATOR AND IT DRAWS NOTHING. It switches virtual
 * terminals, writes four byte strings at the console, execs /bin/sh -i
 * attached to that terminal, waits, and switches back. The font, the
 * scrollback, the cursor and every escape sequence are fbcon, inside the
 * kernel. There is no nd_ui call anywhere below, and no frame to compare.
 *
 * ============ FAILURE IS SILENT, ON PURPOSE ============
 *
 * A missing `chvt`, a VT switch that will not go, a console that cannot be
 * opened: every one of them returns to the menu with nothing on screen. The
 * Python's own comment says why -- "keep the UI responsive rather than trying
 * risky ioctls" -- and spec-engineering.md's risk table says to preserve it.
 * So the C preserves it, and adds ONE THING: a line on the serial console
 * saying which of them happened, because the same risk table asks for that
 * and a log line moves no pixel. The screen behaviour is the Python's.
 *
 * ============ THIS APP NO LONGER MAKES A KEYBOARD ============
 *
 * In Python the app ran inside the core, so it could own a daemon thread,
 * scan the i2c expander directly and open /dev/uinput itself. Every part of
 * that has moved. The core reads the keypad and forwards presses and releases
 * down an inherited pipe, and the core -- which is the only process on this
 * phone allowed to inject keys -- creates the virtual keyboard, drives the
 * shell bridge over it and publishes the node it made. This app asks for one
 * with two lines of manifest.json:
 *
 *     "useKeypadDevice": true, "keypadDeviceMap": "shell"
 *
 * and that is the whole of its side. The map is the same nd_t9_bridge kind
 * this file used to build by hand -- multi-tap letters and * as Tab -- so
 * nothing about what the keys DO has changed.
 *
 * ============ WHY REMOVE CODE THAT WORKED ============
 *
 * The hand-rolled version was not broken. LinuxShell is an engineering app
 * and nd_proc_app_needs_root() runs it as root, so its nd_uinput_open()
 * succeeded, and it is the ONLY app on the phone for which that was still
 * true. That is the reason to take it out rather than a reason to keep it:
 * it was the last copy of the thing this release exists to delete, it would
 * have gone silently keyless the day engineering apps stop being root, and
 * the failure would have looked like the i2c keypad rather than like this.
 * There is now one path to a virtual keyboard on this phone and one process
 * that may take it. See nd_proc.h, THE KEY DEVICE.
 *
 * Nothing here reads the node the core made: /bin/sh takes its input from the
 * tty, and the kernel's own console keyboard handler binds the new device to
 * the VT the moment it appears. nd_app_key_evdev() is consulted only to know
 * whether that happened, which is what decides whether the T9 hint below is
 * true enough to print.
 *
 * ============ THE finally: BLOCK IS THE IMPORTANT PART ============
 *
 * `finally:` runs the teardown -- hide the cursor, switch back to the UI VT,
 * settle -- whether the shell exited normally or the `with open(...)` raised.
 * (The Python stopped its bridge there too; there is no longer one here to
 * stop.) C has no unwinding, so it is a labelled tail every
 * path falls into, and the SIGTERM path falls into it too: if the phone rings
 * while the shell has the screen, the wait loop notices, kills the shell and
 * goes through the SAME teardown, so the incoming-call screen is drawn on the
 * VT that is actually visible. Getting that wrong rings a phone at a black
 * console. nd_app.h's teardown contract, done in app_run() where it is
 * allowed to take a moment, rather than in app_shutdown() where it is not.
 *
 * The two returns ABOVE the try -- no chvt, and a chvt that failed -- do NOT
 * run the teardown, because in the Python they are before the `try`. Nothing
 * has been switched or opened at that point, so there is nothing to undo.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_input.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#include "linuxshell.h"

extern char **environ;

/* Not in nd_log.h's named palette: that is for the core's own subsystems, and
 * an unregistered tag gets a derived colour that is stable from its first
 * boot, which is what the header says the mechanism is for. */
#define LS_LOG_TAG "LinuxShell"

/* 50 ms between wakeups while the shell owns the screen -- short enough that
 * a keypad press reaches the console without a perceptible lag, long enough
 * that a shell session costs no measurable CPU in this process. The same
 * number, for the same reason, as the Browser's POLL_MS. */
#define LS_POLL_MS 50

/* Bounds the drain, nothing more: a burst of held-key repeats between two
 * polls can be long, and a loop that can spin forever on a misbehaving
 * descriptor is not worth the one line it takes to prevent. */
#define LS_PUMP_MAX_KEYS 256

/* The pid of /bin/sh, so app_shutdown() can kill it when an incoming call
 * arrives. sig_atomic_t because the SIGTERM path reads it; 0 means there is
 * no child. See the teardown contract in nd_app.h. */
static volatile sig_atomic_t g_sh_pid;

/* ------------------------------------------------------------------ *
 * The four byte strings
 * ------------------------------------------------------------------ */

const char *const nd_linuxshell_cursor_on = "\x1b[?25h";
const char *const nd_linuxshell_cursor_off = "\x1b[?25l";
const char *const nd_linuxshell_hint = "Type exit to go back to the NeoDCT UI\r\n\r\n";

/* Two adjacent byte literals in the Python, concatenated by the parser. The
 * text names the mapping the bridge implements and must not drift from it. */
const char *const nd_linuxshell_t9_hint =
    "T9 keypad active: 2-9 letters, 0 space, 1 symbols, # mode, C backspace\r\n\r\n";

/* ------------------------------------------------------------------ *
 * _which()
 * ------------------------------------------------------------------ */

bool nd_linuxshell_which(const char *name, char *out, size_t out_sz)
{
    const char *path;
    const char *seg;

    if (name == NULL || name[0] == '\0' || out == NULL || out_sz == 0u)
        return false;

    /* shutil.which(): a name containing a separator is used as given. */
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) != 0)
            return false;
        return (size_t)snprintf(out, out_sz, "%s", name) < out_sz;
    }

    path = getenv("PATH");
    /* shutil.which()'s own fallback is os.defpath (":/bin:/usr/bin") when
     * PATH is unset; this is execvp's confstr(_CS_PATH), which is what every
     * other lookup in the project uses and is a superset of it. */
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

    for (seg = path; seg != NULL;) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        int n;

        /* An empty element means the current directory, as it does to the
         * shell and to execvp. */
        if (len == 0u)
            n = snprintf(out, out_sz, "./%s", name);
        else
            n = snprintf(out, out_sz, "%.*s/%s", (int)len, seg, name);

        if (n > 0 && (size_t)n < out_sz && access(out, X_OK) == 0)
            return true;

        seg = (colon != NULL) ? colon + 1 : NULL;
    }

    out[0] = '\0';
    return false;
}

/* ------------------------------------------------------------------ *
 * int(os.environ.get(...))
 * ------------------------------------------------------------------ */

bool nd_linuxshell_vt(const char *name, int32_t fallback, int32_t *out)
{
    const char *s;
    char *end = NULL;
    long v;

    if (out == NULL)
        return false;
    *out = fallback;
    if (name == NULL)
        return true;

    s = getenv(name);
    /* The DEFAULT arm of os.environ.get(name, "2"). An UNSET variable takes
     * the default; a variable set to "" does not -- int("") is a ValueError,
     * and it is caught below with every other unparseable value. */
    if (s == NULL)
        return true;

    errno = 0;
    v = strtol(s, &end, 10);
    /* strtol skips leading whitespace and int() strips it at both ends, so
     * only the tail has to be checked by hand. Python also allows underscores
     * between digits ("1_0" is 10) and this does not; same deviation, same
     * reasoning, as OPEN-QUESTIONS.md M-10. */
    if (end == s)
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\v' || *end == '\f' ||
           *end == '\r')
        end++;
    if (*end != '\0')
        return false;

    /* Python's int has no range. A VT number too big for one is nonsense
     * either way, and SATURATING reproduces what the Python does with it --
     * chvt refuses it, /dev/tty<huge> does not exist, every write and the
     * open fail silently, and the app comes back having done nothing --
     * whereas reporting it as a ValueError would put a crash screen where the
     * Python has a quiet no-op. The saturation itself is not the Python's;
     * the behaviour it produces is. */
    if (errno == ERANGE || v > (long)INT32_MAX || v < (long)INT32_MIN)
        v = (v < 0) ? (long)INT32_MIN : (long)INT32_MAX;

    *out = (int32_t)v;
    return true;
}

nd_err nd_linuxshell_tty_path(char *out, size_t out_sz, int32_t vt)
{
    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    return nd_snprintf(out, out_sz, "/dev/tty%d", (int)vt);
}

/* ------------------------------------------------------------------ *
 * _write_tty()
 * ------------------------------------------------------------------ */

void nd_linuxshell_write_tty(const char *path, const char *data)
{
    char resolved[ND_PATH_MAX];
    size_t len;
    size_t done = 0u;
    int fd;

    if (path == NULL || data == NULL)
        return;
    len = strlen(data);
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return; /* except Exception: pass */

    /* open(path, "wb", buffering=0): O_WRONLY|O_CREAT|O_TRUNC, 0666 before
     * umask. O_TRUNC means nothing on a character device, and it is kept
     * because "wb" is what the Python asked for and because under a test
     * root the target is an ordinary file, where it is the difference between
     * reproducing the Python and not. */
    fd = open(resolved, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
        return;

    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; /* the IOError _write_tty swallows */
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    /* f.flush() on an unbuffered file is a no-op; the close is the `with`. */
    (void)close(fd);
}

/* ------------------------------------------------------------------ *
 * _run_quiet()
 * ------------------------------------------------------------------ */

bool nd_linuxshell_run_quiet(const char *const *argv, double timeout_s)
{
    char devnull_path[ND_PATH_MAX];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int devnull;
    bool ok = false;

    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0')
        return false;

    /* subprocess.run() raises FileNotFoundError or PermissionError BEFORE the
     * child exists, and the bare `except` turns both into False.
     * nd_proc_spawn() cannot: the fork succeeds and the failed execve is a
     * child that exits 127, which is indistinguishable from a program that
     * chose to exit 127. So the lookup is done here instead, which is the
     * same answer for every case that reaches this function -- argv[0] has
     * already been through nd_linuxshell_which(). Same shape as
     * OPEN-QUESTIONS.md PW-1. */
    if (access(argv[0], X_OK) != 0)
        return false;

    if (nd_path_resolve(devnull_path, sizeof devnull_path, "/dev/null") != ND_OK)
        return false;
    devnull = open(devnull_path, O_WRONLY | O_CLOEXEC);
    /* subprocess.DEVNULL is opened inside subprocess.run, so a failure here
     * is one of the exceptions the bare `except` turns into False. */
    if (devnull < 0)
        return false;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.envp = NULL; /* env=None -- inherit, as subprocess.run does */
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = devnull;
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = devnull;
    spec.n_fds = 2u;

    if (nd_proc_spawn(argv[0], &spec, &pid) == ND_OK) {
        memset(&st, 0, sizeof st);
        if (nd_proc_wait(pid, timeout_s, &st) == ND_OK) {
            /* check=False: a non-zero exit is still True. Only an exception
             * escaping subprocess.run is False, which is why the status is
             * read and then deliberately ignored. */
            ok = true;
        } else {
            /* TimeoutExpired. subprocess.run kills the child before it
             * re-raises; nd_proc_terminate asks with SIGTERM first and
             * escalates, which is the same end with one more courtesy. */
            (void)nd_proc_terminate(pid, 0.2, &st);
        }
    }

    (void)close(devnull);
    return ok;
}

/* ------------------------------------------------------------------ *
 * env.copy() + PS1 + TERM
 * ------------------------------------------------------------------ */

const char **nd_linuxshell_build_envp(void)
{
    static const char ps1_entry[] = "PS1=" ND_LINUXSHELL_PS1;
    static const char term_entry[] = "TERM=" ND_LINUXSHELL_TERM;
    /* owned by the caller; free the array with free(). The strings are
     * environ's own and two literals, and neither is ours to release. */
    const char **envp;
    size_t n = 0u;
    size_t i;
    size_t k = 0u;

    for (i = 0u; environ[i] != NULL; i++)
        n++;

    envp = calloc(n + 3u, sizeof *envp);
    if (envp == NULL)
        return NULL;

    for (i = 0u; i < n; i++) {
        /* env["PS1"] = ... REPLACES the entry. An envp that carried both the
         * inherited value and ours would leave which one the shell sees up to
         * the libc. Python's dict keeps an overridden key in its original
         * position and this appends instead; execve does not care where in
         * the array an entry is. */
        if (strncmp(environ[i], "PS1=", 4u) == 0 || strncmp(environ[i], "TERM=", 5u) == 0)
            continue;
        envp[k++] = environ[i];
    }
    envp[k++] = ps1_entry;
    envp[k++] = term_entry;
    envp[k] = NULL;
    return envp;
}

/* ------------------------------------------------------------------ *
 * _start_t9_bridge()'s two questions, which are not the same question
 * ------------------------------------------------------------------ *
 *
 * The Python asked one -- "is there a matrix keypad? then build a bridge" --
 * because the answer and the act were both its own. They are two processes
 * apart now. What the HARDWARE is, this app can still work out for itself;
 * whether a keyboard was actually made for it is the core's answer and only
 * the core's, and a phone can say yes to the first and no to the second.
 * That gap is worth a log line rather than silence, which is what app_run()
 * does with the pair. */

bool nd_linuxshell_needs_key_bridge(const nd_ui *ui)
{
    ND_UNUSED(ui);

    /* The core hands the fact down in NEODCT_KEYPAD_MATRIX (nd_app.h). This
     * used to re-read keymap.json instead, because ui->has_matrix_keypad was
     * false in every app process on every device -- BR-3, now closed.
     *
     * Asking the core is not merely tidier, it is more correct: a keymap.json
     * on disk is a CLAIM about the hardware, while this is the backend the
     * core actually opened. If the matrix failed and the core fell back to
     * evdev, the old code would have started a bridge against a keyboard that
     * is already reaching the console, and doubled every press.
     *
     * nd_app_keypad_is_matrix() and NOT ui->has_matrix_keypad, even though
     * the second is derived from the first: only the second has the NEODCT_T9
     * override folded into it, and this question is about the hardware. A
     * developer forcing T9 on over a real keyboard must not thereby get a
     * second keyboard bridged on top of it. */
    return nd_app_keypad_is_matrix();
}

/* Is on-device typing actually going to work in the session about to start?
 *
 * The core answers, and only the core can: it is the process that decided
 * whether this phone's keypad is the i2c matrix, tried to create the virtual
 * keyboard, and waited for udev to make its node readable. A non-NULL node is
 * that whole sequence having succeeded.
 *
 * "the shell still works, just without on-device typing" -- the Python's own
 * comment on the bare `except: return None` this replaces. A false answer is
 * not an error and does not stop the shell: an engineer with a serial cable
 * has a keyboard already, and that is who the missing case is usually. */
static bool t9_typing_available(void)
{
    return nd_app_key_evdev() != NULL;
}

/* ------------------------------------------------------------------ *
 * p.wait()
 * ------------------------------------------------------------------ */

/* Empty the core's key channel and throw the presses away.
 *
 * DRAINING IS THE WHOLE JOB NOW, and it is not optional. The channel is a
 * pipe with a finite buffer that the core writes to for as long as this app
 * holds the screen; a shell session is easily long enough to fill it, and a
 * full pipe blocks the core's own loop. This used to hand each press to a
 * bridge on the way past. The bridge is the core's, so the presses have
 * already reached the console by the time they arrive here, and reading them
 * again is only how the pipe stays empty.
 *
 * read_EVENT, not read_key: nd_input_read_key(in, 0.0) returns ND_KEY_NONE as
 * soon as it consumes a RELEASE, so a loop that stops at the first NONE stops
 * after the first press of every pair. The Python's i2c scanner reported no
 * releases at all; this channel reports both. */
static int pump_keys(nd_input *input)
{
    int i;

    if (input == NULL)
        return 0;
    for (i = 0; i < LS_PUMP_MAX_KEYS; i++) {
        nd_key_event ev;

        if (!nd_input_read_event(input, 0.0, &ev))
            break;
    }
    return i;
}

static void nap_ms(long ms)
{
    struct timespec req;

    req.tv_sec = 0;
    req.tv_nsec = ms * 1000000L;
    (void)nanosleep(&req, NULL);
}

/* p.wait(), plus the two things a separate process has to do that the Python
 * did not: keep the core's key channel drained while the shell runs, and
 * notice SIGTERM. */
static void wait_for_shell(pid_t pid, nd_input *input)
{
    int input_fd = (input != NULL) ? nd_input_fd(input) : -1;

    for (;;) {
        nd_proc_status st;
        struct pollfd pfd;
        int r;

        memset(&st, 0, sizeof st);
        if (nd_proc_wait(pid, 0.0, &st) == ND_OK)
            return; /* the shell exited: `exit`, or a signal */

        if (nd_app_should_exit()) {
            /* The phone is ringing. Take the screen back now rather than
             * waiting out a shell nobody is sitting in front of any more; the
             * teardown below this then puts the UI VT back. */
            (void)nd_proc_terminate(pid, 1.0, &st);
            return;
        }

        if (input_fd < 0) {
            nap_ms(LS_POLL_MS);
            continue;
        }

        pfd.fd = input_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1u, LS_POLL_MS);
        if (r < 0) {
            /* nd-apprun's SIGTERM handler is installed WITHOUT SA_RESTART
             * precisely so this returns rather than resuming; the top of the
             * loop is where that is acted on. */
            if (errno == EINTR)
                continue;
            input_fd = -1;
            continue;
        }
        if (r > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            /* A hangup with nothing left to read means the core has gone.
             * Stop polling the descriptor rather than spinning on it for the
             * rest of the session -- POLLHUP is level-triggered and would
             * otherwise make this loop the busiest thing on the phone. */
            if (pump_keys(input) == 0 && (pfd.revents & (POLLHUP | POLLERR)) != 0)
                input_fd = -1;
        }
    }
}

/* time.sleep(0.05) -- "Small settle time before UI redraws". Skipped under
 * the virtual clock for the same reason PhoneBook's dwell() is
 * (OPEN-QUESTIONS.md PB-3): in capture mode time is a frame counter and a
 * real sleep moves no pixel, it only makes the oracle slower. */
static void settle(void)
{
    if (nd_vclock_enabled())
        return;
    nap_ms((long)(ND_LINUXSHELL_SETTLE_S * 1000.0));
}

/* ------------------------------------------------------------------ *
 * run(ui)
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    char chvt_path[ND_PATH_MAX];
    char tty_shell[ND_PATH_MAX];
    char resolved_tty[ND_PATH_MAX];
    char vt_arg[16];
    const char *chvt_argv[3];
    const char *sh_argv[3];
    const char **envp = NULL;
    nd_proc_spec spec;
    int32_t shell_vt = ND_LINUXSHELL_DEFAULT_SHELL_VT;
    int32_t ui_vt = ND_LINUXSHELL_DEFAULT_UI_VT;
    int tty_fd = -1;
    pid_t pid = -1;

    /* A NULL ui is survivable here and is NOT the error it is in every app
     * that draws: run(ui) reads one field of it, ui->input, and asks one
     * question that does not look at it at all -- whether this phone's keypad
     * is the matrix, which the core answered in the environment. "No context
     * at all" is a shell with no key channel to drain, which is exactly what
     * the poll loop does with a NULL input. */

    /* The two int() calls, BEFORE the chvt lookup, as the Python has them --
     * so a bad NEODCT_SHELL_VT crashes on an image with no chvt too. See
     * linuxshell.h for why a ValueError is a non-zero return. */
    if (!nd_linuxshell_vt(ND_LINUXSHELL_ENV_SHELL_VT, ND_LINUXSHELL_DEFAULT_SHELL_VT, &shell_vt) ||
        !nd_linuxshell_vt(ND_LINUXSHELL_ENV_UI_VT, ND_LINUXSHELL_DEFAULT_UI_VT, &ui_vt)) {
        nd_log_err(LS_LOG_TAG, "%s / %s must be whole numbers", ND_LINUXSHELL_ENV_SHELL_VT,
                   ND_LINUXSHELL_ENV_UI_VT);
        return 1;
    }

    /* Unreachable: "/dev/tty-2147483648" is twenty bytes and ND_PATH_MAX is
     * 512. Checked because an unchecked snprintf is how the next one of these
     * gets missed. */
    if (nd_linuxshell_tty_path(tty_shell, sizeof tty_shell, shell_vt) != ND_OK)
        return 0;

    /* `chvt = _which("chvt"); if not chvt: return`. "Hard fail back to UI:
     * without chvt we can't reliably switch the visible console." */
    if (!nd_linuxshell_which(ND_LINUXSHELL_CHVT, chvt_path, sizeof chvt_path)) {
        nd_log_err(LS_LOG_TAG, "chvt is not on PATH; not switching the console");
        return 0;
    }

    envp = nd_linuxshell_build_envp();
    if (envp == NULL) {
        /* os.environ.copy() raising MemoryError is an exception out of run(),
         * and an exception out of run() is the crash screen. */
        nd_log_err(LS_LOG_TAG, "out of memory building the shell environment");
        return 1;
    }

    /* if not _run_quiet([chvt, str(shell_vt)], timeout=1.0): return
     *
     * Still before the try, so a refusal here undoes nothing -- correctly:
     * the console has not moved. */
    if (nd_snprintf(vt_arg, sizeof vt_arg, "%d", (int)shell_vt) != ND_OK)
        goto out_no_teardown;
    chvt_argv[0] = chvt_path;
    chvt_argv[1] = vt_arg;
    chvt_argv[2] = NULL;
    if (!nd_linuxshell_run_quiet(chvt_argv, ND_LINUXSHELL_CHVT_TIMEOUT_S)) {
        nd_log_err(LS_LOG_TAG, "chvt %d did not switch the console", (int)shell_vt);
        goto out_no_teardown;
    }

    nd_linuxshell_write_tty(tty_shell, nd_linuxshell_cursor_on);
    nd_linuxshell_write_tty(tty_shell, nd_linuxshell_hint);

    /* On keypad hardware the core is already typing into this console via T9;
     * say so, so the owner knows the pad is live and what it does. */
    if (t9_typing_available()) {
        nd_linuxshell_write_tty(tty_shell, nd_linuxshell_t9_hint);
    } else if (nd_linuxshell_needs_key_bridge(ui)) {
        /* The one case worth a word: a phone whose only input IS the keypad,
         * with no key device from the core. The console is about to accept
         * nothing at all, and the hint would be a lie. Not fatal -- a serial
         * cable is still a keyboard, and that is who is usually here -- and
         * the core has already logged which of the manifest, /dev/uinput or
         * the udev grant was the reason. */
        nd_log_err(LS_LOG_TAG,
                   "no key device: this phone's keypad cannot reach the console, so only a "
                   "serial or USB keyboard will type at this shell. See " ND_ENV_KEY_EVDEV
                   " and nd_proc.h.");
    }

    /* try: with open(tty_shell, "r+b", buffering=0) as t: ... except: pass */
    if (nd_path_resolve(resolved_tty, sizeof resolved_tty, tty_shell) == ND_OK)
        tty_fd = open(resolved_tty, O_RDWR | O_CLOEXEC);

    if (tty_fd < 0) {
        nd_log_err(LS_LOG_TAG, "cannot open %s: %s", tty_shell, strerror(errno));
    } else {
        sh_argv[0] = ND_LINUXSHELL_SH;
        sh_argv[1] = "-i";
        sh_argv[2] = NULL;

        memset(&spec, 0, sizeof spec);
        spec.argv = sh_argv;
        spec.envp = envp;
        spec.owner = ND_OWNER_SYSTEM;
        /* stdin=t, stdout=t, stderr=t: one descriptor, three numbers. */
        spec.fds[0].child_fd = 0;
        spec.fds[0].our_fd = tty_fd;
        spec.fds[1].child_fd = 1;
        spec.fds[1].our_fd = tty_fd;
        spec.fds[2].child_fd = 2;
        spec.fds[2].our_fd = tty_fd;
        spec.n_fds = 3u;
        /* close_fds=True is nd_proc_spawn's default: everything this process
         * opened is O_CLOEXEC, and the three above lose the flag on the way
         * through dup2. */

        if (nd_proc_spawn(ND_LINUXSHELL_SH, &spec, &pid) != ND_OK) {
            nd_log_err(LS_LOG_TAG, "cannot start %s", ND_LINUXSHELL_SH);
        } else {
            g_sh_pid = (sig_atomic_t)pid;
            wait_for_shell(pid, (ui != NULL) ? ui->input : NULL);
            g_sh_pid = 0;
        }
        (void)close(tty_fd);
        tty_fd = -1;
    }

    /* ---- finally: ------------------------------------------------ */

    /* No keyboard to tear down: the core created it, the core destroys it
     * when this process exits, and it does so before the UI resumes reading
     * the keypad. That ordering is nd_proc.c's keydev_close(), and it matters
     * for the same reason it did when the teardown was here -- a bridge that
     * outlived its app would still be reading the pad nobody is feeding. */

    /* Hide the cursor again: the cmdline carries vt.global_cursor_default=0
     * and the UI expects it back the way it found it. */
    nd_linuxshell_write_tty(tty_shell, nd_linuxshell_cursor_off);

    if (nd_snprintf(vt_arg, sizeof vt_arg, "%d", (int)ui_vt) == ND_OK) {
        chvt_argv[0] = chvt_path;
        chvt_argv[1] = vt_arg;
        chvt_argv[2] = NULL;
        (void)nd_linuxshell_run_quiet(chvt_argv, ND_LINUXSHELL_CHVT_TIMEOUT_S);
    }

    settle();

out_no_teardown:
    free((void *)(uintptr_t)(const void *)envp);
    return 0;
}

/* nd_app.h's teardown contract, step 4. Belt and braces: the wait loop above
 * already notices SIGTERM, kills the shell and runs the teardown itself,
 * because switching the console back needs a spawn and a moment and neither
 * belongs here. This covers the window where the signal lands between the
 * spawn and the loop's first check. */
void app_shutdown(void)
{
    pid_t pid = (pid_t)g_sh_pid;
    nd_proc_status st;

    if (pid <= 0)
        return;
    g_sh_pid = 0;

    (void)kill(pid, SIGTERM);
    /* A moment, and no more: the core is waiting on this process so it can
     * start the ringer. */
    if (nd_proc_wait(pid, 0.2, &st) != ND_OK)
        (void)kill(pid, SIGKILL);
}
