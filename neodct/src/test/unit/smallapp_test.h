/* smallapp_test.h -- the fixture the four small stock-app tests share.
 *
 * test_calculator.c, test_clock_app.c, test_power.c and test_tones.c all need
 * the same four things and nothing else:
 *
 *   1. a check macro,
 *   2. a minimal nd_ui with real fonts, a real canvas and an nd_capture
 *      framebuffer -- the same shape test_cubebench.c builds, because an app
 *      that draws needs a context and there is no smaller one,
 *   3. a key channel it can write scripted presses into, so a blocking
 *      widget can be let out of its loop,
 *   4. the golden manifest, so a rendered frame can be judged by the digest
 *      goldenframe.py compares rather than by eye.
 *
 * Four copies of that would have been six hundred lines of duplicated
 * fixture. It is a header rather than a .c because test/unit/*.c is globbed
 * into one binary per file, so a shared .c there would become a test with no
 * main(); draw_ref.inc and platform_test.h are here for the same reason.
 *
 * ============ WHY THE APPS ARE dlopen()ed ============
 *
 * Each app builds to apps/<Name>/app.so and the Makefile's test rule links a
 * test against libneodct and nothing else. Recompiling an app's main.c into
 * the test binary would test a second copy of the source with different
 * flags; dlopen()ing the built .so tests the artefact that ships. That is
 * what test_cubebench.c and test_phonebook.c do and this follows them.
 *
 * ============ TWO WAYS TO LET A BLOCKING SCREEN GO ============
 *
 * sa_send() queues a press and its release. That is enough for a screen that
 * does NOT flush the channel before its first draw -- Calculator's own loop
 * and VerticalList.
 *
 * sa_hold() presses a key and never releases it, with that code in the repeat
 * set. MessageDialog and PagedList drain the channel before drawing, which
 * eats a queued press but leaves the held state behind it, and the
 * synthesised repeat then arrives after the screen is up. It is the same
 * trick nd-shoot uses on app-clock, and for the same reason: C cannot raise
 * uistub's ScriptExhausted out of a read.
 */

#ifndef SMALLAPP_TEST_H_INCLUDED
#define SMALLAPP_TEST_H_INCLUDED

#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
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
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keypad.h"
#include "nd_keycodes.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#define SA_FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

static int sa_checks;
static int sa_failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        sa_checks++;                                                         \
        if (!(cond)) {                                                       \
            sa_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want, what)                                                  \
    do {                                                                            \
        const char *g_ = (got);                                                     \
        const char *w_ = (want);                                                    \
        sa_checks++;                                                                \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                                    \
            sa_failures++;                                                          \
            fprintf(stderr, "FAIL %s:%d  %s: got \"%s\" want \"%s\"\n", __FILE__,   \
                    __LINE__, (what), (g_ != NULL) ? g_ : "(null)", w_);            \
        }                                                                           \
    } while (0)

#define CHECK_INT(got, want, what)                                                            \
    do {                                                                                      \
        long long g_ = (long long)(got);                                                      \
        long long w_ = (long long)(want);                                                     \
        sa_checks++;                                                                          \
        if (g_ != w_) {                                                                       \
            sa_failures++;                                                                    \
            fprintf(stderr, "FAIL %s:%d  %s: got %lld want %lld\n", __FILE__, __LINE__,       \
                    (what), g_, w_);                                                          \
        }                                                                                     \
    } while (0)

#define CHECK_DBL(got, want, what)                                                            \
    do {                                                                                      \
        double g_ = (got);                                                                    \
        double w_ = (want);                                                                   \
        sa_checks++;                                                                          \
        if (g_ != w_) {                                                                       \
            sa_failures++;                                                                    \
            fprintf(stderr, "FAIL %s:%d  %s: got %.17g want %.17g\n", __FILE__, __LINE__,     \
                    (what), g_, w_);                                                          \
        }                                                                                     \
    } while (0)

#define RUN(fn)     \
    do {            \
        fn();       \
    } while (0)

/* ------------------------------------------------------------------ *
 * Finding things
 * ------------------------------------------------------------------ */

static char sa_golden[ND_PATH_MAX];
static char sa_neodct[ND_PATH_MAX];
static char sa_font[ND_PATH_MAX];
static char sa_outdir[ND_PATH_MAX];

ND_UNUSED_FN static bool sa_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

/* NEODCT_GOLDEN is what the Makefile passes; the two relative guesses are for
 * a developer running the binary by hand. */
ND_UNUSED_FN static bool sa_resolve_golden(void)
{
    const char *env = getenv("NEODCT_GOLDEN");

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(sa_golden, env, sizeof sa_golden);
        return true;
    }
    if (sa_file_exists("../tests/golden/manifest.json")) {
        (void)nd_strlcpy(sa_golden, "../tests/golden", sizeof sa_golden);
        return true;
    }
    if (sa_file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(sa_golden, "neodct/tests/golden", sizeof sa_golden);
        return true;
    }
    return false;
}

/* <repo>/neodct/tests/golden -> <repo>/neodct */
ND_UNUSED_FN static bool sa_resolve_neodct(void)
{
    char base[ND_PATH_MAX];
    char *cut;

    if (sa_golden[0] == '\0' && !sa_resolve_golden())
        return false;
    (void)snprintf(base, sizeof base, "%.480s", sa_golden);
    cut = strrchr(base, '/');
    if (cut != NULL)
        *cut = '\0';
    cut = strrchr(base, '/');
    if (cut != NULL)
        *cut = '\0';
    (void)nd_strlcpy(sa_neodct, base, sizeof sa_neodct);
    return true;
}

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen. */
ND_UNUSED_FN static bool sa_resolve_font(void)
{
    const char *env = getenv("NEODCT_FONT");
    char cand[ND_PATH_MAX];

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(sa_font, env, sizeof sa_font);
        return true;
    }
    if (sa_neodct[0] != '\0' || sa_resolve_neodct()) {
        (void)snprintf(cand, sizeof cand, "%.400s/" SA_FONT_REL, sa_neodct);
        if (sa_file_exists(cand)) {
            (void)nd_strlcpy(sa_font, cand, sizeof sa_font);
            return true;
        }
    }
    if (sa_file_exists("../" SA_FONT_REL)) {
        (void)nd_strlcpy(sa_font, "../" SA_FONT_REL, sizeof sa_font);
        return true;
    }
    if (sa_file_exists(ND_PATH_FONT)) {
        (void)nd_strlcpy(sa_font, ND_PATH_FONT, sizeof sa_font);
        return true;
    }
    return false;
}

/* <repo>/neodct/overlay, which is what ND_ROOT has to be for an absolute
 * "/NeoDCT/System/..." asset path to resolve onto the reference tree. Only a
 * frame test that needs an icon uses it, and only for reading -- nothing in
 * these tests writes with this root set. */
ND_UNUSED_FN static bool sa_overlay_root(char *out, size_t out_sz)
{
    if (sa_neodct[0] == '\0' && !sa_resolve_neodct())
        return false;
    return nd_snprintf(out, out_sz, "%s/overlay", sa_neodct) == ND_OK;
}

/* build/<variant>/test/<test> -> build/<variant>/apps/<Name>/app.so, so an
 * ASan run loads the ASan app and never a stale default-variant one. */
ND_UNUSED_FN static bool sa_resolve_app_so(const char *app_name, char *out, size_t out_sz)
{
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(out, out_sz, "%s/../apps/%s/app.so", exe, app_name) == ND_OK;
}

ND_UNUSED_FN static void *sa_sym(void *h, const char *name)
{
    void *p = dlsym(h, name);

    if (p == NULL)
        fprintf(stderr, "smallapp_test: app.so has no symbol %s\n", name);
    return p;
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
    nd_input_channel ch;
    nd_input *in;
    bool ok;
} sa_fixture;

ND_UNUSED_FN static void sa_fx_free(sa_fixture *fx)
{
    if (fx->in != NULL)
        nd_input_close(fx->in);
    nd_input_channel_close(&fx->ch);
    nd_capture_close(fx->cap);
    nd_imgcache_free(fx->ui.image_cache);
    nd_image_free(fx->canvas);
    nd_image_free(fx->scratch);
    nd_font_free(fx->font_s);
    nd_font_free(fx->font_md);
    nd_font_free(fx->font_n);
    nd_font_free(fx->font_xl);
    memset(fx, 0, sizeof *fx);
    fx->ch.read_fd = -1;
    fx->ch.write_fd = -1;
}

ND_UNUSED_FN static bool sa_fx_init(sa_fixture *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->ch.read_fd = -1;
    fx->ch.write_fd = -1;

    fx->font_s = nd_font_load(sa_font, ND_FONT_PX_S);
    fx->font_md = nd_font_load(sa_font, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(sa_font, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(sa_font, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "smallapp_test: nd_font_load(%s) failed\n", sa_font);
        return false;
    }

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    /* 240 * 145 * 3 = 104,400 bytes -- DetailPage's borrowed column. Unused by
     * these four apps, but nd_ui.h says the context carries one. */
    fx->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL || fx->scratch == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;
    if (nd_capture_open(&fx->cap, sa_outdir, 0u) != ND_OK) {
        fprintf(stderr, "smallapp_test: cannot open %s for frames\n", sa_outdir);
        return false;
    }

    if (nd_input_channel_open(&fx->ch) != ND_OK)
        return false;
    /* nd_input_open_fd() takes ownership of the descriptor, so the channel's
     * read end must not be closed a second time by nd_input_channel_close(). */
    if (nd_input_open_fd(&fx->in, fx->ch.read_fd) != ND_OK)
        return false;
    fx->ch.read_fd = -1;
    /* Auto-repeat is new behaviour (OPEN-QUESTIONS.md I-1) and the reference
     * frames were captured without it. A script must never synthesise a key
     * the Python did not deliver; sa_hold() turns it on deliberately. */
    nd_input_set_repeat(fx->in, 0.0, 0.0);

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
    fx->ui.input = fx->in;
    /* The core's own bar already exists by the time any app runs, so an app's
     * SoftKeyBar is the opaque one. */
    fx->ui.softkey_exists = true;
    fx->ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    fx->ok = fx->ui.image_cache != NULL;
    return fx->ok;
}

/* One press and its release, queued. */
ND_UNUSED_FN static bool sa_send(sa_fixture *fx, int32_t code)
{
    return nd_input_channel_send(&fx->ch, code, true) == ND_OK &&
           nd_input_channel_send(&fx->ch, code, false) == ND_OK;
}

ND_UNUSED_FN static bool sa_send_all(sa_fixture *fx, const int32_t *codes, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (!sa_send(fx, codes[i]))
            return false;
    }
    return true;
}

/* A press with no release, in the repeat set. See the header comment. */
ND_UNUSED_FN static bool sa_hold(sa_fixture *fx, int32_t code)
{
    static int32_t held;

    held = code;
    if (nd_input_set_repeat_codes(fx->in, &held, 1u) != ND_OK)
        return false;
    nd_input_set_repeat(fx->in, 0.20, 0.05);
    return nd_input_channel_send(&fx->ch, code, true) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The oracle
 * ------------------------------------------------------------------ */

static nd_json_doc *sa_manifest;
static const nd_json_val *sa_frames;

ND_UNUSED_FN static void sa_manifest_open(void)
{
    char path[ND_PATH_MAX + 32];
    uint8_t *buf = NULL;
    long len;
    FILE *f;
    char err[128];

    (void)snprintf(path, sizeof path, "%.1000s/manifest.json", sa_golden);
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
    if (nd_json_parse(buf, (size_t)len, &sa_manifest, err, sizeof err) != ND_OK) {
        fprintf(stderr, "smallapp_test: manifest parse: %s\n", err);
        sa_manifest = NULL;
        goto done;
    }
    sa_frames = nd_json_get(nd_json_root(sa_manifest), "frames");
done:
    free(buf);
    (void)fclose(f);
}

ND_UNUSED_FN static const char *sa_golden_sha(const char *name)
{
    size_t i;

    if (sa_frames == NULL)
        return NULL;
    for (i = 0u; i < nd_json_len(sa_frames); i++) {
        const nd_json_val *fr = nd_json_at(sa_frames, i);

        if (strcmp(nd_json_get_str(fr, "name", ""), name) == 0)
            return nd_json_get_str(fr, "sha256", NULL);
    }
    return NULL;
}

/* Differing pixels against the stored PNG, with the bounding box, because
 * "three pixels of antialiasing" and "the whole screen" are different bugs
 * and the number is the only thing that says which. -1 when the reference
 * could not be read. */
ND_UNUSED_FN static int32_t sa_diff_pixels(const nd_image *got, const char *name, nd_rect *box)
{
    char path[ND_PATH_MAX + 32];
    nd_image *ref;
    int32_t n = 0;
    int32_t x;
    int32_t y;

    *box = ND_RECT(0, 0, -1, -1);
    (void)snprintf(path, sizeof path, "%.1000s/%.40s.png", sa_golden, name);
    ref = nd_image_load_png(path);
    if (ref == NULL) {
        fprintf(stderr, "smallapp_test: cannot read %s\n", path);
        return -1;
    }
    if (ref->w != got->w || ref->h != got->h) {
        fprintf(stderr, "smallapp_test: %s is %dx%d, rendered %dx%d\n", name, ref->w, ref->h,
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

/* Judge `frame` against the reference by the SAME digest goldenframe.py
 * compares, and report the pixel count when it does not match -- "should be
 * correct" is not a result. */
ND_UNUSED_FN static void sa_expect_golden(sa_fixture *fx, const nd_image *frame, const char *name)
{
    char got[65];
    const char *want;

    if (frame == NULL) {
        CHECK(false, "a frame was committed");
        return;
    }
    want = sa_golden_sha(name);
    if (nd_capture_digest(frame, got, sizeof got) != ND_OK) {
        CHECK(false, "digest");
        return;
    }
    if (want == NULL) {
        fprintf(stderr, "smallapp_test: no reference for %s (got %s)\n", name, got);
        CHECK(false, "reference present");
        return;
    }
    if (strcmp(got, want) == 0) {
        printf("  %s: BYTE-EXACT (0 differing pixels)\n", name);
        sa_checks++;
        return;
    }
    {
        nd_rect box;
        int32_t n = sa_diff_pixels(frame, name, &box);
        int32_t total = frame->w * frame->h;

        if (n < 0) {
            CHECK(false, "cannot measure the delta");
        } else {
            printf("  %s: %d of %d pixels differ (%.2f%%), box (%d,%d)-(%d,%d)\n", name, n, total,
                   100.0 * (double)n / (double)total, box.x0, box.y0, box.x1, box.y1);
            CHECK(false, "frame is byte-exact");
        }
        /* Leave the frame where a human can look at it. */
        (void)nd_capture_save(fx->cap, name, frame);
        (void)nd_capture_write_manifest(fx->cap);
        fprintf(stderr, "smallapp_test: rendered frame written to %s\n", sa_outdir);
    }
}

/* ------------------------------------------------------------------ *
 * A scratch directory for the run
 * ------------------------------------------------------------------ */

ND_UNUSED_FN static int sa_rm_cb(const char *path, const struct stat *st, int flag,
                                 struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

ND_UNUSED_FN static bool sa_tmpdir(const char *stem, char *out, size_t out_sz)
{
    const char *tmp = getenv("TMPDIR");
    char tmpl[ND_PATH_MAX];

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/%s-XXXXXX", tmp, stem) != ND_OK)
        return false;
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "smallapp_test: mkdtemp under %s: %s\n", tmp, strerror(errno));
        return false;
    }
    return nd_strlcpy(out, tmpl, out_sz) < out_sz;
}

ND_UNUSED_FN static void sa_rmtree(const char *dir)
{
    if (dir != NULL && dir[0] != '\0')
        (void)nftw(dir, sa_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* Everything every one of these tests does at start-up, in one call.
 * `app_name` is the apps/<Name> directory; the handle is returned so the test
 * can dlsym() its own symbols out of it. NULL on any failure, with the reason
 * already printed. */
ND_UNUSED_FN static void *sa_begin(const char *app_name, const char *tmp_stem)
{
    char so[ND_PATH_MAX];
    void *h;

    if (!sa_resolve_golden()) {
        fprintf(stderr, "smallapp_test: cannot find the golden set; set NEODCT_GOLDEN\n");
        return NULL;
    }
    if (!sa_resolve_font()) {
        fprintf(stderr, "smallapp_test: cannot find font.ttf; set NEODCT_FONT\n");
        return NULL;
    }
    if (!sa_resolve_app_so(app_name, so, sizeof so))
        return NULL;
    h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "smallapp_test: dlopen %s: %s -- run `make` first\n", so, dlerror());
        return NULL;
    }
    if (!sa_tmpdir(tmp_stem, sa_outdir, sizeof sa_outdir)) {
        (void)dlclose(h);
        return NULL;
    }
    sa_manifest_open();
    return h;
}

ND_UNUSED_FN static int sa_end(void *handle, const char *test_name)
{
    sa_rmtree(sa_outdir);
    if (sa_manifest != NULL)
        nd_json_free(sa_manifest);
    sa_manifest = NULL;
    sa_frames = NULL;
    if (handle != NULL)
        (void)dlclose(handle);
    printf("%s: %d checks, %d failures\n", test_name, sa_checks, sa_failures);
    return (sa_failures == 0) ? 0 : 1;
}

#endif /* SMALLAPP_TEST_H_INCLUDED */
