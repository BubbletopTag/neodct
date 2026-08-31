/* test_widgets_dialogs.c -- MessageDialog, TextScroller, InfoScreen,
 * ProgressScreen and DetailPage.
 *
 * Four kinds of check, and all four are needed:
 *
 *  1. THREE GOLDEN FRAMES. widget-messagedialog, widget-textscroller and
 *     widget-infoscreen are re-rendered here and their SHA-256 over raw RGB
 *     is compared with neodct/tests/golden/manifest.json -- the same digest
 *     goldenframe.py compares, so a pass here is a pass there. The draw
 *     sequence mirrors shoot_docs.py's shoot_widgets(). All three repaint the
 *     full 240x175 between them (a partial clear plus their own softkey bar),
 *     so unlike the list frames none of them inherits a strip from the frame
 *     before it.
 *
 *     widget-messagedialog is the one that matters most: SESSION-SCOPE.md
 *     makes MessageDialog the entire visible behaviour of all 25 stub apps
 *     and that frame is already the exact stub wording.
 *
 *  2. GEOMETRY AS NUMBERS, so a failure names the value that moved instead of
 *     saying "one frame differs". ProgressScreen's five boxes and DetailPage's
 *     measured worked example from spec-ui-framework.md are here, as are the
 *     two looks of MessageDialog and TextScroller's blank-line gap.
 *
 *  3. PIXELS WHERE THERE IS NO GOLDEN FRAME. ProgressScreen and DetailPage
 *     have none, so their invariants are asserted against the canvas the way
 *     neodct/tests/test_update_ui.py does: nothing within 3 px of the bar, the
 *     fill proportional, nothing below the viewport, the scrollbar only in the
 *     x >= 232 strip, no half-line at the fold.
 *
 *  4. THE BLOCKING LOOPS, over a real nd_input on a pipe.
 *
 * A staged ND_ROOT whose System is a symlink to neodct/overlay is needed
 * because MessageDialog loads /NeoDCT/System/ui/resources/img/errorscreen/
 * warning.png through the image cache, and the cache resolves through ND_ROOT.
 *
 * Set NEODCT_FRAMES_OUT to a directory to also write the PNGs and a
 * manifest.json there, which is what
 *   python3 neodct/tools/goldenframe.py --compare neodct/tests/golden <dir>
 * consumes.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "platform_test.h"

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* ------------------------------------------------------------------ *
 * Finding the reference set, the font and the overlay
 * ------------------------------------------------------------------ */

static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];
static char g_stage[ND_PATH_MAX];

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

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
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

/* <repo>/neodct/tests/golden -> <repo>/neodct */
static bool resolve_neodct_dir(char *out, size_t sz)
{
    char base[ND_PATH_MAX];
    char *cut;

    if (!resolve_golden(g_golden, sizeof g_golden))
        return false;
    (void)snprintf(base, sizeof base, "%.480s", g_golden);
    cut = strrchr(base, '/'); /* .../neodct/tests */
    if (cut != NULL)
        *cut = '\0';
    cut = strrchr(base, '/'); /* .../neodct       */
    if (cut != NULL)
        *cut = '\0';
    (void)nd_strlcpy(out, base, sz);
    return true;
}

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen and
 * not through nd_path_resolve(). Same search test_widgets_lists.c performs. */
static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    char base[ND_PATH_MAX];
    char cand[ND_PATH_MAX];

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
        return true;
    }
    if (resolve_neodct_dir(base, sizeof base)) {
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
    if (file_exists(ND_PATH_FONT)) {
        (void)nd_strlcpy(out, ND_PATH_FONT, sz);
        return true;
    }
    return false;
}

/* ND_ROOT with System symlinked at the overlay, exactly as test_ui.c stages
 * it: nothing under neodct/overlay/ is written to, and the image cache finds
 * warning.png at its real absolute path. */
static bool stage_root(void)
{
    char base[ND_PATH_MAX];
    char tmpl[ND_PATH_MAX];
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    const char *tmp = getenv("TMPDIR");

    if (!resolve_neodct_dir(base, sizeof base))
        return false;
    if (nd_snprintf(g_overlay, sizeof g_overlay, "%s/overlay", base) != ND_OK)
        return false;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/nddlg-XXXXXX", tmp) != ND_OK)
        return false;
    if (mkdtemp(tmpl) == NULL)
        return false;
    (void)nd_strlcpy(g_stage, tmpl, sizeof g_stage);

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        return false;
    (void)mkdir(neodct, 0755);
    if (nd_snprintf(sys_link, sizeof sys_link, "%s/System", neodct) != ND_OK)
        return false;
    if (nd_snprintf(sys_target, sizeof sys_target, "%s/NeoDCT/System", g_overlay) != ND_OK)
        return false;
    if (symlink(sys_target, sys_link) != 0 && errno != EEXIST)
        return false;

    return nd_path_set_root(g_stage) == ND_OK;
}

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

static void drop_stage(void)
{
    (void)nd_path_set_root(NULL);
    if (g_stage[0] != '\0')
        (void)nftw(g_stage, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    g_stage[0] = '\0';
}

/* ------------------------------------------------------------------ *
 * A UI context with nothing behind it but memory
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;
    nd_image *scratch;
    nd_font *font_s;
    nd_font *font_md;
    nd_font *font_n;
    nd_font *font_xl;
} fixture;

static bool fx_init(fixture *fx)
{
    char path[ND_PATH_MAX];

    memset(fx, 0, sizeof *fx);
    if (!resolve_font(path, sizeof path)) {
        fprintf(stderr, "test_widgets_dialogs: cannot find font.ttf; set NEODCT_FONT\n");
        return false;
    }
    fx->font_s = nd_font_load(path, ND_FONT_PX_S);
    fx->font_md = nd_font_load(path, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(path, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(path, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "test_widgets_dialogs: nd_font_load(%s) failed\n", path);
        return false;
    }

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    /* 240 * 145 * 3 = 104,400 bytes -- DetailPage's borrowed column. */
    fx->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL || fx->scratch == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;

    fx->ui.w = ND_UI_W;
    fx->ui.h = ND_UI_H;
    fx->ui.softkey_h = ND_SOFTKEY_H;
    fx->ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    fx->ui.canvas = fx->canvas;
    fx->ui.scratch = fx->scratch;
    fx->ui.draw = &fx->draw;
    fx->ui.fb = NULL; /* no panel: the canvas is the frame */
    fx->ui.font_s = fx->font_s;
    fx->ui.font_md = fx->font_md;
    fx->ui.font_n = fx->font_n;
    fx->ui.font_xl = fx->font_xl;
    fx->ui.keypad_fd = -1;
    fx->ui.softkey_exists = true;
    fx->ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    return fx->ui.image_cache != NULL;
}

static void fx_free(fixture *fx)
{
    nd_imgcache_free(fx->ui.image_cache);
    nd_image_free(fx->canvas);
    nd_image_free(fx->scratch);
    nd_font_free(fx->font_s);
    nd_font_free(fx->font_md);
    nd_font_free(fx->font_n);
    nd_font_free(fx->font_xl);
    memset(fx, 0, sizeof *fx);
}

/* ------------------------------------------------------------------ *
 * Pixel helpers
 * ------------------------------------------------------------------ */

static bool px_lit(const nd_image *img, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(img, x, y);

    return c.r != 0u || c.g != 0u || c.b != 0u;
}

/* First lit row at or after `from`, or -1. */
static int32_t first_lit_row(const nd_image *img, int32_t from, int32_t to)
{
    int32_t y;
    int32_t x;

    for (y = from; y <= to; y++) {
        for (x = 0; x < img->w; x++) {
            if (px_lit(img, x, y))
                return y;
        }
    }
    return -1;
}

static int32_t first_lit_col(const nd_image *img, int32_t row)
{
    int32_t x;

    for (x = 0; x < img->w; x++) {
        if (px_lit(img, x, row))
            return x;
    }
    return -1;
}

static int32_t count_lit(const nd_image *img, nd_rect box)
{
    int32_t n = 0;
    int32_t x;
    int32_t y;

    for (y = box.y0; y <= box.y1; y++) {
        for (x = box.x0; x <= box.x1; x++) {
            if (px_lit(img, x, y))
                n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * The golden manifest
 * ------------------------------------------------------------------ */

static nd_json_doc *g_manifest;
static const nd_json_val *g_frames;

static void manifest_open(void)
{
    char path[ND_PATH_MAX + 32];
    uint8_t *buf = NULL;
    long len;
    FILE *f;
    char err[128];
    const nd_json_val *root;

    if (g_golden[0] == '\0' && !resolve_golden(g_golden, sizeof g_golden))
        return;
    (void)snprintf(path, sizeof path, "%.1000s/manifest.json", g_golden);

    /* Plain fopen: the reference set is not under NEODCT_ROOT. */
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
        fprintf(stderr, "test_widgets_dialogs: manifest parse: %s\n", err);
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

/* ------------------------------------------------------------------ *
 * Frame comparison
 * ------------------------------------------------------------------ */

static nd_capture *g_capture;

static void frames_out_open(void)
{
    const char *dir = getenv("NEODCT_FRAMES_OUT");

    if (dir == NULL || dir[0] == '\0')
        return;
    /* nd_capture_open() resolves through ND_ROOT, and the staged root would
     * hide the output inside a temporary directory nobody can find. An
     * explicit dump is the one place that is wrong. */
    (void)nd_path_set_root(NULL);
    if (nd_capture_open(&g_capture, dir, 0u) != ND_OK) {
        fprintf(stderr, "test_widgets_dialogs: cannot open %s for frames\n", dir);
        g_capture = NULL;
    }
    (void)nd_path_set_root(g_stage);
}

static void frames_out_close(void)
{
    if (g_capture == NULL)
        return;
    (void)nd_path_set_root(NULL);
    (void)nd_capture_write_manifest(g_capture);
    nd_capture_close(g_capture);
    g_capture = NULL;
    (void)nd_path_set_root(g_stage);
}

static void check_frame(const fixture *fx, const char *name)
{
    char got[65];
    const char *want = golden_sha(name);

    if (nd_capture_digest(fx->canvas, got, sizeof got) != ND_OK) {
        CHECK(false);
        return;
    }
    if (g_capture != NULL) {
        (void)nd_path_set_root(NULL);
        (void)nd_capture_save(g_capture, name, fx->canvas);
        (void)nd_path_set_root(g_stage);
    }
    if (want == NULL) {
        fprintf(stderr, "test_widgets_dialogs: no reference for %s (got %s)\n", name, got);
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

/* shoot_docs.py, verbatim. This exact string is also what all 25 stub apps
 * show, so this frame is the acceptance test for the whole stub set. */
#define STUB_MESSAGE "This application has not been implemented yet."

/* shoot_docs.py: the Snake instructions. This was the single worst frame in
 * the raqm incident at 16.81% differing pixels, which makes it the best
 * stress test of the text layer in the widget set. */
#define SNAKE_HELP                                                                \
    "Feed the snake by steering it to the food. Every bite makes it grow longer." \
    " Use keys 2, 4, 6 and 8 to change direction."

static void test_golden_messagedialog(fixture *fx)
{
    nd_msgdialog dlg;

    nd_msgdialog_init(&dlg, &fx->ui, STUB_MESSAGE);
    CHECK_INT(dlg.margin, 8);
    CHECK_INT(dlg.n_accept, 1);
    CHECK_INT(dlg.accept_keys[0], ND_KEY_ENTER);
    CHECK_INT(dlg.n_cancel, 1);
    CHECK_INT(dlg.cancel_keys[0], ND_KEY_CLEAR);
    CHECK_STR(dlg.button_text, "OK");

    nd_msgdialog_render(&dlg);
    check_frame(fx, "widget-messagedialog");
}

static void test_golden_textscroller(fixture *fx)
{
    nd_scroller s;
    size_t line_h = 0u;

    nd_scroller_init(&s, &fx->ui, SNAKE_HELP, "More", "Back");
    CHECK_INT(s.margin, 10);
    CHECK_INT(s.top, 8);
    CHECK(s.font == fx->ui.font_n);

    /* Five 25 px lines fit in the 133 px budget and a sixth does not, so the
     * strip says "More" rather than "Back". */
    CHECK_INT(nd_scroller_paginate(&s, &line_h), 2);
    CHECK_INT(line_h, 25);

    CHECK(!nd_scroller_draw(&s)); /* not the last page */
    check_frame(fx, "widget-textscroller");
}

static void test_golden_infoscreen(fixture *fx)
{
    nd_input *in = NULL;
    int fds[2];

    /* shoot_docs.py pushes BACK and lets show() return through it. */
    CHECK_INT(pipe(fds), 0);
    if (nd_input_open_fd(&in, fds[0]) != ND_OK) {
        CHECK(false);
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }
    nd_input_set_repeat(in, 0.0, 0.0);
    fx->ui.input = in;

    {
        /* InfoScreen(ui, "Top score", 1250) -- str(1250) at the call site. */
        struct {
            long tv_sec;
            long tv_usec;
            uint16_t type;
            uint16_t code;
            int32_t value;
        } ev;
        int32_t i;

        for (i = 0; i < 2; i++) {
            memset(&ev, 0, sizeof ev);
            ev.tv_sec = 1;
            ev.type = 0x01;
            ev.code = (uint16_t)ND_KEY_CLEAR;
            ev.value = (i == 0) ? 1 : 0;
            CHECK_INT(write(fds[1], &ev, sizeof ev), (int)sizeof ev);
            memset(&ev, 0, sizeof ev);
            ev.tv_sec = 1;
            CHECK_INT(write(fds[1], &ev, sizeof ev), (int)sizeof ev);
        }
    }

    CHECK_INT(nd_infoscreen_show(&fx->ui, "Top score", "1250", "Back"), ND_KEY_CLEAR);
    check_frame(fx, "widget-infoscreen");

    fx->ui.input = NULL;
    (void)close(fds[1]);
    nd_input_close(in);
}

static void test_golden_frames(void)
{
    fixture fx;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    frames_out_open();

    /* shoot_docs.py's order. Each of the three repaints all 175 rows between
     * its partial clear and its own softkey bar, so there is nothing to
     * inherit -- unlike the list frames, where "More" survives into the
     * LevelSelector's frame. */
    test_golden_messagedialog(&fx);
    test_golden_textscroller(&fx);
    test_golden_infoscreen(&fx);

    frames_out_close();
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2. MessageDialog, as numbers
 * ------------------------------------------------------------------ */

/* spec-ui-framework.md's worked example: MessageDialog(ui, "LOW BATTERY!") is
 * one alert line at 20 px, line_h 24, max_lines 4, and lands at y = 75. */
static void test_msgdialog_alert_look(void)
{
    fixture fx;
    nd_msgdialog dlg;
    nd_rect bbox;
    int32_t lw = 0;
    int32_t row;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_msgdialog_init(&dlg, &fx.ui, "LOW BATTERY!");
    nd_msgdialog_render(&dlg);

    /* The icon occupies rows 8..31; the body starts well below it. */
    CHECK(px_lit(fx.canvas, 19, 8));
    nd_text_bbox(fx.ui.font_n, "LOW BATTERY!", &bbox);
    nd_text_size(fx.ui.font_n, "LOW BATTERY!", &lw, NULL);

    row = first_lit_row(fx.canvas, 40, 140);
    CHECK_INT(row, 75 + bbox.y0);
    CHECK_INT(first_lit_col(fx.canvas, row), nd_max32(8, (240 - lw) / 2) + bbox.x0);

    /* Centred, so it is NOT at the margin -- that is the whole difference
     * between this look and the paragraph one. */
    CHECK(nd_max32(8, (240 - lw) / 2) > 8);
    fx_free(&fx);
}

/* Three lines at 20 px means the paragraph look: 14 px, left-aligned at the
 * margin, line_h 18. The stub message is exactly that case. */
static void test_msgdialog_paragraph_look(void)
{
    fixture fx;
    nd_msgdialog dlg;
    nd_rect bbox;
    int32_t row;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_msgdialog_init(&dlg, &fx.ui, STUB_MESSAGE);
    nd_msgdialog_render(&dlg);

    /* y = 38 after the icon, two 18 px lines, so the body starts at
     * 38 + (145 - 8 - 38 - 36)/2 = 69. */
    nd_text_bbox(fx.ui.font_s, "This application has not", &bbox);
    row = first_lit_row(fx.canvas, 40, 140);
    CHECK_INT(row, 69 + bbox.y0);
    CHECK_INT(first_lit_col(fx.canvas, row), 8 + bbox.x0);

    /* The second line is 18 px below the first. */
    CHECK(px_lit(fx.canvas, first_lit_col(fx.canvas, row), row));
    CHECK_INT(first_lit_row(fx.canvas, row + 18, 140), row + 18);
    fx_free(&fx);
}

/* A message too tall for the space gets clipped and the last line gains
 * " …" -- U+2026, which this font draws as NOTHING plus 8 px of advance.
 * The check is that the pixels are unchanged and the advance is not, which is
 * exactly the property "..." would break. */
static void test_msgdialog_invisible_ellipsis(void)
{
    fixture fx;
    int32_t plain = 0;
    int32_t with_dots = 0;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_text_size(fx.ui.font_s, "yet.", &plain, NULL);
    nd_text_size(fx.ui.font_s, "yet. \xE2\x80\xA6", &with_dots, NULL);
    /* A space plus an invisible glyph: wider, with no new ink. */
    CHECK(with_dots > plain);
    CHECK_INT(nd_font_advance(fx.ui.font_n, 0x2026u), 8);

    {
        /* Eight paragraphs will not fit in the 99 px the body has, so the
         * last drawn line must end in the ellipsis and nothing may be drawn
         * below the softkey line. */
        nd_msgdialog dlg;
        static const char LONG[] =
            "Alpha bravo charlie delta echo foxtrot golf hotel india juliett kilo lima "
            "mike november oscar papa quebec romeo sierra tango uniform victor whiskey "
            "xray yankee zulu one two three four five six seven eight nine ten eleven "
            "twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen.";

        nd_msgdialog_init(&dlg, &fx.ui, LONG);
        nd_msgdialog_render(&dlg);
        /* Rows 137..144 are the margin below the last of the five 18 px lines
         * that fit; nothing may be lit there. */
        CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 138, 239, 144)), 0);
    }
    fx_free(&fx);
}

/* ============ THE CLIP IS INVISIBLE, SO SOMETHING HAS TO SEE IT ============
 *
 * nd_msgdialog does not fail on an overlong message and does not mark one.
 * append_ellipsis() adds U+2026, the font has no glyph for it, and the text
 * simply stops. The modem fault notice shipped that way -- seven lines into a
 * five-line dialog, ending mid-sentence at "there is" -- and nothing in the
 * suite noticed, because nothing was looking.
 *
 * nd_msgdialog_measure() is what makes it visible, and it reads its numbers
 * from the same pass that draws. These two tests are the only reason a future
 * rewrite of a message string cannot quietly reintroduce the bug. */
static void test_msgdialog_measure_sees_the_clip(void)
{
    fixture fx;
    size_t needed = 0u, fits = 0u;

    /* The message that shipped clipped. Kept verbatim rather than described,
     * because the point of this test is that THIS string does not fit. */
    static const char SHIPPED_CLIPPED[] =
        "Modem ERROR!\n\nYou may need to restart the device. If this does "
        "not fix the issue, there is a potential hardware fault.";

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, &fx.ui, SHIPPED_CLIPPED);
        nd_msgdialog_set_title(&dlg, "Modem");
        nd_msgdialog_set_icon(&dlg, ND_PATH_WARNING_ICON);
        nd_msgdialog_measure(&dlg, &needed, &fits);

        /* 145 content_bottom, a body starting at 38 (clear of the 24 px
         * triangle plus 6), an 8 px bottom margin and 18 px lines: five. */
        CHECK_INT((int)fits, 5);
        /* "Modem ERROR!", a blank the "\n\n" costs in full, and four lines of
         * prose. Two of them had nowhere to go. */
        CHECK_INT((int)needed, 7);
        CHECK(needed > fits);
    }

    /* And it does not cry wolf: two short lines fit with room over. */
    {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, &fx.ui, "LOW BATTERY!");
        nd_msgdialog_measure(&dlg, &needed, &fits);
        CHECK(needed <= fits);
        CHECK_INT((int)needed, 1);
    }

    fx_free(&fx);
}

/* The message the phone actually shows, built exactly the way
 * nd_ui_show_pending_modem_fault() builds it -- same title, same icon. If this
 * fails, the notice is being cut off on the device again. */
static void test_the_modem_fault_message_fits(void)
{
    fixture fx;
    nd_msgdialog dlg;
    size_t needed = 0u, fits = 0u;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }

    nd_msgdialog_init(&dlg, &fx.ui, ND_UI_MODEM_FAULT_MESSAGE);
    nd_msgdialog_set_title(&dlg, "Modem");
    nd_msgdialog_set_icon(&dlg, ND_PATH_WARNING_ICON);
    nd_msgdialog_set_button(&dlg, "OK");
    nd_msgdialog_measure(&dlg, &needed, &fits);

    CHECK_INT((int)fits, 5);
    CHECK(needed <= fits); /* THE INVARIANT. Nothing below is as important. */
    /* A line spare, deliberately. At needed == fits a slightly taller title,
     * a font with a deeper "Ag" or one more word reclips it with no warning,
     * and the whole problem with this dialog is that it gives no warning. */
    CHECK(needed < fits);

    /* Drawn as well as measured, so the string is known to survive a real
     * render -- but note what this second check does NOT prove. A dark band at
     * the bottom means the renderer respected max_lines. It says nothing about
     * whether text was thrown away to achieve that: the older
     * test_msgdialog_invisible_ellipsis() asserts exactly this band and passed
     * happily for every day the modem message was being cut off. Only the
     * needed <= fits check above can tell. */
    nd_msgdialog_render(&dlg);
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 138, 239, 144)), 0);

    fx_free(&fx);
}

static void test_msgdialog_keys(void)
{
    fixture fx;
    nd_msgdialog dlg;
    static const int32_t ACCEPT[] = {ND_KEY_ENTER, ND_KEY_1};

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_msgdialog_init(&dlg, &fx.ui, "x");
    nd_msgdialog_set_title(&dlg, "Notice");
    nd_msgdialog_set_icon(&dlg, ND_PATH_WARNING_ICON);
    nd_msgdialog_set_button(&dlg, "OK");
    nd_msgdialog_set_keys(&dlg, ACCEPT, ND_ARRAY_LEN(ACCEPT), NULL, 0u);
    CHECK_INT(dlg.n_accept, 2);
    CHECK_INT(dlg.accept_keys[1], ND_KEY_1);
    /* cancel_keys=() -- the un-cancellable notice the low-battery shutdown
     * uses. Python coerces with `tuple(x or ())`. */
    CHECK_INT(dlg.n_cancel, 0);

    /* A title pushes the body down past max(8 + th + 6, 8 + icon.h + 6). */
    nd_msgdialog_render(&dlg);
    CHECK(px_lit(fx.canvas, 38 + 1, 8 + 3)); /* the "N" of "Notice" at 18 px */
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 3. TextScroller
 * ------------------------------------------------------------------ */

/* spec-ui-framework.md's measured pagination:
 *   TextScroller(ui, "One.\n\nTwo.\n\nThree.")._paginate() ->
 *   [[('One.',25), ('',8), ('Two.',25), ('',8), ('Three.',25)]], line_h 25.
 * A blank line costs 8 px, not 25, and the proof is where the third line
 * lands: 8 + 25 + 8 + 25 + 8 = 74, not 8 + 3*25 = 83. */
static void test_scroller_blank_line_is_a_gap(void)
{
    fixture fx;
    nd_scroller s;
    size_t line_h = 0u;
    nd_rect bbox;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_scroller_init(&s, &fx.ui, "One.\n\nTwo.\n\nThree.", NULL, NULL);
    CHECK_STR(s.more_text, "More");
    CHECK_STR(s.back_text, "Back");
    CHECK_INT(nd_scroller_paginate(&s, &line_h), 1);
    CHECK_INT(line_h, 25);

    CHECK(nd_scroller_draw(&s)); /* one page, so it is the last page */

    nd_text_bbox(fx.ui.font_n, "One.", &bbox);
    CHECK_INT(first_lit_row(fx.canvas, 0, 144), 8 + bbox.y0);
    nd_text_bbox(fx.ui.font_n, "Two.", &bbox);
    CHECK_INT(first_lit_row(fx.canvas, 8 + 25, 144), 41 + bbox.y0);
    nd_text_bbox(fx.ui.font_n, "Three.", &bbox);
    CHECK_INT(first_lit_row(fx.canvas, 41 + 25, 144), 74 + bbox.y0);

    /* Everything is at the margin: TextScroller never centres. */
    CHECK_INT(first_lit_col(fx.canvas, 8 + bbox.y0 + 2), 10);
    fx_free(&fx);
}

/* An empty source is one empty page, and the strip says "Back". */
static void test_scroller_empty(void)
{
    fixture fx;
    nd_scroller s;
    size_t line_h = 0u;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_scroller_init(&s, &fx.ui, "", "More", "Back");
    CHECK_INT(nd_scroller_paginate(&s, &line_h), 1);
    CHECK(nd_scroller_draw(&s));
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 0, 239, 144)), 0);
    /* The strip is not empty -- "Back" is in it. */
    CHECK(count_lit(fx.canvas, ND_RECT(0, 145, 239, 174)) > 0);
    fx_free(&fx);
}

/* A page never starts with a gap, so paging forward cannot leave 8 px of
 * blank at the top of a screen. */
static void test_scroller_page_never_starts_on_a_gap(void)
{
    fixture fx;
    nd_scroller s;
    size_t line_h = 0u;
    size_t pages;
    nd_rect bbox;
    static const char MANY[] = "A1\n\nA2\n\nA3\n\nA4\n\nA5\n\nA6\n\nA7\n\nA8\n\nA9\n\nB1\n\nB2";

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_scroller_init(&s, &fx.ui, MANY, "More", "Back");
    pages = nd_scroller_paginate(&s, &line_h);
    CHECK(pages > 1u);

    s.page = 1u;
    (void)nd_scroller_draw(&s);
    nd_text_bbox(fx.ui.font_n, "A5", &bbox);
    /* Whatever line page 1 opens with, its ink starts at top + bbox_top and
     * not 8 px lower, which is what a leading gap would look like. */
    CHECK_INT(first_lit_row(fx.canvas, 0, 144), 8 + bbox.y0);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 4. InfoScreen
 * ------------------------------------------------------------------ */

/* value == NULL re-centres the title on its own; "" is NOT the same thing. */
static void test_infoscreen_no_value(void)
{
    fixture fx;
    nd_input *in = NULL;
    int fds[2];
    int32_t th = 0;
    int32_t tw = 0;
    nd_rect bbox;
    int32_t row;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    if (pipe(fds) != 0) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    if (nd_input_open_fd(&in, fds[0]) != ND_OK) {
        CHECK(false);
        (void)close(fds[0]);
        (void)close(fds[1]);
        fx_free(&fx);
        return;
    }
    nd_input_set_repeat(in, 0.0, 0.0);
    fx.ui.input = in;

    {
        struct {
            long tv_sec;
            long tv_usec;
            uint16_t type;
            uint16_t code;
            int32_t value;
        } ev;
        memset(&ev, 0, sizeof ev);
        ev.type = 0x01;
        ev.code = (uint16_t)ND_KEY_ENTER;
        ev.value = 1;
        CHECK_INT(write(fds[1], &ev, sizeof ev), (int)sizeof ev);
        memset(&ev, 0, sizeof ev);
        CHECK_INT(write(fds[1], &ev, sizeof ev), (int)sizeof ev);
    }

    CHECK_INT(nd_infoscreen_show(&fx.ui, "Top score", NULL, "Back"), ND_KEY_ENTER);

    nd_text_size(fx.ui.font_n, "Top score", &tw, &th);
    nd_text_bbox(fx.ui.font_n, "Top score", &bbox);
    row = first_lit_row(fx.canvas, 0, 144);
    CHECK_INT(row, ((145 - th) / 2) + bbox.y0);
    CHECK_INT(first_lit_col(fx.canvas, row), ((240 - tw) / 2) + bbox.x0);

    fx.ui.input = NULL;
    (void)close(fds[1]);
    nd_input_close(in);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 5. ProgressScreen
 * ------------------------------------------------------------------ */

static void detail_mb(void *ctx, int64_t done, int64_t total, char *out, size_t out_sz)
{
    ND_UNUSED(ctx);
    (void)snprintf(out, out_sz, "%.1f of %.1f MB", (double)done / 1048576.0,
                   (double)total / 1048576.0);
}

static void test_progress_boxes(void)
{
    fixture fx;
    nd_progress p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_progress_init(&p, &fx.ui, "Copying", "SOFTWARE UPDATE", "Do not power off", NULL, NULL);

    /* Every one of these is quoted in nd_widgets.h and in
     * spec-ui-framework.md section 15, and all of them are derived. */
    CHECK_INT(p.header_box.x0, 0);
    CHECK_INT(p.header_box.y0, 4);
    CHECK_INT(p.header_box.x1, 240);
    CHECK_INT(p.header_box.y1, 19);
    CHECK_INT(p.divider_y, 24);
    CHECK_INT(p.label_box.y0, 44);
    CHECK_INT(p.label_box.y1, 65);
    CHECK_INT(p.bar_box.x0, 20);
    CHECK_INT(p.bar_box.y0, 79);
    CHECK_INT(p.bar_box.x1, 220);
    CHECK_INT(p.bar_box.y1, 93);
    CHECK_INT(p.status_box.x0, 20);
    CHECK_INT(p.status_box.y0, 102);
    CHECK_INT(p.status_box.x1, 220);
    CHECK_INT(p.status_box.y1, 117);
    CHECK_INT(p.hint_box.y0, 124);
    CHECK_INT(p.hint_box.y1, 139);
    CHECK_INT(p.percent, -1);
    fx_free(&fx);
}

static void test_progress_gate_and_fill(void)
{
    fixture fx;
    nd_progress p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_progress_init(&p, &fx.ui, "Copying", NULL, NULL, NULL, NULL);

    /* 0% draws the outline and no fill at all. */
    CHECK(nd_progress_draw(&p, 0, 100));
    CHECK_INT(p.percent, 0);
    CHECK_INT(count_lit(fx.canvas, ND_RECT(22, 81, 218, 91)), 0);
    CHECK(px_lit(fx.canvas, 20, 79)); /* the frame is there */
    CHECK(px_lit(fx.canvas, 220, 93));

    /* The gate: the same percentage draws nothing and says so. */
    CHECK(!nd_progress_draw(&p, 0, 100));
    CHECK(!nd_progress_draw(&p, 1, 200)); /* still 0% */

    /* 50%: span 196, filled 98, so columns 22..120 inclusive. */
    CHECK(nd_progress_draw(&p, 50, 100));
    CHECK_INT(p.percent, 50);
    CHECK(px_lit(fx.canvas, 120, 86));
    CHECK(!px_lit(fx.canvas, 121, 86));

    /* 100%: filled 196, columns 22..218. */
    CHECK(nd_progress_draw(&p, 100, 100));
    CHECK(px_lit(fx.canvas, 218, 86));
    CHECK_INT(count_lit(fx.canvas, ND_RECT(22, 81, 218, 91)), 197 * 11);

    /* total == 0 means done, not a divide by zero. */
    nd_progress_set_step(&p, "Finishing");
    CHECK_INT(p.percent, -1);
    CHECK(nd_progress_draw(&p, 0, 0));
    CHECK_INT(p.percent, 100);

    /* Nothing is ever drawn on top of the bar's own frame rows. */
    fx_free(&fx);
}

static void test_progress_detail_is_right_aligned(void)
{
    fixture fx;
    nd_progress p;
    char text[64];
    int32_t w = 0;
    int32_t x;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_progress_init(&p, &fx.ui, "Copying", NULL, NULL, detail_mb, NULL);
    CHECK(nd_progress_draw(&p, 5872025, 13002342));

    detail_mb(NULL, 5872025, 13002342, text, sizeof text);
    CHECK_STR(text, "5.6 of 12.4 MB");
    nd_text_size(fx.ui.font_s, text, &w, NULL);
    x = 220 - w;

    /* The reading is at the left edge of the status box and the detail's
     * right edge lands on the box's right edge. */
    CHECK(count_lit(fx.canvas, ND_RECT(20, 102, 40, 117)) > 0);
    CHECK(count_lit(fx.canvas, ND_RECT(x, 102, 220, 117)) > 0);
    CHECK_INT(count_lit(fx.canvas, ND_RECT(221, 102, 239, 117)), 0);
    fx_free(&fx);
}

/* "Backing up your data" does not fit at 20 px; the ladder drops a rung
 * rather than letting the label run off both edges, and _ellipsize is the
 * backstop when even 14 px is too wide. Both must stay inside x in [4, 236]. */
static void test_progress_long_label_stays_on_screen(void)
{
    fixture fx;
    nd_progress p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    nd_progress_init(&p, &fx.ui, "Backing up your data before the update is applied", NULL,
                     "Do not power the phone off while this is running", NULL, NULL);
    CHECK(nd_progress_draw(&p, 45, 100));

    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 0, 3, 144)), 0);
    CHECK_INT(count_lit(fx.canvas, ND_RECT(237, 0, 239, 144)), 0);
    /* The label is above the bar and the hint below the reading; neither may
     * come within 3 px of the bar. */
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 76, 239, 78)), 0);
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 94, 239, 96)), 0);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 6. DetailPage
 * ------------------------------------------------------------------ */

/* spec-ui-framework.md's measured worked example. Every number here was taken
 * off the running Python. */
static void test_detailpage_worked_example(void)
{
    fixture fx;
    nd_detailpage p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    CHECK_INT(nd_detailpage_init(&p, &fx.ui, "NeoDCT 0.3.2a", "12.4 MB", "One.\n\nTwo.", NULL, NULL,
                                 "SOFTWARE UPDATE", "OK"),
              ND_OK);

    CHECK_INT(p.viewport.x0, 0);
    CHECK_INT(p.viewport.y0, 30);
    CHECK_INT(p.viewport.x1, 240);
    CHECK_INT(p.viewport.y1, 143);
    CHECK_INT(p.viewport.y1 - p.viewport.y0, 113);
    CHECK_INT(p.line_height, 18);
    CHECK_INT(p.content_height, 100);
    CHECK_INT(p.body_top, 55);
    CHECK(!p.scrollable);
    CHECK_INT(nd_detailpage_max_offset(&p), 0);

    /* title(27) + subtitle(18) + rule(10) + "One."(18) + gap(9) + "Two."(18) */
    CHECK_INT(p.n_blocks, 6);
    CHECK_INT(p.blocks[0].kind, ND_BLOCK_TEXT);
    CHECK_INT(p.blocks[0].height, 27);
    CHECK_STR(p.blocks[0].text, "NeoDCT 0.3.2a");
    CHECK_INT(p.blocks[1].height, 18);
    CHECK_STR(p.blocks[1].text, "12.4 MB");
    CHECK_INT(p.blocks[2].kind, ND_BLOCK_RULE);
    CHECK_INT(p.blocks[2].height, 10);
    CHECK_INT(p.blocks[3].kind, ND_BLOCK_TEXT);
    CHECK_STR(p.blocks[3].text, "One.");
    CHECK_INT(p.blocks[4].kind, ND_BLOCK_GAP);
    /* A paragraph break is a breath: 0 < gap < line_height. */
    CHECK_INT(p.blocks[4].height, 9);
    CHECK(p.blocks[4].height > 0 && p.blocks[4].height < p.line_height);
    CHECK_INT(p.blocks[5].kind, ND_BLOCK_TEXT);
    CHECK_STR(p.blocks[5].text, "Two.");

    nd_detailpage_draw(&p);
    /* The header row is lit and nothing is drawn below viewport[3]. */
    CHECK(count_lit(fx.canvas, ND_RECT(0, 4, 239, 19)) > 0);
    CHECK(px_lit(fx.canvas, 120, 24)); /* the divider */
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 143, 239, 144)), 0);
    /* Not scrollable, so no scrollbar in the x >= 232 strip. */
    CHECK_INT(count_lit(fx.canvas, ND_RECT(232, 30, 239, 142)), 0);

    nd_detailpage_free(&p);
    CHECK(p.blocks == NULL);
    fx_free(&fx);
}

/* Without a header the viewport starts at 4 and is 139 tall. */
static void test_detailpage_no_header(void)
{
    fixture fx;
    nd_detailpage p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    CHECK_INT(nd_detailpage_init(&p, &fx.ui, "About", NULL, "Body text.", NULL, NULL, NULL, "OK"),
              ND_OK);
    CHECK_INT(p.viewport.y0, 4);
    CHECK_INT(p.viewport.y1 - p.viewport.y0, 139);
    CHECK_STR(p.softkey_text, "OK");
    CHECK_INT(p.accept_keys[0], ND_KEY_ENTER);
    CHECK_INT(p.cancel_keys[0], ND_KEY_CLEAR);
    nd_detailpage_free(&p);
    fx_free(&fx);
}

/* A long body scrolls: the scrollbar appears in the x >= 232 strip, the page
 * starts at the top instead of being centred, scrolling moves by exactly one
 * line_height, and both ends clamp. */
static void test_detailpage_scrolling(void)
{
    fixture fx;
    nd_detailpage p;
    static const char LONG[] =
        "The update replaces the system partition and reboots the phone.\n\n"
        "Nothing in User is touched: contacts, messages and settings all survive.\n\n"
        "If the phone loses power part way through, the previous system is still "
        "there and the next boot falls back to it.\n\n"
        "The whole thing takes about four minutes on a class 10 card, most of which "
        "is the verify pass.\n\n"
        "Do not remove the battery.";
    int32_t max_off;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    CHECK_INT(nd_detailpage_init(&p, &fx.ui, "NeoDCT 0.4.0", "18.2 MB", LONG, NULL, "beta",
                                 "SOFTWARE UPDATE", "Install"),
              ND_OK);
    CHECK(p.scrollable);
    max_off = nd_detailpage_max_offset(&p);
    CHECK(max_off > 0);

    nd_detailpage_draw(&p);
    CHECK(count_lit(fx.canvas, ND_RECT(232, 30, 239, 142)) > 0);
    /* Still nothing below the viewport, scrollbar included. */
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 143, 239, 144)), 0);

    /* Up at the top is silent; Down moves exactly one line. */
    CHECK(!nd_detailpage_handle_key(&p, ND_KEY_UP));
    CHECK(nd_detailpage_handle_key(&p, ND_KEY_DOWN));
    CHECK_INT(p.offset, p.line_height);
    CHECK(!nd_detailpage_handle_key(&p, ND_KEY_ENTER)); /* not a scroll key */

    /* Walk to the bottom and stop there. */
    while (nd_detailpage_handle_key(&p, ND_KEY_DOWN))
        ;
    CHECK_INT(p.offset, max_off);
    CHECK(!nd_detailpage_handle_key(&p, ND_KEY_DOWN));

    /* The last block must not be half-drawn at the fold. */
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 143, 239, 144)), 0);

    nd_detailpage_free(&p);
    fx_free(&fx);
}

/* The hero: picture on the left, title and details in a column beside it, and
 * nothing of the text under the picture. */
static void test_detailpage_hero(void)
{
    fixture fx;
    nd_detailpage p;
    const nd_image *icon;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    icon = nd_ui_get_image_max(&fx.ui, ND_PATH_WARNING_ICON, ND_DETAIL_IMAGE_MAX);
    if (icon == NULL) {
        fprintf(stderr, "test_widgets_dialogs: warning.png not reachable under ND_ROOT\n");
        CHECK(false);
        fx_free(&fx);
        return;
    }

    CHECK_INT(nd_detailpage_init(&p, &fx.ui, "NeoDCT 0.3.2a", "12.4 MB", "What changed.",
                                 ND_PATH_WARNING_ICON, "beta", NULL, "OK"),
              ND_OK);
    CHECK(p.image != NULL);
    CHECK(p.n_blocks >= 2u);
    CHECK_INT(p.blocks[0].kind, ND_BLOCK_HERO);
    CHECK_INT(p.blocks[0].x, ND_DETAIL_MARGIN);

    /* title(+5) + subtitle line + badge = 26 + 18 + 18 = 62 of stack against
     * a 24 px picture, so inner is the stack and the block is stack + 6. */
    CHECK(p.blocks[0].height > 24 + 6);

    nd_detailpage_draw(&p);
    /* The picture is at x = MARGIN and the text column starts to its right. */
    CHECK(count_lit(fx.canvas, ND_RECT(10, 4, 10 + 23, 142)) > 0);
    CHECK(count_lit(fx.canvas, ND_RECT(10 + 24 + 8, 4, 229, 142)) > 0);
    /* Nothing in the gutter to the left of the picture. */
    CHECK_INT(count_lit(fx.canvas, ND_RECT(0, 4, 9, 142)), 0);

    nd_detailpage_free(&p);
    fx_free(&fx);
}

/* CASE B: a picture with neither subtitle nor badge is centred, with the
 * title under it rather than beside it. */
static void test_detailpage_centred_image(void)
{
    fixture fx;
    nd_detailpage p;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    CHECK_INT(
        nd_detailpage_init(&p, &fx.ui, "Warning", NULL, "", ND_PATH_WARNING_ICON, NULL, NULL, "OK"),
        ND_OK);
    CHECK(p.image != NULL);
    CHECK_INT(p.n_blocks, 2);
    CHECK_INT(p.blocks[0].kind, ND_BLOCK_IMAGE);
    CHECK_INT(p.blocks[0].height, 24 + 8);
    CHECK_INT(p.blocks[0].x, (240 - 24) / 2);
    CHECK_INT(p.blocks[1].kind, ND_BLOCK_TEXT);
    CHECK_INT(p.blocks[1].height, 21 + 6);
    nd_detailpage_free(&p);
    fx_free(&fx);
}

/* show() returns the key that left the page, and scrolls on anything else. */
static void test_detailpage_show(void)
{
    fixture fx;
    nd_detailpage p;
    nd_input *in = NULL;
    int fds[2];
    struct {
        long tv_sec;
        long tv_usec;
        uint16_t type;
        uint16_t code;
        int32_t value;
    } ev;
    int32_t i;
    static const int32_t SCRIPT[] = {ND_KEY_DOWN, ND_KEY_DOWN, ND_KEY_ENTER};

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    if (pipe(fds) != 0) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    if (nd_input_open_fd(&in, fds[0]) != ND_OK) {
        CHECK(false);
        (void)close(fds[0]);
        (void)close(fds[1]);
        fx_free(&fx);
        return;
    }
    nd_input_set_repeat(in, 0.0, 0.0);
    fx.ui.input = in;

    for (i = 0; i < (int32_t)ND_ARRAY_LEN(SCRIPT); i++) {
        int32_t v;

        for (v = 1; v >= 0; v--) {
            memset(&ev, 0, sizeof ev);
            ev.type = 0x01;
            ev.code = (uint16_t)SCRIPT[i];
            ev.value = v;
            CHECK_INT(write(fds[1], &ev, sizeof ev), (int)sizeof ev);
            memset(&ev, 0, sizeof ev);
            CHECK_INT(write(fds[1], &ev, sizeof ev), (int)sizeof ev);
        }
    }

    CHECK_INT(nd_detailpage_init(&p, &fx.ui, "Title", "Sub", "Line one.\nLine two.\nLine three.",
                                 NULL, NULL, "HEADER", "OK"),
              ND_OK);
    CHECK_INT(nd_detailpage_show(&p), ND_KEY_ENTER);
    nd_detailpage_free(&p);

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

    if (!stage_root()) {
        fprintf(stderr, "test_widgets_dialogs: cannot stage ND_ROOT\n");
        return 1;
    }
    manifest_open();

    test_msgdialog_alert_look();
    test_msgdialog_paragraph_look();
    test_msgdialog_invisible_ellipsis();
    test_msgdialog_measure_sees_the_clip();
    test_the_modem_fault_message_fits();
    test_msgdialog_keys();

    test_scroller_blank_line_is_a_gap();
    test_scroller_empty();
    test_scroller_page_never_starts_on_a_gap();

    test_infoscreen_no_value();

    test_progress_boxes();
    test_progress_gate_and_fill();
    test_progress_detail_is_right_aligned();
    test_progress_long_label_stays_on_screen();

    test_detailpage_worked_example();
    test_detailpage_no_header();
    test_detailpage_scrolling();
    test_detailpage_hero();
    test_detailpage_centred_image();
    test_detailpage_show();

    test_golden_frames();

    if (g_manifest != NULL)
        nd_json_free(g_manifest);
    drop_stage();

    rc = (g_failures != 0) ? 1 : 0;
    if (rc != 0)
        fprintf(stderr, "test_widgets_dialogs: %d of %d checks FAILED\n", g_failures, g_checks);
    else
        printf("test_widgets_dialogs: %d checks passed\n", g_checks);
    return rc;
}
