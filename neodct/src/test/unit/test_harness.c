/* test_harness.c -- the harness that lets this suite run on a developer's
 * machine without switching it off.
 *
 * On 2026-08-31 and 2026-09-04 `make test` -- and then a loop running the
 * binaries by hand -- powered off the workstation running it, through
 * nd_svc_poweroff(). The fix is four layers, and this file is where three of
 * them are asserted from the inside:
 *
 *   1. nd_svc.c's rule: a real halt resolves only for a root process that is
 *      neither a test root (NEODCT_ROOT) nor disarmed. test_svc.c covers the
 *      table; here the consequence is checked -- this process cannot.
 *   2. test/harness/nd_testguard.c, linked into every test binary by the
 *      Makefile: disarms the halt before main() and refuses to run at all
 *      unless the harness started it. Checked by starting THIS binary again
 *      the wrong way and watching it stop.
 *   3. test/harness/fakebin on $PATH: poweroff, reboot, systemctl and friends
 *      resolve to a script that refuses and logs. Checked by resolving them.
 *   4. test/harness/sandbox.sh: bwrap with no D-Bus, no network, a minimal
 *      /dev. Checked when it is in effect, skipped with a note when the run
 *      opted out.
 *
 * The bare-run case spawns /proc/self/exe with NEODCT_TESTGUARD_PROBE set
 * and the harness variables removed. If the guard lets that child reach
 * main(), main() sees the probe and exits 42 instead of running the suite
 * again -- so a missing guard is a failed check, not a fork bomb. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_svc.h"
#include "nd_types.h"

#include "platform_test.h"

extern char **environ;

#define PROBE_EXIT 42

/* environ minus the harness variables, plus the probe marker. Owned by the
 * caller; the strings are environ's own. */
static const char **bare_env(void)
{
    static const char *const DROP[] = {"NEODCT_TEST_HARNESS=", "NEODCT_ALLOW_BARE=",
                                       "NEODCT_TESTGUARD_PROBE="};
    const char **envp;
    size_t have = 0u;
    size_t n = 0u;
    size_t i;
    size_t k;

    while (environ[have] != NULL)
        have++;
    envp = calloc(have + 2u, sizeof *envp);
    if (envp == NULL)
        return NULL;
    for (i = 0u; i < have; i++) {
        bool drop = false;

        for (k = 0u; k < ND_ARRAY_LEN(DROP); k++) {
            if (strncmp(environ[i], DROP[k], strlen(DROP[k])) == 0)
                drop = true;
        }
        if (!drop)
            envp[n++] = environ[i];
    }
    envp[n++] = "NEODCT_TESTGUARD_PROBE=1";
    envp[n] = NULL;
    return envp;
}

static bool spawn_quiet(const char *path, const char *const *argv, const char *const *envp,
                        nd_proc_status *st)
{
    nd_proc_spec spec;
    pid_t pid = -1;
    int devnull;

    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0)
        return false;
    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.envp = envp;
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = devnull;
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = devnull;
    spec.n_fds = 2u;
    if (nd_proc_spawn(path, &spec, &pid) != ND_OK) {
        (void)close(devnull);
        return false;
    }
    (void)close(devnull);
    return nd_proc_wait(pid, 10.0, st) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * 1. This process cannot halt the machine
 * ------------------------------------------------------------------ */

static void test_this_process_cannot_halt(void)
{
    /* Whatever the environment says. NEODCT_ROOT is the harness's own
     * interlock; take it away and the guard's disarm and the not-root rule
     * are what is left, and either alone is enough. */
    const char *saved = getenv(ND_ENV_ROOT);
    char keep[ND_PATH_MAX];

    (void)nd_strlcpy(keep, saved != NULL ? saved : "", sizeof keep);
    nd_svc_halt_simulate(NULL);
    (void)unsetenv(ND_ENV_ROOT);

    CHECK(!nd_svc_halt_allowed());
    CHECK(!nd_svc_poweroff());
    CHECK(!nd_svc_reboot());
    CHECK(!nd_svc_halt_now(false));
    CHECK(!nd_svc_halt_now(true));

    if (keep[0] != '\0')
        (void)setenv(ND_ENV_ROOT, keep, 1);
}

/* ------------------------------------------------------------------ *
 * 2. A bare run stops before main()
 * ------------------------------------------------------------------ */

static void test_a_bare_run_is_refused_before_main(void)
{
    const char *argv[2];
    const char **envp = bare_env();
    nd_proc_status st;

    CHECK(envp != NULL);
    if (envp == NULL)
        return;
    argv[0] = "/proc/self/exe";
    argv[1] = NULL;

    CHECK(spawn_quiet("/proc/self/exe", argv, envp, &st));
    CHECK(st.exited);
    /* 3 is nd_testguard's; PROBE_EXIT would mean it let main() run. */
    CHECK_INT(st.exit_status, 3);
    free(envp);
}

/* And a run the harness started is let through -- that is this one. */
static void test_a_harness_run_is_let_through(void)
{
    CHECK(getenv("NEODCT_TEST_HARNESS") != NULL);
}

/* ------------------------------------------------------------------ *
 * 3. The system verbs on $PATH are the fakes
 * ------------------------------------------------------------------ */

static bool under_fakebin(const char *path)
{
    const char *fakebin = getenv("NEODCT_FAKEBIN");

    if (fakebin == NULL || fakebin[0] == '\0' || path == NULL)
        return false;
    return strncmp(path, fakebin, strlen(fakebin)) == 0 && path[strlen(fakebin)] == '/';
}

static void test_the_halt_verbs_on_path_are_fakes(void)
{
    static const char *const VERBS[] = {"poweroff", "reboot",   "halt",    "shutdown",
                                        "systemctl", "loginctl", "busybox"};
    size_t i;

    CHECK(getenv("NEODCT_FAKEBIN") != NULL);
    for (i = 0u; i < ND_ARRAY_LEN(VERBS); i++) {
        char out[ND_PATH_MAX];

        out[0] = '\0';
        /* The same lookup halt_resolve() makes, so what it would find. */
        CHECK(nd_svc_halt_which(VERBS[i], out, sizeof out));
        if (!under_fakebin(out))
            fprintf(stderr, "  %s resolves to %s, not the fake\n", VERBS[i], out);
        CHECK(under_fakebin(out));
    }
}

/* The fake does nothing, fails, and writes down what was asked. Spawned
 * ONLY when the lookup really landed in fakebin: the point of this test is
 * never to run the real one. */
static void test_a_fake_verb_refuses_and_is_logged(void)
{
    char exe[ND_PATH_MAX];
    char logpath[ND_PATH_MAX];
    char text[256];
    const char *argv[3];
    const char *saved_log = getenv("NEODCT_FAKEBIN_LOG");
    char keep[ND_PATH_MAX];
    nd_proc_status st;

    exe[0] = '\0';
    CHECK(nd_svc_halt_which("poweroff", exe, sizeof exe));
    CHECK(under_fakebin(exe));
    if (!under_fakebin(exe))
        return;

    /* Our own log, so the run's log stays empty and the run stays green. */
    CHECK_INT(nd_path_resolve(logpath, sizeof logpath, "/fakebin-probe.log"), ND_OK);
    (void)nd_strlcpy(keep, saved_log != NULL ? saved_log : "", sizeof keep);
    CHECK_INT(setenv("NEODCT_FAKEBIN_LOG", logpath, 1), 0);

    argv[0] = exe;
    argv[1] = "--probe";
    argv[2] = NULL;
    CHECK(spawn_quiet(exe, argv, NULL, &st));
    CHECK(st.exited);
    CHECK_INT(st.exit_status, 1);

    if (keep[0] != '\0')
        (void)setenv("NEODCT_FAKEBIN_LOG", keep, 1);
    else
        (void)unsetenv("NEODCT_FAKEBIN_LOG");

    text[0] = '\0';
    (void)pt_read_text("/fakebin-probe.log", text, sizeof text);
    CHECK(strstr(text, "poweroff --probe") != NULL);
}

/* ------------------------------------------------------------------ *
 * 4. The sandbox, when it is in effect
 * ------------------------------------------------------------------ */

static int route_lines(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[512];
    int n = 0;

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof line, f) != NULL)
        n++;
    (void)fclose(f);
    return n;
}

static void test_the_sandbox_has_nothing_to_halt(void)
{
    const char *mode = getenv("NEODCT_TEST_SANDBOX");

    if (mode == NULL || strcmp(mode, "bwrap") != 0) {
        printf("  (NEODCT_TEST_SANDBOX=%s: the container checks do not apply)\n",
               mode != NULL ? mode : "unset");
        return;
    }
    /* No D-Bus: systemctl, poweroff and loginctl have nobody to ask. */
    CHECK(access("/run/dbus/system_bus_socket", F_OK) != 0);
    CHECK(access("/run/systemd/system", F_OK) != 0);
    /* No sound card, no virtual keyboard, no framebuffer, no serial modem. */
    CHECK(access("/dev/snd", F_OK) != 0);
    CHECK(access("/dev/uinput", F_OK) != 0);
    CHECK(access("/dev/fb0", F_OK) != 0);
    CHECK(access("/dev/ttyUSB0", F_OK) != 0);
    /* No network: the header line and nothing else. */
    CHECK_INT(route_lines(), 1);
}

int main(void)
{
    /* Reached only if nd_testguard let a bare run through; see the header. */
    if (getenv("NEODCT_TESTGUARD_PROBE") != NULL)
        return PROBE_EXIT;

    RUN(test_this_process_cannot_halt);
    RUN(test_a_bare_run_is_refused_before_main);
    RUN(test_a_harness_run_is_let_through);
    RUN(test_the_halt_verbs_on_path_are_fakes);
    RUN(test_a_fake_verb_refuses_and_is_logged);
    RUN(test_the_sandbox_has_nothing_to_halt);

    return pt_report("test_harness");
}
