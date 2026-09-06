/* power.h -- the parts of the Power app a unit test can reach.
 *
 * System/apps/Power/main.py is 103 lines: a three-item menu, a confirmation
 * dialog, and three ways of ending the session. Almost all of it is widget
 * calls; what is left is the command-candidate walk, the recovery flag and
 * the two strings that name them.
 *
 * test/unit/test_power.c dlopen()s the BUILT app.so and dlsym()s these, the
 * way test_cubebench.c and test_phonebook.c do.
 *
 * ============ THE HALT MOVED OUT OF THIS APP ============
 *
 * nd_power_which(), nd_power_spawn_first(), nd_power_halt_commands and
 * nd_power_reboot_commands are gone. Resolving a program name along $PATH
 * and fork/exec'ing it was the widest thing any app on this phone did, and
 * the only reason this one needed privilege the others do not, so it now
 * happens in the CORE: nd_power_go_down() sends ND_SVC_OP_REBOOT or
 * ND_SVC_OP_POWEROFF over the service channel and the core resolves, syncs
 * and spawns. The tables and the lookup live in nd_svc.h as
 * nd_svc_reboot_commands / nd_svc_poweroff_commands / nd_svc_halt_which(),
 * with test/unit/test_svc.c the place their tests moved to.
 * docs/c-rewrite/spec-app-services.md section 9.
 *
 * This app now spawns nothing at all, and `sync` with it: the core syncs
 * between answering and spawning, which is closer to the halt than either of
 * the two syncs this app used to run.
 *
 * ============ WHAT THE TEST DELIBERATELY DOES NOT CALL ============
 *
 * nd_power_go_down(). In a process with no service channel -- which a unit
 * test is -- nd_svc_reboot() does the halt itself, because a process with no
 * core to ask IS the core as far as that library can tell. On a developer's
 * machine that is a real poweroff, and a test suite that can switch off the
 * machine it is running on is not a test suite. What is reachable and is
 * tested here is nd_power_request_recovery_flag(); the composition is not,
 * and that is a deliberate hole named here rather than left for someone to
 * discover. test_svc.c reaches the same code with the spawn injected out.
 */

#ifndef ND_POWER_H_INCLUDED
#define ND_POWER_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP_ID = 971 -- manifest.json, and the menu VerticalList's app_id. */
#define ND_POWER_APP_ID 971

/* Must match staging.STATE_DIR and ndsys-recovery.sh RECOVERY_FLAG. Written
 * from the running system, read by the initramfs as /mnt/user/.ndsys.
 *
 * ABSOLUTE, and load-bearing: CODING-STANDARDS.md section 9.5. Both go
 * through nd_path_resolve(), so a test can point ND_ROOT somewhere harmless
 * and the initramfs still sees /NeoDCT/User/.ndsys on the phone. */
#define ND_POWER_STATE_DIR     "/NeoDCT/User/.ndsys"
#define ND_POWER_RECOVERY_FLAG "/NeoDCT/User/.ndsys/boot_recovery"

/* MENU, and POWER_OFF / REBOOT / RECOVERY. */
#define ND_POWER_MENU_ITEMS 3
extern const char *const nd_power_menu[ND_POWER_MENU_ITEMS];

#define ND_POWER_OFF      0
#define ND_POWER_REBOOT   1
#define ND_POWER_RECOVERY 2

/* The two confirmation questions and the three failure messages, so a test
 * can pin the strings a user reads without driving the widgets. */
extern const char *const nd_power_ask_off;
extern const char *const nd_power_ask_reboot;
extern const char *const nd_power_ask_recovery;
extern const char *const nd_power_fail_off;
extern const char *const nd_power_fail_reboot;
/* The recovery path's own failure, which is a different sentence because a
 * different thing happened: the flag was written and then taken back. */
extern const char *const nd_power_fail_recovery;

/* ------------------------------------------------------------------ *
 * The pieces underneath
 * ------------------------------------------------------------------ */

/* _request_recovery()'s filesystem half: mkdir -p the state directory and
 * create the one-shot flag. ND_OK on success; on failure the caller shows
 * "Cannot ask for recovery: <why>" and does NOT reboot.
 *
 * `why`/`why_sz` receive strerror() text, which stands in for the Python's
 * str(OSError). Pass NULL to discard it. */
#define ND_POWER_WHY_MAX 160
nd_err nd_power_request_recovery_flag(char *why, size_t why_sz);

/* And the other half of the pair: remove the flag again.
 *
 * The flag is a REQUEST that the initramfs consumes on the next boot, so one
 * left behind by a restart that never started is a booby trap -- the next boot
 * from any cause lands in recovery. _request_recovery() calls this when
 * nd_power_go_down() comes back false. ND_OK when the flag is gone, which
 * includes it never having been there. */
nd_err nd_power_clear_recovery_flag(void);

/* ------------------------------------------------------------------ *
 * The screens
 * ------------------------------------------------------------------ */

/* _confirm(ui, question): a MessageDialog titled "Power" with a "Yes" button.
 * True when it was dismissed with ENTER. */
bool nd_power_confirm(nd_ui *ui, const char *question);

/* _tell(ui, message): the same dialog with an "OK" button, answer ignored. */
void nd_power_tell(nd_ui *ui, const char *message);

/* _go_down(ui, reboot, failure). See the header comment: NOT called by any
 * test. Asks the core for a halt -- `reboot` picks which of the two verbs --
 * and on false draws `failure`. On true it sits still for thirty seconds, so
 * the key does not look like it did nothing while init brings the phone
 * down. The sync the Python did here is the core's now.
 *
 * TRUE means the halt started. It used to return void, and the caller that
 * needed the answer was the one that had already written the recovery flag:
 * with no way to see the failure it left the flag on the disk, arming the next
 * boot for recovery. Pass NULL for `failure` to draw no dialog, which is what
 * that caller does so it can say something more specific afterwards. */
bool nd_power_go_down(nd_ui *ui, bool reboot, const char *failure);

#ifdef __cplusplus
}
#endif

#endif /* ND_POWER_H_INCLUDED */
