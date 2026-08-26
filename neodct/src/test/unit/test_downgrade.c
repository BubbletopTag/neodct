/* test_downgrade.c -- the Downgrade app, app id 9006.
 *
 * ============ HOW THE NETWORK HALF IS DRIVEN ============
 *
 * It is not stubbed. The transport under nd_remote_all_releases() is a
 * SPAWNED curl, so this test does what test_remote.c does: it puts the
 * committed stand-in (neodct/tests/remote/fake-curl) first on PATH and lets
 * the app find it with the library's own PATH walk. The app runs its real
 * code -- the real argv, the real pipes, the real JSON parse -- against
 * neodct/tests/remote/releases.json, which is a GitHub /releases listing in
 * GitHub's own shape, carrying UPDATE-luckfox-armv7.ndsw and
 * UPDATE-qemu-aarch64.ndsw exactly as a published release does.
 *
 * NO TEST HERE MAKES A NETWORK REQUEST.
 *
 * What is NOT driven is the far end of the app: the confirmation, the
 * download and the handoff to the Update app's installer. Those need a card,
 * 60 MB of package and a second app.so to dlopen; nd_remote's own download
 * path is pinned by test_remote.c and the installer by test_update_app.c,
 * and a third copy of either here would test the fixture rather than the
 * app. What this test pins is the part that is this app's: which page each
 * outcome of the scan draws, and what the release list says.
 *
 * What IS checked, asserted against the strings the Python actually produces
 * -- computed by running the expressions out of
 * engineering/apps/Downgrade/main.py, not guessed:
 *
 *   1. The verbatim strings: NO_NETWORK, the three page titles and bodies,
 *      "The card has no update folder.", both button labels, APP_ID and the
 *      icon path.
 *
 *   2. _format_size(): "%.1f MB" over 1048576, including the two cases where
 *      a megabyte and a million bytes disagree.
 *
 *   3. The release label and its two marks, INCLUDING the two leading spaces
 *      inside each one, and including the Python's redundant `installed and`
 *      guard on the "(older)" branch.
 *
 *   4. The four message builders: both confirmations and both post-download
 *      refusals.
 *
 *   5. _confirm() is Yes only on ENTER, and _page() and _refuse() return.
 *
 *   6. app_run()'s three scan outcomes: a listing with nothing for this
 *      platform draws "Nothing published", a curl that cannot connect draws
 *      "No connection", and a listing that does carry this platform's asset
 *      draws the release list with its "(running)" and "(older)" marks.
 *
 * _refuse()'s other half, that Clear does NOT dismiss it, is deliberately not
 * driven: the dialog has no cancel keys, so a test that pressed Clear at it
 * would hang rather than fail. The empty-cancel-set behaviour belongs to
 * nd_msgdialog and is pinned by test_widgets_dialogs.c.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (for the
 * font); the scratch root is the Makefile's.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smallapp_test.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_remote.h"

#include "../../apps/Downgrade/downgrade.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    void (*format_size)(char *, size_t, int64_t);
    void (*label)(char *, size_t, const char *, const char *, bool);
    void (*for_platform)(char *, size_t, const char *);
    void (*neodct_version)(char *, size_t, const char *);
    void (*noconn_body)(char *, size_t, const char *);
    void (*ask_downgrade)(char *, size_t, const char *, const char *);
    void (*ask_install)(char *, size_t, const char *, int64_t);
    void (*download_failed)(char *, size_t, const char *);
    void (*installer_failed)(char *, size_t, const char *);
    void (*downloading_step)(char *, size_t, const char *);
    int64_t (*progress_total)(int64_t, int64_t);
    void (*page)(nd_ui *, const char *, const char *, const char *);
    bool (*confirm)(nd_ui *, const char *, const char *);
    void (*refuse)(nd_ui *, const char *);
    const char *const *header;
    const char *const *no_network;
    const char *const *nothing_title;
    const char *const *nothing_body;
    const char *const *noconn_title;
    const char *const *noconn_subtitle;
    const char *const *running_title;
    const char *const *running_body;
    const char *const *list_title;
    const char *const *no_folder;
    const char *const *button_downgrade;
    const char *const *button_install;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.format_size = sa_sym(h, "nd_downgrade_format_size");
    *(void **)&api.label = sa_sym(h, "nd_downgrade_label");
    *(void **)&api.for_platform = sa_sym(h, "nd_downgrade_for_platform");
    *(void **)&api.neodct_version = sa_sym(h, "nd_downgrade_neodct_version");
    *(void **)&api.noconn_body = sa_sym(h, "nd_downgrade_noconn_body");
    *(void **)&api.ask_downgrade = sa_sym(h, "nd_downgrade_ask_downgrade");
    *(void **)&api.ask_install = sa_sym(h, "nd_downgrade_ask_install");
    *(void **)&api.download_failed = sa_sym(h, "nd_downgrade_download_failed");
    *(void **)&api.installer_failed = sa_sym(h, "nd_downgrade_installer_failed");
    *(void **)&api.downloading_step = sa_sym(h, "nd_downgrade_downloading_step");
    *(void **)&api.progress_total = sa_sym(h, "nd_downgrade_progress_total");
    *(void **)&api.page = sa_sym(h, "nd_downgrade_page");
    *(void **)&api.confirm = sa_sym(h, "nd_downgrade_confirm");
    *(void **)&api.refuse = sa_sym(h, "nd_downgrade_refuse");

    api.header = dlsym(h, "nd_downgrade_header");
    api.no_network = dlsym(h, "nd_downgrade_no_network");
    api.nothing_title = dlsym(h, "nd_downgrade_nothing_title");
    api.nothing_body = dlsym(h, "nd_downgrade_nothing_body");
    api.noconn_title = dlsym(h, "nd_downgrade_noconn_title");
    api.noconn_subtitle = dlsym(h, "nd_downgrade_noconn_subtitle");
    api.running_title = dlsym(h, "nd_downgrade_running_title");
    api.running_body = dlsym(h, "nd_downgrade_running_body");
    api.list_title = dlsym(h, "nd_downgrade_list_title");
    api.no_folder = dlsym(h, "nd_downgrade_no_folder");
    api.button_downgrade = dlsym(h, "nd_downgrade_button_downgrade");
    api.button_install = dlsym(h, "nd_downgrade_button_install");

    return api.run != NULL && api.shutdown != NULL && api.format_size != NULL &&
           api.label != NULL && api.for_platform != NULL && api.neodct_version != NULL &&
           api.noconn_body != NULL && api.ask_downgrade != NULL && api.ask_install != NULL &&
           api.download_failed != NULL && api.installer_failed != NULL && api.page != NULL &&
           api.confirm != NULL && api.refuse != NULL && api.header != NULL &&
           api.no_network != NULL && api.nothing_title != NULL && api.nothing_body != NULL &&
           api.noconn_title != NULL && api.noconn_subtitle != NULL && api.running_title != NULL &&
           api.running_body != NULL && api.list_title != NULL && api.no_folder != NULL &&
           api.button_downgrade != NULL && api.button_install != NULL &&
           api.downloading_step != NULL && api.progress_total != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. The verbatim strings
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    CHECK_INT(ND_DOWNGRADE_APP_ID, 9006, "APP_ID");
    CHECK_STR(ND_DOWNGRADE_ICON, "/NeoDCT/System/engineering/apps/Downgrade/icon.png", "APP_ICON");
    CHECK_STR(*api.header, "Downgrade", "HEADER");

    /* NO_NETWORK, whose two implicit concatenations in the Python join
     * WITHOUT a space at the seam of each pair -- "needs a " + "working" and
     * "card by " + "hand". */
    CHECK_STR(*api.no_network,
              "This tool reads the release list from GitHub, so the phone needs a working data "
              "connection.\n\nWithout one, an older package can still be copied onto the card by "
              "hand and installed from Update.",
              "NO_NETWORK");

    CHECK_STR(*api.nothing_title, "Nothing published", "NoRelease page title");
    CHECK_STR(*api.nothing_body, "No release carries a package for this phone yet.",
              "NoRelease page body");
    CHECK_STR(*api.noconn_title, "No connection", "NetworkError page title");
    CHECK_STR(*api.noconn_subtitle, "Could not reach GitHub", "NetworkError page subtitle");
    CHECK_STR(*api.running_title, "Already running", "picked the running version, title");
    CHECK_STR(*api.running_body, "That is the version this phone is running.",
              "picked the running version, body");

    CHECK_STR(*api.no_folder, "The card has no update folder.", "_refuse, no update folder");
    CHECK_STR(*api.button_downgrade, "Downgrade", "going-back button");
    CHECK_STR(*api.button_install, "Install", "going-forward button");

    /* VerticalList(ui, "Releases", ...) -- the list's title, which is NOT
     * the "Downgrade" header every page and progress screen carries. */
    CHECK_STR(*api.list_title, "Releases", "the release list's title");
    CHECK(strcmp(*api.list_title, *api.header) != 0, "and it is not the header");
}

/* ------------------------------------------------------------------ *
 * 2. _format_size()
 * ------------------------------------------------------------------ */

static void test_format_size(void)
{
    char out[32];

    api.format_size(out, sizeof out, 0);
    CHECK_STR(out, "0.0 MB", "zero bytes");
    api.format_size(out, sizeof out, 1);
    CHECK_STR(out, "0.0 MB", "one byte still rounds to 0.0");
    api.format_size(out, sizeof out, 1048576);
    CHECK_STR(out, "1.0 MB", "one mebibyte");
    api.format_size(out, sizeof out, 1572864);
    CHECK_STR(out, "1.5 MB", "one and a half");

    /* A real package. 55,574,528 is 53.0 MiB and 55.6 MB -- the divisor is
     * why this is asserted rather than eyeballed. */
    api.format_size(out, sizeof out, 55574528);
    CHECK_STR(out, "53.0 MB", "a 53 MiB package");
    api.format_size(out, sizeof out, 56098816);
    CHECK_STR(out, "53.5 MB", "half a mebibyte more");
}

/* ------------------------------------------------------------------ *
 * 3. The release label
 * ------------------------------------------------------------------ */

static void test_label(void)
{
    char out[ND_DOWNGRADE_LABEL_MAX];

    /* "The running version is marked rather than hidden. Seeing where you are
     * in the list is most of the point of the list." */
    api.label(out, sizeof out, "0.3.7a", "0.3.7a", false);
    CHECK_STR(out, "0.3.7a  (running)", "the running version, two leading spaces");
    api.label(out, sizeof out, "0.3.7a", "0.3.7a", true);
    CHECK_STR(out, "0.3.7a  (running)", "(running) wins over (older)");

    api.label(out, sizeof out, "0.3.2a", "0.3.7a", true);
    CHECK_STR(out, "0.3.2a  (older)", "an older release");
    api.label(out, sizeof out, "0.4.0a", "0.3.7a", false);
    CHECK_STR(out, "0.4.0a", "a newer release carries no mark");

    /* is_newer() returns True for an empty installed version, so `older` is
     * false there anyway; the Python's `installed and` guard makes it true
     * twice over, and both belts are worn here. */
    api.label(out, sizeof out, "0.3.2a", "", true);
    CHECK_STR(out, "0.3.2a", "no installed version -> no mark, even when told older");
    api.label(out, sizeof out, "", "", false);
    CHECK_STR(out, "  (running)", "the empty version equals an empty installed one");
}

/* ------------------------------------------------------------------ *
 * 4. The message builders
 * ------------------------------------------------------------------ */

static void test_messages(void)
{
    char out[ND_DOWNGRADE_MSG_MAX];

    api.for_platform(out, sizeof out, "luckfox");
    CHECK_STR(out, "for luckfox", "NoRelease subtitle");
    api.neodct_version(out, sizeof out, "0.3.7a");
    CHECK_STR(out, "NeoDCT 0.3.7a", "Already running subtitle");

    api.noconn_body(out, sizeof out, "cannot reach GitHub: [Errno -3]");
    CHECK_STR(out,
              "cannot reach GitHub: [Errno -3]\n\nThis tool reads the release list from GitHub, "
              "so the phone needs a working data connection.\n\nWithout one, an older package can "
              "still be copied onto the card by hand and installed from Update.",
              "the exception text, a blank line, then NO_NETWORK");

    /* Spelled out rather than a bare "are you sure": "the consequence is not
     * obvious, and this tool exists to be used deliberately." */
    api.ask_downgrade(out, sizeof out, "0.3.2a", "0.3.7a");
    CHECK_STR(out,
              "Go back to 0.3.2a?\nThis replaces the whole system. User data is kept but stays as "
              "0.3.7a left it.",
              "the downgrade confirmation");

    api.ask_install(out, sizeof out, "0.4.0a", 55574528);
    CHECK_STR(out, "Install 0.4.0a?\n53.0 MB", "the install confirmation carries the size");

    api.download_failed(out, sizeof out, "download stopped early (12 of 34 bytes)");
    CHECK_STR(out,
              "Download failed.\ndownload stopped early (12 of 34 bytes)\n\nNothing was installed.",
              "the download refusal");

    /* This one covers BOTH "the installer would not load" and "the installer
     * ran and refused the package": the Python's try block wraps the call as
     * well as the import. */
    api.installer_failed(out, sizeof out, "BAD SIGNATURE");
    CHECK_STR(out,
              "Downloaded, but could not start the installer.\nBAD SIGNATURE\n\nThe package is on "
              "the card; install it from Update.",
              "the installer refusal");
}

/* ------------------------------------------------------------------ *
 * 5 and 6. The screens
 * ------------------------------------------------------------------ */

static void test_confirm(void)
{
    sa_fixture fx;

    /* MessageDialog drains the channel before it draws, so the answer has to
     * arrive as a repeat. ENTER is Yes. */
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    CHECK(api.confirm(&fx.ui, "Install 0.4.0a?\n53.0 MB", "Install"), "ENTER is Yes");
    nd_vclock_disable();
    sa_fx_free(&fx);

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(sa_hold(&fx, ND_KEY_CLEAR), "held CLEAR");
    nd_vclock_enable();
    CHECK(!api.confirm(&fx.ui, "Go back to 0.3.2a?", "Downgrade"), "CLEAR is No");
    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_refuse_and_page(void)
{
    sa_fixture fx;

    /* _refuse has NO cancel keys, so ENTER is the only way out of it. */
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    api.refuse(&fx.ui, *api.no_folder);
    nd_vclock_disable();
    sa_checks++;
    sa_fx_free(&fx);

    /* _page is a DetailPage, which does NOT drain before drawing, so a queued
     * press is enough. It is also the one widget that allocates; running it
     * under ASan is what proves nd_downgrade_page() frees the block array. */
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");
    nd_vclock_enable();
    api.page(&fx.ui, *api.nothing_title, "for luckfox", *api.nothing_body);
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the page");
    sa_fx_free(&fx);
}

static void test_progress(void)
{
    char out[64];

    api.downloading_step(out, sizeof out, "0.3.11a");
    CHECK_STR(out, "Downloading 0.3.11a", "the ProgressScreen step for the download");
    api.downloading_step(out, sizeof out, NULL);
    CHECK_STR(out, "Downloading ", "a NULL version is the empty one");
    api.downloading_step(NULL, 0u, "x");

    /* `total or picked["size"] or 1`, and `or` on an int is falsy at zero. */
    CHECK_INT(api.progress_total(4096, 999), 4096, "a length from the server wins");
    CHECK_INT(api.progress_total(0, 999), 999, "no length falls back to the listing's size");
    CHECK_INT(api.progress_total(0, 0), 1, "neither is 1, because draw() divides by it");
}

/* ------------------------------------------------------------------ *
 * 7. app_run(): the three ways the scan can end
 * ------------------------------------------------------------------ *
 *
 * The stand-in curl, exactly as test_remote.c installs it. See this file's
 * header: nothing here reaches the network, and nothing in the library is
 * stubbed -- the executable is replaced, not the code.
 */

static char g_ctl[ND_PATH_MAX];      /* the stand-in's control files */
static char g_fixtures[ND_PATH_MAX]; /* neodct/tests/remote          */
static bool g_curl_ready;

static bool shell(const char *fmt, ...) ND_PRINTF(1, 2);

static bool shell(const char *fmt, ...)
{
    char cmd[ND_PATH_MAX * 3];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return false;
    return system(cmd) == 0;
}

/* The fixtures sit beside the source tree and not under a phone root, so
 * this walks up from the test binary the way test_remote.c does. */
static bool fake_curl_install(void)
{
    char self[ND_PATH_MAX];
    char bindir[ND_PATH_MAX];
    char sandbox[ND_PATH_MAX];
    char path[ND_PATH_MAX * 2];
    const char *old_path = getenv("PATH");
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", self, sizeof self - 1u);
    if (n <= 0)
        return false;
    self[n] = '\0';
    slash = strrchr(self, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    /* build/<variant>/test -> ../../../../tests/remote */
    if (nd_snprintf(g_fixtures, sizeof g_fixtures, "%s/../../../../tests/remote", self) != ND_OK)
        return false;

    if (!sa_tmpdir("nddgcurl", sandbox, sizeof sandbox))
        return false;
    if (nd_snprintf(bindir, sizeof bindir, "%s/bin", sandbox) != ND_OK ||
        nd_snprintf(g_ctl, sizeof g_ctl, "%s/ctl", sandbox) != ND_OK)
        return false;
    if (!shell("mkdir -p '%s' '%s'", bindir, g_ctl))
        return false;
    if (!shell("cp '%s/fake-curl' '%s/curl' && chmod 0755 '%s/curl'", g_fixtures, bindir, bindir))
        return false;
    if (nd_snprintf(path, sizeof path, "%s:%s", bindir,
                    (old_path != NULL) ? old_path : "/usr/bin:/bin") != ND_OK)
        return false;
    return setenv("PATH", path, 1) == 0 && setenv("NDCURL_DIR", g_ctl, 1) == 0;
}

static void ctl_set(const char *name, const char *value)
{
    char p[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(p, sizeof p, "%s/%s", g_ctl, name) != ND_OK)
        return;
    f = fopen(p, "w");
    if (f == NULL)
        return;
    if (value != NULL)
        (void)fputs(value, f);
    (void)fclose(f);
}

/* Wipe the scenario. Every case starts clean. */
static void scenario_reset(void)
{
    (void)shell("rm -f '%s'/*", g_ctl);
}

static void ctl_body(const char *fixture_name)
{
    char p[ND_PATH_MAX];

    if (nd_snprintf(p, sizeof p, "%s/%s", g_fixtures, fixture_name) != ND_OK)
        return;
    ctl_set("body", p);
}

/* version.prop describes the IMAGE, and both settings the app reads are
 * "system.os.*" keys, so this is where they come from. Writing it under the
 * scratch root is how a test picks the platform the scan asks for. */
static void write_version_prop(const char *platform, const char *version)
{
    const char *virt = ND_PATH_VERSION_PROP;
    char resolved[ND_PATH_MAX];
    char dir[ND_PATH_MAX];
    const char *slash = strrchr(virt, '/');
    FILE *f;

    if (slash != NULL && slash != virt) {
        (void)nd_strlcpy(dir, virt, (size_t)(slash - virt) + 1u);
        if (nd_mkdir_p(dir, 0755u) != ND_OK) {
            CHECK(false, "mkdir -p for version.prop");
            return;
        }
    }
    if (nd_path_resolve(resolved, sizeof resolved, virt) != ND_OK) {
        CHECK(false, "resolve version.prop");
        return;
    }
    f = fopen(resolved, "wb");
    if (f == NULL) {
        CHECK(false, "open version.prop");
        return;
    }
    (void)fprintf(f, "system.os.platform=%s\nsystem.os.versionnumber=%s\n", platform, version);
    (void)fclose(f);
    (void)nd_settings_init();
}

/* One scan, with `keys` pressed at whatever it draws. Returns the frame
 * count; *rc_out receives app_run's return and *digest_out, when it is not
 * NULL, receives a hash of the LAST frame drawn.
 *
 * The digest is what makes these tests mean anything. A frame COUNT does not
 * discriminate: pressing three keys at a DetailPage that failed to load a
 * release list draws about as many frames as pressing three keys at a list
 * that loaded, so an assertion on the count passes just as happily when the
 * scan never happened -- which is exactly what a mutation that stubbed
 * nd_remote_all_releases() out proved. The hash of the final screen does
 * discriminate, because the "No connection" page and a list of releases do
 * not look alike. */
#define DG_DIGEST_MAX 96

static uint64_t run_scan(const int32_t *keys, size_t n_keys, int *rc_out, char *digest_out,
                         size_t digest_sz, char *after_scan_out, size_t after_scan_sz)
{
    sa_fixture fx;
    const nd_image *last;
    uint64_t frames;
    size_t i;

    if (digest_out != NULL && digest_sz > 0u)
        digest_out[0] = '\0';
    if (after_scan_out != NULL && after_scan_sz > 0u)
        after_scan_out[0] = '\0';
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        *rc_out = -1;
        return 0u;
    }
    for (i = 0u; i < n_keys; i++)
        CHECK(sa_send(&fx, keys[i]), "queued a key");

    *rc_out = api.run(&fx.ui);

    frames = nd_capture_frames_drawn(fx.cap);
    if (digest_out != NULL && digest_sz > 0u) {
        last = nd_capture_recent(fx.cap, 0u);
        if (last == NULL || nd_capture_digest(last, digest_out, digest_sz) != ND_OK)
            digest_out[0] = '\0';
    }
    /* Frame 1 (0-based) -- whatever replaced "Reading releases" the instant
     * the scan came back, before any key moved anything. That frame is a
     * failure PAGE or it is the release LIST, and nothing else, which is
     * what makes it the assertion worth writing. */
    if (after_scan_out != NULL && after_scan_sz > 0u && frames >= 2u) {
        last = nd_capture_recent(fx.cap, (size_t)(frames - 2u));
        if (last == NULL || nd_capture_digest(last, after_scan_out, after_scan_sz) != ND_OK)
            after_scan_out[0] = '\0';
    }
    sa_fx_free(&fx);
    return frames;
}

/* The digest of a page drawn straight from the app's own _page(), with no
 * scan involved -- the oracle a scan outcome is compared against. */
static bool page_digest(char *out, size_t out_sz, const char *title, const char *subtitle,
                        const char *body)
{
    sa_fixture fx;
    const nd_image *last;
    bool ok;

    out[0] = '\0';
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return false;
    }
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");
    nd_vclock_enable();
    api.page(&fx.ui, title, subtitle, body);
    nd_vclock_disable();

    last = nd_capture_recent(fx.cap, 0u);
    ok = last != NULL && nd_capture_digest(last, out, out_sz) == ND_OK;
    sa_fx_free(&fx);
    return ok;
}

/* Ask nd_remote the same question the app just asked, under the scenario
 * that is already armed, and keep the reason it gives. Used to build the
 * expected page without copying another module's wording into this file. */
static bool scan_reason(char *out, size_t out_sz)
{
    char platform[64];
    nd_release *list;
    size_t n = 0u;
    bool got;

    out[0] = '\0';
    (void)nd_settings_get_copy(ND_SET_OS_PLATFORM, "unknown", platform, sizeof platform);
    list = (nd_release *)calloc(4u, sizeof *list);
    if (list == NULL) {
        CHECK(false, "calloc");
        return false;
    }
    got = nd_remote_all_releases(platform, ND_RELEASES_LIMIT, list, 4u, &n, out, out_sz) ==
          ND_UPD_ERR_NETWORK;
    free(list);
    CHECK(got, "the armed scenario is a network failure");
    return got && out[0] != '\0';
}

/* GitHub answered, and nothing it published carries an asset for THIS
 * phone: remote.NoRelease, which is the "Nothing published" page. The
 * platform is left at SettingsStorage's "unknown", so the asset the scan
 * looks for is UPDATE-unknown.ndsw and releases.json has no such thing --
 * which is the real condition, reached the real way. */
static void test_run_nothing_published(void)
{
    const int32_t keys[] = {ND_KEY_CLEAR};
    char got[DG_DIGEST_MAX];
    char want[DG_DIGEST_MAX];
    int rc = -1;
    uint64_t frames;

    scenario_reset();
    ctl_body("releases.json");
    frames = run_scan(keys, 1u, &rc, got, sizeof got, NULL, 0u);

    CHECK_INT(rc, 0, "app_run returns 0");
    CHECK_INT((int64_t)frames, 2, "the progress screen, then one page");
    if (page_digest(want, sizeof want, *api.nothing_title, "for unknown", *api.nothing_body))
        CHECK_STR(got, want, "and the page is 'Nothing published' for this platform");
}

/* remote.NetworkError: curl's exit 6, "Could not resolve host". */
static void test_run_no_connection(void)
{
    const int32_t keys[] = {ND_KEY_CLEAR};
    char got[DG_DIGEST_MAX];
    char want[DG_DIGEST_MAX];
    char body[ND_DOWNGRADE_MSG_MAX];
    char reason[ND_REMOTE_WHY_MAX];
    int rc = -1;
    uint64_t frames;

    scenario_reset();
    ctl_set("exit", "6");
    ctl_set("stderr", "curl: (6) Could not resolve host: api.github.com\n");
    frames = run_scan(keys, 1u, &rc, got, sizeof got, NULL, 0u);

    CHECK_INT(rc, 0, "app_run returns 0");
    CHECK_INT((int64_t)frames, 2, "the progress screen, then one page");

    /* str(exc) reaches the page. The reason's WORDING is nd_remote's and is
     * pinned by test_remote.c, so this asks the same library the same
     * question and expects the app to have put that answer on the screen --
     * rather than restating another module's string here, where it would go
     * stale silently. */
    if (scan_reason(reason, sizeof reason)) {
        api.noconn_body(body, sizeof body, reason);
        if (page_digest(want, sizeof want, *api.noconn_title, *api.noconn_subtitle, body))
            CHECK_STR(got, want, "and the page is 'No connection', carrying curl's reason");
    }
}

/* The whole point of the app: a listing that DOES carry this platform's
 * asset becomes a list to pick from. releases.json holds four releases and
 * three of them carry UPDATE-luckfox-armv7.ndsw, so there is a list to walk.
 *
 * The assertion is that the last frame is NOT either failure page. That is
 * the discriminator a frame count is not -- three keys pressed at a
 * DetailPage draw about as many frames as three keys pressed at a list. */
static void test_run_lists_releases(void)
{
    const int32_t keys[] = {ND_KEY_DOWN, ND_KEY_DOWN, ND_KEY_CLEAR};
    char got[DG_DIGEST_MAX];
    char after[DG_DIGEST_MAX];
    char nothing[DG_DIGEST_MAX];
    char noconn[DG_DIGEST_MAX];
    char body[ND_DOWNGRADE_MSG_MAX];
    int rc = -1;

    scenario_reset();
    ctl_body("releases.json");
    write_version_prop("luckfox-armv7", "0.3.11a");
    (void)run_scan(keys, 3u, &rc, got, sizeof got, after, sizeof after);

    CHECK_INT(rc, 0, "app_run returns 0 when the list is backed out of");
    CHECK(after[0] != '\0', "a frame was drawn after the scan");

    /* The frame the scan produced is neither failure page, so it is the
     * list. Asserted on THAT frame and not on the last one: three keys
     * pressed at a scrollable DetailPage move it, so the last frame differs
     * from both pages even when the scan failed -- which a mutation that
     * stubbed the scanner out proved by surviving the weaker check. */
    api.noconn_body(body, sizeof body, "");
    if (page_digest(nothing, sizeof nothing, *api.nothing_title, "for luckfox-armv7",
                    *api.nothing_body))
        CHECK(strcmp(after, nothing) != 0, "the scan did not draw 'Nothing published'");
    if (page_digest(noconn, sizeof noconn, *api.noconn_title, *api.noconn_subtitle, body))
        CHECK(strcmp(after, noconn) != 0, "nor 'No connection'");
}

/* Picking the version the phone is already running is refused by a page of
 * its own rather than by leaving it out of the list -- "seeing where you are
 * in the list is most of the point of the list".
 *
 * releases.json is deliberately NOT in version order and all_releases()
 * keeps GitHub's order, so the three luckfox releases arrive as 0.3.2a,
 * 0.3.11a, 0.3.10a. One Down reaches 0.3.11a, which is what version.prop
 * says is running; Enter on it is the refusal. Asserting the digest of that
 * page is what proves the scan really returned 0.3.11a in that position --
 * this is the one test here that pins the list's CONTENTS and not just its
 * existence. */
static void test_run_already_running(void)
{
    const int32_t keys[] = {ND_KEY_DOWN, ND_KEY_ENTER, ND_KEY_CLEAR};
    char got[DG_DIGEST_MAX];
    char want[DG_DIGEST_MAX];
    char subtitle[96];
    int rc = -1;

    scenario_reset();
    ctl_body("releases.json");
    write_version_prop("luckfox-armv7", "0.3.11a");
    (void)run_scan(keys, 3u, &rc, got, sizeof got, NULL, 0u);

    CHECK_INT(rc, 0, "app_run returns 0");
    api.neodct_version(subtitle, sizeof subtitle, "0.3.11a");
    if (page_digest(want, sizeof want, *api.running_title, subtitle, *api.running_body))
        CHECK_STR(got, want, "row 1 is 0.3.11a, and picking it is refused as 'Already running'");
}

static void test_null_safety(void)
{
    char out[8];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */

    /* Every builder takes a caller buffer, and a NULL one must not fault --
     * these are called from screens that are already reporting a failure. */
    api.format_size(NULL, 0u, 1);
    api.label(NULL, 0u, "a", "b", true);
    api.ask_install(NULL, 0u, "a", 1);
    api.label(out, sizeof out, NULL, NULL, false);
    CHECK_STR(out, "  (runn", "a NULL version is the empty one, truncated to fit");
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("Downgrade", "nddowngrade");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    g_curl_ready = fake_curl_install();

    RUN(test_strings);
    RUN(test_format_size);
    RUN(test_label);
    RUN(test_messages);
    RUN(test_confirm);
    RUN(test_refuse_and_page);
    RUN(test_progress);
    if (g_curl_ready) {
        RUN(test_run_nothing_published);
        RUN(test_run_no_connection);
        RUN(test_run_lists_releases);
        RUN(test_run_already_running);
    } else {
        /* Loud rather than silent. A missing stand-in means the scan half of
         * this test did not run, and a green result would be a lie. */
        CHECK(false, "the stand-in curl could not be installed; the scan is untested");
    }
    RUN(test_null_safety);

    return sa_end(h, "test_downgrade");
}
