/* test_timeset.c -- typing a time on a keypad that has no colon key.
 *
 * The mask engine is the interesting half and it is pure: no filesystem, no
 * clock, nothing to stage. The formatting half reads the clock through
 * nd_time_localtime(), so those cases pin the virtual clock rather than
 * asking what time it is now.
 */

#include <string.h>

#include "nd_timeset.h"
#include "nd_vclock.h"

#include "platform_test.h"

#define TIME_MASK ND_TIMESET_TIME_MASK /* "##:##"      */
#define DATE_MASK ND_TIMESET_DATE_MASK /* "##/##/####" */

/* Type a whole string of digits into a field, one press each. */
static void type_all(const char *mask, char *text, size_t cap, const char *digits)
{
    size_t i;

    for (i = 0u; digits[i] != '\0'; i++)
        (void)nd_mask_type(mask, text, cap, digits[i]);
}

/* ------------------------------------------------------------------ *
 * The mask engine
 * ------------------------------------------------------------------ */

/* The colon arrives on the SECOND press, not the third. That is the whole
 * point of emitting literals eagerly: the separator appearing is what tells
 * you the hour was taken and the minutes are next. */
static void test_the_separator_appears_with_the_digit_before_it(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    CHECK(nd_mask_type(TIME_MASK, text, sizeof text, '1'));
    CHECK_STR(text, "1");
    CHECK(nd_mask_type(TIME_MASK, text, sizeof text, '1'));
    CHECK_STR(text, "11:");
    CHECK(nd_mask_type(TIME_MASK, text, sizeof text, '5'));
    CHECK_STR(text, "11:5");
    CHECK(nd_mask_type(TIME_MASK, text, sizeof text, '4'));
    CHECK_STR(text, "11:54");
}

/* The date mask has two literals and four digits at the end. */
static void test_the_date_mask_fills_the_same_way(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    type_all(DATE_MASK, text, sizeof text, "2708");
    CHECK_STR(text, "27/08/");
    type_all(DATE_MASK, text, sizeof text, "2026");
    CHECK_STR(text, "27/08/2026");
}

/* A full field ignores the press. Truncating what is already there to make
 * room would throw away a digit somebody typed on purpose to accept one they
 * typed by accident. */
static void test_a_full_field_ignores_the_press(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    type_all(TIME_MASK, text, sizeof text, "1154");
    CHECK(!nd_mask_type(TIME_MASK, text, sizeof text, '9'));
    CHECK_STR(text, "11:54");
}

/* Only digits. A letter off the dev keyboard reaches this the same way a
 * keypad digit does, and must not land in a time. */
static void test_only_digits_are_taken(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    CHECK(!nd_mask_type(TIME_MASK, text, sizeof text, 'a'));
    CHECK(!nd_mask_type(TIME_MASK, text, sizeof text, ':'));
    CHECK_STR(text, "");
}

/* ONE press removes ONE digit, whichever side of a separator the field
 * stopped on. "11:" is the case that matters: the colon was never typed, so
 * deleting it on its own would be a press that appears to do nothing. */
static void test_backspace_takes_the_separator_with_the_digit(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    type_all(TIME_MASK, text, sizeof text, "11");
    CHECK_STR(text, "11:");

    CHECK(nd_mask_backspace(TIME_MASK, text));
    CHECK_STR(text, "1");
    CHECK(nd_mask_backspace(TIME_MASK, text));
    CHECK_STR(text, "");
}

/* From a full field, backspace walks back one digit at a time. */
static void test_backspace_walks_back_one_digit_at_a_time(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    type_all(TIME_MASK, text, sizeof text, "1154");
    CHECK(nd_mask_backspace(TIME_MASK, text));
    CHECK_STR(text, "11:5");
    CHECK(nd_mask_backspace(TIME_MASK, text));
    CHECK_STR(text, "11:");
    CHECK(nd_mask_backspace(TIME_MASK, text));
    CHECK_STR(text, "1");
}

/* An empty field says so, which is what the widget turns into "leave". */
static void test_backspace_on_an_empty_field_reports_it(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    CHECK(!nd_mask_backspace(TIME_MASK, text));
}

static void test_complete_only_when_every_slot_is_filled(void)
{
    char text[ND_TIMESET_TEXT_MAX] = "";

    type_all(TIME_MASK, text, sizeof text, "115");
    CHECK(!nd_mask_complete(TIME_MASK, text));
    (void)nd_mask_type(TIME_MASK, text, sizeof text, '4');
    CHECK(nd_mask_complete(TIME_MASK, text));
}

/* ------------------------------------------------------------------ *
 * Reading what was typed
 * ------------------------------------------------------------------ */

static void test_time_parses_in_24_hour_form(void)
{
    int32_t h = -1;
    int32_t m = -1;

    CHECK(nd_timeset_parse_time("23:59", &h, &m));
    CHECK_INT(h, 23);
    CHECK_INT(m, 59);

    CHECK(nd_timeset_parse_time("00:00", &h, &m));
    CHECK_INT(h, 0);
    CHECK_INT(m, 0);
}

/* 24:00 is refused rather than read as midnight: this field sits beside a
 * DATE field, and "tomorrow" would contradict it. */
static void test_time_refuses_an_hour_past_23(void)
{
    int32_t h;
    int32_t m;

    CHECK(!nd_timeset_parse_time("24:00", &h, &m));
    CHECK(!nd_timeset_parse_time("99:00", &h, &m));
    CHECK(!nd_timeset_parse_time("12:60", &h, &m));
}

/* Half a time is not a time. The field can be left mid-entry and the OK key
 * must not take it.
 *
 * The last case is the one that needs the length check rather than the digit
 * scan: "11:544" has a digit in every position the scan looks at, so without
 * a check that the field is EXACTLY the mask's length it would parse as 11:54
 * and silently drop the stray 4. Nothing typed can reach that state -- the
 * mask refuses a sixth press -- but a caller prefilling the field from the
 * clock can, and that is a caller inside this app. */
static void test_time_refuses_an_incomplete_field(void)
{
    int32_t h;
    int32_t m;

    CHECK(!nd_timeset_parse_time("11:", &h, &m));
    CHECK(!nd_timeset_parse_time("1", &h, &m));
    CHECK(!nd_timeset_parse_time("", &h, &m));
    CHECK(!nd_timeset_parse_time("11:544", &h, &m));
}

static void test_date_parses_day_first(void)
{
    int32_t d = -1;
    int32_t mo = -1;
    int32_t y = -1;

    CHECK(nd_timeset_parse_date("27/08/2026", &d, &mo, &y));
    CHECK_INT(d, 27);
    CHECK_INT(mo, 8);
    CHECK_INT(y, 2026);
}

/* The case a bare 1..31 check lets through, which is why the month's own
 * length is consulted. */
static void test_date_refuses_a_day_that_month_does_not_have(void)
{
    int32_t d;
    int32_t mo;
    int32_t y;

    CHECK(!nd_timeset_parse_date("31/04/2026", &d, &mo, &y)); /* April has 30 */
    CHECK(!nd_timeset_parse_date("31/02/2026", &d, &mo, &y));
    CHECK(!nd_timeset_parse_date("00/01/2026", &d, &mo, &y));
    CHECK(!nd_timeset_parse_date("01/13/2026", &d, &mo, &y));
}

/* February, which is where date validators go wrong. 2024 is a leap year,
 * 2023 is not, 2000 is (divisible by 400) and 2100 is not (a century that is
 * not) -- though 2100 is outside the window anyway. */
static void test_february_knows_about_leap_years(void)
{
    int32_t d;
    int32_t mo;
    int32_t y;

    CHECK_INT(nd_timeset_days_in_month(2, 2024), 29);
    CHECK_INT(nd_timeset_days_in_month(2, 2023), 28);
    CHECK_INT(nd_timeset_days_in_month(2, 2000), 29);
    CHECK_INT(nd_timeset_days_in_month(2, 1900), 28);
    CHECK_INT(nd_timeset_days_in_month(13, 2024), 0);

    CHECK(nd_timeset_parse_date("29/02/2024", &d, &mo, &y));
    CHECK(!nd_timeset_parse_date("29/02/2023", &d, &mo, &y));
}

/* The clock service's own sanity window, applied while the keypad is still in
 * the user's hand. A date it would override later is refused now. */
static void test_date_refuses_a_year_the_clock_service_would_not_keep(void)
{
    int32_t d;
    int32_t mo;
    int32_t y;

    CHECK(!nd_timeset_parse_date("01/01/1999", &d, &mo, &y));
    CHECK(!nd_timeset_parse_date("01/01/2199", &d, &mo, &y));
    CHECK(nd_timeset_parse_date("01/01/2020", &d, &mo, &y));
    /* Over-long, as above: every position the scan reads is a digit. */
    CHECK(!nd_timeset_parse_date("27/08/20260", &d, &mo, &y));
}

/* ------------------------------------------------------------------ *
 * Composing
 * ------------------------------------------------------------------ */

/* Round trip: compose an instant, then read it back through the two field
 * formatters. Doing it this way rather than asserting a raw epoch keeps the
 * test true in whatever zone it runs in -- the phone ships on UTC and CI is
 * not required to. */
static void test_compose_round_trips_through_the_field_text(void)
{
    time_t when = 0;
    char text[ND_TIMESET_TEXT_MAX];

    CHECK(nd_timeset_compose(2026, 8, 27, 23, 59, &when));

    nd_timeset_time_text(text, sizeof text, when);
    CHECK_STR(text, "23:59");
    nd_timeset_date_text(text, sizeof text, when);
    CHECK_STR(text, "27/08/2026");
}

/* Outside the clock service's window it is refused here too, so the app never
 * hands settimeofday something the next boot would undo. */
static void test_compose_refuses_an_instant_outside_the_window(void)
{
    time_t when;

    CHECK(!nd_timeset_compose(1999, 1, 1, 0, 0, &when));
    CHECK(!nd_timeset_compose(2200, 1, 1, 0, 0, &when));
}

/* ------------------------------------------------------------------ *
 * Showing it
 * ------------------------------------------------------------------ */

/* 12-hour with a suffix, and the two hours the modulo alone gets wrong:
 * midnight and noon are both "12", not "0". */
static void test_the_menu_row_reads_the_way_a_person_says_it(void)
{
    time_t when = 0;
    char text[32];

    CHECK(nd_timeset_compose(2026, 8, 27, 11, 54, &when));
    nd_timeset_format_clock(text, sizeof text, when);
    CHECK_STR(text, "11:54 am");

    CHECK(nd_timeset_compose(2026, 8, 27, 23, 5, &when));
    nd_timeset_format_clock(text, sizeof text, when);
    CHECK_STR(text, "11:05 pm");

    CHECK(nd_timeset_compose(2026, 8, 27, 0, 0, &when));
    nd_timeset_format_clock(text, sizeof text, when);
    CHECK_STR(text, "12:00 am");

    CHECK(nd_timeset_compose(2026, 8, 27, 12, 0, &when));
    nd_timeset_format_clock(text, sizeof text, when);
    CHECK_STR(text, "12:00 pm");
}

int main(void)
{
    RUN(test_the_separator_appears_with_the_digit_before_it);
    RUN(test_the_date_mask_fills_the_same_way);
    RUN(test_a_full_field_ignores_the_press);
    RUN(test_only_digits_are_taken);
    RUN(test_backspace_takes_the_separator_with_the_digit);
    RUN(test_backspace_walks_back_one_digit_at_a_time);
    RUN(test_backspace_on_an_empty_field_reports_it);
    RUN(test_complete_only_when_every_slot_is_filled);
    RUN(test_time_parses_in_24_hour_form);
    RUN(test_time_refuses_an_hour_past_23);
    RUN(test_time_refuses_an_incomplete_field);
    RUN(test_date_parses_day_first);
    RUN(test_date_refuses_a_day_that_month_does_not_have);
    RUN(test_february_knows_about_leap_years);
    RUN(test_date_refuses_a_year_the_clock_service_would_not_keep);
    RUN(test_compose_round_trips_through_the_field_text);
    RUN(test_compose_refuses_an_instant_outside_the_window);
    RUN(test_the_menu_row_reads_the_way_a_person_says_it);
    return pt_report("test_timeset");
}
