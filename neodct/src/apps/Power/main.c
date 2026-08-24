/* apps/Power/main.c -- the power menu, app id 971.
 *
 * A one-to-one port of System/apps/Power/main.py (103 lines). Its docstring
 * is the specification and is reproduced rather than summarised, because the
 * recovery mechanism is a contract with a program that runs before this one
 * exists:
 *
 *     Power menu: switch off, restart, or restart into recovery.
 *
 *     Recovery is not a thing the running system can enter by itself -- the
 *     initramfs owns that decision, before any partition is mounted. The
 *     phone asks for it by leaving a one-shot flag on the user partition
 *     (ndsys-recovery.sh RECOVERY_FLAG) and rebooting; the applier deletes
 *     the flag as it reads it, so a recovery boot can never repeat itself.
 *
 * ============ NO GOLDEN FRAME, AND WHY THAT IS NOT A LICENCE ============
 *
 * shoot_docs.py never launches this app, so nothing in tests/golden pins its
 * pixels. That makes the widget calls MORE important to get literally right,
 * not less: the menu is a VerticalList with app_id 971 preceded by a
 * non-presenting "Select" bar, which is the same two lines PhoneBook and
 * Tones use and which golden/widget-verticallist.png does pin. Anything that
 * drifts here drifts away from a frame that is checked elsewhere.
 *
 * ============ THE MENU IS REBUILT EVERY ITERATION ============
 *
 * Unlike PhoneBook, whose `while True:` constructs its VerticalList ONCE so
 * the cursor survives a trip into a submenu, this loop constructs a new one
 * per pass. Come back from a cancelled "Switch the phone off?" and the
 * cursor is on "Power off" again rather than where you left it. That is
 * visible on screen and it is the Python's.
 *
 * ============ THREE PLACES THE C HAD TO SAY SOMETHING ============
 *
 * 1. subprocess.Popen(["poweroff"]) LOOKS ALONG $PATH. nd_proc_spawn() takes
 *    a path, so nd_power_which() does the execvp lookup first and its failure
 *    is the OSError the Python's `except OSError: continue` catches.
 *
 * 2. subprocess.call(["sync"]) WAITS. nd_proc_wait() with a blocking timeout,
 *    and -- unlike the Python -- a `sync` that cannot be found is not fatal:
 *    Python's subprocess.call would raise FileNotFoundError straight out of
 *    _go_down and reach the crash screen. Reproducing that would mean an
 *    image without /bin/sync showing a stack trace instead of switching off,
 *    which is worse in every way; the C logs it and carries on to the halt.
 *    OPEN-QUESTIONS.md PW-2.
 *
 * 3. str(OSError) IS NOT REPRODUCIBLE. "Cannot ask for recovery: %s" % exc
 *    prints "[Errno 30] Read-only file system: '/NeoDCT/User/.ndsys'". The C
 *    prints strerror(errno) after the same colon. Same message, same place,
 *    different words for the same failure. OPEN-QUESTIONS.md PW-3.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "power.h"

/* ------------------------------------------------------------------ *
 * The tables and the strings
 * ------------------------------------------------------------------ */

const char *const nd_power_menu[ND_POWER_MENU_ITEMS] = {"Power off", "Reboot", "Recovery"};

static const char *const HALT_0[] = {"poweroff", NULL};
static const char *const HALT_1[] = {"/sbin/poweroff", NULL};
static const char *const HALT_2[] = {"busybox", "poweroff", NULL};
static const char *const REBOOT_0[] = {"reboot", NULL};
static const char *const REBOOT_1[] = {"/sbin/reboot", NULL};
static const char *const REBOOT_2[] = {"busybox", "reboot", NULL};

const char *const *const nd_power_halt_commands[ND_POWER_CANDIDATES] = {HALT_0, HALT_1, HALT_2};
const char *const *const nd_power_reboot_commands[ND_POWER_CANDIDATES] = {REBOOT_0, REBOOT_1,
                                                                          REBOOT_2};

const char *const nd_power_ask_off = "Switch the phone off?";
const char *const nd_power_ask_reboot = "Restart the phone?";
const char *const nd_power_ask_recovery = "Restart into recovery?";
const char *const nd_power_fail_off = "Power off failed.";
const char *const nd_power_fail_reboot = "Reboot failed.";

/* The dialogs' shared title and buttons, spelled once. */
static const char *const POWER_TITLE = "Power";

/* time.sleep(30): "init takes a moment to bring everything down. Sit here
 * instead of returning to the launcher, which would look like the key did
 * nothing." */
#define POWER_DOWN_DWELL 30.0

/* ------------------------------------------------------------------ *
 * execvp's half of subprocess.Popen
 * ------------------------------------------------------------------ */

bool nd_power_which(const char *name, char *out, size_t out_sz)
{
    const char *path;
    const char *seg;

    if (name == NULL || name[0] == '\0' || out == NULL || out_sz == 0u)
        return false;

    /* execvp: a name containing a slash is a path, not a name. */
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) != 0)
            return false;
        return (size_t)snprintf(out, out_sz, "%s", name) < out_sz;
    }

    path = getenv("PATH");
    /* execvp's own fallback when PATH is unset is confstr(_CS_PATH), which is
     * this on every libc the phone or the test host uses. */
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

    for (seg = path; seg != NULL; ) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        int n;

        /* An empty element means the current directory, as it does to the
         * shell and to execvp. */
        if (len == 0u)
            n = snprintf(out, out_sz, "./%s", name);
        else
            n = snprintf(out, out_sz, "%.*s/%s", (int)len, seg, name);

        if (n > 0 && (size_t)n < out_sz && access(out, X_OK) == 0)
            return true;

        seg = (colon != NULL) ? colon + 1 : NULL;
    }

    out[0] = '\0';
    return false;
}

/* subprocess.Popen(command): no descriptor plan, so the child keeps this
 * process's stdout and stderr exactly as Popen's defaults do. */
static bool spawn_inherit(const char *const *argv, pid_t *pid_out)
{
    char exe[ND_PATH_MAX];
    nd_proc_spec spec;

    if (!nd_power_which(argv[0], exe, sizeof exe))
        return false;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    spec.n_fds = 0u;
    return nd_proc_spawn(exe, &spec, pid_out) == ND_OK;
}

bool nd_power_spawn_first(const char *const *const *candidates, size_t n)
{
    size_t i;

    if (candidates == NULL)
        return false;
    for (i = 0u; i < n; i++) {
        pid_t pid = -1;

        if (spawn_inherit(candidates[i], &pid))
            return true;
        /* `except OSError: continue` */
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * The recovery flag
 * ------------------------------------------------------------------ */

nd_err nd_power_request_recovery_flag(char *why, size_t why_sz)
{
    char flag[ND_PATH_MAX];
    nd_err rc;
    int fd;

    if (why != NULL && why_sz > 0u)
        why[0] = '\0';

    /* os.makedirs(STATE_DIR, exist_ok=True) */
    rc = nd_mkdir_p(ND_POWER_STATE_DIR, 0755u);
    if (rc != ND_OK) {
        if (why != NULL)
            (void)snprintf(why, why_sz, "%s", strerror(errno));
        return rc;
    }

    /* `with open(RECOVERY_FLAG, "w"): pass` -- create it, empty, truncating
     * whatever was there. The initramfs only tests for its existence. */
    if (nd_path_resolve(flag, sizeof flag, ND_POWER_RECOVERY_FLAG) != ND_OK)
        return ND_ERR_TOOLONG;
    fd = open(flag, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (why != NULL)
            (void)snprintf(why, why_sz, "%s", strerror(errno));
        return ND_ERR_IO;
    }
    (void)close(fd);
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * The screens
 * ------------------------------------------------------------------ */

bool nd_power_confirm(nd_ui *ui, const char *question)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, question);
    nd_msgdialog_set_title(&dialog, POWER_TITLE);
    nd_msgdialog_set_button(&dialog, "Yes");
    return nd_msgdialog_show(&dialog) == ND_KEY_ENTER;
}

void nd_power_tell(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, POWER_TITLE);
    nd_msgdialog_set_button(&dialog, "OK");
    (void)nd_msgdialog_show(&dialog);
}

/* time.sleep(seconds), in slices, so SIGTERM from the core's modem thread is
 * noticed rather than sat through. Skipped entirely under the virtual clock:
 * in capture mode time is a frame counter (nd_vclock.h) and a real sleep
 * moves no pixel, it only makes the oracle slower. Same rule as PhoneBook's
 * dwell(); OPEN-QUESTIONS.md PB-3. */
static void dwell(double seconds)
{
    double left = seconds;

    if (seconds <= 0.0 || nd_vclock_enabled())
        return;
    while (left > 0.0 && !nd_app_should_exit()) {
        struct timespec req;
        double slice = (left < 0.1) ? left : 0.1;

        req.tv_sec = 0;
        req.tv_nsec = (long)(slice * 1e9);
        (void)nanosleep(&req, NULL);
        left -= slice;
    }
}

/* ["sync"], the one command both _go_down and _request_recovery run. */
static const char *const SYNC_CMD[] = {"sync", NULL};

/* subprocess.call(["sync"]): spawn and block. A missing `sync` is logged and
 * survived; see the header comment. */
static void run_sync(void)
{
    pid_t pid = -1;

    if (spawn_inherit(SYNC_CMD, &pid))
        (void)nd_proc_wait(pid, -1.0, NULL);
    else
        nd_log_err(ND_LOG_POWER, "cannot run sync: %s", strerror(errno));
}

void nd_power_go_down(nd_ui *ui, const char *const *const *candidates, size_t n,
                      const char *failure)
{

    run_sync();

    if (!nd_power_spawn_first(candidates, n)) {
        nd_power_tell(ui, failure);
        return;
    }
    dwell(POWER_DOWN_DWELL);
}

/* _request_recovery(): leave the one-shot flag, then reboot into it. */
static void request_recovery(nd_ui *ui)
{
    char why[ND_POWER_WHY_MAX];
    char message[ND_POWER_WHY_MAX + 32];

    if (nd_power_request_recovery_flag(why, sizeof why) != ND_OK) {
        /* "A read-only or missing user partition is the interesting case:
         * without it there is nowhere to leave the flag, so say so rather
         * than rebooting into an ordinary boot and looking broken." */
        (void)snprintf(message, sizeof message, "Cannot ask for recovery: %s", why);
        nd_power_tell(ui, message);
        return;
    }

    /* The Python's subprocess.call(["sync"]) inside the try block, which
     * _go_down then runs a SECOND time. Both are reproduced: the flag has to
     * reach the disk before the reboot is asked for, and _go_down does not
     * know it was already synced. */
    run_sync();

    nd_power_go_down(ui, nd_power_reboot_commands, ND_POWER_CANDIDATES, nd_power_fail_reboot);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL)
        return 1;

    for (;;) {
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;

        /* A FRESH LIST PER PASS. See the header comment: the cursor does not
         * survive a submenu here, and that is the Python's behaviour. */
        nd_vlist_init(&menu, ui, POWER_TITLE, nd_power_menu, ND_POWER_MENU_ITEMS,
                      ND_POWER_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);

        if (choice < 0)
            return 0;
        if (choice == ND_POWER_OFF) {
            if (nd_power_confirm(ui, nd_power_ask_off))
                nd_power_go_down(ui, nd_power_halt_commands, ND_POWER_CANDIDATES,
                                 nd_power_fail_off);
        } else if (choice == ND_POWER_REBOOT) {
            if (nd_power_confirm(ui, nd_power_ask_reboot))
                nd_power_go_down(ui, nd_power_reboot_commands, ND_POWER_CANDIDATES,
                                 nd_power_fail_reboot);
        } else if (choice == ND_POWER_RECOVERY) {
            if (nd_power_confirm(ui, nd_power_ask_recovery))
                request_recovery(ui);
        }

        /* Not in the Python, which had exceptions to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* The children this app spawns are `sync` (already waited for) and the halt
 * command itself, which must NOT be killed on the way out -- it is the thing
 * that is switching the phone off. So there is nothing to release, and the
 * symbol exists because nd_app.h requires every app to export one. */
void app_shutdown(void) {}
