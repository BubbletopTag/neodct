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

/* Wait for mpv's IPC socket to appear and connect to it. Returns -1 if mpv
 * exited or never got that far; the caller then just waits it out, and
 * mpv's own input.conf bindings are the way back. */
static int ipc_connect(pid_t child, const char *sock_path)
{
    double deadline = now_ms() + (double)SOCKET_TIMEOUT_MS;
    struct sockaddr_un addr;

    if (strlen(sock_path) >= sizeof addr.sun_path)
        return -1;

    while (now_ms() < deadline) {
        int fd;

        if (waitpid(child, NULL, WNOHANG) == child)
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
        nap_ms(50);
    }
    return -1;
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

/* Forward keypresses to mpv until it exits. */
static void bridge_keys(pid_t child, const char *sock_path, int keypad_fd)
{
    int32_t pending[64];
    size_t n_pending = 0u;
    int conn;

    conn = ipc_connect(child, sock_path);
    if (conn < 0)
        return;

    for (;;) {
        const char *const *command;
        char buf[256];
        size_t n;
        int32_t code;

        if (waitpid(child, NULL, WNOHANG) == child)
            break;

        code = next_keycode(keypad_fd, KEY_POLL_MS, pending, &n_pending);
        if (code < 0)
            continue;
        command = nd_media_ipc_command(code);
        if (command == NULL)
            continue;
        n = nd_media_encode_command(command, buf, sizeof buf);
        if (n == 0u)
            continue;
        if (write(conn, buf, n) < 0) {
            /* mpv closed the socket -- it is on its way out anyway. */
            break;
        }
    }
    (void)close(conn);
}

int nd_media_play(const char *url, nd_media_kind kind, pid_t suspend_pid, int keypad_fd,
                  const char *mpv, const char *fbdev, const char *ipc_socket,
                  const char *input_conf)
{
    nd_media_argv a;
    pid_t stopped = 0;
    pid_t child = -1;
    int status = 0;
    int rc = ND_MEDIA_ENOENT;

    if (url == NULL)
        return ND_MEDIA_ENOENT;
    if (mpv == NULL)
        mpv = ND_MPV_BIN;

    /* Without a key source there is no point in a socket: nothing would be
     * on the other end of it, and mpv's built-in bindings still work. */
    if (keypad_fd < 0)
        ipc_socket = NULL;

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

    stopped = suspend_begin(suspend_pid);

    /* fork + execve, and NOTHING between them but the exec: this process
     * may have threads, and a mutex another thread held at the instant of
     * the fork stays locked forever in the child. nd_proc.h says the same
     * thing at greater length. */
    (void)fflush(NULL);
    child = fork();
    if (child == 0) {
        (void)execv(mpv, (char *const *)(uintptr_t)a.argv);
        _exit(ND_MEDIA_ENOENT); /* no mpv on this image */
    }
    if (child < 0) {
        nd_log_err(ND_LOG_UI, "media: fork: %s", strerror(errno));
        suspend_end(stopped);
        return ND_MEDIA_ENOENT;
    }

    if (ipc_socket != NULL && ipc_socket[0] != '\0')
        bridge_keys(child, ipc_socket, keypad_fd);

    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    rc = WIFEXITED(status) ? WEXITSTATUS(status) : ND_MEDIA_ENOENT;

    /* Whatever happened, give the caller its screen and its CPU back. */
    suspend_end(stopped);
    if (ipc_socket != NULL && ipc_socket[0] != '\0')
        (void)unlink(ipc_socket);
    return rc;
}
