/* test_dialer.c -- lib/nd_dialer.c, the two call screens.
 *
 * ============ WHAT THIS FILE CLAIMS ============
 *
 *  1. TWO GOLDEN FRAMES. call-active and call-incoming are re-rendered here
 *     through the real nd_ui and compared by the SHA-256 over raw RGB that
 *     goldenframe.py compares, so a pass here is a pass there. The recipe is
 *     shoot_docs.shoot_telephony()'s, verbatim:
 *
 *         draw_call_screen(ui, "0741234567", name="Mum"); fb.update(canvas)
 *         draw_incoming_screen(ui, "Mum", True);          fb.update(canvas)
 *
 *     Note what is NOT in it: no SoftKeyBar. The draw halves paint the canvas
 *     and stop, and the loops that own them are what normally flush, so both
 *     reference frames have a black softkey strip with the status sprites
 *     overhanging into rows 146..147 and nothing else below y=148. That is
 *     asserted separately in test_no_softkey_bar_in_the_captured_frames(),
 *     because a bar added "for tidiness" would still hash differently and the
 *     digest alone would not say why.
 *
 *  2. GEOMETRY AS NUMBERS. Every coordinate the two screens compute is
 *     asserted as an integer against the value the Python's arithmetic gives
 *     on this panel, so a failure names the number that moved instead of
 *     saying "one frame differs":
 *
 *         handset outline  (8,12)-(26,20), ear (9,13)-(13,15),
 *                          mouth (21,17)-(25,19)
 *         label            x = max(34, int(240*0.23)) = 55
 *                          y = max(50, int(145*0.20)) = 50   (the floor wins)
 *         number           y = 50 + 26 = 76
 *         timer            y = 76 + 24 = 100, and it is GREY, not white
 *         caller           y = max(18, int(145*0.18)) = 26, centred by INK
 *         "calling"        x = 7 + int(36*175/240) + 6 = 39
 *                          y = content_bottom - 26 = 119
 *
 *  3. THE TWO FITTERS DISAGREE, ON PURPOSE. nd_text.h counts six text-fitting
 *     routines in the framework and says to check each against its own Python
 *     original. Two of the six live in nd_dialer.c and they differ in three
 *     ways that all reach the screen: the ellipsis character (U+2026 versus
 *     three full stops), the give-up string ("…" versus "?"), and whether a
 *     zero-length prefix is a candidate at all. Both are driven here through
 *     the only surface they have -- the pixels they produce.
 *
 *  4. THE BLINK IS A QUIRK AND THE QUIRK IS PINNED. show_incoming() sets
 *     blink_on = true and inverts it before the first draw, so the first
 *     frame of a ringing call has NO "calling" label. If somebody ever
 *     "fixes" that, this test fails.
 *
 *  5. THE HAND-OFF TO nd_modem IS REAL. A simulation-mode nd_modem is opened
 *     -- the same one test_modem.c drives -- and the screens are run against
 *     it over a real evdev pipe:
 *
 *         dial -> CALLING -> show_calling -> End -> hangup -> IDLE
 *         a modem that is already IDLE ends show_calling with no key at all
 *         /tmp/neodct_sim_ring -> RINGING -> Answer/Decline/caller-gave-up
 *
 * ============ WHY THE ROOT IS A SYMLINK FARM ============
 *
 * Same reason test_ui.c gives: the frames need /NeoDCT/System (fonts,
 * wallpaper, ui_home.json, the battery and signal sprites) and a writable
 * /NeoDCT/User. System is symlinked at the repo's overlay so nothing under
 * neodct/overlay/ is ever written to.
 *
 * NEODCT_GOLDEN names the reference set; the Makefile passes it. Without it
 * the frame group is skipped and everything else still runs.
 */

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_layout.h"
#include "nd_modem.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* lib/nd_modem_priv.h spells this, but that header also typedefs a
 * `struct nd_lines` of its own and nd_text.h's `nd_lines` is already in scope
 * here through nd_widgets.h. The path is a fixed absolute one on the developer
 * filesystem (nd_modem_sim.c's header comment documents the whole hook), so it
 * is repeated rather than dragged in with a conflicting type. */
#define DIALER_SIM_RING "/tmp/neodct_sim_ring"

static int g_checks;
static int g_failures;
static int g_skips;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
        }                                                                    \
    } while (0)

#define CHECK_INT(got, want, what)                                                              \
    do {                                                                                        \
        long long g_ = (long long)(got);                                                        \
        long long w_ = (long long)(want);                                                       \
        g_checks++;                                                                             \
        if (g_ != w_) {                                                                         \
            g_failures++;                                                                       \
            fprintf(stderr, "FAIL %s:%d  %s: got %lld want %lld\n", __FILE__, __LINE__, (what), \
                    g_, w_);                                                                    \
        }                                                                                       \
    } while (0)

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* ------------------------------------------------------------------ *
 * Finding the reference set and staging a root
 * ------------------------------------------------------------------ */

static char g_stage[ND_PATH_MAX];
static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];
static char g_font[ND_PATH_MAX];

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

/* <repo>/neodct/tests/golden -> <repo>/neodct/overlay, and the font under it.
 * The frame group needs the golden set; everything else needs only the font,
 * so the two are resolved separately. */
static bool find_reference_dirs(void)
{
    const char *env = getenv("NEODCT_GOLDEN");
    char base[ND_PATH_MAX];
    char *cut;

    if (env != NULL && env[0] != '\0')
        (void)nd_strlcpy(g_golden, env, sizeof g_golden);
    else if (file_exists("../tests/golden/manifest.json"))
        (void)nd_strlcpy(g_golden, "../tests/golden", sizeof g_golden);
    else if (file_exists("neodct/tests/golden/manifest.json"))
        (void)nd_strlcpy(g_golden, "neodct/tests/golden", sizeof g_golden);
    else
        return false;

    if (nd_snprintf(base, sizeof base, "%s", g_golden) != ND_OK)
        return false;
    cut = strrchr(base, '/'); /* .../neodct/tests */
    if (cut != NULL)
        *cut = '\0';
    cut = strrchr(base, '/'); /* .../neodct       */
    if (cut != NULL)
        *cut = '\0';
    if (nd_snprintf(g_overlay, sizeof g_overlay, "%s/overlay", base) != ND_OK)
        return false;
    return nd_snprintf(g_font, sizeof g_font, "%s/" FONT_REL, base) == ND_OK && file_exists(g_font);
}

static bool stage_root(void)
{
    char tmpl[ND_PATH_MAX];
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    const char *tmp = getenv("TMPDIR");

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/nddial-XXXXXX", tmp) != ND_OK)
        return false;
    if (mkdtemp(tmpl) == NULL)
        return false;
    (void)nd_strlcpy(g_stage, tmpl, sizeof g_stage);

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        return false;
    if (mkdir(neodct, 0755) != 0 && errno != EEXIST)
        return false;
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

/* uistub.StubUI._prepare_user_dir(): the ack file skips the first-boot modal,
 * and settings.prop carries the wallpaper the reference frames were shot
 * with. */
static void write_settings(const char *wallpaper_name)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_path_resolve(path, sizeof path, "/NeoDCT/User") == ND_OK)
        (void)mkdir(path, 0755);
    if (nd_path_resolve(path, sizeof path, ND_PATH_ACK_SECURITY) == ND_OK) {
        f = fopen(path, "wb");
        if (f != NULL) {
            (void)fputs("0", f);
            (void)fclose(f);
        }
    }
    if (nd_path_resolve(path, sizeof path, ND_PATH_SETTINGS_PROP) != ND_OK)
        return;
    f = fopen(path, "wb");
    if (f == NULL)
        return;
    (void)fputs("system.ui.engineering_mode=ON\n", f);
    if (wallpaper_name != NULL)
        (void)fprintf(f, "system.ui.wallpaper=/NeoDCT/System/wallpapers/%s\n", wallpaper_name);
    (void)fclose(f);
}

/* ------------------------------------------------------------------ *
 * A UI context with nothing behind it but memory
 * ------------------------------------------------------------------ */

/* The geometry cases need fonts and a canvas and nothing else: no fb, no
 * layout, no services. Leaving home_layout NULL is what makes the assertions
 * readable -- the only lit pixels are the ones nd_dialer.c drew itself,
 * without the battery and signal sprites on top of them. */
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
    memset(fx, 0, sizeof *fx);
    fx->font_s = nd_font_load(g_font, ND_FONT_PX_S);
    fx->font_md = nd_font_load(g_font, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(g_font, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(g_font, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL)
        return false;

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
    fx->ui.softkey_exists = true;
    /* This fixture never had a home layout, and these assertions are written
     * against that: render_status_chrome() returns immediately without one,
     * so the regions checked for "nothing here" really are empty. Say so,
     * rather than leaving the flag clear -- since the layout became lazy
     * (nd_ui.h) a clear flag means "not loaded YET", and the first accessor
     * call would go and read ui_home.json off disk. */
    fx->ui.home_.home_layout = NULL;
    fx->ui.home_.home_layout_ready = true;
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
 * Pixel helpers
 * ------------------------------------------------------------------ */

static bool lit(const nd_image *img, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(img, x, y);

    return c.r != 0u || c.g != 0u || c.b != 0u;
}

static bool white(const nd_image *img, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(img, x, y);

    return c.r == 255u && c.g == 255u && c.b == 255u;
}

/* Ink extents of everything lit inside a box. x0/y0 come back as INT32_MAX
 * when the box is empty, which every caller checks by asking for a specific
 * number instead. */
typedef struct {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    int32_t n;
} inkbox;

static inkbox ink_in(const nd_image *img, int32_t bx0, int32_t by0, int32_t bx1, int32_t by1)
{
    inkbox b = {ND_UI_W, ND_UI_H, -1, -1, 0};
    int32_t x;
    int32_t y;

    for (y = by0; y <= by1; y++) {
        for (x = bx0; x <= bx1; x++) {
            if (!lit(img, x, y))
                continue;
            if (x < b.x0)
                b.x0 = x;
            if (y < b.y0)
                b.y0 = y;
            if (x > b.x1)
                b.x1 = x;
            if (y > b.y1)
                b.y1 = y;
            b.n++;
        }
    }
    return b;
}

static int32_t lit_count(const nd_image *img, int32_t bx0, int32_t by0, int32_t bx1, int32_t by1)
{
    return ink_in(img, bx0, by0, bx1, by1).n;
}

/* How many pixels of a band differ from the background the framework would
 * have painted there -- the chrome wallpaper when one is in force, black
 * otherwise. "Something was drawn here" and "nothing was" are what the two
 * softkey-strip assertions below are really about, and this is the only way
 * to say it that does not assume the background is any particular colour. */
static int32_t painted_over(const nd_image *img, const nd_image *bg, int32_t by0, int32_t by1)
{
    int32_t n = 0;
    int32_t x;
    int32_t y;

    for (y = by0; y <= by1; y++) {
        for (x = 0; x < ND_UI_W; x++) {
            nd_color got = nd_image_get_px(img, x, y);
            nd_color want = bg != NULL ? nd_image_get_px(bg, x, y) : ND_BLACK;

            if (got.r != want.r || got.g != want.g || got.b != want.b)
                n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * 1. The handset glyph
 * ------------------------------------------------------------------ */

/* _draw_handset_icon(draw, 8, 10): an outline rectangle and two filled
 * blocks, at the offsets call_screen.py hard-codes. The outline's interior
 * must stay black -- rect_outline is a border, not a fill -- and that is what
 * distinguishes this glyph from a white slab. */
static void test_handset_icon(void)
{
    fixture fx;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP handset: no fonts\n");
        return;
    }

    nd_dialer_draw_call(&fx.ui, "0741234567", NULL);

    /* The outline: (x, y+2) .. (x+18, y+10) with x=8, y=10. */
    CHECK(white(fx.canvas, 8, 12), "handset outline top-left corner");
    CHECK(white(fx.canvas, 26, 12), "handset outline top-right corner");
    CHECK(white(fx.canvas, 8, 20), "handset outline bottom-left corner");
    CHECK(white(fx.canvas, 26, 20), "handset outline bottom-right corner");
    CHECK(!lit(fx.canvas, 7, 12), "nothing one pixel left of the outline");
    CHECK(!lit(fx.canvas, 27, 12), "nothing one pixel right of the outline");
    CHECK(!lit(fx.canvas, 8, 11), "nothing one row above the outline");
    CHECK(!lit(fx.canvas, 8, 21), "nothing one row below the outline");

    /* The ear block: (9,13)..(13,15). */
    CHECK(white(fx.canvas, 9, 13), "ear block top-left");
    CHECK(white(fx.canvas, 13, 15), "ear block bottom-right");
    CHECK(!lit(fx.canvas, 14, 15), "the ear block stops at x=13");
    CHECK(!lit(fx.canvas, 9, 16), "the ear block stops at y=15");

    /* The mouth block: (21,17)..(25,19). */
    CHECK(white(fx.canvas, 21, 17), "mouth block top-left");
    CHECK(white(fx.canvas, 25, 19), "mouth block bottom-right");
    CHECK(!lit(fx.canvas, 20, 17), "the mouth block starts at x=21");
    CHECK(!lit(fx.canvas, 21, 16), "the mouth block starts at y=17");

    /* The silhouette is hollow: row 18 between the ear and the mouth is the
     * outline's two side pixels and nothing else. */
    CHECK_INT(lit_count(fx.canvas, 9, 18, 25, 18), 5, "row 18 inside the handset (mouth only)");
    CHECK_INT(lit_count(fx.canvas, 14, 14, 20, 14), 0, "row 14 between the blocks is empty");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2. The in-call screen's three text rows
 * ------------------------------------------------------------------ */

/* label_x = max(34, int(240 * 0.23)) = max(34, 55) = 55
 * label_y = max(50, int(145 * 0.20)) = max(50, 29) = 50   <- the FLOOR wins
 *
 * The floor winning is worth an assertion of its own: 0.20 of the content
 * area is 29, and every reference frame has the label at 50 because of the
 * max(). Someone dropping the max() would move the label 21 rows up and only
 * this number would say so. */
static void test_call_screen_text_rows(void)
{
    fixture fx;
    inkbox label;
    inkbox number;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP call rows: no fonts\n");
        return;
    }

    nd_dialer_draw_call(&fx.ui, "0741234567", "Mum");

    /* The label. Text y is the ASCENDER LINE and 'C' has bbox_top 2 at 20 px,
     * so the ink starts at 50 + 2 = 52. */
    label = ink_in(fx.canvas, 30, 46, 239, 70);
    CHECK_INT(label.x0, 55, "label ink starts at label_x");
    CHECK_INT(label.y0, 52, "label ink top = label_y + bbox_top(20px)");

    /* The number, 26 px under the label: ascender line 76, ink from 78. */
    number = ink_in(fx.canvas, 30, 72, 239, 96);
    CHECK_INT(number.x0, 55, "the number starts at label_x too");
    CHECK_INT(number.y0, 78, "the number's ink top = 76 + 2");

    /* "0741234567" measures 155 px at font_n -- ten digits, whole-pixel
     * advances, no kerning -- against a budget of 240 - 55 - 10 = 175, so the
     * fitter leaves it alone and it is drawn WHOLE in the preferred font.
     *
     * The LIT columns run 55..206, which is 152 wide, not 155: the leading
     * '0' and the trailing '7' each carry side bearing, and nd_text_size()
     * reports the laid-out box while ink_in() sees only pixels. Both numbers
     * are measured, neither is derived from the other, and the difference is
     * the reason this asserts the ink edge rather than the metric. If the
     * fitter ever shrinks this string the ink gets shorter and it fails. */
    CHECK_INT(number.x1, 206, "the number is drawn whole at font_n");

    /* Nothing between the two rows, and nothing above the label: the contact
     * name is deliberately not drawn. */
    CHECK_INT(lit_count(fx.canvas, 30, 70, 239, 77), 0, "no third row between label and number");
    CHECK_INT(lit_count(fx.canvas, 30, 22, 239, 51), 0, "the name 'Mum' is NOT drawn");

    /* With no modem the status default is ("CONNECTED", None) and secs is
     * None, so there is no timer -- which is the state every golden frame was
     * captured in. */
    CHECK_INT(lit_count(fx.canvas, 0, 96, 239, 145), 0, "no mm:ss without a connected call");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 3. The ring screen's caller line and the flashing label
 * ------------------------------------------------------------------ */

/* caller y = max(18, int(145 * 0.18)) = max(18, 26) = 26, so here the
 * COMPUTED value wins and the floor does not -- the mirror image of the
 * in-call label. Centring is by ink width: "Mum" is 59 px of advance at
 * font_n, (240 - 59) // 2 = 90.
 *
 * calling_x = 7 + int(36 * (175 / 240.0)) + 6 = 7 + 26 + 6 = 39, and the 36
 * is the ASSET's authored width, not the 26 px the sprite actually occupies.
 * The Python's own comment says so. Moving it to 26 shifts the word ten
 * pixels left, which is why it is asserted as a literal. */
static void test_incoming_screen_geometry(void)
{
    fixture fx;
    inkbox caller;
    inkbox calling;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP incoming geometry: no fonts\n");
        return;
    }

    nd_dialer_draw_incoming(&fx.ui, "Mum", true);

    caller = ink_in(fx.canvas, 0, 0, 239, 100);
    CHECK_INT(caller.x0, 90, "'Mum' is centred by ink at (240 - 59) // 2");
    CHECK_INT(caller.x1, 145, "'Mum' ink ends at 145");
    CHECK_INT(caller.y0, 28, "caller ink top = 26 + bbox_top(20px)");

    calling = ink_in(fx.canvas, 0, 101, 239, 174);
    CHECK_INT(calling.x0, 39, "'calling' starts at 7 + int(36*175/240) + 6");
    /* 'l' is the tallest letter in "calling" (bbox_top 2) and 'g' the lowest
     * (a descender), so the ink runs 121..141 off an ascender line of 119. */
    CHECK_INT(calling.y0, 121, "'calling' ink top = 119 + 2");
    CHECK_INT(calling.y1, 141, "'calling' descender bottom");

    fx_free(&fx);
}

/* The blink argument is the whole reason draw_incoming_screen takes one:
 * blink_on=false must leave the bottom of the screen untouched while the
 * caller line stays exactly where it was. */
static void test_incoming_blink_argument(void)
{
    fixture fx;
    inkbox on_caller;
    inkbox off_caller;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP blink argument: no fonts\n");
        return;
    }

    nd_dialer_draw_incoming(&fx.ui, "Mum", true);
    on_caller = ink_in(fx.canvas, 0, 0, 239, 100);
    CHECK(lit_count(fx.canvas, 0, 101, 239, 174) > 0, "blink on draws 'calling'");

    nd_dialer_draw_incoming(&fx.ui, "Mum", false);
    off_caller = ink_in(fx.canvas, 0, 0, 239, 100);
    CHECK_INT(lit_count(fx.canvas, 0, 101, 239, 174), 0, "blink off draws nothing down there");
    CHECK_INT(off_caller.x0, on_caller.x0, "the caller line does not move with the blink");
    CHECK_INT(off_caller.y0, on_caller.y0, "the caller line does not move with the blink (y)");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 4. The two fitters, which disagree on purpose
 * ------------------------------------------------------------------ */

/* nd_text.h: check each fitter against ITS OWN Python original, never against
 * a sibling. These two are siblings and they differ three ways.
 *
 * call_screen._fit_text: preferred font, then font_s, then a binary search
 * for the longest prefix that fits with U+2026 after it. U+2026 has no glyph
 * in this font, so the ellipsis is invisible and still costs its advance --
 * the fitted string is measurably NARROWER than the budget by roughly one
 * ellipsis, and it draws no dots. A '.' would be visible; that asymmetry is
 * what this case detects. */
static void test_call_screen_fitter_falls_back_and_truncates(void)
{
    fixture fx;
    inkbox row;
    static const char LONG[] = "+353 (0) 87 555 0142 extension 7788 ask for accounts";

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP call fitter: no fonts\n");
        return;
    }

    /* A number that fits font_n is drawn at font_n, and its ink is 152
     * columns wide -- see the note in test_call_screen_text_rows() for why
     * that is three less than the 155 the font reports. */
    nd_dialer_draw_call(&fx.ui, "0741234567", NULL);
    row = ink_in(fx.canvas, 30, 72, 239, 96);
    CHECK_INT(row.x1 - row.x0 + 1, 152, "a fitting number keeps font_n's ink width");

    /* Long enough that neither font fits: the search truncates. The result
     * must stay inside the 240 - 55 - 10 = 175 px budget, i.e. end at or
     * before x = 55 + 175 - 1 = 229. */
    nd_dialer_draw_call(&fx.ui, LONG, NULL);
    row = ink_in(fx.canvas, 30, 72, 239, 110);
    CHECK(row.x0 == 55, "the truncated number still starts at label_x");
    CHECK(row.x1 <= 229, "the truncated number stays inside the width budget");
    /* font_s, not font_n, and the ink height is what says so. The fitter
     * settles on the 18-codepoint prefix "+353 (0) 87 555 01" plus U+2026:
     * 168 px at font_s, inside the 175 budget, where the 19th takes it to
     * 179. That same fitted string at font_n measures 248x21 -- more than
     * twice the budget wide and six rows taller -- so a height of 15 here can
     * only have come from the small face. Both numbers are measured against
     * this font, not derived from the pixel sizes. */
    CHECK_INT(row.y1 - row.y0 + 1, 15, "the truncated number fell back to font_s");

    fx_free(&fx);
}

/* incoming_screen._fit_caller_text: font_n, then font_s, then trim one
 * codepoint at a time with three ASCII full stops appended, and "?" when
 * nothing is left. The dots are VISIBLE, unlike the call screen's U+2026, so
 * a long caller name ends in ink where a long number ends in a gap. */
static void test_incoming_fitter_ladder_and_dots(void)
{
    fixture fx;
    inkbox n_row;
    inkbox s_row;
    inkbox trimmed;
    static const char MEDIUM[] = "Grandmother Josephine";
    static const char VERY_LONG[] =
        "Grandmother Josephine Fitzwilliam-Beauchamp of the Northern Approaches";

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP incoming fitter: no fonts\n");
        return;
    }

    /* Short: font_n, tall ink. */
    nd_dialer_draw_incoming(&fx.ui, "Mum", false);
    n_row = ink_in(fx.canvas, 0, 0, 239, 100);
    CHECK_INT(n_row.y1 - n_row.y0 + 1, 18, "a short caller keeps font_n");

    /* Too wide for font_n at 224 px, but font_s fits: the ladder steps down
     * rather than truncating, and the text is still drawn whole. */
    nd_dialer_draw_incoming(&fx.ui, MEDIUM, false);
    s_row = ink_in(fx.canvas, 0, 0, 239, 100);
    CHECK(s_row.x1 - s_row.x0 + 1 <= 224, "the font_s caller fits the 240-16 budget");
    CHECK(s_row.y1 - s_row.y0 + 1 < n_row.y1 - n_row.y0 + 1,
          "the ladder stepped down to font_s rather than truncating");

    /* Too wide even for font_s: trimmed, with three visible dots. The last
     * three ink runs before the right edge are the stops, all on the SAME
     * baseline row, which no letter of this string shares. */
    nd_dialer_draw_incoming(&fx.ui, VERY_LONG, false);
    trimmed = ink_in(fx.canvas, 0, 0, 239, 100);
    CHECK(trimmed.x1 - trimmed.x0 + 1 <= 224, "the trimmed caller fits the budget");
    /* The trimmed string is "Grandmother Josephin" + "...", 216 px at font_s,
     * centred at x = (240 - 216) // 2 = 12 and running to x = 226.
     *
     * The last thing on the line is a full stop, and the way to say so
     * WITHOUT hard-coding a glyph shape is that the rightmost lit column
     * carries ink only near the baseline: its topmost lit row is 36, nine
     * rows below the line's own ink top of 27. A letter in this face reaches
     * the x-height and would put ink within four rows of the top. The naive
     * form of this check -- lit(x1, y1) -- does NOT hold, and that is
     * correct: y1 is 41, set by the descender of the 'p' in "Josephin",
     * which is nowhere near the last column. */
    {
        inkbox lastcol = ink_in(fx.canvas, trimmed.x1, 0, trimmed.x1, 100);

        CHECK(lastcol.n > 0, "the rightmost column of the trimmed caller has ink");
        CHECK(lastcol.y0 - trimmed.y0 >= 8,
              "the trimmed caller ends in a visible stop, low on the line");
        CHECK(lastcol.y1 <= trimmed.y1,
              "and the stop does not reach the descender that sets the line's bottom");
    }

    /* The two fitters disagree, and this is the assertion that says so: the
     * call screen's ellipsis is invisible, so its truncated row ends in the
     * last GLYPH and the ellipsis after it lights nothing. */
    nd_dialer_draw_call(&fx.ui, VERY_LONG, NULL);
    {
        inkbox num = ink_in(fx.canvas, 30, 72, 239, 110);

        CHECK(num.x1 + 4 <= 229,
              "U+2026 costs its advance and draws nothing, so the ink stops short");
    }

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 5. The blocking loops, over a real evdev pipe and a real modem
 * ------------------------------------------------------------------ */

typedef struct {
    nd_input_channel ch;
    nd_input *in;
} keys;

static bool keys_open(keys *k, nd_ui *ui)
{
    memset(k, 0, sizeof *k);
    k->ch.read_fd = -1;
    k->ch.write_fd = -1;

    if (nd_input_channel_open(&k->ch) != ND_OK)
        return false;
    /* nd_input_open_fd() takes the descriptor over, so the channel must not
     * close its read end a second time. */
    if (nd_input_open_fd(&k->in, k->ch.read_fd) != ND_OK) {
        nd_input_channel_close(&k->ch);
        return false;
    }
    k->ch.read_fd = -1;
    nd_input_set_repeat(k->in, 0.0, 0.0);
    ui->input = k->in;
    return true;
}

static bool keys_push(keys *k, int32_t code)
{
    return nd_input_channel_send(&k->ch, code, true) == ND_OK &&
           nd_input_channel_send(&k->ch, code, false) == ND_OK;
}

static void keys_close(keys *k, nd_ui *ui)
{
    ui->input = NULL;
    nd_input_close(k->in);
    nd_input_channel_close(&k->ch);
}

/* nd_modem_open() with no serial port under the scratch root runs in
 * simulation mode -- the same mode test_modem.c's threaded case uses -- so
 * dial/answer/hangup and the /tmp/neodct_sim_ring hook are all real code
 * paths with a real thread behind them. */
static void wait_for_state(nd_modem *m, nd_call_state want)
{
    int spins;

    for (spins = 0; spins < 60 && nd_modem_state(m) != want; spins++) {
        struct timespec ts = {0, 50 * 1000 * 1000};

        (void)nanosleep(&ts, NULL);
    }
}

/* show_calling(): the modem is CALLING, End hangs up, and the hangup really
 * reaches the modem -- the state afterwards is IDLE, not CALLING. This is the
 * whole hand-off the work package is about, end to end. */
static void test_show_calling_ends_the_call(void)
{
    fixture fx;
    nd_modem *m = NULL;
    keys k;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP show_calling: no fonts\n");
        return;
    }
    if (nd_modem_open(&m) != ND_OK || m == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP show_calling: no modem\n");
        fx_free(&fx);
        return;
    }
    fx.ui.modem = m;

    CHECK(nd_modem_dial(m, "0741234567"), "the dial reached the modem");
    CHECK_INT(nd_modem_state(m), ND_CALL_CALLING, "dialling leaves the modem CALLING");

    if (!keys_open(&k, &fx.ui)) {
        g_skips++;
        fprintf(stderr, "SKIP show_calling: no key channel\n");
        nd_modem_close(m);
        fx_free(&fx);
        return;
    }
    CHECK(keys_push(&k, ND_KEY_CLEAR), "queued the End key");

    nd_dialer_show_calling(&fx.ui, "0741234567", "Mum");

    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE, "End hung the call up for real");
    /* The screen it left behind is the in-call screen: the handset glyph is
     * still in the corner. */
    CHECK(white(fx.canvas, 8, 12), "show_calling drew the in-call screen");

    keys_close(&k, &fx.ui);
    fx.ui.modem = NULL;
    nd_modem_close(m);
    fx_free(&fx);
}

/* The remote-hangup path: an IDLE modem ends the loop with NO key at all.
 * call_screen.py checks the state AFTER read_keypress and BEFORE the key, so
 * one 0.1 s timeout is the whole cost. */
static void test_show_calling_returns_when_the_far_end_goes(void)
{
    fixture fx;
    nd_modem *m = NULL;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP remote hangup: no fonts\n");
        return;
    }
    if (nd_modem_open(&m) != ND_OK || m == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP remote hangup: no modem\n");
        fx_free(&fx);
        return;
    }
    fx.ui.modem = m;
    CHECK_INT(nd_modem_state(m), ND_CALL_IDLE, "a fresh modem is IDLE");

    /* No key channel and no queued key: if the IDLE check were missing this
     * would never return and the test would hang, which is the failure mode
     * worth having. */
    nd_dialer_show_calling(&fx.ui, "0741234567", NULL);
    CHECK(true, "show_calling returned on an IDLE modem without a keypress");

    fx.ui.modem = NULL;
    nd_modem_close(m);
    fx_free(&fx);
}

/* show_incoming(): Answer (28) and Decline (14) map to the two results
 * nd_ui_handle_incoming_call() switches on, and the caller-gave-up case comes
 * back as ND_CALL_GONE without any key being pressed. */
static void test_show_incoming_results(void)
{
    fixture fx;
    nd_modem *m = NULL;
    keys k;
    nd_incoming_result r;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP show_incoming: no fonts\n");
        return;
    }
    if (nd_modem_open(&m) != ND_OK || m == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP show_incoming: no modem\n");
        fx_free(&fx);
        return;
    }
    fx.ui.modem = m;
    /* nd_ui_handle_incoming_call() sets this before it calls in here, and the
     * ring screen does not work without it: _ring_tick runs on every
     * nd_ui_read_keypress() and, with a RINGING modem and handling_call
     * false, every read returns ND_KEY_INCOMING_CALL instead of the key. The
     * Python has the same shape -- poll_modem() raises IncomingCall out of
     * read_keypress() -- so this line is part of the screen's contract with
     * the core, not test scaffolding. */
    fx.ui.handling_call = true;
    if (!keys_open(&k, &fx.ui)) {
        g_skips++;
        fprintf(stderr, "SKIP show_incoming: no key channel\n");
        nd_modem_close(m);
        fx_free(&fx);
        return;
    }

    /* One write to the sim ring file is one ring; the modem thread picks it
     * up without anybody polling, which is decision 1's whole point. */
    {
        char path[ND_PATH_MAX];
        FILE *f;

        if (nd_path_resolve(path, sizeof path, DIALER_SIM_RING) == ND_OK) {
            f = fopen(path, "wb");
            if (f != NULL) {
                (void)fputs("5559876\n", f);
                (void)fclose(f);
            }
        }
    }
    wait_for_state(m, ND_CALL_RINGING);
    CHECK_INT(nd_modem_state(m), ND_CALL_RINGING, "the sim ring reached the modem");

    CHECK(keys_push(&k, ND_KEY_ENTER), "queued Answer");
    r = nd_dialer_show_incoming(&fx.ui, "5559876", NULL);
    CHECK_INT(r, ND_CALL_ANSWERED, "28 answers");

    CHECK(keys_push(&k, ND_KEY_CLEAR), "queued Decline");
    r = nd_dialer_show_incoming(&fx.ui, "5559876", NULL);
    CHECK_INT(r, ND_CALL_DECLINED, "14 declines");

    /* The caller gives up: removing the file drops the modem back to IDLE,
     * and the ring screen leaves with GONE and no keypress. */
    {
        char path[ND_PATH_MAX];

        if (nd_path_resolve(path, sizeof path, DIALER_SIM_RING) == ND_OK)
            (void)remove(path);
    }
    wait_for_state(m, ND_CALL_IDLE);
    r = nd_dialer_show_incoming(&fx.ui, "5559876", NULL);
    CHECK_INT(r, ND_CALL_GONE, "an IDLE modem means the caller gave up");

    keys_close(&k, &fx.ui);
    fx.ui.modem = NULL;
    nd_modem_close(m);
    fx_free(&fx);
}

/* The quirk from incoming_screen.py:100-115: blink_on starts true and the
 * first pass inverts it, so the FIRST frame a ringing call puts up has no
 * "calling" on it. Driven through show_incoming() rather than asserted about
 * the source, so it stays true however the loop is written.
 *
 * With no modem attached the state check is skipped entirely and the queued
 * Answer key ends the loop after exactly one drawn frame. */
static void test_first_ring_frame_has_the_label_off(void)
{
    fixture fx;
    keys k;
    nd_incoming_result r;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP first ring frame: no fonts\n");
        return;
    }
    if (!keys_open(&k, &fx.ui)) {
        g_skips++;
        fprintf(stderr, "SKIP first ring frame: no key channel\n");
        fx_free(&fx);
        return;
    }

    CHECK(keys_push(&k, ND_KEY_ENTER), "queued Answer");
    r = nd_dialer_show_incoming(&fx.ui, "5559876", "Mum");
    CHECK_INT(r, ND_CALL_ANSWERED, "28 answers with no modem attached");

    /* The caller line is there... */
    CHECK(lit_count(fx.canvas, 0, 0, 239, 100) > 0, "the first ring frame drew the caller");

    /* ...and "calling" is NOT, because the first pass flipped blink_on off.
     *
     * The band stops at 144, one row short of the softkey strip. show_incoming
     * builds a bar and paints "Answer" on it at y_start = 175 - 30 = 145, so a
     * band that ran to 174 would count the bar's ink and never be zero however
     * the blink behaved. "calling" itself sits at content_bottom - 26 = 119
     * with its ink from 121, comfortably inside what is measured here. */
    CHECK_INT(lit_count(fx.canvas, 0, 101, 239, 144), 0,
              "the FIRST ring frame has the blink OFF (ported quirk)");

    /* And the bar really is the reason the rows below are lit: the ring
     * screen's softkey says "Answer", drawn with present=false so the one
     * present that follows is the whole frame. */
    CHECK(lit_count(fx.canvas, 0, 145, 239, 174) > 0, "the ring screen paints an Answer softkey");

    keys_close(&k, &fx.ui);
    fx_free(&fx);
}

/* caller_label(): `name or lookup(number) or number or "Unknown"`. Python's
 * `or` treats "" as falsy, so an empty name falls through. There is no
 * phonebook under the scratch root, so the lookup is a miss and the number
 * stands; an empty number lands on "Unknown", which is wider than a short
 * number and therefore distinguishable by ink alone. */
static void test_caller_label_fallbacks(void)
{
    fixture fx;
    keys k;
    inkbox named;
    inkbox numbered;
    inkbox unknown;

    if (!fx_init(&fx)) {
        g_skips++;
        fprintf(stderr, "SKIP caller label: no fonts\n");
        return;
    }
    if (!keys_open(&k, &fx.ui)) {
        g_skips++;
        fprintf(stderr, "SKIP caller label: no key channel\n");
        fx_free(&fx);
        return;
    }

    CHECK(keys_push(&k, ND_KEY_ENTER), "queued Answer (name)");
    (void)nd_dialer_show_incoming(&fx.ui, "5559876", "Mum");
    named = ink_in(fx.canvas, 0, 0, 239, 100);

    CHECK(keys_push(&k, ND_KEY_ENTER), "queued Answer (empty name)");
    (void)nd_dialer_show_incoming(&fx.ui, "5559876", "");
    numbered = ink_in(fx.canvas, 0, 0, 239, 100);

    CHECK(keys_push(&k, ND_KEY_ENTER), "queued Answer (nothing at all)");
    (void)nd_dialer_show_incoming(&fx.ui, "", NULL);
    unknown = ink_in(fx.canvas, 0, 0, 239, 100);

    /* "Mum" (3 chars) is narrower than "5559876" (7 digits), which is
     * narrower than "Unknown" is at the same size -- the point is only that
     * all three differ, i.e. each fallback really fired. */
    CHECK(named.x1 - named.x0 < numbered.x1 - numbered.x0,
          "an empty name falls through to the number");
    CHECK(unknown.n > 0, "no name and no number still draws something");
    CHECK(unknown.x1 - unknown.x0 != numbered.x1 - numbered.x0,
          "no number at all draws 'Unknown', not the empty string");

    keys_close(&k, &fx.ui);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 6. The golden frames
 * ------------------------------------------------------------------ */

static const char *golden_digest(const nd_json_doc *doc, const char *name)
{
    const nd_json_val *frames;
    size_t n;
    size_t i;

    if (doc == NULL)
        return NULL;
    frames = nd_json_get(nd_json_root(doc), "frames");
    if (frames == NULL || nd_json_type_of(frames) != ND_JSON_ARRAY)
        return NULL;
    n = nd_json_len(frames);
    for (i = 0u; i < n; i++) {
        const nd_json_val *e = nd_json_at(frames, i);

        if (strcmp(nd_json_get_str(e, "name", ""), name) == 0)
            return nd_json_get_str(e, "sha256", NULL);
    }
    return NULL;
}

static nd_json_doc *load_golden_manifest(void)
{
    char path[ND_PATH_MAX];
    FILE *f;
    uint8_t *buf;
    long len;
    size_t got;
    nd_json_doc *doc = NULL;

    if (nd_snprintf(path, sizeof path, "%s/manifest.json", g_golden) != ND_OK)
        return NULL;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len);
    if (buf == NULL) {
        (void)fclose(f);
        return NULL;
    }
    got = fread(buf, 1u, (size_t)len, f);
    (void)fclose(f);
    if (got == (size_t)len)
        (void)nd_json_parse(buf, got, &doc, NULL, 0u);
    free(buf);
    return doc;
}

static void check_frame(const nd_json_doc *golden, const char *name, const nd_image *img)
{
    char digest[65];
    const char *want;

    if (img == NULL) {
        CHECK(false, name);
        return;
    }
    if (nd_capture_digest(img, digest, sizeof digest) != ND_OK) {
        CHECK(false, "digest failed");
        return;
    }
    want = golden_digest(golden, name);
    if (want == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP %s: no golden digest\n", name);
        return;
    }
    g_checks++;
    if (strcmp(digest, want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL frame %-16s got %s\n%21swant %s\n", name, digest, "", want);
    }
}

/* Neither draw half builds a SoftKeyBar -- shoot_telephony() calls them and
 * flushes by hand -- so rows 148..174 carry no softkey LABEL, while rows
 * 146..147 do carry ink: the status sprites are 26x131 at y=17 and reach
 * y=148, three rows past content_bottom, and on the home screen the bar
 * covers that overhang. Here nothing does. Asserted as two separate numbers
 * so "the bar came back" and "the sprites stopped overhanging" fail
 * differently.
 *
 * AGAINST THE BACKGROUND, not against black. This used to count any non-black
 * pixel, and the overhang it was detecting is antialiasing whose brightest
 * channel is 25 -- so the check worked only while the background was exactly
 * zero, and wallpaper-everywhere ends that. Comparing with what the framework
 * would have painted says the same two things ("the sprites reach here",
 * "nothing drew here") on any background, including the old black one. */
static void check_no_softkey_bar(nd_ui *ui, const nd_image *img, const char *what)
{
    const nd_image *bg;

    if (img == NULL)
        return;
    bg = nd_ui_chrome_wallpaper(ui);
    CHECK(painted_over(img, bg, 146, 147) > 0, what);
    CHECK_INT(painted_over(img, bg, 148, 174), 0, what);
}

static void shoot_dialer_frames(nd_capture *cap, const nd_json_doc *golden)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;

    /* shoot_telephony()'s block: one StubUI, wallpaper Palestine.jpg, a
     * healthy simulated phone, and a fresh virtual clock. Both call screens
     * come out of it before home-sms-banner does, which is what makes that
     * frame the block's third -- see OPEN-QUESTIONS.md S-3. */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init for the dialer frames");
        nd_vclock_disable();
        return;
    }
    CHECK(nd_ui_home_layout(&ui) != NULL,
          "the home layout loaded (the screens borrow its elements)");
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    nd_dialer_draw_call(&ui, "0741234567", "Mum");
    (void)nd_ui_present(&ui);
    CHECK_INT(nd_vclock_frame(), 1, "call-active is the block's first frame");
    check_frame(golden, "call-active", nd_capture_recent(cap, 0u));
    check_no_softkey_bar(&ui, nd_capture_recent(cap, 0u), "call-active has no softkey bar");

    nd_dialer_draw_incoming(&ui, "Mum", true);
    (void)nd_ui_present(&ui);
    CHECK_INT(nd_vclock_frame(), 2, "call-incoming is the block's second frame");
    check_frame(golden, "call-incoming", nd_capture_recent(cap, 0u));
    check_no_softkey_bar(&ui, nd_capture_recent(cap, 0u), "call-incoming has no softkey bar");

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    nd_capture *cap = NULL;
    nd_json_doc *golden = NULL;

    if (!find_reference_dirs()) {
        fprintf(stderr, "test_dialer: cannot find the reference set; set NEODCT_GOLDEN\n");
        return 1;
    }
    if (!stage_root()) {
        fprintf(stderr, "test_dialer: cannot stage a root\n");
        return 1;
    }
    write_settings(NULL);

    test_handset_icon();
    test_call_screen_text_rows();
    test_incoming_screen_geometry();
    test_incoming_blink_argument();
    test_call_screen_fitter_falls_back_and_truncates();
    test_incoming_fitter_ladder_and_dots();

    /* The ring case goes first of the four that open a modem: it is the only
     * one that writes /tmp/neodct_sim_ring, and nd_modem_sim.c tracks that
     * file by mtime, so a leftover from a later case would make the next
     * modem it opens believe a call is already coming in. */
    test_show_incoming_results();
    test_show_calling_ends_the_call();
    test_show_calling_returns_when_the_far_end_goes();
    test_first_ring_frame_has_the_label_off();
    test_caller_label_fallbacks();

    golden = load_golden_manifest();
    if (golden == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP frames: %s/manifest.json did not parse\n", g_golden);
    } else {
        if (nd_capture_open(&cap, "/frames", 0u) == ND_OK) {
            shoot_dialer_frames(cap, golden);
            nd_capture_close(cap);
        } else {
            CHECK(false, "nd_capture_open");
        }
        nd_json_free(golden);
    }

    drop_stage();

    if (g_failures != 0) {
        fprintf(stderr, "test_dialer: %d of %d checks FAILED (%d skipped)\n", g_failures, g_checks,
                g_skips);
        return 1;
    }
    printf("test_dialer: %d checks passed (%d skipped)\n", g_checks, g_skips);
    return 0;
}
