/* downgrade.h -- the parts of the Downgrade app a unit test can reach, and
 * the boundary with the update system.
 *
 * System/engineering/apps/Downgrade/main.py is 160 lines. Everything it does
 * ITSELF -- the two settings it reads, the two failure pages, the release
 * labels with their "(running)" and "(older)" marks, the two confirmations,
 * the megabyte formatting, the three refusals -- is ported in full below and
 * is exercised by test/unit/test_downgrade.c.
 *
 * Everything it does through System/core/UpdateService is done through
 * nd_remote (in libneodct, which every app.so links) and, for the install
 * itself, through the Update app's own app.so, dlopen()ed at the moment it
 * is needed exactly as the Python reaches for importlib. main.c's header
 * explains why the installer is loaded rather than linked; the short version
 * is the Python's own: "One signature check, one staging path, one applier
 * -- a second copy of that logic is the last thing this phone needs."
 *
 * ============ THE VERSION COMPARISON IS NOT DUPLICATED EITHER ============
 *
 * nd_downgrade_label() takes `older` as an ARGUMENT rather than working it
 * out. remote.version_key/is_newer is subtle -- piecewise numeric comparison,
 * so that 0.3.10a sorts above 0.3.9a and not below it -- and the Update app
 * needs the same answer. It lives once, in nd_remote_is_newer(); this app
 * calls it and passes the result in. That also keeps every builder here
 * pure, which is what lets test/unit/test_downgrade.c pin them against the
 * Python without a network.
 *
 */

#ifndef ND_DOWNGRADE_H_INCLUDED
#define ND_DOWNGRADE_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP_ID = 9006 -- manifest.json, and the release list's app_id. */
#define ND_DOWNGRADE_APP_ID 9006

/* HEADER = "Downgrade": the DetailPage and ProgressScreen header, NOT the
 * release list's title, which is "Releases". */
extern const char *const nd_downgrade_header;

/* APP_ICON. Absolute and load-bearing (CODING-STANDARDS.md 9.5); it is under
 * engineering/apps because the Makefile's STUB_ENG_APPS list routes this app
 * there on install. */
#define ND_DOWNGRADE_ICON "/NeoDCT/System/engineering/apps/Downgrade/icon.png"

/* The longest message any of the builders below produces: the downgrade
 * confirmation carries two version strings, and the two download failures
 * carry an arbitrary reason. */
#define ND_DOWNGRADE_MSG_MAX 320

/* A release label is a version plus at most "  (running)". */
#define ND_DOWNGRADE_LABEL_MAX 64

/* Step 11's importlib dance, as a path and a symbol. The Python loads
 * /NeoDCT/System/apps/Update/main.py; the C loads the app.so the C build
 * installs beside that app's manifest.json, and resolves the one function
 * that is _install()'s port. Both are ABSOLUTE and load-bearing
 * (CODING-STANDARDS.md 9.5): the path goes through nd_path_resolve(), so a
 * test root redirects it and the phone does not. */
#define ND_DOWNGRADE_UPDATE_APP_SO   "/NeoDCT/System/apps/Update/app.so"
#define ND_DOWNGRADE_INSTALL_SYMBOL  "nd_update_install"

/* ------------------------------------------------------------------ *
 * The verbatim strings
 * ------------------------------------------------------------------ */

/* NO_NETWORK, shown under the exception text on the "No connection" page. */
extern const char *const nd_downgrade_no_network;

/* The three pages, as title / subtitle / body triples. The subtitles that
 * carry a value ("for %s", "NeoDCT %s") are built by the functions below. */
extern const char *const nd_downgrade_nothing_title; /* "Nothing published"  */
extern const char *const nd_downgrade_nothing_body;
extern const char *const nd_downgrade_noconn_title;    /* "No connection"      */
extern const char *const nd_downgrade_noconn_subtitle; /* "Could not reach..." */
extern const char *const nd_downgrade_running_title;   /* "Already running"    */
extern const char *const nd_downgrade_running_body;

/* VerticalList(ui, "Releases", ...): the list's own title, which is NOT the
 * "Downgrade" header the pages and progress screens carry. */
extern const char *const nd_downgrade_list_title;

/* _refuse(ui, "The card has no update folder.") */
extern const char *const nd_downgrade_no_folder;

/* The two confirmation buttons. */
extern const char *const nd_downgrade_button_downgrade; /* "Downgrade" */
extern const char *const nd_downgrade_button_install;   /* "Install"   */

/* ------------------------------------------------------------------ *
 * The builders
 * ------------------------------------------------------------------ */

/* _format_size(count): "%.1f MB" % (count / 1048576.0). Megabytes as 1048576
 * bytes, not 1000000, and one decimal place. */
void nd_downgrade_format_size(char *out, size_t out_sz, int64_t count);

/* One line of the release list.
 *
 *     mark = "  (running)" when version == installed
 *            "  (older)"   when installed is set and this is not newer
 *            ""            otherwise
 *
 * TWO LEADING SPACES inside both marks. `older` is `not remote.is_newer(
 * version, installed)` and is the CALLER's to supply: the comparison lives
 * once, in nd_remote_is_newer(), and this app must not grow a second copy of
 * it. Keeping it out also keeps this builder pure, which is what lets the
 * unit test pin it without a network. See the header comment. */
void nd_downgrade_label(char *out, size_t out_sz, const char *version, const char *installed,
                        bool older);

/* "Downloading %s" % version -- the ProgressScreen step for step 10. */
void nd_downgrade_downloading_step(char *out, size_t out_sz, const char *version);

/* The denominator of the Python's progress lambda:
 *
 *     lambda done, total: progress.draw(done, total or picked["size"] or 1)
 *
 * `or` on an int is falsy at zero, so: total, else the size from the release
 * listing, else 1. The 1 stops a division by zero when a server sends no
 * length for a release GitHub also reported as zero bytes. */
int64_t nd_downgrade_progress_total(int64_t total, int64_t size);

/* "for %s" % platform, and "NeoDCT %s" % installed. */
void nd_downgrade_for_platform(char *out, size_t out_sz, const char *platform);
void nd_downgrade_neodct_version(char *out, size_t out_sz, const char *installed);

/* "%s\n\n%s" % (exc, NO_NETWORK) -- the body of the "No connection" page. */
void nd_downgrade_noconn_body(char *out, size_t out_sz, const char *why);

/* The two confirmations. The going-back one is spelled out rather than a bare
 * "are you sure": "the consequence is not obvious, and this tool exists to be
 * used deliberately." */
void nd_downgrade_ask_downgrade(char *out, size_t out_sz, const char *version,
                                const char *installed);
void nd_downgrade_ask_install(char *out, size_t out_sz, const char *version, int64_t size);

/* The two refusals after something was already fetched. */
void nd_downgrade_download_failed(char *out, size_t out_sz, const char *why);
void nd_downgrade_installer_failed(char *out, size_t out_sz, const char *why);

/* ------------------------------------------------------------------ *
 * The screens
 * ------------------------------------------------------------------ */

/* _page(ui, title, subtitle, body): a DetailPage with the app icon, header
 * "Downgrade" and a "Back" softkey. The dismissing key is discarded, exactly
 * as the Python discards DetailPage.show()'s return. */
void nd_downgrade_page(nd_ui *ui, const char *title, const char *subtitle, const char *body);

/* _confirm(ui, message, button_text): true only when ENTER dismissed it. */
bool nd_downgrade_confirm(nd_ui *ui, const char *message, const char *button_text);

/* _refuse(ui, message): a MessageDialog with an "OK" button AND NO CANCEL
 * KEYS, so Clear does not dismiss it and only ENTER does. That is the one
 * thing separating _refuse from a plain notice, and it is deliberate -- these
 * three messages are about a package that is half-installed or not installed,
 * and they should be read. */
void nd_downgrade_refuse(nd_ui *ui, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* ND_DOWNGRADE_H_INCLUDED */
