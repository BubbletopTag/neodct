/* test_power.c -- the Power app, app id 971.
 *
 * ============ WHAT THIS TEST WILL NOT DO ============
 *
 * IT NEVER CALLS nd_power_go_down(). That function runs `sync` and then the
 * first of `poweroff`, `/sbin/poweroff`, `busybox poweroff` that exists. On a
 * developer's machine, and on a CI runner, that is a real poweroff. A test
 * suite that can switch off the machine it is running on is not a test suite,
 * so the composition is left alone and the three pieces underneath it are
 * tested instead -- the PATH lookup, the candidate walk over commands that do
 * not exist, and the recovery flag. power.h says the same thing, so the hole
 * is named in both places rather than discovered.
 *
 * For the same reason app_run() is only ever driven with Back on the first
 * screen. Choosing a menu item and confirming it would reach _go_down.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. MENU, the two command tables and the five user-facing strings are the
 *     Python's, in the Python's order. `poweroff` before `/sbin/poweroff` is
 *     load-bearing: an image with both has two different programs there.
 *
 *  2. nd_power_which() is execvp's lookup -- $PATH for a bare name, the path
 *     itself when it contains a slash, and false (Python's OSError) when
 *     nothing executable is found.
 *
 *  3. nd_power_spawn_first() returns false when NO candidate exists, which is
 *     the branch that puts "Power off failed." on the screen. Driven with a
 *     table of invented names so nothing is ever spawned.
 *
 *  4. The recovery flag lands at /NeoDCT/User/.ndsys/boot_recovery, is
 *     created empty, and is created again over an existing one -- the
 *     initramfs deletes it as it reads it, so a second request has to be able
 *     to write a second flag. When the directory cannot be made, the failure
 *     is reported with a reason rather than silently rebooting into an
 *     ordinary boot.
 *
 *  5. _confirm() is Yes only on ENTER: the dialog's cancel key returns false.
 *
 *  6. Back on the menu returns 0 from app_run().
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (for the
 * font); the scratch root is this test's own.
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
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*which)(const char *, char *, size_t);
    bool (*spawn_first)(const char *const *const *, size_t);
    nd_err (*recovery_flag)(char *, size_t);
    bool (*confirm)(nd_ui *, const char *);
    void (*tell)(nd_ui *, const char *);
    const char *const *menu;
    const char *const *const *halt;
    const char *const *const *reboot;
    const char *const *ask_off;
    const char *const *ask_reboot;
    const char *const *ask_recovery;
    const char *const *fail_off;
    const char *const *fail_reboot;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.which = sa_sym(h, "nd_power_which");
    *(void **)&api.spawn_first = sa_sym(h, "nd_power_spawn_first");
    *(void **)&api.recovery_flag = sa_sym(h, "nd_power_request_recovery_flag");
    *(void **)&api.confirm = sa_sym(h, "nd_power_confirm");
    *(void **)&api.tell = sa_sym(h, "nd_power_tell");
    api.menu = dlsym(h, "nd_power_menu");
    api.halt = dlsym(h, "nd_power_halt_commands");
    api.reboot = dlsym(h, "nd_power_reboot_commands");
    api.ask_off = dlsym(h, "nd_power_ask_off");
    api.ask_reboot = dlsym(h, "nd_power_ask_reboot");
    api.ask_recovery = dlsym(h, "nd_power_ask_recovery");
    api.fail_off = dlsym(h, "nd_power_fail_off");
    api.fail_reboot = dlsym(h, "nd_power_fail_reboot");

    return api.run != NULL && api.shutdown != NULL && api.which != NULL &&
           api.spawn_first != NULL && api.recovery_flag != NULL && api.confirm != NULL &&
           api.tell != NULL && api.menu != NULL && api.halt != NULL && api.reboot != NULL &&
           api.ask_off != NULL && api.ask_reboot != NULL && api.ask_recovery != NULL &&
           api.fail_off != NULL && api.fail_reboot != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. The tables and the strings
 * ------------------------------------------------------------------ */

static void test_tables(void)
{
    CHECK_STR(api.menu[0], "Power off", "MENU[POWER_OFF]");
    CHECK_STR(api.menu[1], "Reboot", "MENU[REBOOT]");
    CHECK_STR(api.menu[2], "Recovery", "MENU[RECOVERY]");
    CHECK_INT(ND_POWER_OFF, 0, "POWER_OFF");
    CHECK_INT(ND_POWER_REBOOT, 1, "REBOOT");
    CHECK_INT(ND_POWER_RECOVERY, 2, "RECOVERY");
    CHECK_INT(ND_POWER_APP_ID, 971, "APP_ID");

    /* _HALT_COMMANDS, in order. The order is what decides which of two
     * different programs runs on an image that has both. */
    CHECK_STR(api.halt[0][0], "poweroff", "halt 0");
    CHECK(api.halt[0][1] == NULL, "halt 0 is one word");
    CHECK_STR(api.halt[1][0], "/sbin/poweroff", "halt 1");
    CHECK_STR(api.halt[2][0], "busybox", "halt 2 argv[0]");
    CHECK_STR(api.halt[2][1], "poweroff", "halt 2 argv[1]");
    CHECK(api.halt[2][2] == NULL, "halt 2 is two words");

    CHECK_STR(api.reboot[0][0], "reboot", "reboot 0");
    CHECK_STR(api.reboot[1][0], "/sbin/reboot", "reboot 1");
    CHECK_STR(api.reboot[2][0], "busybox", "reboot 2 argv[0]");
    CHECK_STR(api.reboot[2][1], "reboot", "reboot 2 argv[1]");

    CHECK_STR(*api.ask_off, "Switch the phone off?", "_confirm text, Power off");
    CHECK_STR(*api.ask_reboot, "Restart the phone?", "_confirm text, Reboot");
    CHECK_STR(*api.ask_recovery, "Restart into recovery?", "_confirm text, Recovery");
    CHECK_STR(*api.fail_off, "Power off failed.", "_go_down failure, Power off");
    CHECK_STR(*api.fail_reboot, "Reboot failed.", "_go_down failure, Reboot");

    /* The two paths the initramfs reads. Absolute and load-bearing. */
    CHECK_STR(ND_POWER_STATE_DIR, "/NeoDCT/User/.ndsys", "STATE_DIR");
    CHECK_STR(ND_POWER_RECOVERY_FLAG, "/NeoDCT/User/.ndsys/boot_recovery", "RECOVERY_FLAG");
}

/* ------------------------------------------------------------------ *
 * 2. nd_power_which()
 * ------------------------------------------------------------------ */

static char g_bindir[ND_PATH_MAX];

/* An executable that does nothing, so a lookup can succeed without anything
 * being spawned. */
static bool make_stub_program(const char *dir, const char *name)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/%s", dir, name) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fputs("#!/bin/sh\nexit 0\n", f);
    (void)fclose(f);
    return chmod(path, 0755) == 0;
}

static void test_which(void)
{
    char out[ND_PATH_MAX];
    char expect[ND_PATH_MAX];
    char abs_path[ND_PATH_MAX];
    const char *saved_path = getenv("PATH");
    char keep[ND_PATH_MAX];

    (void)nd_strlcpy(keep, (saved_path != NULL) ? saved_path : "", sizeof keep);

    CHECK(make_stub_program(g_bindir, "nd-fake-halt"), "stub program written");
    (void)setenv("PATH", g_bindir, 1);

    CHECK(api.which("nd-fake-halt", out, sizeof out), "a bare name is found along $PATH");
    (void)nd_snprintf(expect, sizeof expect, "%s/nd-fake-halt", g_bindir);
    CHECK_STR(out, expect, "and the answer is the full path");

    CHECK(!api.which("nd-there-is-no-such-program", out, sizeof out),
          "a missing name is Python's OSError");

    /* A name containing a slash is a path, not a name -- execvp's rule. */
    (void)nd_snprintf(abs_path, sizeof abs_path, "%s/nd-fake-halt", g_bindir);
    CHECK(api.which(abs_path, out, sizeof out), "an absolute path is used as given");
    CHECK_STR(out, abs_path, "unchanged");
    CHECK(!api.which("/nd/no/such/path", out, sizeof out), "a missing absolute path is refused");

    /* A file that is not executable does not count, which is what stops a
     * README called `reboot` being run. */
    {
        char plain[ND_PATH_MAX];
        FILE *f;

        (void)nd_snprintf(plain, sizeof plain, "%s/nd-not-exec", g_bindir);
        f = fopen(plain, "w");
        if (f != NULL) {
            (void)fputs("not a program\n", f);
            (void)fclose(f);
            (void)chmod(plain, 0644);
        }
        CHECK(!api.which("nd-not-exec", out, sizeof out), "a non-executable file is not a program");
    }

    CHECK(!api.which(NULL, out, sizeof out), "NULL is refused");
    CHECK(!api.which("", out, sizeof out), "the empty name is refused");

    (void)setenv("PATH", keep, 1);
}

/* ------------------------------------------------------------------ *
 * 3. nd_power_spawn_first()
 * ------------------------------------------------------------------ */

static void test_spawn_first_with_nothing_to_spawn(void)
{
    static const char *const A[] = {"nd-no-such-halt-a", NULL};
    static const char *const B[] = {"/nd/no/such/halt-b", NULL};
    static const char *const C[] = {"nd-no-such-halt-c", "poweroff", NULL};
    static const char *const *const NONE[3] = {A, B, C};

    /* Every candidate is invented, so nothing is spawned and the answer is
     * the false that puts "Power off failed." on the screen. */
    CHECK(!api.spawn_first(NONE, 3u), "no candidate exists -> false");
    CHECK(!api.spawn_first(NULL, 3u), "NULL table -> false");
    CHECK(!api.spawn_first(NONE, 0u), "an empty table -> false");
}

/* ------------------------------------------------------------------ *
 * 4. The recovery flag
 * ------------------------------------------------------------------ */

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

static bool flag_exists(off_t *size_out)
{
    char path[ND_PATH_MAX];
    struct stat st;

    if (nd_snprintf(path, sizeof path, "%s%s", g_root, ND_POWER_RECOVERY_FLAG) != ND_OK)
        return false;
    if (stat(path, &st) != 0)
        return false;
    if (size_out != NULL)
        *size_out = st.st_size;
    return S_ISREG(st.st_mode);
}

static void test_recovery_flag(void)
{
    char why[ND_POWER_WHY_MAX];
    off_t size = -1;

    root_to_scratch();

    CHECK(!flag_exists(NULL), "no flag to start with");
    CHECK_INT(api.recovery_flag(why, sizeof why), ND_OK, "the flag is written");
    CHECK_STR(why, "", "and nothing is reported");
    CHECK(flag_exists(&size), "the flag is where the initramfs looks for it");
    CHECK_INT(size, 0, "it is empty -- only its existence is read");

    /* "the applier deletes the flag as it reads it, so a recovery boot can
     * never repeat itself" -- which means asking twice has to work. */
    CHECK_INT(api.recovery_flag(NULL, 0u), ND_OK, "asking again succeeds");
    CHECK(flag_exists(NULL), "and the flag is there again");

    root_restore();
}

static void test_recovery_flag_failure(void)
{
    char blocked[ND_PATH_MAX];
    char why[ND_POWER_WHY_MAX];
    char user[ND_PATH_MAX];
    FILE *f;

    /* A fresh root whose .ndsys is a FILE, so mkdir -p cannot make it. That
     * stands in for the read-only user partition the Python's comment is
     * about: "without it there is nowhere to leave the flag, so say so
     * rather than rebooting into an ordinary boot and looking broken." */
    if (!sa_tmpdir("ndpower-ro", blocked, sizeof blocked)) {
        CHECK(false, "scratch root");
        return;
    }
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(blocked);

    (void)nd_snprintf(user, sizeof user, "%s/NeoDCT/User", blocked);
    CHECK_INT(nd_mkdir_p("/NeoDCT/User", 0755u), ND_OK, "the user directory exists");
    (void)nd_snprintf(user, sizeof user, "%s%s", blocked, ND_POWER_STATE_DIR);
    f = fopen(user, "w");
    if (f != NULL)
        (void)fclose(f);

    why[0] = 'x';
    CHECK(api.recovery_flag(why, sizeof why) != ND_OK, "a blocked state directory is reported");
    CHECK(why[0] != 'x' && why[0] != '\0', "with a reason in it");

    root_restore();
    sa_rmtree(blocked);
}

/* ------------------------------------------------------------------ *
 * 5 and 6. The screens
 * ------------------------------------------------------------------ */

static void test_confirm(void)
{
    sa_fixture fx;

    /* MessageDialog drains the channel before drawing, so the answer has to
     * arrive as a repeat. ENTER is Yes. */
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    CHECK(api.confirm(&fx.ui, *api.ask_off), "ENTER is Yes");
    nd_vclock_disable();
    sa_fx_free(&fx);

    /* And CLEAR is No. Anything else is ignored, which is why this is the
     * only pair worth checking: the dialog's key sets are its own and are
     * pinned by test_widgets_dialogs.c. */
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(sa_hold(&fx, ND_KEY_CLEAR), "held CLEAR");
    nd_vclock_enable();
    CHECK(!api.confirm(&fx.ui, *api.ask_reboot), "CLEAR is No");
    /* _tell() ignores the answer entirely; it must simply return. */
    api.tell(&fx.ui, *api.fail_off);
    nd_vclock_disable();
    sa_checks++;
    sa_fx_free(&fx);
}

static void test_back_leaves(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* VerticalList does NOT flush before its first draw, so a queued press is
     * enough here -- and Back is the one answer that cannot reach _go_down. */
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    nd_vclock_disable();

    CHECK_INT(rc, 0, "Back on the menu returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the menu");
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("Power", "ndpower");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndpower-root", g_root, sizeof g_root) ||
        !sa_tmpdir("ndpower-bin", g_bindir, sizeof g_bindir)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_tables);
    RUN(test_which);
    RUN(test_spawn_first_with_nothing_to_spawn);
    RUN(test_recovery_flag);
    RUN(test_recovery_flag_failure);
    RUN(test_confirm);
    RUN(test_back_leaves);
    RUN(test_null_safety);

    rc = sa_end(h, "test_power");
    sa_rmtree(g_root);
    sa_rmtree(g_bindir);
    return rc;
}
