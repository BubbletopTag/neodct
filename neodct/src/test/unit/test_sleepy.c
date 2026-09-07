/* test_sleepy.c -- the Sleepy engineering app, app id 9008.
 *
 * ============ WHY THIS FILE EXISTS AT ALL ============
 *
 * It did not, for four releases. sleepy.h exported its row labels "so a test
 * can name them without dlopen()ing the app", and no test ever did -- which
 * is how the app shipped saying "Not root, or the pin is taken" about a
 * process that nd_proc.c launches as root, and how a blank that never blanked
 * reported success. Neither of those is a thing a golden frame catches. Both
 * are things a sentence and a return value catch.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The rows are the rows, in order. "Screen off" is THIRD, because the
 *     menu shortcut for a row is its position and the second Display row has
 *     meant Brightness since this app shipped.
 *
 *  2. Every dialog this app can put on the screen FITS. nd_msgdialog
 *     truncates rather than scrolling, so a sentence one word too long
 *     arrives on the phone with its second half missing -- and the half that
 *     goes missing is the half that says what went wrong. Six of these
 *     strings are built at runtime from a kernel error or a frequency name,
 *     so they are measured at their longest reachable form rather than as
 *     they appear in the source.
 *
 *  3. app_run(NULL) is a return, not a fault.
 *
 * There is deliberately NO golden frame here. CODING-STANDARDS.md section 7
 * says a new screen's test is its unit test, and the one screen this app
 * draws by hand is drawn on a panel whose backlight is off.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <string.h>

#include "nd_widgets.h"

#include "smallapp_test.h"

#include "../../apps/Sleepy/sleepy.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    const char *const *title;
    const char *const *root_items;
    const char *const *display_items;
    const char *const *no_cpufreq;
    const char *const *no_backlight;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    api.title = sa_sym(h, "nd_sleepy_title");
    api.root_items = sa_sym(h, "nd_sleepy_root_items");
    api.display_items = sa_sym(h, "nd_sleepy_display_items");
    api.no_cpufreq = sa_sym(h, "nd_sleepy_no_cpufreq");
    api.no_backlight = sa_sym(h, "nd_sleepy_no_backlight");

    return api.run != NULL && api.shutdown != NULL && api.title != NULL &&
           api.root_items != NULL && api.display_items != NULL && api.no_cpufreq != NULL &&
           api.no_backlight != NULL;
}

/* ------------------------------------------------------------------ *
 * The rows
 * ------------------------------------------------------------------ */

static void test_rows(void)
{
    CHECK_STR(*api.title, "Sleepy", "the app names itself");

    CHECK_STR(api.root_items[0], "CPU", "root row 0");
    CHECK_STR(api.root_items[1], "Display", "root row 1");

    /* Order is the assertion, not membership. Appending is what kept the
     * existing shortcuts working; inserting "Screen off" beside BLANK! would
     * have silently moved Brightness. */
    CHECK_STR(api.display_items[0], "BLANK!", "the timed blank stays first");
    CHECK_STR(api.display_items[1], "Brightness", "Brightness did not move");
    CHECK_STR(api.display_items[2], "Screen off", "the untimed blank was appended");
    CHECK_INT(SLEEPY_DISPLAY_ITEMS, 3, "three Display rows");
}

/* ------------------------------------------------------------------ *
 * Nothing on this screen is clipped
 * ------------------------------------------------------------------ */

static void check_fits(sa_fixture *fx, const char *message, const char *what)
{
    nd_msgdialog dlg;
    size_t needed = 0u;
    size_t fits = 0u;

    nd_msgdialog_init(&dlg, &fx->ui, message);
    /* Every dialog this app raises is titled -- show_dialog() sets the app
     * name on all of them -- and a title costs a line, so measuring without
     * one would measure a budget this app never has. */
    nd_msgdialog_set_title(&dlg, "Sleepy");
    nd_msgdialog_measure(&dlg, &needed, &fits);
    CHECK(needed <= fits, what);
}

static void test_the_dialogs_fit(void)
{
    static const char *const WHAT[] = {
        "The backlight stayed on.",
        "Backlight wake failed.",
        "Brightness refused.",
        "Cannot read brightness.",
    };
    sa_fixture fx;
    char message[192];
    size_t i;

    if (!sa_fx_init(&fx))
        return;

    check_fits(&fx, *api.no_cpufreq, "the no-cpufreq notice fits");
    check_fits(&fx, *api.no_backlight, "the no-backlight notice fits");
    check_fits(&fx, "No PWM dimming.\n\nGPIO is on/off only.", "the no-dimming notice fits");

    /* The four that carry a reason from nd_backlight_last_error(), measured
     * with the longest phrase that call can produce. strerror() supplies the
     * wording, and ENAMETOOLONG is the widest one this path can reach --
     * write_text() sets it deliberately so a caller printing strerror(errno)
     * names the right failure. */
    for (i = 0u; i < ND_ARRAY_LEN(WHAT); i++) {
        (void)nd_snprintf(message, sizeof message, "%s\n\n%s", WHAT[i], "File name too long");
        check_fits(&fx, message, "a backlight failure fits with the kernel's reason");
    }

    /* The CPU confirmation at its longest: the widest frequency label this
     * chip produces, and the longest governor name the kernel ships
     * ("conservative", twelve characters). */
    (void)nd_snprintf(message, sizeof message, "Pinned to %s.\n\nNow: %s\nGovernor: %s", "1.20 GHz",
                      "1.20 GHz", "conservative");
    check_fits(&fx, message, "the pin confirmation fits");

    (void)nd_snprintf(message, sizeof message, "%s\n\nNow: %s\nGovernor: %s", "Range opened up.",
                      "1.20 GHz", "conservative");
    check_fits(&fx, message, "the unpin confirmation fits");

    (void)nd_snprintf(message, sizeof message,
                      "%s was refused.\n\nThe kernel kept the range it had.",
                      SLEEPY_CPU_AUTO_LABEL);
    check_fits(&fx, message, "the refusal fits with the longest row label");

    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * Safety
 * ------------------------------------------------------------------ */

/* nd_app.h: a non-zero return gets the crash screen, and that is the right
 * answer for a context this app cannot draw into. It must not be a fault --
 * nd-apprun would then report a signal rather than a refusal. */
static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "a null context is refused, not faulted");
}

/* app_shutdown() with no blank outstanding must not touch the backlight. It
 * runs on every exit, including the ordinary one from the root menu, and a
 * phone that wrote to its panel every time an app closed would be doing it
 * for nothing on the one device where that write can fail. */
static void test_shutdown_without_a_blank_is_a_no_op(void)
{
    api.shutdown();
    api.shutdown();
    CHECK(true, "a shutdown with nothing blanked returns");
}

int main(void)
{
    void *h = sa_begin("Sleepy", "ndsleepy");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_rows);
    RUN(test_the_dialogs_fit);
    RUN(test_null_safety);
    RUN(test_shutdown_without_a_blank_is_a_no_op);

    return sa_end(h, "test_sleepy");
}
