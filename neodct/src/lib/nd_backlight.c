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
#include "nd_log.h"
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

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        /* So that a caller reporting strerror(errno) after a false return
         * names THIS failure rather than whatever errno happened to hold. */
        errno = ENAMETOOLONG;
        return false;
    }
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

/* ============ THE ONE THING THIS FILE NEVER SAID ============
 *
 * Every path here was best-effort and silent, and the two answers it can give
 * are not the same fact at all: "this kernel has no backlight" is a board
 * without pwm9 in its device tree, and "permission denied" is a phone whose
 * gpio53 was never handed to group video. Both came out as `false` with
 * nothing on the console, so a panel that would not blank looked identical to
 * a panel that could not be built to blank, and Sleepy -- whose entire first
 * screen is "turn the backlight off and see whether it comes back" -- had no
 * way to say which it had hit.
 *
 * ONE LINE PER DISTINCT FAILURE. It used to be one line per process, latched
 * on a single bool -- which meant a blank that failed differently from an
 * earlier brightness write printed nothing at all, and console silence was
 * read as "it worked". Suppressing only an exact repeat still keeps a slider
 * from filling the serial console, and lets a NEW failure through.
 */
static bool already_said(const char *line)
{
    static char last[192];

    if (strcmp(last, line) == 0)
        return true;
    (void)nd_strlcpy(last, line, sizeof last);
    return false;
}

/* The phrase nd_backlight_last_error() hands to a dialog. See nd_fb.h for why
 * a caller is not allowed to guess this for itself any more. */
static char g_last_error[64];

static void clear_error(void)
{
    g_last_error[0] = '\0';
}

static void note_error(const char *what)
{
    (void)nd_strlcpy(g_last_error, what, sizeof g_last_error);
}

const char *nd_backlight_last_error(void)
{
    return g_last_error;
}

static void report_no_backlight(void)
{
    char line[192];

    (void)nd_snprintf(line, sizeof line,
                      "No backlight control: no device under %s has a brightness file, and "
                      "gpio%d could not be exported through %s. The panel cannot be dimmed "
                      "or blanked by this process.",
                      ND_BL_BACKLIGHT_ROOT, ND_BL_GPIO_PIN, ND_BL_GPIO_ROOT);
    note_error("no backlight device");
    if (!already_said(line))
        nd_log_err(ND_LOG_UI, "%s", line);
}

static void report_write_failure(const char *path, int err)
{
    char line[192];

    /* EACCES here is the interesting one and it has a specific repair: see
     * S90display, which exports the pin as root and chgrps it to video,
     * because the sysfs GPIO class emits no uevent a udev rule could match. */
    (void)nd_snprintf(line, sizeof line, "Backlight write to %s failed: %s.", path,
                      strerror(err));
    note_error(strerror(err));
    if (!already_said(line))
        nd_log_err(ND_LOG_UI, "%s", line);
}

/* A write(2) that returned without an error is not a driver that accepted the
 * value. sysfs validates on write, drivers clamp, and a backlight that is
 * powered down stores a brightness it will not act on -- so the only honest
 * confirmation is to read the file back.
 *
 * `brightness` is the file to read back and `actual_brightness` is NOT: the
 * latter reports 0 for the whole time the panel is blanked, whatever level is
 * stored, so verifying against it would call every correct write a failure on
 * exactly the phone this code exists for.
 *
 * An unreadable file counts as accepted. Some sysfs attributes are write-only,
 * and "I cannot check" is not the same fact as "the driver refused". */
static bool write_int_verified(const char *path, int32_t value)
{
    char text[16];
    char line[192];

    if (nd_snprintf(text, sizeof text, "%d", value) != ND_OK) {
        note_error("path too long");
        return false;
    }
    if (!write_text(path, text)) {
        report_write_failure(path, errno);
        return false;
    }
    if (read_int(path, value) != value) {
        (void)nd_snprintf(line, sizeof line,
                          "Backlight write to %s was accepted and did not stick: asked for "
                          "%d, reads back %d.",
                          path, value, read_int(path, value));
        note_error("the panel ignored it");
        if (!already_said(line))
            nd_log_err(ND_LOG_UI, "%s", line);
        return false;
    }
    return true;
}

nd_bl_mode nd_backlight_mode(void)
{
    char device[ND_PATH_MAX];

    if (pwm_device(device, sizeof device))
        return ND_BL_PWM;
    if (is_directory(ND_BL_GPIO_ROOT) && export_gpio())
        return ND_BL_GPIO;
    report_no_backlight();
    return ND_BL_NONE;
}

bool nd_backlight_available(void)
{
    return nd_backlight_mode() != ND_BL_NONE;
}

/* Drive gpio53 as well as the PWM, best effort, ignoring every failure.
 *
 * BL is one pad -- header pin 11 -- and two subsystems want it: GPIO1_C5 and
 * PWM9_M1. Which one reaches the LED is decided by the IOMUX, and the IOMUX
 * follows the PWM: rockchip's driver selects its "active" pinctrl state when
 * the PWM is enabled, and leaves the pad to the GPIO controller when it is
 * not. So on a phone whose backlight booted powered down, the PWM is disabled,
 * the pad belongs to GPIO, and S90display's `high` write is the only thing
 * lighting the screen -- while every write this file makes to `brightness`
 * lands in a driver that is not connected to anything.
 *
 * Writing both costs one syscall and is harmless in the case that does not
 * apply: when the PWM owns the pad the GPIO output register is simply not
 * wired to it, and the write moves nothing. That asymmetry -- useless when
 * things are right, the only thing that works when they are wrong -- is why
 * it is unconditional rather than conditional on a state we cannot read.
 *
 * Failures are silent by design. This is the second of two attempts at the
 * same job; the first one already reported. */
static void gpio_mirror(bool lit)
{
    char path[ND_PATH_MAX];

    if (!is_directory(ND_BL_GPIO_ROOT) || !export_gpio())
        return;
    if (!gpio_value_path(path, sizeof path))
        return;
    (void)write_text(path, lit == ND_BL_ACTIVE_LOW ? "0" : "1");
}

bool nd_backlight_set_percent(int32_t percent)
{
    char device[ND_PATH_MAX];
    char path[ND_PATH_MAX];
    char power[ND_PATH_MAX];
    bool lit;

    clear_error();

    percent = nd_clamp32(percent, 0, 100);
    if (percent > 0 && percent < ND_BL_MIN_ON_PERCENT)
        percent = ND_BL_MIN_ON_PERCENT;
    lit = percent > 0;

    if (pwm_device(device, sizeof device)) {
        int32_t top;
        int32_t level;

        if (nd_snprintf(path, sizeof path, "%s/max_brightness", device) != ND_OK) {
            note_error("path too long");
            return false;
        }
        top = read_int(path, 255);
        level = nd_trunc32(nd_round_half_even((double)top * (double)percent / 100.0));
        /* On the ten-step panel, a 5% request rounds to zero. A positive
         * request must keep the light on even on a coarse brightness table. */
        if (lit)
            level = nd_max32(1, level);
        if (nd_snprintf(path, sizeof path, "%s/brightness", device) != ND_OK) {
            note_error("path too long");
            return false;
        }
        if (nd_snprintf(power, sizeof power, "%s/bl_power", device) != ND_OK) {
            note_error("path too long");
            return false;
        }

        /* ============ THE ORDER, AND WHY IT IS THE SAME BOTH WAYS ============
         *
         * Brightness first, then power, whichever direction we are going.
         *
         * Coming ON that is what stops the panel flashing at the old level:
         * a blanked backlight stores props.brightness without acting on it,
         * so the level is already in place when the unblank lands.
         *
         * Going OFF it is what makes the blank work on a panel that is
         * already lit: writing 0 to brightness is what disables the PWM.
         * bl_power then follows -- and THAT is the write this file never made
         * before. A zero duty cycle is not a powered-down backlight; on a
         * driver that leaves the pad driven at zero the LED stays lit at
         * whatever the pull-up gives it. The 0.5.9a fix that added bl_power
         * put it inside `if (percent > 0)`, so it lit the panel correctly and
         * did nothing whatsoever for blanking it. */
        if (!write_int_verified(path, level))
            return false;

        if (path_exists(power) && !write_int_verified(power, lit ? ND_BL_POWER_ON
                                                                : ND_BL_POWER_OFF)) {
            /* Asymmetric on purpose. Coming ON, bl_power is the write that
             * lights a panel that booted powered down, so failing it means
             * the screen is still dark and the caller has to hear so. Going
             * OFF, brightness=0 was already accepted and verified; bl_power
             * was belt and braces, and reporting a failure here would send
             * Sleepy into "the backlight stayed on" about a screen that is
             * off. */
            if (lit)
                return false;
            clear_error();
        }

        gpio_mirror(lit);
        return true;
    }

    if (is_directory(ND_BL_GPIO_ROOT) && export_gpio()) {
        if (!gpio_value_path(path, sizeof path)) {
            note_error("path too long");
            return false;
        }
        if (write_text(path, lit == ND_BL_ACTIVE_LOW ? "0" : "1"))
            return true;
        report_write_failure(path, errno);
        return false;
    }

    report_no_backlight();
    return false;
}

int32_t nd_backlight_get_percent(void)
{
    char device[ND_PATH_MAX];
    char path[ND_PATH_MAX];

    if (pwm_device(device, sizeof device)) {
        int32_t top;
        int32_t now;

        /* bl_power BEFORE brightness, because they can disagree and only one
         * of them is what the owner sees. On the fault docs/HARDWARE_NOTES.md
         * records for this board -- bl_power 4, brightness 10 of 10 -- the
         * brightness file alone says "full" about a panel the kernel is
         * holding dark. Sleepy then opened its picker on Level 10 against a
         * black screen and, worse, captured 100 as the level to wake back to.
         * A powered-down backlight is at zero whatever it is storing. */
        if (nd_snprintf(path, sizeof path, "%s/bl_power", device) != ND_OK)
            return -1;
        if (path_exists(path) && read_int(path, ND_BL_POWER_ON) != ND_BL_POWER_ON)
            return 0;
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
