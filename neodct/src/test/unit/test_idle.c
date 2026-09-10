/* test_idle.c -- the home screen's idle dimmer.
 *
 * Two halves, and they are tested differently on purpose.
 *
 * nd_idle_should_dim() is four numbers in and a bool out, so the boundary
 * cases -- 59 seconds is not idle, 60 is, an already-dimmed phone never dims
 * twice -- are checked directly and without a clock that has to really
 * advance.
 *
 * The rest writes to sysfs, so it is run against a panel built under the case
 * root exactly as test_backlight.c does. That matters more here than usual:
 * these paths are /sys/class/backlight and the cpufreq directory, and a test
 * that reached the real ones would dim the developer's screen and pin their
 * workstation to 600 MHz.
 *
 * Time is passed in rather than read, so every case below is instant.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_cpufreq.h"
#include "nd_fb.h"
#include "nd_idle.h"
#include "nd_paths.h"

#include "platform_test.h"

#define BL_ROOT  "/sys/class/backlight"
#define BL_PANEL BL_ROOT "/backlight"

/* The ten-step panel this phone actually has: max_brightness 10, so a
 * percentage maps onto a level exactly and ND_IDLE_DIM_PERCENT (10%) must
 * land on brightness=1 rather than near it. */
static void given_the_ten_step_panel(const char *brightness)
{
    pt_write_text(BL_PANEL "/brightness", brightness);
    pt_write_text(BL_PANEL "/max_brightness", "10");
    pt_write_text(BL_PANEL "/bl_power", "0");
}

static void given_a_cpufreq(const char *min, const char *max)
{
    pt_write_text(ND_CPUFREQ_MIN, min);
    pt_write_text(ND_CPUFREQ_MAX, max);
    pt_write_text(ND_CPUFREQ_CUR, max);
    pt_write_text(ND_CPUFREQ_HW_MIN, "408000");
    pt_write_text(ND_CPUFREQ_HW_MAX, "1104000");
    pt_write_text(ND_CPUFREQ_GOVERNOR, "ondemand\n");
}

static int read_int_at(const char *path)
{
    char buf[32];

    if (pt_read_text(path, buf, sizeof buf) == (size_t)-1)
        return -1;
    return (int)strtol(buf, NULL, 10);
}

/* ------------------------------------------------------------------ *
 * The decision, on its own
 * ------------------------------------------------------------------ */

static void test_a_fresh_phone_is_not_idle(void)
{
    CHECK(!nd_idle_should_dim(false, 100.0, 100.0, 60.0));
}

static void test_one_second_short_of_the_timeout_does_not_dim(void)
{
    CHECK(!nd_idle_should_dim(false, 100.0, 159.0, 60.0));
}

/* The boundary is inclusive: at exactly the timeout the phone dims. Spelled
 * out because ">= timeout" and "> timeout" are a one-character difference
 * that no other test would catch. */
static void test_exactly_the_timeout_dims(void)
{
    CHECK(nd_idle_should_dim(false, 100.0, 160.0, 60.0));
}

static void test_well_past_the_timeout_dims(void)
{
    CHECK(nd_idle_should_dim(false, 100.0, 100000.0, 60.0));
}

/* The flag is what stops nd_idle_tick() rewriting sysfs ten times a second
 * for the whole time a phone sits idle. */
static void test_an_already_dimmed_phone_does_not_dim_again(void)
{
    CHECK(!nd_idle_should_dim(true, 100.0, 100000.0, 60.0));
}

/* CLOCK_MONOTONIC does not go backwards, and if it ever did, the answer is to
 * do nothing rather than to subtract into a large positive and dim a phone
 * somebody is holding. */
static void test_a_clock_that_went_backwards_does_not_dim(void)
{
    CHECK(!nd_idle_should_dim(false, 100000.0, 100.0, 60.0));
}

static void test_a_zero_timeout_never_dims(void)
{
    CHECK(!nd_idle_should_dim(false, 100.0, 100000.0, 0.0));
}

/* ------------------------------------------------------------------ *
 * The state machine
 * ------------------------------------------------------------------ */

static void test_init_starts_the_countdown_undimmed(void)
{
    nd_idle s;

    nd_idle_init(&s, 500.0);
    CHECK(!s.dimmed);
    CHECK(s.last_activity == 500.0);
    CHECK_INT(s.wake_percent, -1);
}

static void test_activity_restarts_the_countdown(void)
{
    nd_idle s;

    nd_idle_init(&s, 0.0);
    /* 59 s in, a key. The countdown must restart FROM 59 -- so the tick at 60,
     * which is a full timeout after init but one second after the key, must
     * not dim, and neither must 118. */
    nd_idle_note_activity(&s, 59.0);
    nd_idle_tick(&s, 60.0);
    CHECK(!s.dimmed);
    nd_idle_tick(&s, 118.0);
    CHECK(!s.dimmed);
    /* 119 is 60 s after the key, and that is where it dims. Asserted so this
     * case pins down WHERE the countdown restarted rather than merely that it
     * was delayed -- a note_activity() that reset to 0 would pass the two
     * checks above and fail this one. */
    nd_idle_tick(&s, 119.0);
    CHECK(s.dimmed);
}

/* ------------------------------------------------------------------ *
 * What actually gets written
 * ------------------------------------------------------------------ */

static void test_the_dim_drops_the_panel_to_its_lowest_lit_step(void)
{
    nd_idle s;

    given_the_ten_step_panel("10");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);

    CHECK(s.dimmed);
    /* 10% of a ten-step table is level 1 -- lit, and as low as lit goes. */
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 1);
}

static void test_waking_puts_the_brightness_back(void)
{
    nd_idle s;

    given_the_ten_step_panel("10");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 1);

    nd_idle_wake(&s, 61.0);
    CHECK(!s.dimmed);
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 10);
}

/* The level the owner was actually on, not a hard-coded full. A phone left at
 * level 4 must come back to level 4. */
static void test_waking_restores_a_partial_brightness(void)
{
    nd_idle s;

    given_the_ten_step_panel("4");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 1);

    nd_idle_wake(&s, 61.0);
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 4);
}

static void test_the_dim_pins_the_cpu(void)
{
    nd_idle s;

    given_the_ten_step_panel("10");
    given_a_cpufreq("408000", "1104000");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);

    CHECK(s.cpu_moved);
    CHECK_INT(read_int_at(ND_CPUFREQ_MIN), ND_IDLE_CPU_KHZ);
    CHECK_INT(read_int_at(ND_CPUFREQ_MAX), ND_IDLE_CPU_KHZ);
}

static void test_waking_opens_the_cpu_range_again(void)
{
    nd_idle s;

    given_the_ten_step_panel("10");
    given_a_cpufreq("408000", "1104000");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);
    nd_idle_wake(&s, 61.0);

    CHECK(!s.cpu_moved);
    CHECK_INT(read_int_at(ND_CPUFREQ_MIN), 408000);
    CHECK_INT(read_int_at(ND_CPUFREQ_MAX), 1104000);
}

/* A phone somebody pinned BELOW 600 by hand -- Sleepy's CPU screen does
 * exactly this -- must not be sped UP by going idle. */
static void test_a_phone_already_slower_than_the_target_is_left_alone(void)
{
    nd_idle s;

    given_the_ten_step_panel("10");
    given_a_cpufreq("408000", "408000");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);

    CHECK(!s.cpu_moved);
    CHECK_INT(read_int_at(ND_CPUFREQ_MAX), 408000);
}

/* ...and its pin survives the wake, because a wake that "restored" a range it
 * never captured would silently unpin a phone the owner pinned. */
static void test_waking_leaves_a_hand_pinned_cpu_pinned(void)
{
    nd_idle s;

    given_the_ten_step_panel("10");
    given_a_cpufreq("408000", "408000");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);
    nd_idle_wake(&s, 61.0);

    CHECK_INT(read_int_at(ND_CPUFREQ_MIN), 408000);
    CHECK_INT(read_int_at(ND_CPUFREQ_MAX), 408000);
}

/* ------------------------------------------------------------------ *
 * The cases that must not touch anything
 * ------------------------------------------------------------------ */

/* QEMU, every time: no /sys/class/backlight and no cpufreq directory. The
 * dimmer must take that silently and still record that it dimmed, so the
 * matching wake is still a matched pair rather than an unbalanced one. */
static void test_a_phone_with_no_hardware_dims_without_complaint(void)
{
    nd_idle s;

    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);
    CHECK(s.dimmed);
    CHECK(!s.cpu_moved);

    nd_idle_wake(&s, 61.0);
    CHECK(!s.dimmed);
}

/* The ordinary keypress path: wake is called on EVERY key, and the overwhelming
 * majority of them are on a phone that was never dimmed. It must not write to
 * the panel then -- a keypress that rewrote brightness ten times a second
 * would be a visible flicker and a pile of pointless syscalls. */
static void test_waking_a_phone_that_never_dimmed_writes_nothing(void)
{
    nd_idle s;

    given_the_ten_step_panel("7");
    nd_idle_init(&s, 0.0);
    nd_idle_wake(&s, 5.0);

    CHECK(!s.dimmed);
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 7);
    CHECK(s.last_activity == 5.0);
}

/* Two ticks past the timeout must dim once. Without the flag the second tick
 * would capture the DIM level as the wake level and the phone would never get
 * its brightness back. */
static void test_a_second_tick_does_not_capture_the_dim_as_the_wake_level(void)
{
    nd_idle s;

    given_the_ten_step_panel("9");
    nd_idle_init(&s, 0.0);
    nd_idle_tick(&s, 60.0);
    nd_idle_tick(&s, 60.1);
    nd_idle_tick(&s, 120.0);

    nd_idle_wake(&s, 121.0);
    CHECK_INT(read_int_at(BL_PANEL "/brightness"), 9);
}

int main(void)
{
    RUN(test_a_fresh_phone_is_not_idle);
    RUN(test_one_second_short_of_the_timeout_does_not_dim);
    RUN(test_exactly_the_timeout_dims);
    RUN(test_well_past_the_timeout_dims);
    RUN(test_an_already_dimmed_phone_does_not_dim_again);
    RUN(test_a_clock_that_went_backwards_does_not_dim);
    RUN(test_a_zero_timeout_never_dims);
    RUN(test_init_starts_the_countdown_undimmed);
    RUN(test_activity_restarts_the_countdown);
    RUN(test_the_dim_drops_the_panel_to_its_lowest_lit_step);
    RUN(test_waking_puts_the_brightness_back);
    RUN(test_waking_restores_a_partial_brightness);
    RUN(test_the_dim_pins_the_cpu);
    RUN(test_waking_opens_the_cpu_range_again);
    RUN(test_a_phone_already_slower_than_the_target_is_left_alone);
    RUN(test_waking_leaves_a_hand_pinned_cpu_pinned);
    RUN(test_a_phone_with_no_hardware_dims_without_complaint);
    RUN(test_waking_a_phone_that_never_dimmed_writes_nothing);
    RUN(test_a_second_tick_does_not_capture_the_dim_as_the_wake_level);
    return pt_report("test_idle");
}
