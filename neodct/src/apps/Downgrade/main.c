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
 * ============ WHERE THE FOUR UpdateService CALLS GO ============
 *
 * The app's eleven steps call into UpdateService four times, and all four
 * now have somewhere to land:
 *
 *     step 3   remote.all_releases(platform)   nd_remote_all_releases()
 *     step 9   remote.asset_name(platform)     nd_remote_asset_name()
 *     step 10  remote.download(url, dest, ...) nd_remote_download()
 *     step 11  Update/main.py's _install()     nd_update_install(), reached
 *                                              by dlopen()ing the Update
 *                                              app -- see below
 *
 * The first three are in libneodct, which every app.so links, so they are
 * ordinary calls. The fourth is not: nd_update_install() lives in the Update
 * app's OWN app.so, and this is a different app.so in a different process.
 *
 * ============ WHY THE INSTALLER IS dlopen()ED AND NOT LINKED ============
 *
 * Because the Python does the same thing for the same reason, and the reason
 * is in its docstring: "One signature check, one staging path, one applier --
 * a second copy of that logic is the last thing this phone needs." The Python
 * reaches for importlib and loads /NeoDCT/System/apps/Update/main.py at the
 * moment it needs it; the C reaches for dlopen() and loads
 * /NeoDCT/System/apps/Update/app.so. Compiling the Update app's sources
 * into this app.so as well would put two copies of the signature verifier, the zip
 * reader and the stager in the image, which is exactly what that sentence
 * forbids.
 *
 * RTLD_LOCAL matters: both app.so files export app_run and app_shutdown, and
 * loading the Update app globally would leave two definitions of each in one
 * process. Nothing here calls the Update app's app_run(), only _install().
 *
 * The handle is deliberately NOT dlclose()d. nd_update_install() can end in
 * nd_update_reboot(), and unloading the library that the return address
 * points into is a segfault with a confusing backtrace. An app process is
 * torn down after run() returns anyway, so the leak lasts microseconds.
 *
 * ============ THIS APP HAS NO HALT OF ITS OWN, AND NEVER DID ============
 *
 * It is named alongside Power and Update wherever the reboot is discussed,
 * because it is the third app that can end the session -- but it reaches
 * that through nd_update_install() -> nd_update_reboot(), inside the Update
 * app.so it dlopen()s, and it has never spawned anything itself. So when the
 * halt moved into the core (nd_svc_reboot(), spec-app-services.md section 9)
 * this file needed no change: the code it borrows was converted, and the
 * service channel nd_svc_reboot() writes to is THIS process's, opened by
 * nd_ui_init_app() from NEODCT_SERVICE_FD like any other app's. The reason
 * Downgrade needed privilege was never in this file, and it is gone from the
 * file it was in.
 *
 * ============ ONE THING IN THE PYTHON WORTH KNOWING ============
 *
 * THE RUNNING VERSION IS MARKED, NOT HIDDEN. "Seeing where you are in the
 * list is most of the point of the list." Picking it is then refused by a
 * page of its own rather than by leaving it out.
 *
 * ============ THE LAST try/except IS WIDER THAN IT LOOKS ============
 *
 * It wraps both the importlib dance that loads the Update app AND the
 * _install() call inside it, so a failure DURING the install -- a bad
 * signature, no room to stage -- also reports "Downloaded, but could not
 * start the installer." The installer started fine; it refused.
 *
 * The C cannot reproduce that half. nd_update_install() draws its own
 * screens and returns void: a bad signature is a page the owner already saw,
 * not a value this app can see. So the C's message covers only the half it
 * can observe -- dlopen() or dlsym() failing -- and once _install() is
 * entered, whatever it says is the last word. That is a NARROWER message
 * than the Python's, not a wider one, and it is narrower in the honest
 * direction: this app never claims an install failed when it has no way of
 * knowing.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_remote.h"
#include "nd_settings.h"
#include "nd_storage.h"
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

const char *const nd_downgrade_no_folder = "The card has no update folder.";

/* VerticalList(ui, "Releases", ...) -- NOT the "Downgrade" header, which is
 * what the DetailPages and the ProgressScreens carry. */
const char *const nd_downgrade_list_title = "Releases";

const char *const nd_downgrade_button_downgrade = "Downgrade";
const char *const nd_downgrade_button_install = "Install";

/* ENTER = 28, spelled in the Python because it predates a shared table. */
#define DOWNGRADE_KEY_ENTER ND_KEY_ENTER

/* ProgressScreen(ui, "Reading releases", header=HEADER).draw(0, 1) */
#define DOWNGRADE_READING_STEP "Reading releases"

void nd_downgrade_downloading_step(char *out, size_t out_sz, const char *version)
{
    if (out == NULL || out_sz == 0u)
        return;
    (void)snprintf(out, out_sz, "Downloading %s", (version != NULL) ? version : "");
}

/* The denominator the Python's progress lambda passes to draw():
 *
 *     lambda done, total: progress.draw(done, total or picked["size"] or 1)
 *
 * `or` on an int is falsy at zero, so this is "total, else the size the
 * release listing gave, else 1". The 1 is not decoration -- draw() divides
 * by it, and a server that sends no Content-Length for a release whose size
 * GitHub also reported as 0 would otherwise divide by zero. */
int64_t nd_downgrade_progress_total(int64_t total, int64_t size)
{
    if (total != 0)
        return total;
    if (size != 0)
        return size;
    return 1;
}

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

/* What the download progress callback needs. `size` is the release listing's
 * figure, used when the server sends no length of its own. */
typedef struct {
    nd_progress *progress;
    int64_t size;
} downgrade_progress_ctx;

static void downgrade_on_progress(void *ctx, int64_t done, int64_t total)
{
    downgrade_progress_ctx *c = (downgrade_progress_ctx *)ctx;

    if (c == NULL || c->progress == NULL)
        return;
    (void)nd_progress_draw(c->progress, done, nd_downgrade_progress_total(total, c->size));
}

/* Step 11. The Python's importlib dance, as dlopen()/dlsym(). `why` receives
 * the reason on failure, in the position str(exc) occupies in the Python.
 *
 * Nothing is dlclose()d: _install() can end in a reboot, and unloading the
 * library the return address points into is a segfault. See the file header. */
static bool downgrade_hand_to_installer(nd_ui *ui, const char *package, char *why, size_t why_sz)
{
    char resolved[ND_PATH_MAX];
    void (*install)(nd_ui *, const char *);
    void *handle;
    void *symbol;

    if (nd_path_resolve(resolved, sizeof resolved, ND_DOWNGRADE_UPDATE_APP_SO) != ND_OK) {
        (void)snprintf(why, why_sz, "%s: path too long", ND_DOWNGRADE_UPDATE_APP_SO);
        return false;
    }

    handle = dlopen(resolved, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        const char *err = dlerror();
        (void)snprintf(why, why_sz, "%s", (err != NULL) ? err : "dlopen failed");
        return false;
    }

    /* POSIX says a function pointer cannot portably be assigned from the
     * void * dlsym returns; the memcpy is the standard way round it and is
     * what -Wpedantic wants to see. dlerror() is cleared first because a
     * symbol legitimately resolving to NULL is indistinguishable from
     * failure any other way -- this one cannot, but the idiom is the idiom. */
    (void)dlerror();
    symbol = dlsym(handle, ND_DOWNGRADE_INSTALL_SYMBOL);
    if (symbol == NULL) {
        const char *err = dlerror();
        (void)snprintf(why, why_sz, "%s: %s", ND_DOWNGRADE_INSTALL_SYMBOL,
                       (err != NULL) ? err : "not found");
        return false;
    }
    memcpy(&install, &symbol, sizeof install);

    install(ui, package);
    return true;
}

int app_run(nd_ui *ui)
{
    char platform[64];
    char installed[64];
    char subtitle[96];
    char why[ND_REMOTE_WHY_MAX];
    char message[ND_DOWNGRADE_MSG_MAX];
    char folder[ND_PATH_MAX];
    char asset[ND_REMOTE_ASSET_MAX];
    char destination[ND_PATH_MAX];
    char step[ND_DOWNGRADE_LABEL_MAX + 16];
    char labels[ND_RELEASES_LIMIT][ND_DOWNGRADE_LABEL_MAX];
    const char *items[ND_RELEASES_LIMIT];
    nd_release *releases;
    size_t n_releases = 0u;
    size_t i;
    const nd_release *picked;
    nd_progress progress;
    nd_vlist menu;
    nd_softkey bar;
    nd_update_err rc;
    int32_t choice;
    bool going_back;
    downgrade_progress_ctx ctx;

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

    /* Step 3. remote.all_releases(platform).
     *
     * ~1.7 kB per entry and thirty of them is 51 kB, which is more than an
     * app thread's stack should be asked to carry (nd_remote.h says so at
     * the struct). The labels beside it are 64 bytes each and stay here. */
    releases = (nd_release *)calloc(ND_RELEASES_LIMIT, sizeof *releases);
    if (releases == NULL) {
        nd_log_err(ND_LOG_UPDATE, "downgrade: no memory for the release list");
        nd_downgrade_noconn_body(message, sizeof message, "out of memory");
        nd_downgrade_page(ui, nd_downgrade_noconn_title, nd_downgrade_noconn_subtitle, message);
        return 0;
    }

    why[0] = '\0';
    rc = nd_remote_all_releases(platform, ND_RELEASES_LIMIT, releases, ND_RELEASES_LIMIT,
                                &n_releases, why, sizeof why);

    /* remote.NoRelease: GitHub answered, and nothing it published carries an
     * asset for this platform. A reachable-but-empty list is the same thing
     * and takes the same page -- the Python raises NoRelease for both. */
    if (rc == ND_UPD_ERR_NO_PACKAGE || (rc == ND_UPD_OK && n_releases == 0u)) {
        free(releases);
        nd_downgrade_for_platform(subtitle, sizeof subtitle, platform);
        nd_downgrade_page(ui, nd_downgrade_nothing_title, subtitle, nd_downgrade_nothing_body);
        return 0;
    }
    /* remote.NetworkError, in all its forms. `why` is str(exc). */
    if (rc != ND_UPD_OK) {
        free(releases);
        nd_downgrade_noconn_body(message, sizeof message, why);
        nd_downgrade_page(ui, nd_downgrade_noconn_title, nd_downgrade_noconn_subtitle, message);
        return 0;
    }

    /* Step 4. The running version is MARKED, not hidden. Seeing where you are
     * in the list is most of the point of the list. */
    for (i = 0u; i < n_releases; i++) {
        nd_downgrade_label(labels[i], sizeof labels[i], releases[i].version, installed,
                           !nd_remote_is_newer(releases[i].version, installed));
        items[i] = labels[i];
    }

    /* Step 5. VerticalList + SoftKeyBar("Select", present=False). */
    nd_vlist_init(&menu, ui, nd_downgrade_list_title, items, n_releases, ND_DOWNGRADE_APP_ID);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Select", false);

    choice = nd_vlist_show(&menu);
    if (choice < 0 || (size_t)choice >= n_releases) {
        free(releases);
        return 0;
    }
    picked = &releases[choice];

    /* Step 6. Picking the running version is refused by a page of its own. */
    if (strcmp(picked->version, installed) == 0) {
        nd_downgrade_neodct_version(subtitle, sizeof subtitle, installed);
        free(releases);
        nd_downgrade_page(ui, nd_downgrade_running_title, subtitle, nd_downgrade_running_body);
        return 0;
    }

    /* Steps 7 and 8. Going back is spelled out rather than asked as a bare
     * "are you sure": the consequence is not obvious, and this tool exists to
     * be used deliberately. */
    going_back = installed[0] != '\0' && !nd_remote_is_newer(picked->version, installed);
    if (going_back)
        nd_downgrade_ask_downgrade(message, sizeof message, picked->version, installed);
    else
        nd_downgrade_ask_install(message, sizeof message, picked->version, picked->size);
    if (!nd_downgrade_confirm(ui, message,
                              going_back ? nd_downgrade_button_downgrade
                                         : nd_downgrade_button_install)) {
        free(releases);
        return 0;
    }

    /* Step 9. The card, and the one name a package for this platform has. */
    if (!nd_storage_folder("update", folder, sizeof folder)) {
        free(releases);
        nd_downgrade_refuse(ui, nd_downgrade_no_folder);
        return 0;
    }
    if (nd_remote_asset_name(platform, asset, sizeof asset) != ND_OK ||
        nd_snprintf(destination, sizeof destination, "%s/%s", folder, asset) != ND_OK) {
        free(releases);
        nd_downgrade_refuse(ui, nd_downgrade_no_folder);
        return 0;
    }

    /* Step 10. It resumes across dropped connections; see nd_remote.h. */
    nd_downgrade_downloading_step(step, sizeof step, picked->version);
    nd_progress_init(&progress, ui, step, nd_downgrade_header, NULL, NULL, NULL);
    ctx.progress = &progress;
    ctx.size = picked->size;
    why[0] = '\0';
    if (nd_remote_download(picked->url, destination, picked->size, downgrade_on_progress, &ctx,
                           ND_REMOTE_DOWNLOAD_ATTEMPTS, NULL, why, sizeof why) != ND_UPD_OK) {
        free(releases);
        nd_downgrade_download_failed(message, sizeof message, why);
        nd_downgrade_refuse(ui, message);
        return 0;
    }

    /* Step 11. Hand off to the Update app's installer rather than
     * reimplementing it. `releases` dies here: nothing past this point reads
     * it, and _install() may not return at all. */
    free(releases);
    releases = NULL;
    picked = NULL;

    why[0] = '\0';
    if (!downgrade_hand_to_installer(ui, destination, why, sizeof why)) {
        nd_log_err(ND_LOG_UPDATE, "downgrade: installer unreachable: %s", why);
        nd_downgrade_installer_failed(message, sizeof message, why);
        nd_downgrade_refuse(ui, message);
    }
    return 0;
}

/* Nothing is held: no sound card, no child process, no open file. The one
 * thing that WOULD need releasing here is the DetailPage's block array, and
 * nd_downgrade_page() frees it before it returns rather than leaving it to a
 * teardown that must not allocate or free much (nd_app.h). Exported because
 * nd_app.h requires every app to export one. */
void app_shutdown(void) {}
