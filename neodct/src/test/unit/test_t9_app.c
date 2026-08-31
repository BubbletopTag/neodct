/* test_t9_app.c -- T9 survives the process boundary, or it does not exist.
 *
 * ============ WHAT THIS FILE CLAIMS ============
 *
 * The engine, the dictionary, the predictive widget and the mode indicator
 * were all finished and green long before this file existed -- test_t9_engine
 * (108 checks), test_t9_dict (587) and test_widgets_text (2163) all passed
 * while T9 DID NOT WORK ON THE PHONE AT ALL. Every one of them sets
 * ui.has_matrix_keypad by hand, and the bug was that nothing ever set it for
 * real inside an app.
 *
 * So this test refuses to set it by hand. It builds a core, launches a real
 * child through the real nd_proc_launch_app(), and reads back what the child
 * saw. Three launches:
 *
 *   1. a core with no matrix          -> the app must NOT do T9 (QEMU: the
 *                                        dev keyboard has real letters)
 *   2. a core with the matrix         -> the app MUST do T9, on the strength
 *                                        of NEODCT_KEYPAD_MATRIX alone
 *   3. a core with no matrix, but the
 *      developer override set         -> T9 on, which is how it is exercised
 *                                        on a keyboard
 *
 * ============ WHY THE CHILD TYPES ============
 *
 * The flag is a means, not the end. What broke was that a keypad press typed
 * nothing: nd_textinput.c falls through to nd_key_dev_char() when the flag is
 * false, and that table (nd_keycodes.c:57) HAS NO DIGITS IN IT. So the child
 * runs real keypad codes through a real nd_textinput and reports the string
 * that came out. "abc" appearing in case 2 and "" in case 1 is the whole
 * claim, stated in the only terms the owner cares about.
 */

#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "platform_test.h"

#define FONT_REL     "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"
#define T9_APP_DIR   "/NeoDCT/User/testapps/T9App"
#define T9_REPORT    "/NeoDCT/User/t9app-report.txt"
#define REPORT_BYTES 4096

static char g_golden[ND_PATH_MAX];
static char g_neodct[ND_PATH_MAX];
static char g_stage[ND_PATH_MAX];
static char g_bindir[ND_PATH_MAX];
static int g_stage_step;

#define STAGE_FAIL(n)       \
    do {                    \
        g_stage_step = (n); \
        return false;       \
    } while (0)

/* ------------------------------------------------------------------ *
 * Finding and staging the tree -- the same walk test_svc.c does
 * ------------------------------------------------------------------ */

static bool file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static bool resolve_neodct_dir(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_GOLDEN");
    char base[ND_PATH_MAX];
    char *cut;

    if (env != NULL && env[0] != '\0')
        (void)nd_strlcpy(g_golden, env, sizeof g_golden);
    else if (file_exists("../tests/golden/manifest.json"))
        (void)nd_strlcpy(g_golden, "../tests/golden", sizeof g_golden);
    else if (file_exists("neodct/tests/golden/manifest.json"))
        (void)nd_strlcpy(g_golden, "neodct/tests/golden", sizeof g_golden);
    else
        return false;

    (void)snprintf(base, sizeof base, "%.480s", g_golden);
    cut = strrchr(base, '/');
    if (cut != NULL)
        *cut = '\0';
    cut = strrchr(base, '/');
    if (cut != NULL)
        *cut = '\0';
    return nd_strlcpy(out, base, sz) < sz;
}

/* build/<variant>/test/test_t9_app -> build/<variant>/bin, so an ASan run
 * drives the ASan nd-apprun. */
static bool resolve_bindir(char *out, size_t sz)
{
    char exe[ND_PATH_MAX];
    char cand[ND_PATH_MAX];
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
    if (nd_snprintf(cand, sizeof cand, "%s/../bin", exe) != ND_OK)
        return false;
    return nd_strlcpy(out, cand, sz) < sz;
}

static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    char cand[ND_PATH_MAX];

    if (env != NULL && env[0] != '\0')
        return nd_strlcpy(out, env, sz) < sz;
    if (nd_snprintf(cand, sizeof cand, "%.400s/" FONT_REL, g_neodct) == ND_OK && file_exists(cand))
        return nd_strlcpy(out, cand, sz) < sz;
    if (file_exists("../" FONT_REL))
        return nd_strlcpy(out, "../" FONT_REL, sz) < sz;
    if (file_exists(ND_PATH_FONT))
        return nd_strlcpy(out, ND_PATH_FONT, sz) < sz;
    return false;
}

static bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[65536];
    size_t n;

    if (in == NULL)
        return false;
    out = fopen(dst, "wb");
    if (out == NULL) {
        (void)fclose(in);
        return false;
    }
    while ((n = fread(buf, 1u, sizeof buf, in)) > 0u) {
        if (fwrite(buf, 1u, n, out) != n) {
            (void)fclose(in);
            (void)fclose(out);
            return false;
        }
    }
    (void)fclose(in);
    (void)fclose(out);
    return chmod(dst, 0755) == 0;
}

static bool stage_app(const char *built, const char *virtual_dir)
{
    char dst[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];

    if (nd_mkdir_p(virtual_dir, 0755u) != ND_OK)
        return false;
    /* By hand: nd_path_join() RESOLVES, and resolving an already-resolved
     * path prefixes ND_ROOT twice. */
    if (nd_snprintf(dst, sizeof dst, "%s/%s", virtual_dir, ND_APP_SO_NAME) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, dst) != ND_OK)
        return false;
    return copy_file(built, resolved);
}

/* /NeoDCT/User belongs to ndusr on a phone -- S00userdata takes ownership of
 * it on the first boot -- and nd_proc_launch_app() now drops an ordinary app
 * to ndusr. A fixture that creates the directory root-owned therefore gives
 * the app a partition it cannot write, which is not what any phone looks
 * like, and the failure is indirect: the app exits non-zero and the test
 * waits for a reply that is never sent.
 *
 * Best-effort on purpose. It needs root to chown, and on a machine without
 * root the drop does not happen either -- so the case where this matters and
 * the case where it works are the same case. */
static void stage_user_owner(void)
{
    struct passwd *pw;
    char resolved[ND_PATH_MAX];

    if (geteuid() != 0u)
        return;
    pw = getpwnam(ND_PRIV_USER);
    if (pw == NULL)
        return;
    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_USER) != ND_OK)
        return;
    /* Assigned rather than (void)cast: glibc marks chown warn_unused_result
     * and -Werror rejects the cast. Nothing is done with it -- a fixture
     * that cannot chown is a fixture on a machine where the drop will not
     * happen either. */
    if (chown(resolved, pw->pw_uid, pw->pw_gid) != 0)
        return;
}

static bool stage_root(void)
{
    char overlay[ND_PATH_MAX];
    char tmpl[ND_PATH_MAX];
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    char built[ND_PATH_MAX];
    const char *tmp = getenv("TMPDIR");

    if (!resolve_neodct_dir(g_neodct, sizeof g_neodct))
        STAGE_FAIL(1);
    if (nd_snprintf(overlay, sizeof overlay, "%s/overlay", g_neodct) != ND_OK)
        STAGE_FAIL(2);
    if (!resolve_bindir(g_bindir, sizeof g_bindir))
        STAGE_FAIL(3);

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndt9-XXXXXX", tmp) != ND_OK)
        STAGE_FAIL(4);
    if (mkdtemp(tmpl) == NULL)
        STAGE_FAIL(5);
    /* 0711 -- see test_proc.c's stage_root(). A dropped app cannot traverse
     * a 0700 fixture to reach its own app.so. */
    if (chmod(tmpl, 0711) != 0)
        STAGE_FAIL(5);
    (void)nd_strlcpy(g_stage, tmpl, sizeof g_stage);

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        STAGE_FAIL(6);
    (void)mkdir(neodct, 0755);
    if (nd_snprintf(sys_link, sizeof sys_link, "%s/System", neodct) != ND_OK)
        STAGE_FAIL(7);
    if (nd_snprintf(sys_target, sizeof sys_target, "%s/NeoDCT/System", overlay) != ND_OK)
        STAGE_FAIL(8);
    if (symlink(sys_target, sys_link) != 0 && errno != EEXIST)
        STAGE_FAIL(9);
    if (nd_path_set_root(g_stage) != ND_OK)
        STAGE_FAIL(10);
    if (nd_mkdir_p(ND_PATH_USER, 0755u) != ND_OK)
        STAGE_FAIL(11);
    stage_user_owner();

    if (nd_snprintf(built, sizeof built, "%s/../test/apps/T9App/app.so", g_bindir) != ND_OK)
        STAGE_FAIL(12);
    if (!file_exists(built)) {
        fprintf(stderr, "test_t9_app: %s is missing; run `make test`\n", built);
        STAGE_FAIL(13);
    }
    return stage_app(built, T9_APP_DIR);
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
    if (g_stage[0] != '\0')
        (void)nftw(g_stage, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    g_stage[0] = '\0';
}

/* ------------------------------------------------------------------ *
 * A core, exactly as far as nd_proc_launch_app() needs one
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
    nd_input *input;
    int write_fd;
} core_fixture;

static bool core_init(core_fixture *fx)
{
    char path[ND_PATH_MAX];
    int fds[2];

    memset(fx, 0, sizeof *fx);
    fx->write_fd = -1;

    if (!resolve_font(path, sizeof path))
        return false;
    fx->font_s = nd_font_load(path, 14);
    fx->font_md = nd_font_load(path, 18);
    fx->font_n = nd_font_load(path, 20);
    fx->font_xl = nd_font_load(path, 24);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL)
        return false;

    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    fx->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL || fx->scratch == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;
    if (pipe(fds) != 0)
        return false;
    if (nd_input_open_fd(&fx->input, fds[0]) != ND_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }
    fx->write_fd = fds[1];

    fx->ui.w = ND_UI_W;
    fx->ui.h = ND_UI_H;
    fx->ui.softkey_h = ND_SOFTKEY_H;
    fx->ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    fx->ui.canvas = fx->canvas;
    fx->ui.scratch = fx->scratch;
    fx->ui.draw = &fx->draw;
    fx->ui.fb = NULL;
    fx->ui.font_s = fx->font_s;
    fx->ui.font_md = fx->font_md;
    fx->ui.font_n = fx->font_n;
    fx->ui.font_xl = fx->font_xl;
    fx->ui.input = fx->input;
    fx->ui.keypad_fd = nd_input_fd(fx->input);
    fx->ui.softkey_exists = true;
    fx->ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    return fx->ui.image_cache != NULL;
}

static void core_free(core_fixture *fx)
{
    nd_imgcache_free(fx->ui.image_cache);
    if (fx->input != NULL)
        nd_input_close(fx->input);
    if (fx->write_fd >= 0)
        (void)close(fx->write_fd);
    nd_image_free(fx->canvas);
    nd_image_free(fx->scratch);
    nd_font_free(fx->font_s);
    nd_font_free(fx->font_md);
    nd_font_free(fx->font_n);
    nd_font_free(fx->font_xl);
    memset(fx, 0, sizeof *fx);
}

/* ------------------------------------------------------------------ *
 * key=value report reading -- the same shape test_svc.c uses
 * ------------------------------------------------------------------ */

static bool report_get(const char *report, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = report;

    while (p != NULL && *p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = len - klen - 1u;

            if (vlen >= out_sz)
                vlen = out_sz - 1u;
            memcpy(out, p + klen + 1u, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = (eol != NULL) ? eol + 1 : NULL;
    }
    return false;
}

static void check_kv(const char *report, const char *key, const char *want)
{
    char got[256];

    if (!report_get(report, key, got, sizeof got)) {
        g_checks++;
        g_failures++;
        fprintf(stderr, "FAIL %s:%d  the child never reported \"%s\"\n", __FILE__, __LINE__, key);
        return;
    }
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL %s:%d  %s: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, key, got,
                want);
    }
}

/* ------------------------------------------------------------------ *
 * The launches
 * ------------------------------------------------------------------ */

static void app_entry_for(nd_app_entry *e)
{
    memset(e, 0, sizeof *e);
    (void)nd_strlcpy(e->name, "T9App", sizeof e->name);
    (void)nd_strlcpy(e->path, T9_APP_DIR, sizeof e->path);
    (void)nd_strlcpy(e->exec, ND_APP_SO_NAME, sizeof e->exec);
    e->id = 9995;
}

/* Launch once and hand back the report. Returns false when the child wrote
 * nothing, which means it faulted before it could speak. */
static bool launch(core_fixture *fx, char *report, size_t report_sz)
{
    nd_app_entry app;
    nd_crash_info crash;
    char resolved[ND_PATH_MAX];

    /* A stale report from the previous case would pass every check in this
     * one. Remove it and make the child prove itself again. */
    if (nd_path_resolve(resolved, sizeof resolved, T9_REPORT) == ND_OK)
        (void)unlink(resolved);

    app_entry_for(&app);
    memset(&crash, 0, sizeof crash);
    CHECK_INT(nd_proc_launch_app(&fx->ui, &app, NULL, NULL, &crash), ND_OK);
    CHECK(!crash.from_signal);
    CHECK_INT(crash.exit_status, 0);

    if (pt_read_text(T9_REPORT, report, report_sz) == (size_t)-1) {
        CHECK(false);
        fprintf(stderr, "test_t9_app: the child wrote no report\n");
        return false;
    }
    check_kv(report, "done", "1");
    return true;
}

/* Every case: the app's OWN input is still a pipe with no matrix in it. The
 * flag is propagated, never re-detected -- if this ever changes, the fix has
 * been undone and the propagation is doing nothing. */
static void check_boundary_intact(const char *report)
{
    check_kv(report, "input_has_matrix", "0");
}

/* ------------------------------------------------------------------ *
 * 1. No matrix: QEMU and every dev board. T9 stays off.
 * ------------------------------------------------------------------ */

static void test_no_matrix_means_no_t9(core_fixture *fx)
{
    char report[REPORT_BYTES];

    fx->ui.has_matrix_keypad = false;
    (void)unsetenv(ND_ENV_T9);
    if (!launch(fx, report, sizeof report))
        return;

    check_boundary_intact(report);
    check_kv(report, "env_matrix", "");
    check_kv(report, "has_matrix_keypad", "0");

    /* THE OLD BEHAVIOUR ON THE PHONE, pinned here so it cannot come back by
     * accident. nd_key_dev_char() falls through to nd_key_digit_char(), so a
     * keypad press types its own DIGIT and nothing else: 2,2,2,3 gives
     * "2223" and not "cd". No letters, no cycling, and # (code 43) is not a
     * digit so it types nothing and switches nothing. That is what the owner
     * was looking at. */
    check_kv(report, "multitap_text", "2223");
    check_kv(report, "upper_text", "222");
    check_kv(report, "numeric_text", "23");
    check_kv(report, "word_text", "4663");

    /* The engine was never even consulted, so it is still in its start mode. */
    check_kv(report, "multitap_mode", "abc");
    check_kv(report, "upper_mode", "abc");
}

/* ------------------------------------------------------------------ *
 * 2. The phone. This is the case that was broken.
 * ------------------------------------------------------------------ */

static void test_matrix_reaches_the_app(core_fixture *fx)
{
    char report[REPORT_BYTES];

    fx->ui.has_matrix_keypad = true;
    (void)unsetenv(ND_ENV_T9);
    if (!launch(fx, report, sizeof report))
        return;

    check_boundary_intact(report);
    check_kv(report, "env_matrix", "1");
    check_kv(report, "has_matrix_keypad", "1");

    /* MULTI-TAP. 2,2,2 cycles a->b->c and 3 commits it and starts 'd'. */
    check_kv(report, "multitap_init", "1");
    check_kv(report, "multitap_text", "cd");
    check_kv(report, "multitap_mode", "abc");

    /* # MODE SWITCHING, seen from inside the app. abc -> ABC. */
    check_kv(report, "upper_text", "C");
    check_kv(report, "upper_mode", "ABC");

    /* ...and two more reach 123, where a digit key is the digit itself. */
    check_kv(report, "numeric_text", "23");
    check_kv(report, "numeric_mode", "123");

    /* PREDICTIVE, with the shipped dictionary really opened from inside the
     * app process: 4,6,6,3 comes back as a WORD, not as four taps' worth of
     * letters. "home" and not "good" -- t9.dict is ordered by digit key and
     * then by frequency, and home is the commoner of the two. Asserting the
     * exact word is the point: a fallback that echoed the digits would also
     * be four characters long. */
    check_kv(report, "word_mode", "word");
    check_kv(report, "word_text", "home");
}

/* ------------------------------------------------------------------ *
 * 3. NEODCT_T9, the developer override, on a core with no matrix
 * ------------------------------------------------------------------ */

static void test_the_override_reaches_the_app(core_fixture *fx)
{
    char report[REPORT_BYTES];

    fx->ui.has_matrix_keypad = false;
    (void)setenv(ND_ENV_T9, "1", 1);

    /* The core folds the override into its own flag on init. This fixture
     * builds its nd_ui by hand and never calls nd_ui_init(), so it stands in
     * for that here -- the propagation under test is core flag -> child. */
    fx->ui.has_matrix_keypad = true;

    if (!launch(fx, report, sizeof report))
        goto out;

    check_boundary_intact(report);
    check_kv(report, "env_t9", "1");
    check_kv(report, "has_matrix_keypad", "1");
    check_kv(report, "multitap_text", "cd");

out:
    (void)unsetenv(ND_ENV_T9);
}

/* NEODCT_T9=0 is the other half of the override and must WIN over a real
 * matrix, or a developer cannot turn the thing off. */
static void test_the_override_can_force_it_off(core_fixture *fx)
{
    char report[REPORT_BYTES];

    fx->ui.has_matrix_keypad = true;
    (void)setenv(ND_ENV_T9, "0", 1);

    if (!launch(fx, report, sizeof report))
        goto out;

    check_kv(report, "env_matrix", "1"); /* the core still says there is one */
    check_kv(report, "env_t9", "0");
    check_kv(report, "has_matrix_keypad", "0"); /* ...and the override wins */
    check_kv(report, "multitap_text", "2223");  /* straight back to digits */

out:
    (void)unsetenv(ND_ENV_T9);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    core_fixture fx;

    /* This test stands in for nd-core, so it does what nd_main.c does: a key
     * written down the app channel while the child is mid-exit raises
     * SIGPIPE, and the default disposition would kill it. */
    (void)signal(SIGPIPE, SIG_IGN);

    if (!stage_root()) {
        fprintf(stderr, "test_t9_app: cannot stage a root (step %d); skipping\n", g_stage_step);
        return 0;
    }
    if (!core_init(&fx)) {
        fprintf(stderr, "test_t9_app: cannot build a core fixture; skipping\n");
        core_free(&fx);
        drop_stage();
        return 0;
    }

    test_no_matrix_means_no_t9(&fx);
    test_matrix_reaches_the_app(&fx);
    test_the_override_reaches_the_app(&fx);
    test_the_override_can_force_it_off(&fx);

    core_free(&fx);
    drop_stage();
    printf("test_t9_app: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
