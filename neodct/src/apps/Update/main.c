/* apps/Update/main.c -- install an UPDATE.ndsw from the SD card, app id 12.
 *
 * A one-to-one port of System/apps/Update/main.py (467 lines). Its docstring
 * is the specification, and it is reproduced rather than summarised because
 * this is the one app that can turn the phone into a brick:
 *
 *     The app never writes the system partition itself -- it cannot, the
 *     rootfs is a mounted read-only squashfs. It verifies a package, copies
 *     the image to the user partition as a pending update and reboots; the
 *     initramfs applies it before the root is mounted
 *     (neodct/initramfs/ndsys-apply.sh).
 *
 *     The shape of the flow is one page and one bar: a page describing the
 *     update (picture, version, size, release notes) whose softkey installs
 *     it, then a single progress screen that backs up the databases and
 *     copies the image. Nothing is asked twice, and the backup is not a
 *     question -- it just happens.
 *
 *     What each refusal means, and whether it can be overridden:
 *
 *       INVALID UPDATE     the zip is broken or incomplete. No override:
 *                          there is nothing installable in the file.
 *       WRONG UPDATE       built for other hardware or a newer kernel. No
 *                          override: installing it would brick the phone.
 *       BAD SIGNATURE      intact, but unsigned or signed by the wrong key.
 *                          Engineering mode may acknowledge and continue.
 *
 * (The docstring's second paragraph is out of date in the Python itself:
 * _stage()'s own comment records that nothing is copied any more, because the
 * user partition is 8 MiB on the Luckfox and a system image is 51 MiB. The
 * applier reads the image straight out of the .ndsw on the card. Both
 * comments are kept, as they are in the Python.)
 *
 * ============ HALF OF THIS APP CANNOT RUN, AND SAYS SO ============
 *
 * The app is the top half of a two-part system. The bottom half is
 * System/core/UpdateService/ -- the zip reader, the manifest validator, the
 * RSA verifier, the GitHub client -- and in C that is libndupdate.so, which
 * does not exist. See update_app.h and service.c: the boundary is real, every
 * call into it answers "unavailable", and _install() therefore stops on its
 * first step with a refusal that says the reader is missing rather than
 * pretending the package is corrupt.
 *
 * Everything above that boundary is here and works: the last-install report,
 * all four card states, the package menu, the backup, the staging record and
 * the reboot. Everything is exercised by test/unit/test_update_app.c EXCEPT
 * the two functions that end the session -- see the note at the bottom of
 * update_app.h, and the same deliberate hole in power.h.
 *
 * ============ FOUR PLACES THE C HAD TO SAY SOMETHING ============
 *
 * 1. _engineering_mode(ui) reads `getattr(ui, "engineering_mode", None)` and
 *    only falls back to the setting when the attribute is missing. On the
 *    phone it never is: core/main.py:639 assigns it from
 *    _setting_is_enabled(get_setting("system.ui.engineering_mode", "ON")),
 *    which is exactly what nd_ui_engineering_mode() computes. So the C calls
 *    that and the five-literal fallback is dead code in both languages. It
 *    also means nd_setting_update_truthy() -- the third boolean parser,
 *    added to nd_settings.h for this one call site (OPEN-QUESTIONS.md P-1) --
 *    has no caller here after all.
 *
 * 2. str(OSError) IS NOT REPRODUCIBLE. "Could not stage the update.\n%s" %
 *    exc prints "[Errno 30] Read-only file system: '/NeoDCT/User/.ndsys'".
 *    The C prints strerror(errno) after the same newline. Same message, same
 *    place, different words for the same failure -- OPEN-QUESTIONS.md PW-3
 *    records the identical substitution in the Power app.
 *
 * 3. subprocess.Popen(["reboot"]) LOOKS ALONG $PATH and nd_proc_spawn() takes
 *    a path, so nd_update_which() does the execvp lookup first and its
 *    failure is the OSError the Python's `except OSError: continue` catches.
 *
 *    THE REBOOT NO LONGER USES IT. Resolving a program name along $PATH and
 *    fork/exec'ing it with the privilege to power-cycle the machine was the
 *    only reason this app, Power and Downgrade needed privilege the other
 *    twenty-two do not, so nd_update_reboot() asks the core instead --
 *    nd_svc_reboot(), one request with no arguments at all, over the service
 *    channel every app child already has. The candidate list and the lookup
 *    are in lib/nd_svc.c now, where the core is the only reader.
 *    spec-app-services.md section 9.
 *
 *    nd_update_which() stays because STAGING still spawns `sync`, and that
 *    is a different call site: it runs long before a reboot is in sight, it
 *    has to complete before the staging record is written, and a `sync` that
 *    is missing is survivable in a way a missing `reboot` is not.
 *
 * 4. _thumbnail() decodes the package's PNG IN MEMORY and hands the Image
 *    object to DetailPage. nd_detailpage_init() takes a path and nothing
 *    else, and there is no way to hand it a decoded nd_image -- the struct's
 *    `image` field is filled during layout, so assigning it afterwards would
 *    lay the page out for the wrong size. nd_image_open_mem() exists, so when
 *    the package reader lands the thumbnail will have to be written to a
 *    temporary file or nd_widgets.h will need an nd_detailpage_init_image().
 *    Recorded here because it is a missing widget API, not an app decision.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_storage.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "update_app.h"

/* ------------------------------------------------------------------ *
 * The strings, verbatim
 * ------------------------------------------------------------------ *
 *
 * The blank lines are real "\n\n" paragraph breaks, "--" is two ASCII
 * hyphens and not an em dash, and the quotes around "update" and
 * "UPDATE.ndsw" are plain ASCII double quotes. All three are user-visible and
 * test_update_app.c pins them.
 */

const char *const nd_update_no_card_help =
    "Updates are read from an SD card.\n"
    "\n"
    "Format a card as FAT32, make a folder called \"update\" on it and copy "
    "UPDATE.ndsw into that folder.\n"
    "\n"
    "Put the card in the phone and open Update again.";

const char *const nd_update_not_ready_help =
    "The card in the phone is not set up for NeoDCT.\n"
    "\n"
    "Settings can prepare it for you: Settings, Memory card, Prepare card.\n"
    "\n"
    "Preparing a card makes the folders NeoDCT uses. It does not erase what "
    "is already on it.";

const char *const nd_update_no_package_help =
    "There is nothing to install from the card.\n"
    "\n"
    "To update, copy UPDATE.ndsw into the \"update\" folder on the card.\n"
    "\n"
    "An update file is built by \"make update\" in the buildroot tree and "
    "fits one kind of hardware only -- a QEMU build will not install on a "
    "real phone, or the other way round.";

/* The refusals. Two exclamation marks on the end of two of them is not a
 * typo; it is what the phone says. */
const char *const nd_update_msg_invalid = "INVALID UPDATE! UPDATE MAY BE CORRUPT!!";
const char *const nd_update_msg_bad_signature = "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!";
const char *const nd_update_msg_wrong_prefix = "WRONG UPDATE FOR THIS PHONE!\n";
const char *const nd_update_msg_install_anyway = "Install Anyway?";
const char *const nd_update_msg_no_update_folder = "The card has no update folder.";
const char *const nd_update_msg_no_release_notes = "No release notes came with this build.";
const char *const nd_update_msg_cannot_write_prefix = "Cannot write to the user partition.\n";
const char *const nd_update_msg_cannot_stage_prefix = "Could not stage the update.\n";

/* THE ONE STRING THIS PORT ADDED. See update_app.h: the Python has no branch
 * for "the package reader is missing" because in Python it never is. */
const char *const nd_update_msg_no_reader =
    "CANNOT OPEN UPDATES!\nThis build has no package reader.";

/* _reboot's candidate list is gone from this file. It, and the $PATH lookup
 * that walked it, are in lib/nd_svc.c now -- see the header comment and
 * spec-app-services.md section 9. `sync` stays, because STAGING still spawns
 * it, which is a different call site with a different argument. */
static const char *const SYNC_CMD[] = {"sync", NULL};

/* time.sleep(30): "init takes a moment to bring things down; sit here rather
 * than returning to the launcher and looking like nothing happened." */
#define UPDATE_REBOOT_DWELL 30.0

/* ------------------------------------------------------------------ *
 * Formatting
 * ------------------------------------------------------------------ */

void nd_update_format_size(int64_t count, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    /* "%.1f MB" % (count / 1048576.0) -- MiB, called MB on screen. */
    (void)snprintf(out, out_sz, "%.1f MB", (double)count / 1048576.0);
}

void nd_update_format_date(int64_t stamp, char *out, size_t out_sz)
{
    /* Python's %b is the C locale's abbreviated month with the default
     * locale. The table is spelled out rather than left to strftime, because
     * an image that ever sets LC_TIME would silently change a build date that
     * has to read the same everywhere. */
    static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t when;
    struct tm parts;

    if (out == NULL || out_sz == 0u)
        return;

    when = (time_t)stamp;
    /* gmtime, NOT localtime: the build date must read the same on every
     * phone. `except (TypeError, ValueError, OSError): return "unknown"`. */
    if ((int64_t)when != stamp || gmtime_r(&when, &parts) == NULL || parts.tm_mon < 0 ||
        parts.tm_mon > 11) {
        (void)nd_strlcpy(out, "unknown", out_sz);
        return;
    }
    (void)snprintf(out, out_sz, "%02d %s %04d", parts.tm_mday, MONTHS[parts.tm_mon],
                   parts.tm_year + 1900);
}

void nd_update_size_detail(int64_t done, int64_t total, char *out, size_t out_sz)
{
    char a[32];
    char b[32];

    if (out == NULL || out_sz == 0u)
        return;
    nd_update_format_size(done, a, sizeof a);
    nd_update_format_size(total, b, sizeof b);
    (void)snprintf(out, out_sz, "%s of %s", a, b);
}

/* ------------------------------------------------------------------ *
 * Settings
 * ------------------------------------------------------------------ */

bool nd_update_engineering_mode(nd_ui *ui)
{
    /* "Engineering mode as the rest of the UI decides it" -- see note 1 in
     * the header comment for why the setting is not re-read here. */
    return nd_ui_engineering_mode(ui);
}

void nd_update_installed_version(char *out, size_t out_sz)
{
    char raw[ND_UPDATE_VERSION_MAX];
    const char *start;
    size_t len;

    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    if (nd_settings_get_copy(ND_SET_OS_VERSIONNUMBER, "", raw, sizeof raw) != ND_OK)
        return;

    /* str(...).strip() */
    start = raw;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
        start++;
    len = strlen(start);
    while (len > 0u) {
        char c = start[len - 1u];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        len--;
    }
    if (len >= out_sz)
        len = out_sz - 1u;
    memcpy(out, start, len);
    out[len] = '\0';
}

/* ------------------------------------------------------------------ *
 * The three screen helpers
 * ------------------------------------------------------------------ */

void nd_update_refuse(nd_ui *ui, const char *message)
{
    static const int32_t ACCEPT[1] = {ND_KEY_ENTER};
    nd_msgdialog dialog;

    if (ui == NULL || message == NULL)
        return;
    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_button(&dialog, "OK");
    /* cancel_keys=(): C does NOT dismiss a dead end. The softkey is the only
     * way out, which is the whole point -- a refusal that can be swiped away
     * by the key people press to go back is a refusal nobody reads. */
    nd_msgdialog_set_keys(&dialog, ACCEPT, 1u, NULL, 0u);
    (void)nd_msgdialog_show(&dialog);
}

bool nd_update_confirm(nd_ui *ui, const char *message, const char *button_text)
{
    nd_msgdialog dialog;

    if (ui == NULL || message == NULL)
        return false;
    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_button(&dialog, button_text);
    /* The default key sets stand: ENTER is yes, CLEAR cancels. */
    return nd_msgdialog_show(&dialog) == ND_KEY_ENTER;
}

int32_t nd_update_page(nd_ui *ui, const char *title, const char *subtitle, const char *body,
                       const char *image, const char *badge, const char *softkey_text,
                       bool cancellable)
{
    nd_detailpage page;
    int32_t key;

    if (ui == NULL)
        return ND_KEY_NONE;
    if (nd_detailpage_init(&page, ui, title, subtitle, body, image, badge, ND_UPDATE_HEADER,
                           softkey_text) != ND_OK) {
        nd_log_err(ND_LOG_UPDATE, "cannot lay out the \"%s\" page", (title != NULL) ? title : "");
        return ND_KEY_NONE;
    }
    /* cancel_keys=(). There is no setter for a DetailPage's key sets -- the
     * fields are public and nd_detailpage_show() reads them directly. */
    if (!cancellable)
        page.n_cancel = 0u;
    key = nd_detailpage_show(&page);
    nd_detailpage_free(&page);
    return key;
}

/* ------------------------------------------------------------------ *
 * Starting other programs
 * ------------------------------------------------------------------ */

bool nd_update_which(const char *name, char *out, size_t out_sz)
{
    const char *path;
    const char *seg;

    if (name == NULL || name[0] == '\0' || out == NULL || out_sz == 0u)
        return false;

    /* execvp: a name containing a slash is a path, not a name. */
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) != 0)
            return false;
        return (size_t)snprintf(out, out_sz, "%s", name) < out_sz;
    }

    path = getenv("PATH");
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

    for (seg = path; seg != NULL;) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        int n;

        /* An empty element means the current directory, to the shell and to
         * execvp alike. */
        if (len == 0u)
            n = snprintf(out, out_sz, "./%s", name);
        else
            n = snprintf(out, out_sz, "%.*s/%s", (int)len, seg, name);

        if (n > 0 && (size_t)n < out_sz && access(out, X_OK) == 0)
            return true;

        seg = (colon != NULL) ? colon + 1 : NULL;
    }

    out[0] = '\0';
    return false;
}

/* subprocess.Popen(command): no descriptor plan, so the child keeps this
 * process's stdout and stderr exactly as Popen's defaults do. */
static bool spawn_inherit(const char *const *argv, pid_t *pid_out)
{
    char exe[ND_PATH_MAX];
    nd_proc_spec spec;

    if (!nd_update_which(argv[0], exe, sizeof exe))
        return false;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    spec.n_fds = 0u;
    return nd_proc_spawn(exe, &spec, pid_out) == ND_OK;
}

/* subprocess.call(["sync"]): spawn and block until it finishes.
 *
 * Unlike the Python, a `sync` that cannot be found is not fatal here.
 * subprocess.call would raise FileNotFoundError straight out of _stage and
 * reach the crash screen; an image without /bin/sync showing a stack trace
 * instead of staging an update is worse in every way. Same decision as the
 * Power app, OPEN-QUESTIONS.md PW-2. */
static void run_sync(void)
{
    pid_t pid = -1;

    if (spawn_inherit(SYNC_CMD, &pid))
        (void)nd_proc_wait(pid, -1.0, NULL);
    else
        nd_log_err(ND_LOG_UPDATE, "cannot run sync: %s", strerror(errno));
}

/* time.sleep(seconds), in slices, so SIGTERM from the core's modem thread is
 * noticed rather than sat through. Skipped under the virtual clock: in
 * capture mode time is a frame counter and a real sleep moves no pixel. */
static void dwell(double seconds)
{
    double left = seconds;

    if (seconds <= 0.0 || nd_vclock_enabled())
        return;
    while (left > 0.0 && !nd_app_should_exit()) {
        struct timespec req;
        double slice = (left < 0.1) ? left : 0.1;

        req.tv_sec = 0;
        req.tv_nsec = (long)(slice * 1e9);
        (void)nanosleep(&req, NULL);
        left -= slice;
    }
}

void nd_update_reboot(nd_ui *ui)
{
    ND_UNUSED(ui); /* the Python takes it and never uses it either */

    /* The candidate walk and the sync that preceded it are the core's now.
     * It resolves the binary, ANSWERS, syncs and only then spawns, which is
     * why the ordinary five-second wait for that answer cannot abort a
     * reboot that was about to happen. spec-app-services.md 9.4 and 9.5. */
    (void)nd_svc_reboot();

    /* Note there is still no failure dialog: unlike the Power app, _reboot()
     * does not tell anybody when nothing could be started, which is why the
     * return is discarded rather than acted on. It sits for thirty seconds
     * either way. That is the Python's, and it is what a phone with no
     * reboot binary looks like today. */
    dwell(UPDATE_REBOOT_DWELL);
}

/* ------------------------------------------------------------------ *
 * _has_network
 * ------------------------------------------------------------------ */

/* One line of /proc/net/route or /proc/net/ipv6_route, split the way Python's
 * str.split() with no argument splits: on runs of whitespace, no empty
 * tokens. Returns how many fields were found, writing the first `max` of
 * them. */
static size_t split_fields(const char *line, const char **fields, size_t *lens, size_t max)
{
    size_t n = 0u;
    const char *p = line;

    while (*p != '\0') {
        const char *start;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '\0')
            break;
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if (n < max) {
            fields[n] = start;
            lens[n] = (size_t)(p - start);
        }
        n++;
    }
    return n;
}

static bool field_is(const char *field, size_t len, const char *want)
{
    return strlen(want) == len && strncmp(field, want, len) == 0;
}

/* The IPv4 half: skip the header line, then look for a destination of
 * 00000000 in field 1. */
static bool has_default_route_v4(const char *path)
{
    char line[512];
    FILE *f;
    bool first = true;
    bool found = false;

    f = fopen(path, "r");
    if (f == NULL)
        return false; /* `except OSError: pass` */
    while (fgets(line, (int)sizeof line, f) != NULL) {
        const char *fields[4];
        size_t lens[4];
        size_t n;

        if (first) {
            first = false;
            continue; /* read().splitlines()[1:] */
        }
        n = split_fields(line, fields, lens, 4u);
        if (n > 2u && field_is(fields[1], lens[1], "00000000")) {
            found = true;
            break;
        }
    }
    (void)fclose(f);
    return found;
}

/* The IPv6 half, and the one that matters on the real phone: T-Mobile is
 * IPv6-only. A default route is destination ::/0 -- an all-zero 32-hex-digit
 * prefix with a zero prefix length. There is no header line to skip. */
static bool has_default_route_v6(const char *path)
{
    char line[512];
    FILE *f;
    bool found = false;

    f = fopen(path, "r");
    if (f == NULL)
        return false;
    while (fgets(line, (int)sizeof line, f) != NULL) {
        const char *fields[3];
        size_t lens[3];
        size_t n = split_fields(line, fields, lens, 3u);

        if (n > 1u && field_is(fields[0], lens[0], "00000000000000000000000000000000") &&
            field_is(fields[1], lens[1], "00")) {
            found = true;
            break;
        }
    }
    (void)fclose(f);
    return found;
}

bool nd_update_proc_has_default_route(const char *v4_path, const char *v6_path)
{
    if (v4_path != NULL && has_default_route_v4(v4_path))
        return true;
    if (v6_path != NULL && has_default_route_v6(v6_path))
        return true;
    return false;
}

bool nd_update_has_network(void)
{
    /* "uistub drives this code on a build host whose /proc is not the
     * phone's; PathRemap cannot cover /proc, so the stub says so instead."
     * The two paths below are opened with plain fopen for the same reason --
     * they are NOT ND_ROOT-resolved, because /proc is the kernel's. */
    const char *stub = getenv("NEODCT_STUB");

    if (stub != NULL && stub[0] != '\0')
        return false;
    return nd_update_proc_has_default_route("/proc/net/route", "/proc/net/ipv6_route");
}

/* ------------------------------------------------------------------ *
 * _backup_user_data
 * ------------------------------------------------------------------ */

/* shutil.copy2: contents, then mode, then atime/mtime. On FAT32 the mode is
 * meaningless but the timestamps are not, and they are what makes one backup
 * folder distinguishable from the next. */
static bool copy2(const char *src_virtual, const char *dst_virtual)
{
    /* 16 KB: four times a typical filesystem block and small enough to sit on
     * an app's stack without thought. The databases are tens of KB. */
    char buf[16384];
    char src[ND_PATH_MAX];
    char dst[ND_PATH_MAX];
    struct stat st;
    struct timespec times[2];
    int in = -1;
    int outfd = -1;
    bool ok = false;

    if (nd_path_resolve(src, sizeof src, src_virtual) != ND_OK ||
        nd_path_resolve(dst, sizeof dst, dst_virtual) != ND_OK)
        return false;

    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        goto done;
    if (fstat(in, &st) != 0)
        goto done;
    outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (outfd < 0)
        goto done;

    for (;;) {
        ssize_t got = read(in, buf, sizeof buf);
        ssize_t put = 0;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        if (got == 0)
            break;
        while (put < got) {
            ssize_t n = write(outfd, buf + put, (size_t)(got - put));

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                goto done;
            }
            put += n;
        }
    }

    /* copy2 copies the permission bits; a failure on FAT32, which has none,
     * is not a reason to call the copy bad. */
    (void)fchmod(outfd, (mode_t)(st.st_mode & 07777u));
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    (void)futimens(outfd, times);
    ok = true;

done:
    if (in >= 0)
        (void)close(in);
    if (outfd >= 0) {
        if (close(outfd) != 0)
            ok = false;
    }
    return ok;
}

static int name_cmp(const void *a, const void *b)
{
    /* sorted(os.listdir(...)): Python compares str by code point, and these
     * are ASCII filenames, so strcmp is it. */
    return strcmp((const char *)a, (const char *)b);
}

bool nd_update_backup_user_data(nd_progress *progress)
{
    char target[ND_PATH_MAX];
    char db_dir[ND_PATH_MAX];
    char destination[ND_PATH_MAX];
    char stamp[32];
    char names[ND_UPDATE_MAX_DBS][64];
    size_t n = 0u;
    size_t i;
    DIR *d;
    struct dirent *ent;
    time_t now;
    struct tm local;

    /* Storage.folder("backup_db") is NULL unless the card state is "ready". */
    if (!nd_storage_folder("backup_db", target, sizeof target))
        return false;

    if (nd_path_resolve(db_dir, sizeof db_dir, ND_UPDATE_USER_DB_DIR) != ND_OK)
        return false;
    d = opendir(db_dir);
    if (d == NULL)
        return false; /* `except OSError: return False` */
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);

        if (len < 3u || strcmp(ent->d_name + (len - 3u), ".db") != 0)
            continue;
        if (n >= ND_UPDATE_MAX_DBS)
            break;
        if (nd_strlcpy(names[n], ent->d_name, sizeof names[0]) >= sizeof names[0])
            continue;
        n++;
    }
    (void)closedir(d);
    if (n == 0u)
        return false;
    qsort(names, n, sizeof names[0], name_cmp);

    /* time.strftime("%Y%m%d-%H%M%S") -- LOCAL time here, unlike
     * _format_date's gmtime. The Python is inconsistent; this is the folder
     * a person goes looking for on the card, so local is the right one and
     * it is also what the Python does. */
    now = time(NULL);
    if (localtime_r(&now, &local) == NULL)
        return false;
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local) == 0u)
        return false;
    if (nd_snprintf(destination, sizeof destination, "%s/%s", target, stamp) != ND_OK)
        return false;

    if (nd_mkdir_p(destination, 0755u) != ND_OK)
        return false;

    for (i = 0u; i < n; i++) {
        char src[ND_PATH_MAX];
        char dst[ND_PATH_MAX];

        if (progress != NULL)
            (void)nd_progress_draw(progress, (int64_t)i, (int64_t)n);
        if (nd_snprintf(src, sizeof src, "%s/%s", ND_UPDATE_USER_DB_DIR, names[i]) != ND_OK ||
            nd_snprintf(dst, sizeof dst, "%s/%s", destination, names[i]) != ND_OK)
            return false;
        if (!copy2(src, dst))
            return false; /* `except OSError: return False` */
    }
    if (progress != NULL)
        (void)nd_progress_draw(progress, (int64_t)n, (int64_t)n);
    run_sync();
    return true;
}

/* ------------------------------------------------------------------ *
 * _stage
 * ------------------------------------------------------------------ */

/* The Python's _stage(ui, pkg, progress) takes the progress screen and never
 * uses it -- there is no copying step any more, so all that happens here is a
 * record being written. The unused parameter is dropped rather than carried,
 * because -Wunused-parameter would have to be silenced to keep it and a
 * silenced warning says less than this comment. */
bool nd_update_stage(nd_ui *ui, const nd_upd_manifest *m, const char *package_path,
                     int64_t image_size)
{
    char message[ND_UPDATE_MSG_MAX];

    if (ui == NULL || m == NULL || package_path == NULL)
        return false;

    if (nd_mkdir_p(ND_UPDATE_STATE_DIR, 0755u) != ND_OK) {
        (void)snprintf(message, sizeof message, "%s%s", nd_update_msg_cannot_write_prefix,
                       strerror(errno));
        nd_update_refuse(ui, message);
        return false;
    }

    if (nd_upd_stage_package(m, package_path, image_size) != ND_OK) {
        (void)snprintf(message, sizeof message, "%s%s", nd_update_msg_cannot_stage_prefix,
                       strerror(errno));
        nd_update_refuse(ui, message);
        return false;
    }
    run_sync();
    return true;
}

/* ------------------------------------------------------------------ *
 * The pages
 * ------------------------------------------------------------------ */

void nd_update_report_last_result(nd_ui *ui)
{
    nd_upd_record result;
    const char *version;
    char subtitle[ND_UPDATE_VERSION_MAX + 16];
    char message[ND_UPDATE_MSG_MAX];

    if (ui == NULL)
        return;
    if (!nd_upd_read_result(&result))
        return;

    version = nd_upd_record_get(&result, "version", "");
    if (strcmp(nd_upd_record_get(&result, "result", ""), "ok") == 0) {
        (void)snprintf(subtitle, sizeof subtitle, "NeoDCT %s", version);
        (void)nd_update_page(ui, "Updated", subtitle,
                             "Everything on the phone came across: your contacts, "
                             "messages and settings live on their own partition and "
                             "are untouched by an update.",
                             ND_UPDATE_APP_ICON, NULL, "OK", true);
    } else {
        /* `result.get("reason", "unknown reason")`: the fallback is for a
         * MISSING key, so a record that carries an empty reason prints an
         * empty reason. */
        (void)snprintf(message, sizeof message, "Update to %s failed.\n%s", version,
                       nd_upd_record_get(&result, "reason", "unknown reason"));
        nd_update_refuse(ui, message);
    }
    /* Shown once and then forgotten, whichever way it went. */
    nd_upd_clear_result();
}

/* _restart_page. Ends by rebooting, so no test calls it. */
void nd_update_restart_page(nd_ui *ui, const nd_upd_manifest *m, bool backed_up)
{
    char subtitle[ND_UPDATE_VERSION_MAX + 16];
    char body[ND_UPDATE_MSG_MAX];

    if (ui == NULL || m == NULL)
        return;

    (void)snprintf(subtitle, sizeof subtitle, "NeoDCT %s", m->version);
    (void)snprintf(body, sizeof body,
                   "The phone will restart to finish installing NeoDCT %s. It takes "
                   "about a minute and the screen stays dark for part of it.",
                   m->version);
    if (!backed_up)
        (void)nd_strlcat(body,
                         "\n\nYour contacts and messages were not backed up to the "
                         "card. They stay on the phone either way: user data is on "
                         "its own partition and an update does not touch it.",
                         sizeof body);

    /* cancel_keys=(): there is no way back from here. */
    (void)nd_update_page(ui, "Ready", subtitle, body, ND_UPDATE_APP_ICON, NULL, "Restart", false);
    nd_update_reboot(ui);
}

/* _thumbnail(pkg): the package's own picture, falling back to this app's
 * icon. "A picture that does not match the manifest is a broken attachment,
 * not a reason to refuse an update whose image hashes fine -- so it is
 * dropped and the stock icon stands in."
 *
 * See note 4 in the header comment: the Python returns a decoded Image and
 * the C can only return a path. */
static void thumbnail_path(nd_upd_package *pkg, char *out, size_t out_sz)
{
    if (nd_upd_package_thumbnail_path(pkg, out, out_sz) == ND_UPDSVC_OK && out[0] != '\0')
        return;
    (void)nd_strlcpy(out, ND_UPDATE_APP_ICON, out_sz);
}

/* _update_page(ui, pkg): the "an update is available" page. ENTER installs. */
static int32_t update_page(nd_ui *ui, nd_upd_package *pkg)
{
    const nd_upd_manifest *m = nd_upd_package_manifest(pkg);
    char subtitle[96];
    char size_text[32];
    char date_text[32];
    char image[ND_PATH_MAX];

    if (m == NULL)
        return ND_KEY_NONE;

    nd_update_format_size(nd_upd_package_image_size(pkg), size_text, sizeof size_text);
    nd_update_format_date(m->buildtime, date_text, sizeof date_text);
    (void)snprintf(subtitle, sizeof subtitle, "%s\n%s", size_text, date_text);
    thumbnail_path(pkg, image, sizeof image);

    /* The header says what this screen is and the hero column is narrow, so
     * the title is the version alone -- "NeoDCT 0.3.2a" only fits beside a
     * picture at a size that stops looking like a heading. */
    return nd_update_page(ui, m->version, subtitle,
                          (m->changelog[0] != '\0') ? m->changelog : nd_update_msg_no_release_notes,
                          image, nd_upd_package_signed(pkg) ? "Verified" : "Not signed", "Install",
                          true);
}

/* ------------------------------------------------------------------ *
 * _install
 * ------------------------------------------------------------------ */

void nd_update_install(nd_ui *ui, const char *path)
{
    nd_upd_package *pkg = NULL;
    const nd_upd_manifest *m;
    char why[ND_UPDATE_WHY_MAX];
    char message[ND_UPDATE_MSG_MAX];
    char platform[ND_UPDATE_PLATFORM_MAX];
    struct utsname host;
    const char *kernel = "";
    nd_progress progress;
    nd_updsvc_err rc;
    bool backed_up;

    if (ui == NULL || path == NULL)
        return;

    /* 1. open_package. */
    rc = nd_upd_package_open(path, &pkg, why, sizeof why);
    if (rc == ND_UPDSVC_UNAVAILABLE) {
        /* NOT A BRANCH THE PYTHON HAS. It is checked BEFORE the InvalidUpdate
         * branch on purpose: telling the owner of a perfectly good download
         * that it is corrupt would send them off to re-fetch 51 MB over a
         * bearer that took an hour the first time. service.c explains why the
         * reader is missing rather than approximated. */
        nd_log_err(ND_LOG_UPDATE, "cannot open %s: %s", path, why);
        nd_update_refuse(ui, nd_update_msg_no_reader);
        return;
    }
    if (rc != ND_UPDSVC_OK) {
        /* No override: a package with no manifest or no image has nothing to
         * install, so offering to continue would be a lie. */
        nd_update_refuse(ui, nd_update_msg_invalid);
        return;
    }

    /* From here on every exit goes through `done:`, which is the C spelling
     * of the Python's `with pkg:` -- including the one through
     * _restart_page(), which reboots and comes back thirty seconds later
     * only if the reboot did not happen. The manifest below is BORROWED FROM
     * THE PACKAGE, so the close must not move above any use of it. */
    m = nd_upd_package_manifest(pkg);
    if (m == NULL) {
        nd_update_refuse(ui, nd_update_msg_invalid);
        goto done;
    }

    /* 2. check_compatible. NOTE THE ORDERING: compatibility BEFORE signature.
     * Three of the Python's tests depend on it. */
    (void)nd_settings_get_copy(ND_SET_OS_PLATFORM, "unknown", platform, sizeof platform);
    if (uname(&host) == 0)
        kernel = host.release;
    if (nd_upd_manifest_check_compatible(m, platform, kernel, why, sizeof why) != ND_UPDSVC_OK) {
        /* Never overridable: this is the brick case. */
        (void)snprintf(message, sizeof message, "%s%s", nd_update_msg_wrong_prefix, why);
        nd_update_refuse(ui, message);
        goto done;
    }

    /* 3. verify_signature. */
    if (nd_upd_package_verify_signature(pkg, ND_UPDATE_RELEASE_KEY, why, sizeof why) !=
        ND_UPDSVC_OK) {
        /* Same warning either way -- what differs is whether there is a way
         * past it. Outside engineering mode there is not: an image nobody
         * signed is how you end up stuck on a phone that will not boot, and a
         * dead phone cannot be talked out of it afterwards.
         *
         * pkg->signed stays false down the override path, which is what makes
         * the update page's badge read "Not signed". */
        nd_update_refuse(ui, nd_update_msg_bad_signature);
        if (!nd_update_engineering_mode(ui))
            goto done;
        if (!nd_update_confirm(ui, nd_update_msg_install_anyway, "OK"))
            goto done;
    }

    /* 4. the update page. ENTER means install it. */
    if (update_page(ui, pkg) != ND_KEY_ENTER)
        goto done;

    /* 5. One bar for the whole job: backing up and copying are steps of the
     * same wait, not two screens. */
    nd_progress_init(&progress, ui, "Backing up data", ND_UPDATE_HEADER, ND_UPDATE_COPY_HINT, NULL,
                     NULL);
    backed_up = nd_update_backup_user_data(&progress);
    /* No copying step any more -- the image is installed from the card at
     * boot, so all that happens here is a record being written. */
    nd_progress_set_step(&progress, "Preparing update");

    /* 6. */
    if (nd_update_stage(ui, m, nd_upd_package_path(pkg), nd_upd_package_image_size(pkg)))
        nd_update_restart_page(ui, m, backed_up);

done:
    nd_upd_package_close(pkg);
}

/* ------------------------------------------------------------------ *
 * _choose_package
 * ------------------------------------------------------------------ */

int32_t nd_update_choose_package(nd_ui *ui, char (*paths)[ND_STORAGE_PATH_MAX], size_t n)
{
    const char *names[ND_UPDATE_MAX_PACKAGES];
    nd_vlist list;
    nd_softkey bar;
    size_t i;
    int32_t selection;

    if (ui == NULL || paths == NULL || n == 0u)
        return ND_WIDGET_BACK;
    /* One package is used without asking. */
    if (n == 1u)
        return 0;
    if (n > ND_UPDATE_MAX_PACKAGES)
        n = ND_UPDATE_MAX_PACKAGES;

    for (i = 0u; i < n; i++) {
        const char *slash = strrchr(paths[i], '/');

        names[i] = (slash != NULL) ? slash + 1 : paths[i];
    }

    nd_vlist_init(&list, ui, "Updates", names, n, ND_UPDATE_APP_ID);
    /* SoftKeyBar(ui).update("Select", present=False): the list's own draw
     * clears rows 0..145 only, so the "Select" survives into its frame and is
     * presented with it. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Select", false);
    selection = nd_vlist_show(&list);
    return selection; /* -1 is ND_WIDGET_BACK, which is `return None` */
}

/* ------------------------------------------------------------------ *
 * _check_online
 * ------------------------------------------------------------------ */

bool nd_update_check_online(nd_ui *ui, char *out, size_t out_sz)
{
    char platform[ND_UPDATE_PLATFORM_MAX];
    char installed[ND_UPDATE_VERSION_MAX];
    char why[ND_UPDATE_WHY_MAX];
    char body[ND_UPDATE_MSG_MAX];
    char subtitle[ND_UPDATE_VERSION_MAX + 16];
    char size_text[32];
    char folder[ND_PATH_MAX];
    char asset[128];
    char step[ND_UPDATE_VERSION_MAX + 16];
    nd_upd_release found;
    nd_progress dialog;
    nd_progress progress;
    nd_updsvc_err rc;

    if (ui == NULL || out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';

    (void)nd_settings_get_copy(ND_SET_OS_PLATFORM, "unknown", platform, sizeof platform);
    nd_update_installed_version(installed, sizeof installed);

    nd_progress_init(&dialog, ui, "Checking for updates", ND_UPDATE_HEADER, NULL, NULL, NULL);
    (void)nd_progress_draw(&dialog, 0, 1);

    rc = nd_upd_remote_latest(platform, &found, why, sizeof why);
    if (rc == ND_UPDSVC_INVALID) {
        /* remote.NoRelease */
        (void)snprintf(body, sizeof body,
                       "There is no update for %s in the latest release.\n\n"
                       "That is normal while a release is still being "
                       "uploaded. Try again shortly.",
                       platform);
        (void)nd_update_page(ui, "Nothing published", "for this phone", body, ND_UPDATE_APP_ICON,
                             NULL, "Back", true);
        return false;
    }
    if (rc != ND_UPDSVC_OK) {
        /* remote.NetworkError -- and, in this build, also the "there is no
         * downloader at all" case, whose reason string service.c supplies.
         * The Python prints str(exc) in exactly this position, so the shape
         * of the screen is unchanged and the sentence in it is true. */
        (void)snprintf(body, sizeof body,
                       "%s\n\nMobile data has to be working before the phone "
                       "can look for updates. Updates can still be installed "
                       "from the card.",
                       why);
        (void)nd_update_page(ui, "No connection", "Could not reach GitHub", body,
                             ND_UPDATE_APP_ICON, NULL, "Back", true);
        return false;
    }

    if (!nd_upd_remote_is_newer(found.version, installed)) {
        (void)snprintf(subtitle, sizeof subtitle, "NeoDCT %s",
                       (installed[0] != '\0') ? installed : "?");
        (void)snprintf(body, sizeof body,
                       "The newest release is %s, which is what this phone is "
                       "already running.",
                       found.version);
        (void)nd_update_page(ui, "Up to date", subtitle, body, ND_UPDATE_APP_ICON, NULL, "Back",
                             true);
        return false;
    }

    nd_update_format_size(found.size, size_text, sizeof size_text);
    (void)snprintf(body, sizeof body, "Download NeoDCT %s?\n%s", found.version, size_text);
    if (!nd_update_confirm(ui, body, "Download"))
        return false;

    if (!nd_storage_folder("update", folder, sizeof folder)) {
        nd_update_refuse(ui, nd_update_msg_no_update_folder);
        return false;
    }
    if (nd_upd_remote_asset_name(platform, asset, sizeof asset) != ND_UPDSVC_OK ||
        nd_snprintf(out, out_sz, "%s/%s", folder, asset) != ND_OK) {
        nd_update_refuse(ui, nd_update_msg_no_update_folder);
        return false;
    }

    (void)snprintf(step, sizeof step, "Downloading %s", found.version);
    nd_progress_init(&progress, ui, step, ND_UPDATE_HEADER, NULL, NULL, NULL);
    if (nd_upd_remote_download(&found, out, &progress, why, sizeof why) != ND_UPDSVC_OK) {
        /* Expected on this phone rather than exceptional: the carrier drops
         * and a 60MB download is a long time to hold a weak bearer. Say that
         * the progress is kept -- otherwise trying again looks like starting
         * again, and nobody presses a button for that twice. */
        (void)snprintf(body, sizeof body,
                       "Download failed.\n%s\n\nWhat has downloaded so far "
                       "is kept on the card. Choosing it again carries on "
                       "from there.",
                       why);
        nd_update_refuse(ui, body);
        out[0] = '\0';
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * run
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    /* 32 * 256 = 8,192 bytes of stack. nd_storage_find_updates() caps its own
     * directory listing at 4,096 entries; this is how many the menu offers. */
    char paths[ND_UPDATE_MAX_PACKAGES][ND_STORAGE_PATH_MAX];
    char version[ND_UPDATE_VERSION_MAX];
    char subtitle[ND_UPDATE_VERSION_MAX + 16];
    char downloaded[ND_PATH_MAX];
    nd_card card;
    size_t n;
    int32_t choice;

    if (ui == NULL)
        return 1;

    nd_update_report_last_result(ui);

    nd_storage_card(&card);
    if (card.state == ND_CARD_ABSENT) {
        (void)nd_update_page(ui, "No SD card", "Updates come from a card", nd_update_no_card_help,
                             ND_UPDATE_APP_ICON, NULL, "Back", true);
        return 0;
    }
    if (card.state != ND_CARD_READY) {
        (void)nd_update_page(ui, "Not ready", "The card is not set up", nd_update_not_ready_help,
                             ND_UPDATE_APP_ICON, NULL, "Back", true);
        return 0;
    }

    n = nd_storage_find_updates(paths, ND_UPDATE_MAX_PACKAGES);
    if (n == 0u) {
        /* Nothing on the card is the normal case now that the phone can fetch
         * its own updates. Only offer that when there is actually a route
         * out: a phone with no carrier should get the one "up to date"
         * screen, not a dialog it cannot usefully answer. */
        if (nd_update_has_network() &&
            nd_update_confirm(ui, "No update on the card.\nLook online?", "Check")) {
            if (nd_update_check_online(ui, downloaded, sizeof downloaded))
                nd_update_install(ui, downloaded);
            return 0;
        }
        nd_update_installed_version(version, sizeof version);
        if (version[0] != '\0')
            (void)snprintf(subtitle, sizeof subtitle, "NeoDCT %s", version);
        else
            (void)nd_strlcpy(subtitle, "Nothing to install", sizeof subtitle);
        (void)nd_update_page(ui, "Up to date", subtitle, nd_update_no_package_help,
                             ND_UPDATE_APP_ICON, NULL, "Back", true);
        return 0;
    }

    choice = nd_update_choose_package(ui, paths, n);
    if (choice >= 0 && (size_t)choice < n)
        nd_update_install(ui, paths[choice]);
    return 0;
}

/* MANDATORY even though there is nothing to release: nd_app.h requires every
 * app to export one, so that a missing symbol always means the author forgot
 * and never means there was nothing to do.
 *
 * There genuinely is nothing here. The one long-lived resource this app can
 * hold is an open .ndsw, and that lives on _install()'s stack and is closed
 * on every path out of it -- SIGTERM during an install leaves a file
 * descriptor for the kernel to reclaim and a pending.prop that is either
 * fully written or not written at all, which is the property the atomic
 * rename in staging.c exists to give. No sound card, no child process: the
 * only children are `sync` and `reboot`, both of which are meant to outlive
 * this process. */
void app_shutdown(void) {}
