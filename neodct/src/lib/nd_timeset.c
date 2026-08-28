/* nd_timeset.c -- see nd_timeset.h. */

#include <stdio.h>
#include <string.h>

#include "nd_clock.h"
#include "nd_timeset.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* ------------------------------------------------------------------ *
 * The mask engine
 * ------------------------------------------------------------------ */

/* text and mask stay positionally aligned by construction: every literal is
 * emitted the moment the slot before it is filled, so text[i] is a digit
 * exactly where mask[i] is a slot. Everything below relies on that, and the
 * length guard is what keeps a caller who broke it from reading past the
 * mask. */
static bool aligned(const char *mask, const char *text, size_t *len_out, size_t *mask_len_out)
{
    size_t len;
    size_t mask_len;

    if (mask == NULL || text == NULL)
        return false;
    len = strlen(text);
    mask_len = strlen(mask);
    if (len > mask_len)
        return false;
    *len_out = len;
    *mask_len_out = mask_len;
    return true;
}

bool nd_mask_type(const char *mask, char *text, size_t cap, char digit)
{
    size_t len;
    size_t mask_len;

    if (digit < '0' || digit > '9')
        return false;
    if (!aligned(mask, text, &len, &mask_len))
        return false;
    if (len >= mask_len)
        return false; /* full: ignore, never truncate what is already there */
    if (mask[len] != ND_MASK_SLOT)
        return false; /* only reachable if a caller wrote text itself */
    if (len + 2u > cap)
        return false;

    text[len] = digit;
    len++;

    /* Eager literals. See the header: the separator appearing is the feedback
     * that the field before it was accepted. */
    while (len < mask_len && mask[len] != ND_MASK_SLOT && len + 2u <= cap) {
        text[len] = mask[len];
        len++;
    }
    text[len] = '\0';
    return true;
}

bool nd_mask_backspace(const char *mask, char *text)
{
    size_t len;
    size_t mask_len;

    if (!aligned(mask, text, &len, &mask_len))
        return false;

    /* Literals first, then the digit under them -- so one press removes one
     * digit wherever the field happens to have stopped. */
    while (len > 0u && mask[len - 1u] != ND_MASK_SLOT)
        len--;
    if (len == 0u)
        return false;
    len--;
    text[len] = '\0';
    return true;
}

bool nd_mask_complete(const char *mask, const char *text)
{
    size_t len;
    size_t mask_len;

    if (!aligned(mask, text, &len, &mask_len))
        return false;
    return len == mask_len && mask_len > 0u;
}

/* ------------------------------------------------------------------ *
 * Reading what was typed
 * ------------------------------------------------------------------ */

/* `n` digits starting at `off`. The caller has already checked completeness,
 * so the only thing that can go wrong is a caller-written field with a letter
 * in it. */
static bool digits_at(const char *text, size_t off, size_t n, int32_t *out)
{
    int32_t value = 0;
    size_t i;

    for (i = 0u; i < n; i++) {
        char c = text[off + i];

        if (c < '0' || c > '9')
            return false;
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

bool nd_timeset_is_leap(int32_t year)
{
    if ((year % 4) != 0)
        return false;
    if ((year % 100) != 0)
        return true;
    return (year % 400) == 0;
}

int32_t nd_timeset_days_in_month(int32_t month, int32_t year)
{
    static const int32_t LENGTHS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && nd_timeset_is_leap(year))
        return 29;
    return LENGTHS[month - 1];
}

bool nd_timeset_parse_time(const char *text, int32_t *hour, int32_t *minute)
{
    int32_t h;
    int32_t m;

    if (hour == NULL || minute == NULL)
        return false;
    if (!nd_mask_complete(ND_TIMESET_TIME_MASK, text))
        return false;
    if (!digits_at(text, 0u, 2u, &h) || !digits_at(text, 3u, 2u, &m))
        return false;
    /* 24:00 is refused. Some clocks accept it as midnight-tomorrow; this one
     * would then have to decide which day that was, and the answer would
     * disagree with the date field sitting next to it. */
    if (h > 23 || m > 59)
        return false;
    *hour = h;
    *minute = m;
    return true;
}

bool nd_timeset_parse_date(const char *text, int32_t *day, int32_t *month, int32_t *year)
{
    int32_t d;
    int32_t mo;
    int32_t y;

    if (day == NULL || month == NULL || year == NULL)
        return false;
    if (!nd_mask_complete(ND_TIMESET_DATE_MASK, text))
        return false;
    if (!digits_at(text, 0u, 2u, &d) || !digits_at(text, 3u, 2u, &mo) ||
        !digits_at(text, 6u, 4u, &y))
        return false;

    /* The same window ClockService calls sane, converted from its epochs to
     * years. Refusing here is the difference between "the phone would not
     * take that" and "the phone took it and quietly changed it back". */
    if (y < 2020 || y > 2099)
        return false;
    if (mo < 1 || mo > 12)
        return false;
    /* Length of THAT month in THAT year: 31/04 and 29/02/2023 both stop
     * here, which a bare 1..31 check would let through. */
    if (d < 1 || d > nd_timeset_days_in_month(mo, y))
        return false;

    *day = d;
    *month = mo;
    *year = y;
    return true;
}

bool nd_timeset_compose(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                        time_t *out)
{
    struct tm parts;
    time_t when;

    if (out == NULL)
        return false;

    memset(&parts, 0, sizeof parts);
    parts.tm_year = (int)(year - 1900);
    parts.tm_mon = (int)(month - 1);
    parts.tm_mday = (int)day;
    parts.tm_hour = (int)hour;
    parts.tm_min = (int)minute;
    parts.tm_sec = 0;
    /* -1, not 0. See the header: libc decides for an hour that is ambiguous
     * or does not exist, and guessing costs an hour twice a year. */
    parts.tm_isdst = -1;

    when = mktime(&parts);
    if (when == (time_t)-1)
        return false;
    if ((long long)when < (long long)ND_CLOCK_SANE_MIN ||
        (long long)when >= (long long)ND_CLOCK_SANE_MAX)
        return false;

    *out = when;
    return true;
}

/* ------------------------------------------------------------------ *
 * Showing it
 * ------------------------------------------------------------------ */

void nd_timeset_format_clock(char *out, size_t out_sz, time_t when)
{
    struct tm parts;
    int32_t hour12;

    if (out == NULL || out_sz == 0u)
        return;
    nd_time_localtime((double)when, &parts);

    /* 0 and 12 both show as 12: midnight is 12:00 am, noon is 12:00 pm. The
     * modulo alone would print "0:00 am" for one of them. */
    hour12 = parts.tm_hour % 12;
    if (hour12 == 0)
        hour12 = 12;
    (void)nd_snprintf(out, out_sz, "%d:%02d %s", hour12, parts.tm_min,
                      parts.tm_hour < 12 ? "am" : "pm");
}

void nd_timeset_time_text(char *out, size_t out_sz, time_t when)
{
    struct tm parts;

    if (out == NULL || out_sz == 0u)
        return;
    nd_time_localtime((double)when, &parts);
    (void)nd_snprintf(out, out_sz, "%02d:%02d", parts.tm_hour, parts.tm_min);
}

void nd_timeset_date_text(char *out, size_t out_sz, time_t when)
{
    struct tm parts;

    if (out == NULL || out_sz == 0u)
        return;
    nd_time_localtime((double)when, &parts);
    (void)nd_snprintf(out, out_sz, "%02d/%02d/%04d", parts.tm_mday, parts.tm_mon + 1,
                      parts.tm_year + 1900);
}
