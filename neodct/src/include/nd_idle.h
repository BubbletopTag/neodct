/* nd_idle.h -- the home screen's idle dimmer.
 *
 * Sixty seconds with nobody touching the keypad, and the phone drops the
 * panel to its dimmest lit step and pins the CPU to 600 MHz. The next key
 * puts both back. That is the whole feature, and it is deliberately not
 * called sleep: nothing is suspended, no clocks stop, the modem stays
 * registered and the screen keeps its picture. It is the cheap nine tenths of
 * sleep that costs no bring-up.
 *
 * ============ WHY THE HOME SCREEN AND NOT EVERYWHERE ============
 *
 * The tick lives in nd_main.c's loop, which runs only while the home screen
 * is the thing on the panel: opening a menu or an app hands control to that
 * screen's own key loop and this one stops being called. So a menu left open
 * never dims, and an app is never dimmed underneath itself -- which is the
 * behaviour a media player, a download or a game needs, and getting it for
 * free from where the code sits is better than getting it from a list of
 * exceptions that would have to be maintained.
 *
 * The cost is that a phone left in Settings stays bright. That is a real gap
 * and a deliberate one; widening it means moving the tick into
 * nd_ui_read_keypress(), which every screen shares, and then teaching it
 * which screens must be exempt.
 *
 * ============ WHY THE DECISION IS A SEPARATE PURE FUNCTION ============
 *
 * nd_idle_should_dim() takes four numbers and returns a bool, so the boundary
 * -- 59 seconds is not idle, 60 is -- is checkable without a panel, a CPU
 * that scales, or a clock that has to really advance. The half that touches
 * hardware is then small enough to read.
 */

#ifndef ND_IDLE_H_INCLUDED
#define ND_IDLE_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sixty seconds, as asked for. Long enough that reading a message on the home
 * screen does not dim under you, short enough to be worth having. */
#define ND_IDLE_TIMEOUT_S 60.0

/* Level 1 of the panel's ten steps. nd_backlight_set_percent() scales a
 * percentage onto max_brightness, so on this board's ten-step table 10%
 * lands exactly on brightness=1 -- the dimmest step that is still lit, and
 * well clear of ND_BL_MIN_ON_PERCENT. */
#define ND_IDLE_DIM_PERCENT 10

/* 600 MHz is the second entry in the RV1103's OPP table (408, 600, 816,
 * 1104 on this kernel), so it is a real operating point and not a request the
 * driver has to round. 408 would be lower and is deliberately not used: the
 * home screen still animates a wallpaper and still repaints a clock, and the
 * bottom step makes that visibly jerky for a few milliwatts. */
#define ND_IDLE_CPU_KHZ 600000

/* Everything the dimmer has to remember to be able to undo itself. Owned by
 * the caller and zero-initialised by nd_idle_init(); there is no global,
 * because the one thing worse than a phone that will not dim is two of these
 * disagreeing about whether it already did. */
typedef struct {
    double last_activity; /* monotonic seconds, from nd_time_monotonic() */
    bool dimmed;          /* whether the phone is currently held down */
    int32_t wake_percent; /* brightness to restore, -1 when unknown */
    int32_t wake_min_khz; /* the range to restore, -1 when we did not move it */
    int32_t wake_max_khz;
    bool cpu_moved; /* whether the CPU write actually happened */
} nd_idle;

/* Start the clock. `now` is nd_time_monotonic(); passed in rather than read
 * here so that a test owns time. */
void nd_idle_init(nd_idle *s, double now);

/* THE DECISION, and nothing else. True when a phone that is not already
 * dimmed has been idle for at least `timeout_s`.
 *
 * A `now` before `last_activity` cannot happen on CLOCK_MONOTONIC and is
 * treated as not-yet-idle anyway: a clock that has gone backwards is a reason
 * to do nothing, not a reason to dim a phone somebody is holding. */
bool nd_idle_should_dim(bool dimmed, double last_activity, double now, double timeout_s);

/* Somebody pressed something. Restarts the countdown without touching the
 * panel -- call this after a menu or an app returns, where the keys went to
 * that screen's loop and this one never saw them. */
void nd_idle_note_activity(nd_idle *s, double now);

/* One beat of the idle countdown. Dims when nd_idle_should_dim() says so and
 * does nothing at all otherwise, so it is cheap enough for a 10 Hz loop.
 *
 * A phone with no backlight and no cpufreq -- QEMU, every time -- takes this
 * silently and still records that it dimmed, so that the matching wake is
 * still a matched pair. */
void nd_idle_tick(nd_idle *s, double now);

/* Put the panel and the CPU back, if they were moved, and restart the
 * countdown. Safe and cheap when nothing was dimmed, which is the common
 * case: every keypress on the home screen goes through here.
 *
 * MUST also be called on the way out of the main loop. A phone that powers
 * off dimmed is merely odd; one that reboots into recovery dimmed, or hands a
 * pinned 600 MHz CPU to the updater, is not. */
void nd_idle_wake(nd_idle *s, double now);

#ifdef __cplusplus
}
#endif

#endif /* ND_IDLE_H_INCLUDED */
