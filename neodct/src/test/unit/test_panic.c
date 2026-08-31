/* test_panic.c -- the core-crash screen: what it says, and where the words
 * land.
 *
 * The interesting half of this file is the CLIPPING test. Every string on
 * this screen is a fixed literal in a 150 px column, and the first draft used
 * the 20 px face for the headline, where "Core System" measures 153 and
 * quietly lost its final letter off the right edge of the panel. Nothing
 * failed: it built, it ran, and it looked almost right. So the layout tests
 * below assert the thing a person would have had to notice -- that no ink in
 * the text column reaches the last column or the last row -- and they will
 * fail the day somebody lengthens a sentence or changes a font size.
 *
 *     ./test_panic [DIR]
 *
 * With a directory argument it also writes the five frames there as PNGs,
 * which is the same picture nd-panic --out produces and the cheapest way to
 * look at a change to this screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_font.h"
#include "nd_image.h"
#include "nd_panic.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Finding the reference tree
 * ------------------------------------------------------------------ */

/* NEODCT_GOLDEN is <repo>/neodct/tests/golden, which the Makefile passes to
 * every test. Two components up is <repo>/neodct, and the overlay beside it
 * is what ND_ROOT has to be for "/NeoDCT/System/..." to resolve onto real
 * assets. Same derivation as smallapp_test.h's sa_resolve_neodct(); done by
 * hand here because this test wants none of the rest of that fixture. */
static bool overlay_root(char *out, size_t out_sz)
{
    const char *golden = getenv("NEODCT_GOLDEN");
    char base[ND_PATH_MAX];
    char *cut;
    size_t i;

    if (golden == NULL || golden[0] == '\0')
        return false;
    (void)nd_strlcpy(base, golden, sizeof base);
    for (i = 0u; i < 2u; i++) {
        cut = strrchr(base, '/');
        if (cut == NULL)
            return false;
        *cut = '\0';
    }
    return nd_snprintf(out, out_sz, "%s/overlay", base) == ND_OK;
}

static const char *font_file(void)
{
    static char path[ND_PATH_MAX];
    char overlay[ND_PATH_MAX];

    if (path[0] != '\0')
        return path;
    if (!overlay_root(overlay, sizeof overlay))
        return NULL;
    /* A REAL filesystem path: nd_font_load() does not resolve ND_ROOT, by the
     * same split ui_load_fonts() uses. */
    if (nd_snprintf(path, sizeof path, "%s" ND_PATH_FONT, overlay) != ND_OK)
        return NULL;
    return path;
}

/* ------------------------------------------------------------------ *
 * Pixel predicates
 * ------------------------------------------------------------------ */

/* Anything visibly lighter than the black background. The threshold is well
 * above the antialiasing at the edge of a glyph and well below its core, so
 * "has ink" means "a person would see something there". */
static bool inked(const nd_image *img, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(img, x, y);

    return (int32_t)c.r + (int32_t)c.g + (int32_t)c.b > 120;
}

static int32_t rightmost_ink(const nd_image *img, int32_t x0, int32_t x1)
{
    int32_t x;
    int32_t y;

    for (x = x1; x >= x0; x--) {
        for (y = 0; y < img->h; y++) {
            if (inked(img, x, y))
                return x;
        }
    }
    return -1;
}

static int32_t leftmost_ink(const nd_image *img, int32_t x0, int32_t x1)
{
    int32_t x;
    int32_t y;

    for (x = x0; x <= x1; x++) {
        for (y = 0; y < img->h; y++) {
            if (inked(img, x, y))
                return x;
        }
    }
    return -1;
}

static int32_t bottommost_ink(const nd_image *img, int32_t x0, int32_t x1)
{
    int32_t x;
    int32_t y;

    for (y = img->h - 1; y >= 0; y--) {
        for (x = x0; x <= x1; x++) {
            if (inked(img, x, y))
                return y;
        }
    }
    return -1;
}

/* How many separate horizontal bands of ink the text column has. Each drawn
 * row is one band, so this counts the lines on the screen without knowing
 * where any of them is. */
static int32_t ink_bands(const nd_image *img, int32_t x0, int32_t x1)
{
    int32_t y;
    int32_t bands = 0;
    bool in_band = false;

    for (y = 0; y < img->h; y++) {
        int32_t x;
        bool row = false;

        for (x = x0; x <= x1 && !row; x++)
            row = inked(img, x, y);
        if (row && !in_band)
            bands++;
        in_band = row;
    }
    return bands;
}

/* ------------------------------------------------------------------ *
 * The shared fixture
 * ------------------------------------------------------------------ */

static nd_font *g_title;
static nd_font *g_body;
static nd_font *g_count;
static nd_panic_fonts g_fonts;
static nd_image *g_art;

static void fixture_open(void)
{
    const char *font = font_file();
    char overlay[ND_PATH_MAX];
    char saved[ND_PATH_MAX];

    if (font != NULL) {
        g_title = nd_font_load(font, ND_FONT_PX_MD);
        g_body = nd_font_load(font, ND_FONT_PX_S);
        g_count = nd_font_load(font, ND_FONT_PX_XL);
    }
    g_fonts.title = g_title;
    g_fonts.body = g_body;
    g_fonts.count = g_count;

    /* The overlay is READ-ONLY here: the root is put back straight away so no
     * later case can write into the source tree. */
    (void)nd_strlcpy(saved, nd_path_root(), sizeof saved);
    if (overlay_root(overlay, sizeof overlay) && nd_path_set_root(overlay) == ND_OK)
        g_art = nd_panic_load_art();
    (void)nd_path_set_root(saved[0] != '\0' ? saved : NULL);
}

static void fixture_close(void)
{
    nd_image_free(g_art);
    nd_font_free(g_title);
    nd_font_free(g_body);
    nd_font_free(g_count);
}

static nd_image *render(nd_panic_mode mode, int32_t status, int32_t crash, int32_t remaining,
                        const nd_image *art, const nd_panic_fonts *fonts)
{
    nd_panic_state st;
    nd_image *canvas = nd_image_new(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888);

    if (canvas == NULL)
        return NULL;
    memset(&st, 0, sizeof st);
    st.mode = mode;
    st.status = status;
    st.crash = crash;
    st.limit = ND_PANIC_MAX_RESTARTS;
    st.remaining = remaining;
    if (nd_panic_draw(canvas, fonts, art, &st) != ND_OK) {
        nd_image_free(canvas);
        return NULL;
    }
    return canvas;
}

/* ------------------------------------------------------------------ *
 * The wait status a dead nd-core leaves behind
 * ------------------------------------------------------------------ */

static void t_status_text_names_the_signal(void)
{
    char buf[64];

    (void)nd_panic_status_text(139, buf, sizeof buf);
    CHECK_STR(buf, "SIGSEGV (11)");
    (void)nd_panic_status_text(134, buf, sizeof buf);
    CHECK_STR(buf, "SIGABRT (6)");
    (void)nd_panic_status_text(135, buf, sizeof buf);
    CHECK_STR(buf, "SIGBUS (7)");

    /* A signal the table has no name for says only what is known, rather than
     * "signal (37)", which reads like a bug in the crash screen. */
    (void)nd_panic_status_text(128 + 37, buf, sizeof buf);
    CHECK_STR(buf, "signal 37");
}

static void t_status_text_reads_an_exit_code(void)
{
    char buf[64];

    (void)nd_panic_status_text(1, buf, sizeof buf);
    CHECK_STR(buf, "exit code 1");
    (void)nd_panic_status_text(127, buf, sizeof buf);
    CHECK_STR(buf, "exit code 127");

    /* 255 is above 128 and is NOT 128+127: a signal number that high does not
     * exist, so it stays an exit code. */
    (void)nd_panic_status_text(255, buf, sizeof buf);
    CHECK_STR(buf, "exit code 255");

    /* Zero happens: init asked nd-core to stop. The screen only ever shows
     * this one when a clean exit repeated itself into a halt. */
    (void)nd_panic_status_text(0, buf, sizeof buf);
    CHECK_STR(buf, "exited cleanly");
}

static void t_status_becomes_a_crash_report(void)
{
    nd_crash_info info;

    nd_panic_status_info(139, &info);
    CHECK(info.from_signal);
    CHECK_INT(info.signo, 11);
    CHECK_INT(info.exit_status, 0);
    CHECK(strstr(info.detail, "SIGSEGV") != NULL);
    /* Nobody was alive to record either, and the log should not imply
     * otherwise by printing a plausible zero. */
    CHECK_INT(info.si_code, 0);
    CHECK(info.fault_addr == NULL);

    nd_panic_status_info(3, &info);
    CHECK(!info.from_signal);
    CHECK_INT(info.exit_status, 3);
    CHECK(strstr(info.detail, "exit code 3") != NULL);
}

static void t_countdown_stops_before_zero(void)
{
    char buf[32];

    CHECK(nd_panic_countdown_text(3, buf, sizeof buf) > 0u);
    CHECK_STR(buf, "3...");
    CHECK(nd_panic_countdown_text(1, buf, sizeof buf) > 0u);
    CHECK_STR(buf, "1...");

    /* There is no "0..." frame -- the lead line takes over. */
    CHECK_INT(nd_panic_countdown_text(0, buf, sizeof buf), 0);
    CHECK_STR(buf, "");
    CHECK_INT(nd_panic_countdown_text(-4, buf, sizeof buf), 0);
    CHECK_STR(buf, "");
}

/* ------------------------------------------------------------------ *
 * The artwork
 * ------------------------------------------------------------------ */

static void t_art_is_the_phone_and_only_the_phone(void)
{
    CHECK(g_art != NULL);
    if (g_art == NULL)
        return;

    CHECK_INT(g_art->w, ND_PANIC_ART_W);
    CHECK_INT(g_art->h, ND_PANIC_ART_H);
    CHECK_INT(g_art->fmt, ND_PIXFMT_RGB888);

    /* The hand-lettered "Application crashed :(" in the source picture lives
     * at x 3..72, left of the crop. If the crop box ever slid left, this is
     * what would notice. */
    CHECK(rightmost_ink(g_art, 0, g_art->w - 1) >= g_art->w - 4);
    CHECK(leftmost_ink(g_art, 0, g_art->w - 1) <= 3);
}

static void t_missing_artwork_is_not_a_missing_screen(void)
{
    /* ND_ROOT is a fresh empty scratch directory, so CRASH.jpg is not there.
     * This is the case where a corrupt or half-mounted rootfs meets the code
     * whose job is to report a corrupt or half-mounted rootfs. */
    nd_image *art = nd_panic_load_art();
    nd_image *canvas;

    CHECK(art == NULL);

    canvas = render(ND_PANIC_RESTART, 139, 1, 3, NULL, &g_fonts);
    CHECK(canvas != NULL);
    if (canvas == NULL)
        return;

    /* The text has moved into the space the phone would have had. */
    CHECK_INT(leftmost_ink(canvas, 0, ND_UI_W - 1), ND_PANIC_TEXT_X_BARE);
    CHECK(ink_bands(canvas, 0, ND_UI_W - 1) >= 5);
    nd_image_free(canvas);
}

/* ------------------------------------------------------------------ *
 * The layout
 * ------------------------------------------------------------------ */

static void t_nothing_runs_off_the_panel(void)
{
    static const int32_t STATUSES[] = {139, 134, 1, 127, 255, 0, 128 + 37};
    size_t i;

    /* Every string this screen can show, drawn in the narrowest case (the
     * artwork present, so the column is 150 px), checked for ink in the last
     * column or the last row. This is the test the 20 px headline would have
     * failed. */
    for (i = 0u; i < ND_ARRAY_LEN(STATUSES); i++) {
        nd_panic_mode mode = (i % 2u) == 0u ? ND_PANIC_RESTART : ND_PANIC_HALT;
        nd_image *canvas = render(mode, STATUSES[i], 3, 3, g_art, &g_fonts);

        CHECK(canvas != NULL);
        if (canvas == NULL)
            continue;
        CHECK(rightmost_ink(canvas, ND_PANIC_TEXT_X, ND_UI_W - 1) < ND_UI_W - 1);
        CHECK(bottommost_ink(canvas, ND_PANIC_TEXT_X, ND_UI_W - 1) < ND_UI_H - 1);
        nd_image_free(canvas);
    }
}

static void t_the_gutter_stays_empty(void)
{
    nd_image *canvas = render(ND_PANIC_RESTART, 139, 2, 2, g_art, &g_fonts);

    CHECK(canvas != NULL);
    if (canvas == NULL)
        return;

    /* Eight black columns between the phone and the first letter. A layout
     * change that let the text creep left would make the two collide long
     * before it clipped anything. */
    CHECK_INT(leftmost_ink(canvas, ND_PANIC_ART_W, ND_PANIC_TEXT_X - 1), -1);
    CHECK_INT(leftmost_ink(canvas, ND_PANIC_TEXT_X, ND_UI_W - 1), ND_PANIC_TEXT_X);
    nd_image_free(canvas);
}

static void t_the_countdown_is_the_only_thing_that_moves(void)
{
    nd_image *three = render(ND_PANIC_RESTART, 139, 1, 3, g_art, &g_fonts);
    nd_image *two = render(ND_PANIC_RESTART, 139, 1, 2, g_art, &g_fonts);
    nd_image *halt = render(ND_PANIC_HALT, 139, 3, 0, g_art, &g_fonts);
    int32_t y;
    int32_t differing_rows = 0;

    CHECK(three != NULL && two != NULL && halt != NULL);
    if (three == NULL || two == NULL || halt == NULL)
        goto done;

    for (y = 0; y < ND_UI_H; y++) {
        if (memcmp(three->pixels + (size_t)y * three->stride, two->pixels + (size_t)y * two->stride,
                   (size_t)ND_UI_W * 3u) != 0)
            differing_rows++;
    }
    /* One 24 px digit's worth of rows changed and nothing else, which is what
     * makes the screen read as a countdown rather than as a flicker. */
    CHECK(differing_rows > 0);
    CHECK(differing_rows <= 24);

    /* The halt screen is a different screen, not the countdown with a
     * different number in it. */
    CHECK(memcmp(three->pixels, halt->pixels, (size_t)ND_UI_H * three->stride) != 0);

done:
    nd_image_free(three);
    nd_image_free(two);
    nd_image_free(halt);
}

static void t_the_attempt_count_is_shown_only_when_it_is_known(void)
{
    nd_image *known = render(ND_PANIC_RESTART, 139, 2, 3, g_art, &g_fonts);
    nd_image *unknown = render(ND_PANIC_RESTART, 139, 0, 3, g_art, &g_fonts);

    CHECK(known != NULL && unknown != NULL);
    if (known != NULL && unknown != NULL) {
        /* "try 2 of 3" is one whole band of ink, so dropping it drops a line
         * rather than blanking one. */
        CHECK_INT(ink_bands(known, ND_PANIC_TEXT_X, ND_UI_W - 1),
                  ink_bands(unknown, ND_PANIC_TEXT_X, ND_UI_W - 1) + 1);
    }
    nd_image_free(known);
    nd_image_free(unknown);
}

static void t_no_font_leaves_a_red_panel(void)
{
    nd_panic_fonts none = {NULL, NULL, NULL};
    nd_image *canvas = render(ND_PANIC_RESTART, 139, 1, 3, g_art, &none);
    int32_t x;
    int32_t y;
    bool all_red = true;

    CHECK(canvas != NULL);
    if (canvas == NULL)
        return;

    for (y = 0; y < ND_UI_H && all_red; y++) {
        for (x = 0; x < ND_UI_W; x++) {
            nd_color c = nd_image_get_px(canvas, x, y);

            if (c.r != 255u || c.g != 0u || c.b != 0u) {
                all_red = false;
                break;
            }
        }
    }
    /* Not black, not the UI, and visibly not a hang. The artwork is dropped
     * with the text: half a screen would look like a rendering bug. */
    CHECK(all_red);
    nd_image_free(canvas);

    /* One face is enough to draw the whole thing, badly but legibly. */
    none.body = g_body;
    canvas = render(ND_PANIC_RESTART, 139, 1, 3, g_art, &none);
    CHECK(canvas != NULL);
    if (canvas != NULL) {
        CHECK(ink_bands(canvas, ND_PANIC_TEXT_X, ND_UI_W - 1) >= 5);
        nd_image_free(canvas);
    }
}

/* ------------------------------------------------------------------ *
 * Writing the frames out
 * ------------------------------------------------------------------ */

static void save_frames(const char *dir)
{
    static const struct {
        nd_panic_mode mode;
        int32_t remaining;
        const char *name;
    } SHOTS[] = {
        {ND_PANIC_RESTART, 3, "panic-3"},
        {ND_PANIC_RESTART, 2, "panic-2"},
        {ND_PANIC_RESTART, 1, "panic-1"},
        {ND_PANIC_HALT, 0, "panic-halt"},
    };
    char saved[ND_PATH_MAX];
    size_t i;

    /* The output directory belongs to the developer's filesystem, not to the
     * phone's, so ND_ROOT comes off for the duration -- the same exception
     * nd-shoot makes around its capture calls. */
    (void)nd_strlcpy(saved, nd_path_root(), sizeof saved);
    (void)nd_path_set_root(NULL);

    for (i = 0u; i < ND_ARRAY_LEN(SHOTS); i++) {
        nd_image *canvas = render(SHOTS[i].mode, 139, SHOTS[i].mode == ND_PANIC_HALT ? 3 : 1,
                                  SHOTS[i].remaining, g_art, &g_fonts);
        char path[ND_PATH_MAX];

        if (canvas == NULL)
            continue;
        if (nd_snprintf(path, sizeof path, "%s/%s.png", dir, SHOTS[i].name) == ND_OK &&
            nd_image_save_png(canvas, path) == ND_OK)
            (void)printf("wrote %s\n", path);
        else
            (void)fprintf(stderr, "could not write %s/%s.png\n", dir, SHOTS[i].name);
        nd_image_free(canvas);
    }

    /* One with no artwork, because that layout has no other reviewer. */
    {
        nd_image *canvas = render(ND_PANIC_RESTART, 139, 1, 2, NULL, &g_fonts);
        char path[ND_PATH_MAX];

        if (canvas != NULL) {
            if (nd_snprintf(path, sizeof path, "%s/panic-noart.png", dir) == ND_OK &&
                nd_image_save_png(canvas, path) == ND_OK)
                (void)printf("wrote %s\n", path);
            nd_image_free(canvas);
        }
    }

    (void)nd_path_set_root(saved[0] != '\0' ? saved : NULL);
}

int main(int argc, char **argv)
{
    pt_new_case();
    fixture_open();
    if (g_body == NULL)
        (void)fprintf(stderr, "note: no font found; the layout cases will be thin\n");

    RUN(t_status_text_names_the_signal);
    RUN(t_status_text_reads_an_exit_code);
    RUN(t_status_becomes_a_crash_report);
    RUN(t_countdown_stops_before_zero);
    RUN(t_art_is_the_phone_and_only_the_phone);
    RUN(t_missing_artwork_is_not_a_missing_screen);
    RUN(t_nothing_runs_off_the_panel);
    RUN(t_the_gutter_stays_empty);
    RUN(t_the_countdown_is_the_only_thing_that_moves);
    RUN(t_the_attempt_count_is_shown_only_when_it_is_known);
    RUN(t_no_font_leaves_a_red_panel);

    /* An empty argument is not a directory. A harness that passes "$2"
     * unquoted-and-unset would otherwise write panic-3.png into the root of
     * the filesystem, which is exactly what happened the first time. */
    if (argc > 1 && argv[1][0] != '\0')
        save_frames(argv[1]);

    fixture_close();
    pt_cleanup();
    (void)printf("test_panic: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
