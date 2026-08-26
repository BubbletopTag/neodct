/* linuxshell.h -- the parts of apps/LinuxShell/main.c a unit test can reach.
 *
 * System/engineering/apps/LinuxShell/main.py is 120 lines and DRAWS NOTHING.
 * It hands the physical screen to the kernel's own framebuffer console by
 * switching virtual terminals, runs /bin/sh -i attached to that terminal,
 * waits, and switches back. All the rendering -- font, scrollback, cursor,
 * escape sequences -- is fbcon inside the kernel.
 *
 * So there is no frame to compare and the test has to work on the pieces: the
 * PATH lookup, the two environment-variable reads, the tty path, the four
 * byte strings, the environment handed to the shell, and the bridge gate.
 *
 * ============ WHAT THE TEST DELIBERATELY DOES NOT CALL ============
 *
 * The composition. app_run() past the chvt lookup runs `chvt 2` on whatever
 * machine it is on -- on a developer's console that is a real VT switch, and
 * on a CI runner it is at best a permission error -- and then execs an
 * interactive /bin/sh and waits for a human to type `exit`. A test suite that
 * can black out the screen it is running on and then block forever is not a
 * test suite.
 *
 * app_run() IS driven, with PATH pointed at an empty directory so that
 * `chvt` cannot be found: that is the Python's own `if not chvt: return`
 * branch, and it is the only way into and out of run() that touches nothing.
 * Same hole, and same reason, as test_power.c's refusal to call
 * nd_power_go_down(). Named here rather than left to be discovered.
 */

#ifndef ND_LINUXSHELL_H_INCLUDED
#define ND_LINUXSHELL_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* manifest.json: {"name": "Linux Shell", "id": "999"}. The app has no
 * VerticalList, so unlike every other app this id is never handed to a
 * widget; it is here because the manifest is the other half of the port. */
#define ND_LINUXSHELL_APP_ID 999

/* os.environ.get("NEODCT_SHELL_VT", "2") / ("NEODCT_UI_VT", "1"). */
#define ND_LINUXSHELL_ENV_SHELL_VT     "NEODCT_SHELL_VT"
#define ND_LINUXSHELL_ENV_UI_VT        "NEODCT_UI_VT"
#define ND_LINUXSHELL_DEFAULT_SHELL_VT 2
#define ND_LINUXSHELL_DEFAULT_UI_VT    1

/* shutil.which("chvt"). busybox provides it on this image; the header
 * comment in main.py is explicit that no VT ioctl is used instead. */
#define ND_LINUXSHELL_CHVT "chvt"

/* subprocess.Popen(["/bin/sh", "-i"], ...) -- an absolute path, so it is
 * exec'd directly and never looked up. */
#define ND_LINUXSHELL_SH "/bin/sh"

/* env["PS1"] and env["TERM"], added to a COPY of this process's environment.
 * The trailing space in PS1 is load-bearing: busybox ash prints it verbatim. */
#define ND_LINUXSHELL_PS1  "NeoDCT # "
#define ND_LINUXSHELL_TERM "linux"

/* _run_quiet(..., timeout=1.0), and the time.sleep(0.05) that lets the VT
 * settle before the UI redraws over it. */
#define ND_LINUXSHELL_CHVT_TIMEOUT_S 1.0
#define ND_LINUXSHELL_SETTLE_S       0.05

/* The four byte strings written straight at the console, in the order they
 * are written. The cmdline carries vt.global_cursor_default=0, which is why
 * the cursor has to be turned on explicitly and off again on the way out. */
extern const char *const nd_linuxshell_cursor_on;  /* "\x1b[?25h" */
extern const char *const nd_linuxshell_cursor_off; /* "\x1b[?25l" */
extern const char *const nd_linuxshell_hint;
extern const char *const nd_linuxshell_t9_hint;

/* shutil.which(): $PATH for a bare name, the path itself when it contains a
 * slash, false when nothing executable is found. Same lookup as
 * nd_power_which() and deliberately a second copy -- an app is a separate
 * .so and there is no shared "which" in libneodct to call. */
bool nd_linuxshell_which(const char *name, char *out, size_t out_sz);

/* int(os.environ.get(name, str(fallback))).
 *
 * FALSE IS THE ValueError, and the ValueError is a CRASH in the Python: it
 * escapes run() before anything has been switched or opened. app_run()
 * returns 1 for it, which nd_proc.h step 4 classifies as a crash and shows
 * the crash screen -- the same outcome by the only means C has. */
bool nd_linuxshell_vt(const char *name, int32_t fallback, int32_t *out);

/* f"/dev/tty{shell_vt}". Not ND_ROOT-resolved: this is the path as the
 * Python spells it, and the writers resolve it when they open it. */
nd_err nd_linuxshell_tty_path(char *out, size_t out_sz, int32_t vt);

/* _write_tty(): open("wb", buffering=0), write, flush, SWALLOW EVERY
 * EXCEPTION. A missing console is not an error here; it is a phone without
 * fbcon, and the app still has to come back cleanly. `data` is a NUL-
 * terminated string because all four of the Python's byte strings are. */
void nd_linuxshell_write_tty(const char *path, const char *data);

/* _run_quiet(): spawn argv, stdout and stderr to /dev/null, wait up to
 * timeout_s. TRUE UNLESS AN EXCEPTION ESCAPED -- a non-zero exit still
 * counts as success, only a spawn failure or a timeout is false. argv[0] is
 * the path to exec, as shutil.which() left it; a missing or non-executable
 * one is the spawn failure, checked by hand for the reason main.c gives. */
bool nd_linuxshell_run_quiet(const char *const *argv, double timeout_s);

/* os.environ.copy() plus PS1 and TERM.
 *
 * owned by the caller; free the ARRAY with free(). The strings in it are
 * environ's own and two literals, and neither is ours to release. NULL only
 * on allocation failure. */
const char **nd_linuxshell_build_envp(void);

/* _start_t9_bridge()'s gate: "Only reachable when ui.matrix_input is not
 * None -- i.e. keypad-only hardware." An app process has no matrix by
 * construction (its input is the inherited pipe), so this asks the same file
 * the core asks. Identical to the Browser's nd_browser_needs_key_bridge();
 * OPEN-QUESTIONS.md BR-3 is the record. */
bool nd_linuxshell_needs_key_bridge(const nd_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* ND_LINUXSHELL_H_INCLUDED */
