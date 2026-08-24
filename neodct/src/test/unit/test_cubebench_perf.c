/* test_cubebench_perf.c -- how fast the C CubeBench actually is, and why.
 *
 * docs/c-rewrite/PERFORMANCE.md was written before a line of C existed. It
 * decomposed one Python CubeBench frame on this host, found that 75% of it was
 * eight characters of text, and predicted that a naive port would inherit that
 * cost because it is Pillow's FreeType-per-call behaviour and not the
 * interpreter's. It then asked for the number to be MEASURED rather than
 * promised: "Do not promise a 50x cube. Measure it and report what it actually
 * is."
 *
 * This is that measurement. It runs the SHIPPED app.so -- dlopen, app_run(),
 * the real monotonic clock, the same canvas and the same framebuffer sink the
 * golden-frame capture uses -- and then decomposes the frame the same five
 * ways PERFORMANCE.md decomposed the Python's, so the two tables can be read
 * side by side.
 *
 * ============ WHY THIS IS A TEST AND NOT A SCRIPT ============
 *
 * A benchmark that lives in a scratch directory is a number in a commit
 * message that nobody can reproduce six months later. This one runs on every
 * `make test`, against the same app.so the phone ships, so the claim in
 * PERFORMANCE.md stays checkable.
 *
 * ============ WHAT IT ASSERTS, AND WHAT IT ONLY REPORTS ============
 *
 * IT ASSERTS NO TIMING. A unit test that fails when the build machine is busy
 * is a test that gets disabled, and then the frames stop being measured at
 * all. What it asserts is that the measurement is a measurement: the app
 * committed the frames it was asked for, the clock moved, and the cached and
 * uncached text paths drew the SAME PIXELS -- which is the claim that makes
 * the glyph cache free rather than a trade. The times are printed.
 *
 * Under a sanitizer the numbers mean nothing (ASan's -O1 + instrumentation is
 * a different program), so the iteration counts drop and the output says so.
 */

#include <dlfcn.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "nd_capture.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_app.h"
#include "nd_image.h"
#include "nd_keycodes.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* The eight characters PERFORMANCE.md timed: `"FPS %.1f" % 60.0`. Eight is
 * the number the 75% share was measured over, so the comparison only holds if
 * this string stays this long. */
#define FPS_LABEL "FPS 60.0"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif

/* Every figure is the MINIMUM over REPEATS runs, not the mean. Several agents
 * build in this tree at once and a mean would be a measurement of whoever else
 * was compiling; the minimum is the closest this can get to the cost with the
 * machine to itself, and it can only ever be an over-estimate of the true
 * speed of the code, never an under-estimate. */
#define REPEATS 5

#ifdef SANITIZED
#define FRAME_ITERS 60
#define PART_ITERS  200
#define BENCH_NOTE  " (SANITIZER BUILD -- these numbers are not comparable)"
#else
#define FRAME_ITERS 2000
#define PART_ITERS  20000
#define BENCH_NOTE  ""
#endif

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
 * Finding things -- the same three probes test_cubebench.c uses, because
 * every unit test must pass with no arguments from any of the directories
 * the Makefile and the acceptance script invoke it from.
 * ------------------------------------------------------------------ */

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

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen. */
static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    const char *golden = getenv("NEODCT_GOLDEN");
    char cand[ND_PATH_MAX];

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
        return true;
    }
    /* <repo>/neodct/tests/golden -> <repo>/neodct */
    if (golden != NULL && golden[0] != '\0' &&
        nd_snprintf(cand, sizeof cand, "%s/../../" FONT_REL, golden) == ND_OK &&
        file_exists(cand)) {
        (void)nd_strlcpy(out, cand, sz);
        return true;
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

/* build/<variant>/test/... -> build/<variant>/apps/CubeBench/app.so, so a
 * sanitizer run measures the sanitizer app and never a stale default one. */
static bool resolve_app_so(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_CUBEBENCH_SO");
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
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
    return nd_snprintf(out, sz, "%s/../apps/CubeBench/app.so", exe) == ND_OK;
}

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

/* ------------------------------------------------------------------ *
 * The fixture -- the canvas and the sink the golden capture uses
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

static bool fx_init(fixture *fx, size_t ring)
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
    /* 240 * 145 * 3 = 104,400 bytes -- the borrowed column nd_ui.h requires
     * the context to carry. Unused by CubeBench. */
    fx->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL || fx->scratch == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;
    /* A two-slot ring: this run commits thousands of frames and only ever
     * looks at the last one, so a default-sized ring would be megabytes of
     * allocator noise inside the thing being timed. */
    if (nd_capture_open(&fx->cap, g_outdir, ring) != ND_OK)
        return false;

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
    fx->ui.keypad_fd = -1;
    fx->ui.input = NULL; /* read_keypress(0) returns ND_KEY_NONE at once */
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
 * 1. The whole frame
 * ------------------------------------------------------------------ */

static double g_frame_ms;

/* The app is driven exactly as nd-shoot drives it, except that the virtual
 * clock stays OFF: this is a wall-clock measurement, so the app must see real
 * time. The frame budget is what ends the run, the same mechanism that ends
 * the golden capture (OPEN-QUESTIONS.md CB-2), so nothing here is a special
 * benchmark path through the app. */
static void bench_whole_frame(void)
{
    fixture fx;
    void *handle;
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    double t0;
    double t1;
    int32_t rep;

    if (!fx_init(&fx, 2u)) {
        CHECK(false, "fixture");
        fx_free(&fx);
        return;
    }

    handle = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "test_cubebench_perf: %s: %s\n", g_so, dlerror());
        CHECK(false, "the shipped app.so loads");
        fx_free(&fx);
        return;
    }
    run = (int (*)(nd_ui *))(uintptr_t)dlsym(handle, ND_APP_SYM_RUN);
    shutdown = (void (*)(void))(uintptr_t)dlsym(handle, ND_APP_SYM_SHUTDOWN);
    if (run == NULL || shutdown == NULL) {
        CHECK(false, "app_run and app_shutdown are exported");
        (void)dlclose(handle);
        fx_free(&fx);
        return;
    }

    for (rep = 0; rep < REPEATS; rep++) {
        double ms;
        uint64_t before = nd_capture_frames_drawn(fx.cap);

        nd_capture_set_budget(fx.cap, FRAME_ITERS);
        t0 = nd_time_monotonic();
        (void)run(&fx.ui);
        t1 = nd_time_monotonic();

        /* nd_capture_clear_budget() resets the exhausted flag, so the claim
         * that the budget -- and not a key or an error -- ended the loop has
         * to be made before it. frames_drawn counts the capture's whole life,
         * not the budget, so the run's own total is the difference. */
        CHECK_INT(nd_capture_frames_drawn(fx.cap) - before, FRAME_ITERS,
                  "every budgeted frame was committed");
        CHECK(nd_capture_exhausted(fx.cap), "the loop ended on the budget, not on a key");
        CHECK(t1 > t0, "the monotonic clock moved");
        nd_capture_clear_budget(fx.cap);

        ms = (t1 - t0) * 1000.0 / (double)FRAME_ITERS;
        if (rep == 0 || ms < g_frame_ms)
            g_frame_ms = ms;
    }
    shutdown();

    printf("  whole frame          %8.4f ms   %9.0f fps%s\n", g_frame_ms,
           g_frame_ms > 0.0 ? 1000.0 / g_frame_ms : 0.0, BENCH_NOTE);

    (void)dlclose(handle);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2. The same five parts PERFORMANCE.md decomposed the Python into
 * ------------------------------------------------------------------ */

/* One frame's worth of projected vertices, taken from the app's own geometry
 * at a fixed angle so the line and clear timings are over the same pixels
 * every run. Sized and centred like the real thing: 240x145 content area. */
static const int32_t PROJ[8][2] = {
    {94, 46}, {146, 46}, {146, 98}, {94, 98}, {80, 32}, {160, 32}, {160, 112}, {80, 112},
};
static const uint8_t EDGES[12][2] = {
    {0u, 1u}, {1u, 2u}, {2u, 3u}, {3u, 0u}, {4u, 5u}, {5u, 6u},
    {6u, 7u}, {7u, 4u}, {0u, 4u}, {1u, 5u}, {2u, 6u}, {3u, 7u},
};

static double ms_per(double t0, double t1, int32_t iters)
{
    return (t1 - t0) * 1000.0 / (double)iters;
}

static void bench_parts(double *text_ms_out)
{
    fixture fx;
    double t0;
    double t1;
    int32_t i;
    int32_t j;
    int32_t rep;
    double clear_ms = 0.0;
    double lines_ms = 0.0;
    double text_ms = 0.0;
    double blit_ms = 0.0;

    *text_ms_out = 0.0;
    if (!fx_init(&fx, 2u)) {
        CHECK(false, "fixture");
        fx_free(&fx);
        return;
    }

    for (rep = 0; rep < REPEATS; rep++) {
        double ms;

        /* Pillow: draw.rectangle((0, 0, 240, 145), fill="black"). */
        t0 = nd_time_monotonic();
        for (i = 0; i < PART_ITERS; i++)
            (void)nd_draw_rect_fill(fx.ui.draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H - ND_SOFTKEY_H),
                                    ND_BLACK);
        t1 = nd_time_monotonic();
        ms = ms_per(t0, t1, PART_ITERS);
        if (rep == 0 || ms < clear_ms)
            clear_ms = ms;

        /* Pillow: twelve draw.line(..., width=1). */
        t0 = nd_time_monotonic();
        for (i = 0; i < PART_ITERS; i++) {
            for (j = 0; j < 12; j++) {
                int32_t a = EDGES[j][0];
                int32_t b = EDGES[j][1];

                (void)nd_draw_line(fx.ui.draw, PROJ[a][0], PROJ[a][1], PROJ[b][0], PROJ[b][1],
                                   ND_WHITE, 1);
            }
        }
        t1 = nd_time_monotonic();
        ms = ms_per(t0, t1, PART_ITERS);
        if (rep == 0 || ms < lines_ms)
            lines_ms = ms;

        /* Pillow: draw.text((x, 16), "FPS 60.0", font=font_s). THE 75%. */
        t0 = nd_time_monotonic();
        for (i = 0; i < PART_ITERS; i++)
            (void)nd_draw_text(fx.ui.draw, 170, 16, FPS_LABEL, fx.ui.font_s, ND_WHITE);
        t1 = nd_time_monotonic();
        ms = ms_per(t0, t1, PART_ITERS);
        if (rep == 0 || ms < text_ms)
            text_ms = ms;

        /* Pillow: canvas.tobytes() and the write into the mmap. Here it is
         * nd_capture's row-by-row copy, the same 126,000 bytes moved. */
        t0 = nd_time_monotonic();
        for (i = 0; i < PART_ITERS; i++)
            (void)nd_fb_update(fx.ui.fb, fx.canvas);
        t1 = nd_time_monotonic();
        ms = ms_per(t0, t1, PART_ITERS);
        if (rep == 0 || ms < blit_ms)
            blit_ms = ms;
    }

    printf("  clear content rect   %8.4f ms\n", clear_ms);
    printf("  12 wireframe lines   %8.4f ms\n", lines_ms);
    printf("  \"%s\" (cached)  %8.4f ms\n", FPS_LABEL, text_ms);
    printf("  framebuffer commit   %8.4f ms\n", blit_ms);

    *text_ms_out = text_ms;
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 3. What the glyph cache bought
 * ------------------------------------------------------------------ *
 *
 * lib/nd_font.c rasterises all 95 printable ASCII characters into one arena
 * at nd_font_load(), so nd_draw_text() is a blend per character and touches
 * FreeType not at all. Pillow does the opposite: load, hint, rasterise and
 * composite each character on every call, caching nothing between calls --
 * which is where PERFORMANCE.md found 75% of the Python's frame.
 *
 * The cache cannot be switched off at runtime, and adding a switch to
 * lib/nd_font.c to benchmark it would be a change to the most pixel-critical
 * module in the project. So the uncached side is measured by doing what a
 * per-call renderer does, with FreeType directly and with nd_font.c's own
 * three settings (its header comment, points 1-3): FT_Set_Pixel_Sizes in
 * pixels, FT_LOAD_DEFAULT so the font's own hinting runs, and
 * FT_RENDER_MODE_NORMAL for 8-bit coverage. What that leaves out is the
 * compositing, which both paths pay and which the cached measurement above
 * already contains -- so the naive-port figure is the sum of the two.
 */

static void bench_glyph_cache(double cached_text_ms)
{
    FT_Library lib = NULL;
    FT_Face face = NULL;
    double t0;
    double t1;
    double raster_ms = 0.0;
    int32_t i;
    int32_t rep;
    const char *p;

    if (FT_Init_FreeType(&lib) != 0) {
        CHECK(false, "FreeType initialises for the uncached comparison");
        return;
    }
    if (FT_New_Face(lib, g_font, 0, &face) != 0) {
        CHECK(false, "the reference font opens for the uncached comparison");
        (void)FT_Done_FreeType(lib);
        return;
    }
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ND_FONT_PX_S) != 0) {
        CHECK(false, "the small size is settable");
        (void)FT_Done_Face(face);
        (void)FT_Done_FreeType(lib);
        return;
    }

    for (rep = 0; rep < REPEATS; rep++) {
        double ms;

        t0 = nd_time_monotonic();
        for (i = 0; i < PART_ITERS; i++) {
            for (p = FPS_LABEL; *p != '\0'; p++) {
                FT_UInt idx = FT_Get_Char_Index(face, (FT_ULong)(unsigned char)*p);

                if (FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT) != 0)
                    continue;
                (void)FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            }
        }
        t1 = nd_time_monotonic();
        ms = ms_per(t0, t1, PART_ITERS);
        if (rep == 0 || ms < raster_ms)
            raster_ms = ms;
    }

    printf("  \"%s\" FreeType per call:\n", FPS_LABEL);
    printf("    rasterise 8 glyphs %8.4f ms\n", raster_ms);
    printf("    + the same blend   %8.4f ms  = %.4f ms uncached\n", cached_text_ms,
           raster_ms + cached_text_ms);
    if (cached_text_ms > 0.0) {
        printf("    the cache is worth %8.2fx on this call\n",
               (raster_ms + cached_text_ms) / cached_text_ms);
    }

    (void)FT_Done_Face(face);
    (void)FT_Done_FreeType(lib);
}

/* The claim that makes the cache free rather than a trade: a glyph taken out
 * of the arena is the same bitmap FreeType would have produced now. If it
 * were not, every golden frame would be a coin toss. Checked over all 95
 * printable ASCII characters at all four sizes rather than over the eight
 * that happen to be in the label. */
static void test_cache_is_pixel_identical(void)
{
    static const int32_t PX[] = {ND_FONT_PX_S, ND_FONT_PX_MD, ND_FONT_PX_N, ND_FONT_PX_XL};
    FT_Library lib = NULL;
    FT_Face face = NULL;
    size_t s;

    if (FT_Init_FreeType(&lib) != 0 || FT_New_Face(lib, g_font, 0, &face) != 0) {
        CHECK(false, "FreeType opens the reference font");
        if (lib != NULL)
            (void)FT_Done_FreeType(lib);
        return;
    }

    for (s = 0u; s < ND_ARRAY_LEN(PX); s++) {
        nd_font *f = nd_font_load(g_font, PX[s]);
        uint32_t cp;
        bool all_equal = true;

        if (f == NULL || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)PX[s]) != 0) {
            CHECK(false, "a size loads both ways");
            nd_font_free(f);
            continue;
        }
        for (cp = 32u; cp <= 126u; cp++) {
            const nd_glyph *g = nd_font_glyph(f, cp);
            FT_UInt idx = FT_Get_Char_Index(face, (FT_ULong)cp);
            FT_Bitmap *bm;
            int32_t y;

            if (g == NULL || FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT) != 0 ||
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
                all_equal = false;
                break;
            }
            bm = &face->glyph->bitmap;
            if ((int32_t)bm->width != g->ink_w || (int32_t)bm->rows != g->ink_h) {
                all_equal = false;
                break;
            }
            if (g->ink_w > 0 && g->ink_h > 0 && g->coverage == NULL) {
                all_equal = false;
                break;
            }
            for (y = 0; y < g->ink_h; y++) {
                const uint8_t *want = bm->buffer + (ptrdiff_t)y * (ptrdiff_t)bm->pitch;
                const uint8_t *got = g->coverage + (size_t)y * (size_t)g->ink_w;

                if (g->ink_w > 0 && memcmp(want, got, (size_t)g->ink_w) != 0) {
                    all_equal = false;
                    break;
                }
            }
            if (!all_equal)
                break;
        }
        CHECK(all_equal, "every cached glyph equals a freshly rasterised one");
        nd_font_free(f);
    }

    (void)FT_Done_Face(face);
    (void)FT_Done_FreeType(lib);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    const char *base = getenv("TMPDIR");
    char tmpl[ND_PATH_MAX];
    double text_ms = 0.0;

    if (!resolve_font(g_font, sizeof g_font)) {
        printf("test_cubebench_perf: cannot find " FONT_REL "; nothing to measure\n");
        return 1;
    }
    if (!resolve_app_so(g_so, sizeof g_so) || !file_exists(g_so)) {
        printf("test_cubebench_perf: cannot find apps/CubeBench/app.so -- run `make` first\n");
        return 1;
    }

    /* nd_capture_open() resolves through ND_ROOT, and `make test` points that
     * at a shared scratch directory. Clearing it keeps the frames where this
     * test can delete them again. */
    (void)nd_path_set_root(NULL);
    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndcubeperf-XXXXXX", base) != ND_OK ||
        mkdtemp(tmpl) == NULL) {
        printf("test_cubebench_perf: cannot make a scratch directory\n");
        return 1;
    }
    (void)nd_strlcpy(g_outdir, tmpl, sizeof g_outdir);

    /* The oracle's clock must be OFF: this is wall-clock work, and the app
     * reads nd_time_monotonic() once per frame. */
    nd_vclock_disable();

    printf("test_cubebench_perf: %d frames of the shipped app.so, %d iterations per part\n",
           FRAME_ITERS, PART_ITERS);
    bench_whole_frame();
    bench_parts(&text_ms);
    bench_glyph_cache(text_ms);
    test_cache_is_pixel_identical();

    (void)nftw(g_outdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);

    if (g_failures != 0) {
        printf("test_cubebench_perf: %d checks, %d FAILURES\n", g_checks, g_failures);
        return 1;
    }
    printf("test_cubebench_perf: %d checks passed\n", g_checks);
    return 0;
}
