/* nd_backlight.c -- the panel's brightness. See the block in nd_fb.h.
 *
 * Ported from System/hw/backlight.py, which the port left declared and
 * unimplemented: nd_fb.h has carried these six prototypes since the header
 * set was frozen and nothing in lib/ defined them. Nothing linked against
 * them either, so it was a hole rather than a break -- until Sleepy, whose
 * whole first screen is "turn the backlight off and see whether it comes
 * back".
 *
 * ============ THREE TIERS, BEST FIRST ============
 *
 * PWM through /sys/class/backlight is the only one that dims. It exists only
 * when pwm9 is in the device tree, and the device tree lives in the boot
 * partition -- so on a phone updated over the air rather than reflashed, this
 * tier is simply absent and the GPIO tier is the real one. Both are shipped
 * for that reason; neither is speculative.
 *
 * GPIO through /sys/class/gpio is on/off and nothing else. It is the same
 * interface neodct_displayd drives RST and DC through, on the same panel, so
 * a phone where this tier does not work is a phone with no picture either.
 *
 * ============ WHY round() IS SPELLED OUT ============
 *
 * The Python wrote round(top * p / 100.0), and Python's round() is half-to-
 * even. C's round() is half-away-from-zero, and they disagree at exactly the
 * midpoints a percentage slider lands on: round(255 * 50 / 100.0) is
 * round(127.5), which is 128 here and 128 in Python, but 127.5 -> 128 only
 * because 128 is the even one. At 25% of 254 the two answers differ. The
 * brightness a user set and the brightness read back have to agree, so this
 * uses nd_round_half_even() like every other ported round().
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_fb.h"
#include "nd_paths.h"
#include "nd_types.h"

/* The kernel takes a moment to populate /sys/class/gpio/gpioN after the write
 * to export returns, so the direction write immediately afterwards can lose a
 * race with udev. The Python slept 10 ms here and so does this. */
#define BL_EXPORT_SETTLE_NS 10000000L

/* ------------------------------------------------------------------ *
 * Small sysfs reads and writes
 * ------------------------------------------------------------------ */

/* Every write is best-effort: a read-only sysfs, a pin somebody else claimed
 * and a panel that is not there all arrive as an errno, and none of them is
 * worth failing a boot over. The Python returned False; so does this. */
static bool write_text(const char *path, const char *text)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    bool ok;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    f = fopen(resolved, "wb");
    if (f == NULL)
        return false;
    ok = fputs(text, f) >= 0;
    /* fclose can fail where fputs did not: sysfs validates on write, and a
     * value the driver rejects surfaces at flush time. */
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* The whole (tiny) file, NUL-terminated and stripped of trailing whitespace.
 * False when it is not there, which for sysfs is the ordinary way of saying
 * "this kernel was not built with that". */
static bool read_text(const char *path, char *out, size_t out_sz)
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

/* A sysfs integer, or `fallback` when the file is missing or is not one.
 * max_brightness on a driver that reports nonsense is the case this covers,
 * and 255 is a better guess than dividing by zero. */
static int32_t read_int(const char *path, int32_t fallback)
{
    char buf[32];
    long value;
    char *end;

    if (!read_text(path, buf, sizeof buf) || buf[0] == '\0')
        return fallback;
    errno = 0;
    value = strtol(buf, &end, 10);
    if (end == buf || errno != 0)
        return fallback;
    if (value < 0 || value > 0x7FFFFFFFL)
        return fallback;
    return (int32_t)value;
}

static bool path_exists(const char *path)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    return access(resolved, F_OK) == 0;
}

static bool is_directory(const char *path)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    if (stat(resolved, &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

/* ------------------------------------------------------------------ *
 * The PWM tier
 * ------------------------------------------------------------------ */

/* The first /sys/class/backlight/<dev> that has a brightness file, in the
 * order sorted() would give -- NOT in readdir order, which is the hash order
 * of the directory and differs between boots. A phone with two backlight
 * devices would otherwise dim a different one each time it started. */
static bool pwm_device(char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    char best[64];
    DIR *dir;
    struct dirent *entry;
    bool found = false;

    if (nd_path_resolve(resolved, sizeof resolved, ND_BL_BACKLIGHT_ROOT) != ND_OK)
        return false;
    dir = opendir(resolved);
    if (dir == NULL)
        return false;

    best[0] = '\0';
    while ((entry = readdir(dir)) != NULL) {
        char candidate[ND_PATH_MAX];

        if (entry->d_name[0] == '.')
            continue;
        if (found && strcmp(entry->d_name, best) >= 0)
            continue;
        if (nd_snprintf(candidate, sizeof candidate, "%s/%s/brightness", ND_BL_BACKLIGHT_ROOT,
                        entry->d_name) != ND_OK)
            continue;
        if (!path_exists(candidate))
            continue;
        if (nd_strlcpy(best, entry->d_name, sizeof best) >= sizeof best)
            continue;
        found = true;
    }
    (void)closedir(dir);

    if (!found)
        return false;
    return nd_snprintf(out, out_sz, "%s/%s", ND_BL_BACKLIGHT_ROOT, best) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The GPIO tier
 * ------------------------------------------------------------------ */

/* Claim the pin and point it outwards. Returns whether the value file is
 * there afterwards, which is the only claim worth making: a pin that exported
 * but has no value node cannot be driven, and saying "GPIO" about it would
 * hide a dead backlight behind a working-looking mode. */
static bool export_gpio(void)
{
    char dir[ND_PATH_MAX];
    char path[ND_PATH_MAX];
    char direction[32];

    if (nd_snprintf(dir, sizeof dir, "%s/gpio%d", ND_BL_GPIO_ROOT, ND_BL_GPIO_PIN) != ND_OK)
        return false;

    if (!is_directory(dir)) {
        char pin[16];
        struct timespec settle;

        if (nd_snprintf(pin, sizeof pin, "%d", ND_BL_GPIO_PIN) != ND_OK)
            return false;
        if (nd_snprintf(path, sizeof path, "%s/export", ND_BL_GPIO_ROOT) != ND_OK)
            return false;
        /* An EBUSY here means somebody already exported it, which is a
         * success for our purposes -- neodct_displayd exports pins on the
         * same controller. So the return value is deliberately ignored and
         * the question is settled below by looking for the value file. */
        (void)write_text(path, pin);
        settle.tv_sec = 0;
        settle.tv_nsec = BL_EXPORT_SETTLE_NS;
        (void)nanosleep(&settle, NULL);
    }

    if (nd_snprintf(path, sizeof path, "%s/direction", dir) != ND_OK)
        return false;
    /* Only written when it is wrong. Rewriting "out" over "out" is harmless
     * on this driver, but it is a write to a pin that may be driving the
     * panel right now, and there is no reason to take it. */
    if (read_text(path, direction, sizeof direction) && strcmp(direction, "out") != 0)
        (void)write_text(path, "out");

    if (nd_snprintf(path, sizeof path, "%s/value", dir) != ND_OK)
        return false;
    return path_exists(path);
}

static bool gpio_value_path(char *out, size_t out_sz)
{
    return nd_snprintf(out, out_sz, "%s/gpio%d/value", ND_BL_GPIO_ROOT, ND_BL_GPIO_PIN) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The public six
 * ------------------------------------------------------------------ */

nd_bl_mode nd_backlight_mode(void)
{
    char device[ND_PATH_MAX];

    if (pwm_device(device, sizeof device))
        return ND_BL_PWM;
    if (is_directory(ND_BL_GPIO_ROOT) && export_gpio())
        return ND_BL_GPIO;
    return ND_BL_NONE;
}

bool nd_backlight_available(void)
{
    return nd_backlight_mode() != ND_BL_NONE;
}

bool nd_backlight_set_percent(int32_t percent)
{
    char device[ND_PATH_MAX];
    char path[ND_PATH_MAX];
    char value[16];

    percent = nd_clamp32(percent, 0, 100);
    if (percent > 0 && percent < ND_BL_MIN_ON_PERCENT)
        percent = ND_BL_MIN_ON_PERCENT;

    if (pwm_device(device, sizeof device)) {
        int32_t top;
        int32_t level;

        if (nd_snprintf(path, sizeof path, "%s/max_brightness", device) != ND_OK)
            return false;
        top = read_int(path, 255);
        level = nd_trunc32(nd_round_half_even((double)top * (double)percent / 100.0));
        if (nd_snprintf(path, sizeof path, "%s/brightness", device) != ND_OK)
            return false;
        if (nd_snprintf(value, sizeof value, "%d", level) != ND_OK)
            return false;
        return write_text(path, value);
    }

    if (is_directory(ND_BL_GPIO_ROOT) && export_gpio()) {
        bool lit = percent > 0;

        if (!gpio_value_path(path, sizeof path))
            return false;
        return write_text(path, lit == ND_BL_ACTIVE_LOW ? "0" : "1");
    }

    return false;
}

int32_t nd_backlight_get_percent(void)
{
    char device[ND_PATH_MAX];
    char path[ND_PATH_MAX];

    if (pwm_device(device, sizeof device)) {
        int32_t top;
        int32_t now;

        if (nd_snprintf(path, sizeof path, "%s/max_brightness", device) != ND_OK)
            return -1;
        top = read_int(path, 255);
        if (nd_snprintf(path, sizeof path, "%s/brightness", device) != ND_OK)
            return -1;
        now = read_int(path, -1);
        if (now < 0)
            return -1;
        /* max(1, top): a driver reporting max_brightness 0 is broken, and a
         * division by it would be a fault rather than a wrong number. */
        return nd_trunc32(nd_round_half_even((double)now * 100.0 / (double)nd_max32(1, top)));
    }

    if (is_directory(ND_BL_GPIO_ROOT) && export_gpio()) {
        int32_t raw;

        if (!gpio_value_path(path, sizeof path))
            return -1;
        raw = read_int(path, -1);
        if (raw < 0)
            return -1;
        return ((raw != 0) != ND_BL_ACTIVE_LOW) ? 100 : 0;
    }

    return -1;
}

bool nd_backlight_off(void)
{
    return nd_backlight_set_percent(0);
}

bool nd_backlight_on(int32_t percent)
{
    return nd_backlight_set_percent(percent);
}
