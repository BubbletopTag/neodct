/* nd_timeset.h -- typing a time and a date on a numeric keypad, and turning
 * what was typed into a time_t the clock will accept.
 *
 * The Clock app is the only consumer today. The decisions live here rather
 * than in the app for the reason nd_mic.h and nd_cpufreq.h both give: what a
 * test can reach is what gets tested, and an app's .so is the awkward half.
 *
 * ============ THE MASK IS THE WHOLE INPUT METHOD ============
 *
 * A phone keypad has ten digits and no colon. So a field that takes a time
 * cannot ask for one to be typed -- it has to put the separators in itself.
 * That is what a mask is: a template like "##:##" where every '#' is a slot a
 * digit fills and everything else is a literal the field emits on its own the
 * moment the slot before it is filled.
 *
 * Emitting the literal EAGERLY rather than waiting for the next digit is
 * deliberate. Type "1", "1" and the field reads "11:" -- the colon appearing
 * is what tells you the hour was accepted and the minutes are next. Held back
 * until the third digit, the same two presses leave "11" on screen and the
 * field looks like it is still collecting hours.
 *
 * Backspace undoes exactly one digit whichever side of a literal it lands on:
 * trailing literals come off first, then the digit under them. So "11:" takes
 * one press to become "1", not two, and there is no state where the cursor
 * sits on a separator with nothing to delete.
 *
 * ============ VALIDATED AGAINST THE CLOCK SERVICE'S OWN WINDOW ============
 *
 * A date is refused here when nd_clock.h would refuse it there -- the same
 * 2020..2100 sanity window ClockService uses. Accepting 01/01/1999 and then
 * having the clock silently overridden at the next boot is worse than saying
 * no while the keypad is still in the user's hand.
 */

#ifndef ND_TIMESET_H_INCLUDED
#define ND_TIMESET_H_INCLUDED

#include <time.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The mask engine
 * ------------------------------------------------------------------ */

/* The slot character. Everything else in a mask is a literal. */
#define ND_MASK_SLOT '#'

/* "##/##/####" is 10. 24 is slack for a mask nobody has needed yet, and a
 * fixed cap because CODING-STANDARDS.md section 4 puts nothing sized by input
 * on the stack. */
#define ND_MASK_MAX 24

/* type(): place one digit in the next slot, then emit every literal that
 * follows it. Returns false and changes nothing when the digit is not one,
 * when the mask is full, or when the caller's buffer would not hold the
 * result -- a full field ignoring a keypress is the Nokia's behaviour and is
 * better than truncating what is already there. */
bool nd_mask_type(const char *mask, char *text, size_t cap, char digit);

/* backspace(): remove trailing literals, then one digit. Returns false on an
 * already-empty field, which is what the caller turns into "leave the
 * field". */
bool nd_mask_backspace(const char *mask, char *text);

/* complete(): every slot filled. A field that is not complete cannot be
 * parsed and must not be accepted. */
bool nd_mask_complete(const char *mask, const char *text);

/* ------------------------------------------------------------------ *
 * The two masks this phone types into
 * ------------------------------------------------------------------ */

/* 24-hour, because a keypad has no am/pm key and adding a third screen to
 * ask would cost more than it saves. The phone still DISPLAYS 12-hour with a
 * suffix -- see nd_timeset_format_clock() -- because that is what the Nokia
 * shows and what the owner reads at a glance. Entry and display disagreeing
 * on purpose is normal for a clock. */
#define ND_TIMESET_TIME_MASK "##:##"

/* Day first. It matches the 24-hour entry convention beside it, and it is
 * what the phone this one imitates asked for. */
#define ND_TIMESET_DATE_MASK "##/##/####"

/* Longest of the two plus its NUL, for a caller's buffer. */
#define ND_TIMESET_TEXT_MAX 16

/* ------------------------------------------------------------------ *
 * Reading what was typed
 * ------------------------------------------------------------------ */

/* parse_time(): "HH:MM" in 24-hour form. Refuses an incomplete field, an hour
 * above 23 and a minute above 59. */
bool nd_timeset_parse_time(const char *text, int32_t *hour, int32_t *minute);

/* parse_date(): "DD/MM/YYYY". Refuses an incomplete field, a month outside
 * 1..12, a day outside the length of THAT month in THAT year -- 31/02 and
 * 29/02 in a common year are both refused -- and a year outside the clock
 * service's sanity window. */
bool nd_timeset_parse_date(const char *text, int32_t *day, int32_t *month, int32_t *year);

/* days_in_month(): 1..12 and a four-digit year in, 28..31 out, 0 for a month
 * that is not one. Exposed because February is where date validators go
 * wrong and a test should be able to say so directly. */
int32_t nd_timeset_days_in_month(int32_t month, int32_t year);

/* Proleptic Gregorian: divisible by 4, except centuries, except every 400th.
 * 2000 is a leap year and 1900 is not. */
bool nd_timeset_is_leap(int32_t year);

/* compose(): the six fields as LOCAL wall-clock time.
 *
 * mktime(), not timegm(): the user typed what they want the clock to read,
 * and that is local time by definition. tm_isdst is set to -1 so libc decides
 * for an hour that is ambiguous or does not exist -- guessing 0 there would
 * put the phone an hour out twice a year on any zoned build. The phone ships
 * on UTC, where none of this bites, which is exactly why it has to be right
 * before somebody changes the zone rather than after.
 *
 * Returns false when the fields do not describe a real instant, or when the
 * result falls outside the clock service's sanity window. */
bool nd_timeset_compose(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                        time_t *out);

/* ------------------------------------------------------------------ *
 * Showing it
 * ------------------------------------------------------------------ */

/* format_clock(): "11:54 am" -- 12-hour with a lower-case suffix, midnight as
 * 12:00 am and noon as 12:00 pm. What the Clock app's menu row shows, and
 * what the phone being imitated puts there. */
void nd_timeset_format_clock(char *out, size_t out_sz, time_t when);

/* The two fields' text as the mask would hold it, for prefilling a field with
 * the clock's current reading rather than making somebody type all of it. */
void nd_timeset_time_text(char *out, size_t out_sz, time_t when);
void nd_timeset_date_text(char *out, size_t out_sz, time_t when);

#ifdef __cplusplus
}
#endif

#endif /* ND_TIMESET_H_INCLUDED */
