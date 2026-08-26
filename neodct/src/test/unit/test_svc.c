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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_modem.h"
#include "nd_paths.h"
#include "nd_proc.h"
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
                (void)write(k->master, SENT, strlen(SENT));
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
                (void)write(k->master, reply, strlen(reply));
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
 * 5. A real child process, launched by the real launcher
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
    test_a_real_child_reaches_the_core(&fx);
    test_two_launches_in_a_row(&fx);

    /* Whatever the cases did, the client must be shut before the process
     * ends -- an app's nd_ui_teardown() is what does this on the phone. */
    nd_svc_client_close();
    CHECK(!nd_svc_client_active());

    core_free(&fx);
    drop_stage();
    printf("test_svc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
