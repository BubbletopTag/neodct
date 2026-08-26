/* test_nd_capture.c -- the capture backend, the manifest, and the clock.
 *
 * The claim this file has to justify is narrow and important: a directory
 * written by the C build is a directory goldenframe.py will judge against the
 * Python reference without complaint. Three separate things have to be true
 * for that and each is checked on its own, because when it breaks later it
 * will break as one of them:
 *
 *   1. the digest is sha256 over b"<w>,<h>|" plus raw RGB, which is checked
 *      against hashes hashlib produced from the same pixels;
 *   2. manifest.json is what json.dump(indent=2, sort_keys=True) writes,
 *      compared byte for byte;
 *   3. goldenframe.compare() accepts the result -- run for real when this
 *      host has python3 and Pillow, skipped with a note when it does not.
 *
 * Point 3 is the one that could rot silently, so points 1 and 2 do not depend
 * on it: with no Python at all this test still fails if the digest or the
 * schema drifts.
 *
 * Runs with no arguments. Writes under NEODCT_ROOT when set, and into a
 * mkdtemp directory it removes otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_vclock.h"

static int failures;
static int checks;
static int skips;

static void fail(const char *fmt, ...) ND_PRINTF(1, 2);
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    failures++;
}

#define CHECK(cond, ...)       \
    do {                       \
        checks++;              \
        if (!(cond))           \
            fail(__VA_ARGS__); \
    } while (0)

/* Bit-for-bit identical to synth() in tools/gen_fb_ref.py, and to the copy in
 * the Python cross-check below. */
static uint8_t synth(int32_t x, int32_t y, int32_t c)
{
    uint32_t v = ((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 40503u) ^ ((uint32_t)c * 97u);

    v ^= v >> 13;
    v *= 0x5BD1E995u;
    v ^= v >> 15;
    return (uint8_t)(v & 0xFFu);
}

/* owned by the caller; free with nd_image_free() */
static nd_image *make_synth(int32_t w, int32_t h)
{
    nd_image *img = nd_image_new(w, h, ND_PIXFMT_RGB888);
    int32_t x, y, c;

    if (img == NULL)
        return NULL;
    for (y = 0; y < h; y++) {
        uint8_t *row = img->pixels + (size_t)y * img->stride;
        for (x = 0; x < w; x++) {
            for (c = 0; c < 3; c++)
                row[(size_t)x * 3u + (size_t)c] = synth(x, y, c);
        }
    }
    return img;
}

/* ------------------------------------------------------------------ *
 * The clock
 * ------------------------------------------------------------------ */

static void test_virtual_clock(void)
{
    struct tm tm_local;
    struct tm tm_gm;
    double t;

    CHECK(!nd_vclock_enabled(), "the clock did not start on real time");

    nd_vclock_enable();
    CHECK(nd_vclock_enabled(), "enable did not take");
    CHECK(nd_vclock_frame() == 0u, "frame did not start at 0");

    t = nd_time_now();
    CHECK(t == ND_VCLOCK_EPOCH, "frame 0 is %.6f, not the epoch", t);
    /* Frozen within a frame: two reads in the same frame must agree, or a
     * screen composed of several draw calls could tear across a tick. */
    CHECK(nd_time_now() == t, "time moved inside a frame");
    CHECK(nd_time_monotonic() == t, "monotonic and wall disagree under capture");

    nd_vclock_advance();
    CHECK(nd_vclock_frame() == 1u, "advance did not tick");
    CHECK(nd_time_now() == ND_VCLOCK_EPOCH + ND_VCLOCK_TICK, "tick is not 0.1 s");

    nd_vclock_advance();
    nd_vclock_advance();
    CHECK(nd_time_now() == ND_VCLOCK_EPOCH + 3.0 * ND_VCLOCK_TICK, "frame 3 is at the wrong time");

    /* 2024-01-01 12:34:56 UTC, and localtime is aliased onto gmtime so a
     * reference rendered in Dublin matches one rendered in a CI container. */
    nd_time_gmtime(ND_VCLOCK_EPOCH, &tm_gm);
    CHECK(tm_gm.tm_year == 124 && tm_gm.tm_mon == 0 && tm_gm.tm_mday == 1,
          "epoch is not 2024-01-01");
    CHECK(tm_gm.tm_hour == 12 && tm_gm.tm_min == 34 && tm_gm.tm_sec == 56,
          "epoch is not 12:34:56, got %02d:%02d:%02d", tm_gm.tm_hour, tm_gm.tm_min, tm_gm.tm_sec);

    nd_time_localtime(ND_VCLOCK_EPOCH, &tm_local);
    CHECK(tm_local.tm_hour == tm_gm.tm_hour && tm_local.tm_min == tm_gm.tm_min,
          "localtime is not aliased to gmtime under capture");

    {
        const char *tz = getenv("TZ");
        CHECK(tz != NULL && strcmp(tz, "UTC") == 0, "TZ was not pinned to UTC");
    }

    /* Re-enabling restarts from frame 0; nd-shoot leans on that between
     * shots. */
    nd_vclock_enable();
    CHECK(nd_vclock_frame() == 0u, "re-enable did not reset the frame counter");

    nd_vclock_disable();
    CHECK(!nd_vclock_enabled(), "disable did not take");
    CHECK(nd_time_now() > 1700000000.0, "the real clock did not come back");
    /* Advancing a disabled clock is a no-op, not an error: the core loop
     * calls it unconditionally. */
    nd_vclock_advance();
    CHECK(nd_vclock_frame() == 0u, "a disabled clock ticked");
}

/* ------------------------------------------------------------------ *
 * The PRNG
 * ------------------------------------------------------------------ */

static void test_prng(void)
{
    uint32_t first[8];
    uint32_t again[8];
    int32_t counts[6];
    uint8_t deck[32];
    uint8_t seen[32];
    size_t i;

    nd_rand_seed(ND_VCLOCK_SEED);
    for (i = 0u; i < 8u; i++)
        first[i] = nd_rand_u32();
    nd_rand_seed(ND_VCLOCK_SEED);
    for (i = 0u; i < 8u; i++)
        again[i] = nd_rand_u32();
    CHECK(memcmp(first, again, sizeof first) == 0, "the same seed gave a different sequence");

    /* Not stuck, and not obviously patterned: eight consecutive draws being
     * distinct is a weak claim, but a seeding bug usually fails it. */
    {
        bool distinct = true;
        size_t j;
        for (i = 0u; i < 8u; i++)
            for (j = i + 1u; j < 8u; j++)
                if (first[i] == first[j])
                    distinct = false;
        CHECK(distinct, "eight draws were not distinct");
    }

    memset(counts, 0, sizeof counts);
    for (i = 0u; i < 6000u; i++) {
        int32_t v = nd_rand_below(6);
        CHECK(v >= 0 && v < 6, "rand_below(6) returned %d", v);
        if (v >= 0 && v < 6)
            counts[v]++;
    }
    for (i = 0u; i < 6u; i++)
        CHECK(counts[i] > 700 && counts[i] < 1300, "bucket %zu got %d of 6000", i, counts[i]);

    CHECK(nd_rand_below(0) == 0, "rand_below(0) is not 0");
    CHECK(nd_rand_below(-4) == 0, "a negative bound is not 0");
    CHECK(nd_rand_below(1) == 0, "rand_below(1) is not 0");
    CHECK(nd_rand_range(7, 7) == 7, "randint(7,7) is not 7");
    CHECK(nd_rand_range(9, 3) == 9, "an inverted range did not return lo");
    for (i = 0u; i < 500u; i++) {
        int32_t v = nd_rand_range(-3, 4);
        CHECK(v >= -3 && v <= 4, "randint(-3,4) returned %d", v);
    }
    for (i = 0u; i < 500u; i++) {
        double d = nd_rand_double();
        CHECK(d >= 0.0 && d < 1.0, "random() returned %f", d);
    }

    /* A shuffle is a permutation: every card exactly once. */
    for (i = 0u; i < sizeof deck; i++)
        deck[i] = (uint8_t)i;
    nd_rand_shuffle(deck, sizeof deck, 1u);
    memset(seen, 0, sizeof seen);
    for (i = 0u; i < sizeof deck; i++) {
        CHECK(deck[i] < sizeof deck, "shuffle produced %u", deck[i]);
        if (deck[i] < sizeof deck)
            seen[deck[i]]++;
    }
    for (i = 0u; i < sizeof seen; i++)
        CHECK(seen[i] == 1u, "card %zu appears %u times after a shuffle", i, seen[i]);

    /* Degenerate inputs are no-ops, not crashes. */
    nd_rand_shuffle(NULL, 4u, 1u);
    nd_rand_shuffle(deck, 0u, 1u);
    nd_rand_shuffle(deck, 4u, 0u);
}

/* ------------------------------------------------------------------ *
 * The digest, against hashlib
 * ------------------------------------------------------------------ */

static void test_digest(void)
{
    /* Produced by hashlib over exactly the bytes goldenframe.frame_digest()
     * feeds it: b"<w>,<h>|" then the tightly packed RGB rows. */
    static const char SOLID_240x175[] =
        "69c15aa1b07c30c1bb32d282e280580e7821e1f9eb28d14999c31bc4dc0fe7b2";
    static const char SOLID_7x5[] =
        "c355364caf036aa4dcbf341914ea51530ae83020fe55d3036cf927e041ac6f23";
    static const char SYNTH_13x7[] =
        "7146cc86153683eef56f40223bc4bc1c0bf5b524793c017e51ff3101b0548ebe";

    nd_image *img;
    char hex[65];
    char small[64];

    img = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(32, 64, 128));
    CHECK(img != NULL, "allocation failed");
    if (img != NULL) {
        CHECK(nd_capture_digest(img, hex, sizeof hex) == ND_OK, "digest failed");
        CHECK(strcmp(hex, SOLID_240x175) == 0, "240x175 digest %s != %s", hex, SOLID_240x175);
        /* 64 hex digits and a terminator; anything less truncates silently. */
        CHECK(nd_capture_digest(img, small, sizeof small) == ND_ERR_TOOLONG,
              "a 64-byte buffer was accepted");
        nd_image_free(img);
    }

    img = nd_image_new_filled(7, 5, ND_PIXFMT_RGB888, ND_RGB(1, 2, 3));
    CHECK(img != NULL, "allocation failed");
    if (img != NULL) {
        CHECK(nd_capture_digest(img, hex, sizeof hex) == ND_OK, "digest failed");
        CHECK(strcmp(hex, SOLID_7x5) == 0, "7x5 digest %s != %s", hex, SOLID_7x5);
        nd_image_free(img);
    }

    /* A patterned image: a solid colour cannot catch a channel swap. */
    img = make_synth(13, 7);
    CHECK(img != NULL, "allocation failed");
    if (img != NULL) {
        CHECK(nd_capture_digest(img, hex, sizeof hex) == ND_OK, "digest failed");
        CHECK(strcmp(hex, SYNTH_13x7) == 0, "13x7 synth digest %s != %s", hex, SYNTH_13x7);
        nd_image_free(img);
    }

    /* An RGBA frame is converted before hashing, exactly as frame_digest()
     * does, so it must agree with the RGB image of the same colour. */
    img = nd_image_new_filled(7, 5, ND_PIXFMT_RGBA8888, ND_RGBA(1, 2, 3, 200));
    CHECK(img != NULL, "allocation failed");
    if (img != NULL) {
        CHECK(nd_capture_digest(img, hex, sizeof hex) == ND_OK, "digest failed");
        CHECK(strcmp(hex, SOLID_7x5) == 0, "RGBA digest %s != %s", hex, SOLID_7x5);
        nd_image_free(img);
    }

    CHECK(nd_capture_digest(NULL, hex, sizeof hex) == ND_ERR_INVAL, "NULL image accepted");
}

/* ------------------------------------------------------------------ *
 * The capture framebuffer
 * ------------------------------------------------------------------ */

static void test_capture_fb(const char *dir)
{
    nd_capture *cap = NULL;
    nd_fb *fb;
    nd_image *a = NULL;
    nd_image *b = NULL;
    nd_image *panel = NULL;
    const nd_image *rec;
    nd_err rc;
    size_t i;

    rc = nd_capture_open(&cap, dir, 3u);
    CHECK(rc == ND_OK, "nd_capture_open -> %s", nd_strerror(rc));
    if (rc != ND_OK)
        return;

    fb = nd_capture_fb(cap);
    CHECK(fb != NULL, "no capture framebuffer");
    /* uistub reports the panel, not the band. */
    CHECK(nd_fb_xres(fb) == 240 && nd_fb_yres(fb) == 240, "capture fb is not 240x240");
    CHECK(nd_fb_bpp(fb) == 32, "capture fb is not 32bpp");
    CHECK(nd_fb_mem_bytes(fb, NULL) == NULL, "a sink framebuffer reported a mapping");

    a = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(10, 20, 30));
    b = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(40, 50, 60));
    CHECK(a != NULL && b != NULL, "allocation failed");
    if (a == NULL || b == NULL)
        goto out;

    nd_vclock_enable();
    CHECK(nd_fb_update(fb, a) == ND_OK, "capture update failed");
    CHECK(nd_capture_frames_drawn(cap) == 1u, "frame not counted");
    CHECK(nd_vclock_frame() == 1u, "the commit did not tick the clock");

    rec = nd_capture_recent(cap, 0u);
    CHECK(rec != NULL, "no recent frame");
    CHECK(rec != a, "the capture kept a reference instead of a copy");
    if (rec != NULL) {
        nd_color c = nd_image_get_px(rec, 5, 5);
        CHECK(c.r == 10u && c.g == 20u && c.b == 30u, "recorded pixel is wrong");
        CHECK(rec->fmt == ND_PIXFMT_RGB888, "recorded frame is not RGB");
    }

    /* The UI redraws onto one long-lived canvas, so a capture that stored a
     * reference would leave every frame showing the last screen. */
    (void)nd_image_fill(a, ND_RGB(200, 200, 200));
    rec = nd_capture_recent(cap, 0u);
    if (rec != NULL) {
        nd_color c = nd_image_get_px(rec, 5, 5);
        CHECK(c.r == 10u, "editing the canvas changed a recorded frame");
    }

    CHECK(nd_fb_update(fb, b) == ND_OK, "second update failed");
    CHECK(nd_capture_frames_drawn(cap) == 2u, "second frame not counted");
    rec = nd_capture_recent(cap, 1u);
    CHECK(rec != NULL, "no frame one back");
    if (rec != NULL) {
        nd_color c = nd_image_get_px(rec, 5, 5);
        CHECK(c.r == 10u, "one back is not the first frame");
    }
    CHECK(nd_capture_recent(cap, 2u) == NULL, "two back exists after two frames");

    /* The ring is three deep: after five commits the oldest two are gone and
     * asking for them says so rather than handing back a stale frame. */
    for (i = 0u; i < 3u; i++)
        CHECK(nd_fb_update(fb, b) == ND_OK, "ring update failed");
    CHECK(nd_capture_frames_drawn(cap) == 5u, "frame count is wrong");
    CHECK(nd_capture_recent(cap, 2u) != NULL, "the ring lost a frame it should hold");
    CHECK(nd_capture_recent(cap, 3u) == NULL, "the ring held more than three frames");

    /* device_frame(): the band centred in a 240x240 panel, top at row 32. */
    panel = nd_capture_device_frame(cap, 0u, ND_CAPTURE_PANEL_W, ND_CAPTURE_PANEL_H);
    CHECK(panel != NULL, "device_frame failed");
    if (panel != NULL) {
        nd_color above = nd_image_get_px(panel, 120, 31);
        nd_color first = nd_image_get_px(panel, 120, 32);
        nd_color last = nd_image_get_px(panel, 120, 206);
        nd_color below = nd_image_get_px(panel, 120, 207);

        CHECK(panel->w == 240 && panel->h == 240, "panel is not 240x240");
        CHECK(above.r == 0u && above.g == 0u && above.b == 0u, "row 31 is not black");
        CHECK(first.r == 40u && first.g == 50u && first.b == 60u, "the band does not start at 32");
        CHECK(last.r == 40u, "the band does not end at 206");
        CHECK(below.r == 0u && below.g == 0u && below.b == 0u, "row 207 is not black");
    }

    /* The frame budget. Koki and the games never read the keypad, so this is
     * the only way to stop them; a refused frame must not tick the clock. */
    nd_capture_set_budget(cap, 2);
    CHECK(!nd_capture_exhausted(cap), "exhausted before drawing");
    CHECK(nd_fb_update(fb, b) == ND_OK, "budgeted frame 1 refused");
    CHECK(nd_fb_update(fb, b) == ND_OK, "budgeted frame 2 refused");
    {
        uint64_t before = nd_vclock_frame();
        uint64_t drawn = nd_capture_frames_drawn(cap);

        CHECK(nd_fb_update(fb, b) == ND_ERR_BUSY, "the budget did not stop the third frame");
        CHECK(nd_capture_exhausted(cap), "exhaustion not reported");
        CHECK(nd_vclock_frame() == before, "a refused frame advanced the clock");
        CHECK(nd_capture_frames_drawn(cap) == drawn, "a refused frame was counted");
    }
    nd_capture_clear_budget(cap);
    CHECK(nd_fb_update(fb, b) == ND_OK, "clearing the budget did not resume");
    CHECK(!nd_capture_exhausted(cap), "still exhausted after clearing");

    nd_vclock_disable();

out:
    nd_image_free(panel);
    nd_image_free(a);
    nd_image_free(b);
    nd_capture_close(cap);
}

/* ------------------------------------------------------------------ *
 * Writing a reference directory
 * ------------------------------------------------------------------ */

/* json.dump(..., indent=2, sort_keys=True) with no trailing newline. Built
 * here rather than compared field by field, because the thing that will break
 * is the shape, and the shape is only visible whole. */
static void expected_manifest(char *out, size_t out_sz, const char *sha_solid,
                              const char *sha_synth)
{
    int n = snprintf(out, out_sz,
                     "{\n"
                     "  \"epoch\": 1704112496.0,\n"
                     "  \"frames\": [\n"
                     "    {\n"
                     "      \"name\": \"solid\",\n"
                     "      \"sha256\": \"%s\",\n"
                     "      \"size\": [\n"
                     "        240,\n"
                     "        175\n"
                     "      ]\n"
                     "    },\n"
                     "    {\n"
                     "      \"name\": \"synth\",\n"
                     "      \"sha256\": \"%s\",\n"
                     "      \"size\": [\n"
                     "        13,\n"
                     "        7\n"
                     "      ]\n"
                     "    }\n"
                     "  ],\n"
                     "  \"seed\": 20240101,\n"
                     "  \"text_layout\": \"BASIC\",\n"
                     "  \"tick\": 0.1\n"
                     "}",
                     sha_solid, sha_synth);

    if (n < 0 || (size_t)n >= out_sz)
        out[0] = '\0';
}

/* owned by the caller; free() it */
static char *slurp(const char *path, size_t *len_out)
{
    FILE *fh = fopen(path, "rb");
    char *buf = NULL;
    long size;

    if (fh == NULL)
        return NULL;
    if (fseek(fh, 0, SEEK_END) != 0)
        goto done;
    size = ftell(fh);
    if (size < 0 || fseek(fh, 0, SEEK_SET) != 0)
        goto done;
    buf = malloc((size_t)size + 1u);
    if (buf == NULL)
        goto done;
    if (fread(buf, 1u, (size_t)size, fh) != (size_t)size) {
        free(buf);
        buf = NULL;
        goto done;
    }
    buf[size] = '\0';
    if (len_out != NULL)
        *len_out = (size_t)size;
done:
    (void)fclose(fh);
    return buf;
}

static void test_write_reference_dir(const char *dir)
{
    nd_capture *cap = NULL;
    nd_image *solid = NULL;
    nd_image *pattern = NULL;
    nd_image *reloaded = NULL;
    char path[512];
    char sha_solid[65];
    char sha_synth[65];
    char want[2048];
    char *got = NULL;
    nd_err rc;

    rc = nd_capture_open(&cap, dir, 0u);
    CHECK(rc == ND_OK, "nd_capture_open -> %s", nd_strerror(rc));
    if (rc != ND_OK)
        return;

    solid = nd_image_new_filled(240, 175, ND_PIXFMT_RGB888, ND_RGB(32, 64, 128));
    pattern = make_synth(13, 7);
    CHECK(solid != NULL && pattern != NULL, "allocation failed");
    if (solid == NULL || pattern == NULL)
        goto out;

    /* Saved out of alphabetical order on purpose: the manifest has to sort. */
    CHECK(nd_capture_save(cap, "synth", pattern) == ND_OK, "save synth failed");
    CHECK(nd_capture_save(cap, "solid", solid) == ND_OK, "save solid failed");
    CHECK(nd_capture_count(cap) == 2u, "wrong entry count");

    CHECK(nd_capture_save(cap, "solid", solid) == ND_ERR_INVAL, "duplicate name accepted");
    CHECK(nd_capture_save(cap, "a/b", solid) == ND_ERR_INVAL, "a name with a slash accepted");
    CHECK(nd_capture_save(cap, "", solid) == ND_ERR_INVAL, "an empty name accepted");
    CHECK(nd_capture_count(cap) == 2u, "a refused save was recorded");

    CHECK(nd_capture_write_manifest(cap) == ND_OK, "manifest write failed");

    /* The PNG is only there so a human can see a diff, but it still has to be
     * the pixels we hashed -- goldenframe reopens it when a frame fails. */
    CHECK(snprintf(path, sizeof path, "%s/solid.png", dir) > 0, "path too long");
    /* nd_image_open() resolves for itself, so this one stays logical. */
    reloaded = nd_image_open(path);
    CHECK(reloaded != NULL, "the saved PNG did not reload");
    if (reloaded != NULL) {
        char sha_reloaded[65];

        CHECK(nd_capture_digest(solid, sha_solid, sizeof sha_solid) == ND_OK, "digest failed");
        CHECK(nd_capture_digest(reloaded, sha_reloaded, sizeof sha_reloaded) == ND_OK,
              "digest failed");
        CHECK(strcmp(sha_solid, sha_reloaded) == 0, "the PNG round trip changed the pixels");
    }

    CHECK(nd_capture_digest(solid, sha_solid, sizeof sha_solid) == ND_OK, "digest failed");
    CHECK(nd_capture_digest(pattern, sha_synth, sizeof sha_synth) == ND_OK, "digest failed");
    expected_manifest(want, sizeof want, sha_solid, sha_synth);

    CHECK(snprintf(path, sizeof path, "%s/manifest.json", dir) > 0, "path too long");
    {
        /* Read with plain stdio, so resolve by hand. */
        char real[ND_PATH_MAX];
        if (nd_path_resolve(real, sizeof real, path) == ND_OK)
            got = slurp(real, NULL);
    }
    CHECK(got != NULL, "manifest.json missing");
    if (got != NULL && want[0] != '\0')
        CHECK(strcmp(got, want) == 0,
              "manifest.json is not json.dump's shape:\n--- got ---\n%s\n"
              "--- want ---\n%s",
              got, want);

out:
    free(got);
    nd_image_free(reloaded);
    nd_image_free(pattern);
    nd_image_free(solid);
    nd_capture_close(cap);
}

/* ------------------------------------------------------------------ *
 * The umpire itself
 * ------------------------------------------------------------------ */

/* Build the same two frames with Pillow, write them with goldenframe's own
 * write_manifest(), and let goldenframe.compare() judge the pair. That is the
 * whole round trip: our digest against frame_digest(), our PNG against
 * Pillow's pixels, our schema against the parser that will read it.
 *
 * Skipped, loudly, when the host has no Pillow. The digest and manifest
 * checks above do not depend on it. */
static void test_goldenframe_accepts(const char *cdir, const char *scratch)
{
    const char *golden = getenv("NEODCT_GOLDEN");
    char real_cdir[ND_PATH_MAX];
    char real_scratch[ND_PATH_MAX];
    char script[600];
    char refdir[600];
    char cmd[2048];
    FILE *fh;
    int have_pil;
    int rc;

    if (golden == NULL) {
        printf("SKIP: NEODCT_GOLDEN unset, cannot locate goldenframe.py\n");
        skips++;
        return;
    }

    have_pil = system("python3 -c 'import PIL' >/dev/null 2>&1");
    if (have_pil != 0) {
        printf("SKIP: no python3 with Pillow on this host; "
               "goldenframe.py --compare not exercised\n");
        skips++;
        return;
    }

    /* Python knows nothing about ND_ROOT: hand it real paths. */
    if (nd_path_resolve(real_cdir, sizeof real_cdir, cdir) != ND_OK ||
        nd_path_resolve(real_scratch, sizeof real_scratch, scratch) != ND_OK) {
        fail("scratch path too long");
        return;
    }
    if (snprintf(script, sizeof script, "%s/mkref.py", real_scratch) < 0 ||
        snprintf(refdir, sizeof refdir, "%s/pyref", real_scratch) < 0) {
        fail("scratch path too long");
        return;
    }

    fh = fopen(script, "w");
    CHECK(fh != NULL, "cannot write %s", script);
    if (fh == NULL)
        return;
    fputs("import os, sys\n"
          "golden, refdir, cdir = sys.argv[1], sys.argv[2], sys.argv[3]\n"
          "sys.path.insert(0, os.path.join(golden, '..', '..', 'tools'))\n"
          "from PIL import Image\n"
          "import goldenframe as gf\n"
          "M32 = 0xFFFFFFFF\n"
          "def synth(x, y, c):\n"
          "    v = ((x * 2654435761) ^ (y * 40503) ^ (c * 97)) & M32\n"
          "    v ^= v >> 13\n"
          "    v = (v * 0x5BD1E995) & M32\n"
          "    v ^= v >> 15\n"
          "    return v & 0xFF\n"
          "os.makedirs(refdir, exist_ok=True)\n"
          "entries = []\n"
          "solid = Image.new('RGB', (240, 175), (32, 64, 128))\n"
          "data = bytes(synth(x, y, c) for y in range(7) for x in range(13) for c in range(3))\n"
          "pattern = Image.frombytes('RGB', (13, 7), data)\n"
          "for name, im in (('solid', solid), ('synth', pattern)):\n"
          "    im.save(os.path.join(refdir, name + '.png'))\n"
          "    entries.append({'name': name, 'size': [im.width, im.height],\n"
          "                    'sha256': gf.frame_digest(im)})\n"
          "gf.write_manifest(refdir, sorted(entries, key=lambda e: e['name']))\n"
          "ok, diffs = gf.compare(refdir, cdir)\n"
          "sys.exit(0 if ok else 1)\n",
          fh);
    rc = fclose(fh) == 0 ? 0 : -1;
    CHECK(rc == 0, "cannot close %s", script);

    if (snprintf(cmd, sizeof cmd, "python3 '%s' '%s' '%s' '%s'", script, golden, refdir,
                 real_cdir) < 0) {
        fail("command too long");
        return;
    }
    rc = system(cmd);
    CHECK(rc == 0, "goldenframe.py --compare rejected the C capture (exit %d)", rc);
}

/* ------------------------------------------------------------------ *
 * Scratch space
 * ------------------------------------------------------------------ */

static char g_scratch[512];
static bool g_scratch_is_ours;

/* Capture paths are ND_ROOT-resolved like every other path libneodct opens,
 * so the scratch directory is a LOGICAL path and the test resolves it itself
 * whenever it wants to read what was written. Under `make test` that puts the
 * output inside the harness's temporary root, which it removes for us. */
static bool make_scratch(void)
{
    const char *root = getenv("NEODCT_ROOT");
    char real[ND_PATH_MAX];

    if (root != NULL && root[0] != '\0') {
        if (snprintf(g_scratch, sizeof g_scratch, "/capture-test") < 0)
            return false;
        if (nd_path_resolve(real, sizeof real, g_scratch) != ND_OK)
            return false;
        if (mkdir(real, 0755) != 0) {
            perror("mkdir");
            return false;
        }
        return true;
    }

    /* No root: the logical path is the real one, so it has to be somewhere
     * this process may write. */
    if (snprintf(g_scratch, sizeof g_scratch, "/tmp/nd-capture-XXXXXX") < 0)
        return false;
    if (mkdtemp(g_scratch) == NULL) {
        perror("mkdtemp");
        return false;
    }
    g_scratch_is_ours = true;
    return true;
}

static void drop_scratch(void)
{
    char cmd[600];

    if (!g_scratch_is_ours || g_scratch[0] == '\0')
        return;
    /* Only reached when NEODCT_ROOT was unset, i.e. someone ran the binary by
     * hand; under `make test` the harness owns the cleanup. */
    if (snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_scratch) > 0) {
        int rc = system(cmd);
        if (rc != 0)
            printf("note: could not remove %s\n", g_scratch);
    }
}

int main(void)
{
    char cdir[512];

    test_virtual_clock();
    test_prng();
    test_digest();

    if (!make_scratch()) {
        printf("test_nd_capture: cannot create scratch space\n");
        return 1;
    }

    if (snprintf(cdir, sizeof cdir, "%s/frames", g_scratch) < 0) {
        drop_scratch();
        return 1;
    }

    /* nd_capture_open() creates the directory, including a missing parent --
     * nd-shoot --out points at somewhere that does not exist yet. */
    {
        char nested[600];
        if (snprintf(nested, sizeof nested, "%s/deep/er", g_scratch) > 0) {
            nd_capture *cap = NULL;
            CHECK(nd_capture_open(&cap, nested, 0u) == ND_OK, "mkdir -p failed");
            nd_capture_close(cap);
        }
    }

    test_capture_fb(cdir);
    test_write_reference_dir(cdir);
    test_goldenframe_accepts(cdir, g_scratch);

    drop_scratch();

    printf("test_nd_capture: %d checks, %d failures, %d skipped\n", checks, failures, skips);
    return failures == 0 ? 0 : 1;
}
