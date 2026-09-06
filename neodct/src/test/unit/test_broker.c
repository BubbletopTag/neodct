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

#include <signal.h>

#include "nd_broker.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_svc.h"

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

/* ============ WHAT THIS HOST CANNOT BE ASKED ============
 *
 * Four of the cases below need a spawn to SUCCEED, and a spawn succeeds only
 * for a user the broker can RESOLVE: ND_BROKER_USERS is {ndusr, ndusr_ut} and
 * a build host has neither. nd_broker.c then refuses, deliberately -- a name
 * that does not resolve leaves the nd_priv_id zeroed, nd_priv_become() on a
 * zeroed id is a documented no-op, and the child would run as root having
 * never passed the root-exec list. That refusal is the subject of
 * test_a_user_that_does_not_resolve_is_not_a_way_to_stay_root() a few
 * hundred lines down; it is the fix, not a fault.
 *
 * So "does the round trip work" is not a question this host can be asked, and
 * the comment that used to sit on it -- "if the host has no ndusr,
 * nd_priv_lookup fails on the broker's side and the child simply runs
 * undropped" -- has described removed behaviour since the hole was closed. It
 * cost eight permanent FAILs a run, and ten once the kill verb was given a
 * case of its own, which is worse than not running them at all: a suite that
 * is always red is a suite nobody reads, and that is precisely how 0.5.9a came
 * to ship with twenty-five regressions in it.
 *
 * They SKIP now, by name and with the reason. run-tests.sh prints the same
 * configuration once as a CONFIG banner; this is the per-case half of it. A
 * skip must never be read as a pass, so main() repeats the count beside the
 * result and says what the run did not touch.
 */
static int g_skips;

static bool a_drop_user_exists(void)
{
    nd_priv_id id;

    /* The broker's own question, asked the same way, so the two can never
     * disagree about which hosts these cases can run on. */
    return nd_priv_lookup(ND_PRIV_USER, &id);
}

static bool skipped_without_a_drop_user(const char *what)
{
    if (a_drop_user_exists())
        return false;
    g_skips++;
    fprintf(stderr,
            "SKIP %s: no '%s' on this host, so the broker refuses the spawn -- correctly -- "
            "and there is no round trip left to measure\n",
            what, ND_PRIV_USER);
    return true;
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
     * staying root, not about this path.
     *
     * Only this tail needs the name to resolve. Everything above it is pure
     * policy and is checked on every host, which is the half that matters:
     * the case exists to prove the hole is shut, not to prove /bin/true runs. */
    if (!skipped_without_a_drop_user("an allowed user is still spawned")) {
        pid = -1;
        CHECK_INT((int)nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid), (int)ND_OK);
        if (pid > 0) {
            nd_proc_status st;

            memset(&st, 0, sizeof st);
            while (nd_broker_wait(b, pid, 1.0, &st) == ND_ERR_TIMEOUT)
                ;
        }
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
    in[3] = "NEODCT_FB_FD=7"; /* legitimate: a descriptor just remapped */
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
    if (skipped_without_a_drop_user("the broker spawns and reaps"))
        return;
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
     * This case is about the round trip rather than about the drop, but it
     * still needs the name to RESOLVE: one that does not is refused before a
     * child exists, which is why the whole case skips at the top. */
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
     * it.
     *
     * The follow-up request is ANSWERED whatever the host is -- ND_OK where
     * ndusr exists, ND_ERR_PERM where it does not -- and it is the answer,
     * arriving at all and carrying a code the broker chose for it, that shows
     * the framing recovered. Asserting the code the configuration implies,
     * rather than ND_OK, keeps the desynchronisation check running everywhere;
     * only the reap under it needs a child to have been started. */
    CHECK(nd_broker_ok(b));
    {
        const char *ok_argv[2];
        nd_proc_status st;
        const bool have_user = a_drop_user_exists();

        ok_argv[0] = path;
        ok_argv[1] = NULL;
        spec_for(&spec, ok_argv);
        pid = -1;
        CHECK_INT((int)nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid),
                  (int)(have_user ? ND_OK : ND_ERR_PERM));
        if (!have_user) {
            g_skips++;
            fprintf(stderr, "SKIP the reap after an oversized request: no '%s' on this host\n",
                    ND_PRIV_USER);
        } else {
            CHECK(pid > 0);
            memset(&st, 0, sizeof st);
            while (nd_broker_wait(b, pid, 1.0, &st) == ND_ERR_TIMEOUT)
                ;
            CHECK(st.exited);
        }
    }

    nd_broker_stop(b);
}

/* ------------------------------------------------------------------ *
 * KILL: the verb the phone needed and did not have
 * ------------------------------------------------------------------ *
 *
 * The core cannot signal the helpers it starts. `neodct-sdcard format` is
 * spawned UNDROPPED, so it is root and it is the BROKER's child; the core is
 * ndusr and is nobody's parent to it. The 240-second escape hatch called
 * kill(2) from uid 1000 against uid 0, got EPERM -- which nd_proc_terminate
 * only logs -- and then waitpid()ed a process it had never forked, got ECHILD,
 * and nd_proc.c read ECHILD as "exited cleanly". So it reported success, sent
 * no SIGKILL, reaped nothing, and a root mke2fs went on writing the card while
 * the screen said "Formatting failed."
 *
 * The verb that fixes it is a root process taking a pid off an untrusted
 * socket, so its two pins are the whole of what makes it safe, and both are
 * asserted here rather than reasoned about.
 */

/* Something that runs long enough to still be alive on the next line. Skipped
 * rather than failed if the host has no sleep(1), for the same reason the
 * spawn cases skip without /bin/true: that would be testing the host. */
static const char *sleep_path(void)
{
    static const char *const CANDIDATES[] = {"/bin/sleep", "/usr/bin/sleep", NULL};
    size_t i;

    for (i = 0u; CANDIDATES[i] != NULL; i++) {
        if (access(CANDIDATES[i], X_OK) == 0)
            return CANDIDATES[i];
    }
    return NULL;
}

/* Pin one: the signal. A signal number arriving off this socket is otherwise a
 * way to send SIGSTOP to init -- and a STOPped root mkfs still holding the card
 * open is worse than one that is merely still running. Two literals, not a
 * range, so the next number somebody puts on the wire is refused by default. */
static void test_the_kill_verb_takes_two_signals_and_no_others(void)
{
    CHECK(nd_broker__kill_signo_allowed(SIGTERM));
    CHECK(nd_broker__kill_signo_allowed(SIGKILL));
    CHECK(!nd_broker__kill_signo_allowed(SIGSTOP));
    CHECK(!nd_broker__kill_signo_allowed(SIGINT));
    CHECK(!nd_broker__kill_signo_allowed(SIGHUP));
    CHECK(!nd_broker__kill_signo_allowed(0));  /* the "does it exist" probe */
    CHECK(!nd_broker__kill_signo_allowed(-1)); /* and the whole process group */
}

/* Pin two: the pid. The broker signals only a process it forked itself and has
 * not yet reaped, which is a fact it owns rather than a policy it applies.
 *
 * getpid() is the sharpest case available: it is a live process, it is not the
 * broker's child, and it is THIS TEST. A broker that took the caller's word for
 * it would kill the thing asking. */
static void test_the_broker_will_not_signal_a_pid_it_did_not_start(void)
{
    nd_broker *b = nd_broker_start();

    CHECK(b != NULL);
    if (b == NULL)
        return;

    CHECK(!nd_broker_kill(b, getpid(), SIGTERM));
    CHECK(!nd_broker_kill(b, 1, SIGKILL)); /* init */
    CHECK(!nd_broker_kill(b, -1, SIGTERM));
    CHECK(!nd_broker_kill(b, 0, SIGTERM));

    /* Still usable afterwards: a refused request must not desynchronise the
     * socket, or one bad ask takes every later one with it. */
    CHECK(nd_broker_ok(b));
    nd_broker_stop(b);
}

/* And the whole escape hatch, end to end: start something long, fail to wait
 * for it, stop it, reap it.
 *
 * This also pins the OTHER half of the format fix. nd_broker_wait() is given a
 * short deadline against a child that will outlive it, and it has to come back
 * -- on this side, having polled -- rather than parking the broker inside a
 * blocking waitpid. That is what the 240-second format wait used to do, and
 * while it did it the broker served nobody: no app launch, no halt, no clock,
 * and the core's round-trip mutex was held for the duration, which is the mutex
 * the UI thread needs on every turn of the loop that scans the key matrix. */
static void test_the_broker_stops_a_child_it_started(void)
{
    nd_broker *b;
    const char *path = sleep_path();
    const char *argv[3];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    pid_t second = -1;
    const char *tp = true_path();

    if (path == NULL || tp == NULL) {
        printf("test_broker: no sleep(1); skipping\n");
        return;
    }
    /* Every line of this case hangs off a child that started, so there is no
     * half of it to keep. Skipping it leaves the kill verb covered only by its
     * two pins -- the signal list and the not-my-child refusal, both above and
     * both pure -- and leaves the signal-then-reap sequence itself unexercised
     * anywhere a developer runs. nd-selftest on the phone is where that one is
     * actually answered. */
    if (skipped_without_a_drop_user("the broker stops a child it started"))
        return;
    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL)
        return;

    argv[0] = path;
    argv[1] = "30";
    argv[2] = NULL;
    spec_for(&spec, argv);
    CHECK_INT((int)nd_broker_spawn(b, path, &spec, ND_PRIV_USER, &pid), (int)ND_OK);
    CHECK(pid > 0);
    if (pid <= 0) {
        nd_broker_stop(b);
        return;
    }

    /* A bounded wait against a child that is going nowhere. It must TIME OUT
     * and return, which it can only do because the waiting happens here. */
    memset(&st, 0, sizeof st);
    CHECK_INT((int)nd_broker_wait(b, pid, 0.3, &st), (int)ND_ERR_TIMEOUT);

    /* And the broker was never stuck: it answers a completely different
     * request while the long-lived child is still running. Under the old
     * blocking REQ_WAIT this could not have been reached at all. */
    {
        const char *ok_argv[2];
        nd_proc_spec ok_spec;

        ok_argv[0] = tp;
        ok_argv[1] = NULL;
        spec_for(&ok_spec, ok_argv);
        CHECK_INT((int)nd_broker_spawn(b, tp, &ok_spec, ND_PRIV_USER, &second), (int)ND_OK);
        CHECK(second > 0);
        memset(&st, 0, sizeof st);
        while (nd_broker_wait(b, second, 5.0, &st) == ND_ERR_TIMEOUT)
            ;
        CHECK(st.exited);
    }

    /* Now stop the sleeper. Both halves go through the broker because both are
     * syscalls the core no longer has: the signal, and the reap. */
    CHECK(nd_broker_kill(b, pid, SIGTERM));
    memset(&st, 0, sizeof st);
    while (nd_broker_wait(b, pid, 5.0, &st) == ND_ERR_TIMEOUT)
        ;
    CHECK(st.signalled);
    CHECK_INT(st.signo, SIGTERM);

    /* Reaped, so the pid is one the kernel may hand to somebody else now --
     * and the broker stops agreeing to signal it. A table that kept reaped
     * pids would be the one way this verb could become a weapon. */
    CHECK(!nd_broker_kill(b, pid, SIGKILL));

    nd_broker_stop(b);
}

/* ------------------------------------------------------------------ *
 * HALT: the verb that had never once run
 * ------------------------------------------------------------------ *
 *
 * nd_broker_halt() shipped with zero coverage on either side and was, in every
 * configuration a phone could be in, unreachable: the core's own halt gate
 * refused before the delegation branch behind it could be entered. So the
 * CAP_SYS_BOOT half of the whole design had never executed, and the fix for
 * "Power off failed." could have been undone by anybody without a test
 * noticing.
 *
 * The simulation is installed BEFORE nd_broker_start() so that the fork carries
 * it into the broker child: the spawn hook is what makes this safe to run on a
 * developer's machine, and it is checked in the child, which is the process
 * that would otherwise be exec'ing /sbin/poweroff. What the parent can see is
 * the reply, and that is the assertion -- ND_OK means the request crossed the
 * socket, passed the interlock inside the broker, resolved a binary and reached
 * halt_perform(). */
static void halt_hook(bool reboot, const char *exe, void *user)
{
    /* Step 5, replaced. It runs in the BROKER CHILD, so it has nothing to
     * report back to and nothing to assert on; its whole job is to exist, so
     * that halt_perform() cannot reach a real fork and exec. */
    (void)reboot;
    (void)exe;
    (void)user;
}

static void test_a_halt_crosses_the_socket(void)
{
    nd_broker *b;
    nd_svc_halt_sim sim;
    const char *path = true_path();
    static const char *argv0[2];
    static const char *const *const TAB[1] = {argv0};

    if (path == NULL) {
        printf("test_broker: no /bin/true; skipping\n");
        return;
    }
    argv0[0] = path;
    argv0[1] = NULL;

    /* Candidate tables of one entry each, pointing at something that certainly
     * exists, so the case does not depend on the host having /sbin/poweroff --
     * a container usually does not. Nothing is run either way: sim.spawn
     * replaces step 5 and only step 5. */
    memset(&sim, 0, sizeof sim);
    sim.spawn = halt_hook;
    sim.poweroff = TAB;
    sim.reboot = TAB;
    sim.n = 1u;
    nd_svc_halt_simulate(&sim);

    b = nd_broker_start();
    CHECK(b != NULL);
    if (b == NULL) {
        nd_svc_halt_simulate(NULL);
        return;
    }

    /* BOTH guards, deliberately. The tables mean nothing resolves to a real
     * poweroff, and the hook means step 5 is not a fork at all. Either alone
     * would do; this suite has switched a developer's machine off twice, and
     * the second time was a binary run by hand, so the case that reaches
     * furthest into the halt path is the one that gets two of them. */
    CHECK(nd_broker_halt(b, false));
    CHECK(nd_broker_halt(b, true));
    CHECK(nd_broker_ok(b));

    nd_broker_stop(b);
    nd_svc_halt_simulate(NULL);
}

int main(void)
{
    int rc;

    RUN(test_the_broker_refuses_to_spawn_as_root);
    RUN(test_a_nameless_user_is_not_a_way_to_stay_root);
    RUN(test_apprun_as_root_is_only_for_engineering_apps);
    RUN(test_a_root_child_does_not_get_the_callers_environment);
    RUN(test_the_sdcard_helper_may_only_be_asked_to_format_a_device);
    RUN(test_a_user_that_does_not_resolve_is_not_a_way_to_stay_root);
    RUN(test_the_broker_spawns_and_reaps);
    RUN(test_an_oversized_request_is_refused_not_trimmed);
    RUN(test_the_kill_verb_takes_two_signals_and_no_others);
    RUN(test_the_broker_will_not_signal_a_pid_it_did_not_start);
    RUN(test_the_broker_stops_a_child_it_started);
    RUN(test_a_halt_crosses_the_socket);
    rc = pt_report("test_broker");
    /* Printed after the result, not before it: pt_report()'s "checks passed"
     * is a statement about the checks that RAN, and a reader who does not see
     * this line beside it will take it for coverage this run did not have. */
    if (g_skips > 0) {
        /* pt_report() wrote to stdout, which is block-buffered down a pipe
         * while stderr is not; without this the caveat overtakes the result it
         * is a caveat ON. */
        (void)fflush(stdout);
        fprintf(stderr,
                "test_broker: %d case(s) SKIPPED for want of '%s' -- this run did NOT exercise "
                "a successful spawn, a reap, or the kill verb end to end\n",
                g_skips, ND_PRIV_USER);
    }
    return rc;
}
