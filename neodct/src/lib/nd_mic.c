/* nd_mic.c -- see nd_mic.h. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_mic.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_vclock.h"

static bool all_digits(const char *s, size_t n)
{
    size_t i;

    if (n == 0u)
        return false;
    for (i = 0u; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/* "pcm0c" -> "0", and only when the node really is a capture one. The trailing
 * c is the whole distinction between a microphone and a speaker here. */
static bool capture_index(const char *name, char *out, size_t out_sz)
{
    size_t len = strlen(name);

    if (len < 5u || strncmp(name, "pcm", 3u) != 0 || name[len - 1u] != 'c')
        return false;
    if (!all_digits(&name[3], len - 4u))
        return false;
    if (len - 4u >= out_sz)
        return false;
    memcpy(out, &name[3], len - 4u);
    out[len - 4u] = '\0';
    return true;
}

/* /proc/asound/cardN/id, stripped of its newline. Empty when the file is not
 * there, which the caller shows as the device string instead -- a card with no
 * name is still a card you can record from. */
static void read_card_id(const char *card_dir, char *out, size_t out_sz)
{
    char path[512];
    FILE *f;
    size_t n;

    out[0] = '\0';
    if (snprintf(path, sizeof path, "%s/id", card_dir) < 0)
        return;
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    (void)fclose(f);
    while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r' || out[n - 1u] == ' ')) {
        n--;
        out[n] = '\0';
    }
}

size_t nd_mic_scan(const char *asound_root, nd_mic_device *out, size_t max)
{
    DIR *root;
    struct dirent *card;
    size_t found = 0u;

    if (asound_root == NULL || out == NULL || max == 0u)
        return 0u;

    root = opendir(asound_root);
    if (root == NULL)
        return 0u;

    while (found < max && (card = readdir(root)) != NULL) {
        char dir[512];
        char number[8];
        DIR *inside;
        struct dirent *node;
        size_t len = strlen(card->d_name);

        if (len < 5u || strncmp(card->d_name, "card", 4u) != 0)
            continue;
        if (!all_digits(&card->d_name[4], len - 4u))
            continue;
        /* Copied into a bounded buffer rather than pointed at: d_name is 256
         * bytes and the compiler is right that "card" plus 251 digits would
         * not fit the device string. No such card exists; refusing it costs
         * nothing and keeps the format checkable. */
        if (len - 4u >= sizeof number)
            continue;
        memcpy(number, &card->d_name[4], len - 4u);
        number[len - 4u] = '\0';
        if (snprintf(dir, sizeof dir, "%s/%s", asound_root, card->d_name) < 0)
            continue;

        inside = opendir(dir);
        if (inside == NULL)
            continue;
        while (found < max && (node = readdir(inside)) != NULL) {
            char index[8];

            if (!capture_index(node->d_name, index, sizeof index))
                continue;
            (void)snprintf(out[found].device, sizeof out[found].device, "plughw:%s,%s", number,
                           index);
            read_card_id(dir, out[found].label, sizeof out[found].label);
            found++;
        }
        (void)closedir(inside);
    }
    (void)closedir(root);
    return found;
}

size_t nd_mic_reduce(const int16_t *samples, size_t n_samples, nd_mic_column *out, size_t columns)
{
    size_t per;
    size_t c;

    if (samples == NULL || out == NULL || columns == 0u)
        return 0u;

    /* Fewer samples than columns is a short read, which the first read off a
     * freshly started arecord routinely is. One sample per column then, and
     * only as many columns as there are samples: stretching them across the
     * whole screen would draw a waveform nobody captured. */
    if (n_samples < columns)
        columns = n_samples;
    if (columns == 0u)
        return 0u;
    per = n_samples / columns;

    for (c = 0u; c < columns; c++) {
        const int16_t *block = &samples[c * per];
        int16_t lo = block[0];
        int16_t hi = block[0];
        size_t i;

        for (i = 1u; i < per; i++) {
            if (block[i] < lo)
                lo = block[i];
            if (block[i] > hi)
                hi = block[i];
        }
        out[c].min = lo;
        out[c].max = hi;
    }
    return columns;
}

nd_err nd_mic_record_command(nd_mic_command *out, const char *device, int rate)
{
    size_t n = 0u;

    /* No fallback to "default". On a phone with QEMU's playback-only card 0 in
     * it, "default" is a device arecord cannot capture from, and the failure
     * arrives as silence rather than as an error. */
    if (out == NULL || device == NULL || device[0] == '\0' || rate <= 0)
        return ND_ERR_INVAL;

    if (nd_snprintf(out->rate, sizeof out->rate, "%d", rate) != ND_OK)
        return ND_ERR_TOOLONG;

    out->argv[n++] = "arecord";
    out->argv[n++] = "-q"; /* its chatter would land in the middle of the PCM */
    out->argv[n++] = "-t";
    out->argv[n++] = "raw";
    out->argv[n++] = "-f";
    out->argv[n++] = ND_MIC_FORMAT;
    out->argv[n++] = "-r";
    out->argv[n++] = out->rate;
    out->argv[n++] = "-c";
    out->argv[n++] = "1";
    out->argv[n++] = "-D";
    out->argv[n++] = device;
    out->argv[n] = NULL; /* no output file: the stream comes back on stdout */
    return ND_OK;
}

int32_t nd_mic_sample_y(int16_t sample, int32_t top, int32_t height)
{
    int32_t middle;
    int32_t half;
    int32_t offset;

    if (height <= 0)
        return top;

    /* An odd height has a true middle row; an even one rounds down, which puts
     * silence one row above centre rather than between two rows. */
    half = (height - 1) / 2;
    middle = top + half;

    /* The two halves are scaled against different denominators, deliberately.
     * Signed 16-bit runs -32768..32767, so there is no positive twin for the
     * most negative sample: one divisor for both ends leaves one of them a row
     * short of the edge. A loud signal touching the edge is the whole point of
     * a level display, so each end is scaled against its own full scale. */
    if (sample >= 0)
        offset = (int32_t)(((int64_t)sample * half) / 32767);
    else
        offset = (int32_t)(((int64_t)sample * half) / 32768);
    return middle - offset;
}

/* ------------------------------------------------------------------ *
 * The mixer
 * ------------------------------------------------------------------ */

/* The two suffixes, complete with the closing quote amixer prints. The quote
 * is load-bearing: without it "Mic Capture Volume Enum" -- a control some
 * codecs publish for the input SOURCE rather than its level -- would match a
 * prefix test and be cset to a percentage it has no idea what to do with. */
#define MIC_SUFFIX_SWITCH "Capture Switch'"
#define MIC_SUFFIX_VOLUME "Capture Volume'"

/* "numid=6,iface=MIXER,name='...'" -> 6. The comma is required, so a line
 * that merely starts with the word is not mistaken for a control. */
static bool parse_numid(const char *line, int32_t *out)
{
    size_t i;
    int32_t v = 0;

    if (strncmp(line, "numid=", 6u) != 0)
        return false;
    for (i = 6u; line[i] >= '0' && line[i] <= '9'; i++) {
        if (v > 99999) /* nothing real, and it keeps the multiply in range */
            return false;
        v = (v * 10) + (line[i] - '0');
    }
    if (i == 6u || line[i] != ',')
        return false;
    *out = v;
    return true;
}

static bool ends_with(const char *line, size_t len, const char *suffix)
{
    size_t slen = strlen(suffix);

    return len >= slen && strcmp(&line[len - slen], suffix) == 0;
}

size_t nd_mic_parse_controls(const char *text, nd_mic_control *out, size_t max)
{
    const char *p;
    size_t found = 0u;

    if (text == NULL || out == NULL || max == 0u)
        return 0u;

    for (p = text; *p != '\0' && found < max;) {
        char line[256];
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        const char *next = (nl != NULL) ? (nl + 1) : (p + len);
        int32_t numid = 0;

        /* An over-long line is skipped whole rather than truncated: a
         * truncated line still ends in something, and the whole test below is
         * what a line ENDS with. No real control line is anywhere near this. */
        if (len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';
            while (len > 0u &&
                   (line[len - 1u] == '\r' || line[len - 1u] == ' ' || line[len - 1u] == '\t'))
                line[--len] = '\0';

            if (parse_numid(line, &numid)) {
                if (ends_with(line, len, MIC_SUFFIX_SWITCH)) {
                    out[found].numid = numid;
                    out[found].kind = ND_MIC_CTL_SWITCH;
                    found++;
                } else if (ends_with(line, len, MIC_SUFFIX_VOLUME)) {
                    out[found].numid = numid;
                    out[found].kind = ND_MIC_CTL_VOLUME;
                    found++;
                }
            }
        }
        p = next;
    }
    return found;
}

nd_err nd_mic_controls_command(nd_mic_mixer_command *out, int32_t card)
{
    size_t n = 0u;

    if (out == NULL || card < 0 || card > ND_MIC_CARD_MAX)
        return ND_ERR_INVAL;
    if (nd_snprintf(out->card, sizeof out->card, "%d", (int)card) != ND_OK)
        return ND_ERR_TOOLONG;
    out->numid[0] = '\0';
    out->value[0] = '\0';

    out->argv[n++] = "amixer";
    out->argv[n++] = "-c";
    out->argv[n++] = out->card;
    out->argv[n++] = "controls";
    out->argv[n] = NULL;
    return ND_OK;
}

nd_err nd_mic_cset_command(nd_mic_mixer_command *out, int32_t card, const nd_mic_control *ctl,
                           int32_t percent)
{
    size_t n = 0u;

    if (out == NULL || ctl == NULL)
        return ND_ERR_INVAL;
    /* REFUSED, not clamped. Every one of these numbers reaches an execve()
     * argument, and the percentage in particular arrives from settings.prop
     * on the writable partition. A clamp would turn a settings file that has
     * been edited into nonsense into a silent success; the caller wants to
     * know, and nd_mic_gain_from_setting() is where a bad value is turned
     * into a good one on purpose. */
    if (card < 0 || card > ND_MIC_CARD_MAX || ctl->numid < 0)
        return ND_ERR_INVAL;
    if (ctl->kind != ND_MIC_CTL_SWITCH && ctl->kind != ND_MIC_CTL_VOLUME)
        return ND_ERR_INVAL;
    if (ctl->kind == ND_MIC_CTL_VOLUME && (percent < ND_MIC_GAIN_MIN || percent > ND_MIC_GAIN_MAX))
        return ND_ERR_INVAL;

    if (nd_snprintf(out->card, sizeof out->card, "%d", (int)card) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(out->numid, sizeof out->numid, "numid=%d", (int)ctl->numid) != ND_OK)
        return ND_ERR_TOOLONG;
    if (ctl->kind == ND_MIC_CTL_VOLUME) {
        if (nd_snprintf(out->value, sizeof out->value, "%d%%", (int)percent) != ND_OK)
            return ND_ERR_TOOLONG;
    } else {
        (void)nd_strlcpy(out->value, "on", sizeof out->value);
    }

    out->argv[n++] = "amixer";
    out->argv[n++] = "-c";
    out->argv[n++] = out->card;
    out->argv[n++] = "cset";
    out->argv[n++] = out->numid;
    out->argv[n++] = out->value;
    out->argv[n] = NULL;
    return ND_OK;
}

int32_t nd_mic_gain_from_setting(const char *value)
{
    int32_t v = 0;
    size_t i = 0u;

    if (value == NULL)
        return ND_MIC_GAIN_DEFAULT;
    while (value[i] == ' ' || value[i] == '\t')
        i++;
    /* Digits only, and at least one. That is what rejects "-5", "high" and
     * "" in one test: a leading '-' is not a digit, so a negative gain never
     * reaches the arithmetic below and cannot be read as an option by the
     * program this ends up in front of. */
    if (value[i] < '0' || value[i] > '9')
        return ND_MIC_GAIN_DEFAULT;
    for (; value[i] >= '0' && value[i] <= '9'; i++) {
        v = (v * 10) + (value[i] - '0');
        if (v > ND_MIC_GAIN_MAX)
            return ND_MIC_GAIN_DEFAULT;
    }
    while (value[i] == ' ' || value[i] == '\t' || value[i] == '\r' || value[i] == '\n')
        i++;
    if (value[i] != '\0')
        return ND_MIC_GAIN_DEFAULT; /* "80%", "80 dB", "8 0" */
    return v;
}

bool nd_mic_card_of(const char *device, int32_t *out)
{
    int card = 0;

    if (device == NULL || out == NULL)
        return false;
    if (sscanf(device, "plughw:%d,", &card) != 1)
        return false;
    if (card < 0 || card > ND_MIC_CARD_MAX)
        return false;
    *out = card;
    return true;
}

/* nd_proc_spawn() execve()s a path rather than searching PATH, so the search
 * happens here. The fourth copy of these eight lines (nd_modem_audio.c,
 * nd_notify.c, MusicPlayer/audio.c); they are static in all of them for the
 * reason nd_notify.c gives, and a shared header is still not worth the
 * agreement it would need. */
static bool which_amixer(char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    const char *p;

    if (path == NULL || path[0] == '\0')
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (p = path;;) {
        const char *colon = strchr(p, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - p) : strlen(p);
        const char *dir = (len == 0u) ? "." : p; /* an empty entry means "." */
        size_t dlen = (len == 0u) ? 1u : len;

        if (nd_snprintf(out, out_sz, "%.*s/amixer", (int)dlen, dir) == ND_OK &&
            access(out, X_OK) == 0)
            return true;
        if (colon == NULL)
            break;
        p = colon + 1;
    }
    out[0] = '\0';
    return false;
}

/* One amixer child, with its stdout captured when `capture` is given. The
 * exit status, or -1 when it could not be started at all (errno says why).
 *
 * `deadline` is an nd_time_monotonic() instant and it belongs to the WHOLE
 * operation rather than to this child: see the budget note in nd_mic.h. The
 * spawn, the read and the wait are all inside it, so a caller whose earlier
 * children already spent the budget pays nothing here at all -- the child is
 * not even started -- and the one child that runs out of it costs one
 * SIGKILL and ND_MIC_MIXER_REAP_S on top.
 *
 * The shape is nd_btaudio_run()'s, which does the same to bluetoothctl, with
 * one difference: the read is BOUNDED. nd_mic_apply_gain() is called from
 * nd_modem_audio.c's start_mic_pipe(), which runs on the modem thread between
 * the call connecting and the uplink starting, and a child that never writes
 * and never exits would hold that thread -- and, through nd_modem.c's
 * submit(), the UI's END key -- for the rest of the call.
 */
static int run_amixer(const nd_mic_mixer_command *cmd, char *capture, size_t cap_n, double deadline)
{
    char exe[ND_PATH_MAX];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int pipefd[2] = {-1, -1};
    int devnull = -1;
    int status = -1;
    size_t used = 0u;

    if (cmd == NULL)
        return -1;
    if (capture != NULL && cap_n > 0u)
        capture[0] = '\0';
    /* Checked BEFORE the fork, not after: the budget is spent, and a child
     * started now would have to be killed on its first breath anyway. This is
     * what stops ND_MIC_CONTROLS_MAX csets multiplying into a per-child
     * timeout each. */
    if (nd_time_monotonic() >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (!which_amixer(exe, sizeof exe)) {
        errno = ENOENT;
        return -1;
    }

    devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0)
        return -1;
    /* O_CLOEXEC on both: the child must not inherit the READ end, or the loop
     * below never sees end-of-file and the wait never comes. */
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        (void)close(devnull);
        return -1;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = cmd->argv;
    spec.owner = ND_OWNER_AUDIO;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = pipefd[1];
    /* stderr to /dev/null: amixer's complaint about a control that has gone
     * away is drawn over the serial console in the middle of a call, and the
     * exit status already says it failed. */
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = devnull;
    spec.n_fds = 2u;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        pid = -1;
        goto done;
    }

    (void)close(pipefd[1]);
    pipefd[1] = -1;

    for (;;) {
        struct pollfd pfd;
        double left = deadline - nd_time_monotonic();
        ssize_t got;
        int ready;

        if (left <= 0.0)
            break;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1u, (int)(left * 1000.0));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;

        if (capture == NULL || cap_n == 0u || used + 1u >= cap_n) {
            char sink[256];

            got = read(pipefd[0], sink, sizeof sink);
        } else {
            got = read(pipefd[0], capture + used, cap_n - 1u - used);
            if (got > 0)
                used += (size_t)got;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
    }
    if (capture != NULL && cap_n > 0u)
        capture[used] = '\0';

    memset(&st, 0, sizeof st);
    {
        double left = deadline - nd_time_monotonic();

        if (left < 0.0)
            left = 0.0;
        if (nd_proc_wait(pid, left, &st) == ND_OK) {
            status = st.signalled ? -st.signo : st.exit_status;
        } else {
            /* Out of budget with the child still there. SIGKILL rather than
             * nd_proc_terminate(), whose own sequence is SIGTERM, half a
             * second of grace, SIGKILL and then two more seconds of waiting:
             * that tail is 2.5 s of exactly the stall this budget exists to
             * bound, and amixer holds nothing a graceful exit would tidy up.
             * A child that survives even ND_MIC_MIXER_REAP_S is left to the
             * SIGCHLD reaper. */
            (void)kill(pid, SIGKILL);
            (void)nd_proc_wait(pid, ND_MIC_MIXER_REAP_S, &st);
        }
    }
    pid = -1;

done:
    if (pipefd[0] >= 0)
        (void)close(pipefd[0]);
    if (pipefd[1] >= 0)
        (void)close(pipefd[1]);
    if (devnull >= 0)
        (void)close(devnull);
    return status;
}

/* ============ THE _until() PAIR ============
 *
 * The three public entry points below are the same two operations with one
 * difference: who owns the deadline. Each of them takes ONE, and apply_gain()
 * hands the SAME one to both halves -- which is the whole point, because a
 * fresh budget per half is a budget that multiplies. See nd_mic.h.
 */
static nd_err list_controls_until(int32_t card, nd_mic_control *out, size_t max, size_t *n_out,
                                  double deadline)
{
    nd_mic_mixer_command cmd;
    /* A C-Media card lists seven controls in about 300 bytes. 4 KB is room
     * for a codec with ten times as many and is one page. */
    char listing[4096];
    size_t n;
    int rc;

    if (out == NULL || max == 0u || n_out == NULL)
        return ND_ERR_INVAL;
    *n_out = 0u;
    if (nd_mic_controls_command(&cmd, card) != ND_OK)
        return ND_ERR_INVAL;

    rc = run_amixer(&cmd, listing, sizeof listing, deadline);
    if (rc != 0) {
        if (rc < 0)
            nd_log_err(ND_LOG_AUDIO, "amixer -c %d controls could not run: %s", (int)card,
                       strerror(errno));
        else
            nd_log_err(ND_LOG_AUDIO, "amixer -c %d controls exited %d", (int)card, rc);
        return ND_ERR_IO;
    }

    n = nd_mic_parse_controls(listing, out, max);
    if (n == 0u) {
        /* Not a failure. A card whose driver publishes no capture control has
         * nothing to raise, and the phone's own onboard codec is exactly
         * that -- saying so once is more use than an error nobody can act on. */
        nd_log(ND_LOG_AUDIO, "Card %d publishes no capture control; nothing to set.", (int)card);
        return ND_ERR_NOTFOUND;
    }
    *n_out = n;
    return ND_OK;
}

nd_err nd_mic_list_controls(int32_t card, nd_mic_control *out, size_t max, size_t *n_out)
{
    return list_controls_until(card, out, max, n_out, nd_time_monotonic() + ND_MIC_MIXER_BUDGET_S);
}

static nd_err set_controls_until(int32_t card, const nd_mic_control *controls, size_t n,
                                 int32_t percent, double deadline)
{
    nd_mic_mixer_command cmd;
    size_t applied = 0u;
    size_t i;

    if (controls == NULL)
        return ND_ERR_INVAL;
    if (percent < ND_MIC_GAIN_MIN || percent > ND_MIC_GAIN_MAX)
        return ND_ERR_INVAL;
    if (n == 0u)
        return ND_ERR_NOTFOUND;

    /* EVERY control given, not the first. A codec with a master "Capture
     * Volume" as well as a per-input "Mic Capture Volume" holds the
     * microphone at zero through whichever one was left there, so raising one
     * of the two is the same silence with more steps. S17audio sets them all
     * for this reason. */
    for (i = 0u; i < n; i++) {
        /* CHECKED BEFORE EACH CHILD. The remaining controls are dropped, not
         * queued for later: the caller is start_mic_pipe() on the modem
         * thread and the call is waiting on it, so the honest answer to "this
         * card is taking too long" is a card that is part-set and a phone
         * that still answers its keys. Said once, with the count, because a
         * line per skipped control on a wedged card is a log nobody reads. */
        if (nd_time_monotonic() >= deadline) {
            nd_log_err(ND_LOG_AUDIO,
                       "Card %d capture: %.1fs mixer budget spent after %u of %u control(s); "
                       "the rest are left as the driver had them.",
                       (int)card, ND_MIC_MIXER_BUDGET_S, (unsigned)i, (unsigned)n);
            break;
        }
        if (nd_mic_cset_command(&cmd, card, &controls[i], percent) != ND_OK)
            continue;
        if (run_amixer(&cmd, NULL, 0u, deadline) != 0) {
            nd_log_err(ND_LOG_AUDIO, "amixer -c %d cset numid=%d %s refused", (int)card,
                       (int)controls[i].numid, cmd.value);
            continue;
        }
        applied++;
    }
    return (applied > 0u) ? ND_OK : ND_ERR_IO;
}

nd_err nd_mic_set_controls(int32_t card, const nd_mic_control *controls, size_t n, int32_t percent)
{
    return set_controls_until(card, controls, n, percent,
                              nd_time_monotonic() + ND_MIC_MIXER_BUDGET_S);
}

nd_err nd_mic_apply_gain(int32_t card, int32_t percent)
{
    nd_mic_control controls[ND_MIC_CONTROLS_MAX];
    /* ONE deadline, taken here and shared by the listing and every cset. This
     * is the line that turns the fifty-eight seconds nd_mic.h describes into
     * the 1.0 s the call path is promised. */
    double deadline = nd_time_monotonic() + ND_MIC_MIXER_BUDGET_S;
    size_t n = 0u;
    nd_err rc;

    if (percent < ND_MIC_GAIN_MIN || percent > ND_MIC_GAIN_MAX)
        return ND_ERR_INVAL;

    rc = list_controls_until(card, controls, ND_ARRAY_LEN(controls), &n, deadline);
    if (rc != ND_OK)
        return rc;

    rc = set_controls_until(card, controls, n, percent, deadline);
    if (rc == ND_OK)
        nd_log(ND_LOG_AUDIO, "Card %d capture: %u control(s) set, level %d%%.", (int)card,
               (unsigned)n, (int)percent);
    return rc;
}
