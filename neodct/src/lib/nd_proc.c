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
#include "nd_crash.h"
#include "nd_fb_priv.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"

#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "nd_priv.h"
#include "nd_svc.h"
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
    pid_t pid;
    int status;
} reaped_slot;

static reaped_slot g_reaped[REAP_RING];
static volatile sig_atomic_t g_reap_next;
static bool g_reaper_installed;

static void remember(pid_t pid, int status)
{
    int idx = (int)g_reap_next % REAP_RING;

    g_reap_next = (sig_atomic_t)((idx + 1) % REAP_RING);
    g_reaped[idx].pid = pid;
    g_reaped[idx].status = status;
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

/* One non-blocking check: the reaper's ring first, then the kernel. */
static bool collect(pid_t pid, nd_proc_status *out)
{
    int status = 0;
    pid_t r;

    if (take_remembered(pid, &status)) {
        fill_status(out, pid, status);
        return true;
    }

    do {
        r = waitpid(pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);

    if (r == pid) {
        fill_status(out, pid, status);
        return true;
    }
    if (r < 0 && errno == ECHILD) {
        /* The reaper may have taken it between the two checks above. */
        if (take_remembered(pid, &status)) {
            fill_status(out, pid, status);
            return true;
        }
        /* Genuinely nobody's child. Report it as an exit of unknown status
         * rather than spinning until the timeout. */
        memset(out, 0, sizeof *out);
        out->pid = pid;
        out->exited = true;
        out->exit_status = 0;
        return true;
    }
    return false;
}

nd_err nd_proc_wait(pid_t pid, double timeout_s, nd_proc_status *out)
{
    double deadline;

    if (pid <= 0 || out == NULL)
        return ND_ERR_INVAL;

    if (collect(pid, out))
        return ND_OK;
    if (timeout_s == 0.0)
        return ND_ERR_TIMEOUT;

    deadline = monotonic_now() + (timeout_s < 0.0 ? 0.0 : timeout_s);
    for (;;) {
        /* 5 ms is short enough that an app exit feels instant and long enough
         * that waiting for a browsing session costs nothing measurable. */
        nap(0.005);
        if (collect(pid, out))
            return ND_OK;
        if (timeout_s > 0.0 && monotonic_now() >= deadline)
            return ND_ERR_TIMEOUT;
    }
}

nd_err nd_proc_terminate(pid_t pid, double grace_s, nd_proc_status *out)
{
    nd_proc_status local;

    if (pid <= 0)
        return ND_ERR_INVAL;
    if (out == NULL)
        out = &local;

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
static nd_err app_env(char *keypad, size_t keypad_sz, char *crash, size_t crash_sz, char *fbv,
                      size_t fb_sz, char *rootv, size_t root_sz, char *svcv, size_t svc_sz,
                      char *matrixv, size_t matrix_sz, int keypad_fd, int crash_fd, int fb_fd,
                      int svc_fd, bool has_matrix)
{
    const char *root = nd_path_root();

    /* THE CHILD RESOLVES PATHS FOR ITSELF, so it needs the same ND_ROOT we
     * have. Production leaves it empty and this is one wasted environment
     * slot; the host harness stages a root with nd_path_set_root() and never
     * touches the environment, and without this the child would look for
     * app.so at the unprefixed /NeoDCT/System/apps/... and find nothing. */
    if (root != NULL && root[0] != '\0') {
        if (nd_snprintf(rootv, root_sz, "%s=%s", ND_ENV_ROOT, root) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        rootv[0] = '\0';
    }

    if (nd_snprintf(keypad, keypad_sz, "%s=%d", ND_ENV_KEYPAD_FD, keypad_fd) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(crash, crash_sz, "%s=%d", ND_ENV_CRASH_FD, crash_fd) != ND_OK)
        return ND_ERR_TOOLONG;
    if (fb_fd >= 0) {
        if (nd_snprintf(fbv, fb_sz, "%s=%d", ND_ENV_FB_FD, fb_fd) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        fbv[0] = '\0';
    }
    /* Absent when the socketpair could not be made. An app without it simply
     * gets "no service", which is the sentence it drew before this existed --
     * a degraded app beats a launch that failed. */
    if (svc_fd >= 0) {
        if (nd_snprintf(svcv, svc_sz, "%s=%d", ND_ENV_SERVICE_FD, svc_fd) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        svcv[0] = '\0';
    }
    /* Set only when true, so "absent" and "no matrix" are the same thing to
     * the child and there is one less state to get wrong. Without this the
     * app has no way to know: its own input is a pipe. nd_app.h, BR-3. */
    if (has_matrix) {
        if (nd_snprintf(matrixv, matrix_sz, "%s=1", ND_ENV_KEYPAD_MATRIX) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        matrixv[0] = '\0';
    }
    return ND_OK;
}

/* How many of our own entries build_envp() may append, plus the NULL. */
#define APP_ENVP_OURS 7

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
static const char **build_envp(const char *keypad, const char *crash, const char *fbv,
                               const char *rootv, const char *svcv, const char *matrixv)
{
    static const char *const OURS[] = {ND_ENV_KEYPAD_FD "=",  ND_ENV_CRASH_FD "=",
                                       ND_ENV_FB_FD "=",      ND_ENV_ROOT "=",
                                       ND_ENV_SERVICE_FD "=", ND_ENV_KEYPAD_MATRIX "="};
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
    envp[n++] = keypad;
    envp[n++] = crash;
    if (fbv[0] != '\0')
        envp[n++] = fbv;
    if (rootv[0] != '\0')
        envp[n++] = rootv;
    if (svcv[0] != '\0')
        envp[n++] = svcv;
    if (matrixv[0] != '\0')
        envp[n++] = matrixv;
    envp[n] = NULL;
    return envp;
}

/* Forward the core's key stream to the child while it runs.
 *
 * PRESSES AND RELEASES BOTH, which is the entire point of OPEN-QUESTIONS
 * answer 2: the child derives held state and repeat from the same evdev
 * records the core sees, and needs no input-device permission to do it. */
static void pump_keys(nd_ui *ui, nd_input_channel *ch)
{
    nd_key_event ev;

    if (ui == NULL || ui->input == NULL) {
        nap(0.05);
        return;
    }
    if (!nd_input_read_event(ui->input, 0.05, &ev))
        return;
    if (nd_input_channel_send(ch, ev.code, ev.pressed) != ND_OK) {
        /* The child stopped reading, or has gone. Neither is our problem --
         * waitpid will tell us which in a moment. */
    }
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

nd_err nd_proc_launch_app(nd_ui *ui, const nd_app_entry *app, const char *entry, const char *arg,
                          nd_crash_info *crash_out)
{
    char runner[ND_PATH_MAX];
    const char *path;
    const char *argv[5];
    const char **envp = NULL;
    char keypad_env[64];
    char crash_env[64];
    char fb_env[64];
    char svc_env[64];
    char matrix_env[64];
    char root_env[ND_PATH_MAX + 32];
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
    /* ...and an UNTRUSTED app gets none at all.
     *
     * nd_svc validates the RECORD, never the SENDER -- there is no
     * SO_PEERCRED, no SCM_CREDENTIALS, nothing (spec-app-services.md 5). So
     * the socket is a straight line from whoever holds the descriptor to a
     * root thread that will send an SMS, format the card or power the phone
     * off on their behalf. Handing one to the process whose job is to render
     * whatever the internet sends it would make every one of those verbs
     * reachable from a web page.
     *
     * Withholding it is not a check that can be got round: an fd that was
     * never created cannot be inherited, and netsurf inherits its whole
     * environment from this app -- NEODCT_SERVICE_FD included -- so anything
     * short of "there is no socket" would have leaked it one level down. */
    if (!untrusted && nd_svc_server_open(&svc) == ND_OK)
        svc_fd = nd_svc_server_child_fd(svc);

    path = apprun_path(runner, sizeof runner);
    argv[0] = path;
    argv[1] = app->path;
    argv[2] = entry != NULL ? entry : ND_APP_ENTRY_RUN;
    argv[3] = arg; /* NULL when there is none, which also terminates argv */
    argv[4] = NULL;

    /* ui->has_matrix_keypad and not nd_input_has_matrix(ui->input) directly:
     * the core's flag already has the NEODCT_T9 override folded into it, so a
     * developer who forces T9 on in the core gets it in the app too. */
    if (app_env(keypad_env, sizeof keypad_env, crash_env, sizeof crash_env, fb_env, sizeof fb_env,
                root_env, sizeof root_env, svc_env, sizeof svc_env, matrix_env, sizeof matrix_env,
                ch.read_fd, crash_pipe[1], fb_fd, svc_fd, ui->has_matrix_keypad) != ND_OK) {
        rc = ND_ERR_TOOLONG;
        goto done;
    }
    envp = build_envp(keypad_env, crash_env, fb_env, root_env, svc_env, matrix_env);
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
        /* The browser, confined by the core because only the core still has
         * the privilege to do it. nd_proc.h carries the whole argument. */
        static const char *const hide[] = ND_PROC_UNTRUSTED_HIDE_PATHS;

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
            pump_keys(ui, &ch);
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
    if (svc != NULL)
        nd_svc_server_free(svc); /* only reached when the launch never happened */
    if (crash_pipe[0] >= 0)
        (void)close(crash_pipe[0]);
    if (crash_pipe[1] >= 0)
        (void)close(crash_pipe[1]);
    nd_input_channel_close(&ch);

    /* ALWAYS, whatever happened -- the wallpaper, engineering mode, the app
     * list and the unread-SMS count may all have changed inside the app. */
    nd_ui_refresh_after_app(ui);
    return rc;
}
