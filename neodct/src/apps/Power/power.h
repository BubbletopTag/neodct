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
 * ============ WHAT THE TEST DELIBERATELY DOES NOT CALL ============
 *
 * nd_power_go_down(). It runs `sync` and then the first of `poweroff`,
 * `/sbin/poweroff`, `busybox poweroff` that exists -- on a developer's
 * machine that is a real poweroff, and a test suite that can switch off the
 * machine it is running on is not a test suite. The pieces underneath it
 * (nd_power_which, nd_power_request_recovery) are reachable and are tested;
 * the composition is not, and that is a deliberate hole named here rather
 * than left for someone to discover.
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

/* _HALT_COMMANDS and _REBOOT_COMMANDS, flattened.
 *
 * "Same shape as Update/main.py _reboot: which binary exists, and where,
 * differs between the qemu and luckfox images, so try in order." Each entry
 * is a NULL-terminated argv; the ORDER IS THE PYTHON'S and is load-bearing,
 * because `poweroff` on the PATH and `/sbin/poweroff` are different programs
 * on an image that has both. */
#define ND_POWER_CANDIDATES 3
extern const char *const *const nd_power_halt_commands[ND_POWER_CANDIDATES];
extern const char *const *const nd_power_reboot_commands[ND_POWER_CANDIDATES];

/* The two confirmation questions and the three failure messages, so a test
 * can pin the strings a user reads without driving the widgets. */
extern const char *const nd_power_ask_off;
extern const char *const nd_power_ask_reboot;
extern const char *const nd_power_ask_recovery;
extern const char *const nd_power_fail_off;
extern const char *const nd_power_fail_reboot;

/* ------------------------------------------------------------------ *
 * The pieces underneath
 * ------------------------------------------------------------------ */

/* What subprocess.Popen(["name", ...]) resolves argv[0] to.
 *
 * Popen uses execvp semantics: a name containing '/' is used as given, and
 * anything else is looked up along $PATH, with the OSError that Python turns
 * into `continue` raised when nothing is found or nothing is executable.
 * nd_proc_spawn() takes a path and not a name, so the lookup has to happen
 * on this side of it.
 *
 * Returns false when no executable of that name exists; `out` is untouched.
 * The path is NOT ND_ROOT-resolved -- it is an executable, and nd_proc.h is
 * explicit that executables are not. */
bool nd_power_which(const char *name, char *out, size_t out_sz);

/* _spawn_first(candidates): run whichever of these commands the image
 * actually has. Returns true as soon as one is spawned. `n` is
 * ND_POWER_CANDIDATES for both shipped tables. */
bool nd_power_spawn_first(const char *const *const *candidates, size_t n);

/* _request_recovery()'s filesystem half: mkdir -p the state directory and
 * create the one-shot flag. ND_OK on success; on failure the caller shows
 * "Cannot ask for recovery: <why>" and does NOT reboot.
 *
 * `why`/`why_sz` receive strerror() text, which stands in for the Python's
 * str(OSError). Pass NULL to discard it. */
#define ND_POWER_WHY_MAX 160
nd_err nd_power_request_recovery_flag(char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * The screens
 * ------------------------------------------------------------------ */

/* _confirm(ui, question): a MessageDialog titled "Power" with a "Yes" button.
 * True when it was dismissed with ENTER. */
bool nd_power_confirm(nd_ui *ui, const char *question);

/* _tell(ui, message): the same dialog with an "OK" button, answer ignored. */
void nd_power_tell(nd_ui *ui, const char *message);

/* _go_down(ui, candidates, failure). See the header comment: NOT called by
 * any test. sync, then the first candidate that exists, then thirty seconds
 * of sitting still so the key does not look like it did nothing. */
void nd_power_go_down(nd_ui *ui, const char *const *const *candidates, size_t n,
                      const char *failure);

#ifdef __cplusplus
}
#endif

#endif /* ND_POWER_H_INCLUDED */
