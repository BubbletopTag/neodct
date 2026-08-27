/* nd_proc.h -- starting other programs, and finding out what happened to them.
 *
 * (PORT-PLAN.md section 2.4 calls this header nd_child.h. nd_child.h exists
 * and includes this one, so either spelling compiles.)
 *
 * ============ THE RULE ============
 *
 *     fork() IS ALWAYS IMMEDIATELY FOLLOWED BY execve().
 *
 * Not "usually". The core runs threads -- the modem and the clock -- and
 * forking a threaded process gives the child only the calling thread. Any
 * mutex another thread held at the instant of the fork stays locked forever in
 * the child, INCLUDING THE ONES INSIDE malloc(). The child then hangs on its
 * first allocation, which will be somewhere that looks nothing like the fork.
 *
 * Between fork() and execve() only async-signal-safe calls are permitted. No
 * malloc, no printf, no fopen. _exit(), never exit() -- exit() runs atexit
 * handlers and flushes the parent's buffers a second time, in the child.
 *
 * Every helper here obeys that by construction: argv, envp and the descriptor
 * plan are all built BEFORE the fork, so the child does nothing but dup2,
 * close and exec.
 *
 * ============ OWNER TAGS ============
 *
 * Four kinds of child exist and they are reaped differently, so each one
 * carries a tag:
 *
 *   ND_OWNER_APP    the one app child. The core waits for it.
 *   ND_OWNER_AUDIO  the modem's PCM bridge. Long-lived, restarted on failure.
 *   ND_OWNER_TONE   a one-shot aplay. Fire and forget; the reaper collects it.
 *   ND_OWNER_SYSTEM poweroff, ssh-keygen, sshd, the relay tunnel.
 *
 * A single SIGCHLD reaper collects them all, so nothing becomes a zombie and
 * no code path has to remember to waitpid().
 */

#ifndef ND_PROC_H_INCLUDED
#define ND_PROC_H_INCLUDED

#include <sys/types.h>

#include "nd_crash.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ND_OWNER_APP = 0, ND_OWNER_AUDIO, ND_OWNER_TONE, ND_OWNER_SYSTEM } nd_proc_owner;

/* Descriptors to hand the child. Anything not listed is closed on exec. */
#define ND_PROC_MAX_FDS 8

typedef struct {
    int child_fd; /* the number the child will see */
    int our_fd;   /* the descriptor to dup2 onto it */
} nd_fd_map;

typedef struct {
    const char *const *argv; /* argv[0] included; NULL-terminated */
    const char *const *envp; /* NULL means "inherit environ"      */
    nd_fd_map fds[ND_PROC_MAX_FDS];
    size_t n_fds;
    nd_proc_owner owner;

    /* setsid() in the child, between the fork and the execve. Python spells
     * this start_new_session=True, and for anything the core may later
     * signal by PROCESS GROUP it is not optional.
     *
     * The failure it prevents is on the record, in RemoteShell's own
     * _owns() docstring: the phone left sshd.pid=244 and tunnel.pid=246
     * behind, came back up, and Remote Shell killed the process group those
     * numbers had been handed to by then -- its own launcher. The UI never
     * started and the serial log stopped mid-boot.
     *
     * With a new session the child's group contains the child and its own
     * descendants and nothing else, so killing that group cannot reach back
     * into the core. It is off by default because an app SHOULD stay in the
     * core's session: nd_proc_terminate() signals a single pid, and an app
     * that outlives its session leader is exactly the orphan the crash
     * screen exists to catch. */
    bool new_session;

    /* Close every descriptor above stderr in the child, after the fds[] above
     * are in place and before the execve. Off by default because an app is
     * MEANT to inherit what the core hands it -- the crash pipe and the
     * service socket are passed exactly that way. Turn it on for anything
     * long-lived that the core does not own, or it will hold the core's pipes
     * open for as long as it runs. */
    bool close_others;
} nd_proc_spec;

/* fork + execve, in that order and nothing in between. *pid_out receives the
 * child's pid. ND_ERR_IO when the fork itself fails; an execve failure is
 * reported by the child exiting 127, which the caller sees at waitpid.
 *
 * The path is NOT ND_ROOT-resolved: it is an executable, and the test harness
 * runs the real binaries. */
nd_err nd_proc_spawn(const char *path, const nd_proc_spec *spec, pid_t *pid_out);

/* Install the SIGCHLD reaper. Idempotent. Call once, from the core, before
 * anything is spawned. */
nd_err nd_proc_reaper_start(void);
void nd_proc_reaper_stop(void);

/* What waitpid told us. */
typedef struct {
    pid_t pid;
    bool exited;     /* WIFEXITED   */
    int exit_status; /* WEXITSTATUS */
    bool signalled;  /* WIFSIGNALED */
    int signo;       /* WTERMSIG    */
} nd_proc_status;

/* Wait for one child. timeout_s < 0 blocks. ND_ERR_TIMEOUT when it is still
 * running. Safe to call while the reaper is installed -- the reaper skips
 * pids that are being waited on explicitly. */
nd_err nd_proc_wait(pid_t pid, double timeout_s, nd_proc_status *out);

/* SIGTERM, wait up to grace_s, then SIGKILL and wait again. This is the
 * sequence the incoming-call path uses; see the teardown contract in
 * nd_app.h. */
nd_err nd_proc_terminate(pid_t pid, double grace_s, nd_proc_status *out);

/* ------------------------------------------------------------------ *
 * Launching an app -- the whole sequence, in one call
 * ------------------------------------------------------------------ */

/* 1. open the key channel and the crash pipe
 * 2. spawn nd-apprun with the app's directory, the entry point and its
 *    argument, and the three inherited descriptors in the environment
 * 3. pump keys to the child while it runs, so the core stays awake and an
 *    incoming call can interrupt it
 * 4. waitpid, classify:
 *       WIFEXITED   && status == 0  -> a clean return; redraw the menu
 *       WIFEXITED   && status != 0  -> a crash
 *       WIFSIGNALED                 -> a crash, and we know which signal
 * 5. on a crash, read the child's report from the crash pipe, log it, and
 *    show the crash screen
 * 6. ALWAYS call nd_ui_refresh_after_app()
 *
 * Returns ND_OK whether or not the app crashed -- a crashed app is a handled
 * condition, not a failure of the launcher. crash_out, when non-NULL,
 * receives the report and .from_signal tells you whether there was one. */
nd_err nd_proc_launch_app(nd_ui *ui, const nd_app_entry *app, const char *entry, const char *arg,
                          nd_crash_info *crash_out);

#ifdef __cplusplus
}
#endif

#endif /* ND_PROC_H_INCLUDED */
