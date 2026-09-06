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
 * ============ THIS APP NO LONGER SWITCHES THE PHONE OFF ITSELF ============
 *
 * The Python resolved "poweroff" along $PATH and Popen'd it, and so did the
 * first C port. That was the widest thing any app on this phone did, and the
 * only reason this one, Update and Downgrade needed privilege the other
 * twenty-two do not -- reboot(8) is reboot(2) with a wrapper, and reboot(2)
 * wants CAP_SYS_BOOT.
 *
 * So _go_down() now ASKS THE CORE, over the service channel every app child
 * already has (nd_svc.h). The request carries no arguments, so there is no
 * program name for this process to choose and no $PATH for anything to
 * poison; the candidate tables and the execvp lookup that used to live here
 * are in lib/nd_svc.c, where the core is the only thing that reads them.
 * spec-app-services.md section 9.
 *
 * What that costs, and what it does not:
 *
 *   - The two failure dialogs are unchanged, and false still means the same
 *     thing it always did: the halt did not start. It now covers "no
 *     poweroff on this image", "the core refused" and "no answer from the
 *     core", which are honestly one sentence.
 *   - subprocess.call(["sync"]) IS GONE FROM THIS FILE, both copies of it.
 *     The core syncs between answering and spawning, which is strictly
 *     closer to the halt than either of ours was, and it is sync(2) rather
 *     than a spawned `sync` -- so OPEN-QUESTIONS.md PW-2, "what if the image
 *     has no /bin/sync", stops being a question here. 9.5.
 *   - THE RECOVERY FLAG IS STILL WRITTEN BY THIS PROCESS. It is an ordinary
 *     empty file on the one writable partition, which every app may write; a
 *     "create this file" verb on that channel would be a wider hole than the
 *     one being closed. 9.6.
 *   - This app now spawns nothing at all.
 *
 * ============ ONE PLACE THE C STILL HAD TO SAY SOMETHING ============
 *
 * str(OSError) IS NOT REPRODUCIBLE. "Cannot ask for recovery: %s" % exc
 * prints "[Errno 30] Read-only file system: '/NeoDCT/User/.ndsys'". The C
 * prints strerror(errno) after the same colon. Same message, same place,
 * different words for the same failure. OPEN-QUESTIONS.md PW-3.
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
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "power.h"

/* ------------------------------------------------------------------ *
 * The tables and the strings
 * ------------------------------------------------------------------ */

const char *const nd_power_menu[ND_POWER_MENU_ITEMS] = {"Power off", "Reboot", "Recovery"};

const char *const nd_power_ask_off = "Switch the phone off?";
const char *const nd_power_ask_reboot = "Restart the phone?";
const char *const nd_power_ask_recovery = "Restart into recovery?";
const char *const nd_power_fail_off = "Power off failed.";
const char *const nd_power_fail_reboot = "Reboot failed.";
/* Shown only when the recovery flag was written and the restart then did not
 * start. See request_recovery(): the flag is a booby trap if it is left there,
 * so it is removed and the owner is told the request is off rather than being
 * left to discover it at the next battery pull. */
const char *const nd_power_fail_recovery = "Reboot failed. The recovery request was cancelled.";

/* The dialogs' shared title and buttons, spelled once. */
static const char *const POWER_TITLE = "Power";

/* time.sleep(30): "init takes a moment to bring everything down. Sit here
 * instead of returning to the launcher, which would look like the key did
 * nothing." */
#define POWER_DOWN_DWELL 30.0

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

/* ============ AND THE OTHER HALF, WHICH WAS MISSING ============
 *
 * The flag is a REQUEST, not a record: ndsys-recovery.sh tests only that the
 * file exists and deletes it as it reads it. So a flag written for a restart
 * that never happened does not expire -- it sits on the user partition and
 * redirects the NEXT boot from any cause at all into the recovery menu. Pull
 * the battery a week later and the phone comes up in recovery with no
 * explanation, which is exactly the report this fixes.
 *
 * ENOENT is success: there is nothing to take back. */
nd_err nd_power_clear_recovery_flag(void)
{
    char flag[ND_PATH_MAX];

    if (nd_path_resolve(flag, sizeof flag, ND_POWER_RECOVERY_FLAG) != ND_OK)
        return ND_ERR_TOOLONG;
    if (unlink(flag) != 0 && errno != ENOENT) {
        nd_log_err(ND_LOG_POWER, "cannot take back the recovery request: %s", strerror(errno));
        return ND_ERR_IO;
    }
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

/* ============ WHY THIS RETURNS SOMETHING NOW ============
 *
 * It was void, and one caller badly needed the answer: request_recovery()
 * writes a one-shot flag BEFORE asking for the restart, and if the restart
 * does not start, that flag stays on the disk and quietly redirects the next
 * boot by any means into recovery. A caller that cannot see the failure cannot
 * undo what it did in front of it. */
bool nd_power_go_down(nd_ui *ui, bool reboot, const char *failure)
{
    /* The whole of _go_down's body, in one line, in another process. The
     * core resolves the binary, ANSWERS, syncs and only then spawns -- which
     * is why waiting the ordinary five seconds for that answer is safe, and
     * why the two sync() calls that used to be in this file are not missed.
     * spec-app-services.md 9.4 and 9.5. */
    if (!(reboot ? nd_svc_reboot() : nd_svc_poweroff())) {
        if (failure != NULL)
            nd_power_tell(ui, failure);
        return false;
    }
    dwell(POWER_DOWN_DWELL);
    return true;
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

    /* The Python ran subprocess.call(["sync"]) here and _go_down ran it a
     * SECOND time, because neither knew about the other. Neither is
     * reproduced. The flag was written and CLOSED above, so its bytes are
     * already in the kernel; the core's sync(2) -- which happens after it
     * answers and before it spawns -- is what puts them on the flash. One
     * sync instead of two, and later, and closer to the halt than either of
     * ours could be. spec-app-services.md 9.5. */
    if (nd_power_go_down(ui, true, NULL)) /* NULL: the dialog is drawn below */
        return;

    /* THE RESTART DID NOT START, AND THE FLAG IS STILL THERE.
     *
     * The initramfs treats the file's existence as the whole request and
     * deletes it as it reads it, so leaving it behind arms every future boot
     * -- a battery pull, an update, a crash -- to land in the recovery menu
     * for a reason nobody will connect to a key pressed days earlier. Take it
     * back, and say so in the same breath as the failure, because "Reboot
     * failed." on its own would leave the owner believing nothing happened.
     *
     * The clear is best-effort by construction: if it fails, the phone is in
     * the state it would have been in anyway and the log line says why. */
    (void)nd_power_clear_recovery_flag();
    nd_power_tell(ui, nd_power_fail_recovery);
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
        nd_vlist_init(&menu, ui, POWER_TITLE, nd_power_menu, ND_POWER_MENU_ITEMS, ND_POWER_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);

        if (choice < 0)
            return 0;
        if (choice == ND_POWER_OFF) {
            if (nd_power_confirm(ui, nd_power_ask_off))
                nd_power_go_down(ui, false, nd_power_fail_off);
        } else if (choice == ND_POWER_REBOOT) {
            if (nd_power_confirm(ui, nd_power_ask_reboot))
                nd_power_go_down(ui, true, nd_power_fail_reboot);
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

/* This app spawns nothing at all now -- the halt is the CORE's child, not
 * ours, which is the whole point of section 9 -- and it holds no file, no
 * socket and no allocation that outlives a screen. So there is nothing to
 * release, and the symbol exists because nd_app.h requires every app to
 * export one. */
void app_shutdown(void) {}
