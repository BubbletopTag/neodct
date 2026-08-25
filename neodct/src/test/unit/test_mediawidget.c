/* test_mediawidget.c -- the mpv command line, the keymap, and the one
 * agreement that is easy to break.
 *
 * input.conf is what answers when the IPC bridge is not running, so it has
 * to mean the same thing as the bridge does. They are two files, edited by
 * different people for different reasons, and nothing but this test stops
 * them drifting -- which is exactly what the Python suite had a test for
 * (test_mediawidget_input_conf.py), and why it is here too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_media.h"
#include "nd_paths.h"
#include "nd_types.h"

static int g_fail;
static int g_checks;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            (void)fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,    \
                          (what));                                           \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want, what)                                           \
    do {                                                                     \
        const char *g_ = (got);                                              \
        const char *w_ = (want);                                             \
        g_checks++;                                                          \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                             \
            (void)fprintf(stderr, "FAIL %s:%d  %s: got '%s' want '%s'\n",    \
                          __FILE__, __LINE__, (what), g_ ? g_ : "(null)",    \
                          w_);                                               \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

/* ---------------------------------------------------------------- kind */

static void test_kind_for(void)
{
    CHECK(nd_media_kind_for("http://h/clip.avi") == ND_MEDIA_VIDEO, ".avi is video");
    CHECK(nd_media_kind_for("http://h/clip.MP4") == ND_MEDIA_VIDEO, "extension is case-insensitive");
    CHECK(nd_media_kind_for("http://h/pic.png") == ND_MEDIA_IMAGE, ".png is an image");
    CHECK(nd_media_kind_for("http://h/song.mp3") == ND_MEDIA_AUDIO, ".mp3 is audio");

    /* Query and fragment come off first, or every signed url would be a
     * video regardless of what it points at. */
    CHECK(nd_media_kind_for("http://h/pic.png?token=abc") == ND_MEDIA_IMAGE,
          "a query string does not hide the extension");
    CHECK(nd_media_kind_for("http://h/pic.png#top") == ND_MEDIA_IMAGE,
          "a fragment does not hide the extension");

    /* Guess video when there is nothing to go on: a stream url usually has
     * no extension and usually is one. */
    CHECK(nd_media_kind_for("http://h/stream") == ND_MEDIA_VIDEO, "no extension is video");
    CHECK(nd_media_kind_for("http://h/v1.2/stream") == ND_MEDIA_VIDEO,
          "a dotted directory is not an extension");
    CHECK(nd_media_kind_for(NULL) == ND_MEDIA_VIDEO, "NULL is video, not a crash");
}

/* ---------------------------------------------------------------- argv */

static bool argv_has(const nd_media_argv *a, const char *want)
{
    size_t i;

    for (i = 0u; i < a->n; i++) {
        if (strcmp(a->argv[i], want) == 0)
            return true;
    }
    return false;
}

static void test_build_argv(void)
{
    nd_media_argv a;
    size_t i;

    CHECK(nd_media_build_argv(&a, "http://h/clip.avi", ND_MEDIA_VIDEO, "/dev/fb0", NULL,
                              "/usr/bin/mpv", "/etc/input.conf") == ND_OK,
          "a video command line builds");

    CHECK_STR(a.argv[0], "/usr/bin/mpv", "argv[0] is mpv");
    CHECK(a.argv[a.n] == NULL, "argv is NULL terminated for execv");

    /* fbdev and never drm: on this board drm fails, and a failed video
     * output means mpv holds the screen showing nothing. */
    CHECK(argv_has(&a, "--vo=fbdev"), "--vo=fbdev");
    CHECK(!argv_has(&a, "--vo=drm"), "never --vo=drm");
    CHECK(argv_has(&a, "--fbdev-device=/dev/fb0"), "the framebuffer is named");
    CHECK(argv_has(&a, "--hwdec=no"), "no hardware decode to ask for");
    CHECK(argv_has(&a, "--no-config"), "the appliance reads no user config");
    CHECK(argv_has(&a, "--input-conf=/etc/input.conf"), "input.conf is named");
    CHECK(argv_has(&a, "--keep-open=no"), "return to the caller at the end");
    CHECK(argv_has(&a, "--demuxer-max-bytes=512KiB"), "the tiny demuxer cache");

    /* "--" before the url: a src attribute is attacker-controlled text and
     * mpv would read one starting with a dash as an option. */
    CHECK_STR(a.argv[a.n - 2u], "--", "-- immediately precedes the url");
    CHECK_STR(a.argv[a.n - 1u], "http://h/clip.avi", "the url is last");

    for (i = 0u; i < a.n; i++)
        CHECK(a.argv[i] != NULL, "no NULL inside argv");

    /* A url that begins with a dash still lands after the "--". */
    CHECK(nd_media_build_argv(&a, "-oh-no", ND_MEDIA_VIDEO, NULL, NULL, NULL, NULL) == ND_OK,
          "a dash-leading url builds");
    CHECK_STR(a.argv[a.n - 2u], "--", "-- still guards a hostile url");
    CHECK_STR(a.argv[a.n - 1u], "-oh-no", "the hostile url is the last word");

    /* No socket unless one was asked for. */
    CHECK(!argv_has(&a, "--input-ipc-server=/run/neodct/mpv.sock"), "no socket by default");
    CHECK(nd_media_build_argv(&a, "u.avi", ND_MEDIA_VIDEO, NULL, "/run/neodct/mpv.sock", NULL,
                              NULL) == ND_OK,
          "with a socket");
    CHECK(argv_has(&a, "--input-ipc-server=/run/neodct/mpv.sock"), "the socket is named");

    /* An image is a video that does not move; only the argv changes. */
    CHECK(nd_media_build_argv(&a, "p.png", ND_MEDIA_IMAGE, NULL, NULL, NULL, NULL) == ND_OK,
          "an image command line builds");
    CHECK(argv_has(&a, "--image-display-duration=inf"), "an image stays up");
    CHECK(argv_has(&a, "--audio=no"), "an image has no audio");
}

/* ------------------------------------------------------------- commands */

static void test_ipc_command(void)
{
    const char *const *c;
    char buf[256];
    size_t n;

    c = nd_media_ipc_command(14);
    CHECK(c != NULL, "C is bound");
    CHECK_STR(c[0], "quit", "C quits");
    CHECK(c[1] == NULL, "quit takes no argument");

    c = nd_media_ipc_command(28);
    CHECK_STR(c[0], "cycle", "navikey cycles");
    CHECK_STR(c[1], "pause", "...pause");

    /* 1..9 are 10%..90% and 0 is the start, the way every DVD player did
     * it. Keycode 2 is KEY_1 and 11 is KEY_0. */
    c = nd_media_ipc_command(2);
    CHECK_STR(c[1], "10", "1 seeks to 10%");
    c = nd_media_ipc_command(10);
    CHECK_STR(c[1], "90", "9 seeks to 90%");
    c = nd_media_ipc_command(11);
    CHECK_STR(c[1], "0", "0 seeks to the start");
    CHECK_STR(c[2], "absolute-percent", "...as a percentage");

    CHECK(nd_media_ipc_command(999) == NULL, "an unbound key means nothing");
    CHECK(nd_media_ipc_command(1) == NULL, "ESC is not bound");
    CHECK(nd_media_ipc_command(12) == NULL, "the digit range stops at 11");

    /* Compact JSON, newline terminated: the separators Python's json.dumps
     * was given, so the bytes on the socket are the same ones. */
    n = nd_media_encode_command(nd_media_ipc_command(14), buf, sizeof buf);
    CHECK(n > 0u, "quit encodes");
    CHECK_STR(buf, "{\"command\":[\"quit\"]}\n", "quit, on the wire");

    n = nd_media_encode_command(nd_media_ipc_command(105), buf, sizeof buf);
    CHECK(n > 0u, "seek encodes");
    CHECK_STR(buf, "{\"command\":[\"seek\",\"-10\",\"relative\"]}\n", "seek, on the wire");

    /* A buffer that cannot hold the request must produce nothing rather
     * than a truncated line: half a JSON object desynchronises the socket
     * for every command after it. */
    CHECK(nd_media_encode_command(nd_media_ipc_command(105), buf, 8u) == 0u,
          "a short buffer writes nothing at all");
}

/* --------------------------------------------- input.conf must agree */

/* What each NeoDCT keycode is called in mpv's own input layer. The names
 * are mpv's, not Linux's: SHARP because '#' starts a comment in input.conf,
 * BS because that is what mpv calls the key NeoDCT prints "C" on. */
static const struct {
    int32_t code;
    const char *mpv_name;
} MPV_KEY_NAMES[] = {
    {14, "BS"},   {28, "ENTER"}, {103, "UP"},  {108, "DOWN"}, {105, "LEFT"},
    {106, "RIGHT"}, {42, "*"},   {43, "SHARP"},
    {2, "1"}, {3, "2"}, {4, "3"}, {5, "4"}, {6, "5"},
    {7, "6"}, {8, "7"}, {9, "8"}, {10, "9"}, {11, "0"},
};

static void test_input_conf_matches_keymap(void)
{
    const char *path = getenv("NEODCT_INPUT_CONF");
    char line[256];
    FILE *f;
    size_t i;
    int found = 0;

    if (path == NULL)
        path = "../overlay/NeoDCT/System/core/MediaWidget/input.conf";
    f = fopen(path, "r");
    if (f == NULL) {
        (void)fprintf(stderr, "SKIP input.conf not readable at %s\n", path);
        return;
    }

    for (i = 0u; i < ND_ARRAY_LEN(MPV_KEY_NAMES); i++) {
        const char *const *cmd = nd_media_ipc_command(MPV_KEY_NAMES[i].code);
        char want[128];
        size_t used = 0u;
        size_t j;
        bool seen = false;

        CHECK(cmd != NULL, "every named key is in the bridge's keymap");
        if (cmd == NULL)
            continue;

        /* "<KEY> <word> <word>..." -- exactly how input.conf spells it. */
        used = (size_t)snprintf(want, sizeof want, "%s", MPV_KEY_NAMES[i].mpv_name);
        for (j = 0u; cmd[j] != NULL && used < sizeof want; j++)
            used += (size_t)snprintf(want + used, sizeof want - used, " %s", cmd[j]);

        rewind(f);
        while (fgets(line, (int)sizeof line, f) != NULL) {
            size_t n = strlen(line);

            while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r' ||
                              line[n - 1u] == ' '))
                line[--n] = '\0';
            if (strcmp(line, want) == 0) {
                seen = true;
                break;
            }
        }
        if (seen)
            found++;
        CHECK(seen, want);
    }

    (void)fclose(f);
    CHECK((size_t)found == ND_ARRAY_LEN(MPV_KEY_NAMES),
          "input.conf and the IPC keymap say the same thing for every key");
    (void)fprintf(stderr, "  input.conf agrees on %d of %zu keys\n", found,
                  ND_ARRAY_LEN(MPV_KEY_NAMES));
}

/* ------------------------------------- the browser's exact command line */

/* NetSurf builds precisely
 *
 *     neodct-play -- <url>
 *
 * (neodct_media_argv(), netsurf-neodct/.../neodct_media.c), and it passes
 * the "--" ON PURPOSE: a src is text off a web page, and one reading
 * "--parent 1" would otherwise be taken as options.
 *
 * This ran against a neodct-play that did not know "--", so every <video>
 * on the web answered "unknown option --" and the browser reported it
 * could not play anything. Python's argparse had handled it for free; the
 * hand-written C parser had to be told. That is what this test pins.
 *
 * It spawns the real binary rather than testing a parser in isolation,
 * because the bug was in main() and only an end-to-end call would have
 * caught it. */
static void test_browser_command_line(void)
{
    static const char *const HOSTILE[] = {
        "http://h/clip.avi",
        "--parent 1",  /* the exact shape the browser's "--" defends against */
        "--dry-run",   /* our own flag, as a url */
        "-",
    };
    char exe[ND_PATH_MAX];
    char self[ND_PATH_MAX];
    ssize_t got;
    char *slash;
    size_t i;

    /* build/<variant>/bin/neodct-play, found FROM THIS BINARY rather than
     * from the working directory.
     *
     * It used to be the literal "build/default/bin/neodct-play", and that is
     * wrong the moment there is more than one variant: `make ASAN=1 test`
     * runs build/asan/test/test_mediawidget with LD_LIBRARY_PATH pointing at
     * the ASan libneodct, and popen()ing the UNinstrumented neodct-play
     * against it fails to load -- so the child printed nothing and all four
     * hostile-argv checks failed for a reason that had nothing to do with
     * argv. nd_proc.c's apprun_path() has the same note for the same reason:
     * an ASan run has to drive the ASan binary.
     *
     * access(), NOT nd_path_exists(): the path layer resolves against
     * ND_ROOT, and this is a build artefact on the host, not a phone path.
     * Using the wrong one made this test skip in silence -- which is exactly
     * how a test for a shipped bug ends up proving nothing. */
    got = readlink("/proc/self/exe", self, sizeof self - 1u);
    if (got <= 0)
        return;
    self[got] = '\0';
    slash = strrchr(self, '/');
    if (slash == NULL)
        return;
    *slash = '\0';
    if (nd_snprintf(exe, sizeof exe, "%s/../bin/neodct-play", self) != ND_OK)
        return;
    if (access(exe, X_OK) != 0) {
        (void)fprintf(stderr, "SKIP browser command line: no %s\n", exe);
        return;
    }

    for (i = 0u; i < ND_ARRAY_LEN(HOSTILE); i++) {
        char cmd[ND_PATH_MAX + 256];
        char line[1024];
        FILE *p;
        bool saw_url = false;

        /* --dry-run so nothing is played and no process is suspended. */
        if (nd_snprintf(cmd, sizeof cmd, "%s --dry-run --no-suspend -- '%s' 2>&1", exe,
                        HOSTILE[i]) != ND_OK)
            continue;
        p = popen(cmd, "r");
        if (p == NULL) {
            CHECK(false, "popen neodct-play");
            continue;
        }
        while (fgets(line, (int)sizeof line, p) != NULL) {
            size_t n = strlen(line);

            while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r'))
                line[--n] = '\0';
            /* The url must be the LAST word, immediately after mpv's own
             * "--". Anything else means it was parsed as an option. */
            if (n > strlen(HOSTILE[i]) &&
                strcmp(line + n - strlen(HOSTILE[i]), HOSTILE[i]) == 0)
                saw_url = true;
            if (strstr(line, "unknown option") != NULL)
                CHECK(false, "neodct-play rejected the browser's argv");
        }
        (void)pclose(p);
        CHECK(saw_url, HOSTILE[i]);
    }
}

int main(void)
{
    test_kind_for();
    test_build_argv();
    test_ipc_command();
    test_input_conf_matches_keymap();
    test_browser_command_line();

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_mediawidget: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_mediawidget: %d checks passed\n", g_checks);
    return 0;
}
