/* test_priv.c -- the order of a privilege drop, which is the whole of it.
 *
 * nd_priv_become() is four syscalls and there is exactly one order that
 * works. Getting it wrong does not fail; it produces a process that looks
 * confined and is not. So the tests here are about the SHAPE of the drop as
 * much as its result:
 *
 *   - the name is resolved in the parent, never after a fork, because
 *     getpwnam allocates and opens a file and nd_proc.h forbids both there;
 *   - setgroups comes before setgid comes before setuid, because the first
 *     two need the privilege the third gives away;
 *   - a missing user is a no-op and not an error, because an image built
 *     without BR2_ROOTFS_USERS_TABLES has to boot;
 *   - every result is read back, because a partial drop is the failure that
 *     matters.
 *
 * Most of it can be checked without being root. The one thing that cannot --
 * that the drop actually happens -- is checked for real when the suite runs
 * as root, which it does in CI and in a container, and skipped with a line
 * saying so otherwise. A test that silently proves nothing is worse than one
 * that says it could not.
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_types.h"

/* NGROUPS_MAX is not in any header musl exposes here, and its real value
 * (65536 on Linux) is not a stack allocation to make anyway. 64 is far more
 * than root has and the count is checked against it. */
#define ND_TEST_MAX_GROUPS 64

static int g_fail;
static int g_checks;
static int g_skipped;

#define CHECK(cond, what)                                                          \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            (void)fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
            g_fail++;                                                              \
        }                                                                          \
    } while (0)

#define SKIP(what)                                            \
    do {                                                      \
        g_skipped++;                                          \
        (void)fprintf(stderr, "  SKIP %s\n", (what));         \
    } while (0)

/* A user this machine certainly has, so the lookup can be exercised without
 * needing the phone's own users to exist on a build host. */
static const char *some_real_user(uid_t *uid_out)
{
    static const char *const candidates[] = {"nobody", "daemon", "bin", "root"};
    size_t i;

    for (i = 0u; i < sizeof candidates / sizeof candidates[0]; i++) {
        struct passwd *pw = getpwnam(candidates[i]);

        if (pw != NULL) {
            *uid_out = pw->pw_uid;
            return candidates[i];
        }
    }
    return NULL;
}

/* ---- lookup ---------------------------------------------------------- */

static void test_lookup(void)
{
    nd_priv_id id;
    uid_t expect = 0;
    const char *user = some_real_user(&expect);

    CHECK(!nd_priv_lookup(NULL, &id), "a NULL name is not a user");
    CHECK(!id.valid, "and leaves the result invalid");
    CHECK(!nd_priv_lookup("", &id), "nor is the empty name");
    CHECK(!nd_priv_lookup("nd-there-is-no-such-user", &id),
          "a user that does not exist is false, not a crash");
    CHECK(!id.valid, "and invalid, which is what makes become() a no-op");
    CHECK(!nd_priv_lookup("root", NULL), "a NULL output is refused");

    if (user == NULL) {
        SKIP("no ordinary user on this machine to look up");
        return;
    }

    CHECK(nd_priv_lookup(user, &id), "an existing user resolves");
    CHECK(id.valid, "and is marked valid");
    CHECK(id.uid == expect, "to the uid /etc/passwd gives");

    /* setgroups() replaces the supplementary list wholesale, so a child
     * whose own primary group is missing from it cannot read its own
     * files. */
    {
        size_t i;
        bool have_primary = false;

        for (i = 0u; i < id.n_groups; i++) {
            if (id.groups[i] == id.gid)
                have_primary = true;
        }
        CHECK(id.n_groups > 0u, "the group list is not empty");
        CHECK(have_primary, "and always contains the user's own primary group");
        CHECK(id.n_groups <= (size_t)ND_PRIV_MAX_GROUPS, "and is bounded");
    }
}

/* ---- the no-op path, which is what an image without the users does ---- */

static void test_no_user_is_not_a_failure(void)
{
    nd_priv_id id;
    uid_t before = getuid();

    (void)memset(&id, 0, sizeof id);
    CHECK(nd_priv_become(&id) == 0, "an invalid id is a no-op, not a failure");
    CHECK(getuid() == before, "and changes nothing");
    CHECK(nd_priv_become(NULL) == 0, "and so is NULL");
    CHECK(getuid() == before, "still nothing");
}

/* ---- no_new_privs ----------------------------------------------------- */

static void test_no_new_privs(void)
{
    pid_t pid = fork();
    int status = 0;

    /* In a child: it is a one-way door for the whole process tree, and
     * setting it here would silently change every test that runs after. */
    if (pid == 0) {
        if (nd_priv_no_new_privs() != 0)
            _exit(1);
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
            _exit(2);
        _exit(0);
    }
    if (pid < 0) {
        SKIP("fork failed");
        return;
    }
    (void)waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "no_new_privs is set and reads back set");
}

/* ---- the drop itself, which needs privilege to exercise --------------- */

static void test_the_drop(void)
{
    nd_priv_id id;
    uid_t target = 0;
    const char *user;
    pid_t pid;
    int status = 0;

    if (geteuid() != 0) {
        SKIP("not root: cannot exercise an actual drop");
        return;
    }
    user = some_real_user(&target);
    if (user == NULL || target == 0) {
        /* root is the only account here, so there is nothing to become. */
        SKIP("no unprivileged account on this machine");
        return;
    }
    if (!nd_priv_lookup(user, &id)) {
        CHECK(false, "the user that was just found does not resolve");
        return;
    }

    pid = fork();
    if (pid == 0) {
        gid_t groups[ND_TEST_MAX_GROUPS];
        int n;

        if (nd_priv_become(&id) != 0)
            _exit(10);
        if (getuid() != id.uid || geteuid() != id.uid)
            _exit(11);
        if (getgid() != id.gid || getegid() != id.gid)
            _exit(12);
        /* The one that would be silently wrong if setgroups came last: root's
         * supplementary groups must be gone, not merely added to. */
        n = getgroups((int)(sizeof groups / sizeof groups[0]), groups);
        if (n < 0)
            _exit(13);
        {
            int i;

            for (i = 0; i < n; i++) {
                bool wanted = false;
                size_t j;

                for (j = 0u; j < id.n_groups; j++) {
                    if (groups[i] == id.groups[j])
                        wanted = true;
                }
                if (!wanted)
                    _exit(14); /* a group nobody asked for survived the drop */
            }
        }
        /* And that it is a one-way door. */
        if (setuid(0) == 0)
            _exit(15);
        _exit(0);
    }
    if (pid < 0) {
        SKIP("fork failed");
        return;
    }
    (void)waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status), "the child exited rather than dying");
    if (WIFEXITED(status)) {
        char why[96];

        (void)nd_snprintf(why, sizeof why,
                          "the drop landed on every id (child said %d)",
                          WEXITSTATUS(status));
        CHECK(WEXITSTATUS(status) == 0, why);
    }
}

/* ---- through nd_proc_spawn, which is the one call site --------------- */

static void test_spawn_runs_the_child_as_the_user(void)
{
    nd_proc_spec spec;
    nd_proc_status st;
    const char *argv[3];
    uid_t target = 0;
    const char *user;
    pid_t pid = -1;
    char expect[32];
    int fds[2];
    char buf[64];
    ssize_t n;

    if (geteuid() != 0) {
        SKIP("not root: cannot exercise a drop through nd_proc_spawn");
        return;
    }
    user = some_real_user(&target);
    if (user == NULL || target == 0) {
        SKIP("no unprivileged account on this machine");
        return;
    }
    if (pipe(fds) != 0) {
        SKIP("pipe failed");
        return;
    }

    /* `id -u` rather than anything of ours: what is being tested is that the
     * KERNEL agrees about the uid after the exec, and an external program's
     * answer is the kernel's. */
    argv[0] = "id";
    argv[1] = "-u";
    argv[2] = NULL;

    (void)memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = fds[1];
    spec.n_fds = 1u;
    CHECK(nd_priv_lookup(user, &spec.run_as), "the target user resolves");

    if (nd_proc_spawn("/usr/bin/id", &spec, &pid) != ND_OK &&
        nd_proc_spawn("/bin/id", &spec, &pid) != ND_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        SKIP("no id(1) on this machine");
        return;
    }
    (void)close(fds[1]);
    n = read(fds[0], buf, sizeof buf - 1u);
    (void)close(fds[0]);
    (void)nd_proc_wait(pid, 5.0, &st);

    CHECK(n > 0, "the child said something");
    if (n > 0) {
        char why[96];

        buf[n] = '\0';
        (void)nd_snprintf(expect, sizeof expect, "%lu", (unsigned long)target);
        (void)nd_snprintf(why, sizeof why, "the exec'd child is uid %s, not %s",
                          expect, buf);
        CHECK(strncmp(buf, expect, strlen(expect)) == 0, why);
    }
}

/* A spec that says nothing about users must behave exactly as it always
 * did -- this is the path every other spawn in the tree takes. */
static void test_a_spec_that_asks_for_nothing_is_unchanged(void)
{
    nd_proc_spec spec;
    nd_proc_status st;
    const char *argv[3];
    pid_t pid = -1;
    int fds[2];
    char buf[64];
    ssize_t n;
    char expect[32];

    if (pipe(fds) != 0) {
        SKIP("pipe failed");
        return;
    }
    argv[0] = "id";
    argv[1] = "-u";
    argv[2] = NULL;
    (void)memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = fds[1];
    spec.n_fds = 1u;

    if (nd_proc_spawn("/usr/bin/id", &spec, &pid) != ND_OK &&
        nd_proc_spawn("/bin/id", &spec, &pid) != ND_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        SKIP("no id(1) on this machine");
        return;
    }
    (void)close(fds[1]);
    n = read(fds[0], buf, sizeof buf - 1u);
    (void)close(fds[0]);
    (void)nd_proc_wait(pid, 5.0, &st);

    CHECK(n > 0, "the child said something");
    if (n > 0) {
        buf[n] = '\0';
        (void)nd_snprintf(expect, sizeof expect, "%lu", (unsigned long)getuid());
        CHECK(strncmp(buf, expect, strlen(expect)) == 0,
              "a spec with no run_as leaves the child as the caller");
    }
}

int main(void)
{
    test_lookup();
    test_no_user_is_not_a_failure();
    test_no_new_privs();
    test_the_drop();
    test_spawn_runs_the_child_as_the_user();
    test_a_spec_that_asks_for_nothing_is_unchanged();

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_priv: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_priv: %d checks passed, %d skipped\n", g_checks, g_skipped);
    return 0;
}
