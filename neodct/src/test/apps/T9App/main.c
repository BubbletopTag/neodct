/* test/apps/T9App/main.c -- an app that exists to prove T9 reaches an app.
 *
 * The claim is not one a unit test of the engine can check, because the
 * engine was never the broken part:
 *
 *     AN APP IN ITS OWN PROCESS KNOWS THE KEYPAD IS AN i2c MATRIX, AND ITS
 *     TEXT FIELDS THEREFORE DO MULTI-TAP, PREDICTIVE AND # MODE SWITCHING.
 *
 * That failed for a reason no test in lib/ could see. nd_ui_init_app()
 * derived ui->has_matrix_keypad from ui->input, and an app's ui->input is the
 * pipe the core hands it -- which has no matrix by construction, on every
 * device. So the flag was false in every app, nd_textinput.c fell through to
 * the QWERTY dev path, and nd_key_dev_char() has no digits in it at all:
 * pressing 2 on the phone's keypad typed NOTHING. OPEN-QUESTIONS.md BR-3.
 *
 * So this is a real app.so, dlopen()ed by the real nd-apprun, in a real child
 * process forked and exec'd by the real nd_proc_launch_app(). It reports what
 * the flag came out as, then drives a real nd_textinput over real keypad
 * codes and reports the text it produced -- which is the thing the owner
 * actually asked for and the only evidence that the whole chain works.
 *
 * It lives in test/apps/ rather than apps/ for the reason CrashApp and SvcApp
 * do: no shipped image may contain a program whose whole job is to poke at
 * the OS.
 *
 * The report is `key=value` lines, one per fact, so a failure names the fact
 * rather than a byte offset. test_t9_app.c parses it.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_paths.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* Under ND_ROOT, which nd_proc_launch_app() passes to the child, so the
 * report lands inside the parent's scratch root and not in a real /NeoDCT. */
#define T9APP_REPORT "/NeoDCT/User/t9app-report.txt"

static FILE *g_out;

static void say(const char *fmt, ...) ND_PRINTF(1, 2);

static void say(const char *fmt, ...)
{
    va_list ap;

    if (g_out == NULL)
        return;
    va_start(ap, fmt);
    (void)vfprintf(g_out, fmt, ap);
    va_end(ap);
    (void)fputc('\n', g_out);
}

/* Type a scripted run of keypad codes into a field and report the result.
 * handle_key() rather than show(): show() blocks on the key channel, and what
 * is being proved here is which BRANCH handle_key takes, not the loop around
 * it. */
static void type_into_field(nd_ui *ui, const char *label, const int32_t *codes, size_t n)
{
    char buf[64];
    nd_textinput t;
    size_t i;

    if (nd_textinput_init(&t, ui, "T9App", "type:", buf, sizeof buf, "", ND_T9_FILTER_ANY) !=
        ND_OK) {
        say("%s_init=0", label);
        return;
    }
    say("%s_init=1", label);
    for (i = 0u; i < n; i++)
        (void)nd_textinput_handle_key(&t, codes[i]);
    say("%s_text=%s", label, buf);
    say("%s_mode=%s", label, nd_t9_mode_label(nd_t9_engine_mode(&t.t9)));
}

int app_run(nd_ui *ui)
{
    char resolved[ND_PATH_MAX];
    const char *env;

    /* Three taps on 2 -> 'c', then one tap on 3 -> 'd'. On the dev-keyboard
     * path this whole run produces the empty string, because keypad codes
     * 3 and 4 are not in nd_key_dev_char()'s table. */
    static const int32_t MULTITAP[] = {ND_KEY_2, ND_KEY_2, ND_KEY_2, ND_KEY_3};
    /* # walks abc -> ABC. The same three taps then give 'C'. */
    static const int32_t UPPER[] = {ND_KEY_HASH, ND_KEY_2, ND_KEY_2, ND_KEY_2};
    /* The # cycle has FOUR stops and starts at abc, not at the front:
     * abc -> ABC -> 123 -> word -> abc. So 123 is two presses away, and a
     * third would land in predictive. */
    static const int32_t NUMERIC[] = {ND_KEY_HASH, ND_KEY_HASH, ND_KEY_2, ND_KEY_3};
    /* Three # reach predictive, where 4,6,6,3 is looked up in the real
     * /NeoDCT/System/core/t9.dict rather than tapped out. This is the one
     * case that touches the filesystem, so it also proves the dictionary is
     * reachable from inside an app process. */
    static const int32_t WORD[] = {ND_KEY_HASH, ND_KEY_HASH, ND_KEY_HASH, ND_KEY_4,
                                   ND_KEY_6,    ND_KEY_6,    ND_KEY_3};

    if (nd_path_resolve(resolved, sizeof resolved, T9APP_REPORT) != ND_OK)
        return 1;
    g_out = fopen(resolved, "w");
    if (g_out == NULL)
        return 1;

    /* THE FACT ITSELF. */
    say("has_matrix_keypad=%d", (ui != NULL && ui->has_matrix_keypad) ? 1 : 0);

    /* ...and where it came from, so a failure says which half broke. */
    env = getenv(ND_ENV_KEYPAD_MATRIX);
    say("env_matrix=%s", (env != NULL) ? env : "");
    env = getenv(ND_ENV_T9);
    say("env_t9=%s", (env != NULL) ? env : "");

    /* nd_app.h's rule still holds on this side of the fork: the app's own
     * input is a pipe and has no matrix of its own. If this ever reports 1,
     * the flag stopped being propagated and started being detected again. */
    say("input_has_matrix=%d", (ui != NULL && nd_input_has_matrix(ui->input)) ? 1 : 0);

    if (ui != NULL) {
        type_into_field(ui, "multitap", MULTITAP, ND_ARRAY_LEN(MULTITAP));
        type_into_field(ui, "upper", UPPER, ND_ARRAY_LEN(UPPER));
        type_into_field(ui, "numeric", NUMERIC, ND_ARRAY_LEN(NUMERIC));
        type_into_field(ui, "word", WORD, ND_ARRAY_LEN(WORD));
    }

    say("done=1");
    (void)fclose(g_out);
    g_out = NULL;
    return 0;
}

void app_shutdown(void)
{
    if (g_out != NULL) {
        (void)fclose(g_out);
        g_out = NULL;
    }
}
