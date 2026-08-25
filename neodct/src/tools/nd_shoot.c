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
 * ============ IT RENDERS 28 OF THE 49, ON PURPOSE ============
 *
 * The 49 recipes are spec-build-test.md section 3.6. Twenty-four of them
 * launch an app, and SESSION-SCOPE.md ports exactly three of those apps this
 * session: the Clock (which IS the stub dialog), CubeBench and the Browser
 * launcher. The other twenty-one stock and engineering apps ship their real
 * manifest and icon with a stub app.so behind them, so their screens do not
 * exist yet -- and a capture tool that drew *something* for those names would
 * be worse than useless: goldenframe.py would report pixel diffs that say
 * nothing about the port, and a passing frame could not be told from a
 * coincidence.
 *
 * So the twenty-one are SKIPPED, by name and each with the reason that
 * actually applies to it, listed in <out>/nd-shoot-skipped.json and printed
 * on stdout. goldenframe.py's compare() then reports each of them as
 * "missing -- not rendered by candidate", which is the truth. The frames that
 * ARE rendered are the whole home screen set, the whole app-selector set, the
 * whole widget gallery, the crash screen, the SMS banner, app-clock and
 * eng-cubebench.
 *
 * The Browser is real too and is staged with its real app.so, but none of the
 * 49 reference frames launches it -- `menu-browser` is the app-selector tile,
 * which is rendered from the manifest and needs no app.so at all. So there is
 * nothing for this tool to un-skip on its account; it is listed here because
 * "the Browser is missing" would otherwise be the obvious wrong conclusion.
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
#include "nd_contacts.h"
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

/* The nineteen names spec-build-test.md section 3.6 defines that this tree
 * cannot honestly draw yet. Kept in the file so the list is reviewable
 * against the spec table, and emitted verbatim into the output directory.
 *
 * EVERY REASON IS THE REASON FOR THAT ONE FRAME. A blanket string is worse
 * than no string: this table said "neodct/src/apps/ is empty" for months
 * after apps/CubeBench and apps/Browser landed, which is how eng-cubebench
 * stayed skipped while the app that draws it was sitting in the build. A
 * reader has to be able to tell "nobody has written this app" from "the app
 * exists and something else is in the way", so the two are worded
 * differently and the module that is actually missing is named.
 *
 * Two names that used to be here are gone: app-messages and
 * app-messages-inbox. apps/Messages is a real port now and
 * shoot_stock_apps() below launches it, the same way app-phonebook stopped
 * being skipped when apps/PhoneBook landed. Both of those frames are
 * byte-identical to a widget frame that is also rendered (app-messages ==
 * widget-pagedlist, app-phonebook == widget-verticallist); copying the
 * pixels across would have made the count look better and would have been a
 * lie, because nothing would have launched an app. Now something does, and
 * the two sides agreeing is a real check rather than a tautology. */
/* Nothing is skipped any more. Koki was the last entry, and it renders.
 *
 * The {NULL, NULL} sentinel is not decoration: an empty initialiser is a
 * GNU extension whose sizeof is 0, which turns every `i < ND_ARRAY_LEN(
 * SKIPPED)` into an always-false unsigned comparison and -Wtype-limits
 * (via -Wextra -Werror) rejects the file. The three readers below skip a
 * NULL name, so the table stays usable the day something needs skipping
 * again -- which is better than deleting the machinery and rebuilding it
 * from memory later. */
static const shoot_skip SKIPPED[] = {
    {NULL, NULL},
    /* Group 3 -- shoot_stock_apps. SESSION-SCOPE.md keeps all 25 apps with
     * their real manifest and icon, and stubs every one of them except
     * CubeBench and the Browser: what is behind these eleven manifests is
     * apps/Stub/app.so, a "This application has not been implemented yet."
     * dialog that is not the screen the reference frame holds.
     *
     * app-clock is NOT here: the Clock app IS that dialog (section 3.6
     * records app-clock as byte-identical to widget-messagedialog), so the
     * stub renders it for real. */

    /* Group 4 -- shoot_games. Both are rendered now: apps/Games is a real
     * port and shoot_games() below launches it. Decision 4 allowed BOTH to
     * be re-cut; measured, only game-snake needed it, and it is the frame
     * set's one `recut` name (OPEN-QUESTIONS.md GM-1). game-memory came out
     * byte-identical to the Python after all -- every card in that frame is
     * face down, so the different shuffle reaches no pixel -- and it kept
     * its original reference. Nothing from this group is skipped. */

    /* Group 5 -- shoot_telephony is fully rendered now: lib/nd_dialer.c is a
     * real port of System/ui/Dialer, so call-active and call-incoming are
     * drawn rather than stood in for. Nothing from this group is skipped. */

    /* Group 6 -- shoot_engineering_apps. Nothing from this group is skipped
     * any more: apps/CubeBench, apps/Modem, apps/FuelGauge, apps/LCDTest and
     * apps/TestsApp are all real ports and shoot_engineering_apps() below
     * launches all five. */
};

/* What this build DOES draw, in the order the groups draw it. It is a
 * declaration, not a description: main() checks the number of frames actually
 * saved against it and fails the run if the two have drifted apart, because
 * this list was two names short of the truth for as long as the skip table
 * was wrong and nothing noticed. --list prints it. */
/* Real entries, i.e. not the sentinel. */
static size_t n_skipped(void)
{
    size_t i;
    size_t n = 0u;

    for (i = 0u; i < ND_ARRAY_LEN(SKIPPED); i++) {
        if (SKIPPED[i].name != NULL)
            n++;
    }
    return n;
}

static const char *const RENDERED[] = {
    /* shoot_home */
    "home",
    "home-panel",
    "home-dialing",
    "home-nowallpaper",
    "home-simulation",
    /* shoot_app_selector */
    "menu-phone-book",
    "menu-panel",
    "menu-messages",
    "menu-games",
    "menu-settings",
    "menu-calculator",
    "menu-koki-mobile",
    "menu-browser",
    "menu-music",
    /* shoot_stock_apps */
    "app-phonebook",
    "app-messages",
    "app-messages-inbox",
    "app-calllog",
    "app-settings",
    "app-settings-wallpaper",
    "app-games",
    "app-koki",
    "app-calculator",
    "app-calculator-options",
    "app-clock",
    "app-tones",
    "app-musicplayer",
    /* shoot_games */
    "game-snake",
    "game-memory",
    /* shoot_telephony */
    "call-active",
    "call-incoming",
    "home-sms-banner",
    "contacts-picker",
    "crash-screen",
    /* shoot_engineering_apps -- shoot_docs.py's order, which is NOT the
     * manifest-id order: ModemInfo, FuelGauge, LCD Test, Cube Bench, Tests. */
    "eng-modem",
    "eng-fuelgauge",
    "eng-lcdtest",
    "eng-cubebench",
    "eng-tests",
    /* shoot_widgets */
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
 * icons and ui_home.json are byte-for-byte the same files the 27 already-exact
 * frames were rendered from.
 *
 * System/engineering gets the same treatment one level deeper, because the
 * engineering apps live in System/engineering/apps and NOT in System/apps --
 * `Cube Bench` is one of them. So engineering/ is a real directory too, its
 * tools/ stays a symlink, and its apps/ is expanded exactly like the stock
 * one. Before eng-cubebench was rendered this whole subtree was a single
 * symlink and nothing could have gained an app.so under it.
 *
 * ============ WHICH app.so EACH APP DIRECTORY GETS ============
 *
 * The one this build produced for it, if there is one, and apps/Stub/app.so
 * otherwise. The mapping is by directory name: neodct/src/apps/<Name> builds
 * build/<variant>/apps/<Name>/app.so, and <Name> is also the overlay's
 * directory name -- CubeBench and Browser both line up today, and an app a
 * later work package adds lines up without touching this file.
 *
 * Staging the real .so is not the same as rendering with it. Only the frames
 * that call run_app_inproc() dlopen anything; the rest read the manifest and
 * the icon, which are the overlay's own files either way.
 */

/* build/<variant>/bin/nd-shoot -> build/<variant>/apps/<name>/app.so. False
 * when this build has no such app, which is the normal case for the
 * twenty-two apps that are stubbed. */
static bool built_app_so(const char *name, char *out, size_t out_sz)
{
    char exe[ND_PATH_MAX];

    if (!self_exe_dir(exe, sizeof exe))
        return false;
    if (nd_snprintf(out, out_sz, "%s/../apps/%s/app.so", exe, name) != ND_OK)
        return false;
    return access(out, R_OK) == 0;
}

/* The one stub, the one every app directory gets when nothing better exists. */
static bool stub_app_so(char *out, size_t out_sz)
{
    return built_app_so("Stub", out, out_sz);
}

/* One app directory: a real directory of symlinks, plus app.so. `so` is the
 * shared object to link in as this app's app.so, or NULL to leave the
 * directory without one. */
static bool stage_one_app(const char *src_dir, const char *dst_dir, const char *so)
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

    if (so == NULL)
        return true;
    if (nd_snprintf(link, sizeof link, "%s/%s", dst_dir, ND_APP_SO_NAME) != ND_OK)
        return false;
    (void)unlink(link);
    return symlink(so, link) == 0;
}

/* A whole apps/ directory: one real subdirectory per app, each with the
 * app.so this build produced for it, falling back to `stub`. Used for both
 * System/apps and System/engineering/apps -- CubeBench is in the second one,
 * and the two need identical treatment because the app registry scans them
 * identically (nd_ui.c rescan_apps()). */
static bool stage_apps_dir(const char *apps_src, const char *apps_dst, const char *stub)
{
    DIR *d;
    struct dirent *e;
    char link[ND_PATH_MAX];
    char target[ND_PATH_MAX];
    bool ok = true;

    if (mkdir(apps_dst, 0755) != 0 && errno != EEXIST)
        return false;

    d = opendir(apps_src);
    if (d == NULL)
        return false;
    while (ok && (e = readdir(d)) != NULL) {
        char own[ND_PATH_MAX];
        const char *so;

        if (e->d_name[0] == '.')
            continue;
        if (nd_snprintf(target, sizeof target, "%s/%s", apps_src, e->d_name) != ND_OK ||
            nd_snprintf(link, sizeof link, "%s/%s", apps_dst, e->d_name) != ND_OK)
            continue;
        if (!dir_exists(target))
            continue;

        so = built_app_so(e->d_name, own, sizeof own) ? own : stub;
        ok = stage_one_app(target, link, so);
    }
    (void)closedir(d);
    return ok;
}

/* Every entry of `src` symlinked into `dst`, except the names in `skip`,
 * which the caller builds itself. */
static bool stage_links_except(const char *src, const char *dst, const char *const *skip,
                               size_t n_skip)
{
    DIR *d;
    struct dirent *e;
    char link[ND_PATH_MAX];
    char target[ND_PATH_MAX];

    if (mkdir(dst, 0755) != 0 && errno != EEXIST)
        return false;

    d = opendir(src);
    if (d == NULL)
        return false;
    while ((e = readdir(d)) != NULL) {
        size_t i;
        bool skipped = false;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        for (i = 0u; i < n_skip; i++) {
            if (strcmp(e->d_name, skip[i]) == 0)
                skipped = true;
        }
        if (skipped)
            continue;
        if (nd_snprintf(link, sizeof link, "%s/%s", dst, e->d_name) != ND_OK ||
            nd_snprintf(target, sizeof target, "%s/%s", src, e->d_name) != ND_OK)
            continue;
        if (symlink(target, link) != 0 && errno != EEXIST)
            nd_log_err(ND_LOG_OS, "symlink %s: %s", link, strerror(errno));
    }
    (void)closedir(d);
    return true;
}

/* System/engineering: tools/ is a plain symlink, apps/ is expanded so
 * CubeBench can gain the app.so this build made for it. */
static bool stage_engineering(const char *src, const char *dst, const char *stub)
{
    static const char *const EXPANDED[] = {"apps"};
    char apps_src[ND_PATH_MAX];
    char apps_dst[ND_PATH_MAX];

    if (!stage_links_except(src, dst, EXPANDED, ND_ARRAY_LEN(EXPANDED)))
        return false;
    if (nd_snprintf(apps_src, sizeof apps_src, "%s/apps", src) != ND_OK ||
        nd_snprintf(apps_dst, sizeof apps_dst, "%s/apps", dst) != ND_OK)
        return false;
    return stage_apps_dir(apps_src, apps_dst, stub);
}

/* System as a real directory: every entry symlinked, except apps/ and
 * engineering/, which are expanded so each app can gain an app.so. */
static bool stage_system(const char *sys_src, const char *sys_dst)
{
    static const char *const EXPANDED[] = {"apps", "engineering"};
    char stub[ND_PATH_MAX];
    char child_src[ND_PATH_MAX];
    char child_dst[ND_PATH_MAX];
    bool have_stub = stub_app_so(stub, sizeof stub);

    if (!have_stub)
        nd_log(ND_LOG_OS, "no apps/Stub/app.so in this build; app frames will be skipped");

    if (!stage_links_except(sys_src, sys_dst, EXPANDED, ND_ARRAY_LEN(EXPANDED)))
        return false;

    if (nd_snprintf(child_src, sizeof child_src, "%s/apps", sys_src) != ND_OK ||
        nd_snprintf(child_dst, sizeof child_dst, "%s/apps", sys_dst) != ND_OK)
        return false;
    if (!stage_apps_dir(child_src, child_dst, have_stub ? stub : NULL))
        return false;

    /* An overlay without an engineering/ directory is not an error: the
     * engineering menu is simply empty, which is what a non-engineering image
     * looks like. */
    if (nd_snprintf(child_src, sizeof child_src, "%s/engineering", sys_src) != ND_OK ||
        nd_snprintf(child_dst, sizeof child_dst, "%s/engineering", sys_dst) != ND_OK)
        return false;
    if (!dir_exists(child_src))
        return true;
    return stage_engineering(child_src, child_dst, have_stub ? stub : NULL);
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

static size_t app_index_of(nd_ui *ui, const char *name)
{
    size_t n = 0u;
    const nd_app_entry *apps = nd_ui_app_list(ui, &n);
    size_t i;

    for (i = 0u; i < n; i++) {
        if (strcmp(apps[i].name, name) == 0)
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

    {
        size_t n_apps = 0u;
        const nd_app_entry *apps = nd_ui_app_list(&ui, &n_apps);

        nd_appsel_init(&selector, &ui, "Main Menu", apps, n_apps, nd_ui_wallpaper(&ui));
    }

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

    printf("[shoot] telephony\n");

    write_settings("Palestine.jpg");
    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (telephony)");
        g_failed++;
        return;
    }
    nd_ui_sim_status(&ui, 4, 4, "Tello");

    /* The two call screens come from the SAME StubUI as everything else in
     * this block, which is what makes home-sms-banner its THIRD frame and
     * fixes the envelope's int(time.time() * 2) % 2 blink phase.
     *
     * OPEN-QUESTIONS.md S-3 called this out: two throwaway presents stood in
     * here while the Dialer was unported, and had to be DELETED rather than
     * left beside the real draws -- two extra commits would move the blink
     * phase and change home-sms-banner. They are gone. Each screen still
     * costs exactly one tick, because shoot_docs.py calls the draw_* helper
     * (which does not present) and then flushes by hand. */
    nd_dialer_draw_call(&ui, "0741234567", "Mum");
    (void)nd_ui_present(&ui);
    save_recent(cap, "call-active");

    nd_dialer_draw_incoming(&ui, "Mum", true);
    (void)nd_ui_present(&ui);
    save_recent(cap, "call-incoming");

    if (nd_vclock_frame() != 2u) {
        nd_log_err(ND_LOG_UI, "shoot: expected 2 ticks before the banner, got %llu",
                   (unsigned long long)nd_vclock_frame());
        g_failed++;
    }

    /* ui.notify.post_sms(1, tone=False); ui._unread_sms = 1 */
    nd_ui_sim_sms_banner(&ui, 1);
    nd_ui_set_unread_sms(&ui, 1);
    nd_ui_update(&ui);
    save_recent(cap, "home-sms-banner");

    /* "The contact picker the home screen opens on up/down": the last thing
     * the first StubUI block does, on the SAME ui and the same clock.
     *
     *     ui.keys.push(BACK)
     *     contacts.show_contact_selector(ui, title="Select", btn_text="Call")
     *
     * The list has one row -- init_databases() seeded ("NeoDCT Support",
     * "555-1234", 2) into the staged root when nd_ui_init ran -- so the
     * picker paints "Call" on the strip, draws the list, and leaves through
     * the queued Back. The list's frame is the one saved; nothing on it reads
     * the clock, but the two throwaway ticks above still decide its number.
     *
     * shoot_docs.py wraps the call in try/except because in the Python a
     * cancelled picker can raise out of the stub's key script. Here Back is a
     * value, not an exception, so there is nothing to catch. */
    {
        static const int32_t BACK_ONCE[] = {ND_KEY_CLEAR};
        key_script ks;
        nd_contact picked;

        if (!key_script_begin(&ks, &ui, BACK_ONCE, ND_ARRAY_LEN(BACK_ONCE))) {
            nd_log_err(ND_LOG_UI, "shoot: cannot open a key script for the contact picker");
            g_failed++;
        } else {
            (void)nd_contacts_show_selector(&ui, "Select", "Call", &picked);
            key_script_end(&ks, &ui);
            save_recent(cap, "contacts-picker");
        }
    }

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
 * Three of the thirteen apps are rendered, between them four frames.
 *
 *   app-phonebook   apps/PhoneBook is a real port. Its run() builds the
 *                   seven-item VerticalList, paints "Select" and blocks, and
 *                   that second frame is the reference. spec-build-test.md
 *                   section 3.6 records app-phonebook as byte-identical to
 *                   widget-verticallist, and the two are drawn by different
 *                   code here -- one by the widget gallery, one by the app --
 *                   so agreeing is a real check rather than a tautology.
 *   app-messages    apps/Messages is a real port. Its run() builds the
 *                   three-item PagedList and blocks, and that frame is the
 *                   reference -- byte-identical to widget-pagedlist, drawn
 *                   here by the app rather than by the widget gallery.
 *   app-messages-   the "No Messages" empty state, and the ONE case in this
 *   inbox           table that is not app_run(). See below.
 *   app-clock       section 3.6 records app-clock as byte-identical to
 *                   widget-messagedialog because the Clock app IS a "This
 *                   application has not been implemented yet." dialog, so the
 *                   one shipped stub app.so draws it for real.
 *
 * The other ten stock apps draw their own screens; the stub cannot stand in
 * for any of them and they stay skipped.
 *
 * ============ WHY THE INBOX FRAME GOES IN THROUGH open_inbox ============
 *
 * shoot_docs.py reaches it as run_app(ui, "Messages", keys=[ENTER]): Enter
 * picks "Inbox" off the root menu, the inbox is empty, _show_empty_state
 * paints "No Messages", and the next read_keypress raises ScriptExhausted
 * with that frame already committed. C has no exception to raise out of a
 * blocking read, and the empty state exits on key 14 ALONE -- so the only
 * key that gets out of it sends the app straight back to the root menu,
 * which redraws and becomes the last frame. Freezing the recording with the
 * frame budget instead would mean hardcoding a frame count.
 *
 * So this frame is taken through app_open_inbox(), the entry point the core
 * really calls when the notification banner is tapped with several unread
 * messages (nd_app.h, spec-apps-core.md section C3). It is
 * _show_inbox(ui, 2, 1) -- the same function with the same arguments that
 * the root menu's `sel == 0` branch calls -- so the pixels are the same
 * screen by construction, and the app is genuinely launched and genuinely
 * draws it. It also gives that entry point its only coverage in the
 * capture. OPEN-QUESTIONS.md MSG-5.
 *
 * ============ A FRESH UI PER CASE, AS THE RECIPE HAS ============
 *
 * shoot_docs.py opens a new `with StubUI(wallpaper="Palestine.jpg")` block
 * for every case in shoot_stock_apps, which means a fresh virtual clock and a
 * fresh canvas each time. Both frames here clear every row they occupy, so
 * neither is decided by it today -- but the next app ported into this group
 * may well be, and a shared clock is exactly the kind of difference that is
 * invisible until it is not.
 */

/* MessageDialog flushes pending input before its first draw, so a key written
 * into the channel up front is eaten before show() ever waits on it. The way
 * through is the one test_widgets_lists.c found: press a key and do not
 * release it, with that key in the repeat set. The flush consumes the press,
 * the held state survives it, and the synthesised repeat arrives after the
 * screen is up -- which is also how the Python's ScriptExhausted got out of an
 * app that would otherwise loop forever. */
static bool hold_key_begin(key_script *ks, nd_ui *ui, const int32_t *keys, size_t n_keys,
                           int32_t code)
{
    static int32_t held;

    held = code;
    if (!key_script_begin(ks, ui, keys, n_keys))
        return false;
    if (nd_input_set_repeat_codes(ks->in, &held, 1u) != ND_OK)
        return false;
    nd_input_set_repeat(ks->in, 0.20, 0.05);
    return nd_input_channel_send(&ks->ch, code, true) == ND_OK;
}

static size_t app_index_of(nd_ui *ui, const char *name);

/* uistub.run_app(ui, name, keys=...), by dlopen.
 *
 * `manifest_name` is the manifest's "name" field and NOT the directory name:
 * the two differ for the engineering apps, where System/engineering/apps/
 * CubeBench/manifest.json says "Cube Bench", with a space. uistub matches on
 * the manifest name (`if app["name"] == name`) and so does app_index_of(),
 * so the recipe's string is used verbatim and the space is load-bearing.
 *
 * `hold_key` is ND_KEY_NONE for an app whose loop ends on the frame budget,
 * and a key code for one that blocks on input and has to be let out. It is
 * NOT part of the Python recipe -- every case in shoot_docs.py passes
 * keys=[] -- it stands in for uistub's ScriptExhausted, which C cannot raise
 * out of a read. Passing a key an app treats as "quit" would end the run at
 * frame 1, which is why CubeBench (EXIT_KEYS = {14, 28, 46, 50}) must be
 * given ND_KEY_NONE and the MessageDialog behind app-clock must not.
 *
 * `entry_sym` is "app_run" for every case but one. nd_app.h lets Messages
 * export two more entry points that take exactly the same nd_ui * and return
 * exactly the same int, and app-messages-inbox is captured through
 * "app_open_inbox"; see the group comment above for why.
 *
 * `keys`/`n_keys` are shoot_docs.py's key script, written into the channel as
 * press/release pairs before the app is called. Only the two Calculator cases
 * have one -- every other case in the recipe passes keys=[] -- and a queued
 * script works there because neither the Calculator's own loop nor
 * VerticalList flushes the channel before its first draw. An app whose first
 * screen DOES flush (MessageDialog, PagedList) would eat one, which is what
 * `hold_key` is for; the two combine, the flush consuming the held press and
 * leaving the held state behind it. */
static void run_app_inproc(nd_capture *cap, nd_ui *ui, const char *manifest_name,
                           int64_t frame_budget, const char *slug, const int32_t *keys,
                           size_t n_keys, int32_t hold_key, const char *entry_sym)
{
    char so_path[ND_PATH_MAX];
    key_script ks;
    void *handle;
    int (*run)(nd_ui *);
    bool have_keys;
    size_t idx = app_index_of(ui, manifest_name);

    if (idx == (size_t)-1) {
        nd_log_err(ND_LOG_OS, "shoot: no app named '%s'", manifest_name);
        g_failed++;
        return;
    }
    if (nd_path_join(so_path, sizeof so_path, nd_ui_app_list(ui, NULL)[idx].path,
                     ND_APP_SO_NAME) != ND_OK) {
        g_failed++;
        return;
    }

    handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        nd_log_err(ND_LOG_OS, "shoot: %s: %s", slug, dlerror());
        g_failed++;
        return;
    }
    run = (int (*)(nd_ui *))(uintptr_t)dlsym(handle, entry_sym);
    if (run == NULL) {
        nd_log_err(ND_LOG_OS, "shoot: %s exports no %s", so_path, entry_sym);
        (void)dlclose(handle);
        g_failed++;
        return;
    }

    /* Either way the app gets a channel with nobody writing to it, so
     * read_keypress(0) is a poll that returns ND_KEY_NONE -- exactly what
     * uistub's empty KeyScript does, and not dependent on whether this host
     * has a /dev/input/event0. */
    have_keys = (hold_key == ND_KEY_NONE) ? key_script_begin(&ks, ui, keys, n_keys)
                                          : hold_key_begin(&ks, ui, keys, n_keys, hold_key);
    if (!have_keys) {
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

/* shoot_docs.py's `cases` table, restricted to the ports that exist, in its
 * order. `hold` is this file's stand-in for ScriptExhausted; see
 * run_app_inproc().
 *
 * PhoneBook is let out with CLEAR, which its main menu treats as Back. That
 * looks like the mistake run_app_inproc()'s comment warns about -- giving an
 * app a key it reads as "quit" -- and here it is the right answer: the
 * reference frame IS the first screen the app draws, so the app must be
 * stopped the moment that screen is on the panel and not one frame later. The
 * Python reaches the same place from the other side: keys=[] means the first
 * read_keypress() raises, and the frame already committed is the one saved.
 * Clock is different because MessageDialog drains the channel before drawing,
 * so its key has to arrive as a repeat afterwards. */

/* shoot_docs.py types 1, 2, 3 into the Calculator for app-calculator and
 * 7 then Enter for app-calculator-options. The trailing Clears are NOT in the
 * recipe: they are how the C leaves an app that the Python left by having
 * read_keypress() raise. Each one draws a frame the budget below refuses, so
 * they are invisible in the capture -- see CALC_FRAMES / CALC_OPT_FRAMES. */
static const int32_t CALC_KEYS[] = {ND_KEY_1,     ND_KEY_2,     ND_KEY_3,    ND_KEY_CLEAR,
                                    ND_KEY_CLEAR, ND_KEY_CLEAR, ND_KEY_CLEAR};
static const int32_t CALC_OPT_KEYS[] = {ND_KEY_7, ND_KEY_ENTER, ND_KEY_CLEAR, ND_KEY_CLEAR,
                                        ND_KEY_CLEAR};

/* THE BUDGET IS WHAT PICKS THE FRAME for the two Calculator cases, exactly as
 * uistub's idle_budget picks CubeBench's sixtieth (OPEN-QUESTIONS.md CB-2).
 *
 * Calculator.loop() redraws on EVERY key it recognises, Clear included, so
 * the keys that let the app out would each commit a frame after the one the
 * reference holds. There is no key this app treats as "leave without
 * redrawing": Clear deletes a digit and draws, and the only other way out is
 * a key it ignores, which never returns at all. So the budget stops the
 * recording at the frame the Python's ScriptExhausted stopped it at:
 *
 *   app-calculator          draw() + one per digit          = 4 frames
 *   app-calculator-options  draw() + the 7 + the list       = 3 frames
 *
 * The four Clears after them still run, still redraw, and are refused by
 * nd_capture with ND_ERR_BUSY -- which records nothing and does not tick the
 * virtual clock, so neither the ring nor the clock can tell they happened. */
#define CALC_FRAMES     4
#define CALC_OPT_FRAMES 3

/* shoot_docs.py's ("Settings", [ENTER], "app-settings-wallpaper", -1, 240).
 * The Enter is the recipe's; the frame count is this file's stand-in for
 * ScriptExhausted, and it is an exact count, not a guess:
 *
 *   1  run()'s VerticalList, "Settings"
 *   2  _wallpaper_menu_once()'s VerticalList, "Wallpaper"   <- the reference
 *   3  run()'s VerticalList again, after Back unwinds the picker  <- refused
 */
static const int32_t SETTINGS_WP_KEYS[] = {ND_KEY_ENTER};
#define SETTINGS_WP_FRAMES 2

static const struct {
    const char *manifest_name;
    int64_t budget;
    const char *slug;
    const int32_t *keys;
    size_t n_keys;
    int32_t hold;
    const char *entry;
} STOCK_CASES[] = {
    /* Koki is the one case with no way out but the budget: it is a game
     * loop, not a widget, so there is no held key that ends it. 400 frames
     * is what the reference was captured at. */
    {"Koki Mobile", 400, "app-koki", NULL, 0u, ND_KEY_NONE, ND_APP_SYM_RUN},
    {"Phone book", 240, "app-phonebook", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_RUN},
    {"Messages", 240, "app-messages", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_RUN},
    {"Messages", 240, "app-messages-inbox", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_OPEN_INBOX},
    /* CallLog's root PagedList drains the channel before it draws, so its
     * way out is a held key like Tones'; Games' root VerticalList does not
     * drain, but a held Back reaches its wait_for_key() just the same and
     * ends the app on the frame the reference holds. */
    {"Call Log", 240, "app-calllog", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_RUN},
    /* Settings' root VerticalList does not drain the channel either, so a
     * held Back reaches its wait_for_key() with the reference frame already
     * committed. app-settings-wallpaper is shoot_docs.py's keys=[ENTER]: the
     * Enter is queued ahead of the held Back, so the root list draws (frame
     * 1), Enter opens the wallpaper picker, the picker draws (frame 2) and
     * the held Back leaves it. Back then returns the app to its root menu,
     * which redraws -- THE BUDGET OF 2 IS WHAT REFUSES THAT THIRD FRAME, the
     * same way it picks the Calculator's, and the repeat that follows lets
     * the app out of the redrawn root list. */
    {"Settings", 240, "app-settings", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_RUN},
    {"Settings", SETTINGS_WP_FRAMES, "app-settings-wallpaper", SETTINGS_WP_KEYS,
     ND_ARRAY_LEN(SETTINGS_WP_KEYS), ND_KEY_CLEAR, ND_APP_SYM_RUN},
    {"Games", 240, "app-games", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_RUN},
    {"Calculator", CALC_FRAMES, "app-calculator", CALC_KEYS, ND_ARRAY_LEN(CALC_KEYS), ND_KEY_NONE,
     ND_APP_SYM_RUN},
    {"Calculator", CALC_OPT_FRAMES, "app-calculator-options", CALC_OPT_KEYS,
     ND_ARRAY_LEN(CALC_OPT_KEYS), ND_KEY_NONE, ND_APP_SYM_RUN},
    {"Clock", 240, "app-clock", NULL, 0u, ND_KEY_ENTER, ND_APP_SYM_RUN},
    /* Tones' PagedList flushes the channel before it draws, so its way out
     * has to be a held key like Clock's rather than a queued one. Back on
     * the first screen returns from run() at once, and the frame already
     * committed is the one saved. */
    {"Tones", 240, "app-tones", NULL, 0u, ND_KEY_CLEAR, ND_APP_SYM_RUN},
    /* Music is THE THIRD case whose frame is picked by the budget, and the
     * only one where the budget is 1.
     *
     * With no card staged, run(ui) shows a MessageDialog and then a
     * TextScroller, in that order and unconditionally -- there is no key that
     * leaves the app between them. The Python stops at the dialog because
     * MessageDialog.show()'s first read_keypress raises ScriptExhausted with
     * that frame already committed; a held Enter here gets the same frame
     * drawn and then walks straight on into the help text, whose last page
     * would be frames[-1]. So the recording is stopped after the dialog's
     * single present, and the scroller's pages are refused by nd_capture
     * exactly as the Calculator's trailing Clears are. */
    {"Music", 1, "app-musicplayer", NULL, 0u, ND_KEY_ENTER, ND_APP_SYM_RUN},
};

static void shoot_stock_apps(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    size_t i;

    printf("[shoot] stock apps (9 of 13 -- the other four are not ported)\n");

    for (i = 0u; i < ND_ARRAY_LEN(STOCK_CASES); i++) {
        /* A fresh WP + STATUS UI per case, as every `with StubUI(...)` block
         * in shoot_stock_apps gets. */
        write_settings("Palestine.jpg");
        nd_vclock_enable();
        nd_ui_sim_clear(&ui);
        if (nd_ui_init(&ui, fb) != ND_OK) {
            nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (%s)", STOCK_CASES[i].slug);
            g_failed++;
            nd_vclock_disable();
            return;
        }
        nd_ui_sim_status(&ui, 4, 4, "Tello");

        run_app_inproc(cap, &ui, STOCK_CASES[i].manifest_name, STOCK_CASES[i].budget,
                       STOCK_CASES[i].slug, STOCK_CASES[i].keys, STOCK_CASES[i].n_keys,
                       STOCK_CASES[i].hold, STOCK_CASES[i].entry);

        nd_ui_teardown(&ui);
        nd_ui_sim_clear(&ui);
        nd_vclock_disable();
    }
}

/* ------------------------------------------------------------------ *
 * Group 4 -- shoot_games, and the one frame in the set that is `recut`
 * ------------------------------------------------------------------ *
 *
 * shoot_docs.py's recipe:
 *
 *     ([DOWN, ENTER, ENTER], "game-snake",  300)
 *     ([ENTER, ENTER],       "game-memory", 300)
 *     with StubUI() as ui:                       # NO wallpaper
 *         ui.stub.simulate_status(4, 4, "Tello")
 *         frames = run_app(ui, "Games", keys=keys, frame_budget=budget)
 *         save_frame(frames, slug, out)          # frames[-1]
 *
 * The Games menu lists Memory then Snake, which is why Snake needs a Down
 * first and Memory does not.
 *
 * ============ 300 IS NOT WHAT PICKS EITHER FRAME ============
 *
 * Both games poll `read_keypress` rather than blocking, so in the Python it
 * is uistub's idle_budget that ends them: once the script is exhausted the
 * 61st idle poll raises ScriptExhausted. And because the virtual clock only
 * advances when a frame is COMMITTED, an idle poll moves no time -- so the
 * first tick never falls due, the snake never takes a step and the board is
 * never touched. `frames[-1]` is therefore the game's own opening render,
 * and the 300-frame budget never bites.
 *
 * C has no exception to raise out of a read, so the same end state is
 * reached the way CubeBench's is (OPEN-QUESTIONS.md CB-2): the frame budget
 * stops the recording at the opening render, and a HELD Back lets the app
 * out afterwards. The budgets below are therefore exact frame counts, and
 * the count is what test_games.c pins:
 *
 *   game-snake   Games menu, the Down redraw, the Snake menu, the board = 4
 *   game-memory  Games menu, the Memory menu, the board               = 3
 *
 * Everything the held Back draws after that -- the menu each game returns
 * to, and the one above it -- is refused by nd_capture with ND_ERR_BUSY,
 * which records nothing and does not tick the clock.
 *
 * ============ ONE OF THE TWO IS `recut`, NOT BOTH =======================
 *
 * OPEN-QUESTIONS.md decision 4 gave permission to re-capture both. Measured,
 * only one needed it.
 *
 *   game-snake   40 of 42,000 pixels differ from the Python -- two 6x6
 *                outlined boxes, and nothing else. That is the food cell and
 *                only the food cell: CPython's MT19937 put it at grid (5,12)
 *                and libneodct's pinned LCG puts it at (4,0). The score, the
 *                border, all three body cells and the black softkey band are
 *                byte-identical. Its reference is re-cut from this build and
 *                carries "tolerance": "recut" in the golden manifest.
 *
 *   game-memory  0 pixels differ. Memory's shuffle is not the Python's
 *                either, but every card in this frame is FACE DOWN, so the
 *                board is forty identical white rectangles and the shuffle
 *                reaches no pixel. Re-cutting it would have replaced a
 *                reference the Python drew with a byte-identical one this
 *                build drew, losing the cross-check and buying nothing. It
 *                keeps its original reference and stays `exact`.
 *
 * Either way, both are compared like any other frame from now on: a change
 * to either game's geometry fails as loudly as it would anywhere else.
 */

static const int32_t SNAKE_KEYS[] = {ND_KEY_DOWN, ND_KEY_ENTER, ND_KEY_ENTER};
static const int32_t MEMORY_KEYS[] = {ND_KEY_ENTER, ND_KEY_ENTER};

#define SNAKE_FRAMES  4
#define MEMORY_FRAMES 3

static const struct {
    int64_t budget;
    const char *slug;
    const int32_t *keys;
    size_t n_keys;
} GAME_CASES[] = {
    {SNAKE_FRAMES, "game-snake", SNAKE_KEYS, ND_ARRAY_LEN(SNAKE_KEYS)},
    {MEMORY_FRAMES, "game-memory", MEMORY_KEYS, ND_ARRAY_LEN(MEMORY_KEYS)},
};

static void shoot_games(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    size_t i;

    printf("[shoot] games (2 frames; game-snake is the set's one recut)\n");

    for (i = 0u; i < ND_ARRAY_LEN(GAME_CASES); i++) {
        /* A fresh `with StubUI()` per case: no wallpaper, and a fresh
         * virtual clock, which is what the games seed the generator from. */
        write_settings(NULL);
        nd_vclock_enable();
        nd_ui_sim_clear(&ui);
        if (nd_ui_init(&ui, fb) != ND_OK) {
            nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (%s)", GAME_CASES[i].slug);
            g_failed++;
            nd_vclock_disable();
            return;
        }
        nd_ui_sim_status(&ui, 4, 4, "Tello");

        run_app_inproc(cap, &ui, "Games", GAME_CASES[i].budget, GAME_CASES[i].slug,
                       GAME_CASES[i].keys, GAME_CASES[i].n_keys, ND_KEY_CLEAR, ND_APP_SYM_RUN);

        nd_ui_teardown(&ui);
        nd_ui_sim_clear(&ui);
        nd_vclock_disable();
    }
}

/* ------------------------------------------------------------------ *
 * Group 6 -- shoot_engineering_apps, all five of them
 * ------------------------------------------------------------------ *
 *
 * shoot_docs.py's recipe, in this order:
 *
 *     ("ModemInfo", [], "eng-modem")
 *     ("FuelGauge", [], "eng-fuelgauge")
 *     ("LCD Test",  [], "eng-lcdtest")
 *     ("Cube Bench",[], "eng-cubebench")
 *     ("Tests",     [], "eng-tests")
 *     with StubUI() as ui:                       # NO wallpaper
 *         ui.stub.simulate_status(4, 4, "Tello")
 *         frames = run_app(ui, name, keys=keys)
 *         save_frame(frames, slug, out)          # frames[-1]
 *
 * A fresh StubUI per case, so a fresh virtual clock, and no wallpaper -- the
 * default. Neither reaches any of these five frames: each app clears rows
 * 0..content_bottom and repaints the softkey strip underneath, which between
 * them is every pixel of the 240x175. Both are set anyway, because the recipe
 * is the specification.
 *
 * The manifest names are the strings uistub matches on and two of them are
 * NOT the directory name -- "Cube Bench" and "LCD Test" both have a space,
 * and "Tests" is the TestsApp directory. The space is load-bearing.
 *
 * ============ simulate_status() DECIDES TWO OF THESE FRAMES ============
 *
 * It is not chrome here the way it is on the home screen. It patches
 * `battery.hardware`, `modem.signal_level()` and `modem.operator_display()`
 * on the live service objects, and:
 *
 *   eng-fuelgauge  exists ONLY because of it. FuelGauge's first act is
 *                  `if not battery.hardware: MessageDialog(...).show()`, so
 *                  without the patch the reference frame would be a copy of
 *                  widget-messagedialog. With it, the app runs, asks a gauge
 *                  that is not there for its registers, and draws the ERROR
 *                  row the frame holds.
 *   eng-modem      gets "BARS 4/4" from the patched signal_level(), while
 *                  "OPER --" comes from the unpatched raw attribute beside
 *                  it. Two rows, two different readouts, on the same frame.
 *
 * nd_ui_sim_status() is the C spelling of all three (nd_ui_sim.h), and the
 * two apps read it through nd_ui_status_battery_hardware() and
 * nd_ui_status_signal_level().
 *
 * ============ WHAT ENDS EACH RUN ============
 *
 * Every case in the recipe passes keys=[], so in the Python every one of them
 * is ended by uistub: idle_budget=60 for the two that poll, ScriptExhausted
 * on the first read for the two that block. C can raise neither, so each case
 * below names the substitute, and none of them is a guess:
 *
 *   eng-cubebench  THE FRAME BUDGET. main.py polls read_keypress(0) and never
 *                  blocks, so the Python's 61st idle poll raises with 60
 *                  frames committed and frames[-1] is the sixtieth. The C
 *                  reaches the same end state when the 61st nd_fb_update()
 *                  returns ND_ERR_BUSY. Held keys are wrong for this one --
 *                  its EXIT_KEYS are {14, 28, 46, 50} and any of them would
 *                  stop it at frame 1. OPEN-QUESTIONS.md CB-2.
 *
 *   eng-modem      A HELD Back, and the reference frame is frame 1. Both apps
 *   eng-fuelgauge  draw unconditionally at the top of the loop and only then
 *                  read a key, and with a virtual clock that advances one
 *                  0.1 s tick per COMMITTED frame, `now - last_draw` never
 *                  reaches their 1.0 s refresh again -- 60 idle polls move no
 *                  time at all. So the Python's frames[-1] is its first
 *                  frame, and a held Back that arrives at the first
 *                  read_keypress leaves the app with exactly that frame
 *                  committed. Handing an app its own quit key is the mistake
 *                  run_app_inproc() warns about, and here it is the right
 *                  answer for the same reason it is for app-phonebook: the
 *                  reference IS the first screen the app draws.
 *
 *   eng-lcdtest    A HELD Back again, and the reference frame is the SECOND
 *                  of two identical ones. main.py commits twice per pattern
 *                  -- softkey.update() presents and then fb.update() presents
 *                  the same pixels -- before it blocks in wait_for_key(). The
 *                  Python's frames[-1] is the second commit; so is the C's.
 *
 *   eng-tests      A HELD Enter, not Back. TestsApp commits three times
 *                  (softkey, "Hello World", the dialog) and the reference is
 *                  the third, so the key has to arrive at MessageDialog's
 *                  wait and not before. MessageDialog drains the channel
 *                  before its first draw, which eats a queued press and
 *                  leaves the HELD state behind it, so the synthesised repeat
 *                  arrives after the dialog is up -- the same trick app-clock
 *                  uses. Back would be wrong twice over: it is
 *                  MessageDialog's cancel key AND it is not in TestsApp's own
 *                  (46, 28, 50), so the app would loop straight back into
 *                  show() and redraw for ever.
 */

/* uistub.StubUI(idle_budget=60). This is the number that decides which frame
 * eng-cubebench.png is, not shoot_docs.py's frame_budget=240. */
#define ENG_IDLE_BUDGET 60

static const struct {
    const char *manifest_name;
    int64_t budget;
    const char *slug;
    int32_t hold;
} ENG_CASES[] = {
    {"ModemInfo", 240, "eng-modem", ND_KEY_CLEAR},
    {"FuelGauge", 240, "eng-fuelgauge", ND_KEY_CLEAR},
    {"LCD Test", 240, "eng-lcdtest", ND_KEY_CLEAR},
    {"Cube Bench", ENG_IDLE_BUDGET, "eng-cubebench", ND_KEY_NONE},
    {"Tests", 240, "eng-tests", ND_KEY_ENTER},
};

static void shoot_engineering_apps(nd_capture *cap)
{
    nd_fb *fb = nd_capture_fb(cap);
    nd_ui ui;
    size_t i;

    printf("[shoot] engineering apps (5 frames)\n");

    for (i = 0u; i < ND_ARRAY_LEN(ENG_CASES); i++) {
        /* A fresh `with StubUI()` per case: no wallpaper, fresh clock. */
        write_settings(NULL);
        nd_vclock_enable();
        nd_ui_sim_clear(&ui);
        if (nd_ui_init(&ui, fb) != ND_OK) {
            nd_log_err(ND_LOG_UI, "shoot: nd_ui_init failed (%s)", ENG_CASES[i].slug);
            g_failed++;
            nd_vclock_disable();
            return;
        }
        nd_ui_sim_status(&ui, 4, 4, "Tello");

        /* eng-cubebench is the one `tolerance` frame in the set: sin() and
         * cos() disagree by an ULP between libcs and that can move a
         * wireframe vertex by a pixel (OPEN-QUESTIONS.md frame tolerance
         * policy). On a glibc host it is byte-exact, because CPython's
         * math.sin is the platform libm's and the capture ran the same code
         * on the same doubles. The budget exists for musl on the device. */
        run_app_inproc(cap, &ui, ENG_CASES[i].manifest_name, ENG_CASES[i].budget,
                       ENG_CASES[i].slug, NULL, 0u, ENG_CASES[i].hold, ND_APP_SYM_RUN);

        nd_ui_teardown(&ui);
        nd_ui_sim_clear(&ui);
        nd_vclock_disable();
    }
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
        if (SKIPPED[i].name == NULL)
            continue; /* the sentinel */
        (void)fprintf(f, "    {\n      \"name\": \"%s\",\n      \"reason\": \"%s\"\n    }%s\n",
                      SKIPPED[i].name, SKIPPED[i].reason,
                      i + 1u < ND_ARRAY_LEN(SKIPPED) && SKIPPED[i + 1u].name != NULL ? "," : "");
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
    size_t i;

    printf("rendered (%zu):\n", ND_ARRAY_LEN(RENDERED));
    for (i = 0u; i < ND_ARRAY_LEN(RENDERED); i++)
        printf("  %s\n", RENDERED[i]);
    printf("skipped (%zu):\n", n_skipped());
    for (i = 0u; i < ND_ARRAY_LEN(SKIPPED); i++) {
        if (SKIPPED[i].name != NULL)
            printf("  %-24s %s\n", SKIPPED[i].name, SKIPPED[i].reason);
    }
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
    shoot_games(cap);
    shoot_telephony(cap);
    shoot_engineering_apps(cap);
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

    /* RENDERED[] and SKIPPED[] together are this tool's claim about itself,
     * and test_shoot.c checks the claim against the 49 reference names. A
     * group that quietly stopped saving a frame would otherwise show up only
     * as a "missing" line in goldenframe's output, which looks exactly like a
     * frame that was never meant to be there. */
    if (g_saved != ND_ARRAY_LEN(RENDERED)) {
        nd_log_err(ND_LOG_FB, "shoot: saved %zu frames but RENDERED[] names %zu", g_saved,
                   ND_ARRAY_LEN(RENDERED));
        g_failed++;
    }

    printf("[shoot] wrote %zu frames, skipped %zu, %zu failed\n", g_saved, n_skipped(),
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
