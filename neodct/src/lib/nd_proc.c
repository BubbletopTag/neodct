/* nd_proc.c -- starting other programs, and finding out what happened to them.
 *
 * ============ THE RULE, RESTATED WHERE IT IS IMPLEMENTED ============
 *
 *     fork() IS ALWAYS IMMEDIATELY FOLLOWED BY execve().
 *
 * The core runs threads. Forking a threaded process gives the child only the
 * calling thread, and any mutex another thread held at the instant of the fork
 * stays locked forever in the child -- INCLUDING THE ONES INSIDE malloc().
 * The child then hangs on its first allocation, somewhere that looks nothing
 * like the fork.
 *
 * So spawn_child() below builds argv, envp and the descriptor plan BEFORE the
 * fork, and the child does nothing but dup2, fcntl, execve and _exit. Every
 * one of those is async-signal-safe. There is no malloc, no printf, no fopen
 * and no exit() anywhere between the fork and the exec, and if you add one the
 * phone will hang on a machine you do not have.
 *
 * ============ THE REAPER ============
 *
 * Four kinds of child exist (nd_proc.h) and only one of them -- the app -- is
 * waited for. A SIGCHLD handler collects everything else so nothing becomes a
 * zombie, and because the handler cannot know which pid somebody is about to
 * wait on, IT KEEPS WHAT IT REAPED: results go into a small fixed ring that
 * nd_proc_wait() checks before it calls waitpid(). That closes the race where
 * the reaper collects the app child a microsecond before the launcher asks
 * about it and waitpid() answers ECHILD with no status.
 *
 * The ring is written only from the handler and read only from ordinary code,
 * both under a "claimed" flag that is a sig_atomic_t, so no lock is needed and
 * none is taken -- taking one in a signal handler is the other classic way to
 * deadlock a threaded process.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_broker.h"
#include "nd_cpufreq.h"
#include "nd_crash.h"
#include "nd_fb_priv.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"

#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "nd_keypad.h"
#include "nd_priv.h"
#include "nd_settings.h"
#include "nd_svc.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"

extern char **environ;

/* ------------------------------------------------------------------ *
 * The reaper's ring
 * ------------------------------------------------------------------ */

/* 16 is far more than the phone can have outstanding: one app, one audio
 * bridge, one tone and the occasional poweroff. A full ring drops the OLDEST
 * entry, because a status nobody has asked for in sixteen deaths is a status
 * nobody is going to ask for. */
#define REAP_RING 16

typedef struct {
    volatile sig_atomic_t used;
    /* Whether nd_proc_reap_report() has already had its say about this
     * status. Separate from `used` because the two are cleared by different
     * people: `used` goes when somebody CLAIMS the status, and most of the
     * statuses worth complaining about are never claimed by anybody -- a
     * DTMF tone is fire and forget. See nd_proc_reap_report(). */
    volatile sig_atomic_t noted;
    pid_t pid;
    int status;
    /* Which kind of child it was, so the report can say whose corpse this is
     * rather than only its number. Filled by nd_proc_spawn() before the fork
     * and looked up by the handler, because a signal handler cannot be told
     * anything at the moment it runs. */
    nd_proc_owner owner;
} reaped_slot;

static reaped_slot g_reaped[REAP_RING];
static volatile sig_atomic_t g_reap_next;
static bool g_reaper_installed;

/* ------------------------------------------------------------------ *
 * Who owns each live child
 * ------------------------------------------------------------------ *
 *
 * A pid -> owner table, written before each fork and read from the SIGCHLD
 * handler. It exists only so that the reserved-code report below can name
 * the child ("tone", "audio bridge") instead of printing a bare number that
 * means nothing to whoever is reading the boot log at 3am.
 *
 * Same discipline as the ring: written from ordinary code, read from the
 * handler, guarded by one sig_atomic_t flag and no lock -- taking a lock in
 * a signal handler is the other classic way to deadlock a threaded process.
 * A stale entry is harmless, because a pid that has been reaped is not
 * handed out again until the whole pid space has wrapped.
 */
typedef struct {
    volatile sig_atomic_t used;
    pid_t pid;
    nd_proc_owner owner;
} owner_slot;

static owner_slot g_owners[REAP_RING];
static volatile sig_atomic_t g_owner_next;

static void owner_remember(pid_t pid, nd_proc_owner owner)
{
    int idx = (int)g_owner_next % REAP_RING;

    g_owner_next = (sig_atomic_t)((idx + 1) % REAP_RING);
    g_owners[idx].pid = pid;
    g_owners[idx].owner = owner;
    g_owners[idx].used = 1;
}

/* ND_OWNER_SYSTEM for a pid nobody recorded -- a child forked before the
 * table existed, or one whose entry the ring has since overwritten. It is
 * the least specific of the four and reads as "some helper", which is
 * exactly what an unknown pid is. */
static nd_proc_owner owner_of(pid_t pid)
{
    size_t i;

    for (i = 0u; i < REAP_RING; i++) {
        if (g_owners[i].used != 0 && g_owners[i].pid == pid) {
            g_owners[i].used = 0;
            return g_owners[i].owner;
        }
    }
    return ND_OWNER_SYSTEM;
}

static void remember(pid_t pid, int status)
{
    int idx = (int)g_reap_next % REAP_RING;

    g_reap_next = (sig_atomic_t)((idx + 1) % REAP_RING);
    g_reaped[idx].pid = pid;
    g_reaped[idx].status = status;
    g_reaped[idx].owner = owner_of(pid);
    g_reaped[idx].noted = 0;
    g_reaped[idx].used = 1;
}

static bool take_remembered(pid_t pid, int *status_out)
{
    size_t i;

    for (i = 0u; i < REAP_RING; i++) {
        if (g_reaped[i].used != 0 && g_reaped[i].pid == pid) {
            *status_out = g_reaped[i].status;
            g_reaped[i].used = 0;
            return true;
        }
    }
    return false;
}

static void on_sigchld(int signo)
{
    int saved = errno;
    pid_t pid;
    int status;

    ND_UNUSED(signo);
    /* waitpid, kill and write are the only calls in here, and all three are
     * async-signal-safe. errno is saved and restored because the handler can
     * interrupt code that is mid-check of it. */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        remember(pid, status);
    errno = saved;
}

/* ------------------------------------------------------------------ *
 * GIVING THE REAPER A VOICE
 * ------------------------------------------------------------------ *
 *
 * ============ THE BUG THIS IS THE SECOND HALF OF ============
 *
 * The child between fork() and execve() has exactly one way to say anything:
 * its exit status. So every failure on that path is an _exit() of a number in
 * 120..127, and those numbers are RESERVED -- they are nd_proc's own, chosen
 * so that a caller's waitpid can tell a refused setgroups() apart from a
 * failed dup2 and a failed execve.
 *
 * Nothing ever printed them. A tone, an audio bridge and a poweroff are
 * reaped by the handler above and their statuses go into the ring for a
 * caller who, for three of the four owner kinds, does not exist. So when
 * every DTMF tone on the phone began dying at exit 122 -- setgroups() needs
 * CAP_SETGID, and the core is ndusr now -- the phone went silent and the log
 * said NOTHING AT ALL, for eight releases. Several a second while dialling,
 * every one of them discarded.
 *
 * A status in this range can never be a legitimate child exit: aplay, mpv,
 * netsurf and nd-apprun all exit with their own small numbers or die on a
 * signal, and none of them has any way to reach 120..127 except by being one
 * of our own corpses. So printing every one of them cannot be noisy on a
 * healthy phone, and on a broken one it is the whole diagnosis.
 *
 * The handler cannot do this itself: nd_log() formats, allocates and writes
 * to a FILE*, and none of that is async-signal-safe. Hence a flag in the ring
 * and a drain from ordinary code.
 */

/* The names, so the message reads as a diagnosis rather than a number.
 *
 * ============ TWO OF THESE ARE AMBIGUOUS, AND THE RANGE IS FULL ============
 *
 * 120 + nd_priv_become()'s step number collides with the two mount-namespace
 * exits the child also uses: setgid is step 3 and the MS_PRIVATE remount also
 * exits 123, setuid is step 4 and a failed hide mount also exits 124. There is
 * exactly one free code in the reserved range (120, which is step 0 and means
 * success, so it is never returned) and two collisions, so this cannot be
 * renumbered without widening the range -- and the range is one byte of exit
 * status shared with every ordinary program's own codes.
 *
 * Both meanings are therefore printed. The two are trivially told apart by
 * context anyway: the mount exits happen only for an app with
 * private_mounts, which today is the browser and the installed apps alone.
 *
 * The step numbers are nd_priv.c's ND_PRIV_STEP_*, which that file keeps to
 * itself -- they are its private contract with the code between fork and
 * exec. Spelling them out here rather than exporting them keeps nd_priv.h
 * (another package's frozen header) as it is; if they ever move into it,
 * this table is the one place to change. */
static const char *reserved_exit_name(int code)
{
    switch (code) {
    case 120:
        return "nd_priv_become() step 0, which cannot happen";
    case 121:
        return "prctl(PR_SET_NO_NEW_PRIVS) refused";
    case 122:
        return "setgroups() refused -- it needs CAP_SETGID, which an ndusr "
               "process does not have";
    case 123:
        return "setgid() refused, or the MS_PRIVATE remount of / failed";
    case 124:
        return "setuid() refused, or one of the hide mounts failed";
    case 125:
        return "the privilege drop did not read back: a PARTIAL drop";
    case 126:
        return "dup2() of an inherited descriptor failed";
    case 127:
        return "execve() failed -- wrong path, or not executable";
    default:
        return NULL;
    }
}

static const char *owner_name(nd_proc_owner owner)
{
    switch (owner) {
    case ND_OWNER_APP:
        return "app";
    case ND_OWNER_AUDIO:
        return "audio bridge";
    case ND_OWNER_TONE:
        return "tone";
    case ND_OWNER_SYSTEM:
    default:
        return "system helper";
    }
}

void nd_proc_reap_report(void)
{
    size_t i;

    for (i = 0u; i < REAP_RING; i++) {
        int status;
        const char *why;

        /* `used` is read first and set last by remember(), which is the same
         * order take_remembered() has always relied on. A slot the handler
         * recycles at the exact instant this loop is inside it loses one
         * report; that is a benign miss of one entry in sixteen, and the
         * alternative is a lock in a signal handler. */
        if (g_reaped[i].used == 0 || g_reaped[i].noted != 0)
            continue;
        status = g_reaped[i].status;
        if (!WIFEXITED(status))
            continue;
        why = reserved_exit_name(WEXITSTATUS(status));
        if (why == NULL)
            continue;

        g_reaped[i].noted = 1;
        nd_log_err(ND_LOG_OS, "%s pid %ld died before it ever ran: exit %d, %s",
                   owner_name(g_reaped[i].owner), (long)g_reaped[i].pid, WEXITSTATUS(status), why);
    }
}

bool nd_proc_namespaces_available(void)
{
    /* -1 not yet asked, 0 no, 1 yes. Asked at most once per process: the
     * answer is a property of the kernel and cannot change under us. */
    static int known = -1;
    pid_t pid;
    int status = 0;

    if (known >= 0)
        return known == 1;

    /* In a child, because unshare(CLONE_NEWNS) SUCCEEDS -- and the core
     * would then be in a mount namespace of its own for the rest of the
     * boot, which is not a thing to do by accident while asking a question. */
    (void)fflush(NULL);
    pid = fork();
    if (pid < 0)
        return false; /* not remembered: a failed fork says nothing */
    if (pid == 0)
        _exit(unshare(CLONE_NEWNS) == 0 ? 0 : 1);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return false;
    known = (WEXITSTATUS(status) == 0) ? 1 : 0;
    return known == 1;
}

nd_err nd_proc_reaper_start(void)
{
    struct sigaction sa;

    if (g_reaper_installed)
        return ND_OK;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    (void)sigemptyset(&sa.sa_mask);
    /* SA_RESTART here, unlike the app's SIGTERM: the core's own blocking
     * select() on the keypad should resume rather than spuriously report
     * nothing every time a tone process exits. SA_NOCLDSTOP because a stopped
     * child is not a dead one. */
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        nd_log_err(ND_LOG_OS, "sigaction(SIGCHLD): %s", strerror(errno));
        return ND_ERR_IO;
    }
    g_reaper_installed = true;
    return ND_OK;
}

void nd_proc_reaper_stop(void)
{
    if (!g_reaper_installed)
        return;
    (void)signal(SIGCHLD, SIG_DFL);
    g_reaper_installed = false;
}

/* ------------------------------------------------------------------ *
 * Waiting
 * ------------------------------------------------------------------ */

static void fill_status(nd_proc_status *out, pid_t pid, int status)
{
    memset(out, 0, sizeof *out);
    out->pid = pid;
    if (WIFEXITED(status)) {
        out->exited = true;
        out->exit_status = WEXITSTATUS(status);
        /* The other half of nd_proc_reap_report(): that one speaks for the
         * corpses nobody claims, and this one for the corpses somebody does.
         * A caller that waits gets the number, but the number on its own has
         * never told anyone that setgroups() was refused -- the launcher's
         * own "App crashed: Browser (exit 122)" was on the screen for eight
         * releases and read as an app that crashed. */
        {
            const char *why = reserved_exit_name(out->exit_status);

            if (why != NULL)
                nd_log_err(ND_LOG_OS, "pid %ld died before it ever ran: exit %d, %s", (long)pid,
                           out->exit_status, why);
        }
    }
    if (WIFSIGNALED(status)) {
        out->signalled = true;
        out->signo = WTERMSIG(status);
    }
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

static double monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ============ "NOBODY'S CHILD" IS NOT "EXITED CLEANLY" ============
 *
 * This used to be a bool, and the ECHILD arm below used to synthesise
 * `exited = true, exit_status = 0` for a pid this process never forked. It
 * read as a kindness -- do not spin until the timeout for a process that
 * cannot possibly be waited for -- and it was the reason a whole class of
 * failure reported success.
 *
 * The one that cost the owner a card: nd_svc's format hatch signalled a ROOT
 * mke2fs from an unprivileged core, got EPERM, then waited on it, got ECHILD,
 * and was told the helper had exited cleanly. So "Formatting failed." went up
 * on the screen while a root mke2fs carried on writing the card underneath
 * it. The same shape reaches nd_proc_terminate(), which returned ND_OK for a
 * process it had not touched.
 *
 * Since the core dropped to ndusr, "a pid I did not fork" is the NORMAL case
 * rather than a bug -- every app is the broker's child now -- so the answer
 * has to be a third one that the caller can act on. Fetch already gets this
 * right ("Lost track of the download."); everything else at least stops
 * believing a fiction. */
typedef enum {
    COLLECT_PENDING = 0, /* still running, ask again */
    COLLECT_DONE,        /* *out is the real status  */
    COLLECT_NOT_OURS     /* never our child, or already reaped by somebody else */
} collect_result;

/* One non-blocking check: the reaper's ring first, then the kernel. */
static collect_result collect(pid_t pid, nd_proc_status *out)
{
    int status = 0;
    pid_t r;

    if (take_remembered(pid, &status)) {
        fill_status(out, pid, status);
        return COLLECT_DONE;
    }

    do {
        r = waitpid(pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);

    if (r == pid) {
        fill_status(out, pid, status);
        return COLLECT_DONE;
    }
    if (r < 0 && errno == ECHILD) {
        /* The reaper may have taken it between the two checks above. */
        if (take_remembered(pid, &status)) {
            fill_status(out, pid, status);
            return COLLECT_DONE;
        }
        memset(out, 0, sizeof *out);
        out->pid = pid;
        return COLLECT_NOT_OURS;
    }
    return COLLECT_PENDING;
}

nd_err nd_proc_wait(pid_t pid, double timeout_s, nd_proc_status *out)
{
    double deadline;

    if (pid <= 0 || out == NULL)
        return ND_ERR_INVAL;

    switch (collect(pid, out)) {
    case COLLECT_DONE:
        return ND_OK;
    case COLLECT_NOT_OURS:
        return ND_ERR_NOTFOUND;
    default:
        break;
    }
    if (timeout_s == 0.0)
        return ND_ERR_TIMEOUT;

    deadline = monotonic_now() + (timeout_s < 0.0 ? 0.0 : timeout_s);
    for (;;) {
        /* 5 ms is short enough that an app exit feels instant and long enough
         * that waiting for a browsing session costs nothing measurable. */
        nap(0.005);
        switch (collect(pid, out)) {
        case COLLECT_DONE:
            return ND_OK;
        case COLLECT_NOT_OURS:
            /* Not "wait longer": waiting cannot change whose child it is. */
            return ND_ERR_NOTFOUND;
        default:
            break;
        }
        if (timeout_s > 0.0 && monotonic_now() >= deadline)
            return ND_ERR_TIMEOUT;
    }
}

nd_err nd_proc_terminate(pid_t pid, double grace_s, nd_proc_status *out)
{
    nd_proc_status local;
    nd_broker *b = nd_broker_default();

    if (pid <= 0)
        return ND_ERR_INVAL;
    if (out == NULL)
        out = &local;

    /* ============ THE CORE CANNOT SIGNAL ITS OWN APP ANY MORE ============
     *
     * Since the drop, nd-core is uid 1000 and an app is either the broker's
     * child (so this process never forked it, and waitpid answers ECHILD) or
     * uid 1001 (so kill() answers EPERM in both directions). Both halves of
     * this function were therefore no-ops on a real phone, silently: the
     * kill was refused, the wait reported a clean exit that had not happened
     * -- see collect() above -- and nd_app.h's teardown contract, step 1,
     * "the core's modem thread sees RING and sends the child SIGTERM", could
     * not be honoured for the browser at all. A call arriving while the
     * browser was up left netsurf holding /dev/fb0 and the ringtone playing
     * underneath a web page.
     *
     * The broker is the process that forked it and the only one that can
     * signal it, so the whole sequence goes over the socket when there is
     * one. The verbs are pinned on that side -- SIGTERM or SIGKILL, and only
     * for a pid the broker itself started -- so this delegates the ACT and
     * keeps the POLICY (how long a grace, when to escalate) here.
     *
     * The direct path below is kept and is not dead code: a unit test, an app
     * process terminating a helper it forked itself (Fetch's curl, the
     * browser's netsurf, MusicPlayer's mpv) and a core running without a
     * broker all still own their children in the ordinary way. */
    if (b != NULL) {
        if (!nd_broker_kill(b, pid, SIGTERM)) {
            /* Not the broker's child. Fall through to the direct route rather
             * than refuse: it may well be ours. */
            b = NULL;
        } else {
            if (nd_broker_wait(b, pid, grace_s, out) == ND_OK)
                return ND_OK;
            nd_log(ND_LOG_OS, "pid %ld ignored SIGTERM after %.1fs; killing it", (long)pid,
                   grace_s);
            (void)nd_broker_kill(b, pid, SIGKILL);
            return nd_broker_wait(b, pid, 2.0, out);
        }
    }

    if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
        nd_log_err(ND_LOG_OS, "kill(%ld, SIGTERM): %s", (long)pid, strerror(errno));

    if (nd_proc_wait(pid, grace_s, out) == ND_OK)
        return ND_OK;

    /* The teardown contract in nd_app.h ends here: an app that will not go is
     * killed, because the phone is ringing and the sound card has to come
     * back. */
    nd_log(ND_LOG_OS, "pid %ld ignored SIGTERM after %.1fs; killing it", (long)pid, grace_s);
    (void)kill(pid, SIGKILL);
    return nd_proc_wait(pid, 2.0, out);
}

/* ------------------------------------------------------------------ *
 * Spawning
 * ------------------------------------------------------------------ */

nd_err nd_proc_spawn(const char *path, const nd_proc_spec *spec, pid_t *pid_out)
{
    pid_t pid;
    size_t i;
    /* Everything the child touches is read out of the spec into locals BEFORE
     * the fork; after it, the child may not dereference anything that could
     * have been mid-update in another thread. */
    int child_fd[ND_PROC_MAX_FDS];
    int our_fd[ND_PROC_MAX_FDS];
    size_t n_fds;
    char *const *argv;
    char *const *envp;
    bool close_others;
    bool no_new_privs;
    bool private_mounts;
    nd_priv_id run_as;
    int fd_limit;
    /* Copied out of the spec before the fork, and filtered to what exists:
     * a hide that fails in the child is then always a bug rather than a
     * path RemoteShell has not created yet. */
    const char *hide[ND_PROC_MAX_HIDE];
    size_t n_hide;

    if (path == NULL || spec == NULL || spec->argv == NULL || pid_out == NULL)
        return ND_ERR_INVAL;
    if (spec->n_fds > ND_PROC_MAX_FDS)
        return ND_ERR_INVAL;

    /* Say what happened to the LAST batch of children before making another.
     * Every spawn is a moment where ordinary code is running and nothing is
     * held, which is exactly what the drain needs, and on the path that
     * matters -- a tone per keypress while dialling -- it means the first
     * failure is in the log before the second one happens. See
     * nd_proc_reap_report(). */
    nd_proc_reap_report();

    n_fds = spec->n_fds;
    close_others = spec->close_others;
    no_new_privs = spec->no_new_privs;
    private_mounts = spec->private_mounts;
    n_hide = 0u;
    if (spec->hide_paths != NULL) {
        size_t h;

        for (h = 0u; spec->hide_paths[h] != NULL && n_hide < ND_PROC_MAX_HIDE; h++) {
            /* access() rather than a stat of the type: what matters is that
             * mount() will have a target, and a path we cannot even reach is
             * one the child cannot reach either. Done HERE because access()
             * is not on the async-signal-safe list. */
            if (access(spec->hide_paths[h], F_OK) == 0)
                hide[n_hide++] = spec->hide_paths[h];
        }
    }
    /* A copy, like everything else here: after the fork the child may not
     * dereference anything that could have been mid-update in another
     * thread, and nd_priv_id is plain integers precisely so this works. */
    run_as = spec->run_as;

    /* ============ THE EXIT-122 CLASS, CLOSED IN THE PARENT ============
     *
     * A drop is a thing only a privileged process can do, and the child is
     * the worst possible place to find that out. Between fork and execve it
     * can neither log nor recover: nd_priv_become() hands back a step number,
     * the child _exit(120 + step)s, and fork() having succeeded means this
     * function has ALREADY told its caller the spawn worked. So the failure
     * is reported as a success and then discarded, and there is no line
     * anywhere that says otherwise.
     *
     * That is not a hypothetical. When nd-core stopped being root in 0.5.x,
     * every caller that had always asked for `run_as = ndusr` was suddenly an
     * ndusr process asking to become ndusr -- and setgroups() needs CAP_SETGID
     * UNCONDITIONALLY, even to set the list it already has. Every DTMF tone,
     * the SMS chirp, the calendar alert and the incoming-call ringtone died at
     * exit 122 before their aplay ever ran. The phone went silent for eight
     * releases and the log never mentioned it once.
     *
     * So the question is asked HERE, where there is a caller to answer to:
     *
     *   ALREADY THAT USER -- the overwhelmingly common case, and the one that
     *   went wrong. There is nothing to drop, so drop nothing. This is not a
     *   weakening: the child ends up running as exactly the user the caller
     *   asked for, which is the whole of what run_as promises.
     *
     *   SOMEBODY ELSE -- refuse, loudly, and let the caller decide. An
     *   unprivileged process cannot become another user and pretending
     *   otherwise only moves the failure somewhere nothing can see it.
     *
     * geteuid() and not getuid(): the effective id is what the kernel checks
     * for CAP_SETGID, and the two differ on exactly the setuid path this
     * refusal is about. */
    if (run_as.valid && geteuid() != 0) {
        if (run_as.uid == geteuid() && run_as.gid == getegid()) {
            /* One thing IS given up here and it is worth naming. A privileged
             * drop replaces the supplementary group list wholesale, so it
             * would also strip any extra group the parent happened to hold;
             * this cannot, because setgroups() is the very call being refused.
             * The child therefore inherits the parent's list rather than being
             * reduced to the target user's.
             *
             * There is no third option: an unprivileged process cannot shrink
             * its own group list, so the only alternative to inheriting it is
             * to refuse the spawn -- which is the phone with no ringtone. And
             * the parent IS the target user, by the two comparisons above, so
             * on the phone the two lists are the same list. */
            run_as.valid = false;
        } else {
            nd_log_err(ND_LOG_OS,
                       "spawn %s: cannot drop to uid %ld from uid %ld -- setgroups() needs "
                       "CAP_SETGID; refusing rather than exiting 122",
                       path, (long)run_as.uid, (long)geteuid());
            return ND_ERR_PERM;
        }
    }
    /* sysconf() BEFORE the fork: it is not on the async-signal-safe list, and
     * the child between fork and execve may call nothing that is not. */
    fd_limit = close_others ? (int)sysconf(_SC_OPEN_MAX) : 0;
    if (fd_limit < 3 || fd_limit > 4096)
        fd_limit = 4096;
    for (i = 0u; i < n_fds; i++) {
        child_fd[i] = spec->fds[i].child_fd;
        our_fd[i] = spec->fds[i].our_fd;
    }
    /* execve's prototype is not const-correct on any Unix; the cast is the
     * standard one and execve does not write through it. */
    argv = (char *const *)(uintptr_t)(const void *)spec->argv;
    envp = spec->envp != NULL ? (char *const *)(uintptr_t)(const void *)spec->envp : environ;

    (void)fflush(NULL); /* BEFORE the fork, so the child inherits empty buffers */

    pid = fork();
    if (pid < 0) {
        nd_log_err(ND_LOG_OS, "fork: %s", strerror(errno));
        return ND_ERR_IO;
    }

    if (pid == 0) {
        /* ==== ASYNC-SIGNAL-SAFE ONLY FROM HERE TO THE execve ==== */

        /* setsid() first, before anything that can fail in a way worth
         * reporting: a child that is going to be signalled by process group
         * must be in its own group BEFORE it can spawn anything of its own.
         * setsid() is async-signal-safe, and its only failure is EPERM when
         * we are already a group leader -- which for a fresh fork() cannot
         * happen. Ignoring the return is deliberate; there is nothing a
         * child between fork and exec could usefully do about it. */
        if (spec->new_session)
            (void)setsid();

        for (i = 0u; i < n_fds; i++) {
            if (child_fd[i] != our_fd[i]) {
                if (dup2(our_fd[i], child_fd[i]) < 0)
                    _exit(126);
            } else {
                /* dup2 onto itself is a no-op and does NOT clear the
                 * close-on-exec flag, so the descriptor would vanish at exec.
                 * Clearing it by hand is the whole reason this branch exists. */
                if (fcntl(child_fd[i], F_SETFD, 0) < 0)
                    _exit(126);
            }
        }
        /* Everything above stderr that was not asked for, AFTER the dup2s so
         * the descriptors just installed are never among the casualties.
         * close() is async-signal-safe and closing a descriptor that was never
         * open is a harmless EBADF, so no enumeration of what is open is
         * needed -- which is just as well, since reading /proc/self/fd here
         * would not be safe either.
         *
         * fds 0-2 are left alone deliberately: closing stdin outright would
         * leave the number free for the next open() in the child to claim, and
         * a daemon that writes to what it thinks is a file and is actually
         * descriptor 0 is a worse bug than the one being fixed. */
        if (close_others) {
            int fd;

            for (fd = 3; fd < fd_limit; fd++) {
                bool keep = false;

                for (i = 0u; i < n_fds; i++) {
                    if (child_fd[i] == fd) {
                        keep = true;
                        break;
                    }
                }
                if (!keep)
                    (void)close(fd);
            }
        }

        /* The mount namespace, BEFORE the privilege drop, because unsharing
         * one and mounting inside it both need CAP_SYS_ADMIN -- which is
         * exactly the privilege about to be given away.
         *
         * unshare failing is not fatal: CONFIG_MNT_NS is a vendor kernel
         * question and a phone without it must still open its browser. The
         * uid boundary is what carries the confinement; this is defence on
         * top of it. A hide failing after the namespace exists IS fatal,
         * because that one is a bug and the alternative is a child that
         * believes it cannot see something it can. */
        if (private_mounts && unshare(CLONE_NEWNS) == 0) {
            size_t h;

            /* Without this every mount below propagates back into the
             * parent's namespace, which would hide these paths from the
             * whole phone rather than from this child. */
            if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
                _exit(123);
            for (h = 0u; h < n_hide; h++) {
                /* An empty, read-only, mode-0000 tmpfs. The directory still
                 * exists and is empty, which is a quieter failure inside the
                 * child than a missing path and just as final. */
                if (mount("none", hide[h], "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
                          "size=0k,mode=0000") != 0)
                    _exit(124);
            }
        }

        /* Become somebody else, LAST, immediately before the execve.
         *
         * Last because everything above needs the privilege the caller had:
         * dup2 onto a descriptor the child could not have opened for itself
         * is the whole point of the inherited-fd design, and close() on a
         * range is cheaper before the uid changes than after. Immediately
         * before, because the window between dropping and exec'ing is code
         * running as the target user with the parent's memory still mapped,
         * and it should be as close to nothing as it can be.
         *
         * nd_priv_become() is four syscalls on integers copied before the
         * fork, and every one of them is read back -- see nd_priv.h on why
         * the order is the whole of it. A failure is not recoverable and
         * must not be ignored: continuing would run untrusted code with
         * whatever privilege happened to be left. 120 + the step, so a
         * caller's waitpid can tell WHICH call failed apart from the 126 of
         * a failed dup2 and the 127 of a failed execve. */
        if (no_new_privs && !run_as.valid) {
            if (nd_priv_no_new_privs() != 0)
                _exit(121);
        }
        {
            int step = nd_priv_become(&run_as);

            if (step != 0)
                _exit(120 + step);
        }

        (void)execve(path, argv, envp);
        _exit(127); /* only reached if execve failed */
    }

    *pid_out = pid;
    /* AFTER the fork, because there is no pid before it, and racing the
     * handler here costs only the owner's NAME in a log line -- a child
     * reaped in this window is reported as a "system helper" rather than as
     * a tone, and reported either way. */
    owner_remember(pid, spec->owner);
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Launching an app
 * ------------------------------------------------------------------ */

/* nd-apprun's location. The header is explicit that the path is NOT
 * ND_ROOT-resolved -- it is an executable, and the host harness runs the real
 * binaries out of build/<variant>/bin. In priority order:
 *   1. $NEODCT_APPRUN, for a test that built it somewhere else
 *   2. beside the running binary, which covers every host build
 *   3. /NeoDCT/System/bin/nd-apprun, which is where the image puts it */
static const char *apprun_path(char *buf, size_t buf_sz)
{
    const char *env = getenv("NEODCT_APPRUN");
    char exe[ND_PATH_MAX];
    ssize_t n;

    if (env != NULL && env[0] != '\0' && access(env, X_OK) == 0)
        return env;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n > 0) {
        char *slash;

        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (nd_snprintf(buf, buf_sz, "%s/nd-apprun", exe) == ND_OK && access(buf, X_OK) == 0)
                return buf;
            /* A unit test binary lives in build/<variant>/test, one directory
             * over from build/<variant>/bin, and must drive the SAME variant's
             * nd-apprun -- an ASan run has to fork an ASan child or the whole
             * point of check 5 is lost. */
            if (nd_snprintf(buf, buf_sz, "%s/../bin/nd-apprun", exe) == ND_OK &&
                access(buf, X_OK) == 0)
                return buf;
        }
    }
    return ND_PATH_ND_APPRUN;
}

/* The app's directory as the CHILD must see it. Every path in the project is
 * absolute and ND_ROOT-resolved on open, but the child execve's into a fresh
 * process that reads NEODCT_ROOT for itself -- so it gets the UNRESOLVED
 * /NeoDCT/System/apps/<Name> and resolves it the same way we would. */
/* Every variable the core composes for a launch, in one place: seven
 * nd_snprintf() calls used to arrive as fourteen parameters and their sizes,
 * and adding the eighth (the key device) to that shape would have been a
 * sixteen-argument function nobody could read. The strings live in the
 * caller's frame and must outlive the spawn, which is why this is a struct of
 * arrays and not of pointers. */
typedef struct {
    char keypad[64];
    char crash[64];
    char fb[64];
    char svc[64];
    char matrix[64];
    char root[ND_PATH_MAX + 32];
    char evdev[ND_PATH_MAX + 32];
    /* The presentation subset -- nd_proc.h has the whole argument for why
     * these three cross the boundary and settings.prop does not. */
    char wallpaper[ND_PATH_MAX + 32];
    char wp_everywhere[64];
    char wp_dim[64];
} app_env_vars;

/* One setting, VERBATIM, as "NAME=value".
 *
 * nd_settings_get_copy() and not nd_settings_get(): the latter hands back a
 * pointer into one shared static buffer that the next call overwrites, and
 * three of these are composed in a row. Unparsed on purpose -- see
 * ND_ENV_UI_WALLPAPER in nd_proc.h.
 *
 * BEST EFFORT, AND NEVER A REASON NOT TO OPEN THE APP. Every other variable
 * here is a descriptor number or a path the core just built, so a failure to
 * compose one is a bug and ND_ERR_TOOLONG is the right answer. These three
 * are values out of a file on the writable partition, so a value long enough
 * not to fit is somebody's junk -- and refusing to launch every app on the
 * phone because settings.prop has a 4 KB wallpaper name in it would be a
 * far worse failure than drawing the default look. So an overflow falls back
 * to the default, and a default that somehow does not fit leaves the variable
 * absent, which the app already reads as "no core told me". */
static void put_setting(char *out, size_t out_sz, const char *env_name, const char *key,
                        const char *dflt)
{
    char value[ND_PATH_MAX];

    if (nd_settings_get_copy(key, dflt, value, sizeof value) != ND_OK ||
        nd_snprintf(out, out_sz, "%s=%s", env_name, value) != ND_OK) {
        if (nd_snprintf(out, out_sz, "%s=%s", env_name, dflt) != ND_OK)
            out[0] = '\0';
    }
}

static nd_err app_env(app_env_vars *v, int keypad_fd, int crash_fd, int fb_fd, int svc_fd,
                      bool has_matrix, const char *key_evdev)
{
    const char *root = nd_path_root();

    /* THE CHILD RESOLVES PATHS FOR ITSELF, so it needs the same ND_ROOT we
     * have. Production leaves it empty and this is one wasted environment
     * slot; the host harness stages a root with nd_path_set_root() and never
     * touches the environment, and without this the child would look for
     * app.so at the unprefixed /NeoDCT/System/apps/... and find nothing. */
    if (root != NULL && root[0] != '\0') {
        if (nd_snprintf(v->root, sizeof v->root, "%s=%s", ND_ENV_ROOT, root) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        v->root[0] = '\0';
    }

    if (nd_snprintf(v->keypad, sizeof v->keypad, "%s=%d", ND_ENV_KEYPAD_FD, keypad_fd) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(v->crash, sizeof v->crash, "%s=%d", ND_ENV_CRASH_FD, crash_fd) != ND_OK)
        return ND_ERR_TOOLONG;
    if (fb_fd >= 0) {
        if (nd_snprintf(v->fb, sizeof v->fb, "%s=%d", ND_ENV_FB_FD, fb_fd) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        v->fb[0] = '\0';
    }
    /* Absent when the socketpair could not be made. An app without it simply
     * gets "no service", which is the sentence it drew before this existed --
     * a degraded app beats a launch that failed. */
    if (svc_fd >= 0) {
        if (nd_snprintf(v->svc, sizeof v->svc, "%s=%d", ND_ENV_SERVICE_FD, svc_fd) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        v->svc[0] = '\0';
    }
    /* Set only when true, so "absent" and "no matrix" are the same thing to
     * the child and there is one less state to get wrong. Without this the
     * app has no way to know: its own input is a pipe. nd_app.h, BR-3. */
    if (has_matrix) {
        if (nd_snprintf(v->matrix, sizeof v->matrix, "%s=1", ND_ENV_KEYPAD_MATRIX) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        v->matrix[0] = '\0';
    }
    /* Absent when no device was made, which is the normal case: only an app
     * whose manifest asked for one, on a phone whose keypad is the matrix,
     * gets a node. See nd_proc.h's THE KEY DEVICE. */
    if (key_evdev != NULL && key_evdev[0] != '\0') {
        if (nd_snprintf(v->evdev, sizeof v->evdev, "%s=%s", ND_ENV_KEY_EVDEV, key_evdev) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        v->evdev[0] = '\0';
    }

    /* THE PRESENTATION SUBSET. All three unconditionally, including the
     * "NONE" wallpaper, because an app that sees none of them has to be able
     * to conclude "no core told me" and read the file itself -- which is
     * exactly what a hand-run nd-apprun and nd-shoot do. See nd_proc.h.
     *
     * The core reads settings.prop here as ndusr, which it can. The app
     * cannot, and that is the whole reason this exists. */
    put_setting(v->wallpaper, sizeof v->wallpaper, ND_ENV_UI_WALLPAPER, ND_SET_UI_WALLPAPER,
                ND_SET_UI_WALLPAPER_DFLT);
    put_setting(v->wp_everywhere, sizeof v->wp_everywhere, ND_ENV_UI_WP_EVERYWHERE,
                ND_SET_UI_WP_EVERYWHERE, ND_SET_UI_WP_EVERYWHERE_DFLT);
    put_setting(v->wp_dim, sizeof v->wp_dim, ND_ENV_UI_WP_DIM, ND_SET_UI_WP_APP_DIM,
                ND_SET_UI_WP_APP_DIM_DFLT);
    return ND_OK;
}

/* How many of our own entries build_envp() may append, plus the NULL. */
#define APP_ENVP_OURS 11

/* environ plus ours, with any inherited copy of ours removed so the child
 * cannot pick up a stale descriptor number -- or a stale keypad claim -- from
 * a previous launch. NEODCT_T9 is deliberately NOT in OURS: it is the
 * developer's own override and it is meant to reach the child untouched.
 *
 * ============ WHY THIS ALLOCATES ============
 *
 * It used to fill a fixed const char *[64] on the stack and STOP COPYING when
 * it ran out, silently. The phone's environment is small, so nobody saw it;
 * an ordinary desktop shell exports around 66 entries, and the copy then lost
 * whatever sat at the end of environ -- which is exactly where setenv() puts
 * a variable set just before the launch. The first thing that noticed was
 * NEODCT_T9, whose whole job is to be set by hand and reach the child.
 *
 * Dropping part of a caller's environment because it did not fit is the kind
 * of failure that shows up later as an app that cannot find $HOME. The array
 * is a handful of pointers per launch and an app launch already forks; size
 * it to the real environment and there is no cliff to fall off.
 *
 * owned by the caller; free with free(). The STRINGS are environ's own and
 * the caller's stack buffers -- neither is ours to release. */
static const char **build_envp(const app_env_vars *v)
{
    static const char *const OURS[] = {ND_ENV_KEYPAD_FD "=",
                                       ND_ENV_CRASH_FD "=",
                                       ND_ENV_FB_FD "=",
                                       ND_ENV_ROOT "=",
                                       ND_ENV_SERVICE_FD "=",
                                       ND_ENV_KEYPAD_MATRIX "=",
                                       ND_ENV_KEY_EVDEV "=",
                                       /* The presentation subset is ours too: an inherited copy
                                        * would be the wallpaper the owner had BEFORE they changed
                                        * it, which is worse than none at all. */
                                       ND_ENV_UI_WALLPAPER "=",
                                       ND_ENV_UI_WP_EVERYWHERE "=",
                                       ND_ENV_UI_WP_DIM "="};
    const char **envp;
    size_t have = 0u;
    size_t n = 0u;
    size_t i;

    while (environ[have] != NULL)
        have++;

    envp = calloc(have + APP_ENVP_OURS, sizeof *envp);
    if (envp == NULL)
        return NULL;

    for (i = 0u; i < have; i++) {
        size_t k;
        bool ours = false;

        for (k = 0u; k < ND_ARRAY_LEN(OURS); k++) {
            if (strncmp(environ[i], OURS[k], strlen(OURS[k])) == 0)
                ours = true;
        }
        if (!ours)
            envp[n++] = environ[i];
    }
    envp[n++] = v->keypad;
    envp[n++] = v->crash;
    if (v->fb[0] != '\0')
        envp[n++] = v->fb;
    if (v->root[0] != '\0')
        envp[n++] = v->root;
    if (v->svc[0] != '\0')
        envp[n++] = v->svc;
    if (v->matrix[0] != '\0')
        envp[n++] = v->matrix;
    if (v->evdev[0] != '\0')
        envp[n++] = v->evdev;
    if (v->wallpaper[0] != '\0')
        envp[n++] = v->wallpaper;
    if (v->wp_everywhere[0] != '\0')
        envp[n++] = v->wp_everywhere;
    if (v->wp_dim[0] != '\0')
        envp[n++] = v->wp_dim;
    envp[n] = NULL;
    return envp;
}

/* ------------------------------------------------------------------ *
 * The key device, and the escape hatch -- nd_proc.h has the whole argument
 * ------------------------------------------------------------------ */

typedef struct {
    bool have_kbd;
    nd_uinput_kbd kbd;
    /* NULL for ND_APP_KEYDEV_RAW. The bridge is built WITHOUT a thread and
     * without an input source of its own: this loop is the source, and a
     * second thread scanning the same key stream would double every press. */
    nd_t9_bridge *bridge;
    char node[ND_PATH_MAX];

    /* When ND_KEY_CLEAR went down, monotonic, or 0.0 for "not down". */
    double clear_down_at;
} app_keys;

static void keydev_open(app_keys *k, nd_ui *ui, const nd_app_entry *app)
{
    nd_app_keydev want;

    memset(k, 0, sizeof *k);
    k->kbd.fd = -1;
    if (ui == NULL || app == NULL)
        return;

    want = nd_app_manifest_key_device(app->path);
    if (want == ND_APP_KEYDEV_NONE)
        return;

    /* THE HARDWARE FACT, not ui->has_matrix_keypad.
     *
     * A phone whose keypad is the i2c matrix has nothing in /dev/input at
     * all, which is the only reason to synthesise a keyboard. A dev board
     * with a real one already delivers keys to every program that scans for
     * them, and adding a second device there would make netsurf -- which
     * opens every EV_KEY device it finds, up to eight -- read each press
     * twice. NEODCT_T9 must not change this either way; it is a policy
     * override about what keys MEAN and this is a question about what the
     * phone HAS. */
    if (!nd_input_has_matrix(ui->input)) {
        nd_log(ND_LOG_INPUT,
               "%s asked for a key device; this build's keypad is not the i2c matrix, so whatever "
               "it starts reads /dev/input for itself",
               app->name);
        return;
    }

    if (nd_uinput_open(&k->kbd, NULL, NULL) != ND_OK) {
        /* nd_uinput_open has already logged the errno at error level. Say
         * what it COSTS here, because "uinput unavailable" does not read as
         * "the browser will ignore every key you press". */
        nd_log_err(ND_LOG_INPUT,
                   "%s will have NO KEYS: the core could not create its key device. Check that "
                   "/dev/uinput exists and that 61-neodct-devices.rules grants it to the group "
                   "nd-core runs as.",
                   app->name);
        return;
    }
    k->have_kbd = true;

    if (nd_uinput_event_node(&k->kbd, k->node, sizeof k->node) != ND_OK) {
        nd_log_err(ND_LOG_INPUT,
                   "%s will have NO KEYS: the key device was created but the kernel published no "
                   "/dev/input node for it.",
                   app->name);
        nd_uinput_close(&k->kbd);
        k->have_kbd = false;
        k->node[0] = '\0';
        return;
    }

    /* WAIT FOR IT, do not sleep and hope. The node is root:root 0600 until
     * udevd applies eudev's stock `SUBSYSTEM=="input", GROUP="input"` rule,
     * and the program we are about to start scans /dev/input exactly once
     * with no retry -- libnsfb's evdev_open_inputs(). Starting it inside that
     * window produces a keyless session with no message and no recovery.
     *
     * A timeout is NOT fatal: the node may still become readable while
     * netsurf is loading its config, and something that starts later (mpv,
     * exec'd from a page minutes afterwards) will find it either way. So the
     * path is still exported and the failure is stated rather than hidden. */
    if (!nd_uinput_wait_readable(k->node, ND_PROC_KEYDEV_WAIT_S)) {
        nd_log_err(ND_LOG_INPUT,
                   "%s: %s is still not readable after %.0fs -- udev has not applied group "
                   "`input` to it. Keys may not reach the program this app starts.",
                   app->name, k->node, ND_PROC_KEYDEV_WAIT_S);
    }

    if (want == ND_APP_KEYDEV_BROWSER || want == ND_APP_KEYDEV_SHELL) {
        k->bridge = nd_t9_bridge_new_for_test(
            want == ND_APP_KEYDEV_BROWSER ? ND_BRIDGE_BROWSER : ND_BRIDGE_SHELL, &k->kbd);
        if (k->bridge == NULL) {
            /* Out of memory building a mode machine. Raw keys are still keys;
             * the alternative is an app that cannot be driven at all. */
            nd_log_err(ND_LOG_INPUT, "%s: no memory for the key map; forwarding raw keys",
                       app->name);
        }
    }
    nd_log(ND_LOG_INPUT, "%s: key device %s (%s)", app->name, k->node,
           k->bridge != NULL ? nd_t9_bridge_mode_label(k->bridge) : "raw");
}

static void keydev_close(app_keys *k)
{
    if (k->bridge != NULL) {
        nd_t9_bridge_free_for_test(k->bridge);
        k->bridge = NULL;
    }
    if (k->have_kbd) {
        /* UI_DEV_DESTROY, so the node goes away with the app. A bridge that
         * outlived its app would still be the first thing
         * nd_media_discover_keypad() matched on the NEXT launch, and it would
         * be a device nobody is feeding. */
        nd_uinput_close(&k->kbd);
        k->have_kbd = false;
    }
    k->node[0] = '\0';
}

static void keydev_feed(app_keys *k, const nd_key_event *ev)
{
    if (!k->have_kbd)
        return;
    if (k->bridge != NULL) {
        /* The maps are multi-tap state machines: they answer a PRESS with
         * characters and have nothing to say about a release. */
        if (ev->pressed)
            nd_t9_bridge_handle_code(k->bridge, ev->code);
        return;
    }
    /* Raw. NeoDCT keycodes ARE Linux keycodes (nd_keycodes.h), so the sixteen
     * the phone has go out as themselves. The range check is not paranoia:
     * the same read path also produces ND_KEY_NONE (-1) and
     * ND_KEY_INCOMING_CALL (-2), and casting either to uint16_t would emit a
     * keycode of 65535. */
    if (ev->code > 0 && ev->code < 256)
        (void)nd_uinput_send_raw(&k->kbd, (uint16_t)ev->code, ev->pressed);
}

/* Forward the core's key stream to the child while it runs.
 *
 * PRESSES AND RELEASES BOTH, which is the entire point of OPEN-QUESTIONS
 * answer 2: the child derives held state and repeat from the same evdev
 * records the core sees, and needs no input-device permission to do it.
 *
 * True means the owner has held C long enough to ask for the screen back --
 * see ND_PROC_APP_ABORT_HOLD_S. Nothing is forwarded differently for it: the
 * press already went to the app on its way down, so an app that IS listening
 * has already done whatever C means to it. */
static bool pump_keys(nd_ui *ui, nd_input_channel *ch, app_keys *keys)
{
    nd_key_event ev;

    if (ui == NULL || ui->input == NULL) {
        nap(0.05);
        return false;
    }
    if (nd_input_read_event(ui->input, 0.05, &ev)) {
        keydev_feed(keys, &ev);
        if (nd_input_channel_send(ch, ev.code, ev.pressed) != ND_OK) {
            /* The child stopped reading, or has gone. Neither is our problem
             * -- waitpid will tell us which in a moment. */
        }
        if (ev.code == ND_KEY_CLEAR) {
            /* The FIRST press starts the clock and a repeat does not restart
             * it. C does not auto-repeat on this phone, but a dev keyboard
             * does, and a hold that kept resetting its own timer could never
             * reach the threshold there. */
            if (ev.pressed) {
                if (keys->clear_down_at == 0.0)
                    keys->clear_down_at = monotonic_now();
            } else {
                keys->clear_down_at = 0.0;
            }
        } else if (ev.pressed) {
            /* Any other key means this was not a deliberate hold. */
            keys->clear_down_at = 0.0;
        }
    }
    if (keys->clear_down_at != 0.0 &&
        monotonic_now() - keys->clear_down_at >= ND_PROC_APP_ABORT_HOLD_S) {
        keys->clear_down_at = 0.0;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Which user an app runs as -- see nd_proc.h for the reasoning
 * ------------------------------------------------------------------ */

/* Stock apps that hold root because their one privileged operation has no
 * route through nd_svc.
 *
 * ============ IT IS EMPTY, AND THAT IS THE POINT ============
 *
 * NO STOCK APP RUNS AS ROOT. Every one of them is launched as ndusr with
 * no_new_privs set, and the only apps on this phone that are not are the
 * engineering ones, when engineering mode is on, deliberately.
 *
 * The five that used to be here, and what each was traded for:
 *
 *   Power, Update, Downgrade   all three wanted only reboot and poweroff, and
 *                              both are now verbs on the service socket
 *                              (spec-app-services.md section 9). Downgrade
 *                              cost nothing extra -- it has no halt code of
 *                              its own and reaches Update's through dlopen.
 *   Clock                      settimeofday(), now nd_svc_set_clock(), which
 *                              the core performs after refusing any date
 *                              outside what this build will believe.
 *   Settings                   the SD-card format helper, now
 *                              nd_svc_format_card() -- and note that the verb
 *                              takes NO DEVICE: the core reads the card
 *                              itself, so there is no block-device name for
 *                              an app to choose. Section 10.
 *
 * ============ IF YOU ARE ABOUT TO ADD ONE ============
 *
 * Don't, until the verb has been tried and found impossible; the five above
 * all looked like they needed root and none of them did. If a name really
 * must go here, WRITE IT AS A WHOLE PATH, as every entry above was written,
 * because the name alone is not safe to match on:
 *
 * "Does this app's directory end in /Power" grants root to a directory called
 * Power ANYWHERE -- and while nothing today can create one outside the two
 * read-only directories the scanner reads, the moment a third app directory
 * exists (a user-installed set is the obvious one) a folder named Power in it
 * inherits reboot. That is a privilege grant arriving as a side effect of a
 * feature nobody connected to privilege, which is exactly how this kind of
 * hole gets made.
 *
 * A full-path match cannot do that. /NeoDCT/System/apps/Power is a specific
 * directory on a dm-verity'd squashfs, and being called Power buys nothing at
 * all anywhere else.
 *
 * The NULL is the whole array. C forbids a zero-length one, and the loop
 * below reads the terminator, so an empty policy costs a pointer and no
 * special case. */
static const char *const ROOT_STOCK_APPS[] = {
    NULL,
};

/* Is `path` inside `dir`? A prefix test that stops at a component boundary,
 * because a plain strncmp would also match /NeoDCT/System/engineering/appsX
 * -- which nothing creates today, and which would be a privilege grant if
 * anything ever did. */
static bool path_under(const char *path, const char *dir)
{
    size_t n;

    if (path == NULL || dir == NULL)
        return false;
    n = strlen(dir);
    if (strncmp(path, dir, n) != 0)
        return false;
    return path[n] == '/' && path[n + 1u] != '\0';
}

/* Apps that run as ndusr_ut inside a mount namespace. See nd_proc.h for why
 * this is the core's job and not the app's, and for why it is whole paths.
 *
 * One entry, and it is the browser. The media player is not listed because it
 * is not an app -- netsurf exec's neodct-play when a <video> is clicked, and
 * it inherits both the uid and the namespace from its parent, which is the
 * whole point of confining the parent. */
static const char *const UNTRUSTED_APPS[] = {
    ND_PATH_APPS_DIR "/Browser",
    NULL,
};

bool nd_proc_app_is_untrusted(const nd_app_entry *app)
{
    size_t i;

    /* Fail closed means something DIFFERENT here from what it means in
     * nd_proc_app_needs_root(), and the asymmetry is deliberate. There, an
     * unknown app must not get root, so the safe answer is false. Here, an
     * unknown app is a normal app: answering true would confine something
     * that was never meant to be confined and break it in ways nobody
     * predicted. Both defaults are "an app we cannot identify is an ordinary
     * ndusr app". */
    if (app == NULL)
        return false;

    for (i = 0u; UNTRUSTED_APPS[i] != NULL; i++) {
        if (strcmp(app->path, UNTRUSTED_APPS[i]) == 0)
            return true;
    }

    /* ============ AND EVERYTHING THE OWNER INSTALLED ============
     *
     * This is the ONE prefix test in the tree that decides a privilege, and
     * everywhere else the rule is whole-path comparison precisely to stop
     * "/NeoDCT/System/apps/../../tmp/Evil" reading as a stock app. So why is a
     * prefix safe here?
     *
     * Because it is the direction that matters. Everywhere else the prefix
     * would GRANT something, and a traversal that escapes the prefix would
     * grant it wrongly. Here the prefix TAKES something away, so the only thing
     * a traversal can achieve is to escape into being confined -- which is the
     * safe side. An attacker gains nothing by making a path look like it is
     * under the apps directory.
     *
     * The direction it could be attacked from is the other one: a path that IS
     * under the user apps directory but does not look like it, and so escapes
     * confinement. That cannot happen from here, because app->path is not
     * attacker-supplied text -- it is built by nd_ui_scan_apps() from the
     * directory it was asked to read, so an app found under
     * ND_PATH_USER_APPS_DIR has that string as its literal prefix.
     *
     * A symlink under the apps directory pointing into /NeoDCT/System is the
     * remaining case, and it does not help either: the app would still be
     * launched by its apps-directory path, still match here, and still be
     * confined. It would run stock code with LESS privilege, not more.
     *
     * ============ AND IT IS A CARD NOW ============
     *
     * The directory moved to /NeoDCT/User/sdcard/apps, which strengthens the
     * argument rather than complicating it. A card comes out of the phone and
     * goes into a PC, so an app.so there is bytes a stranger chose in the most
     * literal sense available -- and every mode on the card is a claim until
     * neodct-sdcard's apply_layout() has restated it. A rule about WHERE code
     * lives survives all of that; a rule about what the code says about itself
     * would not. */
    {
        static const char PREFIX[] = ND_PATH_USER_APPS_DIR "/";

        if (strncmp(app->path, PREFIX, sizeof PREFIX - 1u) == 0)
            return true;
    }
    return false;
}

bool nd_proc_app_needs_root(const nd_app_entry *app, bool engineering_mode)
{
    size_t i;

    /* Fail closed. A caller with no app to describe gets the confined answer,
     * because the alternative is that a bug hands out root. */
    if (app == NULL)
        return false;

    /* The second gate goes here: `engineering_mode && nd_boot_engmode() &&`.
     * See the header on why one of these lives on the writable partition and
     * the other must not. */
    if (engineering_mode && path_under(app->path, ND_PATH_ENG_APPS_DIR))
        return true;

    for (i = 0u; ROOT_STOCK_APPS[i] != NULL; i++) {
        if (strcmp(app->path, ROOT_STOCK_APPS[i]) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * "wantsPerformance" -- nd_proc.h has the whole argument
 * ------------------------------------------------------------------ */

/* Read exactly the way nd_app_manifest_use_wallpaper() reads its key, and
 * from the app's own manifest.json on the read-only side of the world.
 *
 * ABSENT, unparseable and non-boolean all mean FALSE, which is the opposite
 * default from useWallpaper: this one costs battery, so silence must not buy
 * it. That also makes every manifest written before the key existed behave
 * as it always did. */
bool nd_proc_app_wants_performance(const char *app_dir)
{
    char path[ND_PATH_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    bool want;

    if (app_dir == NULL || app_dir[0] == '\0')
        return false;
    if (nd_snprintf(path, sizeof path, "%s/manifest.json", app_dir) != ND_OK)
        return false;
    if (nd_json_parse_file(path, &doc, NULL, 0u) != ND_OK)
        return false;

    root = nd_json_root(doc);
    want = root != NULL && nd_json_type_of(root) == ND_JSON_OBJECT &&
           nd_json_get_bool(root, ND_APP_KEY_WANTS_PERFORMANCE, false);
    nd_json_free(doc);
    return want;
}

/* scaling_max_freq, on its own.
 *
 * A near-copy of nd_cpufreq.c's write_attr(), and deliberately not a call to
 * nd_cpufreq_set(): that function writes the SAME value to both ends of the
 * range, which is what Sleepy wants (a held frequency it can measure) and the
 * exact opposite of what this wants. Raising only the ceiling leaves the
 * governor free to idle the chip between frames; pinning both would hold an
 * emulator's phone at 1.2 GHz through its pause menu.
 *
 * The fclose() check is not belt-and-braces. sysfs validates in its store
 * handler, so a frequency outside the OPP table is refused at the FLUSH and
 * not at the write, and missing that is how a refused write becomes a silent
 * one. */
static bool write_max_khz(int32_t khz)
{
    char resolved[ND_PATH_MAX];
    char value[16];
    FILE *f;
    bool ok;

    if (nd_snprintf(value, sizeof value, "%d", (int)khz) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, ND_CPUFREQ_MAX) != ND_OK)
        return false;
    f = fopen(resolved, "wb");
    if (f == NULL)
        return false;
    ok = fputs(value, f) >= 0;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* -1 means "nothing was raised, restore nothing"; anything else is the
 * ceiling to put back. Returned rather than stored in a static, so a launch
 * that never happens cannot leave the phone believing it owes a restore. */
static int32_t perf_ceiling_raise(const nd_app_entry *app)
{
    nd_cpufreq_table table;
    nd_cpufreq_state state;
    int32_t top;

    if (!nd_proc_app_wants_performance(app->path))
        return -1;

    /* No cpufreq at all is the ordinary case on QEMU and on any kernel built
     * without CONFIG_CPU_FREQ. It is a fact about the machine, not a failure
     * of the app, so it is silent. */
    if (nd_cpufreq_read_table(&table) != ND_OK || table.n == 0u)
        return -1;
    if (nd_cpufreq_read_state(&state) != ND_OK || state.max_khz <= 0)
        return -1;

    /* Ascending, so the last entry is the top the kernel is offering. The app
     * does not get to name one. */
    top = table.khz[table.n - 1u];
    if (top <= state.max_khz)
        return -1; /* already there; nothing to raise and nothing to restore */

    if (!write_max_khz(top)) {
        /* Worth one line and no more. The cause is nearly always that the
         * udev rule has not applied GROUP=ndusr to the attribute, and the app
         * still runs -- slowly, which is what it would have done anyway. */
        nd_log_err(ND_LOG_OS,
                   "%s asked for performance and did not get it: cannot write %s. Check that "
                   "61-neodct-devices.rules has given it to the group nd-core runs as.",
                   app->name, ND_CPUFREQ_MAX);
        return -1;
    }
    nd_log(ND_LOG_OS, "%s: CPU ceiling raised from %d to %d kHz for the life of the app",
           app->name, state.max_khz, top);
    return state.max_khz;
}

/* The other half, and it must run on EVERY path out of the launcher --
 * a crash, a SIGTERM from an incoming call, a spawn that never happened.
 * nd_cpufreq.h is explicit that pinning is sticky and that nothing puts the
 * range back on its own; an app that could leave the phone fast is the one
 * failure here that would really cost the owner a battery. */
static void perf_ceiling_restore(int32_t saved_khz)
{
    /* No write-order question here, unlike nd_cpufreq_set(). That function
     * has to consider nd_cpufreq_max_first() because it moves BOTH ends and
     * they clamp against each other; this only ever moves the ceiling back
     * down to a value that was already at or above scaling_min_freq when it
     * was read, so there is nothing for the kernel to clamp it against. */
    if (saved_khz < 0)
        return;
    if (!write_max_khz(saved_khz))
        nd_log_err(ND_LOG_OS, "could not put the CPU ceiling back to %d kHz; the phone is left "
                              "free to run fast",
                   saved_khz);
}

nd_err nd_proc_launch_app(nd_ui *ui, const nd_app_entry *app, const char *entry, const char *arg,
                          nd_crash_info *crash_out)
{
    char runner[ND_PATH_MAX];
    const char *path;
    const char *argv[5];
    const char **envp = NULL;
    app_env_vars env_vars;
    app_keys keys;
    nd_input_channel ch = {-1, -1};
    int crash_pipe[2] = {-1, -1};
    nd_svc_server *svc = NULL;
    int svc_fd = -1;
    bool untrusted;
    /* Which user the child becomes, by NAME. The broker looks the uid up on
     * its own side rather than being handed one: nd-core is unprivileged, so a
     * uid on that wire would be a uid the caller chose. NULL means no drop. */
    const char *run_user = NULL;
    int fb_fd = -1;
    /* The ceiling to put back when this app is done, or -1 for "nothing was
     * raised". Declared up here with everything else the cleanup touches, so
     * that every `goto done` restores it whether or not the launch got as far
     * as asking. */
    int32_t perf_saved_khz = -1;
    nd_proc_spec spec;
    nd_proc_status st;
    nd_crash_info info;
    pid_t pid = -1;
    nd_err rc = ND_OK;

    if (ui == NULL || app == NULL)
        return ND_ERR_INVAL;
    if (crash_out != NULL)
        memset(crash_out, 0, sizeof *crash_out);

    /* Decided once, up here, because it changes two things far apart: whether
     * a service socket is created at all (below) and which user the child
     * becomes (further below). Reading the policy twice would let the two
     * drift. */
    untrusted = nd_proc_app_is_untrusted(app);

    /* Zeroed here and not only in keydev_open(), because the first `goto
     * done` is above the call and the cleanup below is unconditional. */
    memset(&keys, 0, sizeof keys);
    keys.kbd.fd = -1;

    if (nd_input_channel_open(&ch) != ND_OK) {
        nd_log_err(ND_LOG_OS, "App load failed: no key channel for %s", app->name);
        return ND_ERR_IO;
    }
    if (pipe(crash_pipe) != 0) {
        nd_log_err(ND_LOG_OS, "App load failed: no crash pipe for %s: %s", app->name,
                   strerror(errno));
        nd_input_channel_close(&ch);
        return ND_ERR_IO;
    }

    /* The one place a private header earns its keep: the child inherits the
     * framebuffer the core already opened, so an app process needs no
     * /dev/fb0 permission at all. NULL fb (a headless core, a unit test) just
     * means the variable is not set and the child opens the device itself. */
    if (ui->fb != NULL)
        fb_fd = ui->fb->fd;

    /* The route back to the core's services (nd_svc.h). Created HERE, before
     * the fork, because the descriptor has to exist to be listed in the plan
     * below -- but the thread that serves it is started AFTER the fork, so
     * that CODING-STANDARDS.md 1.1 keeps its guarantee and no mutex of ours
     * can be held by a thread at the instant the child is made.
     *
     * A failure is not fatal: the app runs without a channel and every
     * nd_svc_* call answers "not present", which is the sentence Messages
     * and the Modem app drew before this existed. */
    /* ...and an UNTRUSTED app gets the READ-ONLY HALF OF IT.
     *
     * nd_svc validates the RECORD, never the SENDER -- there is no
     * SO_PEERCRED, no SCM_CREDENTIALS, nothing (spec-app-services.md 5). So
     * the socket is a straight line from whoever holds the descriptor to a
     * root thread that will send an SMS, format the card or power the phone
     * off on their behalf. Handing one of THOSE to the process whose job is
     * to render whatever the internet sends it would make every one of those
     * verbs reachable from a web page.
     *
     * The socket used to be withheld outright for that reason, and while the
     * browser was the only untrusted thing on the phone that was the right
     * trade. It stopped being right when an app the owner installed became
     * untrusted too: it got no channel at all, so it could not read the
     * battery, could not read the clock and could not tell whether the phone
     * had signal -- three things a game's pause screen wants, none of which
     * costs the owner anything. "No SEND_SMS" is not a reason for "no
     * BATTERY".
     *
     * So the socket is now created either way and carries a MASK, bound to it
     * here, by the core, before the fork. That is exactly as unforgeable as
     * the socket's existence already was -- there is nothing on the wire for
     * a child, or anything the child execs, to widen -- and it is the same
     * property the withholding relied on. nd_svc.h has the full argument and
     * the list of what stays out. */
    if (nd_svc_server_open(&svc, untrusted ? ND_SVC_OPS_UNTRUSTED : ND_SVC_OPS_ALL) == ND_OK)
        svc_fd = nd_svc_server_child_fd(svc);

    /* AFTER the two unconditional returns above and BEFORE the first `goto
     * done`, which is the window in which the cleanup below is guaranteed to
     * run. An app that asked for the CPU and then failed to launch must not
     * leave the ceiling up. */
    perf_saved_khz = perf_ceiling_raise(app);

    path = apprun_path(runner, sizeof runner);
    argv[0] = path;
    argv[1] = app->path;
    argv[2] = entry != NULL ? entry : ND_APP_ENTRY_RUN;
    argv[3] = arg; /* NULL when there is none, which also terminates argv */
    argv[4] = NULL;

    /* BEFORE the environment, because the node's path goes into it, and
     * before the spawn, because the program the child starts scans
     * /dev/input once and never again. */
    keydev_open(&keys, ui, app);

    /* nd_input_has_matrix(ui->input) and NOT ui->has_matrix_keypad.
     *
     * This used to pass the core's flag, with a comment calling that
     * deliberate -- "a developer who forces T9 on in the core gets it in the
     * app too". It cannot be, because ui->has_matrix_keypad already has the
     * NEODCT_T9 override folded in and nd_ui_init_app() folds it in AGAIN on
     * the far side (nd_ui.c: nd_ui_t9_active_for(matrix_from_env())). So the
     * two questions nd_app.h says must stay apart were the same value, and
     * both failed:
     *
     *   NEODCT_T9=0 on a real phone made the flag false, so the app was told
     *   its keypad was not a matrix -- and everything that decides "is the
     *   console's keyboard missing?" from it, including whether to ask for a
     *   key device, answered no. Turning T9 off took the keypad away.
     *
     *   NEODCT_T9=1 on a dev box with a real keyboard made it true, so an app
     *   would bridge a second keyboard alongside the first and netsurf --
     *   which opens every EV_KEY device it finds -- read every press twice.
     *
     * What crosses the boundary is the HARDWARE FACT. The policy is applied
     * once, on the app's side, on top of it. */
    if (app_env(&env_vars, ch.read_fd, crash_pipe[1], fb_fd, svc_fd, nd_input_has_matrix(ui->input),
                keys.node) != ND_OK) {
        rc = ND_ERR_TOOLONG;
        goto done;
    }
    envp = build_envp(&env_vars);
    if (envp == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.envp = envp;
    spec.owner = ND_OWNER_APP;
    spec.n_fds = 0u;

    /* Become ndusr unless this app is one of the two exceptions in
     * nd_proc_app_needs_root().
     *
     * Nothing an ordinary app needs is lost by this, and that is a property
     * of the design rather than luck: the framebuffer, the key channel, the
     * crash pipe and the service socket all arrive as INHERITED DESCRIPTORS,
     * and an open file keeps the access it was opened with no matter who the
     * process becomes afterwards. nd_app.h has said so since it was written
     * -- "the already-open framebuffer, so the child does not need /dev/fb0
     * permission" -- and this is the line that finally makes that sentence
     * do something.
     *
     * The lookup is here, in the parent, because getpwnam() allocates and
     * reads files and neither is allowed between fork and execve. An image
     * built without the users table leaves run_as.valid false, which
     * nd_priv_become() treats as a documented no-op: the app then runs
     * exactly as every build before this one did, rather than refusing to
     * open. */
    if (untrusted) {
        /* Confined by the core, because only the core still has the privilege
         * to do it. nd_proc.h carries the whole argument.
         *
         * Two lists, not one. The browser and an app the owner installed are
         * both untrusted and are not untrusted in the same WAY: the browser
         * is stock code with a hostile input, and an installed app is code
         * from a card. The one thing that separates them here is the
         * browser's own profile directory, which an installed app has no
         * business in -- see ND_PROC_INSTALLED_HIDE_EXTRA. */
        static const char *const hide_common[] = ND_PROC_UNTRUSTED_HIDE_PATHS;
        const char *hide[ND_ARRAY_LEN(hide_common) + 1u];
        size_t h = 0u;

        while (hide_common[h] != NULL && h + 2u < ND_ARRAY_LEN(hide)) {
            hide[h] = hide_common[h];
            h++;
        }
        /* Under the apps directory means the owner installed it, which is the
         * one case that does not get to see the browser's profile. */
        {
            static const char PREFIX[] = ND_PATH_USER_APPS_DIR "/";

            if (strncmp(app->path, PREFIX, sizeof PREFIX - 1u) == 0)
                hide[h++] = ND_PROC_INSTALLED_HIDE_EXTRA;
        }
        hide[h] = NULL;

        run_user = ND_PRIV_USER_UT;

        /* ============ THIS ONE REFUSES ============
         *
         * It used to log and carry on, which meant an image without the users
         * table ran NETSURF AS ROOT -- the single process on this phone that
         * most needs not to be, parsing HTML off the network with every
         * capability the core has. One nd_log_err line in a boot log is not a
         * fair warning for that; it was missed for exactly as long as you
         * would expect, and the first anyone knew was a `top` on a real build
         * showing `root ... /usr/bin/netsurf-fb file:///NeoDCT/...`.
         *
         * An untrusted app that cannot be confined does not run. Not opening
         * is a bad outcome; opening with root is a worse one, and unlike the
         * first it is invisible. The log line says what to do about it,
         * because the cause is nearly always a buildroot output/ tree older
         * than the users table -- .config is generated from the defconfig
         * ONCE, so a stale one never grows the BR2_ROOTFS_USERS_TABLES line
         * no matter how many times it is rebuilt. */
        if (!nd_priv_lookup(ND_PRIV_USER_UT, &spec.run_as)) {
            nd_log_err(ND_LOG_OS,
                       "REFUSING to launch %s: there is no " ND_PRIV_USER_UT
                       " in this image, so it cannot be confined, and an "
                       "untrusted app is not run with the core's privileges. "
                       "Rebuild with the users table: cd buildroot && make "
                       "neodct_qemu_defconfig && make. nd-selftest reports the "
                       "same thing.",
                       app->name);
            rc = ND_ERR_PERM;
            goto done;
        }
        spec.no_new_privs = true;
        spec.private_mounts = true;
        spec.hide_paths = hide;
    } else if (!nd_proc_app_needs_root(app, nd_ui_engineering_mode(ui))) {
        run_user = ND_PRIV_USER;
        /* A trusted app still runs -- refusing here would brick every app on
         * the phone rather than confine anything -- but it is an ERROR and it
         * says so. It was nd_log(), an ordinary informational line, which is
         * how a whole phone came to be running every app as root with nothing
         * in the log that looked like a problem. */
        if (!nd_priv_lookup(ND_PRIV_USER, &spec.run_as))
            nd_log_err(ND_LOG_OS,
                       "SECURITY: no " ND_PRIV_USER " in this image, so %s runs "
                       "as ROOT. Every app does. Rebuild with the users table: "
                       "cd buildroot && make neodct_qemu_defconfig && make.",
                       app->name);
        /* Only on the dropped ones. On a root app it would forbid nothing
         * that root cannot already do, and an engineering app is the one
         * place something might legitimately want to exec a setuid helper. */
        spec.no_new_privs = true;
    }
    /* The three inherited descriptors keep the numbers they already have; the
     * child is told what they are rather than being given fixed slots, which
     * is what nd_app.h means by "the numbers themselves are not fixed". Each
     * still has to survive the exec, and O_CLOEXEC is set on all of them, so
     * each is listed as a map onto itself purely to clear that flag. */
    spec.fds[spec.n_fds].child_fd = ch.read_fd;
    spec.fds[spec.n_fds].our_fd = ch.read_fd;
    spec.n_fds++;
    spec.fds[spec.n_fds].child_fd = crash_pipe[1];
    spec.fds[spec.n_fds].our_fd = crash_pipe[1];
    spec.n_fds++;
    if (fb_fd >= 0) {
        spec.fds[spec.n_fds].child_fd = fb_fd;
        spec.fds[spec.n_fds].our_fd = fb_fd;
        spec.n_fds++;
    }
    if (svc_fd >= 0) {
        spec.fds[spec.n_fds].child_fd = svc_fd;
        spec.fds[spec.n_fds].our_fd = svc_fd;
        spec.n_fds++;
    }

    nd_log(ND_LOG_OS, "Launching %s: %s %s %s", app->name, path, app->path, argv[2]);

    /* THE ONE OPERATION THE UI CANNOT DO ANY MORE.
     *
     * setgroups() needs CAP_SETGID, and an ndusr nd-core has none -- measured,
     * not assumed: dropping the core and launching an app fails at exit 122,
     * ND_PRIV_STEP_SETGROUPS, before execve. Everything else on this path the
     * unprivileged core still does itself, which is why only the fork and the
     * reap cross the socket. */
    if (nd_broker_default() != NULL)
        rc = nd_broker_spawn(nd_broker_default(), path, &spec, run_user, &pid);
    else
        rc = nd_proc_spawn(path, &spec, &pid);
    if (rc != ND_OK)
        goto done;

    /* Our copies of the child's ends go now: while we hold the crash pipe's
     * write end open, reading it would block forever instead of reporting the
     * clean exit that produced nothing. */
    nd_input_channel_close_read(&ch);
    (void)close(crash_pipe[1]);
    crash_pipe[1] = -1;

    /* AFTER the fork, never before it. nd_svc_server_start() closes our copy
     * of the child's socket end too, for the same reason the two lines above
     * exist: while we hold it, the child exiting would not look like EOF. */
    if (svc != NULL) {
        svc_fd = -1;
        if (nd_svc_server_start(svc, ui) != ND_OK) {
            nd_svc_server_free(svc);
            svc = NULL;
        }
    }

    /* The child can exit at any instant, and the next key we forward then hits
     * a pipe with no reader. nd-core ignores SIGPIPE for exactly this reason,
     * but a library must not depend on its caller having done that -- a unit
     * test that launches an app would be killed by the tenth keystroke. */
    {
        struct sigaction ign;
        struct sigaction prev;
        bool restore;

        memset(&ign, 0, sizeof ign);
        ign.sa_handler = SIG_IGN;
        (void)sigemptyset(&ign.sa_mask);
        restore = sigaction(SIGPIPE, &ign, &prev) == 0;

        for (;;) {
            /* The app is the BROKER's child when there is one, so only the
             * broker can reap it -- waitpid() here would return ECHILD for a
             * process this one never forked. */
            nd_err w = (nd_broker_default() != NULL)
                           ? nd_broker_wait(nd_broker_default(), pid, 0.0, &st)
                           : nd_proc_wait(pid, 0.0, &st);

            if (w == ND_OK)
                break;
            /* A broker that has died takes the app's exit status with it.
             * Stop rather than spin: the child is unreachable either way. */
            if (nd_broker_default() != NULL && w != ND_ERR_TIMEOUT)
                break;
            /* And without a broker, ND_ERR_NOTFOUND means somebody else
             * reaped it. Waiting cannot change whose child it is, so this
             * loop must not treat it as "not yet". */
            if (w == ND_ERR_NOTFOUND)
                break;

            /* The watchdog's heartbeat, taken here because THIS is the loop
             * that wedged. While an app is up the core's UI thread spends all
             * of its time in this for(;;), and a privileged operation that
             * blocks underneath it -- the card format holding the broker's
             * round-trip mutex for four minutes is the case that was reported
             * as "the phone froze" -- stops the beat without stopping the
             * loop's own liveness in any way an outside observer could see.
             * nd_ui_watch_beat() is what turns that into one log line naming
             * what it was blocked on. */
            nd_ui_watch_beat();

            if (pump_keys(ui, &ch, &keys)) {
                /* ============ THE ONE WAY OUT ============
                 *
                 * Everything above this line forwards keys and inspects
                 * none of them, which is right until the program on the far
                 * side stops answering them. Then there is no key sequence
                 * left that ends it: netsurf's own quit is Ctrl-Q, an
                 * emulator's is its pause menu, and both are reached
                 * THROUGH the input that has failed. The phone is not hung,
                 * but it looks exactly like a phone that is.
                 *
                 * So the core keeps one gesture for itself, and it is the
                 * longest hold on the key that already means "back". */
                nd_log_err(ND_LOG_OS,
                           "%s: CLEAR held for %.0fs -- the core is taking the screen back",
                           app->name, ND_PROC_APP_ABORT_HOLD_S);
                if (nd_proc_terminate(pid, 2.0, &st) == ND_OK)
                    break;
            }
        }

        if (restore)
            (void)sigaction(SIGPIPE, &prev, NULL);
    }

    /* The child is gone, so nothing can ask for a service any more. Bounded:
     * a request still in flight is left to finish on its own thread rather
     * than made the core's problem. See ND_SVC_JOIN_S. */
    nd_svc_server_stop(svc);
    svc = NULL;

    memset(&info, 0, sizeof info);
    if (nd_crash_read_report(crash_pipe[0], &info)) {
        /* The child told us what hit it, which is better than what waitpid
         * knows: si_code and the faulting address only exist on that side. */
        if (st.signalled)
            info.signo = st.signo;
    } else if (st.signalled) {
        info.from_signal = true;
        info.signo = st.signo;
    } else {
        info.exit_status = st.exit_status;
    }

    if (st.signalled) {
        nd_log_err(ND_LOG_OS, "App crashed: %s (%s, %s)", app->name, app->path,
                   nd_crash_signal_name(st.signo));
    } else if (st.exit_status != 0) {
        nd_log_err(ND_LOG_OS, "App crashed: %s (%s, exit %d)", app->name, app->path,
                   st.exit_status);
    }

    if (st.signalled || st.exit_status != 0) {
        /* SIGTERM is not a crash: it is how the core reclaims the screen for
         * an incoming call, and the Python's IncomingCall was explicitly not
         * treated as one either. */
        if (st.signalled && (st.signo == SIGTERM || st.signo == SIGKILL)) {
            nd_log(ND_LOG_OS, "%s was stopped by the core, not by a fault", app->name);
        } else {
            nd_crash_show_app(ui, NULL, app->name, &info);
        }
    }

    if (crash_out != NULL)
        *crash_out = info;

done:
    free((void *)envp);
    /* The key device goes before the UI resumes reading the keypad, so
     * nothing is left publishing an evdev node the core is no longer
     * feeding -- and so nd_media_discover_keypad() cannot match a dead one on
     * the next launch. */
    keydev_close(&keys);
    if (svc != NULL)
        nd_svc_server_free(svc); /* only reached when the launch never happened */
    if (crash_pipe[0] >= 0)
        (void)close(crash_pipe[0]);
    if (crash_pipe[1] >= 0)
        (void)close(crash_pipe[1]);
    nd_input_channel_close(&ch);

    /* Before the UI comes back, and unconditionally. nd_cpufreq.h is explicit
     * that nothing puts the range back on its own, so this is the only thing
     * standing between "the emulator was fast" and "the phone is fast for
     * ever". A no-op when nothing was raised. */
    perf_ceiling_restore(perf_saved_khz);

    /* ALWAYS, whatever happened -- the wallpaper, engineering mode, the app
     * list and the unread-SMS count may all have changed inside the app. */
    nd_ui_refresh_after_app(ui);
    return rc;
}
