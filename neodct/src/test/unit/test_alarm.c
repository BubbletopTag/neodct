/* test_alarm.c -- the one alarm: parsing it, storing it, and deciding it.
 *
 * The decision is the part with teeth. "Has this alarm come due" has four
 * answers -- not yet, right now, already gone off today, too late to bother --
 * and three of them are silent, so a mistake in any of them looks exactly like
 * an alarm that works until the morning it does not. nd_alarm_due() takes the
 * clock as arguments precisely so all four can be checked here rather than by
 * waiting for 07:30 to come round.
 *
 * Everything that touches settings runs against a scratch ND_ROOT
 * (pt_new_case()), so no test can write the developer's real settings.prop.
 */

#include <string.h>

#include "nd_alarm.h"
#include "nd_settings.h"
#include "nd_types.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------ */

static void check_parse(const char *text, int32_t want_h, int32_t want_m)
{
    int32_t h = -1;
    int32_t m = -1;

    CHECK(nd_alarm_parse(text, &h, &m));
    CHECK_INT(h, want_h);
    CHECK_INT(m, want_m);
}

/* Both spellings, because two callers disagree: the masked field hands over
 * "07:30" and settings.prop holds "0730". */
static void test_both_spellings_parse(void)
{
    check_parse("0730", 7, 30);
    check_parse("07:30", 7, 30);
    check_parse("0000", 0, 0);
    check_parse("2359", 23, 59);
}

/* Refused rather than normalised: an alarm stored as 24:00 would be one that
 * silently never comes due, which is worse than being told the entry is bad. */
static void test_impossible_times_are_refused(void)
{
    CHECK(!nd_alarm_parse("2400", NULL, NULL));
    CHECK(!nd_alarm_parse("0760", NULL, NULL));
    CHECK(!nd_alarm_parse("9999", NULL, NULL));
}

static void test_malformed_input_is_refused(void)
{
    CHECK(!nd_alarm_parse(NULL, NULL, NULL));
    CHECK(!nd_alarm_parse("", NULL, NULL));
    CHECK(!nd_alarm_parse("073", NULL, NULL));   /* short */
    CHECK(!nd_alarm_parse("07300", NULL, NULL)); /* long */
    CHECK(!nd_alarm_parse("07:3x", NULL, NULL)); /* not digits */
    CHECK(!nd_alarm_parse("--:--", NULL, NULL)); /* an untouched mask */
}

/* ------------------------------------------------------------------ *
 * Formatting
 * ------------------------------------------------------------------ */

static void test_formatting(void)
{
    nd_alarm a;
    char out[16];

    a.set = true;
    a.hour = 7;
    a.minute = 5;
    nd_alarm_format(&a, out, sizeof out);
    CHECK_STR(out, "07:05");

    /* One word for "no alarm", so the Clock row and every other reader cannot
     * word it differently. */
    a.set = false;
    nd_alarm_format(&a, out, sizeof out);
    CHECK_STR(out, "Off");

    nd_alarm_format(NULL, out, sizeof out);
    CHECK_STR(out, "Off");
}

/* ------------------------------------------------------------------ *
 * The decision
 * ------------------------------------------------------------------ */

static nd_alarm at(int32_t h, int32_t m)
{
    nd_alarm a;

    a.set = true;
    a.hour = h;
    a.minute = m;
    return a;
}

static void test_not_due_before_the_time(void)
{
    nd_alarm a = at(7, 30);

    CHECK(!nd_alarm_due(&a, 7, 29, 20260910, 0, NULL));
    CHECK(!nd_alarm_due(&a, 0, 0, 20260910, 0, NULL));
}

/* The boundary. An alarm set for 07:30 fires on the first tick at or after
 * 07:30 and not a minute before. */
static void test_due_exactly_on_the_minute(void)
{
    nd_alarm a = at(7, 30);
    int32_t late = -1;

    CHECK(nd_alarm_due(&a, 7, 30, 20260910, 0, &late));
    CHECK_INT(late, 0);
}

/* Already gone off today. Without this the phone would re-ring on every tick
 * for the whole minute. */
static void test_not_due_twice_in_one_day(void)
{
    nd_alarm a = at(7, 30);

    CHECK(!nd_alarm_due(&a, 7, 30, 20260910, 20260910, NULL));
    CHECK(!nd_alarm_due(&a, 9, 0, 20260910, 20260910, NULL));
}

/* ...but the next day it is due again, which is the other half of the same
 * marker and the half a stale-marker bug would break silently. */
static void test_due_again_the_next_day(void)
{
    nd_alarm a = at(7, 30);

    CHECK(nd_alarm_due(&a, 7, 30, 20260911, 20260910, NULL));
}

/* Lateness is reported so the caller can decide to stay quiet. The decision
 * itself is still "due" -- being late does not make it not due, it makes it
 * not worth waking somebody for, and those are different facts. */
static void test_lateness_is_reported(void)
{
    nd_alarm a = at(7, 30);
    int32_t late = -1;

    CHECK(nd_alarm_due(&a, 8, 30, 20260910, 0, &late));
    CHECK_INT(late, 3600);
    CHECK(late > ND_ALARM_LATE_S);
}

static void test_an_unset_alarm_is_never_due(void)
{
    nd_alarm a;

    memset(&a, 0, sizeof a);
    CHECK(!nd_alarm_due(&a, 12, 0, 20260910, 0, NULL));
    CHECK(!nd_alarm_due(NULL, 12, 0, 20260910, 0, NULL));
}

/* ------------------------------------------------------------------ *
 * The store
 * ------------------------------------------------------------------ */

static void test_nothing_is_stored_to_begin_with(void)
{
    nd_alarm a;

    nd_alarm_load(&a);
    CHECK(!a.set);
}

static void test_save_then_load_round_trips(void)
{
    nd_alarm a;

    CHECK(nd_alarm_save(6, 45) == ND_OK);
    nd_alarm_load(&a);
    CHECK(a.set);
    CHECK_INT(a.hour, 6);
    CHECK_INT(a.minute, 45);
}

static void test_clear_forgets_it(void)
{
    nd_alarm a;

    CHECK(nd_alarm_save(6, 45) == ND_OK);
    CHECK(nd_alarm_clear() == ND_OK);
    nd_alarm_load(&a);
    CHECK(!a.set);
}

/* A hand-edited settings.prop must not put the core into a state it cannot
 * decide about: garbage reads as "no alarm", not as an alarm at 00:00. */
static void test_a_corrupt_stored_value_reads_as_no_alarm(void)
{
    nd_alarm a;

    CHECK(nd_settings_set(ND_SET_ALARM_TIME, "banana") == ND_OK);
    nd_alarm_load(&a);
    CHECK(!a.set);

    CHECK(nd_settings_set(ND_SET_ALARM_TIME, "2599") == ND_OK);
    nd_alarm_load(&a);
    CHECK(!a.set);
}

static void test_save_refuses_an_impossible_time(void)
{
    CHECK(nd_alarm_save(24, 0) != ND_OK);
    CHECK(nd_alarm_save(7, 60) != ND_OK);
    CHECK(nd_alarm_save(-1, 0) != ND_OK);
}

/* SETTING AN ALARM CLEARS THE FIRED MARKER, and this is the case it exists
 * for: set 07:00 at 08:00 and you mean tomorrow. If the marker survived, and
 * the alarm had already gone off today, tomorrow's would be suppressed too --
 * because nothing else ever clears it. */
static void test_saving_clears_the_fired_marker(void)
{
    char text[16];

    CHECK(nd_settings_set(ND_SET_ALARM_FIRED, "20260910") == ND_OK);
    CHECK(nd_alarm_save(7, 0) == ND_OK);
    (void)nd_settings_get_copy(ND_SET_ALARM_FIRED, "unset", text, sizeof text);
    CHECK_STR(text, "");
}

/* ------------------------------------------------------------------ *
 * The tick
 * ------------------------------------------------------------------ */

/* take_due() must say yes at most once, whatever the caller does. Both calls
 * here are within the same second, so the only thing that can stop the second
 * one is the marker being written before the first returned. */
static void test_take_due_fires_at_most_once(void)
{
    nd_alarm out;
    int fired = 0;
    int i;

    /* Set for a time that has certainly passed today, so the first call is
     * due. Whether it is LATE depends on the hour the suite runs at -- which
     * is exactly why this asserts "no more than once" rather than "once". */
    CHECK(nd_alarm_save(0, 0) == ND_OK);
    for (i = 0; i < 5; i++) {
        if (nd_alarm_take_due(&out))
            fired++;
    }
    CHECK(fired <= 1);

    /* And either way the marker is now set, so nothing can fire again today. */
    CHECK(!nd_alarm_take_due(&out));
}

static void test_take_due_is_false_with_no_alarm(void)
{
    nd_alarm out;

    CHECK(!nd_alarm_take_due(&out));
    CHECK(!out.set);
}

int main(void)
{
    RUN(test_both_spellings_parse);
    RUN(test_impossible_times_are_refused);
    RUN(test_malformed_input_is_refused);
    RUN(test_formatting);
    RUN(test_not_due_before_the_time);
    RUN(test_due_exactly_on_the_minute);
    RUN(test_not_due_twice_in_one_day);
    RUN(test_due_again_the_next_day);
    RUN(test_lateness_is_reported);
    RUN(test_an_unset_alarm_is_never_due);
    RUN(test_nothing_is_stored_to_begin_with);
    RUN(test_save_then_load_round_trips);
    RUN(test_clear_forgets_it);
    RUN(test_a_corrupt_stored_value_reads_as_no_alarm);
    RUN(test_save_refuses_an_impossible_time);
    RUN(test_saving_clears_the_fired_marker);
    RUN(test_take_due_fires_at_most_once);
    RUN(test_take_due_is_false_with_no_alarm);
    return pt_report("test_alarm");
}
