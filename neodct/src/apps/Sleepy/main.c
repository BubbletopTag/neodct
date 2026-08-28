/* Sleepy -- the two hardware pokes that "sleep" is going to be made of.
 *
 * NeoDCT's sleep is not a kernel suspend and is not going to be one. The
 * RV1103 has no suspend path that keeps the modem alive, and a feature phone
 * that stops answering calls when the screen goes off is not a feature phone.
 * So sleep here is a fake: drop the CPU to its lowest operating point, blank
 * the panel, stop everything that can be stopped, and keep polling i2c and
 * the modem at the bare minimum rate.
 *
 * Nobody had ever done either of the first two on this hardware. That is what
 * this app is for and why it is only two rows: before there can be a sleep
 * state there has to be an answer to "does the backlight actually go off when
 * you write to gpio53, and does the CPU actually change speed when you write
 * to scaling_min_freq". Both answers are things you have to SEE, on the
 * bench, with a phone in your hand -- neither shows up in a unit test, which
 * is exactly the lesson MicTest's commit recorded.
 *
 * ============ WHAT IS DELIBERATELY NOT HERE ============
 *
 * No sleep. This app does not enter one, does not schedule one and does not
 * own a timeout. It exercises the two primitives separately so that the thing
 * built on top of them has something known-good to stand on. Suspending the
 * services, slowing the modem poll and deciding what wakes the phone are all
 * the core's business, and none of them can be designed until these two are
 * proven.
 *
 * ============ THE BACKLIGHT COMES BACK. ALWAYS. ============
 *
 * The one way this app can leave the phone in a state its owner cannot get
 * out of is by turning the screen off and not turning it back on. So the
 * restore happens on every path out of the blank, app_shutdown() included --
 * an incoming call during a blank arrives as SIGTERM (nd_app.h), and a phone
 * that rang in the dark with no way to see who was calling would be a worse
 * bug than any this app was written to find.
 *
 * The CPU pin is the other way round and on purpose: nothing puts the
 * frequency back. An engineering app whose effect vanished the moment you
 * left it could not be used to measure anything. nd_cpufreq.h says so at
 * greater length.
 */

#include <string.h>

#include "sleepy.h"

#include "nd_app.h"
#include "nd_cpufreq.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

const char *const nd_sleepy_title = "Sleepy";

/* CPU first because it is the reversible one. */
const char *const nd_sleepy_root_items[SLEEPY_ROOT_ITEMS] = {"CPU", "Display"};

/* The exclamation mark is the warning. There is exactly one row on that
 * screen and pressing it turns the screen off, which is indistinguishable
 * from a crash for the ten seconds it lasts. */
const char *const nd_sleepy_blank_item = "BLANK!";

/* Short on purpose. The dialog gives a title line and three at 14 px, and it
 * TRUNCATES rather than scrolling -- a longer and more precise sentence
 * arrives on the phone with its second half missing, which is worse than a
 * blunt one. The precise version of each of these is in the log line and in
 * docs/SLEEP.md. */
const char *const nd_sleepy_no_cpufreq =
    "No cpufreq.\n\nThis kernel does not scale the CPU, so there is nothing to pick.";
const char *const nd_sleepy_no_backlight =
    "No backlight.\n\nNo PWM device and no gpio53, so the panel cannot go off.";

/* Set for exactly as long as the panel is dark, and read by app_shutdown().
 * volatile sig_atomic_t would be the careful type for a flag a handler reads,
 * but nd-apprun calls app_shutdown() from ordinary code rather than from the
 * handler (nd_app.h, step 3), so this is an ordinary read on the same thread
 * of control that wrote it. */
static bool g_blanked;

/* ------------------------------------------------------------------ *
 * Display -> BLANK!
 * ------------------------------------------------------------------ */

/* Painted BEFORE the light goes out, because it cannot be painted after: on
 * the GPIO tier the framebuffer is untouched and simply invisible, so
 * whatever is on the canvas when the backlight dies is what greets you when
 * it comes back. Leaving the menu there would make the wake look like the
 * blank never happened. */
static void draw_wake_screen(nd_ui *ui, nd_bl_mode mode)
{
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_content_bottom(ui)),
                            ND_BLACK);
    (void)nd_draw_text(ui->draw, 8, 4, nd_sleepy_title, ui->font_xl, ND_WHITE);
    /* font_md, not font_n. At 20 px this line runs off the right edge of a
     * 240 px panel and loses its full stop, which was found by looking at the
     * screen and could not have been found any other way. */
    (void)nd_draw_text(ui->draw, 8, 42, "Screen off for 10 s.", ui->font_md, ND_WHITE);
    (void)nd_draw_text(ui->draw, 8, 74,
                       mode == ND_BL_PWM ? "Tier: PWM backlight" : "Tier: gpio53 (BL)", ui->font_s,
                       ND_GRAY);
    /* Stated because the measurement depends on nobody pressing anything, and
     * somebody who does not know the escape exists will press everything. */
    (void)nd_draw_text(ui->draw, 8, 96, "Any key wakes it early.", ui->font_s, ND_GRAY);
}

static void show_dialog(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_sleepy_title);
    (void)nd_msgdialog_show(&dialog);
}

static void blank(nd_ui *ui)
{
    nd_bl_mode mode = nd_backlight_mode();
    nd_softkey bar;
    double until;

    if (mode == ND_BL_NONE) {
        show_dialog(ui, nd_sleepy_no_backlight);
        return;
    }

    draw_wake_screen(ui, mode);
    /* Repainted, not left alone. draw_wake_screen() clears rows 0..145 and
     * the strip below is whatever the menu underneath put there -- which is
     * "Select", on a screen where there is nothing to select. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Wake", false);
    if (nd_ui_present(ui) != ND_OK)
        return;

    if (!nd_backlight_off()) {
        /* The tier said it was there and the write was still refused, which on
         * this phone means the process is not root or somebody else holds the
         * pin. Worth its own words: "no backlight" and "backlight refused" send
         * an engineer to two different places. */
        show_dialog(ui, "The backlight would not go off.\n\nNot root, or another process "
                        "holds the pin.");
        return;
    }
    g_blanked = true;
    nd_log(ND_LOG_SLEEPY, "Sleepy: panel dark (%s) for %.0f s",
           mode == ND_BL_PWM ? "pwm" : "gpio53", SLEEPY_BLANK_SECONDS);

    /* Nothing is drawn and nothing is presented for the whole ten seconds.
     * That is not laziness -- it is the closest this app gets to the thing it
     * is a rehearsal for. A countdown would mean rasterising and blitting
     * frames nobody can see, on the one core, while measuring how little the
     * phone can get away with doing.
     *
     * So the only work in the loop is the wait itself, and the timeout is the
     * wake rate: half a second is four wakes shorter than a tenth would be
     * and still ends the blank inside a keypress of being asked to. */
    until = nd_time_monotonic() + SLEEPY_BLANK_SECONDS;
    while (nd_time_monotonic() < until) {
        if (nd_ui_read_keypress(ui, 0.5) != ND_KEY_NONE)
            break;
        if (nd_app_should_exit())
            break;
    }

    (void)nd_backlight_on(100);
    g_blanked = false;
    nd_log(ND_LOG_SLEEPY, "Sleepy: panel lit");
}

static void show_display_menu(nd_ui *ui)
{
    for (;;) {
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;

        nd_vlist_init(&menu, ui, "Display", &nd_sleepy_blank_item, 1u, SLEEPY_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return;
        blank(ui);
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * CPU -> pick an operating point
 * ------------------------------------------------------------------ */

/* Which row to start on: the frequency the range is already pinned to, or the
 * one the CPU is running at, or the top of the list. Preselecting the current
 * value is what every other list-of-choices in this OS does -- see Settings'
 * show_engineering_mode() -- and it is what makes "which one am I on" a thing
 * you read rather than a thing you work out. */
static size_t current_row(const nd_cpufreq_table *table, const nd_cpufreq_state *state)
{
    size_t i;

    /* A pinned range (min == max) is a deliberate choice somebody made and is
     * the better answer; scaling_cur_freq is wherever the governor happens to
     * be this millisecond and moves while you look at it. */
    if (state->min_khz > 0 && state->min_khz == state->max_khz) {
        for (i = 0u; i < table->n; i++) {
            if (table->khz[i] == state->min_khz)
                return i;
        }
    }
    for (i = 0u; i < table->n; i++) {
        if (table->khz[i] == state->cur_khz)
            return i;
    }
    return 0u;
}

/* What the phone says about itself after a write, read back rather than
 * assumed. A confirmation that echoed the request would say "Pinned to
 * 408 MHz" on a kernel that had refused it. */
static void report(nd_ui *ui, int32_t requested_khz, nd_err set_result)
{
    nd_cpufreq_state after;
    char message[256];
    char asked[32];
    char now[32];

    nd_cpufreq_format(asked, sizeof asked, requested_khz);

    if (set_result != ND_OK) {
        (void)nd_snprintf(message, sizeof message,
                          "%s was refused.\n\nNot root, or the governor will not give up the "
                          "range.",
                          asked);
        show_dialog(ui, message);
        return;
    }

    if (nd_cpufreq_read_state(&after) != ND_OK) {
        (void)nd_snprintf(message, sizeof message, "Pinned to %s.", asked);
        show_dialog(ui, message);
        return;
    }

    nd_cpufreq_format(now, sizeof now, after.cur_khz);
    (void)nd_snprintf(message, sizeof message, "Pinned to %s.\n\nNow: %s\nGovernor: %s", asked, now,
                      after.governor[0] != '\0' ? after.governor : "?");
    show_dialog(ui, message);
}

static void show_cpu_menu(nd_ui *ui)
{
    for (;;) {
        nd_cpufreq_table table;
        nd_cpufreq_state state;
        char labels[ND_CPUFREQ_MAX_STEPS][32];
        const char *items[ND_CPUFREQ_MAX_STEPS];
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;
        size_t i;

        /* Re-read every time round rather than once. The list itself cannot
         * change, but the pinned row can -- it changes every time somebody
         * picks one -- and coming back from a confirmation to a menu still
         * highlighting the old frequency would be a lie about the phone. */
        if (nd_cpufreq_read_table(&table) != ND_OK) {
            show_dialog(ui, nd_sleepy_no_cpufreq);
            return;
        }
        if (nd_cpufreq_read_state(&state) != ND_OK)
            memset(&state, 0, sizeof state);

        for (i = 0u; i < table.n; i++) {
            nd_cpufreq_format(labels[i], sizeof labels[i], table.khz[i]);
            items[i] = labels[i];
        }

        nd_vlist_init(&menu, ui, "CPU", items, table.n, SLEEPY_APP_ID);
        menu.selected_index = current_row(&table, &state);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return;
        if (choice >= 0 && (size_t)choice < table.n) {
            int32_t khz = table.khz[choice];
            nd_err set_result = nd_cpufreq_set(khz);

            nd_log(ND_LOG_SLEEPY, "Sleepy: pin %d kHz -> %s", khz, nd_strerror(set_result));
            report(ui, khz, set_result);
        }

        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    for (;;) {
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;

        nd_vlist_init(&menu, ui, nd_sleepy_title, nd_sleepy_root_items, SLEEPY_ROOT_ITEMS,
                      SLEEPY_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return 0;
        if (choice == 0)
            show_cpu_menu(ui);
        else if (choice == 1)
            show_display_menu(ui);

        if (nd_app_should_exit())
            return 0;
    }
}

/* The one thing this app can be holding that matters: a dark screen. SIGTERM
 * during a blank is an incoming call (nd_app.h), and the ringer is about to
 * start on a phone nobody can read. Turning the light back on is a single
 * sysfs write, which is well inside what this function is allowed to do.
 *
 * The CPU pin is deliberately NOT undone here -- see the header block. */
void app_shutdown(void)
{
    if (g_blanked) {
        (void)nd_backlight_on(100);
        g_blanked = false;
    }
}
