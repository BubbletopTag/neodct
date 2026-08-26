/* test_cubebench.c -- the CubeBench app, arithmetic and pixels.
 *
 * ============ WHY THIS TEST dlopen()s THE APP ============
 *
 * apps/CubeBench builds to app.so, and the Makefile's test rule links a test
 * against libneodct and nothing else. Recompiling main.c into this binary
 * would test a second copy of the source; dlopen()ing the built app.so tests
 * the artefact that actually ships, the same way test_shoot.c spawns the real
 * nd-shoot rather than calling into it. libneodct is already mapped by this
 * process, so the app resolves against the same copy -- which matters,
 * because the virtual clock this test enables has to be the one the app
 * reads.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The derived constants are main.py's: size 31.9, fov 159.5,
 *     view_dist 175.45, centre (120,72) at 240x145. These were tuned by eye
 *     and are load-bearing; a "tidied" 0.22 moves every vertex.
 *  2. The rotations are the Python's three matrices, including the sign
 *     conventions, and are exact at the angles where the answer is exact.
 *  3. _project truncates toward zero and clamps its denominator at 0.1.
 *  4. The FPS window opens at 0.5 s, reports frames/elapsed, and resets its
 *     start to `now` rather than advancing it by 0.5.
 *  5. EXIT_KEYS is exactly {14, 28, 46, 50}.
 *  6. THE FRAME. Driven through nd_capture with a 60-frame budget and the
 *     virtual clock, app_run() must reproduce golden/eng-cubebench.png. That
 *     is the same 60 frames uistub gives it: shoot_docs.py runs CubeBench
 *     with the default idle_budget of 60, so the 61st read_keypress raises
 *     ScriptExhausted and the 60th frame is the one saved.
 *
 * ============ THE TOLERANCE ON FRAME 60 ============
 *
 * OPEN-QUESTIONS.md puts eng-cubebench in the `tolerance` class: the cube's
 * vertices go through sin() and cos(), and glibc, musl and uClibc-ng do not
 * agree to the last bit, so a one-ULP difference can move a vertex by a
 * pixel. The cap here is ND_CUBEBENCH_PIXEL_CAP differing pixels, which is
 * "single-digit pixels along the wireframe edges" -- the honest budget the
 * policy names. It is a budget, not an excuse: on the host that captured the
 * reference the delta is expected to be ZERO, and the test prints whatever it
 * actually is so a regression cannot hide inside the allowance.
 */

#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_json.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#include "../../apps/CubeBench/cubebench.h"

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* uistub.StubUI's default idle_budget. The 61st poll raises, so 60 frames
 * reach the framebuffer and frames[-1] is the sixtieth. */
#define ND_CUBEBENCH_FRAMES 60

/* Single-digit pixels along the wireframe edges. See the header comment. */
#define ND_CUBEBENCH_PIXEL_CAP 9

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

/* Exact equality on purpose: every value checked with this is one IEEE
 * multiplication away from an integer, so an epsilon would hide the very
 * rounding the port has to get right. */
#define CHECK_EXACT(got, want, what)                                                              \
    do {                                                                                          \
        double g_ = (got);                                                                        \
        double w_ = (want);                                                                       \
        g_checks++;                                                                               \
        if (g_ != w_) {                                                                           \
            g_failures++;                                                                         \
            fprintf(stderr, "FAIL %s:%d  %s: got %.17g want %.17g\n", __FILE__, __LINE__, (what), \
                    g_, w_);                                                                      \
        }                                                                                         \
    } while (0)

#define CHECK_NEAR(got, want, tol, what)                                                          \
    do {                                                                                          \
        double g_ = (got);                                                                        \
        double w_ = (want);                                                                       \
        g_checks++;                                                                               \
        if (!(fabs(g_ - w_) <= (tol))) {                                                          \
            g_failures++;                                                                         \
            fprintf(stderr, "FAIL %s:%d  %s: got %.17g want %.17g\n", __FILE__, __LINE__, (what), \
                    g_, w_);                                                                      \
        }                                                                                         \
    } while (0)

/* ------------------------------------------------------------------ *
 * Finding things
 * ------------------------------------------------------------------ */

static char g_golden[ND_PATH_MAX];
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

    if (g_golden[0] == '\0' && !resolve_golden(g_golden, sizeof g_golden))
        return false;
    (void)snprintf(base, sizeof base, "%.480s", g_golden);
    cut = strrchr(base, '/');
    if (cut != NULL)
        *cut = '\0';
    cut = strrchr(base, '/');
    if (cut != NULL)
        *cut = '\0';
    (void)nd_strlcpy(out, base, sz);
    return true;
}

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen. */
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

/* build/<variant>/test/test_cubebench -> build/<variant>/apps/CubeBench/app.so,
 * so an ASan run loads the ASan app and never a stale default-variant one. */
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

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

typedef struct {
    void *handle;
    int (*run)(nd_ui *ui);
    void (*shutdown)(void);
    void (*geom_init)(nd_cubebench_geom *, int32_t, int32_t);
    void (*rot_x)(const double *, double, double *);
    void (*rot_y)(const double *, double, double *);
    void (*rot_z)(const double *, double, double *);
    void (*project)(const double *, int32_t, int32_t, double, double, int32_t *, int32_t *);
    void (*fps_init)(nd_cubebench_fps *, double);
    void (*fps_tick)(nd_cubebench_fps *, double, double);
    bool (*is_exit_key)(int32_t);
    const uint8_t (*edges)[2];
} cube_api;

static cube_api g_api;

/* dlsym returns void*, and ISO C has no conversion between an object pointer
 * and a function pointer. POSIX requires this one to work and says to launder
 * it through an integer of the right width; -Wpedantic is not in the warning
 * set, but the cast is written this way so it stays correct if it ever is. */
static void *sym(void *h, const char *name)
{
    void *p = dlsym(h, name);

    if (p == NULL)
        fprintf(stderr, "test_cubebench: app.so has no symbol %s\n", name);
    return p;
}

static bool api_open(void)
{
    void *h;

    if (!resolve_app_so(g_so, sizeof g_so))
        return false;
    h = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "test_cubebench: dlopen %s: %s\n", g_so, dlerror());
        return false;
    }
    g_api.handle = h;

    *(void **)&g_api.run = sym(h, "app_run");
    *(void **)&g_api.shutdown = sym(h, "app_shutdown");
    *(void **)&g_api.geom_init = sym(h, "nd_cubebench_geom_init");
    *(void **)&g_api.rot_x = sym(h, "nd_cubebench_rotate_x");
    *(void **)&g_api.rot_y = sym(h, "nd_cubebench_rotate_y");
    *(void **)&g_api.rot_z = sym(h, "nd_cubebench_rotate_z");
    *(void **)&g_api.project = sym(h, "nd_cubebench_project");
    *(void **)&g_api.fps_init = sym(h, "nd_cubebench_fps_init");
    *(void **)&g_api.fps_tick = sym(h, "nd_cubebench_fps_tick");
    *(void **)&g_api.is_exit_key = sym(h, "nd_cubebench_is_exit_key");
    g_api.edges = dlsym(h, "nd_cubebench_edges");

    return g_api.run != NULL && g_api.shutdown != NULL && g_api.geom_init != NULL &&
           g_api.rot_x != NULL && g_api.rot_y != NULL && g_api.rot_z != NULL &&
           g_api.project != NULL && g_api.fps_init != NULL && g_api.fps_tick != NULL &&
           g_api.is_exit_key != NULL && g_api.edges != NULL;
}

static void api_close(void)
{
    if (g_api.handle != NULL)
        (void)dlclose(g_api.handle);
    memset(&g_api, 0, sizeof g_api);
}

/* ------------------------------------------------------------------ *
 * 1. The derived constants
 * ------------------------------------------------------------------ */

static void test_geometry(void)
{
    nd_cubebench_geom g;
    int32_t i;
    int32_t n_neg_x = 0;

    memset(&g, 0, sizeof g);
    g_api.geom_init(&g, ND_UI_W, ND_UI_H - ND_SOFTKEY_H);

    CHECK_INT(g.center_x, 120, "center_x = screen_w // 2");
    CHECK_INT(g.center_y, 72, "center_y = content_bottom // 2");
    CHECK_EXACT(g.size, 145.0 * 0.22, "size = min(w, content_bottom) * 0.22");
    CHECK_EXACT(g.fov, 145.0 * 1.1, "fov = min(w, content_bottom) * 1.1");
    CHECK_EXACT(g.view_dist, g.size * 5.5, "view_dist = size * 5.5");
    /* The numbers as the Python prints them, so a reader can check them
     * against main.py without running anything. */
    CHECK_NEAR(g.size, 31.9, 1e-12, "size is 31.9");
    CHECK_NEAR(g.fov, 159.5, 1e-12, "fov is 159.5");
    CHECK_NEAR(g.view_dist, 175.45, 1e-12, "view_dist is 175.45");

    /* Every corner is +/- size on all three axes, and the eight are
     * distinct -- which is the whole content of the vertex list. */
    for (i = 0; i < ND_CUBEBENCH_N_VERTICES; i++) {
        CHECK_EXACT(fabs(g.vertices[i][0]), g.size, "|x| == size");
        CHECK_EXACT(fabs(g.vertices[i][1]), g.size, "|y| == size");
        CHECK_EXACT(fabs(g.vertices[i][2]), g.size, "|z| == size");
        if (g.vertices[i][0] < 0.0)
            n_neg_x++;
    }
    CHECK_INT(n_neg_x, 4, "four corners on each side of x");

    /* main.py's order, spot-checked at both ends. */
    CHECK_EXACT(g.vertices[0][2], -g.size, "vertex 0 is the -z face");
    CHECK_EXACT(g.vertices[7][2], g.size, "vertex 7 is the +z face");
    CHECK_EXACT(g.vertices[7][0], -g.size, "vertex 7 x");
    CHECK_EXACT(g.vertices[7][1], g.size, "vertex 7 y");

    /* Twelve edges, each vertex meeting three of them: a cube, not a
     * mistyped list. */
    {
        int32_t degree[ND_CUBEBENCH_N_VERTICES] = {0};

        for (i = 0; i < ND_CUBEBENCH_N_EDGES; i++) {
            int32_t a = g_api.edges[i][0];
            int32_t b = g_api.edges[i][1];

            CHECK(a >= 0 && a < ND_CUBEBENCH_N_VERTICES, "edge endpoint in range");
            CHECK(b >= 0 && b < ND_CUBEBENCH_N_VERTICES, "edge endpoint in range");
            degree[a]++;
            degree[b]++;
        }
        for (i = 0; i < ND_CUBEBENCH_N_VERTICES; i++)
            CHECK_INT(degree[i], 3, "every corner meets three edges");
    }
}

/* ------------------------------------------------------------------ *
 * 2. The rotations
 * ------------------------------------------------------------------ */

static void test_rotations(void)
{
    const double v[3] = {1.0, 2.0, 3.0};
    double out[3];

    /* Zero angle is the identity, exactly: cos(0) is 1.0 and sin(0) is 0.0
     * in every libm, so this one really is bit-for-bit. */
    g_api.rot_x(v, 0.0, out);
    CHECK_EXACT(out[0], 1.0, "rotate_x(0) x");
    CHECK_EXACT(out[1], 2.0, "rotate_x(0) y");
    CHECK_EXACT(out[2], 3.0, "rotate_x(0) z");

    g_api.rot_y(v, 0.0, out);
    CHECK_EXACT(out[0], 1.0, "rotate_y(0) x");
    CHECK_EXACT(out[1], 2.0, "rotate_y(0) y");
    CHECK_EXACT(out[2], 3.0, "rotate_y(0) z");

    g_api.rot_z(v, 0.0, out);
    CHECK_EXACT(out[0], 1.0, "rotate_z(0) x");
    CHECK_EXACT(out[1], 2.0, "rotate_z(0) y");
    CHECK_EXACT(out[2], 3.0, "rotate_z(0) z");

    /* A quarter turn fixes the axis and swaps the other two with one sign
     * flip. This is where a transposed matrix shows up. */
    g_api.rot_x(v, M_PI / 2.0, out);
    CHECK_NEAR(out[0], 1.0, 1e-12, "rotate_x(90) fixes x");
    CHECK_NEAR(out[1], -3.0, 1e-12, "rotate_x(90) y = y*ca - z*sa");
    CHECK_NEAR(out[2], 2.0, 1e-12, "rotate_x(90) z = y*sa + z*ca");

    g_api.rot_y(v, M_PI / 2.0, out);
    CHECK_NEAR(out[0], 3.0, 1e-12, "rotate_y(90) x = x*ca + z*sa");
    CHECK_NEAR(out[1], 2.0, 1e-12, "rotate_y(90) fixes y");
    CHECK_NEAR(out[2], -1.0, 1e-12, "rotate_y(90) z = -x*sa + z*ca");

    g_api.rot_z(v, M_PI / 2.0, out);
    CHECK_NEAR(out[0], -2.0, 1e-12, "rotate_z(90) x = x*ca - y*sa");
    CHECK_NEAR(out[1], 1.0, 1e-12, "rotate_z(90) y = x*sa + y*ca");
    CHECK_NEAR(out[2], 3.0, 1e-12, "rotate_z(90) fixes z");

    /* Rotation preserves length; a dropped term would not. */
    {
        double len_in = sqrt(1.0 + 4.0 + 9.0);

        g_api.rot_x(v, 0.7, out);
        g_api.rot_y(out, 1.3, out);
        g_api.rot_z(out, 2.9, out);
        CHECK_NEAR(sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]), len_in, 1e-12,
                   "three rotations preserve length");
    }

    /* out aliasing v is how run() uses these: r = _rotate_x(v); r =
     * _rotate_y(r) in Python allocates a new tuple, C writes in place. */
    {
        double a[3] = {1.0, 2.0, 3.0};

        g_api.rot_z(a, M_PI / 2.0, a);
        CHECK_NEAR(a[0], -2.0, 1e-12, "in-place rotate x");
        CHECK_NEAR(a[1], 1.0, 1e-12, "in-place rotate y");
    }
}

/* ------------------------------------------------------------------ *
 * 3. The projection
 * ------------------------------------------------------------------ */

static void test_projection(void)
{
    double v[3];
    int32_t sx = 0;
    int32_t sy = 0;

    /* Dead centre: no offset survives the multiply, whatever the scale. */
    v[0] = 0.0;
    v[1] = 0.0;
    v[2] = 0.0;
    g_api.project(v, 120, 72, 159.5, 175.45, &sx, &sy);
    CHECK_INT(sx, 120, "centre projects to centre x");
    CHECK_INT(sy, 72, "centre projects to centre y");

    /* int() TRUNCATES toward zero. scale = 100/100 = 1, so x lands at
     * 120 + 8.5 = 128.5 -> 128, and y at 72 - 8.5 = 63.5 -> 63. A round()
     * would give 129 and 64: two pixels of difference on one vertex. */
    v[0] = 8.5;
    v[1] = -8.5;
    v[2] = 0.0;
    g_api.project(v, 120, 72, 100.0, 100.0, &sx, &sy);
    CHECK_INT(sx, 128, "positive coordinate truncates down");
    CHECK_INT(sy, 63, "negative coordinate truncates toward zero");

    /* The clamp: z + view_dist below 0.1 is pinned at 0.1, so scale becomes
     * fov * 10 rather than a division by zero. */
    v[0] = 1.0;
    v[1] = 0.0;
    v[2] = -100.0;
    g_api.project(v, 0, 0, 1.0, 100.0, &sx, &sy);
    CHECK_INT(sx, 10, "denominator clamps at 0.1");

    v[2] = -1000.0;
    g_api.project(v, 0, 0, 1.0, 100.0, &sx, &sy);
    CHECK_INT(sx, 10, "a vertex behind the camera clamps too, it does not flip");

    /* Nearer is bigger: the whole point of the perspective divide. */
    {
        int32_t near_x;
        int32_t far_x;

        v[0] = 10.0;
        v[1] = 0.0;
        v[2] = -20.0;
        g_api.project(v, 120, 72, 159.5, 175.45, &near_x, &sy);
        v[2] = 20.0;
        g_api.project(v, 120, 72, 159.5, 175.45, &far_x, &sy);
        CHECK(near_x > far_x, "a nearer vertex projects further from centre");
    }
}

/* ------------------------------------------------------------------ *
 * 4. The FPS window
 * ------------------------------------------------------------------ */

static void test_fps_window(void)
{
    nd_cubebench_fps f;
    int32_t i;

    g_api.fps_init(&f, 100.0);
    CHECK_EXACT(f.display, 0.0, "display starts at zero");
    CHECK_INT(f.counter, 0, "counter starts at zero");

    /* Five 0.1 s frames is 0.5 s, but the window is measured from `now`, and
     * `now` on the fifth call is 100.4 -- 0.4 elapsed, so it does not close.
     * The sixth, at 100.5, does, and reports six frames over 0.5 s = 12.0.
     * That 12.0 is the first number eng-cubebench's label ever shows, and it
     * is why frame 6 of the reference reads "FPS 12.0" and frame 5 "FPS 0.0". */
    for (i = 0; i < 5; i++)
        g_api.fps_tick(&f, 100.0 + 0.1 * (double)i, 0.1);
    CHECK_EXACT(f.display, 0.0, "window still open at 0.4 s");
    CHECK_INT(f.counter, 5, "five frames counted");

    g_api.fps_tick(&f, 100.5, 0.1);
    CHECK_NEAR(f.display, 12.0, 1e-9, "six frames over 0.5 s");
    CHECK_INT(f.counter, 0, "counter reset");
    CHECK_EXACT(f.window_start, 100.5, "window restarts at now, not at start+0.5");

    /* Every window after the first holds five frames, because the start
     * moved to the frame that closed the previous one. 5 / 0.5 = 10.0, which
     * is what the reference frame's label reads. */
    for (i = 1; i <= 5; i++)
        g_api.fps_tick(&f, 100.5 + 0.1 * (double)i, 0.1);
    CHECK_NEAR(f.display, 10.0, 1e-9, "five frames over 0.5 s");

    /* inst is 1/dt and is computed but never drawn -- main.py assigns
     * fps_inst and then only ever shows fps_display. Ported as-is. */
    g_api.fps_tick(&f, 101.1, 0.02);
    CHECK_NEAR(f.inst, 50.0, 1e-9, "instantaneous reading is 1/dt");
}

/* ------------------------------------------------------------------ *
 * 5. EXIT_KEYS
 * ------------------------------------------------------------------ */

static void test_exit_keys(void)
{
    int32_t code;
    int32_t n = 0;

    /* {14, 28, 46, 50}: BACK, ENTER, C, MENU. Written as the numbers
     * main.py holds so a renumbered keycode header cannot silently move
     * them. */
    CHECK(g_api.is_exit_key(14), "14 exits");
    CHECK(g_api.is_exit_key(28), "28 exits");
    CHECK(g_api.is_exit_key(46), "46 exits");
    CHECK(g_api.is_exit_key(50), "50 exits");

    for (code = -1; code < 256; code++) {
        if (g_api.is_exit_key(code))
            n++;
    }
    CHECK_INT(n, 4, "exactly four exit keys");
    CHECK(!g_api.is_exit_key(-1), "ND_KEY_NONE does not exit");
}

/* ------------------------------------------------------------------ *
 * 6. The frame
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

    fx->font_s = nd_font_load(g_font, ND_FONT_PX_S);
    fx->font_md = nd_font_load(g_font, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(g_font, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(g_font, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "test_cubebench: nd_font_load(%s) failed\n", g_font);
        return false;
    }

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    /* 240 * 145 * 3 = 104,400 bytes -- DetailPage's borrowed column. Unused
     * here, but nd_ui.h says the context carries one. */
    fx->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL || fx->scratch == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;
    if (nd_capture_open(&fx->cap, g_outdir, 0u) != ND_OK) {
        fprintf(stderr, "test_cubebench: cannot open %s for frames\n", g_outdir);
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
    fx->ui.keypad_fd = -1;
    fx->ui.input = NULL; /* read_keypress(0) returns ND_KEY_NONE at once */
    /* The core's own bar already exists by the time any app runs, so an
     * app's SoftKeyBar is the opaque one. */
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

/* ---- the reference ---- */

static nd_json_doc *g_manifest;
static const nd_json_val *g_frames;

static void manifest_open(void)
{
    char path[ND_PATH_MAX + 32];
    uint8_t *buf = NULL;
    long len;
    FILE *f;
    char err[128];

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
        fprintf(stderr, "test_cubebench: manifest parse: %s\n", err);
        g_manifest = NULL;
        goto done;
    }
    g_frames = nd_json_get(nd_json_root(g_manifest), "frames");
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

/* Differing pixels between the rendered frame and the stored PNG, with the
 * bounding box, because "three pixels of antialiasing" and "the whole screen"
 * are different bugs and the number is the only thing that says which. */
static int32_t diff_pixels(const nd_image *got, const char *name, nd_rect *box)
{
    char path[ND_PATH_MAX + 32];
    nd_image *ref;
    int32_t n = 0;
    int32_t x;
    int32_t y;

    *box = ND_RECT(0, 0, -1, -1);
    (void)snprintf(path, sizeof path, "%.1000s/%.40s.png", g_golden, name);
    /* nd_image_load_png() resolves through ND_ROOT; the reference set is not
     * under it, and this test never sets one. */
    ref = nd_image_load_png(path);
    if (ref == NULL) {
        fprintf(stderr, "test_cubebench: cannot read %s\n", path);
        return -1;
    }
    if (ref->w != got->w || ref->h != got->h) {
        fprintf(stderr, "test_cubebench: %s is %dx%d, rendered %dx%d\n", name, ref->w, ref->h,
                got->w, got->h);
        nd_image_free(ref);
        return -1;
    }

    for (y = 0; y < ref->h; y++) {
        for (x = 0; x < ref->w; x++) {
            nd_color a = nd_image_get_px(ref, x, y);
            nd_color b = nd_image_get_px(got, x, y);

            if (a.r == b.r && a.g == b.g && a.b == b.b)
                continue;
            if (n == 0)
                *box = ND_RECT(x, y, x, y);
            if (x < box->x0)
                box->x0 = x;
            if (x > box->x1)
                box->x1 = x;
            if (y < box->y0)
                box->y0 = y;
            if (y > box->y1)
                box->y1 = y;
            n++;
        }
    }
    nd_image_free(ref);
    return n;
}

static void test_golden_frame(void)
{
    fixture fx;
    const nd_image *frame;
    char got[65];
    const char *want;
    int rc;

    if (!fx_init(&fx)) {
        CHECK(false, "fixture");
        fx_free(&fx);
        return;
    }

    /* The three substitutions goldenframe.py makes: the virtual clock in
     * place of _Frozen, nd_capture in place of CapturingFramebuffer, and the
     * 60-frame budget in place of KeyScript's idle_budget. */
    nd_vclock_enable();
    nd_capture_set_budget(fx.cap, ND_CUBEBENCH_FRAMES);

    rc = g_api.run(&fx.ui);
    g_api.shutdown();

    CHECK_INT(rc, 0, "app_run returns 0 when the frames run out");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), ND_CUBEBENCH_FRAMES, "60 frames committed");
    CHECK(nd_capture_exhausted(fx.cap), "the loop stopped on the frame budget, not early");
    /* The clock advances once per COMMITTED frame; the refused 61st must not
     * have ticked it. */
    CHECK_INT(nd_vclock_frame(), ND_CUBEBENCH_FRAMES, "virtual clock advanced once per frame");

    frame = nd_capture_recent(fx.cap, 0u);
    if (frame == NULL) {
        CHECK(false, "frames[-1] exists");
        nd_vclock_disable();
        fx_free(&fx);
        return;
    }

    want = golden_sha("eng-cubebench");
    if (nd_capture_digest(frame, got, sizeof got) != ND_OK) {
        CHECK(false, "digest");
    } else if (want == NULL) {
        fprintf(stderr, "test_cubebench: no reference for eng-cubebench (got %s)\n", got);
        CHECK(false, "reference present");
    } else if (strcmp(got, want) == 0) {
        printf("  eng-cubebench: BYTE-EXACT (0 differing pixels)\n");
        g_checks++;
    } else {
        nd_rect box;
        int32_t n = diff_pixels(frame, "eng-cubebench", &box);
        int32_t total = frame->w * frame->h;

        if (n < 0) {
            CHECK(false, "cannot measure the delta against eng-cubebench.png");
        } else {
            printf("  eng-cubebench: %d of %d pixels differ (%.2f%%), box (%d,%d)-(%d,%d)\n", n,
                   total, 100.0 * (double)n / (double)total, box.x0, box.y0, box.x1, box.y1);
            /* A tolerance is a budget, not an excuse: OPEN-QUESTIONS.md. */
            CHECK(n <= ND_CUBEBENCH_PIXEL_CAP, "eng-cubebench within the libm tolerance budget");
        }
        /* Leave the frame where a human can look at it. */
        (void)nd_capture_save(fx.cap, "eng-cubebench", frame);
        (void)nd_capture_write_manifest(fx.cap);
        fprintf(stderr, "test_cubebench: rendered frame written to %s\n", g_outdir);
    }

    nd_vclock_disable();
    fx_free(&fx);
}

/* One frame twice from a fresh clock has to be the same frame, or the
 * reference could not be compared with anything. */
static void test_deterministic(void)
{
    fixture a;
    fixture b;
    char da[65] = "";
    char db[65] = "";

    if (!fx_init(&a)) {
        CHECK(false, "fixture a");
        fx_free(&a);
        return;
    }
    nd_vclock_enable();
    nd_capture_set_budget(a.cap, 7);
    (void)g_api.run(&a.ui);
    (void)nd_capture_digest(nd_capture_recent(a.cap, 0u), da, sizeof da);
    nd_vclock_disable();
    fx_free(&a);

    if (!fx_init(&b)) {
        CHECK(false, "fixture b");
        fx_free(&b);
        return;
    }
    nd_vclock_enable();
    nd_capture_set_budget(b.cap, 7);
    (void)g_api.run(&b.ui);
    (void)nd_capture_digest(nd_capture_recent(b.cap, 0u), db, sizeof db);
    nd_vclock_disable();
    fx_free(&b);

    CHECK(da[0] != '\0' && strcmp(da, db) == 0, "two runs render the same frame");
}

/* ------------------------------------------------------------------ *
 * 7. Robustness
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    nd_cubebench_geom g;
    nd_cubebench_fps f;

    /* An app that dereferences a NULL ui kills only itself now, but there is
     * no reason to make the core prove it. */
    CHECK_INT(g_api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    g_api.shutdown(); /* must be safe with nothing held */

    g_api.geom_init(NULL, 240, 145);
    g_api.fps_init(NULL, 0.0);
    g_api.fps_tick(NULL, 0.0, 0.1);
    {
        /* Both output pointers NULL: the projection has nowhere to write and
         * must not write anywhere anyway. */
        const double origin[3] = {0.0, 0.0, 0.0};

        g_api.project(origin, 0, 0, 1.0, 1.0, NULL, NULL);
    }

    /* And a degenerate screen must not divide by zero or read off the end. */
    memset(&g, 0, sizeof g);
    g_api.geom_init(&g, 0, 0);
    CHECK_EXACT(g.size, 0.0, "a zero-sized screen gives a zero-sized cube");
    CHECK_INT(g.center_x, 0, "centre of nothing");

    g_api.fps_init(&f, 0.0);
    g_api.fps_tick(&f, 0.0, 0.001);
    CHECK_NEAR(f.inst, 1000.0, 1e-9, "1/0.001");
    g_checks++; /* reaching here without a fault is the claim */
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char tmpl[ND_PATH_MAX];

    if (!resolve_golden(g_golden, sizeof g_golden)) {
        fprintf(stderr, "test_cubebench: cannot find the golden set; set NEODCT_GOLDEN\n");
        return 1;
    }
    if (!resolve_font(g_font, sizeof g_font)) {
        fprintf(stderr, "test_cubebench: cannot find font.ttf; set NEODCT_FONT\n");
        return 1;
    }
    if (!api_open()) {
        fprintf(stderr, "test_cubebench: %s is not loadable -- run `make` first\n", g_so);
        return 1;
    }

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndcube-XXXXXX", tmp) != ND_OK)
        return 1;
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "test_cubebench: mkdtemp under %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    (void)nd_strlcpy(g_outdir, tmpl, sizeof g_outdir);

    manifest_open();

    test_geometry();
    test_rotations();
    test_projection();
    test_fps_window();
    test_exit_keys();
    test_golden_frame();
    test_deterministic();
    test_null_safety();

    nd_json_free(g_manifest);
    api_close();

    if (g_failures == 0)
        (void)nftw(g_outdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);

    if (g_failures != 0) {
        fprintf(stderr, "test_cubebench: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_cubebench: %d checks passed\n", g_checks);
    return 0;
}
