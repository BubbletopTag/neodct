/* test_displayd.c -- the panel daemon's SHARED half, run as a program.
 *
 * ============ WHY THIS TEST EXISTS AT ALL ============
 *
 * neodct_displayd was the one program in this tree with no host test, no ASAN
 * coverage and a -Wconversion exemption granted on the strength of being
 * proven on hardware nobody here has. The reason was structural rather than
 * neglect: every entry point it had went through /dev/spidev0.0 and
 * /dev/fb0, and a build host has neither.
 *
 * The panel-backend seam removes both obstacles without moving a line of the
 * composing code. `--panel stream:<path>` writes the DC-framed byte stream it
 * would have put on the SPI bus; `--fb-at WxH@BPP:STRIDE <file>` reads an
 * ordinary file as the framebuffer, geometry supplied rather than asked for,
 * exactly as nd_bootfb_open_at() does and with the same "never used on a
 * device" restriction. So the daemon's arithmetic -- the 8888->RGB565
 * big-endian pack, the red/blue order, the CASET/RASET encoding and the
 * `+ opt_yoff` letterbox -- can be driven from here, and under
 * `make ASAN=1 test` it is instrumented for the first time.
 *
 * It runs the BINARY rather than linking its objects, and that is deliberate:
 * the daemon's composing half is a set of file-scope statics wired together
 * by main(), and reshaping it into something linkable would be exactly the
 * "improvement while moving" the seam's own header rules out. The pixel
 * behaviour over TIME -- the dirty rectangle, the frame skip -- needs a
 * second frame and lives in neodct/tests/test_displayd_stream.py, which can
 * poll a running daemon; this file takes the single-frame half, which is the
 * half worth having under a sanitizer.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The wire format under test. tools/ and displayd/ are both outside the
 * library's include path, so this test gets -Idisplayd and nobody else does
 * -- the same arrangement test_bootbar has with tools/. */
#include "nd_panel.h"

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

#define CHECK_INT(got, want, what)                                              \
    do {                                                                        \
        long long g_ = (long long)(got);                                        \
        long long w_ = (long long)(want);                                       \
        g_checks++;                                                             \
        if (g_ != w_) {                                                         \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d  %s (got %lld, want %lld)\n", __FILE__, \
                    __LINE__, (what), g_, w_);                                  \
        }                                                                       \
    } while (0)

/* The panel and the band, spelled out here rather than included: this test is
 * a second opinion about the numbers, so taking them from the daemon's own
 * header would make it agree by construction. */
#define PANEL_W 240
#define PANEL_H 240
#define BAND_W  240
#define BAND_H  175
#define BAND_Y  65      /* PANEL_H - BAND_H, the Nokia faceplate letterbox */
#define STRIDE  960     /* BAND_W * 4, what vfb grants at 32 bpp */

/* ------------------------------------------------------------------ *
 * Finding the daemon and a place to work
 * ------------------------------------------------------------------ */

/* build/<flavour>/test/test_displayd -> build/<flavour>/bin/neodct_displayd.
 * /proc/self/exe rather than argv[0] because `make test` runs the binaries
 * through a sandbox and a runner, and neither promises a useful argv[0]. */
static int daemon_path(char *out, size_t out_sz)
{
    char self[1024];
    ssize_t n;
    char *cut;

    n = readlink("/proc/self/exe", self, sizeof self - 1u);
    if (n <= 0)
        return -1;
    self[n] = '\0';
    cut = strrchr(self, '/');
    if (cut == NULL)
        return -1;
    *cut = '\0';                 /* .../build/<flavour>/test */
    cut = strrchr(self, '/');
    if (cut == NULL)
        return -1;
    *cut = '\0';                 /* .../build/<flavour>      */
    if (snprintf(out, out_sz, "%s/bin/neodct_displayd", self) < 0)
        return -1;
    return access(out, X_OK);
}

static void work_dir(char *out, size_t out_sz)
{
    const char *root = getenv("NEODCT_ROOT");

    if (root == NULL || root[0] == '\0')
        root = "/tmp";
    (void)snprintf(out, out_sz, "%s/displayd", root);
    (void)mkdir(out, 0700);
}

/* ------------------------------------------------------------------ *
 * A framebuffer with known pixels in it
 * ------------------------------------------------------------------ */

/* Byte 0 is RED. That is vfb's 32bpp layout (red.offset 0, bytes R G B x),
 * which is the phone's framebuffer and now the emulator's -- and it is the
 * layout convert_rect() reaches by READING fb_var_screeninfo rather than
 * assuming, which is the bug fix its own comment block is about. */
static void put_px(unsigned char *fb, int x, int y,
                   unsigned char r, unsigned char g, unsigned char b)
{
    size_t o = (size_t)y * STRIDE + (size_t)x * 4u;

    fb[o + 0u] = r;
    fb[o + 1u] = g;
    fb[o + 2u] = b;
    fb[o + 3u] = 0u;
}

static unsigned short pack565(unsigned char r, unsigned char g, unsigned char b)
{
    return (unsigned short)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static int write_fb(const char *path, unsigned char *fb)
{
    FILE *f = fopen(path, "wb");
    size_t want = (size_t)BAND_H * STRIDE;

    if (f == NULL)
        return -1;
    if (fwrite(fb, 1u, want, f) != want) {
        (void)fclose(f);
        return -1;
    }
    return fclose(f);
}

/* ------------------------------------------------------------------ *
 * Running it
 * ------------------------------------------------------------------ */

/* CODING-STANDARDS 1.1: execv() is the first statement in the child, and
 * _exit() rather than exit() so the parent's buffers are not flushed twice. */
static int run_daemon(const char *bin, char *const argv[])
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        execv(bin, argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long size;

    *len = 0u;
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        (void)fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        (void)fclose(f);
        return NULL;
    }
    *len = fread(buf, 1u, (size_t)size, f);
    (void)fclose(f);
    return buf;                              /* freed by the caller */
}

/* ------------------------------------------------------------------ *
 * A transcript reader, written from nd_panel.h rather than from the sink
 * ------------------------------------------------------------------ */

typedef struct {
    unsigned char tag;
    unsigned long len;
    const unsigned char *payload;
} record;

static size_t read_records(const unsigned char *buf, size_t len,
                           record *out, size_t max)
{
    size_t off = ND_PANEL_HDR_LEN;
    size_t n = 0u;

    while (off + ND_PANEL_REC_HDR_LEN <= len && n < max) {
        unsigned long rl = (unsigned long)buf[off + 1u] |
                           ((unsigned long)buf[off + 2u] << 8) |
                           ((unsigned long)buf[off + 3u] << 16) |
                           ((unsigned long)buf[off + 4u] << 24);
        if (off + ND_PANEL_REC_HDR_LEN + rl > len)
            break;
        out[n].tag = buf[off];
        out[n].len = rl;
        out[n].payload = buf + off + ND_PANEL_REC_HDR_LEN;
        n++;
        off += ND_PANEL_REC_HDR_LEN + rl;
    }
    return n;
}

static int is_cmd(const record *r, unsigned char op)
{
    return r->tag == ND_PANEL_TAG_CMD && r->len == 1u && r->payload[0] == op;
}

static int is_window(const record *r, unsigned short lo, unsigned short hi)
{
    return r->tag == ND_PANEL_TAG_DATA && r->len == 4u &&
           r->payload[0] == (unsigned char)(lo >> 8) &&
           r->payload[1] == (unsigned char)(lo & 0xFFu) &&
           r->payload[2] == (unsigned char)(hi >> 8) &&
           r->payload[3] == (unsigned char)(hi & 0xFFu);
}

/* ------------------------------------------------------------------ *
 * The cases
 * ------------------------------------------------------------------ */

#define MAX_RECORDS 64

static void test_one_full_frame(const char *bin, const char *dir)
{
    char fb_path[768], tr_path[768], spec[64];
    unsigned char *fb = calloc(1u, (size_t)BAND_H * STRIDE);
    unsigned char *buf = NULL;
    record rec[MAX_RECORDS];
    size_t len = 0u, n;
    size_t i;
    char *argv[10];
    int rc;

    if (fb == NULL) {
        CHECK(0, "out of memory for the fixture framebuffer");
        return;
    }
    /* Three pixels whose 565 is worth naming: saturated red at the origin,
     * a mid grey, and a colour whose green survives six bits and whose blue
     * does not. Everything else stays black. */
    put_px(fb, 0, 0, 0xFFu, 0x00u, 0x00u);
    put_px(fb, 1, 0, 0x80u, 0x80u, 0x80u);
    put_px(fb, BAND_W - 1, BAND_H - 1, 0x12u, 0x7Eu, 0xC3u);

    (void)snprintf(fb_path, sizeof fb_path, "%s/fb.raw", dir);
    (void)snprintf(tr_path, sizeof tr_path, "%s/panel.nd79", dir);
    (void)snprintf(spec, sizeof spec, "%dx%d@32:%d", BAND_W, BAND_H, STRIDE);
    CHECK_INT(write_fb(fb_path, fb), 0, "the fixture framebuffer was written");

    {
        char panel_arg[832];
        FILE *touch = fopen(tr_path, "wb");   /* the sink does not O_CREAT */
        if (touch != NULL)
            (void)fclose(touch);
        (void)snprintf(panel_arg, sizeof panel_arg, "stream:%s", tr_path);
        argv[0] = (char *)(void *)"neodct_displayd";
        argv[1] = (char *)(void *)"--fb-at";
        argv[2] = spec;
        argv[3] = fb_path;
        argv[4] = (char *)(void *)"--panel";
        argv[5] = panel_arg;
        argv[6] = (char *)(void *)"--once";
        argv[7] = NULL;
        rc = run_daemon(bin, argv);
    }
    CHECK_INT(rc, 0, "neodct_displayd --once exits 0 with a file for a panel");

    buf = slurp(tr_path, &len);
    if (buf == NULL || len < ND_PANEL_HDR_LEN) {
        CHECK(0, "the transcript was written");
        free(fb);
        free(buf);
        return;
    }

    CHECK(memcmp(buf, ND_PANEL_MAGIC, ND_PANEL_MAGIC_LEN) == 0,
          "the transcript opens with ND79");
    CHECK_INT(buf[4], ND_PANEL_VERSION, "transcript version");
    CHECK_INT((unsigned)buf[6] | ((unsigned)buf[7] << 8), PANEL_W,
              "the header records the panel width");
    CHECK_INT((unsigned)buf[8] | ((unsigned)buf[9] << 8), PANEL_H,
              "the header records the panel height");

    n = read_records(buf, len, rec, MAX_RECORDS);
    CHECK_INT(n, 22, "one reset, the init sequence, the blanking fill, one frame");
    if (n < 22u) {
        free(fb);
        free(buf);
        return;
    }

    /* The bring-up, byte for byte. Every one of these reaches a backend that
     * is not the panel, so an ST7789 sequence is now readable by something
     * other than the panel itself. */
    i = 0u;
    CHECK(rec[i].tag == ND_PANEL_TAG_RESET && rec[i].len == 0u,
          "the reset pulse train is recorded as one empty record");
    i++;
    CHECK(is_cmd(&rec[i++], 0x01u), "SWRESET");
    CHECK(is_cmd(&rec[i++], 0x11u), "SLPOUT");
    CHECK(is_cmd(&rec[i++], 0x3Au), "COLMOD");
    CHECK(rec[i].tag == ND_PANEL_TAG_DATA && rec[i].len == 1u &&
          rec[i].payload[0] == 0x55u, "COLMOD 0x55 is RGB565");
    i++;
    CHECK(is_cmd(&rec[i++], 0x36u), "MADCTL");
    CHECK(rec[i].tag == ND_PANEL_TAG_DATA && rec[i].len == 1u &&
          rec[i].payload[0] == 0x00u, "MADCTL 0x00");
    i++;
    CHECK(is_cmd(&rec[i++], 0x21u), "INVON, required on this IPS panel");
    CHECK(is_cmd(&rec[i++], 0x13u), "NORON");
    CHECK(is_cmd(&rec[i++], 0x29u), "DISPON");

    /* fill_color(0,0,0): the whole 240x240, once. This is the letterboxing --
     * there is no compose buffer, so the rows above the band are black
     * because the panel was blanked and never written again. */
    CHECK(is_cmd(&rec[i++], 0x2Au), "CASET for the blanking fill");
    CHECK(is_window(&rec[i++], 0u, PANEL_W - 1u), "the fill spans every column");
    CHECK(is_cmd(&rec[i++], 0x2Bu), "RASET for the blanking fill");
    CHECK(is_window(&rec[i++], 0u, PANEL_H - 1u),
          "the fill spans every row, including the 65 above the band");
    CHECK(is_cmd(&rec[i++], 0x2Cu), "RAMWR for the blanking fill");
    CHECK_INT(rec[i].len, (size_t)PANEL_W * PANEL_H * 2u,
              "the fill payload is one whole 240x240 RGB565 frame");
    {
        int all_black = 1;
        unsigned long k;
        for (k = 0u; k < rec[i].len; k++)
            if (rec[i].payload[k] != 0u)
                all_black = 0;
        CHECK(all_black, "the fill is black");
    }
    i++;

    /* render_full(): the band, placed at y = 65. The offset arrives as BYTES
     * -- RASET 00 41 00 EF -- which is the whole reason this seam is at the
     * DC-framed byte and not at a rectangle. */
    CHECK(is_cmd(&rec[i++], 0x2Au), "CASET for the frame");
    CHECK(is_window(&rec[i++], 0u, BAND_W - 1u), "CASET 00 00 00 EF");
    CHECK(is_cmd(&rec[i++], 0x2Bu), "RASET for the frame");
    CHECK(is_window(&rec[i++], BAND_Y, PANEL_H - 1u), "RASET 00 41 00 EF");
    CHECK(is_cmd(&rec[i++], 0x2Cu), "RAMWR for the frame");
    CHECK_INT(rec[i].len, (size_t)BAND_W * BAND_H * 2u,
              "the frame payload is 240x175 RGB565 and not a whole panel");

    /* The pack, big-endian, red first. */
    {
        const unsigned char *p = rec[i].payload;
        unsigned short want;

        want = pack565(0xFFu, 0x00u, 0x00u);
        CHECK_INT(p[0], want >> 8, "pixel (0,0) high byte");
        CHECK_INT(p[1], want & 0xFFu, "pixel (0,0) low byte");
        CHECK_INT(want, 0xF800u, "saturated red is 0xF800, so red is bits 15..11");

        want = pack565(0x80u, 0x80u, 0x80u);
        CHECK_INT(p[2], want >> 8, "pixel (1,0) high byte");
        CHECK_INT(p[3], want & 0xFFu, "pixel (1,0) low byte");

        want = pack565(0x12u, 0x7Eu, 0xC3u);
        {
            size_t last = ((size_t)(BAND_H - 1) * BAND_W + (BAND_W - 1)) * 2u;
            CHECK_INT(p[last], want >> 8, "the last pixel's high byte");
            CHECK_INT(p[last + 1u], want & 0xFFu, "the last pixel's low byte");
        }
        /* And a pixel nobody set is black rather than whatever was in
         * out_buf from the blanking fill -- convert_rect() writes every byte
         * of the rectangle it was asked for. */
        CHECK_INT(p[4], 0, "an untouched pixel is black, high byte");
        CHECK_INT(p[5], 0, "an untouched pixel is black, low byte");
    }

    free(fb);
    free(buf);
}

static void test_null_backend_writes_nothing(const char *bin, const char *dir)
{
    char fb_path[768], tr_path[768], spec[64];
    unsigned char *fb = calloc(1u, (size_t)BAND_H * STRIDE);
    char *argv[9];
    struct stat st;
    int rc;

    if (fb == NULL) {
        CHECK(0, "out of memory for the fixture framebuffer");
        return;
    }
    (void)snprintf(fb_path, sizeof fb_path, "%s/fb-null.raw", dir);
    (void)snprintf(tr_path, sizeof tr_path, "%s/should-not-exist.nd79", dir);
    (void)snprintf(spec, sizeof spec, "%dx%d@32:%d", BAND_W, BAND_H, STRIDE);
    (void)write_fb(fb_path, fb);
    (void)unlink(tr_path);

    argv[0] = (char *)(void *)"neodct_displayd";
    argv[1] = (char *)(void *)"--fb-at";
    argv[2] = spec;
    argv[3] = fb_path;
    argv[4] = (char *)(void *)"--panel";
    argv[5] = (char *)(void *)"null";
    argv[6] = (char *)(void *)"--once";
    argv[7] = NULL;
    rc = run_daemon(bin, argv);
    CHECK_INT(rc, 0, "--panel null composes a frame and exits 0");
    CHECK(stat(tr_path, &st) != 0 && errno == ENOENT,
          "--panel null allocates no sink and writes no file");
    free(fb);
}

static void test_the_backend_is_chosen_never_guessed(const char *bin,
                                                     const char *dir)
{
    char fb_path[768], spec[64];
    char *argv[9];
    int rc;

    (void)snprintf(fb_path, sizeof fb_path, "%s/fb-null.raw", dir);
    (void)snprintf(spec, sizeof spec, "%dx%d@32:%d", BAND_W, BAND_H, STRIDE);

    /* `stream:` with nothing after it must NOT quietly become the null sink.
     * That is the one shape a chosen-never-detected backend cannot have: a
     * phone told to write a transcript and silently writing none would report
     * success while driving no panel. */
    argv[0] = (char *)(void *)"neodct_displayd";
    argv[1] = (char *)(void *)"--fb-at";
    argv[2] = spec;
    argv[3] = fb_path;
    argv[4] = (char *)(void *)"--panel";
    argv[5] = (char *)(void *)"stream:";
    argv[6] = (char *)(void *)"--once";
    argv[7] = NULL;
    rc = run_daemon(bin, argv);
    CHECK_INT(rc, 2, "--panel stream: with no path is refused");

    argv[5] = (char *)(void *)"vport";
    rc = run_daemon(bin, argv);
    CHECK_INT(rc, 2, "an unknown backend name is refused, not guessed at");

    /* And a path that cannot be opened is fatal at startup rather than a
     * quiet degrade: runtime is lossy on purpose, startup is loud.
     *
     * The second of these is the one a boot found. The sink used to pass
     * O_CREAT, so a guest whose virtio-serial port was not attached opened
     * /dev/vport0p1 as a new regular file in devtmpfs and reported success --
     * a panel stream written to nowhere, which is the one thing a
     * chosen-never-detected backend must never do. */
    argv[5] = (char *)(void *)"stream:/nonexistent-directory/panel.nd79";
    rc = run_daemon(bin, argv);
    CHECK_INT(rc, 1, "a transcript path in no directory stops the daemon");

    {
        char missing[832];
        (void)snprintf(missing, sizeof missing, "stream:%s/never-created.nd79", dir);
        argv[5] = missing;
        rc = run_daemon(bin, argv);
        CHECK_INT(rc, 1, "a transcript path that does not exist is NOT created");
    }
}

static void test_fb_at_refuses_a_short_file(const char *bin, const char *dir)
{
    char fb_path[768], spec[64];
    char *argv[9];
    FILE *f;
    int rc;

    (void)snprintf(fb_path, sizeof fb_path, "%s/short.raw", dir);
    (void)snprintf(spec, sizeof spec, "%dx%d@32:%d", BAND_W, BAND_H, STRIDE);
    f = fopen(fb_path, "wb");
    if (f != NULL) {
        (void)fputs("not a framebuffer", f);
        (void)fclose(f);
    }

    argv[0] = (char *)(void *)"neodct_displayd";
    argv[1] = (char *)(void *)"--fb-at";
    argv[2] = spec;
    argv[3] = fb_path;
    argv[4] = (char *)(void *)"--panel";
    argv[5] = (char *)(void *)"null";
    argv[6] = (char *)(void *)"--once";
    argv[7] = NULL;
    rc = run_daemon(bin, argv);
    /* mmap() past the end of a short file is a SIGBUS on first read, which
     * looks like a daemon bug rather than a mis-sized fixture. */
    CHECK_INT(rc, 1, "--fb-at refuses a file too small for the geometry");
}

/* --fb-at is the one path where a human types the geometry, and a stride
 * SMALLER than the row it describes passes the length test: 240x175@32 with
 * stride 480 is exactly 84,000 bytes, which is exactly stride * h. Everything
 * downstream then indexes rows at y * line_length and reads copy_w * 4 bytes
 * inside them. Reproduced under ASAN before the guard existed: a
 * heap-buffer-overflow WRITE of 960 bytes 0 bytes past an 84,000-byte region,
 * in render_full()'s memcpy into prev_fb. This case is here rather than only
 * in the Python suite because ASAN is what would catch a regression. */
static void test_fb_at_refuses_an_impossible_stride(const char *bin,
                                                    const char *dir)
{
    char fb_path[768], spec[64];
    char *argv[9];
    unsigned char *fb = calloc(1u, (size_t)BAND_H * (BAND_W * 2u));
    FILE *f;
    int rc;

    if (fb == NULL) {
        CHECK(0, "out of memory for the fixture framebuffer");
        return;
    }
    (void)snprintf(fb_path, sizeof fb_path, "%s/narrow.raw", dir);
    /* 480 bytes a row where 240 pixels at 32 bpp need 960. The FILE is the
     * right length for the stride it declares, so only a stride-against-width
     * check can refuse it. */
    (void)snprintf(spec, sizeof spec, "%dx%d@32:%d", BAND_W, BAND_H, BAND_W * 2);
    f = fopen(fb_path, "wb");
    if (f != NULL) {
        (void)fwrite(fb, 1u, (size_t)BAND_H * (BAND_W * 2u), f);
        (void)fclose(f);
    }
    free(fb);

    argv[0] = (char *)(void *)"neodct_displayd";
    argv[1] = (char *)(void *)"--fb-at";
    argv[2] = spec;
    argv[3] = fb_path;
    argv[4] = (char *)(void *)"--panel";
    argv[5] = (char *)(void *)"null";
    argv[6] = (char *)(void *)"--once";
    argv[7] = NULL;
    rc = run_daemon(bin, argv);
    CHECK_INT(rc, 1, "--fb-at refuses a stride narrower than its own width");
}

/* ============ THE OTHER BRANCH OF convert_rect(), WHICH NOTHING RAN =======
 *
 * force_mode() asks for 32 bpp and falls back to 16, and every other test on
 * either machine drives the daemon at 32 -- so the branch the fallback lands
 * in had no coverage anywhere. That is not hypothetical: the
 * fb0.fix.smem_len record in tests/parity/allow.txt exists precisely because
 * a phone whose vfb is smaller than 168,000 bytes cannot be granted
 * 240x175x32 at all, takes this branch, and runs a different pixel pipeline
 * from the emulator with nothing saying so. The branch that record warns
 * about was the one branch nothing exercised.
 *
 * 16 bpp on vfb is 5-6-5 with red at offset 11, so red > blue and
 * fb_swap_rb is 0: the daemon's job here is a pure little-endian to
 * big-endian repack, and these three values are the ones that make a channel
 * swap visible if one ever appears. */
static void test_the_16bpp_fallback_branch_repacks_565(const char *bin,
                                                       const char *dir)
{
    const unsigned short want[3] = { 0xF800u, 0x07E0u, 0x001Fu };
    size_t stride16 = (size_t)BAND_W * 2u;
    unsigned char *fb = calloc(1u, (size_t)BAND_H * stride16);
    char fb_path[768], tr_path[768], spec[64], panel_arg[832];
    record rec[MAX_RECORDS];
    unsigned char *buf = NULL;
    size_t len = 0u, n, i;
    char *argv[10];
    FILE *f;
    int rc, found = 0;

    if (fb == NULL) {
        CHECK(0, "out of memory for the fixture framebuffer");
        return;
    }
    /* Little-endian in the fixture, because that is how the driver's
     * framebuffer holds a native 565 value. */
    for (i = 0u; i < 3u; i++) {
        fb[i * 2u]      = (unsigned char)(want[i] & 0xFFu);
        fb[i * 2u + 1u] = (unsigned char)(want[i] >> 8);
    }

    (void)snprintf(fb_path, sizeof fb_path, "%s/fb16.raw", dir);
    (void)snprintf(tr_path, sizeof tr_path, "%s/panel16.nd79", dir);
    (void)snprintf(spec, sizeof spec, "%dx%d@16:%lu", BAND_W, BAND_H,
                   (unsigned long)stride16);
    f = fopen(fb_path, "wb");
    if (f != NULL) {
        (void)fwrite(fb, 1u, (size_t)BAND_H * stride16, f);
        (void)fclose(f);
    }
    free(fb);
    f = fopen(tr_path, "wb");            /* the sink does not O_CREAT */
    if (f != NULL)
        (void)fclose(f);

    (void)snprintf(panel_arg, sizeof panel_arg, "stream:%s", tr_path);
    argv[0] = (char *)(void *)"neodct_displayd";
    argv[1] = (char *)(void *)"--fb-at";
    argv[2] = spec;
    argv[3] = fb_path;
    argv[4] = (char *)(void *)"--panel";
    argv[5] = panel_arg;
    argv[6] = (char *)(void *)"--once";
    argv[7] = NULL;
    rc = run_daemon(bin, argv);
    CHECK_INT(rc, 0, "--fb-at at 16 bpp composes a frame and exits 0");

    buf = slurp(tr_path, &len);
    if (buf == NULL) {
        CHECK(0, "the 16 bpp transcript was written");
        return;
    }
    n = read_records(buf, len, rec, MAX_RECORDS);
    /* The LAST data record is the frame's payload; the one before the
     * letterbox band is the 240x240 blanking fill. */
    for (i = n; i-- > 0u; ) {
        if (rec[i].tag == ND_PANEL_TAG_DATA &&
            rec[i].len == (size_t)BAND_W * BAND_H * 2u) {
            const unsigned char *p = rec[i].payload;
            size_t k;

            found = 1;
            for (k = 0u; k < 3u; k++) {
                CHECK_INT(p[k * 2u], want[k] >> 8,
                          "16 bpp: the panel takes the high byte first");
                CHECK_INT(p[k * 2u + 1u], want[k] & 0xFFu,
                          "16 bpp: and the low byte second");
            }
            break;
        }
    }
    CHECK(found, "the 16 bpp frame payload is a whole 240x175 band");
    free(buf);
}

int main(void)
{
    char bin[2048];
    char dir[512];

    if (daemon_path(bin, sizeof bin) != 0) {
        (void)printf("test_displayd: SKIP (no neodct_displayd beside this test)\n");
        return 0;
    }
    work_dir(dir, sizeof dir);

    test_one_full_frame(bin, dir);
    test_null_backend_writes_nothing(bin, dir);
    test_the_backend_is_chosen_never_guessed(bin, dir);
    test_fb_at_refuses_a_short_file(bin, dir);
    test_fb_at_refuses_an_impossible_stride(bin, dir);
    test_the_16bpp_fallback_branch_repacks_565(bin, dir);

    (void)printf("test_displayd: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
