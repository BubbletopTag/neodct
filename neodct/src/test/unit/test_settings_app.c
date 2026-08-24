/* test_settings_app.c -- the Settings app, app id 4.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The strings are the Python's, including both halves of the "Get
 *     more..." help -- which one is shown depends on whether a card is in the
 *     phone, and getting that backwards would tell a user with no card to
 *     copy files onto it -- and the whole of SDCARD_HELP, whose two-space
 *     indents TextScroller really draws.
 *
 *  2. ENGINEERING_MODE_KEY is the key the CORE reads. This app's only job
 *     after Decision 3 is to write a setting; writing it under a key
 *     nd_ui_refresh_after_app() does not read would be a silent no-op, so the
 *     app's spelling and nd_settings.h's are asserted equal.
 *
 *  3. SUPPORTED_WALLPAPERS is (".jpg", ".jpeg") -- a real two-element tuple,
 *     unlike Tones' `(".mp3")` -- matched case-insensitively and ASCII-only.
 *
 *  4. The display name is os.path.splitext(os.path.basename(f))[0], including
 *     splitext's rule that leading dots belong to the name.
 *
 *  5. _scan_wallpapers() walks every wallpaper directory, descends into
 *     subdirectories, keeps only .jpg/.jpeg, and sorts by name.lower().
 *     _wallpaper_dirs() puts the stock directory first.
 *
 *  6. _wrap_text() is the SEVENTH wrapper and behaves like none of the other
 *     six: newlines are lost, an empty string gives ONE empty line, and an
 *     over-wide word is left over-wide rather than ellipsised. That last one
 *     is the only line that differs from Messages' otherwise identical
 *     wrapper, so it is asserted directly.
 *
 *  7. _show_about() draws the geometry the Python draws: the divider from
 *     line_pad to screen_w - line_pad at header_y, and grey body text.
 *
 *  8. DECISION 3 END TO END. Picking a wallpaper writes system.ui.wallpaper
 *     and picking "Get more..." writes nothing; picking Off writes
 *     system.ui.engineering_mode=OFF. Nothing reaches into the ui.
 *
 *  9. THE TWO GOLDEN FRAMES. app-settings is run()'s VerticalList and
 *     app-settings-wallpaper is the picker after one Enter, both judged by
 *     the SHA-256 over raw RGB that goldenframe.py compares.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_settings.h"
#include "nd_storage.h"
#include "nd_text.h"

#include "smallapp_test.h"

#include "../../apps/Settings/settings_app.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*is_supported)(const char *);
    const char *(*display_name)(const char *, char *, size_t);
    size_t (*dirs)(char (*)[ND_SETAPP_PATH_MAX], size_t);
    size_t (*scan)(nd_wallpaper *, size_t);
    void (*wrap)(nd_lines *, nd_ui *, const char *, int32_t, const nd_font *);
    void (*draw_about)(nd_ui *);
    const char *const *get_more_label;
    const char *const *get_more_help;
    const char *const *get_more_help_with_card;
    const char *const *sdcard_help;
    const char *const *menu;
    const char *const *eng_options;
    const char *const *exts;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.is_supported = sa_sym(h, "nd_setapp_is_supported");
    *(void **)&api.display_name = sa_sym(h, "nd_setapp_display_name");
    *(void **)&api.dirs = sa_sym(h, "nd_setapp_wallpaper_dirs");
    *(void **)&api.scan = sa_sym(h, "nd_setapp_scan");
    *(void **)&api.wrap = sa_sym(h, "nd_setapp_wrap_text");
    *(void **)&api.draw_about = sa_sym(h, "nd_setapp_draw_about");
    api.get_more_label = dlsym(h, "nd_setapp_get_more_label");
    api.get_more_help = dlsym(h, "nd_setapp_get_more_help");
    api.get_more_help_with_card = dlsym(h, "nd_setapp_get_more_help_with_card");
    api.sdcard_help = dlsym(h, "nd_setapp_sdcard_help");
    api.menu = dlsym(h, "nd_setapp_menu");
    api.eng_options = dlsym(h, "nd_setapp_eng_options");
    api.exts = dlsym(h, "nd_setapp_exts");

    return api.run != NULL && api.shutdown != NULL && api.is_supported != NULL &&
           api.display_name != NULL && api.dirs != NULL && api.scan != NULL && api.wrap != NULL &&
           api.draw_about != NULL && api.get_more_label != NULL && api.get_more_help != NULL &&
           api.get_more_help_with_card != NULL && api.sdcard_help != NULL && api.menu != NULL &&
           api.eng_options != NULL && api.exts != NULL;
}

static char g_root[ND_PATH_MAX];    /* the controlled tree              */
static char g_wproot[ND_PATH_MAX];  /* System/wallpapers -> the overlay */
static char g_saved_root[ND_PATH_MAX];

/* ------------------------------------------------------------------ *
 * 1 and 2. The strings, and the key the core reads
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    CHECK_STR(api.menu[0], "Wallpaper", "the root menu's first row");
    CHECK_STR(api.menu[1], "Memory card", "second");
    CHECK_STR(api.menu[2], "Engineering Mode", "third");
    CHECK_STR(api.menu[3], "About", "fourth");
    CHECK_STR(api.eng_options[0], "On", "Eng. Mode[0]");
    CHECK_STR(api.eng_options[1], "Off", "Eng. Mode[1]");
    CHECK_STR(*api.get_more_label, "Get more...", "GET_MORE_LABEL");
    CHECK_STR(api.exts[0], ".jpg", "SUPPORTED_WALLPAPERS[0]");
    CHECK_STR(api.exts[1], ".jpeg", "SUPPORTED_WALLPAPERS[1]");

    CHECK_STR(*api.get_more_help,
              "Get more wallpapers by adding an SD card!\n"
              "\n"
              "Format a card as FAT32, make a folder called \"wallpapers\" on it, and "
              "copy your .jpg files into it.\n"
              "\n"
              "240x240 pictures look best. Put the card in the phone and they appear in "
              "this list. The phone can set a blank card up for you.",
              "GET_MORE_HELP");
    CHECK_STR(*api.get_more_help_with_card,
              "Get more wallpapers from your SD card!\n"
              "\n"
              "Copy .jpg files into the \"wallpapers\" folder on the card that is in "
              "the phone and they appear in this list. 240x240 looks best.",
              "GET_MORE_HELP_WITH_CARD");
    CHECK_STR(*api.sdcard_help,
              "A NeoDCT memory card is a FAT32 card with these folders on it:\n"
              "\n"
              "  wallpapers   .jpg pictures\n"
              "  tones        .mp3 ringtones\n"
              "  music        your music\n"
              "  backup_db    copies of your contacts\n"
              "  update       UPDATE.ndsw system updates\n"
              "\n"
              "You can make one on a computer, or let the phone do it. Setting up only "
              "adds the folders. Formatting erases everything on the card.",
              "SDCARD_HELP");

    CHECK_STR(ND_SETAPP_SYSTEM_WALLPAPER_DIR, "/NeoDCT/System/wallpapers", "SYSTEM_WALLPAPER_DIR");
    CHECK_STR(ND_SETAPP_WALLPAPER_DIR, "/NeoDCT/User/wallpapers", "WALLPAPER_DIR");
    CHECK_STR(ND_SETAPP_SDCARD_HELPER, "/NeoDCT/System/hw/neodct-sdcard", "SDCARD_HELPER");
    CHECK_INT(ND_SETAPP_ROOT_ID, 4, "ROOT_ID");

    /* The whole of Decision 3 rests on this: the app writes a setting, the
     * core re-reads it. Different spellings and nothing happens. */
    CHECK_STR(ND_SETAPP_ENG_KEY, ND_SET_UI_ENGINEERING, "ENGINEERING_MODE_KEY is the core's key");
    CHECK_STR(ND_SETAPP_WALLPAPER_NONE, "NONE", "the \"no wallpaper\" magic value");
}

/* ------------------------------------------------------------------ *
 * 3 and 4. The two string helpers
 * ------------------------------------------------------------------ */

static void test_is_supported(void)
{
    CHECK(api.is_supported("Grasslands.jpg"), "lower case .jpg");
    CHECK(api.is_supported("Grasslands.JPG"), "upper case");
    CHECK(api.is_supported("Grasslands.JpG"), "mixed case");
    CHECK(api.is_supported("Grasslands.jpeg"), ".jpeg is in the tuple too");
    CHECK(api.is_supported("Grasslands.JPEG"), "upper case .jpeg");
    CHECK(api.is_supported(".jpg"), "a file called nothing but the extension");
    CHECK(!api.is_supported("Grasslands.png"), "png is not in SUPPORTED_WALLPAPERS");
    CHECK(!api.is_supported("Grasslands.jpg.txt"), "the extension has to be last");
    CHECK(!api.is_supported("jpg"), "no dot");
    CHECK(!api.is_supported("jpeg"), "no dot, longer");
    CHECK(!api.is_supported(""), "the empty name");
    CHECK(!api.is_supported(NULL), "NULL");
}

static void expect_name(const char *filename, const char *want)
{
    char out[ND_SETAPP_NAME_MAX];

    CHECK_STR(api.display_name(filename, out, sizeof out), want, filename);
}

static void test_display_name(void)
{
    expect_name("Grasslands.jpg", "Grasslands");
    expect_name("/NeoDCT/System/wallpapers/90s throwback.jpg", "90s throwback");
    expect_name("no-extension", "no-extension");
    expect_name("two.dots.jpeg", "two.dots");
    /* splitext(".hidden") is (".hidden", "") */
    expect_name(".hidden", ".hidden");
    expect_name(".hidden.jpg", ".hidden");
    expect_name("", "");
}

/* ------------------------------------------------------------------ *
 * 5. The scan
 * ------------------------------------------------------------------ */

static bool write_file(const char *logical)
{
    char real[ND_PATH_MAX];
    FILE *f;

    if (nd_path_resolve(real, sizeof real, logical) != ND_OK)
        return false;
    f = fopen(real, "w");
    if (f == NULL)
        return false;
    (void)fputs("not really a jpeg\n", f);
    (void)fclose(f);
    return true;
}

static void build_wallpaper_tree(void)
{
    CHECK_INT(nd_mkdir_p(ND_SETAPP_SYSTEM_WALLPAPER_DIR "/sub", 0755u), ND_OK, "stock directory");
    CHECK_INT(nd_mkdir_p(ND_SETAPP_WALLPAPER_DIR, 0755u), ND_OK, "user directory");
    CHECK_INT(nd_mkdir_p("/NeoDCT/User", 0755u), ND_OK, "the user partition");

    CHECK(write_file(ND_SETAPP_SYSTEM_WALLPAPER_DIR "/Beta.jpg"), "Beta.jpg");
    CHECK(write_file(ND_SETAPP_SYSTEM_WALLPAPER_DIR "/alpha.JPEG"), "alpha.JPEG");
    CHECK(write_file(ND_SETAPP_SYSTEM_WALLPAPER_DIR "/notes.txt"), "notes.txt");
    CHECK(write_file(ND_SETAPP_SYSTEM_WALLPAPER_DIR "/Zeta.png"), "Zeta.png");
    CHECK(write_file(ND_SETAPP_SYSTEM_WALLPAPER_DIR "/sub/delta.jpg"), "sub/delta.jpg");
    CHECK(write_file(ND_SETAPP_WALLPAPER_DIR "/Omega.jpg"), "Omega.jpg");
}

static void test_dirs(void)
{
    char dirs[ND_SETAPP_DIRS_MAX][ND_SETAPP_PATH_MAX];
    size_t n;

    /* The card state file is not under this scratch root, so there is no
     * card -- which is the state the ordering claim is made in. */
    CHECK(!nd_storage_is_ready(), "no SD card in this test");

    n = api.dirs(dirs, ND_SETAPP_DIRS_MAX);
    CHECK_INT(n, 2, "the stock directory and the user one");
    CHECK_STR(dirs[0], ND_SETAPP_SYSTEM_WALLPAPER_DIR, "STOCK CONTENT COMES FIRST");
    CHECK_STR(dirs[1], ND_SETAPP_WALLPAPER_DIR, "then the user's");
    CHECK(strncmp(dirs[0], "/NeoDCT/", 8u) == 0, "and they are logical /NeoDCT paths");

    CHECK_INT(api.dirs(NULL, 4u), 0, "NULL out");
    CHECK_INT(api.dirs(dirs, 0u), 0, "zero capacity");
}

static void test_scan(void)
{
    nd_wallpaper *w = calloc(ND_SETAPP_MAX, sizeof *w);
    size_t n;

    if (w == NULL) {
        CHECK(false, "allocation");
        return;
    }

    n = api.scan(w, ND_SETAPP_MAX);

    /* notes.txt and Zeta.png are not wallpapers; the other four are, one of
     * them a directory deep and one in the user directory. */
    CHECK_INT(n, 4, "four wallpapers");
    if (n == 4u) {
        CHECK_STR(w[0].name, "alpha", "alpha.JPEG sorts first, case-insensitively");
        CHECK_STR(w[1].name, "Beta", "Beta");
        CHECK_STR(w[2].name, "delta", "a subdirectory's wallpaper sorts among the rest");
        CHECK_STR(w[3].name, "Omega", "the user directory's is in the same list");

        CHECK_STR(w[0].path, ND_SETAPP_SYSTEM_WALLPAPER_DIR "/alpha.JPEG", "alpha path");
        CHECK_STR(w[2].path, ND_SETAPP_SYSTEM_WALLPAPER_DIR "/sub/delta.jpg", "delta path");
        CHECK_STR(w[3].path, ND_SETAPP_WALLPAPER_DIR "/Omega.jpg", "Omega path");
    }

    CHECK_INT(api.scan(NULL, 4u), 0, "NULL out");
    CHECK_INT(api.scan(w, 0u), 0, "zero capacity");
    CHECK_INT(api.scan(w, 2u), 2, "a cap of two stops at two");

    free(w);
}

/* ------------------------------------------------------------------ *
 * 6. _wrap_text -- the seventh wrapper
 * ------------------------------------------------------------------ */

static void test_wrap_text(void)
{
    sa_fixture fx;
    char storage[8][ND_TEXT_LINE_MAX];
    nd_lines lines;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    nd_lines_init(&lines, storage, ND_ARRAY_LEN(storage));

    /* `if not words: return [""]` -- ONE empty line, where nd_text_wrap()
     * returns none and nd_text_wrap_break() returns one. */
    api.wrap(&lines, &fx.ui, "", 220, fx.ui.font_s);
    CHECK_INT(lines.n, 1, "the empty string gives one line");
    CHECK_STR(nd_lines_at(&lines, 0u), "", "and that line is empty");

    api.wrap(&lines, &fx.ui, "   \t\n  ", 220, fx.ui.font_s);
    CHECK_INT(lines.n, 1, "whitespace only gives one line");
    CHECK_STR(nd_lines_at(&lines, 0u), "", "still empty");

    api.wrap(&lines, &fx.ui, NULL, 220, fx.ui.font_s);
    CHECK_INT(lines.n, 1, "`(text or \"\")` -- NULL is the empty string");

    /* str.split() with no argument: runs of whitespace collapse and NEWLINES
     * ARE LOST. nd_text_wrap() would emit three lines here. */
    api.wrap(&lines, &fx.ui, "a\nb   c", 220, fx.ui.font_s);
    CHECK_INT(lines.n, 1, "a newline is a separator, not a break");
    CHECK_STR(nd_lines_at(&lines, 0u), "a b c", "and runs of whitespace collapse to one space");

    /* An ordinary wrap at a narrow column. */
    api.wrap(&lines, &fx.ui, "aa bb cc dd", 20, fx.ui.font_s);
    CHECK(lines.n >= 2u, "a narrow column wraps");

    /* THE ONE LINE THAT DIFFERS FROM MESSAGES' WRAPPER. A word wider than
     * the column is emitted whole and over-wide: no trim, no "...".
     * nd_msg_wrap_text() would produce something ending in "...". */
    api.wrap(&lines, &fx.ui, "Supercalifragilistic", 12, fx.ui.font_s);
    CHECK_INT(lines.n, 1, "one over-wide word is one line");
    CHECK_STR(nd_lines_at(&lines, 0u), "Supercalifragilistic", "left over-wide, NOT ellipsised");

    /* ...and the word after it starts a new line rather than joining it. */
    api.wrap(&lines, &fx.ui, "Supercalifragilistic ok", 12, fx.ui.font_s);
    CHECK_INT(lines.n, 2, "the next word starts a line of its own");
    CHECK_STR(nd_lines_at(&lines, 0u), "Supercalifragilistic", "the over-wide word");
    CHECK_STR(nd_lines_at(&lines, 1u), "ok", "then the rest");

    api.wrap(NULL, &fx.ui, "x", 20, fx.ui.font_s); /* must not fault */
    api.wrap(&lines, NULL, "x", 20, fx.ui.font_s);
    sa_checks++;

    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 7. _show_about()'s geometry
 * ------------------------------------------------------------------ */

static bool px_is(const nd_image *img, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    nd_color c = nd_image_get_px(img, x, y);

    return c.r == r && c.g == g && c.b == b;
}

static void test_about(void)
{
    sa_fixture fx;
    int32_t header_y;
    int32_t line_pad;
    int32_t x;
    int32_t y;
    bool any_grey = false;
    bool any_white_in_bar = false;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    nd_vclock_enable();

    header_y = nd_ui_header_divider_y(&fx.ui);       /* max(30, H*0.11) = 30 */
    line_pad = nd_max32(10, (int32_t)(240 * 0.12));  /* max(10, int(W*0.12)) = 28 */
    CHECK_INT(header_y, 30, "header_y");
    CHECK_INT(line_pad, 28, "line_pad");

    api.draw_about(&fx.ui);

    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "the bar's present=False plus one fb.update");

    /* The divider spans line_pad .. screen_w - line_pad INCLUSIVE, because
     * Pillow's line() draws both endpoints. */
    CHECK(px_is(fx.canvas, line_pad, header_y, 255, 255, 255), "the divider starts at line_pad");
    CHECK(px_is(fx.canvas, 240 - line_pad, header_y, 255, 255, 255),
          "and ends at screen_w - line_pad");
    CHECK(px_is(fx.canvas, line_pad - 1, header_y, 0, 0, 0), "one pixel left of it is black");
    CHECK(px_is(fx.canvas, 240 - line_pad + 1, header_y, 0, 0, 0),
          "one pixel right of it is black");
    CHECK(px_is(fx.canvas, 0, 0, 0, 0, 0), "the screen was cleared to black first");

    /* The body is grey (128,128,128); a fully-covered pixel of it is exactly
     * that, since the text is composited onto black. */
    for (y = header_y + 12; y < 145 && !any_grey; y++) {
        for (x = 0; x < 240; x++) {
            if (px_is(fx.canvas, x, y, 128, 128, 128)) {
                any_grey = true;
                break;
            }
        }
    }
    CHECK(any_grey, "the version/build lines are drawn in grey");

    /* SoftKeyBar("Back") owns rows 145..174. */
    for (y = 145; y < 175 && !any_white_in_bar; y++) {
        for (x = 0; x < 240; x++) {
            if (px_is(fx.canvas, x, y, 255, 255, 255)) {
                any_white_in_bar = true;
                break;
            }
        }
    }
    CHECK(any_white_in_bar, "the \"Back\" softkey is on the bar");

    api.draw_about(NULL); /* must not fault */
    sa_checks++;

    nd_vclock_disable();
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 8. Decision 3, end to end
 * ------------------------------------------------------------------ */

static void expect_setting(const char *key, const char *want, const char *what)
{
    char got[ND_PATH_MAX];

    (void)nd_settings_get_copy(key, "<absent>", got, sizeof got);
    CHECK_STR(got, want, what);
}

/* Every scenario below ends with a HELD Back, which is what lets the app out
 * of whatever screen the script left it on: MessageDialog drains the channel
 * before drawing, so a queued Back would be eaten, but the held state
 * survives the drain and the repeat arrives after the screen is up. */
static void run_script(const int32_t *keys, size_t n, int *rc_out, uint64_t *frames_out)
{
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    if (!sa_send_all(&fx, keys, n) || !sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "key script");
        sa_fx_free(&fx);
        return;
    }
    nd_vclock_enable();
    *rc_out = api.run(&fx.ui);
    *frames_out = nd_capture_frames_drawn(fx.cap);
    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_wallpaper_writes_the_setting(void)
{
    /* Enter opens the picker on "None" (index 0); Enter again chooses it. */
    static const int32_t PICK_NONE[] = {ND_KEY_ENTER, ND_KEY_ENTER};
    /* Enter, Down, Enter: index 1, the first scanned wallpaper -- "alpha". */
    static const int32_t PICK_FIRST[] = {ND_KEY_ENTER, ND_KEY_DOWN, ND_KEY_ENTER};
    /* The list is None, alpha, Beta, delta, Omega, Get more... -- six rows,
     * so the number shortcut 6 lands on "Get more...". */
    static const int32_t PICK_GET_MORE[] = {ND_KEY_ENTER, ND_KEY_6};
    int rc = -1;
    uint64_t frames = 0u;

    (void)nd_settings_set(ND_SET_UI_WALLPAPER, "sentinel");

    run_script(PICK_NONE, ND_ARRAY_LEN(PICK_NONE), &rc, &frames);
    CHECK_INT(rc, 0, "Back out of the root menu returns 0");
    CHECK(frames >= 3u, "the root list, the picker and the confirmation were drawn");
    expect_setting(ND_SET_UI_WALLPAPER, "NONE", "picking None writes the literal NONE");

    run_script(PICK_FIRST, ND_ARRAY_LEN(PICK_FIRST), &rc, &frames);
    CHECK_INT(rc, 0, "and again");
    expect_setting(ND_SET_UI_WALLPAPER, ND_SETAPP_SYSTEM_WALLPAPER_DIR "/alpha.JPEG",
                   "picking a wallpaper writes its LOGICAL path");

    /* "Get more..." opens the help and selects nothing: the row whose path is
     * the Python's None. */
    run_script(PICK_GET_MORE, ND_ARRAY_LEN(PICK_GET_MORE), &rc, &frames);
    CHECK_INT(rc, 0, "and again");
    expect_setting(ND_SET_UI_WALLPAPER, ND_SETAPP_SYSTEM_WALLPAPER_DIR "/alpha.JPEG",
                   "\"Get more...\" writes nothing");
}

static void test_engineering_mode_writes_the_setting(void)
{
    /* 3 picks "Engineering Mode" off the root list; then 2 picks "Off" and
     * 1 picks "On" -- VerticalList's digit shortcuts are 1-based. */
    static const int32_t TURN_OFF[] = {ND_KEY_3, ND_KEY_2};
    static const int32_t TURN_ON[] = {ND_KEY_3, ND_KEY_1};
    int rc = -1;
    uint64_t frames = 0u;

    run_script(TURN_OFF, ND_ARRAY_LEN(TURN_OFF), &rc, &frames);
    CHECK_INT(rc, 0, "Back out of the root menu returns 0");
    CHECK(frames >= 3u, "the root list, the Eng. Mode list and the confirmation were drawn");
    expect_setting(ND_SET_UI_ENGINEERING, "OFF", "Off writes OFF");

    run_script(TURN_ON, ND_ARRAY_LEN(TURN_ON), &rc, &frames);
    CHECK_INT(rc, 0, "and again");
    expect_setting(ND_SET_UI_ENGINEERING, "ON", "On writes ON");
}

/* With no /run/neodct/sdcard.prop under this root the card is ABSENT, which
 * is the "No memory card." dialog followed by the SDCARD_HELP scroller. The
 * claim is that both screens appear and that the app comes back. */
static void test_memory_card_absent(void)
{
    static const int32_t OPEN_CARD[] = {ND_KEY_2};
    int rc = -1;
    uint64_t frames = 0u;

    CHECK(!nd_storage_is_ready(), "no card");
    run_script(OPEN_CARD, ND_ARRAY_LEN(OPEN_CARD), &rc, &frames);
    CHECK_INT(rc, 0, "the memory-card screen returns to the root menu");
    /* root list, dialog, help page, root list again -- at least four. */
    CHECK(frames >= 4u, "the dialog and the help screen were both drawn");
}

/* ------------------------------------------------------------------ *
 * 9. The two golden frames
 * ------------------------------------------------------------------ */

static void test_golden_root(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* VerticalList does not drain the channel before its first draw, so a
     * held Back reaches wait_for_key() with the frame already committed. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back on the root menu returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: run()'s VerticalList");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "app-settings");

    nd_vclock_disable();
    sa_fx_free(&fx);
}

/* shoot_docs.py's ("Settings", [ENTER], "app-settings-wallpaper"). Rendered
 * against a root whose System/wallpapers is the overlay's, so the six stock
 * names are the ones the Python saw, and whose User is real and writable --
 * _wallpaper_menu_once() does os.makedirs(WALLPAPER_DIR) on every visit and
 * neodct/overlay/ must never be written to. */
static void test_golden_wallpaper(void)
{
    sa_fixture fx;
    int rc;

    (void)nd_path_set_root(g_wproot);

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        (void)nd_path_set_root(g_root);
        return;
    }
    if (!sa_send(&fx, ND_KEY_ENTER) || !sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "key script");
        sa_fx_free(&fx);
        (void)nd_path_set_root(g_root);
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back unwinds the picker and then the root menu");
    /* run()'s list, the picker, then run()'s list again after Back. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 3, "three frames; the second is the reference");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 1u), "app-settings-wallpaper");

    nd_vclock_disable();
    sa_fx_free(&fx);
    (void)nd_path_set_root(g_root);
}

/* ------------------------------------------------------------------ *
 * Null safety
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    char out[4];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with no child held */
    api.shutdown();
    CHECK_STR(api.display_name(NULL, out, sizeof out), "", "display_name(NULL)");
    CHECK(api.display_name("x.jpg", NULL, 0u) == NULL, "display_name with no buffer");
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

/* <root>/NeoDCT/System/wallpapers -> <repo>/neodct/overlay/NeoDCT/System/
 * wallpapers, with a real writable <root>/NeoDCT/User beside it. */
/* mkdir -p on the HOST filesystem. nd_mkdir_p() resolves through ND_ROOT and
 * this runs before ND_ROOT is set, on a path that is already absolute. */
static bool mkdirs_real(const char *path)
{
    char buf[ND_PATH_MAX];
    char *p;

    if (nd_strlcpy(buf, path, sizeof buf) >= sizeof buf)
        return false;
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

static bool build_overlay_root(void)
{
    char overlay[ND_PATH_MAX];
    char dir[ND_PATH_MAX];
    char link[ND_PATH_MAX];
    char target[ND_PATH_MAX];

    if (!sa_overlay_root(overlay, sizeof overlay))
        return false;
    if (nd_snprintf(dir, sizeof dir, "%s/NeoDCT/System", g_wproot) != ND_OK)
        return false;
    if (!mkdirs_real(dir))
        return false;
    if (nd_snprintf(dir, sizeof dir, "%s/NeoDCT/User", g_wproot) != ND_OK)
        return false;
    if (!mkdirs_real(dir))
        return false;
    if (nd_snprintf(link, sizeof link, "%s/NeoDCT/System/wallpapers", g_wproot) != ND_OK)
        return false;
    if (nd_snprintf(target, sizeof target, "%s/NeoDCT/System/wallpapers", overlay) != ND_OK)
        return false;
    return symlink(target, link) == 0;
}

int main(void)
{
    void *h = sa_begin("Settings", "ndsetapp");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndsetapp-root", g_root, sizeof g_root) ||
        !sa_tmpdir("ndsetapp-wp", g_wproot, sizeof g_wproot)) {
        (void)dlclose(h);
        return 1;
    }
    if (!build_overlay_root()) {
        fprintf(stderr, "test_settings_app: cannot stage the overlay root\n");
        (void)dlclose(h);
        return 1;
    }

    /* Everything below /NeoDCT is this test's own. Set before the tree is
     * built, because build_wallpaper_tree() writes through nd_path_resolve(). */
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);
    (void)nd_settings_init();

    RUN(test_strings);
    RUN(test_is_supported);
    RUN(test_display_name);
    RUN(build_wallpaper_tree);
    RUN(test_dirs);
    RUN(test_scan);
    RUN(test_wrap_text);
    RUN(test_about);
    RUN(test_wallpaper_writes_the_setting);
    RUN(test_engineering_mode_writes_the_setting);
    RUN(test_memory_card_absent);
    RUN(test_golden_root);
    RUN(test_golden_wallpaper);
    RUN(test_null_safety);

    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
    rc = sa_end(h, "test_settings_app");
    sa_rmtree(g_root);
    sa_rmtree(g_wproot);
    return rc;
}
