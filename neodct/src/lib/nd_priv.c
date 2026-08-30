/* nd_priv.c -- see nd_priv.h. */

#include "nd_priv.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_log.h"

/* The step that failed, returned by nd_priv_become(). Small positive numbers
 * rather than errno, because the caller is between fork and execve and its
 * only move is _exit(); what it can usefully carry out is WHICH of the four
 * calls went wrong, and an exit status is one byte. */
#define ND_PRIV_STEP_NNP       1
#define ND_PRIV_STEP_SETGROUPS 2
#define ND_PRIV_STEP_SETGID    3
#define ND_PRIV_STEP_SETUID    4
#define ND_PRIV_STEP_READBACK  5

bool nd_priv_lookup(const char *user, nd_priv_id *out)
{
    struct passwd *pw;
    gid_t list[ND_PRIV_MAX_GROUPS];
    int n = (int)ND_PRIV_MAX_GROUPS;
    int i;
    bool have_primary = false;

    if (out == NULL)
        return false;
    (void)memset(out, 0, sizeof *out);
    if (user == NULL || user[0] == '\0')
        return false;

    errno = 0;
    pw = getpwnam(user);
    if (pw == NULL) {
        /* Not an error, and deliberately not logged as one: an image built
         * without the users table has no ndusr, and the caller's answer to
         * that is "run it as myself", which is what every build before this
         * one did. */
        return false;
    }

    out->uid = pw->pw_uid;
    out->gid = pw->pw_gid;

    /* Pre-filled with the primary gid rather than left uninitialised, and
     * that is not belt and braces.
     *
     * getgrouplist() returns -1 for TWO different reasons and musl's
     * (src/passwd/getgrouplist.c) does not let the caller tell them apart
     * from the return value alone:
     *
     *   truncation  the array is filled with the first *ngroups groups and
     *               *ngroups is set to how many there really are;
     *   an error    -- a failed allocation, an unreadable /etc/group -- takes
     *               a goto straight to cleanup, BEFORE `*ngroups = n`. The
     *               array is then PARTIALLY filled and *ngroups is whatever
     *               the caller passed in.
     *
     * Reading the whole array back on that second path means handing
     * setgroups() gids off the stack. Filling it with the primary gid first
     * makes every unwritten slot a harmless duplicate of a group the process
     * is entitled to, whichever path was taken. Zeroing it would be worse
     * than useless: gid 0 is root's group. */
    for (i = 0; i < (int)ND_PRIV_MAX_GROUPS; i++)
        list[i] = pw->pw_gid;

    if (getgrouplist(user, pw->pw_gid, list, &n) < 0) {
        if (n > (int)ND_PRIV_MAX_GROUPS) {
            /* Truncation: what fitted is real, so take it and say so. */
            nd_log_err(ND_LOG_OS, "%s is in %d groups; taking the first %d", user, n,
                       (int)ND_PRIV_MAX_GROUPS);
        } else {
            /* Anything else. The contents are not to be trusted, so fall
             * back to the primary group alone -- which the block below adds
             * back. A process in too few groups fails visibly; one in groups
             * nobody granted it fails invisibly. */
            nd_log_err(ND_LOG_OS, "cannot read the groups of %s; using its own only", user);
        }
        n = (int)ND_PRIV_MAX_GROUPS;
    }
    if (n < 0)
        n = 0;
    if (n > (int)ND_PRIV_MAX_GROUPS)
        n = (int)ND_PRIV_MAX_GROUPS;

    for (i = 0; i < n; i++) {
        out->groups[i] = list[i];
        if (list[i] == pw->pw_gid)
            have_primary = true;
    }
    out->n_groups = (size_t)n;

    /* setgroups() replaces the supplementary list wholesale. A child whose
     * own primary group is missing from it cannot read its own files, and
     * getgrouplist is only documented to include the gid it was given -- so
     * make sure rather than assume. */
    if (!have_primary && out->n_groups < ND_PRIV_MAX_GROUPS) {
        out->groups[out->n_groups] = pw->pw_gid;
        out->n_groups++;
    }

    out->valid = true;
    return true;
}

int nd_priv_no_new_privs(void)
{
    /* Unconditional since Linux 3.5, so a failure here means a kernel older
     * than anything this phone has ever run. Reported rather than ignored:
     * it is the precondition for the seccomp filter in Phase 3, and a
     * silently missing no_new_privs would make that filter refuse to load
     * for a reason nobody would look for. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    return 0;
}

int nd_priv_become(const nd_priv_id *id)
{
    /* "No user in this image" is a no-op, not a failure. See nd_priv.h. */
    if (id == NULL || !id->valid)
        return 0;

    /* ==== ASYNC-SIGNAL-SAFE ONLY FROM HERE ====
     *
     * prctl, setgroups, setgid, setuid, getuid, getgid and getgroups are all
     * plain syscalls. Nothing below allocates, opens a file or formats a
     * string, because this runs between fork() and execve() in a process
     * that has a modem thread and a clock thread. */

    if (nd_priv_no_new_privs() != 0)
        return ND_PRIV_STEP_NNP;

    /* setgroups FIRST. It needs privilege, so after setuid() it fails --
     * quietly, because the caller that ignores the return then leaves the
     * child in ROOT'S supplementary groups while believing it dropped. That
     * is the classic form of this bug. */
    if (setgroups(id->n_groups, id->groups) != 0)
        return ND_PRIV_STEP_SETGROUPS;

    /* setgid before setuid, for exactly the same reason. */
    if (setgid(id->gid) != 0)
        return ND_PRIV_STEP_SETGID;

    if (setuid(id->uid) != 0)
        return ND_PRIV_STEP_SETUID;

    /* Read it back. A return of 0 is not proof: what is being guarded
     * against is a partial drop, and there is no cost to asking the kernel
     * what actually happened. On a 32-bit kernel with 16-bit id syscalls the
     * setuid() a libc chooses can succeed and truncate; this catches that
     * too. */
    if (getuid() != id->uid || geteuid() != id->uid || getgid() != id->gid ||
        getegid() != id->gid)
        return ND_PRIV_STEP_READBACK;

    /* And that the privilege is really gone: if uid 0 can be reclaimed, the
     * drop bought nothing. setuid() as a non-root user with no saved set-uid
     * must fail. */
    if (id->uid != 0 && setuid(0) == 0)
        return ND_PRIV_STEP_READBACK;

    return 0;
}
