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

    /* NULL user: "do not drop", which is what an engineering app gets. The
     * host has no guarantee of an ndusr to become, so the drop itself is not
     * what this test is about -- the round trip is. */
    rc = nd_broker_spawn(b, path, &spec, NULL, &pid);
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

    rc = nd_broker_spawn(b, path, &spec, NULL, &pid);
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
        CHECK_INT((int)nd_broker_spawn(b, path, &spec, NULL, &pid), (int)ND_OK);
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
    RUN(test_the_broker_spawns_and_reaps);
    RUN(test_an_oversized_request_is_refused_not_trimmed);
    return pt_report("test_broker");
}
