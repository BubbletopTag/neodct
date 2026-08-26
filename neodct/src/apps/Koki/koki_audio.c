/* koki_audio.c -- SoundManager: the in-process mixer's sink, the
 * external-player backend, and the two ways of having no sound at all.
 *
 * ============ THE THREE BACKENDS, IN THE PYTHON'S ORDER ============
 *
 * engine.py prefers _MiniaudioMixer -- an in-process mixer that decodes
 * every asset itself, sums up to three sfx over one looping music voice with
 * saturating int16 adds, and hands 22,050 Hz mono s16 to a single ALSA
 * device -- and falls back to external player processes when it cannot be
 * built. Both are here now.
 *
 * The mixer proper is koki_mixer.c. What THIS file adds is its sink: one
 * `aplay` reading raw PCM from a socket, and one feeder thread that turns
 * koki_mixer_pull() into bytes. The fallback ladder below it is unchanged.
 *
 * ============ WHY THE SUM HAS TO HAPPEN IN HERE ============
 *
 * /etc/init.d/S17audio, written on the real device:
 *
 *     ALSA's stock "default" mixes through dmix, and dmix needs an ALSA
 *     timer this kernel does not provide. Opening it fails outright. So the
 *     slave here is plain hw -- one program at a time gets the card.
 *
 * engine.py's own comments corroborate from the other side: mpg123
 * "stuttered and reset whenever aplay grabbed the card", and "two concurrent
 * mpvs OOM'd a 72 MB VM". Two players cannot overlap on this phone. One
 * player fed a pre-mixed stream can.
 *
 * ============ LATENCY IS THE POINT, NOT A DETAIL ============
 *
 * The owner's complaint about the shipped audio is that it is laggy, so
 * replacing spawn latency with buffer latency would be no fix at all. The
 * three queues between a mixed sample and the speaker are sized in koki.h
 * and justified in README-PORT.md: 5.8 ms of chunk, ~23 ms of socket and
 * 30 ms of ALSA ring -- about 59 ms, under two frames at 30 FPS.
 *
 * The socket is the one that has to be asked for. A default AF_UNIX
 * SOCK_STREAM send buffer holds 969 ms of 22050 Hz mono audio, so a stream
 * that simply writes until it blocks is a second behind the game and nobody
 * would guess why.
 *
 * ============ WHY MPV FOR MUSIC AND APLAY FOR SFX (fallback only) ============
 *
 * engine.py's comment, kept because the reasoning is not recoverable from
 * the code: aplay starts in milliseconds and its RSS is trivial, which
 * matters because mpv's init delay is audible on an emulated CPU; mpv's deep
 * buffering survives sharing the device with aplay bursts, which mpg123 did
 * not -- it stuttered and reset whenever aplay grabbed QEMU's emulated card.
 * And mpv's footprint is per PROCESS, so if sfx ever fall back to mpv on a
 * small-RAM system MAX_SFX drops to one: two concurrent mpvs OOM'd a 72 MB VM.
 *
 * ============ fork() AND THE FEEDER THREAD ============
 *
 * CODING-STANDARDS 1.1. `aplay` is spawned BEFORE the feeder thread exists,
 * exactly once, and the mixer never forks again -- so no fork in this
 * process ever happens with that thread running. The two backends are never
 * both live: if the mixer fails to start it joins its thread before
 * returning, and only then may the ladder spawn players.
 *
 * The consequence is stated where it bites: if aplay dies mid-game the mixer
 * stops and the game goes silent with a log line, rather than falling back
 * to spawning players from a threaded process.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"

#include "koki.h"
#include "koki_audio_priv.h"

/* Memory trims, probed against the installed mpv at init and dropped
 * wholesale if its build rejects any of them: an unknown option is an
 * instant exit, which is total silence rather than a louder game. */
static const char *const MPV_EXTRA[] = {
    "--no-config", "--load-scripts=no",        "--audio-display=no",
    "--cache=no",  "--demuxer-max-bytes=1MiB", "--demuxer-max-back-bytes=256KiB",
};

/* One de-duplicating log, standing in for reasons_logged. Bounded: the
 * Python's set is unbounded but only ever holds a handful of fixed strings. */
#define KOKI_SND_LOG_MAX 12
static char g_logged[KOKI_SND_LOG_MAX][160];
static size_t g_n_logged;

static bool log_once(const char *msg)
{
    size_t i;

    for (i = 0u; i < g_n_logged; i++) {
        if (strcmp(g_logged[i], msg) == 0)
            return false;
    }
    if (g_n_logged < KOKI_SND_LOG_MAX)
        (void)nd_strlcpy(g_logged[g_n_logged++], msg, sizeof g_logged[0]);
    return true;
}

static void snd_disable(koki_sound_mgr *sm, const char *reason)
{
    if (log_once(reason))
        nd_log(ND_LOG_KOKI, "SOUND DISABLED: %s -- game continues silent", reason);
    (void)nd_strlcpy(sm->disabled_reason, reason, sizeof sm->disabled_reason);
    sm->enabled = false;
}

static void snd_log_once(const char *msg)
{
    if (log_once(msg))
        nd_log(ND_LOG_KOKI, "%s", msg);
}

static bool dir_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* shutil.which(). PATH order, executable bit, no shell. */
static bool which(const char *name, char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    const char *p;

    if (name == NULL || path == NULL)
        return false;
    for (p = path; *p != '\0';) {
        const char *sep = strchr(p, ':');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char cand[ND_PATH_MAX];

        if (len > 0u && len + strlen(name) + 2u < sizeof cand) {
            (void)memcpy(cand, p, len);
            cand[len] = '/';
            (void)nd_strlcpy(cand + len + 1u, name, sizeof cand - len - 1u);
            if (access(cand, X_OK) == 0) {
                (void)nd_strlcpy(out, cand, out_sz);
                return true;
            }
        }
        if (sep == NULL)
            break;
        p = sep + 1;
    }
    return false;
}

/* The first line of /proc/meminfo, "MemTotal: <kB>". Returns 0 when it
 * cannot be read, which leaves every caller on its default. */
static long meminfo_total_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = 0;

    if (f == NULL)
        return 0;
    if (fgets(line, sizeof line, f) != NULL) {
        char *sp = strchr(line, ' ');

        if (sp != NULL)
            kb = strtol(sp, NULL, 10);
    }
    (void)fclose(f);
    return kb;
}

/* Ask mpv whether it accepts the trim flags. Ten-second cap, as the Python
 * has, because an mpv that hangs at startup would hang the game's launch. */
static bool probe_mpv(const char *mpv_path)
{
    const char *argv[ND_ARRAY_LEN(MPV_EXTRA) + 3u];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    size_t i;
    size_t n = 0u;
    int devnull;

    argv[n++] = "mpv";
    for (i = 0u; i < ND_ARRAY_LEN(MPV_EXTRA); i++)
        argv[n++] = MPV_EXTRA[i];
    argv[n++] = "--version";
    argv[n] = NULL;

    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_AUDIO;
    if (devnull >= 0) {
        spec.fds[0].child_fd = 1;
        spec.fds[0].our_fd = devnull;
        spec.fds[1].child_fd = 2;
        spec.fds[1].our_fd = devnull;
        spec.n_fds = 2u;
    }
    if (nd_proc_spawn(mpv_path, &spec, &pid) != ND_OK) {
        if (devnull >= 0)
            (void)close(devnull);
        return false;
    }
    if (devnull >= 0)
        (void)close(devnull);
    memset(&st, 0, sizeof st);
    if (nd_proc_wait(pid, 10.0, &st) != ND_OK) {
        (void)nd_proc_terminate(pid, 0.0, NULL);
        return false;
    }
    return st.exited && st.exit_status == 0;
}

/* ------------------------------------------------------------------ *
 * The sink: one aplay, one feeder thread
 * ------------------------------------------------------------------ */

/* The feeder holds two pointers and one chunk. 128 kB is musl's default and
 * glibc's is 8 MB; setting it explicitly is what MUSL.md asks for, so the
 * difference stops mattering. */
#define KOKI_SINK_STACK (128u * 1024u)

/* nd_notify.c gives its player 0.3 s between SIGTERM and SIGKILL. Same
 * number here, and app_shutdown() must not block for longer than a moment. */
#define KOKI_SINK_GRACE 0.3

struct koki_sink {
    koki_mixer *mixer; /* borrowed; koki_sound_mgr owns it */
    int fd;            /* our end of the socketpair; -1 when idle */
    pid_t pid;         /* aplay; -1 when idle                     */
    pthread_t thread;
    bool thread_live;
    volatile sig_atomic_t stop;
    int32_t alsa_ms;    /* what aplay was asked for      */
    int32_t sock_bytes; /* payload the socket really holds */

    /* One chunk, on the heap with the rest of the sink. Never on a stack and
     * never sized by input. */
    int16_t buf[KOKI_MIX_CHUNK_FRAMES];
};

static double now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* NEODCT_KOKI_ABUF_MS, engine.py's own knob. Its Python default is 150; this
 * port's is five times tighter because the complaint is latency. Clamped
 * because it sizes a device buffer (CODING-STANDARDS 1.5). */
static int32_t alsa_ms_from_env(void)
{
    const char *env = getenv("NEODCT_KOKI_ABUF_MS");
    long ms = KOKI_MIX_ALSA_MS;

    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        long v = strtol(env, &end, 10);

        /* Unparseable is ignored, as the Python's except ValueError is. */
        if (end != NULL && end != env && *end == '\0')
            ms = v;
    }
    if (ms < KOKI_MIX_ALSA_MS_MIN)
        ms = KOKI_MIX_ALSA_MS_MIN;
    if (ms > KOKI_MIX_ALSA_MS_MAX)
        ms = KOKI_MIX_ALSA_MS_MAX;
    return (int32_t)ms;
}

/* Mix a chunk, hand it over, and notice if we were late doing it.
 *
 * The send() is deliberately OUTSIDE koki_mixer_pull(): this is where the
 * thread spends its life blocked, and blocking there while holding the
 * mixer's lock would make every koki_sound_sfx() call on the game thread
 * wait for the sound card. */
static void *sink_feed(void *arg)
{
    struct koki_sink *s = (struct koki_sink *)arg;
    uint64_t frames_written = 0u;
    double t0 = 0.0;
    bool started = false;

    while (s->stop == 0) {
        size_t bytes = KOKI_MIX_CHUNK_FRAMES * sizeof s->buf[0];
        size_t sent = 0u;

        (void)koki_mixer_pull(s->mixer, s->buf, KOKI_MIX_CHUNK_FRAMES);

        while (sent < bytes && s->stop == 0) {
            /* MSG_NOSIGNAL, not write(): when the sink is torn down the
             * player dies and this send is what notices, and on a pipe that
             * would be a process-wide SIGPIPE landing in an app with no
             * handler. lib/nd_notify.c's reason, verbatim. */
            ssize_t n = send(s->fd, (const char *)s->buf + sent, bytes - sent, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return NULL; /* EPIPE: aplay is gone. So are we. */
            }
            if (n == 0)
                return NULL;
            sent += (size_t)n;
        }
        if (sent < bytes)
            break; /* stopping */

        if (!started) {
            t0 = now_ms();
            started = true;
        }
        frames_written += KOKI_MIX_CHUNK_FRAMES;

        {
            double written_ms = (double)frames_written * 1000.0 / (double)KOKI_MIX_RATE;
            double elapsed_ms = now_ms() - t0;

            if (koki_mix_underrun(elapsed_ms, written_ms, s->alsa_ms)) {
                koki_mixer_note_underrun(s->mixer);
                /* An underrun IS the resynchronisation -- the samples the
                 * card wanted are gone and it has already moved on -- so the
                 * accounting restarts here. Without this one starve would
                 * keep testing true and count once per chunk forever. */
                t0 = now_ms();
                frames_written = 0u;
                started = true;
            }
        }
    }
    return NULL;
}

void koki_sink_stop(struct koki_sink *s)
{
    if (s == NULL)
        return;

    s->stop = 1;

    /* ORDER MATTERS, and it is lib/nd_notify.c's order for its reasons:
     *   1. the player dies, which is what makes a blocked send() return --
     *      audio plays in real time, so the socket is full within a chunk or
     *      two and stays full, and the stop flag is not looked at until that
     *      send comes back;
     *   2. shut our end down as a second way out of that send;
     *   3. join, THEN close -- closing a descriptor another thread is
     *      sitting in send() on is how a number gets recycled underneath it.
     */
    if (s->pid > 0)
        (void)nd_proc_terminate(s->pid, KOKI_SINK_GRACE, NULL);
    s->pid = -1;
    if (s->fd >= 0)
        (void)shutdown(s->fd, SHUT_RDWR);
    if (s->thread_live)
        (void)pthread_join(s->thread, NULL);
    s->thread_live = false;
    if (s->fd >= 0)
        (void)close(s->fd);
    s->fd = -1;
    free(s);
}

/* aplay, told the mix format and nothing else. `why` receives a short reason
 * for the log line that sends the game to the external players. */
struct koki_sink *koki_sink_start(koki_mixer *mixer, const char **why)
{
    char exe[ND_PATH_MAX];
    char rate[16];
    char buftime[32];
    char pertime[32];
    const char *argv[14];
    struct koki_sink *s;
    nd_proc_spec spec;
    pthread_attr_t attr;
    sigset_t all;
    sigset_t saved;
    socklen_t optlen;
    int sv[2];
    int sndbuf;
    int devnull;
    size_t stack;
    int32_t period_us;

    *why = "internal error";

    if (!which("aplay", exe, sizeof exe)) {
        *why = "no aplay";
        return NULL;
    }

    /* owned here; freed by sink_stop() on every path out */
    s = calloc(1u, sizeof *s);
    if (s == NULL) {
        *why = "out of memory";
        return NULL;
    }
    s->mixer = mixer;
    s->fd = -1;
    s->pid = -1;
    s->alsa_ms = alsa_ms_from_env();

    /* SOCK_STREAM, not a pipe, so the feeder can use MSG_NOSIGNAL. CLOEXEC
     * on both ends: nd_proc_spawn dup2()s the one the child needs, which
     * clears the flag on the copy, and nothing else may leak into aplay. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        *why = strerror(errno);
        koki_sink_stop(s);
        return NULL;
    }

    /* THE LATENCY KNOB. Left alone, this socket would hold 969 ms of audio
     * and every effect would arrive a second late. The kernel clamps
     * sk_sndbuf up to its own floor and charges skb truesize rather than
     * payload, so what is granted is read back and what is LOGGED is the
     * real figure -- the floor is a kernel constant and the phone's may
     * differ from the one this was measured on. */
    sndbuf = KOKI_MIX_SOCK_BYTES;
    if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, (socklen_t)sizeof sndbuf) != 0)
        snd_log_once("cannot shrink the audio socket; effects may lag");
    optlen = (socklen_t)sizeof sndbuf;
    if (getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen) != 0)
        sndbuf = KOKI_MIX_SOCK_BYTES * 2;
    /* getsockopt reports the doubled kernel figure, and roughly half of it
     * is skb overhead at this write size. Halved twice, measured. */
    s->sock_bytes = (int32_t)(sndbuf / 4);

    period_us = (int32_t)((int64_t)KOKI_MIX_CHUNK_FRAMES * 1000000 / KOKI_MIX_RATE);
    if (nd_snprintf(rate, sizeof rate, "%d", KOKI_MIX_RATE) != ND_OK ||
        nd_snprintf(buftime, sizeof buftime, "--buffer-time=%d", s->alsa_ms * 1000) != ND_OK ||
        nd_snprintf(pertime, sizeof pertime, "--period-time=%d", period_us) != ND_OK) {
        *why = "cannot build aplay's arguments";
        (void)close(sv[0]);
        (void)close(sv[1]);
        koki_sink_stop(s);
        return NULL;
    }

    argv[0] = "aplay";
    argv[1] = "-q";
    argv[2] = "-t";
    argv[3] = "raw";
    argv[4] = "-f";
    argv[5] = "S16_LE";
    argv[6] = "-c";
    argv[7] = "1";
    argv[8] = "-r";
    argv[9] = rate;
    argv[10] = buftime;
    argv[11] = pertime;
    argv[12] = NULL;

    /* THE FORK. It happens here, before any thread exists in this process,
     * and it is the only one the mixer path ever performs. */
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_AUDIO;
    spec.fds[0].child_fd = 0;
    spec.fds[0].our_fd = sv[1];
    spec.n_fds = 1u;
    if (devnull >= 0) {
        spec.fds[1].child_fd = 1;
        spec.fds[1].our_fd = devnull;
        spec.fds[2].child_fd = 2;
        spec.fds[2].our_fd = devnull;
        spec.n_fds = (getenv("NEODCT_KOKI_SOUND_DEBUG") != NULL) ? 2u : 3u;
    }
    if (nd_proc_spawn(exe, &spec, &s->pid) != ND_OK) {
        *why = strerror(errno);
        if (devnull >= 0)
            (void)close(devnull);
        (void)close(sv[0]);
        (void)close(sv[1]);
        s->pid = -1;
        koki_sink_stop(s);
        return NULL;
    }
    if (devnull >= 0)
        (void)close(devnull);
    (void)close(sv[1]);
    s->fd = sv[0];
    /* We never read from the player; shutting the read side down means an
     * aplay that exits is noticed by the next send() rather than by nothing. */
    (void)shutdown(s->fd, SHUT_RD);

    if (pthread_attr_init(&attr) != 0) {
        *why = "pthread_attr_init";
        koki_sink_stop(s);
        return NULL;
    }
    stack = KOKI_SINK_STACK;
#ifdef PTHREAD_STACK_MIN
    if (stack < (size_t)PTHREAD_STACK_MIN)
        stack = (size_t)PTHREAD_STACK_MIN;
#endif
    (void)pthread_attr_setstacksize(&attr, stack);

    /* A new thread inherits the creator's signal mask, so blocking
     * everything across pthread_create leaves the feeder unable to receive a
     * signal at all. That is what we want: nd-apprun's SIGTERM handler
     * belongs on the main thread, and a signal landing here would only turn
     * a send() into an EINTR. Python got this free -- CPython delivers to
     * the main thread. */
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &saved);
    if (pthread_create(&s->thread, &attr, sink_feed, s) == 0)
        s->thread_live = true;
    (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    (void)pthread_attr_destroy(&attr);

    if (!s->thread_live) {
        *why = "cannot start the feeder thread";
        koki_sink_stop(s);
        return NULL;
    }
    return s;
}

int32_t koki_sink_sock_bytes(const struct koki_sink *s)
{
    return (s != NULL) ? s->sock_bytes : 0;
}

int32_t koki_sink_alsa_ms(const struct koki_sink *s)
{
    return (s != NULL) ? s->alsa_ms : 0;
}

/* Build the whole in-process path: mixer, then aplay, then the thread. Any
 * failure leaves nothing running and nothing to join, which is what lets the
 * caller fall through to the ladder and fork safely. */
static bool mixer_backend_start(koki_sound_mgr *sm, const char **why)
{
    sm->mixer = koki_mixer_new(sm->base_dir);
    if (sm->mixer == NULL) {
        *why = "out of memory";
        return false;
    }
    sm->sink = koki_sink_start(sm->mixer, why);
    if (sm->sink == NULL) {
        koki_mixer_free(sm->mixer);
        sm->mixer = NULL;
        return false;
    }
    sm->alsa_ms = sm->sink->alsa_ms;
    sm->latency_ms = koki_mix_latency_ms(sm->sink->sock_bytes, sm->sink->alsa_ms);
    return true;
}

/* Take the whole in-process path down. Idempotent, and safe to call from
 * app_shutdown() -- it terminates one child and joins one thread. */
static void mixer_backend_stop(koki_sound_mgr *sm)
{
    koki_sink_stop(sm->sink);
    sm->sink = NULL;
    koki_mixer_free(sm->mixer);
    sm->mixer = NULL;
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

void koki_sound_open(koki_sound_mgr *sm, const char *assets_dir)
{
    char aplay[ND_PATH_MAX];
    char mpg123[ND_PATH_MAX];
    char mpv[ND_PATH_MAX];
    bool have_aplay;
    bool have_mpg123;
    bool have_mpv;
    const char *forced;
    const char *env;
    size_t i;

    if (sm == NULL)
        return;
    memset(sm, 0, sizeof *sm);
    g_n_logged = 0u;
    sm->enabled = true;
    sm->max_sfx = KOKI_SND_MAX_SFX;
    sm->music_pid = -1;
    for (i = 0u; i < KOKI_SND_MAX_SFX; i++)
        sm->sfx_pid[i] = -1;
    (void)nd_strlcpy(sm->base_dir, (assets_dir != NULL) ? assets_dir : "", sizeof sm->base_dir);

    if (getenv("NEODCT_KOKI_NOSOUND") != NULL) {
        snd_disable(sm, "NEODCT_KOKI_NOSOUND set");
        return;
    }
    /* A plain stat, NOT nd_path_is_dir(): /dev/snd is a kernel device path
     * and has nothing to do with the ND_ROOT namespace. Resolving it would
     * make a staged test root claim the host has no sound card, which is
     * true here by accident and would be wrong for the wrong reason. */
    if (!dir_exists("/dev/snd")) {
        snd_disable(sm, "/dev/snd missing (no ALSA device; kernel audio not implemented yet?)");
        return;
    }

    /* THE PREFERRED BACKEND, with engine.py's branch structure exactly:
     * NEODCT_KOKI_AUDIO=subprocess forces the external players, =miniaudio
     * makes the mixer mandatory rather than preferred, and anything else
     * tries the mixer and falls through on failure. The token stays spelled
     * "miniaudio" because that is the documented value (spec-koki.md's
     * environment table) even though what it now selects is ours. */
    forced = getenv("NEODCT_KOKI_AUDIO");
    if (forced == NULL || strcmp(forced, "subprocess") != 0) {
        const char *why = "";
        char msg[160];

        if (mixer_backend_start(sm, &why)) {
            nd_log(ND_LOG_KOKI,
                   "audio: in-process mixer -> aplay (%d Hz mono, %d ms, %d sfx + music)",
                   KOKI_MIX_RATE, sm->latency_ms, KOKI_SND_MAX_SFX);
            return;
        }
        (void)nd_snprintf(msg, sizeof msg, "in-process mixer unavailable (%s)", why);
        if (forced != NULL && strcmp(forced, "miniaudio") == 0) {
            snd_disable(sm, msg);
            return;
        }
        nd_log(ND_LOG_KOKI, "%s; falling back to external players", msg);
    }

    have_aplay = which("aplay", aplay, sizeof aplay);
    have_mpg123 = which("mpg123", mpg123, sizeof mpg123);
    have_mpv = which("mpv", mpv, sizeof mpv);

    /* pick(): an explicit override wins, then the preference order. */
    env = getenv("NEODCT_KOKI_WAV_PLAYER");
    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(sm->wav_player, env, sizeof sm->wav_player);
        sm->have_wav_player = true;
    } else if (have_aplay) {
        (void)nd_strlcpy(sm->wav_player, "aplay", sizeof sm->wav_player);
        sm->have_wav_player = true;
    } else if (have_mpv) {
        (void)nd_strlcpy(sm->wav_player, "mpv", sizeof sm->wav_player);
        sm->have_wav_player = true;
    }

    env = getenv("NEODCT_KOKI_MP3_PLAYER");
    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(sm->mp3_player, env, sizeof sm->mp3_player);
        sm->have_mp3_player = true;
    } else if (have_mpv) {
        (void)nd_strlcpy(sm->mp3_player, "mpv", sizeof sm->mp3_player);
        sm->have_mp3_player = true;
    } else if (have_mpg123) {
        (void)nd_strlcpy(sm->mp3_player, "mpg123", sizeof sm->mp3_player);
        sm->have_mp3_player = true;
    }

    if (sm->have_wav_player && strcmp(sm->wav_player, "mpv") == 0) {
        long kb = meminfo_total_kb();

        if (kb > 0 && kb < 72L * 1024L)
            sm->max_sfx = 1;
    }
    env = getenv("NEODCT_KOKI_MAX_SFX");
    if (env != NULL) {
        char *end = NULL;
        long v = strtol(env, &end, 10);

        /* Unparseable is ignored, as the Python's except ValueError is. */
        if (end != NULL && end != env && *end == '\0' && v >= 0 && v <= KOKI_SND_MAX_SFX)
            sm->max_sfx = (int)v;
    }

    if (have_mpv) {
        sm->mpv_trim_ok = probe_mpv(mpv);
        if (!sm->mpv_trim_ok)
            nd_log(ND_LOG_KOKI, "this mpv rejects the memory-trim flags; running it plain");
    }

    if (!sm->have_wav_player && !sm->have_mp3_player)
        snd_disable(sm, "no audio player found (aplay/mpg123/mpv)");
    else
        nd_log(ND_LOG_KOKI, "audio players: sfx=%s music=%s",
               sm->have_wav_player ? sm->wav_player : "None",
               sm->have_mp3_player ? sm->mp3_player : "None");
}

/* ------------------------------------------------------------------ *
 * Spawning a player
 * ------------------------------------------------------------------ */

static pid_t snd_spawn(koki_sound_mgr *sm, const char *rel, bool loop)
{
    char virt[ND_PATH_MAX];
    char real[ND_PATH_MAX];
    char exe[ND_PATH_MAX];
    const char *argv[16];
    const char *player;
    nd_proc_spec spec;
    pid_t pid = -1;
    size_t n = 0u;
    size_t i;
    bool is_mp3;
    int devnull;

    if (nd_snprintf(virt, sizeof virt, "%s/%s", sm->base_dir, rel) != ND_OK)
        return -1;
    /* The player is a separate program opening a real path, so this is one
     * of the few places that has to leave the ND_ROOT namespace. */
    if (nd_path_resolve(real, sizeof real, virt) != ND_OK)
        return -1;

    is_mp3 = (strlen(rel) > 4u && strcmp(rel + strlen(rel) - 4u, ".mp3") == 0);
    player = is_mp3 ? (sm->have_mp3_player ? sm->mp3_player : NULL)
                    : (sm->have_wav_player ? sm->wav_player : NULL);

    if (loop && player != NULL && strcmp(player, "aplay") == 0) {
        /* aplay has no loop mode. Exactly one asset needs this: "Koki D
         * score" is 14.46 s, under the builder's 15 s music threshold, so it
         * was baked as a WAV and is then used as looping music. */
        char probe[ND_PATH_MAX];

        if (which("mpv", probe, sizeof probe)) {
            player = "mpv";
            snd_log_once("looped wav needs mpv (aplay can't loop)");
        } else {
            player = NULL;
            snd_log_once("no looping wav player; music skipped");
        }
    }
    if (player == NULL) {
        snd_log_once(is_mp3 ? "no mp3 player installed; skipping mp3 audio"
                            : "no wav player installed; skipping wav audio");
        return -1;
    }
    if (!which(player, exe, sizeof exe))
        return -1;

    if (strcmp(player, "aplay") == 0) {
        argv[n++] = "aplay";
        argv[n++] = "-q";
        argv[n++] = real;
    } else if (strcmp(player, "mpg123") == 0) {
        argv[n++] = "mpg123";
        argv[n++] = "-q";
        if (loop) {
            argv[n++] = "--loop";
            argv[n++] = "-1";
        }
        argv[n++] = real;
    } else {
        argv[n++] = "mpv";
        argv[n++] = "--no-video";
        argv[n++] = "--really-quiet";
        argv[n++] = "--no-terminal";
        if (sm->mpv_trim_ok) {
            for (i = 0u; i < ND_ARRAY_LEN(MPV_EXTRA); i++)
                argv[n++] = MPV_EXTRA[i];
        }
        if (loop)
            argv[n++] = "--loop=inf";
        argv[n++] = real;
    }
    argv[n] = NULL;

    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_AUDIO;
    if (devnull >= 0) {
        spec.fds[0].child_fd = 1;
        spec.fds[0].our_fd = devnull;
        spec.fds[1].child_fd = 2;
        spec.fds[1].our_fd = devnull;
        spec.n_fds = (getenv("NEODCT_KOKI_SOUND_DEBUG") != NULL) ? 1u : 2u;
    }
    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        char reason[160];

        (void)snprintf(reason, sizeof reason, "%s spawn failed", player);
        snd_disable(sm, reason);
        pid = -1;
    }
    if (devnull >= 0)
        (void)close(devnull);
    return pid;
}

/* ------------------------------------------------------------------ *
 * The API game.py sees
 * ------------------------------------------------------------------ */

void koki_sound_music(koki_sound_mgr *sm, const char *rel)
{
    if (sm == NULL || !sm->enabled || rel == NULL)
        return;
    /* The mixer replaces the music voice in place: no player is killed, no
     * player is started, and whatever effects are sounding keep sounding
     * over the new track. That is the whole difference from the line below
     * it, where a change of music is a SIGKILL and a fork. */
    if (sm->mixer != NULL) {
        koki_mixer_music(sm->mixer, rel);
        return;
    }
    koki_sound_stop_music(sm);
    sm->music_pid = snd_spawn(sm, rel, true);
    sm->music_death_logged = false;
}

void koki_sound_sfx(koki_sound_mgr *sm, const char *rel)
{
    size_t i;
    int live = 0;
    int slot = -1;

    if (sm == NULL || !sm->enabled || rel == NULL)
        return;
    if (sm->mixer != NULL) {
        koki_mixer_sfx(sm->mixer, rel);
        return;
    }
    /* Prune finished voices first, then refuse rather than queue: a fourth
     * simultaneous sfx is DROPPED, which is what Scratch does too. */
    for (i = 0u; i < KOKI_SND_MAX_SFX; i++) {
        if (sm->sfx_pid[i] > 0) {
            nd_proc_status st;

            memset(&st, 0, sizeof st);
            if (nd_proc_wait(sm->sfx_pid[i], 0.0, &st) == ND_OK)
                sm->sfx_pid[i] = -1;
        }
        if (sm->sfx_pid[i] > 0)
            live++;
        else if (slot < 0)
            slot = (int)i;
    }
    if (live >= sm->max_sfx || slot < 0)
        return;
    sm->sfx_pid[slot] = snd_spawn(sm, rel, false);
}

void koki_sound_stop_music(koki_sound_mgr *sm)
{
    if (sm == NULL)
        return;
    if (sm->mixer != NULL) {
        koki_mixer_stop_music(sm->mixer);
        return;
    }
    if (sm->music_pid <= 0)
        return;
    (void)nd_proc_terminate(sm->music_pid, 0.0, NULL);
    sm->music_pid = -1;
}

void koki_sound_stop_all(koki_sound_mgr *sm)
{
    size_t i;

    if (sm == NULL)
        return;
    if (sm->mixer != NULL) {
        /* The grade screens call this mid-game, so it drops the voices and
         * leaves the sink running: the card is not released and re-acquired
         * to go quiet for two seconds. */
        koki_mixer_stop_all(sm->mixer);
        return;
    }
    koki_sound_stop_music(sm);
    for (i = 0u; i < KOKI_SND_MAX_SFX; i++) {
        if (sm->sfx_pid[i] > 0)
            (void)nd_proc_terminate(sm->sfx_pid[i], 0.0, NULL);
        sm->sfx_pid[i] = -1;
    }
}

/* Called every 30 frames from the main loop. */
void koki_sound_check(koki_sound_mgr *sm)
{
    nd_proc_status st;

    if (sm == NULL)
        return;

    if (sm->mixer != NULL) {
        uint32_t n = koki_mixer_underruns(sm->mixer);

        if (n != sm->underruns) {
            sm->underruns = n;
            /* Once, not once per crossing: a phone that is short of CPU will
             * do this steadily and a per-event log would be the reason it is
             * short of CPU. */
            snd_log_once("audio underrun: the mixer fell behind the card -- "
                         "raise NEODCT_KOKI_ABUF_MS");
        }
        if (sm->sink != NULL && sm->sink->pid > 0) {
            memset(&st, 0, sizeof st);
            if (nd_proc_wait(sm->sink->pid, 0.0, &st) == ND_OK) {
                /* aplay is gone: OOM kill, a device that went away, or a
                 * format it could not open. The feeder is already unblocked
                 * by the EPIPE, so this only has to join it.
                 *
                 * It does NOT fall back to the external players. That path
                 * forks, this process now has a thread, and CODING-STANDARDS
                 * 1.1 is not negotiable. The game continues silent. */
                sm->sink->pid = -1;
                mixer_backend_stop(sm);
                snd_disable(sm, "aplay died -- OOM kill or a device that went away? "
                                "check dmesg / NEODCT_KOKI_SOUND_DEBUG=1");
            }
        }
        return;
    }

    /* Looping music should never exit on its own, so an exit here means the
     * player crashed or the OOM killer got it. */
    if (sm->music_pid <= 0)
        return;
    memset(&st, 0, sizeof st);
    if (nd_proc_wait(sm->music_pid, 0.0, &st) != ND_OK)
        return;
    sm->music_pid = -1;
    if (!sm->music_death_logged) {
        sm->music_death_logged = true;
        snd_log_once("music player died -- bad option or OOM kill? check dmesg / run with "
                     "NEODCT_KOKI_SOUND_DEBUG=1");
    }
}

/* THE ONE THAT RELEASES THE CARD. Reached from koki_engine_teardown(), which
 * app_shutdown() calls after SIGTERM so the ringtone can have the device --
 * see the teardown contract in nd_app.h. Idempotent: the normal exit path
 * runs teardown first and this costs nothing the second time. */
void koki_sound_shutdown(koki_sound_mgr *sm)
{
    if (sm == NULL)
        return;
    if (sm->mixer != NULL) {
        /* Voices first so the last chunk the feeder mixes is silence, then
         * the sink, which kills aplay, joins the thread and closes the
         * socket in that order. */
        koki_mixer_stop_all(sm->mixer);
        mixer_backend_stop(sm);
    }
    koki_sound_stop_all(sm);
    sm->enabled = false;
}
