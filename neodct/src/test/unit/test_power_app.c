/* test_power_app.c -- what the Power app does when the halt does NOT start.
 *
 * test_power.c covers the menu, the strings and writing the recovery flag.
 * This file covers the branch that was missing entirely until a real phone
 * found it, and it is a separate binary rather than more cases in that one
 * because the thing under test is a FAILURE path that only exists now that
 * nd_power_go_down() has a return value.
 *
 * ============ THE BUG THIS IS THE REGRESSION TEST FOR ============
 *
 * Power > Recovery does two things in order: it writes
 * /NeoDCT/User/.ndsys/boot_recovery, and then it asks the core to reboot. The
 * flag is a REQUEST, not a record -- ndsys-recovery.sh tests only that the file
 * exists and deletes it as it reads it -- so it has no expiry and no owner.
 *
 * On every 0.5.x phone the reboot was refused (the core's halt gate asked
 * geteuid()==0 inside a core that had dropped to ndusr; see nd_svc.h). The app
 * drew "Reboot failed." and returned to the menu WITH THE FLAG STILL ON DISK.
 * The next boot from any cause at all -- a battery pull a week later, an
 * update, a crash -- then landed in the recovery menu, with nothing to connect
 * it to a key pressed days before.
 *
 * nd_power_go_down() returned void, so the caller could not even see that it
 * had happened. It returns bool now and request_recovery() takes the flag back.
 *
 * ============ WHAT THIS TEST WILL NOT DO ============
 *
 * It never calls nd_power_go_down(), for the reason power.h and test_power.c
 * both give in the same words: a process with no service channel IS the core as
 * far as nd_svc.h can tell, so the halt would be resolved and spawned HERE. A
 * test suite that can switch off the machine it is running on is not a test
 * suite, and this suite has done it twice. What is reachable is the half that
 * cleans up, and that is what is pinned.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "smallapp_test.h"

#include "../../apps/Power/power.h"

static struct {
    nd_err (*request_flag)(char *, size_t);
    nd_err (*clear_flag)(void);
    const char *const *fail_reboot;
    const char *const *fail_recovery;
} api;

static bool api_open(void *h)
{
    *(void **)&api.request_flag = sa_sym(h, "nd_power_request_recovery_flag");
    *(void **)&api.clear_flag = sa_sym(h, "nd_power_clear_recovery_flag");
    api.fail_reboot = dlsym(h, "nd_power_fail_reboot");
    api.fail_recovery = dlsym(h, "nd_power_fail_recovery");
    return api.request_flag != NULL && api.clear_flag != NULL && api.fail_reboot != NULL &&
           api.fail_recovery != NULL;
}

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];

static void root_to_scratch(void)
{
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);
}

static void root_restore(void)
{
    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
}

static bool exists_under_root(const char *abs_path)
{
    char path[ND_PATH_MAX];
    struct stat st;

    if (nd_snprintf(path, sizeof path, "%s%s", g_root, abs_path) != ND_OK)
        return false;
    return stat(path, &st) == 0;
}

/* ------------------------------------------------------------------ *
 * 1. The flag can be taken back
 * ------------------------------------------------------------------ */

static void test_the_flag_can_be_taken_back(void)
{
    root_to_scratch();

    CHECK(!exists_under_root(ND_POWER_RECOVERY_FLAG), "no flag to start with");
    CHECK_INT(api.request_flag(NULL, 0u), ND_OK, "the flag is written");
    CHECK(exists_under_root(ND_POWER_RECOVERY_FLAG), "and it is where the initramfs looks");

    CHECK_INT(api.clear_flag(), ND_OK, "it can be taken back");
    CHECK(!exists_under_root(ND_POWER_RECOVERY_FLAG), "and it is gone");

    root_restore();
}

/* ------------------------------------------------------------------ *
 * 2. Taking back a flag that is not there is success
 * ------------------------------------------------------------------ *
 *
 * Not pedantry. The caller runs this on EVERY failed recovery restart, and one
 * of the ways the restart can fail is that the flag was never written in the
 * first place (a read-only user partition). A clear that reported ENOENT as an
 * error would put a second, wrong dialog in front of the owner, and would log
 * an error line on a phone where nothing had gone wrong here.
 */
static void test_taking_back_nothing_is_not_a_failure(void)
{
    root_to_scratch();

    CHECK(!exists_under_root(ND_POWER_RECOVERY_FLAG), "nothing there");
    CHECK_INT(api.clear_flag(), ND_OK, "clearing nothing succeeds");
    CHECK_INT(api.clear_flag(), ND_OK, "and again");

    root_restore();
}

/* ------------------------------------------------------------------ *
 * 3. It takes back the FLAG and nothing else
 * ------------------------------------------------------------------ *
 *
 * /NeoDCT/User/.ndsys is not the recovery flag's directory, it is the state
 * directory: pending.prop and last_result.prop live there, and pending.prop is
 * a staged update that the initramfs is going to install. A cleanup that
 * removed the directory -- or that was written as "clear the recovery state"
 * rather than "unlink one file" -- would throw a downloaded, verified,
 * committed update away as a side effect of a failed reboot.
 *
 * That combination is not hypothetical: with the halt broken, the Update app's
 * restart failed too, so both records were sitting in that directory at the
 * moment this code runs.
 */
static void test_it_takes_back_only_the_flag(void)
{
    char path[ND_PATH_MAX];
    FILE *f;

    root_to_scratch();

    CHECK_INT(api.request_flag(NULL, 0u), ND_OK, "the flag is written");
    CHECK_INT(nd_path_resolve(path, sizeof path, ND_POWER_STATE_DIR "/pending.prop"), ND_OK,
              "a staged update beside it");
    f = fopen(path, "w");
    CHECK(f != NULL, "the staging record was created");
    if (f != NULL) {
        (void)fputs("version=0.5.9b\n", f);
        (void)fclose(f);
    }

    CHECK_INT(api.clear_flag(), ND_OK, "the flag is taken back");
    CHECK(!exists_under_root(ND_POWER_RECOVERY_FLAG), "the flag is gone");
    CHECK(exists_under_root(ND_POWER_STATE_DIR), "the state directory is NOT");
    CHECK(exists_under_root(ND_POWER_STATE_DIR "/pending.prop"),
          "and the staged update is still there");

    root_restore();
}

/* ------------------------------------------------------------------ *
 * 4. The owner is told something different, because something different
 *    happened
 * ------------------------------------------------------------------ *
 *
 * "Reboot failed." is true of a plain Reboot and is the whole story there. On
 * the Recovery path it is half the story: a file was written and then removed,
 * and the request the owner made is no longer pending. Saying only "Reboot
 * failed." would leave them believing the phone would come up in recovery the
 * next time it started, which used to be true and deliberately is not any more.
 */
static void test_the_recovery_failure_says_the_request_is_off(void)
{
    CHECK_STR(*api.fail_reboot, "Reboot failed.", "the plain reboot failure is unchanged");
    CHECK(*api.fail_recovery != NULL && strstr(*api.fail_recovery, "cancelled") != NULL,
          "the recovery failure says the request was cancelled");
    CHECK(strcmp(*api.fail_recovery, *api.fail_reboot) != 0,
          "and it is not the same sentence as the plain one");
}

int main(void)
{
    void *h = sa_begin("Power", "ndpowerapp");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndpowerapp-root", g_root, sizeof g_root)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_the_flag_can_be_taken_back);
    RUN(test_taking_back_nothing_is_not_a_failure);
    RUN(test_it_takes_back_only_the_flag);
    RUN(test_the_recovery_failure_says_the_request_is_off);

    rc = sa_end(h, "test_power_app");
    sa_rmtree(g_root);
    return rc;
}
