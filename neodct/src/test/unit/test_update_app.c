/* test_update_app.c -- the Update app, app id 12.
 *
 * Every assertion below was read out of System/apps/Update/main.py,
 * System/core/UpdateService/staging.py or neodct/initramfs/ndsys-apply.sh.
 * Where the C and the Python differ, the difference is asserted too, so it
 * cannot drift back by accident.
 *
 * ============ WHAT THIS TEST CANNOT REACH, AND WHY ============
 *
 * 1. THE INSTALL PATH DOES NOT EXIST YET. apps/Update/service.c is a
 *    boundary, not an implementation: there is no C zip reader, no manifest
 *    parser and no RSA verifier anywhere in this tree, so an .ndsw cannot be
 *    opened. _install() therefore stops on its first call every time. What is
 *    tested is that it stops SAFELY -- the refusal that appears is the
 *    "no package reader" one and not "INVALID UPDATE", and nothing is staged.
 *    Steps 2 to 6 of _install (compatibility, signature, the update page, the
 *    backup, the staging record, the restart page) have never been run end to
 *    end and this test does not pretend otherwise. The pieces underneath them
 *    that CAN be reached -- the backup, the staging record, the page helper --
 *    are driven directly instead.
 *
 * 2. nd_update_reboot() AND nd_update_restart_page() ARE NEVER CALLED. The
 *    first runs `sync` and then the first of `reboot`, `/sbin/reboot`,
 *    `busybox reboot` that exists; on a developer's machine and on a CI
 *    runner that is a real reboot. The second ends by calling the first. Same
 *    deliberate hole as nd_power_go_down(), named in update_app.h as well as
 *    here rather than left to be discovered.
 *
 * 3. "CLEAR does not dismiss a refusal" is NOT asserted. _refuse() passes
 *    cancel_keys=(), so a MessageDialog driven with a held CLEAR would loop
 *    forever -- the failure mode of that test is a hung suite rather than a
 *    red line, which is worse than not having it. The empty cancel set is
 *    visible in the source and nd_msgdialog's own key handling is pinned by
 *    test_widgets_dialogs.c.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (for the
 * font); the scratch roots are this test's own.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "smallapp_test.h"

#include "nd_settings.h"
#include "nd_storage.h"

#include "../../apps/Update/update_app.h"

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);

    void (*format_size)(int64_t, char *, size_t);
    void (*format_date)(int64_t, char *, size_t);
    void (*size_detail)(int64_t, int64_t, char *, size_t);
    bool (*engineering_mode)(nd_ui *);
    void (*installed_version)(char *, size_t);
    void (*refuse)(nd_ui *, const char *);
    bool (*confirm)(nd_ui *, const char *, const char *);
    int32_t (*page)(nd_ui *, const char *, const char *, const char *, const char *, const char *,
                    const char *, bool);
    bool (*has_network)(void);
    bool (*proc_route)(const char *, const char *);
    bool (*backup)(nd_progress *);
    bool (*stage)(nd_ui *, const nd_upd_manifest *, const char *, int64_t);
    void (*report_last_result)(nd_ui *);
    int32_t (*choose)(nd_ui *, char (*)[ND_STORAGE_PATH_MAX], size_t);
    void (*install)(nd_ui *, const char *);
    bool (*which)(const char *, char *, size_t);

    bool (*svc_available)(void);
    nd_updsvc_err (*svc_open)(const char *, nd_upd_package **, char *, size_t);
    nd_updsvc_err (*svc_compat)(const nd_upd_manifest *, const char *, const char *, char *,
                                size_t);
    nd_updsvc_err (*svc_verify)(nd_upd_package *, const char *, char *, size_t);
    nd_updsvc_err (*svc_latest)(const char *, nd_upd_release *, char *, size_t);

    bool (*read_result)(nd_upd_record *);
    void (*clear_result)(void);
    const char *(*record_get)(const nd_upd_record *, const char *, const char *);
    nd_err (*stage_package)(const nd_upd_manifest *, const char *, int64_t);

    const char *const *no_card_help;
    const char *const *not_ready_help;
    const char *const *no_package_help;
    const char *const *msg_invalid;
    const char *const *msg_bad_signature;
    const char *const *msg_wrong_prefix;
    const char *const *msg_install_anyway;
    const char *const *msg_no_update_folder;
    const char *const *msg_no_release_notes;
    const char *const *msg_cannot_write_prefix;
    const char *const *msg_cannot_stage_prefix;
    const char *const *msg_no_reader;
    const char *const *const *reboot_commands;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.format_size = sa_sym(h, "nd_update_format_size");
    *(void **)&api.format_date = sa_sym(h, "nd_update_format_date");
    *(void **)&api.size_detail = sa_sym(h, "nd_update_size_detail");
    *(void **)&api.engineering_mode = sa_sym(h, "nd_update_engineering_mode");
    *(void **)&api.installed_version = sa_sym(h, "nd_update_installed_version");
    *(void **)&api.refuse = sa_sym(h, "nd_update_refuse");
    *(void **)&api.confirm = sa_sym(h, "nd_update_confirm");
    *(void **)&api.page = sa_sym(h, "nd_update_page");
    *(void **)&api.has_network = sa_sym(h, "nd_update_has_network");
    *(void **)&api.proc_route = sa_sym(h, "nd_update_proc_has_default_route");
    *(void **)&api.backup = sa_sym(h, "nd_update_backup_user_data");
    *(void **)&api.stage = sa_sym(h, "nd_update_stage");
    *(void **)&api.report_last_result = sa_sym(h, "nd_update_report_last_result");
    *(void **)&api.choose = sa_sym(h, "nd_update_choose_package");
    *(void **)&api.install = sa_sym(h, "nd_update_install");
    *(void **)&api.which = sa_sym(h, "nd_update_which");

    *(void **)&api.svc_available = sa_sym(h, "nd_upd_service_available");
    *(void **)&api.svc_open = sa_sym(h, "nd_upd_package_open");
    *(void **)&api.svc_compat = sa_sym(h, "nd_upd_manifest_check_compatible");
    *(void **)&api.svc_verify = sa_sym(h, "nd_upd_package_verify_signature");
    *(void **)&api.svc_latest = sa_sym(h, "nd_upd_remote_latest");

    *(void **)&api.read_result = sa_sym(h, "nd_upd_read_result");
    *(void **)&api.clear_result = sa_sym(h, "nd_upd_clear_result");
    *(void **)&api.record_get = sa_sym(h, "nd_upd_record_get");
    *(void **)&api.stage_package = sa_sym(h, "nd_upd_stage_package");

    api.no_card_help = dlsym(h, "nd_update_no_card_help");
    api.not_ready_help = dlsym(h, "nd_update_not_ready_help");
    api.no_package_help = dlsym(h, "nd_update_no_package_help");
    api.msg_invalid = dlsym(h, "nd_update_msg_invalid");
    api.msg_bad_signature = dlsym(h, "nd_update_msg_bad_signature");
    api.msg_wrong_prefix = dlsym(h, "nd_update_msg_wrong_prefix");
    api.msg_install_anyway = dlsym(h, "nd_update_msg_install_anyway");
    api.msg_no_update_folder = dlsym(h, "nd_update_msg_no_update_folder");
    api.msg_no_release_notes = dlsym(h, "nd_update_msg_no_release_notes");
    api.msg_cannot_write_prefix = dlsym(h, "nd_update_msg_cannot_write_prefix");
    api.msg_cannot_stage_prefix = dlsym(h, "nd_update_msg_cannot_stage_prefix");
    api.msg_no_reader = dlsym(h, "nd_update_msg_no_reader");
    api.reboot_commands = dlsym(h, "nd_update_reboot_commands");

    return api.run != NULL && api.shutdown != NULL && api.format_size != NULL &&
           api.format_date != NULL && api.size_detail != NULL && api.engineering_mode != NULL &&
           api.installed_version != NULL && api.refuse != NULL && api.confirm != NULL &&
           api.page != NULL && api.has_network != NULL && api.proc_route != NULL &&
           api.backup != NULL && api.stage != NULL && api.report_last_result != NULL &&
           api.choose != NULL && api.install != NULL && api.which != NULL &&
           api.svc_available != NULL && api.svc_open != NULL && api.svc_compat != NULL &&
           api.svc_verify != NULL && api.svc_latest != NULL && api.read_result != NULL &&
           api.clear_result != NULL && api.record_get != NULL && api.stage_package != NULL &&
           api.no_card_help != NULL && api.not_ready_help != NULL && api.no_package_help != NULL &&
           api.msg_invalid != NULL && api.msg_bad_signature != NULL &&
           api.msg_wrong_prefix != NULL && api.msg_install_anyway != NULL &&
           api.msg_no_update_folder != NULL && api.msg_no_release_notes != NULL &&
           api.msg_cannot_write_prefix != NULL && api.msg_cannot_stage_prefix != NULL &&
           api.msg_no_reader != NULL && api.reboot_commands != NULL;
}

/* ------------------------------------------------------------------ *
 * Scratch roots
 * ------------------------------------------------------------------ *
 *
 * A fresh one per case, because half of these cases assert on whether a file
 * exists and would otherwise see the previous case's leftovers. Every root is
 * remembered so the run can tidy up after itself; CODING-STANDARDS.md 1.7
 * applies to the filesystem too.
 */

#define MAX_ROOTS 32
static char g_roots[MAX_ROOTS][ND_PATH_MAX];
static size_t g_n_roots;

static const char *fresh_root(void)
{
    if (g_n_roots >= MAX_ROOTS) {
        CHECK(false, "too many scratch roots");
        return g_roots[0];
    }
    if (!sa_tmpdir("ndupdate", g_roots[g_n_roots], ND_PATH_MAX)) {
        CHECK(false, "scratch root");
        return "";
    }
    (void)nd_path_set_root(g_roots[g_n_roots]);
    return g_roots[g_n_roots++];
}

static void drop_roots(void)
{
    size_t i;

    (void)nd_path_set_root(NULL);
    for (i = 0u; i < g_n_roots; i++)
        sa_rmtree(g_roots[i]);
    g_n_roots = 0u;
}

/* Writes at a VIRTUAL path, creating parents. Deliberately not going through
 * the code under test. */
static void write_virtual(const char *path, const char *text)
{
    char resolved[ND_PATH_MAX];
    char dir[ND_PATH_MAX];
    const char *slash = strrchr(path, '/');
    FILE *f;

    if (slash != NULL && slash != path) {
        (void)nd_strlcpy(dir, path, (size_t)(slash - path) + 1u);
        if (nd_mkdir_p(dir, 0755u) != ND_OK) {
            CHECK(false, "mkdir -p");
            return;
        }
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        CHECK(false, "resolve");
        return;
    }
    f = fopen(resolved, "wb");
    if (f == NULL) {
        CHECK(false, "open for writing");
        return;
    }
    (void)fputs(text, f);
    (void)fclose(f);
}

static size_t read_virtual(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    size_t n;

    out[0] = '\0';
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return (size_t)-1;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return (size_t)-1;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    (void)fclose(f);
    return n;
}

static bool exists_virtual(const char *path)
{
    return nd_path_exists(path);
}

/* nd_storage's paths are its own globals, and the app links the same
 * libneodct this test does, so pointing them here points the app too. */
#define CARD_MOUNT "/sdcard"
#define CARD_STATE "/sdcard.prop"

static void insert_ready_card(void)
{
    static const char *const FOLDERS[5] = {"wallpapers", "tones", "backup_db", "music", "update"};
    size_t i;

    nd_storage_set_paths(CARD_MOUNT, CARD_STATE);
    write_virtual(CARD_STATE, "state=mounted\ndevice=/dev/vdc\nfstype=vfat\nlabel=NEODCT\n");
    for (i = 0u; i < 5u; i++) {
        char path[ND_PATH_MAX];

        (void)nd_snprintf(path, sizeof path, "%s/%s", CARD_MOUNT, FOLDERS[i]);
        if (nd_mkdir_p(path, 0755u) != ND_OK)
            CHECK(false, "card folder");
    }
}

/* ------------------------------------------------------------------ *
 * 1. The constants and the strings
 * ------------------------------------------------------------------ */

static void test_constants(void)
{
    CHECK_INT(ND_UPDATE_APP_ID, 12, "ROOT_ID");
    CHECK_STR(ND_UPDATE_RELEASE_KEY, "/NeoDCT/System/keys/neodct-release.pub", "RELEASE_KEY");
    CHECK_STR(ND_UPDATE_APP_ICON, "/NeoDCT/System/apps/Update/icon.png", "APP_ICON");
    CHECK_STR(ND_UPDATE_USER_DB_DIR, "/NeoDCT/User/db", "USER_DB_DIR");
    CHECK_STR(ND_UPDATE_HEADER, "SOFTWARE UPDATE", "HEADER");
    CHECK_STR(ND_UPDATE_COPY_HINT, "Do not remove the card", "COPY_HINT");

    /* staging.STATE_DIR, and the two record names ndsys-apply.sh reads with
     * sed. nd_update.h spells the last two "pending" and "result", which is
     * not what is on the disk; update_app.h records the discrepancy. */
    CHECK_STR(ND_UPDATE_STATE_DIR, "/NeoDCT/User/.ndsys", "STATE_DIR");
    CHECK_STR(ND_UPDATE_PENDING_RECORD, "/NeoDCT/User/.ndsys/pending.prop", "PENDING_RECORD");
    CHECK_STR(ND_UPDATE_PENDING_IMAGE, "/NeoDCT/User/.ndsys/pending.img", "PENDING_IMAGE");
    CHECK_STR(ND_UPDATE_RESULT_RECORD, "/NeoDCT/User/.ndsys/last_result.prop", "RESULT_RECORD");

    /* ENTER is 28 and BACK is 14, as main.py spells them itself. */
    CHECK_INT(ND_KEY_ENTER, 28, "ENTER");
    CHECK_INT(ND_KEY_BACK, 14, "BACK");
}

static void test_help_strings(void)
{
    CHECK_STR(*api.no_card_help,
              "Updates are read from an SD card.\n"
              "\n"
              "Format a card as FAT32, make a folder called \"update\" on it and copy "
              "UPDATE.ndsw into that folder.\n"
              "\n"
              "Put the card in the phone and open Update again.",
              "NO_CARD_HELP");
    CHECK_STR(*api.not_ready_help,
              "The card in the phone is not set up for NeoDCT.\n"
              "\n"
              "Settings can prepare it for you: Settings, Memory card, Prepare card.\n"
              "\n"
              "Preparing a card makes the folders NeoDCT uses. It does not erase what "
              "is already on it.",
              "NOT_READY_HELP");
    CHECK_STR(*api.no_package_help,
              "There is nothing to install from the card.\n"
              "\n"
              "To update, copy UPDATE.ndsw into the \"update\" folder on the card.\n"
              "\n"
              "An update file is built by \"make update\" in the buildroot tree and "
              "fits one kind of hardware only -- a QEMU build will not install on a "
              "real phone, or the other way round.",
              "NO_PACKAGE_HELP");

    /* The exclamation marks are the phone's, not a typo. */
    CHECK_STR(*api.msg_invalid, "INVALID UPDATE! UPDATE MAY BE CORRUPT!!", "InvalidUpdate refusal");
    CHECK_STR(*api.msg_bad_signature, "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!",
              "BadSignature refusal");
    CHECK_STR(*api.msg_wrong_prefix, "WRONG UPDATE FOR THIS PHONE!\n", "IncompatibleUpdate prefix");
    CHECK_STR(*api.msg_install_anyway, "Install Anyway?", "the engineering-mode override");
    CHECK_STR(*api.msg_no_update_folder, "The card has no update folder.", "no update folder");
    CHECK_STR(*api.msg_no_release_notes, "No release notes came with this build.",
              "empty changelog");
    CHECK_STR(*api.msg_cannot_write_prefix, "Cannot write to the user partition.\n",
              "_stage mkdir failure");
    CHECK_STR(*api.msg_cannot_stage_prefix, "Could not stage the update.\n",
              "_stage record failure");

    /* The one string the Python does not have. It must not be either of the
     * two refusals it could be mistaken for. */
    CHECK(strcmp(*api.msg_no_reader, *api.msg_invalid) != 0,
          "the no-reader refusal is not the InvalidUpdate one");
    CHECK(strcmp(*api.msg_no_reader, *api.msg_bad_signature) != 0,
          "the no-reader refusal is not the BadSignature one");
    CHECK(strstr(*api.msg_no_reader, "reader") != NULL, "and it says what is missing");
}

static void test_reboot_table(void)
{
    /* The order is the Python's and is load-bearing: `reboot` on the PATH and
     * `/sbin/reboot` are different programs on an image that has both. */
    CHECK_STR(api.reboot_commands[0][0], "reboot", "candidate 0");
    CHECK(api.reboot_commands[0][1] == NULL, "candidate 0 is one word");
    CHECK_STR(api.reboot_commands[1][0], "/sbin/reboot", "candidate 1");
    CHECK_STR(api.reboot_commands[2][0], "busybox", "candidate 2 argv[0]");
    CHECK_STR(api.reboot_commands[2][1], "reboot", "candidate 2 argv[1]");
    CHECK(api.reboot_commands[2][2] == NULL, "candidate 2 is two words");
}

/* ------------------------------------------------------------------ *
 * 2. Formatting
 * ------------------------------------------------------------------ */

static void test_format_size(void)
{
    char out[32];

    api.format_size(0, out, sizeof out);
    CHECK_STR(out, "0.0 MB", "_format_size(0)");
    api.format_size(1048576, out, sizeof out);
    CHECK_STR(out, "1.0 MB", "_format_size(1 MiB)");
    /* A real system image: 53,534,720 bytes reads as 51.1 MB, not 51.0. */
    api.format_size(53534720, out, sizeof out);
    CHECK_STR(out, "51.1 MB", "_format_size(a system image)");
    api.format_size(5872025, out, sizeof out);
    CHECK_STR(out, "5.6 MB", "_format_size rounds to one place");
}

static void test_format_date(void)
{
    char out[32];

    /* gmtime, never localtime: the build date must read the same on every
     * phone, whatever the clock is set to. */
    api.format_date(1785160800, out, sizeof out);
    CHECK_STR(out, "27 Jul 2026", "_format_date");
    api.format_date(0, out, sizeof out);
    CHECK_STR(out, "01 Jan 1970", "_format_date(0)");
    /* Python's time.gmtime happily goes backwards, so this one is a date and
     * not the failure branch. */
    api.format_date(-1, out, sizeof out);
    CHECK_STR(out, "31 Dec 1969", "_format_date(-1)");
    /* `except (TypeError, ValueError, OSError): return "unknown"`. */
    api.format_date(INT64_MAX, out, sizeof out);
    CHECK_STR(out, "unknown", "_format_date(out of range)");
}

static void test_size_detail(void)
{
    char out[64];

    /* Dead code in the Python -- _size_detail has no caller -- and ported
     * anyway, because it is the ProgressScreen detail callback. */
    api.size_detail(5872025, 13000000, out, sizeof out);
    CHECK_STR(out, "5.6 MB of 12.4 MB", "_size_detail");
}

/* ------------------------------------------------------------------ *
 * 3. staging.py: reading the applier's note
 * ------------------------------------------------------------------ */

static void test_read_result_absent(void)
{
    nd_upd_record rec;

    (void)fresh_root();
    CHECK(!api.read_result(&rec), "no record reads as `if not result: return`");
    /* clear_result on a missing file is staging._unlink: not an error. */
    api.clear_result();
    sa_checks++;
}

static void test_read_result_parsing(void)
{
    nd_upd_record rec;

    (void)fresh_root();
    /* Exactly what ndsys-apply.sh:record_result() writes, plus the three
     * kinds of line staging._read_record skips and one duplicate. */
    write_virtual(ND_UPDATE_RESULT_RECORD, "result=ok\n"
                                           "version=0.3.2a\n"
                                           "reason=installed\n"
                                           "\n"
                                           "# a comment\n"
                                           "not a pair\n"
                                           "  spaced  =  value with spaces  \n"
                                           "version=0.3.3a\n");
    CHECK(api.read_result(&rec), "the record is read");
    CHECK_STR(api.record_get(&rec, "result", ""), "ok", "result");
    CHECK_STR(api.record_get(&rec, "reason", ""), "installed", "reason");
    /* "last duplicate wins", which a dict assignment gives for free. */
    CHECK_STR(api.record_get(&rec, "version", ""), "0.3.3a", "the last duplicate wins");
    /* The KEY is stripped and the VALUE is not -- staging._read_record strips
     * the line, splits on the first '=', and keeps what is left as it stands.
     * All three nd_props dialects would strip the value too, which is why
     * this parser is written out rather than borrowed. */
    CHECK_STR(api.record_get(&rec, "spaced", "(missing)"), "  value with spaces",
              "the key is stripped, the value is not");
    CHECK_STR(api.record_get(&rec, "nothing", "fallback"), "fallback", "dict.get's default");

    api.clear_result();
    CHECK(!exists_virtual(ND_UPDATE_RESULT_RECORD), "clear_result removes it");
}

static void test_read_result_empty_file(void)
{
    nd_upd_record rec;

    (void)fresh_root();
    /* A file with nothing parseable in it is an empty dict, and `if not
     * result` is true for an empty dict as well as for None. */
    write_virtual(ND_UPDATE_RESULT_RECORD, "# only a comment\n\n");
    CHECK(!api.read_result(&rec), "an empty record reads as no record");
}

/* ------------------------------------------------------------------ *
 * 4. staging.py: the record the initramfs reads
 * ------------------------------------------------------------------ */

#define SHA_A "abababababababababababababababababababababababababababababababab"
#define SHA_B "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

static void fill_manifest(nd_upd_manifest *m)
{
    memset(m, 0, sizeof *m);
    (void)nd_strlcpy(m->version, "0.3.2a", sizeof m->version);
    m->buildtime = 1785160800;
    (void)nd_strlcpy(m->platform, "qemu", sizeof m->platform);
    (void)nd_strlcpy(m->sha256, SHA_A, sizeof m->sha256);
    (void)nd_strlcpy(m->changelog, "Fixed the thing.", sizeof m->changelog);
    (void)nd_strlcpy(m->verity_root_hash, SHA_B, sizeof m->verity_root_hash);
    m->verity_block_size = 4096;
    m->verity_image_blocks = 13056;
    (void)nd_strlcpy(m->verity_salt, "beef", sizeof m->verity_salt);
}

static void test_stage_package_record(void)
{
    nd_upd_manifest m;
    char text[2048];

    (void)fresh_root();
    fill_manifest(&m);

    CHECK_INT(api.stage_package(&m, "/NeoDCT/User/sdcard/update/UPDATE.ndsw", 53534720), ND_OK,
              "the record is written");

    (void)read_virtual(ND_UPDATE_PENDING_RECORD, text, sizeof text);
    /* Byte for byte, and the expected string below is not hand-written: it
     * is the output of staging.stage_package() called on the same manifest,
     * captured from the Python. This is a wire format with a busybox shell
     * script that runs before any of this code exists -- ndsys-apply.sh reads
     * every one of these keys with `sed -n "s/^KEY=//p"` -- so a rename here
     * breaks the boot rather than the build. Keys are in Python's sorted()
     * order, which is code-point order, which is strcmp. */
    CHECK_STR(text,
              "attempts=0\n"
              "buildtime=1785160800\n"
              "image_bytes=53534720\n"
              "package=UPDATE.ndsw\n"
              "platform=qemu\n"
              "sha256=" SHA_A "\n"
              "verity_block_size=4096\n"
              "verity_image_blocks=13056\n"
              "verity_root_hash=" SHA_B "\n"
              "verity_salt=beef\n"
              "version=0.3.2a\n",
              "pending.prop");

    /* The record names the package by BASENAME and never by path: the card is
     * mounted somewhere else in the initramfs than it was here, which is why
     * ndsys-apply.sh:find_package() searches for it by name. */
    CHECK(strstr(text, "package=/") == NULL, "no path in the package field");
    /* Nothing is copied -- there is nowhere on an 8 MiB user partition to put
     * a 51 MiB image, which is what stage() used to try. */
    CHECK(!exists_virtual(ND_UPDATE_PENDING_IMAGE), "no image is staged");
    /* The temp file is renamed, never left behind. */
    CHECK(!exists_virtual("/NeoDCT/User/.ndsys/pending.prop.new"), "the temp file is gone");
}

static void test_stage_package_replaces_an_earlier_attempt(void)
{
    nd_upd_manifest m;
    char text[2048];

    (void)fresh_root();
    fill_manifest(&m);

    /* An earlier attempt, image and all. Both must go, and the RECORD must go
     * first, so that a crash between the two reads as "nothing pending"
     * rather than "pending, image missing". */
    write_virtual(ND_UPDATE_PENDING_RECORD, "version=0.0.1\nattempts=2\n");
    write_virtual(ND_UPDATE_PENDING_IMAGE, "not really an image");

    CHECK_INT(api.stage_package(&m, "/media/UPDATE-2.ndsw", 100), ND_OK, "staged again");
    CHECK(!exists_virtual(ND_UPDATE_PENDING_IMAGE), "the stale image is gone");
    (void)read_virtual(ND_UPDATE_PENDING_RECORD, text, sizeof text);
    CHECK(strstr(text, "attempts=0\n") != NULL, "the attempt count starts again");
    CHECK(strstr(text, "package=UPDATE-2.ndsw\n") != NULL, "and it names the new package");
    CHECK(strstr(text, "version=0.0.1") == NULL, "nothing of the old record survives");
}

static void test_stage_package_refuses_a_newline(void)
{
    nd_upd_manifest m;

    (void)fresh_root();
    fill_manifest(&m);
    /* staging._write_record raises ValueError: "records are one line per
     * key". A changelog or a version with a newline in it would become two
     * records to a reader that parses with sed. */
    (void)nd_strlcpy(m.version, "0.3.2a\nattempts=99", sizeof m.version);
    CHECK(api.stage_package(&m, "/media/UPDATE.ndsw", 100) != ND_OK, "a newline is refused");
    CHECK(!exists_virtual(ND_UPDATE_PENDING_RECORD), "and nothing is written");
}

static void test_stage_package_reports_a_read_only_partition(void)
{
    nd_upd_manifest m;

    (void)fresh_root();
    fill_manifest(&m);
    /* .ndsys as a FILE, so mkdir -p cannot make it. That stands in for the
     * read-only user partition _stage()'s refusal is about. */
    write_virtual(ND_UPDATE_STATE_DIR, "not a directory");
    CHECK(api.stage_package(&m, "/media/UPDATE.ndsw", 100) != ND_OK,
          "a blocked state directory is reported");
}

/* ------------------------------------------------------------------ *
 * 5. Settings
 * ------------------------------------------------------------------ */

static void test_installed_version(void)
{
    char out[ND_UPDATE_VERSION_MAX];

    (void)fresh_root();
    (void)nd_settings_init();

    /* No version.prop: SettingsStorage.DEFAULTS answers. */
    api.installed_version(out, sizeof out);
    CHECK_STR(out, ND_SET_OS_VERSIONNUMBER_DFLT, "_installed_version falls back to DEFAULTS");

    /* version.prop describes the IMAGE and outranks settings.prop, which is
     * exactly why the update app reads the version from it. */
    write_virtual(ND_PATH_VERSION_PROP, "system.os.versionnumber=9.9.9z\n");
    api.installed_version(out, sizeof out);
    CHECK_STR(out, "9.9.9z", "_installed_version reads version.prop");
}

static void test_engineering_mode(void)
{
    sa_fixture fx;

    (void)fresh_root();
    (void)nd_settings_init();
    write_virtual(ND_PATH_SETTINGS_PROP, "system.ui.engineering_mode=OFF\n");

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(!api.engineering_mode(&fx.ui), "engineering_mode OFF is off");
    sa_fx_free(&fx);

    /* The default is ON: an update signed by nobody can be forced through on
     * a development phone, and this is the flag that decides it. */
    (void)fresh_root();
    write_virtual(ND_PATH_SETTINGS_PROP, "system.ui.wallpaper=NONE\n");
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    CHECK(api.engineering_mode(&fx.ui), "the default is ON");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 6. _has_network
 * ------------------------------------------------------------------ */

static char g_procdir[ND_PATH_MAX];

static void write_plain(const char *dir, const char *name, const char *text, char *out,
                        size_t out_sz)
{
    FILE *f;

    (void)nd_snprintf(out, out_sz, "%s/%s", dir, name);
    f = fopen(out, "wb");
    if (f == NULL) {
        CHECK(false, "open a fake /proc file");
        return;
    }
    (void)fputs(text, f);
    (void)fclose(f);
}

static void test_has_network(void)
{
    char v4[ND_PATH_MAX];
    char v6[ND_PATH_MAX];
    char none[ND_PATH_MAX];

    (void)nd_snprintf(none, sizeof none, "%s/nothing-here", g_procdir);

    /* The header line is skipped -- read().splitlines()[1:] -- so a file with
     * only a header has no route in it. */
    write_plain(g_procdir, "route-empty",
                "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n", v4, sizeof v4);
    CHECK(!api.proc_route(v4, none), "a header alone is not a default route");

    write_plain(g_procdir, "route-default",
                "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n"
                "wwan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\n",
                v4, sizeof v4);
    CHECK(api.proc_route(v4, none), "destination 00000000 in field 1 is the default route");

    /* `if len(fields) > 2` -- a truncated line is ignored rather than read. */
    write_plain(g_procdir, "route-short",
                "Iface\tDestination\n"
                "wwan0\t00000000\n",
                v4, sizeof v4);
    CHECK(!api.proc_route(v4, none), "a line with three fields or fewer is ignored");

    write_plain(g_procdir, "route-host",
                "Iface\tDestination\tGateway\n"
                "wwan0\t0101A8C0\t00000000\t0003\t0\t0\t0\tFFFFFF00\n",
                v4, sizeof v4);
    CHECK(!api.proc_route(v4, none), "a route to a host is not a default route");

    /* The branch that matters on the real phone: T-Mobile is IPv6-only, and
     * ::/0 is an all-zero 32-hex-digit prefix with a 00 prefix length. There
     * is no header line here. */
    write_plain(g_procdir, "ipv6-default",
                "00000000000000000000000000000000 00 "
                "00000000000000000000000000000000 00 "
                "fe800000000000000000000000000001 00000400 00000001 00000000 00000003 wwan0\n",
                v6, sizeof v6);
    CHECK(api.proc_route(none, v6), "an IPv6 default route counts");

    write_plain(g_procdir, "ipv6-prefix",
                "20010db8000000000000000000000000 40 "
                "00000000000000000000000000000000 00 "
                "00000000000000000000000000000000 00000100 00000000 00000000 00000001 wwan0\n",
                v6, sizeof v6);
    CHECK(!api.proc_route(none, v6), "a /40 prefix is not a default route");

    CHECK(!api.proc_route(none, none), "two missing files are `except OSError: pass`");
    CHECK(!api.proc_route(NULL, NULL), "NULL is refused");

    /* "uistub drives this code on a build host whose /proc is not the
     * phone's; PathRemap cannot cover /proc, so the stub says so instead." */
    (void)setenv("NEODCT_STUB", "1", 1);
    CHECK(!api.has_network(), "NEODCT_STUB forces offline");
    (void)unsetenv("NEODCT_STUB");
}

/* ------------------------------------------------------------------ *
 * 7. execvp's lookup
 * ------------------------------------------------------------------ */

static void test_which(void)
{
    char out[ND_PATH_MAX];

    /* The same function as nd_power_which(), duplicated because two app
     * shared objects cannot share one -- exactly as the two Python modules
     * each carry their own copy. */
    CHECK(api.which("/bin/sh", out, sizeof out) || api.which("sh", out, sizeof out),
          "a shell is found one way or the other");
    CHECK(!api.which("nd-there-is-no-such-program", out, sizeof out),
          "a missing name is Python's OSError");
    CHECK(!api.which("/nd/no/such/path", out, sizeof out), "a missing absolute path is refused");
    CHECK(!api.which(NULL, out, sizeof out), "NULL is refused");
    CHECK(!api.which("", out, sizeof out), "the empty name is refused");
}

/* ------------------------------------------------------------------ *
 * 8. The service boundary
 * ------------------------------------------------------------------ */

static void test_service_is_present(void)
{
    nd_upd_package *pkg = (nd_upd_package *)(void *)&api;
    nd_upd_manifest m;
    nd_upd_release rel;
    char why[ND_UPDATE_WHY_MAX];

    /* This used to assert the opposite of every line below it, and was
     * right to: there was no zip reader, manifest parser or RSA verifier in
     * the tree, and the boundary said so rather than approximating any of
     * them. All three landed, so the assertions invert -- but only the
     * CARD path. remote.* still has no HTTP or TLS underneath it. */
    CHECK(api.svc_available(), "the update service is in this build");

    /* A path that is not a package is INVALID -- a broken package -- and
     * never UNAVAILABLE. Telling the owner their build cannot read updates
     * when it can is the more confusing of the two lies. */
    why[0] = '\0';
    pkg = NULL;
    CHECK_INT(api.svc_open("/media/does-not-exist.ndsw", &pkg, why, sizeof why), ND_UPDSVC_INVALID,
              "a missing package is INVALID, not unavailable");
    CHECK(pkg == NULL, "and hands back nothing");
    CHECK(why[0] != '\0', "with a reason");

    /* check_compatible is delegated to nd_manifest.c -- the brick check has
     * exactly one implementation. fill_manifest() builds a qemu manifest. */
    fill_manifest(&m);
    CHECK_INT(api.svc_compat(&m, "qemu", "6.12.47", why, sizeof why), ND_UPDSVC_OK,
              "a matching platform and new enough kernel is compatible");

    why[0] = '\0';
    CHECK_INT(api.svc_compat(&m, "luckfox-armv7", "6.12.47", why, sizeof why),
              ND_UPDSVC_INCOMPATIBLE, "the wrong platform is THE BRICK CASE");
    CHECK(why[0] != '\0', "and says which way round");

    /* A NULL package cannot be verified, and the answer is not "unsigned":
     * engineering mode may acknowledge an unsigned package and continue, so
     * BAD_SIGNATURE must mean the signature was examined. */
    CHECK_INT(api.svc_verify(NULL, ND_UPDATE_RELEASE_KEY, why, sizeof why), ND_UPDSVC_INVALID,
              "verifying nothing is INVALID, not BAD_SIGNATURE");

    /* remote.* is wired now, so this no longer answers UNAVAILABLE -- and it
     * must not reach a network to prove it. PATH is nd_remote's transport
     * seam: with no curl anywhere on it, nd_remote_latest() stops at
     * "cannot reach GitHub" without opening a socket, which is a real code
     * path (a broken image) and a deterministic one. test_remote.c drives
     * the rest of it against a stand-in curl serving committed fixtures. */
    {
        char *saved = getenv("PATH");
        char keep[ND_PATH_MAX];

        keep[0] = '\0';
        if (saved != NULL)
            (void)nd_strlcpy(keep, saved, sizeof keep);
        (void)setenv("PATH", "/nonexistent-so-there-is-no-curl", 1);

        why[0] = '\0';
        CHECK_INT(api.svc_latest("qemu", &rel, why, sizeof why), ND_UPDSVC_NETWORK,
                  "remote.latest is wired, and a phone with no curl says so");
        CHECK(strstr(why, "cannot reach GitHub") != NULL, "with the Python's own wording");

        if (keep[0] != '\0')
            (void)setenv("PATH", keep, 1);
        else
            (void)unsetenv("PATH");
    }
}

/* ------------------------------------------------------------------ *
 * 9. The screens
 * ------------------------------------------------------------------ */

static bool fixture_up(sa_fixture *fx)
{
    if (sa_fx_init(fx))
        return true;
    CHECK(false, "fixture");
    sa_fx_free(fx);
    return false;
}

static void test_confirm_and_refuse(void)
{
    sa_fixture fx;

    (void)fresh_root();
    /* MessageDialog drains the channel before drawing, so the answer has to
     * arrive as a repeat rather than as a queued press. */
    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    CHECK(api.confirm(&fx.ui, "Install Anyway?", "OK"), "ENTER is yes");
    /* _refuse ignores the answer entirely; it must simply return. */
    api.refuse(&fx.ui, *api.msg_invalid);
    nd_vclock_disable();
    sa_checks++;
    sa_fx_free(&fx);

    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_CLEAR), "held CLEAR");
    nd_vclock_enable();
    CHECK(!api.confirm(&fx.ui, "No update on the card.\nLook online?", "Check"),
          "CLEAR cancels a confirmation");
    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_page(void)
{
    sa_fixture fx;
    int32_t key;

    (void)fresh_root();
    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    key = api.page(&fx.ui, "Up to date", "NeoDCT 0.3.1a", *api.no_package_help, NULL, NULL, "Back",
                   true);
    nd_vclock_disable();
    CHECK_INT(key, ND_KEY_ENTER, "_page returns the key that dismissed it");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the page");
    sa_fx_free(&fx);
}

static void test_choose_package(void)
{
    char paths[3][ND_STORAGE_PATH_MAX];
    sa_fixture fx;

    (void)nd_strlcpy(paths[0], "/sdcard/update/UPDATE.ndsw", ND_STORAGE_PATH_MAX);
    (void)nd_strlcpy(paths[1], "/sdcard/update/older.ndsw", ND_STORAGE_PATH_MAX);
    (void)nd_strlcpy(paths[2], "/sdcard/update/other.ndsw", ND_STORAGE_PATH_MAX);

    (void)fresh_root();

    /* One package is used without asking -- and without drawing anything. */
    if (!fixture_up(&fx))
        return;
    nd_vclock_enable();
    CHECK_INT(api.choose(&fx.ui, paths, 1u), 0, "one package needs no menu");
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 0, "and draws nothing");
    sa_fx_free(&fx);

    /* More than one gets a VerticalList, which does NOT flush before its
     * first draw, so a queued press is enough. */
    if (!fixture_up(&fx))
        return;
    CHECK(sa_send(&fx, ND_KEY_ENTER), "queued ENTER");
    nd_vclock_enable();
    CHECK_INT(api.choose(&fx.ui, paths, 3u), 0, "ENTER picks the highlighted entry");
    nd_vclock_disable();
    sa_fx_free(&fx);

    if (!fixture_up(&fx))
        return;
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");
    nd_vclock_enable();
    CHECK_INT(api.choose(&fx.ui, paths, 3u), ND_WIDGET_BACK, "Back is `return None`");
    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_report_last_result(void)
{
    sa_fixture fx;

    /* result=ok is a page with an "OK" softkey. */
    (void)fresh_root();
    write_virtual(ND_UPDATE_RESULT_RECORD, "result=ok\nversion=0.3.2a\nreason=installed\n");
    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    api.report_last_result(&fx.ui);
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "the \"Updated\" page is shown");
    /* Shown ONCE and then forgotten, whichever way it went. */
    CHECK(!exists_virtual(ND_UPDATE_RESULT_RECORD), "and the record is cleared");
    sa_fx_free(&fx);

    /* Anything else is a refusal. */
    (void)fresh_root();
    write_virtual(ND_UPDATE_RESULT_RECORD,
                  "result=failed\nversion=0.3.2a\nreason=image sha256 mismatch before write\n");
    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    api.report_last_result(&fx.ui);
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "the failure is reported");
    CHECK(!exists_virtual(ND_UPDATE_RESULT_RECORD), "and forgotten too");
    sa_fx_free(&fx);

    /* No record at all draws nothing. */
    (void)fresh_root();
    if (!fixture_up(&fx))
        return;
    nd_vclock_enable();
    api.report_last_result(&fx.ui);
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 0, "no record, no screen");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 10. _backup_user_data
 * ------------------------------------------------------------------ */

static void test_backup_without_a_card(void)
{
    (void)fresh_root();
    nd_storage_set_paths(CARD_MOUNT, CARD_STATE);
    /* Storage.folder() hands out nothing unless the card state is "ready", so
     * the backup is skipped -- and it is skipped SILENTLY, which is what
     * makes _install carry on regardless. */
    CHECK(!api.backup(NULL), "no card, no backup");
}

static void test_backup_copies_the_databases(void)
{
    char dir[ND_PATH_MAX];
    char found[ND_PATH_MAX];
    char text[256];
    DIR *d;
    struct dirent *ent;
    bool have = false;

    (void)fresh_root();
    insert_ready_card();

    /* A card but no databases: still false, and nothing is created. */
    CHECK(!api.backup(NULL), "a missing db directory is not a backup");

    write_virtual("/NeoDCT/User/db/phonebook.db", "phonebook bytes");
    write_virtual("/NeoDCT/User/db/sms_inbox.db", "inbox bytes");
    /* Only *.db is copied: the walk filters on the suffix, so a journal or a
     * stray note stays on the phone. */
    write_virtual("/NeoDCT/User/db/notes.txt", "not a database");

    CHECK(api.backup(NULL), "the databases are backed up");

    /* The destination is <card>/backup_db/<local %Y%m%d-%H%M%S>. The exact
     * name is a timestamp, so the test looks for the one directory rather
     * than guessing the second it ran in. */
    (void)nd_snprintf(dir, sizeof dir, "%s/backup_db", CARD_MOUNT);
    {
        char resolved[ND_PATH_MAX];

        (void)nd_path_resolve(resolved, sizeof resolved, dir);
        d = opendir(resolved);
    }
    if (d == NULL) {
        CHECK(false, "backup_db is readable");
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        (void)nd_snprintf(found, sizeof found, "%s/%s", dir, ent->d_name);
        have = true;
        break;
    }
    (void)closedir(d);
    if (!have) {
        CHECK(false, "a dated folder was made");
        return;
    }
    CHECK_INT(strlen(strrchr(found, '/') + 1), 15, "the folder is %Y%m%d-%H%M%S");

    {
        char path[ND_PATH_MAX];

        (void)nd_snprintf(path, sizeof path, "%s/phonebook.db", found);
        (void)read_virtual(path, text, sizeof text);
        CHECK_STR(text, "phonebook bytes", "the contents came across");

        (void)nd_snprintf(path, sizeof path, "%s/sms_inbox.db", found);
        CHECK(exists_virtual(path), "both databases came across");

        (void)nd_snprintf(path, sizeof path, "%s/notes.txt", found);
        CHECK(!exists_virtual(path), "and nothing that is not a .db did");
    }
}

/* ------------------------------------------------------------------ *
 * 11. _stage
 * ------------------------------------------------------------------ */

static void test_stage_screen(void)
{
    nd_upd_manifest m;
    sa_fixture fx;

    (void)fresh_root();
    fill_manifest(&m);
    if (!fixture_up(&fx))
        return;
    nd_vclock_enable();
    CHECK(api.stage(&fx.ui, &m, "/sdcard/update/UPDATE.ndsw", 53534720), "_stage succeeds");
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 0, "and says nothing when it works");
    CHECK(exists_virtual(ND_UPDATE_PENDING_RECORD), "the record is there for the applier");
    sa_fx_free(&fx);

    /* .ndsys as a file, so mkdir -p fails and the refusal appears. */
    (void)fresh_root();
    write_virtual(ND_UPDATE_STATE_DIR, "not a directory");
    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    CHECK(!api.stage(&fx.ui, &m, "/sdcard/update/UPDATE.ndsw", 53534720),
          "a read-only user partition is refused");
    nd_vclock_disable();
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "with a dialog");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 12. _install stops safely
 * ------------------------------------------------------------------ */

static void test_install_stops_at_the_missing_reader(void)
{
    sa_fixture fx;

    (void)fresh_root();
    insert_ready_card();
    write_virtual("/sdcard/update/UPDATE.ndsw", "PK\x03\x04 not really a package");

    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    api.install(&fx.ui, "/sdcard/update/UPDATE.ndsw");
    nd_vclock_disable();

    /* Exactly one screen: the refusal. Not the update page, which needs a
     * manifest nothing can produce. */
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one screen, the refusal");
    /* AND NOTHING WAS STAGED. This is the assertion that matters: the app
     * cannot verify a package, so it must not leave the applier anything to
     * write over the system partition on the next boot. */
    CHECK(!exists_virtual(ND_UPDATE_PENDING_RECORD), "nothing is staged");
    CHECK(!exists_virtual(ND_UPDATE_PENDING_IMAGE), "and no image is left behind");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 13. run()
 * ------------------------------------------------------------------ */

static void run_with(const char *what, int32_t expect_frames)
{
    sa_fixture fx;
    int rc;

    if (!fixture_up(&fx))
        return;
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER");
    nd_vclock_enable();
    rc = api.run(&fx.ui);
    nd_vclock_disable();
    CHECK_INT(rc, 0, what);
    CHECK_INT(nd_capture_frames_drawn(fx.cap), expect_frames, "the expected number of screens");
    sa_fx_free(&fx);
}

static void test_run_no_card(void)
{
    (void)fresh_root();
    nd_storage_set_paths(CARD_MOUNT, CARD_STATE);
    /* No sdcard.prop at all is an absent card. */
    run_with("no card: one page, then 0", 1);
}

static void test_run_card_not_ready(void)
{
    (void)fresh_root();
    nd_storage_set_paths(CARD_MOUNT, CARD_STATE);
    /* A card straight out of a camera: mountable, but not ours yet. */
    write_virtual(CARD_STATE, "state=mounted\ndevice=/dev/vdc\nfstype=vfat\nlabel=CAM\n");
    if (nd_mkdir_p(CARD_MOUNT "/DCIM", 0755u) != ND_OK)
        CHECK(false, "card folder");
    run_with("not ready: one page, then 0", 1);
}

static void test_run_up_to_date(void)
{
    (void)fresh_root();
    (void)nd_settings_init();
    insert_ready_card();
    /* NEODCT_STUB is what keeps _has_network() off a build host's /proc, so
     * the "Look online?" dialog is not offered and the one page appears. */
    (void)setenv("NEODCT_STUB", "1", 1);
    run_with("nothing on the card: the \"Up to date\" page, then 0", 1);
    (void)unsetenv("NEODCT_STUB");
}

static void test_run_reports_then_shows_the_card(void)
{
    (void)fresh_root();
    (void)nd_settings_init();
    insert_ready_card();
    write_virtual(ND_UPDATE_RESULT_RECORD, "result=ok\nversion=0.3.2a\nreason=installed\n");
    (void)setenv("NEODCT_STUB", "1", 1);
    /* _report_last_result runs FIRST, before the card is even looked at, so
     * this is two screens: the report and then "Up to date". */
    run_with("the last result is reported first", 2);
    (void)unsetenv("NEODCT_STUB");
    CHECK(!exists_virtual(ND_UPDATE_RESULT_RECORD), "and cleared on the way past");
}

static void test_run_with_a_package_on_the_card(void)
{
    (void)fresh_root();
    (void)nd_settings_init();
    insert_ready_card();
    write_virtual("/sdcard/update/UPDATE.ndsw", "PK\x03\x04 not really a package");
    /* One package, so no menu: straight into _install, which stops at the
     * missing reader with one refusal. */
    run_with("a package on the card goes straight to _install", 1);
    CHECK(!exists_virtual(ND_UPDATE_PENDING_RECORD), "and still nothing is staged");
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    api.refuse(NULL, "x");
    CHECK(!api.confirm(NULL, "x", "OK"), "confirm(NULL) is no");
    CHECK_INT(api.page(NULL, "t", "s", "b", NULL, NULL, "Back", true), ND_KEY_NONE,
              "page(NULL) draws nothing");
    CHECK(!api.stage(NULL, NULL, NULL, 0), "stage(NULL) is false");
    api.install(NULL, NULL);
    api.report_last_result(NULL);
    CHECK_INT(api.choose(NULL, NULL, 0u), ND_WIDGET_BACK, "choose(NULL) is Back");
    CHECK_INT(api.stage_package(NULL, NULL, 0), ND_ERR_INVAL, "stage_package(NULL) is refused");
    CHECK(!api.read_result(NULL), "read_result(NULL) is false");
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("Update", "ndupdate-frames");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndupdate-proc", g_procdir, sizeof g_procdir)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_constants);
    RUN(test_help_strings);
    RUN(test_reboot_table);
    RUN(test_format_size);
    RUN(test_format_date);
    RUN(test_size_detail);
    RUN(test_read_result_absent);
    RUN(test_read_result_parsing);
    RUN(test_read_result_empty_file);
    RUN(test_stage_package_record);
    RUN(test_stage_package_replaces_an_earlier_attempt);
    RUN(test_stage_package_refuses_a_newline);
    RUN(test_stage_package_reports_a_read_only_partition);
    RUN(test_installed_version);
    RUN(test_engineering_mode);
    RUN(test_has_network);
    RUN(test_which);
    RUN(test_service_is_present);
    RUN(test_confirm_and_refuse);
    RUN(test_page);
    RUN(test_choose_package);
    RUN(test_report_last_result);
    RUN(test_backup_without_a_card);
    RUN(test_backup_copies_the_databases);
    RUN(test_stage_screen);
    RUN(test_install_stops_at_the_missing_reader);
    RUN(test_run_no_card);
    RUN(test_run_card_not_ready);
    RUN(test_run_up_to_date);
    RUN(test_run_reports_then_shows_the_card);
    RUN(test_run_with_a_package_on_the_card);
    RUN(test_null_safety);

    rc = sa_end(h, "test_update_app");
    drop_roots();
    sa_rmtree(g_procdir);
    nd_storage_set_paths(NULL, NULL);
    return rc;
}
