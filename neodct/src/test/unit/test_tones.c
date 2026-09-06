/* test_tones.c -- the Tones app, app id 9.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The strings are the Python's, including both halves of the "Add
 *     more..." help -- which one is shown depends on whether a card is in the
 *     phone, and getting that backwards would tell a user with no card to
 *     copy files onto it.
 *
 *  2. SUPPORTED_EXTS is ".mp3", case-insensitively and ASCII-only. Python's
 *     str.lower() does not consult the locale; strcasecmp does, and on a
 *     Turkish locale the two disagree about "I".
 *
 *  3. The display name is os.path.splitext(os.path.basename(f))[0], including
 *     splitext's rule that leading dots belong to the name.
 *
 *  4. _scan_tones() walks every tone directory, descends into
 *     subdirectories, keeps only .mp3, and sorts by name.lower() -- so
 *     "alpha.MP3" sorts before "Beta.mp3" and a file in a subdirectory sorts
 *     among the ones beside it. Driven against a real directory tree under a
 *     scratch ND_ROOT.
 *
 *  5. _tone_dirs() puts the stock directory first and adds the user one only
 *     when it exists. "STOCK CONTENT ALWAYS COMES FIRST and the apps rely on
 *     that ordering" (nd_storage.h).
 *
 *  6. TonePreviewPlayer really spawns and really stops. Driven against a stub
 *     `mpv` on $PATH -- the only program the spawn has to find, because
 *     MPV_CMD execs the player itself -- so no real mpv is required and
 *     nothing is played: the claim is that a child appears, that stopping it
 *     reaps it, and that app_shutdown() stops it too -- which is the SIGTERM
 *     teardown contract in nd_app.h, and the reason the phone does not ring
 *     silently. $PATH holding NOTHING BUT that stub is itself the assertion
 *     that argv[0] is the player: an MPV_CMD that went back to naming a
 *     wrapper would have nothing to exec and no preview would start.
 *
 *  7. THE GOLDEN FRAME. app-tones is the PagedList's first page, judged by
 *     the SHA-256 over raw RGB that goldenframe.py compares.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_settings.h"
#include "nd_storage.h"

#include "smallapp_test.h"

#include "../../apps/Tones/tones.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*is_supported)(const char *);
    const char *(*display_name)(const char *, char *, size_t);
    size_t (*dirs)(char (*)[ND_TONES_PATH_MAX], size_t);
    size_t (*scan)(nd_tone *, size_t);
    void (*preview_play)(const char *);
    void (*preview_stop)(void);
    pid_t (*preview_pid)(void);
    const char *const *add_more_label;
    const char *const *add_more_help;
    const char *const *add_more_help_with_card;
    const char *const *menu;
    const char *const *ringing_options;
    const char *const *mpv_cmd;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.is_supported = sa_sym(h, "nd_tones_is_supported");
    *(void **)&api.display_name = sa_sym(h, "nd_tones_display_name");
    *(void **)&api.dirs = sa_sym(h, "nd_tones_dirs");
    *(void **)&api.scan = sa_sym(h, "nd_tones_scan");
    *(void **)&api.preview_play = sa_sym(h, "nd_tones_preview_play");
    *(void **)&api.preview_stop = sa_sym(h, "nd_tones_preview_stop");
    *(void **)&api.preview_pid = sa_sym(h, "nd_tones_preview_pid");
    api.add_more_label = dlsym(h, "nd_tones_add_more_label");
    api.add_more_help = dlsym(h, "nd_tones_add_more_help");
    api.add_more_help_with_card = dlsym(h, "nd_tones_add_more_help_with_card");
    api.menu = dlsym(h, "nd_tones_menu");
    api.ringing_options = dlsym(h, "nd_tones_ringing_options");
    api.mpv_cmd = dlsym(h, "nd_tones_mpv_cmd");

    return api.run != NULL && api.shutdown != NULL && api.is_supported != NULL &&
           api.display_name != NULL && api.dirs != NULL && api.scan != NULL &&
           api.preview_play != NULL && api.preview_stop != NULL && api.preview_pid != NULL &&
           api.add_more_label != NULL && api.add_more_help != NULL &&
           api.add_more_help_with_card != NULL && api.menu != NULL && api.ringing_options != NULL &&
           api.mpv_cmd != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. The strings
 * ------------------------------------------------------------------ */

/* ============ THE SHAPE OF MPV_CMD, NOT ITS SPELLING ============
 *
 * Checking the argv element by element is not enough on its own, and this
 * test is the proof: `nice -n -10 mpv ...` passed an element-by-element
 * check of its own spelling for a whole release while every preview on the
 * handset was silent. What was wrong with that argv was structural. argv[0]
 * was a WRAPPER, so the program the kernel exec'd was not the player, and
 * busybox nice dies on setpriority rather than degrading -- and an ndusr app
 * has no CAP_SYS_NICE and no RLIMIT_NICE headroom, so it always died.
 *
 * So this pins the shape: the exec target is the player itself, and is none
 * of the wrappers a future edit might reach for to influence scheduling or
 * identity. Every name below needs a privilege this phone grants no app, so
 * putting one in front of mpv makes the spawn dead on the handset -- and it
 * can be dead there while working perfectly on a developer's desktop, where
 * a negative nice level is often allowed and sudo exists at all. */
static void check_exec_target_is_the_player(const char *argv0)
{
    static const char *const wrappers[] = {"nice",    "renice", "chrt", "ionice",
                                           "setpriv", "sudo",   "su",   "doas"};
    size_t i;

    CHECK_STR(argv0, "mpv", "MPV_CMD[0] is the program that actually gets exec'd");
    for (i = 0u; i < sizeof wrappers / sizeof wrappers[0]; i++) {
        char what[96];

        (void)nd_snprintf(what, sizeof what, "MPV_CMD[0] is not the wrapper `%s`", wrappers[i]);
        CHECK(strcmp(argv0, wrappers[i]) != 0, what);
    }
}

static void test_strings(void)
{
    CHECK_STR(*api.add_more_label, "Add more...", "ADD_MORE_LABEL");
    CHECK_STR(api.menu[0], "Ringing Options", "the PagedList's first page");
    CHECK_STR(api.menu[1], "Ringing Tones", "the PagedList's second page");
    CHECK_STR(api.ringing_options[0], "Ring", "Ringing Options[0]");
    CHECK_STR(api.ringing_options[1], "Vibrate", "Ringing Options[1]");

    CHECK_STR(*api.add_more_help,
              "Add more ringtones by adding an SD card!\n"
              "\n"
              "Format a card as FAT32, make a folder called \"tones\" on it, and copy "
              "your .mp3 files into it.\n"
              "\n"
              "Put the card in the phone and the tones appear in this list next to the "
              "built-in ones. The phone can set a blank card up for you from Settings.",
              "ADD_MORE_HELP");
    CHECK_STR(*api.add_more_help_with_card,
              "Add more ringtones from your SD card!\n"
              "\n"
              "Copy .mp3 files into the \"tones\" folder on the card that is in the "
              "phone, and they appear in this list next to the built-in ones.",
              "ADD_MORE_HELP_WITH_CARD");

    /* MPV_CMD, which is the Python's minus its `nice -n -10`; tones.h says at
     * length why the nice had to go. Four elements, so anything that reads a
     * fifth is reading off the end of the array. */
    CHECK_INT(ND_TONES_MPV_ARGC, 4, "MPV_CMD is four elements long");
    CHECK_STR(api.mpv_cmd[0], "mpv", "MPV_CMD[0]");
    CHECK_STR(api.mpv_cmd[1], "--no-video", "MPV_CMD[1]");
    CHECK_STR(api.mpv_cmd[2], "--audio-buffer=4", "MPV_CMD[2]");
    CHECK_STR(api.mpv_cmd[3], "--quiet", "MPV_CMD[3]");
    check_exec_target_is_the_player(api.mpv_cmd[0]);

    CHECK_STR(ND_TONES_SYSTEM_DIR, "/NeoDCT/System/tones", "SYSTEM_TONES_DIR");
    CHECK_STR(ND_TONES_USER_DIR, "/NeoDCT/User/tones", "USER_TONES_DIR");
    CHECK_INT(ND_TONES_ROOT_ID, 9, "ROOT_ID");
    CHECK_DBL(ND_TONES_PREVIEW_DELAY, 0.5, "the half-second preview delay");
}

/* ------------------------------------------------------------------ *
 * 2 and 3. The two string helpers
 * ------------------------------------------------------------------ */

static void test_is_supported(void)
{
    CHECK(api.is_supported("Low.mp3"), "lower case");
    CHECK(api.is_supported("Low.MP3"), "upper case");
    CHECK(api.is_supported("Low.Mp3"), "mixed case");
    CHECK(api.is_supported(".mp3"), "a file called nothing but the extension");
    CHECK(!api.is_supported("Low.wav"), "wav is not in SUPPORTED_EXTS");
    CHECK(!api.is_supported("Low.ogg"), "ogg is not either");
    CHECK(!api.is_supported("mp3"), "no dot");
    CHECK(!api.is_supported("Low.mp3.txt"), "the extension has to be last");
    CHECK(!api.is_supported(""), "the empty name");
    CHECK(!api.is_supported(NULL), "NULL");
}

static void expect_name(const char *filename, const char *want)
{
    char out[ND_TONES_NAME_MAX];

    CHECK_STR(api.display_name(filename, out, sizeof out), want, filename);
}

static void test_display_name(void)
{
    expect_name("Low.mp3", "Low");
    expect_name("/NeoDCT/System/tones/Nokia Tune.mp3", "Nokia Tune");
    expect_name("no-extension", "no-extension");
    expect_name("two.dots.mp3", "two.dots");
    /* splitext(".hidden") is (".hidden", "") -- the dots that START a
     * basename are part of the name. */
    expect_name(".hidden", ".hidden");
    expect_name(".hidden.mp3", ".hidden");
    expect_name("", "");
}

/* ------------------------------------------------------------------ *
 * 4 and 5. The scan
 * ------------------------------------------------------------------ */

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];

static bool write_file(const char *logical)
{
    char real[ND_PATH_MAX];
    FILE *f;

    if (nd_path_resolve(real, sizeof real, logical) != ND_OK)
        return false;
    f = fopen(real, "w");
    if (f == NULL)
        return false;
    (void)fputs("not really an mp3\n", f);
    (void)fclose(f);
    return true;
}

static void build_tone_tree(void)
{
    CHECK_INT(nd_mkdir_p(ND_TONES_SYSTEM_DIR "/sub", 0755u), ND_OK, "stock tones directory");
    CHECK_INT(nd_mkdir_p(ND_TONES_USER_DIR, 0755u), ND_OK, "user tones directory");

    CHECK(write_file(ND_TONES_SYSTEM_DIR "/Beta.mp3"), "Beta.mp3");
    CHECK(write_file(ND_TONES_SYSTEM_DIR "/alpha.MP3"), "alpha.MP3");
    CHECK(write_file(ND_TONES_SYSTEM_DIR "/notes.txt"), "notes.txt");
    CHECK(write_file(ND_TONES_SYSTEM_DIR "/Zeta.wav"), "Zeta.wav");
    CHECK(write_file(ND_TONES_SYSTEM_DIR "/sub/Gamma.mp3"), "sub/Gamma.mp3");
    CHECK(write_file(ND_TONES_SYSTEM_DIR "/sub/delta.mp3"), "sub/delta.mp3");
    CHECK(write_file(ND_TONES_USER_DIR "/Omega.mp3"), "Omega.mp3");
}

static void test_dirs(void)
{
    char dirs[ND_TONES_DIRS_MAX][ND_TONES_PATH_MAX];
    size_t n;

    /* The card state file is not under this scratch root, so there is no
     * card -- which is the state the ordering claim is made in. */
    CHECK(!nd_storage_is_ready(), "no SD card in this test");

    n = api.dirs(dirs, ND_TONES_DIRS_MAX);
    CHECK_INT(n, 2, "the stock directory and the user one");
    CHECK_STR(dirs[0], ND_TONES_SYSTEM_DIR, "STOCK CONTENT COMES FIRST");
    CHECK_STR(dirs[1], ND_TONES_USER_DIR, "then the user's");

    /* The paths are LOGICAL, not ND_ROOT-resolved: what goes into
     * system.audio.ringtone has to be the path the phone will read back.
     * OPEN-QUESTIONS.md TN-3. */
    CHECK(dirs[0][0] == '/' && strncmp(dirs[0], "/NeoDCT/", 8u) == 0,
          "and they are logical /NeoDCT paths");

    CHECK_INT(api.dirs(NULL, 4u), 0, "NULL out");
    CHECK_INT(api.dirs(dirs, 0u), 0, "zero capacity");
}

static void test_scan(void)
{
    nd_tone *tones = calloc(ND_TONES_MAX, sizeof *tones);
    size_t n;

    if (tones == NULL) {
        CHECK(false, "allocation");
        return;
    }

    n = api.scan(tones, ND_TONES_MAX);

    /* notes.txt and Zeta.wav are not .mp3; the other five are, one of them
     * two directories deep and one of them in the user directory. */
    CHECK_INT(n, 5, "five tones");
    if (n == 5u) {
        /* sorted by name.lower(): alpha, beta, delta, gamma, omega */
        CHECK_STR(tones[0].name, "alpha", "alpha.MP3 sorts first, case-insensitively");
        CHECK_STR(tones[1].name, "Beta", "Beta");
        CHECK_STR(tones[2].name, "delta", "a subdirectory's tone sorts among the rest");
        CHECK_STR(tones[3].name, "Gamma", "Gamma");
        CHECK_STR(tones[4].name, "Omega", "the user directory's tone is in the same list");

        CHECK_STR(tones[0].path, ND_TONES_SYSTEM_DIR "/alpha.MP3", "alpha path");
        CHECK_STR(tones[2].path, ND_TONES_SYSTEM_DIR "/sub/delta.mp3", "delta path");
        CHECK_STR(tones[4].path, ND_TONES_USER_DIR "/Omega.mp3", "Omega path");
    }

    CHECK_INT(api.scan(NULL, 4u), 0, "NULL out");
    CHECK_INT(api.scan(tones, 0u), 0, "zero capacity");

    /* The cap is honoured rather than overrun. */
    CHECK_INT(api.scan(tones, 2u), 2, "a cap of two stops at two");

    free(tones);
}

/* ------------------------------------------------------------------ *
 * 6. TonePreviewPlayer
 * ------------------------------------------------------------------ */

static char g_bindir[ND_PATH_MAX];

/* The blocking command the stub below runs, found by absolute path.
 *
 * ============ THE STUB CANNOT USE THE $PATH IT IS TESTING ============
 *
 * test_preview cuts $PATH down to the single directory holding the stub, and
 * that narrowing is the assertion -- but the stub is a child and inherits it,
 * so a script whose body is `exec sleep 30` hands /bin/sh a name it has
 * nowhere left to look up. The shell exits 127 and the "preview" is a corpse
 * before it ever blocks.
 *
 * That is not hypothetical; it is what this stub did for as long as it
 * existed, under both its names. It went unseen because the app under test
 * ALSO treated a successful fork() as playback, so a dead child and a playing
 * one were indistinguishable -- the very blind spot that let `nice -n -10`
 * silence every preview on the handset for eight releases. 0.5.9a gave
 * nd_tones_preview_play() an execve probe, and the first corpse the probe
 * found was this one's.
 *
 * Resolving it HERE, while the real $PATH is still in place, is what makes
 * the stub independent of the environment the test then constructs. */
static const char *find_sleep(void)
{
    static const char *const dirs[] = {"/bin", "/usr/bin", "/usr/local/bin"};
    static char buf[ND_PATH_MAX];
    size_t i;

    for (i = 0u; i < sizeof dirs / sizeof dirs[0]; i++) {
        if (nd_snprintf(buf, sizeof buf, "%s/sleep", dirs[i]) == ND_OK && access(buf, X_OK) == 0)
            return buf;
    }
    return NULL;
}

/* A stub `mpv` that blocks, so a preview can be started and stopped without
 * a real mpv, without a sound card and without playing anything. MPV_CMD's
 * argv[0] is `mpv`, so this is the only program the spawn needs to find --
 * and, with $PATH cut down to this directory below, the only program it CAN
 * find. That is deliberate: it is what makes test_preview fail if argv[0]
 * ever goes back to being a wrapper. */
static bool make_stub_mpv(void)
{
    const char *sleeper = find_sleep();
    char path[ND_PATH_MAX];
    FILE *f;

    if (sleeper == NULL)
        return false;
    if (nd_snprintf(path, sizeof path, "%s/mpv", g_bindir) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fprintf(f, "#!/bin/sh\nexec '%s' 30\n", sleeper);
    (void)fclose(f);
    return chmod(path, 0755) == 0;
}

static void test_preview(void)
{
    char keep[ND_PATH_MAX];
    const char *saved_path = getenv("PATH");
    pid_t first;

    CHECK_INT(api.preview_pid(), -1, "nothing playing to start with");

    /* `if not path: return` -- and stopping nothing is not an error. */
    api.preview_play(NULL);
    CHECK_INT(api.preview_pid(), -1, "a NULL path plays nothing");
    api.preview_play("");
    CHECK_INT(api.preview_pid(), -1, "an empty path plays nothing");
    api.preview_stop();
    CHECK_INT(api.preview_pid(), -1, "stopping nothing is a no-op");

    (void)nd_strlcpy(keep, (saved_path != NULL) ? saved_path : "", sizeof keep);
    if (!make_stub_mpv()) {
        CHECK(false, "stub mpv");
        return;
    }
    /* $PATH is now exactly one directory holding exactly one program, `mpv`.
     * The spawn resolves argv[0] against it, so this check does not merely
     * say that SOMETHING started: it says the thing that started was the
     * player. Prefixing MPV_CMD with a wrapper again would leave which_exec()
     * with nothing to find and this line would fail. */
    (void)setenv("PATH", g_bindir, 1);

    api.preview_play(ND_TONES_SYSTEM_DIR "/Beta.mp3");
    first = api.preview_pid();
    CHECK(first > 0, "a preview process was started, and it is mpv itself");

    /* "self.stop()" is the FIRST line of play(): a second preview replaces
     * the first rather than playing over it. */
    api.preview_play(ND_TONES_SYSTEM_DIR "/alpha.MP3");
    CHECK(api.preview_pid() > 0, "the second preview started");
    CHECK(api.preview_pid() != first, "and it is a different process");

    api.preview_stop();
    CHECK_INT(api.preview_pid(), -1, "stop reaps it");

    /* app_shutdown() is what runs when the modem thread signals an incoming
     * call. If it did not stop the preview, mpv would hold the sound card
     * and the phone would ring silently -- nd_app.h's whole reason for
     * making the symbol mandatory. */
    api.preview_play(ND_TONES_SYSTEM_DIR "/Beta.mp3");
    CHECK(api.preview_pid() > 0, "a preview to tear down");
    api.shutdown();
    CHECK_INT(api.preview_pid(), -1, "app_shutdown() releases the sound card");

    (void)setenv("PATH", keep, 1);
}

/* ------------------------------------------------------------------ *
 * 7. The golden frame
 * ------------------------------------------------------------------ */

static void test_golden_frame(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* PagedList drains the channel before its first draw, so Back has to
     * arrive as a repeat rather than be queued. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Back on the first page returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the PagedList's first page");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "app-tones");

    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    char out[4];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    CHECK_STR(api.display_name(NULL, out, sizeof out), "", "display_name(NULL)");
    CHECK(api.display_name("x.mp3", NULL, 0u) == NULL, "display_name with no buffer");
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("Tones", "ndtones");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndtones-root", g_root, sizeof g_root) ||
        !sa_tmpdir("ndtones-bin", g_bindir, sizeof g_bindir)) {
        (void)dlclose(h);
        return 1;
    }

    /* Everything below /NeoDCT is this test's own. Set before the tree is
     * built, because build_tone_tree() writes through nd_path_resolve(). */
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);

    RUN(test_strings);
    RUN(test_is_supported);
    RUN(test_display_name);
    RUN(build_tone_tree);
    RUN(test_dirs);
    RUN(test_scan);
    RUN(test_preview);
    RUN(test_golden_frame);
    RUN(test_null_safety);

    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
    rc = sa_end(h, "test_tones");
    sa_rmtree(g_root);
    sa_rmtree(g_bindir);
    return rc;
}
