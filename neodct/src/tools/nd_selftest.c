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
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_storage.h"
#include "nd_priv.h"
#include "nd_proc.h"

/* ------------------------------------------------------------------ *
 * Results
 * ------------------------------------------------------------------ */

typedef enum { R_PASS, R_FAIL, R_SKIP, R_INFO } result;

static int g_pass, g_fail, g_skip;
static int g_verbose;

static void report(result r, const char *name, const char *fmt, ...)
{
    va_list ap;
    const char *tag = r == R_PASS ? "PASS"
                    : r == R_FAIL ? "FAIL"
                    : r == R_SKIP ? "SKIP"
                                  : "----";

    if (r == R_PASS) {
        g_pass++;
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
        report(R_SKIP, name, "%s is not on this build", path);
    else if (a == A_NODROP)
        report(R_SKIP, name, "could not become %s", who);
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
    else if (a == A_NODROP)
        report(R_SKIP, name, "could not become %s", who);
    else
        report(R_FAIL, name, "%s: %s (expected a clean denial)", who, answer_name(a));
}

/* ------------------------------------------------------------------ *
 * Section 1: the users and groups exist, and are what the table says
 * ------------------------------------------------------------------ */

#include <grp.h>
#include <pwd.h>

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

static const char *const I2C_NODES[] = {"/dev/i2c-3", "/dev/i2c-2", "/dev/i2c-1",
                                        "/dev/i2c-0", NULL};
static const char *const MODEM_NODES[] = {"/dev/ttyUSB2", "/dev/ttyUSB1", "/dev/ttyUSB0",
                                          NULL};
static const char *const INPUT_NODES[] = {"/dev/input/event0", "/dev/input/event1", NULL};

static void section_devices(const nd_priv_id *usr, const nd_priv_id *ut)
{
    char i2c[64], modem[64], evdev[64];
    int have_i2c = first_existing(I2C_NODES, i2c, sizeof i2c);
    int have_modem = first_existing(MODEM_NODES, modem, sizeof modem);
    int have_evdev = first_existing(INPUT_NODES, evdev, sizeof evdev);

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
        expect_allow(usr, ND_PRIV_USER, P_READ, "/dev/rtc0",
                     ND_PRIV_USER " can open /dev/rtc0");
        if (have_evdev)
            expect_allow(usr, ND_PRIV_USER, P_READ, evdev, ND_PRIV_USER " can read the keypad");
        if (have_i2c)
            expect_allow(usr, ND_PRIV_USER, P_RDWR, i2c,
                         ND_PRIV_USER " can open the i2c bus");
        else
            report(R_SKIP, ND_PRIV_USER " can open the i2c bus",
                   "no /dev/i2c-* on this build (expected on QEMU; NOT expected on the phone)");
        if (have_modem)
            expect_allow(usr, ND_PRIV_USER, P_RDWR, modem, ND_PRIV_USER " can open the modem");
        else
            report(R_SKIP, ND_PRIV_USER " can open the modem", "no /dev/ttyUSB* on this build");
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
    expect_deny(ut, ND_PRIV_USER_UT, P_RDWR, "/dev/uinput",
                ND_PRIV_USER_UT " CANNOT inject keys through /dev/uinput");
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

    if (mount_options(ND_SD_UNTRUSTED_MOUNT, opts, sizeof opts)) {
        /* This one is the point of the split: downloads and MMS attachments
         * land here, and nothing that lands here may be executed. */
        check_option(ND_SD_UNTRUSTED_MOUNT, opts, "noexec", 1);
        check_option(ND_SD_UNTRUSTED_MOUNT, opts, "nosuid", 1);
        check_option(ND_SD_UNTRUSTED_MOUNT, opts, "nodev", 1);
    } else {
        report(R_SKIP, ND_SD_UNTRUSTED_MOUNT " is mounted",
               "no arrival partition -- a card formatted elsewhere, or no card");
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
 * main
 * ------------------------------------------------------------------ */

static void usage(void)
{
    (void)fprintf(stderr,
                  "nd-selftest [-v] [section ...]\n"
                  "\n"
                  "  Checks that the confinement SECURITY-PLAN.md describes is really\n"
                  "  in effect on this image, by becoming each user and trying.\n"
                  "  Start it as root: it drops privilege in children, and cannot\n"
                  "  test a boundary it is already on the wrong side of.\n"
                  "\n"
                  "  sections: users layout devices boundary mounts kernel processes\n"
                  "  -v        print the detail of passing checks too\n"
                  "\n"
                  "  exit 0    nothing failed\n"
                  "  exit 1    at least one check failed\n"
                  "  exit 2    bad arguments\n");
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
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 2;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (strcmp(argv[i], "users") != 0 && strcmp(argv[i], "layout") != 0 &&
                   strcmp(argv[i], "devices") != 0 && strcmp(argv[i], "boundary") != 0 &&
                   strcmp(argv[i], "mounts") != 0 && strcmp(argv[i], "kernel") != 0 &&
                   strcmp(argv[i], "processes") != 0) {
            (void)fprintf(stderr, "nd-selftest: no section called '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

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
    if (wanted(argc, argv, "boundary"))
        section_boundary(&ut);
    if (wanted(argc, argv, "mounts"))
        section_mounts();
    if (wanted(argc, argv, "kernel"))
        section_kernel();
    if (wanted(argc, argv, "processes"))
        section_processes(&usr, &ut);

    printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    if (g_skip > 0 && g_fail == 0)
        printf("A skip is not a pass: it means the check did not run.\n");
    return g_fail > 0 ? 1 : 0;
}
