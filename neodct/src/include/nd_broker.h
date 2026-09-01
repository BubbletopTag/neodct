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
 * still running, which is what the launch loop polls on. */
nd_err nd_broker_wait(nd_broker *b, pid_t pid, double timeout_s, nd_proc_status *out);

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

#ifdef __cplusplus
}
#endif

#endif /* ND_BROKER_H_INCLUDED */
