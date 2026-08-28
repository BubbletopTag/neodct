/* nd_cpufreq.c -- see nd_cpufreq.h. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_cpufreq.h"
#include "nd_paths.h"
#include "nd_types.h"

/* ------------------------------------------------------------------ *
 * Sysfs, one small file at a time
 * ------------------------------------------------------------------ */

/* Sysfs attributes are a page at most and these are a line. Reading the whole
 * thing into a caller buffer avoids an allocation on a phone that has 64 MB
 * and a menu to draw. False means the file is not there, which for cpufreq is
 * an ordinary answer -- QEMU's kernel has no CONFIG_CPU_FREQ at all. */
static bool read_attr(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    size_t n;

    if (out_sz == 0u)
        return false;
    out[0] = '\0';
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return false;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    (void)fclose(f);
    while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r' || out[n - 1u] == ' ' ||
                      out[n - 1u] == '\t')) {
        n--;
        out[n] = '\0';
    }
    return true;
}

static int32_t read_int_attr(const char *path)
{
    char buf[32];
    long value;
    char *end;

    if (!read_attr(path, buf, sizeof buf) || buf[0] == '\0')
        return -1;
    errno = 0;
    value = strtol(buf, &end, 10);
    if (end == buf || errno != 0 || value < 0 || value > 0x7FFFFFFFL)
        return -1;
    return (int32_t)value;
}

/* The write is checked at fclose as well as at fputs: sysfs validates the
 * value in its store handler, and a frequency outside the OPP table comes
 * back as an error from the flush rather than from the write. Missing that is
 * how a refused write becomes a silent one. */
static bool write_attr(const char *path, int32_t khz)
{
    char resolved[ND_PATH_MAX];
    char value[16];
    FILE *f;
    bool ok;

    if (nd_snprintf(value, sizeof value, "%d", khz) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    f = fopen(resolved, "wb");
    if (f == NULL)
        return false;
    ok = fputs(value, f) >= 0;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* ------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------ */

size_t nd_cpufreq_parse_table(const char *text, int32_t *out, size_t max)
{
    const char *p;
    size_t n = 0u;

    if (text == NULL || out == NULL || max == 0u)
        return 0u;

    for (p = text; *p != '\0';) {
        char *end;
        long value;
        size_t i;
        size_t at;

        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }

        errno = 0;
        value = strtol(p, &end, 10);
        if (end == p) {
            /* Not a number at all. Skip the whole token rather than one
             * character, so a stray word cannot make every letter in it a
             * separate parse attempt. */
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                p++;
            continue;
        }
        p = end;
        if (errno != 0 || value <= 0 || value > 0x7FFFFFFFL)
            continue;

        /* Insertion sort, ascending, skipping a value already present. The
         * table is five entries on this chip and sixteen at most, so the
         * quadratic cost is nothing and the alternative -- collect then
         * qsort then unique -- is three passes over the same tiny array. */
        for (i = 0u; i < n; i++) {
            if (out[i] == (int32_t)value)
                break;
            if (out[i] > (int32_t)value)
                break;
        }
        if (i < n && out[i] == (int32_t)value)
            continue;
        if (n == max)
            continue;
        for (at = n; at > i; at--)
            out[at] = out[at - 1u];
        out[i] = (int32_t)value;
        n++;
    }
    return n;
}

void nd_cpufreq_format(char *out, size_t out_sz, int32_t khz)
{
    if (out == NULL || out_sz == 0u)
        return;

    if (khz <= 0) {
        (void)nd_strlcpy(out, "--", out_sz);
        return;
    }
    /* The boundary is a whole gigahertz, so 1000000 kHz reads "1.00 GHz" and
     * 999000 reads "999 MHz". Two decimals below that would be "999.00 MHz",
     * which is four characters of nothing on a 240-pixel row. */
    if (khz >= 1000000)
        (void)nd_snprintf(out, out_sz, "%.2f GHz", (double)khz / 1000000.0);
    else
        (void)nd_snprintf(out, out_sz, "%d MHz", khz / 1000);
}

bool nd_cpufreq_max_first(int32_t target_khz, int32_t current_max_khz)
{
    /* An unreadable current max (-1) counts as "below the target", so the max
     * is written first. That is the safe way round when the answer is not
     * known: raising the ceiling first can never drop the floor, whereas
     * writing the floor first against an unknown ceiling can be refused. */
    return target_khz > current_max_khz;
}

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

nd_err nd_cpufreq_read_table(nd_cpufreq_table *out)
{
    /* Sixteen ten-digit numbers with separators is 176 bytes; 512 is a
     * comfortable page fragment and still nothing on the stack. */
    char text[512];

    if (out == NULL)
        return ND_ERR_INVAL;
    out->n = 0u;

    if (!read_attr(ND_CPUFREQ_AVAILABLE, text, sizeof text))
        return ND_ERR_NOTFOUND;
    out->n = nd_cpufreq_parse_table(text, out->khz, ND_ARRAY_LEN(out->khz));
    /* The file existed and held nothing usable. That is a driver publishing an
     * empty table, not a kernel without cpufreq, and the two deserve different
     * words on screen -- so it is NOTFOUND either way but the caller can tell
     * them apart by whether the read got this far. */
    if (out->n == 0u)
        return ND_ERR_NOTFOUND;
    return ND_OK;
}

nd_err nd_cpufreq_read_state(nd_cpufreq_state *out)
{
    if (out == NULL)
        return ND_ERR_INVAL;

    memset(out, 0, sizeof *out);
    out->cur_khz = read_int_attr(ND_CPUFREQ_CUR);
    out->min_khz = read_int_attr(ND_CPUFREQ_MIN);
    out->max_khz = read_int_attr(ND_CPUFREQ_MAX);
    (void)read_attr(ND_CPUFREQ_GOVERNOR, out->governor, sizeof out->governor);

    /* Every one of the four unreadable means there is no cpufreq here. Any one
     * of them readable means there is, and the rest are gaps in what this
     * kernel publishes -- scaling_cur_freq in particular is absent on drivers
     * that cannot read the real clock back. */
    if (out->cur_khz < 0 && out->min_khz < 0 && out->max_khz < 0 && out->governor[0] == '\0')
        return ND_ERR_NOTFOUND;
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Writing
 * ------------------------------------------------------------------ */

nd_err nd_cpufreq_set(int32_t khz)
{
    int32_t current_max;
    bool a;
    bool b;

    if (khz <= 0)
        return ND_ERR_INVAL;

    current_max = read_int_attr(ND_CPUFREQ_MAX);

    /* Both writes always happen, even when the first one failed. Stopping
     * after a failed max on the way up would leave the range straddling two
     * frequencies with no message saying which one won; going on gets the pair
     * as close to consistent as this kernel will allow, and the return value
     * still says something went wrong. */
    if (nd_cpufreq_max_first(khz, current_max)) {
        a = write_attr(ND_CPUFREQ_MAX, khz);
        b = write_attr(ND_CPUFREQ_MIN, khz);
    } else {
        a = write_attr(ND_CPUFREQ_MIN, khz);
        b = write_attr(ND_CPUFREQ_MAX, khz);
    }
    return (a && b) ? ND_OK : ND_ERR_IO;
}
