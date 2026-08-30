/* nd_priv.h -- becoming somebody else, correctly, between fork and execve.
 *
 * SECURITY-PLAN.md section 1. The phone gets two users -- ndusr for the UI
 * and trusted apps, ndusr_ut for the browser and the media player -- and
 * this is the code that puts a child process into one of them.
 *
 * ============ WHY THE ORDER IS THE WHOLE OF IT ============
 *
 * Dropping privilege is four calls and there is exactly one order that
 * works. Getting it wrong does not fail; it produces a process that looks
 * confined and is not, which is the worst possible outcome:
 *
 *   1. prctl(PR_SET_NO_NEW_PRIVS)  before anything, so that no execve after
 *                                  this point can regain privilege through a
 *                                  setuid binary. It is also the precondition
 *                                  seccomp needs in Phase 3.
 *   2. setgroups()                 the supplementary groups. FIRST of the
 *                                  three id calls, because it needs root --
 *                                  do it after setuid() and it silently
 *                                  fails, leaving the child in root's groups.
 *                                  That is the classic bug and it is why
 *                                  this file exists rather than three inline
 *                                  calls at each site.
 *   3. setgid()                    before setuid, for the same reason.
 *   4. setuid()                    last, and the point of no return.
 *
 * Then every one of them is READ BACK. setuid() returning 0 is not proof:
 * the failure being guarded against is a partial drop, and a child that
 * believes it dropped and did not is a browser running as root.
 *
 * ============ WHY THE LOOKUP IS SEPARATE FROM THE DROP ============
 *
 * getpwnam() and getgrouplist() allocate, open /etc/passwd, and on a glibc
 * build may dlopen an NSS module. None of that is async-signal-safe, and
 * nd_proc.h's rule is absolute: between fork() and execve() only
 * async-signal-safe calls are permitted, because a mutex another thread held
 * at the instant of the fork -- including the ones inside malloc -- stays
 * locked forever in the child.
 *
 * So the name is resolved to numbers in the PARENT, before the fork, and the
 * child does nothing but four syscalls on integers it was handed. That split
 * is the same one nd_proc_spawn already makes for argv, envp and the
 * descriptor plan, and it is what makes any of this safe in a process with a
 * modem thread and a clock thread running.
 */

#ifndef ND_PRIV_H_INCLUDED
#define ND_PRIV_H_INCLUDED

#include <sys/types.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two users, by the names in neodct/configs/users-table.txt. */
#define ND_PRIV_USER       "ndusr"
#define ND_PRIV_USER_UT    "ndusr_ut"

/* Supplementary groups are looked up per user, and there are never many:
 * ndusr has five, ndusr_ut has three. Sixteen is room for the design to grow
 * without a heap allocation in code that has to hand numbers to a child. */
#define ND_PRIV_MAX_GROUPS 16

/* Resolved ids, ready to be used after a fork. Nothing in here is a pointer,
 * on purpose: the child may dereference only what was copied before the
 * fork, and a struct of integers is safe to read from anywhere. */
typedef struct {
    bool valid;   /* false means "no such user"; see nd_priv_lookup */
    uid_t uid;
    gid_t gid;
    gid_t groups[ND_PRIV_MAX_GROUPS];
    size_t n_groups;
} nd_priv_id;

/* Resolve a user name to ids. NOT async-signal-safe -- call it in the
 * parent, before any fork.
 *
 * false, with out->valid false, when the user does not exist. That is the
 * normal case on an image built without BR2_ROOTFS_USERS_TABLES and it is
 * NOT an error: the caller runs the child as itself, exactly as every build
 * before this one did. A phone that refuses to open its browser because a
 * user is missing is a worse phone than one that opens it as root.
 *
 * Deliberately quiet about one thing: if the primary gid is not among the
 * supplementary groups the system reports, it is added, because setgroups()
 * replaces the list wholesale and a child whose own group is missing from it
 * cannot read its own files. */
bool nd_priv_lookup(const char *user, nd_priv_id *out);

/* Become that user. Call ONLY between fork() and execve().
 *
 * Every call it makes is async-signal-safe and every one is read back. 0 on
 * success; on failure it returns a small positive number identifying the
 * step that failed, and THE CALLER MUST _exit() -- there is no recovery from
 * a partial drop, and continuing means running untrusted code with whatever
 * privilege happened to be left.
 *
 * A no-op returning 0 when id->valid is false, which is what makes the
 * "image without the users" path work with no branch at the call site. */
int nd_priv_become(const nd_priv_id *id);

/* prctl(PR_SET_NO_NEW_PRIVS) on its own, for a child that keeps its uid but
 * should still not be able to gain privilege through a setuid binary.
 * Async-signal-safe. 0 on success, -1 if the kernel refuses (it cannot: the
 * call is unconditional since 3.5) or does not have it. */
int nd_priv_no_new_privs(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_PRIV_H_INCLUDED */
