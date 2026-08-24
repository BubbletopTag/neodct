/* test_shoot.c -- the nd-shoot capture tool, end to end.
 *
 * ============ WHY THIS TEST RUNS A BINARY ============
 *
 * nd-shoot has no library surface to call: it is a main() that stages a
 * /NeoDCT root, renders, and writes a directory. The thing that can be wrong
 * is the directory. So this spawns the real binary the way a developer does
 * and then judges what came out -- which also means the argument parsing, the
 * root staging, the output layout and the exit status are all covered, and
 * none of them could be covered by linking a helper.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. nd-shoot exits 0 and writes a manifest goldenframe.py's compare() will
 *     accept: text_layout BASIC, and the epoch/tick/seed triple that all 49
 *     stored reference frames were captured under. Changing any of the three
 *     invalidates the whole oracle, so they are pinned here as numbers.
 *
 *  2. EVERY FRAME IT RENDERS IS BYTE-IDENTICAL TO THE PYTHON'S. Checked as
 *     SHA-256 over raw RGB against neodct/tests/golden/manifest.json -- the
 *     same digest goldenframe.py compares, so a pass here is a pass there.
 *     This is the acceptance check for the whole port, not just for the tool.
 *
 *     ONE frame is allowed not to be: eng-cubebench, the single `tolerance`
 *     name in OPEN-QUESTIONS.md's frame tolerance policy. sin() and cos()
 *     disagree by an ULP between glibc, uClibc-ng and musl, and one ULP in a
 *     rotation matrix can move a wireframe vertex by a pixel. So a digest
 *     mismatch there is measured against the stored PNG and checked against a
 *     cap of nine pixels rather than failing outright -- and the count is
 *     PRINTED either way, so a regression that happens to stay under the cap
 *     cannot hide behind it. On a glibc host the delta is zero, because
 *     CPython's math.sin is the platform libm's and the Python capture ran
 *     the same code on the same doubles.
 *
 *     No other name gets this. A tolerance is a budget, not an excuse.
 *
 *  3. The skip list is honest and complete: rendered and skipped together
 *     account for all 49 reference names, exactly once each, with nothing
 *     invented and nothing quietly dropped. A frame that was skipped must NOT
 *     also have a PNG on disk -- "never emit a frame you cannot justify" is
 *     the property under test, and it is checkable.
 *
 *  4. RENDERED[] AND SKIPPED[] ARE THE SAME LIST THE RUN PRODUCED. `--list`
 *     is nd-shoot's claim about itself and it went two names stale once
 *     already; it is checked against the manifest a real run wrote, so the
 *     documentation and the behaviour cannot drift apart again.
 *
 *  5. Two runs produce byte-identical output. The virtual clock, the pinned
 *     PRNG and the UTC timezone are the whole reason the oracle works; a
 *     capture that drifted between runs could not be compared with anything.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (the Makefile
 * passes it) and the overlay is found relative to it. NEODCT_SHOOT_BIN
 * overrides where nd-shoot is looked for; by default it is ../bin/nd-shoot
 * relative to this binary, which keeps an ASan test pointed at the ASan tool.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_image.h"
#include "nd_json.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* OPEN-QUESTIONS.md frame tolerance policy: the one `tolerance` frame, and
 * the honest cap -- "single-digit pixels along the wireframe edges". The same
 * two constants test_cubebench.c pins; kept in both because each test must
 * pass on its own with no arguments. */
#define TOLERANCE_FRAME     "eng-cubebench"
#define TOLERANCE_PIXEL_CAP 9

/* The same policy's `recut` class: a reference re-captured from the C build
 * because no amount of correct porting could reproduce it. There is exactly
 * one -- game-snake, whose food cell comes out of a generator decision 4
 * refused to reimplement. It is compared like any other frame and must be
 * EXACT against its reference; the list exists only so the summary line
 * below does not claim it is byte-identical to the Python, which it is not.
 *
 * game-memory is deliberately NOT here. Memory's shuffle differs from the
 * Python's too, but every card in that frame is face down, so the shuffle
 * reaches no pixel and the frame is byte-identical to the Python's after
 * all. It kept its original reference. */
static const char *const RECUT[] = {"game-snake", "eng-fuelgauge"};

static bool is_recut(const char *name)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(RECUT); i++) {
        if (strcmp(name, RECUT[i]) == 0)
            return true;
    }
    return false;
}

extern char **environ;

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
 * Finding things
 * ------------------------------------------------------------------ */

static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];
static char g_shoot[ND_PATH_MAX];
static char g_dirs[4][ND_PATH_MAX];
static size_t g_n_dirs;

static bool path_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static bool find_inputs(void)
{
    const char *golden = getenv("NEODCT_GOLDEN");
    const char *override = getenv("NEODCT_SHOOT_BIN");
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    if (golden == NULL || golden[0] == '\0')
        return false;
    if (nd_snprintf(g_golden, sizeof g_golden, "%s", golden) != ND_OK)
        return false;
    /* <repo>/neodct/tests/golden -> <repo>/neodct/overlay */
    if (nd_snprintf(g_overlay, sizeof g_overlay, "%s/../../overlay", golden) != ND_OK)
        return false;

    if (override != NULL && override[0] != '\0')
        return nd_snprintf(g_shoot, sizeof g_shoot, "%s", override) == ND_OK;

    /* build/<variant>/test/test_shoot -> build/<variant>/bin/nd-shoot, so an
     * ASan run tests the ASan tool and not a stale default-variant one. */
    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(g_shoot, sizeof g_shoot, "%s/../bin/nd-shoot", exe) == ND_OK;
}

static bool make_out_dir(char *out, size_t out_sz)
{
    const char *base = getenv("TMPDIR");
    char tmpl[ND_PATH_MAX];

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndshoot-test-XXXXXX", base) != ND_OK)
        return false;
    if (mkdtemp(tmpl) == NULL)
        return false;
    if (g_n_dirs < ND_ARRAY_LEN(g_dirs))
        (void)nd_strlcpy(g_dirs[g_n_dirs++], tmpl, ND_PATH_MAX);
    return nd_strlcpy(out, tmpl, out_sz) < out_sz;
}

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

static void drop_dirs(void)
{
    size_t i;

    for (i = 0u; i < g_n_dirs; i++)
        (void)nftw(g_dirs[i], rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    g_n_dirs = 0u;
}

/* ------------------------------------------------------------------ *
 * Running nd-shoot
 * ------------------------------------------------------------------ */

/* Returns the child's exit status, or -1 if it did not exit normally.
 *
 * CODING-STANDARDS.md section 1.1: execve is the FIRST statement in the
 * child, and _exit -- not exit -- is the only other call there. Everything
 * the child needs is built before the fork. */
static int run_shoot(char *const argv[])
{
    pid_t pid;
    int status = 0;

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execve(g_shoot, argv, environ);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int shoot_into(const char *out_dir)
{
    char *argv[4];

    argv[0] = g_shoot;
    argv[1] = (char *)"--out";
    argv[2] = (char *)out_dir;
    argv[3] = NULL;
    return run_shoot(argv);
}

/* Same fork/execve, with the child's stdout on a file so --list can be read
 * back. CODING-STANDARDS.md section 1.1 still applies: the descriptor is
 * opened BEFORE the fork, and dup2/_exit are the only calls the child makes
 * before execve -- both are async-signal-safe. */
static int shoot_capture(char *const argv[], const char *to_path)
{
    pid_t pid;
    int status = 0;
    int fd = open(to_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", to_path, strerror(errno));
        return -1;
    }
    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        (void)close(fd);
        return -1;
    }
    if (pid == 0) {
        if (dup2(fd, STDOUT_FILENO) < 0)
            _exit(126);
        execve(g_shoot, argv, environ);
        _exit(127);
    }
    (void)close(fd);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/* ------------------------------------------------------------------ *
 * Reading what it wrote
 * ------------------------------------------------------------------ */

/* Plain fopen throughout: none of these paths is under ND_ROOT, so
 * nd_json_parse_file() -- which resolves -- would look in the wrong place. */
static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long len;

    *len_out = 0u;
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0)
        goto done;
    buf = malloc((size_t)len);
    if (buf == NULL)
        goto done;
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
        goto done;
    }
    *len_out = (size_t)len;
done:
    (void)fclose(f);
    return buf;
}

static nd_json_doc *load_json(const char *dir, const char *name)
{
    char path[ND_PATH_MAX];
    nd_json_doc *doc = NULL;
    uint8_t *buf;
    size_t len;

    if (nd_snprintf(path, sizeof path, "%s/%s", dir, name) != ND_OK)
        return NULL;
    buf = slurp(path, &len);
    if (buf == NULL)
        return NULL;
    if (nd_json_parse(buf, len, &doc, NULL, 0u) != ND_OK)
        doc = NULL;
    free(buf);
    return doc;
}

static const nd_json_val *frame_named(const nd_json_val *frames, const char *name)
{
    size_t i;

    for (i = 0u; i < nd_json_len(frames); i++) {
        const nd_json_val *e = nd_json_at(frames, i);

        if (strcmp(nd_json_get_str(e, "name", ""), name) == 0)
            return e;
    }
    return NULL;
}

static bool size_of(const nd_json_val *frame, int64_t *w, int64_t *h)
{
    const nd_json_val *size = nd_json_get(frame, "size");

    if (size == NULL || nd_json_type_of(size) != ND_JSON_ARRAY || nd_json_len(size) != 2u)
        return false;
    return nd_json_int(nd_json_at(size, 0u), w) && nd_json_int(nd_json_at(size, 1u), h);
}

/* ------------------------------------------------------------------ *
 * 1. The manifest goldenframe.py has to accept
 * ------------------------------------------------------------------ */

/* compare() rejects any manifest whose text_layout is not BASIC, on both
 * sides, because a reference captured on a libraqm host describes a phone
 * that does not exist -- that mistake cost this project 46 wrong reference
 * frames once. */
static void test_manifest_shape(const nd_json_doc *doc)
{
    const nd_json_val *root = nd_json_root(doc);
    const nd_json_val *frames;
    double epoch = 0.0;
    double tick = 0.0;

    CHECK_STR(nd_json_get_str(root, "text_layout", ""), "BASIC", "text_layout");
    CHECK(nd_json_real(nd_json_get(root, "epoch"), &epoch), "epoch is a number");
    CHECK(epoch == ND_VCLOCK_EPOCH, "epoch is goldenframe.EPOCH");
    CHECK(nd_json_real(nd_json_get(root, "tick"), &tick), "tick is a number");
    CHECK(tick == ND_VCLOCK_TICK, "tick is goldenframe.TICK");
    CHECK_INT(nd_json_get_int(root, "seed", -1), ND_VCLOCK_SEED, "seed is goldenframe.SEED");

    frames = nd_json_get(root, "frames");
    CHECK(frames != NULL && nd_json_type_of(frames) == ND_JSON_ARRAY, "frames is an array");
}

/* write_manifest() sorts by name, and compare() looks entries up by name --
 * so an unsorted manifest still compares, but a DUPLICATE name would silently
 * hide one of the two. Sorted-and-strictly-ascending rules both out at once. */
static void test_frames_sorted(const nd_json_val *frames)
{
    size_t i;

    for (i = 1u; i < nd_json_len(frames); i++) {
        const char *prev = nd_json_get_str(nd_json_at(frames, i - 1u), "name", "");
        const char *cur = nd_json_get_str(nd_json_at(frames, i), "name", "");

        CHECK(strcmp(prev, cur) < 0, "frame names strictly ascending");
    }
}

/* ------------------------------------------------------------------ *
 * 2. Every rendered frame against the Python's own pixels
 * ------------------------------------------------------------------ */

/* Differing pixels between two PNGs, with the bounding box: "four pixels of
 * wireframe" and "the whole content rectangle" are different bugs, and only
 * the number and the box say which. Negative when either file cannot be read
 * or the two disagree on size, which is a failure in its own right. */
static int32_t png_diff_pixels(const char *a_path, const char *b_path, nd_rect *box)
{
    nd_image *a = nd_image_load_png(a_path);
    nd_image *b = NULL;
    int32_t n = -1;
    int32_t x;
    int32_t y;

    *box = ND_RECT(0, 0, -1, -1);
    /* nd_image_load_png() resolves through ND_ROOT; neither the reference set
     * nor nd-shoot's output directory is under one, and this test sets none. */
    if (a == NULL) {
        fprintf(stderr, "test_shoot: cannot read %s\n", a_path);
        goto done;
    }
    b = nd_image_load_png(b_path);
    if (b == NULL) {
        fprintf(stderr, "test_shoot: cannot read %s\n", b_path);
        goto done;
    }
    if (a->w != b->w || a->h != b->h) {
        fprintf(stderr, "test_shoot: %s is %dx%d, %s is %dx%d\n", a_path, a->w, a->h, b_path, b->w,
                b->h);
        goto done;
    }

    n = 0;
    for (y = 0; y < a->h; y++) {
        for (x = 0; x < a->w; x++) {
            nd_color pa = nd_image_get_px(a, x, y);
            nd_color pb = nd_image_get_px(b, x, y);

            if (pa.r == pb.r && pa.g == pb.g && pa.b == pb.b)
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

done:
    nd_image_free(a);
    nd_image_free(b);
    return n;
}

/* The tolerance path. Called only when eng-cubebench's digest did not match,
 * which on this host does not happen. */
static void check_tolerance_frame(const char *out_dir, const char *name)
{
    char ours[ND_PATH_MAX];
    char ref[ND_PATH_MAX];
    nd_rect box;
    int32_t n;

    if (nd_snprintf(ours, sizeof ours, "%s/%s.png", out_dir, name) != ND_OK ||
        nd_snprintf(ref, sizeof ref, "%s/%s.png", g_golden, name) != ND_OK) {
        CHECK(false, "tolerance frame paths");
        return;
    }

    n = png_diff_pixels(ref, ours, &box);
    if (n < 0) {
        CHECK(false, "the tolerance frame can be measured against its reference");
        return;
    }
    fprintf(stderr,
            "test_shoot: %s differs by %d px (%.3f%%), box (%d,%d)-(%d,%d) -- libm tolerance\n",
            name, n, 100.0 * (double)n / 42000.0, box.x0, box.y0, box.x1, box.y1);
    CHECK(n <= TOLERANCE_PIXEL_CAP, "the tolerance frame is inside its pixel budget");
}

static void test_against_golden(const char *out_dir, const nd_json_val *ours,
                                const nd_json_val *golden)
{
    size_t i;
    size_t exact = 0u;

    for (i = 0u; i < nd_json_len(ours); i++) {
        const nd_json_val *mine = nd_json_at(ours, i);
        const char *name = nd_json_get_str(mine, "name", "");
        const nd_json_val *ref = frame_named(golden, name);
        int64_t gw = 0;
        int64_t gh = 0;
        int64_t mw = 0;
        int64_t mh = 0;
        char png[ND_PATH_MAX];

        if (ref == NULL) {
            g_checks++;
            g_failures++;
            fprintf(stderr, "FAIL frame %s is not in the reference set at all\n", name);
            continue;
        }

        /* The PNG is written only so goldenframe's _describe_pixel_diff() can
         * show a human WHERE a frame differs; the digest is over the pixels.
         * Both have to be there for a diff to be investigable. */
        CHECK(nd_snprintf(png, sizeof png, "%s/%s.png", out_dir, name) == ND_OK, "png path");
        CHECK(path_exists(png), name);

        CHECK(size_of(mine, &mw, &mh) && size_of(ref, &gw, &gh), "sizes parse");
        CHECK_INT(mw, gw, name);
        CHECK_INT(mh, gh, name);

        if (strcmp(nd_json_get_str(mine, "sha256", "?"), nd_json_get_str(ref, "sha256", "!")) !=
            0) {
            /* The one name the policy budgets for; everything else is a
             * straight failure. */
            if (strcmp(name, TOLERANCE_FRAME) == 0) {
                check_tolerance_frame(out_dir, name);
            } else {
                g_checks++;
                g_failures++;
                fprintf(stderr, "FAIL frame %-30s got  %s\n%36swant %s\n", name,
                        nd_json_get_str(mine, "sha256", "?"), "",
                        nd_json_get_str(ref, "sha256", "!"));
            }
        } else {
            g_checks++;
            exact++;
            if (strcmp(name, TOLERANCE_FRAME) == 0) {
                /* Worth saying out loud: the budget was not needed. */
                printf("test_shoot: %s is BYTE-EXACT (0 differing pixels)\n", name);
            } else if (is_recut(name)) {
                printf("test_shoot: %s is exact against its RECUT reference "
                       "(OPEN-QUESTIONS.md decision 4)\n",
                       name);
            }
        }
    }
    printf("test_shoot: %zu of %zu rendered frames match their reference exactly "
           "(%zu of them recut from C)\n",
           exact, nd_json_len(ours), ND_ARRAY_LEN(RECUT));
}

/* ------------------------------------------------------------------ *
 * 3. The skip list is honest and complete
 * ------------------------------------------------------------------ */

static void test_skip_list(const char *out_dir, const nd_json_val *ours, const nd_json_val *golden)
{
    nd_json_doc *doc = load_json(out_dir, "nd-shoot-skipped.json");
    const nd_json_val *skipped;
    size_t i;

    if (doc == NULL) {
        CHECK(false, "nd-shoot-skipped.json parses");
        return;
    }
    skipped = nd_json_get(nd_json_root(doc), "skipped");
    if (skipped == NULL || nd_json_type_of(skipped) != ND_JSON_ARRAY) {
        CHECK(false, "skipped is an array");
        nd_json_free(doc);
        return;
    }

    for (i = 0u; i < nd_json_len(skipped); i++) {
        const nd_json_val *e = nd_json_at(skipped, i);
        const char *name = nd_json_get_str(e, "name", "");
        const char *reason = nd_json_get_str(e, "reason", "");
        char png[ND_PATH_MAX];

        CHECK(reason[0] != '\0', "every skip carries a reason");
        CHECK(frame_named(golden, name) != NULL, "a skipped name is a real reference name");
        CHECK(frame_named(ours, name) == NULL, "a skipped name is not also claimed as rendered");

        /* The property the task states: never emit a frame you cannot
         * justify. A skipped name with a PNG on disk would be exactly that. */
        CHECK(nd_snprintf(png, sizeof png, "%s/%s.png", out_dir, name) == ND_OK, "png path");
        CHECK(!path_exists(png), "a skipped frame left no picture behind");
    }

    /* Together they must account for the whole reference set, so a name that
     * is neither rendered nor declared missing cannot slip through. */
    CHECK_INT(nd_json_len(ours) + nd_json_len(skipped), nd_json_len(golden),
              "rendered + skipped == the reference set");
    for (i = 0u; i < nd_json_len(golden); i++) {
        const char *name = nd_json_get_str(nd_json_at(golden, i), "name", "");
        bool accounted = frame_named(ours, name) != NULL || frame_named(skipped, name) != NULL;

        if (!accounted)
            fprintf(stderr, "FAIL reference frame %s is neither rendered nor skipped\n", name);
        CHECK(accounted, "every reference frame is accounted for");
    }

    nd_json_free(doc);
}

/* ------------------------------------------------------------------ *
 * 4. Determinism
 * ------------------------------------------------------------------ */

/* The whole oracle rests on this: same binary, same inputs, same bytes. If
 * this fails, a golden comparison means nothing, because a "pass" could be
 * luck and a "fail" could be the wall clock. */
static void test_determinism(const char *a_dir, const char *b_dir, const nd_json_val *a_frames)
{
    size_t i;

    for (i = 0u; i < nd_json_len(a_frames); i++) {
        const char *name = nd_json_get_str(nd_json_at(a_frames, i), "name", "");
        char pa[ND_PATH_MAX];
        char pb[ND_PATH_MAX];
        uint8_t *ba;
        uint8_t *bb;
        size_t la = 0u;
        size_t lb = 0u;

        if (nd_snprintf(pa, sizeof pa, "%s/%s.png", a_dir, name) != ND_OK ||
            nd_snprintf(pb, sizeof pb, "%s/%s.png", b_dir, name) != ND_OK) {
            CHECK(false, "png paths");
            continue;
        }
        ba = slurp(pa, &la);
        bb = slurp(pb, &lb);
        g_checks++;
        if (ba == NULL || bb == NULL || la != lb || memcmp(ba, bb, la) != 0) {
            g_failures++;
            fprintf(stderr, "FAIL %s differs between two runs\n", name);
        }
        free(ba);
        free(bb);
    }

    {
        char pa[ND_PATH_MAX];
        char pb[ND_PATH_MAX];
        uint8_t *ba;
        uint8_t *bb;
        size_t la = 0u;
        size_t lb = 0u;

        CHECK(nd_snprintf(pa, sizeof pa, "%s/manifest.json", a_dir) == ND_OK, "manifest path a");
        CHECK(nd_snprintf(pb, sizeof pb, "%s/manifest.json", b_dir) == ND_OK, "manifest path b");
        ba = slurp(pa, &la);
        bb = slurp(pb, &lb);
        CHECK(ba != NULL && bb != NULL && la == lb && memcmp(ba, bb, la) == 0,
              "the two manifests are byte-identical");
        free(ba);
        free(bb);
    }
}

/* ------------------------------------------------------------------ *
 * 4. --list says what the run actually did
 * ------------------------------------------------------------------ *
 *
 * nd-shoot carries two hand-written tables -- RENDERED[] and SKIPPED[] -- and
 * they are what a reader consults to find out what the port covers. They are
 * also the two things nothing forced to stay true: SKIPPED[] claimed
 * "neodct/src/apps/ is empty" long after two apps had landed in it, and
 * RENDERED[] was two names short at the same time. Both were only wrong on
 * paper, which is the kind of wrong that survives.
 *
 * So the tables are checked against a real run: every name --list calls
 * rendered is in the manifest that run wrote, every name it calls skipped is
 * in the skip file, and neither list has anything the run did not produce.
 */

/* One line of `--list` output, which is "  <name>" under "rendered (N):" and
 * "  <name>  <reason>" under "skipped (N):". Returns the name, in place, with
 * the reason cut off. */
static char *list_line_name(char *line)
{
    char *p = line;
    char *end;

    while (*p == ' ' || *p == '\t')
        p++;
    end = p;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n')
        end++;
    *end = '\0';
    return p;
}

static void test_list_matches_run(const char *out_dir, const nd_json_val *ours)
{
    char path[ND_PATH_MAX];
    char line[512];
    char *argv[3];
    FILE *f;
    nd_json_doc *skip_doc = load_json(out_dir, "nd-shoot-skipped.json");
    const nd_json_val *skipped = NULL;
    size_t n_rendered = 0u;
    size_t n_skipped = 0u;
    bool in_skipped = false;

    if (skip_doc != NULL)
        skipped = nd_json_get(nd_json_root(skip_doc), "skipped");
    if (skipped == NULL) {
        CHECK(false, "the skip file parses for the --list comparison");
        nd_json_free(skip_doc);
        return;
    }

    if (!make_out_dir(path, sizeof path) ||
        nd_strlcat(path, "/list.txt", sizeof path) >= sizeof path) {
        CHECK(false, "a place to capture --list");
        nd_json_free(skip_doc);
        return;
    }

    argv[0] = g_shoot;
    argv[1] = (char *)"--list";
    argv[2] = NULL;
    CHECK_INT(shoot_capture(argv, path), 0, "--list exits 0 with stdout captured");

    f = fopen(path, "r");
    if (f == NULL) {
        CHECK(false, "the captured --list output can be read");
        nd_json_free(skip_doc);
        return;
    }
    while (fgets(line, (int)sizeof line, f) != NULL) {
        char *name;

        if (strncmp(line, "rendered (", 10u) == 0) {
            in_skipped = false;
            continue;
        }
        if (strncmp(line, "skipped (", 9u) == 0) {
            in_skipped = true;
            continue;
        }
        if (line[0] != ' ')
            continue;

        name = list_line_name(line);
        if (name[0] == '\0')
            continue;

        if (in_skipped) {
            n_skipped++;
            if (frame_named(skipped, name) == NULL) {
                g_failures++;
                fprintf(stderr, "FAIL --list calls %s skipped; the run did not\n", name);
            }
            g_checks++;
        } else {
            n_rendered++;
            if (frame_named(ours, name) == NULL) {
                g_failures++;
                fprintf(stderr, "FAIL --list claims %s is rendered; the run did not write it\n",
                        name);
            }
            g_checks++;
        }
    }
    (void)fclose(f);

    /* Both directions: a name the run produced and --list never mentions
     * would pass every check above. */
    CHECK_INT(n_rendered, nd_json_len(ours), "--list names exactly the frames the run rendered");
    CHECK_INT(n_skipped, nd_json_len(skipped), "--list names exactly the frames the run skipped");

    /* The whole point of the exercise: CubeBench is a real port, so its frame
     * is on the rendered side and NOT in the skip file. */
    CHECK(frame_named(ours, "eng-cubebench") != NULL, "eng-cubebench is rendered");
    CHECK(frame_named(skipped, "eng-cubebench") == NULL, "eng-cubebench is not also skipped");

    nd_json_free(skip_doc);
}

/* ------------------------------------------------------------------ *
 * 5. The command line
 * ------------------------------------------------------------------ */

static void test_cli(void)
{
    char *argv[3];

    argv[0] = g_shoot;
    argv[1] = (char *)"--list";
    argv[2] = NULL;
    CHECK_INT(run_shoot(argv), 0, "--list exits 0");

    argv[1] = (char *)"--help";
    CHECK_INT(run_shoot(argv), 0, "--help exits 0");

    /* Refusing rather than defaulting: a capture written somewhere the caller
     * did not name is worse than no capture. */
    argv[1] = NULL;
    CHECK_INT(run_shoot(argv), 2, "no --out is a usage error");

    argv[1] = (char *)"--nonsense";
    argv[2] = NULL;
    CHECK_INT(run_shoot(argv), 2, "an unknown flag is a usage error");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    char a_dir[ND_PATH_MAX];
    char b_dir[ND_PATH_MAX];
    nd_json_doc *golden_doc = NULL;
    nd_json_doc *a_doc = NULL;
    nd_json_doc *b_doc = NULL;
    const nd_json_val *golden;
    const nd_json_val *ours;

    if (!find_inputs()) {
        printf("test_shoot: NEODCT_GOLDEN is not set; nothing to compare against\n");
        return 1;
    }
    if (!path_exists(g_shoot)) {
        printf("test_shoot: %s does not exist -- run `make` first\n", g_shoot);
        return 1;
    }

    /* nd-shoot stages its own /NeoDCT root; the Makefile's shared NEODCT_ROOT
     * is an empty scratch directory and would give it a phone with no fonts.
     * Point it at the overlay explicitly and take the root out of the way. */
    if (setenv("NEODCT_OVERLAY", g_overlay, 1) != 0)
        return 1;
    (void)unsetenv(ND_ENV_ROOT);

    if (!make_out_dir(a_dir, sizeof a_dir) || !make_out_dir(b_dir, sizeof b_dir)) {
        printf("test_shoot: cannot make an output directory\n");
        return 1;
    }

    CHECK_INT(shoot_into(a_dir), 0, "nd-shoot run 1 exits 0");
    CHECK_INT(shoot_into(b_dir), 0, "nd-shoot run 2 exits 0");

    golden_doc = load_json(g_golden, "manifest.json");
    a_doc = load_json(a_dir, "manifest.json");
    b_doc = load_json(b_dir, "manifest.json");
    if (golden_doc == NULL || a_doc == NULL || b_doc == NULL) {
        CHECK(false, "all three manifests parse");
        goto done;
    }

    golden = nd_json_get(nd_json_root(golden_doc), "frames");
    ours = nd_json_get(nd_json_root(a_doc), "frames");
    if (golden == NULL || ours == NULL) {
        CHECK(false, "both manifests have a frames array");
        goto done;
    }

    test_manifest_shape(a_doc);
    test_frames_sorted(ours);
    test_against_golden(a_dir, ours, golden);
    test_skip_list(a_dir, ours, golden);
    test_list_matches_run(a_dir, ours);
    test_determinism(a_dir, b_dir, ours);
    test_cli();

done:
    nd_json_free(b_doc);
    nd_json_free(a_doc);
    nd_json_free(golden_doc);
    drop_dirs();
    printf("test_shoot: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
