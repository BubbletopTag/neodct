/* calendar_app.h -- the shape of the Calendar app, app id 5.
 *
 * NOT called calendar.h: lib/nd_calendar.c and include/nd_calendar.h are the
 * event store, which the CORE also reads so that a reminder can reach the
 * home screen while this app is closed. This is the screen that writes into
 * it. They meet at the store's public functions and nowhere else -- the same
 * split, and the same naming, as clock_app.h against nd_clock.h.
 *
 * Everything a test needs to reach is declared here rather than left static,
 * so test/unit/test_calendar_app.c can dlopen() the BUILT app.so and assert
 * on the artefact that ships.
 *
 * ============ WHY THE MONTH GRID IS THE APP ============
 *
 * Every other stock app opens on a PagedList of its screens. This one opens
 * on the month, because a calendar's front page IS the month -- a list whose
 * first row is "Month view" would be a door in front of a door. The screens
 * the PagedList would have held are behind the NaviKey instead, which on this
 * phone is where a Nokia puts Options.
 *
 * ============ SIXTEEN KEYS, AND NO LEFT OR RIGHT ============
 *
 * The 5190's keypad is NaviKey, C, Up, Down, 1-9, 0, * and #. There is no
 * left and no right (nd_keypadsetup.c's enrolment list is the whole set), so
 * a grid cursor that needs two axes cannot get the second one from the rocker.
 *
 * It comes from the number pad, which has a 3x3 block sitting right there and
 * which MusicPlayer and Messages already use as a d-pad:
 *
 *     1  previous month     2  up, one week     3  next month
 *     4  previous day       5  today            6  next day
 *     7  previous year      8  down, one week   9  next year
 *
 * plus * and # as a second previous/next month, because they are the outer
 * pair and a thumb finds them without looking. Up and Down do what 2 and 8
 * do; Left and Right, which only a development QWERTY keyboard has, do what 4
 * and 6 do. On the phone the whole map is reachable; on QEMU it is
 * comfortable. Neither had to give anything up for the other.
 */

#ifndef ND_CALENDAR_APP_H_INCLUDED
#define ND_CALENDAR_APP_H_INCLUDED

#include "nd_calendar.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App id 5 -- manifest.json, and the "5" the header draws. The one free slot
 * in the stock menu's 1..12: the phone being imitated put Call Divert there
 * and this phone has never had one. A string as well as a number because the
 * widgets take the root id as text and the two must not be able to drift. */
#define ND_CAL_APP_ID   5
#define ND_CAL_APP_ROOT "5"

extern const char *const nd_cal_app_title;

/* ------------------------------------------------------------------ *
 * The month grid, as data
 * ------------------------------------------------------------------ */

#define ND_CAL_GRID_COLS  7
#define ND_CAL_GRID_ROWS  6
#define ND_CAL_GRID_CELLS (ND_CAL_GRID_ROWS * ND_CAL_GRID_COLS)

/* Six rows always, even for a month that fits in five. A grid that changed
 * height would move every other month's cells under the cursor as you paged,
 * which is the one thing a calendar must not do. */

typedef struct {
    int32_t year;
    int32_t month;
    int32_t day;
    bool in_month; /* false for the neighbouring month's days at either end */
} nd_cal_cell;

/* Lay the month containing (year, month) into 42 cells, starting on the
 * MONDAY on or before the 1st. Returns the index of the cell holding `day`
 * within that month, or -1 when the month is not a real one.
 *
 * Pure: no database, no drawing, no clock. It is the half of the month view
 * a test can hold still. */
int32_t nd_cal_grid_fill(int32_t year, int32_t month, int32_t day, nd_cal_cell *out);

/* ------------------------------------------------------------------ *
 * The month view's key map
 * ------------------------------------------------------------------ */

typedef enum {
    ND_CAL_NAV_NONE = 0, /* the key meant nothing here; do not redraw */
    ND_CAL_NAV_MOVED,    /* the cursor moved                          */
    ND_CAL_NAV_OPEN,     /* NaviKey                                   */
    ND_CAL_NAV_BACK      /* C                                         */
} nd_cal_nav;

/* One key against the cursor. The header block above is the specification;
 * this is it in one function, so the map can be tested without a panel --
 * which matters more here than anywhere else in the app, because it is the
 * part that has to work on a keypad the test machine does not have.
 *
 * ND_CAL_NAV_MOVED is returned even when the cursor was already at the edge
 * of the navigable range and did not actually move, so that holding a key
 * against the end of 2099 is a steady screen rather than a dead one -- the
 * same choice nd_vlist_show() makes at the ends of a list. */
nd_cal_nav nd_cal_month_key(int32_t key, int32_t *year, int32_t *month, int32_t *day);

/* ------------------------------------------------------------------ *
 * The month view itself
 * ------------------------------------------------------------------ *
 *
 * Every dimension is derived from the panel rather than written down, the
 * way nd_vlist_draw() derives its row metrics -- on this display it comes out
 * as a 34x16 cell with the grid at y = 48, and on a taller one it still
 * fills the content area. What IS fixed is six rows of seven, always.
 */

/* Clears rows 0..content_bottom ONLY, so a caller's
 * nd_softkey_update(..., false) survives into the frame this presents. That
 * is the same contract nd_vlist_draw() offers and the reason both cost one
 * framebuffer write per frame instead of two.
 *
 * `mask` is nd_cal_month_mask()'s answer for that month; passing it in rather
 * than querying keeps sqlite out of the render path. */
void nd_cal_month_draw(nd_ui *ui, int32_t year, int32_t month, int32_t day, uint32_t mask);

/* Draw, loop on keys, and return when NaviKey (true) or C (false) is pressed.
 * The cursor is read and written through the three pointers, so the caller
 * comes back to the month it left. The event mask is reloaded only when the
 * month under the cursor changes -- moving a day within one month is a redraw
 * and no query at all. */
bool nd_cal_month_show(nd_ui *ui, int32_t *year, int32_t *month, int32_t *day);

/* ------------------------------------------------------------------ *
 * The lists
 * ------------------------------------------------------------------ */

/* NaviKey on the month. "View day" is row 0 so that NaviKey, NaviKey opens
 * the day -- two presses on one key, no navigation between them. */
#define ND_CAL_OPTIONS_COUNT 4
typedef enum {
    ND_CAL_OPT_VIEW_DAY = 0,
    ND_CAL_OPT_NEW,
    ND_CAL_OPT_GOTO,
    ND_CAL_OPT_DELETE_ALL
} nd_cal_option;
extern const char *const nd_cal_app_options[ND_CAL_OPTIONS_COUNT];

/* NaviKey on an event. */
#define ND_CAL_EVENT_OPTIONS_COUNT 2
typedef enum { ND_CAL_EVOPT_EDIT = 0, ND_CAL_EVOPT_DELETE } nd_cal_event_option;
extern const char *const nd_cal_app_event_options[ND_CAL_EVENT_OPTIONS_COUNT];

/* The day list's first row, always present. It is what an empty day offers
 * instead of a dead end, and it is the same trick the chat front end's
 * conversation list plays with "New Message". */
extern const char *const nd_cal_app_new_row;

/* One row of the day list: the clock reading, then the title.
 * "9:00 am Team call".
 *
 * Formatting only -- the caller fits the result to the list's text width with
 * nd_text_ellipsize(), because that needs a font and this needs to be
 * checkable without one.
 *
 * `when` is the occurrence, not the event's first instant, so a weekly
 * reminder read back for next Tuesday says next Tuesday's time. An event
 * with no title reads as its clock time alone rather than as a trailing
 * space. */
#define ND_CAL_ROW_MAX 96
void nd_cal_app_row_label(char *out, size_t out_sz, int64_t when, const char *title);

/* "Sat 29", the day list's title. Short because a VerticalList title is
 * drawn at 24 px beside the breadcrumb and a spelled-out month would be
 * ellipsized to nothing; the month is on the screen it was opened from. */
void nd_cal_app_day_title(char *out, size_t out_sz, int32_t year, int32_t month, int32_t day);

/* ------------------------------------------------------------------ *
 * What it says
 * ------------------------------------------------------------------ */

/* Every one of these is written to the MessageDialog's budget -- a title
 * line and three at 14 px -- because it truncates rather than scrolling.
 * Clock's commit records the same lesson. */
/* There is deliberately no "nothing on this day" string. The day list always
 * carries "New event" as its first row, so an empty day is an offer rather
 * than a dead end and the screen that message was written for does not
 * exist. */
extern const char *const nd_cal_app_bad_time;    /* the time field would not parse  */
extern const char *const nd_cal_app_bad_date;    /* the date field would not parse  */
extern const char *const nd_cal_app_no_title;    /* an event needs a name           */
extern const char *const nd_cal_app_save_failed; /* the database would not take it  */
extern const char *const nd_cal_app_no_room;     /* the day list would not allocate */
extern const char *const nd_cal_app_gone;        /* opened from a banner, then gone */
extern const char *const nd_cal_app_empty;       /* "Delete all" with nothing to    */
extern const char *const nd_cal_app_delete_all_q;
extern const char *const nd_cal_app_delete_q;

#ifdef __cplusplus
}
#endif

#endif /* ND_CALENDAR_APP_H_INCLUDED */
