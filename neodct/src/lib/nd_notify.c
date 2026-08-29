/* nd_notify.c -- Nokia-style notifications, the beeps, and the ringer.
 *
 * A port of System/core/NotifyService/__init__.py (232 lines). Its docstring
 * is the specification for the visible half and is worth keeping in mind
 * while reading this file:
 *
 *     SMS notifications are deliberately NOT modal. Posting one beeps
 *     immediately; the visual part lives on the HOME screen exactly like the
 *     3310 -- "N message(s) received" mid-left with the carrier line hidden,
 *     the softkey turning into "Read", C dismissing the banner while the
 *     messages stay unread in the inbox and the envelope keeps flashing until
 *     they are actually read. Inside apps nothing visual happens: the banner
 *     is waiting when you get back to HOME.
 *
 * None of that geometry is here. nd_ui.c draws it, this file only owns the
 * state it draws from -- which is four fields, none of them time-based and
 * none of them persisted. A reboot clears the banner, and the unread count on
 * the home screen is a separate SQL query, not this counter.
 *
 * ============ THE ONE DELIBERATE DEVIATION: THE RINGER STREAMS ============
 *
 * The Python decodes the entire ringtone into memory before the first note
 * (miniaudio.decode_file at 44100 Hz stereo int16) and then loops the buffer.
 * Measured against the sixteen shipped tones, that costs 599 kB for the
 * default Low.mp3 and 6.4 MB for Tchaikovsky.mp3 -- resident in the CORE
 * process from the instant the phone rings until somebody answers. On a
 * 53 MB phone with a 9 MB target for the whole OS, one allocation bigger than
 * the entire remaining budget is not acceptable. It is risk R-9 in
 * spec-core-services.md and the fix was authorised there.
 *
 * So the decode streams: dr_mp3 / dr_wav pull a few thousand source frames at
 * a time into nd_tone_src (nd_notify_priv.h), which resamples to the same
 * 44100 Hz stereo int16 the Python asked for and seeks back to frame 0 at
 * EOF. Peak cost is about 100 kB for any tone. The loop stays sample-exact,
 * because that is the property the Python's _loop_generator was careful about
 * and a click every four seconds is the thing a ringtone cannot have.
 *
 * ============ AND WHERE THE SAMPLES GO ============
 *
 * The Python opens a miniaudio PlaybackDevice, which is miniaudio's ALSA
 * backend. This writes them to `aplay` over a socket instead, because:
 *
 *   * aplay is ALREADY a hard dependency -- both defconfigs set
 *     BR2_PACKAGE_ALSA_UTILS_APLAY=y and play_tone() below has always used
 *     it -- so nothing new ships;
 *   * it costs a few hundred kB of RSS, not the ~24 MB of the mpv process
 *     the Python's docstring says miniaudio exists to avoid. The reason for
 *     not using mpv is honoured, not the letter of the implementation;
 *   * libneodct then links no audio library at all, which matters because
 *     the golden-frame host has no libasound to link against.
 *
 * A SOCKETPAIR, not a pipe, and send(MSG_NOSIGNAL), not write(). When the
 * ringer is stopped the player dies first and the feeder thread is mid-write;
 * on a pipe that is SIGPIPE, and the only way to suppress SIGPIPE is
 * process-wide, which a library has no business doing to nd-core. On a socket
 * MSG_NOSIGNAL turns the same event into a plain EPIPE on one thread.
 *
 * mpv REMAINS the fallback, reached by exactly the condition that reaches it
 * in the Python: the decode failed. dr_mp3 and dr_wav cannot read .wma,
 * .flac or .ogg, and neither can miniaudio -- a .wma tone already falls
 * through to mpv today, and it still does.
 *
 * ============ LIFETIME ============
 *
 * nd_notify_close() stops the ringer. The ringer owns a joinable thread, a
 * socket and a child process, in that order, and stop_ring() unwinds them in
 * that order: signal the thread, shut the socket down so a blocked send()
 * returns, join, close, then SIGTERM the player with the same 0.3 s grace the
 * Python gives mpv. Nothing here is safe to call from two threads at once;
 * the core owns it from the UI thread.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_notify.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_timeset.h"
#include "nd_types.h"

#include "nd_notify_priv.h"

/* ------------------------------------------------------------------ *
 * The vendored decoders
 * ------------------------------------------------------------------ *
 *
 * dr_mp3 and dr_wav, single-header, public domain / MIT-0, unmodified from
 * upstream so they can be re-pulled without a diff. They are the two formats
 * that must actually decode: MP3 for all sixteen shipped ringtones, WAV for
 * sms.wav, which is the last entry in the fallback chain.
 *
 * The warning set is suspended across them ON PURPOSE and only across them.
 * -Wconversion on somebody else's 14,000-line DSP header produces hundreds of
 * warnings about arithmetic that is correct, and the alternative -- editing a
 * vendored decoder -- is how a port acquires a bug nobody can find upstream.
 * Everything this file writes is compiled with the full set.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"

#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"

#define DR_WAV_NO_WCHAR
#define DR_WAV_IMPLEMENTATION
#include "vendor/dr_wav.h"

#pragma GCC diagnostic pop

/* ------------------------------------------------------------------ *
 * Constants the header promises
 * ------------------------------------------------------------------ */

/* Last-resort ringtones if the configured one is missing or unplayable.
 * ORDER IS LOAD-BEARING and it is the Python's, verbatim: Low.mp3 is the
 * system default ringtone across the board, then the two Nokia tunes, then
 * the SMS chirp -- anything audible beats a silent ring. */
const char *const ND_RING_FALLBACKS[ND_RING_FALLBACK_COUNT] = {
    ND_TONES_DIR "/Low.mp3",
    ND_TONES_DIR "/Nokia Tune.mp3",
    ND_TONES_DIR "/Ring Ring.mp3",
    ND_SMS_TONE,
};

/* A renamed or re-encoded tone (.wma -> .mp3, say) still rings: the same
 * basename is retried with each of these before the fallback chain. */
const char *const ND_RING_EXT_RETRY[ND_RING_EXT_RETRY_COUNT] = {
    ".mp3", ".wav", ".wma", ".flac", ".ogg",
};

/* The last-resort sweep of the tones directory takes only what a player has
 * any chance with. Note .flac and .ogg are NOT here; that asymmetry is the
 * Python's and it is reproduced, not corrected. */
const char *const ND_RING_SWEEP_EXT[ND_RING_SWEEP_EXT_COUNT] = {
    ".mp3",
    ".wav",
    ".wma",
};

/* The feeder thread's stack. It holds a couple of pointers and dr_mp3's
 * decode frames; 128 kB is musl's default and generous for both, and it is
 * set explicitly because glibc's default is 8 MB and MUSL.md asks for the
 * difference to stop mattering. */
#define RING_THREAD_STACK (128u * 1024u)

/* The Python gives mpv 0.3 s to die before SIGKILL. Same number, same place,
 * now also for aplay. */
#define RING_TERM_GRACE 0.3

/* ------------------------------------------------------------------ *
 * The streaming source
 * ------------------------------------------------------------------ */

struct nd_tone_src {
    bool is_mp3;
    union {
        drmp3 mp3;
        drwav wav;
    } d;
    uint32_t rate;     /* the FILE's rate, before conversion  */
    uint32_t channels; /* the FILE's channel count            */

    /* Source frames as decoded, in the file's own channel count. */
    int16_t *stage;
    size_t stage_frames; /* how many are valid   */
    size_t stage_pos;    /* how many are spent   */

    /* Linear resampler. `frac` counts in units of 1/ND_RING_RATE of one
     * output frame, so it is exact integer arithmetic and cannot drift --
     * a float accumulator over a tone looped for a minute does. `a` and `b`
     * are consecutive SOURCE frames, already widened to stereo. */
    uint32_t frac;
    int16_t a[2];
    int16_t b[2];
    bool primed;
    bool empty; /* the file decodes to nothing; see the header */
};

static size_t src_decode(nd_tone_src *s, int16_t *out, size_t frames)
{
    if (s->is_mp3)
        return (size_t)drmp3_read_pcm_frames_s16(&s->d.mp3, (drmp3_uint64)frames, out);
    return (size_t)drwav_read_pcm_frames_s16(&s->d.wav, (drwav_uint64)frames, out);
}

static bool src_rewind(nd_tone_src *s)
{
    if (s->is_mp3)
        return drmp3_seek_to_pcm_frame(&s->d.mp3, 0u) != 0u;
    return drwav_seek_to_pcm_frame(&s->d.wav, 0u) != 0u;
}

/* Refill the staging buffer, wrapping to frame 0 at EOF. THIS is the loop:
 * the frame after the file's last frame is its first, with nothing in
 * between. Returns false only when the file yields no frames at all even
 * from the top, which is the empty-file case. */
static bool src_fill(nd_tone_src *s)
{
    size_t got = src_decode(s, s->stage, ND_TONE_STAGE_FRAMES);

    if (got == 0u) {
        if (!src_rewind(s))
            return false;
        got = src_decode(s, s->stage, ND_TONE_STAGE_FRAMES);
        if (got == 0u)
            return false;
    }
    s->stage_frames = got;
    s->stage_pos = 0u;
    return true;
}

/* One source frame, widened to stereo. A mono file plays down both channels,
 * which is what miniaudio's converter does with nchannels=2; a file with more
 * than two channels keeps the first two, which no shipped tone is. */
static bool src_pull(nd_tone_src *s, int16_t out[2])
{
    const int16_t *f;

    if (s->stage_pos >= s->stage_frames) {
        if (!src_fill(s))
            return false;
    }
    f = s->stage + s->stage_pos * (size_t)s->channels;
    out[0] = f[0];
    out[1] = (s->channels >= 2u) ? f[1] : f[0];
    s->stage_pos++;
    return true;
}

/* x0 + (x1 - x0) * frac/ND_RING_RATE, in integers. frac < ND_RING_RATE, so
 * the result is always between the two samples and always fits int16. */
static int16_t lerp16(int16_t x0, int16_t x1, uint32_t frac)
{
    int32_t d = (int32_t)x1 - (int32_t)x0;
    int64_t step = ((int64_t)d * (int64_t)frac) / (int64_t)ND_RING_RATE;

    return (int16_t)((int32_t)x0 + (int32_t)step);
}

nd_err nd_tone_src_open(nd_tone_src **out, const char *path)
{
    nd_tone_src *s;

    if (out == NULL || path == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    /* owned by the caller; free with nd_tone_src_close() */
    s = calloc(1u, sizeof *s);
    if (s == NULL)
        return ND_ERR_NOMEM;

    if (drmp3_init_file(&s->d.mp3, path, NULL) != 0u) {
        s->is_mp3 = true;
        s->rate = s->d.mp3.sampleRate;
        s->channels = s->d.mp3.channels;
    } else if (drwav_init_file(&s->d.wav, path, NULL) != 0u) {
        s->is_mp3 = false;
        s->rate = s->d.wav.sampleRate;
        s->channels = s->d.wav.channels;
    } else {
        free(s);
        return ND_ERR_UNSUPPORTED;
    }

    if (s->rate == 0u || s->channels == 0u) {
        nd_tone_src_close(s);
        return ND_ERR_PARSE;
    }

    /* owned by s; freed in nd_tone_src_close() */
    s->stage = malloc(ND_TONE_STAGE_FRAMES * (size_t)s->channels * sizeof *s->stage);
    if (s->stage == NULL) {
        nd_tone_src_close(s);
        return ND_ERR_NOMEM;
    }

    *out = s;
    return ND_OK;
}

void nd_tone_src_close(nd_tone_src *s)
{
    if (s == NULL)
        return;
    if (s->is_mp3)
        drmp3_uninit(&s->d.mp3);
    else
        (void)drwav_uninit(&s->d.wav);
    free(s->stage);
    free(s);
}

size_t nd_tone_src_read(nd_tone_src *s, int16_t *out, size_t frames)
{
    size_t i;

    if (s == NULL || out == NULL)
        return 0u;
    if (s->empty)
        return 0u;

    if (!s->primed) {
        if (!src_pull(s, s->a) || !src_pull(s, s->b)) {
            s->empty = true;
            return 0u;
        }
        s->primed = true;
        s->frac = 0u;
    }

    for (i = 0u; i < frames; i++) {
        out[2u * i] = lerp16(s->a[0], s->b[0], s->frac);
        out[2u * i + 1u] = lerp16(s->a[1], s->b[1], s->frac);

        /* At 44100 Hz in, frac is 0 on every output frame and this advances
         * exactly one source frame per output frame -- a bit-exact copy, not
         * a resample. That is why sms.wav's neighbours and Nokia Tune.mp3
         * come out untouched and 48 kHz tones do not. */
        s->frac += s->rate;
        while (s->frac >= (uint32_t)ND_RING_RATE) {
            s->frac -= (uint32_t)ND_RING_RATE;
            s->a[0] = s->b[0];
            s->a[1] = s->b[1];
            if (!src_pull(s, s->b)) {
                s->empty = true;
                return i + 1u;
            }
        }
    }
    return frames;
}

uint32_t nd_tone_src_rate(const nd_tone_src *s)
{
    return (s != NULL) ? s->rate : 0u;
}

uint32_t nd_tone_src_channels(const nd_tone_src *s)
{
    return (s != NULL) ? s->channels : 0u;
}

/* ------------------------------------------------------------------ *
 * The ringer
 * ------------------------------------------------------------------ */

typedef struct {
    nd_tone_src *src;
    int fd;    /* our end of the socketpair; -1 when idle */
    pid_t pid; /* the player; -1 when idle                */
    pthread_t thread;
    bool thread_live;
    volatile sig_atomic_t stop;
    /* The ~64 kB the whole deviation is about. On the heap with the rest of
     * the ringer, never on a stack and never per-callback. */
    int16_t buf[ND_RING_CHUNK_FRAMES * 2u];
} nd_ringer;

struct nd_notify {
    /* Banner state -- the four Python fields, plus the two an event needs
     * that a text does not. */
    const char *kind; /* ND_NOTIFY_KIND_SMS, ND_NOTIFY_KIND_EVENT, or NULL */
    int32_t count;
    int64_t latest;

    /* The reminder's own line. A text's banner is derived entirely from
     * `count`, so it needed no storage; an event's says what it is, and the
     * title has to survive from the poll that found it until the frame that
     * draws it. Truncated to the banner's own width on the way in rather
     * than on the way out, so nothing downstream has to know the limit. */
    char event_title[ND_NOTIFY_LINE_MAX];
    int64_t event_when;

    /* What ringtone_path() last settled on. Kept because the header hands
     * the caller a const char * and the Python hands back a str; the buffer
     * lives as long as the service does. This is the VIRTUAL path, the one
     * settings.prop holds, not the ND_ROOT-resolved one. */
    char ring_path[ND_PATH_MAX];
    bool have_ring_path;

    nd_ringer *ring; /* streaming ringer, NULL when not ringing */
    pid_t mpv_pid;   /* mpv fallback, -1 when not ringing       */
};

/* ------------------------------------------------------------------ *
 * Spawning
 * ------------------------------------------------------------------ */

/* subprocess.Popen(["aplay", ...]) searches PATH; nd_proc_spawn() takes a
 * path and execve()s it, so the search happens here. A miss is the Python's
 * FileNotFoundError. (The same function exists in nd_modem_audio.c. It is
 * eight lines and static in both, which is cheaper than a shared header that
 * two work packages would have had to agree on.) */
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

/* stdout and stderr to /dev/null, exactly as subprocess.DEVNULL does, plus an
 * optional descriptor for the child's stdin. The /dev/null path is NOT
 * ND_ROOT-resolved: it is the child's plumbing, not phone data, and a scratch
 * root has no /dev/null. */
static bool spawn_quiet(const char *const *argv, int stdin_fd, nd_proc_owner owner, pid_t *pid_out)
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
    spec.owner = owner;
    spec.n_fds = 0u;
    if (stdin_fd >= 0) {
        spec.fds[spec.n_fds].child_fd = 0;
        spec.fds[spec.n_fds].our_fd = stdin_fd;
        spec.n_fds++;
    }
    spec.fds[spec.n_fds].child_fd = 1;
    spec.fds[spec.n_fds].our_fd = devnull;
    spec.n_fds++;
    spec.fds[spec.n_fds].child_fd = 2;
    spec.fds[spec.n_fds].our_fd = devnull;
    spec.n_fds++;

    rc = nd_proc_spawn(exe, &spec, pid_out);
    (void)close(devnull);
    return rc == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The feeder thread
 * ------------------------------------------------------------------ */

static void *ring_feed(void *arg)
{
    nd_ringer *r = (nd_ringer *)arg;

    while (r->stop == 0) {
        size_t frames = nd_tone_src_read(r->src, r->buf, ND_RING_CHUNK_FRAMES);
        size_t bytes;
        size_t sent = 0u;

        /* The Python generator's `if total == 0: return`: an empty tone
         * leaves the device running and silent rather than falling back. */
        if (frames == 0u)
            break;

        bytes = frames * 2u * sizeof r->buf[0];
        while (sent < bytes && r->stop == 0) {
            /* MSG_NOSIGNAL, not write(): when stop_ring() kills the player
             * this send is what notices, and on a pipe that would be a
             * process-wide SIGPIPE. See the file header. */
            ssize_t n = send(r->fd, (const char *)r->buf + sent, bytes - sent, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return NULL; /* EPIPE: the player is gone. So are we. */
            }
            if (n == 0)
                return NULL;
            sent += (size_t)n;
        }
    }
    return NULL;
}

static void ringer_free(nd_ringer *r)
{
    if (r == NULL)
        return;

    r->stop = 1;

    /* ORDER MATTERS, and this is the order:
     *
     *   1. kill the player. The feeder is almost certainly blocked in send()
     *      -- a ringtone plays in real time, so the socket fills within a
     *      second and stays full -- and the stop flag is not looked at until
     *      that send returns. What makes it return is the read end closing,
     *      which is what the player dying does. Same 0.3 s grace then SIGKILL
     *      the Python gives mpv.
     *   2. shut our end down too, as a second way for a send that somehow
     *      survived step 1 to come back.
     *   3. join, THEN close the descriptor. Closing an fd another thread is
     *      sitting in send() on is how a number gets recycled underneath it.
     */
    if (r->pid > 0)
        (void)nd_proc_terminate(r->pid, RING_TERM_GRACE, NULL);
    if (r->fd >= 0)
        (void)shutdown(r->fd, SHUT_RDWR);
    if (r->thread_live)
        (void)pthread_join(r->thread, NULL);
    if (r->fd >= 0)
        (void)close(r->fd);
    nd_tone_src_close(r->src);
    free(r);
}

/* Start the in-process streaming ringer. `why` receives a short reason on
 * failure, for the log line that sends us to mpv. */
static bool ringer_start(nd_notify *n, const char *virt_path, const char **why)
{
    char resolved[ND_PATH_MAX];
    char rate[16];
    char buftime[32];
    const char *argv[14];
    nd_ringer *r;
    pthread_attr_t attr;
    sigset_t all;
    sigset_t saved;
    int sv[2];
    size_t stack;
    nd_err rc;

    *why = "internal error";

    if (nd_path_resolve(resolved, sizeof resolved, virt_path) != ND_OK) {
        *why = "path too long";
        return false;
    }

    /* owned here; freed by ringer_free() on every path out */
    r = calloc(1u, sizeof *r);
    if (r == NULL) {
        *why = "out of memory";
        return false;
    }
    r->fd = -1;
    r->pid = -1;

    rc = nd_tone_src_open(&r->src, resolved);
    if (rc != ND_OK) {
        *why = (rc == ND_ERR_UNSUPPORTED) ? "not MP3 or WAV" : nd_strerror(rc);
        ringer_free(r);
        return false;
    }

    if (nd_snprintf(rate, sizeof rate, "%d", ND_RING_RATE) != ND_OK ||
        nd_snprintf(buftime, sizeof buftime, "--buffer-time=%d", ND_RING_BUF_MS * 1000) != ND_OK) {
        ringer_free(r);
        return false;
    }

    /* SOCK_STREAM, not a pipe -- see the file header. CLOEXEC on both ends:
     * nd_proc_spawn dup2()s the one the child needs, which clears the flag on
     * the copy, and everything else must not leak into aplay. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        *why = strerror(errno);
        ringer_free(r);
        return false;
    }

    /* The same S16_LE / 2 channels / 44100 the Python configures its
     * PlaybackDevice with, and the same 500 ms of buffering, expressed the
     * way aplay spells it. */
    argv[0] = "aplay";
    argv[1] = "-q";
    argv[2] = "-t";
    argv[3] = "raw";
    argv[4] = "-f";
    argv[5] = "S16_LE";
    argv[6] = "-c";
    argv[7] = "2";
    argv[8] = "-r";
    argv[9] = rate;
    argv[10] = buftime;
    argv[11] = NULL;

    if (!spawn_quiet(argv, sv[1], ND_OWNER_TONE, &r->pid)) {
        *why = strerror(errno);
        (void)close(sv[0]);
        (void)close(sv[1]);
        r->pid = -1;
        ringer_free(r);
        return false;
    }
    (void)close(sv[1]);
    r->fd = sv[0];
    /* We never read from the player, and shutting the read side down means a
     * player that exits is noticed by the next send() rather than by nothing. */
    (void)shutdown(r->fd, SHUT_RD);

    if (pthread_attr_init(&attr) != 0) {
        ringer_free(r);
        return false;
    }
    stack = RING_THREAD_STACK;
#ifdef PTHREAD_STACK_MIN
    if (stack < (size_t)PTHREAD_STACK_MIN)
        stack = (size_t)PTHREAD_STACK_MIN;
#endif
    (void)pthread_attr_setstacksize(&attr, stack);

    /* A new thread inherits the creator's signal mask, so blocking everything
     * across pthread_create leaves the feeder unable to receive a signal at
     * all. That is what we want: nd-core's SIGCHLD reaper and its SIGTERM
     * handler belong on the main thread, and a SIGCHLD landing here would
     * only turn a send() into an EINTR. Python got this free -- CPython
     * delivers signals to the main thread and nowhere else. */
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &saved);
    if (pthread_create(&r->thread, &attr, ring_feed, r) == 0)
        r->thread_live = true;
    (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    (void)pthread_attr_destroy(&attr);

    if (!r->thread_live) {
        *why = "cannot start the feeder thread";
        ringer_free(r);
        return false;
    }

    n->ring = r;
    return true;
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

nd_err nd_notify_open(nd_notify **out)
{
    nd_notify *n;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    /* owned by the caller; free with nd_notify_close() */
    n = calloc(1u, sizeof *n);
    if (n == NULL)
        return ND_ERR_NOMEM;

    n->kind = NULL;
    n->count = 0;
    n->latest = -1; /* the header's spelling of Python's None */
    n->mpv_pid = -1;

    nd_log(ND_LOG_NOTIFY, "Initializing NotifyService...");
    *out = n;
    return ND_OK;
}

void nd_notify_close(nd_notify *n)
{
    if (n == NULL)
        return;
    nd_notify_stop_ring(n);
    free(n);
}

/* ------------------------------------------------------------------ *
 * Posting, and the banner the home screen draws from
 * ------------------------------------------------------------------ */

/* A banner of a different kind is a different banner: its count does not
 * carry over. nd_notify.h says why the newest news simply wins. Reached only
 * when both kinds exist, so an SMS-only phone follows exactly the path it
 * always did. */
static void take_over(nd_notify *n, const char *kind)
{
    if (n->kind != NULL && strcmp(n->kind, kind) != 0) {
        n->count = 0;
        n->latest = -1;
        n->event_title[0] = '\0';
        n->event_when = 0;
    }
    n->kind = kind;
}

void nd_notify_post_sms(nd_notify *n, int64_t row_id, bool tone)
{
    if (n == NULL)
        return;
    take_over(n, ND_NOTIFY_KIND_SMS);
    n->count++;
    n->latest = row_id;
    if (tone)
        (void)nd_notify_play_tone(n, ND_SMS_TONE);
}

void nd_notify_post_event(nd_notify *n, int64_t row_id, const char *title, int64_t when, bool tone)
{
    if (n == NULL)
        return;
    take_over(n, ND_NOTIFY_KIND_EVENT);
    n->count++;
    n->latest = row_id;
    /* An untitled appointment still has to say something. */
    (void)nd_strlcpy(n->event_title, (title != NULL && title[0] != '\0') ? title : "Reminder",
                     sizeof n->event_title);
    n->event_when = when;
    nd_log(ND_LOG_NOTIFY, "Reminder due (event %lld): %s", (long long)row_id, n->event_title);
    if (tone)
        (void)nd_notify_play_tone(n, ND_NOTIFY_EVENT_TONE);
}

bool nd_notify_active(const nd_notify *n)
{
    return n != NULL && n->kind != NULL;
}

const char *nd_notify_kind(const nd_notify *n)
{
    return (n != NULL) ? n->kind : NULL;
}

int32_t nd_notify_count(const nd_notify *n)
{
    return (n != NULL) ? n->count : 0;
}

int64_t nd_notify_latest_data(const nd_notify *n)
{
    return (n != NULL) ? n->latest : -1;
}

size_t nd_notify_banner_lines(const nd_notify *n, char l1[ND_NOTIFY_LINE_MAX],
                              char l2[ND_NOTIFY_LINE_MAX])
{
    if (l1 == NULL || l2 == NULL)
        return 0u;
    l1[0] = '\0';
    l2[0] = '\0';
    /* banner_lines() returns () for a banner it has no wording for. The
     * comparison the Python needed for one kind is what lets a second one be
     * added here without either drawing the other's words. */
    if (n == NULL || n->kind == NULL)
        return 0u;

    if (strcmp(n->kind, ND_NOTIFY_KIND_SMS) == 0) {
        (void)nd_snprintf(l1, ND_NOTIFY_LINE_MAX, "%d %s", (int)n->count,
                          (n->count == 1) ? "message" : "messages");
        (void)nd_strlcpy(l2, "received", ND_NOTIFY_LINE_MAX);
        return 2u;
    }

    if (strcmp(n->kind, ND_NOTIFY_KIND_EVENT) == 0) {
        if (n->count == 1) {
            /* The one that is due, named, with the time it is due at --
             * which is the whole message on a phone whose banner is two
             * lines. Several at once fall back to counting them, exactly as
             * several texts do. */
            (void)nd_strlcpy(l1, n->event_title, ND_NOTIFY_LINE_MAX);
            nd_timeset_format_clock(l2, ND_NOTIFY_LINE_MAX, (time_t)n->event_when);
        } else {
            (void)nd_snprintf(l1, ND_NOTIFY_LINE_MAX, "%d reminders", (int)n->count);
            (void)nd_strlcpy(l2, "due", ND_NOTIFY_LINE_MAX);
        }
        return 2u;
    }

    return 0u;
}

void nd_notify_dismiss(nd_notify *n)
{
    if (n == NULL)
        return;
    /* C pressed, or the messages were opened. The banner goes; the mail stays
     * unread in the inbox and the envelope keeps flashing, because the unread
     * count is a SQL query and not this counter. A dismissed reminder has no
     * such second life: the appointment is still in the calendar, but nothing
     * on the home screen goes on saying so. */
    n->kind = NULL;
    n->count = 0;
    n->latest = -1;
    n->event_title[0] = '\0';
    n->event_when = 0;
}

/* ------------------------------------------------------------------ *
 * Beeps
 * ------------------------------------------------------------------ */

bool nd_notify_play_tone(nd_notify *n, const char *path)
{
    char resolved[ND_PATH_MAX];
    const char *argv[4];
    pid_t pid;

    ND_UNUSED(n);
    if (path == NULL)
        return false;

    if (!nd_path_exists(path)) {
        nd_log(ND_LOG_NOTIFY, "Tone missing: %s", path);
        return false;
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        nd_log(ND_LOG_NOTIFY, "Tone playback unavailable: %s", nd_strerror(ND_ERR_TOOLONG));
        return false;
    }

    /* Fire and forget, exactly as the Python does: no wait, no reaping here.
     * nd-core's SIGCHLD reaper collects it, which is the one thing the port
     * MUST add -- CPython's subprocess module reaped these opportunistically
     * and C will not, and a DTMF tone fires on every dial-pad keypress. */
    argv[0] = "aplay";
    argv[1] = "-q";
    argv[2] = resolved;
    argv[3] = NULL;
    if (!spawn_quiet(argv, -1, ND_OWNER_TONE, &pid)) {
        nd_log(ND_LOG_NOTIFY, "Tone playback unavailable: aplay: %s", strerror(errno));
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * Which tone rings
 * ------------------------------------------------------------------ */

/* os.path.splitext()'s stem, which is not just "up to the last dot": the dots
 * that START a basename are part of the name, so splitext("/a/.hidden") is
 * ("/a/.hidden", "") and not ("/a/", ".hidden"). Writes the stem into out. */
static void path_stem(const char *path, char *out, size_t out_sz)
{
    const char *slash = strrchr(path, '/');
    const char *base = (slash != NULL) ? slash + 1 : path;
    const char *scan = base;
    const char *dot = NULL;
    const char *p;
    size_t keep;

    while (*scan == '.')
        scan++;
    for (p = scan; *p != '\0'; p++) {
        if (*p == '.')
            dot = p;
    }
    if (dot == NULL) {
        (void)nd_strlcpy(out, path, out_sz);
        return;
    }
    keep = (size_t)(dot - path) + 1u;
    (void)nd_strlcpy(out, path, (keep < out_sz) ? keep : out_sz);
}

/* str.strip(): whitespace off both ends, in place. Python's default set is
 * str.isspace(), which for the ASCII a settings.prop can hold is these six. */
static void str_strip(char *s)
{
    static const char WS[] = " \t\n\r\f\v";
    size_t len = strlen(s);
    size_t start = 0u;

    while (len > 0u && strchr(WS, s[len - 1u]) != NULL)
        len--;
    while (start < len && strchr(WS, s[start]) != NULL)
        start++;
    if (start > 0u)
        memmove(s, s + start, len - start);
    s[len - start] = '\0';
}

static bool ends_with_ci(const char *name, const char *suffix)
{
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    size_t i;

    if (sl > nl)
        return false;
    for (i = 0u; i < sl; i++) {
        char a = name[nl - sl + i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

/* The Python sorts the whole directory and takes the first playable name.
 * Taking the lexicographically smallest playable name is the same answer
 * without holding the listing, which matters because a tones directory on the
 * SD card can be arbitrarily large. strcmp, not a natural sort: byte order is
 * what sorted() does to ASCII names and "Ring Ring.mp3" must keep sorting
 * where it does. */
static bool sweep_tones_dir(char *out, size_t out_sz)
{
    char dirpath[ND_PATH_MAX];
    char best[ND_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    bool found = false;

    if (nd_path_resolve(dirpath, sizeof dirpath, ND_TONES_DIR) != ND_OK)
        return false;
    d = opendir(dirpath);
    if (d == NULL)
        return false;

    best[0] = '\0';
    while ((ent = readdir(d)) != NULL) {
        size_t i;
        bool playable = false;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        for (i = 0u; i < ND_RING_SWEEP_EXT_COUNT; i++) {
            if (ends_with_ci(ent->d_name, ND_RING_SWEEP_EXT[i])) {
                playable = true;
                break;
            }
        }
        if (!playable)
            continue;
        if (!found || strcmp(ent->d_name, best) < 0) {
            (void)nd_strlcpy(best, ent->d_name, sizeof best);
            found = true;
        }
    }
    (void)closedir(d);

    if (!found)
        return false;
    return nd_snprintf(out, out_sz, "%s/%s", ND_TONES_DIR, best) == ND_OK;
}

const char *nd_notify_ringtone_path(nd_notify *n)
{
    char configured[ND_PATH_MAX];
    char stem[ND_PATH_MAX];
    char candidate[ND_PATH_MAX];
    size_t i;

    if (n == NULL)
        return NULL;
    n->have_ring_path = false;
    n->ring_path[0] = '\0';

    /* The Python wraps this in try/except and prints "[NOTIFY] Ringtone
     * setting unreadable (...)" if SettingsStorage raises. nd_settings_get()
     * cannot raise -- it returns the default -- so that branch has no C
     * equivalent and is not reproduced. See OPEN-QUESTIONS N-4. */
    if (nd_settings_get_copy(ND_RING_SETTING, "", configured, sizeof configured) != ND_OK)
        configured[0] = '\0';
    str_strip(configured);

    if (configured[0] != '\0' && nd_path_exists(configured)) {
        (void)nd_strlcpy(n->ring_path, configured, sizeof n->ring_path);
        n->have_ring_path = true;
        return n->ring_path;
    }

    if (configured[0] != '\0') {
        /* Python's {path!r}: a repr, so quoted. Paths with a quote or a
         * backslash in them would be escaped by Python and are not here;
         * no shipped tone has either. */
        nd_log(ND_LOG_NOTIFY, "Ringtone missing: '%s'", configured);

        /* A renamed or re-encoded tone (.wma -> .mp3) still rings: try the
         * same basename with the other known extensions. */
        path_stem(configured, stem, sizeof stem);
        for (i = 0u; i < ND_RING_EXT_RETRY_COUNT; i++) {
            if (nd_snprintf(candidate, sizeof candidate, "%s%s", stem, ND_RING_EXT_RETRY[i]) !=
                ND_OK)
                continue;
            if (nd_path_exists(candidate)) {
                nd_log(ND_LOG_NOTIFY, "Using %s instead.", candidate);
                (void)nd_strlcpy(n->ring_path, candidate, sizeof n->ring_path);
                n->have_ring_path = true;
                return n->ring_path;
            }
        }
    }

    for (i = 0u; i < ND_RING_FALLBACK_COUNT; i++) {
        if (nd_path_exists(ND_RING_FALLBACKS[i])) {
            nd_log(ND_LOG_NOTIFY, "Falling back to ringtone %s.", ND_RING_FALLBACKS[i]);
            (void)nd_strlcpy(n->ring_path, ND_RING_FALLBACKS[i], sizeof n->ring_path);
            n->have_ring_path = true;
            return n->ring_path;
        }
    }

    /* Anything playable at all beats a silent ring -- and deliberately with
     * no log line, which is the Python's choice and is reproduced. */
    if (sweep_tones_dir(candidate, sizeof candidate)) {
        (void)nd_strlcpy(n->ring_path, candidate, sizeof n->ring_path);
        n->have_ring_path = true;
        return n->ring_path;
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Ringing
 * ------------------------------------------------------------------ */

bool nd_notify_start_ring(nd_notify *n)
{
    char path[ND_PATH_MAX];
    const char *chosen;
    const char *why = "";
    const char *argv[7];

    if (n == NULL)
        return false;

    nd_notify_stop_ring(n); /* idempotent, and the Python starts here too */

    chosen = nd_notify_ringtone_path(n);
    if (chosen == NULL) {
        nd_log(ND_LOG_NOTIFY, "No ringtone available; ringing silently.");
        return false;
    }
    /* ringtone_path() hands back a pointer into n->ring_path, which the
     * ringer does not touch -- but the ringer is the only reader and a copy
     * costs nothing, so nothing here depends on that staying true. */
    (void)nd_strlcpy(path, chosen, sizeof path);

    if (ringer_start(n, path, &why)) {
        nd_log(ND_LOG_NOTIFY, "Ringing: %s", path);
        return true;
    }
    /* The Python says "miniaudio ring failed"; this is not miniaudio, and
     * SESSION-SCOPE's logging rule is that the MECHANISM matches and the
     * message may say what the C actually did. Same tag, same colour, same
     * place in the chain, same next step. */
    nd_log(ND_LOG_NOTIFY, "Streaming ring failed (%s); trying mpv.", why);

    argv[0] = "mpv";
    argv[1] = "--no-video";
    argv[2] = "--quiet";
    argv[3] = "--loop-file=inf";
    argv[4] = path;
    argv[5] = NULL;
    if (spawn_quiet(argv, -1, ND_OWNER_TONE, &n->mpv_pid)) {
        nd_log(ND_LOG_NOTIFY, "Ringing (mpv): %s", path);
        return true;
    }
    nd_log(ND_LOG_NOTIFY, "Ringer unavailable: mpv: %s", strerror(errno));
    n->mpv_pid = -1;
    return false;
}

void nd_notify_stop_ring(nd_notify *n)
{
    if (n == NULL)
        return;

    /* Both handles are checked, and both print, so a state where BOTH were
     * somehow live prints the line twice. That is the Python's behaviour and
     * it is reproduced rather than tidied -- if it ever prints twice, that
     * is worth seeing on the serial console. */
    if (n->ring != NULL) {
        ringer_free(n->ring);
        n->ring = NULL;
        nd_log(ND_LOG_NOTIFY, "Ringer stopped.");
    }
    if (n->mpv_pid > 0) {
        (void)nd_proc_terminate(n->mpv_pid, RING_TERM_GRACE, NULL);
        n->mpv_pid = -1;
        nd_log(ND_LOG_NOTIFY, "Ringer stopped.");
    }
}

bool nd_notify_ringing(const nd_notify *n)
{
    return n != NULL && (n->ring != NULL || n->mpv_pid > 0);
}
