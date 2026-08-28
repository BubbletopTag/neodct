/* test_backlight.c -- the three tiers, against a sysfs tree built per case.
 *
 * The port of backlight.py had a test suite (test_backlight.py, ~20 cases) and
 * the C side had neither the module nor the tests: nd_fb.h declared six
 * functions that nothing defined. These are those cases, rewritten against the
 * C module, plus the ones the C version needs and the Python's did not -- a
 * driver reporting max_brightness 0, and readdir order.
 *
 * Everything is under the case root, which matters more here than usual: these
 * paths are /sys/class/backlight and /sys/class/gpio, and a test that reached
 * the real ones would turn the developer's screen off.
 */

#include <string.h>

#include "nd_fb.h"
#include "nd_paths.h"

#include "platform_test.h"

/* The pin is a compile-time constant, so the paths the module will touch are
 * known here and spelled out rather than built -- if ND_BL_GPIO_PIN ever
 * moves, these strings should fail to match and say so. */
#define GPIO_ROOT      "/sys/class/gpio"
#define GPIO_EXPORT    GPIO_ROOT "/export"
#define GPIO_DIR       GPIO_ROOT "/gpio53"
#define GPIO_VALUE     GPIO_DIR "/value"
#define GPIO_DIRECTION GPIO_DIR "/direction"

#define BL_ROOT  "/sys/class/backlight"
#define BL_PANEL BL_ROOT "/backlight"

/* An exported, output-configured pin sitting at whatever `value` says. This is
 * what the module finds on a phone where neodct_displayd or a previous run
 * already claimed the pin. */
static void given_a_gpio_pin(const char *value)
{
    pt_mkdir(GPIO_ROOT);
    pt_write_text(GPIO_DIRECTION, "out\n");
    pt_write_text(GPIO_VALUE, value);
}

/* A pwm-backlight device with a brightness pair. */
static void given_a_pwm_panel(const char *dir, const char *brightness, const char *max)
{
    char path[256];

    (void)nd_snprintf(path, sizeof path, "%s/brightness", dir);
    pt_write_text(path, brightness);
    (void)nd_snprintf(path, sizeof path, "%s/max_brightness", dir);
    pt_write_text(path, max);
}

/* ------------------------------------------------------------------ *
 * Which tier
 * ------------------------------------------------------------------ */

/* Nothing at all is a real answer and the commonest one: QEMU has no
 * /sys/class/backlight and no /sys/class/gpio, and an app that treated that as
 * a failure would refuse to start on the emulator. */
static void test_mode_is_none_with_no_hardware(void)
{
    CHECK_INT(nd_backlight_mode(), ND_BL_NONE);
    CHECK(!nd_backlight_available());
}

static void test_mode_prefers_pwm(void)
{
    given_a_gpio_pin("1\n");
    given_a_pwm_panel(BL_PANEL, "128\n", "255\n");

    /* Both tiers present. PWM wins because it is the only one that dims, and
     * on this board they drive the same wire through different drivers. */
    CHECK_INT(nd_backlight_mode(), ND_BL_PWM);
}

static void test_mode_falls_back_to_gpio(void)
{
    given_a_gpio_pin("1\n");

    CHECK_INT(nd_backlight_mode(), ND_BL_GPIO);
    CHECK(nd_backlight_available());
}

/* A backlight device with no brightness file is not one this can drive. The
 * directory exists on plenty of systems -- an ACPI stub, a driver that failed
 * to probe -- and picking it would report PWM and then dim nothing. */
static void test_mode_ignores_a_backlight_device_with_no_brightness(void)
{
    pt_mkdir(BL_ROOT "/acpi_video0");

    CHECK_INT(nd_backlight_mode(), ND_BL_NONE);
}

/* Several devices, and the answer must not depend on the order readdir handed
 * them over -- that is the directory's hash or creation order and it differs
 * between boots. "backlight" sorts before all three decoys and is created
 * LAST, so an implementation that took whatever readdir offered first would
 * read one of the decoys and report 0 rather than 100. */
static void test_mode_picks_the_first_device_in_sorted_order(void)
{
    given_a_pwm_panel(BL_ROOT "/zz_video0", "0\n", "100\n");
    given_a_pwm_panel(BL_ROOT "/mm_video0", "0\n", "100\n");
    given_a_pwm_panel(BL_ROOT "/backlight1", "0\n", "100\n");
    given_a_pwm_panel(BL_ROOT "/backlight", "255\n", "255\n");

    CHECK_INT(nd_backlight_get_percent(), 100);
}

/* /sys/class/gpio exists but the pin is not exported. The module claims it and
 * then looks for the value file; with no kernel behind the write there is
 * none, so the honest answer is NONE -- and the claim still has to have been
 * attempted, which is what the export file records. */
static void test_mode_tries_to_export_an_unclaimed_pin(void)
{
    char text[32];

    pt_mkdir(GPIO_ROOT);

    CHECK_INT(nd_backlight_mode(), ND_BL_NONE);
    CHECK(pt_read_text(GPIO_EXPORT, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "53");
}

/* ------------------------------------------------------------------ *
 * Setting, PWM
 * ------------------------------------------------------------------ */

static void test_set_percent_scales_against_max_brightness(void)
{
    char text[32];

    given_a_pwm_panel(BL_PANEL, "0\n", "255\n");

    CHECK(nd_backlight_set_percent(100));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "255");

    CHECK(nd_backlight_set_percent(50));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "128");
}

/* Half-to-even, not half-away-from-zero. The two disagree at any exact .5 that
 * would round up onto an ODD integer, and 75% of a max_brightness of 254 is
 * exactly that: 190.5, which Python's round() -- what the brightness slider
 * was written against -- makes 190, and C's round() makes 191. */
static void test_set_percent_rounds_half_to_even(void)
{
    char text[32];

    given_a_pwm_panel(BL_PANEL, "0\n", "254\n");

    CHECK(nd_backlight_set_percent(75));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "190");
}

/* A driver reporting max_brightness 0 would be a division by zero on the way
 * back out. The read clamps the divisor to 1 rather than faulting. */
static void test_get_percent_survives_a_zero_max_brightness(void)
{
    given_a_pwm_panel(BL_PANEL, "0\n", "0\n");

    CHECK_INT(nd_backlight_get_percent(), 0);
}

/* No max_brightness at all falls back to 255, which is what every panel this
 * phone has ever had reports. */
static void test_set_percent_assumes_255_with_no_max(void)
{
    char text[32];

    pt_write_text(BL_PANEL "/brightness", "0\n");

    CHECK(nd_backlight_set_percent(100));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "255");
}

/* ------------------------------------------------------------------ *
 * Setting, GPIO
 * ------------------------------------------------------------------ */

/* Active high on this board: 1 is lit. Getting the polarity backwards darkens
 * the screen exactly when somebody starts using the phone. */
static void test_gpio_off_writes_zero_and_on_writes_one(void)
{
    char text[32];

    given_a_gpio_pin("1\n");

    CHECK(nd_backlight_off());
    CHECK(pt_read_text(GPIO_VALUE, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "0");

    CHECK(nd_backlight_on(100));
    CHECK(pt_read_text(GPIO_VALUE, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "1");
}

/* The GPIO tier has two brightnesses and reports them as the two ends. A
 * caller asking "how bright is it" during a blank has to get 0 back, because
 * that is the whole question Sleepy's Display screen asks. */
static void test_gpio_get_percent_is_all_or_nothing(void)
{
    given_a_gpio_pin("0\n");
    CHECK_INT(nd_backlight_get_percent(), 0);

    given_a_gpio_pin("1\n");
    CHECK_INT(nd_backlight_get_percent(), 100);
}

/* Any nonzero request is on, so the 5% floor does not turn a dim request into
 * an off one here -- it cannot, because there is no dim. */
static void test_gpio_treats_any_nonzero_percent_as_on(void)
{
    char text[32];

    given_a_gpio_pin("0\n");

    CHECK(nd_backlight_set_percent(1));
    CHECK(pt_read_text(GPIO_VALUE, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "1");
}

/* A pin whose direction is already "out" is left alone. Rewriting it is
 * harmless on this driver, but it is a write to a pin that may be driving the
 * panel at that moment and there is no reason to take it. */
static void test_gpio_leaves_a_correct_direction_alone(void)
{
    char text[32];

    given_a_gpio_pin("1\n");
    pt_write_text(GPIO_DIRECTION, "out");

    CHECK_INT(nd_backlight_mode(), ND_BL_GPIO);
    CHECK(pt_read_text(GPIO_DIRECTION, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "out");
}

/* A pin left as an input by whoever exported it is turned round, or the value
 * write goes nowhere. */
static void test_gpio_corrects_an_input_direction(void)
{
    char text[32];

    given_a_gpio_pin("1\n");
    pt_write_text(GPIO_DIRECTION, "in\n");

    CHECK_INT(nd_backlight_mode(), ND_BL_GPIO);
    CHECK(pt_read_text(GPIO_DIRECTION, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "out");
}

/* ------------------------------------------------------------------ *
 * The percentage clamp
 * ------------------------------------------------------------------ */

/* Out of range is clamped rather than refused: a caller computing a percentage
 * from a slider position can land on 101, and refusing would leave the screen
 * at whatever it was. */
static void test_set_percent_clamps_out_of_range(void)
{
    char text[32];

    given_a_pwm_panel(BL_PANEL, "0\n", "255\n");

    CHECK(nd_backlight_set_percent(400));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "255");

    CHECK(nd_backlight_set_percent(-20));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "0");
}

/* "On but very dim" must not read as a broken screen, so 1% becomes 5%. Zero
 * is untouched, because zero means off and off is a thing somebody asked for
 * on purpose. */
static void test_set_percent_raises_a_dim_request_to_the_floor(void)
{
    char text[32];

    given_a_pwm_panel(BL_PANEL, "0\n", "100\n");

    CHECK(nd_backlight_set_percent(1));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "5");

    CHECK(nd_backlight_set_percent(0));
    CHECK(pt_read_text(BL_PANEL "/brightness", text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "0");
}

/* ------------------------------------------------------------------ *
 * Nothing there
 * ------------------------------------------------------------------ */

/* With no tier, every write says so and the read says "I do not know" rather
 * than "off" -- which are different things, and a caller that confused them
 * would report a blank screen on a phone whose screen was lit. */
static void test_with_no_hardware_writes_fail_and_reads_are_unknown(void)
{
    CHECK(!nd_backlight_set_percent(50));
    CHECK(!nd_backlight_off());
    CHECK(!nd_backlight_on(100));
    CHECK_INT(nd_backlight_get_percent(), -1);
}

int main(void)
{
    RUN(test_mode_is_none_with_no_hardware);
    RUN(test_mode_prefers_pwm);
    RUN(test_mode_falls_back_to_gpio);
    RUN(test_mode_ignores_a_backlight_device_with_no_brightness);
    RUN(test_mode_picks_the_first_device_in_sorted_order);
    RUN(test_mode_tries_to_export_an_unclaimed_pin);
    RUN(test_set_percent_scales_against_max_brightness);
    RUN(test_set_percent_rounds_half_to_even);
    RUN(test_get_percent_survives_a_zero_max_brightness);
    RUN(test_set_percent_assumes_255_with_no_max);
    RUN(test_gpio_off_writes_zero_and_on_writes_one);
    RUN(test_gpio_get_percent_is_all_or_nothing);
    RUN(test_gpio_treats_any_nonzero_percent_as_on);
    RUN(test_gpio_leaves_a_correct_direction_alone);
    RUN(test_gpio_corrects_an_input_direction);
    RUN(test_set_percent_clamps_out_of_range);
    RUN(test_set_percent_raises_a_dim_request_to_the_floor);
    RUN(test_with_no_hardware_writes_fail_and_reads_are_unknown);
    return pt_report("test_backlight");
}
