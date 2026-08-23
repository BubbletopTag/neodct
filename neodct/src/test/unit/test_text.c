/* test_text.c -- the six text-fitting routines against the Python they came
 * from.
 *
 * Every expected value in this file was produced by running the actual
 * functions out of System/ui/framework.py -- fit_text, _ellipsize,
 * _wrap_lines, TextInputLong._wrap_text and MessageDialog._wrap_text -- over
 * the same inputs with Pillow's layout engine pinned to BASIC, which is the
 * engine the phone has. They are not hand-derived, and several of them look
 * wrong until you read the Python:
 *
 *   * _ellipsize("Messages", 20 px, max_w=30) returns "Messages", over-wide,
 *     because trimming ran out of characters and the fallback is the ORIGINAL
 *     text. fit_text on the same input returns "".
 *   * TextInputLong's wrapper emits a trailing EMPTY line after a hard-broken
 *     word, because `cur` is "" when the word loop ends and it appends `cur`
 *     unconditionally. MessageDialog's copy pops it. That one blank line is
 *     the whole difference between the two functions.
 *   * A line of only a TAB wraps to nothing under _wrap_lines (its
 *     `raw.strip()` shortcut) but to one line holding the tab under both
 *     hard-breaking wrappers, which have no such shortcut.
 *
 *   ./test_text [path/to/font.ttf]
 *
 * With no argument it finds the TTF beside $NEODCT_GOLDEN, the same way
 * test_font does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_font.h"
#include "nd_text.h"
#include "nd_ui.h"

static size_t g_checks;
static size_t g_fails;

static void eq_str(const char *what, const char *got, const char *want)
{
    g_checks++;
    if (strcmp(got, want) == 0)
        return;
    g_fails++;
    fprintf(stderr, "FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
}

static void eq_sz(const char *what, size_t got, size_t want)
{
    g_checks++;
    if (got == want)
        return;
    g_fails++;
    fprintf(stderr, "FAIL %s: got %zu want %zu\n", what, got, want);
}

static void eq_ptr(const char *what, const void *got, const void *want)
{
    g_checks++;
    if (got == want)
        return;
    g_fails++;
    fprintf(stderr, "FAIL %s: pointer mismatch\n", what);
}

/* ------------------------------------------------------------------ *
 * Cases
 * ------------------------------------------------------------------ */

typedef struct {
    const char *text;
    int32_t px;
    int32_t max_w;
    const char *want;
} fit_case;

static const fit_case FIT[] = {
    {"Messages", 20, 120, "Messag..."},
    {"Messages", 20, 60, "Me..."},
    {"Messages", 20, 30, ""},
    {"Messages", 20, 10, ""},
    {"Messages", 20, 0, ""},
    {"", 20, 100, ""},
    {"Phone Book", 24, 100, "Phon..."},
    {"Phone Book", 24, 200, "Phone Book"},
    {"This application has not been implemented yet.", 14, 220,
     "This application has no..."},
    /* rstrip(): the prefix "A " loses its space before the dots. */
    {"A  B  C", 20, 50, "A..."},
    {"WWWWW", 20, 20, ""},
    {"iiiii", 14, 12, ""},
    {"Koki Mobile", 18, 80, "Koki..."},
};

static const fit_case ELL[] = {
    {"Messages", 20, 120, "Messag..."},
    {"Messages", 20, 60, "Me..."},
    /* THE ASYMMETRY: fit_text gives "" for these three, _ellipsize gives the
     * original string back, over-wide. */
    {"Messages", 20, 30, "Messages"},
    {"Messages", 20, 10, "Messages"},
    {"Messages", 20, 0, "Messages"},
    {"", 20, 100, ""},
    {"Phone Book", 24, 100, "Phon..."},
    {"This application has not been implemented yet.", 14, 220,
     "This application has no..."},
    {"WWWWW", 20, 20, "WWWWW"},
    {"A", 20, 1, "A"},
    {"Settings", 18, 200, "Settings"},
};

typedef struct {
    const char *text;
    int32_t px;
    int32_t max_w;
    size_t n;
    const char *want[8];
} wrap_case;

static const wrap_case WRAP[] = {
    {"This application has not been implemented yet.", 14, 220, 2,
     {"This application has not", "been implemented yet."}},
    {"This application has not been implemented yet.", 20, 220, 4,
     {"This application", "has not been", "implemented", "yet."}},
    {"hello world", 20, 240, 1, {"hello world"}},
    {"", 20, 100, 0, {NULL}},
    {"\n", 20, 100, 0, {NULL}},
    {"a\n\nb", 20, 100, 3, {"a", "", "b"}},
    {"a\n\n", 20, 100, 1, {"a"}},
    {"a\r\nb", 20, 100, 2, {"a", "b"}},
    {"  leading and trailing  ", 14, 220, 1, {"leading and trailing"}},
    /* Left over-wide: _wrap_lines never breaks a word. */
    {"supercalifragilisticexpialidocious", 20, 100, 1, {"supercalifragilisticexpialidocious"}},
    {"short WWWWWWWWWWWWWWWWWWWW tail", 20, 100, 3,
     {"short", "WWWWWWWWWWWWWWWWWWWW", "tail"}},
    {"one   two", 20, 240, 1, {"one two"}},
    {"\t", 20, 100, 0, {NULL}},
    {"word", 20, 1, 1, {"word"}},
    {"aaa bbb ccc ddd", 14, 50, 4, {"aaa", "bbb", "ccc", "ddd"}},
};

static const wrap_case WRAPB[] = {
    {"This application has not been implemented yet.", 14, 220, 2,
     {"This application has not", "been implemented yet."}},
    {"This application has not been implemented yet.", 20, 220, 4,
     {"This application", "has not been", "implemented", "yet."}},
    {"hello world", 20, 240, 1, {"hello world"}},
    {"", 20, 100, 1, {""}},
    {"\n", 20, 100, 1, {""}},
    {"a\n\nb", 20, 100, 3, {"a", "", "b"}},
    {"a\n\n", 20, 100, 2, {"a", ""}},
    {"a\r\nb", 20, 100, 2, {"a", "b"}},
    {"  leading and trailing  ", 14, 220, 1, {"leading and trailing"}},
    /* The trailing "" is the appended empty `cur` after the hard break. */
    {"supercalifragilisticexpialidocious", 20, 100, 6,
     {"superc", "alifragi", "listicex", "pialido", "cious", ""}},
    {"short WWWWWWWWWWWWWWWWWWWW tail", 20, 100, 7,
     {"short", "WWWW", "WWWW", "WWWW", "WWWW", "WWWW", "tail"}},
    {"one   two", 20, 240, 1, {"one two"}},
    {"\t", 20, 100, 1, {"\t"}},
    {"word", 20, 1, 5, {"w", "o", "r", "d", ""}},
    {"aaa bbb ccc ddd", 14, 50, 4, {"aaa", "bbb", "ccc", "ddd"}},
};

static const wrap_case WRAPBP[] = {
    {"This application has not been implemented yet.", 14, 220, 2,
     {"This application has not", "been implemented yet."}},
    {"This application has not been implemented yet.", 20, 220, 4,
     {"This application", "has not been", "implemented", "yet."}},
    {"hello world", 20, 240, 1, {"hello world"}},
    {"", 20, 100, 0, {NULL}},
    {"\n", 20, 100, 0, {NULL}},
    {"a\n\nb", 20, 100, 3, {"a", "", "b"}},
    {"a\n\n", 20, 100, 1, {"a"}},
    {"a\r\nb", 20, 100, 2, {"a", "b"}},
    {"  leading and trailing  ", 14, 220, 1, {"leading and trailing"}},
    {"supercalifragilisticexpialidocious", 20, 100, 5,
     {"superc", "alifragi", "listicex", "pialido", "cious"}},
    {"short WWWWWWWWWWWWWWWWWWWW tail", 20, 100, 7,
     {"short", "WWWW", "WWWW", "WWWW", "WWWW", "WWWW", "tail"}},
    {"one   two", 20, 240, 1, {"one two"}},
    {"\t", 20, 100, 1, {"\t"}},
    {"word", 20, 1, 4, {"w", "o", "r", "d"}},
    {"aaa bbb ccc ddd", 14, 50, 4, {"aaa", "bbb", "ccc", "ddd"}},
};

static nd_font *pick(nd_font *fonts[4], int32_t px)
{
    size_t i;

    for (i = 0; i < 4; i++) {
        if (nd_font_px(fonts[i]) == px)
            return fonts[i];
    }
    return NULL;
}

static void run_wrap(const char *name, nd_font *fonts[4], const wrap_case *cases, size_t n,
                     void (*fn)(nd_lines *, const char *, const nd_font *, int32_t))
{
    char storage[16][ND_TEXT_LINE_MAX];
    nd_lines lines;
    size_t c;

    for (c = 0; c < n; c++) {
        char what[160];
        size_t i;

        nd_lines_init(&lines, storage, 16);
        fn(&lines, cases[c].text, pick(fonts, cases[c].px), cases[c].max_w);

        snprintf(what, sizeof what, "%s[%zu] line count", name, c);
        eq_sz(what, lines.n, cases[c].n);
        for (i = 0; i < cases[c].n && i < lines.n; i++) {
            snprintf(what, sizeof what, "%s[%zu] line %zu", name, c, i);
            eq_str(what, nd_lines_at(&lines, i), cases[c].want[i]);
        }
    }
}

int main(int argc, char **argv)
{
    static const int32_t SIZES[4] = {14, 18, 20, 24};
    char fontpath[1024];
    nd_font *fonts[4] = {NULL, NULL, NULL, NULL};
    char out[256];
    size_t i;

    if (argc > 1) {
        snprintf(fontpath, sizeof fontpath, "%s", argv[1]);
    } else {
        const char *golden = getenv("NEODCT_GOLDEN");
        char base[512];
        char *cut;

        if (!golden || !*golden) {
            fprintf(stderr, "test_text: no font; set NEODCT_GOLDEN or pass a path\n");
            return 1;
        }
        snprintf(base, sizeof base, "%s", golden);
        cut = strrchr(base, '/');
        if (cut)
            *cut = '\0';
        cut = strrchr(base, '/');
        if (cut)
            *cut = '\0';
        snprintf(fontpath, sizeof fontpath,
                 "%s/overlay/NeoDCT/System/ui/resources/fonts/font.ttf", base);
    }

    for (i = 0; i < 4; i++) {
        fonts[i] = nd_font_load(fontpath, SIZES[i]);
        if (!fonts[i]) {
            fprintf(stderr, "test_text: cannot load %s at %d px\n", fontpath, (int)SIZES[i]);
            return 1;
        }
    }

    for (i = 0; i < ND_ARRAY_LEN(FIT); i++) {
        char what[64];

        snprintf(what, sizeof what, "fit_text[%zu]", i);
        nd_text_fit(out, sizeof out, FIT[i].text, pick(fonts, FIT[i].px), FIT[i].max_w);
        eq_str(what, out, FIT[i].want);
    }

    for (i = 0; i < ND_ARRAY_LEN(ELL); i++) {
        char what[64];

        snprintf(what, sizeof what, "ellipsize[%zu]", i);
        nd_text_ellipsize(out, sizeof out, ELL[i].text, pick(fonts, ELL[i].px), ELL[i].max_w);
        eq_str(what, out, ELL[i].want);
    }

    run_wrap("wrap", fonts, WRAP, ND_ARRAY_LEN(WRAP), nd_text_wrap);
    run_wrap("wrap_break", fonts, WRAPB, ND_ARRAY_LEN(WRAPB), nd_text_wrap_break);
    run_wrap("wrap_break_pop", fonts, WRAPBP, ND_ARRAY_LEN(WRAPBP), nd_text_wrap_break_pop);

    /* ---- the font ladder ---- */
    {
        nd_ui ui;
        const nd_font *ladder[4];
        size_t n;

        memset(&ui, 0, sizeof ui);
        ui.font_s = fonts[0];
        ui.font_md = fonts[1];
        ui.font_n = fonts[2];
        ui.font_xl = fonts[3];

        n = nd_font_ladder(&ui, ladder, ND_ARRAY_LEN(ladder));
        eq_sz("ladder length", n, 3);
        eq_ptr("ladder[0] is font_n", ladder[0], fonts[2]);
        eq_ptr("ladder[1] is font_md", ladder[1], fonts[1]);
        eq_ptr("ladder[2] is font_s", ladder[2], fonts[0]);

        /* A UI whose font_md failed to load holds font_n twice; the ladder
         * must not offer the same size on two rungs. */
        ui.font_md = ui.font_n;
        n = nd_font_ladder(&ui, ladder, ND_ARRAY_LEN(ladder));
        eq_sz("ladder dedupes", n, 2);
        eq_ptr("deduped ladder[1] is font_s", ladder[1], fonts[0]);

        ui.font_md = fonts[1];
        ui.font_n = NULL;
        n = nd_font_ladder(&ui, ladder, ND_ARRAY_LEN(ladder));
        eq_sz("ladder skips NULL", n, 2);
        eq_ptr("ladder without font_n starts at font_md", ladder[0], fonts[1]);

        /* _fit_font: "Messages" is 123 px at 20, 103 at 18, 85 at 14. */
        ui.font_n = fonts[2];
        n = nd_font_ladder(&ui, ladder, ND_ARRAY_LEN(ladder));
        eq_ptr("fit_font picks 20", nd_fit_font("Messages", 130, ladder, n), fonts[2]);
        eq_ptr("fit_font picks 18", nd_fit_font("Messages", 110, ladder, n), fonts[1]);
        eq_ptr("fit_font picks 14", nd_fit_font("Messages", 90, ladder, n), fonts[0]);
        /* Nothing fits -> the LAST rung, over-wide. */
        eq_ptr("fit_font falls back to the last", nd_fit_font("Messages", 10, ladder, n),
               fonts[0]);
    }

    /* ---- nd_lines bookkeeping ---- */
    {
        char storage[2][ND_TEXT_LINE_MAX];
        nd_lines l;

        nd_lines_init(&l, storage, 2);
        eq_sz("lines start empty", l.n, 0);
        eq_str("out-of-range line is empty", nd_lines_at(&l, 0), "");
        nd_lines_push(&l, "one");
        nd_lines_push(&l, "two");
        eq_sz("two pushes", l.n, 2);
        g_checks++;
        if (nd_lines_push(&l, "three")) {
            g_fails++;
            fprintf(stderr, "FAIL push past cap reported success\n");
        }
        g_checks++;
        if (!l.truncated) {
            g_fails++;
            fprintf(stderr, "FAIL push past cap did not set truncated\n");
        }
        nd_lines_clear(&l);
        eq_sz("clear resets n", l.n, 0);
        g_checks++;
        if (l.truncated) {
            g_fails++;
            fprintf(stderr, "FAIL clear did not reset truncated\n");
        }

        /* A wrap that needs more lines than the caller gave it drops the
         * extras and says so, rather than writing off the end. */
        nd_lines_init(&l, storage, 2);
        nd_text_wrap(&l, "aaa bbb ccc ddd", fonts[0], 50);
        eq_sz("over-long wrap is clipped to cap", l.n, 2);
        g_checks++;
        if (!l.truncated) {
            g_fails++;
            fprintf(stderr, "FAIL clipped wrap did not set truncated\n");
        }
    }

    printf("test_text: %zu checks, %zu failures\n", g_checks, g_fails);
    for (i = 0; i < 4; i++)
        nd_font_free(fonts[i]);
    return g_fails == 0 ? 0 : 1;
}
