/* nd_modem_audio.c -- full-duplex call audio over the SIM7600's PCM port.
 *
 *     speaker <- aplay   <- PCM port     (the far end's voice)
 *     far end <- PCM port <- arecord     (our mic, via the USB sound card)
 *
 * Zero in-process audio code, exactly as in the Python: two fire-and-forget
 * alsa-utils children write to and read from the bidirectional serial device
 * on USB interface 4 once AT+CPCMREG=1 has been sent.
 *
 * ============ THE SEQUENCE, AND WHY IT IS NOT "START BOTH AT DIAL" ============
 *
 * The Python started both pipes the instant ATD returned, re-asserted
 * CPCMREG=1 when the call came up, and left them running. On the real phone
 * that produced calls where the far end was a wall of static, and calls
 * where the mic never worked. Both have the same root: the PCM stream is raw
 * S16_LE with no framing, so it is only intelligible if the reader starts on
 * a sample boundary and never loses a byte. Two things broke that:
 *
 *   - re-asserting CPCMREG=1 while aplay was already reading. If the modem
 *     restarts the stream at connect -- it switches from ringback to the
 *     vocoder -- a partial frame lands mid-read, and every sample after it
 *     is the wrong two bytes. That is the static, and it lasts the call.
 *   - a reader stalling. The tty holds 64 KB, two seconds at 16 kHz, then
 *     drops bytes in whatever amount fits, odd counts included. The old
 *     speaker restart waited three seconds with nobody reading.
 *
 * So the order is now, all on the modem thread (nd_modem_poll()):
 *
 *   ATD -> OK           state CALLING. Nothing else inside dial(): the UI
 *                       thread is blocked on it.
 *   next tick           CPCMFRM, CPCMREG=1. If accepted, the SPEAKER only,
 *                       for ringback. Nothing is listening on the uplink
 *                       yet, so the mic does not push 32 KB/s at a port
 *                       that is not in PCM mode.
 *   the call comes up   CPCMREG=1 again FIRST, while nothing reads the port;
 *                       then the speaker is killed, the port flushed and the
 *                       speaker restarted from a clean buffer; then the mic.
 *                       This happens once per call, on the first of VOICE
 *                       CALL: BEGIN, CLCC <stat> 0 or ATA to report it.
 *   a pipe dies         the speaker is restarted from a flushed port after
 *                       the holdoff; the mic gets its three strikes.
 *   the mic misses      a mic that never STARTED is now owed a retry too,
 *                       out of the same three strikes. It was not, and a
 *                       card caught halfway through re-enumerating therefore
 *                       cost the whole call. See mic_start_failed().
 *   the call ends       both pipes SIGKILLed, CPCMREG=0 on the next tick.
 *   the modem is lost   the same, because a pipe recorded as live with no
 *                       modem behind it blocked every later call's audio.
 *
 * ============ THINGS THAT LOOK WRONG AND ARE NOT ============
 *
 *  - The PCM port is opened, put in raw mode, FLUSHED and CLOSED AGAIN before
 *    a child starts. The configuration sticks to the device, not to the fd,
 *    and the flush is what guarantees the child's first byte is the first
 *    byte of a frame the modem sent after it.
 *  - Its baud rate is NOT set, unlike the AT port's. PCM is not framed by the
 *    UART clock here, and the Python does not set it either.
 *  - VMIN=1, VTIME=0 are set explicitly. Whoever last had the port may have
 *    left VMIN=0, and then aplay's first read returns nothing, which it takes
 *    as end of file and exits -- to be restarted every three seconds.
 *  - "default" is a trap for the microphone once the guest has two cards:
 *    QEMU's USB Audio is playback-only card 0, so ALSA's default maps to a
 *    device arecord cannot capture from. /proc/asound is scanned for a pcm*c
 *    node instead, and the scan is byte-sorted, so card10 precedes card2.
 *  - The speaker retries for ever; the mic gets three tries and then the call
 *    carries on listen-only, because "there is no capture device" is a normal
 *    state on this hardware, not something to spin on. A mic that ran for
 *    ND_MIC_STABLE_S first was working, and starts its three over.
 *  - _stop_call_audio() sends SIGKILL, not SIGTERM. Popen.kill() is SIGKILL
 *    on Linux and an aplay left in D-state would hold the port.
 *  - The capture MIXER is re-applied before every arecord, not trusted from
 *    boot. It is a handful of amixer children, bounded as a whole at 1.0 s
 *    per attempt, and it is the fix for the intermittent dead microphone;
 *    apply_capture_mixer() has the whole story, including why that bound had
 *    to be stated rather than left to be multiplied out.
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
#include "nd_mic.h"
#include "nd_modem_priv.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_types.h"

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

/* Byte-sorted listing of the entries of one directory whose names start with
 * `prefix`, and the prefix is checked BEFORE the entry is counted against
 * `max`.
 *
 * The twin of nd_modem.c's sorted_listdir(), and it had the same bug for the
 * same reason: `max` is ND_MODEM_CAND_MAX = 32, which is a bound on modem
 * ports, and it was being spent on /sys/class/tty's tty0..tty63. The victim
 * here is nd_modem__pcm_port(): a miss falls through to a hardcoded
 * /dev/ttyUSB4 and stops being a detection at all, so a phone whose PCM port
 * enumerated as anything else got a call with no audio and no error. */
static size_t sorted_listdir(const char *dir, const char *prefix,
                             char names[][ND_MODEM_PORT_MAX], size_t max)
{
    char resolved[ND_PATH_MAX];
    size_t plen = (prefix != NULL) ? strlen(prefix) : 0u;
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
        if (plen > 0u && strncmp(ent->d_name, prefix, plen) != 0)
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

    n = sorted_listdir(ND_MODEM_TTY_DIR, "ttyUSB", names, ND_ARRAY_LEN(names));
    for (i = 0u; i < n; i++) {
        int32_t iface;

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
    n_nodes = sorted_listdir(dir, "pcm", nodes, ND_ARRAY_LEN(nodes));
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

    n_cards = sorted_listdir(ND_MODEM_ASOUND_DIR, "card", cards, ND_ARRAY_LEN(cards));

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

/* ============ EVERY WAY OUT OF start_mic_pipe(), AND WHY IT SAYS SO ======
 *
 * It had four returns that left mic_live false, and only two of them logged
 * anything a person could act on -- the path-resolve failure logged NOTHING
 * AT ALL. That mattered more than tidiness, because the retry watchdog in
 * nd_modem__watch_audio_proc() was gated on mic_live: a pipe that DIED was
 * retried three times, and a pipe that never started was not retried at all.
 * So one transient miss -- a card halfway through re-enumerating, an arecord
 * that lost a race for the device -- cost the whole call, silently, and the
 * only evidence was a caller saying they could not be heard.
 *
 * So every failure now goes through here, and it does two things: it names
 * itself in the log, and it decides whether the mic is owed another try.
 *
 * `retryable` is the whole judgement. A capture device that is not there yet
 * may be there in three seconds, and that is exactly the re-enumeration case.
 * system.hw.modem_mic_device=OFF will still be OFF in three seconds, and
 * retrying a deliberate configuration until the strikes run out would fill
 * the log arguing with the owner.
 *
 * The count is ND_MIC_GIVE_UP_AFTER, shared with the pipe-died path, because
 * it is the same promise: three tries and the call carries on listen-only
 * rather than spinning for its whole length. nd_modem__start_mic() zeroes it
 * at the start of each call, so the three are per call.
 */
static void mic_start_failed(nd_modem *m, bool retryable)
{
    m->mic_pid = -1;
    m->mic_live = false;
    m->mic_retry_at = 0.0;
    if (!retryable)
        return;

    m->mic_fails++;
    if (m->mic_fails >= ND_MIC_GIVE_UP_AFTER) {
        nd_log(ND_LOG_MODEM, "Mic uplink failed to start %d times; call continues listen-only.",
               (int)m->mic_fails);
        return;
    }
    m->mic_retry_at = nd_modem__now() + ND_AUDIO_RESTART_HOLDOFF_S;
}

/* ============ THE MIXER, BEFORE EVERY arecord ============
 *
 * This is the fix for the intermittent dead microphone, and it is three lines
 * because the whole bug was that nobody owned the mixer after boot.
 *
 * /etc/init.d/S17audio turns the capture switch on and sets the capture level
 * ONCE, out of its `start` case, out of rcS. There is no alsactl in this
 * image, no asound.state, and no udev rule that re-runs it -- its own comment
 * says "NOTHING ELSE IN THIS IMAGE DOES". The header of this file already
 * records that this card "re-enumerates once per call", and a C-Media adapter
 * that re-enumerates comes back with the driver's defaults: capture switch
 * OFF, capture volume ZERO. arecord then opens it, reports success, and
 * streams a flat line at the far end for the rest of the session. Every layer
 * says it worked.
 *
 * So the mixer is re-applied here instead of trusted: at the start of every
 * call, on whatever card the uplink is about to open, from the level the
 * owner set.
 *
 * ============ WHAT THIS COSTS THE CALL, AS ONE NUMBER ============
 *
 * A handful of short-lived amixer children, and they are WAITED FOR on the
 * modem thread -- which nothing else on this path is. spawn_quiet() starts
 * arecord and aplay and never looks back; this blocks.
 *
 * That matters because of who is behind it. start_mic_pipe() runs inside
 * nd_modem_poll(), and nd_modem.c's submit() makes every UI-thread modem call
 * -- hangup, answer, dial -- wait on done_cv until the tick it landed in has
 * finished. So whatever this costs is time the END key does not respond, and
 * a call that will not hang up is a worse fault than a call nobody can hear.
 *
 * nd_mic_apply_gain() therefore takes ONE deadline for the listing and every
 * cset together: ND_MIC_MIXER_BUDGET_S + ND_MIC_MIXER_REAP_S, 1.0 s, whatever
 * the card publishes and however badly amixer behaves. It is stated here as
 * well as in nd_mic.h because a per-child bound written down on its own is
 * what let this reach nine children of six and a half seconds -- 58 s in one
 * tick -- before anybody multiplied it out.
 *
 * ND_MIC_GIVE_UP_AFTER attempts per call means the mixer is re-applied on
 * each retry, so the per-CALL total is three of those. That is deliberate and
 * it is not folded into one: each retry is a separate tick, separated by
 * ND_AUDIO_RESTART_HOLDOFF_S, so no single tick pays more than the 1.0 s
 * above -- and skipping the re-apply on a retry would break the fix outright.
 * The commonest retry is the AUTO path finding no capture device and then
 * finding one three seconds later, which IS a card that has just finished
 * re-enumerating: it is sitting at the driver's defaults, muted, and it is
 * the one attempt that most needs the mixer. "Re-applied before every
 * arecord" at the top of this file is the rule, and the retry is an arecord.
 *
 * The gain comes from m->mic_gain, cached at nd_modem__create(). It is NOT
 * read here: R-24 means an nd_settings_get() rewrites settings.prop with an
 * fsync whatever key it was asked for, and doing that once per call is flash
 * wear on UBIFS for a number that changes about once in the life of a phone.
 *
 * A failure is logged by nd_mic_apply_gain() and then ignored on purpose. A
 * card that refuses a cset may still record perfectly well -- the onboard
 * codec publishes no capture control at all -- and refusing to open it would
 * turn "possibly quiet" into "definitely nothing".
 */
static void apply_capture_mixer(nd_modem *m, const char *device)
{
    int32_t card;

    if (!nd_mic_card_of(device, &card)) {
        /* system.hw.modem_mic_device can be set to anything arecord takes,
         * "hw:1,0" and "default" included, and neither carries a card number
         * in a form this can read. Not an error: the owner asked for that
         * device by name and gets it, unmixed. */
        nd_log(ND_LOG_MODEM, "Mic device %s carries no card number; capture mixer left alone.",
               device);
        return;
    }
    (void)nd_mic_apply_gain(card, m->mic_gain);
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
            /* Not retryable: this one is a decision, not a fault. */
            mic_start_failed(m, false);
            return;
        }
        if (strcmp(upper, "AUTO") == 0) {
            if (!nd_modem__find_capture_device(device, sizeof device)) {
                nd_log(ND_LOG_MODEM, "No ALSA capture device found (arecord -l); will look again.");
                /* THE re-enumeration case. A card that is halfway through
                 * coming back has no pcm*c node for a moment, and three
                 * seconds later it has one. */
                mic_start_failed(m, true);
                return;
            }
            nd_log(ND_LOG_MODEM, "Mic auto-detected: %s", device);
        }
    }

    apply_capture_mixer(m, device);

    (void)snprintf(rate, sizeof rate, "%d", (int)m->pcm_rate);
    if (nd_path_resolve(resolved, sizeof resolved, port) != ND_OK) {
        /* This branch logged nothing whatsoever. It is reachable when the
         * port string is longer than ND_PATH_MAX once ND_ROOT is prepended,
         * which is a configuration mistake nobody could see from the phone. */
        nd_log_err(ND_LOG_MODEM, "Mic uplink: PCM port %s does not resolve to a path.", port);
        mic_start_failed(m, false);
        return;
    }

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
        m->mic_retry_at = 0.0;
        m->mic_started_at = nd_modem__now();
        nd_log(ND_LOG_MODEM, "Mic uplink: arecord -D %s -> %s (%d Hz %s).", device, port,
               (int)m->pcm_rate, ND_MODEM_PCM_FORMAT);
    } else {
        nd_log(ND_LOG_MODEM, "Mic uplink unavailable (arecord: %s); retrying.", strerror(errno));
        /* Retryable: the common cause is the device being busy for a beat,
         * and a missing arecord simply uses up the three strikes at once. */
        mic_start_failed(m, true);
    }
}

/* ------------------------------------------------------------------ *
 * The port, before every start
 * ------------------------------------------------------------------ */

/* Find the PCM port, put it in raw mode, optionally discard whatever is
 * waiting in it, and record it as the active port. Called before EVERY pipe
 * start, not once per call, because the flush is the point: a speaker that
 * starts reading from an empty buffer starts on a frame boundary. The mic
 * does not ask for the flush -- discarding unread downlink while the speaker
 * is reading it would only cost a few milliseconds of the far end. */
static bool pcm_port_ready(nd_modem *m, bool flush_input)
{
    char port[ND_MODEM_PORT_MAX];
    char resolved[ND_PATH_MAX];
    struct termios t;
    int fd;

    if (m->pcm_active)
        (void)nd_strlcpy(port, m->active_pcm_port, sizeof port);
    else
        nd_modem__pcm_port(port, sizeof port);
    if (!nd_path_exists(port)) {
        nd_log(ND_LOG_MODEM, "PCM port %s not found; call audio unavailable.", port);
        return false;
    }

    /* Opened only to configure the device, then closed again. */
    if (nd_path_resolve(resolved, sizeof resolved, port) != ND_OK)
        return false;
    fd = open(resolved, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || tcgetattr(fd, &t) != 0) {
        nd_log(ND_LOG_MODEM, "PCM port setup failed: %s", strerror(errno));
        if (fd >= 0)
            (void)close(fd);
        return false;
    }
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = CS8 | CREAD | CLOCAL; /* no cfsetspeed here -- see the header */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        nd_log(ND_LOG_MODEM, "PCM port setup failed: %s", strerror(errno));
        (void)close(fd);
        return false;
    }
    if (flush_input)
        (void)tcflush(fd, TCIFLUSH);
    (void)close(fd);

    (void)nd_strlcpy(m->active_pcm_port, port, sizeof m->active_pcm_port);
    m->pcm_active = true;
    return true;
}

/* SIGKILL one child and collect it. A pid of -1 would signal every process
 * we may signal and 0 our own group, so the guard is not decoration. */
static void kill_child(pid_t *pid, bool *live)
{
    nd_proc_status st;

    if (*live && *pid > 0) {
        (void)kill(*pid, SIGKILL);
        (void)nd_proc_wait(*pid, 1.0, &st);
    }
    *pid = -1;
    *live = false;
}

/* ------------------------------------------------------------------ *
 * The three starts
 * ------------------------------------------------------------------ */

void nd_modem__start_speaker(nd_modem *m)
{
    if (m->audio_live)
        return;
    if (!pcm_port_ready(m, /*flush_input=*/true))
        return;
    start_speaker_pipe(m, m->active_pcm_port);
}

void nd_modem__restart_speaker(nd_modem *m)
{
    kill_child(&m->audio_pid, &m->audio_live);
    if (!pcm_port_ready(m, /*flush_input=*/true))
        return;
    start_speaker_pipe(m, m->active_pcm_port);
}

void nd_modem__start_mic(nd_modem *m)
{
    if (m->mic_live)
        return;
    if (!pcm_port_ready(m, /*flush_input=*/false)) {
        /* pcm_port_ready() logs which of its own two failures this was. The
         * mic is not owed a retry for it: without a PCM port there is no call
         * audio in either direction, and the speaker path drives what happens
         * next. */
        m->mic_retry_at = 0.0;
        return;
    }
    m->mic_fails = 0;
    m->mic_retry_at = 0.0;
    start_mic_pipe(m, m->active_pcm_port);
}

/* ------------------------------------------------------------------ *
 * _stop_call_audio, line 746
 * ------------------------------------------------------------------ */

void nd_modem__stop_call_audio(nd_modem *m)
{
    bool stopped = m->audio_live || m->mic_live;

    kill_child(&m->audio_pid, &m->audio_live);
    kill_child(&m->mic_pid, &m->mic_live);
    if (stopped)
        nd_log(ND_LOG_MODEM, "Call audio stopped.");

    m->active_pcm_port[0] = '\0';
    m->pcm_active = false;
    /* Nothing is owed to a call that is over. */
    m->mic_retry_at = 0.0;
    m->pcm_setup_pending = false;
    m->audio_connect_pending = false;
    m->pcm_reg_ok = false;
    m->next_pcm_try = 0.0;
    m->clcc_empty = 0;
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
        /* Through the restart, for the flush: whatever piled up in the port
         * while nobody was reading is exactly the odd-byte hazard. */
        nd_modem__restart_speaker(m);
    }
    /* Both branches run in the same call and both may re-arm the holdoff. */
    if (m->mic_live && nd_proc_wait(m->mic_pid, 0.0, &st) == ND_OK) {
        int rc = child_rc(&st);

        m->mic_pid = -1;
        m->mic_live = false;
        /* The strikes are for a pipe that cannot get going, not for one that
         * carried a whole conversation and then hiccupped -- a card that
         * re-enumerates once per call would otherwise use up the three over
         * three calls and leave every call after that listen-only. */
        if (now - m->mic_started_at >= ND_MIC_STABLE_S)
            m->mic_fails = 0;
        m->mic_fails++;
        if (m->mic_fails >= ND_MIC_GIVE_UP_AFTER) {
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

    /* AND THE MIC THAT NEVER STARTED. Everything above is gated on mic_live,
     * which is the bug this closes: a start that failed left no child to
     * reap, so no branch here ever ran and the call finished listen-only over
     * a condition that had cleared seconds later.
     *
     * mic_retry_at is set only by mic_start_failed(), only for a failure it
     * judged transient, and only while the strikes are left -- so this cannot
     * spin, cannot fire outside a call (the early return above needs
     * pcm_active, which _stop_call_audio() clears), and cannot touch the
     * speaker's own restart timer. It is cleared before the attempt so that a
     * second failure has to arm it again. */
    if (!m->mic_live && m->mic_retry_at > 0.0 && now >= m->mic_retry_at) {
        m->mic_retry_at = 0.0;
        start_mic_pipe(m, m->active_pcm_port);
    }
}
