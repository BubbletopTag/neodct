/* nd_idle.c -- dim the panel and slow the CPU when nobody is using the phone.
 *
 * See the block in nd_idle.h for what this is and why it lives on the home
 * screen only. This file is the half that touches hardware, and every bit of
 * it is arranged around one rule: A DIM MUST ALWAYS BE UNDOABLE.
 *
 * That is why the state to restore is captured BEFORE anything is written,
 * why `dimmed` is set even when the writes fail, and why nd_idle_wake()
 * restores on a best-effort basis rather than giving up at the first refusal.
 * A phone that fails to dim is a phone with a slightly worse battery. A phone
 * that dims and cannot undo it is a brick with a picture on it.
 *
 * ============ WHY FAILURES ARE LOGGED ONCE AND NOT PER TICK ============
 *
 * This runs at 10 Hz and dims once a minute. A dim that fails for a standing
 * reason -- no permission on bl_power, a kernel with no cpufreq -- would fail
 * the same way every minute for as long as the phone is on, and a serial
 * console full of one repeated line is a console nobody reads. So each
 * distinct complaint is said once per process, the same discipline
 * nd_backlight.c uses and for the same reason.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_cpufreq.h"
#include "nd_fb.h"
#include "nd_idle.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_vclock.h"

/* ------------------------------------------------------------------ *
 * Saying a thing once
 * ------------------------------------------------------------------ */

/* One latch per distinct complaint rather than one shared one: "the panel
 * would not dim" and "the CPU would not slow" are different repairs, and a
 * single latch would let whichever happened first hide the other for ever. */
static bool g_said_backlight_dim;
static bool g_said_backlight_wake;
static bool g_said_cpu_dim;
static bool g_said_cpu_wake;

/* ------------------------------------------------------------------ *
 * The decision
 * ------------------------------------------------------------------ */

bool nd_idle_should_dim(bool dimmed, double last_activity, double now, double timeout_s)
{
    if (dimmed)
        return false;
    if (timeout_s <= 0.0)
        return false;
    /* Not (now - last >= timeout), which would be true for a `now` that has
     * gone backwards far enough to wrap the subtraction into a large
     * positive. Comparing the instants directly cannot do that. */
    if (now < last_activity)
        return false;
    return now - last_activity >= timeout_s;
}

/* ------------------------------------------------------------------ *
 * Capturing what has to be put back
 * ------------------------------------------------------------------ */

/* The brightness to wake to, in percent.
 *
 * The panel is asked first, because what is on the screen right now is the
 * truest answer and it is what an owner who just moved the slider expects to
 * come back to. The stored setting is the fallback for a panel that will not
 * report -- and 100 is the fallback for that, because a phone that wakes too
 * bright is a phone somebody can fix, and one that wakes to nothing is not.
 *
 * Deliberately NOT capturing a percentage we are about to overwrite: this is
 * called before nd_backlight_on(), never after, or the dim level would be
 * captured as the wake level and the phone would stay dim for ever. */
static int32_t capture_wake_percent(void)
{
    char stored[16];
    int32_t percent = nd_backlight_get_percent();

    if (percent > 0)
        return percent;

    (void)nd_settings_get_copy(ND_SET_UI_BRIGHTNESS, ND_SET_UI_BRIGHTNESS_DFLT, stored,
                               sizeof stored);
    percent = (int32_t)strtol(stored, NULL, 10);
    if (percent > 0 && percent <= 100)
        return percent;
    return 100;
}

/* ------------------------------------------------------------------ *
 * The two halves of a dim
 * ------------------------------------------------------------------ */

/* Silent when there is no backlight at all. QEMU has none, and a line about
 * it once a minute would be noise about a panel that does not exist --
 * nd_main.c's brightness restore makes exactly the same call for exactly the
 * same reason. */
static void dim_panel(nd_idle *s)
{
    if (!nd_backlight_available())
        return;

    s->wake_percent = capture_wake_percent();
    if (nd_backlight_on(ND_IDLE_DIM_PERCENT))
        return;

    /* The write may still have LANDED: nd_backlight_set_percent() reports
     * failure when the bl_power write is refused even though the brightness
     * write before it verified. So this does not clear wake_percent -- the
     * restore has to happen either way. */
    if (!g_said_backlight_dim) {
        g_said_backlight_dim = true;
        nd_log_err(ND_LOG_UI, "Idle dim: the panel would not go to %d%%: %s",
                   ND_IDLE_DIM_PERCENT, nd_backlight_last_error());
    }
}

/* Pin both ends of the range to 600 MHz.
 *
 * nd_cpufreq_set() rather than lowering the ceiling alone, because the point
 * is to hold the chip down: leaving the floor at 408 and the ceiling at 600
 * would let ondemand wander between them, which is a smaller saving and a
 * less measurable one. Sleepy's CPU screen pins the same way.
 *
 * The range in force is captured first so that a phone somebody has already
 * pinned by hand -- Sleepy does exactly this -- gets its own pin back rather
 * than being quietly unpinned by the next keypress. */
static void slow_cpu(nd_idle *s)
{
    nd_cpufreq_state state;

    /* No cpufreq directory at all is QEMU and any kernel built without
     * CONFIG_CPU_FREQ. A fact about the machine, so it is silent. */
    if (nd_cpufreq_read_state(&state) != ND_OK)
        return;
    if (state.min_khz <= 0 || state.max_khz <= 0)
        return;
    /* Already at or below where we would put it: nothing to do, and nothing
     * to restore. Without this, a phone pinned to 408 by hand would be
     * RAISED to 600 by going idle. */
    if (state.max_khz <= ND_IDLE_CPU_KHZ)
        return;

    if (nd_cpufreq_set(ND_IDLE_CPU_KHZ) != ND_OK) {
        if (!g_said_cpu_dim) {
            g_said_cpu_dim = true;
            nd_log_err(ND_LOG_OS,
                       "Idle dim: cannot pin the CPU to %d kHz. Check that "
                       "61-neodct-devices.rules has given scaling_min_freq and "
                       "scaling_max_freq to the group nd-core runs as.",
                       ND_IDLE_CPU_KHZ);
        }
        return;
    }

    s->wake_min_khz = state.min_khz;
    s->wake_max_khz = state.max_khz;
    s->cpu_moved = true;
}

/* ------------------------------------------------------------------ *
 * The public four
 * ------------------------------------------------------------------ */

void nd_idle_init(nd_idle *s, double now)
{
    if (s == NULL)
        return;
    memset(s, 0, sizeof *s);
    s->last_activity = now;
    s->wake_percent = -1;
    s->wake_min_khz = -1;
    s->wake_max_khz = -1;
}

void nd_idle_note_activity(nd_idle *s, double now)
{
    if (s == NULL)
        return;
    s->last_activity = now;
}

void nd_idle_tick(nd_idle *s, double now)
{
    if (s == NULL)
        return;
    if (!nd_idle_should_dim(s->dimmed, s->last_activity, now, ND_IDLE_TIMEOUT_S))
        return;

    /* Set BEFORE the writes, not after. Both helpers can fail and both can
     * fail having already changed something; the flag is what guarantees the
     * matching nd_idle_wake(), so it must not be conditional on either of
     * them having worked. */
    s->dimmed = true;
    dim_panel(s);
    slow_cpu(s);
    nd_log(ND_LOG_UI, "Idle: %.0f s with no key, panel to %d%% and CPU to %d kHz",
           ND_IDLE_TIMEOUT_S, ND_IDLE_DIM_PERCENT, ND_IDLE_CPU_KHZ);
}

void nd_idle_wake(nd_idle *s, double now)
{
    if (s == NULL)
        return;

    /* The countdown restarts whether or not anything was dimmed -- this is
     * the ordinary keypress path and it is the cheap case, one store. */
    s->last_activity = now;
    if (!s->dimmed)
        return;
    s->dimmed = false;

    /* CPU FIRST. The panel coming back is what the owner sees, and they see
     * it as part of whatever the key they pressed does next -- a menu
     * opening, an app starting. Doing that redraw at 600 MHz and then
     * speeding up afterwards would put the slow frame exactly where it is
     * most visible. */
    if (s->cpu_moved) {
        if (nd_cpufreq_set_range(s->wake_min_khz, s->wake_max_khz) != ND_OK && !g_said_cpu_wake) {
            g_said_cpu_wake = true;
            nd_log_err(ND_LOG_OS,
                       "Idle wake: could not put the CPU range back to %d-%d kHz; the phone is "
                       "left slow",
                       s->wake_min_khz, s->wake_max_khz);
        }
        s->cpu_moved = false;
        s->wake_min_khz = -1;
        s->wake_max_khz = -1;
    }

    if (s->wake_percent > 0) {
        if (!nd_backlight_on(s->wake_percent) && !g_said_backlight_wake) {
            g_said_backlight_wake = true;
            nd_log_err(ND_LOG_UI, "Idle wake: the panel would not go back to %d%%: %s",
                       s->wake_percent, nd_backlight_last_error());
        }
        s->wake_percent = -1;
    }
}
