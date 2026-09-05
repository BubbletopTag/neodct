/* test_broker.c -- the process that is still root, and what it refuses.
 *
 * The broker exists so nd-core does not have to be root. That makes nd-core an
 * UNTRUSTED SENDER on this socket: the interesting tests here are not "does a
 * spawn work" but "what happens when the request is one nd-core should not be
 * able to make".
 */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_broker.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_proc.h"

#include "platform_test.h"

/* /bin/true is the smallest thing that can be spawned and reaped. Falling back
 * to /usr/bin/true covers a host that puts it there; if neither exists the
 * round-trip tests skip rather than fail, because they would be testing the
 * host's layout instead of the broker. */
static const char *true_path(void)
{
    static const char *const CANDIDATES[] = {"/bin/true", "/usr/bin/true", NULL};
    size_t i;

    for (i = 0u; CANDIDATES[i] != NULL; i++) {
        if (access(CANDIDATES[i], X_OK) == 0)
            return CANDIDATES[i];
    }
    return NULL;
}

static void spec_for(nd_proc_spec *spec, const char *const *argv)
{
    memset(spec, 0, sizeof *spec);
    spec->argv = argv;
    spec->envp = NULL;
    spec->owner = ND_OWNER_APP;
    spec->n_fds = 0u;
}

/* The whole point, in one test: a request to run something as root is refused,
 * whatever else is true of it.
 *
 * nd-core is unprivileged now and may be compromised. If "spawn as root" were
 * a request it could make, the broker would hand back the entire privilege
 * boundary to whoever took the UI -- and it would do it through the one
 * channel that exists specifically to keep them apart. ND_BROKER_USERS is the
 * list, and root is not on it. */
static void test_the_broker_refuses_to_spawn_as_root(void)
{
    nd_broker *b;
    const char *path = true_path();
    const char *argv[2];
    nd_proc_spec spec;
    pid_t pid = -1;
    nd_err rc;

    if (path == NULL) {
        printf("test_broker: no /bin/true; skipping\n");
        return;
    }
    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL)
        return;

    argv[0] = path;
    argv[1] = NULL;
    spec_for(&spec, argv);

    rc = nd_broker_spawn(b, path, &spec, "root", &pid);
    CHECK_INT((int)rc, (int)ND_ERR_PERM);
    CHECK_INT((int)pid, 0); /* and nothing was started */

    /* Nor by any other name that is not on the list. "ndusr " with a trailing
     * space, "0", an empty-looking name that is not empty -- all refused,
     * because the check is an exact match against the list rather than an
     * attempt to recognise the bad ones. */
    pid = -1;
    rc = nd_broker_spawn(b, path, &spec, "ndusr ", &pid);
    CHECK_INT((int)rc, (int)ND_ERR_PERM);
    rc = nd_broker_spawn(b, path, &spec, "0", &pid);
    CHECK_INT((int)rc, (int)ND_ERR_PERM);
    rc = nd_broker_spawn(b, path, &spec, "daemon", &pid);
    CHECK_INT((int)rc, (int)ND_ERR_PERM);

    nd_broker_stop(b);
}

/* ============ THE ONE THE FIRST VERSION OF THIS FILE MISSED ============
 *
 * test_the_broker_refuses_to_spawn_as_root() above asserts that the NAME "root"
 * is refused, and it passed from the day it was written. It was also nearly
 * worthless, because not naming a user at all means "do not drop" -- and a
 * child that is never dropped stays root. nd_broker_spawn(b, "/bin/sh", &spec,
 * NULL, &pid) was a root shell, reachable by anything that could reach the
 * socket, which is the entire population the socket exists to keep out.
 *
 * The lesson is in the test, not just the fix: a boundary that only stops the
 * spelling you thought of is not a boundary. */
static void test_a_nameless_user_is_not_a_way_to_stay_root(void)
{
    nd_broker *b;
    const char *path = true_path();
    const char *argv[2];
    nd_proc_spec spec;
    pid_t pid = -1;

    if (path == NULL)
        return;
    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL)
        return;

    argv[0] = path;
    argv[1] = NULL;
    spec_for(&spec, argv);

    /* /bin/true is not on ND_BROKER_ROOT_EXEC, so "run it undropped" is
     * refused however the request is phrased. */
    CHECK_INT((int)nd_broker_spawn(b, path, &spec, NULL, &pid), (int)ND_ERR_PERM);
    CHECK_INT((int)pid, 0);
    CHECK_INT((int)nd_broker_spawn(b, path, &spec, "", &pid), (int)ND_ERR_PERM);

    /* Naming a user that IS allowed still works -- the refusal is about
     * staying root, not about this path. */
    pid = -1;
    CHECK_INT((int)nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid), (int)ND_OK);
    if (pid > 0) {
        nd_proc_status st;

        memset(&st, 0, sizeof st);
        while (nd_broker_wait(b, pid, 1.0, &st) == ND_ERR_TIMEOUT)
            ;
    }

    nd_broker_stop(b);
}

/* nd-apprun IS on the list, because engineering apps run as root. That makes it
 * the weak link: one allowed path would otherwise mean every app on the phone
 * could be asked for as root, since which app it runs is argv[1]. */
static void test_apprun_as_root_is_only_for_engineering_apps(void)
{
    nd_broker *b;
    const char *argv[4];
    nd_proc_spec spec;
    pid_t pid = -1;

    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL)
        return;

    /* An ordinary stock app, asked for as root. */
    argv[0] = ND_PATH_ND_APPRUN;
    argv[1] = "/NeoDCT/System/apps/PhoneBook";
    argv[2] = "run";
    argv[3] = NULL;
    spec_for(&spec, argv);
    CHECK_INT((int)nd_broker_spawn(b, ND_PATH_ND_APPRUN, &spec, NULL, &pid), (int)ND_ERR_PERM);

    /* A user-installed app, asked for as root -- the interesting one, because
     * that directory is on the MEMORY CARD, which anyone with the phone in
     * their hand can take out and write on a PC. "nd-apprun is allowed to run
     * as root" plus "which app it runs is argv[1]" would otherwise mean any
     * .so a stranger left on a card runs as root. */
    argv[1] = ND_PATH_USER_APPS_DIR "/Pentest";
    CHECK_INT((int)nd_broker_spawn(b, ND_PATH_ND_APPRUN, &spec, NULL, &pid), (int)ND_ERR_PERM);

    /* And a path that starts with the engineering directory but climbs out. */
    argv[1] = ND_PATH_ENG_APPS_DIR "/../../.." ND_PATH_USER_APPS_DIR "/Pentest";
    CHECK_INT((int)nd_broker_spawn(b, ND_PATH_ND_APPRUN, &spec, NULL, &pid), (int)ND_ERR_PERM);

    nd_broker_stop(b);
}

/* ============ THE ENVIRONMENT IS THE PROGRAM ============
 *
 * The path allow-list above was, for a while, the whole of the root-spawn
 * defence -- and it was worth nothing, because both executables on it are
 * steered by their environment rather than by their arguments. A #!/bin/sh
 * helper with a chosen PATH runs the chooser's binaries; nd-apprun with a
 * chosen NEODCT_ROOT dlopens the chooser's library. Neither is setuid, so the
 * loader honours LD_PRELOAD from anyone who can set it.
 *
 * This is the test the earlier ones did not think to write: not "is the path
 * checked" but "does the checked path still do what the caller says". */
static void test_a_root_child_does_not_get_the_callers_environment(void)
{
    const char *in[8];
    const char *out[8];
    size_t i;
    bool saw_path = false;
    bool saw_fb = false;

    in[0] = "LD_PRELOAD=" ND_PATH_USER_APPS_DIR "/x/evil.so";
    in[1] = "PATH=" ND_PATH_USER_APPS_DIR "/x/bin";
    in[2] = "NEODCT_ROOT=" ND_PATH_USER_APPS_DIR "/x";
    in[3] = "NEODCT_FB_FD=7";      /* legitimate: a descriptor just remapped */
    in[4] = "LD_AUDIT=/tmp/a.so";
    in[5] = "NEODCT_FB_FD_EVIL=1"; /* a prefix of a kept name is not that name */
    in[6] = "NEODCT_SERVICE_FD";   /* a kept name with no '=' is not an entry */
    in[7] = NULL;

    memset(out, 0, sizeof out);
    nd_broker__root_env_filter(in, 7u, out, ND_ARRAY_LEN(out));

    for (i = 0u; out[i] != NULL; i++) {
        CHECK(strncmp(out[i], "LD_", 3u) != 0);
        CHECK(strncmp(out[i], "NEODCT_ROOT=", 12u) != 0);
        CHECK(strcmp(out[i], "NEODCT_FB_FD_EVIL=1") != 0);
        CHECK(strcmp(out[i], "NEODCT_SERVICE_FD") != 0);
        if (strcmp(out[i], ND_BROKER_ROOT_PATH) == 0)
            saw_path = true;
        if (strcmp(out[i], "NEODCT_FB_FD=7") == 0)
            saw_fb = true;
    }

    /* A PATH the caller did not choose, and it is not the caller's. */
    CHECK(saw_path);
    for (i = 0u; out[i] != NULL; i++)
        CHECK(strcmp(out[i], "PATH=" ND_PATH_USER_APPS_DIR "/x/bin") != 0);

    /* And the one thing a launch genuinely cannot do without survives. */
    CHECK(saw_fb);
}

/* neodct-sdcard is the other name on the list, and it had no argument check at
 * all -- so "the path is pinned" meant `format` could be `add`, against any
 * device, at any mountpoint. The core asks for exactly one thing. */
static void test_the_sdcard_helper_may_only_be_asked_to_format_a_device(void)
{
    const char *argv[5];

    /* What nd_svc_format_card() actually sends. */
    argv[0] = ND_PATH_SDCARD_HELPER;
    argv[1] = "format";
    argv[2] = "/dev/mmcblk0";
    argv[3] = NULL;
    CHECK(nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));

    /* `layout` is the other thing the core asks for (nd_svc_layout_card),
     * and it takes nothing: the helper lays out the card at the phone's own
     * mountpoint. A layout WITH an argument is therefore not a request the
     * core makes, whatever the argument is. */
    argv[1] = "layout";
    argv[2] = NULL;
    CHECK(nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 2u));
    argv[2] = "/dev/mmcblk0";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));
    argv[2] = "/NeoDCT/User";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));

    /* Every other verb the helper implements. */
    argv[1] = "add";
    argv[2] = "/dev/mmcblk0";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));
    argv[2] = NULL;
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 2u));
    argv[1] = "scan";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 2u));
    argv[2] = "/dev/mmcblk0";
    argv[1] = "remove";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));
    argv[1] = "scan";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));

    /* A "device" that is not one. */
    argv[1] = "format";
    argv[2] = "/NeoDCT/User/browser/loop.img";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));
    argv[2] = "/dev/../NeoDCT/User/x";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));
    argv[2] = "/dev/mapper/../../etc/x";
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 3u));

    /* And the shapes that are not a format request at all. */
    argv[2] = "/dev/mmcblk0";
    argv[3] = "extra";
    argv[4] = NULL;
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 4u));
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, argv, 2u));
    CHECK(!nd_broker__root_exec_allowed(ND_PATH_SDCARD_HELPER, NULL, 3u));
}

/* ============ THE THIRD SPELLING ============
 *
 * "root" was refused. Then no name at all was refused. This is the one that
 * was left: a name that IS on the list but does not exist in the image. The
 * lookup failed, the broker logged and carried on, and nd_priv_become() on a
 * zeroed id is a documented no-op -- so the child ran as root, having never
 * passed the root-exec list, because that gate only fires on an EMPTY name.
 *
 * The host is the image in question here: unit tests run somewhere with no
 * ndusr, so this is the ordinary case rather than a contrived one. */
static void test_a_user_that_does_not_resolve_is_not_a_way_to_stay_root(void)
{
    /* Enumerated rather than spawned, and that is the point of the shape.
     *
     * The end-to-end version of this test would need a host that is MISSING
     * an ndusr, so on a host that has one it would skip -- silently, and pass.
     * That is precisely how the first root-shell hole survived its own test:
     * the test asserted the case the author had in mind and never reached the
     * one that mattered. A pure predicate has no host to depend on. */
    CHECK(nd_broker__spawn_stays_root(NULL, true));
    CHECK(nd_broker__spawn_stays_root(NULL, false));
    CHECK(nd_broker__spawn_stays_root("", true));
    CHECK(nd_broker__spawn_stays_root("", false));

    /* The one that was missed: an allowed NAME, absent from the image. */
    CHECK(nd_broker__spawn_stays_root(ND_PRIV_USER, false));
    CHECK(nd_broker__spawn_stays_root(ND_PRIV_USER_UT, false));

    /* And the only shape that drops. */
    CHECK(!nd_broker__spawn_stays_root(ND_PRIV_USER, true));
    CHECK(!nd_broker__spawn_stays_root(ND_PRIV_USER_UT, true));

    /* Which the broker then refuses unless the path is on the root-exec list
     * -- and /bin/true is not, however the staying-root was spelled. */
    {
        const char *argv[2];

        argv[0] = "/bin/true";
        argv[1] = NULL;
        CHECK(!nd_broker__root_exec_allowed("/bin/true", argv, 1u));
        CHECK(!nd_broker__root_exec_allowed("/bin/sh", argv, 1u));
        CHECK(!nd_broker__root_exec_allowed(NULL, argv, 1u));
    }
}

/* And it still spawns what it should. A refusal that refuses everything is not
 * a boundary, it is a broken phone -- which is what the first attempt at this
 * produced, since a build failure left a stale image and the UI never dropped
 * at all. */
static void test_the_broker_spawns_and_reaps(void)
{
    nd_broker *b;
    const char *path = true_path();
    const char *argv[2];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    nd_err rc;

    if (path == NULL) {
        printf("test_broker: no /bin/true; skipping\n");
        return;
    }
    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL)
        return;
    CHECK(nd_broker_ok(b));

    argv[0] = path;
    argv[1] = NULL;
    spec_for(&spec, argv);

    /* A NAMED user, because "do not drop" is no longer a request an arbitrary
     * path may make -- see test_a_nameless_user_is_not_a_way_to_stay_root().
     * This test is about the round trip, not the drop: if the host has no
     * ndusr, nd_priv_lookup fails on the broker's side and the child simply
     * runs undropped, which does not change what is being measured here. */
    rc = nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid);
    CHECK_INT((int)rc, (int)ND_OK);
    CHECK(pid > 0);

    /* The child belongs to the BROKER, so only the broker can reap it. A
     * waitpid() here would return ECHILD: this process never forked it. */
    memset(&st, 0, sizeof st);
    for (;;) {
        nd_err w = nd_broker_wait(b, pid, 1.0, &st);

        if (w == ND_OK)
            break;
        CHECK_INT((int)w, (int)ND_ERR_TIMEOUT);
        if (w != ND_ERR_TIMEOUT)
            break;
    }
    CHECK(st.exited);
    CHECK_INT(st.exit_status, 0);

    nd_broker_stop(b);
}

/* A record longer than the blob is refused whole rather than truncated. An
 * argv that lost its tail is a different command from the one that was asked
 * for, and the broker runs it as somebody else. */
static void test_an_oversized_request_is_refused_not_trimmed(void)
{
    nd_broker *b;
    const char *path = true_path();
    static char big[ND_BROKER_BLOB_MAX];
    const char *argv[3];
    nd_proc_spec spec;
    pid_t pid = -1;
    nd_err rc;

    if (path == NULL)
        return;
    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL)
        return;

    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';
    argv[0] = path;
    argv[1] = big;
    argv[2] = NULL;
    spec_for(&spec, argv);

    rc = nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid);
    CHECK_INT((int)rc, (int)ND_ERR_TOOLONG);

    /* And the broker is still usable afterwards: a rejected record must not
     * desynchronise the socket, or one bad launch takes every later one with
     * it. */
    CHECK(nd_broker_ok(b));
    {
        const char *ok_argv[2];
        nd_proc_status st;

        ok_argv[0] = path;
        ok_argv[1] = NULL;
        spec_for(&spec, ok_argv);
        pid = -1;
        CHECK_INT((int)nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid), (int)ND_OK);
        CHECK(pid > 0);
        memset(&st, 0, sizeof st);
        while (nd_broker_wait(b, pid, 1.0, &st) == ND_ERR_TIMEOUT)
            ;
        CHECK(st.exited);
    }

    nd_broker_stop(b);
}

int main(void)
{
    RUN(test_the_broker_refuses_to_spawn_as_root);
    RUN(test_a_nameless_user_is_not_a_way_to_stay_root);
    RUN(test_apprun_as_root_is_only_for_engineering_apps);
    RUN(test_a_root_child_does_not_get_the_callers_environment);
    RUN(test_the_sdcard_helper_may_only_be_asked_to_format_a_device);
    RUN(test_a_user_that_does_not_resolve_is_not_a_way_to_stay_root);
    RUN(test_the_broker_spawns_and_reaps);
    RUN(test_an_oversized_request_is_refused_not_trimmed);
    return pt_report("test_broker");
}
