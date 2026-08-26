/* test_linuxshell.c -- the Linux Shell app, app id 999.
 *
 * The app DRAWS NOTHING: it switches virtual terminals, writes four byte
 * strings at the console, execs /bin/sh -i on it and switches back. So there
 * is no frame to compare and no golden reference to compare it against --
 * shoot_docs.py never launches it and tests/golden/manifest.json has no entry
 * for it. Everything checkable is a piece: the PATH lookup, the two
 * environment-variable reads, the tty path, the four strings, the shell's
 * environment, the quiet spawn and the bridge gate.
 *
 * ============ WHAT THIS TEST WILL NOT DO ============
 *
 * IT NEVER LETS app_run() PAST THE chvt LOOKUP. Past it the app runs
 * `chvt 2`, which on a developer's console is a real VT switch and on a CI
 * runner is at best a permission error, and then execs an INTERACTIVE shell
 * and waits for a human to type `exit`. A test suite that can black out the
 * screen it is running on and then block forever is not a test suite.
 *
 * app_run() is driven twice, both times with PATH pointed at an empty
 * directory so `chvt` cannot be found: once for the Python's own
 * `if not chvt: return` branch, and once with a bad NEODCT_SHELL_VT, which
 * has to fail EARLIER than that. The pieces underneath are reachable and are
 * tested. Same deliberate hole, and same reason, as test_power.c's refusal to
 * call nd_power_go_down(); linuxshell.h says so too, so it is named in both
 * places rather than discovered.
 *
 * ============ THE ORACLE IS main.py, LINE BY LINE ============
 *
 * Every expected value below is the Python's literal: "\x1b[?25h", the hint
 * text with its two CRLF pairs, PS1 with its trailing space, VT 2 and VT 1,
 * the 1.0 s timeout. Where the C had to choose something the Python does not
 * spell -- a saturating integer, a PATH lookup standing in for
 * FileNotFoundError -- the check says so in its own message.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"

#include "smallapp_test.h"

#include "../../apps/LinuxShell/linuxshell.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*which)(const char *, char *, size_t);
    bool (*vt)(const char *, int32_t, int32_t *);
    nd_err (*tty_path)(char *, size_t, int32_t);
    void (*write_tty)(const char *, const char *);
    bool (*run_quiet)(const char *const *, double);
    const char **(*build_envp)(void);
    bool (*needs_bridge)(const nd_ui *);
    const char *const *cursor_on;
    const char *const *cursor_off;
    const char *const *hint;
    const char *const *t9_hint;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.which = sa_sym(h, "nd_linuxshell_which");
    *(void **)&api.vt = sa_sym(h, "nd_linuxshell_vt");
    *(void **)&api.tty_path = sa_sym(h, "nd_linuxshell_tty_path");
    *(void **)&api.write_tty = sa_sym(h, "nd_linuxshell_write_tty");
    *(void **)&api.run_quiet = sa_sym(h, "nd_linuxshell_run_quiet");
    *(void **)&api.build_envp = sa_sym(h, "nd_linuxshell_build_envp");
    *(void **)&api.needs_bridge = sa_sym(h, "nd_linuxshell_needs_key_bridge");
    api.cursor_on = dlsym(h, "nd_linuxshell_cursor_on");
    api.cursor_off = dlsym(h, "nd_linuxshell_cursor_off");
    api.hint = dlsym(h, "nd_linuxshell_hint");
    api.t9_hint = dlsym(h, "nd_linuxshell_t9_hint");

    return api.run != NULL && api.shutdown != NULL && api.which != NULL && api.vt != NULL &&
           api.tty_path != NULL && api.write_tty != NULL && api.run_quiet != NULL &&
           api.build_envp != NULL && api.needs_bridge != NULL && api.cursor_on != NULL &&
           api.cursor_off != NULL && api.hint != NULL && api.t9_hint != NULL;
}

/* ------------------------------------------------------------------ *
 * Scratch
 * ------------------------------------------------------------------ */

static char g_root[ND_PATH_MAX];   /* NEODCT_ROOT for the tty writes */
static char g_bindir[ND_PATH_MAX]; /* stub programs                  */
static char g_empty[ND_PATH_MAX];  /* a PATH with nothing in it      */
static char g_saved_root[ND_PATH_MAX];
static char g_saved_path[ND_PATH_MAX];

static void path_save(void)
{
    const char *p = getenv("PATH");

    (void)nd_strlcpy(g_saved_path, (p != NULL) ? p : "", sizeof g_saved_path);
}

static void path_restore(void)
{
    (void)setenv("PATH", g_saved_path, 1);
}

static void root_to(const char *root)
{
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(root);
}

static void root_restore(void)
{
    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
}

/* An executable with a chosen exit code, so a spawn can be watched without
 * anything real being run. */
static bool make_stub_program(const char *dir, const char *name, const char *body)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/%s", dir, name) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fputs("#!/bin/sh\n", f);
    (void)fputs(body, f);
    (void)fclose(f);
    return chmod(path, 0755) == 0;
}

/* Whatever is in <root><path>, or NULL. */
static bool read_scratch_file(const char *path, char *out, size_t out_sz, size_t *len_out)
{
    char full[ND_PATH_MAX];
    FILE *f;
    size_t n;

    if (nd_snprintf(full, sizeof full, "%s%s", g_root, path) != ND_OK)
        return false;
    f = fopen(full, "rb");
    if (f == NULL)
        return false;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    if (len_out != NULL)
        *len_out = n;
    (void)fclose(f);
    return true;
}

/* ------------------------------------------------------------------ *
 * 1. The constants and the four byte strings
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    /* manifest.json: {"name": "Linux Shell", "id": "999"}. */
    CHECK_INT(ND_LINUXSHELL_APP_ID, 999, "APP_ID");

    /* shell_vt = int(os.environ.get("NEODCT_SHELL_VT", "2"))
     * ui_vt    = int(os.environ.get("NEODCT_UI_VT",    "1")) */
    CHECK_STR(ND_LINUXSHELL_ENV_SHELL_VT, "NEODCT_SHELL_VT", "shell VT variable");
    CHECK_STR(ND_LINUXSHELL_ENV_UI_VT, "NEODCT_UI_VT", "UI VT variable");
    CHECK_INT(ND_LINUXSHELL_DEFAULT_SHELL_VT, 2, "default shell VT");
    CHECK_INT(ND_LINUXSHELL_DEFAULT_UI_VT, 1, "default UI VT");

    CHECK_STR(ND_LINUXSHELL_CHVT, "chvt", "the program that switches the console");
    CHECK_STR(ND_LINUXSHELL_SH, "/bin/sh", "the shell, by absolute path");

    /* env["PS1"] = "NeoDCT # " -- THE TRAILING SPACE IS PART OF IT. */
    CHECK_STR(ND_LINUXSHELL_PS1, "NeoDCT # ", "PS1, trailing space included");
    CHECK_STR(ND_LINUXSHELL_TERM, "linux", "TERM");

    CHECK_DBL(ND_LINUXSHELL_CHVT_TIMEOUT_S, 1.0, "_run_quiet timeout");
    CHECK_DBL(ND_LINUXSHELL_SETTLE_S, 0.05, "the settle sleep");

    /* b"\x1b[?25h" and b"\x1b[?25l". The cmdline carries
     * vt.global_cursor_default=0, so the app turns the cursor on itself. */
    CHECK_STR(*api.cursor_on, "\x1b[?25h", "cursor on");
    CHECK_STR(*api.cursor_off, "\x1b[?25l", "cursor off");

    CHECK_STR(*api.hint, "Type exit to go back to the NeoDCT UI\r\n\r\n", "the exit hint");
    /* Two adjacent byte literals in the Python; one string here. CRLF, not
     * LF: it is going at a raw console with no ONLCR of its own. */
    CHECK_STR(*api.t9_hint,
              "T9 keypad active: 2-9 letters, 0 space, 1 symbols, # mode, C backspace\r\n\r\n",
              "the T9 hint");
}

/* ------------------------------------------------------------------ *
 * 2. _which()
 * ------------------------------------------------------------------ */

static void test_which(void)
{
    char out[ND_PATH_MAX];
    char expect[ND_PATH_MAX];
    char abs_path[ND_PATH_MAX];

    path_save();
    CHECK(make_stub_program(g_bindir, "nd-fake-chvt", "exit 0\n"), "stub program written");
    (void)setenv("PATH", g_bindir, 1);

    CHECK(api.which("nd-fake-chvt", out, sizeof out), "a bare name is found along $PATH");
    (void)nd_snprintf(expect, sizeof expect, "%s/nd-fake-chvt", g_bindir);
    CHECK_STR(out, expect, "and the answer is the full path");

    /* `if not chvt: return` -- this is the branch that keeps the UI alive on
     * an image with no chvt. */
    CHECK(!api.which("nd-there-is-no-such-program", out, sizeof out),
          "a missing name is shutil.which()'s None");

    (void)nd_snprintf(abs_path, sizeof abs_path, "%s/nd-fake-chvt", g_bindir);
    CHECK(api.which(abs_path, out, sizeof out), "a name with a slash is used as given");
    CHECK_STR(out, abs_path, "unchanged");
    CHECK(!api.which("/nd/no/such/path", out, sizeof out), "a missing absolute path is refused");

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
    CHECK(!api.which("nd-fake-chvt", NULL, 0u), "no output buffer is refused");

    path_restore();
}

/* ------------------------------------------------------------------ *
 * 3. int(os.environ.get(...))
 * ------------------------------------------------------------------ */

#define VT_VAR "NEODCT_TEST_VT"

static void set_vt(const char *value)
{
    if (value == NULL)
        (void)unsetenv(VT_VAR);
    else
        (void)setenv(VT_VAR, value, 1);
}

static void test_vt(void)
{
    int32_t v = -99;

    /* os.environ.get(name, default) -- unset takes the default. */
    set_vt(NULL);
    CHECK(api.vt(VT_VAR, 2, &v), "an unset variable is not an error");
    CHECK_INT(v, 2, "and gives the default");

    set_vt("5");
    CHECK(api.vt(VT_VAR, 2, &v), "a number parses");
    CHECK_INT(v, 5, "5");

    /* int() strips whitespace at both ends and accepts a sign. */
    set_vt("  7\t");
    CHECK(api.vt(VT_VAR, 2, &v), "surrounding whitespace is stripped");
    CHECK_INT(v, 7, "7");
    set_vt("+3");
    CHECK(api.vt(VT_VAR, 2, &v), "a leading + is accepted");
    CHECK_INT(v, 3, "3");
    /* int("-2") is -2, and the Python would go on to write /dev/tty-2. Ported
     * as-is: it is nonsense that fails silently, not an error to catch. */
    set_vt("-2");
    CHECK(api.vt(VT_VAR, 2, &v), "a negative VT is a number like any other");
    CHECK_INT(v, -2, "-2");

    /* Every one of these is a ValueError in the Python, and a ValueError out
     * of run() is the crash screen. */
    set_vt("");
    CHECK(!api.vt(VT_VAR, 2, &v), "int(\"\") is a ValueError, NOT the default");
    set_vt("banana");
    CHECK(!api.vt(VT_VAR, 2, &v), "int(\"banana\") is a ValueError");
    set_vt("2x");
    CHECK(!api.vt(VT_VAR, 2, &v), "trailing rubbish is a ValueError");
    set_vt("2.0");
    CHECK(!api.vt(VT_VAR, 2, &v), "int(\"2.0\") is a ValueError");

    /* Python's int has no range. C saturates instead of reporting an error,
     * because the Python's behaviour for an absurd VT is a silent no-op and
     * an error here would be a crash screen. apps/LinuxShell/main.c says so
     * at more length. */
    set_vt("99999999999999999999");
    CHECK(api.vt(VT_VAR, 2, &v), "an out-of-range number is not a ValueError");
    CHECK_INT(v, 2147483647, "it saturates rather than crashing");

    set_vt(NULL);
    CHECK(!api.vt(VT_VAR, 2, NULL), "no output pointer is refused");
    v = -99;
    CHECK(api.vt(NULL, 4, &v), "a NULL name takes the fallback");
    CHECK_INT(v, 4, "4");
}

/* ------------------------------------------------------------------ *
 * 4. f"/dev/tty{shell_vt}"
 * ------------------------------------------------------------------ */

static void test_tty_path(void)
{
    char out[ND_PATH_MAX];
    char small[6];

    CHECK_INT(api.tty_path(out, sizeof out, 2), ND_OK, "the default shell VT");
    CHECK_STR(out, "/dev/tty2", "/dev/tty2");
    CHECK_INT(api.tty_path(out, sizeof out, 1), ND_OK, "the default UI VT");
    CHECK_STR(out, "/dev/tty1", "/dev/tty1");
    CHECK_INT(api.tty_path(out, sizeof out, 12), ND_OK, "two digits");
    CHECK_STR(out, "/dev/tty12", "/dev/tty12");

    CHECK(api.tty_path(small, sizeof small, 2) != ND_OK, "a short buffer is refused");
    CHECK_INT(api.tty_path(NULL, 0u, 2), ND_ERR_INVAL, "NULL is refused");
}

/* ------------------------------------------------------------------ *
 * 5. _write_tty()
 * ------------------------------------------------------------------ */

static void test_write_tty(void)
{
    char got[256];
    size_t len = 0u;

    root_to(g_root);

    /* "except Exception: pass" -- a console that is not there is not an
     * error. Nothing to assert but that it returns. */
    api.write_tty("/dev/tty2", *api.cursor_on);
    CHECK(!read_scratch_file("/dev/tty2", got, sizeof got, NULL),
          "a missing /dev is swallowed, not created");

    CHECK_INT(nd_mkdir_p("/dev", 0755u), ND_OK, "a /dev to write into");

    api.write_tty("/dev/tty2", *api.cursor_on);
    CHECK(read_scratch_file("/dev/tty2", got, sizeof got, &len), "the console was written");
    CHECK_STR(got, "\x1b[?25h", "the cursor-on sequence, byte for byte");
    CHECK_INT(len, 6, "six bytes and no newline");

    /* open(path, "wb") TRUNCATES. On a character device that means nothing;
     * the flag is the Python's and is kept, and this is where it shows. */
    api.write_tty("/dev/tty2", *api.hint);
    CHECK(read_scratch_file("/dev/tty2", got, sizeof got, &len), "and written again");
    CHECK_STR(got, "Type exit to go back to the NeoDCT UI\r\n\r\n", "the hint replaced it");

    api.write_tty("/dev/tty2", *api.t9_hint);
    CHECK(read_scratch_file("/dev/tty2", got, sizeof got, NULL), "the T9 hint");
    CHECK_STR(got, "T9 keypad active: 2-9 letters, 0 space, 1 symbols, # mode, C backspace\r\n\r\n",
              "byte for byte");

    api.write_tty("/dev/tty2", *api.cursor_off);
    CHECK(read_scratch_file("/dev/tty2", got, sizeof got, &len), "cursor off");
    CHECK_STR(got, "\x1b[?25l", "the cursor-off sequence");

    /* Neither of these may fault, and neither may write anything. */
    api.write_tty(NULL, *api.cursor_on);
    api.write_tty("/dev/tty2", NULL);
    CHECK(read_scratch_file("/dev/tty2", got, sizeof got, NULL), "still there");
    CHECK_STR(got, "\x1b[?25l", "NULL wrote nothing");

    root_restore();
}

/* ------------------------------------------------------------------ *
 * 6. _run_quiet()
 * ------------------------------------------------------------------ */

static void test_run_quiet(void)
{
    char ok_path[ND_PATH_MAX];
    char fail_path[ND_PATH_MAX];
    char slow_path[ND_PATH_MAX];
    const char *argv[3];

    /* The real /dev/null, not one under the scratch root: subprocess.DEVNULL
     * is opened by subprocess.run itself, and a DEVNULL that cannot be opened
     * is one of the exceptions _run_quiet turns into False. That is faithful
     * -- and it means this test has to stand where the phone stands, with no
     * NEODCT_ROOT in front of /dev. The stub programs below are absolute host
     * paths and are unaffected either way. */
    root_to(NULL);

    CHECK(make_stub_program(g_bindir, "nd-quiet-ok", "exit 0\n"), "a program that succeeds");
    CHECK(make_stub_program(g_bindir, "nd-quiet-fail", "exit 3\n"), "a program that fails");
    CHECK(make_stub_program(g_bindir, "nd-quiet-slow", "sleep 5\n"), "a program that hangs");
    (void)nd_snprintf(ok_path, sizeof ok_path, "%s/nd-quiet-ok", g_bindir);
    (void)nd_snprintf(fail_path, sizeof fail_path, "%s/nd-quiet-fail", g_bindir);
    (void)nd_snprintf(slow_path, sizeof slow_path, "%s/nd-quiet-slow", g_bindir);

    argv[0] = ok_path;
    argv[1] = "2";
    argv[2] = NULL;
    CHECK(api.run_quiet(argv, 5.0), "a clean exit is True");

    /* check=False: subprocess.run does not raise on a non-zero exit, so
     * _run_quiet returns True for it. This is the branch that decides whether
     * the app carries on to open the console, so getting it backwards would
     * make the app give up on every chvt that grumbles. */
    argv[0] = fail_path;
    CHECK(api.run_quiet(argv, 5.0), "exit 3 is STILL True -- check=False");

    /* TimeoutExpired -> False, and the child must not be left behind. */
    argv[0] = slow_path;
    CHECK(!api.run_quiet(argv, 0.3), "a timeout is False");

    /* FileNotFoundError / PermissionError -> False. */
    argv[0] = "/nd/no/such/program";
    CHECK(!api.run_quiet(argv, 1.0), "a missing program is False");

    CHECK(!api.run_quiet(NULL, 1.0), "NULL argv is refused");
    argv[0] = "";
    CHECK(!api.run_quiet(argv, 1.0), "an empty argv[0] is refused");

    root_restore();
}

/* ------------------------------------------------------------------ *
 * 7. env.copy() + PS1 + TERM
 * ------------------------------------------------------------------ */

static size_t count_prefix(const char **envp, const char *prefix)
{
    size_t n = 0u;
    size_t i;

    for (i = 0u; envp[i] != NULL; i++) {
        if (strncmp(envp[i], prefix, strlen(prefix)) == 0)
            n++;
    }
    return n;
}

static const char *find_prefix(const char **envp, const char *prefix)
{
    size_t i;

    for (i = 0u; envp[i] != NULL; i++) {
        if (strncmp(envp[i], prefix, strlen(prefix)) == 0)
            return envp[i];
    }
    return NULL;
}

static void test_build_envp(void)
{
    const char **envp;
    const char *marker;

    /* Something of our own to prove the COPY is a copy. */
    (void)setenv("NEODCT_TEST_MARKER", "kept", 1);
    /* And a PS1 that has to be REPLACED, not accompanied. */
    (void)setenv("PS1", "$ ", 1);

    envp = api.build_envp();
    if (envp == NULL) {
        CHECK(false, "build_envp");
        return;
    }

    CHECK_INT(count_prefix(envp, "PS1="), 1, "exactly one PS1");
    CHECK_STR(find_prefix(envp, "PS1="), "PS1=NeoDCT # ", "and it is ours, trailing space and all");
    CHECK_INT(count_prefix(envp, "TERM="), 1, "exactly one TERM");
    CHECK_STR(find_prefix(envp, "TERM="), "TERM=linux", "TERM=linux");

    marker = find_prefix(envp, "NEODCT_TEST_MARKER=");
    CHECK_STR(marker, "NEODCT_TEST_MARKER=kept", "the rest of the environment is carried over");

    /* owned by the caller; the ARRAY is freed and the strings are not. */
    free((void *)(uintptr_t)(const void *)envp);
    (void)unsetenv("NEODCT_TEST_MARKER");
    (void)unsetenv("PS1");
}

/* ------------------------------------------------------------------ *
 * 8. _start_t9_bridge()'s gate
 * ------------------------------------------------------------------ */

static void test_needs_key_bridge(void)
{
    sa_fixture fx;

    /* An empty root, so /NeoDCT/User/keymap.json is not there: that is QEMU
     * and every dev build, where a real keyboard already reaches the console
     * and start_shell_bridge() returns None. */
    root_to(g_root);
    CHECK(!api.needs_bridge(NULL), "no ui and no keymap.json -> no bridge");

    if (sa_fx_init(&fx)) {
        CHECK(!fx.ui.has_matrix_keypad, "the fixture has no matrix, like an app process");
        CHECK(!api.needs_bridge(&fx.ui), "a ui without a matrix -> no bridge");

        /* The core's word for it, which is the only evidence there is inside
         * an app process. BR-3: this used to re-read keymap.json, because
         * ui->has_matrix_keypad was false in every app on every device. */
        (void)setenv(ND_ENV_KEYPAD_MATRIX, "1", 1);
        CHECK(api.needs_bridge(&fx.ui), "the core says there is a matrix -> bridge");
        (void)unsetenv(ND_ENV_KEYPAD_MATRIX);
        CHECK(!api.needs_bridge(&fx.ui), "and without it, no bridge");

        /* NOT ui->has_matrix_keypad: that one carries the NEODCT_T9 override,
         * and a developer forcing T9 on over a real keyboard must not get a
         * uinput keyboard bridged on top of it and every press doubled. */
        fx.ui.has_matrix_keypad = true;
        CHECK(!api.needs_bridge(&fx.ui), "the T9 override alone does not bridge");
    } else {
        CHECK(false, "fixture");
    }
    sa_fx_free(&fx);
    root_restore();
}

/* ------------------------------------------------------------------ *
 * 9. run(ui), as far as it is safe to go
 * ------------------------------------------------------------------ */

static void test_run_without_chvt(void)
{
    char got[64];
    sa_fixture fx;

    path_save();
    root_to(g_root);
    /* An empty directory, so `chvt` cannot be found and app_run() takes the
     * Python's `if not chvt: return` before it touches anything. */
    (void)setenv("PATH", g_empty, 1);
    (void)unsetenv(ND_LINUXSHELL_ENV_SHELL_VT);
    (void)unsetenv(ND_LINUXSHELL_ENV_UI_VT);

    /* A NULL ui is fine here and is NOT the error it is in an app that draws:
     * run(ui) touches ui once, to ask whether there is a matrix keypad. */
    CHECK_INT(api.run(NULL), 0, "no chvt: run(NULL) returns 0, silently");

    if (sa_fx_init(&fx))
        CHECK_INT(api.run(&fx.ui), 0, "no chvt: run(ui) returns 0, silently");
    else
        CHECK(false, "fixture");
    sa_fx_free(&fx);

    /* Silently: nothing was written at the console and no frame was drawn.
     * /dev/tty2 still holds what test_write_tty left in it. */
    CHECK(read_scratch_file("/dev/tty2", got, sizeof got, NULL), "the console file is still there");
    CHECK_STR(got, "\x1b[?25l", "and nothing new was written to it");

    root_restore();
    path_restore();
}

static void test_run_with_a_bad_vt(void)
{
    path_save();
    root_to(g_root);
    (void)setenv("PATH", g_empty, 1);

    /* int("banana") is a ValueError, and it is raised BEFORE the chvt lookup
     * -- so this must fail even on an image where chvt is missing, which is
     * exactly the state PATH is in. A non-zero return is nd_proc.h step 4's
     * "WIFEXITED && status != 0 -> a crash", which is the crash screen the
     * Python's uncaught ValueError produces. */
    (void)setenv(ND_LINUXSHELL_ENV_SHELL_VT, "banana", 1);
    CHECK_INT(api.run(NULL), 1, "a bad NEODCT_SHELL_VT crashes, before the chvt lookup");
    (void)unsetenv(ND_LINUXSHELL_ENV_SHELL_VT);

    (void)setenv(ND_LINUXSHELL_ENV_UI_VT, "", 1);
    CHECK_INT(api.run(NULL), 1, "and so does an empty NEODCT_UI_VT");
    (void)unsetenv(ND_LINUXSHELL_ENV_UI_VT);

    /* A well-formed one gets past both int() calls and stops at the chvt
     * lookup, which is where every safe path through this app ends. */
    (void)setenv(ND_LINUXSHELL_ENV_SHELL_VT, "3", 1);
    CHECK_INT(api.run(NULL), 0, "a good one falls through to the chvt branch");
    (void)unsetenv(ND_LINUXSHELL_ENV_SHELL_VT);

    root_restore();
    path_restore();
}

/* ------------------------------------------------------------------ *
 * 10. The contract
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    /* Mandatory, and must be safe with no child to kill -- which is the state
     * it is in on every path this test can reach. */
    api.shutdown();
    api.shutdown();
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("LinuxShell", "ndlinuxshell");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndlsh-root", g_root, sizeof g_root) ||
        !sa_tmpdir("ndlsh-bin", g_bindir, sizeof g_bindir) ||
        !sa_tmpdir("ndlsh-empty", g_empty, sizeof g_empty)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_strings);
    RUN(test_which);
    RUN(test_vt);
    RUN(test_tty_path);
    RUN(test_write_tty);
    RUN(test_run_quiet);
    RUN(test_build_envp);
    RUN(test_needs_key_bridge);
    RUN(test_run_without_chvt);
    RUN(test_run_with_a_bad_vt);
    RUN(test_null_safety);

    sa_rmtree(g_empty);
    sa_rmtree(g_bindir);
    sa_rmtree(g_root);
    return sa_end(h, "test_linuxshell");
}
