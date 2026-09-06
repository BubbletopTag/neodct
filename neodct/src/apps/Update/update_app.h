/* update_app.h -- the parts of the Update app a unit test can reach, and the
 * boundary with the half of the update system that is NOT in C yet.
 *
 * System/apps/Update/main.py is 467 lines and almost all of it is screens.
 * The machinery underneath it -- the zip reader, the manifest validator, the
 * RSA verifier, the GitHub client -- is System/core/UpdateService/, a
 * separate 1,272-line package that spec-update-system.md places in a separate
 * shared library (libndupdate.so, shared with the Downgrade app). None of
 * that library exists yet: see the WHAT IS MISSING section below.
 *
 * ============ WHAT IS MISSING, AND WHY IT IS NOT FAKED HERE ============
 *
 * Grepping include/ and lib/ for the update system finds exactly one file,
 * nd_update.h, which declares an error enum, a handful of string constants
 * and one function, nd_update_message(). THAT FUNCTION HAS NO DEFINITION
 * ANYWHERE IN lib/ -- calling it would leave app.so with an unresolved symbol
 * and dlopen() would refuse to load the app. It is therefore not called.
 *
 * There is no nd_zip, no nd_manifest, no nd_rsa, no nd_package, no nd_staging
 * and no nd_remote. So an .ndsw cannot be opened, its manifest cannot be
 * parsed, and its signature cannot be verified.
 *
 * Those pieces are NOT reimplemented inside this app. Two reasons, and the
 * second is the one that matters:
 *
 *   1. They are shared. spec-update-system.md puts them in libndupdate.so
 *      because the engineering Downgrade app is their second consumer. A
 *      private copy inside apps/Update would have to be duplicated or
 *      re-exported the moment Downgrade is ported, and a second copy of the
 *      code that decides whether an image is genuine is worse than none.
 *
 *   2. A hand-rolled RSA PKCS#1 v1.5 verifier that is subtly wrong ACCEPTS
 *      IMAGES NOBODY SIGNED, and this app's whole job is to write one of
 *      those over the system partition. An approximation here does not
 *      degrade gracefully; it bricks phones. signing.py's own docstring is
 *      about exactly this ("Verifiers that instead go looking for a
 *      DigestInfo inside the decrypted block are the ones that fall to
 *      Bleichenbacher'06 forgeries").
 *
 * So service.c holds the boundary, every entry point answers
 * ND_UPDSVC_UNAVAILABLE, and the install path stops at its first step with a
 * refusal that says so. Everything ABOVE that boundary is ported in full and
 * is exercised by test/unit/test_update_app.c; everything below it is one
 * file to replace when libndupdate lands.
 *
 * ============ THE RECORD FILE NAMES DISAGREE WITH nd_update.h ============
 *
 * nd_update.h says
 *
 *     #define ND_UPDATE_PENDING "/NeoDCT/User/.ndsys/pending"
 *     #define ND_UPDATE_RESULT  "/NeoDCT/User/.ndsys/result"
 *
 * and both are wrong. The files the boot-time applier actually reads and
 * writes are pending.prop and last_result.prop -- staging.py names them
 * (PENDING_RECORD, RESULT_RECORD) and ndsys-apply.sh reads them by those
 * names with sed. This is a wire format with a busybox shell script that runs
 * before any of this code exists, so the shell script wins. The two constants
 * below are the real ones. nd_update.h is frozen and is not edited by this
 * work package; the discrepancy is recorded here instead.
 *
 * ND_UPDATE_STATE_DIR from nd_update.h IS correct and is used as-is.
 */

#ifndef ND_UPDATE_APP_H_INCLUDED
#define ND_UPDATE_APP_H_INCLUDED

#include "nd_paths.h"
#include "nd_storage.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_update.h"
#include "nd_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The constants at the top of main.py
 * ------------------------------------------------------------------ */

/* ROOT_ID = 12 -- manifest.json, and the "Updates" VerticalList's app_id. */
#define ND_UPDATE_APP_ID 12

#define ND_UPDATE_RELEASE_KEY "/NeoDCT/System/keys/neodct-release.pub"
#define ND_UPDATE_APP_ICON    "/NeoDCT/System/apps/Update/icon.png"
#define ND_UPDATE_USER_DB_DIR "/NeoDCT/User/db"

#define ND_UPDATE_HEADER    "SOFTWARE UPDATE"
#define ND_UPDATE_COPY_HINT "Do not remove the card"

/* The two records this app touches. See the header comment: these names, not
 * nd_update.h's, are what ndsys-apply.sh reads. */
#define ND_UPDATE_PENDING_RECORD "/NeoDCT/User/.ndsys/pending.prop"
#define ND_UPDATE_PENDING_IMAGE  "/NeoDCT/User/.ndsys/pending.img"
#define ND_UPDATE_RESULT_RECORD  "/NeoDCT/User/.ndsys/last_result.prop"

/* ------------------------------------------------------------------ *
 * Bounds the Python does not have
 * ------------------------------------------------------------------ *
 *
 * OPEN-QUESTIONS.md P-2's rule: the Python has no limits in this subsystem
 * and the C needs them, because two of these inputs arrive on an SD card.
 * Each is far above anything the project ships.
 */

/* Packages offered by _choose_package. nd_storage_find_updates() itself caps
 * the directory listing at ND_SD_MAX_LISTING (4096); this is the number the
 * menu will show. A card with more than 64 .ndsw files on it is not a case
 * anybody has. */
#define ND_UPDATE_MAX_PACKAGES 32

/* Databases copied by _backup_user_data. /NeoDCT/User/db holds four. */
#define ND_UPDATE_MAX_DBS 64

/* str(exc) stands in for strerror(); see OPEN-QUESTIONS.md PW-3 for the same
 * substitution in the Power app. */
#define ND_UPDATE_WHY_MAX 160

/* Long enough for the longest composed refusal and the longest composed page
 * body: "Update to <version> failed.\n<reason>" can carry two whole record
 * values, and _restart_page's body is two paragraphs plus a version. */
#define ND_UPDATE_MSG_MAX 768

#define ND_UPDATE_VERSION_MAX   64
#define ND_UPDATE_PLATFORM_MAX  64
#define ND_UPDATE_HEX_MAX       65  /* 64 hex digits of sha256, plus NUL     */
#define ND_UPDATE_SALT_MAX      513 /* verity.py caps the salt at 256 bytes  */
#define ND_UPDATE_CHANGELOG_MAX 1024

/* ------------------------------------------------------------------ *
 * manifest.json, as far as this app and the staging record need it
 * ------------------------------------------------------------------ *
 *
 * A flat mirror of UpdateService/manifest.py's Manifest. Filling it in is
 * nd_manifest.c's job in the missing library; the struct lives here because
 * the app reads six of its fields on screen and nd_upd_stage_package() writes
 * nine of them into pending.prop.
 */
typedef struct {
    char version[ND_UPDATE_VERSION_MAX];
    int64_t buildtime; /* a JSON INTEGER; see nd_update.h                  */
    char platform[ND_UPDATE_PLATFORM_MAX];
    char sha256[ND_UPDATE_HEX_MAX];
    char changelog[ND_UPDATE_CHANGELOG_MAX];
    char min_kernel[ND_UPDATE_VERSION_MAX];
    char thumbnail_sha256[ND_UPDATE_HEX_MAX];

    /* manifest["verity"], which staging.py copies out field by field. */
    char verity_root_hash[ND_UPDATE_HEX_MAX];
    int64_t verity_block_size;
    int64_t verity_image_blocks;
    char verity_salt[ND_UPDATE_SALT_MAX];
} nd_upd_manifest;

/* ------------------------------------------------------------------ *
 * service.c -- the boundary with UpdateService
 * ------------------------------------------------------------------ *
 *
 * A 1:1 mirror of UpdateService/__init__.py's three exception classes, plus
 * the one state C has and Python does not: the library is not in this build.
 */
typedef enum {
    ND_UPDSVC_OK = 0,
    ND_UPDSVC_INVALID,       /* InvalidUpdate       -- nothing installable  */
    ND_UPDSVC_BAD_SIGNATURE, /* BadSignature        -- unsigned or wrong    */
    ND_UPDSVC_INCOMPATIBLE,  /* IncompatibleUpdate  -- the brick case       */
    ND_UPDSVC_UNAVAILABLE,   /* no libndupdate in this build                */

    /* remote.NetworkError, and the two local failures that reach the same
     * screen: no room on the card, and the card refusing the write. All
     * three end the same way -- the download did not happen and `why` says
     * what stopped it -- which is exactly the page _check_online draws.
     *
     * NOT folded into ND_UPDSVC_UNAVAILABLE, which it briefly was: that one
     * means "this build has no downloader", and telling the owner of a
     * working phone that their software cannot do updates because the
     * carrier dropped is the wrong sentence. remote.NoRelease keeps
     * ND_UPDSVC_INVALID, because _check_online tests for exactly that value
     * to draw "Nothing published". */
    ND_UPDSVC_NETWORK
} nd_updsvc_err;

/* An opened .ndsw. Opaque, and today nothing can produce one. */
typedef struct nd_upd_package nd_upd_package;

/* False in this build. Exposed so a caller -- and a test -- can ask rather
 * than infer it from a failure. */
bool nd_upd_service_available(void);

/* package.open_package(path). ND_UPDSVC_INVALID is the Python's InvalidUpdate
 * and is the one the app turns into "INVALID UPDATE! UPDATE MAY BE CORRUPT!!"
 * `why` receives str(exc); pass NULL to discard it. */
nd_updsvc_err nd_upd_package_open(const char *path, nd_upd_package **out, char *why, size_t why_sz);
void nd_upd_package_close(nd_upd_package *pkg);

const nd_upd_manifest *nd_upd_package_manifest(const nd_upd_package *pkg);
const char *nd_upd_package_path(const nd_upd_package *pkg);

/* Package.image_size: the UNCOMPRESSED size of the rootfs.squashfs member,
 * from the zip's central directory. -1 when it cannot be known. */
int64_t nd_upd_package_image_size(const nd_upd_package *pkg);

/* Package.signed. Stays false on the engineering-mode override path, which is
 * what makes the update page's badge say "Not signed". */
bool nd_upd_package_signed(const nd_upd_package *pkg);

/* Package.verify_signature(key_path) -> ND_UPDSVC_BAD_SIGNATURE. */
nd_updsvc_err nd_upd_package_verify_signature(nd_upd_package *pkg, const char *key_path, char *why,
                                              size_t why_sz);

/* Manifest.check_compatible(platform, kernel) -> ND_UPDSVC_INCOMPATIBLE, with
 * the message the app pastes after "WRONG UPDATE FOR THIS PHONE!\n". */
nd_updsvc_err nd_upd_manifest_check_compatible(const nd_upd_manifest *m, const char *platform,
                                               const char *kernel, char *why, size_t why_sz);

/* _thumbnail()'s C shape. The Python decodes the PNG bytes in memory and
 * hands the Image straight to DetailPage; nd_detailpage_init() takes a PATH
 * and nothing else, so the C asks for a path instead. See main.c. */
nd_updsvc_err nd_upd_package_thumbnail_path(nd_upd_package *pkg, char *out, size_t out_sz);

/* ---- remote.py, the online half ---- */

typedef struct {
    char version[ND_UPDATE_VERSION_MAX];
    char url[ND_PATH_MAX];
    int64_t size;
} nd_upd_release;

/* remote.latest(platform). ND_UPDSVC_INVALID stands in for remote.NoRelease
 * ("nothing published for this phone"); everything else is the NetworkError
 * branch, and `why` is the text the app prints as str(exc). */
nd_updsvc_err nd_upd_remote_latest(const char *platform, nd_upd_release *out, char *why,
                                   size_t why_sz);
/* remote.is_newer(found, installed). */
bool nd_upd_remote_is_newer(const char *found, const char *installed);
/* remote.asset_name(platform) -- the file the download lands in. */
nd_updsvc_err nd_upd_remote_asset_name(const char *platform, char *out, size_t out_sz);
/* remote.download(url, destination, size, progress). */
nd_updsvc_err nd_upd_remote_download(const nd_upd_release *rel, const char *destination,
                                     nd_progress *progress, char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * staging.c -- the records the initramfs applier reads
 * ------------------------------------------------------------------ *
 *
 * This is the ONE piece of UpdateService implemented here rather than
 * declared missing, for two reasons. It is the only piece the app cannot do
 * without -- read_result() is on the app's very first screen -- and it is
 * flat KEY=value text with no dependency beyond libc, which is why
 * spec-update-system.md puts nd_staging.c in libneodct rather than in the
 * crypto library. When lib/nd_staging.c lands, this file is deleted and the
 * three calls below are repointed at it.
 */

/* One parsed KEY=value record. staging.py's _read_record, which is NOT
 * nd_props: it strips the LINE, then splits on the first '=' and keeps the
 * value exactly as it then stands. nd_props strips the value as well. */
#define ND_UPDREC_MAX_KEYS  32
#define ND_UPDREC_KEY_MAX   64
#define ND_UPDREC_VALUE_MAX 256

typedef struct {
    char keys[ND_UPDREC_MAX_KEYS][ND_UPDREC_KEY_MAX];
    char values[ND_UPDREC_MAX_KEYS][ND_UPDREC_VALUE_MAX];
    size_t n;
} nd_upd_record;

/* staging.read_result(). false when the file is missing, unreadable or holds
 * no KEY=value line at all -- the Python's `if not result: return`. */
bool nd_upd_read_result(nd_upd_record *out);
void nd_upd_clear_result(void);

/* dict.get(key, dflt), never NULL when dflt is not. */
const char *nd_upd_record_get(const nd_upd_record *rec, const char *key, const char *dflt);

/* staging.stage_package(manifest, package_path, image_bytes, STATE_DIR).
 *
 * Nothing is copied. The record names the package by BASENAME and the applier
 * streams the image straight out of the .ndsw on the card at boot, because
 * the user partition is 8 MiB on the Luckfox and a system image is 51 MiB.
 *
 * ND_ERR_IO with errno set on any failure, which is the OSError the caller
 * turns into "Could not stage the update.\n%s". */
nd_err nd_upd_stage_package(const nd_upd_manifest *m, const char *package_path,
                            int64_t image_bytes);

/* ------------------------------------------------------------------ *
 * main.c -- the app itself
 * ------------------------------------------------------------------ */

/* The three long help strings, and every refusal, so a test can pin what a
 * user reads without driving a widget. */
extern const char *const nd_update_no_card_help;
extern const char *const nd_update_not_ready_help;
extern const char *const nd_update_no_package_help;

extern const char *const nd_update_msg_invalid;
extern const char *const nd_update_msg_bad_signature;
extern const char *const nd_update_msg_wrong_prefix;
extern const char *const nd_update_msg_install_anyway;
extern const char *const nd_update_msg_no_update_folder;
extern const char *const nd_update_msg_no_release_notes;
extern const char *const nd_update_msg_cannot_write_prefix;
extern const char *const nd_update_msg_cannot_stage_prefix;

/* NOT IN THE PYTHON either, and for the same kind of reason as the one below.
 * _reboot() used to discard nd_svc_reboot()'s return, because in the Python
 * false could only mean "this image has no reboot binary" -- nothing an owner
 * could do anything about. It now also means the core refused or did not
 * answer, and the phone is left in a state that IS actionable: the pending
 * record is already written and synced, so the update installs at the next
 * boot by any means. This is that sentence. */
extern const char *const nd_update_msg_no_restart;

/* NOT IN THE PYTHON. The one string this port had to add: it is what the
 * install path says when service.c reports ND_UPDSVC_UNAVAILABLE. The Python
 * has no such branch because in Python the package reader is always there.
 * Reusing "INVALID UPDATE! UPDATE MAY BE CORRUPT!!" for it was rejected --
 * that tells the owner of a perfectly good download to throw it away. */
extern const char *const nd_update_msg_no_reader;

/* "%.1f MB", count / 1048576.0. */
void nd_update_format_size(int64_t count, char *out, size_t out_sz);

/* strftime("%d %b %Y", gmtime(stamp)) -- gmtime, so a build date reads the
 * same on every phone. "unknown" on any failure. */
void nd_update_format_date(int64_t stamp, char *out, size_t out_sz);

/* "%s of %s". DEAD CODE IN THE PYTHON -- _size_detail has no caller. Ported
 * because it is the ProgressScreen `detail` callback the widget was built
 * for, and dropping it silently would leave the next reader wondering. */
void nd_update_size_detail(int64_t done, int64_t total, char *out, size_t out_sz);

/* _engineering_mode(ui). */
bool nd_update_engineering_mode(nd_ui *ui);

/* _installed_version(): strip(get_setting("system.os.versionnumber", "")). */
void nd_update_installed_version(char *out, size_t out_sz);

/* _refuse(): one dialog, an "OK" softkey, and NO cancel keys -- C does not
 * dismiss it, the softkey is the only way out. */
void nd_update_refuse(nd_ui *ui, const char *message);

/* _confirm(): true only on ENTER; the dialog's default cancel key backs out. */
bool nd_update_confirm(nd_ui *ui, const char *message, const char *button_text);

/* _page(): every screen that is a page rather than a warning. Returns the key
 * that dismissed it. cancellable=false is the Python's cancel_keys=(). */
int32_t nd_update_page(nd_ui *ui, const char *title, const char *subtitle, const char *body,
                       const char *image, const char *badge, const char *softkey_text,
                       bool cancellable);

/* _has_network(): a default route in /proc/net/route or /proc/net/ipv6_route.
 * NEODCT_STUB forces false, exactly as the Python does. */
bool nd_update_has_network(void);

/* The half of it that can be pointed somewhere else. Both files are opened
 * with plain fopen and are NOT ND_ROOT-resolved -- /proc is the kernel's, and
 * the Python says so: "PathRemap cannot cover /proc, so the stub says so
 * instead". Split out so a test can drive the two parsers over real files
 * without a real default route. Either path may be NULL. */
bool nd_update_proc_has_default_route(const char *v4_path, const char *v6_path);

/* _backup_user_data(progress): best effort, and true only when a backup was
 * actually written. progress may be NULL. */
bool nd_update_backup_user_data(nd_progress *progress);

/* _stage(ui, pkg, progress). Shows its own refusal on failure. */
bool nd_update_stage(nd_ui *ui, const nd_upd_manifest *m, const char *package_path,
                     int64_t image_size);

/* _report_last_result(ui): show how the last install went, then forget it. */
void nd_update_report_last_result(nd_ui *ui);

/* _choose_package(): the index into `paths`, or ND_WIDGET_BACK. One package
 * is used without asking. */
int32_t nd_update_choose_package(nd_ui *ui, char (*paths)[ND_STORAGE_PATH_MAX], size_t n);

/* _install(ui, path). */
void nd_update_install(nd_ui *ui, const char *path);

/* _check_online(ui): the downloaded package's path, or false. */
bool nd_update_check_online(nd_ui *ui, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * NOT called by any test
 * ------------------------------------------------------------------ */

/* _reboot(ui): ask the CORE to restart the phone (nd_svc_reboot()), then sit
 * still for thirty seconds -- or, when the restart did not start, draw
 * nd_update_msg_no_restart and return at once.
 *
 * The candidate list this used to walk -- `reboot`, `/sbin/reboot`,
 * `busybox reboot` -- and the sync that preceded it are in lib/nd_svc.c now,
 * as nd_svc_reboot_commands. An app process resolving a program name along
 * $PATH and fork/exec'ing it with the privilege to power-cycle the machine
 * was the only reason this app needed privilege the others do not.
 * docs/c-rewrite/spec-app-services.md section 9.
 *
 * Still not called by any test: in a process with no service channel -- and
 * a unit test is one -- nd_svc_reboot() does the halt itself, because a
 * process with no core to ask IS the core as far as that library can tell.
 * On a developer's machine and on a CI runner that is a real reboot, so
 * test_update_app.c never calls it, never calls _restart_page and never
 * reaches the end of _install. The same deliberate hole as
 * nd_power_go_down(); power.h says so in the same words, and test_svc.c
 * reaches the same code with the spawn injected out. */
void nd_update_reboot(nd_ui *ui);

/* _restart_page(): the last screen before the reboot. Ends by calling
 * nd_update_reboot(), so it is not called by any test either. */
void nd_update_restart_page(nd_ui *ui, const nd_upd_manifest *m, bool backed_up);

/* execvp's argv[0] lookup, which nd_proc_spawn() does not do for us. STAGING
 * still spawns `sync`, so this stays even though the reboot no longer needs
 * it -- and staging is a different call site with a different argument, run
 * long before a reboot is in sight. */
bool nd_update_which(const char *name, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_UPDATE_APP_H_INCLUDED */
