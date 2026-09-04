/* test_bootbar.c -- the boot install screen, held against the widget it is
 * a copy of and against the promise that it can never fail an install.
 *
 * Three things are checked here and each of them is load-bearing.
 *
 * 1. THE FIVE BOXES. nd_bootbar hard-codes the layout lib/nd_progress.c
 *    derives, because there is no nd_ui in an initramfs to derive it from.
 *    So this file builds a real 240x175 nd_ui, calls nd_progress_init(), and
 *    asserts the constants equal what came out. Change the panel size or
 *    bar_top = trunc(content_bottom * 0.55) and this fails and names the boot
 *    screen, instead of the two screens quietly drifting apart. There is
 *    precedent: test_initramfs_apply.py pins verity_table() against
 *    verity.py for exactly this reason.
 *
 * 2. THE PERCENTAGE. nd_bootbar_percent() and nd_progress_draw() have to
 *    round identically, including total == 0 meaning done, or the same
 *    install reads differently depending on which screen is showing it.
 *
 * 3. THE FILTER CANNOT BREAK THE INSTALL. With a framebuffer that cannot be
 *    opened, stdin still reaches stdout byte for byte. That is the property
 *    that stops a cosmetic feature from stopping an operating system from
 *    being installed, and it is the reason the tool is a filter rather than
 *    a poller.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "nd_bootbar.h"
#include "nd_bootfb.h"

static int g_fail;
static int g_checks;

#define CHECK(cond, what)                                                          \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            (void)fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
            g_fail++;                                                              \
        }                                                                          \
    } while (0)

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

static char g_golden[ND_PATH_MAX];

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

/* The same search test_widgets_dialogs.c does, for the same reason: the test
 * binary runs from build/<variant>/test/ and font.ttf is never under
 * NEODCT_ROOT. */
static bool resolve_golden(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_GOLDEN");

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
        return true;
    }
    if (file_exists("../../../../tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "../../../../tests/golden", sz);
        return true;
    }
    if (file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "neodct/tests/golden", sz);
        return true;
    }
    return false;
}

static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    char base[ND_PATH_MAX];
    char cand[ND_PATH_MAX];
    char *cut;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
        return true;
    }
    if (resolve_golden(g_golden, sizeof g_golden)) {
        (void)snprintf(base, sizeof base, "%.480s", g_golden);
        cut = strrchr(base, '/'); /* .../neodct/tests */
        if (cut != NULL)
            *cut = '\0';
        cut = strrchr(base, '/'); /* .../neodct       */
        if (cut != NULL)
            *cut = '\0';
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

/* ------------------------------------------------------------------ *
 * 1. The layout, against the widget that owns it
 * ------------------------------------------------------------------ */

static void check_layout(void)
{
    char path[ND_PATH_MAX];
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;
    nd_progress p;

    if (!resolve_font(path, sizeof path)) {
        (void)fprintf(stderr, "test_bootbar: cannot find font.ttf; set NEODCT_FONT\n");
        g_fail++;
        return;
    }

    memset(&ui, 0, sizeof ui);
    canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (canvas == NULL || nd_draw_bind(&draw, canvas) != ND_OK) {
        (void)fprintf(stderr, "test_bootbar: no canvas\n");
        g_fail++;
        nd_image_free(canvas);
        return;
    }
    ui.w = ND_UI_W;
    ui.h = ND_UI_H;
    ui.softkey_h = ND_SOFTKEY_H;
    ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    ui.canvas = canvas;
    ui.draw = &draw;
    ui.font_s = nd_font_load(path, ND_FONT_PX_S);
    ui.font_md = nd_font_load(path, ND_FONT_PX_MD);
    ui.font_n = nd_font_load(path, ND_FONT_PX_N);
    ui.font_xl = nd_font_load(path, ND_FONT_PX_XL);
    ui.keypad_fd = -1;

    if (ui.font_s == NULL || ui.font_n == NULL) {
        (void)fprintf(stderr, "test_bootbar: nd_font_load(%s) failed\n", path);
        g_fail++;
        goto done;
    }

    nd_progress_init(&p, &ui, "Installing", "SOFTWARE UPDATE", "Do not power off", NULL, NULL);

    CHECK(p.header_box.x0 == nd_bootbar_header_box.x0 &&
              p.header_box.y0 == nd_bootbar_header_box.y0 &&
              p.header_box.x1 == nd_bootbar_header_box.x1 &&
              p.header_box.y1 == nd_bootbar_header_box.y1,
          "nd_bootbar_header_box is nd_progress's header_box");
    CHECK(p.divider_y == nd_bootbar_divider_y, "nd_bootbar_divider_y is nd_progress's divider_y");
    CHECK(p.label_box.x0 == nd_bootbar_label_box.x0 && p.label_box.y0 == nd_bootbar_label_box.y0 &&
              p.label_box.x1 == nd_bootbar_label_box.x1 &&
              p.label_box.y1 == nd_bootbar_label_box.y1,
          "nd_bootbar_label_box is nd_progress's label_box");
    CHECK(p.bar_box.x0 == nd_bootbar_bar_box.x0 && p.bar_box.y0 == nd_bootbar_bar_box.y0 &&
              p.bar_box.x1 == nd_bootbar_bar_box.x1 && p.bar_box.y1 == nd_bootbar_bar_box.y1,
          "nd_bootbar_bar_box is nd_progress's bar_box");
    CHECK(p.status_box.x0 == nd_bootbar_status_box.x0 &&
              p.status_box.y0 == nd_bootbar_status_box.y0 &&
              p.status_box.x1 == nd_bootbar_status_box.x1 &&
              p.status_box.y1 == nd_bootbar_status_box.y1,
          "nd_bootbar_status_box is nd_progress's status_box");
    CHECK(p.hint_box.x0 == nd_bootbar_hint_box.x0 && p.hint_box.y0 == nd_bootbar_hint_box.y0 &&
              p.hint_box.x1 == nd_bootbar_hint_box.x1 && p.hint_box.y1 == nd_bootbar_hint_box.y1,
          "nd_bootbar_hint_box is nd_progress's hint_box");
    CHECK(ND_BOOTBAR_INSET == ND_PROGRESS_INSET, "the bar fill is inset by the widget's inset");
    CHECK(ND_BOOTFB_W == ND_UI_W && ND_BOOTFB_H == ND_UI_H,
          "the boot shadow is the panel the widget is laid out for");

    /* The metrics too, not only the boxes: nd_bootfb's tables were rendered
     * from this very font, so the two measurements of a string have to agree
     * or a centred label lands on a different column on the boot screen. */
    {
        static const char *const strings[] = {
            "SOFTWARE UPDATE", "Installing", "Checking the update", "Do not power off", "100%",
            "51.0 of 51.0 MB", "2/3"};
        size_t i;

        for (i = 0u; i < ND_ARRAY_LEN(strings); i++) {
            int32_t want = 0;

            nd_text_size(ui.font_s, strings[i], &want, NULL);
            CHECK(nd_bootfb_text_w(strings[i], ND_BOOTFB_SMALL) == want,
                  "the boot font measures a string as nd_text_size does at 14 px");
            nd_text_size(ui.font_md, strings[i], &want, NULL);
            CHECK(nd_bootfb_text_w(strings[i], ND_BOOTFB_MID) == want,
                  "the boot font measures a string as nd_text_size does at 18 px");
            nd_text_size(ui.font_n, strings[i], &want, NULL);
            CHECK(nd_bootfb_text_w(strings[i], ND_BOOTFB_STEP) == want,
                  "the boot font measures a string as nd_text_size does at 20 px");
        }
    }

    /* And the ladder, so a long step name drops to the same size on both. */
    {
        const nd_font *ladder[3];
        size_t n = nd_font_ladder(&ui, ladder, ND_ARRAY_LEN(ladder));
        const nd_font *fit = nd_fit_font("Checking the update", ND_UI_W - 16, ladder, n);
        nd_bootfb_size mine = nd_bootfb_fit("Checking the update", ND_UI_W - 16);

        CHECK(n == 3u, "the ladder the widget offers is three rungs");
        CHECK(nd_font_px(fit) == (int32_t)mine,
              "the boot screen picks the size nd_fit_font picks for the same label");
    }

done:
    nd_font_free(ui.font_s);
    nd_font_free(ui.font_md);
    nd_font_free(ui.font_n);
    nd_font_free(ui.font_xl);
    nd_image_free(canvas);
}

/* ------------------------------------------------------------------ *
 * 2. The percentage
 * ------------------------------------------------------------------ */

static void check_percent(void)
{
    /* done, total. The last row is the one that matters most: nd_progress
     * treats total == 0 as "done", not as a division by zero. */
    static const int64_t cases[][2] = {
        {0, 100},
        {1, 100},
        {50, 100},
        {99, 100},
        {100, 100},
        {101, 100},
        {0, 0},
        {5, 0},
        {1, 3},
        {2, 3},
        {51380224, 53477376},
        {53477376, 53477376},
        {1, 53477376},
        {26738688, 53477376},
    };
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(cases); i++) {
        int64_t done = cases[i][0];
        int64_t total = cases[i][1];
        /* nd_progress_draw()'s expression, quoted from lib/nd_progress.c. */
        int32_t want = (total != 0) ? nd_trunc32((double)done * 100.0 / (double)total) : 100;

        want = nd_clamp32(want, 0, 100);
        CHECK(nd_bootbar_percent(done, total) == want,
              "nd_bootbar_percent rounds as nd_progress_draw rounds");
    }
}

/* ------------------------------------------------------------------ *
 * 3. The filter
 * ------------------------------------------------------------------ */

/* Run the copy through a real pair of pipes, in the shape the applier uses
 * it: something upstream writes, nd_bootbar_filter copies, we read it back.
 *
 * Two forks that do NOT execve, which CODING-STANDARDS 1.1 forbids -- in the
 * core, whose whole point is that it runs threads and a forked child inherits
 * their held mutexes. This process has one thread and no such hazard, and a
 * pipeline is the only shape in which this function is ever used, so faking
 * one would test something else. */
static bool copy_through(nd_bootfb *fb, const uint8_t *in, size_t n, uint8_t *out, size_t out_sz,
                         size_t *got)
{
    int to_filter[2];
    int from_filter[2];
    pid_t writer;
    pid_t copier;
    size_t have = 0;
    int status = 0;
    bool ok = true;

    if (pipe(to_filter) != 0)
        return false;
    if (pipe(from_filter) != 0) {
        (void)close(to_filter[0]);
        (void)close(to_filter[1]);
        return false;
    }

    writer = fork();
    if (writer == 0) {
        size_t left = n;
        const uint8_t *p = in;

        (void)close(to_filter[0]);
        (void)close(from_filter[0]);
        (void)close(from_filter[1]);
        while (left > 0u) {
            ssize_t w = write(to_filter[1], p, left > 4093u ? 4093u : left);

            if (w <= 0)
                break;
            p += (size_t)w;
            left -= (size_t)w;
        }
        (void)close(to_filter[1]);
        _exit(0);
    }

    copier = fork();
    if (copier == 0) {
        bool copied;

        (void)close(to_filter[1]);
        (void)close(from_filter[0]);
        copied = nd_bootbar_filter(fb, to_filter[0], from_filter[1], "Installing", 2, (int64_t)n);
        (void)close(to_filter[0]);
        (void)close(from_filter[1]);
        _exit(copied ? 0 : 1);
    }

    (void)close(to_filter[0]);
    (void)close(to_filter[1]);
    (void)close(from_filter[1]);

    for (;;) {
        ssize_t r = read(from_filter[0], out + have, out_sz - have);

        if (r <= 0)
            break;
        have += (size_t)r;
        if (have == out_sz)
            break;
    }
    (void)close(from_filter[0]);
    (void)waitpid(writer, NULL, 0);
    (void)waitpid(copier, &status, 0);
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    *got = have;
    return ok;
}

static void check_filter(void)
{
    /* Deliberately not a multiple of the 64 KiB read chunk, and big enough
     * to cross it several times: a copy that only ever saw whole chunks
     * would not notice a short final read. */
    static uint8_t src[64u * 1024u * 3u + 1237u];
    static uint8_t dst[sizeof src + 16u];
    static nd_bootfb fb;
    size_t got = 0;
    size_t i;

    for (i = 0u; i < sizeof src; i++)
        src[i] = (uint8_t)((i * 31u + (i >> 8)) & 0xFFu);

    /* A framebuffer that cannot be opened. This is THE test: the install
     * must not care. */
    memset(&fb, 0, sizeof fb);
    CHECK(!nd_bootfb_open(&fb, "/nonexistent/neodct/fb0"),
          "opening a framebuffer that is not there fails rather than crashing");
    CHECK(!fb.usable, "a framebuffer that did not open is not usable");

    /* Every drawing call on it is a no-op, not a fault. */
    nd_bootfb_clear(&fb);
    nd_bootfb_fill(&fb, ND_BRECT(0, 0, 10, 10), true);
    nd_bootfb_outline(&fb, ND_BRECT(0, 0, 10, 10));
    nd_bootfb_text(&fb, 0, 0, "nothing happens", ND_BOOTFB_SMALL);
    nd_bootbar_frame(&fb, "Installing", 2, 1, 2);
    nd_bootbar_frame_at(&fb, "Installing", 2, 100, 2);
    nd_bootbar_fail(&fb, "Update refused", "Not signed by NeoDCT");
    nd_bootfb_present(&fb);
    CHECK(true, "drawing on an unopened framebuffer is a no-op, not a fault");

    CHECK(copy_through(&fb, src, sizeof src, dst, sizeof dst, &got),
          "the filter reports success with no framebuffer");
    CHECK(got == sizeof src, "every byte of stdin reached stdout");
    CHECK(got == sizeof src && memcmp(src, dst, sizeof src) == 0,
          "the bytes that came out are the bytes that went in");

    /* And with one that IS open, so the drawing path is exercised over the
     * same stream and the copy still comes out exact. */
    {
        char tmp[] = "/tmp/nd-bootbar-testXXXXXX";
        int fd = mkstemp(tmp);
        static nd_bootfb live;

        if (fd >= 0) {
            (void)close(fd);
            memset(&live, 0, sizeof live);
            CHECK(nd_bootfb_open_at(&live, tmp, ND_BOOTFB_W, ND_BOOTFB_H, 32, 0u),
                  "an ordinary file opens when the geometry is supplied");
            got = 0;
            memset(dst, 0, sizeof dst);
            CHECK(copy_through(&live, src, sizeof src, dst, sizeof dst, &got),
                  "the filter reports success while drawing");
            CHECK(got == sizeof src && memcmp(src, dst, sizeof src) == 0,
                  "drawing does not disturb a single byte of the copy");
            nd_bootfb_close(&live);
            /* A frame really was written: 240 * 175 * 4 bytes of it. */
            {
                struct stat st;

                CHECK(stat(tmp, &st) == 0 && st.st_size == (off_t)(ND_BOOTFB_W * ND_BOOTFB_H * 4),
                      "a full 240x175 frame was written for every present");
            }
            (void)unlink(tmp);
        }
    }
}

/* ------------------------------------------------------------------ *
 * 4. Drawing, checked as pixels rather than as "it did not crash"
 * ------------------------------------------------------------------ */

static void check_pixels(void)
{
    static nd_bootfb fb;
    char tmp[] = "/tmp/nd-bootbar-pixXXXXXX";
    int fd = mkstemp(tmp);
    int32_t span;
    int32_t x;
    int32_t filled_at_50;

    if (fd < 0)
        return;
    (void)close(fd);

    memset(&fb, 0, sizeof fb);
    if (!nd_bootfb_open_at(&fb, tmp, ND_BOOTFB_W, ND_BOOTFB_H, 32, 0u)) {
        (void)unlink(tmp);
        g_fail++;
        return;
    }

    nd_bootbar_frame_at(&fb, "Installing", 2, 50, 53477376);

    span = (nd_bootbar_bar_box.x1 - ND_BOOTBAR_INSET) - (nd_bootbar_bar_box.x0 + ND_BOOTBAR_INSET);
    filled_at_50 = nd_trunc32((double)span * 50.0 / 100.0);

    /* The outline occupies the box's own rows and columns. */
    CHECK(fb.shadow[(size_t)nd_bootbar_bar_box.y0 * ND_BOOTFB_W + (size_t)nd_bootbar_bar_box.x0] ==
              1u,
          "the bar's top-left corner is drawn");
    CHECK(fb.shadow[(size_t)nd_bootbar_bar_box.y1 * ND_BOOTFB_W + (size_t)nd_bootbar_bar_box.x1] ==
              1u,
          "the bar's bottom-right corner is drawn");

    /* The fill reaches exactly as far as the widget's arithmetic says. */
    {
        size_t row = (size_t)(nd_bootbar_bar_box.y0 + 4) * ND_BOOTFB_W;
        int32_t last = nd_bootbar_bar_box.x0 + ND_BOOTBAR_INSET + filled_at_50;

        CHECK(fb.shadow[row + (size_t)last] == 1u, "the fill reaches its last column");
        CHECK(fb.shadow[row + (size_t)(last + 1)] == 0u,
              "the fill stops one column later, as trunc(span * percent / 100) says");
    }

    /* NOTHING IS EVER DRAWN ON TOP OF THE BAR -- nd_progress.c's first rule,
     * and the reason a percentage never sits across its own fill.
     *
     * The window checked is three pixels, not "everything below the label
     * box": label_box is an ANCHOR and not a clip, so the descender of
     * "Installing" legitimately hangs a few rows below y1. That is the same
     * bound test_update_ui.py uses -- "nothing within 3 px of the bar". */
    for (x = 0; x < ND_BOOTFB_W; x++) {
        int32_t y;

        for (y = nd_bootbar_bar_box.y0 - 3; y < nd_bootbar_bar_box.y0; y++) {
            if (fb.shadow[(size_t)y * ND_BOOTFB_W + (size_t)x] != 0u) {
                CHECK(false, "nothing is drawn within 3 px above the bar");
                x = ND_BOOTFB_W;
                break;
            }
        }
    }
    for (x = 0; x < ND_BOOTFB_W; x++) {
        int32_t y;

        for (y = nd_bootbar_bar_box.y1 + 1; y <= nd_bootbar_bar_box.y1 + 3; y++) {
            if (fb.shadow[(size_t)y * ND_BOOTFB_W + (size_t)x] != 0u) {
                CHECK(false, "nothing is drawn within 3 px below the bar");
                x = ND_BOOTFB_W;
                break;
            }
        }
    }

    /* Rows 146..174 are the softkey strip and this screen never draws there:
     * there is nothing to press during a boot install. */
    {
        bool clean = true;
        size_t i;

        for (i = (size_t)146 * ND_BOOTFB_W; i < (size_t)ND_BOOTFB_W * ND_BOOTFB_H; i++) {
            if (fb.shadow[i] != 0u)
                clean = false;
        }
        CHECK(clean, "the softkey strip stays black");
    }

    /* The failure screen: an empty bar, and no "Do not power off" -- there is
     * nothing left to interrupt. */
    nd_bootbar_fail(&fb, "Update refused", "Not signed by NeoDCT");
    {
        bool empty = true;
        int32_t y;

        for (y = nd_bootbar_bar_box.y0 + ND_BOOTBAR_INSET;
             y <= nd_bootbar_bar_box.y1 - ND_BOOTBAR_INSET; y++) {
            for (x = nd_bootbar_bar_box.x0 + ND_BOOTBAR_INSET;
                 x <= nd_bootbar_bar_box.x1 - ND_BOOTBAR_INSET; x++) {
                if (fb.shadow[(size_t)y * ND_BOOTFB_W + (size_t)x] != 0u)
                    empty = false;
            }
        }
        CHECK(empty, "a refusal draws the bar as an empty outline");
    }
    {
        bool clean = true;
        int32_t y;

        for (y = nd_bootbar_hint_box.y0; y <= nd_bootbar_hint_box.y1; y++) {
            for (x = 0; x < ND_BOOTFB_W; x++) {
                if (fb.shadow[(size_t)y * ND_BOOTFB_W + (size_t)x] != 0u)
                    clean = false;
            }
        }
        CHECK(clean, "a refusal draws no hint");
    }

    /* Something is actually on the screen -- a test that only checked what is
     * black would pass on a blank panel. */
    {
        size_t ink = 0;
        size_t i;

        nd_bootbar_frame_at(&fb, "Installing", 2, 63, 53477376);
        for (i = 0u; i < (size_t)ND_BOOTFB_W * ND_BOOTFB_H; i++)
            ink += (fb.shadow[i] != 0u) ? 1u : 0u;
        CHECK(ink > 2000u, "the frame has ink on it");
    }

    nd_bootfb_close(&fb);
    (void)unlink(tmp);
}

/* ------------------------------------------------------------------ *
 * 5. Geometry is read, never assumed
 * ------------------------------------------------------------------ */

static void check_geometry(void)
{
    static nd_bootfb fb;
    char tmp[] = "/tmp/nd-bootbar-geomXXXXXX";
    int fd = mkstemp(tmp);
    struct stat st;

    if (fd < 0)
        return;
    (void)close(fd);

    /* 24bpp is neither of the two depths a monochrome packer can be sure
     * about, so it is refused rather than guessed at. */
    memset(&fb, 0, sizeof fb);
    CHECK(!nd_bootfb_open_at(&fb, tmp, 240, 175, 24, 0u), "an unexpected bit depth is refused");
    nd_bootfb_close(&fb);

    /* 16bpp works, and a frame is half the size. */
    memset(&fb, 0, sizeof fb);
    CHECK(nd_bootfb_open_at(&fb, tmp, 240, 175, 16, 0u), "16bpp opens");
    nd_bootbar_frame_at(&fb, "Installing", 2, 100, 0);
    nd_bootfb_close(&fb);
    CHECK(stat(tmp, &st) == 0 && st.st_size == (off_t)(240 * 175 * 2),
          "a 16bpp frame is 240 * 175 * 2 bytes");

    /* A framebuffer taller and wider than the panel: the band goes top-left
     * and the row stride is honoured, exactly as `cat splash.raw > /dev/fb0`
     * lands it. */
    memset(&fb, 0, sizeof fb);
    CHECK(nd_bootfb_open_at(&fb, tmp, 240, 240, 32, 0u), "a 240x240 framebuffer opens");
    nd_bootbar_frame_at(&fb, "Installing", 2, 100, 0);
    nd_bootfb_close(&fb);
    CHECK(stat(tmp, &st) == 0 && st.st_size == (off_t)(240 * 175 * 4),
          "only the 175 rows of the band are written to a 240x240 panel");

    /* A padded stride: rows land at line_length, not at xres * bpp / 8. */
    memset(&fb, 0, sizeof fb);
    CHECK(nd_bootfb_open_at(&fb, tmp, 240, 175, 32, 1024u), "a row-padded framebuffer opens");
    nd_bootbar_frame_at(&fb, "Installing", 2, 100, 0);
    nd_bootfb_close(&fb);
    CHECK(stat(tmp, &st) == 0 && st.st_size == (off_t)(174 * 1024 + 240 * 4),
          "a padded framebuffer's last row starts at 174 * line_length");

    /* A stride narrower than one row is a driver nobody can draw for. */
    memset(&fb, 0, sizeof fb);
    CHECK(!nd_bootfb_open_at(&fb, tmp, 240, 175, 32, 100u),
          "a stride too small for one row is refused");
    nd_bootfb_close(&fb);

    (void)unlink(tmp);
}

int main(void)
{
    check_layout();
    check_percent();
    check_filter();
    check_pixels();
    check_geometry();

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_bootbar: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_bootbar: %d checks passed\n", g_checks);
    return 0;
}
