/* nd_selftest.c -- does the confinement actually hold on THIS phone?
 *
 *     nd-selftest [-v] [section ...]
 *     exit 0   every check that ran either passed or was skipped
 *     exit 1   at least one check failed
 *     exit 2   the arguments are wrong
 *
 * ============ WHY A PROGRAM AND NOT MORE HOST TESTS ============
 *
 * SECURITY-PLAN.md section 6 ends with the sentence that matters: "Nothing
 * here has been booted." Everything Phase 0 and Phase 1 built is covered by
 * host tests, and host tests cannot see any of the following:
 *
 *   - whether mkusers actually put ndusr in `dialout` on the built image, as
 *     opposed to whether users-table.txt asks it to;
 *   - whether eudev parsed 61-neodct-devices.rules and applied it, as
 *     opposed to whether the file is well-formed;
 *   - whether /dev/i2c-3 exists at all, which is a Rockchip device tree
 *     question with no answer on a build host;
 *   - whether S00userdata ran before anything opened the partition;
 *   - whether CONFIG_MNT_NS is in the vendor kernel -- section 6's first
 *     unverified item;
 *   - whether the boundary between ndusr and ndusr_ut is a boundary, which
 *     can only be established by BEING ndusr_ut and finding a door shut.
 *
 * Every one of those is a silent failure. A missing group does not stop the
 * boot, it makes the keypad dead; a wrong mode bit does not stop the boot, it
 * makes the browser able to read the contacts database. The class of bug this
 * whole branch is about is the class that looks exactly like nothing.
 *
 * ============ THE PROBES FORK AND REALLY DROP ============
 *
 * The device and layout probes are not stat() calls with arithmetic on the
 * mode bits. Arithmetic on mode bits is what the host tests already do, and
 * it answers a different question: it says what the kernel WOULD decide if
 * the process were ndusr_ut and if the supplementary groups were what the
 * table says and if no ACL, capability or namespace intervened.
 *
 * So each probe forks, calls nd_priv_become() in the child exactly as
 * nd_proc.c does before an execve, performs ONE operation, and reports
 * through the exit status. The parent never drops, so one probe cannot
 * poison the next, and a probe that segfaults costs one line of output.
 *
 * That means the answers are the kernel's, about this image, on this
 * hardware. It also means the whole tool needs to start as root -- and says
 * so rather than quietly passing, because a selftest that reports success
 * when it tested nothing is worse than no selftest.
 *
 * ============ WHAT AN EXPECTED DENIAL IS ============
 *
 * Half the checks here PASS when an open() fails, and that asymmetry is the
 * point: `ndusr_ut cannot open /dev/ttyUSB2` is SECURITY-AUDIT.md section 4
 * Q1, the premium-rate dialling vector, and it is a pass only if the failure
 * is EACCES or EPERM. ENOENT is NOT a pass; it means the modem is not there
 * and the test proved nothing, so it reports SKIP. A boundary that holds
 * because the thing behind it is missing is not a boundary.
 *
 * ============ AND WHAT THE FIRST VERSION COULD NOT SEE ============
 *
 * Written down after 0.5.8b, because this tool is the only thing in the
 * project that runs as the real user on the real phone and it did not catch
 * one of the ninety-four faults that shipped between 0.5.0b and 0.5.8b.
 *
 * It asked ONE question -- "can this user open this device node" -- of SIX
 * nodes, and there are three other questions, each of which broke something
 * the owner could hear or see:
 *
 *   CAN A CHILD STILL BE STARTED. The drop is not only a mode bit; it is a
 *   capability. setgroups(2) needs CAP_SETGID unconditionally, so an ndusr
 *   core asking a child to become ndusr gets a corpse at exit 122 rather
 *   than a player, and every sound the phone made died there for eight
 *   releases. No open() can see that. Only a fork can, and only from a
 *   process that has really dropped -- so `tone` below drops twice.
 *
 *   CAN THE CORE STILL REACH THE OPERATIONS IT DELEGATES. Power off, reboot
 *   and format do not belong to the core any more; they belong to the root
 *   broker on the other end of a socket. A predicate answering the wrong
 *   question cost every phone its power button. `halt` asks the policy and
 *   never, ever performs one.
 *
 *   AND -- the one that should be uncomfortable -- IS A PASS HERE A PASS FOR
 *   THE THING THAT MATTERS. This file asserted, and counted as a PASS, that
 *   ndusr_ut cannot open /dev/uinput. That assertion is correct as a rule and
 *   it was, at the same moment, the certificate on the bug that left the
 *   browser unable to receive a single keypress: the Browser app IS ndusr_ut
 *   and it was the process opening /dev/uinput. A denial is only half a
 *   fact. Every denial in `browser` below is now paired with the positive it
 *   is supposed to leave standing, and the pair fails if the positive is
 *   gone -- because "nobody can do this" is not the property anybody wanted.
 *
 * ============ A SKIP IS NOT A PASS, AND IT IS SAID TWICE ============
 *
 * Half of what is checked here is absent on a developer's checkout and on
 * QEMU, so skipping is normal and must not be quiet. Every skipped check is
 * named again in a recap at the bottom, and a run in which NOTHING was
 * proved exits 4 rather than 0, so a boot script that appends this to a log
 * cannot record "green" for a run that tested nothing at all.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_cpufreq.h"
#include "nd_fb.h"
#include "nd_media.h"
#include "nd_paths.h"
#include "nd_storage.h"
#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_svc.h"

/* The 120..127 pre-exec decoder. It is in libneodct, next to the code that
 * needed it first (nd_notify.c logs a dead player from it), and there is one
 * copy on purpose -- a second would drift the first time nd_proc.c reserved
 * another number. */
#include "../lib/nd_notify_priv.h"

/* ------------------------------------------------------------------ *
 * Results
 * ------------------------------------------------------------------ */

typedef enum { R_PASS, R_FAIL, R_SKIP, R_INFO } result;

static int g_pass, g_fail, g_skip;
static int g_verbose;
/* -q: only failures, the skip recap and the summary. The shape a boot script
 * wants when it appends this to /NeoDCT/User/logs and nobody is watching. */
static int g_quiet;

/* ============ EVERY SKIP, NAMED AGAIN AT THE BOTTOM ============
 *
 * The tool already printed "A skip is not a pass" at the end and it was not
 * enough, because on a serial console at 115200 the skips have scrolled off
 * by then and what is left on the screen is a line of green. So they are kept
 * and reprinted together: a person reading a boot log has to be able to see,
 * in one place, the list of things this run did not establish.
 *
 * Bounded and truncated rather than allocated. This runs on a phone with
 * 64 MB and it must not be the thing that fails; a run with more than 64
 * skipped checks has a much larger problem than a clipped list, and the
 * counter still reports the true total. */
#define SKIP_RECAP_MAX 64
#define SKIP_NAME_MAX  96
static char g_skipped[SKIP_RECAP_MAX][SKIP_NAME_MAX];
static int g_skipped_n;

static void report(result r, const char *name, const char *fmt, ...)
{
    va_list ap;
    const char *tag = r == R_PASS ? "PASS"
                    : r == R_FAIL ? "FAIL"
                    : r == R_SKIP ? "SKIP"
                                  : "----";

    if (r == R_PASS) {
        g_pass++;
        if (g_quiet)
            return;
        /* A passing check with nothing to say is noise on a serial console
         * at 115200. -v is for when you want the whole list anyway. */
        if (!g_verbose && fmt == NULL) {
            printf("  PASS  %s\n", name);
            return;
        }
    } else if (r == R_FAIL) {
        g_fail++;
    } else if (r == R_SKIP) {
        g_skip++;
        if (g_skipped_n < SKIP_RECAP_MAX)
            (void)nd_strlcpy(g_skipped[g_skipped_n++], name, SKIP_NAME_MAX);
        if (g_quiet)
            return;
    } else if (g_quiet) {
        return; /* R_INFO */
    }
    /* R_INFO is counted nowhere on purpose: it is an observation, not a
     * verdict, and totting it up would imply somebody decided something. */

    printf("  %s  %s", tag, name);
    if (fmt != NULL) {
        printf(" -- ");
        va_start(ap, fmt);
        (void)vprintf(fmt, ap);
        va_end(ap);
    }
    printf("\n");
}

static void section(const char *title)
{
    if (g_quiet)
        return;
    printf("\n== %s ==\n", title);
}

/* ------------------------------------------------------------------ *
 * The probe: fork, become somebody, do one thing, exit
 * ------------------------------------------------------------------ */

/* What the child attempted. */
typedef enum {
    P_READ,   /* open(O_RDONLY) -- can it read this file/node */
    P_RDWR,   /* open(O_RDWR) -- can it write to this node */
    P_LIST,   /* opendir + one readdir -- can it ENUMERATE this directory */
    P_TRAVERSE, /* open a name THROUGH the directory -- x without r */
    P_CREATE  /* create and unlink a file in this directory */
} probe_kind;

/* The child's answer, carried in one byte of exit status.
 *
 * Distinguishing "denied" from "not there" is the whole reason this is not a
 * boolean: an expected denial that is really an ENOENT proves nothing, and
 * reporting it as a pass would be the exact failure mode this tool exists to
 * catch. See the header. */
#define A_OK      0  /* the operation succeeded */
#define A_DENIED  1  /* EACCES or EPERM -- the kernel said no */
#define A_ABSENT  2  /* ENOENT or ENODEV -- there is nothing there to ask about */
#define A_OTHER   3  /* some other errno; the detail is lost but the fact is not */
#define A_NODROP  4  /* nd_priv_become() failed, so nothing was tested */
/* EBUSY: the node is there and this user is allowed, and somebody else has
 * it. A THIRD answer, added because the two-way split above forced a wrong
 * one on the case that matters most. "Is the modem missing, or is ndusr not
 * allowed to open it" was the whole of finding E, and on a running phone the
 * true answer to `ndusr can open the modem` is often neither: nd-core has it,
 * with TIOCEXCL, and a selftest run from the serial console beside a working
 * radio must not print FAIL for that. It is a SKIP -- nothing was proved --
 * and the message says to close the holder and ask again. */
#define A_BUSY    5

static int probe_child(const nd_priv_id *id, probe_kind kind, const char *path)
{
    int fd;
    DIR *d;

    if (nd_priv_become(id) != 0)
        return A_NODROP;

    switch (kind) {
    case P_READ:
    case P_RDWR:
        fd = open(path, kind == P_RDWR ? O_RDWR | O_CLOEXEC : O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            (void)close(fd);
            return A_OK;
        }
        break;
    case P_LIST:
        d = opendir(path);
        if (d != NULL) {
            (void)readdir(d);
            (void)closedir(d);
            return A_OK;
        }
        break;
    case P_TRAVERSE:
        /* x on a directory permits resolution THROUGH it; r permits listing.
         * 0751 grants the first and withholds the second, and that split is
         * what the whole /NeoDCT/User layout rests on. Asking for a name
         * that need not exist separates them: EACCES means the traverse was
         * refused, ENOENT means it was allowed and the name was simply not
         * there -- which is the answer being looked for. */
        {
            char buf[512];
            int n = snprintf(buf, sizeof buf, "%s/.nd-selftest-traverse", path);

            if (n < 0 || (size_t)n >= sizeof buf)
                return A_OTHER;
            fd = open(buf, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                (void)close(fd);
                return A_OK;
            }
            if (errno == ENOENT)
                return A_OK; /* resolved through the directory: traversal works */
        }
        break;
    case P_CREATE:
        {
            char buf[512];
            int n = snprintf(buf, sizeof buf, "%s/.nd-selftest-write", path);

            if (n < 0 || (size_t)n >= sizeof buf)
                return A_OTHER;
            fd = open(buf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) {
                (void)close(fd);
                (void)unlink(buf);
                return A_OK;
            }
        }
        break;
    default:
        return A_OTHER;
    }

    if (errno == EACCES || errno == EPERM)
        return A_DENIED;
    if (errno == ENOENT || errno == ENODEV || errno == ENXIO)
        return A_ABSENT;
    if (errno == EBUSY || errno == ETXTBSY || errno == EAGAIN)
        return A_BUSY;
    return A_OTHER;
}

/* Run one probe in a child and return its answer. */
static int probe(const nd_priv_id *id, probe_kind kind, const char *path)
{
    pid_t pid;
    int status;

    /* Flushed before the fork: anything sitting in the child's copy of the
     * stdio buffer would be printed twice, once by each process. */
    (void)fflush(stdout);

    pid = fork();
    if (pid < 0)
        return A_OTHER;
    if (pid == 0)
        _exit(probe_child(id, kind, path));

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return A_OTHER;
    }
    if (!WIFEXITED(status))
        return A_OTHER; /* the probe died; it certainly did not succeed */
    return WEXITSTATUS(status);
}

static const char *answer_name(int a)
{
    switch (a) {
    case A_OK:     return "allowed";
    case A_DENIED: return "denied";
    case A_ABSENT: return "not present";
    case A_BUSY:   return "in use by something else";
    case A_NODROP: return "could not drop privilege";
    default:       return "failed for another reason";
    }
}

/* Expect the operation to SUCCEED as this user. */
static void expect_allow(const nd_priv_id *id, const char *who, probe_kind kind,
                         const char *path, const char *name)
{
    int a = probe(id, kind, path);

    if (a == A_OK)
        report(R_PASS, name, g_verbose ? "%s: allowed" : NULL, who);
    else if (a == A_ABSENT)
        report(R_SKIP, name, "%s is not there -- an ABSENCE, not a refusal; "
                             "nothing was proved", path);
    else if (a == A_BUSY)
        report(R_SKIP, name, "%s is held by something else (EBUSY) -- close it and "
                             "run this again", path);
    else if (a == A_NODROP)
        report(R_SKIP, name, "could not become %s", who);
    else if (a == A_DENIED)
        report(R_FAIL, name, "%s is REFUSED %s -- a permission problem, not an "
                             "absence: the node is there and the kernel said no",
               who, path);
    else
        report(R_FAIL, name, "%s cannot reach %s (%s)", who, path, answer_name(a));
}

/* Expect the operation to be REFUSED as this user -- and refused by the
 * kernel, not by the thing being absent. See the header. */
static void expect_deny(const nd_priv_id *id, const char *who, probe_kind kind,
                        const char *path, const char *name)
{
    int a = probe(id, kind, path);

    if (a == A_DENIED)
        report(R_PASS, name, g_verbose ? "%s: denied, as it should be" : NULL, who);
    else if (a == A_OK)
        report(R_FAIL, name, "%s CAN reach %s -- the boundary is open", who, path);
    else if (a == A_ABSENT)
        report(R_SKIP, name, "%s is not present, so nothing was proved", path);
    else if (a == A_BUSY)
        report(R_SKIP, name, "%s is held by something else, so nothing was proved",
               path);
    else if (a == A_NODROP)
        report(R_SKIP, name, "could not become %s", who);
    else
        report(R_FAIL, name, "%s: %s (expected a clean denial)", who, answer_name(a));
}

/* ------------------------------------------------------------------ *
 * Section 1: the users and groups exist, and are what the table says
 * ------------------------------------------------------------------ */

static int has_group(const nd_priv_id *id, const char *group)
{
    struct group *gr = getgrnam(group);
    size_t i;

    if (gr == NULL)
        return -1; /* the group itself is missing: a different failure */
    for (i = 0u; i < id->n_groups; i++) {
        if (id->groups[i] == gr->gr_gid)
            return 1;
    }
    return id->gid == gr->gr_gid ? 1 : 0;
}

static void check_membership(const nd_priv_id *id, const char *who, const char *group,
                             int want)
{
    char name[128];
    int got = has_group(id, group);

    (void)snprintf(name, sizeof name, "%s %s group %s", who,
                   want ? "is in" : "is NOT in", group);
    if (got < 0) {
        report(want ? R_FAIL : R_SKIP, name, "there is no group called %s in this image",
               group);
        return;
    }
    if (got == want) {
        report(R_PASS, name, NULL);
        return;
    }
    if (want)
        report(R_FAIL, name, "the device nodes in this group are unreachable");
    else
        report(R_FAIL, name, "an untrusted process has this access");
}

static void section_users(const nd_priv_id *usr, const nd_priv_id *ut)
{
    section("Users and groups (neodct/configs/users-table.txt)");

    if (!usr->valid) {
        report(R_FAIL, "user " ND_PRIV_USER " exists",
               "not in /etc/passwd -- the image was built without the users table, "
               "and every process is still root");
    } else {
        report(R_PASS, "user " ND_PRIV_USER " exists", "uid %ld gid %ld",
               (long)usr->uid, (long)usr->gid);
        /* The ids are written out in the table rather than allocated, because
         * /NeoDCT/User stores numeric ids and survives every update. A build
         * that renumbered them would hand the owner's contacts to somebody
         * else, silently. */
        if (usr->uid == 1000u)
            report(R_PASS, ND_PRIV_USER " kept uid 1000", NULL);
        else
            report(R_FAIL, ND_PRIV_USER " kept uid 1000",
                   "it is %ld -- files on /NeoDCT/User now have the wrong owner",
                   (long)usr->uid);
    }

    if (!ut->valid) {
        report(R_FAIL, "user " ND_PRIV_USER_UT " exists",
               "not in /etc/passwd -- the browser cannot be confined");
    } else {
        report(R_PASS, "user " ND_PRIV_USER_UT " exists", "uid %ld gid %ld",
               (long)ut->uid, (long)ut->gid);
        if (ut->uid == 1001u)
            report(R_PASS, ND_PRIV_USER_UT " kept uid 1001", NULL);
        else
            report(R_FAIL, ND_PRIV_USER_UT " kept uid 1001", "it is %ld", (long)ut->uid);
    }

    if (usr->valid) {
        check_membership(usr, ND_PRIV_USER, "video", 1);
        check_membership(usr, ND_PRIV_USER, "audio", 1);
        check_membership(usr, ND_PRIV_USER, "input", 1);
        check_membership(usr, ND_PRIV_USER, "dialout", 1);
        check_membership(usr, ND_PRIV_USER, "i2c", 1);
    }
    if (ut->valid) {
        check_membership(ut, ND_PRIV_USER_UT, "video", 1);
        check_membership(ut, ND_PRIV_USER_UT, "audio", 1);
        check_membership(ut, ND_PRIV_USER_UT, "input", 1);
        /* The three that carry the confinement. dialout is the premium-rate
         * dialling vector (SECURITY-AUDIT.md 4 Q1); i2c is a bus, so anything
         * on it is reachable by address; ndusr is the group the whole
         * /NeoDCT/User mode layout is written against. */
        check_membership(ut, ND_PRIV_USER_UT, "dialout", 0);
        check_membership(ut, ND_PRIV_USER_UT, "i2c", 0);
        check_membership(ut, ND_PRIV_USER_UT, ND_PRIV_USER, 0);
    }
}

/* ------------------------------------------------------------------ *
 * Section 2: the mode bits on /NeoDCT/User, which ARE the confinement
 * ------------------------------------------------------------------ */

/* Kept in step with NEODCT_USER_LAYOUT in overlay/etc/init.d/S00userdata.
 * Duplicated on purpose rather than parsed: this is a check, and a check that
 * reads its expectations from the thing it is checking checks nothing. */
static const struct {
    const char *rel;
    unsigned mode;
    const char *group; /* NULL means ndusr */
} LAYOUT[] = {
    {"",            ND_MODE_USER_DIR, NULL},
    {"browser",     0770u, ND_PRIV_USER_UT},
    {"db",          0750u, NULL},
    {"logs",        0750u, NULL},
    {"tones",       0750u, NULL},
    {"wallpapers",  0750u, NULL},
    {"sdcard",      0751u, NULL},
    {".ndsys",      0700u, NULL},
    {".remote",     0700u, NULL},
    {".seedrng",    0700u, NULL},
};

static void section_layout(const nd_priv_id *usr, const nd_priv_id *ut)
{
    size_t i;

    section("The /NeoDCT/User layout (overlay/etc/init.d/S00userdata)");

    for (i = 0u; i < sizeof LAYOUT / sizeof LAYOUT[0]; i++) {
        char path[256];
        char name[384];
        struct stat st;
        unsigned got;
        gid_t want_gid;

        if (LAYOUT[i].rel[0] == '\0')
            (void)snprintf(path, sizeof path, "%s", ND_PATH_USER);
        else
            (void)snprintf(path, sizeof path, "%s/%s", ND_PATH_USER, LAYOUT[i].rel);
        (void)snprintf(name, sizeof name, "%s is %04o", path, LAYOUT[i].mode);

        if (stat(path, &st) != 0) {
            report(R_SKIP, name, "does not exist (S00userdata creates it on first boot)");
            continue;
        }
        got = (unsigned)(st.st_mode & 07777);
        if (got != LAYOUT[i].mode) {
            report(R_FAIL, name, "it is %04o", got);
            continue;
        }
        report(R_PASS, name, NULL);

        if (!usr->valid)
            continue;
        (void)snprintf(name, sizeof name, "%s is owned by %s", path, ND_PRIV_USER);
        if (st.st_uid != usr->uid) {
            report(R_FAIL, name, "uid %ld owns it", (long)st.st_uid);
            continue;
        }
        want_gid = usr->gid;
        if (LAYOUT[i].group != NULL && ut->valid)
            want_gid = ut->gid;
        if (st.st_gid != want_gid)
            report(R_FAIL, name, "its group is %ld, wanted %ld", (long)st.st_gid,
                   (long)want_gid);
        else
            report(R_PASS, name, NULL);
    }
}

/* ------------------------------------------------------------------ *
 * Section 3: what each user can actually open
 * ------------------------------------------------------------------ */

/* /dev/i2c-* and /dev/ttyUSB* have no fixed number: the bus the keypad
 * expander sits on is a device-tree question and the modem's tty depends on
 * enumeration order. Take the first that exists rather than guessing. */
static int first_existing(const char *const *candidates, char *out, size_t out_sz)
{
    size_t i;

    for (i = 0u; candidates[i] != NULL; i++) {
        if (access(candidates[i], F_OK) == 0) {
            (void)snprintf(out, out_sz, "%s", candidates[i]);
            return 1;
        }
    }
    return 0;
}

/* The first entry of `dir` whose name starts with `prefix` and ends with
 * `suffix` (either may be NULL), in readdir order.
 *
 * For /dev/snd, where a list of candidates is not workable: ALSA numbers the
 * card and the device independently -- pcmC0D0p on the Luckfox's rk809
 * codec, pcmC1D0p on a QEMU box whose first card is a virtual one -- and the
 * PLAYBACK/CAPTURE split that 61-neodct-devices.rules spends a paragraph on
 * lives in the last letter. So the pattern is the question and enumerating is
 * the only honest way to ask it.
 *
 * readdir order rather than sorted, deliberately: this is looking for "is
 * there ONE of these", and sorting would suggest a preference the caller does
 * not have. */
static int first_matching(const char *dir, const char *prefix, const char *suffix,
                          char *out, size_t out_sz)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (d == NULL)
        return 0;
    while (!found && (e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        size_t plen = (prefix != NULL) ? strlen(prefix) : 0u;
        size_t slen = (suffix != NULL) ? strlen(suffix) : 0u;

        /* Unconditionally, because a caller with no prefix at all -- the one
         * that walks /sys/class/backlight looking for whatever the board
         * called its panel -- would otherwise match "." and be handed
         * "/sys/class/backlight/./brightness", which exists and is the wrong
         * answer for the right reason. */
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (nlen < plen + slen)
            continue;
        if (plen != 0u && strncmp(e->d_name, prefix, plen) != 0)
            continue;
        if (slen != 0u && strcmp(e->d_name + (nlen - slen), suffix) != 0)
            continue;
        (void)snprintf(out, out_sz, "%s/%s", dir, e->d_name);
        found = 1;
    }
    (void)closedir(d);
    return found;
}

static const char *const I2C_NODES[] = {"/dev/i2c-3", "/dev/i2c-2", "/dev/i2c-1",
                                        "/dev/i2c-0", NULL};
static const char *const MODEM_NODES[] = {"/dev/ttyUSB2", "/dev/ttyUSB1", "/dev/ttyUSB0",
                                          NULL};
static const char *const INPUT_NODES[] = {"/dev/input/event0", "/dev/input/event1", NULL};

static void section_devices(const nd_priv_id *usr, const nd_priv_id *ut)
{
    char i2c[64], modem[64], evdev[64];
    char pcm_play[96], pcm_cap[96];
    int have_i2c = first_existing(I2C_NODES, i2c, sizeof i2c);
    int have_modem = first_existing(MODEM_NODES, modem, sizeof modem);
    int have_evdev = first_existing(INPUT_NODES, evdev, sizeof evdev);
    int have_play = first_matching("/dev/snd", "pcmC", "p", pcm_play, sizeof pcm_play);
    int have_cap = first_matching("/dev/snd", "pcmC", "c", pcm_cap, sizeof pcm_cap);

    section("Devices, probed by really becoming the user (61-neodct-devices.rules)");

    if (geteuid() != 0u) {
        report(R_SKIP, "device probes", "nd-selftest must start as root to drop to "
                                        "somebody else; run it from the serial console");
        return;
    }
    if (!usr->valid && !ut->valid) {
        report(R_SKIP, "device probes",
               "neither " ND_PRIV_USER " nor " ND_PRIV_USER_UT " exists, so there is "
               "nobody to become");
        return;
    }

    if (usr->valid) {
        expect_allow(usr, ND_PRIV_USER, P_RDWR, "/dev/fb0", ND_PRIV_USER " can open /dev/fb0");
        expect_allow(usr, ND_PRIV_USER, P_RDWR, "/dev/uinput",
                     ND_PRIV_USER " can open /dev/uinput");
        /* READ, and not RDWR, on purpose: the core reads the RTC at boot and
         * the SET goes through the broker, which is root and holds the
         * CAP_SYS_TIME that RTC_SET_TIME needs whatever the mode bits say
         * (nd_clock.c opens it O_RDONLY for exactly that ioctl). Widening this
         * probe to RDWR would fail on a correct phone. */
        expect_allow(usr, ND_PRIV_USER, P_READ, "/dev/rtc0",
                     ND_PRIV_USER " can open /dev/rtc0");
        if (have_evdev)
            expect_allow(usr, ND_PRIV_USER, P_READ, evdev, ND_PRIV_USER " can read the keypad");
        /* ============ WHAT THIS LINE MEANS NOW ============
         *
         * It used to be the keypad's check and it is not any more, and
         * mistaking one for the other in either direction would be expensive.
         *
         * The keypad no longer depends on this node's group at all: nd_main.c
         * opens /dev/i2c-<bus> while it is still root and hands the validated
         * descriptor across the drop, because I2C_SLAVE is per-descriptor
         * state and open() checks permission only at open() time. That is what
         * took the udev race out of the keypad's boot.
         *
         * What still opens this node AFTER the drop is the MAX17048 fuel
         * gauge, on the same bus, from nd_battery.c -- and it opens FIRST, a
         * few lines before the keypad in nd_ui_init(). So a FAIL here means
         * the battery meter is stuck at a simulated 3.85 V, and it is also
         * the earliest evidence in core.log that the i2c grant lost its race.
         * The keypad may well be fine on the same boot. */
        if (have_i2c)
            expect_allow(usr, ND_PRIV_USER, P_RDWR, i2c,
                         ND_PRIV_USER " can open the i2c bus (the fuel gauge; the "
                         "keypad crosses the drop on a descriptor)");
        else
            report(R_SKIP, ND_PRIV_USER " can open the i2c bus",
                   "no /dev/i2c-* on this build (expected on QEMU; NOT expected on the phone)");
        if (have_modem)
            expect_allow(usr, ND_PRIV_USER, P_RDWR, modem, ND_PRIV_USER " can open the modem");
        else
            report(R_SKIP, ND_PRIV_USER " can open the modem", "no /dev/ttyUSB* on this build");

        /* ============ THE SOUND CARD, WHICH NOTHING HERE ASKED ABOUT ============
         *
         * Every noise the phone makes is aplay or mpv, spawned by the core and
         * dropped to ndusr, opening a node under /dev/snd. The tone findings
         * turned out to be a capability problem rather than a permission one --
         * see the `tone` section -- but that was established by reading the
         * code, and this file existed precisely so that such things would not
         * have to be. There was no /dev/snd probe at all.
         *
         * PLAYBACK is the one the ringer needs, and it belongs to `audio`,
         * which ndusr and ndusr_ut both hold: a web page is allowed to play
         * sound.
         *
         * CAPTURE is the other half, and it is the reason the rules file
         * carries a rule of its own for it. eudev's stock rule puts EVERY
         * /dev/snd node in `audio` at 0660, capture included, so the same
         * membership that lets the browser play a video lets it open the
         * microphone. 61-neodct-devices.rules narrows pcmC*D*c to ndusr; the
         * pair of checks below is what proves the narrowing landed, and the
         * denial for ndusr_ut is a real boundary rather than a nicety. */
        if (have_play)
            expect_allow(usr, ND_PRIV_USER, P_RDWR, pcm_play,
                         ND_PRIV_USER " can open the sound card");
        else
            report(R_SKIP, ND_PRIV_USER " can open the sound card",
                   "no /dev/snd/pcmC*D*p here -- there is no playback device at all, "
                   "so the phone can make no sound whatever the permissions say");
        if (have_cap)
            expect_allow(usr, ND_PRIV_USER, P_RDWR, pcm_cap,
                         ND_PRIV_USER " can open the microphone");
        else
            report(R_SKIP, ND_PRIV_USER " can open the microphone",
                   "no /dev/snd/pcmC*D*c on this build");

        /* ============ AND THE CARD STATE FILE ============
         *
         * /run/neodct/sdcard.prop is written by the sdcard helper, which the
         * broker runs AS ROOT, and read by nd_storage.c in the UI, which is
         * ndusr. That is a privilege boundary with no device node in it and
         * therefore nothing in this section had ever looked at it -- and a
         * helper that inherited umask 0027 published it root:root 0640, so
         * the UI read nothing and reported "no card" for a card that had just
         * been formatted successfully.
         *
         * A file, not a node, and the probe is the same probe: become ndusr
         * and try to read it. Absent is a SKIP and means only that no scan has
         * published anything yet on this boot. */
        if (access(ND_PATH_SDCARD_STATE, F_OK) == 0) {
            struct stat st;

            expect_allow(usr, ND_PRIV_USER, P_READ, ND_PATH_SDCARD_STATE,
                         "the UI's user can read the card state file");
            /* Printed whatever the verdict, because when it FAILS this line is
             * the whole diagnosis and nobody should have to go and stat it. */
            if (stat(ND_PATH_SDCARD_STATE, &st) == 0)
                report(R_INFO, ND_PATH_SDCARD_STATE, "uid %ld gid %ld mode %04o",
                       (long)st.st_uid, (long)st.st_gid,
                       (unsigned)(st.st_mode & 07777));
        } else {
            report(R_SKIP, "the UI's user can read the card state file",
                   "%s does not exist -- nothing has scanned a card on this boot",
                   ND_PATH_SDCARD_STATE);
        }
    }

    if (!ut->valid)
        return;

    /* The untrusted user. Every one of these is a pass only when the kernel
     * says no. */
    expect_allow(ut, ND_PRIV_USER_UT, P_RDWR, "/dev/fb0",
                 ND_PRIV_USER_UT " can still draw on /dev/fb0");
    if (have_evdev)
        expect_allow(ut, ND_PRIV_USER_UT, P_READ, evdev,
                     ND_PRIV_USER_UT " can still receive keys");

    if (have_modem)
        expect_deny(ut, ND_PRIV_USER_UT, P_RDWR, modem,
                    ND_PRIV_USER_UT " CANNOT open the modem (premium-rate dialling)");
    else
        report(R_SKIP, ND_PRIV_USER_UT " CANNOT open the modem",
               "no /dev/ttyUSB* here, so the denial was not tested");
    if (have_i2c)
        expect_deny(ut, ND_PRIV_USER_UT, P_RDWR, i2c,
                    ND_PRIV_USER_UT " CANNOT open the i2c bus");
    if (have_play)
        expect_allow(ut, ND_PRIV_USER_UT, P_RDWR, pcm_play,
                     ND_PRIV_USER_UT " can still play sound");
    if (have_cap)
        expect_deny(ut, ND_PRIV_USER_UT, P_RDWR, pcm_cap,
                    ND_PRIV_USER_UT " CANNOT open the microphone");
    else
        report(R_SKIP, ND_PRIV_USER_UT " CANNOT open the microphone",
               "no capture device here, so the denial was not tested");

    /* This one is now HALF of a check, and saying so is the point.
     *
     * It stood here alone as the last line of the section and it passed on
     * every phone whose browser could not receive a keypress -- see the
     * header. The rule it asserts is right; what was wrong was believing that
     * a denial, on its own, said anything about whether the browser still
     * worked. The other half is in `browser` below, and that section FAILS
     * when this passes and the browser has no keys, which is the state this
     * line used to certify as correct. */
    expect_deny(ut, ND_PRIV_USER_UT, P_RDWR, "/dev/uinput",
                ND_PRIV_USER_UT " CANNOT inject keys through /dev/uinput"
                " (see the browser section for the other half)");
}

/* ------------------------------------------------------------------ *
 * Section 4: traversal is not listing, which is the whole design
 * ------------------------------------------------------------------ */

static void section_boundary(const nd_priv_id *ut)
{
    section("The boundary: 0751 grants traversal and withholds listing");

    if (geteuid() != 0u || !ut->valid) {
        report(R_SKIP, "boundary probes", "needs root and " ND_PRIV_USER_UT);
        return;
    }

    /* The two that are the whole of it. If the first fails the browser has
     * nowhere to write; if the second passes it can enumerate the ssh keys,
     * the databases and the update records by name. */
    expect_allow(ut, ND_PRIV_USER_UT, P_TRAVERSE, ND_PATH_USER,
                 ND_PRIV_USER_UT " can traverse " ND_PATH_USER);
    expect_deny(ut, ND_PRIV_USER_UT, P_LIST, ND_PATH_USER,
                ND_PRIV_USER_UT " CANNOT list " ND_PATH_USER);

    expect_allow(ut, ND_PRIV_USER_UT, P_LIST, ND_PATH_USER "/browser",
                 ND_PRIV_USER_UT " can use its own directory");
    expect_allow(ut, ND_PRIV_USER_UT, P_CREATE, ND_PATH_USER "/browser",
                 ND_PRIV_USER_UT " can write to its own directory");

    expect_deny(ut, ND_PRIV_USER_UT, P_CREATE, ND_PATH_USER,
                ND_PRIV_USER_UT " CANNOT write to " ND_PATH_USER);
    expect_deny(ut, ND_PRIV_USER_UT, P_LIST, ND_PATH_DB_DIR,
                ND_PRIV_USER_UT " CANNOT list the databases");
    expect_deny(ut, ND_PRIV_USER_UT, P_READ, ND_PATH_DB_PHONEBOOK,
                ND_PRIV_USER_UT " CANNOT read the phonebook");
    expect_deny(ut, ND_PRIV_USER_UT, P_LIST, ND_PATH_REMOTE_DIR,
                ND_PRIV_USER_UT " CANNOT reach the ssh keys");
    expect_deny(ut, ND_PRIV_USER_UT, P_LIST, ND_PATH_USER "/.ndsys",
                ND_PRIV_USER_UT " CANNOT reach the update records");
}

/* ------------------------------------------------------------------ *
 * Section 5: the mount flags, which are the half DAC cannot express
 * ------------------------------------------------------------------ */

/* Find `point` in /proc/mounts and copy its option field.
 *
 * /proc/mounts rather than mountinfo because the fields are simpler and the
 * option list is the whole question. The LAST match wins: a later mount on
 * the same point shadows an earlier one, and it is the one in effect. */
static int mount_options(const char *point, char *out, size_t out_sz)
{
    FILE *f = fopen("/proc/mounts", "re");
    char line[1024];
    int found = 0;

    if (f == NULL)
        return 0;
    while (fgets(line, (int)sizeof line, f) != NULL) {
        char dev[256], mp[256], type[64], opts[512];

        if (sscanf(line, "%255s %255s %63s %511s", dev, mp, type, opts) != 4)
            continue;
        if (strcmp(mp, point) != 0)
            continue;
        (void)snprintf(out, out_sz, "%s", opts);
        found = 1;
    }
    (void)fclose(f);
    return found;
}

/* A whole-word search of a comma-separated option list. `strstr(opts, "ro")`
 * would match the "ro" inside "errors=remount-ro", and `nodev` is a prefix of
 * nothing but is worth doing properly anyway. */
static int has_option(const char *opts, const char *want)
{
    size_t want_len = strlen(want);
    const char *p = opts;

    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);

        if (len == want_len && strncmp(p, want, want_len) == 0)
            return 1;
        if (comma == NULL)
            break;
        p = comma + 1;
    }
    return 0;
}

static void check_option(const char *point, const char *opts, const char *want, int fatal)
{
    char name[256];

    (void)snprintf(name, sizeof name, "%s is mounted %s", point, want);
    if (has_option(opts, want))
        report(R_PASS, name, NULL);
    else
        report(fatal ? R_FAIL : R_SKIP, name, "options are: %s", opts);
}

static void section_mounts(void)
{
    char opts[512];

    section("Mount flags (overlay/etc/fstab, initramfs/init)");

    if (mount_options("/", opts, sizeof opts)) {
        /* The read-only root is what dm-verity is protecting. A rw root here
         * means either the verity setup did not happen or something
         * remounted it, and both make the signature check pointless. */
        check_option("/", opts, "ro", 1);
    } else {
        report(R_SKIP, "/ is mounted ro", "no / in /proc/mounts");
    }

    if (mount_options(ND_PATH_USER, opts, sizeof opts)) {
        /* nosuid is the one that matters most and the one a bare DAC layout
         * cannot express: without it, anything that can write here can leave
         * a setuid binary behind for later. nodev likewise -- a device node
         * on a writable partition is a way past every mode bit above. */
        check_option(ND_PATH_USER, opts, "nosuid", 1);
        check_option(ND_PATH_USER, opts, "nodev", 1);
        check_option(ND_PATH_USER, opts, "rw", 1);
    } else {
        report(R_SKIP, ND_PATH_USER " is a separate mount",
               "it is part of the root filesystem on this build -- expected on a "
               "developer's tree, NOT expected on a phone");
    }

    /* The two halves of a NeoDCT card are NOT mounted alike, and asking the
     * same question of both is how the first version of this got it wrong.
     *
     * noexec belongs on the partition things ARRIVE on, not on the one the
     * owner copied their media to -- SECURITY-PLAN.md section 1, and the
     * reason is that noexec refuses mmap(PROT_EXEC), and therefore dlopen(),
     * as well as execve. Put it on the media partition and a plugin or a
     * codec loaded from a card stops working; put it on the arrival
     * partition and a downloaded binary cannot be run without the owner
     * first copying it across, which is the deliberate act the design wants.
     *
     * So: nosuid and nodev on both, noexec required on arrival and expected
     * ABSENT on media. */
    if (mount_options(ND_PATH_SDCARD_MOUNT, opts, sizeof opts)) {
        check_option(ND_PATH_SDCARD_MOUNT, opts, "nosuid", 1);
        check_option(ND_PATH_SDCARD_MOUNT, opts, "nodev", 1);
        if (has_option(opts, "noexec"))
            report(R_FAIL, ND_PATH_SDCARD_MOUNT " is NOT noexec",
                   "noexec here refuses dlopen too, and this is the owner's own "
                   "media -- it belongs on the arrival partition instead");
        else
            report(R_PASS, ND_PATH_SDCARD_MOUNT " is NOT noexec", NULL);
    } else {
        report(R_SKIP, "the SD card is mounted", "no card in the slot");
    }

    /* ============ AND IT IS A DIRECTORY NOW, NOT A PARTITION ============
     *
     * This asked whether the arrival area was MOUNTED, because on a FAT card
     * it had to be: FAT records no ownership, so the only way to give
     * downloads a different regime from the owner's music was a second
     * filesystem with its own uid= and gid=.
     *
     * On ext4 the ownership is on the inode, so untrusted/ is an ordinary
     * directory on the one partition -- and the old check answered SKIP
     * ("no arrival partition") on a perfectly correct card, which is a test
     * quietly reporting nothing on the thing it was written to guard.
     *
     * What matters has not changed, only where it is written down: ndusr owns
     * it, ndusr_ut may write it, and nobody else may read it. */
    {
        struct stat st;
        nd_priv_id ut_id;

        if (stat(ND_PATH_CARD_UNTRUSTED, &st) != 0) {
            report(R_SKIP, ND_PATH_CARD_UNTRUSTED " is 0770 " ND_PRIV_USER ":" ND_PRIV_USER_UT,
                   "not present -- no card, or a card the phone has not laid out");
        } else if ((unsigned)(st.st_mode & 07777) != 0770u) {
            report(R_FAIL, ND_PATH_CARD_UNTRUSTED " is 0770 " ND_PRIV_USER ":" ND_PRIV_USER_UT,
                   "the untrusted set writes here and nobody else may read it");
        } else if (!nd_priv_lookup(ND_PRIV_USER_UT, &ut_id) || st.st_gid != ut_id.gid) {
            report(R_FAIL, ND_PATH_CARD_UNTRUSTED " is 0770 " ND_PRIV_USER ":" ND_PRIV_USER_UT,
                   "wrong group -- the untrusted set cannot write its own area");
        } else {
            report(R_PASS, ND_PATH_CARD_UNTRUSTED " is 0770 " ND_PRIV_USER ":" ND_PRIV_USER_UT,
                   NULL);
        }

        /* ============ AND noexec, WHICH IS THE OTHER HALF OF IT ============
         *
         * The mode bits above say who may write here. They say nothing about
         * what happens to what is written, and this is the ONE directory on
         * the phone whose contents arrived from the internet.
         *
         * On the FAT card it was a second filesystem and got noexec from its
         * own mount line. On ext4 it is a directory on the media partition --
         * which must NOT be noexec, because noexec refuses mmap(PROT_EXEC) and
         * therefore dlopen(), and an installed app is a shared object. So the
         * helper bind-mounts untrusted/ onto itself and remounts that bind
         * noexec, which is the only way to give one directory a different
         * regime from the partition it sits on.
         *
         * A bind that fails is deliberately not fatal in the helper -- it
         * warns and carries on, because gating the whole download area on a
         * remount that some kernels refuse would turn hardening into "nothing
         * downloads". That decision is only defensible if somebody eventually
         * NOTICES, and this is the somebody. FAIL when the directory is there
         * and the bind is not; SKIP only when there is no directory at all. */
        if (stat(ND_PATH_CARD_UNTRUSTED, &st) == 0) {
            if (!mount_options(ND_PATH_CARD_UNTRUSTED, opts, sizeof opts))
                report(R_FAIL, ND_PATH_CARD_UNTRUSTED " is noexec",
                       "it is not a mount point at all -- the helper's noexec bind "
                       "did not happen, so a binary downloaded here can be run");
            else
                check_option(ND_PATH_CARD_UNTRUSTED, opts, "noexec", 1);
        } else {
            report(R_SKIP, ND_PATH_CARD_UNTRUSTED " is noexec",
                   "the directory is not there, so there is nothing to check");
        }
    }
}

/* ------------------------------------------------------------------ *
 * Section 6: what the vendor kernel actually gives us
 * ------------------------------------------------------------------ */

static void section_kernel(void)
{
    section("Kernel support (SECURITY-PLAN.md section 6 -- the unverified list)");

    /* Section 6's first item, and the one it says to check first. Two
     * features rest on it and neither can be tested any other way: this
     * really forks and really calls unshare(CLONE_NEWNS). */
    if (nd_proc_namespaces_available())
        report(R_PASS, "CONFIG_MNT_NS: unshare(CLONE_NEWNS) works", NULL);
    else
        report(R_FAIL, "CONFIG_MNT_NS: unshare(CLONE_NEWNS) works",
               "the browser gets no mount namespace on this kernel -- the uid "
               "boundary still holds, Phase 2 does not");

    /* no_new_privs is unconditional since 3.5, so a failure means something
     * much stranger than a missing config. It is the precondition for the
     * seccomp filter in Phase 3, and a silently missing one would make that
     * filter refuse to load for a reason nobody would look for. */
    {
        pid_t pid;
        int status;

        (void)fflush(stdout);
        pid = fork();
        if (pid == 0)
            _exit(nd_priv_no_new_privs() == 0 ? 0 : 1);
        if (pid < 0) {
            report(R_SKIP, "PR_SET_NO_NEW_PRIVS is accepted", "fork failed");
        } else {
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                continue;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                report(R_PASS, "PR_SET_NO_NEW_PRIVS is accepted", NULL);
            else
                report(R_FAIL, "PR_SET_NO_NEW_PRIVS is accepted",
                       "Phase 3's seccomp filter will not load on this kernel");
        }
    }

    /* Phase 3's other half. /proc/self/status carries it on every kernel that
     * has it, which is a cheaper answer than /proc/config.gz -- that is often
     * not built in, and is the one thing section 6 could not read from here. */
    {
        FILE *f = fopen("/proc/self/status", "re");
        char line[256];
        int seen = 0;

        if (f != NULL) {
            while (fgets(line, (int)sizeof line, f) != NULL) {
                if (strncmp(line, "Seccomp:", 8) == 0) {
                    seen = 1;
                    break;
                }
            }
            (void)fclose(f);
        }
        if (seen)
            report(R_PASS, "CONFIG_SECCOMP is built in", NULL);
        else
            report(R_FAIL, "CONFIG_SECCOMP is built in",
                   "no Seccomp: line in /proc/self/status -- Phase 3 cannot be built "
                   "for this kernel");
    }

    /* SELinux, which SECURITY-PLAN.md section 8 makes the answer to the core
     * staying root. Three distinguishable states and they mean quite
     * different things, so none of them is reported as a plain pass or fail:
     *
     *   no /sys/fs/selinux    not built into this kernel. On QEMU that is a
     *                         config to add; on the Luckfox it is section 6's
     *                         open question and the reason this check exists.
     *   present, permissive   built in, policy loaded, DENYING NOTHING. This
     *                         is the state policy authoring happens in, and
     *                         it takes no power off root -- so it must never
     *                         read as a pass.
     *   present, enforcing    the policy is in effect.
     *
     * Reported and not judged, because which of the three is CORRECT depends
     * on how far the policy work has got, and a check that fails for being
     * early is a check people learn to ignore. */
    {
        FILE *f = fopen("/sys/fs/selinux/enforce", "re");

        if (f == NULL) {
            if (access("/sys/fs/selinux", F_OK) == 0)
                report(R_INFO, "SELinux", "present, but no policy is loaded -- "
                                          "denying nothing");
            else
                report(R_INFO, "SELinux", "not in this kernel (CONFIG_SECURITY_SELINUX)");
        } else {
            int mode = -1;

            if (fscanf(f, "%d", &mode) != 1)
                mode = -1;
            (void)fclose(f);
            if (mode == 1)
                report(R_INFO, "SELinux", "ENFORCING");
            else if (mode == 0)
                report(R_INFO, "SELinux", "permissive -- logging denials, allowing "
                                          "them; this takes no power off root");
            else
                report(R_INFO, "SELinux", "present, mode unreadable");
        }
    }

    /* The signing key the initramfs verifies a release against. Its absence
     * does not fail a boot; it fails an UPDATE, months later, in a place
     * nobody is watching. */
    if (access(ND_PATH_SYSTEM "/keys/neodct-release.pub", R_OK) == 0)
        report(R_PASS, "the release public key is on the image", NULL);
    else
        report(R_SKIP, "the release public key is on the image",
               "no %s/keys/neodct-release.pub -- updates cannot be verified",
               ND_PATH_SYSTEM);
}

/* ------------------------------------------------------------------ *
 * Section 7: who is actually running as whom, right now
 * ------------------------------------------------------------------ */

/* This is the section to read first when the phone is up and something is
 * open, because it is the only one that reports the live answer rather than
 * the arrangements made for it. Everything above establishes that the
 * boundary COULD hold; this says who is currently standing on which side.
 *
 * Most of it is deliberately not a verdict. nd-core runs as root today and
 * SECURITY-PLAN.md section 7 says exactly why -- clock_settime, reboot,
 * starting sshd, formatting a card, and unshare(CLONE_NEWNS) for the
 * browser's own namespace, which has to happen while the privilege is still
 * there. Reporting that as a failure would be scoring the plan against work
 * it explicitly defers. So those lines observe.
 *
 * The one real check is netsurf, because netsurf is the only thing on this
 * image that IS supposed to have dropped, and a netsurf running as root
 * means apps/Browser/main.c's nd_priv_lookup() found no ndusr_ut and carried
 * on -- which is a silent, total loss of the confinement. */

static int proc_name(const char *pid, char *out, size_t out_sz)
{
    char path[280]; /* /proc/ + a NAME_MAX-sized d_name + /comm */
    FILE *f;
    size_t n;

    (void)snprintf(path, sizeof path, "/proc/%s/comm", pid);
    f = fopen(path, "re");
    if (f == NULL)
        return 0;
    if (fgets(out, (int)out_sz, f) == NULL) {
        (void)fclose(f);
        return 0;
    }
    (void)fclose(f);
    n = strlen(out);
    while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r'))
        out[--n] = '\0';
    return 1;
}

/* The real uid out of /proc/<pid>/status. Not stat() on the directory: that
 * gives the EFFECTIVE uid, and the whole question here is whether a real
 * drop happened rather than a seteuid that can be undone. */
static int proc_ruid(const char *pid, uid_t *out)
{
    char path[280];
    char line[256];
    FILE *f;
    int found = 0;

    (void)snprintf(path, sizeof path, "/proc/%s/status", pid);
    f = fopen(path, "re");
    if (f == NULL)
        return 0;
    while (fgets(line, (int)sizeof line, f) != NULL) {
        unsigned long r;

        if (strncmp(line, "Uid:", 4) == 0 && sscanf(line + 4, "%lu", &r) == 1) {
            *out = (uid_t)r;
            found = 1;
            break;
        }
    }
    (void)fclose(f);
    return found;
}

/* An nd-apprun's argv[1] is the app's directory (nd_app.h: "nd-apprun
 * <app-dir> [entry] [arg]"), so /proc/<pid>/cmdline says WHICH app a given
 * process is running -- which is what turns "some app is root" into a
 * verdict. Without it the uid alone cannot be judged: root is correct for a
 * diagnostic and wrong for everything else. */
static int proc_argv1(const char *pid, char *out, size_t out_sz)
{
    char path[280];
    char buf[1024];
    FILE *f;
    size_t n, i, start;

    (void)snprintf(path, sizeof path, "/proc/%s/cmdline", pid);
    f = fopen(path, "re");
    if (f == NULL)
        return 0;
    n = fread(buf, 1u, sizeof buf - 1u, f);
    (void)fclose(f);
    if (n == 0u)
        return 0;
    buf[n] = '\0';
    /* NUL-separated. Skip argv[0]. */
    for (i = 0u; i < n && buf[i] != '\0'; i++)
        ;
    start = i + 1u;
    if (start >= n)
        return 0;
    (void)snprintf(out, out_sz, "%s", buf + start);
    return 1;
}

static const char *user_name_of(uid_t uid, const nd_priv_id *usr, const nd_priv_id *ut)
{
    if (uid == 0u)
        return "root";
    if (usr->valid && uid == usr->uid)
        return ND_PRIV_USER;
    if (ut->valid && uid == ut->uid)
        return ND_PRIV_USER_UT;
    return "somebody else";
}

static void section_processes(const nd_priv_id *usr, const nd_priv_id *ut)
{
    /* Matched against /proc/<pid>/comm, which is the first 15 bytes of the
     * name and has no path -- so these are prefixes of what the kernel
     * stores, not paths to binaries. */
    static const char *const WATCH[] = {"nd-core", "nd-apprun", "netsurf", "mpv",
                                        "neodct-play", "sshd", NULL};
    DIR *d = opendir("/proc");
    struct dirent *e;
    int seen_netsurf = 0;
    int seen_any = 0;

    section("Who is running as whom (the live answer)");

    if (d == NULL) {
        report(R_SKIP, "process owners", "cannot read /proc");
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char comm[64];
        uid_t uid;
        size_t i;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (!proc_name(e->d_name, comm, sizeof comm))
            continue;
        for (i = 0u; WATCH[i] != NULL; i++) {
            if (strncmp(comm, WATCH[i], strlen(WATCH[i])) != 0)
                continue;
            if (!proc_ruid(e->d_name, &uid))
                break;
            seen_any = 1;
            if (strcmp(WATCH[i], "nd-apprun") == 0) {
                char appdir[256];
                int eng;

                if (!proc_argv1(e->d_name, appdir, sizeof appdir)) {
                    report(R_INFO, comm, "pid %s, running as %s (uid %ld)", e->d_name,
                           user_name_of(uid, usr, ut), (long)uid);
                    break;
                }
                eng = strncmp(appdir, ND_PATH_ENG_APPS_DIR "/",
                              sizeof(ND_PATH_ENG_APPS_DIR "/") - 1u) == 0;
                if (eng) {
                    /* A diagnostic is SUPPOSED to be root -- see
                     * nd_proc_app_needs_root(). Reported, not judged: if it
                     * is not root that is a broken engineering app rather
                     * than a broken boundary, and the two want different
                     * sentences. */
                    report(R_INFO, appdir, "engineering app, running as %s (uid %ld)",
                           user_name_of(uid, usr, ut), (long)uid);
                } else if (usr->valid && uid == usr->uid) {
                    report(R_PASS, appdir, "running as " ND_PRIV_USER);
                } else {
                    report(R_FAIL, appdir,
                           "a stock app is running as %s (uid %ld) -- it did not drop",
                           user_name_of(uid, usr, ut), (long)uid);
                }
                break;
            }
            if (strcmp(WATCH[i], "netsurf") == 0) {
                seen_netsurf = 1;
                if (ut->valid && uid == ut->uid)
                    report(R_PASS, "netsurf is running as " ND_PRIV_USER_UT, "pid %s",
                           e->d_name);
                else
                    report(R_FAIL, "netsurf is running as " ND_PRIV_USER_UT,
                           "pid %s is running as %s (uid %ld) -- the browser did not "
                           "drop", e->d_name, user_name_of(uid, usr, ut), (long)uid);
            } else {
                report(R_INFO, comm, "pid %s, running as %s (uid %ld)", e->d_name,
                       user_name_of(uid, usr, ut), (long)uid);
            }
            break;
        }
    }
    (void)closedir(d);

    if (!seen_netsurf)
        report(R_SKIP, "netsurf is running as " ND_PRIV_USER_UT,
               "the browser is not open; open it and run this again");
    if (!seen_any)
        report(R_SKIP, "process owners", "none of the NeoDCT processes are running");
}

/* ------------------------------------------------------------------ *
 * Section 8: did the grants LAND, or is this still devtmpfs's default?
 * ------------------------------------------------------------------ *
 *
 * The section above asks the kernel whether a user may open a node, which is
 * the right question and hides one answer inside another: a node the dropped
 * core cannot open because the rule never fired looks exactly like a node it
 * cannot open because the rule is wrong, and both look like a broken phone.
 *
 * eudev applies 61-neodct-devices.rules and eudev's own 50-udev-default.rules
 * asynchronously, on uevents, some of which arrive seconds after the node
 * does. Everything in this file that broke on the phone and not in QEMU broke
 * because of that gap. So this section asks the FILESYSTEM what the group and
 * mode actually are, right now, and names the two failures separately:
 *
 *   "still root:root 0600"   nothing has touched this node. On a character
 *                            device that is devtmpfs's default -- a node with
 *                            no GROUP= from any rule gets 0600, not 0660 --
 *                            so this is the fingerprint of a rule that never
 *                            ran, and it is what a lost coldplug race looks
 *                            like from the outside.
 *
 *   "group X, wanted Y"      a rule ran and says something else. That is a
 *                            mistake in the rules file, not a race, and it
 *                            wants a different person to look at it.
 *
 * The table is written out longhand rather than parsed out of the rules file
 * or out of S10udevd's DEVICE_GRANTS, for the reason section_layout gives
 * about S00userdata: a check that reads its expectations from the thing it is
 * checking checks nothing. When the two disagree, THAT is the finding.
 */

typedef struct {
    /* Either `path` is a literal, or dir/prefix/suffix name a pattern and the
     * first match is taken. The pattern form exists because the numbers are
     * not ours to predict: which i2c bus the expander is on is a device-tree
     * question, ALSA numbers cards by probe order, and the backlight
     * directory is named after whatever driver claimed the panel. */
    const char *path;
    const char *dir;
    const char *prefix;
    const char *suffix;
    const char *tail;  /* appended to a pattern match, e.g. "/brightness" */
    const char *group; /* the group the rules grant it to */
    unsigned mode;     /* the mode the rules set */
    unsigned deflt;    /* what "nothing touched it" looks like */
    const char *what;  /* for the human, and for the check's name */
    const char *rule;  /* which file is supposed to have done it */
} grant_row;

static const grant_row GRANTS[] = {
    /* The four eudev's own 50-udev-default.rules covers. They are here
     * because "already right" is a claim about a file this tree does not own
     * and does not ship, and it has been wrong once already: the modem's
     * ttyUSB grant depends entirely on stock rules and on the coldplug
     * reaching the tty subsystem in time. */
    {"/dev/fb0", NULL, NULL, NULL, NULL, "video", 0660u, 0600u, "the framebuffer",
     "eudev 50-udev-default.rules"},
    {NULL, "/dev/input", "event", NULL, NULL, "input", 0660u, 0600u, "an evdev keypad",
     "eudev 50-udev-default.rules"},
    {NULL, "/dev/snd", "pcmC", "p", NULL, "audio", 0660u, 0600u, "the sound card",
     "eudev 50-udev-default.rules"},
    {NULL, "/dev", "ttyUSB", NULL, NULL, "dialout", 0660u, 0600u, "the modem's AT port",
     "eudev 50-udev-default.rules"},

    /* And the ones this tree does own. */
    {NULL, "/dev", "i2c-", NULL, NULL, "i2c", 0660u, 0600u, "the i2c bus",
     "61-neodct-devices.rules"},
    {"/dev/uinput", NULL, NULL, NULL, NULL, "ndusr", 0660u, 0600u, "the key bridge",
     "61-neodct-devices.rules"},
    {NULL, "/dev", "rtc", NULL, NULL, "ndusr", 0660u, 0600u, "the hardware clock",
     "61-neodct-devices.rules"},
    {NULL, "/dev/snd", "pcmC", "c", NULL, "ndusr", 0660u, 0600u, "the microphone",
     "61-neodct-devices.rules"},

    /* The sysfs tiers. A sysfs attribute with no rule is root-owned 0644 --
     * READABLE by everyone and writable by nobody but root -- so the failure
     * here is quieter than a device node's: the phone can read the brightness
     * it cannot change, and nd_backlight.c's write silently returns false. */
    {NULL, ND_BL_BACKLIGHT_ROOT, NULL, NULL, "/brightness", "video", 0664u, 0644u,
     "the PWM backlight", "61-neodct-devices.rules"},
    {ND_BL_GPIO_ROOT "/gpio53/value", NULL, NULL, NULL, NULL, "video", 0664u, 0644u,
     "the GPIO backlight", "S90display"},
    {ND_CPUFREQ_DIR "/scaling_max_freq", NULL, NULL, NULL, NULL, "ndusr", 0664u, 0644u,
     "the CPU ceiling", "61-neodct-devices.rules"},
};

static void section_grants(void)
{
    size_t i;

    section("Are the grants applied, or still at the kernel's defaults?");

    for (i = 0u; i < ND_ARRAY_LEN(GRANTS); i++) {
        const grant_row *g = &GRANTS[i];
        char path[ND_PATH_MAX];
        char name[256];
        struct stat st;
        struct group *gr;
        unsigned mode;

        (void)snprintf(name, sizeof name, "%s is group %s", g->what, g->group);

        /* An image built without BR2_ROOTFS_USERS_TABLES has none of these
         * groups and every process on it is root, so the grant is not wrong,
         * it is not applicable. Skipping is the truthful answer and the
         * banner at the top already says which kind of image this is. */
        gr = getgrnam(g->group);
        if (gr == NULL) {
            report(R_SKIP, name, "this image has no group called %s", g->group);
            continue;
        }

        if (g->path != NULL) {
            (void)nd_strlcpy(path, g->path, sizeof path);
        } else {
            char match[ND_PATH_MAX];

            if (!first_matching(g->dir, g->prefix, g->suffix, match, sizeof match)) {
                report(R_SKIP, name, "nothing matching %s/%s* here", g->dir,
                       g->prefix != NULL ? g->prefix : "");
                continue;
            }
            (void)snprintf(path, sizeof path, "%s%s", match,
                           g->tail != NULL ? g->tail : "");
        }

        if (stat(path, &st) != 0) {
            report(R_SKIP, name, "%s is not there", path);
            continue;
        }
        mode = (unsigned)(st.st_mode & 07777);

        if (st.st_gid != gr->gr_gid) {
            if (st.st_gid == 0u && mode == g->deflt)
                report(R_FAIL, name,
                       "%s is still root:root %04o -- the default nothing has "
                       "touched. The %s grant did not land (a lost coldplug race, "
                       "or udevd was not listening yet)",
                       path, mode, g->rule);
            else
                report(R_FAIL, name, "%s is group %ld mode %04o -- %s says %s",
                       path, (long)st.st_gid, mode, g->rule, g->group);
            continue;
        }
        /* The group is right; now, does holding it buy anything? A chgrp that
         * landed and a chmod that did not is a real state -- the rules do them
         * as two operations in one shell -- and it grants nothing at all. */
        if ((mode & 0060u) != 0060u) {
            report(R_FAIL, name,
                   "%s is group %s but mode %04o: the group cannot write it, so the "
                   "grant is decoration", path, g->group, mode);
            continue;
        }
        if (mode != g->mode)
            report(R_PASS, name, "%s (mode %04o rather than %04o, which still grants "
                                 "the group read and write)", path, mode, g->mode);
        else
            report(R_PASS, name, g_verbose ? "%s" : NULL, path);
    }
}

/* ------------------------------------------------------------------ *
 * Section 9: the browser's keys -- the pair, not the half
 * ------------------------------------------------------------------ *
 *
 * See the header. This file used to assert that ndusr_ut cannot open
 * /dev/uinput and stop there, and that single assertion was, for two
 * releases, a green tick beside a browser that could not receive a keypress.
 *
 * The rule is right. What was missing is the other half of the sentence:
 * "...and SOMETHING can still create the bridge, and the browser can still
 * read from it". A confinement that removes a capability from everybody has
 * not confined anything; it has broken a feature.
 *
 * Three questions, in the order they fail:
 *
 *   1. WHO is the browser? Not hardcoded -- asked of nd_proc_app_is_untrusted(),
 *      which is the function the core itself uses to decide. If the policy
 *      moves, this check moves with it instead of certifying the old one.
 *
 *   2. Is there an evdev keypad the browser's user can read? Asked through
 *      nd_media_discover_keypad(), which is not a re-implementation -- it is
 *      literally the function neodct-play calls to find the same device, so a
 *      pass here is a statement about mpv's keys as well as netsurf's.
 *
 *   3. WHILE A BROWSER IS OPEN, is anybody holding /dev/uinput? This is the
 *      one that cannot be fooled by a design change. It does not care which
 *      process creates the bridge -- the app today, the core after the fix --
 *      only that on a phone with a browser on screen the device EXISTS. A
 *      permission can be correct and the bridge still absent; an open
 *      descriptor cannot.
 */

/* Count the processes holding `target` open, and name one of them. Root only:
 * /proc/<pid>/fd is 0500 and owned by the process's user, so an unprivileged
 * caller silently sees only its own and would conclude "nobody". */
static int openers_of(const char *target, pid_t *pid_out, uid_t *uid_out)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char fddir[280];
        DIR *fd;
        struct dirent *f;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        (void)snprintf(fddir, sizeof fddir, "/proc/%s/fd", e->d_name);
        fd = opendir(fddir);
        if (fd == NULL)
            continue;
        while ((f = readdir(fd)) != NULL) {
            char link[ND_PATH_MAX];
            char dest[ND_PATH_MAX];
            ssize_t got;

            if (f->d_name[0] < '0' || f->d_name[0] > '9')
                continue;
            if (nd_snprintf(link, sizeof link, "%s/%s", fddir, f->d_name) != ND_OK)
                continue;
            got = readlink(link, dest, sizeof dest - 1u);
            if (got < 0)
                continue;
            dest[(size_t)got] = '\0';
            if (strcmp(dest, target) != 0)
                continue;
            if (n == 0) {
                if (pid_out != NULL)
                    *pid_out = (pid_t)strtol(e->d_name, NULL, 10);
                if (uid_out != NULL) {
                    uid_t u = 0u;

                    if (proc_ruid(e->d_name, &u))
                        *uid_out = u;
                }
            }
            n++;
            break; /* one process, counted once */
        }
        (void)closedir(fd);
    }
    (void)closedir(d);
    return n;
}

/* Is a browser on screen right now? Either netsurf itself, or the nd-apprun
 * carrying the Browser app -- the launcher counts, because it is the process
 * that has to build the bridge before netsurf is started. */
static int browser_is_open(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int open_now = 0;

    if (d == NULL)
        return 0;
    while (!open_now && (e = readdir(d)) != NULL) {
        char comm[64];
        char appdir[256];

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (!proc_name(e->d_name, comm, sizeof comm))
            continue;
        if (strncmp(comm, "netsurf", 7) == 0) {
            open_now = 1;
            break;
        }
        if (strncmp(comm, "nd-apprun", 9) != 0)
            continue;
        if (proc_argv1(e->d_name, appdir, sizeof appdir) &&
            strcmp(appdir, ND_PATH_APPS_DIR "/Browser") == 0)
            open_now = 1;
    }
    (void)closedir(d);
    return open_now;
}

static void section_browser(const nd_priv_id *usr, const nd_priv_id *ut)
{
    nd_app_entry browser;
    const nd_priv_id *who_id;
    const char *who;
    char keypad[ND_PATH_MAX];
    int openers;
    pid_t holder = 0;
    uid_t holder_uid = 0u;

    section("The browser's keys: a denial and the positive it must leave standing");

    /* Ask the policy rather than assert it. `path` is the only field
     * nd_proc_app_is_untrusted() reads, and it compares whole paths against
     * the same virtual prefix nd_ui_scan_apps() stores, so this means the
     * same thing here as it does inside the core. */
    memset(&browser, 0, sizeof browser);
    (void)nd_strlcpy(browser.path, ND_PATH_APPS_DIR "/Browser", sizeof browser.path);
    (void)nd_strlcpy(browser.name, "Browser", sizeof browser.name);

    if (nd_proc_app_is_untrusted(&browser)) {
        who = ND_PRIV_USER_UT;
        who_id = ut;
    } else {
        who = ND_PRIV_USER;
        who_id = usr;
    }
    report(R_INFO, "the Browser app runs as",
           "%s -- nd_proc_app_is_untrusted() was asked, not assumed", who);

    if (geteuid() != 0u || !who_id->valid) {
        /* Three checks, three skip lines. One line saying "the browser
         * section skipped" would collapse three unproved facts into one, and
         * the recap at the bottom exists precisely so that a person can count
         * what was not established. */
        report(R_SKIP, "somebody can still CREATE the key bridge", "needs root and %s",
               ND_PRIV_USER);
        report(R_SKIP, "the browser's user can read the keypad", "needs root and %s", who);
        report(R_SKIP, "the key bridge exists",
               "reading another process's open descriptors needs root");
        return;
    }

    /* ---- 1. the pair ------------------------------------------------- *
     *
     * Both halves, side by side, so that neither can be read on its own. The
     * denial is asserted in the devices section; what is asserted HERE is
     * that the capability still exists somewhere, which is the half that was
     * missing when the browser went keyless. */
    if (usr->valid)
        expect_allow(usr, ND_PRIV_USER, P_RDWR, "/dev/uinput",
                     "somebody can still CREATE the key bridge (" ND_PRIV_USER
                     " can open /dev/uinput)");

    /* ---- 2. can the browser's user reach an evdev keypad? ------------- */
    if (nd_media_discover_keypad(keypad, sizeof keypad) == ND_OK) {
        char label[ND_PATH_MAX + 64];

        (void)nd_snprintf(label, sizeof label,
                          "the browser's user can read the keypad (%s)", keypad);
        expect_allow(who_id, who, P_READ, keypad, label);
    } else if (first_existing(I2C_NODES, keypad, sizeof keypad)) {
        /* An i2c bus and no evdev keypad is the phone: its sixteen keys are a
         * PCF8575 matrix the core scans itself, and the ONLY evdev node that
         * can ever exist is the uinput bridge. So on this board "no keypad
         * device" is not a quirk of the hardware, it is the bridge being
         * absent -- and netsurf and neodct-play both find their keys by
         * scanning /dev/input and will find nothing.
         *
         * A verdict only while a browser is actually open. The bridge is
         * created when the browser starts and destroyed when it exits, so
         * demanding it on an idle phone would print FAIL on a perfectly good
         * one and teach people to ignore this line. */
        if (browser_is_open())
            report(R_FAIL, "the browser's user can read the keypad",
                   "there is no evdev keypad at all and a browser IS open: the "
                   "uinput bridge does not exist, so netsurf receives nothing and "
                   "neodct-play's C-to-quit does not work either");
        else
            report(R_SKIP, "the browser's user can read the keypad",
                   "no evdev keypad: on this board the only one is the bridge the "
                   "browser creates, and no browser is open. Open it and run this "
                   "again");
    } else {
        report(R_SKIP, "the browser's user can read the keypad",
               "no evdev keypad and no i2c bus -- this is not the phone");
    }

    /* ---- 3. the live descriptor, which no permission can fake --------- */
    openers = openers_of("/dev/uinput", &holder, &holder_uid);
    if (openers > 0) {
        report(R_PASS, "the key bridge exists",
               "/dev/uinput is held by pid %ld running as %s (uid %ld)", (long)holder,
               user_name_of(holder_uid, usr, ut), (long)holder_uid);
    } else if (browser_is_open()) {
        report(R_FAIL, "the key bridge exists",
               "a browser is open and NOTHING holds /dev/uinput. Whichever process is "
               "supposed to create the bridge could not, so every keypress is read by "
               "the core and dropped: the browser cannot be typed into and cannot be "
               "left by any key");
    } else {
        report(R_SKIP, "the key bridge exists",
               "no browser is open, so there is nothing to hold /dev/uinput");
    }
}

/* ------------------------------------------------------------------ *
 * Section 10: can a child still be STARTED -- the capability, not the mode
 * ------------------------------------------------------------------ *
 *
 * Everything above this point is about permission: who owns a node and what
 * its mode bits say. None of it can see the fault that made the phone silent,
 * because that fault is not a permission at all.
 *
 *     setgroups(2) needs CAP_SETGID unconditionally. Not "unless the list is
 *     your own", not "unless the target uid is the uid you already have" --
 *     unconditionally. An nd-core that reached uid 1000 by a plain setuid()
 *     from root holds no capabilities whatever, so asking a child to become
 *     ndusr returns EPERM at nd_priv_become()'s SECOND step, the child does
 *     _exit(122) before execve, and no aplay ever runs. Eight releases of a
 *     phone that lit up for an incoming call and rang in total silence.
 *
 * The only instrument that can see that is a fork, and it has to be a fork
 * made by a process that has REALLY DROPPED -- because as root the drop
 * succeeds and the measurement is of the wrong machine. So this section forks
 * twice: once to become ndusr, and then, from inside that, once more to spawn
 * the player exactly as nd_notify.c does.
 *
 * ============ AND IT DOES NOT USE nd_proc_wait() ============
 *
 * Deliberately, and it is worth a sentence because it looks like an
 * oversight. nd_proc.c's collect() answers waitpid's ECHILD by synthesising
 * `exited = true, exit_status = 0` for a pid this process never forked. An
 * observer built on that would report a clean exit for a child that never
 * existed -- which is the same lie, one layer down, as the one this section
 * exists to catch. waitpid(2) directly, therefore, and the distinction is
 * kept. test/harness/nd_testspawn.h carries the same reasoning for the C
 * suite.
 */

/* What the stage child sends back: one line, "<verdict> <sentence>". A pipe
 * rather than an exit status because the interesting part is a SENTENCE --
 * "never started: exit 122, setgroups() refused" -- and an exit status is one
 * byte with room for a number and nothing else. The number was what the last
 * eight releases had, and it was in nobody's log. */
#define STAGE_MSG_MAX 400

static void stage_say(int fd, char verdict, const char *fmt, ...)
{
    char buf[STAGE_MSG_MAX];
    va_list ap;
    int n;
    ssize_t wrote;

    buf[0] = verdict;
    va_start(ap, fmt);
    n = vsnprintf(buf + 1, sizeof buf - 1u, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    wrote = write(fd, buf, 1u + strnlen(buf + 1, sizeof buf - 1u));
    (void)wrote;
}

/* The exact spec nd_notify.c's spawn_quiet() builds, run from here.
 *
 * `id` was resolved in the grandparent, which is where nd_priv.h says a
 * lookup belongs; this child only ever hands integers to nd_priv_become().
 *
 * argv MUST name something that exits on its own. This waits for it without a
 * deadline, because the thing being measured is a status and a status only
 * exists once the child is gone; a long-lived player would hang the tool. The
 * one caller passes `aplay --version`. */
static void tone_stage_child(const nd_priv_id *id, const char *const *argv, int report_fd)
{
    char exe[ND_PATH_MAX];
    char captured[256];
    nd_proc_spec spec;
    int cap[2];
    pid_t pid;
    int status = 0;
    ssize_t got;
    int drop;

    drop = nd_priv_become(id);
    if (drop != 0) {
        /* Named rather than counted, for the same reason the grandchild's
         * status is: "could not drop" is where the last eight releases
         * stopped, and "setgroups() refused -- it needs CAP_SETGID" is where
         * somebody can act. */
        const char *reason = nd_tone_pre_exec_reason(120 + drop);

        stage_say(report_fd, 'S', "could not become %s: refused at step %d -- %s",
                  ND_PRIV_USER, drop, reason != NULL ? reason : "an unnamed step");
        return;
    }

    /* execvp's rule, which nd_proc_spawn() deliberately does not implement --
     * it takes a path. nd_svc_halt_which() is that rule, written once and
     * documented as general; the halt is only where it happened to be needed
     * first, and it resolves without executing anything. */
    if (!nd_svc_halt_which(argv[0], exe, sizeof exe)) {
        stage_say(report_fd, 'S', "%s is not on $PATH in this image", argv[0]);
        return;
    }

    /* The player's own words, if it has any. spawn_quiet() sends these to
     * /dev/null, which is right for a phone and wrong for a diagnostic: "no
     * soundcards found" and "never started at all" are different problems and
     * on a silent phone they are indistinguishable without this. */
    if (pipe2(cap, O_CLOEXEC) != 0) {
        stage_say(report_fd, 'S', "no pipe for the player's stderr");
        return;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_TONE;
    spec.no_new_privs = true;
    /* THE LINE THE WHOLE SECTION IS ABOUT. nd_notify.c asks for a drop only
     * when it can perform one, and this asks the same question with the same
     * predicate rather than a copy of its reasoning -- so if that policy is
     * ever changed back, this probe changes with it and the phone goes quiet
     * in the report instead of in the owner's hand. */
    if (nd_tone_drop_to_user(geteuid()))
        spec.run_as = *id;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = cap[1];
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = cap[1];
    spec.n_fds = 2u;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        (void)close(cap[0]);
        (void)close(cap[1]);
        stage_say(report_fd, 'F', "fork() failed: %s", strerror(errno));
        return;
    }
    (void)close(cap[1]);

    /* Drained to EOF, not read once.
     *
     * The read end is closed before the waitpid, and a child still writing
     * into a pipe nobody is reading gets SIGPIPE -- so a single short read
     * would turn a chatty player into "killed by signal 13", which is a
     * fabricated failure in the one place this file exists to stop
     * fabricating them. The first 255 bytes are kept for the message and the
     * rest is swallowed; the point is to let the child finish, not to
     * transcribe it. */
    {
        size_t have = 0u;

        for (;;) {
            char discard[256];
            char *dst = (have < sizeof captured - 1u) ? captured + have : discard;
            size_t room = (have < sizeof captured - 1u) ? sizeof captured - 1u - have
                                                        : sizeof discard;

            got = read(cap[0], dst, room);
            if (got <= 0) {
                if (got < 0 && errno == EINTR)
                    continue;
                break;
            }
            if (dst != discard)
                have += (size_t)got;
        }
        captured[have] = '\0';
        /* One line is enough for a report; a newline mid-message would break
         * the one-check-one-line shape the whole file is read in. */
        {
            char *nl = strchr(captured, '\n');

            if (nl != NULL)
                *nl = '\0';
        }
    }
    (void)close(cap[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            stage_say(report_fd, 'F', "waitpid failed: %s", strerror(errno));
            return;
        }
    }

    if (WIFSIGNALED(status)) {
        stage_say(report_fd, 'F', "%s was killed by signal %d before it could finish",
                  argv[0], WTERMSIG(status));
        return;
    }
    if (!WIFEXITED(status)) {
        stage_say(report_fd, 'F', "%s neither exited nor was signalled", argv[0]);
        return;
    }
    {
        int code = WEXITSTATUS(status);
        const char *pre = nd_tone_pre_exec_reason(code);

        if (pre != NULL) {
            stage_say(report_fd, 'F',
                      "%s NEVER STARTED: exit %d -- %s. Every sound the phone makes "
                      "dies here", argv[0], code, pre);
            return;
        }
        if (code != 0) {
            /* It ran. A non-zero status from a player that started is a
             * different fault entirely -- no card, a wrong device name -- and
             * saying so is the difference between a fixable phone and a
             * mystery. */
            stage_say(report_fd, 'F', "%s started and exited %d%s%s", argv[0], code,
                      captured[0] != '\0' ? ": " : "", captured);
            return;
        }
        stage_say(report_fd, 'P', "%s ran to completion as uid %ld", argv[0],
                  (long)geteuid());
    }
}

/* Run one case in a stage child and report whatever sentence it sends back. */
static void probe_from_dropped(const nd_priv_id *id, const char *const *argv,
                               const char *name)
{
    char msg[STAGE_MSG_MAX];
    int pfd[2];
    pid_t pid;
    ssize_t got;
    int status = 0;

    if (pipe(pfd) != 0) {
        report(R_SKIP, name, "no pipe");
        return;
    }
    (void)fflush(stdout);
    pid = fork();
    if (pid < 0) {
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        report(R_SKIP, name, "fork failed");
        return;
    }
    if (pid == 0) {
        (void)close(pfd[0]);
        tone_stage_child(id, argv, pfd[1]);
        _exit(0);
    }
    (void)close(pfd[1]);
    got = read(pfd[0], msg, sizeof msg - 1u);
    msg[(got > 0) ? (size_t)got : 0u] = '\0';
    (void)close(pfd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;

    if (msg[0] == 'P')
        report(R_PASS, name, g_verbose ? "%s" : NULL, msg + 1);
    else if (msg[0] == 'F')
        report(R_FAIL, name, "%s", msg + 1);
    else if (msg[0] == 'S')
        report(R_SKIP, name, "%s", msg + 1);
    else
        report(R_FAIL, name, "the probe said nothing at all -- it died before it "
                             "could report, which is itself the answer");
}

/* The measurement, rather than the product's behaviour: from a process that
 * is really ndusr, ask for a drop that cannot work and print the step the
 * kernel refuses at.
 *
 * Reported as an observation and never as a failure. On every correct phone
 * this is refused, and it SHOULD be -- it is the reason nd_notify.c asks for
 * a drop only when it is root. The value is that the number is written down
 * where a person can see it, in the words that name it, so that the next time
 * something dies at 122 nobody has to work out what 122 means. */
static void measure_the_drop(const nd_priv_id *usr)
{
    pid_t pid;
    int status = 0;
    int rc;

    /* Guarded here as well as at the call site, because nd_priv_become() is a
     * documented NO-OP returning 0 for an invalid id -- so a caller that
     * forgot would be told "this process CAN drop again", which is the most
     * confidently wrong sentence this section could produce. */
    if (!usr->valid) {
        report(R_SKIP, "an already-dropped process cannot drop again",
               "no " ND_PRIV_USER " to become, so the drop was never attempted");
        return;
    }

    (void)fflush(stdout);
    pid = fork();
    if (pid < 0) {
        report(R_SKIP, "an already-dropped process cannot drop again", "fork failed");
        return;
    }
    if (pid == 0) {
        if (nd_priv_become(usr) != 0)
            _exit(100); /* could not even get to ndusr */
        /* Now ndusr, holding no capabilities, asking to become ndusr again --
         * the phone's exact configuration, and the one nothing else can
         * reproduce: on a build host getpwnam("ndusr") misses, and inside
         * QEMU `make test` runs as root. */
        _exit(nd_priv_become(usr));
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;

    if (!WIFEXITED(status)) {
        report(R_SKIP, "an already-dropped process cannot drop again",
               "the probe was killed by a signal");
        return;
    }
    rc = WEXITSTATUS(status);
    if (rc == 100) {
        report(R_SKIP, "an already-dropped process cannot drop again",
               "could not become %s in the first place", ND_PRIV_USER);
    } else if (rc == 0) {
        report(R_INFO, "an already-dropped process CAN drop again",
               "this process kept CAP_SETGID after the drop, which is unusual and "
               "worth knowing: something is granting file capabilities or the "
               "bounding set is not what SECURITY-PLAN.md assumes");
    } else {
        const char *reason = nd_tone_pre_exec_reason(120 + rc);

        report(R_INFO, "an already-dropped process cannot drop again",
               "refused at step %d -- %s. This is CORRECT, and it is why every "
               "spawn that asks for a drop must first ask whether it is root; a "
               "child of one that does not exits %d before execve",
               rc, reason != NULL ? reason : "an unnamed step", 120 + rc);
    }
}

static void section_tone(const nd_priv_id *usr)
{
    static const char *const APLAY_ARGV[] = {"aplay", "--version", NULL};
    struct rlimit rl;

    section("Can the phone still start a player (the capability, not the mode bits)");

    if (geteuid() != 0u || !usr->valid) {
        const char *why = (geteuid() != 0u)
                              ? "nd-selftest must start as root to become " ND_PRIV_USER
                              : "no " ND_PRIV_USER " in this image, so there is nobody "
                                "to become";

        /* Named one by one for the recap, as in the browser section. */
        report(R_SKIP, "an already-dropped process cannot drop again", "%s", why);
        report(R_SKIP, "a tone player still reaches execve", "%s", why);
        report(R_SKIP, "the DTMF tones are readable by the player's user", "%s", why);
        report(R_SKIP, "the message tone is readable by the player's user", "%s", why);
        return;
    }

    measure_the_drop(usr);
    probe_from_dropped(usr, APLAY_ARGV, "a tone player still reaches execve");

    /* The tones themselves. A player that starts and cannot open the file is
     * as silent as one that never started, and the DTMF directory is on the
     * read-only squashfs where a wrong mode survives every reboot. */
    expect_allow(usr, ND_PRIV_USER, P_READ, ND_PATH_DTMF_DIR "/5.wav",
                 "the DTMF tones are readable by the player's user");
    expect_allow(usr, ND_PRIV_USER, P_READ, ND_PATH_SMS_TONE,
                 "the message tone is readable by the player's user");

    /* Recorded because it is the whole of a decision somebody will revisit.
     * The preview and the ringer used to be spawned through `nice -n -10`,
     * which on a root phone raised their priority and on an ndusr one made
     * nice(1) refuse and take the player down with it -- a stutter fixed at
     * the cost of the sound. The nice is gone; if anyone wants the priority
     * back, RLIMIT_NICE is the number that says whether they can have it
     * without a capability. 0 means no negative nice at all. */
    if (getrlimit(RLIMIT_NICE, &rl) != 0)
        report(R_INFO, "RLIMIT_NICE headroom", "unreadable: %s", strerror(errno));
    else if (rl.rlim_cur == RLIM_INFINITY)
        report(R_INFO, "RLIMIT_NICE headroom",
               "unlimited -- any negative nice may be asked for here");
    else
        report(R_INFO, "RLIMIT_NICE headroom",
               "soft %lu -- the lowest nice value an unprivileged process may ask "
               "for here is %ld",
               (unsigned long)rl.rlim_cur, 20L - (long)rl.rlim_cur);
}

/* ------------------------------------------------------------------ *
 * Section 11: can the phone still be switched off -- ASKED, NEVER DONE
 * ------------------------------------------------------------------ *
 *
 * ============ READ THIS BEFORE ADDING ANYTHING TO THIS SECTION ============
 *
 * NOTHING HERE MAY PERFORM A HALT. No spawn, no reboot(2), no candidate
 * executed, not even a resolved one. On 2026-08-31 and again on 2026-09-04 a
 * process in this tree switched a developer's workstation off in the middle
 * of a build, and this file runs AS ROOT by design -- it is the one program
 * here for which nothing else would stand in the way.
 *
 * So the whole section is pure predicates and stat(). nd_svc_halt_allowed_for()
 * and nd_svc_halt_delegate_allowed_for() are the policy TABLES with every
 * input as a parameter; nd_svc_halt_which() is execvp's PATH search with an
 * access(X_OK) and no exec. main() additionally calls nd_svc_halt_disarm()
 * before any of this runs, so even a future line that reached for the real
 * nd_svc_poweroff() would be refused by rule 2 of the table itself. Three
 * independent reasons, because one is what the workstation had.
 *
 * ============ AND WHY THE QUESTION NEEDED ASKING AT ALL ============
 *
 * The predicate answers "may THIS process fork a poweroff". For the whole of
 * 0.5.x the core asked it as though it answered "may this phone be halted",
 * and since 0.5.0b the core is ndusr -- so the answer was no, on every phone,
 * for every press of Power off, Restart and Restart into recovery, and the
 * root broker that could have done it was never asked. The panel said "Power
 * off failed." and the log blamed the image for having no poweroff binary in
 * it.
 *
 * The configuration below is the PHONE's, spelled out as literals rather than
 * read from this process: nd-selftest is not the core, has no broker and is
 * root, so asking about itself would answer a fourth question nobody has.
 */

static void section_halt(const nd_priv_id *usr, const nd_priv_id *ut)
{
    /* Rule 3 of both tables. On the phone this is never set; under `make test`
     * it always is, and a run in a scratch root must not claim to have
     * measured the phone. */
    const bool in_test_root = getenv("NEODCT_ROOT") != NULL;
    DIR *d;
    struct dirent *e;
    int core_root = 0;
    int core_dropped = 0;
    char exe[ND_PATH_MAX];

    section("Can the phone reach a halt (asked as a policy; nothing is performed)");

    if (in_test_root)
        report(R_INFO, "NEODCT_ROOT is set",
               "this is a scratch root, so both tables refuse by rule 3 and the "
               "verdicts below are about the harness rather than about a phone");

    /* THE ROW THAT COST EVERY PHONE ITS POWER BUTTON. A core that is ndusr and
     * has a root broker must be allowed to DELEGATE. */
    if (nd_svc_halt_delegate_allowed_for(false, false, in_test_root, true))
        report(R_PASS, "the core may hand a halt to the broker", NULL);
    else
        report(in_test_root ? R_SKIP : R_FAIL, "the core may hand a halt to the broker",
               "refused with a broker present -- Power off, Restart and Restart into "
               "recovery will all say \"failed\" and do nothing");

    /* The other table, reported and NOT judged. A false here is correct for a
     * dropped core and was, for eight releases, mistaken for the answer to the
     * question above. Printing both side by side is the cheapest way to stop
     * anybody making that mistake twice. */
    if (nd_svc_halt_allowed_for(false, false, in_test_root, false))
        report(R_INFO, "an unprivileged core may spawn a halt ITSELF",
               "yes -- unexpected; rule 4 says only root may");
    else
        report(R_INFO, "an unprivileged core may NOT spawn a halt itself",
               "correct, and NOT the same question as the one above: the spawn "
               "belongs to the broker, which is root and passes rule 4 honestly");

    /* Is there anything to spawn? Resolved, never run. A phone whose image
     * carries no poweroff at all fails at the last step of five, after the
     * reply has gone back to the app, and the app is then telling the truth
     * for a reason nobody can see from the panel. */
    if (nd_svc_halt_which("poweroff", exe, sizeof exe))
        report(R_PASS, "there is a poweroff binary on $PATH", "%s", exe);
    else
        report(R_FAIL, "there is a poweroff binary on $PATH",
               "nothing called poweroff is executable anywhere on $PATH, so the "
               "broker has nothing to exec even once it is asked");
    if (nd_svc_halt_which("reboot", exe, sizeof exe))
        report(R_PASS, "there is a reboot binary on $PATH", "%s", exe);
    else
        report(R_FAIL, "there is a reboot binary on $PATH",
               "an update that installs will never be able to finish itself");

    /* ============ AND IS THE BROKER ACTUALLY THERE ============
     *
     * The delegation above is a rule; this is the fact. nd_broker_start()
     * forks before the drop and does not rename itself, so on a healthy phone
     * /proc carries TWO nd-core processes: one still root -- that is the
     * broker, the only thing left holding CAP_SYS_BOOT -- and one as ndusr,
     * which is the UI. Either one alone is a specific, nameable failure, and
     * neither can be seen from a mode bit. */
    d = opendir("/proc");
    if (d == NULL) {
        report(R_SKIP, "there is a root broker behind the unprivileged core",
               "cannot read /proc");
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char comm[64];
        uid_t uid = 0u;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (!proc_name(e->d_name, comm, sizeof comm))
            continue;
        if (strncmp(comm, "nd-core", 7) != 0)
            continue;
        if (!proc_ruid(e->d_name, &uid))
            continue;
        if (uid == 0u)
            core_root++;
        else
            core_dropped++;
    }
    (void)closedir(d);

    if (core_root == 0 && core_dropped == 0)
        report(R_SKIP, "there is a root broker behind the unprivileged core",
               "nd-core is not running; start the phone and run this again");
    else if (core_root > 0 && core_dropped > 0)
        report(R_PASS, "there is a root broker behind the unprivileged core",
               "%d root, %d unprivileged", core_root, core_dropped);
    else if (core_dropped > 0)
        report(R_FAIL, "there is a root broker behind the unprivileged core",
               "the core dropped to %s and there is no root nd-core left: nothing on "
               "this phone can halt it, format a card or spawn an app",
               user_name_of(usr->valid ? usr->uid : 1000u, usr, ut));
    else
        report(R_FAIL, "the core dropped privilege at all",
               "every nd-core is still root -- nd_main.c's step 4b did not happen, "
               "and the whole confinement this file measures is not in effect");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

/* Every section name, in one place: usage() prints it, wanted() validates
 * against it, and main() runs them in this order. Three copies of the list is
 * how the last one grew a section nobody could ask for by name. */
static const char *const SECTIONS[] = {"users",  "layout", "devices", "grants",
                                       "browser", "tone",  "boundary", "mounts",
                                       "kernel", "halt",   "processes", NULL};

static void usage(void)
{
    size_t i;

    (void)fprintf(stderr,
                  "nd-selftest [-v] [-q] [section ...]\n"
                  "\n"
                  "  Checks that the confinement SECURITY-PLAN.md describes is really\n"
                  "  in effect on this image, by becoming each user and trying.\n"
                  "  Start it as root: it drops privilege in children, and cannot\n"
                  "  test a boundary it is already on the wrong side of.\n"
                  "\n"
                  "  It never performs a halt, a reboot or a format. The halt section\n"
                  "  asks the policy and stops there; see the comment above it.\n"
                  "\n"
                  "  sections:");
    for (i = 0u; SECTIONS[i] != NULL; i++)
        (void)fprintf(stderr, " %s", SECTIONS[i]);
    (void)fprintf(stderr,
                  "\n"
                  "  -v        print the detail of passing checks too\n"
                  "  -q        only failures, the skip list and the summary\n"
                  "\n"
                  "  exit 0    nothing failed\n"
                  "  exit 1    at least one check failed\n"
                  "  exit 2    bad arguments\n"
                  "  exit 4    nothing failed and nothing PASSED either: the run\n"
                  "            proved nothing, which is not the same as being green\n");
}

static int known_section(const char *name)
{
    size_t i;

    for (i = 0u; SECTIONS[i] != NULL; i++) {
        if (strcmp(SECTIONS[i], name) == 0)
            return 1;
    }
    return 0;
}

static int wanted(int argc, char **argv, const char *name)
{
    int i;
    int any = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        any = 1;
        if (strcmp(argv[i], name) == 0)
            return 1;
    }
    return !any; /* no sections named: run them all */
}

int main(int argc, char **argv)
{
    nd_priv_id usr, ut;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 2;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (!known_section(argv[i])) {
            (void)fprintf(stderr, "nd-selftest: no section called '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    /* ============ BELT, BRACES AND A THIRD THING ============
     *
     * This program runs as root by design and it links the library that knows
     * how to switch the phone off. Nothing below asks for a halt -- the halt
     * section is pure predicates, and its comment says why at length -- but
     * "nothing below asks for one" is a property of today's code, and on
     * 2026-08-31 and again on 2026-09-04 exactly that property stopped being
     * true and took a developer's workstation down with it.
     *
     * So the latch goes on before anything runs. nd_svc_halt_disarm() is
     * sticky and is rule 2 of the table itself, which means a future line
     * that reached for the real nd_svc_poweroff() would be refused by the
     * library rather than by anybody's care. It costs one call and it removes
     * a whole category of accident from a program whose entire job is to be
     * run on a phone somebody is holding. */
    nd_svc_halt_disarm();

    /* Looked up ONCE, in the parent, because nd_priv_lookup() reads
     * /etc/passwd and /etc/group and is not async-signal-safe -- nd_priv.h is
     * explicit that the lookup belongs to the parent and only the become()
     * belongs to the child. Every probe below forks with these already in
     * hand. */
    (void)nd_priv_lookup(ND_PRIV_USER, &usr);
    (void)nd_priv_lookup(ND_PRIV_USER_UT, &ut);

    printf("nd-selftest: the confinement, as this kernel sees it\n");
    /* Said once, at the top, because otherwise the failures below read as
     * defects in the image rather than as the absence of one. A developer's
     * checkout has no /NeoDCT/System, no users table and a writable root, and
     * every check here is correct to fail on it -- but only the banner
     * explains why. */
    if (access(ND_PATH_SYSTEM, F_OK) != 0)
        printf("  NOTE: there is no %s here, so this is not a built image.\n"
               "  The failures below are that absence, not a broken phone.\n",
               ND_PATH_SYSTEM);
    if (geteuid() != 0u)
        printf("  (running as uid %ld -- the probes that need to drop will skip)\n",
               (long)geteuid());

    if (wanted(argc, argv, "users"))
        section_users(&usr, &ut);
    if (wanted(argc, argv, "layout"))
        section_layout(&usr, &ut);
    if (wanted(argc, argv, "devices"))
        section_devices(&usr, &ut);
    if (wanted(argc, argv, "grants"))
        section_grants();
    if (wanted(argc, argv, "browser"))
        section_browser(&usr, &ut);
    if (wanted(argc, argv, "tone"))
        section_tone(&usr);
    if (wanted(argc, argv, "boundary"))
        section_boundary(&ut);
    if (wanted(argc, argv, "mounts"))
        section_mounts();
    if (wanted(argc, argv, "kernel"))
        section_kernel();
    if (wanted(argc, argv, "halt"))
        section_halt(&usr, &ut);
    if (wanted(argc, argv, "processes"))
        section_processes(&usr, &ut);

    /* The skips again, together, because scrolled-off is the same as unsaid.
     * See the comment on g_skipped. */
    if (g_skipped_n > 0) {
        int i2;

        printf("\nDID NOT RUN -- each of these proved nothing:\n");
        for (i2 = 0; i2 < g_skipped_n; i2++)
            printf("  SKIP  %s\n", g_skipped[i2]);
        if (g_skip > g_skipped_n)
            printf("  ... and %d more\n", g_skip - g_skipped_n);
    }

    printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    if (g_skip > 0 && g_fail == 0)
        printf("A skip is not a pass: it means the check did not run.\n");

    /* ============ AND A RUN THAT PROVED NOTHING IS NOT A GREEN RUN ============
     *
     * The original contract was "exit 0 when every check that ran either
     * passed or was skipped", and on a phone that is a trap: run this without
     * root and every probe that matters skips, the summary reads 0 failed,
     * and a boot script appending it to a log records success for a run that
     * established nothing at all. That is the same shape as every bug this
     * tool exists to find -- a silent degradation reported as a normal
     * outcome -- and it would be indefensible to leave it in the instrument.
     *
     * 4 rather than 3, because 3 is what nd_testguard.c's bare-run refusal
     * uses and the two must not be confused in a log. */
    if (g_fail == 0 && g_pass == 0) {
        printf("NOTHING WAS PROVED: no check passed and none failed. This run is not "
               "evidence of anything.\n");
        return 4;
    }
    return g_fail > 0 ? 1 : 0;
}
