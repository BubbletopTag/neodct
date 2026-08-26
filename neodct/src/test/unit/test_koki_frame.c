/* test_koki_frame.c -- Koki's reference frame, and the frames on the way to it.
 *
 * golden/app-koki.png is the ONE stored reference for a real-time game, and
 * spec-build-test.md section 3.6 says how it was made: run the app with a
 * 400-frame budget and keep the last frame. Koki never polls read_keypress,
 * so the budget is the only thing that can stop it -- which is also why
 * nd_capture grew a budget in the first place.
 *
 * This drives the built app.so through exactly that recipe:
 *
 *     nd_vclock_enable()          <- goldenframe._Frozen
 *     nd_capture (budget 400)     <- uistub.CapturingFramebuffer
 *     staged ND_ROOT              <- uistub.PathRemap
 *     app_run(ui)                 <- uistub.run_app
 *
 * and compares frame 400 against the stored PNG, byte for byte. Nothing in
 * Koki's render path calls sin() or cos(), so unlike eng-cubebench there is
 * no tolerance here: the answer is zero differing pixels or the port is
 * wrong.
 *
 * ============ WHY THE VIRTUAL CLOCK MAKES THIS POSSIBLE AT ALL ============
 *
 * Every wait, glide and invincibility window in this game is measured against
 * time.monotonic(), so a wall-clock run is not reproducible and could not be
 * compared with anything. goldenframe.py replaces that clock with one that
 * advances 0.1 s per COMMITTED frame; nd_vclock.h is the C substitution, and
 * koki_engine.c reads nd_time_monotonic() rather than clock_gettime()
 * precisely so this test can exist. Note the consequence: under capture a
 * frame is 0.1 s, not 1/30 s, so W(0.05) completes in ONE frame here and in
 * two on the phone. The reference was captured that way too.
 *
 * ============ nd-shoot STILL SKIPS app-koki ============
 *
 * tools/nd_shoot.c:135 lists app-koki as out of scope, and this session may
 * not edit that file. This test is the other route to the same frame: it
 * launches the same app.so through the same capture machinery with the same
 * budget. If it passes, the skip in nd-shoot can be lifted by deleting that
 * one table entry -- see the session report.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_capture.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

/* shoot_docs.py: ("Koki Mobile", [], "app-koki", -1, 400). */
#define KOKI_FRAME_BUDGET 400

#define FONT_REL     "NeoDCT/System/ui/resources/fonts/font.ttf"
#define KOKI_APP_DIR "/NeoDCT/System/apps/Koki"

static int g_checks;
static int g_failures;

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

/* ------------------------------------------------------------------ *
 * Staging -- test_ui.c's symlink farm, for the same reason
 * ------------------------------------------------------------------ */

static char g_stage[ND_PATH_MAX];
static bool g_stage_is_temp;
static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];
static char g_font[ND_PATH_MAX];
static char g_so[ND_PATH_MAX];
static char g_outdir[ND_PATH_MAX];

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

static bool find_reference_dirs(void)
{
    const char *golden = getenv("NEODCT_GOLDEN");

    if (golden != NULL && golden[0] != '\0') {
        if (nd_snprintf(g_golden, sizeof g_golden, "%s", golden) != ND_OK)
            return false;
    } else if (file_exists("../tests/golden/manifest.json")) {
        (void)nd_strlcpy(g_golden, "../tests/golden", sizeof g_golden);
    } else if (file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(g_golden, "neodct/tests/golden", sizeof g_golden);
    } else {
        return false;
    }
    /* <repo>/neodct/tests/golden -> <repo>/neodct/overlay */
    return nd_snprintf(g_overlay, sizeof g_overlay, "%s/../../overlay", g_golden) == ND_OK;
}

static bool stage_root(void)
{
    char tmpl[ND_PATH_MAX];
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    char user[ND_PATH_MAX];
    const char *want = getenv("NEODCT_KOKI_STAGE");

    if (want != NULL && want[0] != '\0') {
        (void)nd_strlcpy(g_stage, want, sizeof g_stage);
        (void)mkdir(g_stage, 0755);
        g_stage_is_temp = false;
    } else {
        const char *base = getenv("TMPDIR");

        if (base == NULL || base[0] == '\0')
            base = "/tmp";
        if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndkoki-XXXXXX", base) != ND_OK)
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

static int unlink_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

static void unstage(void)
{
    (void)nd_path_set_root(NULL);
    if (g_stage_is_temp && g_stage[0] != '\0')
        (void)nftw(g_stage, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* font.ttf is never under ND_ROOT, so it is opened with plain fopen. */
static bool resolve_font(void)
{
    char cand[ND_PATH_MAX];

    if (nd_snprintf(cand, sizeof cand, "%s/" FONT_REL, g_overlay) != ND_OK)
        return false;
    if (!file_exists(cand))
        return false;
    (void)nd_strlcpy(g_font, cand, sizeof g_font);
    return true;
}

/* build/<variant>/test/test_koki_frame -> build/<variant>/apps/Koki/app.so,
 * so an ASan run loads the ASan app and never a stale default-variant one. */
static bool resolve_app_so(void)
{
    const char *env = getenv("NEODCT_KOKI_SO");
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(g_so, env, sizeof g_so);
        return true;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(g_so, sizeof g_so, "%s/../apps/Koki/app.so", exe) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The fixture
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
    nd_capture *cap;
} fixture;

static bool fx_init(fixture *fx)
{
    memset(fx, 0, sizeof *fx);

    /* Koki draws no text in a normal frame -- only the pause dialog does --
     * so a missing face is not fatal here. Loaded anyway so the context is
     * the one an app really gets. */
    fx->font_s = nd_font_load(g_font, ND_FONT_PX_S);
    fx->font_md = nd_font_load(g_font, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(g_font, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(g_font, ND_FONT_PX_XL);

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame, and the surface Koki
     * composites its whole stage into. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    /* 240 * 145 * 3 = 104,400 bytes. Unused by Koki; nd_ui.h says the
     * context carries one. */
    fx->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL || fx->scratch == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;
    if (nd_capture_open(&fx->cap, g_outdir, 0u) != ND_OK) {
        fprintf(stderr, "test_koki_frame: cannot open %s for frames\n", g_outdir);
        return false;
    }

    fx->ui.w = ND_UI_W;
    fx->ui.h = ND_UI_H;
    fx->ui.softkey_h = ND_SOFTKEY_H;
    fx->ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    fx->ui.canvas = fx->canvas;
    fx->ui.scratch = fx->scratch;
    fx->ui.draw = &fx->draw;
    fx->ui.fb = nd_capture_fb(fx->cap);
    fx->ui.font_s = fx->font_s;
    fx->ui.font_md = fx->font_md;
    fx->ui.font_n = fx->font_n;
    fx->ui.font_xl = fx->font_xl;
    /* -1, so Koki's own evdev drain finds nothing and every key reads as
     * released -- uistub's empty KeyScript, exactly. */
    fx->ui.keypad_fd = -1;
    fx->ui.input = NULL;
    fx->ui.softkey_exists = true;
    fx->ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    return fx->ui.image_cache != NULL;
}

static void fx_free(fixture *fx)
{
    nd_capture_close(fx->cap);
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
 * Comparing against the reference
 * ------------------------------------------------------------------ */

/* Differing pixels plus the bounding box, because "three pixels of
 * antialiasing" and "the whole screen" are different bugs and the number is
 * the only thing that says which. */
static int32_t diff_pixels(const nd_image *got, const char *name, nd_rect *box)
{
    char path[ND_PATH_MAX + 32];
    nd_image *ref;
    int32_t n = 0;
    int32_t x;
    int32_t y;

    *box = ND_RECT(0, 0, -1, -1);
    if (nd_snprintf(path, sizeof path, "%s/%s.png", g_golden, name) != ND_OK)
        return -1;
    /* Plain fopen through nd_image_open would be ND_ROOT-resolved, and the
     * reference set is NOT under the staged root -- so the root is cleared
     * for the duration of the read. nd_shoot.c does the same dance for its
     * output directory. */
    {
        char saved[ND_PATH_MAX];

        (void)nd_strlcpy(saved, nd_path_root(), sizeof saved);
        (void)nd_path_set_root(NULL);
        ref = nd_image_open(path);
        (void)nd_path_set_root(saved[0] != '\0' ? saved : NULL);
    }
    if (ref == NULL) {
        fprintf(stderr, "test_koki_frame: cannot read reference %s\n", path);
        return -1;
    }
    if (ref->w != got->w || ref->h != got->h) {
        fprintf(stderr, "test_koki_frame: %s is %dx%d, rendered %dx%d\n", name, ref->w, ref->h,
                got->w, got->h);
        nd_image_free(ref);
        return -1;
    }

    for (y = 0; y < ref->h; y++) {
        for (x = 0; x < ref->w; x++) {
            nd_color a = nd_image_get_px(got, x, y);
            nd_color b = nd_image_get_px(ref, x, y);

            if (a.r != b.r || a.g != b.g || a.b != b.b) {
                if (n == 0)
                    *box = ND_RECT(x, y, x, y);
                else {
                    box->x0 = nd_min32(box->x0, x);
                    box->y0 = nd_min32(box->y0, y);
                    box->x1 = nd_max32(box->x1, x);
                    box->y1 = nd_max32(box->y1, y);
                }
                n++;
            }
        }
    }
    nd_image_free(ref);
    return n;
}

/* ------------------------------------------------------------------ *
 * The run
 * ------------------------------------------------------------------ */

typedef struct {
    void *handle;
    int (*run)(nd_ui *);
    void (*shutdown)(void);
} koki_api;

static bool api_open(koki_api *api)
{
    void *h;

    memset(api, 0, sizeof *api);
    if (!resolve_app_so())
        return false;
    h = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "test_koki_frame: dlopen %s: %s\n", g_so, dlerror());
        return false;
    }
    api->handle = h;
    *(void **)&api->run = dlsym(h, "app_run");
    *(void **)&api->shutdown = dlsym(h, "app_shutdown");
    if (api->run == NULL || api->shutdown == NULL) {
        fprintf(stderr, "test_koki_frame: app.so is missing app_run/app_shutdown\n");
        return false;
    }
    return true;
}

static void test_reference_frame(void)
{
    fixture fx;
    koki_api api;
    const nd_image *frame;
    nd_rect box;
    int32_t diff;
    int rc;

    if (!api_open(&api)) {
        g_failures++;
        return;
    }
    if (!fx_init(&fx)) {
        g_failures++;
        fx_free(&fx);
        (void)dlclose(api.handle);
        return;
    }

    /* nd_app_dir() answers this; the app opens <dir>/assets/manifest.json
     * through it, and the staged root turns that into the repo's overlay. */
    CHECK(nd_app_set_dir(KOKI_APP_DIR) == ND_OK, "app dir set");

    nd_vclock_enable();
    nd_capture_set_budget(fx.cap, KOKI_FRAME_BUDGET);

    rc = api.run(&fx.ui);
    api.shutdown();

    CHECK_INT(rc, 0, "app_run returned 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), KOKI_FRAME_BUDGET, "400 frames committed");
    CHECK(nd_capture_exhausted(fx.cap), "the game stopped on the frame budget, not early");

    frame = nd_capture_recent(fx.cap, 0u);
    CHECK(frame != NULL, "frame 400 was recorded");
    if (frame != NULL) {
        diff = diff_pixels(frame, "app-koki", &box);
        if (diff < 0) {
            g_checks++;
            g_failures++;
        } else {
            printf("  app-koki: %d differing pixels\n", diff);
            if (diff != 0)
                fprintf(stderr, "  diff box (%d,%d)-(%d,%d)\n", box.x0, box.y0, box.x1, box.y1);
            CHECK_INT(diff, 0, "frame 400 matches golden/app-koki.png exactly");
        }
        /* Written whatever the result: a failing frame is much easier to
         * argue about when you can look at it. */
        (void)nd_capture_save(fx.cap, "app-koki", frame);
        (void)nd_capture_write_manifest(fx.cap);
    }

    nd_vclock_disable();
    fx_free(&fx);
    (void)dlclose(api.handle);
}

int main(void)
{
    /* Keep a developer's machine silent and keep the run free of child
     * processes. It reaches no pixel: play_until_done waits the manifest's
     * declared duration rather than the device's. */
    (void)setenv("NEODCT_KOKI_NOSOUND", "1", 1);

    if (!find_reference_dirs()) {
        fprintf(stderr, "test_koki_frame: NEODCT_GOLDEN is not set and the reference set was "
                        "not found; skipping\n");
        return 0;
    }
    if (!resolve_font()) {
        fprintf(stderr, "test_koki_frame: cannot find %s under %s; skipping\n", FONT_REL,
                g_overlay);
        return 0;
    }
    if (!stage_root()) {
        fprintf(stderr, "test_koki_frame: cannot stage a root\n");
        return 1;
    }
    if (nd_snprintf(g_outdir, sizeof g_outdir, "%s/frames", g_stage) != ND_OK) {
        unstage();
        return 1;
    }

    test_reference_frame();

    unstage();
    printf("test_koki_frame: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
