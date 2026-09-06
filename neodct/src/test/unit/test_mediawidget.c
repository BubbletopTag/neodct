/* test_mediawidget.c -- the mpv command line, the keymap, and the one
 * agreement that is easy to break.
 *
 * input.conf is what answers when the IPC bridge is not running, so it has
 * to mean the same thing as the bridge does. They are two files, edited by
 * different people for different reasons, and nothing but this test stops
 * them drifting -- which is exactly what the Python suite had a test for
 * (test_mediawidget_input_conf.py), and why it is here too.
 */

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
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

    /* The loading ring needs the output to exist before the file does,
     * and a song must not take the screen for a ring it will never need. */
    CHECK(nd_media_build_argv(&a, "u.avi", ND_MEDIA_VIDEO, NULL, NULL, NULL, NULL) == ND_OK,
          "a video builds");
    CHECK(argv_has(&a, "--force-window=immediate"), "a video opens the screen at once");
    CHECK(argv_has(&a, "--cache-pause-initial=yes"), "a video buffers before it starts");
    CHECK(argv_has(&a, "--cache-pause-wait=2"), "...two seconds of it");
    CHECK(argv_has(&a, "--demuxer-max-back-bytes=256KiB"),
          "the backward cache is bounded (its default is 50 MiB)");
    CHECK(nd_media_build_argv(&a, "s.mp3", ND_MEDIA_AUDIO, NULL, NULL, NULL, NULL) == ND_OK,
          "a song builds");
    CHECK(!argv_has(&a, "--force-window=immediate"), "a song leaves the screen alone");
    CHECK(argv_has(&a, "--demuxer-max-back-bytes=256KiB"), "...but is bounded the same way");

    /* An image is a video that does not move; only the argv changes. */
    CHECK(nd_media_build_argv(&a, "p.png", ND_MEDIA_IMAGE, NULL, NULL, NULL, NULL) == ND_OK,
          "an image command line builds");
    CHECK(argv_has(&a, "--image-display-duration=inf"), "an image stays up");
    CHECK(argv_has(&a, "--audio=no"), "an image has no audio");
    CHECK(argv_has(&a, "--force-window=immediate"), "an image opens the screen at once");
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

/* ------------------------------------------------------- what mpv said */

static void test_outcome_feed(void)
{
    nd_media_outcome o;

    memset(&o, 0, sizeof o);

    /* ffmpeg's 404, as mpv relays it once error logging is requested */
    CHECK(nd_media_outcome_feed(&o, "{\"event\":\"log-message\",\"prefix\":\"ffmpeg\","
                                    "\"level\":\"error\",\"text\":\"https: HTTP error 404 "
                                    "Not Found\\n\"}"),
          "a 404 log line is understood");
    CHECK(o.not_found, "...as not found");
    CHECK(!o.unreachable, "...and not as no network");
    CHECK(!o.ended, "a log line does not end the file");

    /* the same line again changes nothing */
    CHECK(!nd_media_outcome_feed(&o, "{\"event\":\"log-message\",\"text\":\"HTTP error 404\"}"),
          "a repeat is not a change");

    /* the end of the file, with mpv's reason attached */
    CHECK(nd_media_outcome_feed(&o, "{\"event\":\"end-file\",\"reason\":\"error\","
                                    "\"playlist_entry_id\":1,\"file_error\":\"loading failed\"}"),
          "end-file is understood");
    CHECK(o.ended && o.error, "...as an error");
    CHECK(!o.format_error, "loading failed is not a format problem");

    /* the network never answered: ffmpeg's tcp.c wording, in a line whose
     * keys are in a different order, with a quote escaped in it */
    memset(&o, 0, sizeof o);
    CHECK(nd_media_outcome_feed(&o, "{\"text\":\"tcp: Failed to resolve hostname "
                                    "\\\"vidlii.com\\\": Name or service not known\\n\","
                                    "\"level\":\"error\",\"event\":\"log-message\"}"),
          "key order does not matter");
    CHECK(o.unreachable, "a resolver failure is no network");
    CHECK(!o.not_found, "...not a 404");
    memset(&o, 0, sizeof o);
    (void)nd_media_outcome_feed(&o, "{\"event\":\"log-message\",\"text\":\"Connection to "
                                    "tcp://h:80 failed: Connection timed out\\n\"}");
    CHECK(o.unreachable, "a timeout is no network");
    memset(&o, 0, sizeof o);
    (void)nd_media_outcome_feed(&o, "{\"event\":\"log-message\",\"text\":\"NETWORK IS "
                                    "UNREACHABLE\"}");
    CHECK(o.unreachable, "matching is case-insensitive");

    /* the bytes arrived and could not be decoded */
    memset(&o, 0, sizeof o);
    (void)nd_media_outcome_feed(&o, "{\"event\":\"end-file\",\"reason\":\"error\","
                                    "\"file_error\":\"unrecognized file format\"}");
    CHECK(o.error && o.format_error, "a demuxer refusal is a format problem");
    memset(&o, 0, sizeof o);
    (void)nd_media_outcome_feed(&o, "{\"event\":\"end-file\",\"reason\":\"error\","
                                    "\"file_error\":\"no audio or video data played\"}");
    CHECK(o.error && o.format_error, "every decoder refusing is a format problem");

    /* a normal end is an end and nothing more */
    memset(&o, 0, sizeof o);
    CHECK(nd_media_outcome_feed(&o, "{\"event\":\"end-file\",\"reason\":\"eof\","
                                    "\"playlist_entry_id\":1}"),
          "eof is understood");
    CHECK(o.ended && !o.error && !o.format_error, "...as a clean end");

    /* and a 5xx is the server's problem, not "not found" */
    memset(&o, 0, sizeof o);
    (void)nd_media_outcome_feed(&o, "{\"event\":\"log-message\",\"text\":\"https: HTTP error "
                                    "503 Service Unavailable\\n\"}");
    CHECK(!o.not_found && !o.unreachable, "a 503 is neither");

    /* things that are not ours: property changes, replies, garbage */
    memset(&o, 0, sizeof o);
    CHECK(!nd_media_outcome_feed(&o, "{\"event\":\"property-change\",\"name\":\"pause\"}"),
          "another event is ignored");
    CHECK(!nd_media_outcome_feed(&o, "{\"request_id\":0,\"error\":\"success\"}"),
          "a command reply is ignored");
    CHECK(!nd_media_outcome_feed(&o, ""), "an empty line is ignored");
    CHECK(!nd_media_outcome_feed(&o, "not json at all {{{\"event\":\""), "garbage is ignored");
    CHECK(!nd_media_outcome_feed(&o, NULL), "NULL is ignored");
    CHECK(!nd_media_outcome_feed(NULL, "{\"event\":\"end-file\"}"), "no outcome, no crash");
    CHECK(!o.ended && !o.error && !o.not_found && !o.unreachable, "nothing stuck");

    /* a text longer than the buffer is truncated, not overrun */
    {
        char line[2048];
        size_t n = (size_t)snprintf(line, sizeof line, "{\"event\":\"log-message\",\"text\":\"");
        size_t i;

        for (i = n; i + 3u < sizeof line; i++)
            line[i] = 'x';
        line[i++] = '"';
        line[i++] = '}';
        line[i] = '\0';
        memset(&o, 0, sizeof o);
        CHECK(!nd_media_outcome_feed(&o, line), "a huge line is harmless");
    }
}

static void test_outcome_feed_text(void)
{
    nd_media_outcome o;

    /* mpv's terminal log, as it comes off the pipe */
    memset(&o, 0, sizeof o);
    CHECK(nd_media_outcome_feed_text(&o, "[ffmpeg] https: HTTP error 404 Not Found"),
          "a 404 line is understood");
    CHECK(o.not_found, "...as not found");
    memset(&o, 0, sizeof o);
    CHECK(nd_media_outcome_feed_text(&o, "[ffmpeg] tcp: Connection to tcp://h:1 failed: "
                                         "Connection refused"),
          "a refusal is understood");
    CHECK(o.unreachable, "...as no network");
    memset(&o, 0, sizeof o);
    CHECK(nd_media_outcome_feed_text(&o, "Failed to recognize file format."),
          "mpv's own demuxer failure is understood");
    CHECK(o.format_error, "...as a format problem");
    memset(&o, 0, sizeof o);
    (void)nd_media_outcome_feed_text(&o, "[cplayer] No video or audio streams selected.");
    CHECK(o.format_error, "every decoder refusing is a format problem");

    /* the lines that are nothing to us */
    memset(&o, 0, sizeof o);
    CHECK(!nd_media_outcome_feed_text(&o, " (+) Video --vid=1 (h264 320x180 15.000fps)"),
          "a track line is ignored");
    CHECK(!nd_media_outcome_feed_text(&o, "Failed to open http://h/x.mp4."),
          "mpv's generic failure line says nothing specific");
    CHECK(!nd_media_outcome_feed_text(&o, ""), "an empty line is ignored");
    CHECK(!nd_media_outcome_feed_text(&o, NULL), "NULL is ignored");
    CHECK(!nd_media_outcome_feed_text(NULL, "HTTP error 404"), "no outcome, no crash");
    CHECK(!o.not_found && !o.unreachable && !o.format_error, "nothing stuck");
}

static void test_exit_status(void)
{
    nd_media_outcome o;

    memset(&o, 0, sizeof o);

    /* mpv's own verdicts pass through when it has nothing to add */
    CHECK(nd_media_exit_status(&o, true, 0) == ND_MEDIA_EXIT_OK, "0 is played");
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_FAILED, "a bare 2 stays a 2");
    CHECK(nd_media_exit_status(&o, true, 1) == 1, "a 1 stays a 1");
    CHECK(nd_media_exit_status(&o, true, ND_MEDIA_ENOENT) == ND_MEDIA_ENOENT,
          "no mpv stays no mpv");
    CHECK(nd_media_exit_status(NULL, true, 2) == ND_MEDIA_EXIT_FAILED, "no outcome, no crash");

    /* killed is its own word: the OOM killer is a real way for mpv to end
     * on this phone, and "No media player" for it sent people looking for
     * a missing binary */
    CHECK(nd_media_exit_status(&o, false, 0) == ND_MEDIA_EXIT_DIED, "a signal death is DIED");

    /* a 2 with a reason becomes the reason, most specific first */
    o.error = true;
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_NOLOAD, "an error alone is NOLOAD");
    o.unreachable = true;
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_NONET, "no network is NONET");
    o.not_found = true;
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_NOTFOUND,
          "a 4xx beats a network complaint");
    o.format_error = true;
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_FORMAT, "a format error beats both");

    /* ...but never a 0: if it played, it played */
    CHECK(nd_media_exit_status(&o, true, 0) == ND_MEDIA_EXIT_OK, "a played file is played");
    /* a reason seen on stderr refines a 2 even when the end-file event
     * was missed (a fast failure, or a socket that never connected)... */
    memset(&o, 0, sizeof o);
    o.not_found = true;
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_NOTFOUND,
          "a 404 on stderr refines a bare 2");
    /* ...but not a 1: mpv failing to start is not about the url */
    CHECK(nd_media_exit_status(&o, true, 1) == 1, "a 404 on stderr does not touch a 1");
    /* and a 2 with nothing learned stays a 2 */
    memset(&o, 0, sizeof o);
    CHECK(nd_media_exit_status(&o, true, 2) == ND_MEDIA_EXIT_FAILED, "nothing learned: 2");

    /* every status has a word for the log, and the words differ */
    {
        static const int codes[] = {ND_MEDIA_EXIT_OK, ND_MEDIA_EXIT_FAILED, ND_MEDIA_EXIT_NOLOAD,
                                    ND_MEDIA_EXIT_NONET, ND_MEDIA_EXIT_NOTFOUND,
                                    ND_MEDIA_EXIT_FORMAT, ND_MEDIA_EXIT_DIED, ND_MEDIA_ENOENT};
        size_t i, j;

        for (i = 0u; i < ND_ARRAY_LEN(codes); i++) {
            CHECK(nd_media_exit_name(codes[i]) != NULL, "a name for every status");
            for (j = i + 1u; j < ND_ARRAY_LEN(codes); j++)
                CHECK(strcmp(nd_media_exit_name(codes[i]), nd_media_exit_name(codes[j])) != 0,
                      "no two statuses share a name");
        }
        CHECK(nd_media_exit_name(99) != NULL, "an unknown status still has a name");
    }
}

static void test_socket_path(void)
{
    char path[ND_PATH_MAX];
    char want[ND_PATH_MAX];

    CHECK(nd_media_ipc_socket_path(path, sizeof path) == ND_OK, "the socket path builds");
    (void)snprintf(want, sizeof want, "/tmp/neodct-mpv.%ld.sock", (long)getpid());
    CHECK_STR(path, want, "under /tmp, named by pid");
    /* /tmp and not /run: the browser runs as ndusr_ut and cannot create
     * anything under /run; a socket that cannot be bound is a keypad that
     * does nothing, C included */
    CHECK(strncmp(path, "/run/", 5) != 0, "never under /run");
    CHECK(nd_media_ipc_socket_path(path, 8u) != ND_OK, "too small is refused, not truncated");
    CHECK(nd_media_ipc_socket_path(NULL, sizeof path) != ND_OK, "NULL is refused");
}

/* ------------------------------------------- the browser's side of it */

/* The browser fork carries the same exit-status table under other names
 * and turns each into a status-bar line. It is a separately built program
 * in another tree, so nothing but this keeps the two in step. */
static void test_browser_exit_table(void)
{
    static const struct {
        const char *name;
        int value;
    } TABLE[] = {
        {"NEODCT_MEDIA_EXIT_OK", ND_MEDIA_EXIT_OK},
        {"NEODCT_MEDIA_EXIT_FAILED", ND_MEDIA_EXIT_FAILED},
        {"NEODCT_MEDIA_EXIT_NOLOAD", ND_MEDIA_EXIT_NOLOAD},
        {"NEODCT_MEDIA_EXIT_NONET", ND_MEDIA_EXIT_NONET},
        {"NEODCT_MEDIA_EXIT_NOTFOUND", ND_MEDIA_EXIT_NOTFOUND},
        {"NEODCT_MEDIA_EXIT_FORMAT", ND_MEDIA_EXIT_FORMAT},
        {"NEODCT_MEDIA_EXIT_DIED", ND_MEDIA_EXIT_DIED},
        {"NEODCT_MEDIA_EXIT_NOPLAYER", ND_MEDIA_ENOENT},
    };
    const char *path = getenv("NEODCT_BROWSER_MEDIA_H");
    char line[512];
    FILE *f;
    size_t i;
    int found = 0;

    if (path == NULL)
        path = "../../netsurf-neodct/netsurf/frontends/framebuffer/neodct/neodct_media.h";
    f = fopen(path, "r");
    if (f == NULL) {
        (void)fprintf(stderr, "SKIP browser header not readable at %s\n", path);
        return;
    }

    for (i = 0u; i < ND_ARRAY_LEN(TABLE); i++) {
        bool seen = false;

        rewind(f);
        while (fgets(line, (int)sizeof line, f) != NULL) {
            char name[64];
            int value;

            if (sscanf(line, "#define %63s %d", name, &value) != 2)
                continue;
            if (strcmp(name, TABLE[i].name) != 0)
                continue;
            seen = true;
            CHECK(value == TABLE[i].value, TABLE[i].name);
            break;
        }
        if (seen)
            found++;
        CHECK(seen, TABLE[i].name);
    }
    (void)fclose(f);
    (void)fprintf(stderr, "  browser header agrees on %d of %zu exit statuses\n", found,
                  ND_ARRAY_LEN(TABLE));
}

/* ---------------------------------------------- the bridge, end to end */

/* A stand-in for mpv that speaks the real IPC protocol over a real unix
 * socket: this binary, re-executed with ND_FAKE_MPV set. Nothing about the
 * bridge is mocked, because the part most likely to be wrong is the
 * handshake with a process that has not created its socket yet, and a
 * mock proves nothing about that.
 *
 *   ND_FAKE_MPV_SAY       lines to send once connected, '|' separated
 *   ND_FAKE_MPV_RC        exit status (default 0)
 *   ND_FAKE_MPV_LOG       file every received command is appended to
 *   ND_FAKE_MPV_NOSOCK    never create the socket: exit at once with RC
 *   ND_FAKE_MPV_KILL      die of SIGKILL instead of exiting
 *   ND_FAKE_MPV_LINGER_MS how long to keep reading before exiting (300) */
static int fake_mpv(int argc, char **argv)
{
    const char *sock_path = NULL;
    const char *say = getenv("ND_FAKE_MPV_SAY");
    const char *logp = getenv("ND_FAKE_MPV_LOG");
    const char *rc_s = getenv("ND_FAKE_MPV_RC");
    const char *linger_s = getenv("ND_FAKE_MPV_LINGER_MS");
    const char *err_text = getenv("ND_FAKE_MPV_STDERR");
    int rc = rc_s != NULL ? atoi(rc_s) : 0;
    int linger = linger_s != NULL ? atoi(linger_s) : 300;
    struct sockaddr_un addr;
    int srv;
    int conn = -1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--input-ipc-server=", 19) == 0)
            sock_path = argv[i] + 19;
    }
    /* What mpv prints on its way to giving up: before the socket, as a
     * fast failure really does -- and on STDOUT, as mpv really does (its
     * terminal log is stdout; only the status line is stderr). One line on
     * each, so a bridge that listens to only one of them fails here. */
    if (err_text != NULL && err_text[0] != '\0') {
        (void)fprintf(stdout, "%s\n", err_text);
        (void)fflush(stdout);
        (void)fprintf(stderr, "[cplayer] (the stderr half of a fake mpv)\n");
        (void)fflush(stderr);
    }
    if (getenv("ND_FAKE_MPV_NOSOCK") != NULL || sock_path == NULL)
        return rc;

    srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0)
        return 99;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 1) != 0)
        return 98;

    {
        struct pollfd pfd = {srv, POLLIN, 0};

        if (poll(&pfd, 1u, 3000) <= 0)
            return 97; /* the bridge never connected */
        conn = accept(srv, NULL, NULL);
        if (conn < 0)
            return 96;
    }

    if (say != NULL && say[0] != '\0') {
        char buf[1024];
        size_t j;
        size_t n = 0u;

        for (j = 0u; say[j] != '\0' && n + 1u < sizeof buf; j++)
            buf[n++] = say[j] == '|' ? '\n' : say[j];
        buf[n++] = '\n';
        (void)send(conn, buf, n, MSG_NOSIGNAL);
    }

    {
        double deadline;
        struct timespec t;
        bool quit = false;

        (void)clock_gettime(CLOCK_MONOTONIC, &t);
        deadline = (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6 + (double)linger;
        while (!quit) {
            struct pollfd pfd = {conn, POLLIN, 0};
            char chunk[512];
            ssize_t got;
            double now;

            (void)clock_gettime(CLOCK_MONOTONIC, &t);
            now = (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
            if (now >= deadline)
                break;
            if (poll(&pfd, 1u, (int)(deadline - now)) <= 0)
                continue;
            got = recv(conn, chunk, sizeof chunk - 1u, 0);
            if (got <= 0)
                break;
            chunk[got] = '\0';
            if (logp != NULL) {
                FILE *lf = fopen(logp, "a");

                if (lf != NULL) {
                    (void)fputs(chunk, lf);
                    (void)fclose(lf);
                }
            }
            if (strstr(chunk, "\"quit\"") != NULL)
                quit = true;
        }
    }

    (void)close(conn);
    (void)close(srv);
    (void)unlink(sock_path);
    if (getenv("ND_FAKE_MPV_KILL") != NULL)
        (void)raise(SIGKILL);
    return rc;
}

static bool self_path(char *out, size_t out_sz)
{
    ssize_t got = readlink("/proc/self/exe", out, out_sz - 1u);

    if (got <= 0)
        return false;
    out[got] = '\0';
    return true;
}

static bool file_contains(const char *path, const char *needle)
{
    char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;

    if (f == NULL)
        return false;
    n = fread(buf, 1u, sizeof buf - 1u, f);
    (void)fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int run_fake(const char *self, const char *say, const char *rc, const char *log_path,
                    bool nosock, bool kill_it, int keypad_fd, const char *sock)
{
    int status;

    (void)setenv("ND_FAKE_MPV", "1", 1);
    (void)unsetenv("ND_FAKE_MPV_STDERR");
    if (say != NULL)
        (void)setenv("ND_FAKE_MPV_SAY", say, 1);
    else
        (void)unsetenv("ND_FAKE_MPV_SAY");
    (void)setenv("ND_FAKE_MPV_RC", rc, 1);
    if (log_path != NULL)
        (void)setenv("ND_FAKE_MPV_LOG", log_path, 1);
    else
        (void)unsetenv("ND_FAKE_MPV_LOG");
    if (nosock)
        (void)setenv("ND_FAKE_MPV_NOSOCK", "1", 1);
    else
        (void)unsetenv("ND_FAKE_MPV_NOSOCK");
    if (kill_it)
        (void)setenv("ND_FAKE_MPV_KILL", "1", 1);
    else
        (void)unsetenv("ND_FAKE_MPV_KILL");

    status = nd_media_play("http://h/clip.avi", ND_MEDIA_VIDEO, 0, keypad_fd, self, "/dev/fb0",
                           sock, "/etc/input.conf");

    (void)unsetenv("ND_FAKE_MPV");
    return status;
}

static void test_play_against_a_fake_mpv(void)
{
    char self[ND_PATH_MAX];
    char sock[ND_PATH_MAX];
    char log_path[ND_PATH_MAX];
    const char *tmp = getenv("TMPDIR");

    if (!self_path(self, sizeof self)) {
        (void)fprintf(stderr, "SKIP fake mpv: no /proc/self/exe\n");
        return;
    }
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    (void)snprintf(sock, sizeof sock, "%s/nd-test-mpv.%ld.sock", tmp, (long)getpid());
    (void)snprintf(log_path, sizeof log_path, "%s/nd-test-mpv.%ld.log", tmp, (long)getpid());
    (void)unlink(log_path);

    /* No keypad at all, and the socket is still there: a 404 becomes
     * "not found" only because the bridge listened. */
    CHECK(run_fake(self,
                   "{\"event\":\"log-message\",\"prefix\":\"ffmpeg\",\"level\":\"error\","
                   "\"text\":\"https: HTTP error 404 Not Found\\n\"}|"
                   "{\"event\":\"end-file\",\"reason\":\"error\",\"playlist_entry_id\":1,"
                   "\"file_error\":\"loading failed\"}",
                   "2", log_path, false, false, -1, sock) == ND_MEDIA_EXIT_NOTFOUND,
          "a 404 over the socket is NOTFOUND, with no keypad to forward");
    CHECK(file_contains(log_path, "\"request_log_messages\""),
          "the bridge asked mpv for its error log");

    /* the network never answered */
    CHECK(run_fake(self,
                   "{\"event\":\"log-message\",\"level\":\"error\",\"text\":\"tcp: Failed to "
                   "resolve hostname h: Name or service not known\\n\"}|"
                   "{\"event\":\"end-file\",\"reason\":\"error\",\"file_error\":\"loading failed\"}",
                   "2", NULL, false, false, -1, sock) == ND_MEDIA_EXIT_NONET,
          "a resolver failure is NONET");

    /* the bytes arrived and meant nothing to this build */
    CHECK(run_fake(self,
                   "{\"event\":\"end-file\",\"reason\":\"error\",\"file_error\":\"unrecognized "
                   "file format\"}",
                   "2", NULL, false, false, -1, sock) == ND_MEDIA_EXIT_FORMAT,
          "an unrecognised format is FORMAT");

    /* it played */
    CHECK(run_fake(self, "{\"event\":\"end-file\",\"reason\":\"eof\"}", "0", NULL, false, false,
                   -1, sock) == ND_MEDIA_EXIT_OK,
          "a clean end is OK");

    /* mpv that never bound its socket: nothing to refine a 2 with */
    CHECK(run_fake(self, NULL, "2", NULL, true, false, -1, sock) == ND_MEDIA_EXIT_FAILED,
          "no socket, no reason: a bare 2");

    /* no mpv at all */
    CHECK(nd_media_play("u.avi", ND_MEDIA_VIDEO, 0, -1, "/nonexistent/mpv", NULL, sock, NULL) ==
              ND_MEDIA_ENOENT,
          "a missing mpv is ENOENT");

    /* killed */
    CHECK(run_fake(self, NULL, "0", NULL, false, true, -1, sock) == ND_MEDIA_EXIT_DIED,
          "an mpv that died of a signal is DIED");

    /* the reason on stderr, and NO socket at all: what a 404 from a fast
     * server looks like, and what the socket route alone got wrong */
    (void)setenv("ND_FAKE_MPV_STDERR", "[ffmpeg] http: HTTP error 404 Not Found", 1);
    (void)setenv("ND_FAKE_MPV", "1", 1);
    (void)setenv("ND_FAKE_MPV_RC", "2", 1);
    (void)setenv("ND_FAKE_MPV_NOSOCK", "1", 1);
    (void)unsetenv("ND_FAKE_MPV_SAY");
    (void)unsetenv("ND_FAKE_MPV_LOG");
    (void)unsetenv("ND_FAKE_MPV_KILL");
    CHECK(nd_media_play("http://h/clip.avi", ND_MEDIA_VIDEO, 0, -1, self, "/dev/fb0", sock,
                        "/etc/input.conf") == ND_MEDIA_EXIT_NOTFOUND,
          "a 404 on stderr is NOTFOUND even with no socket");
    (void)setenv("ND_FAKE_MPV_STDERR", "Failed to recognize file format.", 1);
    (void)unsetenv("ND_FAKE_MPV_NOSOCK");
    (void)setenv("ND_FAKE_MPV_SAY",
                 "{\"event\":\"end-file\",\"reason\":\"error\",\"file_error\":\"loading failed\"}",
                 1);
    CHECK(nd_media_play("http://h/clip.avi", ND_MEDIA_VIDEO, 0, -1, self, "/dev/fb0", sock,
                        "/etc/input.conf") == ND_MEDIA_EXIT_FORMAT,
          "stderr's format complaint beats the socket's vague loading failed");
    (void)unsetenv("ND_FAKE_MPV_STDERR");
    (void)unsetenv("ND_FAKE_MPV");

    /* and the keys, forwarded from a pipe standing in for the evdev node:
     * C becomes quit, and mpv leaves because of it rather than its timer */
    {
        int pfd[2];
        struct input_event ev[2];

        if (pipe(pfd) == 0) {
            memset(ev, 0, sizeof ev);
            ev[0].type = EV_KEY;
            ev[0].code = 14; /* KEY_BACKSPACE: the key NeoDCT prints C on */
            ev[0].value = 1;
            ev[1].type = EV_SYN;
            CHECK(write(pfd[1], ev, sizeof ev) == (ssize_t)sizeof ev, "the press is queued");
            (void)unlink(log_path);
            (void)setenv("ND_FAKE_MPV_LINGER_MS", "4000", 1);
            CHECK(run_fake(self, NULL, "0", log_path, false, false, pfd[0], sock) ==
                      ND_MEDIA_EXIT_OK,
                  "mpv quit on the forwarded C");
            (void)unsetenv("ND_FAKE_MPV_LINGER_MS");
            CHECK(file_contains(log_path, "{\"command\":[\"quit\"]}"),
                  "...because it received quit over the socket");
            (void)close(pfd[0]);
            (void)close(pfd[1]);
        }
    }

    (void)unlink(log_path);
    (void)unlink(sock);
}

/* ---------------------------------- where neodct-play finds the keypad */

/* ============ THE PLAYER USED TO GO LOOKING, AND FIND NOTHING ============
 *
 * nd_media_discover_keypad() scans /dev/input for a device named
 * "neodct-t9-keypad", then anything called "keypad", then anything called
 * "keyboard". On a development box the third branch matches the real virtio
 * keyboard and mpv has keys, which is why every automated run of this player
 * has always worked.
 *
 * On the phone /dev/input is EMPTY -- the keypad is a PCF8575 matrix the core
 * scans over i2c, and a matrix is not an input device -- so the only node
 * that could ever match was a uinput device the Browser app created. The
 * browser became ndusr_ut, ndusr_ut may not open /dev/uinput, and from that
 * day mpv had no keys at all on hardware: a full-screen player that ignored
 * C, with the browser SIGSTOPped underneath it, which is a phone the owner
 * has to take the battery out of.
 *
 * The core makes the device now and names it in NEODCT_KEY_EVDEV. This pins
 * that neodct-play ASKS before it searches, and that it says out loud which
 * answer it got -- the "keypad: none" line used to be printed under
 * --dry-run only, so a real run with no keypad logged nothing whatsoever. */
static void test_keypad_comes_from_the_environment(void)
{
    char exe[ND_PATH_MAX];
    char self[ND_PATH_MAX];
    char node[ND_PATH_MAX];
    char cmd[ND_PATH_MAX * 3];
    char line[1024];
    ssize_t got;
    char *slash;
    FILE *p;
    int fd;
    bool saw_node = false;

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
        (void)fprintf(stderr, "SKIP keypad from the environment: no %s\n", exe);
        return;
    }

    /* An ordinary readable file stands in for the evdev node: neodct-play
     * only open()s it and hands the descriptor on, so a real character device
     * -- which no host test may create -- buys nothing here. */
    if (nd_snprintf(node, sizeof node, "%s/../keydev-fixture", self) != ND_OK)
        return;
    fd = open(node, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        (void)fprintf(stderr, "SKIP keypad from the environment: cannot stage %s\n", node);
        return;
    }
    (void)close(fd);

    if (nd_snprintf(cmd, sizeof cmd, "NEODCT_KEY_EVDEV='%s' %s --dry-run --no-suspend -- "
                                     "http://h/clip.avi 2>&1",
                    node, exe) != ND_OK) {
        (void)unlink(node);
        return;
    }
    p = popen(cmd, "r");
    if (p == NULL) {
        CHECK(false, "popen neodct-play");
        (void)unlink(node);
        return;
    }
    while (fgets(line, (int)sizeof line, p) != NULL) {
        if (strncmp(line, "keypad: ", 8u) == 0 && strstr(line, node) != NULL)
            saw_node = true;
    }
    (void)pclose(p);
    CHECK(saw_node, "neodct-play uses the node the core named");

    /* And with nothing named and nothing to discover, it SAYS SO on stderr
     * rather than playing in silence. NEODCT_KEYPAD_DEVICE is cleared too:
     * an inherited one from the surrounding shell would win. */
    if (nd_snprintf(cmd, sizeof cmd,
                    "NEODCT_KEY_EVDEV= NEODCT_KEYPAD_DEVICE= %s --no-suspend --device '' -- "
                    "/nonexistent-neodct-clip.avi 2>&1",
                    exe) == ND_OK) {
        bool saw_complaint = false;

        p = popen(cmd, "r");
        if (p != NULL) {
            while (fgets(line, (int)sizeof line, p) != NULL) {
                if (strstr(line, "NO KEYPAD FOUND") != NULL)
                    saw_complaint = true;
            }
            (void)pclose(p);
            /* Only assert the complaint on a machine that really has no
             * keyboard-shaped evdev node; a developer's box has one and
             * discovery is then RIGHT to find it. */
            if (nd_media_discover_keypad(line, sizeof line) != ND_OK)
                CHECK(saw_complaint, "a keypadless run says so on stderr");
        }
    }
    (void)unlink(node);
}

int main(int argc, char **argv)
{
    if (getenv("ND_FAKE_MPV") != NULL)
        return fake_mpv(argc, argv);

    test_kind_for();
    test_build_argv();
    test_ipc_command();
    test_input_conf_matches_keymap();
    test_browser_command_line();
    test_keypad_comes_from_the_environment();
    test_outcome_feed();
    test_outcome_feed_text();
    test_exit_status();
    test_socket_path();
    test_browser_exit_table();
    test_play_against_a_fake_mpv();

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_mediawidget: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_mediawidget: %d checks passed\n", g_checks);
    return 0;
}
