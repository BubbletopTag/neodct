/* nd_alarm.h -- one alarm, the time it goes off, and whether it has.
 *
 * ============ WHY THIS IS IN lib/ AND NOT IN THE CLOCK APP ============
 *
 * The Clock app is not running when the alarm comes due -- that is the whole
 * point of an alarm. Nobody is there to notice. The core is the only process
 * still alive and the only one holding the NotifyService handle, so the CORE
 * reads this store on a tick and posts the banner, exactly as it does for a
 * calendar reminder. references/notifications.md states the rule: a notifying
 * feature's data layer belongs in lib/. The app is just the other caller.
 *
 * ============ ONE ALARM, IN SETTINGS, NOT A TABLE IN SQLITE ============
 *
 * The feature asked for is "type a time and it goes off at that time". That
 * is one alarm, and one alarm is two short strings -- so it lives in
 * settings.prop beside every other preference rather than in a database that
 * would have to be opened, migrated and vacuumed to hold four digits.
 *
 * The cost of the decision is that a second alarm cannot be added without
 * moving the store, and that is the right trade at this size: nd_calendar.c
 * already exists for the case where rows are the answer, and this is not it.
 */

#ifndef ND_ALARM_H_INCLUDED
#define ND_ALARM_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "HHMM", or empty for no alarm. Four digits and not "HH:MM" because the
 * separator would be one more thing a hand-edited settings.prop could get
 * wrong for no gain. */
#define ND_SET_ALARM_TIME "system.clock.alarm"

/* "YYYYMMDD" of the last day this alarm went off, so it goes off once a day
 * and not once per tick for the whole minute. */
#define ND_SET_ALARM_FIRED "system.clock.alarm_fired"

/* ============ HOW LATE IS TOO LATE ============
 *
 * A phone that was switched off at 07:00 and switched on at 14:00 must not
 * blare the moment it boots. But one whose core restarted at 07:00:20, or
 * that was busy in an app across the minute, must still ring.
 *
 * So a due alarm is one whose time has passed today, that has not already
 * gone off today, AND that is no more than this far past. Anything older is
 * marked as fired without a sound: missed, not queued. */
#define ND_ALARM_LATE_S 300

/* How often the core asks. The store is a small properties file rather than a
 * database, but it is still a read, and a minute-resolution alarm does not
 * need asking ten times a second. Five seconds keeps the worst-case lateness
 * comfortably inside the window above. */
#define ND_ALARM_POLL_S 5.0

/* The alarm sound. Rung -- looped until dismissed -- rather than played once,
 * because it is meant to wake somebody. See nd_notify_start_ring_file().
 *
 * NOT under /NeoDCT/System/tones, and that is the whole reason this directory
 * exists. nd_tones_scan() walks the tones tree recursively to build the
 * ringtone picker, so a file left there would be offered as something to
 * receive calls on -- and an alarm blare is the last thing anybody wants a
 * call to sound like. Assets the phone plays but nobody chooses live here. */
#define ND_ALARM_TONE "/NeoDCT/System/sounds/alarm.mp3"

typedef struct {
    bool set;       /* false when no alarm is stored */
    int32_t hour;   /* 0..23 */
    int32_t minute; /* 0..59 */
} nd_alarm;

/* ------------------------------------------------------------------ *
 * Pure -- no settings, no clock. The test drives these directly.
 * ------------------------------------------------------------------ */

/* "0730" or "07:30" into hour and minute. False for anything that is not a
 * real time of day, INCLUDING "2400" and "0760": a phone that accepted those
 * would store an alarm that could never come due. */
bool nd_alarm_parse(const char *text, int32_t *hour_out, int32_t *minute_out);

/* "07:30", always NUL-terminated. An unset alarm formats as "Off", so the
 * Clock app's row and any other reader cannot disagree about the wording. */
void nd_alarm_format(const nd_alarm *a, char *out, size_t out_sz);

/* Should this alarm sound now?
 *
 * `now` is a local-time broken-down clock; `today` and `fired_day` are
 * YYYYMMDD, the day it is and the day the alarm last sounded. Split out from
 * the settings and the system clock so that every branch -- not yet, already
 * gone off, too late, right now -- is checkable without waiting for a
 * particular minute of a particular day to come round. */
bool nd_alarm_due(const nd_alarm *a, int32_t now_hour, int32_t now_minute, int32_t today,
                  int32_t fired_day, int32_t *lateness_s_out);

/* ------------------------------------------------------------------ *
 * Stored -- these read and write settings.prop.
 * ------------------------------------------------------------------ */

/* What is stored, or {false,0,0}. A stored value that will not parse is the
 * same as none: a settings file somebody edited by hand must not be able to
 * put the core into a state where it cannot decide. */
void nd_alarm_load(nd_alarm *out);

/* Store one, and clear the fired-day marker so an alarm set for a time that
 * has ALREADY passed today does not count as already gone off. */
nd_err nd_alarm_save(int32_t hour, int32_t minute);

/* Forget it. */
nd_err nd_alarm_clear(void);

/* ------------------------------------------------------------------ *
 * The core's tick
 * ------------------------------------------------------------------ */

/* Has the alarm come due since the last call? True AT MOST ONCE per day:
 * the fired-day marker is written before this returns true, so a caller that
 * is called again a tick later does not ring twice.
 *
 * Also silently consumes an alarm that is too late (ND_ALARM_LATE_S) -- see
 * the block on that constant. `out` receives what went off, for the banner.
 */
bool nd_alarm_take_due(nd_alarm *out);

#ifdef __cplusplus
}
#endif

#endif /* ND_ALARM_H_INCLUDED */
