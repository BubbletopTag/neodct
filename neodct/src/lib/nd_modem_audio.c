/* nd_modem_audio.c -- full-duplex call audio over the SIM7600's PCM port.
 *
 *     speaker <- aplay   <- PCM port     (the far end's voice)
 *     far end <- PCM port <- arecord     (our mic, via the USB sound card)
 *
 * Zero in-process audio code, exactly as in the Python: two fire-and-forget
 * alsa-utils children write to and read from the bidirectional serial device
 * on USB interface 4 once AT+CPCMREG=1 has been sent.
 *
 * ============ THINGS THAT LOOK WRONG AND ARE NOT ============
 *
 *  - The PCM port is opened, put in raw mode and CLOSED AGAIN before either
 *    child starts. The configuration sticks to the device, not to the fd.
 *  - Its baud rate is NOT set, unlike the AT port's. PCM is not framed by the
 *    UART clock here, and the Python does not set it either.
 *  - "default" is a trap for the microphone once the guest has two cards:
 *    QEMU's USB Audio is playback-only card 0, so ALSA's default maps to a
 *    device arecord cannot capture from. /proc/asound is scanned for a pcm*c
 *    node instead, and the scan is byte-sorted, so card10 precedes card2.
 *  - The speaker retries for ever; the mic gets three tries and then the call
 *    carries on listen-only, because "there is no capture device" is a normal
 *    state on this hardware, not something to spin on.
 *  - _stop_call_audio() sends SIGKILL, not SIGTERM. Popen.kill() is SIGKILL
 *    on Linux and an aplay left in D-state would hold the port.
 *
 * The children go through nd_proc_spawn(), which forks and execs with nothing
 * in between (CODING-STANDARDS.md section 1.1) and whose reaper keeps the
 * exit status around for the watchdog below to find.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_modem_priv.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_types.h"

#define MIC_GIVE_UP_AFTER 3

/* ------------------------------------------------------------------ *
 * Finding the PCM port and the capture device
 * ------------------------------------------------------------------ */

static bool read_iface_hex(const char *name, int32_t *out)
{
    char virt[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    char text[64];
    FILE *f;
    size_t n;
    size_t i;

    if (nd_snprintf(virt, sizeof virt, "%s/%s/device/../bInterfaceNumber", ND_MODEM_TTY_DIR,
                    name) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, virt) != ND_OK)
        return false;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return false;
    n = fread(text, 1u, sizeof text - 1u, f);
    (void)fclose(f);
    text[n] = '\0';
    for (i = n; i > 0u && (text[i - 1u] == '\n' || text[i - 1u] == '\r' || text[i - 1u] == ' ');
         i--)
        text[i - 1u] = '\0';
    return nd_modem__parse_hex(text, out);
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static size_t sorted_listdir(const char *dir, char names[][ND_MODEM_PORT_MAX], size_t max)
{
    char resolved[ND_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    size_t n = 0u;

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return 0u;
    d = opendir(resolved);
    if (d == NULL)
        return 0u;
    while ((ent = readdir(d)) != NULL && n < max) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        (void)nd_strlcpy(names[n], ent->d_name, ND_MODEM_PORT_MAX);
        n++;
    }
    (void)closedir(d);
    qsort(names, n, ND_MODEM_PORT_MAX, cmp_name);
    return n;
}

void nd_modem__pcm_port(char *out, size_t out_sz)
{
    char names[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    const char *configured = nd_settings_get(ND_SET_HW_MODEM_PCM_PORT, "AUTO");
    size_t n;
    size_t i;

    if (configured != NULL && strcmp(configured, "AUTO") != 0) {
        (void)nd_strlcpy(out, configured, out_sz);
        return;
    }

    n = sorted_listdir(ND_MODEM_TTY_DIR, names, ND_ARRAY_LEN(names));
    for (i = 0u; i < n; i++) {
        int32_t iface;

        if (strncmp(names[i], "ttyUSB", 6u) != 0)
            continue;
        if (read_iface_hex(names[i], &iface) && iface == 4) {
            (void)snprintf(out, out_sz, "/dev/%s", names[i]);
            return;
        }
    }
    (void)nd_strlcpy(out, "/dev/ttyUSB4", out_sz);
}

static bool all_digits(const char *s, size_t len)
{
    size_t i;

    if (len == 0u)
        return false;
    for (i = 0u; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/* The first "pcmNc" node in one card's directory, as "plughw:CARD,DEVICE". */
static bool capture_node_in(const char *card, char *out, size_t out_sz)
{
    char dir[ND_PATH_MAX];
    char nodes[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    size_t n_nodes;
    size_t j;

    if (nd_snprintf(dir, sizeof dir, "%s/%s", ND_MODEM_ASOUND_DIR, card) != ND_OK)
        return false;
    n_nodes = sorted_listdir(dir, nodes, ND_ARRAY_LEN(nodes));
    for (j = 0u; j < n_nodes; j++) {
        size_t len = strlen(nodes[j]);

        if (len < 5u || strncmp(nodes[j], "pcm", 3u) != 0 || nodes[j][len - 1u] != 'c')
            continue;
        if (!all_digits(&nodes[j][3], len - 4u))
            continue;
        nodes[j][len - 1u] = '\0';
        (void)snprintf(out, out_sz, "plughw:%s,%s", &card[4], &nodes[j][3]);
        return true;
    }
    return false;
}

/* A USB sound card announces itself with a usbid; the SoC's own codec does
 * not. This is the same test S17audio uses to choose the PLAYBACK card, and
 * using it here is what keeps both ends of a call on one piece of hardware. */
static bool card_is_usb(const char *card)
{
    char path[ND_PATH_MAX];

    if (nd_snprintf(path, sizeof path, "%s/%s/usbid", ND_MODEM_ASOUND_DIR, card) != ND_OK)
        return false;
    return nd_path_exists(path);
}

static bool card_name_is_valid(const char *name)
{
    if (strncmp(name, "card", 4u) != 0)
        return false;
    return all_digits(&name[4], strlen(name) - 4u);
}

bool nd_modem__find_capture_device(char *out, size_t out_sz)
{
    char cards[ND_MODEM_CAND_MAX][ND_MODEM_PORT_MAX];
    size_t n_cards;
    size_t i;

    n_cards = sorted_listdir(ND_MODEM_ASOUND_DIR, cards, ND_ARRAY_LEN(cards));

    /* THE USB CARD FIRST, and the reason is the difference between this phone
     * and the emulator it was written on. In QEMU card 0 is USB Audio with no
     * capture node at all, so "the first card with a pcm*c" was the microphone
     * by luck. On the real board card 0 is the RV1103's own rv-acodec: it has
     * a capture node, arecord opens it without complaint, and it is wired to
     * nothing. Taking the first match there sends a call's uplink to a codec
     * with no microphone on it, and the far end hears silence with no error
     * raised anywhere -- which is the hardest kind of fault to find, because
     * every part reports success.
     *
     * The microphone is on the USB sound card, which is also where the
     * earpiece is, and both ends of a call belong on one card. */
    for (i = 0u; i < n_cards; i++) {
        if (!card_name_is_valid(cards[i]) || !card_is_usb(cards[i]))
            continue;
        if (capture_node_in(cards[i], out, out_sz))
            return true;
    }

    /* Then anything that can capture. A board with a wired-up on-chip
     * microphone and no dongle is a perfectly good phone, and this is also
     * the path every host test that does not create a usbid takes. */
    for (i = 0u; i < n_cards; i++) {
        if (!card_name_is_valid(cards[i]))
            continue;
        if (capture_node_in(cards[i], out, out_sz))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Spawning the two pipes
 * ------------------------------------------------------------------ */

/* subprocess.Popen(["aplay", ...]) searches PATH; nd_proc_spawn() takes a
 * path and execve()s it, so the search happens here. A miss is the Python's
 * FileNotFoundError. */
static bool which_exec(const char *name, char *out, size_t out_sz)
{
    const char *path;
    const char *p;

    if (strchr(name, '/') != NULL) {
        (void)nd_strlcpy(out, name, out_sz);
        return access(out, X_OK) == 0;
    }
    path = getenv("PATH");
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (p = path;;) {
        const char *colon = strchr(p, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - p) : strlen(p);
        const char *dir = (len == 0u) ? "." : p; /* an empty entry means "." */
        size_t dlen = (len == 0u) ? 1u : len;

        if (nd_snprintf(out, out_sz, "%.*s/%s", (int)dlen, dir, name) == ND_OK &&
            access(out, X_OK) == 0)
            return true;
        if (colon == NULL)
            break;
        p = colon + 1;
    }
    out[0] = '\0';
    return false;
}

/* stdout and stderr to /dev/null, exactly as subprocess.DEVNULL does. The
 * path is NOT ND_ROOT-resolved: it is the child's plumbing, not phone data,
 * and a scratch root has no /dev/null. */
static bool spawn_quiet(const char *const *argv, pid_t *pid_out)
{
    char exe[ND_PATH_MAX];
    nd_proc_spec spec;
    int devnull;
    nd_err rc;

    if (!which_exec(argv[0], exe, sizeof exe)) {
        errno = ENOENT;
        return false;
    }
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0)
        return false;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_AUDIO;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = devnull;
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = devnull;
    spec.n_fds = 2u;

    rc = nd_proc_spawn(exe, &spec, pid_out);
    (void)close(devnull);
    return rc == ND_OK;
}

static void start_speaker_pipe(nd_modem *m, const char *port)
{
    char rate[16];
    char resolved[ND_PATH_MAX];
    const char *argv[12];
    pid_t pid;

    (void)snprintf(rate, sizeof rate, "%d", (int)m->pcm_rate);
    if (nd_path_resolve(resolved, sizeof resolved, port) != ND_OK)
        return;

    argv[0] = "aplay";
    argv[1] = "-q";
    argv[2] = "-t";
    argv[3] = "raw";
    argv[4] = "-f";
    argv[5] = ND_MODEM_PCM_FORMAT;
    argv[6] = "-r";
    argv[7] = rate;
    argv[8] = "-c";
    argv[9] = "1";
    argv[10] = resolved;
    argv[11] = NULL;

    if (spawn_quiet(argv, &pid)) {
        m->audio_pid = pid;
        m->audio_live = true;
        nd_log(ND_LOG_MODEM, "Call audio: aplay <- %s (%d Hz %s).", port, (int)m->pcm_rate,
               ND_MODEM_PCM_FORMAT);
    } else {
        m->audio_pid = -1;
        m->audio_live = false;
        nd_log(ND_LOG_MODEM, "Speaker pipe unavailable: aplay: %s", strerror(errno));
    }
}

static void start_mic_pipe(nd_modem *m, const char *port)
{
    char device[128];
    char rate[16];
    char resolved[ND_PATH_MAX];
    const char *argv[14];
    const char *configured;
    pid_t pid;
    size_t i;

    configured = nd_settings_get(ND_SET_HW_MODEM_MIC_DEV, "AUTO");
    (void)nd_strlcpy(device, configured != NULL ? configured : "AUTO", sizeof device);
    /* .strip(), then .upper() for the comparison only. */
    {
        size_t start = 0u;
        size_t end = strlen(device);

        while (start < end && (device[start] == ' ' || device[start] == '\t'))
            start++;
        while (end > start && (device[end - 1u] == ' ' || device[end - 1u] == '\t' ||
                               device[end - 1u] == '\n' || device[end - 1u] == '\r'))
            end--;
        memmove(device, &device[start], end - start);
        device[end - start] = '\0';
    }

    {
        char upper[sizeof device];

        for (i = 0u; device[i] != '\0' && i + 1u < sizeof upper; i++)
            upper[i] = (device[i] >= 'a' && device[i] <= 'z') ? (char)(device[i] - 32) : device[i];
        upper[i] = '\0';

        if (upper[0] == '\0' || strcmp(upper, "OFF") == 0 || strcmp(upper, "NONE") == 0) {
            nd_log(ND_LOG_MODEM, "Mic uplink disabled (system.hw.modem_mic_device=OFF).");
            return;
        }
        if (strcmp(upper, "AUTO") == 0) {
            if (!nd_modem__find_capture_device(device, sizeof device)) {
                nd_log(ND_LOG_MODEM,
                       "No ALSA capture device found (arecord -l); call is listen-only.");
                return;
            }
            nd_log(ND_LOG_MODEM, "Mic auto-detected: %s", device);
        }
    }

    (void)snprintf(rate, sizeof rate, "%d", (int)m->pcm_rate);
    if (nd_path_resolve(resolved, sizeof resolved, port) != ND_OK)
        return;

    argv[0] = "arecord";
    argv[1] = "-q";
    argv[2] = "-t";
    argv[3] = "raw";
    argv[4] = "-f";
    argv[5] = ND_MODEM_PCM_FORMAT;
    argv[6] = "-r";
    argv[7] = rate;
    argv[8] = "-c";
    argv[9] = "1";
    argv[10] = "-D";
    argv[11] = device;
    argv[12] = resolved;
    argv[13] = NULL;

    if (spawn_quiet(argv, &pid)) {
        m->mic_pid = pid;
        m->mic_live = true;
        nd_log(ND_LOG_MODEM, "Mic uplink: arecord -D %s -> %s (%d Hz %s).", device, port,
               (int)m->pcm_rate, ND_MODEM_PCM_FORMAT);
    } else {
        m->mic_pid = -1;
        m->mic_live = false;
        nd_log(ND_LOG_MODEM, "Mic uplink unavailable (arecord: %s); call is listen-only.",
               strerror(errno));
    }
}

/* ------------------------------------------------------------------ *
 * _start_call_audio, line 677
 * ------------------------------------------------------------------ */

void nd_modem__start_call_audio(nd_modem *m)
{
    char port[ND_MODEM_PORT_MAX];
    char resolved[ND_PATH_MAX];
    struct termios t;
    int fd;

    if (m->audio_live || m->mic_live)
        return;

    nd_modem__pcm_port(port, sizeof port);
    if (!nd_path_exists(port)) {
        nd_log(ND_LOG_MODEM, "PCM port %s not found; call audio unavailable.", port);
        return;
    }

    /* Opened only to configure the device, then closed again. */
    if (nd_path_resolve(resolved, sizeof resolved, port) != ND_OK)
        return;
    fd = open(resolved, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || tcgetattr(fd, &t) != 0) {
        nd_log(ND_LOG_MODEM, "PCM port setup failed: %s", strerror(errno));
        if (fd >= 0)
            (void)close(fd);
        return;
    }
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = CS8 | CREAD | CLOCAL; /* no cfsetspeed here -- see the header */
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        nd_log(ND_LOG_MODEM, "PCM port setup failed: %s", strerror(errno));
        (void)close(fd);
        return;
    }
    (void)close(fd);

    (void)nd_strlcpy(m->active_pcm_port, port, sizeof m->active_pcm_port);
    m->pcm_active = true;
    m->mic_fails = 0;
    start_speaker_pipe(m, port);
    start_mic_pipe(m, port);
}

/* ------------------------------------------------------------------ *
 * _stop_call_audio, line 746
 * ------------------------------------------------------------------ */

void nd_modem__stop_call_audio(nd_modem *m)
{
    nd_proc_status st;
    bool stopped = false;

    if (m->audio_live) {
        (void)kill(m->audio_pid, SIGKILL);
        (void)nd_proc_wait(m->audio_pid, 1.0, &st);
        m->audio_pid = -1;
        m->audio_live = false;
        stopped = true;
    }
    if (m->mic_live) {
        (void)kill(m->mic_pid, SIGKILL);
        (void)nd_proc_wait(m->mic_pid, 1.0, &st);
        m->mic_pid = -1;
        m->mic_live = false;
        stopped = true;
    }
    if (stopped)
        nd_log(ND_LOG_MODEM, "Call audio stopped.");

    m->active_pcm_port[0] = '\0';
    m->pcm_active = false;
    nd_modem__lock(m);
    m->call_stat = -1;
    m->call_connected = false;
    m->call_connected_at = 0.0;
    nd_modem__unlock(m);
    if (m->hardware)
        m->pcm_cleanup = true; /* AT+CPCMREG=0 goes out on the next free tick */
}

/* ------------------------------------------------------------------ *
 * _watch_audio_proc, line 523
 * ------------------------------------------------------------------ */

/* Popen.returncode: the exit status, or -N when a signal killed it. */
static int child_rc(const nd_proc_status *st)
{
    if (st->signalled)
        return -st->signo;
    return st->exit_status;
}

void nd_modem__watch_audio_proc(nd_modem *m, double now)
{
    nd_proc_status st;

    if (!m->pcm_active || now < m->next_audio_restart)
        return;

    if (m->audio_live && nd_proc_wait(m->audio_pid, 0.0, &st) == ND_OK) {
        m->next_audio_restart = now + ND_AUDIO_RESTART_HOLDOFF_S;
        nd_log(ND_LOG_MODEM, "Speaker pipe exited rc=%d mid-call; restarting.", child_rc(&st));
        m->audio_pid = -1;
        m->audio_live = false;
        start_speaker_pipe(m, m->active_pcm_port);
    }
    /* Both branches run in the same call and both may re-arm the holdoff. */
    if (m->mic_live && nd_proc_wait(m->mic_pid, 0.0, &st) == ND_OK) {
        int rc = child_rc(&st);

        m->mic_pid = -1;
        m->mic_live = false;
        m->mic_fails++;
        if (m->mic_fails >= MIC_GIVE_UP_AFTER) {
            nd_log(ND_LOG_MODEM,
                   "Mic pipe keeps dying (rc=%d); giving up -- call continues listen-only. "
                   "Check `arecord -l` and the system.hw.modem_mic_device setting.",
                   rc);
        } else {
            m->next_audio_restart = now + ND_AUDIO_RESTART_HOLDOFF_S;
            nd_log(ND_LOG_MODEM, "Mic pipe exited rc=%d; retrying.", rc);
            start_mic_pipe(m, m->active_pcm_port);
        }
    }
}
