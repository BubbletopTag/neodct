/* test_widgets_text.c -- TextInput, TextInputLong, PredictiveText and the T9
 * mode indicator.
 *
 * Five kinds of check:
 *
 *  1. TWO GOLDEN FRAMES. widget-textinput and widget-textinputlong are
 *     re-rendered and their SHA-256 over raw RGB compared with the reference
 *     manifest -- the same digest goldenframe.py compares. The draw sequence
 *     mirrors shoot_docs.py's shoot_widgets(), INCLUDING the "Select" softkey
 *     the PagedList before them left behind: both widgets clear rows 0..145
 *     only, so the strip survives into both frames.
 *
 *  2. GEOMETRY, as numbers and as individual pixels, so a failure says which
 *     coordinate moved rather than "one frame differs".
 *
 *  3. THE PENCIL, compared against a bitmap generated from the Python's own
 *     _draw_pencil at two sizes. Size 10 and size 30 are in there because
 *     they are where Python's banker's rounding and C's round() disagree.
 *
 *  4. KEY HANDLING on both paths -- the QWERTY dev keyboard and the i2c
 *     multi-tap keypad -- including predictive mode against a small
 *     dictionary written into the scratch root.
 *
 *  5. THE INCREMENTAL REWRAP (decision C-2), proved by DIFFERENCE: a message
 *     composed one keypress at a time, with the watermark doing its work, has
 *     to render to the same bytes as the same message wrapped from scratch.
 *     That is the only test that matters for the optimisation -- it says the
 *     shortcut is invisible.
 *
 * Set NEODCT_FRAMES_OUT to a directory to also write the PNGs and a
 * manifest.json there, which is what
 *   python3 neodct/tools/goldenframe.py --compare neodct/tests/golden <dir>
 * consumes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_paths.h"
#include "nd_t9.h"
#include "nd_text.h"
#include "nd_timeset.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Finding the font and the reference set (same search as the other
 * rendering tests; the acceptance gate supplies neither variable)
 * ------------------------------------------------------------------ */

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

static bool resolve_golden(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_GOLDEN");

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    if (file_exists("../tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "../tests/golden", sz);
        return true;
    }
    if (file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "neodct/tests/golden", sz);
        return true;
    }
    return false;
}

static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    char golden[1024];
    char base[512];
    char cand[1024];
    char *cut;

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    if (resolve_golden(golden, sizeof golden)) {
        (void)snprintf(base, sizeof base, "%.480s", golden);
        cut = strrchr(base, '/'); /* .../neodct/tests */
        if (cut != NULL)
            *cut = '\0';
        cut = strrchr(base, '/'); /* .../neodct       */
        if (cut != NULL)
            *cut = '\0';
        (void)snprintf(cand, sizeof cand, "%.400s/" FONT_REL, base);
        if (file_exists(cand)) {
            (void)nd_strlcpy(out, cand, sz);
            return true;
        }
    }
    if (file_exists("../" FONT_REL)) {
        (void)nd_strlcpy(out, "../" FONT_REL, sz);
        return true;
    }
    if (file_exists("neodct/" FONT_REL)) {
        (void)nd_strlcpy(out, "neodct/" FONT_REL, sz);
        return true;
    }
    if (file_exists("/NeoDCT/System/ui/resources/fonts/font.ttf")) {
        (void)nd_strlcpy(out, "/NeoDCT/System/ui/resources/fonts/font.ttf", sz);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * A UI context with nothing behind it but memory
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;
    nd_font *font_s;
    nd_font *font_md;
    nd_font *font_n;
    nd_font *font_xl;
} fixture;

static fixture g_fx;

static bool fx_init(fixture *fx)
{
    char path[1024];

    memset(fx, 0, sizeof *fx);
    if (!resolve_font(path, sizeof path)) {
        fprintf(stderr, "test_widgets_text: cannot find font.ttf; set NEODCT_FONT\n");
        return false;
    }
    fx->font_s = nd_font_load(path, ND_FONT_PX_S);
    fx->font_md = nd_font_load(path, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(path, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(path, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "test_widgets_text: nd_font_load(%s) failed\n", path);
        return false;
    }

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;

    fx->ui.w = ND_UI_W;
    fx->ui.h = ND_UI_H;
    fx->ui.softkey_h = ND_SOFTKEY_H;
    fx->ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    fx->ui.canvas = fx->canvas;
    fx->ui.draw = &fx->draw;
    fx->ui.fb = NULL; /* no panel: the canvas is the frame */
    fx->ui.font_s = fx->font_s;
    fx->ui.font_md = fx->font_md;
    fx->ui.font_n = fx->font_n;
    fx->ui.font_xl = fx->font_xl;
    fx->ui.keypad_fd = -1;
    /* No matrix keypad, which is how every golden frame was captured: T9 runs
     * on the i2c keypad only, so the mode indicator is not drawn. */
    fx->ui.has_matrix_keypad = false;
    fx->ui.softkey_exists = true;
    return true;
}

static void fx_free(fixture *fx)
{
    nd_image_free(fx->canvas);
    nd_font_free(fx->font_s);
    nd_font_free(fx->font_md);
    nd_font_free(fx->font_n);
    nd_font_free(fx->font_xl);
    memset(fx, 0, sizeof *fx);
}

/* ------------------------------------------------------------------ *
 * The golden manifest
 * ------------------------------------------------------------------ */

static nd_json_doc *g_manifest;
static const nd_json_val *g_frames;

static void manifest_open(void)
{
    char dir[1024];
    char path[1200];
    uint8_t *buf = NULL;
    long len;
    FILE *f;
    char err[128];
    const nd_json_val *root;

    if (!resolve_golden(dir, sizeof dir))
        return;
    (void)snprintf(path, sizeof path, "%.1000s/manifest.json", dir);

    f = fopen(path, "rb");
    if (f == NULL)
        return;
    if (fseek(f, 0, SEEK_END) != 0)
        goto done;
    len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0)
        goto done;
    buf = malloc((size_t)len);
    if (buf == NULL)
        goto done;
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len)
        goto done;
    if (nd_json_parse(buf, (size_t)len, &g_manifest, err, sizeof err) != ND_OK) {
        fprintf(stderr, "test_widgets_text: manifest parse: %s\n", err);
        g_manifest = NULL;
        goto done;
    }
    root = nd_json_root(g_manifest);
    g_frames = nd_json_get(root, "frames");
done:
    free(buf);
    (void)fclose(f);
}

static const char *golden_sha(const char *name)
{
    size_t i;

    if (g_frames == NULL)
        return NULL;
    for (i = 0u; i < nd_json_len(g_frames); i++) {
        const nd_json_val *fr = nd_json_at(g_frames, i);

        if (strcmp(nd_json_get_str(fr, "name", ""), name) == 0)
            return nd_json_get_str(fr, "sha256", NULL);
    }
    return NULL;
}

static const char *g_frames_out;
static nd_capture *g_capture;

static void frames_out_open(void)
{
    g_frames_out = getenv("NEODCT_FRAMES_OUT");
    if (g_frames_out == NULL || g_frames_out[0] == '\0')
        return;
    /* An explicit dump must not land under the per-case scratch root, where
     * nobody could find it. */
    (void)nd_path_set_root(NULL);
    if (nd_capture_open(&g_capture, g_frames_out, 0u) != ND_OK) {
        fprintf(stderr, "test_widgets_text: cannot open %s for frames\n", g_frames_out);
        g_capture = NULL;
    }
}

static void frames_out_close(void)
{
    if (g_capture == NULL)
        return;
    (void)nd_capture_write_manifest(g_capture);
    nd_capture_close(g_capture);
    g_capture = NULL;
}

static void check_frame(const fixture *fx, const char *name)
{
    char got[65];
    const char *want = golden_sha(name);

    if (nd_capture_digest(fx->canvas, got, sizeof got) != ND_OK) {
        CHECK(false);
        return;
    }
    if (g_capture != NULL)
        (void)nd_capture_save(g_capture, name, fx->canvas);

    if (want == NULL) {
        fprintf(stderr, "test_widgets_text: no reference for %s (got %s)\n", name, got);
        CHECK(false);
        return;
    }
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL frame %s\n  got  %s\n  want %s\n", name, got, want);
    }
}

static bool px_white(const fixture *fx, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(fx->canvas, x, y);

    return c.r > 127u && c.g > 127u && c.b > 127u;
}

/* ------------------------------------------------------------------ *
 * 1. The golden frames
 * ------------------------------------------------------------------ */

/* shoot_docs.py draws the PagedList immediately before these two. Its draw()
 * clears the WHOLE screen and then paints "Select" without presenting; both
 * text widgets clear rows 0..content_bottom only, so that strip is still
 * there in both captured frames. */
static void paint_inherited_softkey(fixture *fx)
{
    nd_softkey bar;

    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    nd_softkey_init(&bar, &fx->ui, false);
    nd_softkey_update(&bar, "Select", false);
}

static void test_golden_textinput(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;

    paint_inherited_softkey(fx);

    CHECK_INT(nd_textinput_init(&t, &fx->ui, "Phonebook", "Name:", buf, sizeof buf, "Sam",
                                ND_T9_FILTER_ANY),
              ND_OK);
    CHECK_STR(t.text, "Sam");
    nd_textinput_draw(&t, true);
    check_frame(fx, "widget-textinput");
}

static void test_golden_textinputlong(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTLONG_CAP];
    nd_textlong t;

    /* Continues from the TextInput frame exactly as shoot_widgets() does; the
     * strip below row 145 is still the PagedList's. */
    CHECK_INT(nd_textlong_init(&t, &fx->ui, "Write Message", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);
    CHECK_INT(nd_textlong_set_text(&t, "Meet me by the old phone box at six"), ND_OK);
    CHECK_INT(t.cursor, 35);
    nd_textlong_draw(&t, true);
    check_frame(fx, "widget-textinputlong");
}

/* ------------------------------------------------------------------ *
 * 2. Geometry
 * ------------------------------------------------------------------ */

static void test_textinput_geometry(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;

    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    CHECK_INT(nd_textinput_init(&t, &fx->ui, "Phonebook", "Name:", buf, sizeof buf, "Sam",
                                ND_T9_FILTER_ANY),
              ND_OK);
    nd_textinput_draw(&t, true);

    /* header divider at y = max(30, int(175 * 0.11)) = 30, full width */
    CHECK(px_white(fx, 0, 30));
    CHECK(px_white(fx, 239, 30));
    CHECK(!px_white(fx, 0, 31));

    /* The box: rectangle((10, 80, 230, 120), outline) -- inclusive corners. */
    CHECK(px_white(fx, 10, 80));
    CHECK(px_white(fx, 230, 80));
    CHECK(px_white(fx, 10, 120));
    CHECK(px_white(fx, 230, 120));
    CHECK(!px_white(fx, 9, 80));
    CHECK(!px_white(fx, 231, 120));
    CHECK(!px_white(fx, 11, 81)); /* hollow */

    /* Rows below content_bottom are untouched by draw(): the clear is
     * (0, 0, w, 145) and the softkey strip starts at 145. */
    CHECK(!px_white(fx, 120, 160));
}

static void test_textlong_geometry(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTLONG_CAP];
    nd_textlong t;

    CHECK_INT(nd_textlong_init(&t, &fx->ui, "Write Message", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);
    CHECK_INT(t.text_area_top, 40);     /* header_y + 10  */
    CHECK_INT(t.text_area_bottom, 141); /* content_bottom - 4 */
    CHECK(t.font == fx->font_s);
    CHECK_STR(nd_textlong_get_text(&t), "");
    CHECK_INT(t.cursor, 0);
}

/* ------------------------------------------------------------------ *
 * 3. The pencil and the mode indicator
 * ------------------------------------------------------------------ */

/* Generated from framework.py's own _draw_pencil. Size 10 is here because
 * round(10 / 4.0) is 2 under Python's banker's rounding and 3 under C's
 * round(); a C port that used round() draws a barrel one pixel wider. */
static const char *const PENCIL_10[] = {
    ".......###", "......####", ".....#####", "....#####.", "....####..",
    "..#..##...", "..##......", ".####.....", ".##.......", "#.........",
};

static const char *const PENCIL_15[] = {
    "..........#####", ".........######", "........#######", ".......########", "......#########",
    ".....#########.", "....#########..", "....########...", "..#..######....", "..##..####.....",
    "..###..##......", ".#####.........", ".######........", ".###...........", "#..............",
};

static void check_pencil(fixture *fx, int32_t size, const char *const *ref)
{
    int32_t row;
    int32_t col;

    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    nd_draw_pencil(&fx->draw, 20, 20, size, ND_WHITE);
    for (row = 0; row < size; row++) {
        for (col = 0; col < size; col++) {
            bool want = ref[row][col] == '#';

            if (px_white(fx, 20 + col, 20 + row) != want) {
                g_failures++;
                fprintf(stderr, "FAIL pencil %d at (%d,%d)\n", size, col, row);
            }
            g_checks++;
        }
    }
}

static void test_pencil(void)
{
    check_pencil(&g_fx, 10, PENCIL_10);
    check_pencil(&g_fx, 15, PENCIL_15);
}

/* Counted from the Python at the six sizes _draw_pencil is plausibly called
 * with. 30 is the second place round() and round-half-even disagree. */
static void test_pencil_pixel_counts(void)
{
    fixture *fx = &g_fx;
    static const struct {
        int32_t size;
        int32_t n;
    } want[] = {{8, 25}, {10, 33}, {12, 55}, {15, 94}, {17, 108}, {30, 381}};
    size_t k;

    for (k = 0u; k < ND_ARRAY_LEN(want); k++) {
        int32_t n = 0;
        int32_t row;

        (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
        nd_draw_pencil(&fx->draw, 5, 5, want[k].size, ND_WHITE);
        for (row = 0; row < want[k].size; row++) {
            int32_t col;

            for (col = 0; col < want[k].size; col++) {
                if (px_white(fx, 5 + col, 5 + row))
                    n++;
            }
        }
        CHECK_INT(n, want[k].n);
    }
}

static void test_indicator_size(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;
    const char *label = NULL;
    int32_t pencil = -1;

    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);

    /* No matrix keypad: nothing is drawn and nothing is written out. */
    fx->ui.has_matrix_keypad = false;
    CHECK_INT(nd_t9ind_size(&fx->ui, &t.t9, &label, &pencil), 0);
    CHECK(label == NULL);
    CHECK_INT(pencil, -1);
    CHECK_INT(nd_t9ind_draw(&fx->ui, 228, 50, &t.t9), 0);

    fx->ui.has_matrix_keypad = true;

    /* The engine starts in "abc" -- multi-tap is what every existing field
     * expects; predictive is one # press away. */
    CHECK_INT(nd_t9_engine_mode(&t.t9), ND_T9_MODE_ABC);
    CHECK_INT(nd_t9ind_size(&fx->ui, &t.t9, &label, &pencil), 45); /* width("abc") @20 */
    CHECK_STR(label, "abc");
    CHECK_INT(pencil, 0);

    (void)nd_t9_engine_set_mode_index(&t.t9, 2u);
    CHECK_INT(nd_t9_engine_mode(&t.t9), ND_T9_MODE_UPPER);
    CHECK_INT(nd_t9ind_size(&fx->ui, &t.t9, &label, &pencil), 48);
    CHECK_STR(label, "ABC");

    (void)nd_t9_engine_set_mode_index(&t.t9, 3u);
    CHECK_INT(nd_t9ind_size(&fx->ui, &t.t9, &label, &pencil), 43);
    CHECK_STR(label, "123");

    /* Predictive: pencil + 4 + width("abc"), and the pencil is
     * max(8, int(height("abc") * 0.85)) = max(8, int(18 * 0.85)) = 15. */
    (void)nd_t9_engine_set_mode_index(&t.t9, 0u);
    CHECK_INT(nd_t9_engine_mode(&t.t9), ND_T9_MODE_WORD);
    CHECK_INT(nd_t9ind_size(&fx->ui, &t.t9, &label, &pencil), 15 + ND_T9_PENCIL_GAP + 45);
    CHECK_STR(label, "abc");
    CHECK_INT(pencil, 15);

    /* draw() returns the same width and puts the right edge where asked. */
    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    CHECK_INT(nd_t9ind_draw(&fx->ui, 228, 50, &t.t9), 64);
    /* The pencil's point is its bottom-left pixel, sitting on the text
     * baseline: x = 228 - 64 = 164, y = 50 + max(0, 18 - 15) = 53. */
    CHECK(px_white(fx, 164, 53 + 14));
    CHECK(!px_white(fx, 163, 53 + 14));

    fx->ui.has_matrix_keypad = false;
}

/* ------------------------------------------------------------------ *
 * 4. Keys -- the QWERTY dev path
 * ------------------------------------------------------------------ */

static void test_textinput_qwerty(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;

    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);

    /* "Simple capitalization logic for start of message". */
    CHECK_INT(nd_textinput_handle_key(&t, 30), ND_WIDGET_RESULT_TYPED); /* a */
    CHECK_STR(t.text, "A");
    CHECK_INT(nd_textinput_handle_key(&t, 31), ND_WIDGET_RESULT_TYPED); /* s */
    CHECK_STR(t.text, "As");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_SPACE), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "As ");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_1), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "As 1");

    /* An unmapped code is ignored with no redraw. 42 and 43 are '*' and '#'
     * on the keypad but shift and backslash on a keyboard, and the table
     * carries neither. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_STAR), ND_WIDGET_RESULT_NONE);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_HASH), ND_WIDGET_RESULT_NONE);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_UP), ND_WIDGET_RESULT_NONE);
    CHECK_STR(t.text, "As 1");

    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "As ");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_ENTER), ND_WIDGET_RESULT_CONFIRM);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_KPENTER), ND_WIDGET_RESULT_CONFIRM);

    while (t.text[0] != '\0')
        CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    /* Clear on an empty one-line field backs out of it. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_CANCEL);
}

/* A masked field is a different input method wearing the same widget. The
 * cases that matter are the ones where it must NOT behave like the field
 * around it: no letters, no multi-tap, no growing past the mask, and Clear
 * removing the separator along with the digit that pulled it in. */
static void test_textinput_mask(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TIMESET_TEXT_MAX];
    nd_textinput t;

    CHECK_INT(
        nd_textinput_init(&t, &fx->ui, "Clock", "Time:", buf, sizeof buf, "", ND_T9_FILTER_NUMBERS),
        ND_OK);
    nd_textinput_set_mask(&t, ND_TIMESET_TIME_MASK);

    /* The colon arrives with the second digit, not with the third. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_2), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "2");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_3), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "23:");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_5), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_9), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "23:59");

    /* Full: the press is ignored, and nothing already typed is lost. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_1), ND_WIDGET_RESULT_NONE);
    CHECK_STR(t.text, "23:59");

    /* A letter off the dev keyboard is not a digit. Without the mask branch
     * this same code would reach the QWERTY path and insert an 'a'. */
    CHECK_INT(nd_textinput_handle_key(&t, 30), ND_WIDGET_RESULT_NONE);
    CHECK_STR(t.text, "23:59");

    /* One press, one digit -- and the third takes the colon with it. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "23:5");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "23:");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "2");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "");

    /* Empty is the way out, as on any one-line field. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_CANCEL);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_ENTER), ND_WIDGET_RESULT_CONFIRM);

    /* Clearing the mask hands the field back to the ordinary path. Two digits
     * are the proof: masked they would read "11:", unmasked they are just
     * "11". (A letter would still be refused -- the field's NUMBERS filter is
     * a separate thing from the mask and outlives it.) */
    nd_textinput_set_mask(&t, NULL);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_1), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_1), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "11");
    CHECK_INT(nd_textinput_handle_key(&t, 30), ND_WIDGET_RESULT_NONE);
}

static void test_input_filters(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;

    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "", ND_T9_FILTER_NUMBERS),
              ND_OK);
    CHECK_INT(nd_textinput_handle_key(&t, 30), ND_WIDGET_RESULT_NONE); /* 'a' refused */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_5), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "5");

    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "", ND_T9_FILTER_LETTERS),
              ND_OK);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_5), ND_WIDGET_RESULT_NONE); /* '5' refused */
    CHECK_INT(nd_textinput_handle_key(&t, 30), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "A");
}

/* C-2: at the cap the widget ignores the key rather than truncating. */
static void test_textinput_cap(void)
{
    fixture *fx = &g_fx;
    char buf[5]; /* four characters plus the NUL */
    nd_textinput t;

    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "abcd", ND_T9_FILTER_ANY),
              ND_OK);
    CHECK_INT(nd_textinput_handle_key(&t, 31), ND_WIDGET_RESULT_NONE);
    CHECK_STR(t.text, "abcd");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "abc");
    CHECK_INT(nd_textinput_handle_key(&t, 31), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "abcs");

    /* An initial string that does not fit is refused outright. */
    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "abcde", ND_T9_FILTER_ANY),
              ND_ERR_TOOLONG);
    /* And so is a cap above the ceiling the draw path can copy. */
    {
        char big[ND_TEXTLONG_CAP + 8];
        nd_textlong tl;

        CHECK_INT(nd_textlong_init(&tl, &fx->ui, "T", big, sizeof big, "", ND_T9_FILTER_ANY),
                  ND_ERR_INVAL);
    }
}

/* ------------------------------------------------------------------ *
 * 5. Keys -- the i2c multi-tap keypad
 * ------------------------------------------------------------------ */

static void test_textinput_multitap(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;

    fx->ui.has_matrix_keypad = true;
    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);

    /* Key 2 in "abc" mode: a, then b, then c, then '2' -- FILTER_ANY appends
     * the digit to the cycle so digits stay reachable. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_2), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "a");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_2), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "b");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_2), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "c");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_2), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "2");

    /* A different key commits the pending letter and appends. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_3), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "2d");

    /* '#' walks the mode cycle: word, abc, ABC, 123, starting at abc. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_HASH), ND_WIDGET_RESULT_MODE);
    CHECK_INT(nd_t9_engine_mode(&t.t9), ND_T9_MODE_UPPER);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_4), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "2dG");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_HASH), ND_WIDGET_RESULT_MODE);
    CHECK_INT(nd_t9_engine_mode(&t.t9), ND_T9_MODE_123);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_4), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "2dG4");
    /* '*' is a literal only in 123. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_STAR), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "2dG4*");

    fx->ui.has_matrix_keypad = false;
}

/* ------------------------------------------------------------------ *
 * 6. Predictive text
 * ------------------------------------------------------------------ */

/* Written at the path nd_t9_dict_shared() resolves, so the widgets' own
 * lookup finds it. The shared handle is opened once per process, so this must
 * be in place before anything predictive runs. */
static void install_dict(void)
{
    /* Sorted by digit key then by rank, which is the order the builder emits
     * and the order the binary search assumes:
     *   good 4663, gone 4663, home 4663, hood 4663, inn 466, hi 44
     * "466" is the prefix "good"/"gone"/"home"/"hood" and the whole key of
     * "inn". */
    static const char blob[] = "hi\n"
                               "inn\n"
                               "good\n"
                               "gone\n"
                               "home\n"
                               "hood\n";

    pt_write_text(ND_PATH_T9_DICT, blob);
}

static void test_predictive(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;
    size_t n_first;

    install_dict();
    fx->ui.has_matrix_keypad = true;
    CHECK_INT(nd_textinput_init(&t, &fx->ui, "T", "P", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);
    (void)nd_t9_engine_set_mode_index(&t.t9, 0u); /* predictive */
    CHECK_INT(nd_t9_engine_mode(&t.t9), ND_T9_MODE_WORD);

    /* One digit is below ND_T9_MIN_PREFIX, so the dictionary declines and the
     * field shows the digits -- the keypresses stay visible. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_4), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(t.text, "4");
    CHECK_INT(t.predict.pending_len, 1);
    CHECK_INT(t.predict.n_candidates, 0);

    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_6), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(nd_t9_engine_word_digits(&t.t9), "46");
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_6), ND_WIDGET_RESULT_TYPED);
    CHECK_STR(nd_t9_engine_word_digits(&t.t9), "466");
    n_first = t.predict.n_candidates;
    CHECK(n_first >= 2u);
    CHECK_STR(t.text, t.predict.candidates[0]);
    CHECK_INT(t.predict.pending_len, strlen(t.predict.candidates[0]));

    /* '*' shows the next word these digits could spell, and does NOT reset
     * the engine -- the digits typed so far ARE the word. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_STAR), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(t.predict.candidate_idx, 1);
    CHECK_STR(t.text, t.predict.candidates[1]);
    CHECK_STR(nd_t9_engine_word_digits(&t.t9), "466");

    /* Clear takes a typed DIGIT off, not a guessed letter. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(nd_t9_engine_word_digits(&t.t9), "46");
    CHECK_INT(t.predict.pending_len, strlen(t.text));

    /* Emptying the digit string clears the pending word outright. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "");
    CHECK_INT(t.predict.pending_len, 0);
    CHECK_STR(nd_t9_engine_word_digits(&t.t9), "");

    /* 0 and 1 carry no letters, so they END the word: the engine drops the
     * digits and the ordinary space/punctuation cycle produces the character. */
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_4), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_6), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_6), ND_WIDGET_RESULT_TYPED);
    CHECK(t.predict.pending_len > 0u);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_0), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(t.predict.pending_len, 0); /* the word was committed */
    CHECK_STR(nd_t9_engine_word_digits(&t.t9), "");

    /* '*' with nothing typed is not a predictive op and not a literal either. */
    nd_predictive_reset(&t.predict, &t.t9);
    CHECK_INT(nd_textinput_handle_key(&t, ND_KEY_STAR), ND_WIDGET_RESULT_NONE);

    fx->ui.has_matrix_keypad = false;
}

static void test_predictive_underline_survives_wrap(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTLONG_CAP];
    nd_textlong t;

    install_dict();
    fx->ui.has_matrix_keypad = true;
    CHECK_INT(nd_textlong_init(&t, &fx->ui, "Write", buf, sizeof buf, "", ND_T9_FILTER_ANY), ND_OK);
    (void)nd_t9_engine_set_mode_index(&t.t9, 0u);

    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_4), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_6), ND_WIDGET_RESULT_TYPED);
    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_6), ND_WIDGET_RESULT_TYPED);
    CHECK(t.predict.pending_len > 0u);
    /* The cursor follows the provisional word. */
    CHECK_INT(t.cursor, strlen(t.text));

    /* Drawing must not fall over with the underline on the last line. */
    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    nd_textlong_draw(&t, true);
    /* The rule sits one pixel under the ink of the first text line, which
     * starts at text_area_top = 40 and is 15 px of "Ag" ink tall at most. */
    {
        int32_t y;
        bool any = false;

        for (y = 40; y < 60; y++) {
            if (px_white(fx, 11, y))
                any = true;
        }
        CHECK(any);
    }

    fx->ui.has_matrix_keypad = false;
}

/* ------------------------------------------------------------------ *
 * 7. TextInputLong editing
 * ------------------------------------------------------------------ */

static int g_empty_backspaces;

static void on_empty(void *ctx)
{
    ND_UNUSED(ctx);
    g_empty_backspaces++;
}

static void test_textlong_editing(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTLONG_CAP];
    nd_textlong t;

    CHECK_INT(nd_textlong_init(&t, &fx->ui, "Write", buf, sizeof buf, "Hi", ND_T9_FILTER_ANY),
              ND_OK);
    CHECK_STR(nd_textlong_get_text(&t), "Hi");
    CHECK_INT(t.cursor, 2);

    g_empty_backspaces = 0;
    nd_textlong_set_on_empty_backspace(&t, on_empty, NULL);

    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "H");
    CHECK_INT(t.cursor, 1);
    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
    CHECK_STR(t.text, "");
    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_EMPTY_BACKSPACE);
    CHECK_INT(g_empty_backspaces, 1);

    /* There is no confirm key here: the composing loop lives in Messages. */
    CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_ENTER), ND_WIDGET_RESULT_NONE);

    CHECK_INT(nd_textlong_handle_key(&t, 34), ND_WIDGET_RESULT_TYPED); /* g -> G */
    CHECK_STR(t.text, "G");
    CHECK_INT(nd_textlong_handle_key(&t, 24), ND_WIDGET_RESULT_TYPED); /* o */
    CHECK_STR(t.text, "Go");
    CHECK_INT(t.cursor, 2);

    CHECK_INT(nd_textlong_set_text(&t, "Reset"), ND_OK);
    CHECK_INT(t.cursor, 5);
    nd_textlong_clear_text(&t);
    CHECK_STR(t.text, "");
    CHECK_INT(t.cursor, 0);
}

/* ------------------------------------------------------------------ *
 * 8. The incremental rewrap -- proved by difference
 * ------------------------------------------------------------------ */

static void digest_of(const fixture *fx, char out[65])
{
    if (nd_capture_digest(fx->canvas, out, sizeof(char[65])) != ND_OK)
        out[0] = '\0';
}

/* Render `text` with the watermark forced to zero, i.e. the Python's
 * whole-string rewrap, and return the digest. */
static void render_full(fixture *fx, const char *text, bool blink, char out[65])
{
    char buf[ND_TEXTLONG_CAP];
    nd_textlong t;

    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    CHECK_INT(nd_textlong_init(&t, fx == NULL ? NULL : &fx->ui, "Write Message", buf, sizeof buf,
                               text, ND_T9_FILTER_ANY),
              ND_OK);
    t.wrap_clean_off = 0u;
    t.wrap_clean_lines = 0u;
    nd_textlong_draw(&t, blink);
    /* One draw from a cold widget is exactly the Python's behaviour, and the
     * watermark cannot have helped it. */
    digest_of(fx, out);
}

static void test_incremental_rewrap_matches_full(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTLONG_CAP];
    nd_textlong t;
    char want[65];
    char got[65];
    /* 26 letters, no space, so the wrapper hard-breaks it -- the case the
     * watermark is not allowed to restart inside. */
    static const char *const corpus[] = {
        "",
        "Hi",
        "Meet me by the old phone box at six",
        "one two three four five six seven eight nine ten eleven twelve "
        "thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnop "
        "then some ordinary words after the very long one",
        "line one\nline two\n\nline four\nline five\nline six\nline seven\n"
        "line eight\nline nine\nline ten\nline eleven\nline twelve",
        "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\nu\nv\nw\n"
        "x\ny\nz\na\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt",
        "  leading spaces and  a  double  space inside a fairly long line that "
        "has to wrap more than once on a two hundred and twenty pixel column",
    };
    size_t k;

    for (k = 0u; k < ND_ARRAY_LEN(corpus); k++) {
        bool blink;

        for (blink = false;; blink = true) {
            render_full(fx, corpus[k], blink, want);

            /* Same text, but reached through set_text on a widget that has
             * already drawn -- so the watermark is live. */
            (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
            CHECK_INT(nd_textlong_init(&t, &fx->ui, "Write Message", buf, sizeof buf, corpus[k],
                                       ND_T9_FILTER_ANY),
                      ND_OK);
            nd_textlong_draw(&t, blink); /* establishes the watermark */
            (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
            nd_textlong_draw(&t, blink); /* and now uses it */
            digest_of(fx, got);
            if (strcmp(got, want) != 0) {
                g_failures++;
                fprintf(stderr, "FAIL rewrap[%zu] blink=%d\n  got  %s\n  want %s\n", k, (int)blink,
                        got, want);
            }
            g_checks++;

            if (blink)
                break;
        }
    }
}

static void test_incremental_rewrap_while_typing(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTLONG_CAP];
    char plain[ND_TEXTLONG_CAP];
    nd_textlong t;
    char want[65];
    char got[65];
    /* evdev codes for "the quick brown fox jumps over the lazy dog " */
    static const int32_t script[] = {
        20, 35, 18, 57, 16, 22, 23, 46, 37, 57, 48, 19, 24, 49, 25, 57, 33, 24, 45, 57, 36, 22,
        50, 25, 31, 57, 24, 47, 18, 19, 57, 20, 35, 18, 57, 38, 30, 44, 21, 57, 32, 24, 34, 57,
    };
    size_t round;
    size_t i;

    CHECK_INT(nd_textlong_init(&t, &fx->ui, "Write Message", buf, sizeof buf, "", ND_T9_FILTER_ANY),
              ND_OK);

    /* Type the pangram six times over -- about 264 characters, which is
     * comfortably more than the watermark's ten-line window, so it advances
     * several times. Every single keypress is checked against a cold render. */
    for (round = 0u; round < 6u; round++) {
        for (i = 0u; i < ND_ARRAY_LEN(script); i++) {
            CHECK_INT(nd_textlong_handle_key(&t, script[i]), ND_WIDGET_RESULT_TYPED);

            (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
            nd_textlong_draw(&t, true);
            digest_of(fx, got);

            (void)nd_strlcpy(plain, t.text, sizeof plain);
            render_full(fx, plain, true, want);

            if (strcmp(got, want) != 0) {
                g_failures++;
                fprintf(stderr, "FAIL typing rewrap at %zu chars\n  got  %s\n  want %s\n",
                        strlen(plain), got, want);
                return;
            }
            g_checks++;
        }
    }
    CHECK(t.wrap_clean_off > 0u); /* it really did move */
    CHECK(t.wrap_clean_lines > 0u);

    /* And backspacing all the way out stays consistent too. */
    while (t.text[0] != '\0') {
        CHECK_INT(nd_textlong_handle_key(&t, ND_KEY_CLEAR), ND_WIDGET_RESULT_BACKSPACE);
        (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
        nd_textlong_draw(&t, false);
        digest_of(fx, got);

        (void)nd_strlcpy(plain, t.text, sizeof plain);
        render_full(fx, plain, false, want);
        if (strcmp(got, want) != 0) {
            g_failures++;
            fprintf(stderr, "FAIL backspace rewrap at %zu chars\n", strlen(plain));
            return;
        }
        g_checks++;
    }
}

/* ------------------------------------------------------------------ *
 * 9. The blocking show() loop, end to end over a real nd_input on a pipe
 * ------------------------------------------------------------------ */

#define EV_KEY_T 1u
#define EV_SYN_T 0u

static void write_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct {
        int64_t tv_sec;
        int64_t tv_usec;
        uint16_t type;
        uint16_t code;
        int32_t value;
    } ev;

    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    CHECK_INT(write(fd, &ev, sizeof ev), (int)sizeof ev);
}

static void script_key(int fd, int32_t code)
{
    write_event(fd, EV_KEY_T, (uint16_t)code, 1);
    write_event(fd, EV_SYN_T, 0u, 0);
    write_event(fd, EV_KEY_T, (uint16_t)code, 0);
    write_event(fd, EV_SYN_T, 0u, 0);
}

/* Every key is written before show() is called, so the loop never blocks: it
 * reads the script, redraws, and leaves through the key that ends it. */
static void test_textinput_show(void)
{
    fixture *fx = &g_fx;
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput t;
    nd_input *in = NULL;
    int fds[2];
    const char *got;

    CHECK_INT(pipe(fds), 0);
    if (nd_input_open_fd(&in, fds[0]) != ND_OK) {
        CHECK(false);
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }
    /* A scripted press is released immediately; nothing may repeat. */
    nd_input_set_repeat(in, 0.0, 0.0);
    fx->ui.input = in;

    CHECK_INT(
        nd_textinput_init(&t, &fx->ui, "Phonebook", "Name:", buf, sizeof buf, "", ND_T9_FILTER_ANY),
        ND_OK);
    script_key(fds[1], 30);           /* a -> "A" */
    script_key(fds[1], 31);           /* s -> "As" */
    script_key(fds[1], ND_KEY_UP);    /* ignored, no redraw */
    script_key(fds[1], ND_KEY_ENTER); /* confirm */
    got = nd_textinput_show(&t);
    CHECK(got == t.text);
    CHECK_STR(got, "As");
    /* show() paints its own "OK" bar before the first draw, and TextInput's
     * clear stops at content_bottom, so it is still on the canvas. */
    {
        int32_t y;
        bool any = false;

        for (y = 146; y < 175; y++) {
            if (px_white(fx, 120, y))
                any = true;
        }
        CHECK(any);
    }

    /* Clear on an already-empty field cancels, and show() returns NULL. */
    CHECK_INT(nd_textinput_init(&t, &fx->ui, "Phonebook", "Name:", buf, sizeof buf, "x",
                                ND_T9_FILTER_ANY),
              ND_OK);
    script_key(fds[1], ND_KEY_CLEAR); /* deletes the "x" */
    script_key(fds[1], ND_KEY_CLEAR); /* empty -> cancel */
    CHECK(nd_textinput_show(&t) == NULL);
    CHECK_STR(t.text, "");

    fx->ui.input = NULL;
    (void)close(fds[1]);
    nd_input_close(in); /* closes fds[0] */
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    pt_new_case();
    if (!fx_init(&g_fx)) {
        fx_free(&g_fx);
        return 1;
    }
    manifest_open();
    frames_out_open();

    /* The two golden frames run back to back and in shoot_widgets() order:
     * the second inherits the softkey strip the first inherited. */
    test_golden_textinput();
    test_golden_textinputlong();

    test_textinput_geometry();
    test_textlong_geometry();
    test_pencil();
    test_pencil_pixel_counts();
    test_indicator_size();
    test_textinput_qwerty();
    test_textinput_mask();
    test_input_filters();
    test_textinput_cap();
    test_textinput_multitap();
    test_predictive();
    test_predictive_underline_survives_wrap();
    test_textlong_editing();
    test_incremental_rewrap_matches_full();
    test_incremental_rewrap_while_typing();
    test_textinput_show();

    frames_out_close();
    fx_free(&g_fx);
    if (g_manifest != NULL)
        nd_json_free(g_manifest);
    pt_cleanup();

    if (g_failures != 0) {
        fprintf(stderr, "test_widgets_text: %d checks, %d FAILURES\n", g_checks, g_failures);
        return 1;
    }
    printf("test_widgets_text: %d checks passed\n", g_checks);
    return 0;
}
