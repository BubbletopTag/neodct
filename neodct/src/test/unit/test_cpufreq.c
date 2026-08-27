/* test_cpufreq.c -- reading the kernel's operating points, and pinning one.
 *
 * The whole sysfs tree is built under the case root, which is what lets these
 * run at all: the real /sys/devices/system/cpu is root-owned, read-mostly, and
 * writing to it from a test would change the speed of the machine running the
 * suite.
 *
 * ============ WHAT THIS CANNOT CHECK, AND WHY IT IS SPLIT ============
 *
 * The write ORDER -- max before min when raising, min before max when
 * lowering -- leaves no trace in a directory of ordinary files: both values
 * are there afterwards whichever way round they were written. So the rule is
 * a function of its own, nd_cpufreq_max_first(), and it is checked directly.
 * The alternative would have been a test that passes on a wrong order.
 */

#include <string.h>
#include <sys/stat.h>

#include "nd_cpufreq.h"
#include "nd_paths.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------ */

/* The RV1103's own table, in the order the rockchip driver prints it, with the
 * trailing space and newline it really has. */
static void test_parse_reads_the_rv1103_table(void)
{
    int32_t out[ND_CPUFREQ_MAX_STEPS];
    size_t n;

    n = nd_cpufreq_parse_table("408000 600000 816000 1008000 1200000 \n", out, ND_ARRAY_LEN(out));

    CHECK_INT(n, 5);
    CHECK_INT(out[0], 408000);
    CHECK_INT(out[4], 1200000);
}

/* Not every driver prints the table in OPP order, and a menu that jumps from
 * 1.2 GHz to 408 MHz and back reads as a bug in the menu. */
static void test_parse_sorts_ascending(void)
{
    int32_t out[ND_CPUFREQ_MAX_STEPS];
    size_t n;

    n = nd_cpufreq_parse_table("1200000 408000 816000", out, ND_ARRAY_LEN(out));

    CHECK_INT(n, 3);
    CHECK_INT(out[0], 408000);
    CHECK_INT(out[1], 816000);
    CHECK_INT(out[2], 1200000);
}

/* Two OPPs at the same frequency with different voltages are one choice as far
 * as this menu is concerned, and listing it twice would look like a rendering
 * fault rather than like the table it came from. */
static void test_parse_drops_duplicates(void)
{
    int32_t out[ND_CPUFREQ_MAX_STEPS];
    size_t n;

    n = nd_cpufreq_parse_table("816000 408000 816000", out, ND_ARRAY_LEN(out));

    CHECK_INT(n, 2);
    CHECK_INT(out[0], 408000);
    CHECK_INT(out[1], 816000);
}

/* A word in the middle costs its own token and nothing else. Skipping one
 * character at a time instead would turn "n/a" into three failed parses. */
static void test_parse_skips_a_token_that_is_not_a_number(void)
{
    int32_t out[ND_CPUFREQ_MAX_STEPS];
    size_t n;

    n = nd_cpufreq_parse_table("408000 n/a 1200000", out, ND_ARRAY_LEN(out));

    CHECK_INT(n, 2);
    CHECK_INT(out[0], 408000);
    CHECK_INT(out[1], 1200000);
}

/* The cap is a refusal, not a truncation of the caller's buffer. */
static void test_parse_stops_at_the_caller_s_limit(void)
{
    int32_t out[2];
    size_t n;

    n = nd_cpufreq_parse_table("408000 600000 816000 1008000", out, 2u);

    CHECK_INT(n, 2);
    CHECK_INT(out[0], 408000);
    CHECK_INT(out[1], 600000);
}

/* ------------------------------------------------------------------ *
 * Formatting
 * ------------------------------------------------------------------ */

/* Megahertz below a gigahertz, two decimals above it. "999.00 MHz" would be
 * four characters of nothing on a 240-pixel row, and "1200 MHz" is not how
 * anybody says it. */
static void test_format_switches_unit_at_a_gigahertz(void)
{
    char text[32];

    nd_cpufreq_format(text, sizeof text, 408000);
    CHECK_STR(text, "408 MHz");
    nd_cpufreq_format(text, sizeof text, 999000);
    CHECK_STR(text, "999 MHz");
    nd_cpufreq_format(text, sizeof text, 1000000);
    CHECK_STR(text, "1.00 GHz");
    nd_cpufreq_format(text, sizeof text, 1200000);
    CHECK_STR(text, "1.20 GHz");
}

/* A frequency the kernel would not report is drawn as a dash rather than as
 * "0 MHz", which would read as a CPU that had stopped. */
static void test_format_shows_a_dash_for_nothing(void)
{
    char text[32];

    nd_cpufreq_format(text, sizeof text, -1);
    CHECK_STR(text, "--");
    nd_cpufreq_format(text, sizeof text, 0);
    CHECK_STR(text, "--");
}

/* ------------------------------------------------------------------ *
 * The write-order rule
 * ------------------------------------------------------------------ */

/* Raising the target above the ceiling means the ceiling moves first;
 * lowering it means the floor moves first. Equal is neither, and either order
 * works, so the answer only has to be stable. */
static void test_max_first_only_when_raising(void)
{
    CHECK(nd_cpufreq_max_first(1200000, 816000));
    CHECK(!nd_cpufreq_max_first(408000, 1200000));
    CHECK(!nd_cpufreq_max_first(816000, 816000));
}

/* An unreadable ceiling is treated as being below the target, so the ceiling
 * is raised first. Raising a ceiling can never drop a floor; writing a floor
 * against a ceiling you cannot see can be refused. */
static void test_max_first_when_the_ceiling_is_unknown(void)
{
    CHECK(nd_cpufreq_max_first(408000, -1));
}

/* ------------------------------------------------------------------ *
 * Reading the tree
 * ------------------------------------------------------------------ */

static void write_rv1103_tree(void)
{
    pt_write_text(ND_CPUFREQ_AVAILABLE, "408000 600000 816000 1008000 1200000 \n");
    pt_write_text(ND_CPUFREQ_CUR, "1008000\n");
    pt_write_text(ND_CPUFREQ_MIN, "408000\n");
    pt_write_text(ND_CPUFREQ_MAX, "1200000\n");
    pt_write_text(ND_CPUFREQ_GOVERNOR, "schedutil\n");
}

static void test_read_table_finds_the_operating_points(void)
{
    nd_cpufreq_table table;

    write_rv1103_tree();

    CHECK_INT(nd_cpufreq_read_table(&table), ND_OK);
    CHECK_INT(table.n, 5);
    CHECK_INT(table.khz[0], 408000);
    CHECK_INT(table.khz[4], 1200000);
}

/* A kernel built without CONFIG_CPU_FREQ has no such directory. That is QEMU
 * every time, so it is the case the app spends most of its life in and it has
 * to be a reportable answer rather than a crash. */
static void test_read_table_says_notfound_without_cpufreq(void)
{
    nd_cpufreq_table table;

    CHECK_INT(nd_cpufreq_read_table(&table), ND_ERR_NOTFOUND);
    CHECK_INT(table.n, 0);
}

/* The file is there and holds nothing usable -- a driver publishing an empty
 * table. Zero entries is not a menu, so it is refused rather than shown. */
static void test_read_table_refuses_an_empty_table(void)
{
    nd_cpufreq_table table;

    pt_write_text(ND_CPUFREQ_AVAILABLE, "\n");

    CHECK_INT(nd_cpufreq_read_table(&table), ND_ERR_NOTFOUND);
    CHECK_INT(table.n, 0);
}

static void test_read_state_reads_all_four(void)
{
    nd_cpufreq_state state;

    write_rv1103_tree();

    CHECK_INT(nd_cpufreq_read_state(&state), ND_OK);
    CHECK_INT(state.cur_khz, 1008000);
    CHECK_INT(state.min_khz, 408000);
    CHECK_INT(state.max_khz, 1200000);
    CHECK_STR(state.governor, "schedutil");
}

/* Plenty of drivers cannot read the real clock back and so publish no
 * scaling_cur_freq. The range and the governor are still worth showing, so a
 * gap is a -1 in one field rather than a failed read of the lot. */
static void test_read_state_tolerates_a_missing_field(void)
{
    nd_cpufreq_state state;

    pt_write_text(ND_CPUFREQ_MIN, "408000\n");
    pt_write_text(ND_CPUFREQ_MAX, "1200000\n");
    pt_write_text(ND_CPUFREQ_GOVERNOR, "performance\n");

    CHECK_INT(nd_cpufreq_read_state(&state), ND_OK);
    CHECK_INT(state.cur_khz, -1);
    CHECK_INT(state.max_khz, 1200000);
    CHECK_STR(state.governor, "performance");
}

static void test_read_state_says_notfound_when_nothing_is_there(void)
{
    nd_cpufreq_state state;

    CHECK_INT(nd_cpufreq_read_state(&state), ND_ERR_NOTFOUND);
}

/* ------------------------------------------------------------------ *
 * Pinning
 * ------------------------------------------------------------------ */

/* Both ends of the range, not one. Writing only the max leaves the governor
 * free to drop below the chosen point, which for a downclock test means the
 * number on screen and the number the silicon is running at disagree. */
static void test_set_pins_both_ends_of_the_range(void)
{
    char text[32];

    write_rv1103_tree(); /* min 408000, max 1200000 */

    /* 600000 is neither end of the fixture's range on purpose: pinning to a
     * value one of them already holds would pass even if that write never
     * happened. */
    CHECK_INT(nd_cpufreq_set(600000), ND_OK);

    CHECK(pt_read_text(ND_CPUFREQ_MIN, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "600000");
    CHECK(pt_read_text(ND_CPUFREQ_MAX, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "600000");
}

/* The other branch of the order rule. Raising the target above the ceiling
 * takes the max-first path, and both files still end up holding it. */
static void test_set_pins_both_ends_when_raising(void)
{
    char text[32];

    pt_write_text(ND_CPUFREQ_AVAILABLE, "408000 600000 1200000\n");
    pt_write_text(ND_CPUFREQ_MIN, "408000\n");
    pt_write_text(ND_CPUFREQ_MAX, "408000\n");

    CHECK_INT(nd_cpufreq_set(1200000), ND_OK);

    CHECK(pt_read_text(ND_CPUFREQ_MIN, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "1200000");
    CHECK(pt_read_text(ND_CPUFREQ_MAX, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "1200000");
}

/* A refused write is reported, and the other half still happens. Stopping at
 * the first failure would leave the range straddling two frequencies with
 * nothing saying which one won; going on gets the pair as close to consistent
 * as the kernel will allow, and the return value still says so.
 *
 * scaling_min_freq is a DIRECTORY here, which is the cheapest way to make an
 * fopen("wb") fail without needing a kernel that refuses the value. */
static void test_set_reports_a_refused_write_and_finishes_the_pair(void)
{
    char text[32];

    /* Built by hand rather than with write_rv1103_tree(), because
     * scaling_min_freq has to be a directory from the start -- creating the
     * file first and then asking for the directory leaves the file. */
    pt_write_text(ND_CPUFREQ_AVAILABLE, "408000 600000 1200000\n");
    pt_write_text(ND_CPUFREQ_MAX, "1200000\n");
    pt_mkdir(ND_CPUFREQ_MIN);

    CHECK_INT(nd_cpufreq_set(600000), ND_ERR_IO);

    CHECK(pt_read_text(ND_CPUFREQ_MAX, text, sizeof text) != (size_t)-1);
    CHECK_STR(text, "600000");
}

/* Zero is not an operating point and neither is a negative one. Refusing here
 * rather than at the kernel keeps a typo from writing 0 to scaling_min_freq,
 * which some drivers accept and interpret as "no floor". */
static void test_set_refuses_a_frequency_that_is_not_one(void)
{
    write_rv1103_tree();

    CHECK_INT(nd_cpufreq_set(0), ND_ERR_INVAL);
    CHECK_INT(nd_cpufreq_set(-408000), ND_ERR_INVAL);
}

int main(void)
{
    RUN(test_parse_reads_the_rv1103_table);
    RUN(test_parse_sorts_ascending);
    RUN(test_parse_drops_duplicates);
    RUN(test_parse_skips_a_token_that_is_not_a_number);
    RUN(test_parse_stops_at_the_caller_s_limit);
    RUN(test_format_switches_unit_at_a_gigahertz);
    RUN(test_format_shows_a_dash_for_nothing);
    RUN(test_max_first_only_when_raising);
    RUN(test_max_first_when_the_ceiling_is_unknown);
    RUN(test_read_table_finds_the_operating_points);
    RUN(test_read_table_says_notfound_without_cpufreq);
    RUN(test_read_table_refuses_an_empty_table);
    RUN(test_read_state_reads_all_four);
    RUN(test_read_state_tolerates_a_missing_field);
    RUN(test_read_state_says_notfound_when_nothing_is_there);
    RUN(test_set_pins_both_ends_of_the_range);
    RUN(test_set_pins_both_ends_when_raising);
    RUN(test_set_reports_a_refused_write_and_finishes_the_pair);
    RUN(test_set_refuses_a_frequency_that_is_not_one);
    return pt_report("test_cpufreq");
}
