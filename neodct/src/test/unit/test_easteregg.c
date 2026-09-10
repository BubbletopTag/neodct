/* test_easteregg.c -- the dialled code and the nonsense generator.
 *
 * Only the two pure halves are exercised. nd_easteregg_run() draws to a
 * canvas and spawns a player, and neither belongs in a unit test: what CAN
 * go wrong here without a screen is the code matching something it should
 * not, and a generated line overrunning its buffer.
 *
 * The match is the one with teeth. The code is typed into the same buffer as
 * a phone number, so a sloppy comparison would either fire while somebody was
 * still typing, or swallow a real call.
 */

#include <string.h>

#include "nd_easteregg.h"
#include "nd_types.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * The code
 * ------------------------------------------------------------------ */

static void test_the_code_matches_itself(void)
{
    CHECK(nd_easteregg_is_code(ND_EASTEREGG_CODE));
}

static void test_nothing_else_matches(void)
{
    CHECK(!nd_easteregg_is_code(""));
    CHECK(!nd_easteregg_is_code(NULL));
    CHECK(!nd_easteregg_is_code("999"));
    CHECK(!nd_easteregg_is_code("+15551234567"));
    /* The digits alone, without the surround. */
    CHECK(!nd_easteregg_is_code("22327753"));
}

/* A PREFIX must not fire: the buffer holds the code as it is being typed, and
 * a match on "starts with" would trigger before the last character. */
static void test_a_partial_code_does_not_match(void)
{
    char partial[32];
    size_t full = strlen(ND_EASTEREGG_CODE);
    size_t i;

    for (i = 1u; i < full; i++) {
        (void)nd_strlcpy(partial, ND_EASTEREGG_CODE, i + 1u);
        CHECK(!nd_easteregg_is_code(partial));
    }
}

/* And a SUPERSET must not fire either, in either direction -- a number that
 * merely contains the code is still a number. */
static void test_the_code_with_anything_added_does_not_match(void)
{
    CHECK(!nd_easteregg_is_code(ND_EASTEREGG_CODE "1"));
    CHECK(!nd_easteregg_is_code(ND_EASTEREGG_CODE "#"));
    CHECK(!nd_easteregg_is_code("1" ND_EASTEREGG_CODE));
}

/* ------------------------------------------------------------------ *
 * The nonsense
 * ------------------------------------------------------------------ */

/* Every line the scroll will ever ask for must fit the panel row it is drawn
 * into, and must be a string. A generator that ran off the end of its buffer
 * would be a stack smash in the core, for a joke. */
static void test_every_line_is_terminated_and_fits(void)
{
    char line[ND_EGG_LINE_MAX];
    uint32_t n;

    for (n = 0u; n < (uint32_t)ND_EGG_LINES_TOTAL; n++) {
        size_t len;

        memset(line, 'x', sizeof line);
        nd_easteregg_line(n, line, sizeof line);
        len = strnlen(line, sizeof line);
        CHECK(len < sizeof line); /* i.e. a NUL was written inside the buffer */
        CHECK(len > 0u);          /* and it is not an empty row */
    }
}

/* Deterministic in n alone. This is what makes the case above meaningful:
 * without it, "every line fits" would only be true of the run that happened
 * to be measured. */
static void test_the_same_line_number_gives_the_same_line(void)
{
    char a[ND_EGG_LINE_MAX];
    char b[ND_EGG_LINE_MAX];
    uint32_t n;

    for (n = 0u; n < 32u; n++) {
        nd_easteregg_line(n, a, sizeof a);
        nd_easteregg_line(n, b, sizeof b);
        CHECK_STR(a, b);
    }
}

/* It has to look busy. If the scrambler collapsed, every row would be the
 * same string and the effect would be gone -- so this asserts the output
 * actually varies rather than merely that it exists. */
static void test_the_lines_are_not_all_the_same(void)
{
    char first[ND_EGG_LINE_MAX];
    char line[ND_EGG_LINE_MAX];
    uint32_t n;
    int different = 0;

    nd_easteregg_line(0u, first, sizeof first);
    for (n = 1u; n < (uint32_t)ND_EGG_LINES_TOTAL; n++) {
        nd_easteregg_line(n, line, sizeof line);
        if (strcmp(line, first) != 0)
            different++;
    }
    /* Not "all different" -- twenty shapes over ninety-six lines must repeat,
     * and random numbers repeat too. Most of them differing is the claim. */
    CHECK(different > (ND_EGG_LINES_TOTAL / 2));
}

/* A caller with no room must get an empty string rather than a partial line
 * or a walked-off buffer. */
static void test_a_tiny_buffer_is_handled(void)
{
    char one[1];

    one[0] = 'x';
    nd_easteregg_line(3u, one, sizeof one);
    CHECK_INT((int)one[0], 0);

    /* And a zero-length buffer must not be written to at all. */
    nd_easteregg_line(3u, one, 0u);
}

/* The rows the scroll keeps must fit the panel: twelve rows at a pitch of
 * fourteen is 168, and the panel is 175. Asserted because both numbers are
 * constants somebody could reasonably change. */
static void test_the_rows_fit_the_panel(void)
{
    CHECK(ND_EGG_ROWS * ND_EGG_PITCH <= 175);
    /* And there are enough of them to look like a terminal rather than a
     * caption. */
    CHECK(ND_EGG_ROWS >= 8);
}

int main(void)
{
    RUN(test_the_code_matches_itself);
    RUN(test_nothing_else_matches);
    RUN(test_a_partial_code_does_not_match);
    RUN(test_the_code_with_anything_added_does_not_match);
    RUN(test_every_line_is_terminated_and_fits);
    RUN(test_the_same_line_number_gives_the_same_line);
    RUN(test_the_lines_are_not_all_the_same);
    RUN(test_a_tiny_buffer_is_handled);
    RUN(test_the_rows_fit_the_panel);
    return pt_report("test_easteregg");
}
