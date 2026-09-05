/* nd_media.c -- see nd_media.h. */

#include "nd_media.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"

/* ------------------------------------------------------------------ *
 * What kind of thing a url is
 * ------------------------------------------------------------------ */

static const char *const IMAGE_EXT[] = {".jpg", ".jpeg", ".png", ".gif",
                                        ".bmp", ".webp", NULL};
static const char *const AUDIO_EXT[] = {".mp3", ".wav", ".m4a", ".ogg",
                                        ".flac", ".aac", ".amr", NULL};

static bool ext_is(const char *ext, const char *const *table)
{
    size_t i;

    for (i = 0u; table[i] != NULL; i++) {
        if (strcasecmp(ext, table[i]) == 0)
            return true;
    }
    return false;
}

nd_media_kind nd_media_kind_for(const char *url)
{
    const char *end;
    const char *dot;
    const char *slash;
    const char *p;
    char ext[16];
    size_t n;

    if (url == NULL)
        return ND_MEDIA_VIDEO;

    /* Query and fragment first: a .mp4?token=... is still an mp4, and the
     * Python cuts on '?' then '#' in that order. */
    end = url + strlen(url);
    for (p = url; p < end; p++) {
        if (*p == '?' || *p == '#') {
            end = p;
            break;
        }
    }

    /* The extension is the last dot AFTER the last slash: a dotted
     * directory name is not an extension. os.path.splitext() gets this
     * right by only ever looking at the basename. */
    slash = NULL;
    for (p = url; p < end; p++) {
        if (*p == '/')
            slash = p;
    }
    dot = NULL;
    for (p = (slash != NULL ? slash + 1 : url); p < end; p++) {
        if (*p == '.')
            dot = p;
    }
    if (dot == NULL)
        return ND_MEDIA_VIDEO;

    n = (size_t)(end - dot);
    if (n >= sizeof ext)
        return ND_MEDIA_VIDEO;
    memcpy(ext, dot, n);
    ext[n] = '\0';

    if (ext_is(ext, IMAGE_EXT))
        return ND_MEDIA_IMAGE;
    if (ext_is(ext, AUDIO_EXT))
        return ND_MEDIA_AUDIO;
    /* Everything else, including no extension at all, is a video. A stream
     * url usually has no extension and is usually a video. */
    return ND_MEDIA_VIDEO;
}

/* ------------------------------------------------------------------ *
 * The mpv command line
 * ------------------------------------------------------------------ */

static nd_err argv_push(nd_media_argv *a, const char *s)
{
    if (a->n + 1u >= ND_MEDIA_ARGV_MAX) /* +1: room for the NULL */
        return ND_ERR_TOOLONG;
    a->argv[a->n++] = s;
    return ND_OK;
}

/* Push "<prefix><value>", copied into the struct's own storage so the argv
 * entry outlives any caller buffer. */
static nd_err argv_pushf(nd_media_argv *a, const char *prefix, const char *value)
{
    char *slot;

    if (a->n_storage >= ND_ARRAY_LEN(a->storage))
        return ND_ERR_TOOLONG;
    slot = a->storage[a->n_storage];
    if (nd_snprintf(slot, sizeof a->storage[0], "%s%s", prefix, value) != ND_OK)
        return ND_ERR_TOOLONG;
    a->n_storage++;
    return argv_push(a, slot);
}

nd_err nd_media_build_argv(nd_media_argv *out, const char *url, nd_media_kind kind,
                           const char *fbdev, const char *ipc_socket, const char *mpv,
                           const char *input_conf)
{
    nd_err rc;

    if (out == NULL || url == NULL)
        return ND_ERR_INVAL;
    memset(out, 0, sizeof *out);

    if (fbdev == NULL)
        fbdev = ND_MEDIA_FBDEV;
    if (mpv == NULL)
        mpv = ND_MPV_BIN;
    if (input_conf == NULL)
        input_conf = ND_MEDIA_INPUT_CONF;

    rc = argv_pushf(out, "", mpv);
    if (rc == ND_OK)
        rc = argv_push(out, "--no-config");
    if (rc == ND_OK)
        rc = argv_pushf(out, "--input-conf=", input_conf);

    /* The whole point: software decode straight into /dev/fb0. */
    if (rc == ND_OK)
        rc = argv_push(out, "--vo=fbdev");
    if (rc == ND_OK)
        rc = argv_pushf(out, "--fbdev-device=", fbdev);
    if (rc == ND_OK)
        rc = argv_push(out, "--hwdec=no");
    if (rc == ND_OK)
        rc = argv_push(out, "--vd-lavc-threads=1");
    if (rc == ND_OK)
        rc = argv_push(out, "--ao=alsa");

    /* Subtitle auto-loading stats the whole directory for every file
     * played, off a card mounted over SPI. */
    if (rc == ND_OK)
        rc = argv_push(out, "--sub-auto=no");

    /* Nothing here turns off the OSC, scripts or ytdl. They are all parts
     * of mpv's Lua layer, this build has no Lua in it, and the options that
     * control them therefore do not exist -- mpv exits on an unrecognised
     * option rather than warning about it. */

    /* Return to the caller at the end of the file instead of parking on a
     * black screen waiting for input. */
    if (rc == ND_OK)
        rc = argv_push(out, "--keep-open=no");
    if (rc == ND_OK)
        rc = argv_push(out, "--idle=no");

    /* The demuxer cache, kept deliberately tiny. Measured on the device: at
     * 4MiB mpv is 19.4 MB RSS with 7.8 MB dirty; at 512KiB it is 16.3 MB
     * with 5.4 MB dirty, and the same clip plays at the same speed. Dirty
     * pages are the ones that matter, because they can only go to zram --
     * and compressing them costs the single core the decoder is already
     * using. A bigger cache buys nothing here: the phone either has the
     * bandwidth to keep up or it does not, and no amount of buffering fixes
     * the second case. */
    if (rc == ND_OK)
        rc = argv_push(out, "--cache=yes");
    if (rc == ND_OK)
        rc = argv_push(out, "--demuxer-max-bytes=512KiB");
    if (rc == ND_OK)
        rc = argv_push(out, "--demuxer-readahead-secs=1");

    /* The BACKWARD cache is a separate budget and its default is 50 MiB.
     * It holds packets already played so that a seek back can be served
     * without refetching, and with --cache=yes it fills for as long as the
     * file runs: a 20-minute clip at 300 kbit/s is 45 MB of it, on a phone
     * with 64. The forward limit above never touched it. 256 KiB keeps a
     * few seconds behind the play position for the LEFT key and bounds the
     * whole cache at three quarters of a megabyte, however long the file. */
    if (rc == ND_OK)
        rc = argv_push(out, "--demuxer-max-back-bytes=256KiB");

    /* Buffer before starting, and buffer a little before resuming. mpv's
     * default is to start the moment one packet is in and pause a second
     * later when the next one is not; over a mobile link that is a stutter
     * on every start and on every stall. Two seconds of readahead first is
     * a slightly longer ring and a much smoother first minute, and it costs
     * no memory the cache above was not already allowed. */
    if (rc == ND_OK)
        rc = argv_push(out, "--cache-pause-initial=yes");
    if (rc == ND_OK)
        rc = argv_push(out, "--cache-pause-wait=2");

    /* The screen from the first moment, not from the first frame. The
     * NeoDCT fbdev output draws a loading ring while it has nothing else;
     * for the ring to be there during the seconds the url takes to open,
     * the output has to be created before the file is, which is exactly
     * what this option is for. Not for audio: a song would otherwise play
     * to a black screen where the caller's page used to be. */
    if (rc == ND_OK && kind != ND_MEDIA_AUDIO)
        rc = argv_push(out, "--force-window=immediate");

    if (rc == ND_OK && kind == ND_MEDIA_IMAGE) {
        rc = argv_push(out, "--image-display-duration=inf");
        if (rc == ND_OK)
            rc = argv_push(out, "--audio=no");
    }

    if (rc == ND_OK && ipc_socket != NULL && ipc_socket[0] != '\0')
        rc = argv_pushf(out, "--input-ipc-server=", ipc_socket);

    /* "--" first: a src attribute is attacker-controlled text, and mpv
     * would read one starting with a dash as an option. */
    if (rc == ND_OK)
        rc = argv_push(out, "--");
    if (rc == ND_OK)
        rc = argv_pushf(out, "", url);

    if (rc != ND_OK)
        return rc;
    out->argv[out->n] = NULL;
    return ND_OK;
}

nd_err nd_media_ipc_socket_path(char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    return nd_snprintf(out, out_sz, ND_MEDIA_IPC_SOCKET_FMT, (long)getpid());
}

const char *nd_media_exit_name(int status)
{
    switch (status) {
    case ND_MEDIA_EXIT_OK:       return "played";
    case ND_MEDIA_EXIT_FAILED:   return "could not play";
    case ND_MEDIA_EXIT_NOLOAD:   return "could not load";
    case ND_MEDIA_EXIT_NONET:    return "no connection";
    case ND_MEDIA_EXIT_NOTFOUND: return "not found";
    case ND_MEDIA_EXIT_FORMAT:   return "unsupported format";
    case ND_MEDIA_EXIT_DIED:     return "player killed";
    case ND_MEDIA_ENOENT:        return "no mpv";
    default:                     return "mpv error";
    }
}

/* ------------------------------------------------------------------ *
 * What mpv said
 * ------------------------------------------------------------------ */

/* The value of "key":"..." in one line of mpv's JSON, unescaped, or false
 * if the key is not there. Deliberately not a JSON parser: mpv writes one
 * flat object per line with string values, and the two escapes it uses in
 * practice are the quote and the newline ffmpeg leaves on every message.
 * Anything stranger is copied through, which for pattern matching is
 * exactly as good as decoding it. */
static bool json_string(const char *line, const char *key, char *out, size_t out_sz)
{
    char pat[64];
    const char *p;
    size_t used = 0u;

    if (nd_snprintf(pat, sizeof pat, "\"%s\":\"", key) != ND_OK)
        return false;
    p = strstr(line, pat);
    if (p == NULL)
        return false;
    p += strlen(pat);

    while (*p != '\0' && *p != '"' && used + 1u < out_sz) {
        char c = *p++;

        if (c == '\\' && *p != '\0') {
            char e = *p++;

            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = e;    break; /* \" \\ \/ and the rest */
            }
        }
        out[used++] = c;
    }
    out[used] = '\0';
    return true;
}

static bool contains_ci_str(const char *hay, const char *needle);

/* What ffmpeg says when the network, not the file, is the problem. Matched
 * case-insensitively against the log text; the phrases are ffmpeg's own
 * (libavformat/tcp.c, network.c, http.c) and glibc's resolver's. */
static const char *const UNREACHABLE_PHRASES[] = {
    "failed to resolve hostname",
    "name or service not known",
    "temporary failure in name resolution",
    "connection refused",
    "network is unreachable",
    "no route to host",
    "connection timed out",
    "timed out",
    "connection reset",
    "host is down",
    NULL
};

/* And when the server answered and said no. "HTTP error 404 Not Found" is
 * http.c's log line; "Server returned 404 Not Found" is the same condition
 * as an error string, which mpv also logs. Anything in the 400s is the
 * same story from the user's side: the thing the page linked is not there
 * for them. */
static bool http_client_error(const char *text)
{
    static const char *const prefixes[] = {"http error 4", "server returned 4", NULL};
    size_t i;

    for (i = 0u; prefixes[i] != NULL; i++) {
        if (contains_ci_str(text, prefixes[i]))
            return true;
    }
    return false;
}

/* What mpv itself says when the bytes arrived and could not be used:
 * loadfile.c's "Failed to recognize file format." and "No video or audio
 * streams selected." -- the same two conditions the end-file event names
 * as "unrecognized file format" and "no audio or video data played". */
static const char *const FORMAT_PHRASES[] = {
    "failed to recognize file format",
    "unrecognized file format",
    "no video or audio streams selected",
    "no audio or video data played",
    NULL
};

bool nd_media_outcome_feed_text(nd_media_outcome *o, const char *text)
{
    bool changed = false;
    size_t i;

    if (o == NULL || text == NULL)
        return false;

    for (i = 0u; UNREACHABLE_PHRASES[i] != NULL; i++) {
        if (contains_ci_str(text, UNREACHABLE_PHRASES[i])) {
            changed |= !o->unreachable;
            o->unreachable = true;
            break;
        }
    }
    if (http_client_error(text)) {
        changed |= !o->not_found;
        o->not_found = true;
    }
    for (i = 0u; FORMAT_PHRASES[i] != NULL; i++) {
        if (contains_ci_str(text, FORMAT_PHRASES[i])) {
            changed |= !o->format_error;
            o->format_error = true;
            break;
        }
    }
    return changed;
}

bool nd_media_outcome_feed(nd_media_outcome *o, const char *line)
{
    char event[32];
    char text[256];
    bool changed = false;

    if (o == NULL || line == NULL)
        return false;
    if (!json_string(line, "event", event, sizeof event))
        return false;

    if (strcmp(event, "log-message") == 0) {
        if (!json_string(line, "text", text, sizeof text))
            return false;
        return nd_media_outcome_feed_text(o, text);
    }

    if (strcmp(event, "end-file") == 0) {
        changed = !o->ended;
        o->ended = true;
        if (json_string(line, "reason", text, sizeof text) && strcmp(text, "error") == 0) {
            changed |= !o->error;
            o->error = true;
        }
        /* mpv_error_string() words, from libmpv/client.h: "unrecognized
         * file format" is the demuxer giving up, "no audio or video data
         * played" is every decoder giving up. Both mean the bytes arrived
         * and this build cannot do anything with them. */
        if (json_string(line, "file_error", text, sizeof text) &&
            (strstr(text, "unrecognized file format") != NULL ||
             strstr(text, "no audio or video data") != NULL ||
             strstr(text, "not supported") != NULL)) {
            changed |= !o->format_error;
            o->format_error = true;
        }
        return changed;
    }

    return false;
}

int nd_media_exit_status(const nd_media_outcome *o, bool exited, int mpv_rc)
{
    if (!exited)
        return ND_MEDIA_EXIT_DIED;
    if (mpv_rc == ND_MEDIA_EXIT_OK || mpv_rc == ND_MEDIA_ENOENT || o == NULL)
        return mpv_rc;

    /* A failure with a reason attached beats mpv's bare 2. The order is
     * from most to least specific about the cause. A reason seen on stderr
     * counts even when the end-file event was missed -- the socket may
     * never have connected -- but only against a 2: a 1 is mpv failing to
     * start, and a 404 in its log is not why. */
    if (mpv_rc == ND_MEDIA_EXIT_FAILED || o->error) {
        if (o->format_error)
            return ND_MEDIA_EXIT_FORMAT;
        if (o->not_found)
            return ND_MEDIA_EXIT_NOTFOUND;
        if (o->unreachable)
            return ND_MEDIA_EXIT_NONET;
        if (o->error)
            return ND_MEDIA_EXIT_NOLOAD;
    }
    return mpv_rc;
}

/* ------------------------------------------------------------------ *
 * Keys
 * ------------------------------------------------------------------ */

/* NeoDCT keycode -> mpv IPC command. Matches input.conf exactly, which is
 * what answers when this bridge is not there; test_mediawidget.c fails if
 * the two ever drift. */
static const char *const CMD_QUIT[] = {"quit", NULL};
static const char *const CMD_PAUSE[] = {"cycle", "pause", NULL};
static const char *const CMD_BACK10[] = {"seek", "-10", "relative", NULL};
static const char *const CMD_FWD10[] = {"seek", "10", "relative", NULL};
static const char *const CMD_VOLUP[] = {"add", "volume", "5", NULL};
static const char *const CMD_VOLDOWN[] = {"add", "volume", "-5", NULL};
static const char *const CMD_BACK60[] = {"seek", "-60", "relative", NULL};
static const char *const CMD_FWD60[] = {"seek", "60", "relative", NULL};

/* 1..9 jump to 10%..90%, 0 to the start -- the way every DVD player did it.
 * Keycodes 2..11 are the digit row: KEY_1 is 2 and KEY_0 is 11. */
static const char *const CMD_PCT[10][4] = {
    {"seek", "10", "absolute-percent", NULL}, {"seek", "20", "absolute-percent", NULL},
    {"seek", "30", "absolute-percent", NULL}, {"seek", "40", "absolute-percent", NULL},
    {"seek", "50", "absolute-percent", NULL}, {"seek", "60", "absolute-percent", NULL},
    {"seek", "70", "absolute-percent", NULL}, {"seek", "80", "absolute-percent", NULL},
    {"seek", "90", "absolute-percent", NULL}, {"seek", "0", "absolute-percent", NULL},
};

const char *const *nd_media_ipc_command(int32_t keycode)
{
    if (keycode >= 2 && keycode <= 11)
        return CMD_PCT[keycode - 2];

    switch (keycode) {
    case 14:  return CMD_QUIT;     /* C: back to the application */
    case 28:  return CMD_PAUSE;    /* navikey */
    case 105: return CMD_BACK10;
    case 106: return CMD_FWD10;
    case 103: return CMD_VOLUP;
    case 108: return CMD_VOLDOWN;
    case 42:  return CMD_BACK60;   /* * */
    case 43:  return CMD_FWD60;    /* # */
    default:  return NULL;
    }
}

size_t nd_media_encode_command(const char *const *command, char *out, size_t out_sz)
{
    size_t used = 0u;
    size_t i;

    if (command == NULL || out == NULL || out_sz == 0u)
        return 0u;

    /* Compact JSON, the separators Python's json.dumps was given: no space
     * after ':' or ','. mpv does not care, but the tests compare bytes. */
#define PUT(s)                                       \
    do {                                             \
        size_t _n = strlen(s);                       \
        if (used + _n >= out_sz)                     \
            return 0u;                               \
        memcpy(out + used, (s), _n);                 \
        used += _n;                                  \
    } while (0)

    PUT("{\"command\":[");
    for (i = 0u; command[i] != NULL; i++) {
        const char *p;

        if (i > 0u)
            PUT(",");
        PUT("\"");
        /* Every string in the table is a bare command word, but escaping is
         * two lines and removes the question. */
        for (p = command[i]; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') {
                if (used + 2u >= out_sz)
                    return 0u;
                out[used++] = '\\';
            } else if (used + 1u >= out_sz) {
                return 0u;
            }
            out[used++] = *p;
        }
        PUT("\"");
    }
    PUT("]}\n");
#undef PUT

    out[used] = '\0';
    return used;
}

/* ------------------------------------------------------------------ *
 * Finding the keypad
 * ------------------------------------------------------------------ */

static bool read_device_name(const char *sysfs_event, char *out, size_t out_sz)
{
    char path[ND_PATH_MAX];
    FILE *f;
    size_t n;

    if (nd_snprintf(path, sizeof path, "/sys/class/input/%s/device/name", sysfs_event) != ND_OK)
        return false;
    f = fopen(path, "r");
    if (f == NULL)
        return false;
    if (fgets(out, (int)out_sz, f) == NULL) {
        (void)fclose(f);
        return false;
    }
    (void)fclose(f);
    n = strlen(out);
    while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r' || out[n - 1u] == ' '))
        out[--n] = '\0';
    return true;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t hn = strlen(hay);
    size_t nn = strlen(needle);
    size_t i;

    if (nn == 0u || nn > hn)
        return nn == 0u;
    for (i = 0u; i + nn <= hn; i++) {
        if (strncasecmp(hay + i, needle, nn) == 0)
            return true;
    }
    return false;
}

static bool contains_ci_str(const char *hay, const char *needle)
{
    return contains_ci(hay, needle);
}

/* The event names in /sys/class/input, sorted by strcmp -- which is what
 * Python's sorted() does, and it matters: "event10" must not be searched
 * before "event2", because the first match wins. There is no shared
 * directory helper in this codebase; every module opens its own. */
static size_t list_input_events(char names[][16], size_t max)
{
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    size_t n = 0u;

    if (d == NULL)
        return 0u;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t i;

        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        if (strlen(e->d_name) >= 16u)
            continue;

        /* Insertion sort: this list is a handful of entries on any real
         * device, and it keeps the whole thing on the stack. */
        for (i = n; i > 0u && strcmp(names[i - 1u], e->d_name) > 0; i--)
            memcpy(names[i], names[i - 1u], 16u);
        (void)nd_snprintf(names[i], 16u, "%s", e->d_name);
        n++;
    }
    (void)closedir(d);
    return n;
}

nd_err nd_media_discover_keypad(char *out, size_t out_sz)
{
    char events[64][16];
    char keypad[ND_PATH_MAX];
    char keyboard[ND_PATH_MAX];
    size_t n_events;
    size_t i;
    const char *override;

    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    out[0] = '\0';
    keypad[0] = '\0';
    keyboard[0] = '\0';

    override = getenv(ND_MEDIA_KEYPAD_DEVICE_ENV);
    if (override != NULL && override[0] != '\0' && nd_path_exists(override))
        return nd_snprintf(out, out_sz, "%s", override);

    n_events = list_input_events(events, ND_ARRAY_LEN(events));
    for (i = 0u; i < n_events; i++) {
        char node[ND_PATH_MAX];
        char name[64];

        if (nd_snprintf(node, sizeof node, "/dev/input/%s", events[i]) != ND_OK)
            continue;
        if (!nd_path_exists(node))
            continue;
        if (!read_device_name(events[i], name, sizeof name))
            continue;

        /* The bridge wins outright, wherever it is in the list. */
        if (strcmp(name, ND_MEDIA_KEYPAD_UINPUT_NAME) == 0)
            return nd_snprintf(out, out_sz, "%s", node);
        if (keypad[0] == '\0' && contains_ci(name, "keypad"))
            (void)nd_snprintf(keypad, sizeof keypad, "%s", node);
        if (keyboard[0] == '\0' && contains_ci(name, "keyboard"))
            (void)nd_snprintf(keyboard, sizeof keyboard, "%s", node);
    }

    if (keypad[0] != '\0')
        return nd_snprintf(out, out_sz, "%s", keypad);
    if (keyboard[0] != '\0')
        return nd_snprintf(out, out_sz, "%s", keyboard);
    return ND_ERR_NOTFOUND;
}

int nd_media_open_keypad(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return -1;
    return open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* ------------------------------------------------------------------ *
 * Playing
 * ------------------------------------------------------------------ */

/* How long to wait for mpv to bind its IPC socket. It does so during
 * startup, after loading its config but before the first frame, so this is
 * generous: dropping the user's first keypress is worse than a slow start. */
#define SOCKET_TIMEOUT_MS 5000

/* The bridge's poll interval. Long enough not to spin a core the decoder
 * needs, short enough that a keypress does not feel dropped. */
#define KEY_POLL_MS 100

static void nap_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static double now_ms(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0.0;
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* SIGSTOP `pid`, or leave it alone if that would be a mistake.
 *
 * Stopping ourselves, or init, produces a phone that looks powered on and
 * answers nothing and cannot be recovered without pulling the battery.
 * Returns the pid actually stopped, or 0. */
static pid_t suspend_begin(pid_t pid)
{
    if (pid <= 1)
        return 0;
    if (pid == getpid()) {
        nd_log_err(ND_LOG_UI, "media: refusing to suspend the calling process");
        return 0;
    }
    if (kill(pid, SIGSTOP) != 0) {
        /* Already gone, or not ours. Neither is a reason not to play. */
        return 0;
    }
    return pid;
}

/* The SIGCONT half. A SIGSTOP that is never undone leaves a phone that looks
 * powered on and answers nothing, so every exit path runs this -- including
 * the one where mpv could not be started at all. */
static void suspend_end(pid_t pid)
{
    if (pid > 1)
        (void)kill(pid, SIGCONT);
}

/* Has the child exited? Asked WITHOUT reaping it (WNOWAIT), so that the
 * one waitpid() at the end of nd_media_play() still has a status to read.
 *
 * This used to be waitpid(child, NULL, WNOHANG), which reaps -- and throws
 * the status away. The final waitpid() then found no child at all, left
 * its status variable at zero, and zero said "played": every failure mpv
 * ever reported while the bridge was running came back to the browser as
 * a success. Found by the fake-mpv test, not on a phone. */
static bool child_exited(pid_t child)
{
    siginfo_t info;

    memset(&info, 0, sizeof info);
    if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT) != 0)
        return true; /* ECHILD: already gone, whatever the reason */
    return info.si_pid == child;
}

/* One evdev read, returning the first key-DOWN code in it or -1.
 *
 * value 1 is a press; 0 is a release and 2 is autorepeat, and neither
 * should count as the user asking for something again. A single read can
 * carry a whole burst, so the rest are held rather than dropped. */
static int32_t next_keycode(int fd, int timeout_ms, int32_t *pending, size_t *n_pending)
{
    struct input_event evs[64];
    struct pollfd pfd;
    ssize_t got;
    size_t i;
    size_t n;

    if (*n_pending > 0u) {
        int32_t code = pending[0];

        for (i = 1u; i < *n_pending; i++)
            pending[i - 1u] = pending[i];
        (*n_pending)--;
        return code;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1u, timeout_ms) <= 0)
        return -1;

    got = read(fd, evs, sizeof evs);
    if (got <= 0)
        return -1;

    n = (size_t)got / sizeof evs[0];
    for (i = 0u; i < n; i++) {
        if (evs[i].type == EV_KEY && evs[i].value == 1) {
            if (*n_pending < 64u)
                pending[(*n_pending)++] = (int32_t)evs[i].code;
        }
    }
    if (*n_pending == 0u)
        return -1;
    return next_keycode(fd, 0, pending, n_pending);
}

/* One chunk off a descriptor, split into lines. A line longer than the
 * buffer is dropped up to its newline rather than matched in pieces: half a
 * log message can be the half without the "not" in it. */
typedef struct {
    char buf[1024];
    size_t used;
    bool overflow;
} line_buf;

/* The socket carries JSON events; stderr carries mpv's terminal log, which
 * is classified as text and then passed on to OUR stderr so the serial
 * console still shows every line mpv wrote. */
static void feed_lines(line_buf *l, const char *data, size_t n, nd_media_outcome *o,
                       bool terminal)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        char c = data[i];

        if (c == '\n') {
            if (!l->overflow) {
                l->buf[l->used] = '\0';
                if (terminal) {
                    (void)nd_media_outcome_feed_text(o, l->buf);
                    l->buf[l->used] = '\n';
                    (void)!write(STDERR_FILENO, l->buf, l->used + 1u);
                } else {
                    (void)nd_media_outcome_feed(o, l->buf);
                }
            }
            l->used = 0u;
            l->overflow = false;
        } else if (l->used + 2u < sizeof l->buf) {
            l->buf[l->used++] = c;
        } else {
            l->overflow = true;
        }
    }
}

/* One attempt at mpv's socket; -1 if it is not there yet. */
static int ipc_try_connect(const char *sock_path)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(sock_path) >= sizeof addr.sun_path)
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)nd_snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof addr) == 0)
        return fd;
    (void)close(fd);
    return -1;
}

/* Ask mpv to send its error log over the socket too. Late -- see
 * nd_media_outcome_feed_text() for why stderr is the one that catches a
 * fast failure -- but it costs nothing and covers a stderr that could not
 * be piped. */
static void ipc_request_log(int conn)
{
    static const char *const req[] = {"request_log_messages", "error", NULL};
    char buf[128];
    size_t n = nd_media_encode_command(req, buf, sizeof buf);

    if (n > 0u)
        (void)send(conn, buf, n, MSG_NOSIGNAL);
}

/* How often to retry the socket while mpv is still starting. */
#define CONNECT_RETRY_MS 50

/* How many more polls to give the stderr pipe after mpv has exited before
 * giving up on its EOF. It arrives at once in practice -- nothing else
 * holds the write end -- so this is a guard against a stuck fd, not a
 * wait. */
#define DRAIN_POLLS_AFTER_EXIT 20

/* Talk to mpv until it exits: keys go in over the socket, events come out
 * of it, and the terminal log comes out of the pipe.
 *
 * Everything learned lands in `o`. mpv's own input.conf bindings are the
 * fallback if the socket never comes up, and the exit status is the
 * fallback for the reason. `sock_path` may be NULL and `keypad_fd` and
 * `err_fd` may be -1; the loop runs for whichever of them exist. */
static void bridge(pid_t child, const char *sock_path, int keypad_fd, int err_fd,
                   nd_media_outcome *o)
{
    int32_t pending[64];
    size_t n_pending = 0u;
    line_buf sock_lines;
    line_buf err_lines;
    int conn = -1;
    bool exited = false;
    int drain_polls = 0;
    double sock_deadline = now_ms() + (double)SOCKET_TIMEOUT_MS;
    double next_try = 0.0;

    memset(&sock_lines, 0, sizeof sock_lines);
    memset(&err_lines, 0, sizeof err_lines);
    if (sock_path != NULL && sock_path[0] == '\0')
        sock_path = NULL;

    for (;;) {
        struct pollfd pfd[3];
        nfds_t n_fds = 0u;
        int conn_slot = -1;
        int keypad_slot = -1;
        int err_slot = -1;
        int ready;
        double now = now_ms();

        if (!exited && child_exited(child))
            exited = true;

        /* The socket appears during mpv's startup, after its config but
         * before its first frame. Keep trying, without ever blocking the
         * stderr pipe: a pipe nobody reads fills, and a full pipe blocks
         * mpv on its next log line. */
        if (conn < 0 && sock_path != NULL && !exited && now < sock_deadline && now >= next_try) {
            conn = ipc_try_connect(sock_path);
            if (conn >= 0)
                ipc_request_log(conn);
            else
                next_try = now + (double)CONNECT_RETRY_MS;
        }

        if (conn >= 0) {
            conn_slot = (int)n_fds;
            pfd[n_fds].fd = conn;
            pfd[n_fds].events = POLLIN;
            pfd[n_fds].revents = 0;
            n_fds++;
            if (keypad_fd >= 0) {
                keypad_slot = (int)n_fds;
                pfd[n_fds].fd = keypad_fd;
                pfd[n_fds].events = POLLIN;
                pfd[n_fds].revents = 0;
                n_fds++;
            }
        }
        if (err_fd >= 0) {
            err_slot = (int)n_fds;
            pfd[n_fds].fd = err_fd;
            pfd[n_fds].events = POLLIN;
            pfd[n_fds].revents = 0;
            n_fds++;
        }

        if (exited) {
            /* Nothing more can arrive on the socket; only the pipe's tail
             * is worth waiting for, and not for long. */
            if (err_fd < 0 || drain_polls++ > DRAIN_POLLS_AFTER_EXIT)
                break;
        }

        if (n_fds == 0u) {
            if (exited)
                break;
            nap_ms(CONNECT_RETRY_MS);
            continue;
        }

        ready = poll(pfd, n_fds, CONNECT_RETRY_MS);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready <= 0)
            continue;

        if (err_slot >= 0 && (pfd[err_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            char chunk[512];
            ssize_t got;

            while ((got = read(err_fd, chunk, sizeof chunk)) > 0)
                feed_lines(&err_lines, chunk, (size_t)got, o, true);
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
                /* EOF: every writer is gone, mpv included. */
                (void)close(err_fd);
                err_fd = -1;
            }
        }

        if (conn_slot >= 0 && (pfd[conn_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            char chunk[512];
            ssize_t got = recv(conn, chunk, sizeof chunk, 0);

            if (got > 0) {
                feed_lines(&sock_lines, chunk, (size_t)got, o, false);
            } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
                /* mpv closed its end: it is on its way out. */
                (void)close(conn);
                conn = -1;
                sock_path = NULL;
            }
        }

        if (keypad_slot >= 0 && conn >= 0 && (pfd[keypad_slot].revents & POLLIN)) {
            /* Everything queued on the device, then everything queued in
             * pending: a burst is forwarded whole rather than one key per
             * poll interval. */
            int32_t code;

            while ((code = next_keycode(keypad_fd, 0, pending, &n_pending)) >= 0) {
                const char *const *command = nd_media_ipc_command(code);
                char buf[256];
                size_t n;

                if (command == NULL)
                    continue;
                n = nd_media_encode_command(command, buf, sizeof buf);
                if (n == 0u)
                    continue;
                /* MSG_NOSIGNAL: a write to a socket mpv has just closed
                 * would otherwise be a SIGPIPE, which would end THIS
                 * process before it could resume the application it
                 * stopped -- a browser frozen with mpv already gone. */
                if (send(conn, buf, n, MSG_NOSIGNAL) < 0) {
                    (void)close(conn);
                    conn = -1;
                    sock_path = NULL;
                    break;
                }
            }
        }
    }
    if (conn >= 0)
        (void)close(conn);
    if (err_fd >= 0)
        (void)close(err_fd);
}

int nd_media_play(const char *url, nd_media_kind kind, pid_t suspend_pid, int keypad_fd,
                  const char *mpv, const char *fbdev, const char *ipc_socket,
                  const char *input_conf)
{
    nd_media_argv a;
    nd_media_outcome outcome;
    pid_t stopped = 0;
    pid_t child = -1;
    int status = 0;
    int rc = ND_MEDIA_ENOENT;
    int errp[2] = {-1, -1};

    if (url == NULL)
        return ND_MEDIA_ENOENT;
    if (mpv == NULL)
        mpv = ND_MPV_BIN;
    memset(&outcome, 0, sizeof outcome);

    /* The socket is wanted even with no keypad to forward from: it is how
     * mpv says why a file would not play, and "Video not found" on the
     * status bar is worth a socket on its own. */
    if (ipc_socket != NULL && ipc_socket[0] != '\0') {
        char dir[ND_PATH_MAX];
        const char *slash = strrchr(ipc_socket, '/');

        /* A killed mpv leaves its socket behind, and binding onto an
         * existing path fails -- so one crash would break playback until
         * the next reboot. */
        (void)unlink(ipc_socket);
        if (slash != NULL && (size_t)(slash - ipc_socket) < sizeof dir) {
            memcpy(dir, ipc_socket, (size_t)(slash - ipc_socket));
            dir[slash - ipc_socket] = '\0';
            (void)mkdir(dir, 0755);
        }
    }

    if (nd_media_build_argv(&a, url, kind, fbdev, ipc_socket, mpv, input_conf) != ND_OK) {
        nd_log_err(ND_LOG_UI, "media: command line too long for %s", url);
        return ND_MEDIA_ENOENT;
    }

    /* mpv's terminal output comes back through a pipe: it is where a fast
     * failure says why (see nd_media_outcome_feed_text). No pipe is not
     * fatal -- mpv then keeps our descriptors and the socket is all there
     * is. */
    if (pipe(errp) != 0) {
        errp[0] = -1;
        errp[1] = -1;
    } else {
        (void)fcntl(errp[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(errp[1], F_SETFD, FD_CLOEXEC);
        (void)fcntl(errp[0], F_SETFL, fcntl(errp[0], F_GETFL) | O_NONBLOCK);
    }

    stopped = suspend_begin(suspend_pid);

    /* fork + execve, and NOTHING between them but the exec: this process
     * may have threads, and a mutex another thread held at the instant of
     * the fork stays locked forever in the child. nd_proc.h says the same
     * thing at greater length. dup2 is on the async-signal-safe list. */
    (void)fflush(NULL);
    child = fork();
    if (child == 0) {
        /* BOTH descriptors. mpv's terminal log goes to STDOUT (common/
         * msg.c: only the status line is stderr), and the browser starts
         * us with stdout on /dev/null -- so a pipe on stderr alone heard
         * nothing, and the first emulator run said "Could not load video"
         * for a 404 with the reason sitting in a discarded stdout. */
        if (errp[1] >= 0) {
            (void)dup2(errp[1], STDOUT_FILENO);
            (void)dup2(errp[1], STDERR_FILENO);
        }
        (void)execv(mpv, (char *const *)(uintptr_t)a.argv);
        _exit(ND_MEDIA_ENOENT); /* no mpv on this image */
    }
    if (errp[1] >= 0)
        (void)close(errp[1]);
    if (child < 0) {
        nd_log_err(ND_LOG_UI, "media: fork: %s", strerror(errno));
        if (errp[0] >= 0)
            (void)close(errp[0]);
        suspend_end(stopped);
        return ND_MEDIA_ENOENT;
    }

    /* Closes errp[0] itself. */
    bridge(child, ipc_socket, keypad_fd, errp[0], &outcome);

    {
        pid_t waited;

        while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR) {}
        if (waited != child) {
            /* Nothing to read a status from. It cannot happen now that the
             * bridge no longer reaps, but a guess of "played" is the wrong
             * guess to make about a player that has vanished. */
            nd_log_err(ND_LOG_UI, "media: lost mpv's exit status: %s", strerror(errno));
            rc = ND_MEDIA_EXIT_DIED;
        } else {
            rc = nd_media_exit_status(&outcome, WIFEXITED(status) != 0,
                                      WIFEXITED(status) ? WEXITSTATUS(status) : 0);
        }
    }

    /* Whatever happened, give the caller its screen and its CPU back. */
    suspend_end(stopped);
    if (ipc_socket != NULL && ipc_socket[0] != '\0')
        (void)unlink(ipc_socket);
    return rc;
}
