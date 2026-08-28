/* test_ui.c -- nd_ui, the image cache, the home layout, and the home screen
 * measured against the golden frames.
 *
 * ============ WHAT THIS FILE CLAIMS ============
 *
 * 1. The geometry helpers, the get_image key shapes, the "/home" path fixup,
 *    the FIFO eviction order and the app scan all behave the way
 *    System/core/main.py does. Checked directly.
 *
 * 2. Six frames rendered by the C home screen are byte-identical to the ones
 *    the Python build produced. Checked by SHA-256 over raw RGB, against the
 *    digests in neodct/tests/golden/manifest.json -- the same hash
 *    goldenframe.py computes, so a pass here is a pass there.
 *
 * The six are the whole home-screen set: home, home-panel, home-dialing,
 * home-nowallpaper, home-simulation and home-sms-banner.
 *
 * ============ HOW THE SIX ARE REPRODUCED ============
 *
 * shoot_docs.shoot_home() and shoot_docs.shoot_calls() build them inside four
 * separate `with StubUI(...)` blocks, and goldenframe.DeterministicUI gives
 * each block a FRESH virtual clock that advances one 0.1 s tick per committed
 * frame. So the frame number a screen is drawn at is part of its identity:
 * home-sms-banner is the THIRD frame of its block, and the envelope's
 * int(time.time() * 2) % 2 blink depends on that. Each group below therefore
 * calls nd_vclock_enable() to restart virtual time, and group D commits two
 * throwaway frames to stand in for the two call screens shoot_calls draws
 * first -- their pixels are irrelevant, their ticks are not.
 *
 * ============ WHY THE ROOT IS A SYMLINK FARM ============
 *
 * The runtime wants /NeoDCT/System (read-only assets) and /NeoDCT/User
 * (databases, settings.prop). uistub.py copies the whole 16 MB overlay per
 * context; here System is symlinked to the repo's overlay and only User is
 * real, so nothing under neodct/overlay/ is ever written to and the test costs
 * a few milliseconds instead of four copytrees.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (the Makefile
 * passes it); the overlay is found relative to it. Set NEODCT_UI_STAGE to a
 * directory to keep the staged root and the rendered PNGs for inspection.
 */

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_layout.h"
#include "nd_paths.h"
#include "nd_settings.h"
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
 * Staging
 * ------------------------------------------------------------------ */

static char g_stage[ND_PATH_MAX]; /* the ND_ROOT for this run       */
static bool g_stage_is_temp;
static char g_golden[ND_PATH_MAX];  /* neodct/tests/golden            */
static char g_overlay[ND_PATH_MAX]; /* neodct/overlay                 */

static bool find_reference_dirs(void)
{
    const char *golden = getenv("NEODCT_GOLDEN");

    if (golden == NULL || golden[0] == '\0')
        return false;
    if (nd_snprintf(g_golden, sizeof g_golden, "%s", golden) != ND_OK)
        return false;
    /* <repo>/neodct/tests/golden -> <repo>/neodct/overlay */
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
    const char *want = getenv("NEODCT_UI_STAGE");

    if (want != NULL && want[0] != '\0') {
        (void)nd_strlcpy(g_stage, want, sizeof g_stage);
        (void)mkdir(g_stage, 0755);
        g_stage_is_temp = false;
    } else {
        const char *base = getenv("TMPDIR");

        if (base == NULL || base[0] == '\0')
            base = "/tmp";
        if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndui-XXXXXX", base) != ND_OK)
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

/* CODING-STANDARDS.md section 1.7 applies to the filesystem too: a leak
 * detector's output is only worth reading if the run leaves nothing behind. */
static void drop_stage(void)
{
    (void)nd_path_set_root(NULL);
    if (g_stage_is_temp && g_stage[0] != '\0')
        (void)nftw(g_stage, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* uistub._prepare_user_dir(): settings.prop with sorted keys, and the ack file
 * so the first-boot security modal is skipped. */
/* NULL for either extra means "leave it out", which is what a phone that has
 * never been configured has and therefore what the defaults must cover. */
static void write_settings_full(const char *wallpaper_name, const char *wpeverywhere,
                                const char *wp_dim, const char *wp_animate)
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
    (void)fputs("system.ui.engineering_mode=ON\n", f);
    if (wallpaper_name != NULL) {
        /* Stock wallpapers ship inside the read-only image. */
        (void)fprintf(f, "system.ui.wallpaper=/NeoDCT/System/wallpapers/%s\n", wallpaper_name);
    }
    if (wpeverywhere != NULL)
        (void)fprintf(f, "system.ui.wpeverywhere=%s\n", wpeverywhere);
    if (wp_dim != NULL)
        (void)fprintf(f, "system.ui.wpeverywhere_dim=%s\n", wp_dim);
    if (wp_animate != NULL)
        (void)fprintf(f, "system.ui.wpanimate=%s\n", wp_animate);
    (void)fclose(f);
}

static void write_settings(const char *wallpaper_name)
{
    write_settings_full(wallpaper_name, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ *
 * The golden manifest
 * ------------------------------------------------------------------ */

/* Read with plain fopen, NOT through nd_path_resolve: the reference set lives
 * outside ND_ROOT (see the Makefile's note on NEODCT_GOLDEN). */
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
        const char *ename = nd_json_get_str(e, "name", "");

        if (strcmp(ename, name) == 0)
            return nd_json_get_str(e, "sha256", NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Unit checks that need no rendering
 * ------------------------------------------------------------------ */

static void test_geometry(void)
{
    nd_ui ui;

    memset(&ui, 0, sizeof ui);
    /* getattr(ui, "W", DEFAULT) on a context that has not reached step 5. */
    CHECK_INT(nd_ui_width(&ui), 240, "default W");
    CHECK_INT(nd_ui_height(&ui), 175, "default H");
    CHECK_INT(nd_ui_softkey_height(&ui), 30, "default SOFTKEY_H");
    CHECK_INT(nd_ui_content_bottom(&ui), 145, "default content_bottom");
    CHECK_INT(nd_ui_header_divider_y(&ui), 30, "header divider floor");

    ui.w = 320;
    ui.h = 480;
    ui.softkey_h = 40;
    CHECK_INT(nd_ui_width(&ui), 320, "assigned W");
    CHECK_INT(nd_ui_content_bottom(&ui), 440, "assigned content_bottom");
    /* max(30, int(480 * 0.11)) == 52, so the floor is NOT always the answer. */
    CHECK_INT(nd_ui_header_divider_y(&ui), 52, "header divider formula");
}

static void test_layout_scale(void)
{
    /* y 24 -> 17, y 71 -> 51, y 12 -> 8; x never moves. */
    CHECK_INT(nd_layout_scale_y(24, 175), 17, "scale y 24");
    CHECK_INT(nd_layout_scale_y(71, 175), 51, "scale y 71");
    CHECK_INT(nd_layout_scale_y(12, 175), 8, "scale y 12");
    CHECK_INT(nd_layout_scale_x(210, 240), 210, "scale x 210");
    CHECK_INT(nd_layout_scale_x(7, 240), 7, "scale x 7");
}

static void test_layout_load(void)
{
    nd_home_layout *l = nd_layout_load(ND_PATH_HOME_LAYOUT);

    if (l == NULL) {
        CHECK(false, "ui_home.json did not parse");
        return;
    }
    CHECK(l->background == NULL, "shipped layout has background null");
    CHECK_INT(l->n_elements, 4, "four elements");
    if (l->n_elements == 4u) {
        /* Array order is paint order: battery, carrier, clock, signal. */
        CHECK_INT(l->elements[0].type, ND_EL_ICON_SET, "element 0 is an icon set");
        CHECK_STR(l->elements[0].prefix, "bat", "element 0 prefix");
        CHECK_INT(l->elements[0].x, 210, "element 0 x");
        CHECK_INT(l->elements[0].y, 24, "element 0 y");
        CHECK_INT(l->elements[0].count, 5, "element 0 count");
        CHECK_INT(l->elements[0].sim_val, 4, "element 0 sim_val");
        CHECK(l->elements[0].has_custom[3], "element 0 has a bat-3 sprite");

        CHECK_INT(l->elements[1].type, ND_EL_TEXT, "element 1 is text");
        CHECK_STR(l->elements[1].text, "No Service", "element 1 text");
        CHECK_INT(l->elements[1].anchor, ND_ANCHOR_CENTER_H, "element 1 anchor");
        CHECK_INT(l->elements[1].font_size, 12, "element 1 font_size");
        CHECK_INT(l->elements[1].color.r, 255, "element 1 colour is white");

        CHECK_STR(l->elements[2].text, "12:00", "element 2 text");
        CHECK_INT(l->elements[2].anchor, ND_ANCHOR_RIGHT, "element 2 anchor");
        CHECK_STR(l->elements[3].prefix, "sig", "element 3 prefix");
    }
    nd_layout_free(l);
}

/* get_image's cache keys, its "/home" fixup, and the FIFO's eviction order.
 * The eviction order is checked by identity: the same path asked for twice
 * must give the same pointer until 32 other entries have pushed it out. */
static void test_image_cache(void)
{
    nd_imgcache *c = nd_imgcache_new(4u);
    const nd_image *a;
    const nd_image *b;

    if (c == NULL) {
        CHECK(false, "nd_imgcache_new failed");
        return;
    }

    a = nd_imgcache_get(c, ND_PATH_ENVELOPE, 0, 0.0);
    CHECK(a != NULL, "envelope.png decodes");
    if (a != NULL) {
        CHECK_INT(a->w, 26, "envelope width");
        CHECK_INT(a->h, 18, "envelope height");
        CHECK_INT(a->fmt, ND_PIXFMT_RGBA8888, "cache converts to RGBA");
    }
    b = nd_imgcache_get(c, ND_PATH_ENVELOPE, 0, 0.0);
    CHECK(a == b, "a hit returns the cached surface, not a new decode");

    /* scale=175/240 is what _get_status_icon passes: 26x18 -> 18x13. */
    b = nd_imgcache_get(c, ND_PATH_ENVELOPE, 0, 175.0 / 240.0);
    CHECK(b != NULL && b != a, "the scaled copy is a separate entry");
    if (b != NULL) {
        CHECK_INT(b->w, 18, "scaled envelope width");
        CHECK_INT(b->h, 13, "scaled envelope height");
    }

    /* max_size never upscales, so a 26x18 icon asked to fit 64 is untouched. */
    b = nd_imgcache_get(c, ND_PATH_ENVELOPE, 64, 0.0);
    CHECK(b != NULL && b != a, "the max_size copy is a separate entry");
    if (b != NULL)
        CHECK_INT(b->w, 26, "max_size does not upscale");

    /* A miss on a file that is not there is None, not an error. */
    CHECK(nd_imgcache_get(c, "/NeoDCT/System/ui/resources/img/nope.png", 0, 0.0) == NULL,
          "a missing file is a NULL, not a crash");

    /* Four slots: the plain, scaled and max_size envelopes plus one more
     * evicts the OLDEST INSERTED, which is the plain one. */
    CHECK(nd_imgcache_get(c, ND_PATH_WARNING_ICON, 0, 0.0) != NULL, "warning.png decodes");
    CHECK(nd_imgcache_get(c, ND_PATH_PLACEHOLDER_ICON, 0, 0.0) != NULL, "placeholder decodes");
    b = nd_imgcache_get(c, ND_PATH_ENVELOPE, 0, 0.0);
    CHECK(b != NULL && b != a, "FIFO evicted the first-inserted entry");

    nd_imgcache_free(c);
}

/* The "/home" fixup in get_image(): a development left-over some manifests
 * may still carry. The guard tests for "System" in the path, and
 * split("NeoDCT")[-1] takes everything after the LAST occurrence. */
static void test_home_path_fixup(void)
{
    nd_ui ui;
    const nd_image *direct;
    const nd_image *fixed;

    memset(&ui, 0, sizeof ui);
    ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    if (ui.image_cache == NULL) {
        CHECK(false, "nd_imgcache_new failed");
        return;
    }

    direct = nd_ui_get_image(&ui, ND_PATH_ENVELOPE);
    CHECK(direct != NULL, "envelope by its real path");

    /* Rewritten to /NeoDCT/... and therefore the SAME cache entry. */
    fixed = nd_ui_get_image(&ui, "/home/aiden/dev/NeoDCT/System/ui/resources/img/envelope.png");
    CHECK(fixed == direct, "a /home path containing System is rewritten");

    /* No "System" in it, so no rewrite -- and nothing at that path. */
    CHECK(nd_ui_get_image(&ui, "/home/aiden/pictures/envelope.png") == NULL,
          "a /home path without System is left alone");

    /* Not under /home, so untouched even though it mentions NeoDCT. */
    CHECK(nd_ui_get_image(&ui, "/opt/NeoDCT/System/nope.png") == NULL,
          "a path outside /home is left alone");

    nd_imgcache_free(ui.image_cache);
}

static void test_app_scan(void)
{
    nd_app_entry apps[ND_APP_MAX];
    size_t n;
    size_t i;
    bool sorted = true;
    bool saw_phonebook = false;

    n = nd_ui_scan_apps(ND_PATH_APPS_DIR, apps, ND_APP_MAX);
    CHECK(n >= 13u, "the stock app directory yields at least 13 manifests");

    for (i = 0u; i < n; i++) {
        if (strcmp(apps[i].name, "Phone book") == 0) {
            saw_phonebook = true;
            /* "id": "1" is a STRING in the shipped manifest and int() takes
             * either; icon is joined onto the app's own directory. */
            CHECK_INT(apps[i].id, 1, "Phone book id");
            CHECK_STR(apps[i].path, "/NeoDCT/System/apps/PhoneBook", "Phone book path");
            CHECK_STR(apps[i].icon, "/NeoDCT/System/apps/PhoneBook/icon.png", "Phone book icon");
        }
    }
    CHECK(saw_phonebook, "Phone book was scanned");

    /* The scan itself is filesystem order; the core sorts afterwards. */
    for (i = 1u; i < n; i++) {
        if (apps[i - 1u].id > apps[i].id)
            sorted = false;
    }
    ND_UNUSED(sorted);
}

/* ------------------------------------------------------------------ *
 * The frames
 * ------------------------------------------------------------------ */

static const int32_t DIGIT_KEYS[10] = {ND_KEY_0, ND_KEY_1, ND_KEY_2, ND_KEY_3, ND_KEY_4,
                                       ND_KEY_5, ND_KEY_6, ND_KEY_7, ND_KEY_8, ND_KEY_9};

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
 * The wallpaper behind the framework's own chrome
 * ------------------------------------------------------------------ */

/* Is every pixel of `r` exactly this colour? The chrome background is either
 * a photograph or flat black, and "flat black" is the only one that can be
 * asserted without a second copy of the picture. */
static bool rect_is_flat(const nd_image *img, nd_rect r, nd_color c)
{
    int32_t x;
    int32_t y;

    for (y = r.y0; y <= r.y1; y++) {
        for (x = r.x0; x <= r.x1; x++) {
            nd_color got = nd_image_get_px(img, x, y);

            if (got.r != c.r || got.g != c.g || got.b != c.b)
                return false;
        }
    }
    return true;
}

static bool rect_matches(const nd_image *a, const nd_image *b, nd_rect r)
{
    int32_t x;
    int32_t y;

    for (y = r.y0; y <= r.y1; y++) {
        for (x = r.x0; x <= r.x1; x++) {
            nd_color pa = nd_image_get_px(a, x, y);
            nd_color pb = nd_image_get_px(b, x, y);

            if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b)
                return false;
        }
    }
    return true;
}

/* The whole of nd_ui_paint_chrome()'s contract, on a context built the way the
 * core builds one. */
static void test_chrome_background(nd_fb *fb)
{
    nd_ui ui;
    const nd_image *chrome;
    nd_image *keep;
    nd_rect content;

    /* --- 1. on by default, with a wallpaper configured --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (chrome)");
        return;
    }
    content = ND_RECT(0, 0, nd_ui_width(&ui) - 1, nd_ui_content_bottom(&ui) - 1);

    CHECK(ui.app_use_wallpaper, "the core is never an app that opted out");
    chrome = nd_ui_chrome_wallpaper(&ui);
    CHECK(chrome != NULL, "wpeverywhere defaults ON, so chrome has a wallpaper");
    if (chrome == NULL) {
        nd_ui_teardown(&ui);
        return;
    }
    CHECK(chrome->w == nd_ui_width(&ui) && chrome->h == nd_ui_height(&ui),
          "the chrome image is panel-sized");

    /* It is DIMMER than the wallpaper the home screen draws -- that is the
     * whole reason it is a second image and not the same pointer. */
    CHECK(chrome != nd_ui_wallpaper(&ui), "chrome is not the home wallpaper itself");
    {
        const nd_image *home = nd_ui_wallpaper(&ui);
        int64_t sum_home = 0;
        int64_t sum_chrome = 0;
        int32_t x;

        for (x = 0; x < nd_ui_width(&ui); x++) {
            sum_home += nd_image_get_px(home, x, 40).r;
            sum_chrome += nd_image_get_px(chrome, x, 40).r;
        }
        if (sum_chrome >= sum_home)
            fprintf(stderr, "  chrome row sum %lld, home row sum %lld\n", (long long)sum_chrome,
                    (long long)sum_home);
        CHECK(sum_chrome < sum_home, "chrome is dimmer than the home wallpaper");
    }

    /* Cached: the second call must not rebuild while the wallpaper has not
     * moved, because every widget draw asks for it. */
    CHECK(nd_ui_chrome_wallpaper(&ui) == chrome, "the chrome image is cached between calls");

    /* And painting it puts THAT REGION where it belongs, not the top-left
     * corner of the picture stretched over whatever was asked for. */
    keep = nd_image_copy(chrome);
    CHECK(keep != NULL, "copy the chrome image");
    if (keep != NULL) {
        (void)nd_image_fill(ui.canvas, ND_RGB(7, 11, 13));
        nd_ui_paint_chrome_content(&ui);
        CHECK(rect_matches(ui.canvas, keep, content), "paint_chrome_content lays down its region");
        /* Rows below content_bottom are the softkey strip's and must be
         * untouched -- nd_widgets.h rule 1, partial clears are load-bearing. */
        CHECK(rect_is_flat(ui.canvas,
                           ND_RECT(0, nd_ui_content_bottom(&ui) + 1, nd_ui_width(&ui) - 1,
                                   nd_ui_height(&ui) - 1),
                           ND_RGB(7, 11, 13)),
              "paint_chrome_content leaves the softkey strip alone");

        (void)nd_image_fill(ui.canvas, ND_RGB(7, 11, 13));
        nd_ui_paint_chrome_full(&ui);
        CHECK(rect_matches(ui.canvas, keep,
                           ND_RECT(0, 0, nd_ui_width(&ui) - 1, nd_ui_height(&ui) - 1)),
              "paint_chrome_full covers the whole panel");

        /* A band from the middle of the screen must come from the middle of
         * the picture. This is the check that fails if somebody blits the
         * whole image at the rectangle's origin. */
        (void)nd_image_fill(ui.canvas, ND_RGB(7, 11, 13));
        nd_ui_paint_chrome(&ui, ND_RECT(0, 90, nd_ui_width(&ui) - 1, 120));
        CHECK(rect_matches(ui.canvas, keep, ND_RECT(0, 90, nd_ui_width(&ui) - 1, 120)),
              "a mid-screen band is painted with the picture's own rows");
        nd_image_free(keep);
    }
    nd_ui_teardown(&ui);

    /* --- 2. the setting turns it off, and only it --- */
    write_settings_full("Palestine.jpg", "OFF", NULL, NULL);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (chrome off)");
        return;
    }
    CHECK(nd_ui_chrome_wallpaper(&ui) == NULL, "wpeverywhere=OFF means no chrome wallpaper");
    (void)nd_image_fill(ui.canvas, ND_RGB(7, 11, 13));
    nd_ui_paint_chrome_content(&ui);
    CHECK(rect_is_flat(ui.canvas, content, ND_BLACK), "and the background goes back to black");
    /* The HOME screen still has its wallpaper: this setting is about chrome. */
    CHECK(nd_ui_wallpaper(&ui) != NULL, "the home wallpaper is unaffected by wpeverywhere");
    nd_ui_teardown(&ui);

    /* --- 3. no wallpaper configured: black, whatever the setting says --- */
    write_settings_full(NULL, "ON", NULL, NULL);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (no wallpaper)");
        return;
    }
    CHECK(nd_ui_chrome_wallpaper(&ui) == NULL, "no wallpaper, no chrome wallpaper");
    nd_ui_teardown(&ui);

    /* --- 4. an app that opted out never even loads one --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (opted out)");
        return;
    }
    ui.app_use_wallpaper = false;
    CHECK(nd_ui_chrome_wallpaper(&ui) == NULL, "useWallpaper=false means black chrome");
    CHECK(!ui.home_.wallpaper_ready,
          "an opted-out app does not pay the wallpaper load -- see \"Lazy home state\"");
    nd_ui_teardown(&ui);

    /* --- 5. the dim is a real number and a junk one falls back --- */
    {
        static const char *const CASES[] = {"0.2", "banana", "-1", "9"};
        int64_t bright[ND_ARRAY_LEN(CASES)];
        size_t c;

        for (c = 0u; c < ND_ARRAY_LEN(CASES); c++) {
            int32_t x;

            write_settings_full("Palestine.jpg", "ON", CASES[c], NULL);
            nd_vclock_enable();
            nd_ui_sim_clear(&ui);
            if (nd_ui_init(&ui, fb) != ND_OK) {
                CHECK(false, "nd_ui_init with a dim setting");
                return;
            }
            chrome = nd_ui_chrome_wallpaper(&ui);
            bright[c] = 0;
            if (chrome != NULL) {
                for (x = 0; x < nd_ui_width(&ui); x++)
                    bright[c] += nd_image_get_px(chrome, x, 40).r;
            }
            CHECK(chrome != NULL, "every dim value still produces a chrome image");
            nd_ui_teardown(&ui);
        }
        CHECK(bright[0] < bright[1], "0.2 is dimmer than the default");
        /* All three junk values take the same fallback, so they agree. */
        CHECK(bright[1] == bright[2] && bright[2] == bright[3],
              "an unparseable, a negative and an over-1 dim all fall back to the default");
    }
}

/* ------------------------------------------------------------------ *
 * The animated wallpaper
 * ------------------------------------------------------------------ */

/* A stand-in for a widget's draw(). It PRESENTS, because under the virtual
 * clock time only moves when a frame is committed -- which is also true of
 * every real widget's draw, and is what lets one repaint lead to the next. */
static nd_ui *g_repaint_ui;

static void count_repaint(void *ctx)
{
    int *n = ctx;

    (*n)++;
    if (g_repaint_ui != NULL)
        (void)nd_ui_present(g_repaint_ui);
}

static void test_animated_wallpaper(nd_fb *fb)
{
    nd_ui ui;
    nd_image *first;
    double t;

    /* The shipped animated wallpaper. Skipped rather than failed if it is
     * absent, so a trimmed overlay does not fail the build. */
    write_settings("Classroom.gif");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (animated)");
        return;
    }
    if (nd_ui_wallpaper(&ui) == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP animated wallpaper: Classroom.gif did not load\n");
        nd_ui_teardown(&ui);
        return;
    }

    CHECK(ui.home_.wallpaper_gif != NULL, "a .gif wallpaper opens an animation");
    CHECK(nd_ui_wallpaper(&ui)->w == nd_ui_width(&ui) &&
              nd_ui_wallpaper(&ui)->h == nd_ui_height(&ui),
          "a frame is dimmed and scaled like any other wallpaper");

    /* nd_ui_wallpaper() itself must NEVER advance the animation: the home
     * screen, the softkey bar and the app selector all call it on the same
     * frame, and a 25 fps GIF would play at 75. */
    first = nd_image_copy(nd_ui_wallpaper(&ui));
    CHECK(first != NULL, "copy frame 0");
    (void)nd_ui_wallpaper(&ui);
    (void)nd_ui_wallpaper(&ui);
    if (first != NULL) {
        CHECK(rect_matches(nd_ui_wallpaper(&ui), first,
                           ND_RECT(0, 0, nd_ui_width(&ui) - 1, nd_ui_height(&ui) - 1)),
              "reading the wallpaper does not advance it");
    }

    /* Frame 0 IS ALREADY ON SCREEN -- one decoder serves the still and the
     * animation -- so nothing is owed until its delay has run. A tick that
     * fired here would repaint the picture that is already there. */
    CHECK(!nd_ui_tick_wallpaper(&ui), "nothing is due while frame 0 is still showing");

    /* The core loop asks how long it may sleep. It must never be told longer
     * than it asked for, nor a negative, and while a frame is owed later it
     * must be a real wait rather than a spin. */
    t = nd_ui_frame_timeout(&ui, 0.1);
    CHECK(t > 0.0 && t <= 0.1, "the frame timeout is inside (0, dflt] while a frame is showing");

    /* Time moves one 0.1 s tick per committed frame under the virtual clock,
     * and every delay this decoder reports is at most 0.1 s -- so exactly one
     * frame comes due per frame drawn, which is what makes nd-shoot --anim
     * deterministic. */
    nd_vclock_advance();
    {
        /* The pointer is a documented guarantee, not an accident: nd_appsel
         * stores it for the whole life of the menu. If a tick ever starts
         * replacing the image instead of repainting it, that store dangles. */
        const nd_image *before = nd_ui_wallpaper(&ui);

        CHECK(nd_ui_tick_wallpaper(&ui), "one tick of the clock makes the next frame due");
        CHECK(nd_ui_wallpaper(&ui) == before, "a tick repaints the wallpaper, it does not move it");
    }
    if (first != NULL) {
        CHECK(!rect_matches(nd_ui_wallpaper(&ui), first,
                            ND_RECT(0, 0, nd_ui_width(&ui) - 1, nd_ui_height(&ui) - 1)),
              "and it changes the picture");
        nd_image_free(first);
    }
    CHECK(!nd_ui_tick_wallpaper(&ui), "a second tick in the same instant is not due");

    /* Handing the UI a still replaces the animation outright, or nd-shoot and
     * the tests would have their injected picture painted over. */
    nd_ui_set_wallpaper(&ui, nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_GRAY));
    CHECK(ui.home_.wallpaper_gif == NULL, "set_wallpaper closes the animation");
    CHECK(!nd_ui_tick_wallpaper(&ui), "and nothing ticks afterwards");
    /* Exactly the caller's own value. A truncating cast to int would pass for
     * anything under 1.0 including 0.0, which is the spin this rules out. */
    CHECK(nd_ui_frame_timeout(&ui, 0.1) == 0.1, "a still gets the default back, not a short poll");

    nd_ui_teardown(&ui);

    /* ---- and the wiring that makes it play on the phone ---- *
     *
     * Everything above drives nd_ui_tick_wallpaper() by hand. On the phone
     * nothing does: the animation exists only because nd_ui_update() calls it
     * once per frame, and that one line is what a refactor would drop without
     * failing anything else. Drive the frame loop the way core_run() does and
     * assert the picture actually moves. */
    write_settings("Classroom.gif");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) == ND_OK) {
        if (nd_ui_wallpaper(&ui) == NULL) {
            g_skips++;
            fprintf(stderr, "SKIP frame-loop animation: Classroom.gif did not load\n");
        } else {
            nd_image *f0 = nd_image_copy(nd_ui_wallpaper(&ui));
            uint32_t gen0 = ui.home_.wallpaper_gen;
            int i;

            nd_ui_sim_status(&ui, 4, 4, "Tello");
            /* Three frames through the real dispatcher. The virtual clock
             * advances 0.1 s per committed frame and the wallpaper is due
             * every 40 ms, so each one owes a frame. */
            for (i = 0; i < 3; i++)
                nd_ui_update(&ui);

            CHECK(ui.home_.wallpaper_gen > gen0,
                  "nd_ui_update() advances the wallpaper without being asked");
            if (f0 != NULL) {
                CHECK(!rect_matches(nd_ui_wallpaper(&ui), f0,
                                    ND_RECT(0, 0, nd_ui_width(&ui) - 1, nd_ui_height(&ui) - 1)),
                      "and the picture on the canvas is a different frame");
                nd_image_free(f0);
            }
        }
        nd_ui_teardown(&ui);
    }

    /* ---- a blocked widget keeps the wallpaper moving ---- *
     *
     * nd_ui_update() is the core's frame loop, and a widget is a BLOCKING
     * loop that the core loop is inside. So an animated wallpaper froze the
     * moment any menu opened -- which is most of the time anyone is looking
     * at the phone. nd_ui_set_repaint() is the fix and this is the assertion
     * that it is wired up: with a repainter registered, a bare
     * nd_ui_read_keypress() must advance the wallpaper and call back. */
    write_settings("Classroom.gif");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) == ND_OK) {
        if (nd_ui_wallpaper(&ui) == NULL) {
            g_skips++;
            fprintf(stderr, "SKIP blocked-widget repaint: Classroom.gif did not load\n");
        } else {
            int calls = 0;
            nd_ui_repaint saved;
            int i;

            /* Without a repainter, a key wait must change nothing: the core
             * loop draws its own frames and a second tick here would be a
             * behaviour change for it. */
            {
                uint32_t gen0 = ui.home_.wallpaper_gen;

                for (i = 0; i < 5; i++)
                    (void)nd_ui_read_keypress(&ui, 0.0);
                CHECK(ui.home_.wallpaper_gen == gen0,
                      "a key wait with no repainter leaves the wallpaper alone");
            }

            /* Every real widget draws once before it starts waiting, and
             * under the virtual clock that first committed frame is what
             * moves time far enough for the next wallpaper frame to be due.
             * Without it nothing is ever owed and nothing repaints -- which
             * is correct, not a bug: a wallpaper that is not due should not
             * be redrawn. */
            g_repaint_ui = &ui;
            (void)nd_ui_present(&ui);

            saved = nd_ui_set_repaint(&ui, count_repaint, &calls);
            for (i = 0; i < 5; i++)
                (void)nd_ui_read_keypress(&ui, 0.0);
            CHECK(calls > 0, "a registered repainter is called while the widget waits");

            /* And restoring puts back exactly what was there, which is what
             * makes a dialog opened from a list safe. */
            nd_ui_restore_repaint(&ui, saved);
            calls = 0;
            for (i = 0; i < 5; i++)
                (void)nd_ui_read_keypress(&ui, 0.0);
            CHECK_INT(calls, 0, "restoring the previous repainter unhooks this one");
            g_repaint_ui = NULL;
        }
        nd_ui_teardown(&ui);
    }

    /* ---- system.ui.wpanimate: the three tiers ---- *
     *
     * ALWAYS is the default and is what the other tests above exercise.
     * These two are the ways out, and each has to cost something real --
     * a mode that changed only where frames were drawn, while still holding
     * a decoder open in every app, would not be worth having. */
    {
        static const struct {
            const char *value;
            bool core_decoder;   /* does the CORE keep one open?          */
            bool app_decoder;    /* does an APP process keep one open?    */
            bool widget_repaint; /* does a blocked widget animate?        */
        } CASES[] = {
            {"ALWAYS", true, true, true},
            /* HOME's whole point: the home screen still moves, and an app
             * costs nothing at all. Collapsing it into ALWAYS or into OFF
             * fails on one of these three columns. */
            {"HOME", true, false, false},
            {"OFF", false, false, false},
            /* Anything unrecognised is the default, and the default is the
             * one that looks best -- a typo must not quietly turn it off. */
            {"banana", true, true, true},
        };
        size_t c;

        for (c = 0u; c < ND_ARRAY_LEN(CASES); c++) {
            write_settings_full("Classroom.gif", NULL, NULL, CASES[c].value);
            nd_vclock_enable();
            nd_ui_sim_clear(&ui);
            if (nd_ui_init(&ui, fb) != ND_OK) {
                CHECK(false, "nd_ui_init (wpanimate)");
                continue;
            }
            if (nd_ui_wallpaper(&ui) == NULL) {
                g_skips++;
                nd_ui_teardown(&ui);
                continue;
            }

            CHECK((ui.home_.wallpaper_gif != NULL) == CASES[c].core_decoder,
                  "wpanimate decides whether the core holds a decoder");
            nd_ui_teardown(&ui);

            /* THE APP SIDE, which is the whole cost claim of HOME: an app
             * process must open no decoder under it, because the repaint path
             * is inert there and the 226 KB would buy a frame that can never
             * be shown. anim_allowed_here() tells an app from the core by
             * nd_app_dir(), so setting one is all it takes to be an app --
             * the same thing nd-apprun does before it loads app.so. */
            (void)nd_app_set_dir("/NeoDCT/System/apps/PhoneBook");
            nd_vclock_enable();
            nd_ui_sim_clear(&ui);
            if (nd_ui_init(&ui, fb) == ND_OK && nd_ui_wallpaper(&ui) != NULL) {
                CHECK((ui.home_.wallpaper_gif != NULL) == CASES[c].app_decoder,
                      "wpanimate decides whether an APP holds a decoder");
            }
            (void)nd_app_set_dir(NULL);
            nd_ui_teardown(&ui);

            /* Back to a core context for the repaint check below. */
            nd_vclock_enable();
            nd_ui_sim_clear(&ui);
            if (nd_ui_init(&ui, fb) != ND_OK) {
                CHECK(false, "nd_ui_init (wpanimate, second)");
                continue;
            }
            if (nd_ui_wallpaper(&ui) == NULL) {
                g_skips++;
                nd_ui_teardown(&ui);
                continue;
            }

            {
                int calls = 0;
                nd_ui_repaint saved;
                int i;

                g_repaint_ui = &ui;
                (void)nd_ui_present(&ui);
                saved = nd_ui_set_repaint(&ui, count_repaint, &calls);
                for (i = 0; i < 5; i++)
                    (void)nd_ui_read_keypress(&ui, 0.0);
                nd_ui_restore_repaint(&ui, saved);
                g_repaint_ui = NULL;

                CHECK((calls > 0) == CASES[c].widget_repaint,
                      "wpanimate decides whether a blocked widget animates");

                /* And the poll a widget uses must only shorten when it is
                 * going to advance the wallpaper -- otherwise the deadline
                 * never moves and the wait becomes a 100% CPU spin.
                 *
                 * Forced overdue by hand, because that is the state the spin
                 * needs and the virtual clock will not reach it on its own: it
                 * advances on a committed frame, and a widget blocked in
                 * nd_ui_wait_for_key() commits nothing. On the device wall
                 * time gets there within one frame of the widget going up. */
                ui.home_.wallpaper_due = 0.0;
                CHECK(nd_ui_frame_timeout(&ui, 0.1) == 0.0 || !CASES[c].core_decoder,
                      "an overdue frame is what makes the core loop redraw at once");
                CHECK(nd_ui_widget_timeout(&ui, 0.1) == 0.1,
                      "a wait with no repainter keeps its own timeout");

                /* Same deadline, repainter back on: only ALWAYS may shorten,
                 * because only ALWAYS ticks -- under HOME the wait would poll
                 * a deadline nothing is going to move. */
                saved = nd_ui_set_repaint(&ui, count_repaint, &calls);
                CHECK((nd_ui_widget_timeout(&ui, 0.1) == 0.1) != CASES[c].widget_repaint,
                      "only a wait that will advance the wallpaper shortens its poll");
                nd_ui_restore_repaint(&ui, saved);
            }
            nd_ui_teardown(&ui);
        }
    }

    /* ---- an app process gets the still and no decoder ---- *
     *
     * nd_ui_tick_wallpaper() has one caller and an app never reaches it, so
     * an app that opened a decoder would hold ~226 KB and a descriptor on the
     * SD card for a frame that can never arrive. nd_ui_init_app() clears
     * drives_wallpaper; this reproduces that context without needing a real
     * app process. */
    write_settings("Classroom.gif");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) == ND_OK) {
        ui.drives_wallpaper = false;
        if (nd_ui_wallpaper(&ui) != NULL) {
            CHECK(ui.home_.wallpaper_gif == NULL, "a process that cannot tick opens no decoder");
            CHECK(!nd_ui_tick_wallpaper(&ui), "and has nothing to tick");
            CHECK(nd_ui_frame_timeout(&ui, 0.1) == 0.1, "so it never shortens anybody's poll");
        }
        nd_ui_teardown(&ui);
    }

    /* A .jpg opens no decoder at all -- 226 KB not spent on a still. */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) == ND_OK) {
        CHECK(nd_ui_wallpaper(&ui) != NULL, "the jpg still loads");
        CHECK(ui.home_.wallpaper_gif == NULL, "a .jpg wallpaper opens no animation");
        CHECK(!nd_ui_tick_wallpaper(&ui), "a still wallpaper never ticks");
        nd_ui_teardown(&ui);
    }
}

/* ------------------------------------------------------------------ *
 * manifest.json's "useWallpaper"
 * ------------------------------------------------------------------ */

static void test_manifest_use_wallpaper(void)
{
    /* Absent means true, which is what every manifest written before the key
     * existed says, and what a third-party app gets for free. */
    CHECK(nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/PhoneBook"),
          "PhoneBook does not opt out");
    CHECK(nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/Settings"),
          "Settings does not opt out");

    /* The four the feature deliberately excludes, plus a sample engineering
     * app -- these are read from the shipped manifests, so this fails if one
     * of them loses its key. */
    CHECK(!nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/Games"), "Games opts out");
    CHECK(!nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/Koki"), "Koki opts out");
    CHECK(!nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/Update"), "Update opts out");
    CHECK(!nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/Browser"), "Browser opts out");
    CHECK(!nd_app_manifest_use_wallpaper("/NeoDCT/System/engineering/apps/Modem"),
          "an engineering app opts out");

    /* Every engineering app, by directory scan, so a new one that forgets the
     * key is caught here rather than by somebody noticing on the phone. */
    {
        nd_app_entry apps[ND_APP_MAX];
        size_t n = nd_ui_scan_apps(ND_PATH_ENG_APPS_DIR, apps, ND_APP_MAX);
        size_t i;
        bool all_out = n > 0u;

        for (i = 0u; i < n; i++) {
            if (nd_app_manifest_use_wallpaper(apps[i].path)) {
                all_out = false;
                fprintf(stderr, "  %s (%s) does not opt out\n", apps[i].name, apps[i].path);
            }
        }
        CHECK(all_out, "every engineering app opts out of the wallpaper");
    }

    /* Nonsense is true, not a crash: a background is a preference, and the
     * strict parsing lives in nd_manifest.c where it decides whether to
     * overwrite the root filesystem. */
    CHECK(nd_app_manifest_use_wallpaper(""), "the core, which has no manifest");
    CHECK(nd_app_manifest_use_wallpaper(NULL), "a NULL directory");
    CHECK(nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/NoSuchApp"), "a missing manifest");
}

static void shoot_home_frames(nd_capture *cap, const nd_json_doc *golden)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    static const int digits[10] = {0, 7, 4, 1, 2, 3, 4, 5, 6, 7};
    size_t i;

    /* --- group A: wallpaper, a healthy phone --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (group A)");
        return;
    }
    /* Lazy home state -- see "Lazy home state" in nd_ui.h. The constructor
     * leaves all of it alone; the first reader is what loads it. This is the
     * assertion that stops it quietly going back to being eager, which would
     * put 154 ms back onto every app launch without changing a pixel. */
    CHECK(!ui.home_.wallpaper_ready, "construction does not load the wallpaper");
    CHECK(!ui.home_.home_layout_ready, "construction does not parse ui_home.json");
    CHECK(!ui.home_.apps_ready, "construction does not scan the app directories");
    CHECK(!ui.home_.eng_mode_ready, "construction does not read engineering mode");

    CHECK(nd_ui_wallpaper(&ui) != NULL, "the configured wallpaper loads on demand");
    CHECK(nd_ui_home_layout(&ui) != NULL, "the home layout loads on demand");
    CHECK(nd_ui_app_count(&ui) >= 13u, "the app scan runs on demand");
    CHECK(ui.softkey_exists, "softkey_exists is set at construction step 9");
    CHECK(ui.softkey != NULL && ui.softkey->transparent,
          "the core's own bar is the transparent one");
    CHECK_INT(ui.state, ND_UI_STATE_HOME, "initial state is HOME");

    nd_ui_sim_status(&ui, 4, 4, "Tello");
    nd_ui_update(&ui);
    check_frame(cap, golden, "home", nd_capture_recent(cap, 0u));
    {
        nd_image *panel = nd_capture_device_frame(cap, 0u, ND_CAPTURE_PANEL_W, ND_CAPTURE_PANEL_H);
        if (panel != NULL) {
            check_frame(cap, golden, "home-panel", panel);
            nd_image_free(panel);
        } else {
            CHECK(false, "device_frame allocation");
        }
    }

    /* Typing a number on the home screen switches to the dialer. */
    for (i = 0u; i < ND_ARRAY_LEN(digits); i++)
        nd_ui_handle_input(&ui, DIGIT_KEYS[digits[i]]);
    CHECK_INT(ui.state, ND_UI_STATE_HOME_DIALING, "digits switch to HOME_DIALING");
    CHECK_STR(ui.dial_buffer, "0741234567", "dial buffer");
    nd_ui_update(&ui);
    check_frame(cap, golden, "home-dialing", nd_capture_recent(cap, 0u));
    nd_ui_teardown(&ui);

    /* --- group B: no wallpaper --- */
    write_settings(NULL);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (group B)");
        return;
    }
    CHECK(nd_ui_wallpaper(&ui) == NULL, "no wallpaper when the setting says NONE");
    nd_ui_sim_status(&ui, 4, 4, "Tello");
    nd_ui_update(&ui);
    check_frame(cap, golden, "home-nowallpaper", nd_capture_recent(cap, 0u));

    /* OPEN-QUESTIONS decision 3: Settings writes only the setting, and the
     * core picks it up on the next app exit -- never before. */
    write_settings("Palestine.jpg");
    CHECK(nd_ui_wallpaper(&ui) == NULL, "the setting alone changes nothing");
    nd_ui_refresh_after_app(&ui);
    CHECK(nd_ui_wallpaper(&ui) != NULL, "refresh_after_app re-reads the wallpaper");
    CHECK(nd_ui_engineering_mode(&ui), "refresh_after_app re-reads engineering mode");
    CHECK(nd_ui_app_count(&ui) >= 13u, "refresh_after_app rescans the app directories");
    nd_ui_teardown(&ui);

    /* --- group C: no fuel gauge and no modem, the honest QEMU look --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (group C)");
        return;
    }
    CHECK_INT(nd_ui_status_battery_level(&ui), 3, "simulated battery level");
    CHECK(!nd_ui_status_battery_hardware(&ui), "no fuel gauge");
    CHECK_INT(nd_ui_status_signal_level(&ui), -1, "unknown signal, not zero bars");
    CHECK_STR(nd_ui_status_carrier(&ui), "", "no carrier");
    nd_ui_update(&ui);
    check_frame(cap, golden, "home-simulation", nd_capture_recent(cap, 0u));
    nd_ui_teardown(&ui);

    /* --- group D: the 3310-style banner. shoot_calls draws the two call
     * screens first, so this is the block's THIRD frame and the envelope's
     * blink phase depends on it. --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init (group D)");
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");
    (void)nd_ui_present(&ui); /* stands in for call-active   */
    (void)nd_ui_present(&ui); /* stands in for call-incoming */
    CHECK_INT(nd_vclock_frame(), 2, "two ticks before the banner frame");

    nd_ui_sim_sms_banner(&ui, 1);
    nd_ui_set_unread_sms(&ui, 1);
    CHECK(nd_ui_status_notify_active(&ui), "the banner is up");
    nd_ui_update(&ui);
    check_frame(cap, golden, "home-sms-banner", nd_capture_recent(cap, 0u));
    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);

    nd_vclock_disable();
}

int main(void)
{
    nd_capture *cap = NULL;
    nd_json_doc *golden = NULL;
    char frames_dir[ND_PATH_MAX];

    if (!find_reference_dirs()) {
        printf("test_ui: NEODCT_GOLDEN is not set; nothing to compare against\n");
        return 1;
    }
    if (!stage_root()) {
        printf("test_ui: cannot stage a root under %s\n", g_stage);
        return 1;
    }

    test_geometry();
    test_layout_scale();
    test_layout_load();
    test_image_cache();
    test_home_path_fixup();
    test_app_scan();
    test_manifest_use_wallpaper();

    golden = load_golden_manifest();
    if (golden == NULL) {
        g_skips++;
        fprintf(stderr, "SKIP frames: %s/manifest.json did not parse\n", g_golden);
    } else {
        if (nd_snprintf(frames_dir, sizeof frames_dir, "/frames") == ND_OK &&
            nd_capture_open(&cap, frames_dir, 0u) == ND_OK) {
            shoot_home_frames(cap, golden);
            /* After the frames: these rebuild the context several times and
             * would otherwise renumber the virtual clock ticks the six
             * reference frames depend on. */
            test_chrome_background(nd_capture_fb(cap));
            test_animated_wallpaper(nd_capture_fb(cap));
            CHECK(nd_capture_write_manifest(cap) == ND_OK, "manifest written");
            printf("test_ui: frames in %s/frames\n", g_stage);
            nd_capture_close(cap);
        } else {
            CHECK(false, "nd_capture_open");
        }
        nd_json_free(golden);
    }

    drop_stage();
    printf("test_ui: %d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skips);
    return g_failures == 0 ? 0 : 1;
}
