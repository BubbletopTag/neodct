/* test_svc.c -- the app -> core service channel, proved rather than asserted.
 *
 * nd_svc.h makes one claim and it is not a claim about a serialiser:
 *
 *     AN APP IN ITS OWN PROCESS CAN REACH THE CORE'S MODEM AND BATTERY,
 *     AND NOTHING ELSE OF THEM.
 *
 * So the centre of this file is test_a_real_child_reaches_the_core(): a real
 * app.so, dlopen()ed by the real nd-apprun, in a child forked and exec'd by
 * the real nd_proc_launch_app(), whose parent holds a real nd_modem and a
 * real nd_battery. The child writes down what it got; the parent compares it
 * against its OWN live services, so the case cannot pass by both sides
 * agreeing on a wrong answer.
 *
 * ============ THE LOOPBACK CASE PROVES THE THREAD ============
 *
 * test_loopback_over_a_live_server() opens a server, hands its socket to this
 * process's own client, and then makes an ordinary nd_svc_* call FROM THE
 * MAIN THREAD. The main thread blocks in recv() waiting for an answer that
 * only the server can produce. If the serving were done inline on the caller
 * -- which is the design this one deliberately does not have, because the
 * caller in production is the pump loop that scans the i2c key matrix -- this
 * case would deadlock and the suite would hang instead of failing. It passing
 * is the proof that the two halves are on different threads.
 *
 * ============ AND THE GOLDEN FRAMES ============
 *
 * test_a_direct_handle_wins() pins the rule every reference frame depends on:
 * when ui->modem is non-NULL the socket is not touched, even when one is
 * open and pointed at a server that would answer differently. nd-shoot runs
 * apps in-process with live services, so that is the path all 49 frames were
 * captured through and it must stay identical.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_clock.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_modem.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_proc.h"
#include "nd_storage.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "platform_test.h"

#define FONT_REL     "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"
#define SVC_APP_DIR  "/NeoDCT/User/testapps/SvcApp"
#define SVC_REPORT   "/NeoDCT/User/svcapp-report.txt"
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
 * Finding and staging the tree -- the same walk test_proc.c does
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

/* build/<variant>/test/test_svc -> build/<variant>/bin, so an ASan run drives
 * the ASan nd-apprun. */
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
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndsvc-XXXXXX", tmp) != ND_OK)
        STAGE_FAIL(4);
    if (mkdtemp(tmpl) == NULL)
        STAGE_FAIL(5);
    /* 0711, because nd_proc_launch_app() drops an ordinary app to ndusr and
     * the app it launches lives under here. See test_proc.c's stage_root()
     * for the long version; the short one is that a 0700 fixture makes the
     * dropped child unable to reach nd-apprun, and this test then HANGS
     * rather than failing, because it waits for a reply from a process that
     * never started. */
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

    if (nd_snprintf(built, sizeof built, "%s/../test/apps/SvcApp/app.so", g_bindir) != ND_OK)
        STAGE_FAIL(12);
    if (!file_exists(built)) {
        fprintf(stderr, "test_svc: %s is missing; run `make test`\n", built);
        STAGE_FAIL(13);
    }
    return stage_app(built, SVC_APP_DIR);
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
 * A core: a UI over memory, with the two services really open
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
    fx->ui.home_.engineering_mode = true;
    fx->ui.home_.eng_mode_ready = true;
    fx->ui.image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    if (fx->ui.image_cache == NULL)
        return false;

    /* THE POINT OF THIS FIXTURE: the two services the channel serves, really
     * open. With no hardware they run in simulation mode, which is still the
     * whole code path down to do_send_sms()'s own simulation branch. */
    if (nd_modem_open(&fx->ui.modem) != ND_OK)
        fx->ui.modem = NULL;
    if (nd_battery_open(&fx->ui.battery, -1, -1) != ND_OK)
        fx->ui.battery = NULL;
    return fx->ui.modem != NULL && fx->ui.battery != NULL;
}

static void core_free(core_fixture *fx)
{
    if (fx->ui.modem != NULL)
        nd_modem_close(fx->ui.modem);
    if (fx->ui.battery != NULL)
        nd_battery_close(fx->ui.battery);
    fx->ui.modem = NULL;
    fx->ui.battery = NULL;
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
 * key=value report reading
 * ------------------------------------------------------------------ */

static bool report_get(const char *report, const char *key, char *out, size_t out_sz)
{
    char needle[64];
    const char *p = report;
    size_t klen;

    if (nd_snprintf(needle, sizeof needle, "%s=", key) != ND_OK)
        return false;
    klen = strlen(needle);
    while (p != NULL && *p != '\0') {
        if (strncmp(p, needle, klen) == 0) {
            const char *end = strchr(p + klen, '\n');
            size_t n = end != NULL ? (size_t)(end - (p + klen)) : strlen(p + klen);

            if (n >= out_sz)
                n = out_sz - 1u;
            memcpy(out, p + klen, n);
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    return false;
}

/* A missing key reads as a value nothing equals, so an absent line fails as
 * itself rather than as a mysterious 0. */
static void check_kv(const char *report, const char *key, const char *want)
{
    char got[256];

    if (!report_get(report, key, got, sizeof got)) {
        g_checks++;
        g_failures++;
        fprintf(stderr, "FAIL %s:%d  the child never reported \"%s\"\n", __FILE__, __LINE__, key);
        return;
    }
    if (strcmp(got, want) != 0) {
        g_checks++;
        g_failures++;
        fprintf(stderr, "FAIL %s:%d  %s: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, key, got,
                want);
        return;
    }
    g_checks++;
}

/* ------------------------------------------------------------------ *
 * 1. No channel and no handle: the sentence the Python drew is preserved
 * ------------------------------------------------------------------ */

static void test_no_route_at_all(void)
{
    nd_ui ui;
    nd_modem_status st;
    nd_svc_battery b;
    char detail[ND_MODEM_DETAIL_MAX];

    memset(&ui, 0, sizeof ui);
    nd_svc_client_close();
    CHECK(!nd_svc_client_active());

    /* This is what Messages tests before it draws "ModemService is not
     * running." Nothing about this change may make that sentence disappear
     * from a phone that really has no ModemService. */
    CHECK(!nd_svc_modem_present(&ui));
    CHECK(!nd_svc_battery_present(&ui));

    CHECK(!nd_svc_send_sms(&ui, "0871234567", "hello", detail, sizeof detail));
    CHECK_STR(detail, "core service is gone");

    memset(&st, 0, sizeof st);
    CHECK(!nd_svc_modem_status(&ui, &st));
    /* Still the "nothing is known" snapshot, not uninitialised memory. */
    CHECK_INT(st.signal_level, -1);
    CHECK_INT(st.reg_stat, -1);
    CHECK_INT(st.call_secs, -1);

    memset(&b, 0xAB, sizeof b);
    CHECK(!nd_svc_battery_read(&ui, &b));
    CHECK(!b.ok);
    CHECK(!b.hardware);
    CHECK(!nd_svc_battery_quickstart(&ui));

    /* A NULL detail buffer must not be a crash: nd_svc.h says detail is
     * optional and nd_strlcpy() dereferences whatever it is given. */
    CHECK(!nd_svc_send_sms(&ui, "0871234567", "hello", NULL, 0u));
}

/* ------------------------------------------------------------------ *
 * 2. A direct handle wins -- the rule all 49 golden frames stand on
 * ------------------------------------------------------------------ */

static void test_a_direct_handle_wins(core_fixture *fx)
{
    nd_modem_status via_svc;
    nd_modem_status direct;
    nd_svc_battery b;
    char detail[ND_MODEM_DETAIL_MAX];

    CHECK(nd_svc_modem_present(&fx->ui));
    CHECK(nd_svc_battery_present(&fx->ui));

    /* nd-shoot runs apps in process with exactly this shape, so these two
     * had better be the same object's answer and not a wire's. */
    nd_modem_status_snapshot(fx->ui.modem, &direct);
    memset(&via_svc, 0, sizeof via_svc);
    CHECK(nd_svc_modem_status(&fx->ui, &via_svc));
    CHECK_INT(via_svc.hardware, direct.hardware);
    CHECK_STR(via_svc.port, direct.port);
    CHECK_INT(via_svc.reg_stat, direct.reg_stat);

    CHECK(nd_svc_battery_read(&fx->ui, &b));
    CHECK_INT(b.hardware, nd_battery_has_hardware(fx->ui.battery));
    CHECK_INT(b.level, nd_battery_level(fx->ui.battery));

    /* Simulation mode answers "simulated" and reports the send as done --
     * the modem's own wording, which is what Messages renders. */
    detail[0] = '\0';
    CHECK(nd_svc_send_sms(&fx->ui, "0871234567", "direct", detail, sizeof detail));
    CHECK_STR(detail, "simulated");
}

/* ------------------------------------------------------------------ *
 * 3. The loopback: a live server on its own thread, this process as client
 * ------------------------------------------------------------------ */

/* Point this process's client at `fd`. The client takes ownership, so the
 * caller hands it a dup. */
static bool client_from_fd(int fd)
{
    char v[32];

    if (nd_snprintf(v, sizeof v, "%d", fd) != ND_OK)
        return false;
    if (setenv(ND_ENV_SERVICE_FD, v, 1) != 0)
        return false;
    nd_svc_client_open_from_env();
    (void)unsetenv(ND_ENV_SERVICE_FD);
    return nd_svc_client_active();
}

static void test_loopback_over_a_live_server(core_fixture *fx)
{
    nd_svc_server *s = NULL;
    nd_ui app; /* an APP's context: the handles are NULL, as nd_app.h says */
    nd_modem_status st;
    nd_modem_status direct;
    nd_svc_battery b;
    char detail[ND_MODEM_DETAIL_MAX];
    int child_fd;

    memset(&app, 0, sizeof app);

    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL)
        return;
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        return;
    }
    CHECK(client_from_fd(child_fd));
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    /* EVERY CALL BELOW BLOCKS THIS THREAD IN recv() UNTIL THE SERVER THREAD
     * ANSWERS. That is the assertion: an inline design would deadlock here
     * rather than fail, so the suite completing is the proof. */
    CHECK(nd_svc_modem_present(&app));
    CHECK(nd_svc_battery_present(&app));

    nd_modem_status_snapshot(fx->ui.modem, &direct);
    memset(&st, 0, sizeof st);
    CHECK(nd_svc_modem_status(&app, &st));
    CHECK_INT(st.hardware, direct.hardware);
    CHECK_STR(st.port, direct.port);
    CHECK_INT(st.reg_stat, direct.reg_stat);

    CHECK(nd_svc_battery_read(&app, &b));
    CHECK_INT(b.hardware, nd_battery_has_hardware(fx->ui.battery));
    CHECK_INT(b.level, nd_battery_level(fx->ui.battery));
    CHECK_INT(b.snap.bus, ND_BATT_DEFAULT_I2C_BUS);
    CHECK_INT(b.snap.addr, ND_BATT_DEFAULT_I2C_ADDR);

    /* No gauge on this host, so the write must come back false -- and coming
     * back at all is what proves it reached nd_battery_quickstart(). */
    CHECK(!nd_svc_battery_quickstart(&app));

    detail[0] = '\0';
    CHECK(nd_svc_send_sms(&app, "0871234567", "over the wire", detail, sizeof detail));
    CHECK_STR(detail, "simulated");

    /* ---- validation, which happens in the CORE and not in the app ---- */

    /* AT injection: everything the modem would read as a command. The app
     * side sends what it is handed; the core refuses it. */
    detail[0] = '\0';
    CHECK(!nd_svc_send_sms(&app, "0871234567\r\nATH\r\n", "hang up", detail, sizeof detail));
    CHECK_STR(detail, "the core refused the request");

    detail[0] = '\0';
    CHECK(!nd_svc_send_sms(&app, "08712 345 67", "spaces are not digits", detail, sizeof detail));
    CHECK_STR(detail, "the core refused the request");

    detail[0] = '\0';
    CHECK(!nd_svc_send_sms(&app, "087+1234567", "a plus in the middle", detail, sizeof detail));
    CHECK_STR(detail, "the core refused the request");

    detail[0] = '\0';
    CHECK(!nd_svc_send_sms(&app, "", "no number at all", detail, sizeof detail));
    CHECK_STR(detail, "the core refused the request");

    detail[0] = '\0';
    CHECK(!nd_svc_send_sms(&app, "0871234567", "", detail, sizeof detail));
    CHECK_STR(detail, "the core refused the request");

    /* Malformed UTF-8: a lone continuation byte. The composer cannot produce
     * one, which is the point -- a hostile app can. */
    detail[0] = '\0';
    CHECK(!nd_svc_send_sms(&app, "0871234567", "bad \x80 byte", detail, sizeof detail));
    CHECK_STR(detail, "the core refused the request");

    /* Well-formed UTF-8 above ASCII still goes through: the modem's own
     * ascii_replace() decides what to do with it, as it always did. */
    detail[0] = '\0';
    CHECK(
        nd_svc_send_sms(&app, "+353871234567", "caf\xc3\xa9 \xe2\x82\xac", detail, sizeof detail));
    CHECK_STR(detail, "simulated");

    /* A leading + is a number; the whole set nd_modem__filter_number() keeps
     * is accepted. */
    detail[0] = '\0';
    CHECK(nd_svc_send_sms(&app, "*100#", "ussd-looking but still a number", detail, sizeof detail));
    CHECK_STR(detail, "simulated");

    /* One refused REQUEST does not close the channel: the next one works. */
    CHECK(nd_svc_modem_present(&app));

    nd_svc_client_close();
    nd_svc_server_stop(s);
    (void)close(child_fd);
}

/* ------------------------------------------------------------------ *
 * 4. A datagram that is not a request at all
 * ------------------------------------------------------------------ */

/* The core is the trusting end by construction -- it reads whatever the child
 * writes. A short datagram must not crash it, must not be answered, and must
 * not leave nd_svc_server_stop() blocked. */
static void test_garbage_does_not_take_the_core_down(core_fixture *fx)
{
    nd_svc_server *s = NULL;
    struct pollfd pfd;
    int child_fd;

    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL)
        return;
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        return;
    }
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    CHECK(send(child_fd, "not a request", 13u, MSG_NOSIGNAL) == 13);

    /* Nothing comes back -- a record of the wrong size is refused before it
     * is looked at, so there is no reply for a fuzzer to steer. */
    pfd.fd = child_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    (void)poll(&pfd, 1u, 250);
    CHECK((pfd.revents & POLLIN) == 0 || recv(child_fd, NULL, 0u, MSG_DONTWAIT) <= 0);

    /* And the stop is still prompt: no request is in flight, so this joins
     * rather than detaching. */
    nd_svc_server_stop(s);
    (void)close(child_fd);

    /* The core survived, which is the whole claim: it can still serve. */
    CHECK(nd_svc_modem_present(&fx->ui));
}

/* ------------------------------------------------------------------ *
 * A modem that answers, so `hardware` can be TRUE on this host
 * ------------------------------------------------------------------ *
 *
 * Every other case in this file runs against a modem in Simulation Mode,
 * where nd_modem_has_hardware() is false -- so `CHECK_INT(st.hardware,
 * direct.hardware)` in the loopback case was comparing false against false
 * and would have passed just as happily if the wire forced the field to zero.
 * The engineering Modem app draws SIMULATION off exactly that field, and it
 * was reported saying SIMULATION on a phone whose modem worked. So the wire
 * has to be tested with the field TRUE, and that needs a port that answers.
 *
 * Not a SIM7600 emulator -- test_modem.c has one of those and it belongs
 * there. This answers OK to everything, which is all _probe_ports() and the
 * init sequence require, and it is deliberately the smallest thing that makes
 * nd_modem_has_hardware() true.
 */

#define MODEM_LINK "/dev/modem"

/* Point ModemService at the pty, or clear the setting again. The staged root
 * makes "/dev/modem" a path inside the scratch tree, so nothing here can
 * reach a real device node. */
static void use_modem_port(const char *dev)
{
    char body[128];

    if (dev == NULL) {
        pt_write_text(ND_PATH_SETTINGS_PROP, "");
        return;
    }
    (void)nd_snprintf(body, sizeof body, "system.hw.modem_at_port=%s\n", dev);
    pt_write_text(ND_PATH_SETTINGS_PROP, body);
}

typedef struct {
    int master;
    int keepalive;
    pthread_t th;
    bool running;
    bool awaiting_body;
    volatile bool stop;
} okmodem;

/* `(void)write(...)` is not enough to silence -Wunused-result: glibc marks
 * write() warn_unused_result under _FORTIFY_SOURCE, which -O2 turns on, and
 * a cast to void does not count as using the value. The tree builds with
 * -Werror, so on a toolchain that pairs a recent glibc with -O2 this file
 * would not compile at all.
 *
 * Looping is also the right thing on a pty: a short write is legal and the
 * modem stub's replies are parsed by the code under test, so half of one is
 * a mysterious failure somewhere else. */
static void write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0u;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (n == 0)
            return;
        off += (size_t)n;
    }
}

static void *okmodem_loop(void *arg)
{
    okmodem *k = arg;
    char line[256];
    size_t len = 0u;

    while (!k->stop) {
        struct pollfd pfd;
        char c;
        ssize_t n;

        pfd.fd = k->master;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 50) <= 0)
            continue;
        n = read(k->master, &c, 1u);
        if (n <= 0)
            continue;
        if (c == '\x1a') { /* Ctrl-Z ends an SMS body */
            line[len] = '\0';
            len = 0u;
            if (k->awaiting_body) {
                static const char SENT[] = "\r\n+CMGS: 42\r\n\r\nOK\r\n";

                k->awaiting_body = false;
                write_all(k->master, SENT, strlen(SENT));
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0u)
                continue;
            line[len] = '\0';
            len = 0u;
            {
                static const char IMEI[] = "\r\n866758041234567\r\n\r\nOK\r\n";
                static const char PROMPT[] = "\r\n> ";
                static const char SENT[] = "\r\n+CMGS: 42\r\n\r\nOK\r\n";
                static const char OK[] = "\r\nOK\r\n";
                const char *reply;

                /* The body arrives as its own "line" after the prompt, and it
                 * is answered with the send receipt rather than a bare OK --
                 * without this, send_sms() waits out its whole timeout and
                 * the test takes 45 seconds to say nothing useful. */
                if (k->awaiting_body) {
                    k->awaiting_body = false;
                    reply = SENT;
                } else if (strncmp(line, "AT+CMGS=", 8u) == 0) {
                    k->awaiting_body = true;
                    reply = PROMPT;
                } else if (strcmp(line, "AT+CGSN") == 0) {
                    reply = IMEI;
                } else {
                    reply = OK;
                }
                write_all(k->master, reply, strlen(reply));
            }
            continue;
        }
        if (len + 1u < sizeof line)
            line[len++] = c;
    }
    return NULL;
}

static bool okmodem_start(okmodem *k, const char *link_at)
{
    char link[ND_PATH_MAX];
    const char *slave;

    memset(k, 0, sizeof *k);
    k->master = -1;
    k->keepalive = -1;

    k->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (k->master < 0 || grantpt(k->master) != 0 || unlockpt(k->master) != 0)
        return false;
    slave = ptsname(k->master);
    if (slave == NULL)
        return false;

    /* Held open for the life of the fake so the master never sees a hangup
     * between the probe closing one candidate and opening the next. */
    k->keepalive = open(slave, O_RDWR | O_NOCTTY);
    if (k->keepalive < 0)
        return false;

    pt_mkdir("/dev");
    if (nd_path_resolve(link, sizeof link, link_at) != ND_OK)
        return false;
    (void)unlink(link);
    if (symlink(slave, link) != 0)
        return false;

    if (pthread_create(&k->th, NULL, okmodem_loop, k) != 0)
        return false;
    k->running = true;
    return true;
}

static void okmodem_stop(okmodem *k)
{
    k->stop = true;
    if (k->running)
        (void)pthread_join(k->th, NULL);
    if (k->keepalive >= 0)
        (void)close(k->keepalive);
    if (k->master >= 0)
        (void)close(k->master);
    memset(k, 0, sizeof *k);
}

/* ------------------------------------------------------------------ *
 * 4b. The wire with a modem that ANSWERS
 * ------------------------------------------------------------------ *
 *
 * The reported bug: "modem works completely, just the info app doesn't." The
 * app draws SIMULATION off st.hardware and gets st.hardware from here.
 */

/* Defined with the other launch helpers in section 5, below. */
static void app_entry_for(nd_app_entry *e, const char *dir, const char *name);

static void test_hardware_true_survives_the_wire(void)
{
    okmodem fake;
    core_fixture hw;
    nd_svc_server *s = NULL;
    nd_ui app; /* an APP's context: the handles are NULL, as nd_app.h says */
    nd_modem_status st;
    nd_modem_status direct;
    int child_fd;

    use_modem_port(MODEM_LINK);
    if (!okmodem_start(&fake, MODEM_LINK)) {
        fprintf(stderr, "test_svc: no pty on this host; skipping the hardware case\n");
        okmodem_stop(&fake);
        return;
    }
    if (!core_init(&hw)) {
        fprintf(stderr, "test_svc: cannot build a core fixture; skipping\n");
        core_free(&hw);
        okmodem_stop(&fake);
        return;
    }

    /* The precondition. Without it this case proves nothing, so it is checked
     * rather than assumed: if the fake did not get adopted, say so loudly. */
    nd_modem_status_snapshot(hw.ui.modem, &direct);
    CHECK(direct.hardware);
    if (!direct.hardware) {
        fprintf(stderr, "test_svc: the fake modem was not adopted (%s)\n", direct.probe_why);
        core_free(&hw);
        okmodem_stop(&fake);
        return;
    }

    memset(&app, 0, sizeof app);
    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL) {
        core_free(&hw);
        okmodem_stop(&fake);
        return;
    }
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    CHECK(client_from_fd(child_fd));
    CHECK_INT(nd_svc_server_start(s, &hw.ui), ND_OK);

    /* Asked twice, because the app asks once per second forever and the
     * failure being chased looks like "the first answer is fine and every
     * one after it is not". */
    memset(&st, 0, sizeof st);
    CHECK(nd_svc_modem_present(&app));
    CHECK(nd_svc_modem_status(&app, &st));
    CHECK(st.hardware); /* <-- the app's SIMULATION line, over the wire */
    CHECK_STR(st.port, MODEM_LINK);

    memset(&st, 0, sizeof st);
    CHECK(nd_svc_modem_status(&app, &st));
    CHECK(st.hardware);
    CHECK_STR(st.port, MODEM_LINK);

    nd_svc_client_close();
    nd_svc_server_stop(s);

    /* ...and the same question from a REAL forked child, which is the shape
     * the phone actually runs: nd_proc_launch_app() forks nd-apprun, the
     * child adopts NEODCT_SERVICE_FD and asks across it. The loopback above
     * shares this process; this does not. */
    {
        nd_app_entry entry;
        nd_crash_info crash;
        char report[REPORT_BYTES];
        char resolved[ND_PATH_MAX];

        if (nd_path_resolve(resolved, sizeof resolved, SVC_REPORT) == ND_OK)
            (void)unlink(resolved);

        app_entry_for(&entry, SVC_APP_DIR, "SvcApp");
        memset(&crash, 0, sizeof crash);
        CHECK_INT(nd_proc_launch_app(&hw.ui, &entry, NULL, NULL, &crash), ND_OK);
        CHECK(!crash.from_signal);
        if (pt_read_text(SVC_REPORT, report, sizeof report) != (size_t)-1) {
            check_kv(report, "done", "1");
            check_kv(report, "modem_handle", "0"); /* still an app, still NULL */
            check_kv(report, "status_ok", "1");
            check_kv(report, "status_hardware", "1"); /* the reported bug */
            check_kv(report, "status_port", MODEM_LINK);
        } else {
            CHECK(false);
            fprintf(stderr, "test_svc: the hardware-case child wrote no report\n");
        }
    }

    core_free(&hw);
    okmodem_stop(&fake);
    use_modem_port(NULL);
}

/* ------------------------------------------------------------------ *
 * 5. The two verbs that end the session
 * ------------------------------------------------------------------ *
 *
 * A reboot verb cannot be tested by rebooting. So the simulation below
 * replaces STEP 5 of the core's sequence -- the spawn -- and nothing else:
 * the record is really validated, the binary is really resolved, the reply
 * is really sent, and sync(2) really runs. What is left out is the one line
 * whose success would be that this process stopped existing.
 *
 * That is enough to prove the thing section 9 is actually about, which is
 * the ORDER -- everything reportable before the reply, the unbounded sync
 * after it. See halt_fake_spawn().
 */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int32_t calls;
    bool reboot; /* which verb the last call was for */
    char exe[ND_PATH_MAX];

    /* The ordering proof. With `want_reply_first` set, the fake refuses to
     * return until the CLIENT says its answer is already in. So if the core
     * ever halted BEFORE replying, the client would still be sitting in
     * recv(), the fake would never be released, and `saw_reply_first` would
     * come back false when the deadline expired -- a failed case rather than
     * a hung suite. Same trick as test_loopback_over_a_live_server(). */
    bool want_reply_first;
    bool reply_seen;
    bool saw_reply_first;
} halt_fake;

#define HALT_FAKE_WAIT_S 2

static void halt_fake_init(halt_fake *f)
{
    memset(f, 0, sizeof *f);
    (void)pthread_mutex_init(&f->mu, NULL);
    (void)pthread_cond_init(&f->cv, NULL);
}

static void halt_fake_free(halt_fake *f)
{
    (void)pthread_cond_destroy(&f->cv);
    (void)pthread_mutex_destroy(&f->mu);
}

/* Runs on the CORE's serving thread, in place of nd_proc_spawn(). */
static void halt_fake_spawn(bool reboot, const char *exe, void *user)
{
    halt_fake *f = user;

    (void)pthread_mutex_lock(&f->mu);
    f->calls++;
    f->reboot = reboot;
    (void)nd_strlcpy(f->exe, (exe != NULL) ? exe : "", sizeof f->exe);

    if (f->want_reply_first) {
        struct timespec deadline;

        (void)clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += HALT_FAKE_WAIT_S;
        while (!f->reply_seen) {
            if (pthread_cond_timedwait(&f->cv, &f->mu, &deadline) != 0)
                break;
        }
        f->saw_reply_first = f->reply_seen;
    }
    (void)pthread_cond_broadcast(&f->cv);
    (void)pthread_mutex_unlock(&f->mu);
}

/* The client half of that handshake: "my answer is in". */
static void halt_fake_reply_landed(halt_fake *f)
{
    (void)pthread_mutex_lock(&f->mu);
    f->reply_seen = true;
    (void)pthread_cond_broadcast(&f->cv);
    (void)pthread_mutex_unlock(&f->mu);
}

/* Wait for the serving thread to have finished its fake spawn, so the
 * assertions read settled fields instead of racing them. */
static bool halt_fake_wait_called(halt_fake *f)
{
    struct timespec deadline;
    bool called;

    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += HALT_FAKE_WAIT_S;
    (void)pthread_mutex_lock(&f->mu);
    while (f->calls == 0) {
        if (pthread_cond_timedwait(&f->cv, &f->mu, &deadline) != 0)
            break;
    }
    called = f->calls > 0;
    (void)pthread_mutex_unlock(&f->mu);
    return called;
}

/* ---- 5a. the tables and the lookup, moved here out of test_power.c ---- */

static void test_the_halt_tables_are_the_pythons(void)
{
    /* THE ORDER DECIDES WHICH OF TWO PROGRAMS RUNS on an image carrying both
     * `poweroff` and `/sbin/poweroff`, so it is asserted rather than assumed.
     * These were nd_power_halt_commands, nd_power_reboot_commands and
     * nd_update_reboot_commands -- three tables in two shared objects that
     * could not see each other -- until the halt moved into the core. */
    CHECK_STR(nd_svc_poweroff_commands[0][0], "poweroff");
    CHECK(nd_svc_poweroff_commands[0][1] == NULL);
    CHECK_STR(nd_svc_poweroff_commands[1][0], "/sbin/poweroff");
    CHECK_STR(nd_svc_poweroff_commands[2][0], "busybox");
    CHECK_STR(nd_svc_poweroff_commands[2][1], "poweroff");
    CHECK(nd_svc_poweroff_commands[2][2] == NULL);

    CHECK_STR(nd_svc_reboot_commands[0][0], "reboot");
    CHECK(nd_svc_reboot_commands[0][1] == NULL);
    CHECK_STR(nd_svc_reboot_commands[1][0], "/sbin/reboot");
    CHECK_STR(nd_svc_reboot_commands[2][0], "busybox");
    CHECK_STR(nd_svc_reboot_commands[2][1], "reboot");
    CHECK(nd_svc_reboot_commands[2][2] == NULL);
}

/* An executable that does nothing, so a lookup can succeed without anything
 * being spawned even if it were. */
static bool make_stub_program(const char *dir, const char *name, char *out, size_t out_sz)
{
    FILE *f;

    if (nd_snprintf(out, out_sz, "%s/%s", dir, name) != ND_OK)
        return false;
    f = fopen(out, "w");
    if (f == NULL)
        return false;
    (void)fputs("#!/bin/sh\nexit 0\n", f);
    (void)fclose(f);
    return chmod(out, 0755) == 0;
}

/* <stage>/halt-bin, made once and used by both of the cases below. */
static char g_haltbin[ND_PATH_MAX];

static bool halt_bin_dir(void)
{
    if (g_haltbin[0] != '\0')
        return true;
    if (nd_snprintf(g_haltbin, sizeof g_haltbin, "%s/halt-bin", g_stage) != ND_OK)
        return false;
    if (mkdir(g_haltbin, 0755) != 0 && errno != EEXIST) {
        g_haltbin[0] = '\0';
        return false;
    }
    return true;
}

static void test_the_halt_lookup_is_execvps(void)
{
    char out[ND_PATH_MAX];
    char expect[ND_PATH_MAX];
    char keep[ND_PATH_MAX];
    const char *saved = getenv("PATH");

    CHECK(halt_bin_dir());
    if (g_haltbin[0] == '\0')
        return;

    (void)nd_strlcpy(keep, (saved != NULL) ? saved : "", sizeof keep);
    CHECK(make_stub_program(g_haltbin, "nd-fake-halt", expect, sizeof expect));
    (void)setenv("PATH", g_haltbin, 1);

    CHECK(nd_svc_halt_which("nd-fake-halt", out, sizeof out));
    CHECK_STR(out, expect);

    /* Python's `except OSError: continue`, which is what walks the table on
     * to the next candidate. */
    CHECK(!nd_svc_halt_which("nd-there-is-no-such-program", out, sizeof out));

    /* A name containing a slash is a path, not a name -- execvp's rule, and
     * the reason /sbin/poweroff can be a candidate at all. */
    CHECK(nd_svc_halt_which(expect, out, sizeof out));
    CHECK_STR(out, expect);
    CHECK(!nd_svc_halt_which("/nd/no/such/path", out, sizeof out));

    /* A file that is not executable does not count, which is what stops a
     * README called `reboot` being run. */
    {
        char plain[ND_PATH_MAX];
        FILE *f;

        if (nd_snprintf(plain, sizeof plain, "%s/nd-not-exec", g_haltbin) == ND_OK) {
            f = fopen(plain, "w");
            if (f != NULL) {
                (void)fputs("not a program\n", f);
                (void)fclose(f);
                (void)chmod(plain, 0644);
            }
        }
        CHECK(!nd_svc_halt_which("nd-not-exec", out, sizeof out));
    }

    CHECK(!nd_svc_halt_which(NULL, out, sizeof out));
    CHECK(!nd_svc_halt_which("", out, sizeof out));

    (void)setenv("PATH", keep, 1);
}

/* ---- 5b. the wire, with the consequence injected out ---- */

/* Two candidate tables of one entry each, pointing at stub programs this
 * test writes. Substituted for the shipped tables so that the case does not
 * depend on the host having a real /sbin/reboot -- a container often does
 * not -- and so that the path the core resolves is one this file knows
 * exactly. The shipped tables are asserted separately above; what is under
 * test here is the sequence, not the spelling. */
static char g_stub_reboot[ND_PATH_MAX];
static char g_stub_poweroff[ND_PATH_MAX];
static const char *g_stub_reboot_argv[2];
static const char *g_stub_poweroff_argv[2];
static const char *const *const g_stub_reboot_tab[1] = {g_stub_reboot_argv};
static const char *const *const g_stub_poweroff_tab[1] = {g_stub_poweroff_argv};

static bool stub_halt_programs(void)
{
    if (!halt_bin_dir())
        return false;
    if (!make_stub_program(g_haltbin, "nd-stub-reboot", g_stub_reboot, sizeof g_stub_reboot))
        return false;
    if (!make_stub_program(g_haltbin, "nd-stub-poweroff", g_stub_poweroff, sizeof g_stub_poweroff))
        return false;
    g_stub_reboot_argv[0] = g_stub_reboot;
    g_stub_reboot_argv[1] = NULL;
    g_stub_poweroff_argv[0] = g_stub_poweroff;
    g_stub_poweroff_argv[1] = NULL;
    return true;
}

/* The fake the two LAUNCH cases share, so that SvcApp's poweroff request is
 * caught by the launcher's own serving thread. Lives here rather than in
 * main() so halt_fake_spawn() can reach it by address. */
static halt_fake g_child_halt;

/* Candidates that certainly do not exist, so "nothing resolved" is reachable
 * on a host where /sbin/poweroff really does. */
static const char *const NOHALT_A[] = {"nd-no-such-halt-a", NULL};
static const char *const NOHALT_B[] = {"/nd/no/such/halt-b", NULL};
static const char *const NOHALT_C[] = {"nd-no-such-halt-c", "poweroff", NULL};
static const char *const *const NOHALT[3] = {NOHALT_A, NOHALT_B, NOHALT_C};

static void test_a_halt_over_the_wire(core_fixture *fx)
{
    nd_svc_halt_sim sim;
    halt_fake fake;
    nd_svc_server *s = NULL;
    nd_ui app; /* an APP's context: no handles, as nd_app.h says */
    int child_fd;

    memset(&app, 0, sizeof app);
    CHECK(stub_halt_programs());
    if (g_stub_reboot[0] == '\0')
        return;

    halt_fake_init(&fake);
    memset(&sim, 0, sizeof sim);
    sim.spawn = halt_fake_spawn;
    sim.user = &fake;
    sim.reboot = g_stub_reboot_tab;
    sim.poweroff = g_stub_poweroff_tab;
    sim.n = 1u;

    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL) {
        halt_fake_free(&fake);
        return;
    }
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        halt_fake_free(&fake);
        return;
    }
    CHECK(client_from_fd(child_fd));

    /* Installed BEFORE the thread exists and cleared after it is joined.
     * pthread_create() and pthread_join() are what make a plain global safe
     * to read from the serving thread; nd_svc.h says so. */
    fake.want_reply_first = true;
    nd_svc_halt_simulate(&sim);
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    /* The call returns when the REPLY lands -- by which time the core has
     * already committed: it has resolved a binary and is about to sync and
     * spawn. There is nothing left to un-ask. */
    CHECK(nd_svc_reboot());
    halt_fake_reply_landed(&fake);

    CHECK(halt_fake_wait_called(&fake));
    CHECK_INT(fake.calls, 1);
    CHECK(fake.reboot);
    CHECK_STR(fake.exe, g_stub_reboot);
    /* THE ORDERING CLAIM. False here means the core halted before it
     * answered, which on a tired flash is the screen that says "Reboot
     * failed." and then reboots. spec-app-services.md 9.4. */
    CHECK(fake.saw_reply_first);

    /* The other verb, over the same live channel, with the handshake off so
     * that this half is a plain round trip. */
    (void)pthread_mutex_lock(&fake.mu);
    fake.want_reply_first = false;
    fake.calls = 0;
    (void)pthread_mutex_unlock(&fake.mu);

    CHECK(nd_svc_poweroff());
    CHECK(halt_fake_wait_called(&fake));
    CHECK(!fake.reboot);
    CHECK_STR(fake.exe, g_stub_poweroff);

    /* Neither verb is a service that can be absent, so neither can answer
     * "no such service": the channel is still good for anything else. */
    CHECK(nd_svc_modem_present(&app));

    nd_svc_client_close();
    nd_svc_server_stop(s);
    nd_svc_halt_simulate(NULL);
    (void)close(child_fd);
    halt_fake_free(&fake);
}

/* The other branch: an image with no halt binary on it anywhere. The app is
 * told false -- which is what Power turns into "Reboot failed." -- and
 * NOTHING is spawned, faked or otherwise. */
static void test_a_halt_with_nothing_to_spawn(core_fixture *fx)
{
    nd_svc_halt_sim sim;
    halt_fake fake;
    nd_svc_server *s = NULL;
    nd_ui app;
    int child_fd;

    memset(&app, 0, sizeof app);
    halt_fake_init(&fake);
    memset(&sim, 0, sizeof sim);
    sim.spawn = halt_fake_spawn;
    sim.user = &fake;
    sim.poweroff = NOHALT;
    sim.reboot = NOHALT;
    sim.n = 3u;

    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL) {
        halt_fake_free(&fake);
        return;
    }
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        halt_fake_free(&fake);
        return;
    }
    CHECK(client_from_fd(child_fd));
    nd_svc_halt_simulate(&sim);
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    CHECK(!nd_svc_reboot());
    CHECK(!nd_svc_poweroff());
    CHECK_INT(fake.calls, 0);

    /* A refused halt leaves the channel open, exactly as a refused SEND_SMS
     * does: this is a failed operation, not a protocol error. */
    CHECK(nd_svc_modem_present(&app));

    nd_svc_client_close();
    nd_svc_server_stop(s);
    nd_svc_halt_simulate(NULL);
    (void)close(child_fd);
    halt_fake_free(&fake);
}

/* ---- 5c. the clock, over the wire ---- */

/* The WINDOW is pinned in test_clock.c, which is the file that can stage a
 * version.prop and therefore the only one that can say what the window is to
 * the second. What is under test here is the other half: that the request
 * crosses the socket at all, that the core -- not the app -- decides, and
 * that a refusal is a failed operation rather than a protocol error.
 *
 * So this needs one value INSIDE the window and two certainly outside it,
 * and it derives the first the way the implementation does rather than
 * hard-coding a year that stops being true. */
static time_t clock_window_floor(void)
{
    time_t epoch = 0;

    if (!nd_clock_build_epoch(&epoch) || epoch < (time_t)ND_CLOCK_SANE_MIN)
        epoch = (time_t)ND_CLOCK_SANE_MIN;
    return epoch;
}

static time_t a_time_this_build_will_believe(void)
{
    return clock_window_floor() + 86400;
}

static void test_the_clock_over_the_wire(core_fixture *fx)
{
    nd_svc_server *s = NULL;
    nd_ui app; /* an APP's context: no handles, as nd_app.h says */
    int child_fd;

    memset(&app, 0, sizeof app);
    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL)
        return;
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        return;
    }
    CHECK(client_from_fd(child_fd));
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    /* Nothing here moves the machine's clock: nd_clock.c refuses to call
     * clock_settime() while NEODCT_ROOT is set, and stage_root() set it. */
    CHECK(nd_svc_set_clock(a_time_this_build_will_believe()));

    /* The epoch, and a date past 2100. Both are outside every window this
     * build could compute, whatever version.prop says -- and both are refused
     * by the CLIENT half, before a record is built, because they are outside
     * the coarse 2020-2100 range as well. */
    CHECK(!nd_svc_set_clock((time_t)0));
    CHECK(!nd_svc_set_clock((time_t)ND_CLOCK_SANE_MAX + 1));

    /* A day past the ceiling: inside 2020-2100, so this one CROSSES THE WIRE
     * and is refused by the core. Without it the two cases above would pass
     * on a build whose server-side bound had been deleted entirely, since
     * they never reach it. */
    {
        time_t past_the_ceiling = clock_window_floor() + ND_SVC_CLOCK_MAX_SKEW_S + 86400;

        if (past_the_ceiling <= (time_t)ND_CLOCK_SANE_MAX)
            CHECK(!nd_svc_set_clock(past_the_ceiling));
    }

    /* A refused clock leaves the channel open, exactly as a refused SEND_SMS
     * does: this is a failed operation, not a protocol error. */
    CHECK(nd_svc_modem_present(&app));

    nd_svc_client_close();
    nd_svc_server_stop(s);
    (void)close(child_fd);
}

/* ---- 5d. formatting a card, with the mkfs injected out ---- */

/* Where this case's fake /run/neodct/sdcard.prop lives. ND_ROOT keeps it
 * inside the stage, exactly as test_storage.c does it. */
#define FMT_MOUNT "/sdcard"
#define FMT_STATE "/sdcard.prop"

typedef struct {
    pthread_mutex_t mu;
    int32_t calls;
    char device[64]; /* what the CORE resolved, which is the whole point */
    int result;      /* what the fake helper exits with */
} format_fake;

/* Runs on the CORE's serving thread, in place of the spawn-and-wait.
 *
 * No handshake, unlike halt_fake_spawn(): the format's answer DEPENDS on the
 * helper's exit status, so the reply cannot precede the work and there is no
 * ordering claim to prove. The client returning is already proof that this
 * ran. */
static int format_fake_run(const char *device, void *user)
{
    format_fake *f = user;
    int rc;

    (void)pthread_mutex_lock(&f->mu);
    f->calls++;
    (void)nd_strlcpy(f->device, (device != NULL) ? device : "", sizeof f->device);
    rc = f->result;
    (void)pthread_mutex_unlock(&f->mu);
    return rc;
}

static int format_fake_calls(format_fake *f)
{
    int n;

    (void)pthread_mutex_lock(&f->mu);
    n = (int)f->calls;
    (void)pthread_mutex_unlock(&f->mu);
    return n;
}

static void format_fake_device(format_fake *f, char *out, size_t out_sz)
{
    (void)pthread_mutex_lock(&f->mu);
    (void)nd_strlcpy(out, f->device, out_sz);
    (void)pthread_mutex_unlock(&f->mu);
}

/* neodct-sdcard's own output format, which nd_storage.c parses. */
static void write_card_state(const char *state, const char *device, const char *fstype)
{
    char body[256];

    CHECK_INT(nd_snprintf(body, sizeof body, "state=%s\ndevice=%s\nfstype=%s\nlabel=NEODCT\n",
                          state, device, fstype),
              ND_OK);
    pt_write_text(FMT_STATE, body);
}

static void test_a_format_over_the_wire(core_fixture *fx)
{
    nd_svc_format_sim sim;
    format_fake fake;
    nd_svc_server *s = NULL;
    nd_ui app;
    char device[64];
    int child_fd;

    memset(&app, 0, sizeof app);
    memset(&fake, 0, sizeof fake);
    (void)pthread_mutex_init(&fake.mu, NULL);
    memset(&sim, 0, sizeof sim);
    sim.run = format_fake_run;
    sim.user = &fake;

    nd_storage_set_paths(FMT_MOUNT, FMT_STATE);

    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL) {
        (void)pthread_mutex_destroy(&fake.mu);
        nd_storage_set_paths(NULL, NULL);
        return;
    }
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        (void)pthread_mutex_destroy(&fake.mu);
        nd_storage_set_paths(NULL, NULL);
        return;
    }
    CHECK(client_from_fd(child_fd));
    nd_svc_format_simulate(&sim);
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    /* THE ASSERTION THIS WHOLE CASE EXISTS FOR. nd_svc_format_card() takes no
     * argument -- the app could not name a device if it wanted to -- and the
     * device the helper is pointed at comes out of the state file the CORE
     * read. Settings used to pass card->device across the boundary; this is
     * what replaced it. */
    write_card_state("mounted", "/dev/vdc1", "vfat");
    fake.result = 0;
    CHECK(nd_svc_format_card());
    CHECK_INT(format_fake_calls(&fake), 1);
    format_fake_device(&fake, device, sizeof device);
    CHECK_STR(device, "/dev/vdc1");

    /* A helper that ran and failed. Settings draws "Formatting failed." */
    (void)pthread_mutex_lock(&fake.mu);
    fake.result = 1;
    (void)pthread_mutex_unlock(&fake.mu);
    CHECK(!nd_svc_format_card());
    CHECK_INT(format_fake_calls(&fake), 2);

    /* The QEMU share. `removable` is (fstype != "virtiofs") and the refusal
     * happens BEFORE the helper is reached -- the call count is the proof,
     * because mkfs on a directory of the developer's machine is not something
     * a "did it work" boolean could take back. */
    write_card_state("share", "/dev/vdc", "virtiofs");
    CHECK(!nd_svc_format_card());
    CHECK_INT(format_fake_calls(&fake), 2);

    /* No card: ABSENT blanks the device, so there is nothing to point at and
     * again nothing is spawned. */
    write_card_state("nocard", "/dev/vdc", "vfat");
    CHECK(!nd_svc_format_card());
    CHECK_INT(format_fake_calls(&fake), 2);

    /* Every one of those refusals is a failed operation, not a protocol
     * error: the channel is still good. */
    CHECK(nd_svc_modem_present(&app));

    nd_svc_client_close();
    nd_svc_server_stop(s);
    nd_svc_format_simulate(NULL);
    (void)close(child_fd);
    (void)pthread_mutex_destroy(&fake.mu);
    nd_storage_set_paths(NULL, NULL);
}

/* ---- 5e. what an UNTRUSTED process can do with this socket ---- */

/* THE UID IS NOT THE BOUNDARY. THE SOCKET IS.
 *
 * nd_svc validates the RECORD and never the SENDER: there is no SO_PEERCRED,
 * no SCM_CREDENTIALS, no peer-uid check anywhere in nd_svc.c. So a process
 * that holds the descriptor is served, whoever it has become -- and the
 * descriptor survives a uid change, because an open file keeps the access it
 * was opened with.
 *
 * This case proves that, deliberately, so that the fact is written down as an
 * assertion rather than as an assumption. It is the reason nd_proc.c gives an
 * UNTRUSTED app NO SERVICE SOCKET AT ALL rather than trying to check who is
 * calling: a check the caller can be on the wrong side of is not a boundary,
 * and an fd that was never created cannot be inherited.
 *
 * It needs root (to become somebody else) and an ndusr_ut in /etc/passwd, so
 * it announces itself as skipped when it cannot run rather than passing
 * quietly -- a confinement test that silently did not run is the failure mode
 * this whole file exists against. */
static void test_an_untrusted_uid_is_still_served(core_fixture *fx)
{
    nd_priv_id ut;
    nd_svc_server *s = NULL;
    int child_fd;
    int pipefd[2];
    pid_t pid;

    if (geteuid() != 0u || !nd_priv_lookup(ND_PRIV_USER_UT, &ut) || !ut.valid) {
        fprintf(stderr, "SKIP an untrusted uid on the service socket: "
                        "needs root and " ND_PRIV_USER_UT "\n");
        return;
    }

    CHECK_INT(nd_svc_server_open(&s), ND_OK);
    if (s == NULL)
        return;
    child_fd = dup(nd_svc_server_child_fd(s));
    CHECK(child_fd >= 0);
    if (child_fd < 0) {
        nd_svc_server_free(s);
        return;
    }
    CHECK_INT(nd_svc_server_start(s, &fx->ui), ND_OK);

    if (pipe(pipefd) != 0) {
        CHECK(false);
        nd_svc_server_stop(s);
        (void)close(child_fd);
        return;
    }

    pid = fork();
    if (pid == 0) {
        /* The child: become ndusr_ut, then use the socket it inherited. This
         * is exactly what netsurf would have been able to do while the
         * Browser app still had a channel to leak to it. */
        char detail[ND_MODEM_DETAIL_MAX];
        nd_ui app;
        uint8_t ok = 0u;

        (void)close(pipefd[0]);
        memset(&app, 0, sizeof app);
        if (nd_priv_become(&ut) != 0)
            _exit(70);
        if (getuid() != ut.uid)
            _exit(71);
        if (!client_from_fd(child_fd))
            _exit(72);
        detail[0] = '\0';
        ok = nd_svc_send_sms(&app, "0871234567", "Test", detail, sizeof detail) ? 1u : 0u;
        if (write(pipefd[1], &ok, 1u) != 1)
            _exit(73);
        (void)close(pipefd[1]);
        _exit(0);
    }
    CHECK(pid > 0);
    (void)close(pipefd[1]);

    if (pid > 0) {
        uint8_t ok = 0xFFu;
        ssize_t n = read(pipefd[0], &ok, 1u);
        nd_proc_status st;

        memset(&st, 0, sizeof st);
        (void)nd_proc_wait(pid, 10.0, &st);

        CHECK_INT((int)n, 1);
        /* THE ASSERTION, and it is deliberately the uncomfortable one: an
         * ndusr_ut process holding this descriptor gets its text sent, by the
         * root core, through the real ModemService. Nothing about the uid
         * stopped it, because nothing about the uid was ever consulted. */
        CHECK_INT((int)ok, 1);
    }
    (void)close(pipefd[0]);

    nd_svc_client_close();
    nd_svc_server_stop(s);
    (void)close(child_fd);
}

/* ------------------------------------------------------------------ *
 * 6. A real child process, launched by the real launcher
 * ------------------------------------------------------------------ */

static void app_entry_for(nd_app_entry *e, const char *dir, const char *name)
{
    memset(e, 0, sizeof *e);
    (void)nd_strlcpy(e->name, name, sizeof e->name);
    (void)nd_strlcpy(e->path, dir, sizeof e->path);
    (void)nd_strlcpy(e->exec, ND_APP_SO_NAME, sizeof e->exec);
    e->id = 9996;
}

static void test_a_real_child_reaches_the_core(core_fixture *fx)
{
    nd_app_entry app;
    nd_crash_info crash;
    nd_modem_status direct;
    char report[REPORT_BYTES];
    char port_want[80];
    char reg_want[32];
    size_t n;

    app_entry_for(&app, SVC_APP_DIR, "SvcApp");
    memset(&crash, 0, sizeof crash);

    CHECK_INT(nd_proc_launch_app(&fx->ui, &app, NULL, NULL, &crash), ND_OK);
    /* A crash here would mean the app faulted before it could report, and
     * every check below would then fail as "never reported". */
    CHECK(!crash.from_signal);
    CHECK_INT(crash.exit_status, 0);

    n = pt_read_text(SVC_REPORT, report, sizeof report);
    if (n == (size_t)-1) {
        CHECK(false);
        fprintf(stderr, "test_svc: the child wrote no report\n");
        return;
    }
    check_kv(report, "done", "1");

    /* The boundary is real: nd_app.h's rule still holds on the far side. */
    check_kv(report, "modem_handle", "0");
    check_kv(report, "battery_handle", "0");
    check_kv(report, "channel", "1");

    /* ...and the app can nonetheless see both services. */
    check_kv(report, "modem_present", "1");
    check_kv(report, "battery_present", "1");

    /* THE ONE THAT MATTERS. A child process, in its own address space, with
     * no serial port of its own, sent an SMS through the core's ModemService
     * and got the modem's own answer back. */
    check_kv(report, "sms_ok", "1");
    check_kv(report, "sms_detail", "simulated");

    /* And could not smuggle an AT command past the boundary while doing it. */
    check_kv(report, "bad_number_ok", "0");
    check_kv(report, "bad_number_detail", "the core refused the request");
    check_kv(report, "empty_body_ok", "0");

    /* The snapshots agree with what the parent's own live services say --
     * which is what makes this a test and not two processes being wrong
     * together. */
    nd_modem_status_snapshot(fx->ui.modem, &direct);
    check_kv(report, "status_ok", "1");
    check_kv(report, "status_hardware", direct.hardware ? "1" : "0");
    (void)nd_snprintf(port_want, sizeof port_want, "%s", direct.port);
    check_kv(report, "status_port", port_want);
    (void)nd_snprintf(reg_want, sizeof reg_want, "%d", (int)direct.reg_stat);
    check_kv(report, "status_reg_stat", reg_want);

    check_kv(report, "batt_read", "1");
    check_kv(report, "batt_hardware", nd_battery_has_hardware(fx->ui.battery) ? "1" : "0");
    (void)nd_snprintf(reg_want, sizeof reg_want, "%d", (int)nd_battery_level(fx->ui.battery));
    check_kv(report, "batt_level", reg_want);
    (void)nd_snprintf(reg_want, sizeof reg_want, "%d", ND_BATT_DEFAULT_I2C_BUS);
    check_kv(report, "batt_bus", reg_want);

    /* No gauge on this host, so both the snapshot and the write say so --
     * having travelled to the core and back to say it. */
    check_kv(report, "batt_ok", "0");
    check_kv(report, "quickstart", "0");

    /* AND THE VERB THAT ENDS THE SESSION. A real child process asked this
     * core to switch the phone off; the core validated it, resolved a
     * binary, answered, synced, and reached the spawn -- which the fake
     * installed around this launch caught instead of performing. Both halves
     * are checked: the child was told yes, and this side really was asked.
     * spec-app-services.md 9.9. */
    check_kv(report, "poweroff", "1");
    CHECK(halt_fake_wait_called(&g_child_halt));
    CHECK(!g_child_halt.reboot);
    CHECK_STR(g_child_halt.exe, g_stub_poweroff);
}

/* Launching twice must work: the socketpair and the thread are per launch,
 * so anything the first left behind would show up here as a hang or a
 * report from the wrong run. */
static void test_two_launches_in_a_row(core_fixture *fx)
{
    nd_app_entry app;
    char resolved[ND_PATH_MAX];
    char report[REPORT_BYTES];

    if (nd_path_resolve(resolved, sizeof resolved, SVC_REPORT) == ND_OK)
        (void)unlink(resolved);

    app_entry_for(&app, SVC_APP_DIR, "SvcApp");
    CHECK_INT(nd_proc_launch_app(&fx->ui, &app, NULL, NULL, NULL), ND_OK);
    CHECK(pt_read_text(SVC_REPORT, report, sizeof report) != (size_t)-1);
    check_kv(report, "done", "1");
    check_kv(report, "sms_ok", "1");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

/* ============ "NO SOCKET" MUST NOT MEAN "I AM THE CORE" ============
 *
 * Every verb works on both sides of the boundary, and the side used to be
 * decided by "do I have a client socket?". An untrusted app has none BECAUSE
 * nd_proc_launch_app refused it one -- so it fell through into the core's
 * branch and tried to act locally.
 *
 * Nothing was ever actually rebooted: the kernel stopped an ndusr_ut process
 * with no_new_privs from gaining anything by exec'ing setuid busybox. But
 * halt_perform() does not check its child, so nd_svc_reboot() RETURNED TRUE,
 * and the confinement probe printed "*** ALLOWED ***" for rebooting the phone.
 * The boundary held and the library lied about it.
 *
 * Found by running the probe on a phone, not by reading the code. */
static void test_an_app_with_no_socket_is_not_the_core(void)
{
    /* No NEODCT_SERVICE_FD, and marked as an app: the untrusted case exactly. */
    (void)unsetenv(ND_ENV_SERVICE_FD);
    nd_svc_client_open_from_env();
    CHECK(!nd_svc_client_active());
    nd_svc_mark_app_process();

    /* Each of the three that can act locally must refuse rather than try. */
    CHECK(!nd_svc_reboot());
    CHECK(!nd_svc_poweroff());
    CHECK(!nd_svc_set_clock((time_t)1893456000)); /* 2030, comfortably in range */
    CHECK(!nd_svc_format_card());
}

int main(void)
{
    core_fixture fx;

    /* This test stands in for nd-core, so it does what nd_main.c does: a key
     * written down the app channel while the child is mid-exit raises
     * SIGPIPE, and the default disposition would kill it. nd_input.h's
     * contract on nd_input_channel_send() spells this out. */
    (void)signal(SIGPIPE, SIG_IGN);

    if (!stage_root()) {
        fprintf(stderr, "test_svc: cannot stage a root (step %d); skipping\n", g_stage_step);
        return 0;
    }

    test_no_route_at_all();

    if (!core_init(&fx)) {
        fprintf(stderr, "test_svc: cannot build a core fixture; skipping the rest\n");
        core_free(&fx);
        drop_stage();
        return g_failures == 0 ? 0 : 1;
    }

    test_a_direct_handle_wins(&fx);
    test_loopback_over_a_live_server(&fx);
    test_garbage_does_not_take_the_core_down(&fx);
    test_hardware_true_survives_the_wire();
    test_the_halt_tables_are_the_pythons();
    test_the_halt_lookup_is_execvps();
    test_a_halt_over_the_wire(&fx);
    test_a_halt_with_nothing_to_spawn(&fx);
    test_the_clock_over_the_wire(&fx);
    test_a_format_over_the_wire(&fx);
    test_an_untrusted_uid_is_still_served(&fx);
    test_an_app_with_no_socket_is_not_the_core();

    /* SvcApp asks for a poweroff, and the launcher's own server thread is
     * the one that serves it -- so the fake has to be installed around the
     * launch and not around a server this file opened. Non-blocking: the
     * ordering handshake belongs to the loopback case, which can hold both
     * ends; here the child is a separate process and there is nobody to
     * release it. */
    {
        nd_svc_halt_sim sim;

        halt_fake_init(&g_child_halt);
        memset(&sim, 0, sizeof sim);
        sim.spawn = halt_fake_spawn;
        sim.user = &g_child_halt;
        sim.reboot = g_stub_reboot_tab;
        sim.poweroff = g_stub_poweroff_tab;
        sim.n = 1u;
        if (g_stub_poweroff[0] != '\0')
            nd_svc_halt_simulate(&sim);

        test_a_real_child_reaches_the_core(&fx);
        test_two_launches_in_a_row(&fx);

        nd_svc_halt_simulate(NULL);
        halt_fake_free(&g_child_halt);
    }

    /* Whatever the cases did, the client must be shut before the process
     * ends -- an app's nd_ui_teardown() is what does this on the phone. */
    nd_svc_client_close();
    CHECK(!nd_svc_client_active());

    core_free(&fx);
    drop_stage();
    printf("test_svc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
