/* nd_broker.h -- the one part of the core that is still root.
 *
 * ============ WHY THIS EXISTS ============
 *
 * nd-core was root because it is the process that forks apps and drops them to
 * ndusr, and NOTHING ELSE. That was measured rather than assumed: nd-core was
 * made to become ndusr at startup (NEODCT_DROP_PRIV, core/nd_main.c) and run
 * on a booted phone. The panel, the keypad, the modem, the battery, the
 * settings, the wallpaper and the fonts all came up exactly as before --
 * everything the UI touches is group-reachable to ndusr already, by the
 * layout in 61-neodct-devices.rules. The single thing that broke was launching
 * an app, with exit 122: ND_PRIV_STEP_SETGROUPS, because setgroups() needs
 * CAP_SETGID and an ordinary ndusr process has none.
 *
 * So the privilege that kept the entire UI at uid 0 was one syscall in the
 * child, between fork and execve. This moves that syscall -- and only that
 * syscall -- into a separate root process, so the UI can stop being root.
 *
 * ============ THE SHAPE ============
 *
 *   nd-core forks the broker BEFORE it drops, keeping one end of a
 *   SOCK_SEQPACKET socketpair. The broker stays root and never draws, never
 *   reads a key and never opens a device. nd-core becomes ndusr and carries on
 *   being the whole phone.
 *
 *   When an app is launched, nd-core still creates the pipes, the input
 *   channel and the service socket -- it needs the parent ends anyway -- and
 *   sends the CHILD ends to the broker with SCM_RIGHTS. The broker forks,
 *   drops to the user nd-core named, and execve's. It reaps, and reports the
 *   exit status back.
 *
 * The broker is forked while nd-core is still SINGLE-THREADED, before the
 * clock and remote-shell services start. Forking a process with threads leaves
 * the child holding locks no thread will ever release -- malloc's among them
 * -- and the broker allocates.
 *
 * ============ WHAT IT DELIBERATELY DOES NOT DO ============
 *
 * It does not decide anything. nd-core says which app, as which user, with
 * which descriptors; the broker performs it. That is not a weakness to be
 * fixed later: the broker's whole value is that it is small enough to read,
 * and a policy engine at uid 0 is the thing this was built to avoid.
 *
 * The check it DOES make is on the user name, against a fixed list. nd-core is
 * unprivileged and may be compromised, and "spawn this as root" must not be a
 * request it can make. See ND_BROKER_USERS.
 */

#ifndef ND_BROKER_H_INCLUDED
#define ND_BROKER_H_INCLUDED

#include <sys/types.h>

#include "nd_app.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The only users the broker will drop a child to. "root" is not here and must
 * never be: nd-core is unprivileged, so every request on this socket is
 * untrusted input, and a broker that honours "spawn as root" gives the whole
 * privilege boundary back to whoever compromises the UI. */
#define ND_BROKER_USERS                     \
    {                                       \
        ND_PRIV_USER, ND_PRIV_USER_UT, NULL \
    }

/* ============ AND THE HOLE THAT LIST DID NOT CLOSE ============
 *
 * Refusing the NAME "root" is worth nothing on its own, because not naming a
 * user at all means "do not drop" -- and a child the broker never drops stays
 * root. So the first version of this file shipped with
 *
 *     nd_broker_spawn(b, "/bin/sh", &spec, NULL, &pid)
 *
 * as a working root shell, reachable by anything that could talk to the socket,
 * which is the whole point of the socket. The test asserted that "root" was
 * refused and passed, because it never tried NULL. A boundary that only stops
 * the spelling you thought of is not a boundary.
 *
 * So a no-drop spawn is now allowed only for these exact executables. Both live
 * on the read-only squashfs under dm-verity, so "replace the binary and ask for
 * it by name" is not available either -- the list would be worth much less if
 * it named anything on /NeoDCT/User.
 *
 * BOTH carry an EXTRA condition the list cannot express, and the second one
 * was missing for as long as the first one existed:
 *
 *   nd-apprun    argv[1] is the app directory and must be under
 *                ND_PATH_ENG_APPS_DIR. Without it, "run nd-apprun as root" is
 *                "run any app as root" -- the same hole with one more step.
 *
 *   neodct-sdcard  the core asks two things of it through here: `format
 *                <device>` (nd_svc_format_card) and the argument-less
 *                `layout` (nd_svc_layout_card). It was given no argv
 *                condition at all, so "format" could just as well have been
 *                `add` with an attacker-named device and an attacker-named
 *                mountpoint -- and the helper's own "never touch the disk the
 *                phone runs from" guard reads NEODCT_CMDLINE, NEODCT_MOUNTS
 *                and NEODCT_BOOT_STATE, every one of which the same request
 *                supplied. Pinning the verb and the device shape is what makes
 *                that guard the helper's business again rather than the
 *                caller's.
 *
 * See root_exec_allowed(). */
#define ND_BROKER_ROOT_EXEC                            \
    {                                                  \
        ND_PATH_SDCARD_HELPER, ND_PATH_ND_APPRUN, NULL \
    }

/* ============ AND THE HOLE *THAT* LIST DID NOT CLOSE EITHER ============
 *
 * Pinning the path is worth nothing while the ENVIRONMENT still comes off the
 * wire, because for both of these executables the environment is the program:
 *
 *   - neodct-sdcard is `#!/bin/sh` and calls blkid, mkfs.vfat, mount and a
 *     dozen others by bare name, setting no PATH of its own. One
 *     "PATH=/somewhere/I/can/write" in the request and the first bare name is
 *     the attacker's binary, running as root.
 *   - nd-apprun resolves its app directory through nd_path_resolve(), which
 *     prepends $NEODCT_ROOT. So argv[1] can satisfy the engineering-directory
 *     test above while the .so that is actually dlopen'd comes from wherever
 *     NEODCT_ROOT points.
 *   - and neither binary is setuid, so the process is not AT_SECURE and the
 *     dynamic loader honours LD_PRELOAD from anyone who can set it.
 *
 * Three ways to the same place, all of them past a check that was looking at
 * the path. So a no-drop spawn does not get the caller's environment at all:
 * the broker builds one, from ND_BROKER_ROOT_ENV_KEEP plus a fixed PATH.
 *
 * The keep list is what a launch genuinely cannot do without -- descriptor
 * numbers the broker itself just remapped, and one keypad flag. Every one is
 * a small integer the child re-reads and re-validates. NEODCT_ROOT is
 * deliberately NOT here even though ordinary app launches pass it: it is
 * empty on a phone (nd_proc.c's app_env says so) and it is the redirect in
 * the second vector above, so a root spawn is exactly the case where it must
 * not be honoured.
 *
 * A DROPPED spawn keeps the caller's environment untouched. It is not a
 * privilege boundary there -- the child is about to become ndusr or ndusr_ut,
 * and a compromised core can hand an ndusr child anything it likes by simply
 * being ndusr itself. */
#define ND_BROKER_ROOT_ENV_KEEP                                            \
    {                                                                      \
        ND_ENV_KEYPAD_FD, ND_ENV_CRASH_FD, ND_ENV_FB_FD, ND_ENV_SERVICE_FD, \
            ND_ENV_KEYPAD_MATRIX, NULL                                     \
    }

/* Not inherited, not from the request: a literal, matching the one busybox
 * init sets. The sdcard helper needs *a* PATH -- it is a shell script full of
 * bare names -- and this is the only way it gets one that the caller did not
 * choose. */
#define ND_BROKER_ROOT_PATH "PATH=/usr/sbin:/usr/bin:/sbin:/bin"

/* The largest descriptor number a spawn may ask the child to be given.
 *
 * A launch uses four (the key channel, the crash pipe, the framebuffer and
 * the service socket) and the sdcard helper three (stdin, stdout, stderr), so
 * anything past a couple of dozen is a request nobody is making. The bound
 * exists because the number arrives off the socket: the relocation loop takes
 * `highest + 1`, which is undefined at INT32_MAX, and dup2() onto a negative
 * one is a child that dies between fork and exec for a reason nothing logs. */
#define ND_BROKER_CHILD_FD_MAX 63

/* argv, envp and the hide list, flattened into one NUL-separated run. 8 KiB is
 * far above what a launch needs (five argv entries, six env strings and at most
 * ND_PROC_MAX_HIDE paths) and far below SOCK_SEQPACKET's default limit. */
#define ND_BROKER_BLOB_MAX 8192

typedef struct nd_broker nd_broker;

/* Fork the broker. Call this BEFORE dropping privilege and BEFORE starting any
 * thread. Returns NULL when the fork or the socketpair failed, which the
 * caller must treat as "stay root": a phone that cannot launch apps is worse
 * than one that launches them from a privileged UI.
 *
 * In the child this never returns; the broker runs until its end of the socket
 * reports EOF, which is nd-core exiting. */
nd_broker *nd_broker_start(void);

/* True when the broker is alive and nd-core may therefore drop. */
bool nd_broker_ok(const nd_broker *b);

/* nd_proc_spawn(), performed on the other side of the socket.
 *
 * spec->run_as is NOT sent -- a uid on the wire is a uid the sender chose.
 * `user` names one of ND_BROKER_USERS and the broker looks it up itself; NULL
 * means "do not drop", which is what an engineering app under
 * /NeoDCT/System/engineering gets. */
nd_err nd_broker_spawn(nd_broker *b, const char *path, const nd_proc_spec *spec, const char *user,
                       pid_t *pid_out);

/* nd_proc_wait(), likewise: the app is the BROKER's child, so only the broker
 * can reap it. Same contract as nd_proc_wait -- ND_ERR_TIMEOUT while it is
 * still running, which is what the launch loop polls on.
 *
 * ============ THE WAITING HAPPENS ON THIS SIDE, ALWAYS ============
 *
 * `timeout_s` is honoured HERE, by a loop of short non-blocking round trips.
 * It is never sent to the broker as a duration, and the broker has no verb
 * that blocks.
 *
 * That is not an implementation detail, it is the fix for a phone that froze.
 * The core asks the helper that formats the SD card for up to
 * ND_SVC_FORMAT_WAIT_S (240 s), and both halves of the old shape made that a
 * stop-the-world:
 *
 *   - round_trip() holds b->lock across its send AND its recv, so a REQ_WAIT
 *     that blocked for four minutes held the lock for four minutes. The core's
 *     UI thread takes that same lock on every turn of nd_proc_launch_app()'s
 *     pump loop -- and that loop is what SCANS THE I2C KEY MATRIX. The matrix
 *     has no kernel queue, so every key pressed during a format was not
 *     delayed, it was lost, and nothing repainted either.
 *   - broker_loop() serves one request at a time. A verb that blocks inside
 *     the server also stalls REQ_SPAWN, REQ_HALT and REQ_CLOCK for every other
 *     thread in the core, whatever the lock does.
 *
 * Neither was visible under QEMU, where mkfs on a host-backed image file
 * finishes in well under a second and the evdev input path buffers keys across
 * a stall anyway. A poll loop on this side costs one tiny round trip every
 * ND_BROKER_WAIT_POLL_S and cannot stall anybody. */
nd_err nd_broker_wait(nd_broker *b, pid_t pid, double timeout_s, nd_proc_status *out);

/* How often nd_broker_wait() probes while it waits. 50 ms is far below what a
 * person notices at the end of a format and far above the cost of a socketpair
 * round trip on this SoC; a wait of 0.0 naps not at all, which is the case the
 * app-launch pump loop takes on every turn. */
#define ND_BROKER_WAIT_POLL_S 0.05

/* ============ SIGNALLING A CHILD THE BROKER STARTED ============
 *
 * The core cannot signal its own helpers any more, and found out the
 * expensive way. `neodct-sdcard format` is spawned UNDROPPED (root) by the
 * broker, so it is the broker's child and root's process. When the core's
 * 240-second escape hatch fired, it called kill(2) from uid 1000 against uid
 * 0 and got EPERM -- and then waitpid()ed a process it had never forked, got
 * ECHILD, and nd_proc.c's collect() reported that as a clean exit. So the
 * hatch reported success, nothing was stopped, nothing was reaped, and a root
 * mke2fs carried on writing the card underneath "Formatting failed."
 *
 * This verb is what the hatch needs, and it is pinned two ways so the broker
 * still decides nothing:
 *
 *   THE SIGNAL is exactly SIGTERM or SIGKILL. Nothing else is a request the
 *   core makes, and a signal number off the wire is otherwise a way to send
 *   SIGSTOP to init.
 *
 *   THE PID must be one the broker itself forked and has not yet reaped. The
 *   broker already sees every spawn, so this is a fact it owns rather than a
 *   policy it applies -- and it is strictly narrower than what the core could
 *   do before it dropped, which was to signal anything it owned.
 *
 * False means the signal was not delivered: a pid the broker did not start, a
 * signal it will not send, or a channel that has gone. */
bool nd_broker_kill(nd_broker *b, pid_t pid, int signo);

/* How many live children the broker remembers. One app, plus the sdcard
 * helper, plus room to spare: the design has never had more than two at once,
 * and a table that overflows forgets its OLDEST entry, which makes that child
 * unkillable rather than making somebody else's process killable. */
#define ND_BROKER_CHILDREN_MAX 16

/* reboot(2) and clock_settime(2), which an unprivileged core also lost.
 *
 * These are here for the same reason the spawn is: dropping nd-core did not
 * only stop it forking apps, it stopped it rebooting the phone and stopped NTP
 * setting the clock. Both were found by reading the code after the drop
 * worked, not by the phone -- which is the wrong order, and the reason they
 * are in the same commit as the drop rather than a later one.
 *
 * The POLICY stays on the core's side. clock_set_bounded() decides whether a
 * time is plausible before it ever gets here; the broker performs a syscall
 * and does not have an opinion about the value. */
bool nd_broker_halt(nd_broker *b, bool reboot);
bool nd_broker_set_clock(nd_broker *b, int64_t when);

/* The one nd-core forked, for the library code that cannot be handed it.
 * nd_proc, nd_svc and nd_clock all need the broker and none of them has a
 * plausible route to a pointer nd-core owns. NULL until nd-core sets it, which
 * is every unit test and any build whose core is still root -- and every one of
 * those callers keeps its direct path for exactly that case. */
void nd_broker_set_default(nd_broker *b);
nd_broker *nd_broker_default(void);

/* Close the socket and reap the broker. */
void nd_broker_stop(nd_broker *b);

/* ---- the two policy decisions, exposed so a test can ask them directly ----
 *
 * Both are pure functions of the request, and both are the whole of what
 * stands between a compromised core and a root child. A test that has to
 * spawn a real process to reach them can only check the cases the host
 * happens to support -- which is how the first version of this file shipped
 * with a passing test and a working root shell.
 *
 * nd_broker__root_exec_allowed: may this path, with this argv, run undropped?
 * nd_broker__root_env_filter:   the environment such a child actually gets.
 *   `out` holds out_max entries and is NUL-terminated; out_max >= 2. */
bool nd_broker__root_exec_allowed(const char *path, const char *const *argv, uint32_t n_argv);
/* nd_broker__spawn_stays_root: would a child spawned for this user still be
 *   root? All three spellings of "do not drop", enumerable by a test. */
bool nd_broker__spawn_stays_root(const char *user, bool resolved);
void nd_broker__root_env_filter(const char *const *in, uint32_t n_in, const char **out,
                                size_t out_max);
/* nd_broker__kill_signo_allowed: half of the KILL verb's pinning, and the half
 *   that is a pure function of the request. The other half -- "is this pid one
 *   of mine" -- is a fact about the broker's own bookkeeping and is tested
 *   through a real spawn. */
bool nd_broker__kill_signo_allowed(int32_t signo);

#ifdef __cplusplus
}
#endif

#endif /* ND_BROKER_H_INCLUDED */
