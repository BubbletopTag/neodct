/* nd_shoot.c -- the C half of the golden-frame oracle: render the reference
 * screens headlessly and write them, plus a manifest goldenframe.py accepts.
 *
 *     ./build/default/bin/nd-shoot --out DIR
 *     python3 neodct/tools/goldenframe.py --compare neodct/tests/golden DIR
 *
 * This is neodct/tools/shoot_docs.py driven through goldenframe.py's
 * deterministic wrapper, with three substitutions C has to make because it
 * cannot monkey-patch:
 *
 *   uistub.CapturingFramebuffer  ->  nd_capture (lib/nd_capture.c)
 *   uistub.PathRemap             ->  nd_path_set_root() over a staged root
 *   goldenframe._Frozen          ->  nd_vclock_enable() (lib/nd_vclock.c)
 *
 * ============ IT RENDERS 25 OF THE 49, ON PURPOSE ============
 *
 * The 49 recipes are spec-build-test.md section 3.6. Twenty-four of them
 * launch an app, and no app exists in this tree yet -- apps/ is empty, the
 * Dialer and the crash handler are not ported, and the two games are not
 * either. A capture tool that drew *something* for those names would be
 * worse than useless: goldenframe.py would report 24 pixel diffs that say
 * nothing about the port, and a passing frame could not be told from a
 * coincidence.
 *
 * So the twenty-four are SKIPPED, by name and with a reason, listed in
 * <out>/nd-shoot-skipped.json and printed on stdout. goldenframe.py's
 * compare() then reports each of them as "missing -- not rendered by
 * candidate", which is the truth. The frames that ARE rendered are the whole
 * home screen set, the whole app-selector set and the whole widget gallery.
 *
 * ============ WHY THE STAGED ROOT IS A SYMLINK FARM ============
 *
 * uistub.py copies the entire 16 MB overlay per StubUI. Here /NeoDCT/System
 * is a symlink onto the repo's overlay and only /NeoDCT/User is real, so
 * nothing under neodct/overlay/ can be written to and a run costs
 * milliseconds. This is test_ui.c's trick; it is here for the same reason.
 *
 * ============ THE OUTPUT DIRECTORY IS DELIBERATELY NOT ROOTED ============
 *
 * Every path this library opens goes through nd_path_resolve(), and
 * nd_capture_open()/nd_capture_save() are no exception (OPEN-QUESTIONS.md
 * F-2). With a staged root in force, --out DIR would land inside the staging
 * directory and be deleted on exit. So the root is cleared for the duration
 * of every capture call and restored afterwards -- capture_* is the one API
 * whose paths belong to the developer's filesystem, not to the phone's.
 *
 * ============ FRAME NUMBER IS PART OF A FRAME'S IDENTITY ============
 *
 * The virtual clock advances one 0.1 s tick per COMMITTED frame, and
 * shoot_docs.py opens a fresh `with StubUI(...)` block -- hence a fresh clock
 * -- for each group. Every group below therefore calls nd_vclock_enable(),
 * which restarts virtual time at frame 0. home-sms-banner is the clearest
 * case: it is the third frame of its block, and the envelope's
 * int(time.time() * 2) % 2 blink phase depends on exactly that.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_capture.h"
#include "nd_crash.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* ------------------------------------------------------------------ *
 * What is rendered and what is not
 * ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *reason;
} shoot_skip;

/* The twenty-four names spec-build-test.md section 3.6 defines that this tree
 * cannot honestly draw yet. Kept in the file so the list is reviewable
 * against the spec table, and emitted verbatim into the output directory.
 *
 * Three of these are byte-identical to widget frames that ARE rendered
 * (app-clock == widget-messagedialog, app-messages == widget-pagedlist,
 * app-phonebook == widget-verticallist). Copying the pixels across would make
 * the count look better and would be a lie: nothing launched an app. */
static const shoot_skip SKIPPED[] = {
    /* Group 3 -- shoot_stock_apps. SESSION-SCOPE.md keeps all 25 apps with
     * their real manifest and icon, but their app.so is not written yet; the
     * whole of neodct/src/apps/ is empty. */
    /* app-clock is NOT here: the Clock app IS the stub dialog (section 3.6
     * records app-clock as byte-identical to widget-messagedialog), so the one
     * shipped app.so renders it for real. The other twelve draw their own
     * screens and the stub cannot stand in for any of them. */
    {"app-phonebook", "app not implemented: neodct/src/apps/ is empty"},
    {"app-messages", "app not implemented: neodct/src/apps/ is empty"},
    {"app-messages-inbox", "app not implemented: neodct/src/apps/ is empty"},
    {"app-calllog", "app not implemented: neodct/src/apps/ is empty"},
    {"app-settings", "app not implemented: neodct/src/apps/ is empty"},
    {"app-settings-wallpaper", "app not implemented: neodct/src/apps/ is empty"},
    {"app-games", "app not implemented: neodct/src/apps/ is empty"},
    {"app-calculator", "app not implemented: neodct/src/apps/ is empty"},
    {"app-calculator-options", "app not implemented: neodct/src/apps/ is empty"},
    {"app-tones", "app not implemented: neodct/src/apps/ is empty"},
    {"app-musicplayer", "app not implemented: neodct/src/apps/ is empty"},
    {"app-koki", "Koki is out of scope this session (SESSION-SCOPE.md)"},

    /* Group 4 -- shoot_games. Both are launched through the Games app. */
    {"game-snake", "app not implemented: Games has no app.so yet"},
    {"game-memory", "app not implemented: Games has no app.so yet"},

    /* Group 5 -- shoot_telephony. home-sms-banner IS rendered; the other four
     * need the Dialer screens, the PhoneBook contact picker and the crash
     * handler, none of which exist in neodct/src yet. */
    {"call-active", "System/ui/Dialer/call_screen not ported yet"},
    {"call-incoming", "System/ui/Dialer/incoming_screen not ported yet"},
    {"contacts-picker", "PhoneBook shared/list_ui not ported yet"},

    /* Group 6 -- shoot_engineering_apps. */
    {"eng-modem", "app not implemented: neodct/src/apps/ is empty"},
    {"eng-fuelgauge", "app not implemented: neodct/src/apps/ is empty"},
    {"eng-lcdtest", "app not implemented: neodct/src/apps/ is empty"},
    {"eng-cubebench", "app not implemented: neodct/src/apps/ is empty"},
    {"eng-tests", "app not implemented: neodct/src/apps/ is empty"},
};

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

static char g_out[ND_PATH_MAX];     /* --out, a real filesystem path      */
static char g_overlay[ND_PATH_MAX]; /* <repo>/neodct/overlay              */
static char g_stage[ND_PATH_MAX];   /* the ND_ROOT for this run           */
static bool g_stage_is_temp;
static bool g_keep_stage;
static size_t g_saved;
static size_t g_failed;

/* nd_capture's paths belong to the developer's filesystem; everything else
 * belongs to the phone's. See the header comment. */
static void root_off(void)
{
    (void)nd_path_set_root(NULL);
}

static void root_on(void)
{
    (void)nd_path_set_root(g_stage);
}

static void save_frame(nd_capture *cap, const char *name, const nd_image *img)
{
    nd_err rc;

    if (img == NULL) {
        nd_log_err(ND_LOG_FB, "shoot: no frame for '%s'", name);
        g_failed++;
        return;
    }
    root_off();
    rc = nd_capture_save(cap, name, img);
    root_on();
    if (rc != ND_OK) {
        nd_log_err(ND_LOG_FB, "shoot: cannot save '%s'", name);
        g_failed++;
        return;
    }
    g_saved++;
    printf("  %-30s %dx%d\n", name, img->w, img->h);
}

/* frames[-1]: the most recently committed frame. */
static void save_recent(nd_capture *cap, const char *name)
{
    save_frame(cap, name, nd_capture_recent(cap, 0u));
}

/* uistub.CapturingFramebuffer.device_frame(): the band centred in the panel.
 * Never bottom-aligned -- spec-build-test.md section 3.7 is explicit that the
 * two 240x240 names are documentation aids reproducing the STUB, and that
 * "fixing" them to the hardware's y=65 would break the oracle and change
 * nothing on the phone. */
static void save_device_frame(nd_capture *cap, const char *name)
{
    nd_image *panel = nd_capture_device_frame(cap, 0u, ND_CAPTURE_PANEL_W, ND_CAPTURE_PANEL_H);

    if (panel == NULL) {
        nd_log_err(ND_LOG_FB, "shoot: device_frame for '%s'", name);
        g_failed++;
        return;
    }
    save_frame(cap, name, panel);
    nd_image_free(panel);
}

/* ------------------------------------------------------------------ *
 * The staged root -- uistub.StubUI._prepare_user_dir() and PathRemap
 * ------------------------------------------------------------------ */

static bool dir_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The directory the running binary sits in. Used only to guess where the
 * overlay is, so a failure here is not fatal -- the flag and the two
 * environment variables still work. */
static bool self_exe_dir(char *out, size_t out_sz)
{
    char buf[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", buf, sizeof buf - 1u);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    slash = strrchr(buf, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_strlcpy(out, buf, out_sz) < out_sz;
}

/* The overlay is where the fonts, icons, wallpapers and ui_home.json live.
 * Tried in the order a caller would expect to win: an explicit flag, the
 * environment, then two guesses relative to the binary, then the repo root as
 * a working directory. */
static bool find_overlay(const char *flag)
{
    static const char *const RELATIVE[] = {
        "%s/../../../../overlay", /* build/<variant>/bin -> neodct/overlay  */
        "%s/../../../overlay",    /* an installed bin/ beside the overlay   */
    };
    char exe[ND_PATH_MAX];
    const char *env;
    size_t i;

    if (flag != NULL && flag[0] != '\0') {
        if (nd_snprintf(g_overlay, sizeof g_overlay, "%s", flag) != ND_OK)
            return false;
        return true; /* an explicit path is honoured even if it looks wrong */
    }

    env = getenv("NEODCT_OVERLAY");
    if (env != NULL && env[0] != '\0') {
        if (nd_snprintf(g_overlay, sizeof g_overlay, "%s", env) != ND_OK)
            return false;
        return true;
    }

    /* NEODCT_GOLDEN is <repo>/neodct/tests/golden, which the Makefile already
     * passes to every test; the overlay is two levels up from it. */
    env = getenv("NEODCT_GOLDEN");
    if (env != NULL && env[0] != '\0' &&
        nd_snprintf(g_overlay, sizeof g_overlay, "%s/../../overlay", env) == ND_OK) {
        char probe[ND_PATH_MAX];

        if (nd_snprintf(probe, sizeof probe, "%s/NeoDCT/System", g_overlay) == ND_OK &&
            dir_exists(probe))
            return true;
    }

    if (self_exe_dir(exe, sizeof exe)) {
        for (i = 0u; i < ND_ARRAY_LEN(RELATIVE); i++) {
            char probe[ND_PATH_MAX];

            if (nd_snprintf(g_overlay, sizeof g_overlay, RELATIVE[i], exe) != ND_OK)
                continue;
            if (nd_snprintf(probe, sizeof probe, "%s/NeoDCT/System", g_overlay) == ND_OK &&
                dir_exists(probe))
                return true;
        }
    }

    if (nd_snprintf(g_overlay, sizeof g_overlay, "neodct/overlay") == ND_OK &&
        dir_exists("neodct/overlay/NeoDCT/System"))
        return true;

    g_overlay[0] = '\0';
    return false;
}

/* ------------------------------------------------------------------ *
 * Giving the staged apps an app.so
 * ------------------------------------------------------------------ *
 *
 * /NeoDCT/System is a SYMLINK onto neodct/overlay, and neodct/overlay is the
 * Python reference -- nothing may be written there, so app.so cannot simply be
 * dropped beside each manifest.json. Instead the one directory that needs to
 * gain a file, System/apps, is rebuilt as a real directory of real app
 * directories whose contents are symlinks back to the overlay, plus a symlink
 * to the app.so this build produced.
 *
 * Every other entry of System stays a plain symlink, so the fonts, wallpapers,
 * icons and ui_home.json are byte-for-byte the same files the 25 already-exact
 * frames were rendered from. The engineering apps are not expanded: nothing
 * launches one, and the menu only needs their manifest and icon.
 */

/* build/<variant>/bin/nd-shoot -> build/<variant>/apps/Stub/app.so. The one
 * stub, the one every app directory gets. */
static bool stub_app_so(char *out, size_t out_sz)
{
    char exe[ND_PATH_MAX];

    if (!self_exe_dir(exe, sizeof exe))
        return false;
    if (nd_snprintf(out, out_sz, "%s/../apps/Stub/app.so", exe) != ND_OK)
        return false;
    return access(out, R_OK) == 0;
}

/* One app directory: a real directory of symlinks, plus app.so. */
static bool stage_one_app(const char *src_dir, const char *dst_dir, const char *stub)
{
    DIR *d = opendir(src_dir);
    struct dirent *e;
    char link[ND_PATH_MAX];
    char target[ND_PATH_MAX];

    if (d == NULL)
        return false;
    if (mkdir(dst_dir, 0755) != 0 && errno != EEXIST) {
        (void)closedir(d);
        return false;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (nd_snprintf(link, sizeof link, "%s/%s", dst_dir, e->d_name) != ND_OK ||
            nd_snprintf(target, sizeof target, "%s/%s", src_dir, e->d_name) != ND_OK)
            continue;
        /* An entry that is already there is fine -- the same overlay file. */
        if (symlink(target, link) != 0 && errno != EEXIST)
            nd_log_err(ND_LOG_OS, "symlink %s: %s", link, strerror(errno));
    }
    (void)closedir(d);

    if (stub == NULL)
        return true;
    if (nd_snprintf(link, sizeof link, "%s/%s", dst_dir, ND_APP_SO_NAME) != ND_OK)
        return false;
    (void)unlink(link);
    return symlink(stub, link) == 0;
}

/* System as a real directory: every entry symlinked, except apps/, which is
 * expanded so each app can gain an app.so. */
static bool stage_system(const char *sys_src, const char *sys_dst)
{
    char stub[ND_PATH_MAX];
    char apps_src[ND_PATH_MAX];
    char apps_dst[ND_PATH_MAX];
    char link[ND_PATH_MAX];
    char target[ND_PATH_MAX];
    DIR *d;
    struct dirent *e;
    bool have_stub = stub_app_so(stub, sizeof stub);

    if (!have_stub)
        nd_log(ND_LOG_OS, "no apps/Stub/app.so in this build; app frames will be skipped");

    if (mkdir(sys_dst, 0755) != 0 && errno != EEXIST)
        return false;

    d = opendir(sys_src);
    if (d == NULL)
        return false;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (strcmp(e->d_name, "apps") == 0)
            continue; /* expanded below */
        if (nd_snprintf(link, sizeof link, "%s/%s", sys_dst, e->d_name) != ND_OK ||
            nd_snprintf(target, sizeof target, "%s/%s", sys_src, e->d_name) != ND_OK)
            continue;
        if (symlink(target, link) != 0 && errno != EEXIST)
            nd_log_err(ND_LOG_OS, "symlink %s: %s", link, strerror(errno));
    }
    (void)closedir(d);

    if (nd_snprintf(apps_src, sizeof apps_src, "%s/apps", sys_src) != ND_OK)
        return false;
    if (nd_snprintf(apps_dst, sizeof apps_dst, "%s/apps", sys_dst) != ND_OK)
        return false;
    if (mkdir(apps_dst, 0755) != 0 && errno != EEXIST)
        return false;

    d = opendir(apps_src);
    if (d == NULL)
        return false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (nd_snprintf(target, sizeof target, "%s/%s", apps_src, e->d_name) != ND_OK ||
            nd_snprintf(link, sizeof link, "%s/%s", apps_dst, e->d_name) != ND_OK)
            continue;
        if (!dir_exists(target))
            continue;
        if (!stage_one_app(target, link, have_stub ? stub : NULL))
            return false;
    }
    (void)closedir(d);
    return true;
}

/* /NeoDCT/System is the read-only overlay, symlinked; /NeoDCT/User is real
 * and writable, because settings.prop and the sqlite databases live there. */
static bool stage_root(void)
{
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    char user[ND_PATH_MAX];
    const char *want = getenv(ND_ENV_ROOT);

    /* A caller who supplied a root owns it: never staged, never removed. But
     * only when it really is a phone root -- `make test` points NEODCT_ROOT at
     * a shared empty scratch directory for every unit test, and rendering the
     * home screen out of that would silently produce a screen with no fonts,
     * no wallpaper and no apps rather than an error. */
    if (want != NULL && want[0] != '\0') {
        char probe[ND_PATH_MAX];

        if (nd_snprintf(g_stage, sizeof g_stage, "%s", want) != ND_OK)
            return false;
        if (nd_snprintf(probe, sizeof probe, "%s/NeoDCT/System", g_stage) == ND_OK &&
            dir_exists(probe)) {
            g_stage_is_temp = false;
            return nd_path_set_root(g_stage) == ND_OK;
        }
        nd_log(ND_LOG_OS, "%s=%s has no NeoDCT/System; staging a root instead", ND_ENV_ROOT,
               g_stage);
    }

    {
        const char *base = getenv("TMPDIR");
        char tmpl[ND_PATH_MAX];

        if (base == NULL || base[0] == '\0')
            base = "/tmp";
        if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndshoot-XXXXXX", base) != ND_OK)
            return false;
        if (mkdtemp(tmpl) == NULL) {
            nd_log_err(ND_LOG_OS, "mkdtemp under %s: %s", base, strerror(errno));
            return false;
        }
        (void)nd_strlcpy(g_stage, tmpl, sizeof g_stage);
        g_stage_is_temp = true;
    }

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        return false;
    if (mkdir(neodct, 0755) != 0 && errno != EEXIST)
        return false;

    if (nd_snprintf(sys_link, sizeof sys_link, "%s/System", neodct) != ND_OK)
        return false;
    if (nd_snprintf(sys_target, sizeof sys_target, "%s/NeoDCT/System", g_overlay) != ND_OK)
        return false;
    if (!stage_system(sys_target, sys_link)) {
        nd_log_err(ND_LOG_OS, "cannot stage %s: %s", sys_link, strerror(errno));
        return false;
    }

    if (nd_snprintf(user, sizeof user, "%s/User", neodct) != ND_OK)
        return false;
    if (mkdir(user, 0755) != 0 && errno != EEXIST)
        return false;

    return nd_path_set_root(g_stage) == ND_OK;
}

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

/* CODING-STANDARDS.md section 1.7 applies to the filesystem too, and
 * uistub._cleanup()'s comment says why: nothing removed the staged overlay
 * for a long time and every run leaked ~16 MB into /tmp. */
static void drop_stage(void)
{
    root_off();
    if (g_stage_is_temp && !g_keep_stage && g_stage[0] != '\0')
        (void)nftw(g_stage, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* uistub.StubUI._prepare_user_dir(): the ack file that skips the first-boot
 * security modal, and settings.prop with the keys sorted. Every StubUI in
 * shoot_docs.py defaults to engineering=True and skip_notice=True. */
static void write_settings(const char *wallpaper_name)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/NeoDCT/User/.ack_security_warning", g_stage) == ND_OK) {
        f = fopen(path, "w");
        if (f != NULL) {
            (void)fputs("0", f);
            (void)fclose(f);
        }
    }
    if (nd_snprintf(path, sizeof path, "%s/NeoDCT/User/settings.prop", g_stage) != ND_OK)
        return;
    f = fopen(path, "w");
    if (f == NULL) {
        nd_log_err(ND_LOG_OS, "cannot write %s: %s", path, strerror(errno));
        return;
    }
    (void)fputs("system.ui.engineering_mode=ON\n", f);
    if (wallpaper_name != NULL) {
        /* Stock wallpapers ship inside the read-only image; only user-added
         * ones live under /NeoDCT/User. */
        (void)fprintf(f, "system.ui.wallpaper=/NeoDCT/System/wallpapers/%s\n", wallpaper_name);
    }
    (void)fclose(f);
}

/* ------------------------------------------------------------------ *
 * uistub.KeyScript -- a canned list of presses, over the real channel
 * ------------------------------------------------------------------ */

/* The one blocking screen in the renderable set is InfoScreen.show(), which
 * shoot_docs.py dismisses with `ui.keys.push(BACK)`. The core's own
 * press/release channel already writes native evdev records, so a script is
 * just a channel with nobody on the other end.
 *
 * The caller keeps its previous ui->input and restores it: nd_ui_teardown()
 * closes whatever ui->input points at, and it must close the one it opened. */
typedef struct {
    nd_input_channel ch;
    nd_input *in;
    nd_input *saved;
    bool active;
} key_script;

static bool key_script_begin(key_script *ks, nd_ui *ui, const int32_t *keys, size_t n_keys)
{
    size_t i;

    memset(ks, 0, sizeof *ks);
    ks->ch.read_fd = -1;
    ks->ch.write_fd = -1;

    if (nd_input_channel_open(&ks->ch) != ND_OK)
        return false;
    /* nd_input_open_fd() takes ownership of the descriptor, so the channel's
     * read end must not be closed again by nd_input_channel_close(). */
    if (nd_input_open_fd(&ks->in, ks->ch.read_fd) != ND_OK) {
        nd_input_channel_close(&ks->ch);
        return false;
    }
    ks->ch.read_fd = -1;

    /* Auto-repeat is new behaviour (OPEN-QUESTIONS.md I-1) and the reference
     * frames were captured without it. A script must never synthesise a key
     * the Python did not deliver. */
    nd_input_set_repeat(ks->in, 0.0, 0.0);

    for (i = 0u; i < n_keys; i++) {
        if (nd_input_channel_send(&ks->ch, keys[i], true) != ND_OK ||
            nd_input_channel_send(&ks->ch, keys[i], false) != ND_OK) {
            nd_input_close(ks->in);
            nd_input_channel_close(&ks->ch);
            return false;
        }
    }

    ks->saved = ui->input;
    ui->input = ks->in;
    ks->active = true;
    return true;
}

static void key_script_end(key_script *ks, nd_ui *ui)
{
    if (!ks->active)
        return;
    ui->input = ks->saved;
    nd_input_close(ks->in);
    nd_input_channel_close(&ks->ch);
    ks->active = false;
}

/* ------------------------------------------------------------------ *
 * Group 1 -- shoot_home
 * ------------------------------------------------------------------ */

/* shoot_docs.py types 0 7 4 1 2 3 4 5 6 7 on the home screen. */
static const int32_t DIGIT_KEY[10] = {ND_KEY_0, ND_KEY_1, ND_KEY_2, ND_KEY_3, ND_KEY_4,
                                      ND_KEY_5, ND_KEY_6, ND_KEY_7, ND_KEY_8, ND_KEY_9};
static const int DIAL_DIGITS[10] = {0, 7, 4, 1, 2, 3, 4, 5, 6, 7};

static void shoot_home(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    size_t i;

    printf("[shoot] home\n");

    /* --- WP + STATUS: wallpaper and a healthy phone --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (home, wallpaper)");
        g_failed++;
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");
    nd_ui_update(&ui);
    save_recent(cap, "home");
    save_device_frame(cap, "home-panel");

    /* Typing a number on the home screen switches to the dialer. */
    for (i = 0u; i < ND_ARRAY_LEN(DIAL_DIGITS); i++)
        nd_ui_handle_input(&ui, DIGIT_KEY[DIAL_DIGITS[i]]);
    nd_ui_update(&ui);
    save_recent(cap, "home-dialing");
    nd_ui_teardown(&ui);

    /* --- PLAIN + STATUS: no wallpaper --- */
    write_settings(NULL);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (home, no wallpaper)");
        g_failed++;
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");
    nd_ui_update(&ui);
    save_recent(cap, "home-nowallpaper");
    nd_ui_teardown(&ui);

    /* --- WP, no STATUS: the honest QEMU/dev look, "?" battery and
     * "No Service". This is the only group that leaves the simulation hook
     * alone, and the fallbacks in nd_ui_sim.h are what it renders. --- */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (home, simulation)");
        g_failed++;
        return;
    }
    nd_ui_update(&ui);
    save_recent(cap, "home-simulation");
    nd_ui_teardown(&ui);

    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * Group 2 -- shoot_app_selector
 * ------------------------------------------------------------------ */

/* shoot_docs.py's list, in its order. A name the scan does not produce is
 * skipped there ("if name not in by_name: continue"), so it is skipped here
 * too -- and said out loud, because in this tree it would mean the registry
 * is wrong rather than that an app was removed. */
static const char *const MENU_WANTED[][2] = {
    {"Phone book", "menu-phone-book"}, {"Messages", "menu-messages"},
    {"Games", "menu-games"},           {"Settings", "menu-settings"},
    {"Calculator", "menu-calculator"}, {"Koki Mobile", "menu-koki-mobile"},
    {"Browser", "menu-browser"},       {"Music", "menu-music"},
};

static size_t app_index_of(const nd_ui *ui, const char *name)
{
    size_t i;

    for (i = 0u; i < ui->n_apps; i++) {
        if (strcmp(ui->apps[i].name, name) == 0)
            return i;
    }
    return (size_t)-1;
}

static void shoot_app_selector(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    nd_appsel selector;
    size_t i;

    printf("[shoot] app selector\n");

    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (app selector)");
        g_failed++;
        return;
    }
    /* AppSelector draws no status bar, so this changes no pixel. It is set
     * because the reference block sets it, and a divergence in what the
     * status readouts report would otherwise be invisible here. */
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    nd_appsel_init(&selector, &ui, "Main Menu", ui.apps, ui.n_apps, ui.wallpaper);

    for (i = 0u; i < ND_ARRAY_LEN(MENU_WANTED); i++) {
        size_t idx = app_index_of(&ui, MENU_WANTED[i][0]);

        if (idx == (size_t)-1) {
            nd_log_err(ND_LOG_UI, "shoot: no app named '%s' in the registry", MENU_WANTED[i][0]);
            g_failed++;
            continue;
        }
        selector.selected_index = idx;
        nd_appsel_draw(&selector);
        save_recent(cap, MENU_WANTED[i][1]);

        /* device_frame() taken immediately after menu-phone-book. */
        if (strcmp(MENU_WANTED[i][0], "Phone book") == 0)
            save_device_frame(cap, "menu-panel");
    }

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * Group 5 -- shoot_telephony, the one frame of it that exists
 * ------------------------------------------------------------------ */

static void shoot_telephony(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;

    printf("[shoot] telephony (1 of 5 -- the Dialer and CrashHandler are not ported)\n");

    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (telephony)");
        g_failed++;
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    /* shoot_telephony draws call-active and call-incoming from the SAME
     * StubUI before this one, so home-sms-banner is the block's THIRD frame.
     * Their pixels are not reproducible yet; their TICKS are, and the
     * envelope's int(time.time() * 2) % 2 blink phase is decided by them.
     * Two throwaway commits stand in. */
    (void)nd_ui_present(&ui);
    (void)nd_ui_present(&ui);
    if (nd_vclock_frame() != 2u) {
        nd_log_err(ND_LOG_UI, "shoot: expected 2 ticks before the banner, got %llu",
                   (unsigned long long)nd_vclock_frame());
        g_failed++;
    }

    /* ui.notify.post_sms(1, tone=False); ui._unread_sms = 1 */
    nd_ui_sim_sms_banner(&ui, 1);
    ui.unread_sms = 1;
    nd_ui_update(&ui);
    save_recent(cap, "home-sms-banner");

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();

    /* The crash screen is a SECOND `with StubUI()` block in shoot_telephony:
     * no wallpaper, no simulate_status, and a fresh clock. shoot_docs.py calls
     * CrashHandler._draw_engineering_crash_screen() directly and then flushes
     * by hand, so the frame is committed twice -- the draw itself presents,
     * and the recipe presents again. Nothing on this screen depends on the
     * clock, so the extra tick moves no pixel; it is reproduced because the
     * recipe is the specification. */
    write_settings(NULL);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (crash screen)");
        g_failed++;
        return;
    }
    nd_crash_draw_engineering(&ui, "RuntimeError: example failure");
    (void)nd_ui_present(&ui);
    save_recent(cap, "crash-screen");

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * Group 3 -- shoot_stock_apps, the one case the stub app can honestly draw
 * ------------------------------------------------------------------ *
 *
 * uistub.run_app() imports the app IN-PROCESS and calls run(ui), because a
 * capture harness has to be in the same address space as the canvas it is
 * capturing. This does the same thing with dlopen: the app.so beside the
 * manifest, RTLD_NOW, app_run(ui). It is NOT the launcher -- nd-core forks and
 * execve's nd-apprun, and a child process cannot draw into this process's
 * canvas. That path is proved separately, out of process and with a real
 * SIGSEGV, in test/unit/test_proc.c.
 *
 * Only app-clock is rendered. spec-build-test.md section 3.6 records
 * app-clock as byte-identical to widget-messagedialog because the Clock app IS
 * a "This application has not been implemented yet." dialog -- so the one
 * shipped app.so draws it for real. The other twelve stock apps draw their own
 * screens; the stub cannot stand in for any of them and they stay skipped.
 */

/* MessageDialog flushes pending input before its first draw, so a key written
 * into the channel up front is eaten before show() ever waits on it. The way
 * through is the one test_widgets_lists.c found: press a key and do not
 * release it, with that key in the repeat set. The flush consumes the press,
 * the held state survives it, and the synthesised repeat arrives after the
 * screen is up -- which is also how the Python's ScriptExhausted got out of an
 * app that would otherwise loop forever. */
static bool hold_key_begin(key_script *ks, nd_ui *ui, int32_t code)
{
    static int32_t held;

    held = code;
    if (!key_script_begin(ks, ui, NULL, 0u))
        return false;
    if (nd_input_set_repeat_codes(ks->in, &held, 1u) != ND_OK)
        return false;
    nd_input_set_repeat(ks->in, 0.20, 0.05);
    return nd_input_channel_send(&ks->ch, code, true) == ND_OK;
}

static size_t app_index_of(const nd_ui *ui, const char *name);

static void run_stock_app(nd_capture *cap, nd_ui *ui, const char *manifest_name,
                          int64_t frame_budget, const char *slug)
{
    char so_path[ND_PATH_MAX];
    key_script ks;
    void *handle;
    int (*run)(nd_ui *);
    size_t idx = app_index_of(ui, manifest_name);

    if (idx == (size_t)-1) {
        nd_log_err(ND_LOG_OS, "shoot: no app named '%s'", manifest_name);
        g_failed++;
        return;
    }
    if (nd_path_join(so_path, sizeof so_path, ui->apps[idx].path, ND_APP_SO_NAME) != ND_OK) {
        g_failed++;
        return;
    }

    handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        nd_log_err(ND_LOG_OS, "shoot: %s: %s", slug, dlerror());
        g_failed++;
        return;
    }
    run = (int (*)(nd_ui *))(uintptr_t)dlsym(handle, ND_APP_SYM_RUN);
    if (run == NULL) {
        nd_log_err(ND_LOG_OS, "shoot: %s exports no %s", so_path, ND_APP_SYM_RUN);
        (void)dlclose(handle);
        g_failed++;
        return;
    }

    if (!hold_key_begin(&ks, ui, ND_KEY_ENTER)) {
        nd_log_err(ND_LOG_OS, "shoot: %s: no key channel", slug);
        (void)dlclose(handle);
        g_failed++;
        return;
    }

    nd_capture_set_budget(cap, frame_budget);
    (void)run(ui);
    nd_capture_clear_budget(cap);

    key_script_end(&ks, ui);
    save_recent(cap, slug);
    /* Not dlclose()d until after the frame is saved: the canvas holds no
     * pointer into the .so, but unloading a library whose code just ran is
     * cheap to get wrong and there is nothing to gain from it here. */
    (void)dlclose(handle);
}

static void shoot_stock_apps(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;

    printf("[shoot] stock apps (1 of 13 -- the other twelve are not ported)\n");

    /* A fresh WP + STATUS UI, as every case in shoot_stock_apps gets. */
    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (stock apps)");
        g_failed++;
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    run_stock_app(cap, &ui, "Clock", 240, "app-clock");

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * Group 7 -- shoot_widgets
 * ------------------------------------------------------------------ */

/* ONE UI for all ten, drawn in this order. The order is load-bearing twice
 * over: the virtual clock advances across them, and each widget draws onto
 * the same canvas -- VerticalList and both text boxes clear only rows
 * 0..content_bottom, so the softkey strip a previous widget left behind is
 * still in the frame. LevelSelector inherits TextScroller's "More"; both text
 * boxes inherit PagedList's "Select". */

static const char *const PHONEBOOK[] = {"Search", "Add entry",  "Edit",
                                        "Erase",  "Send entry", "Options"};
static const char *const MESSAGES[] = {"Inbox", "Outbox", "Write Message"};

#define STUB_MESSAGE "This application has not been implemented yet."

#define SNAKE_HELP                                                                \
    "Feed the snake by steering it to the food. Every bite makes it grow longer." \
    " Use keys 2, 4, 6 and 8 to change direction."

static void shoot_widgets(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    nd_softkey bar;

    printf("[shoot] widgets\n");

    /* PLAIN + STATUS: shoot_widgets() opens StubUI() with no wallpaper. */
    write_settings(NULL);
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (widgets)");
        g_failed++;
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    /* --- VerticalList, twice --- */
    {
        nd_vlist list;

        nd_vlist_init(&list, &ui, "Phonebook", PHONEBOOK, ND_ARRAY_LEN(PHONEBOOK), 1);

        /* The caller paints "Select" WITHOUT presenting, and the list's
         * draw() clears only rows 0..145, so the strip survives into the
         * frame the list itself presents. */
        nd_softkey_init(&bar, &ui, false);
        nd_softkey_update(&bar, "Select", false);
        nd_vlist_draw(&list);
        save_recent(cap, "widget-verticallist");

        list.selected_index = 2u;
        nd_softkey_init(&bar, &ui, false);
        nd_softkey_update(&bar, "Select", false);
        nd_vlist_draw(&list);
        save_recent(cap, "widget-verticallist-scrolled");
    }

    /* --- PagedList --- */
    {
        nd_pagedlist paged;

        nd_pagedlist_init(&paged, &ui, "Messages", MESSAGES, ND_ARRAY_LEN(MESSAGES), "2", true);
        nd_pagedlist_draw(&paged);
        save_recent(cap, "widget-pagedlist");
    }

    /* --- TextInput --- */
    {
        char buf[ND_TEXTINPUT_CAP];
        nd_textinput entry;

        if (nd_textinput_init(&entry, &ui, "Phonebook", "Name:", buf, sizeof buf, "Sam",
                              ND_T9_FILTER_ANY) != ND_OK) {
            nd_log_err(ND_LOG_UI, "shoot: nd_textinput_init failed");
            g_failed++;
        } else {
            /* TextInput.draw(blink_state=True) is the default at the call
             * site, and the cursor changes the measured ink height of the
             * line, so it is a pixel decision rather than a cosmetic one. */
            nd_textinput_draw(&entry, true);
            save_recent(cap, "widget-textinput");
        }
    }

    /* --- TextInputLong --- */
    {
        char buf[ND_TEXTLONG_CAP];
        nd_textlong longtext;

        if (nd_textlong_init(&longtext, &ui, "Write Message", buf, sizeof buf, "",
                             ND_T9_FILTER_ANY) != ND_OK ||
            nd_textlong_set_text(&longtext, "Meet me by the old phone box at six") != ND_OK) {
            nd_log_err(ND_LOG_UI, "shoot: nd_textlong setup failed");
            g_failed++;
        } else {
            nd_textlong_draw(&longtext, true);
            save_recent(cap, "widget-textinputlong");
        }
    }

    /* --- MessageDialog. SESSION-SCOPE.md makes this frame the acceptance
     * test for all 25 stub apps, so it is the one to look at first if the
     * widget group regresses. --- */
    {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, &ui, STUB_MESSAGE);
        nd_msgdialog_render(&dlg);
        save_recent(cap, "widget-messagedialog");
    }

    /* --- TextScroller. draw() itself presents nothing; its last act is
     * SoftKeyBar.update("More"), which does. --- */
    {
        nd_scroller scroller;

        nd_scroller_init(&scroller, &ui, SNAKE_HELP, "More", "Back");
        (void)nd_scroller_draw(&scroller);
        save_recent(cap, "widget-textscroller");
    }

    /* --- LevelSelector(ui, current=3): count 9, title "Level", app_id 6.
     * draw() is VerticalList's, and it does not touch the softkey strip, so
     * the "More" the TextScroller left is still in this frame. --- */
    {
        nd_levelsel level;

        nd_levelsel_init(&level, &ui, 3, 9, "Level", 6);
        nd_vlist_draw(&level.list);
        save_recent(cap, "widget-levelselector");
    }

    /* --- InfoScreen. The only blocking screen in the set: shoot_docs.py
     * pushes BACK and lets show() return through it. --- */
    {
        static const int32_t BACK_ONCE[] = {ND_KEY_CLEAR};
        key_script ks;

        if (!key_script_begin(&ks, &ui, BACK_ONCE, ND_ARRAY_LEN(BACK_ONCE))) {
            nd_log_err(ND_LOG_UI, "shoot: cannot open a key script for InfoScreen");
            g_failed++;
        } else {
            /* InfoScreen(ui, "Top score", 1250) -- str(1250) at the call site. */
            (void)nd_infoscreen_show(&ui, "Top score", "1250", "Back");
            key_script_end(&ks, &ui);
            save_recent(cap, "widget-infoscreen");
        }
    }

    /* --- SoftKeyBar, composed by hand in shoot_docs.py: a black screen, a
     * title, a "3-2" breadcrumb, the divider, then update("Options"). --- */
    {
        nd_header header;

        nd_header_init_int(&header, &ui, 3);
        (void)nd_draw_rect_fill(ui.draw, ND_RECT(0, 0, ui.w, ui.h), ND_BLACK);
        (void)nd_draw_text(ui.draw, 5, 0, "Call log", ui.font_xl, ND_WHITE);
        nd_header_draw(&header, 2);
        (void)nd_draw_line(ui.draw, 0, 30, ui.w, 30, ND_WHITE, 1);

        nd_softkey_init(&bar, &ui, false);
        nd_softkey_update(&bar, "Options", true);
        save_recent(cap, "widget-softkeybar");
    }

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
}

/* ------------------------------------------------------------------ *
 * The skip list
 * ------------------------------------------------------------------ */

/* goldenframe.py's manifest schema is fixed and compare() parses it, so the
 * skips go in a file of their own rather than as an extra key -- an unknown
 * key would be ignored today and could collide tomorrow. compare() reports
 * each skipped name as "missing -- not rendered by candidate", and this file
 * says why. */
static nd_err write_skip_list(void)
{
    char path[ND_PATH_MAX];
    FILE *f;
    size_t i;
    nd_err rc = ND_OK;

    if (nd_snprintf(path, sizeof path, "%s/nd-shoot-skipped.json", g_out) != ND_OK)
        return ND_ERR_TOOLONG;

    root_off();
    f = fopen(path, "wb");
    root_on();
    if (f == NULL) {
        nd_log_err(ND_LOG_FB, "cannot write %s: %s", path, strerror(errno));
        return ND_ERR_IO;
    }

    (void)fprintf(f, "{\n  \"skipped\": [\n");
    for (i = 0u; i < ND_ARRAY_LEN(SKIPPED); i++) {
        (void)fprintf(f, "    {\n      \"name\": \"%s\",\n      \"reason\": \"%s\"\n    }%s\n",
                      SKIPPED[i].name, SKIPPED[i].reason,
                      i + 1u < ND_ARRAY_LEN(SKIPPED) ? "," : "");
    }
    (void)fprintf(f, "  ]\n}");

    if (ferror(f) != 0) {
        nd_log_err(ND_LOG_FB, "short write on %s", path);
        rc = ND_ERR_IO;
    }
    if (fclose(f) != 0 && rc == ND_OK) {
        nd_log_err(ND_LOG_FB, "cannot close %s: %s", path, strerror(errno));
        rc = ND_ERR_IO;
    }
    return rc;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    (void)fprintf(out, "usage: nd-shoot --out DIR [--overlay DIR] [--keep-stage]\n"
                       "\n"
                       "Renders the golden reference screens this tree can draw and writes\n"
                       "them, a manifest.json goldenframe.py accepts, and a list of the\n"
                       "frames that were skipped and why.\n"
                       "\n"
                       "  --out DIR       where the PNGs and manifest.json go (required)\n"
                       "  --overlay DIR   the NeoDCT overlay; default: found near the binary,\n"
                       "                  or $NEODCT_OVERLAY, or $NEODCT_GOLDEN/../../overlay\n"
                       "  --keep-stage    do not delete the staged /NeoDCT root on exit\n"
                       "  --list          print the frame names this build renders and skips\n"
                       "\n"
                       "$NEODCT_ROOT is used as the staged root only when it already contains\n"
                       "NeoDCT/System; otherwise a temporary one is staged.\n");
}

static void print_list(void)
{
    static const char *const RENDERED[] = {
        "home",
        "home-panel",
        "home-dialing",
        "home-nowallpaper",
        "home-simulation",
        "menu-phone-book",
        "menu-panel",
        "menu-messages",
        "menu-games",
        "menu-settings",
        "menu-calculator",
        "menu-koki-mobile",
        "menu-browser",
        "menu-music",
        "home-sms-banner",
        "widget-verticallist",
        "widget-verticallist-scrolled",
        "widget-pagedlist",
        "widget-textinput",
        "widget-textinputlong",
        "widget-messagedialog",
        "widget-textscroller",
        "widget-levelselector",
        "widget-infoscreen",
        "widget-softkeybar",
    };
    size_t i;

    printf("rendered (%zu):\n", ND_ARRAY_LEN(RENDERED));
    for (i = 0u; i < ND_ARRAY_LEN(RENDERED); i++)
        printf("  %s\n", RENDERED[i]);
    printf("skipped (%zu):\n", ND_ARRAY_LEN(SKIPPED));
    for (i = 0u; i < ND_ARRAY_LEN(SKIPPED); i++)
        printf("  %-24s %s\n", SKIPPED[i].name, SKIPPED[i].reason);
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char *overlay = NULL;
    nd_capture *cap = NULL;
    int rc = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--overlay") == 0 && i + 1 < argc) {
            overlay = argv[++i];
        } else if (strcmp(argv[i], "--keep-stage") == 0) {
            g_keep_stage = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            print_list();
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            (void)fprintf(stderr, "nd-shoot: unrecognised argument '%s'\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (out == NULL) {
        (void)fprintf(stderr, "nd-shoot: --out is required\n");
        usage(stderr);
        return 2;
    }
    if (nd_snprintf(g_out, sizeof g_out, "%s", out) != ND_OK) {
        (void)fprintf(stderr, "nd-shoot: --out path is too long\n");
        return 2;
    }

    if (!find_overlay(overlay)) {
        (void)fprintf(stderr,
                      "nd-shoot: cannot find the NeoDCT overlay. Pass --overlay DIR or set\n"
                      "          NEODCT_OVERLAY to <repo>/neodct/overlay.\n");
        return 1;
    }
    if (!stage_root()) {
        (void)fprintf(stderr, "nd-shoot: cannot stage a /NeoDCT root\n");
        return 1;
    }

    printf("[shoot] overlay: %s\n", g_overlay);
    printf("[shoot] root:    %s\n", g_stage);
    printf("[shoot] output:  %s\n", g_out);

    root_off();
    if (nd_capture_open(&cap, g_out, 0u) != ND_OK) {
        root_on();
        (void)fprintf(stderr, "nd-shoot: cannot open %s for capture\n", g_out);
        goto done;
    }
    root_on();

    shoot_home(cap);
    shoot_app_selector(cap);
    shoot_stock_apps(cap);
    shoot_telephony(cap);
    shoot_widgets(cap);

    root_off();
    if (nd_capture_write_manifest(cap) != ND_OK) {
        root_on();
        (void)fprintf(stderr, "nd-shoot: cannot write manifest.json\n");
        goto done;
    }
    root_on();

    if (write_skip_list() != ND_OK)
        goto done;

    printf("[shoot] wrote %zu frames, skipped %zu, %zu failed\n", g_saved, ND_ARRAY_LEN(SKIPPED),
           g_failed);
    printf("[shoot] compare with:\n"
           "  python3 neodct/tools/goldenframe.py --compare neodct/tests/golden %s\n",
           g_out);
    rc = (g_failed == 0u) ? 0 : 1;

done:
    if (cap != NULL) {
        root_off();
        nd_capture_close(cap);
        root_on();
    }
    drop_stage();
    return rc;
}
