/* downgrade.h -- the parts of the Downgrade app a unit test can reach, and
 * the boundary with the half of the update system that is NOT in C yet.
 *
 * System/engineering/apps/Downgrade/main.py is 160 lines. Everything it does
 * ITSELF -- the two settings it reads, the two failure pages, the release
 * labels with their "(running)" and "(older)" marks, the two confirmations,
 * the megabyte formatting, the three refusals -- is ported in full below and
 * is exercised by test/unit/test_downgrade.c.
 *
 * Everything it does through System/core/UpdateService is not, because that
 * package has no C implementation.
 *
 * ============ WHAT IS MISSING, AND WHY IT IS NOT INVENTED HERE ============
 *
 * The app's eleven steps call into UpdateService four times:
 *
 *     step 3   remote.all_releases(platform)   the GitHub release list
 *     step 9   remote.asset_name(platform)     "UPDATE-<platform>.ndsw"
 *     step 10  remote.download(url, dest, ...) a resumable HTTPS download
 *     step 11  Update/main.py's _install(ui, destination)
 *
 * spec-update-system.md puts the first three in `nd_remote.c` and the fourth
 * in `nd_update_install()`, both inside a shared library called
 * libndupdate.so. Grepping include/ and lib/ finds none of it: no nd_remote,
 * no nd_package, no nd_zip, no nd_manifest, no nd_rsa, no nd_staging. The
 * whole of the C update system today is include/nd_update.h -- an enum, some
 * string constants, and one function (nd_update_message()) that lib/ never
 * defines, so calling even that would leave app.so with an unresolved symbol
 * and dlopen() would refuse to load the app.
 *
 * There is also no HTTP client anywhere in the tree and no TLS: the Makefile's
 * PKG_DEPS are freetype2, libpng, libjpeg and sqlite3. spec-update-system.md
 * line 1861 wants a libcurl backend behind a transport vtable. Nothing to
 * call, and nothing to build it out of.
 *
 * NONE OF IT IS REIMPLEMENTED IN THIS APP, for the reason the app's own
 * docstring gives about the step it refuses to duplicate: "One signature
 * check, one staging path, one applier -- a second copy of that logic is the
 * last thing this phone needs." That applies to the release list and the
 * download too. It applies twice over to the version comparison behind the
 * "(older)" mark, which is `remote.version_key`/`remote.is_newer` and which
 * the Update app needs as well: this app takes the answer as an argument
 * (see nd_downgrade_label) rather than growing a second copy of it.
 *
 * SESSION-SCOPE.md is explicit that this is deliberate, not an oversight:
 * "The update system and its crypto. Security-critical; it gets its own pass
 * with the RSA verifier reviewed properly, not rushed alongside everything
 * else."
 *
 * ============ SO WHAT DOES THE APP DO TODAY ============
 *
 * Steps 1 and 2, and then it stops. It reads the platform and the running
 * version, draws the Python's "Reading releases" progress screen at 0%, and
 * then -- with nothing to ask for the list -- logs
 *
 *     [UPDATE] release list unavailable: this build has no release reader
 *
 * and shows one DetailPage in the same shape as the Python's two failure
 * pages. That page is NOT one of the Python's: the Python has a page for
 * "GitHub says there is nothing for this phone" and a page for "the phone
 * could not reach GitHub", and this is neither of those conditions. Putting
 * either of the Python's screens up would be the app telling the owner
 * something it does not know. spec-core-loop.md line 183 and OPEN-QUESTIONS
 * X-20 set the precedent for the wording: a subsystem that is not linked says
 * so, in those words, rather than borrowing a message meant for something
 * else.
 *
 * When libndupdate lands, app_run() grows steps 3-11 around the helpers below
 * and nothing else in this directory changes. The call shapes are already
 * written down twice: spec-update-system.md's nd_remote.h block (line 1633),
 * and apps/Update/update_app.h, where the same boundary is drawn for the
 * Update app.
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

/* The one page that is NOT the Python's, for the condition the Python cannot
 * be in. See the header comment. */
extern const char *const nd_downgrade_nosvc_title;
extern const char *const nd_downgrade_nosvc_body;
extern const char *const nd_downgrade_nosvc_why;

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
 * version, installed)` and is the CALLER's to supply: version comparison
 * lives in nd_remote, which does not exist in this build, and this app must
 * not grow a second copy of it. See the header comment. */
void nd_downgrade_label(char *out, size_t out_sz, const char *version, const char *installed,
                        bool older);

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
