/* apps/Downgrade/main.c -- install any published release, not just the newest
 * one. App id 9006, engineering menu.
 *
 * A port of System/engineering/apps/Downgrade/main.py (160 lines). Its
 * docstring is the specification and is reproduced rather than summarised,
 * because it is the safety argument for the whole app:
 *
 *     Downgrade: install any published release, not just the newest one.
 *
 *     The Update app deliberately only offers what is newer. This one lists
 *     every release that carries a package for this phone and lets you pick,
 *     which is what you want when a new version broke something and you need
 *     to get back to the one that worked.
 *
 *     It is an engineering tool on purpose. Going backwards is a real thing
 *     to want during development, and a genuinely dangerous thing to offer a
 *     phone's owner by accident:
 *
 *       * An update replaces the whole root filesystem. Going back to an
 *         older version is not "undo" -- it is installing a different system
 *         that happens to be older, and anything the newer one changed on the
 *         user partition stays changed.
 *
 *       * Older packages can carry bugs that were fixed for good reasons.
 *         0.3.4a and 0.3.5a leak QMI clients until the modem stops answering,
 *         which needs a power cycle to clear.
 *
 *     Everything past "pick one" is the ordinary update path: the same
 *     signature check, the same staging, the same applier. Nothing here
 *     installs anything itself.
 *
 * ============ THIS PORT IS INCOMPLETE, ON PURPOSE ============
 *
 * "Everything past 'pick one' is the ordinary update path" -- and that path
 * has no C implementation. Neither does the release list that "pick one"
 * picks from. downgrade.h says exactly what is missing, why it is not
 * reimplemented here, and what wiring it up will look like; READ THAT BEFORE
 * CHANGING THIS FILE. In short: there is no libndupdate, no nd_remote, no
 * HTTP client and no TLS anywhere in neodct/src, and SESSION-SCOPE.md puts
 * the update system and its crypto out of scope for this pass deliberately.
 *
 * So app_run() below is steps 1 and 2 of eleven, and then an honest stop.
 * Everything the app itself defines -- every string, every label, every
 * confirmation, every refusal, all three screen shapes -- is ported and
 * exported, and test/unit/test_downgrade.c pins it against the Python.
 *
 * ============ TWO THINGS IN THE PYTHON WORTH KNOWING ============
 *
 * 1. THE RUNNING VERSION IS MARKED, NOT HIDDEN. "Seeing where you are in the
 *    list is most of the point of the list." Picking it is then refused by a
 *    page of its own rather than by leaving it out.
 *
 * 2. THE LAST try/except IS WIDER THAN IT LOOKS. It wraps both the importlib
 *    dance that loads the Update app AND the _install() call inside it, so a
 *    failure DURING the install -- a bad signature, no room to stage --
 *    reports "Downloaded, but could not start the installer." The installer
 *    started fine; it refused. That is the Python's and it is reproduced as
 *    the message for both cases when this is wired up.
 */

#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "downgrade.h"

const char *const nd_downgrade_header = "Downgrade";

const char *const nd_downgrade_no_network =
    "This tool reads the release list from GitHub, so the phone needs a "
    "working data connection.\n\n"
    "Without one, an older package can still be copied onto the card by "
    "hand and installed from Update.";

const char *const nd_downgrade_nothing_title = "Nothing published";
const char *const nd_downgrade_nothing_body = "No release carries a package for this phone yet.";
const char *const nd_downgrade_noconn_title = "No connection";
const char *const nd_downgrade_noconn_subtitle = "Could not reach GitHub";
const char *const nd_downgrade_running_title = "Already running";
const char *const nd_downgrade_running_body = "That is the version this phone is running.";

/* Not the Python's. See downgrade.h: the phone is in neither of the two
 * states the Python has a page for, and borrowing one of them would be the
 * app claiming to know something it does not. */
const char *const nd_downgrade_nosvc_title = "No release list";
const char *const nd_downgrade_nosvc_why = "this build has no release reader";
const char *const nd_downgrade_nosvc_body =
    "Downgrade reads the list of published releases through the update "
    "service, and this build does not have one.\n\n"
    "Nothing was changed.";

const char *const nd_downgrade_no_folder = "The card has no update folder.";

const char *const nd_downgrade_button_downgrade = "Downgrade";
const char *const nd_downgrade_button_install = "Install";

/* ENTER = 28, spelled in the Python because it predates a shared table. */
#define DOWNGRADE_KEY_ENTER ND_KEY_ENTER

/* ProgressScreen(ui, "Reading releases", header=HEADER).draw(0, 1) */
#define DOWNGRADE_READING_STEP "Reading releases"

/* ------------------------------------------------------------------ *
 * The builders
 * ------------------------------------------------------------------ */

void nd_downgrade_format_size(char *out, size_t out_sz, int64_t count)
{
    if (out == NULL || out_sz == 0u)
        return;
    /* 1048576.0, not 1e6: the Python divides by 1024*1024 and the difference
     * shows on a 53 MB package (53.0 against 55.6). */
    (void)snprintf(out, out_sz, "%.1f MB", (double)count / 1048576.0);
}

void nd_downgrade_label(char *out, size_t out_sz, const char *version, const char *installed,
                        bool older)
{
    const char *mark = "";

    if (out == NULL || out_sz == 0u)
        return;
    if (version == NULL)
        version = "";
    if (installed == NULL)
        installed = "";

    if (strcmp(version, installed) == 0)
        mark = "  (running)";
    /* `elif installed and not remote.is_newer(...)`. The `installed and` is
     * redundant -- is_newer() returns True for an empty installed version, so
     * `older` is already false there -- and it is kept because it is the
     * Python's own guard and costs nothing. */
    else if (installed[0] != '\0' && older)
        mark = "  (older)";

    (void)snprintf(out, out_sz, "%s%s", version, mark);
}

void nd_downgrade_for_platform(char *out, size_t out_sz, const char *platform)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz, "for %s", (platform != NULL) ? platform : "");
}

void nd_downgrade_neodct_version(char *out, size_t out_sz, const char *installed)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz, "NeoDCT %s", (installed != NULL) ? installed : "");
}

void nd_downgrade_noconn_body(char *out, size_t out_sz, const char *why)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz, "%s\n\n%s", (why != NULL) ? why : "", nd_downgrade_no_network);
}

void nd_downgrade_ask_downgrade(char *out, size_t out_sz, const char *version,
                                const char *installed)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz,
                   "Go back to %s?\nThis replaces the whole system. "
                   "User data is kept but stays as %s left it.",
                   (version != NULL) ? version : "", (installed != NULL) ? installed : "");
}

void nd_downgrade_ask_install(char *out, size_t out_sz, const char *version, int64_t size)
{
    char megabytes[32];

    if (out == NULL || out_sz == 0u)
        return;
    nd_downgrade_format_size(megabytes, sizeof megabytes, size);
    (void)snprintf(out, out_sz, "Install %s?\n%s", (version != NULL) ? version : "", megabytes);
}

void nd_downgrade_download_failed(char *out, size_t out_sz, const char *why)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz, "Download failed.\n%s\n\nNothing was installed.",
                   (why != NULL) ? why : "");
}

void nd_downgrade_installer_failed(char *out, size_t out_sz, const char *why)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz,
                   "Downloaded, but could not start the installer.\n%s\n\n"
                   "The package is on the card; install it from Update.",
                   (why != NULL) ? why : "");
}

/* ------------------------------------------------------------------ *
 * The screens
 * ------------------------------------------------------------------ */

void nd_downgrade_page(nd_ui *ui, const char *title, const char *subtitle, const char *body)
{
    nd_detailpage page;

    if (ui == NULL)
        return;
    /* The only widget that allocates; released with nd_detailpage_free(). */
    if (nd_detailpage_init(&page, ui, title, subtitle, body, ND_DOWNGRADE_ICON, NULL,
                           nd_downgrade_header, "Back") != ND_OK)
        return;
    (void)nd_detailpage_show(&page);
    nd_detailpage_free(&page);
}

bool nd_downgrade_confirm(nd_ui *ui, const char *message, const char *button_text)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_button(&dialog, button_text);
    /* No title: _confirm passes none, so the dialog draws the warning
     * triangle and the message alone. */
    return nd_msgdialog_show(&dialog) == DOWNGRADE_KEY_ENTER;
}

void nd_downgrade_refuse(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;
    static const int32_t accept[] = {ND_KEY_ENTER};

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_button(&dialog, "OK");
    /* cancel_keys=() -- Clear does NOT dismiss this one. */
    nd_msgdialog_set_keys(&dialog, accept, 1u, NULL, 0u);
    (void)nd_msgdialog_show(&dialog);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    char platform[64];
    char installed[64];
    char subtitle[96];
    nd_progress progress;

    if (ui == NULL)
        return 1;

    /* Step 1. platform = get_setting("system.os.platform", "unknown");
     * installed = get_setting("system.os.versionnumber", "") or "". Both are
     * "system.os.*" keys, so they come from version.prop and describe the
     * image rather than a preference (nd_settings.h). */
    (void)nd_settings_get_copy(ND_SET_OS_PLATFORM, "unknown", platform, sizeof platform);
    (void)nd_settings_get_copy(ND_SET_OS_VERSIONNUMBER, "", installed, sizeof installed);

    /* Step 2. The progress screen goes up before the network is touched,
     * which is the Python's order and is honest here too: the app really is
     * about to try, and the answer really does come back immediately. */
    nd_progress_init(&progress, ui, DOWNGRADE_READING_STEP, nd_downgrade_header, NULL, NULL, NULL);
    (void)nd_progress_draw(&progress, 0, 1);

    /* Step 3 is remote.all_releases(platform), and there is nothing in this
     * build to call. See downgrade.h. The log line follows the form
     * nd_main.c uses for a subsystem that is not linked, so that grepping the
     * serial console for one of them finds all of them. */
    nd_log_err(ND_LOG_UPDATE, "release list unavailable: %s (platform %s, running %s)",
               nd_downgrade_nosvc_why, platform, (installed[0] != '\0') ? installed : "unknown");

    nd_downgrade_for_platform(subtitle, sizeof subtitle, platform);
    nd_downgrade_page(ui, nd_downgrade_nosvc_title, subtitle, nd_downgrade_nosvc_body);
    return 0;
}

/* Nothing is held: no sound card, no child process, no open file. The one
 * thing that WOULD need releasing here is the DetailPage's block array, and
 * nd_downgrade_page() frees it before it returns rather than leaving it to a
 * teardown that must not allocate or free much (nd_app.h). Exported because
 * nd_app.h requires every app to export one. */
void app_shutdown(void) {}
