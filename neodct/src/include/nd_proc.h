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
#include "nd_priv.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ND_OWNER_APP = 0, ND_OWNER_AUDIO, ND_OWNER_TONE, ND_OWNER_SYSTEM } nd_proc_owner;

/* Descriptors to hand the child. Anything not listed is closed on exec. */
#define ND_PROC_MAX_FDS 8

/* Paths a child may be given an empty view of. See `hide_paths`. */
#define ND_PROC_MAX_HIDE 16

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

    /* Run the child as somebody else. SECURITY-PLAN.md section 1.
     *
     * Resolved with nd_priv_lookup() in the parent -- getpwnam allocates and
     * reads a file, and neither is allowed after the fork -- then applied by
     * the child with nd_priv_become(), which is four syscalls on integers
     * that were copied before the fork. A `valid` of false means "no such
     * user in this image", which is a no-op rather than a failure, so a
     * caller needs no branch for a build without the users table.
     *
     * This is the ONE call site the plan asks for, and the reason section 3
     * insists on the order it does: an app denied libneodct writes ten lines
     * of termios and talks to /dev/ttyUSB2 itself, so a library is not a
     * boundary. A uid is, because it is the kernel's answer rather than
     * ours. Zeroed by memset like every other field, so a spec that does not
     * mention it runs the child as the caller, exactly as before. */
    nd_priv_id run_as;

    /* prctl(PR_SET_NO_NEW_PRIVS) in the child even when it keeps its uid.
     * Independent of run_as: a child that stays root gains nothing from it,
     * but a child that is going to load untrusted code should not be able to
     * regain privilege through a setuid binary either way, and it is the
     * precondition for the seccomp filter in Phase 3. */
    bool no_new_privs;

    /* ---- the mount namespace. SECURITY-PLAN.md section 2. --------------
     *
     * unshare(CLONE_NEWNS) in the child, then MS_REC|MS_PRIVATE on / so
     * nothing done afterwards propagates back out. This is the 5.10
     * stand-in for Landlock, which does not exist before 5.13, and it is
     * strictly more thorough than a permission would be: it removes paths
     * from EXISTENCE rather than denying access to them, so there is no
     * `..`, no symlink and no /proc/self/root trick to walk back out.
     *
     * It matters for the world-readable half of the image, which is
     * precisely what DAC cannot help with. /NeoDCT/System has to be readable
     * by every app, so `file:///NeoDCT/System/...` in the browser enumerates
     * the whole system tree no matter who the browser runs as --
     * SECURITY-AUDIT.md finding 5.
     *
     * hide_paths is over-mounted with an empty, read-only, mode-0000 tmpfs:
     * the directory still exists and is empty and unreadable, which is a
     * quieter failure for the program inside than a missing path and just as
     * final. Resolve the list before the fork -- a path that does not exist
     * is dropped there, so that a failure in the child is always a real one.
     *
     * FAIL-OPEN ON unshare, deliberately. CONFIG_MNT_NS is a vendor BSP
     * question the plan's section 6 lists as unverified on the phone, and a
     * kernel without it must still open the browser -- the uid boundary is
     * what carries the confinement, and this is defence on top of it. A hide
     * that fails AFTER the namespace exists is a different matter and kills
     * the child, because that one is a bug rather than a missing feature. */
    bool private_mounts;
    const char *const *hide_paths; /* NULL-terminated, or NULL */
} nd_proc_spec;

/* Whether this kernel can give a child a mount namespace at all.
 *
 * Answered by trying it in a throwaway child, once, and remembering. Call it
 * from the parent and log the answer: a phone whose kernel was built without
 * CONFIG_MNT_NS otherwise loses section 2's confinement silently, which is
 * the failure mode the whole plan is written against. */
bool nd_proc_namespaces_available(void);

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

/* ------------------------------------------------------------------ *
 * Which user an app runs as
 * ------------------------------------------------------------------ */

/* True when this app must keep the core's privileges rather than dropping to
 * ndusr. Pure, and separated from the launcher so that the POLICY can be
 * tested without forking anything -- the launcher's own behaviour cannot be,
 * because a test cannot become another user.
 *
 * ============ THE ONE REASON AN APP KEEPS ROOT ============
 *
 * IT IS AN ENGINEERING APP AND ENGINEERING MODE IS ON.
 *
 *    RemoteShell exists to give a developer a root shell; LinuxShell, Modem,
 *    KeypadMapperI2C and FuelGauge exist to poke at hardware. Confining them
 *    would not make the phone safer, it would make them useless, and a
 *    diagnostic that cannot diagnose is worse than none.
 *
 *    "Is it an engineering app" is decided by WHERE IT LIVES:
 *    /NeoDCT/System/engineering/apps/ rather than /NeoDCT/System/apps/. Both
 *    are on the read-only, dm-verity'd rootfs, so an app cannot move itself
 *    into the privileged directory, and nothing in its own manifest -- which
 *    is also read-only, but is at least app-authored in spirit -- is
 *    consulted. The path is not ND_ROOT-resolved either: nd_ui_scan_apps()
 *    stores the virtual prefix it was given, so this comparison means the
 *    same thing under a test root as on a phone.
 *
 * ============ THERE USED TO BE A SECOND, AND THERE IS NOT ANY MORE ============
 *
 * Five STOCK apps kept root because each had one privileged operation and
 * nowhere to send it. The list was written out longhand so that it was
 * uncomfortable to look at, and it is empty now:
 *
 *      Power     poweroff, reboot        -> nd_svc_reboot/poweroff
 *      Update    reboot, to finish       -> nd_svc_reboot
 *      Downgrade reboot                  -> nd_svc_reboot, via Update's dlopen
 *      Clock     settimeofday by hand    -> nd_svc_set_clock
 *      Settings  neodct-sdcard format    -> nd_svc_format_card
 *
 * None of those five operations belonged to the app. Each is a thing the CORE
 * does on the app's behalf, over the nd_svc socket that already existed for
 * sending an SMS -- and the core stays root, so it can. That is what "the
 * abstraction is the only path" means in SECURITY-PLAN.md section 3, and it
 * is now true of every stock app on the phone.
 *
 * ROOT_STOCK_APPS still exists, empty, in nd_proc.c. Read the comment above
 * it before adding anything to it.
 *
 * ============ WHAT THIS IS NOT ============
 *
 * `engineering_mode` comes from system.ui.engineering_mode in settings.prop,
 * on the WRITABLE partition. AGENTS.md is blunt that this is not a security
 * gate -- "it lives in settings.prop, on the partition the attacker just
 * wrote to" -- and it is right. Anything running as ndusr can set that key
 * and get root back on the next engineering app it opens.
 *
 * That is a known and accepted hole while the architecture is being settled,
 * and the shape of the fix is already decided and already proven elsewhere in
 * this tree: env.sh had exactly this problem and is now gated on
 * neodct.devenv=1 on the kernel command line, which the writable partition
 * cannot set. The same second gate belongs here -- both conditions true, the
 * settings key for the menu and the cmdline flag for the privilege -- and
 * when the initramfs recovery lands it is the natural place to set it.
 *
 * Written so that adding it is one && in the line below and nothing else. */
bool nd_proc_app_needs_root(const nd_app_entry *app, bool engineering_mode);

/* ------------------------------------------------------------------ *
 * The other direction: apps that run as LESS than ndusr
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THIS HAD TO MOVE INTO THE CORE ============
 *
 * The Browser app is a launcher: its whole job is to start netsurf and pump
 * its stderr. netsurf is the untrusted half of this phone -- it renders
 * whatever the internet sends it -- so apps/Browser used to do the confining
 * itself, calling nd_priv_lookup(ND_PRIV_USER_UT) and asking nd_proc_spawn()
 * for a mount namespace.
 *
 * THAT STOPPED WORKING THE DAY APPS STOPPED BEING ROOT, and it stopped
 * working silently, in the worst possible direction:
 *
 *   setgroups() needs CAP_SETGID. As ndusr it fails, so nd_priv_become()
 *   returns a step code and the child _exit(122)s BEFORE execve -- netsurf
 *   never starts. The Browser app draws nothing and returns to the home
 *   screen, so the phone looks like the browser simply did not open.
 *
 *   unshare(CLONE_NEWNS) needs CAP_SYS_ADMIN. As ndusr it fails too, and
 *   because the code treats that as "this kernel has no namespaces" the
 *   phone printed "no mount namespaces in this kernel" on a kernel that has
 *   them -- nd-selftest reports CONFIG_MNT_NS PASS from the root core on the
 *   very same boot.
 *
 * Both were verified on a booted phone, not reasoned about.
 *
 * The privilege being asked for is "become a LESS privileged user, and drop
 * a namespace" -- which Linux still gates behind CAP_SETGID/CAP_SETUID and
 * CAP_SYS_ADMIN. Only one process on this phone has those and should keep
 * them: the core. So the confinement moves here, where it works, and the app
 * is left with nothing to get wrong.
 *
 * ============ WHAT THE APP GETS INSTEAD ============
 *
 * An app named here is launched as ndusr_ut with private_mounts and the hide
 * list below, AND WITHOUT A SERVICE SOCKET -- see nd_proc_launch_app(). That
 * last part is not tidiness. nd_svc has no peer credential check of any kind
 * (spec-app-services.md 5), so the socket is a straight line from any process
 * holding it to a root thread that will send an SMS on its behalf. An
 * untrusted app must not hold one, and neither must anything it forks.
 *
 * WHOLE PATHS, for the reason ROOT_STOCK_APPS' comment gives at length: a
 * name is not safe to match on. The direction of the grant is different here
 * -- this one only ever takes privilege away -- but a future user-installed
 * directory called Browser must not be able to claim the browser's profile
 * directory or the untrusted half of the SD card, both of which ndusr_ut owns
 * and ndusr does not. */
bool nd_proc_app_is_untrusted(const nd_app_entry *app);

/* Emptied over the untrusted app's view of the filesystem: each becomes a
 * read-only, mode-0000 tmpfs, so the directory still exists and is simply
 * unreadable. Moved here from apps/Browser/browser.h, which is where it lived
 * when the app did its own confining.
 *
 * These are the things ndusr_ut has no business reading even though the DAC
 * bits alone might let it: the engineering apps, the release signing keys,
 * the owner's tones and wallpapers, the databases, the SSH keys, the update
 * records and the RNG seed. */
#define ND_PROC_UNTRUSTED_HIDE_PATHS                                                 \
    {                                                                                \
        "/NeoDCT/System/engineering", "/NeoDCT/System/keys", "/NeoDCT/System/tones", \
            "/NeoDCT/System/wallpapers", "/NeoDCT/User/db", "/NeoDCT/User/.remote",  \
            "/NeoDCT/User/.ndsys", "/NeoDCT/User/.seedrng", NULL                     \
    }

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
