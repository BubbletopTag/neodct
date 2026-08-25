/* test_downgrade.c -- the Downgrade app, app id 9006.
 *
 * ============ WHAT THIS TEST CANNOT CHECK, AND WHY ============
 *
 * Eight of the app's eleven steps need System/core/UpdateService, which has no
 * C implementation: no nd_remote, no libndupdate, no HTTP client, no TLS. So
 * there is no release list to pick from, nothing to download and nothing to
 * install, and no test here can pretend otherwise. apps/Downgrade/downgrade.h
 * says what is missing in full.
 *
 * What IS checked is everything the Python app defines itself, asserted
 * against the strings the Python actually produces -- computed by running the
 * expressions out of engineering/apps/Downgrade/main.py, not guessed:
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
 *   6. app_run() draws the two frames it can honestly draw -- the "Reading
 *      releases" progress screen and the page that says the release list is
 *      not available in this build -- and returns 0.
 *
 * _refuse()'s other half, that Clear does NOT dismiss it, is deliberately not
 * driven: the dialog has no cancel keys, so a test that pressed Clear at it
 * would hang rather than fail. The empty-cancel-set behaviour belongs to
 * nd_msgdialog and is pinned by test_widgets_dialogs.c.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (for the
 * font); the scratch root is the Makefile's.
 */

#include <stdio.h>
#include <string.h>

#include "smallapp_test.h"

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
    const char *const *nosvc_title;
    const char *const *nosvc_body;
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
    api.nosvc_title = dlsym(h, "nd_downgrade_nosvc_title");
    api.nosvc_body = dlsym(h, "nd_downgrade_nosvc_body");
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
           api.running_body != NULL && api.nosvc_title != NULL && api.nosvc_body != NULL &&
           api.no_folder != NULL && api.button_downgrade != NULL && api.button_install != NULL;
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

    /* The one page that is not the Python's. Its wording is this port's, so
     * the test pins it as this port's rather than as the Python's -- what
     * matters is that it does not claim to be either of the two conditions
     * the Python has a page for. */
    CHECK_STR(*api.nosvc_title, "No release list", "the not-linked page title");
    CHECK(strstr(*api.nosvc_body, "update service") != NULL,
          "the not-linked page says which subsystem is missing");
    CHECK(strcmp(*api.nosvc_title, *api.noconn_title) != 0,
          "and is not the Python's 'No connection' page");
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

static void test_run(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* One press dismisses the only page the app can reach. */
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    nd_vclock_disable();

    CHECK_INT(rc, 0, "app_run returns 0");
    /* Two frames: "Reading releases" at 0%, then the page. Anything more
     * would mean the app had found a release list to show, which it cannot
     * have. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 2, "the progress screen, then one page");
    sa_fx_free(&fx);
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

    RUN(test_strings);
    RUN(test_format_size);
    RUN(test_label);
    RUN(test_messages);
    RUN(test_confirm);
    RUN(test_refuse_and_page);
    RUN(test_run);
    RUN(test_null_safety);

    return sa_end(h, "test_downgrade");
}
