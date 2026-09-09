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
 * this app is for: before there can be a sleep state there has to be an
 * answer to "does the backlight actually go off when you write to gpio53, and
 * does the CPU actually change speed when you write to scaling_min_freq".
 * Both answers are things you have to SEE, on the bench, with a phone in your
 * hand -- neither shows up in a unit test, which is exactly the lesson
 * MicTest's commit recorded.
 *
 * The first answer turned out to be no, for a year, for a reason no screen
 * here could show: the panel was blanked with a write to `brightness` and
 * never one to `bl_power`, so the kernel stored a zero it was not going to
 * act on and the write(2) came back successful. What this app said about
 * that failure was "Not root, or the pin is taken", which is a sentence about
 * an app that runs as root. Both halves are fixed -- nd_backlight.c makes the
 * write and verifies it, and every dialog here now repeats what the kernel
 * said instead of guessing.
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
 * The CPU pin is the other way round and on purpose: LEAVING the app does not
 * put the frequency back. An engineering app whose effect vanished the moment
 * you left it could not be used to measure anything, and nd_cpufreq.h says so
 * at greater length.
 *
 * That is not the same as having no way back, which is what it used to mean.
 * A pin held until the next reboot, on a screen offering nothing but other
 * frequencies, made the top row the obvious thing to press -- and the top row
 * pins the phone at full speed, which is worse than whatever sent somebody
 * looking. "Auto (unpinned)" is the row that was missing; it widens the range
 * to the silicon's own limits and it has to be chosen, like every other row
 * here.
 */

#include <stdlib.h>
#include <string.h>

#include "sleepy.h"

#include "nd_app.h"
#include "nd_cpufreq.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_theme.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

const char *const nd_sleepy_title = "Sleepy";

/* CPU first because it is the reversible one. */
const char *const nd_sleepy_root_items[SLEEPY_ROOT_ITEMS] = {"CPU", "Display"};

/* Keep the timed blank first so its existing menu shortcut stays the same.
 * "Screen off" is the one an owner actually wants -- dark until you touch it
 * -- and BLANK! stays because a fixed ten seconds is the measurement, and an
 * instrument you can walk away from is not the same instrument. */
const char *const nd_sleepy_display_items[SLEEPY_DISPLAY_ITEMS] = {"BLANK!", "Brightness",
                                                                   "Screen off"};

/* Short on purpose. The dialog gives a title line and three at 14 px, and it
 * TRUNCATES rather than scrolling -- a longer and more precise sentence
 * arrives on the phone with its second half missing, which is worse than a
 * blunt one. The precise version of each of these is in the log line and in
 * docs/SLEEP.md. */
const char *const nd_sleepy_no_cpufreq = "No cpufreq.\n\nThis kernel does not scale the CPU.";
const char *const nd_sleepy_no_backlight = "No backlight.\n\nNo PWM device and no gpio53.";

/* Set for exactly as long as the panel is dark, and read by app_shutdown().
 * volatile sig_atomic_t would be the careful type for a flag a handler reads,
 * but nd-apprun calls app_shutdown() from ordinary code rather than from the
 * handler (nd_app.h, step 3), so this is an ordinary read on the same thread
 * of control that wrote it. */
static bool g_blanked;
static int32_t g_wake_percent = 100;

/* ------------------------------------------------------------------ *
 * Display -> BLANK!
 * ------------------------------------------------------------------ */

/* Painted BEFORE the light goes out, because it cannot be painted after: on
 * the GPIO tier the framebuffer is untouched and simply invisible, so
 * whatever is on the canvas when the backlight dies is what greets you when
 * it comes back. Leaving the menu there would make the wake look like the
 * blank never happened. */
static void draw_wake_screen(nd_ui *ui, nd_bl_mode mode, double seconds)
{
    char headline[32];

    /* The chrome background, not a black fill: AGENTS.md's Conventions reserve
     * a literal fill for a surface that is not chrome, and this is a status
     * screen over the phone's own wallpaper. */
    nd_ui_paint_chrome_content(ui);
    nd_theme_text_light(ui->draw, 8, 4, nd_sleepy_title, ui->font_xl);
    /* font_md, not font_n. At 20 px this line runs off the right edge of a
     * 240 px panel and loses its full stop, which was found by looking at the
     * screen and could not have been found any other way. "Screen off." is
     * shorter than the line that was measured, so the untimed row is safe by
     * construction. */
    if (seconds < 0.0)
        (void)nd_strlcpy(headline, "Screen off.", sizeof headline);
    else
        (void)nd_snprintf(headline, sizeof headline, "Screen off for %.0f s.", seconds);
    nd_theme_text_light(ui->draw, 8, 42, headline, ui->font_md);
    nd_theme_text(ui->draw, 8, 74, mode == ND_BL_PWM ? "Tier: PWM backlight" : "Tier: gpio53 (BL)",
                  ui->font_s, ND_TH_SKY_TOP, ND_RGB(0x08, 0x1E, 0x33));
    /* Stated because the timed measurement depends on nobody pressing
     * anything, and somebody who does not know the escape exists will press
     * everything. On the untimed row a key is not an escape, it is the only
     * way out, so the line is not optional there. */
    nd_theme_text(ui->draw, 8, 96, seconds < 0.0 ? "Any key wakes it." : "Any key wakes it early.",
                  ui->font_s, ND_TH_SKY_TOP, ND_RGB(0x08, 0x1E, 0x33));
}

static void show_dialog(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_sleepy_title);
    (void)nd_msgdialog_show(&dialog);
}

static bool restore_backlight(void)
{
    if (!g_blanked)
        return true;
    if (!nd_backlight_on(g_wake_percent))
        return false;
    g_blanked = false;
    return true;
}

/* One dialog shape for every backlight failure, because the app is no longer
 * in the business of guessing which one it hit. It used to say "Not root, or
 * the pin is taken" -- about an app that nd_proc.c launches as root, so the
 * first half was impossible and the second was a guess -- and "Check backlight
 * permissions" for writes the kernel had refused for other reasons entirely.
 * nd_backlight_last_error() carries the kernel's own word for it. */
static void show_backlight_error(nd_ui *ui, const char *what)
{
    const char *why = nd_backlight_last_error();
    char message[192];

    (void)nd_snprintf(message, sizeof message, "%s\n\n%s", what,
                      why[0] != '\0' ? why : "No reason given.");
    show_dialog(ui, message);
}

/* `seconds` negative means SLEEPY_BLANK_UNTIL_KEY: dark until somebody presses
 * something. The two rows differ in that number and in nothing else, which is
 * the point -- the restore guarantee at the top of this file has to hold for
 * both, and there is only one copy of it. */
static void blank(nd_ui *ui, double seconds)
{
    nd_bl_mode mode = nd_backlight_mode();
    nd_softkey bar;
    double until;

    if (mode == ND_BL_NONE) {
        show_dialog(ui, nd_sleepy_no_backlight);
        return;
    }

    draw_wake_screen(ui, mode, seconds);
    /* Repainted, not left alone. draw_wake_screen() clears rows 0..145 and
     * the strip below is whatever the menu underneath put there -- which is
     * "Select", on a screen where there is nothing to select. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Wake", false);
    if (nd_ui_present(ui) != ND_OK)
        return;

    /* A blank/wake measurement must not overwrite the level just chosen in
     * Brightness. An unreadable or already-dark panel still wakes at full. */
    g_wake_percent = nd_backlight_get_percent();
    if (g_wake_percent <= 0)
        g_wake_percent = 100;

    if (!nd_backlight_off()) {
        /* The tier said it was there and the write still did not take. Which
         * of the several reasons that can be is the kernel's to say, not this
         * app's to guess -- "Permission denied" and "the panel ignored it"
         * send an engineer to two very different places. */
        show_backlight_error(ui, "The backlight stayed on.");
        return;
    }
    g_blanked = true;
    if (seconds < 0.0)
        nd_log(ND_LOG_SLEEPY, "Sleepy: panel dark (%s) until a key",
               mode == ND_BL_PWM ? "pwm" : "gpio53");
    else
        nd_log(ND_LOG_SLEEPY, "Sleepy: panel dark (%s) for %.0f s",
               mode == ND_BL_PWM ? "pwm" : "gpio53", seconds);

    /* Nothing is drawn and nothing is presented for the whole ten seconds.
     * That is not laziness -- it is the closest this app gets to the thing it
     * is a rehearsal for. A countdown would mean rasterising and blitting
     * frames nobody can see, on the one core, while measuring how little the
     * phone can get away with doing.
     *
     * So the only work in the loop is the wait itself, and the timeout is the
     * wake rate: half a second is four wakes shorter than a tenth would be
     * and still ends the blank inside a keypress of being asked to. */
    until = nd_time_monotonic() + (seconds < 0.0 ? 0.0 : seconds);
    for (;;) {
        if (seconds >= 0.0 && nd_time_monotonic() >= until)
            break;
        if (nd_ui_read_keypress(ui, 0.5) != ND_KEY_NONE)
            break;
        if (nd_app_should_exit())
            break;
    }

    if (!restore_backlight()) {
        /* Leave g_blanked set so shutdown can retry the restore. */
        show_backlight_error(ui, "Backlight wake failed.");
        return;
    }
    nd_log(ND_LOG_SLEEPY, "Sleepy: panel lit");
}

/* The level the owner last chose, 1..SLEEPY_BRIGHTNESS_LEVELS, or 0 when
 * nothing has been chosen on this phone.
 *
 * Reading it back from the panel would nearly work and is what this screen
 * used to do -- level to percent to a raw sysfs value and all the way back.
 * On the panel this phone has (max_brightness 10) that round trip is exact,
 * which is why nobody noticed it is a hostage to the device tree: with a
 * seven-step brightness-levels table, level 5 comes back as level 6 and the
 * picker opens one row below where it was left. What somebody chose is a fact
 * worth storing rather than reconstructing. */
static int32_t stored_level(void)
{
    char text[16];
    int32_t percent;

    (void)nd_settings_get_copy(ND_SET_UI_BRIGHTNESS, "", text, sizeof text);
    if (text[0] == '\0')
        return 0;
    percent = (int32_t)strtol(text, NULL, 10);
    if (percent <= 0)
        return 0;
    /* Clamped BEFORE the rounding, not after. A hand-edited settings.prop
     * saying 2147483647 fits the buffer, and (percent + 5) then overflows --
     * undefined, a diagnostic under ASAN=1, and in practice a wrap to the
     * dimmest row for a value claiming to be the brightest. 0..100 is the
     * domain nd_backlight_on() accepts anyway. */
    percent = nd_clamp32(percent, 0, 100);
    return nd_clamp32((percent + 5) / 10, 1, SLEEPY_BRIGHTNESS_LEVELS);
}

/* Stored as a PERCENTAGE, not as a level. The core re-applies it at boot
 * (nd_main.c, step 4a-quater) through the same nd_backlight_on() this screen
 * calls, and that API speaks percent; storing a level would put this app's
 * ten-step table into a settings file the core would then have to know how to
 * read. */
static void store_level(int32_t level)
{
    char text[16];

    if (nd_snprintf(text, sizeof text, "%d", level * 10) == ND_OK)
        (void)nd_settings_set(ND_SET_UI_BRIGHTNESS, text);
}

static void show_brightness(nd_ui *ui)
{
    nd_bl_mode mode = nd_backlight_mode();

    if (mode == ND_BL_NONE) {
        show_dialog(ui, nd_sleepy_no_backlight);
        return;
    }
    if (mode != ND_BL_PWM) {
        show_dialog(ui, "No PWM dimming.\n\nGPIO is on/off only.");
        return;
    }

    while (!nd_app_should_exit()) {
        nd_levelsel picker;
        int32_t current = stored_level();
        int32_t picked;

        if (current <= 0) {
            /* Nothing stored yet, so ask the panel. Derived rather than
             * remembered only on the first visit -- see stored_level(). */
            int32_t percent = nd_backlight_get_percent();

            if (percent < 0) {
                show_backlight_error(ui, "Cannot read brightness.");
                return;
            }
            current = nd_clamp32((percent + 5) / 10, 1, SLEEPY_BRIGHTNESS_LEVELS);
        }
        nd_levelsel_init(&picker, ui, current, SLEEPY_BRIGHTNESS_LEVELS, "Brightness",
                         SLEEPY_APP_ID);
        picked = nd_levelsel_show(&picker);
        if (picked == ND_WIDGET_BACK || nd_app_should_exit())
            return;

        /* Level 5 is the middle table index, not 50% optical brightness.
         * The API scales onto max_brightness; the kernel owns the curve. */
        if (!nd_backlight_on(picked * 10)) {
            show_backlight_error(ui, "Brightness refused.");
            return;
        }
        /* Only after the panel actually took it. A settings file that
         * remembers a level the hardware refused would be re-applied at every
         * boot by nd_main.c and fail there too, silently, forever. */
        store_level(picked);
        nd_log(ND_LOG_SLEEPY, "Sleepy: brightness level %d/%d", picked, SLEEPY_BRIGHTNESS_LEVELS);
    }
}

static void show_display_menu(nd_ui *ui)
{
    for (;;) {
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;

        nd_vlist_init(&menu, ui, "Display", nd_sleepy_display_items, SLEEPY_DISPLAY_ITEMS,
                      SLEEPY_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return;
        if (choice == 0)
            blank(ui, SLEEPY_BLANK_SECONDS);
        else if (choice == 1)
            show_brightness(ui);
        else if (choice == 2)
            blank(ui, SLEEPY_BLANK_UNTIL_KEY);
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

    /* The Auto row sits after the frequencies, and an unpinned phone belongs
     * on it rather than on whichever frequency the governor happens to be
     * passing through. Highlighting one of those would say the phone is held
     * there, which is the opposite of what unpinned means. */
    if (nd_cpufreq_is_unpinned(state))
        return table->n;

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
static void report(nd_ui *ui, const char *asked, const char *headline, nd_err set_result)
{
    nd_cpufreq_state after;
    char message[256];
    char now[32];

    if (set_result != ND_OK) {
        /* Not "not root": this app runs as root, and saying otherwise sent
         * the last engineer who read it to the wrong place for a day. What is
         * actually true is that the write returned and the range did not
         * move. */
        (void)nd_snprintf(message, sizeof message,
                          "%s was refused.\n\nThe kernel kept the range it had.", asked);
        show_dialog(ui, message);
        return;
    }

    if (nd_cpufreq_read_state(&after) != ND_OK) {
        show_dialog(ui, headline);
        return;
    }

    nd_cpufreq_format(now, sizeof now, after.cur_khz);
    (void)nd_snprintf(message, sizeof message, "%s\n\nNow: %s\nGovernor: %s", headline, now,
                      after.governor[0] != '\0' ? after.governor : "?");
    show_dialog(ui, message);
}

static void show_cpu_menu(nd_ui *ui)
{
    for (;;) {
        nd_cpufreq_table table;
        nd_cpufreq_state state;
        char labels[ND_CPUFREQ_MAX_STEPS][32];
        /* One longer than the table: the frequencies, then Auto. */
        const char *items[ND_CPUFREQ_MAX_STEPS + 1u];
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
        /* Appended, not prepended. The frequencies are the measurement and
         * the way out belongs after them, in the place a Back-adjacent row
         * goes everywhere else in this OS. */
        items[table.n] = SLEEPY_CPU_AUTO_LABEL;

        nd_vlist_init(&menu, ui, "CPU", items, table.n + 1u, SLEEPY_APP_ID);
        menu.selected_index = current_row(&table, &state);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return;
        if (choice >= 0 && (size_t)choice == table.n) {
            /* Both bounds left open, so nd_cpufreq_set_range() reads them off
             * cpuinfo_min_freq and cpuinfo_max_freq -- the numbers that were
             * true before anybody pinned anything, which is the only honest
             * definition of "unpinned" once scaling_* has been written. */
            nd_err set_result = nd_cpufreq_set_range(0, 0);

            nd_log(ND_LOG_SLEEPY, "Sleepy: unpin -> %s", nd_strerror(set_result));
            report(ui, SLEEPY_CPU_AUTO_LABEL, "Range opened up.", set_result);
        } else if (choice >= 0 && (size_t)choice < table.n) {
            int32_t khz = table.khz[choice];
            nd_err set_result = nd_cpufreq_set(khz);
            char asked[32];
            char headline[64];

            nd_cpufreq_format(asked, sizeof asked, khz);
            (void)nd_snprintf(headline, sizeof headline, "Pinned to %s.", asked);
            nd_log(ND_LOG_SLEEPY, "Sleepy: pin %d kHz -> %s", khz, nd_strerror(set_result));
            report(ui, asked, headline, set_result);
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
    (void)restore_backlight();
}
