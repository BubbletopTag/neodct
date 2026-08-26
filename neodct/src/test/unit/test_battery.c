/* test_battery.c -- the fuel gauge's state machine, driven by voltage.
 *
 * BatteryService has no Python test of its own, so the oracle for the two
 * voltage vectors below was produced by running the real
 * System/core/BatteryService against the same sequence through its own
 * simulation file and recording (level, pending_warning, shutdown) after every
 * sample. The tables are that recording, transcribed. If a table entry looks
 * arbitrary -- level 2 appearing twice, "critical" landing on the fifth sample
 * rather than the first -- that is the five-sample moving average lagging, and
 * it is the behaviour the home screen depends on.
 *
 * Everything here runs in Simulation Mode, because there is no i2c bus on the
 * machine this runs on. That is not a weakness of the test: the sim path feeds
 * the identical state machine, and the state machine is the part with the
 * hysteresis, the latches and the three-sample shutdown confirmation in it.
 * The register-level code is exercised on the phone, by tests/hw.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_battery.h"
#include "nd_types.h"
#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Fixtures
 * ------------------------------------------------------------------ */

static void set_sim_vcell(const char *text)
{
    pt_write_text(ND_BATT_SIM_FILE, text);
}

static void clear_sim_vcell(void)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, ND_BATT_SIM_FILE) == ND_OK)
        (void)unlink(resolved);
}

/* The gauge is chatty at construction -- four lines every time -- and the
 * replay tests build twenty of them. Swallowing stdout keeps `make test`
 * readable; stderr is left alone so a real failure still shows. */
static int g_saved_stdout = -1;

static void mute_stdout(void)
{
    int devnull;

    (void)fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        (void)dup2(devnull, STDOUT_FILENO);
        (void)close(devnull);
    }
}

static void unmute_stdout(void)
{
    (void)fflush(stdout);
    if (g_saved_stdout >= 0) {
        (void)dup2(g_saved_stdout, STDOUT_FILENO);
        (void)close(g_saved_stdout);
        g_saved_stdout = -1;
    }
}

/* Runs `body` with stdout pointed at a file under the case root, then reads
 * the file back so a test can assert on what was logged. */
static void capture_begin(void)
{
    char resolved[ND_PATH_MAX];
    int fd;

    pt_write_text("/capture.log", "");
    (void)fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    if (nd_path_resolve(resolved, sizeof resolved, "/capture.log") != ND_OK)
        return;
    fd = open(resolved, O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        (void)dup2(fd, STDOUT_FILENO);
        (void)close(fd);
    }
}

static void capture_end(char *out, size_t out_sz)
{
    unmute_stdout();
    if (pt_read_text("/capture.log", out, out_sz) == (size_t)-1)
        out[0] = '\0';
}

/* ------------------------------------------------------------------ *
 * The voltage vectors
 * ------------------------------------------------------------------ */

typedef struct {
    const char *volts; /* written to the simulation file before the poll */
    int32_t level;
    const char *warn; /* NULL, ND_BATT_WARN_LOW or ND_BATT_WARN_CRITICAL */
    bool shutdown;
} batt_step;

/* A full discharge, a shutdown, a charger, and a second discharge. The
 * warning column is what take_pending_warning() hands back when it is called
 * after EVERY sample -- i.e. a phone sitting on the home screen. */
static const batt_step VEC_A[] = {
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.4", 3, NULL, false},
    {"3.4", 2, NULL, false},
    {"3.4", 2, NULL, false},
    {"3.4", 1, NULL, false},
    {"3.4", 1, ND_BATT_WARN_LOW, false},
    {"3.2", 1, NULL, false},
    {"3.2", 0, NULL, false},
    {"3.2", 0, NULL, false},
    {"3.2", 0, ND_BATT_WARN_CRITICAL, false},
    {"3.2", 0, NULL, false},
    {"3.2", 0, NULL, false},
    /* Three consecutive means at or below 3.20 V. The mean of five 3.20s is
     * exactly 3.2 in IEEE-754 -- 16.0 / 5 -- which is why this fires at all. */
    {"3.2", 0, NULL, true},
    {"3.2", 0, NULL, true},
    {"3.9", 0, NULL, false},
    {"3.9", 1, NULL, false},
    {"3.9", 2, NULL, false},
    {"3.9", 3, NULL, false},
    {"3.9", 3, NULL, false},
    {"3.4", 3, NULL, false},
    {"3.4", 2, NULL, false},
    {"3.4", 2, NULL, false},
    {"3.4", 1, NULL, false},
    /* Re-armed by the charge above, so the low warning latches a second time. */
    {"3.4", 1, ND_BATT_WARN_LOW, false},
};

/* The same idea with nobody consuming the latch, which is what happens while
 * the user is inside an app: show_pending_battery_warning() only runs on the
 * home screen. The warning column is therefore the LATCH's contents, and it
 * shows "low" being overwritten by "critical" and then dropped entirely when
 * the charger goes on. 3.24 V rather than 3.20 keeps the shutdown counter out
 * of it. */
static const batt_step VEC_B[] = {
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.85", 3, NULL, false},
    {"3.4", 3, NULL, false},
    {"3.4", 2, NULL, false},
    {"3.4", 2, NULL, false},
    {"3.4", 1, NULL, false},
    {"3.4", 1, ND_BATT_WARN_LOW, false},
    {"3.24", 1, ND_BATT_WARN_LOW, false},
    {"3.24", 0, ND_BATT_WARN_LOW, false},
    {"3.24", 0, ND_BATT_WARN_LOW, false},
    {"3.24", 0, ND_BATT_WARN_LOW, false},
    {"3.24", 0, ND_BATT_WARN_CRITICAL, false},
    /* mean climbs to 3.392 V, past 3.25 + 0.05, so the critical latch re-arms
     * and the warning nobody ever saw is dropped. */
    {"4.0", 1, NULL, false},
    {"4.0", 1, NULL, false},
    {"4.0", 2, NULL, false},
    {"4.0", 3, NULL, false},
    {"4.0", 4, NULL, false},
};

static void check_warn(const char *got, const char *want, size_t step)
{
    g_checks++;
    if ((got == NULL) != (want == NULL) || (got != NULL && strcmp(got, want) != 0)) {
        g_failures++;
        fprintf(stderr, "FAIL %s:%d  step %zu warning got %s want %s\n", __FILE__, __LINE__, step,
                got != NULL ? got : "(none)", want != NULL ? want : "(none)");
    }
}

/* Opens a gauge already seeded with steps[0], because the constructor polls
 * once so the first home-screen frame is real. */
static nd_battery *open_seeded(const batt_step *steps)
{
    nd_battery *b = NULL;

    set_sim_vcell(steps[0].volts);
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    return b;
}

/* --- the two vectors --- */

static void test_a_discharge_warns_then_shuts_down(void)
{
    nd_battery *b;
    size_t i;

    (void)unsetenv(ND_SIM_ENV_VAR);
    mute_stdout();
    b = open_seeded(VEC_A);

    for (i = 0u; i < ND_ARRAY_LEN(VEC_A); i++) {
        const char *shutdown;

        if (i > 0u) {
            set_sim_vcell(VEC_A[i].volts);
            shutdown = nd_battery_poll(b, true);
        } else {
            /* Sample 0 was taken by the constructor, which discards its
             * return -- a phone cannot shut down on its very first reading
             * anyway, the counter needs three. */
            shutdown = NULL;
        }

        CHECK_INT(nd_battery_level(b), VEC_A[i].level);
        if (i > 0u)
            CHECK((shutdown != NULL) == VEC_A[i].shutdown);
        check_warn(nd_battery_take_pending_warning(b), VEC_A[i].warn, i);
    }

    nd_battery_close(b);
    unmute_stdout();
}

static void test_a_latched_warning_survives_until_someone_reads_it(void)
{
    size_t k;

    (void)unsetenv(ND_SIM_ENV_VAR);
    mute_stdout();

    /* One fresh gauge per step, replayed from the start without ever popping
     * the latch, so what we read at the end is genuinely what accumulated
     * rather than what one earlier take() left behind. */
    for (k = 0u; k < ND_ARRAY_LEN(VEC_B); k++) {
        nd_battery *b = open_seeded(VEC_B);
        size_t i;

        for (i = 1u; i <= k; i++) {
            set_sim_vcell(VEC_B[i].volts);
            CHECK(nd_battery_poll(b, true) == NULL);
        }

        CHECK_INT(nd_battery_level(b), VEC_B[k].level);
        check_warn(nd_battery_take_pending_warning(b), VEC_B[k].warn, k);
        nd_battery_close(b);
    }

    unmute_stdout();
}

/* --- simulation plumbing --- */

static void test_with_no_gauge_the_service_simulates_385(void)
{
    nd_battery *b = NULL;
    double v = 0.0;

    (void)unsetenv(ND_SIM_ENV_VAR);
    clear_sim_vcell();
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    CHECK(!nd_battery_has_hardware(b));
    CHECK(nd_battery_vcell(b, &v));
    CHECK(v > 3.8499 && v < 3.8501);
    /* 3.85 clears 3.35, 3.55 and 3.75 but not 3.95. */
    CHECK_INT(nd_battery_level(b), 3);
    nd_battery_close(b);
}

static void test_the_environment_variable_overrides_the_default(void)
{
    nd_battery *b = NULL;

    clear_sim_vcell();
    CHECK(setenv(ND_SIM_ENV_VAR, "3.30", 1) == 0);
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    CHECK_INT(nd_battery_level(b), 0);
    nd_battery_close(b);
    (void)unsetenv(ND_SIM_ENV_VAR);
}

static void test_the_file_beats_the_environment_variable(void)
{
    nd_battery *b = NULL;
    double v = 0.0;

    CHECK(setenv(ND_SIM_ENV_VAR, "3.30", 1) == 0);
    set_sim_vcell("4.05\n");
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    CHECK(nd_battery_vcell(b, &v));
    CHECK(v > 4.0499 && v < 4.0501);
    CHECK_INT(nd_battery_level(b), 4);
    nd_battery_close(b);
    (void)unsetenv(ND_SIM_ENV_VAR);
}

static void test_a_corrupt_simulation_file_falls_through_to_the_env(void)
{
    nd_battery *b = NULL;
    double v = 0.0;

    CHECK(setenv(ND_SIM_ENV_VAR, "3.70", 1) == 0);
    set_sim_vcell("not a voltage\n");
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    CHECK(nd_battery_vcell(b, &v));
    CHECK(v > 3.6999 && v < 3.7001);
    nd_battery_close(b);
    (void)unsetenv(ND_SIM_ENV_VAR);
}

static void test_corrupt_everywhere_still_gives_a_voltage(void)
{
    nd_battery *b = NULL;
    double v = 0.0;

    CHECK(setenv(ND_SIM_ENV_VAR, "also not a voltage", 1) == 0);
    set_sim_vcell("!!\n");
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    /* Never no reading at all: the home screen has to draw something. */
    CHECK(nd_battery_vcell(b, &v));
    CHECK(v > 3.8499 && v < 3.8501);
    nd_battery_close(b);
    (void)unsetenv(ND_SIM_ENV_VAR);
}

/* --- the tick's rate limit --- */

static void test_polling_twice_inside_two_seconds_changes_nothing(void)
{
    nd_battery *b = NULL;
    double v = 0.0;

    (void)unsetenv(ND_SIM_ENV_VAR);
    set_sim_vcell("3.85");
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    /* read_keypress() calls this roughly ten times a second on every screen.
     * Without the interval check the gauge would be hammered, and the
     * five-sample window would cover half a second instead of ten. */
    set_sim_vcell("3.00");
    CHECK(nd_battery_poll(b, false) == NULL);
    CHECK(nd_battery_vcell(b, &v));
    CHECK(v > 3.8499 && v < 3.8501);
    CHECK_INT(nd_battery_level(b), 3);

    CHECK(nd_battery_poll(b, true) == NULL);
    CHECK(nd_battery_vcell(b, &v));
    /* (3.85 + 3.00) / 2 */
    CHECK(v > 3.4249 && v < 3.4251);

    nd_battery_close(b);
}

/* --- what simulation mode refuses to do --- */

static void test_simulation_mode_has_no_registers_to_show(void)
{
    nd_battery *b = NULL;
    nd_battery_snap snap;

    (void)unsetenv(ND_SIM_ENV_VAR);
    clear_sim_vcell();
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    /* This is exactly why the FuelGauge engineering app refuses to run on a
     * board with no battery daughterboard: there is nothing to report. */
    memset(&snap, 0, sizeof snap);
    CHECK(!nd_battery_debug_snapshot(b, &snap));
    CHECK(!nd_battery_quickstart(b));
    nd_battery_close(b);
}

/* --- where the bus and the address come from --- */

static void test_the_i2c_bus_and_address_come_from_settings(void)
{
    char log[4096];
    nd_battery *b = NULL;

    (void)unsetenv(ND_SIM_ENV_VAR);
    pt_write_text(ND_PATH_SETTINGS_PROP, "system.hw.battery_i2c_bus=7\n"
                                         "system.hw.battery_i2c_addr=0x40\n");
    /* A plain file where the character device would be: open() succeeds, so
     * the probe gets far enough to name the address it tried to select. */
    pt_write_text("/dev/i2c-7", "");

    capture_begin();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    capture_end(log, sizeof log);

    CHECK(strstr(log, "/dev/i2c-7") != NULL);
    CHECK(strstr(log, "I2C_SLAVE 0x40") != NULL);
    CHECK(strstr(log, "Running in Simulation Mode") != NULL);
    CHECK(!nd_battery_has_hardware(b));
    nd_battery_close(b);
}

static void test_an_explicit_bus_and_address_win_over_settings(void)
{
    char log[4096];
    nd_battery *b = NULL;

    (void)unsetenv(ND_SIM_ENV_VAR);
    pt_write_text(ND_PATH_SETTINGS_PROP, "system.hw.battery_i2c_bus=7\n");
    pt_write_text("/dev/i2c-2", "");

    capture_begin();
    CHECK(nd_battery_open(&b, 2, 0x36) == ND_OK);
    capture_end(log, sizeof log);

    CHECK(strstr(log, "/dev/i2c-2") != NULL);
    CHECK(strstr(log, "I2C_SLAVE 0x36") != NULL);
    nd_battery_close(b);
}

static void test_a_nonsense_bus_setting_falls_back_to_three(void)
{
    char log[4096];
    nd_battery *b = NULL;

    (void)unsetenv(ND_SIM_ENV_VAR);
    pt_write_text(ND_PATH_SETTINGS_PROP, "system.hw.battery_i2c_bus=eleven\n"
                                         "system.hw.battery_i2c_addr=0x40\n");

    capture_begin();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    capture_end(log, sizeof log);

    /* The Python's try block wraps BOTH reads, so a bad bus discards the
     * perfectly good address alongside it. Ported as-is. */
    CHECK(strstr(log, "Settings unavailable") != NULL);
    CHECK(strstr(log, "/dev/i2c-3") != NULL);
    nd_battery_close(b);
}

/* --- the seed poll --- */

static void test_the_constructor_polls_once_so_the_first_frame_is_real(void)
{
    nd_battery *b = NULL;

    (void)unsetenv(ND_SIM_ENV_VAR);
    set_sim_vcell("3.05");
    mute_stdout();
    CHECK(nd_battery_open(&b, -1, -1) == ND_OK);
    unmute_stdout();

    /* The field starts at 3 -- the pre-0.2.4a static gauge -- and would still
     * read 3 here if the constructor had not sampled. */
    CHECK_INT(nd_battery_level(b), 0);
    nd_battery_close(b);
}

int main(void)
{
    RUN(test_a_discharge_warns_then_shuts_down);
    RUN(test_a_latched_warning_survives_until_someone_reads_it);
    RUN(test_with_no_gauge_the_service_simulates_385);
    RUN(test_the_environment_variable_overrides_the_default);
    RUN(test_the_file_beats_the_environment_variable);
    RUN(test_a_corrupt_simulation_file_falls_through_to_the_env);
    RUN(test_corrupt_everywhere_still_gives_a_voltage);
    RUN(test_polling_twice_inside_two_seconds_changes_nothing);
    RUN(test_simulation_mode_has_no_registers_to_show);
    RUN(test_the_i2c_bus_and_address_come_from_settings);
    RUN(test_an_explicit_bus_and_address_win_over_settings);
    RUN(test_a_nonsense_bus_setting_falls_back_to_three);
    RUN(test_the_constructor_polls_once_so_the_first_frame_is_real);
    return pt_report("test_battery");
}
