/* test_appsel.c -- AppSelector and the app registry, measured against the
 * golden frames.
 *
 * ============ WHAT THIS FILE CLAIMS ============
 *
 * 1. The registry produces the same 24 entries, in the same order, with the
 *    same names, ids, paths and icon paths that main.py's _scan_apps_from_dir
 *    plus `sort(key=id)` produce -- including engineering mode adding the
 *    second directory and turning it off removing exactly eleven apps.
 *
 * 2. Nine frames rendered by the C AppSelector are byte-identical to the ones
 *    the Python build produced. Checked by SHA-256 over raw RGB against the
 *    digests in neodct/tests/golden/manifest.json, which is the same hash
 *    goldenframe.py computes -- so a pass here is a pass there.
 *
 * The nine are every frame the reference set has for this widget:
 * menu-phone-book, menu-panel (the 240x240 device shot), menu-messages,
 * menu-games, menu-settings, menu-calculator, menu-koki-mobile, menu-browser
 * and menu-music.
 *
 * 3. The geometry that is NOT covered by a frame -- the empty list, the
 *    single-item scrollbar, the wrap-around at both ends of the carousel --
 *    behaves the way framework.py does.
 *
 * ============ HOW THE NINE ARE REPRODUCED ============
 *
 * shoot_docs.shoot_app_selector() opens ONE `with StubUI(wallpaper=...)`
 * block, builds one AppSelector over nd_ui_app_list(&ui, NULL) with background=nd_ui_wallpaper(&ui),
 * then walks a fixed list of app NAMES, setting selected_index to that app's
 * index and calling draw(). The index is what the page number and the
 * scrollbar notch are computed from, so looking the apps up by name -- rather
 * than hard-coding 0, 1, 4, ... -- is what keeps this test honest if a
 * manifest is ever added.
 *
 * The staging trick is test_ui.c's: /NeoDCT/System is a symlink onto the
 * repo's overlay and only /NeoDCT/User is real, so nothing under
 * neodct/overlay/ is written to.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (the Makefile
 * passes it); the overlay is found relative to it. Set NEODCT_APPSEL_STAGE to
 * a directory to keep the staged root and the rendered PNGs for inspection.
 */

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

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

#define CHECK_STR(got, want, what)                                                          \
    do {                                                                                    \
        const char *g_ = (got);                                                             \
        const char *w_ = (want);                                                            \
        g_checks++;                                                                         \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                                            \
            g_failures++;                                                                   \
            fprintf(stderr, "FAIL %s:%d  %s: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                    (what), g_ != NULL ? g_ : "(null)", w_);                                \
        }                                                                                   \
    } while (0)

/* ------------------------------------------------------------------ *
 * Staging -- test_ui.c's symlink farm
 * ------------------------------------------------------------------ */

static char g_stage[ND_PATH_MAX];
static bool g_stage_is_temp;
static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];

static bool find_reference_dirs(void)
{
    const char *golden = getenv("NEODCT_GOLDEN");

    if (golden == NULL || golden[0] == '\0')
        return false;
    if (nd_snprintf(g_golden, sizeof g_golden, "%s", golden) != ND_OK)
        return false;
    if (nd_snprintf(g_overlay, sizeof g_overlay, "%s/../../overlay", golden) != ND_OK)
        return false;
    return true;
}

static bool stage_root(void)
{
    char tmpl[ND_PATH_MAX];
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    char user[ND_PATH_MAX];
    const char *want = getenv("NEODCT_APPSEL_STAGE");

    if (want != NULL && want[0] != '\0') {
        (void)nd_strlcpy(g_stage, want, sizeof g_stage);
        (void)mkdir(g_stage, 0755);
        g_stage_is_temp = false;
    } else {
        const char *base = getenv("TMPDIR");

        if (base == NULL || base[0] == '\0')
            base = "/tmp";
        if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndappsel-XXXXXX", base) != ND_OK)
            return false;
        if (mkdtemp(tmpl) == NULL)
            return false;
        (void)nd_strlcpy(g_stage, tmpl, sizeof g_stage);
        g_stage_is_temp = true;
    }

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        return false;
    (void)mkdir(neodct, 0755);

    if (nd_snprintf(sys_link, sizeof sys_link, "%s/System", neodct) != ND_OK)
        return false;
    if (nd_snprintf(sys_target, sizeof sys_target, "%s/NeoDCT/System", g_overlay) != ND_OK)
        return false;
    if (symlink(sys_target, sys_link) != 0 && errno != EEXIST)
        return false;

    if (nd_snprintf(user, sizeof user, "%s/User", neodct) != ND_OK)
        return false;
    (void)mkdir(user, 0755);

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
    if (g_stage_is_temp && g_stage[0] != '\0')
        (void)nftw(g_stage, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* uistub._prepare_user_dir(): the ack file so the first-boot modal is skipped,
 * and settings.prop with the two keys the menu depends on. */
static void write_settings(const char *wallpaper_name, bool engineering)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/NeoDCT/User/.ack_security_warning", g_stage) == ND_OK) {
        f = fopen(path, "w");
        if (f != NULL) {
            (void)fputs("0", f);
            (void)fclose(f);
        }
    }
    if (nd_snprintf(path, sizeof path, "%s/NeoDCT/User/settings.prop", g_stage) != ND_OK)
        return;
    f = fopen(path, "w");
    if (f == NULL)
        return;
    (void)fprintf(f, "system.ui.engineering_mode=%s\n", engineering ? "ON" : "OFF");
    if (wallpaper_name != NULL)
        (void)fprintf(f, "system.ui.wallpaper=/NeoDCT/System/wallpapers/%s\n", wallpaper_name);
    (void)fclose(f);
}

/* ------------------------------------------------------------------ *
 * The golden manifest
 * ------------------------------------------------------------------ */

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

static void check_frame(nd_capture *cap, const nd_json_doc *golden, const char *name,
                        const nd_image *img)
{
    char digest[65];
    const char *want;

    CHECK(nd_capture_save(cap, name, img) == ND_OK, name);
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
        fprintf(stderr, "FAIL frame %-18s got %s\n%23swant %s\n", name, digest, "", want);
    }
}

/* ------------------------------------------------------------------ *
 * The registry
 * ------------------------------------------------------------------ */

/* The 24 shipped manifests in the order `sort(key=lambda x: x["id"])` puts
 * them. This is the list the menu walks, so the index of each name is also
 * its page number minus one, and every scrollbar notch in the nine frames is
 * derived from it. */
static const struct {
    int32_t id;
    const char *name;
    const char *path;
} EXPECTED[] = {
    {1, "Phone book", "/NeoDCT/System/apps/PhoneBook"},
    {2, "Messages", "/NeoDCT/System/apps/Messages"},
    {3, "Call Log", "/NeoDCT/System/apps/CallLog"},
    {4, "Settings", "/NeoDCT/System/apps/Settings"},
    {6, "Games", "/NeoDCT/System/apps/Games"},
    {7, "Calculator", "/NeoDCT/System/apps/Calculator"},
    {8, "Clock", "/NeoDCT/System/apps/Clock"},
    {9, "Tones", "/NeoDCT/System/apps/Tones"},
    {10, "Koki Mobile", "/NeoDCT/System/apps/Koki"},
    {11, "Browser", "/NeoDCT/System/apps/Browser"},
    {12, "Update", "/NeoDCT/System/apps/Update"},
    {970, "Music", "/NeoDCT/System/apps/MusicPlayer"},
    {971, "Power", "/NeoDCT/System/apps/Power"},
    {999, "Linux Shell", "/NeoDCT/System/engineering/apps/LinuxShell"},
    {9001, "LCD Test", "/NeoDCT/System/engineering/apps/LCDTest"},
    {9002, "KeyMap", "/NeoDCT/System/engineering/apps/KeypadMapper"},
    {9003, "KeyMapI2C", "/NeoDCT/System/engineering/apps/KeypadMapperI2C"},
    {9004, "FuelGauge", "/NeoDCT/System/engineering/apps/FuelGauge"},
    {9005, "ModemInfo", "/NeoDCT/System/engineering/apps/Modem"},
    {9006, "Downgrade", "/NeoDCT/System/engineering/apps/Downgrade"},
    {9990, "Remote Shell", "/NeoDCT/System/engineering/apps/RemoteShell"},
    {9997, "Crash", "/NeoDCT/System/engineering/apps/Crash"},
    {9998, "Cube Bench", "/NeoDCT/System/engineering/apps/CubeBench"},
    {9999, "Tests", "/NeoDCT/System/engineering/apps/TestsApp"},
};

static size_t index_of(nd_ui *ui, const char *name)
{
    size_t i;

    for (i = 0u; i < nd_ui_app_count(ui); i++) {
        if (strcmp(nd_ui_app_list(ui, NULL)[i].name, name) == 0)
            return i;
    }
    return (size_t)-1;
}

static void test_registry(nd_ui *ui)
{
    size_t i;

    CHECK_INT(nd_ui_app_count(ui), ND_ARRAY_LEN(EXPECTED), "every shipped manifest is registered");
    CHECK(nd_ui_engineering_mode(ui), "engineering mode is on");
    if (nd_ui_app_count(ui) != ND_ARRAY_LEN(EXPECTED))
        return;

    for (i = 0u; i < ND_ARRAY_LEN(EXPECTED); i++) {
        char icon[ND_APP_PATH_MAX];

        CHECK_INT(nd_ui_app_list(ui, NULL)[i].id, EXPECTED[i].id, EXPECTED[i].name);
        CHECK_STR(nd_ui_app_list(ui, NULL)[i].name, EXPECTED[i].name, "name in id order");
        CHECK_STR(nd_ui_app_list(ui, NULL)[i].path, EXPECTED[i].path, "path in id order");
        /* Every shipped manifest omits "icon", so the default joins
         * "icon.png" onto the app's own directory. */
        (void)nd_snprintf(icon, sizeof icon, "%s/icon.png", EXPECTED[i].path);
        CHECK_STR(nd_ui_app_list(ui, NULL)[i].icon, icon, "icon path defaults to <dir>/icon.png");
        /* U-6: the field is populated from the manifest and launches nothing.
         * All 24 still say main.py. */
        CHECK_STR(nd_ui_app_list(ui, NULL)[i].exec, "main.py", "exec as the manifest spells it");
    }
}

/* Every icon the menu will ever ask for must decode at the size the widget
 * asks for it -- 82 px on this panel. A missing or corrupt icon draws the
 * white "?" placeholder instead, which is visibly wrong on a shipped phone. */
static void test_icons_load(nd_ui *ui)
{
    size_t i;

    for (i = 0u; i < nd_ui_app_count(ui); i++) {
        const nd_image *img = nd_ui_get_image_max(ui, nd_ui_app_list(ui, NULL)[i].icon, 82);

        g_checks++;
        if (img == NULL) {
            g_failures++;
            fprintf(stderr, "FAIL icon %s did not decode (%s)\n", nd_ui_app_list(ui, NULL)[i].name,
                    nd_ui_app_list(ui, NULL)[i].icon);
            continue;
        }
        /* thumbnail((82,82)) never upscales and preserves the aspect ratio,
         * so the long side is 82 and neither side exceeds it. */
        CHECK(img->w <= 82 && img->h <= 82 && (img->w == 82 || img->h == 82),
              "icon thumbnailed to fit 82 px");
    }
}

static void test_engineering_off(void)
{
    nd_app_entry apps[ND_APP_MAX];
    size_t stock;
    size_t eng;

    stock = nd_ui_scan_apps(ND_PATH_APPS_DIR, apps, ND_APP_MAX);
    eng = nd_ui_scan_apps(ND_PATH_ENG_APPS_DIR, apps, ND_APP_MAX);
    CHECK_INT(stock, 13, "thirteen stock apps");
    CHECK_INT(eng, 11, "eleven engineering apps");
}

/* ------------------------------------------------------------------ *
 * Behaviour the frames do not cover
 * ------------------------------------------------------------------ */

static void test_init_defaults(nd_ui *ui)
{
    nd_appsel s;

    nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), nd_ui_wallpaper(ui));
    CHECK_INT(s.selected_index, 0, "the carousel starts on the first app");
    CHECK(s.title != NULL && strcmp(s.title, "Main Menu") == 0,
          "title is stored -- and never drawn");
    CHECK(s.background == nd_ui_wallpaper(ui), "background is the wallpaper");
    CHECK_INT(s.n_items, nd_ui_app_count(ui), "the whole registry is the carousel");

    nd_appsel_init(&s, ui, "Main Menu", NULL, 99u, NULL);
    CHECK_INT(s.n_items, 0, "a NULL item array is an empty list whatever the count says");
}

/* ------------------------------------------------------------------ *
 * The blocking show() loop, end to end
 * ------------------------------------------------------------------ */

/* The same problem PagedList's tests hit: show() FLUSHES the channel before
 * its first draw, so a script written in advance is eaten and cannot drive it.
 * The only thing that can arrive after the flush is a repeat of a key that is
 * still held, so that is how every key below is delivered.
 *
 * The interval is set to 100 s and the delay to 50 ms, which turns "held" into
 * "fires exactly once". Two keys held in a known order therefore arrive in
 * that order and once each: nd_input's take_repeat() picks the SOONEST due
 * repeat, and a key pressed earlier is armed earlier. That is what makes
 * "one Down, then Enter" a deterministic script rather than a race. */

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
    CHECK_INT(write(fd, &ev, sizeof ev), (int)sizeof ev, "write event");
}

static void write_key(int fd, int32_t code, int32_t value)
{
    write_event(fd, EV_KEY_T, (uint16_t)code, value);
    write_event(fd, EV_SYN_T, 0u, 0);
}

/* Release everything and let the channel go quiet, so the next scenario
 * starts from a known held state. */
static void release_all(nd_input *in, int fd, const int32_t *codes, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++)
        write_key(fd, codes[i], 0);
    while (nd_input_read_key(in, 0.05) != ND_KEY_NONE) {}
}

static void test_show_loop(nd_ui *ui)
{
    /* Enter and Clear do NOT repeat by default and must not -- a repeat on
     * Enter would open whatever the menu happened to be sitting on. Asking
     * for them here is also a check that the widget sees the input object at
     * all rather than reaching around it. */
    static const int32_t REPEATERS[4] = {ND_KEY_UP, ND_KEY_DOWN, ND_KEY_ENTER, ND_KEY_CLEAR};
    nd_input *saved = ui->input;
    nd_input *in = NULL;
    int fds[2];
    size_t last;

    if (nd_ui_app_count(ui) < 2u) {
        CHECK(false, "the show() loop needs at least two apps");
        return;
    }
    last = nd_ui_app_count(ui) - 1u;

    if (pipe(fds) != 0) {
        CHECK(false, "pipe");
        return;
    }
    if (nd_input_open_fd(&in, fds[0]) != ND_OK) {
        CHECK(false, "nd_input_open_fd");
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }
    CHECK_INT(nd_input_set_repeat_codes(in, REPEATERS, ND_ARRAY_LEN(REPEATERS)), ND_OK,
              "repeat codes accepted");
    nd_input_set_repeat(in, 0.05, 100.0);
    ui->input = in;

    {
        /* Down then Enter: one step forward, and the index is the answer. */
        nd_appsel s;
        const int32_t held[2] = {ND_KEY_DOWN, ND_KEY_ENTER};

        nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);
        write_key(fds[1], ND_KEY_DOWN, 1);
        write_key(fds[1], ND_KEY_ENTER, 1);
        CHECK_INT(nd_appsel_show(&s), 1, "Down then Enter returns index 1");
        CHECK_INT(s.selected_index, 1, "and leaves the selection there");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        /* Up from the FIRST app. This is the branch where a naive
         * (index - 1) % n on a size_t gives SIZE_MAX % n instead of n - 1. */
        nd_appsel s;
        const int32_t held[2] = {ND_KEY_UP, ND_KEY_ENTER};

        nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);
        write_key(fds[1], ND_KEY_UP, 1);
        write_key(fds[1], ND_KEY_ENTER, 1);
        CHECK_INT(nd_appsel_show(&s), (int32_t)last, "Up from the first wraps to the last");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        /* Down from the LAST app wraps to the first. */
        nd_appsel s;
        const int32_t held[2] = {ND_KEY_DOWN, ND_KEY_ENTER};

        nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);
        s.selected_index = last;
        write_key(fds[1], ND_KEY_DOWN, 1);
        write_key(fds[1], ND_KEY_ENTER, 1);
        CHECK_INT(nd_appsel_show(&s), 0, "Down from the last wraps to the first");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        /* Clear alone backs out of the menu. */
        nd_appsel s;
        const int32_t held[1] = {ND_KEY_CLEAR};

        nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);
        write_key(fds[1], ND_KEY_CLEAR, 1);
        CHECK_INT(nd_appsel_show(&s), ND_WIDGET_BACK, "Clear leaves the menu");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        /* The flush. Two complete Down presses are pending when show() is
         * called and neither may reach the loop; the answer is the index the
         * caller set, not two rows past it. */
        nd_appsel s;
        const int32_t held[1] = {ND_KEY_ENTER};

        nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);
        s.selected_index = 3u;
        write_key(fds[1], ND_KEY_DOWN, 1);
        write_key(fds[1], ND_KEY_DOWN, 0);
        write_key(fds[1], ND_KEY_DOWN, 1);
        write_key(fds[1], ND_KEY_DOWN, 0);
        write_key(fds[1], ND_KEY_ENTER, 1);
        CHECK_INT(nd_appsel_show(&s), 3, "the two Downs were flushed, not acted on");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        /* A key that means nothing here is ignored and the loop keeps going.
         * '*' is not a repeater, so it is delivered as an ordinary press that
         * arrives during the flush -- what is being checked is that the loop
         * does not mistake it for an answer. */
        nd_appsel s;
        const int32_t held[1] = {ND_KEY_CLEAR};

        nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);
        write_key(fds[1], ND_KEY_STAR, 1);
        write_key(fds[1], ND_KEY_STAR, 0);
        write_key(fds[1], ND_KEY_CLEAR, 1);
        CHECK_INT(nd_appsel_show(&s), ND_WIDGET_BACK, "'*' is ignored, Clear still answers");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        /* The empty list. Down must NOT take the modulo -- the guard runs
         * first -- and Enter answers Back rather than 0, which is where this
         * widget differs from PagedList. */
        nd_appsel s;
        const int32_t held[2] = {ND_KEY_DOWN, ND_KEY_ENTER};

        nd_appsel_init(&s, ui, "Main Menu", NULL, 0u, NULL);
        write_key(fds[1], ND_KEY_DOWN, 1);
        write_key(fds[1], ND_KEY_ENTER, 1);
        CHECK_INT(nd_appsel_show(&s), ND_WIDGET_BACK, "an empty menu answers Back to Enter");
        CHECK_INT(s.selected_index, 0, "and never moved");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }
    {
        nd_appsel s;
        const int32_t held[1] = {ND_KEY_CLEAR};

        nd_appsel_init(&s, ui, "Main Menu", NULL, 0u, NULL);
        write_key(fds[1], ND_KEY_CLEAR, 1);
        CHECK_INT(nd_appsel_show(&s), ND_WIDGET_BACK, "an empty menu answers Back to Clear");
        release_all(in, fds[1], held, ND_ARRAY_LEN(held));
    }

    ui->input = saved;
    (void)close(fds[1]);
    nd_input_close(in);
}

/* A manifest is user-supplied data, so a name wider than the screen is
 * reachable in a way nothing shipped is: at 24 px the longest shipped name,
 * "Remote Shell", is well inside 240. Centring then puts the draw origin at a
 * NEGATIVE x, where Python's `//` floors and C's `/` truncates -- see
 * floordiv2 in nd_appsel.c. What is checked here is only that the branch is
 * taken and the title is clipped rather than wrapped or dropped; the one-pixel
 * difference between the two divisions is invisible at the screen edge,
 * because an overflowing title runs off BOTH sides. Recorded in
 * OPEN-QUESTIONS.md as A-2. */
static void test_overflowing_name(nd_capture *cap, nd_ui *ui)
{
    static nd_app_entry wide;
    nd_appsel s;
    const nd_image *frame;
    int32_t w = 0;
    int32_t h = 0;
    int32_t x;

    memset(&wide, 0, sizeof wide);
    (void)nd_strlcpy(wide.name, "Wide Enough To Overflow The Whole Panel", sizeof wide.name);
    wide.id = 1;

    nd_ui_text_size(ui, wide.name, ui->font_xl, &w, &h);
    CHECK(w > nd_ui_width(ui), "the test name really is wider than the screen");
    x = (nd_ui_width(ui) - w) / 2;
    CHECK(x < 0, "so the draw origin is negative");

    nd_appsel_init(&s, ui, "Main Menu", &wide, 1u, NULL);
    nd_appsel_draw(&s);
    frame = nd_capture_recent(cap, 0u);
    if (frame == NULL) {
        CHECK(false, "overflowing-title frame");
        return;
    }
    {
        /* Ink on BOTH outermost columns of the title band: the string is
         * clipped at each edge, not shrunk to fit, not ellipsised and not
         * dropped. The band starts at the 24 px face's first ink row,
         * title_y + bbox_top = 14 + 3, and ends before the scrollbar's
         * track_top of 36 plus the descenders. */
        bool left = false;
        bool right = false;
        int32_t y;

        for (y = 17; y <= 37; y++) {
            if (nd_image_get_px(frame, 0, y).r > 0u)
                left = true;
            if (nd_image_get_px(frame, nd_ui_width(ui) - 1, y).r > 0u)
                right = true;
        }
        CHECK(left, "the title runs off the left edge");
        CHECK(right, "and off the right edge");
    }
    (void)nd_capture_save(cap, "menu-overflow", frame);
}

/* The empty list is what a failed scan looks like. It must draw "No Apps"
 * without touching the scrollbar arithmetic, and its show() must accept only
 * the two ways out. */
static void test_empty_list(nd_capture *cap, nd_ui *ui)
{
    nd_appsel s;
    const nd_image *frame;

    nd_appsel_init(&s, ui, "Main Menu", NULL, 0u, NULL);
    CHECK_INT(s.n_items, 0, "a NULL item array is an empty list");
    nd_appsel_draw(&s);
    frame = nd_capture_recent(cap, 0u);
    CHECK(frame != NULL, "the empty menu still produces a frame");
    if (frame != NULL) {
        /* Black background, and the only white is the centred "No Apps": the
         * scrollbar track at x=232..233 must NOT be there. */
        nd_color px = nd_image_get_px(frame, 232, 100);

        CHECK(px.r == 0u && px.g == 0u && px.b == 0u, "no scrollbar on the empty menu");
        px = nd_image_get_px(frame, 0, 0);
        CHECK(px.r == 0u, "background filled black without a wallpaper");
    }
    (void)nd_capture_save(cap, "menu-empty", nd_capture_recent(cap, 0u));
}

/* A one-app menu takes the `else` branch of the notch computation -- the
 * division by len-1 would be a division by zero. The notch sits at the top of
 * the track, rows 33..39 (36 +/- 3), columns 228..234. */
static void test_single_item_scrollbar(nd_capture *cap, nd_ui *ui)
{
    nd_appsel s;
    const nd_image *frame;

    nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), 1u, NULL);
    nd_appsel_draw(&s);
    frame = nd_capture_recent(cap, 0u);
    if (frame == NULL) {
        CHECK(false, "single-item frame");
        return;
    }
    {
        nd_color top = nd_image_get_px(frame, 230, 36);
        nd_color below = nd_image_get_px(frame, 228, 60);
        nd_color track = nd_image_get_px(frame, 232, 60);
        nd_color track2 = nd_image_get_px(frame, 233, 60);
        nd_color past = nd_image_get_px(frame, 232, 136);

        CHECK(top.r == 255u, "the notch is at the top of the track");
        CHECK(below.r == 0u, "the notch does not extend down the track");
        CHECK(track.r == 255u, "the track is drawn at x=232");
        CHECK(track2.r == 255u, "width 2 grows into the MINOR axis: x=233 too");
        CHECK(past.r == 0u, "the track stops at y=135 inclusive");
    }
    (void)nd_capture_save(cap, "menu-single", frame);
}

/* ------------------------------------------------------------------ *
 * The nine frames
 * ------------------------------------------------------------------ */

/* shoot_docs.shoot_app_selector()'s list, in its order. A name the scan does
 * not produce is skipped there ("if name not in by_name: continue"), so a
 * missing one here is a registry failure, reported as such. */
static const char *const WANTED[][2] = {
    {"Phone book", "menu-phone-book"}, {"Messages", "menu-messages"},
    {"Games", "menu-games"},           {"Settings", "menu-settings"},
    {"Calculator", "menu-calculator"}, {"Koki Mobile", "menu-koki-mobile"},
    {"Browser", "menu-browser"},       {"Music", "menu-music"},
};

static void shoot_menu_frames(nd_capture *cap, const nd_json_doc *golden)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    nd_appsel selector;
    size_t i;

    write_settings("Palestine.jpg", true);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init");
        return;
    }
    CHECK(nd_ui_wallpaper(&ui) != NULL, "the configured wallpaper loaded");

    test_registry(&ui);
    test_icons_load(&ui);
    test_init_defaults(&ui);

    /* simulate_status is what the reference block sets. AppSelector draws no
     * status bar, so it changes no pixel here -- it is set because the
     * reference sets it and a divergence would otherwise be invisible. */
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    nd_appsel_init(&selector, &ui, "Main Menu", nd_ui_app_list(&ui, NULL), nd_ui_app_count(&ui), nd_ui_wallpaper(&ui));

    for (i = 0u; i < ND_ARRAY_LEN(WANTED); i++) {
        size_t idx = index_of(&ui, WANTED[i][0]);

        if (idx == (size_t)-1) {
            CHECK(false, WANTED[i][0]);
            continue;
        }
        selector.selected_index = idx;
        nd_appsel_draw(&selector);
        check_frame(cap, golden, WANTED[i][1], nd_capture_recent(cap, 0u));

        if (strcmp(WANTED[i][0], "Phone book") == 0) {
            nd_image *panel =
                nd_capture_device_frame(cap, 0u, ND_CAPTURE_PANEL_W, ND_CAPTURE_PANEL_H);

            if (panel != NULL) {
                check_frame(cap, golden, "menu-panel", panel);
                nd_image_free(panel);
            } else {
                CHECK(false, "device_frame allocation");
            }
        }
    }

    test_empty_list(cap, &ui);
    test_single_item_scrollbar(cap, &ui);
    test_overflowing_name(cap, &ui);
    test_show_loop(&ui);

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

int main(void)
{
    nd_capture *cap = NULL;
    nd_json_doc *golden = NULL;

    if (!find_reference_dirs()) {
        printf("test_appsel: NEODCT_GOLDEN is not set; nothing to compare against\n");
        return 1;
    }
    if (!stage_root()) {
        printf("test_appsel: cannot stage a root under %s\n", g_stage);
        return 1;
    }

    test_engineering_off();

    golden = load_golden_manifest();
    if (golden == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP frames: %s/manifest.json did not parse\n", g_golden);
    } else {
        if (nd_capture_open(&cap, "/frames", 0u) == ND_OK) {
            shoot_menu_frames(cap, golden);
            CHECK(nd_capture_write_manifest(cap) == ND_OK, "manifest written");
            printf("test_appsel: frames in %s/frames\n", g_stage);
            nd_capture_close(cap);
        } else {
            CHECK(false, "nd_capture_open");
        }
        nd_json_free(golden);
    }

    drop_stage();
    printf("test_appsel: %d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skips);
    return g_failures == 0 ? 0 : 1;
}
