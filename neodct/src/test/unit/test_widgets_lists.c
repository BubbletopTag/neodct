/* test_widgets_lists.c -- SoftKeyBar, HeaderWidget, VerticalList, PagedList
 * and LevelSelector, against the golden frames and against the Python's own
 * arithmetic.
 *
 * Four kinds of check, and all four are needed:
 *
 *  1. FIVE GOLDEN FRAMES. The five widget screens in neodct/tests/golden that
 *     this work package can reach are re-rendered here and their SHA-256 over
 *     raw RGB is compared with the reference manifest -- the same digest
 *     goldenframe.py compares, so a pass here means a pass there. The draw
 *     sequence mirrors shoot_docs.py's shoot_widgets() exactly, INCLUDING the
 *     softkey text each frame inherited from the widget drawn before it: the
 *     LevelSelector frame carries TextScroller's "More" because a
 *     VerticalList's draw() clears only rows 0..145.
 *
 *  2. GEOMETRY AND STATE, checked as numbers rather than as pixels, so a
 *     failure says which value moved instead of "one frame differs". The
 *     scrollbar notch truncation, the 223 px centring quirk, the window
 *     clamp and the digit shortcuts are all here.
 *
 *  3. THE BLOCKING show() LOOPS, end to end, over a real nd_input on a pipe.
 *     VerticalList takes a script written in advance; PagedList cannot,
 *     because the first thing its show() does is drain the channel -- so its
 *     terminating key is delivered as a REPEAT of a key still held, which
 *     also proves the flush happened.
 *
 *  4. HOLD-TO-REPEAT reaching the list, driven the same way. A single held
 *     Down, with nothing further written, must advance the selection once per
 *     synthesised repeat.
 *
 * Set NEODCT_FRAMES_OUT to a directory to also write the five PNGs and a
 * manifest.json there, which is what
 *   python3 neodct/tools/goldenframe.py --compare neodct/tests/golden <dir>
 * consumes.
 */

#include <fcntl.h>
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
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Finding the font and the reference set
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

/* $NEODCT_GOLDEN when make test set it, otherwise the two places the tree is
 * ever run from. The acceptance gate supplies neither. */
static bool resolve_golden(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_GOLDEN");
    char cand[1024];

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    (void)snprintf(cand, sizeof cand, "../tests/golden");
    if (file_exists("../tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, cand, sz);
        return true;
    }
    if (file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "neodct/tests/golden", sz);
        return true;
    }
    return false;
}

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen and
 * not through nd_path_resolve(). Same search test_draw.c performs. */
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

static bool fx_init(fixture *fx)
{
    char path[1024];

    memset(fx, 0, sizeof *fx);
    if (!resolve_font(path, sizeof path)) {
        fprintf(stderr, "test_widgets_lists: cannot find font.ttf; set NEODCT_FONT\n");
        return false;
    }
    fx->font_s = nd_font_load(path, ND_FONT_PX_S);
    fx->font_md = nd_font_load(path, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(path, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(path, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "test_widgets_lists: nd_font_load(%s) failed\n", path);
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
    /* Every bar a widget builds for itself is opaque; only the core's own bar
     * is transparent, and this context is not the core. */
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

    /* Plain fopen: the reference set is not under NEODCT_ROOT, so
     * nd_json_parse_file() -- which resolves -- would look in the wrong place. */
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
        fprintf(stderr, "test_widgets_lists: manifest parse: %s\n", err);
        g_manifest = NULL;
        goto done;
    }
    root = nd_json_root(g_manifest);
    g_frames = nd_json_get(root, "frames");
    CHECK_STR(nd_json_get_str(root, "text_layout", ""), "BASIC");
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
        const char *n = nd_json_get_str(fr, "name", "");

        if (strcmp(n, name) == 0)
            return nd_json_get_str(fr, "sha256", NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Frame comparison
 * ------------------------------------------------------------------ */

static const char *g_frames_out;
static nd_capture *g_capture;

static void frames_out_open(void)
{
    g_frames_out = getenv("NEODCT_FRAMES_OUT");
    if (g_frames_out == NULL || g_frames_out[0] == '\0')
        return;
    /* nd_capture_open() resolves through ND_ROOT like every other path this
     * library opens, and the per-case scratch root would hide the output where
     * nobody could find it. An explicit dump is the one place that is wrong. */
    (void)nd_path_set_root(NULL);
    if (nd_capture_open(&g_capture, g_frames_out, 0u) != ND_OK) {
        fprintf(stderr, "test_widgets_lists: cannot open %s for frames\n", g_frames_out);
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
        fprintf(stderr, "test_widgets_lists: no reference for %s (got %s)\n", name, got);
        CHECK(false);
        return;
    }
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL frame %s\n  got  %s\n  want %s\n", name, got, want);
    }
}

/* ------------------------------------------------------------------ *
 * 1. The golden frames
 * ------------------------------------------------------------------ */

/* shoot_docs.py: VerticalList(ui, "Phonebook", [...], app_id=1). */
static const char *const PHONEBOOK[] = {"Search", "Add entry",  "Edit",
                                        "Erase",  "Send entry", "Options"};

/* shoot_docs.py: PagedList(ui, "Messages", [...], root_id=2). */
static const char *const MESSAGES[] = {"Inbox", "Outbox", "Write Message"};

static void test_golden_verticallist(fixture *fx)
{
    nd_vlist list;
    nd_softkey bar;

    nd_vlist_init(&list, &fx->ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
    nd_softkey_init(&bar, &fx->ui, false);

    /* The caller paints "Select" WITHOUT presenting and the list's draw()
     * clears only rows 0..145, so the strip survives into the frame. */
    nd_softkey_update(&bar, "Select", false);
    nd_vlist_draw(&list);
    check_frame(fx, "widget-verticallist");

    /* Same object, selection moved to row 2. The window does not scroll --
     * three rows fit and index 2 is the third of them -- but the breadcrumb
     * becomes "1-3" and the notch moves down two steps. */
    list.selected_index = 2u;
    nd_softkey_update(&bar, "Select", false);
    nd_vlist_draw(&list);
    check_frame(fx, "widget-verticallist-scrolled");

    CHECK_INT(list.window_start, 0);
    CHECK_INT(list.max_lines, 3);
}

static void test_golden_pagedlist(fixture *fx)
{
    nd_pagedlist p;

    nd_pagedlist_init(&p, &fx->ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", true);
    CHECK_INT(p.content_top, 38);
    CHECK_INT(p.content_bottom, 135);
    CHECK_INT(p.bar_x, 235);

    nd_pagedlist_draw(&p);
    check_frame(fx, "widget-pagedlist");
}

static void test_golden_levelselector(fixture *fx)
{
    nd_levelsel sel;
    nd_softkey bar;

    /* In shoot_docs.py the LevelSelector is drawn straight after the
     * TextScroller, whose last act is SoftKeyBar.update("More"). The list's
     * draw() does not touch the softkey strip, so "More" is still there in the
     * reference frame. Reproduce the inherited strip, not an "OK". */
    nd_softkey_init(&bar, &fx->ui, false);
    nd_softkey_update(&bar, "More", false);

    nd_levelsel_init(&sel, &fx->ui, 3, 9, "Level", 6);
    CHECK_INT(sel.count, 9);
    CHECK_INT(sel.list.selected_index, 2);
    CHECK_STR(sel.list.items[0], "Level 1");
    CHECK_STR(sel.list.items[8], "Level 9");

    nd_vlist_draw(&sel.list);
    check_frame(fx, "widget-levelselector");
}

static void test_golden_softkeybar(fixture *fx)
{
    nd_header h;
    nd_softkey bar;

    /* shoot_docs.py composes this one by hand: a black screen, a title, a
     * "3-2" breadcrumb, the divider, then SoftKeyBar(ui).update("Options"). */
    nd_header_init_int(&h, &fx->ui, 3);
    (void)nd_draw_rect_fill(&fx->draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_BLACK);
    (void)nd_draw_text(&fx->draw, 5, 0, "Call log", fx->ui.font_xl, ND_WHITE);
    nd_header_draw(&h, 2);
    (void)nd_draw_line(&fx->draw, 0, 30, ND_UI_W, 30, ND_WHITE, 1);

    nd_softkey_init(&bar, &fx->ui, false);
    nd_softkey_update(&bar, "Options", true);
    check_frame(fx, "widget-softkeybar");
}

static void test_golden_frames(void)
{
    fixture fx;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    frames_out_open();

    test_golden_verticallist(&fx);
    test_golden_pagedlist(&fx);
    test_golden_levelselector(&fx);
    test_golden_softkeybar(&fx);

    frames_out_close();
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2a. Geometry
 * ------------------------------------------------------------------ */

static void test_derived_geometry(void)
{
    nd_ui ui;

    memset(&ui, 0, sizeof ui);
    ui.w = 240;
    ui.h = 175;
    ui.softkey_h = 30;
    CHECK_INT(nd_ui_width(&ui), 240);
    CHECK_INT(nd_ui_height(&ui), 175);
    CHECK_INT(nd_ui_softkey_height(&ui), 30);
    CHECK_INT(nd_ui_content_bottom(&ui), 145);
    /* max(30, int(175 * 0.11)) == max(30, 19) -- the FLOOR wins here, which is
     * why nobody may hard-code the 30. */
    CHECK_INT(nd_ui_header_divider_y(&ui), 30);

    /* A taller panel is where the formula stops agreeing with the constant. */
    ui.h = 400;
    CHECK_INT(nd_ui_header_divider_y(&ui), 44); /* int(44.0) */
    ui.h = 320;
    CHECK_INT(nd_ui_header_divider_y(&ui), 35); /* int(35.2) truncates */

    /* A zero field is C's spelling of the Python's missing attribute. */
    memset(&ui, 0, sizeof ui);
    CHECK_INT(nd_ui_width(&ui), 240);
    CHECK_INT(nd_ui_height(&ui), 175);
    CHECK_INT(nd_ui_softkey_height(&ui), 30);
    CHECK_INT(nd_ui_content_bottom(&ui), 145);
    CHECK_INT(nd_ui_width(NULL), 240);
}

/* ------------------------------------------------------------------ *
 * 2b. HeaderWidget
 * ------------------------------------------------------------------ */

static void test_header(void)
{
    fixture fx;
    nd_header h;
    char buf[32];
    int32_t w = 0;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_header_init_int(&h, &fx.ui, 1);
    nd_header_text_for(&h, 4, buf, sizeof buf);
    CHECK_STR(buf, "1-4");
    nd_header_text_for(&h, -1, buf, sizeof buf);
    CHECK_STR(buf, "1");
    nd_header_text_for(&h, 0, buf, sizeof buf);
    CHECK_STR(buf, "1-0");

    /* A compound root id is a string that was never a number. */
    nd_header_init(&h, &fx.ui, "5-5");
    nd_header_text_for(&h, 2, buf, sizeof buf);
    CHECK_STR(buf, "5-5-2");
    nd_header_text_for(&h, -1, buf, sizeof buf);
    CHECK_STR(buf, "5-5");

    /* width() is the ink width PLUS the 5 px right margin, and callers
     * subtract the whole number. */
    nd_header_init_int(&h, &fx.ui, 99);
    nd_text_size(fx.ui.font_n, "99-1", &w, NULL);
    CHECK_INT(nd_header_width(&h, 1), 5 + w);
    nd_text_size(fx.ui.font_n, "99", &w, NULL);
    CHECK_INT(nd_header_width(&h, -1), 5 + w);

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2c. SoftKeyBar
 * ------------------------------------------------------------------ */

static bool row_is_black(const nd_image *img, int32_t y)
{
    int32_t x;

    for (x = 0; x < img->w; x++) {
        nd_color c = nd_image_get_px(img, x, y);

        if (c.r != 0u || c.g != 0u || c.b != 0u)
            return false;
    }
    return true;
}

static bool strip_has_ink(const nd_image *img)
{
    int32_t x;
    int32_t y;

    for (y = 145; y < img->h; y++) {
        for (x = 0; x < img->w; x++) {
            nd_color c = nd_image_get_px(img, x, y);

            if (c.r != 0u || c.g != 0u || c.b != 0u)
                return true;
        }
    }
    return false;
}

static void test_softkey(void)
{
    fixture fx;
    nd_softkey bar;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_softkey_init(&bar, &fx.ui, false);
    CHECK_INT(bar.height, 30);
    CHECK_INT(bar.y_start, 145);
    CHECK(!bar.transparent);
    CHECK(!bar.has_text);

    /* A label lights the strip and nothing above it. */
    (void)nd_image_fill(fx.canvas, ND_WHITE);
    nd_softkey_update(&bar, "Select", false);
    CHECK(strip_has_ink(fx.canvas));
    CHECK(row_is_black(fx.canvas, 145));
    CHECK(row_is_black(fx.canvas, 174));
    CHECK_STR(bar.current_text, "Select");
    CHECK(bar.has_text);
    /* Row 144 is ABOVE the strip and must be untouched -- that is the whole
     * reason a caller can paint the softkey before drawing a list. */
    CHECK_INT(nd_image_get_px(fx.canvas, 0, 144).r, 255);

    /* Both "" and NULL clear the strip and draw nothing. ProgressScreen and
     * PagedList's empty state depend on it, so this is not an error case. */
    nd_softkey_update(&bar, "", false);
    CHECK(!strip_has_ink(fx.canvas));
    CHECK_STR(bar.current_text, "");
    CHECK(bar.has_text); /* "" is not None */

    nd_softkey_update(&bar, "Options", false);
    CHECK(strip_has_ink(fx.canvas));
    nd_softkey_update(&bar, NULL, false);
    CHECK(!strip_has_ink(fx.canvas));
    CHECK(!bar.has_text);

    /* Transparent with no wallpaper falls back to black -- which is what the
     * home screen looks like before one is chosen. */
    nd_softkey_init(&bar, &fx.ui, true);
    CHECK(bar.transparent);
    (void)nd_image_fill(fx.canvas, ND_WHITE);
    nd_softkey_update(&bar, NULL, false);
    CHECK(row_is_black(fx.canvas, 150));

    /* Transparent WITH a wallpaper pastes the wallpaper's own rows 145..174
     * back, so the strip shows the picture rather than a black band. */
    {
        nd_image *wall = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(10, 20, 30));

        CHECK(wall != NULL);
        if (wall != NULL) {
            nd_color c;

            (void)nd_image_fill_rect(wall, ND_RECT(0, 145, 239, 174), ND_RGB(7, 8, 9));
            nd_ui_set_wallpaper(&fx.ui, wall);
            (void)nd_image_fill(fx.canvas, ND_WHITE);
            nd_softkey_update(&bar, NULL, false);
            c = nd_image_get_px(fx.canvas, 120, 160);
            CHECK_INT(c.r, 7);
            CHECK_INT(c.g, 8);
            CHECK_INT(c.b, 9);
            /* The row above the strip is still the caller's pixels. */
            CHECK_INT(nd_image_get_px(fx.canvas, 120, 144).r, 255);
            /* The last row of the panel is copied, not left behind. */
            CHECK_INT(nd_image_get_px(fx.canvas, 0, 174).b, 9);
            nd_ui_set_wallpaper(&fx.ui, NULL); /* frees wall */
        }
    }

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2d. VerticalList
 * ------------------------------------------------------------------ */

/* Where the white selection bar is, as a row range, so the test talks about
 * the same thing the spec table does. */
static bool row_is_selection_bar(const nd_image *img, int32_t y)
{
    /* x 0..225 inclusive white, 226.. black (the scrollbar column aside). */
    return nd_image_get_px(img, 0, y).r == 255u && nd_image_get_px(img, 225, y).r == 255u &&
           nd_image_get_px(img, 226, y).r == 0u;
}

static void test_vlist_layout(void)
{
    fixture fx;
    nd_vlist list;
    int32_t y;
    int32_t lit;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
    nd_vlist_draw(&list);

    /* Rows at 40, 73, 106 with a 29 px bar: 40..69, 73..102, 106..135. */
    CHECK(row_is_selection_bar(fx.canvas, 40));
    CHECK(row_is_selection_bar(fx.canvas, 69));
    CHECK(!row_is_selection_bar(fx.canvas, 70));
    CHECK(!row_is_selection_bar(fx.canvas, 39));

    list.selected_index = 1u;
    nd_vlist_draw(&list);
    CHECK(row_is_selection_bar(fx.canvas, 73));
    CHECK(row_is_selection_bar(fx.canvas, 102));
    CHECK(!row_is_selection_bar(fx.canvas, 103));

    list.selected_index = 2u;
    nd_vlist_draw(&list);
    CHECK(row_is_selection_bar(fx.canvas, 106));
    CHECK(row_is_selection_bar(fx.canvas, 135));
    CHECK(!row_is_selection_bar(fx.canvas, 136));

    /* The divider is row 30 and it is the only row lit at both ends. */
    CHECK_INT(nd_image_get_px(fx.canvas, 0, 30).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 239, 30).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 0, 29).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 239, 31).r, 0);

    /* The scrollbar track is GREY and ONE column wide -- the only grey in the
     * framework, and the one track that is not width 2. */
    list.selected_index = 5u;
    nd_vlist_draw(&list);
    CHECK_INT(nd_image_get_px(fx.canvas, 235, 50).r, 128);
    CHECK_INT(nd_image_get_px(fx.canvas, 235, 50).g, 128);
    CHECK_INT(nd_image_get_px(fx.canvas, 235, 50).b, 128);
    CHECK_INT(nd_image_get_px(fx.canvas, 236, 50).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 234, 50).r, 0);

    /* The notch is x 233..237. At selected 0 with 6 items the step is
     * (140-40)/5 = 20 exactly, so it sits at y 37..43. */
    list.selected_index = 0u;
    nd_vlist_draw(&list);
    lit = 0;
    /* From 31, not 30: the divider spans the full width and lights this
     * column too, and it is not part of the notch. */
    for (y = 31; y < 145; y++) {
        if (nd_image_get_px(fx.canvas, 237, y).r == 255u)
            lit++;
    }
    CHECK_INT(lit, 7);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 37).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 237, 43).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 237, 44).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 232, 40).r, 0);

    /* And at the bottom of the list, 40 + 5*20 = 140 -> 137..143. */
    list.selected_index = 5u;
    nd_vlist_draw(&list);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 137).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 237, 143).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 237, 136).r, 0);

    fx_free(&fx);
}

/* A caller that PRESELECTS a row past the first windowful gets it drawn.
 *
 * draw() has always pulled window_start DOWN to meet a selected_index above
 * it, so opening a list on row 0 or 1 worked and nobody noticed the other
 * direction was missing: with six items and selected_index 5, window_start
 * stayed 0, the window showed rows 0..2, and the frame came back with NO
 * selection bar anywhere -- a menu that looks like it has lost its place.
 *
 * The key loop maintains this invariant itself, which is why the widget went
 * this long without it. It only bites the callers who set selected_index by
 * hand, and until Sleepy's CPU menu -- which opens on whichever frequency is
 * already pinned, and on this chip that is often the last row -- every one of
 * them happened to preselect a row inside the first window. */
static void test_vlist_preselected_row_scrolls_into_view(void)
{
    fixture fx;
    nd_vlist list;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    /* Six items, three visible: the last row can only be drawn in the bottom
     * slot, with the window scrolled to start at item 3. */
    nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
    list.selected_index = 5u;
    nd_vlist_draw(&list);

    CHECK_INT(list.window_start, 3);
    CHECK(row_is_selection_bar(fx.canvas, 106));
    CHECK(row_is_selection_bar(fx.canvas, 135));
    CHECK(!row_is_selection_bar(fx.canvas, 40));

    /* And back the other way, which already worked -- kept so a fix to one
     * direction cannot quietly break the other. */
    list.selected_index = 0u;
    nd_vlist_draw(&list);

    CHECK_INT(list.window_start, 0);
    CHECK(row_is_selection_bar(fx.canvas, 40));

    fx_free(&fx);
}

/* The notch position is a float that Pillow TRUNCATES. With 3 items the step
 * is 50.0 and nothing rounds; with 7 it is 16.666..., and index 1 lands at
 * 56.66 -> the box is 53.66..59.66 -> rows 53..59. Rounding instead would give
 * 54..60 and move the notch a pixel. */
static void test_vlist_notch_truncates(void)
{
    fixture fx;
    nd_vlist list;
    static const char *const SEVEN[] = {"a", "b", "c", "d", "e", "f", "g"};

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_vlist_init(&list, &fx.ui, "T", SEVEN, ND_ARRAY_LEN(SEVEN), 9);
    list.selected_index = 1u;
    nd_vlist_draw(&list);
    CHECK_INT(nd_image_get_px(fx.canvas, 235, 52).r, 128); /* track, not notch */
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 53).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 59).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 60).r, 0);

    /* A one-item list has no step at all and pins the notch to the track top. */
    nd_vlist_init(&list, &fx.ui, "T", SEVEN, 1u, 9);
    nd_vlist_draw(&list);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 37).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 43).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 233, 44).r, 0);

    fx_free(&fx);
}

static void test_vlist_keys(void)
{
    fixture fx;
    nd_vlist list;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
    nd_vlist_draw(&list); /* sets max_lines to 3 */

    /* Down walks the selection and drags the window only once the selection
     * leaves it. */
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_DOWN), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 1);
    CHECK_INT(list.window_start, 0);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_DOWN), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 2);
    CHECK_INT(list.window_start, 0);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_DOWN), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 3);
    CHECK_INT(list.window_start, 1);

    /* The end of the list is a hard stop -- it does NOT wrap, unlike
     * PagedList and AppSelector. */
    while (nd_vlist_handle_key(&list, ND_KEY_DOWN) == ND_VLIST_CONTINUE &&
           list.selected_index < 5u) {}
    CHECK_INT(list.selected_index, 5);
    CHECK_INT(list.window_start, 3);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_DOWN), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 5);
    CHECK_INT(list.window_start, 3);

    /* Up, back to the top, dragging the window with it. */
    while (list.selected_index > 0u)
        CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_UP), ND_VLIST_CONTINUE);
    CHECK_INT(list.window_start, 0);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_UP), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 0);

    /* Enter and Clear. */
    list.selected_index = 4u;
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_ENTER), 4);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_CLEAR), ND_WIDGET_BACK);

    /* Digit shortcuts are 1..9, i.e. codes 2..10. Code 11 is '0' and is not
     * one, and a shortcut past the end of the list is ignored outright. */
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_1), 0);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_6), 5);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_7), ND_VLIST_CONTINUE);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_0), ND_VLIST_CONTINUE);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_STAR), ND_VLIST_CONTINUE);

    /* An empty list cannot move and cannot be modulo'd by zero. */
    nd_vlist_init(&list, &fx.ui, "Nothing", PHONEBOOK, 0u, 1);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_DOWN), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 0);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_UP), ND_VLIST_CONTINUE);
    CHECK_INT(list.selected_index, 0);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_1), ND_VLIST_CONTINUE);
    CHECK_INT(nd_vlist_handle_key(&list, ND_KEY_CLEAR), ND_WIDGET_BACK);
    nd_vlist_draw(&list); /* must not read items[0] */

    fx_free(&fx);
}

/* The title is trimmed so it cannot run underneath the breadcrumb. The bug
 * this fixed produced "Remote Sh<overlap>7-7" on the Remote Shell menu. */
static void test_vlist_title_is_trimmed(void)
{
    fixture fx;
    nd_vlist list;
    nd_header h;
    int32_t reserved;
    int32_t avail;
    char want[ND_TEXT_LINE_MAX];
    int32_t w = 0;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_vlist_init(&list, &fx.ui, "Remote Shell", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 9007);
    nd_header_init_int(&h, &fx.ui, 9007);
    reserved = nd_header_width(&h, 7);
    avail = 240 - 5 - reserved - 6;

    list.selected_index = 6u; /* breadcrumb "9007-7" */
    (void)nd_text_fit(want, sizeof want, "Remote Shell", fx.ui.font_xl, avail);
    nd_text_size(fx.ui.font_xl, want, &w, NULL);
    CHECK(w <= avail);
    /* It really does have to trim at this width -- otherwise the test proves
     * nothing about the overlap. */
    nd_text_size(fx.ui.font_xl, "Remote Shell", &w, NULL);
    CHECK(w > avail);

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2e. LevelSelector
 * ------------------------------------------------------------------ */

static void test_levelsel(void)
{
    fixture fx;
    nd_levelsel sel;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_levelsel_init(&sel, &fx.ui, 1, 9, "Level", 6);
    CHECK_INT(sel.list.n_items, 9);
    CHECK_INT(sel.list.selected_index, 0);
    CHECK_STR(sel.list.title, "Level");

    /* max(0, min(count - 1, current - 1)) at both ends. */
    nd_levelsel_init(&sel, &fx.ui, 9, 9, "Level", 6);
    CHECK_INT(sel.list.selected_index, 8);
    nd_levelsel_init(&sel, &fx.ui, 0, 9, "Level", 6);
    CHECK_INT(sel.list.selected_index, 0);
    nd_levelsel_init(&sel, &fx.ui, -5, 9, "Level", 6);
    CHECK_INT(sel.list.selected_index, 0);
    nd_levelsel_init(&sel, &fx.ui, 99, 9, "Level", 6);
    CHECK_INT(sel.list.selected_index, 8);

    /* A shorter ladder, which Games never asks for but the constructor takes. */
    nd_levelsel_init(&sel, &fx.ui, 3, 4, "Speed", 6);
    CHECK_INT(sel.count, 4);
    CHECK_INT(sel.list.n_items, 4);
    CHECK_INT(sel.list.selected_index, 2);
    CHECK_STR(sel.list.items[3], "Level 4");

    /* The inherited digit shortcut is what makes "press 5 for level 5" work,
     * and it is why nd_levelsel_show adds one to whatever the list returns. */
    nd_levelsel_init(&sel, &fx.ui, 1, 9, "Level", 6);
    CHECK_INT(nd_vlist_handle_key(&sel.list, ND_KEY_5), 4);

    /* Beyond ND_LEVELSEL_MAX the ladder is clamped rather than overrunning. */
    nd_levelsel_init(&sel, &fx.ui, 12, 20, "Level", 6);
    CHECK_INT(sel.count, ND_LEVELSEL_MAX);
    CHECK_INT(sel.list.n_items, ND_LEVELSEL_MAX);
    CHECK_INT(sel.list.selected_index, ND_LEVELSEL_MAX - 1);

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2f. PagedList
 * ------------------------------------------------------------------ */

static void test_pagedlist_wrap(void)
{
    fixture fx;
    char storage[2][ND_TEXT_LINE_MAX];
    nd_lines lines;
    const nd_font *f;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    f = fx.ui.font_xl;
    nd_lines_init(&lines, storage, 2u);

    /* Empty input still occupies one (blank) line, which is what keeps the
     * vertical centring stable on an item with no name. */
    nd_pagedlist_wrap(&lines, "", f, 223, 2u);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0u), "");
    nd_pagedlist_wrap(&lines, NULL, f, 223, 2u);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0u), "");
    nd_pagedlist_wrap(&lines, "   \n\t ", f, 223, 2u);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0u), "");

    /* Short enough: one line, unchanged. */
    nd_pagedlist_wrap(&lines, "Inbox", f, 223, 2u);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0u), "Inbox");

    /* THE COLLAPSE: this wrapper splits on any whitespace with no empty
     * tokens, so newlines and runs of spaces vanish. The other three wrappers
     * keep blank lines; this one cannot. */
    nd_pagedlist_wrap(&lines, "Write\n\n  Message", f, 223, 2u);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0u), "Write Message");

    /* Two lines when it does not fit on one. */
    nd_pagedlist_wrap(&lines, "Write a longer message now", f, 223, 2u);
    CHECK_INT(lines.n, 2);
    CHECK(strstr(nd_lines_at(&lines, 0u), " ") != NULL);

    /* Words left over past the second line: the last line gains "...". */
    nd_pagedlist_wrap(&lines, "one two three four five six seven eight nine ten eleven twelve", f,
                      223, 2u);
    CHECK_INT(lines.n, 2);
    {
        const char *last = nd_lines_at(&lines, 1u);
        size_t n = strlen(last);

        CHECK(n >= 3u && strcmp(last + (n - 3u), "...") == 0);
    }

    /* A single over-long word, and it is the ONLY word: hard-trimmed with NO
     * "..." on the end, because the quirk only appends when a word follows. */
    nd_pagedlist_wrap(&lines, "Supercalifragilisticexpialidocious", f, 223, 2u);
    CHECK_INT(lines.n, 1);
    {
        const char *only = nd_lines_at(&lines, 0u);
        size_t n = strlen(only);
        int32_t w = 0;

        CHECK(n > 0u && n < strlen("Supercalifragilisticexpialidocious"));
        CHECK(!(n >= 3u && strcmp(only + (n - 3u), "...") == 0));
        nd_text_size(f, only, &w, NULL);
        CHECK(w <= 223);
    }

    /* The same word with something after it DOES get the dots. */
    nd_pagedlist_wrap(&lines, "Supercalifragilisticexpialidocious tail", f, 223, 2u);
    CHECK(lines.n >= 1u);
    {
        const char *first = nd_lines_at(&lines, 0u);
        size_t n = strlen(first);

        CHECK(n >= 3u && strcmp(first + (n - 3u), "...") == 0);
    }

    /* Nothing at all fits: the line becomes a bare "...". */
    nd_pagedlist_wrap(&lines, "Supercalifragilistic tail", f, 4, 2u);
    CHECK(lines.n >= 1u);
    CHECK_STR(nd_lines_at(&lines, 0u), "...");

    fx_free(&fx);
}

static void test_pagedlist_layout(void)
{
    fixture fx;
    nd_pagedlist p;
    int32_t x;
    int32_t lit;
    int32_t y;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", true);
    nd_pagedlist_draw(&p);

    /* The scrollbar is WHITE and TWO columns wide -- 235 and 236, the minor
     * axis only, which is the opposite of VerticalList's grey single column. */
    CHECK_INT(nd_image_get_px(fx.canvas, 235, 100).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 236, 100).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 237, 100).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 234, 100).r, 0);

    /* Notch x 231..237, at the track top for index 0 -> rows 35..41. */
    CHECK_INT(nd_image_get_px(fx.canvas, 231, 35).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 237, 41).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 231, 42).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 230, 38).r, 0);

    /* Step for three items is (135-38)/2 = 48.5, so index 1 truncates to
     * 86.5 -> 83.5..89.5 -> rows 83..89. Rounding would give 84..90. */
    p.selected_index = 1u;
    nd_pagedlist_draw(&p);
    CHECK_INT(nd_image_get_px(fx.canvas, 231, 83).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 231, 89).r, 255);
    CHECK_INT(nd_image_get_px(fx.canvas, 231, 90).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 231, 82).r, 0);

    /* THE CENTRING QUIRK: the item is centred inside 223, not 240, so its ink
     * sits left of the true centre. Find the ink span of the single line. */
    p.selected_index = 0u;
    nd_pagedlist_draw(&p);
    {
        int32_t first = -1;
        int32_t last = -1;

        for (x = 0; x < 230; x++) {
            for (y = 38; y <= 135; y++) {
                if (nd_image_get_px(fx.canvas, x, y).r != 0u) {
                    if (first < 0)
                        first = x;
                    last = x;
                    break;
                }
            }
        }
        CHECK(first > 0 && last > first);
        /* Centred in 240 the left margin would equal the right margin; centred
         * in 223 the right margin is about 17 px larger. */
        CHECK((239 - last) - first >= 12);
    }

    /* Empty: "No Items" centred in the content band, the root id alone in the
     * corner, and the softkey strip explicitly cleared. */
    nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, 0u, "2", true);
    (void)nd_image_fill(fx.canvas, ND_WHITE);
    nd_pagedlist_draw(&p);
    CHECK(!strip_has_ink(fx.canvas));
    lit = 0;
    for (x = 0; x < 240; x++) {
        if (nd_image_get_px(fx.canvas, x, 80).r != 0u)
            lit++;
    }
    CHECK(lit > 0); /* the "No Items" label really is on that row */
    /* The empty branch returns before the scrollbar, so there is no track and
     * no notch at all -- a list with nothing in it has nothing to scroll. */
    CHECK_INT(nd_image_get_px(fx.canvas, 235, 100).r, 0);
    CHECK_INT(nd_image_get_px(fx.canvas, 236, 100).r, 0);

    fx_free(&fx);
}

static void test_pagedlist_show_index_guard(void)
{
    fixture fx;
    nd_pagedlist p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    /* show() clamps a stale index before its first draw. Reproduced without
     * entering the key loop, which has no input to read here. */
    nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", true);
    p.selected_index = 9u;
    if (p.selected_index >= p.n_items)
        p.selected_index = 0u;
    CHECK_INT(p.selected_index, 0);

    /* Paging wraps in both directions, unlike VerticalList's hard stop. */
    p.selected_index = 2u;
    p.selected_index = (p.selected_index + 1u) % p.n_items;
    CHECK_INT(p.selected_index, 0);
    p.selected_index = (p.selected_index + p.n_items - 1u) % p.n_items;
    CHECK_INT(p.selected_index, 2);

    /* An empty list draws its own screen and must not divide by zero. */
    nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, 0u, "2", false);
    nd_pagedlist_draw(&p);
    CHECK_INT(p.selected_index, 0);

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 3. Hold-to-repeat
 * ------------------------------------------------------------------ */

#define EV_KEY_T 0x01
#define EV_SYN_T 0x00

typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} ev_native;

static void write_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    ev_native ev;

    memset(&ev, 0, sizeof ev);
    ev.tv_sec = 1;
    ev.tv_usec = 2;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    CHECK_INT(write(fd, &ev, sizeof ev), (int)sizeof ev);
}

static void write_key(int fd, uint16_t code, int32_t value)
{
    write_event(fd, EV_KEY_T, code, value);
    write_event(fd, EV_SYN_T, 0u, 0);
}

/* A held Down must walk the list one row per synthesised repeat, through
 * exactly the call nd_vlist_show() makes -- nd_ui_wait_for_key(). Nothing more
 * is written to the pipe after the single press, so every key after the first
 * came out of nd_input's repeat timer.
 *
 * The delay and interval are shortened so the test does not sit for a second;
 * nd_keypad.h's 400/120 ms defaults are checked in test_input.c. */
static void test_held_down_scrolls_the_list(void)
{
    fixture fx;
    nd_vlist list;
    nd_input *in = NULL;
    int fds[2];
    int i;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    CHECK_INT(pipe(fds), 0);
    CHECK_INT(nd_input_open_fd(&in, fds[0]), ND_OK);
    if (in == NULL) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        fx_free(&fx);
        return;
    }
    nd_input_set_repeat(in, 0.02, 0.01);
    fx.ui.input = in;

    nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
    nd_vlist_draw(&list);

    write_key(fds[1], (uint16_t)ND_KEY_DOWN, 1); /* pressed, never released */

    /* This is nd_vlist_show()'s loop body, run a bounded number of times so
     * the test terminates: the real loop only ever leaves through a key that
     * returns something other than ND_VLIST_CONTINUE. */
    for (i = 0; i < 5; i++) {
        int32_t key = nd_ui_wait_for_key(&fx.ui);

        CHECK_INT(key, ND_KEY_DOWN);
        CHECK_INT(nd_vlist_handle_key(&list, key), ND_VLIST_CONTINUE);
        if (key == ND_KEY_UP || key == ND_KEY_DOWN)
            nd_vlist_draw(&list);
    }
    CHECK_INT(list.selected_index, 5); /* five rows down, one press */
    CHECK_INT(list.window_start, 3);   /* and the window followed it */
    CHECK(nd_input_last_was_repeat(in));

    /* Letting go stops it: after the release there is nothing left to read. */
    write_key(fds[1], (uint16_t)ND_KEY_DOWN, 0);
    while (nd_input_read_key(in, 0.05) != ND_KEY_NONE) {}
    CHECK_INT(nd_ui_read_keypress(&fx.ui, 0.05), ND_KEY_NONE);
    CHECK(!nd_input_is_held(in, ND_KEY_DOWN));

    fx.ui.input = NULL;
    (void)close(fds[1]);
    nd_input_close(in); /* closes fds[0] */
    fx_free(&fx);
}

/* The same, for PagedList, where a held arrow wraps rather than stopping. */
/* ------------------------------------------------------------------ *
 * 4. The blocking show() loops, end to end
 * ------------------------------------------------------------------ */

/* Every key is written before show() is called, so the pipe never runs dry and
 * the loop cannot block: it reads the script, redraws, and leaves through the
 * key that ends it. Repeat plays no part here -- each press is released. */
static int script_open(nd_input **in, int *write_fd)
{
    int fds[2];

    if (pipe(fds) != 0)
        return -1;
    if (nd_input_open_fd(in, fds[0]) != ND_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    /* Nothing may repeat: a scripted press is released immediately, but the
     * flush poll inside PagedList would otherwise be a place for a stray
     * synthesised event to appear. */
    nd_input_set_repeat(*in, 0.0, 0.0);
    *write_fd = fds[1];
    return 0;
}

static void script_key(int fd, int32_t code)
{
    write_key(fd, (uint16_t)code, 1);
    write_key(fd, (uint16_t)code, 0);
}

static void test_vlist_show_runs_a_script(void)
{
    fixture fx;
    nd_input *in = NULL;
    int w = -1;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    if (script_open(&in, &w) != 0) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    fx.ui.input = in;

    {
        nd_vlist list;

        nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
        script_key(w, ND_KEY_DOWN);
        script_key(w, ND_KEY_DOWN);
        script_key(w, ND_KEY_UP);
        script_key(w, ND_KEY_ENTER);
        CHECK_INT(nd_vlist_show(&list), 1);
        CHECK_INT(list.selected_index, 1);
    }
    {
        nd_vlist list;

        nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
        script_key(w, ND_KEY_STAR); /* ignored, no redraw, loop continues */
        script_key(w, ND_KEY_CLEAR);
        CHECK_INT(nd_vlist_show(&list), ND_WIDGET_BACK);
    }
    {
        /* The digit shortcut short-circuits everything, wherever the
         * selection happens to be. */
        nd_vlist list;

        nd_vlist_init(&list, &fx.ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);
        script_key(w, ND_KEY_4);
        CHECK_INT(nd_vlist_show(&list), 3);
        CHECK_INT(list.selected_index, 0);
    }
    {
        /* LevelSelector paints "OK" without presenting, runs the list, and
         * turns the zero-based row into a one-based level. */
        nd_levelsel sel;

        nd_levelsel_init(&sel, &fx.ui, 3, 9, "Level", 6);
        script_key(w, ND_KEY_DOWN);
        script_key(w, ND_KEY_ENTER);
        CHECK_INT(nd_levelsel_show(&sel), 4);

        nd_levelsel_init(&sel, &fx.ui, 3, 9, "Level", 6);
        script_key(w, ND_KEY_CLEAR);
        CHECK_INT(nd_levelsel_show(&sel), ND_WIDGET_BACK); /* Python's None */

        nd_levelsel_init(&sel, &fx.ui, 1, 9, "Level", 6);
        script_key(w, ND_KEY_7);
        CHECK_INT(nd_levelsel_show(&sel), 7);
    }

    fx.ui.input = NULL;
    (void)close(w);
    nd_input_close(in);
    fx_free(&fx);
}

/* PagedList's show() DRAINS the channel before its first draw, so a script
 * written in advance is precisely what it is built to throw away -- that is
 * the behaviour under test, not a way to drive it. The keys that have to
 * survive the flush are therefore delivered the only way anything can arrive
 * after it: as repeats of a key that is still held.
 *
 * ND_KEY_ENTER and ND_KEY_CLEAR do not repeat by default, and must not -- a
 * repeat on Enter would open whatever the list landed on. The test asks for it
 * explicitly, which is also a check that nd_input_set_repeat_codes() reaches
 * the widget at all. */
static void test_pagedlist_show_flushes_then_runs(void)
{
    static const int32_t REPEATERS[2] = {ND_KEY_ENTER, ND_KEY_CLEAR};
    fixture fx;
    nd_input *in = NULL;
    int w = -1;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    if (script_open(&in, &w) != 0) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    CHECK_INT(nd_input_set_repeat_codes(in, REPEATERS, ND_ARRAY_LEN(REPEATERS)), ND_OK);
    /* Long enough that the 0.01 s flush poll sees an idle channel and stops,
     * short enough that the test does not sit here. */
    nd_input_set_repeat(in, 0.20, 0.05);
    fx.ui.input = in;

    {
        nd_pagedlist p;

        nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", true);
        script_key(w, ND_KEY_DOWN); /* both of these are eaten by the flush */
        script_key(w, ND_KEY_DOWN);
        write_key(w, (uint16_t)ND_KEY_ENTER, 1); /* held: its REPEAT ends show() */

        CHECK_INT(nd_pagedlist_show(&p), 0); /* the two Downs never happened */
        CHECK_INT(p.selected_index, 0);

        write_key(w, (uint16_t)ND_KEY_ENTER, 0);
        while (nd_input_read_key(in, 0.05) != ND_KEY_NONE) {}
    }
    {
        nd_pagedlist p;

        nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", false);
        p.selected_index = 40u; /* show() clamps a stale index before drawing */
        write_key(w, (uint16_t)ND_KEY_CLEAR, 1);

        CHECK_INT(nd_pagedlist_show(&p), ND_WIDGET_BACK);
        CHECK_INT(p.selected_index, 0);

        write_key(w, (uint16_t)ND_KEY_CLEAR, 0);
        while (nd_input_read_key(in, 0.05) != ND_KEY_NONE) {}
    }
    {
        /* ENTER on an EMPTY list still answers 0, even though draw() renders
         * no row at all. Latent in the Python; reproduced. */
        nd_pagedlist p;

        nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, 0u, "2", true);
        write_key(w, (uint16_t)ND_KEY_ENTER, 1);
        CHECK_INT(nd_pagedlist_show(&p), 0);
        write_key(w, (uint16_t)ND_KEY_ENTER, 0);
        while (nd_input_read_key(in, 0.05) != ND_KEY_NONE) {}
    }

    fx.ui.input = NULL;
    (void)close(w);
    nd_input_close(in);
    fx_free(&fx);
}

static void test_held_up_pages_backwards(void)
{
    fixture fx;
    nd_pagedlist p;
    nd_input *in = NULL;
    int fds[2];
    int i;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    CHECK_INT(pipe(fds), 0);
    CHECK_INT(nd_input_open_fd(&in, fds[0]), ND_OK);
    if (in == NULL) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        fx_free(&fx);
        return;
    }
    nd_input_set_repeat(in, 0.02, 0.01);
    fx.ui.input = in;

    nd_pagedlist_init(&p, &fx.ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", true);
    write_key(fds[1], (uint16_t)ND_KEY_UP, 1);

    for (i = 0; i < 4; i++) {
        int32_t key = nd_ui_wait_for_key(&fx.ui);

        CHECK_INT(key, ND_KEY_UP);
        p.selected_index = (p.selected_index + p.n_items - 1u) % p.n_items;
        nd_pagedlist_draw(&p);
    }
    /* Four steps backwards through three items wraps once and a bit. */
    CHECK_INT(p.selected_index, 2);

    fx.ui.input = NULL;
    (void)close(fds[1]);
    nd_input_close(in);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    int rc;

    pt_new_case();
    manifest_open();

    RUN(test_derived_geometry);
    RUN(test_header);
    RUN(test_softkey);
    RUN(test_vlist_layout);
    RUN(test_vlist_preselected_row_scrolls_into_view);
    RUN(test_vlist_notch_truncates);
    RUN(test_vlist_keys);
    RUN(test_vlist_title_is_trimmed);
    RUN(test_levelsel);
    RUN(test_pagedlist_wrap);
    RUN(test_pagedlist_layout);
    RUN(test_pagedlist_show_index_guard);
    RUN(test_vlist_show_runs_a_script);
    RUN(test_pagedlist_show_flushes_then_runs);
    RUN(test_held_down_scrolls_the_list);
    RUN(test_held_up_pages_backwards);
    RUN(test_golden_frames);

    rc = pt_report("test_widgets_lists");
    if (g_manifest != NULL)
        nd_json_free(g_manifest);
    return rc;
}
