/* clock_app.h -- the strings and the shape of the Clock app, app id 8.
 *
 * Declared rather than left as literals inside main.c so that
 * test/unit/test_clock_app.c can dlsym() the built app.so and assert on the
 * artefact that ships, the way test_cubebench.c and test_phonebook.c do.
 *
 * The file is NOT called clock.h: lib/nd_clock.c and include/nd_clock.h are
 * the ClockService, which sets the machine's time. This app is the screen
 * that asks it to. They meet at exactly two functions -- nd_clock_set() and
 * nd_clock_ntp_enabled() -- and nowhere else.
 */

#ifndef ND_CLOCK_APP_H_INCLUDED
#define ND_CLOCK_APP_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App id 8 -- manifest.json, and the "8" the header draws before the "-1".
 * A string as well as a number because nd_pagedlist_init() takes the root id
 * as text and the two must not be able to drift apart. */
#define ND_CLOCK_APP_ID   8
#define ND_CLOCK_APP_ROOT "8"

/* ------------------------------------------------------------------ *
 * The root menu
 * ------------------------------------------------------------------ *
 *
 * A PagedList, one row per screen in big type, because that is what the phone
 * being imitated puts here and because three rows do not need a scrolling
 * list. Each row shows what it is currently SET to underneath its name --
 * "Off", "11:54 am", "On" -- so the state of all three is readable by paging
 * rather than by opening each one.
 */

#define ND_CLOCK_APP_ROWS 3

typedef enum { ND_CLOCK_ROW_ALARM = 0, ND_CLOCK_ROW_SETTINGS, ND_CLOCK_ROW_NTP } nd_clock_app_row;

extern const char *const nd_clock_app_title;
extern const char *const nd_clock_app_rows[ND_CLOCK_APP_ROWS];

/* ------------------------------------------------------------------ *
 * NTP time sync
 * ------------------------------------------------------------------ */

/* On first, so the row a phone arrives set to is the row that opens
 * highlighted. */
#define ND_CLOCK_NTP_OPTIONS 2
extern const char *const nd_clock_app_ntp_options[ND_CLOCK_NTP_OPTIONS];

/* ------------------------------------------------------------------ *
 * What it says when it will not do something
 * ------------------------------------------------------------------ */

/* Whether the clock may be set by hand right now -- which is exactly "is NTP
 * sync off". It is a function of its own, and main.c calls it rather than
 * testing the setting inline, because it is the one guard in this app that
 * prevents a confusing failure rather than causing one: without it the clock
 * accepts what was typed and the sync thread moves it back within the minute,
 * with nothing on screen admitting that happened.
 *
 * A test cannot reach this decision through the UI -- PagedList drains the key
 * channel before its first draw, so a scripted "Down, Enter" never arrives and
 * only a single held key survives. So the decision is exposed instead. */
bool nd_clock_app_may_set_manually(void);

/* Shown when Clock settings is opened while NTP is on. Setting the clock by
 * hand and then having the sync thread move it back within the minute is the
 * confusing failure this exists to prevent -- so the app refuses up front and
 * names the row that has to change. */
extern const char *const nd_clock_app_ntp_is_on;

/* The time and date fields both take a fixed number of digits and both refuse
 * an incomplete or impossible entry. See nd_timeset.h for what counts. */
extern const char *const nd_clock_app_bad_time;
extern const char *const nd_clock_app_bad_date;

/* The core would not set the clock: either it refused the date (outside what
 * this build will believe -- nd_svc.h) or the write itself failed. The app
 * cannot tell them apart and says one sentence for both. */
extern const char *const nd_clock_app_set_failed;

/* The alarm row exists and does not work yet; it says so rather than opening
 * an empty screen. */
extern const char *const nd_clock_app_no_alarms;

#ifdef __cplusplus
}
#endif

#endif /* ND_CLOCK_APP_H_INCLUDED */
