/* test_proc.c -- the process boundary, proved rather than asserted.
 *
 * The whole point of fork() + execve() into nd-apprun is this claim:
 *
 *     A NULL DEREFERENCE IN AN APP KILLS THE APP AND NOT THE PHONE.
 *
 * That claim is worth nothing as prose. So this test launches an app whose
 * entire job is to dereference NULL, and checks that
 *
 *   - the core is still running afterwards (it goes on to launch another app
 *     and finish the suite, which is the only proof that means anything);
 *   - waitpid reported WIFSIGNALED with SIGSEGV;
 *   - the child's own report -- si_code and the faulting address, which only
 *     exist on that side of the boundary -- arrived down the crash pipe;
 *   - /NeoDCT/User/logs/crash.log gained a report;
 *   - and the CRASH SCREEN WAS ACTUALLY DRAWN: CRASH.jpg over the frame, the
 *     black summary strip at the top with text in it, and "Continue" on the
 *     softkey bar.
 *
 * The same app also aborts and exits non-zero on demand, because the launcher
 * has to classify three different deaths and a test that only covers SIGSEGV
 * leaves two branches unvisited.
 *
 * ============ DRIVING A BLOCKING SCREEN ============
 *
 * The crash screen and the stub app's dialog both flush pending input before
 * their first draw, so a pre-written key script is eaten before it is read.
 * The way through is the one test_widgets_lists.c found: press a key and DO
 * NOT release it, with that key in the repeat set. The flush consumes the
 * press, held state survives it, and the synthesised repeat arrives after the
 * screen is up -- which doubles as proof that the flush ran.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_crash.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "platform_test.h"

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* Where the staged apps live. Under User, not System: System is a symlink
 * onto neodct/overlay and nothing may be written there. */
#define CRASH_APP_DIR "/NeoDCT/User/testapps/CrashApp"
#define STUB_APP_DIR  "/NeoDCT/User/testapps/StubApp"

static char g_golden[ND_PATH_MAX];
static char g_neodct[ND_PATH_MAX];
static char g_stage[ND_PATH_MAX];
static char g_bindir[ND_PATH_MAX];
/* Which step of stage_root() gave up, so a skipped test says why rather than
 * leaving somebody to bisect a shell script. */
static int g_stage_step;
#define STAGE_FAIL(n)       \
    do {                    \
        g_stage_step = (n); \
        return false;       \
    } while (0)

/* ------------------------------------------------------------------ *
 * Finding the tree
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
    cut = strrchr(base, '/'); /* .../neodct/tests */
    if (cut != NULL)
        *cut = '\0';
    cut = strrchr(base, '/'); /* .../neodct       */
    if (cut != NULL)
        *cut = '\0';
    return nd_strlcpy(out, base, sz) < sz;
}

/* build/<variant>/test/test_proc -> build/<variant>/bin, so an ASan run drives
 * the ASan nd-apprun. Same reasoning as nd_proc.c's own search. */
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

/* ------------------------------------------------------------------ *
 * Staging
 * ------------------------------------------------------------------ */

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

/* Copy one .so from the build tree to a VIRTUAL app directory under ND_ROOT. */
static bool stage_app(const char *built, const char *virtual_dir)
{
    char dst[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];

    if (nd_mkdir_p(virtual_dir, 0755u) != ND_OK)
        return false;
    /* Built by hand rather than with nd_path_join(), which RESOLVES its
     * result -- resolving an already-resolved path prefixes ND_ROOT twice. */
    if (nd_snprintf(dst, sizeof dst, "%s/%s", virtual_dir, ND_APP_SO_NAME) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, dst) != ND_OK)
        return false;
    return copy_file(built, resolved);
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
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndproc-XXXXXX", tmp) != ND_OK)
        STAGE_FAIL(4);
    if (mkdtemp(tmpl) == NULL)
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

    if (nd_snprintf(built, sizeof built, "%s/../test/apps/CrashApp/app.so", g_bindir) != ND_OK)
        STAGE_FAIL(12);
    if (!file_exists(built)) {
        fprintf(stderr, "test_proc: %s is missing; run `make test`\n", built);
        STAGE_FAIL(13);
    }
    if (!stage_app(built, CRASH_APP_DIR))
        STAGE_FAIL(14);

    if (nd_snprintf(built, sizeof built, "%s/../apps/Stub/app.so", g_bindir) != ND_OK)
        STAGE_FAIL(15);
    if (!file_exists(built)) {
        fprintf(stderr, "test_proc: %s is missing\n", built);
        STAGE_FAIL(16);
    }
    return stage_app(built, STUB_APP_DIR);
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
 * A UI with nothing behind it but memory, plus a key channel
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
} fixture;

static bool fx_init(fixture *fx)
{
    static const int32_t REPEATERS[3] = {ND_KEY_ENTER, ND_KEY_CLEAR, 46};
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
    (void)nd_input_set_repeat_codes(fx->input, REPEATERS, ND_ARRAY_LEN(REPEATERS));
    /* Long enough that a 0.0 or 0.01 flush poll finds the channel idle, short
     * enough that the suite does not sit here. */
    nd_input_set_repeat(fx->input, 0.20, 0.05);

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
    /* This fixture builds an nd_ui by hand instead of calling nd_ui_init(),
     * so it also has to set the lazy flag: writing the value alone would be
     * overwritten the first time nd_ui_engineering_mode() went to settings. */
    fx->ui.home_.engineering_mode = true;
    fx->ui.home_.eng_mode_ready = true;
    fx->ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    return fx->ui.image_cache != NULL;
}

static void fx_free(fixture *fx)
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

static void write_key(int fd, uint16_t code, int32_t value)
{
    struct input_event ev[2];

    memset(ev, 0, sizeof ev);
    ev[0].type = EV_KEY;
    ev[0].code = code;
    ev[0].value = value;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    (void)!write(fd, ev, sizeof ev);
}

static void release_all(fixture *fx)
{
    write_key(fx->write_fd, (uint16_t)ND_KEY_ENTER, 0);
    write_key(fx->write_fd, (uint16_t)ND_KEY_CLEAR, 0);
    while (nd_input_read_key(fx->input, 0.05) != ND_KEY_NONE) {}
}

static void app_entry_for(nd_app_entry *e, const char *dir, const char *name)
{
    memset(e, 0, sizeof *e);
    (void)nd_strlcpy(e->name, name, sizeof e->name);
    (void)nd_strlcpy(e->path, dir, sizeof e->path);
    (void)nd_strlcpy(e->exec, ND_APP_SO_NAME, sizeof e->exec);
    e->id = 9997;
}

/* ------------------------------------------------------------------ *
 * Pixel helpers
 * ------------------------------------------------------------------ */

static bool px_lit(const nd_image *img, int32_t x, int32_t y)
{
    nd_color c = nd_image_get_px(img, x, y);

    return c.r != 0u || c.g != 0u || c.b != 0u;
}

static size_t lit_count(const nd_image *img, int32_t y0, int32_t y1)
{
    size_t n = 0u;
    int32_t x;
    int32_t y;

    for (y = y0; y <= y1; y++) {
        for (x = 0; x < img->w; x++) {
            if (px_lit(img, x, y))
                n++;
        }
    }
    return n;
}

static size_t crash_log_size(void)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_CRASH_LOG) != ND_OK)
        return 0u;
    if (stat(resolved, &st) != 0)
        return 0u;
    return (size_t)st.st_size;
}

static bool crash_log_contains(const char *needle)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    char line[512];
    bool found = false;

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_CRASH_LOG) != ND_OK)
        return false;
    f = fopen(resolved, "r");
    if (f == NULL)
        return false;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = true;
            break;
        }
    }
    (void)fclose(f);
    return found;
}

/* ------------------------------------------------------------------ *
 * nd_proc_spawn / wait / terminate on their own
 * ------------------------------------------------------------------ */

/* spec.new_session must actually call setsid(), not merely be stored.
 *
 * This is the flag that stops a kill(-pgid) reaching back into the core.
 * RemoteShell's _owns() docstring records what happens without it: stale
 * pid files, a boot that reused those numbers, and Remote Shell killing
 * the process group they now belonged to -- its own launcher. No UI, and
 * the serial log stopped mid-boot.
 *
 * `ps -o sid=` is not portable enough to rely on, so the child reports its
 * own session id: getsid(0) differs from the parent's exactly when setsid()
 * ran, which is the property that matters and the only one worth asserting.
 */
static void test_spawn_new_session(void)
{
    static const char *const SH_ARGV[] = {"/bin/sh", "-c", "exec ps -o sid= -p $$", NULL};
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int pipefd[2];
    char buf[64];
    ssize_t got;
    long child_sid = -1;
    long our_sid = (long)getsid(0);

    if (pipe(pipefd) != 0) {
        CHECK(pipefd[0] >= 0); /* no pipe: nothing to observe the child with */
        return;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = SH_ARGV;
    spec.owner = ND_OWNER_SYSTEM;
    spec.new_session = true;
    spec.fds[0].child_fd = 1; /* the child's stdout is our pipe */
    spec.fds[0].our_fd = pipefd[1];
    spec.n_fds = 1u;

    CHECK_INT(nd_proc_spawn("/bin/sh", &spec, &pid), ND_OK);
    (void)close(pipefd[1]);
    if (pid > 0) {
        got = read(pipefd[0], buf, sizeof buf - 1u);
        if (got > 0) {
            buf[got] = '\0';
            child_sid = strtol(buf, NULL, 10);
        }
        CHECK_INT(nd_proc_wait(pid, 5.0, &st), ND_OK);
    }
    (void)close(pipefd[0]);

    if (child_sid <= 0) {
        /* No usable ps on this host: the flag cannot be observed from here,
         * and a test that silently passes is worse than one that says so. */
        fprintf(stderr, "SKIP new_session: ps gave no session id\n");
        return;
    }
    /* Its own session, and the leader of it. */
    CHECK(child_sid != our_sid);
    CHECK(child_sid == (long)pid);
}

static void test_spawn_same_session(void)
{
    static const char *const SH_ARGV[] = {"/bin/sh", "-c", "exec ps -o sid= -p $$", NULL};
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int pipefd[2];
    char buf[64];
    ssize_t got;
    long child_sid = -1;
    long our_sid = (long)getsid(0);

    if (pipe(pipefd) != 0) {
        CHECK(pipefd[0] >= 0);
        return;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = SH_ARGV;
    spec.owner = ND_OWNER_SYSTEM;
    /* new_session left false: an APP must stay in the core's session, so
     * that nd_proc_terminate()'s single-pid signal is the whole story and
     * an orphaned app cannot outlive the session leader. */
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = pipefd[1];
    spec.n_fds = 1u;

    CHECK_INT(nd_proc_spawn("/bin/sh", &spec, &pid), ND_OK);
    (void)close(pipefd[1]);
    if (pid > 0) {
        got = read(pipefd[0], buf, sizeof buf - 1u);
        if (got > 0) {
            buf[got] = '\0';
            child_sid = strtol(buf, NULL, 10);
        }
        CHECK_INT(nd_proc_wait(pid, 5.0, &st), ND_OK);
    }
    (void)close(pipefd[0]);

    if (child_sid <= 0) {
        fprintf(stderr, "SKIP same_session: ps gave no session id\n");
        return;
    }
    /* The default leaves the child in our session. */
    CHECK(child_sid == our_sid);
}

static void test_spawn_and_wait(void)
{
    static const char *const TRUE_ARGV[] = {"/bin/true", NULL};
    static const char *const SLEEP_ARGV[] = {"/bin/sleep", "30", NULL};
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;

    memset(&spec, 0, sizeof spec);
    spec.argv = TRUE_ARGV;
    spec.owner = ND_OWNER_SYSTEM;
    CHECK_INT(nd_proc_spawn("/bin/true", &spec, &pid), ND_OK);
    CHECK(pid > 0);
    CHECK_INT(nd_proc_wait(pid, 5.0, &st), ND_OK);
    CHECK(st.exited);
    CHECK_INT(st.exit_status, 0);
    CHECK(!st.signalled);

    /* An execve that fails is reported by the child exiting 127 -- the header
     * says so, and the number is what every shell reports for "not found". */
    spec.argv = TRUE_ARGV;
    CHECK_INT(nd_proc_spawn("/nonexistent/definitely-not-here", &spec, &pid), ND_OK);
    CHECK_INT(nd_proc_wait(pid, 5.0, &st), ND_OK);
    CHECK(st.exited);
    CHECK_INT(st.exit_status, 127);

    /* A child that is still running must time out rather than block. */
    spec.argv = SLEEP_ARGV;
    CHECK_INT(nd_proc_spawn("/bin/sleep", &spec, &pid), ND_OK);
    CHECK_INT(nd_proc_wait(pid, 0.0, &st), ND_ERR_TIMEOUT);
    CHECK_INT(nd_proc_terminate(pid, 1.0, &st), ND_OK);
    CHECK(st.signalled);
    CHECK_INT(st.signo, SIGTERM);

    CHECK_INT(nd_proc_spawn(NULL, &spec, &pid), ND_ERR_INVAL);
    CHECK_INT(nd_proc_wait(-1, 0.0, &st), ND_ERR_INVAL);
}

/* The reaper must not lose a status: it collects every child, so an explicit
 * waitpid afterwards would get ECHILD if the ring did not exist. */
static void test_reaper_keeps_the_status(void)
{
    static const char *const ARGV[] = {"/bin/sh", "-c", "exit 7", NULL};
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    struct timespec ts = {0, 250L * 1000L * 1000L};

    CHECK_INT(nd_proc_reaper_start(), ND_OK);
    CHECK_INT(nd_proc_reaper_start(), ND_OK); /* idempotent */

    memset(&spec, 0, sizeof spec);
    spec.argv = ARGV;
    spec.owner = ND_OWNER_TONE;
    CHECK_INT(nd_proc_spawn("/bin/sh", &spec, &pid), ND_OK);
    /* Give the handler time to reap it out from under us. */
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}

    CHECK_INT(nd_proc_wait(pid, 2.0, &st), ND_OK);
    CHECK(st.exited);
    CHECK_INT(st.exit_status, 7);

    nd_proc_reaper_stop();
}

/* ------------------------------------------------------------------ *
 * Launching an app
 * ------------------------------------------------------------------ */

/* mode: 0 clean, 1 SIGSEGV, 2 abort, 3 exit 3. Delivered through
 * open_message's argument, so argv plumbing is exercised too. */
static nd_err launch_crash_app(fixture *fx, int mode, nd_crash_info *info)
{
    nd_app_entry app;
    char arg[16];

    app_entry_for(&app, CRASH_APP_DIR, "Crash");
    (void)nd_snprintf(arg, sizeof arg, "%d", mode);

    /* Held, not tapped: the crash screen flushes input before its first draw,
     * so only a repeat can reach the wait behind it. */
    write_key(fx->write_fd, (uint16_t)ND_KEY_ENTER, 1);
    return nd_proc_launch_app(&fx->ui, &app, ND_APP_ENTRY_OPEN_MESSAGE, arg, info);
}

static void test_segfaulting_app_shows_the_crash_screen(void)
{
    fixture fx;
    nd_crash_info info;
    size_t before;
    size_t lit_body;

    if (!fx_init(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    before = crash_log_size();
    (void)nd_image_fill(fx.canvas, ND_BLACK);

    memset(&info, 0, sizeof info);
    CHECK_INT(launch_crash_app(&fx, 1, &info), ND_OK);
    release_all(&fx);

    /* 1. The child died of the signal, and we know which. */
    CHECK(info.from_signal);
    CHECK_INT(info.signo, SIGSEGV);
    /* 2. The report came from the CHILD, not from waitpid: si_code and the
     *    faulting address only exist on that side of the boundary. */
    CHECK(strstr(info.detail, "SIGSEGV") != NULL);
    CHECK(strstr(info.detail, ND_APP_ENTRY_OPEN_MESSAGE) != NULL);
    /* 3. It was written down, durably. */
    CHECK(crash_log_size() > before);
    CHECK(crash_log_contains("SIGSEGV"));
    CHECK(crash_log_contains("source: Crash"));
    /* 4. And the screen was drawn: CRASH.jpg fills the frame, so the body is
     *    overwhelmingly lit where a black canvas would be empty. */
    lit_body = lit_count(fx.canvas, 40, 140);
    CHECK(lit_body > (size_t)(ND_UI_W * 101) / 2u);
    /* 5. The summary strip is at the top, black, with white text in it. The
     *    text is drawn at y = 2, which is the ASCENDER line -- at 14 px the ink
     *    starts one row below it, so the scan starts at 2 and not at 3 only
     *    because a taller glyph could reach up. */
    CHECK(lit_count(fx.canvas, 2, 14) > 0u);

    fx_free(&fx);
}

static void test_abort_and_nonzero_exit_are_crashes_too(void)
{
    fixture fx;
    nd_crash_info info;

    if (!fx_init(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    memset(&info, 0, sizeof info);
    CHECK_INT(launch_crash_app(&fx, 2, &info), ND_OK);
    release_all(&fx);
    CHECK(info.from_signal);
    CHECK_INT(info.signo, SIGABRT);

    memset(&info, 0, sizeof info);
    CHECK_INT(launch_crash_app(&fx, 3, &info), ND_OK);
    release_all(&fx);
    CHECK(!info.from_signal);
    CHECK_INT(info.exit_status, 3);

    fx_free(&fx);
}

static void test_clean_exit_draws_nothing(void)
{
    fixture fx;
    nd_crash_info info;
    size_t before;

    if (!fx_init(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    (void)nd_image_fill(fx.canvas, ND_BLACK);
    before = crash_log_size();

    memset(&info, 0, sizeof info);
    CHECK_INT(launch_crash_app(&fx, 0, &info), ND_OK);
    release_all(&fx);

    CHECK(!info.from_signal);
    CHECK_INT(info.exit_status, 0);
    /* Normal return -> back to HOME, no screen of any kind, no log entry. */
    CHECK_INT(crash_log_size(), before);
    CHECK_INT(lit_count(fx.canvas, 0, ND_UI_H - 1), 0);

    fx_free(&fx);
}

/* THE ONE THAT MATTERS: after all of the above, the core is still here and can
 * still launch an app that draws. If the boundary leaked, this is where it
 * would show. */
static void test_the_core_survived_and_the_stub_app_runs(void)
{
    fixture fx;
    nd_app_entry app;
    nd_crash_info info;

    if (!fx_init(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    (void)nd_image_fill(fx.canvas, ND_BLACK);
    app_entry_for(&app, STUB_APP_DIR, "Clock");

    write_key(fx.write_fd, (uint16_t)ND_KEY_ENTER, 1);
    memset(&info, 0, sizeof info);
    CHECK_INT(nd_proc_launch_app(&fx.ui, &app, NULL, NULL, &info), ND_OK);
    release_all(&fx);

    CHECK(!info.from_signal);
    CHECK_INT(info.exit_status, 0);

    /* The stub app draws into its OWN canvas, in its own process, so the
     * core's canvas is untouched -- which is exactly the isolation being
     * claimed. The app is verified pixel-for-pixel where it can be seen:
     * nd-shoot renders app-clock against golden/app-clock.png. */
    CHECK_INT(lit_count(fx.canvas, 0, ND_UI_H - 1), 0);

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * The crash screen on its own, without a child
 * ------------------------------------------------------------------ */

static void test_crash_screen_geometry(void)
{
    fixture fx;
    int32_t th = 0;
    int32_t tw = 0;
    int32_t y;
    bool strip_has_ink = false;

    if (!fx_init(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    (void)nd_image_fill(fx.canvas, ND_RGB(9, 9, 9));
    nd_crash_draw_engineering(&fx.ui, "RuntimeError: example failure");

    nd_ui_text_size(&fx.ui, "RuntimeError: example failure", fx.ui.font_s, &tw, &th);
    /* The strip is (0, 0, W, th+4) filled black, then the text at (2, 2). */
    CHECK(th > 0);
    for (y = 2; y < th + 2 && y < ND_UI_H; y++) {
        if (lit_count(fx.canvas, y, y) > 0u)
            strip_has_ink = true;
    }
    CHECK(strip_has_ink);
    /* Row 0 is inside the black strip and above the text's ink line. */
    CHECK_INT(lit_count(fx.canvas, 0, 0), 0);
    /* CRASH.jpg is behind everything, so the body is not the grey we filled. */
    CHECK(lit_count(fx.canvas, 60, 120) > 0u);

    fx_free(&fx);
}

static void test_summary_is_capped_at_ninety(void)
{
    nd_crash_info info;
    char out[128];

    memset(&info, 0, sizeof info);
    memset(info.detail, 'x', 200u);
    info.detail[200] = '\0';
    CHECK_INT(nd_crash_summary(&info, out, sizeof out), 90);
    CHECK_INT(strlen(out), 90);
    CHECK_STR(out + 87, "...");

    memset(&info, 0, sizeof info);
    info.from_signal = true;
    info.signo = SIGSEGV;
    (void)nd_crash_summary(&info, out, sizeof out);
    CHECK(strstr(out, "SIGSEGV") != NULL);

    memset(&info, 0, sizeof info);
    info.exit_status = 5;
    (void)nd_crash_summary(&info, out, sizeof out);
    CHECK(strstr(out, "status 5") != NULL);

    /* A clean exit has no summary at all -- the Python's _exc_summary returns
     * None and the notice dialog then shows only its message. */
    memset(&info, 0, sizeof info);
    CHECK_INT(nd_crash_summary(&info, out, sizeof out), 0);
    CHECK_STR(out, "");
}

/* Rotation is checked by PLANTING an oversized log rather than by writing four
 * hundred reports: _rotate_if_needed() runs on every call, so a loop that
 * grows the file also rotates it partway through and leaves crash.log small
 * again -- the test would then be asserting on wherever the loop happened to
 * stop. One planted file and one report is the rule itself, and it keeps four
 * hundred [CRASH] lines out of the suite's output. */
static void test_crash_log_rotates(void)
{
    char cur[ND_PATH_MAX];
    char old[ND_PATH_MAX];
    nd_crash_info info;
    struct stat st;
    FILE *f;
    size_t i;

    CHECK_INT(nd_mkdir_p(ND_PATH_LOG_DIR, 0755u), ND_OK);
    CHECK_INT(nd_path_resolve(cur, sizeof cur, ND_PATH_CRASH_LOG), ND_OK);
    CHECK_INT(nd_path_resolve(old, sizeof old, ND_PATH_CRASH_LOG_1), ND_OK);
    (void)unlink(old);

    f = fopen(cur, "wb");
    if (f == NULL) {
        CHECK(false);
        return;
    }
    for (i = 0u; i < (size_t)ND_CRASH_LOG_MAX_BYTES + 1024u; i++)
        (void)fputc('x', f);
    (void)fclose(f);

    memset(&info, 0, sizeof info);
    info.from_signal = true;
    info.signo = SIGILL;
    CHECK(nd_crash_log("rotation", &info, NULL) != NULL);

    /* The oversized file became crash.log.1 ... */
    CHECK(stat(old, &st) == 0);
    CHECK(st.st_size > (off_t)ND_CRASH_LOG_MAX_BYTES);
    /* ... and the new report started a fresh crash.log. Total on disk is
     * therefore capped at 2 x 64 KiB, which is the whole point on 128 MB of
     * NAND. */
    CHECK(stat(cur, &st) == 0);
    CHECK(st.st_size > 0);
    CHECK(st.st_size < (off_t)ND_CRASH_LOG_MAX_BYTES);
    CHECK(crash_log_contains("source: rotation"));
}

int main(void)
{
    /* This test stands in for nd-core, so it has to do what nd-core does at
     * startup (nd_main.c). Sending a key down the app channel while the child
     * is mid-exit raises SIGPIPE, and the default disposition killed this
     * process with 141 before any assertion could run -- the crash-isolation
     * cases below deliberately kill their child, so they hit it every time.
     * See the contract on nd_input_channel_send() in nd_input.h. */
    (void)signal(SIGPIPE, SIG_IGN);

    if (!stage_root()) {
        fprintf(stderr, "test_proc: cannot stage a root (step %d); skipping\n", g_stage_step);
        return 0;
    }

    test_spawn_new_session();
    test_spawn_same_session();
    test_spawn_and_wait();
    test_reaper_keeps_the_status();
    test_summary_is_capped_at_ninety();
    test_crash_screen_geometry();
    test_segfaulting_app_shows_the_crash_screen();
    test_abort_and_nonzero_exit_are_crashes_too();
    test_clean_exit_draws_nothing();
    test_the_core_survived_and_the_stub_app_runs();
    test_crash_log_rotates();

    drop_stage();
    printf("test_proc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
