/* nd_alarm.c -- see nd_alarm.h.
 *
 * Four digits in settings.prop and one question asked every few seconds. The
 * only thing in here that is not obvious is the fired-day marker, and the
 * comments on it are longer than the code because it is the difference
 * between an alarm and a phone that screams once a second for a minute.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_alarm.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* ------------------------------------------------------------------ *
 * Pure
 * ------------------------------------------------------------------ */

static bool all_digits(const char *s, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

bool nd_alarm_parse(const char *text, int32_t *hour_out, int32_t *minute_out)
{
    char digits[5];
    int32_t hour;
    int32_t minute;
    size_t n = 0u;
    size_t i;

    if (text == NULL)
        return false;

    /* Both spellings, because two callers exist and they disagree: the Clock
     * app's masked field hands over "07:30" and the settings file holds
     * "0730". Stripping anything that is not a digit accepts both without
     * either caller having to know about the other. */
    for (i = 0u; text[i] != '\0'; i++) {
        if (text[i] == ':' || text[i] == ' ')
            continue;
        if (n >= 4u)
            return false; /* too long: not a time */
        digits[n] = text[i];
        n++;
    }
    if (n != 4u || !all_digits(digits, 4u))
        return false;
    digits[4] = '\0';

    hour = (digits[0] - '0') * 10 + (digits[1] - '0');
    minute = (digits[2] - '0') * 10 + (digits[3] - '0');

    /* 24:00 and 07:60 are refused rather than normalised. Storing an alarm
     * the clock can never read would be an alarm that silently never goes
     * off, which is worse than being told the entry was wrong. */
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return false;

    if (hour_out != NULL)
        *hour_out = hour;
    if (minute_out != NULL)
        *minute_out = minute;
    return true;
}

void nd_alarm_format(const nd_alarm *a, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    if (a == NULL || !a->set) {
        (void)nd_strlcpy(out, "Off", out_sz);
        return;
    }
    (void)nd_snprintf(out, out_sz, "%02d:%02d", (int)a->hour, (int)a->minute);
}

bool nd_alarm_due(const nd_alarm *a, int32_t now_hour, int32_t now_minute, int32_t today,
                  int32_t fired_day, int32_t *lateness_s_out)
{
    int32_t alarm_min;
    int32_t now_min;
    int32_t late;

    if (lateness_s_out != NULL)
        *lateness_s_out = 0;
    if (a == NULL || !a->set)
        return false;

    /* Already gone off today. The marker is the whole reason this is not
     * true for every tick of the minute it fires in. */
    if (fired_day == today)
        return false;

    alarm_min = a->hour * 60 + a->minute;
    now_min = now_hour * 60 + now_minute;

    /* Not yet. A minute-resolution comparison, so an alarm set for 07:30
     * fires on the first tick at or after 07:30:00 and not at 07:29:59. */
    if (now_min < alarm_min)
        return false;

    late = (now_min - alarm_min) * 60;
    if (lateness_s_out != NULL)
        *lateness_s_out = late;
    return true;
}

/* ------------------------------------------------------------------ *
 * Stored
 * ------------------------------------------------------------------ */

void nd_alarm_load(nd_alarm *out)
{
    char text[16];

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);

    (void)nd_settings_get_copy(ND_SET_ALARM_TIME, "", text, sizeof text);
    if (text[0] == '\0')
        return;
    /* A value that will not parse is the same as no alarm. A hand-edited
     * settings.prop must not be able to leave the core holding something it
     * cannot decide about. */
    if (!nd_alarm_parse(text, &out->hour, &out->minute))
        return;
    out->set = true;
}

/* YYYYMMDD for a local-time struct tm. */
static int32_t ymd_of(const struct tm *tm)
{
    return (int32_t)((tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday);
}

static int32_t stored_fired_day(void)
{
    char text[16];

    (void)nd_settings_get_copy(ND_SET_ALARM_FIRED, "", text, sizeof text);
    if (text[0] == '\0')
        return 0;
    return (int32_t)strtol(text, NULL, 10);
}

nd_err nd_alarm_save(int32_t hour, int32_t minute)
{
    char text[8];
    nd_err rc;

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return ND_ERR_INVAL;
    if (nd_snprintf(text, sizeof text, "%02d%02d", (int)hour, (int)minute) != ND_OK)
        return ND_ERR_INVAL;

    rc = nd_settings_set(ND_SET_ALARM_TIME, text);
    if (rc != ND_OK)
        return rc;

    /* THE MARKER IS CLEARED HERE, and this is the case it exists for.
     *
     * Set an alarm for 07:00 at 08:00 and you mean tomorrow. If the marker
     * still said "fired today" the alarm would be suppressed tomorrow as
     * well, because nothing else ever clears it -- and if it said nothing,
     * an alarm set for a time already past would be "due and very late" and
     * get consumed silently within five seconds. Clearing it makes the first
     * case work; ND_ALARM_LATE_S makes the second one silent instead of
     * loud. Both are needed and neither is enough alone. */
    return nd_settings_set(ND_SET_ALARM_FIRED, "");
}

nd_err nd_alarm_clear(void)
{
    nd_err rc = nd_settings_set(ND_SET_ALARM_TIME, "");

    if (rc != ND_OK)
        return rc;
    return nd_settings_set(ND_SET_ALARM_FIRED, "");
}

/* ------------------------------------------------------------------ *
 * The tick
 * ------------------------------------------------------------------ */

bool nd_alarm_take_due(nd_alarm *out)
{
    nd_alarm a;
    struct tm now;
    char day_text[16];
    int32_t today;
    int32_t late = 0;

    nd_alarm_load(&a);
    if (out != NULL)
        *out = a;
    if (!a.set)
        return false;

    /* LOCAL time, not UTC: an alarm is a wall-clock promise. */
    nd_time_localtime(nd_time_now(), &now);
    today = ymd_of(&now);

    if (!nd_alarm_due(&a, (int32_t)now.tm_hour, (int32_t)now.tm_min, today, stored_fired_day(),
                      &late))
        return false;

    /* Marked BEFORE returning either way, so a caller that rings cannot be
     * asked again a tick later and ring twice. */
    if (nd_snprintf(day_text, sizeof day_text, "%d", (int)today) == ND_OK)
        (void)nd_settings_set(ND_SET_ALARM_FIRED, day_text);

    /* Too late to be worth waking anybody: the phone was off, or in an app,
     * across the whole window. Consumed silently -- see ND_ALARM_LATE_S. */
    if (late > ND_ALARM_LATE_S)
        return false;

    return true;
}
