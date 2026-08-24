/* apps/MusicPlayer/audio.c -- the two playback backends.
 *
 * The Python has `_MiniaudioPlayer` and `_MpvPlayer` and picks between them
 * at start-up. This file is both, plus the fallback ladder between them.
 *
 * ============ WHY THIS IS NOT miniaudio, AND WHY THAT IS THE POINT ========
 *
 * The Python's own docstring says why miniaudio is there:
 *
 *     Playback now streams in-process via python-miniaudio instead of
 *     spawning mpv (~24MB private RSS per process; the OOM killer's first
 *     pick on real hardware). Tracks decode chunk-by-chunk, so RAM use is
 *     just the device buffer.
 *
 * Neither miniaudio nor mutagen is in the image. docs/c-rewrite/AUDIO.md
 * records what 0.4.0 does instead and lib/nd_notify.c already does it for the
 * ringtone: vendored dr_mp3 / dr_wav decoding a few thousand source frames at
 * a time, straight into `aplay`, which both defconfigs already build and
 * which costs a few hundred kB against mpv's ~24 MB. THE SAME DECODERS ARE
 * REUSED -- the implementations live in libneodct because nd_notify.c
 * compiled them, and this file includes the headers for their declarations
 * only. There is one copy of dr_mp3 in the system, not two.
 *
 * What is deliberately NOT copied from nd_notify.c is `nd_tone_src`. A
 * ringtone LOOPS: nd_tone_src_read() seeks back to frame 0 at EOF and never
 * reports an end, which is exactly wrong for a track that has to finish so
 * the screen can return. It also has no position to report. So the source
 * here is its own thirty lines around the same two decoders, and
 * OPEN-QUESTIONS.md MU-3 proposes the shared non-looping source that would
 * let both use one.
 *
 * ============ THE OTHER DELIBERATE DIFFERENCE: NO RESAMPLER ============
 *
 * The Python asks miniaudio to convert everything to 44100 Hz stereo.
 * nd_notify.c reproduces that with its own integer linear resampler because
 * it hardcodes `aplay -r 44100`. This file instead tells aplay the FILE's own
 * rate and channel count. aplay's default device is ALSA's `plug` layer,
 * whose entire job is rate and channel conversion, so the same conversion
 * happens one layer down -- and a second copy of a resampler is exactly what
 * the brief said not to write. position() then counts in source frames over
 * the source rate, which is the same number of seconds either way.
 * OPEN-QUESTIONS.md MU-6.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"

#include "music.h"

/* Declarations only -- lib/nd_notify.c is the one translation unit that
 * defines DR_MP3_IMPLEMENTATION / DR_WAV_IMPLEMENTATION, and libneodct
 * exports the symbols. The warning set is suspended across the two headers
 * for the reason nd_notify.c gives: -Wconversion on somebody else's DSP
 * header produces hundreds of warnings about arithmetic that is correct, and
 * editing a vendored decoder is how a port acquires a bug nobody can find
 * upstream. Everything this file writes is compiled with the full set. */
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
#pragma GCC diagnostic ignored "-Wpedantic"

#include "../../lib/vendor/dr_mp3.h"

#define DR_WAV_NO_WCHAR
#include "../../lib/vendor/dr_wav.h"

#pragma GCC diagnostic pop

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

/* The Python's `frames_to_read = max(16384, RATE * buf_ms * 2 // 1000)`,
 * which is the stream's decode buffer and is what NEODCT_MUSIC_ABUF_MS
 * actually sizes. At the default 500 ms that is 44100 frames -- 176 kB
 * stereo, still three orders of magnitude under decode_file()'s 6.3 MB.
 *
 * THE CAP IS THIS PORT'S. buf_ms comes from the environment and the Python
 * hands it straight to miniaudio; here it sizes a malloc, and a malloc sized
 * by an environment variable needs a ceiling (CODING-STANDARDS.md 1.5).
 * 65536 frames is 256 kB stereo, or 1.5 s at 44.1 kHz. */
#define MUSIC_CHUNK_MIN    16384u
#define MUSIC_CHUNK_MAX    65536u
#define MUSIC_ABUF_MS_MAX  2000

/* The Python gives mpv 0.2 s between terminate() and kill(). Same number for
 * aplay, which has nothing to flush that we want kept. */
#define MUSIC_TERM_GRACE 0.2

/* The feeder holds two pointers and dr_mp3's decode frames. 128 kB is musl's
 * default and glibc's is 8 MB; setting it explicitly is what MUSL.md asks
 * for, so the difference stops mattering. */
#define MUSIC_THREAD_STACK (128u * 1024u)

/* ------------------------------------------------------------------ *
 * The source: dr_mp3 / dr_wav, pulled a chunk at a time, NOT looping
 * ------------------------------------------------------------------ */

typedef struct {
    bool is_mp3;
    union {
        drmp3 mp3;
        drwav wav;
    } d;
    uint32_t rate;
    uint32_t channels;
} music_src;

/* ND_ERR_UNSUPPORTED when neither decoder recognises the file, which is the
 * branch .flac / .ogg / .aac take and is what sends this ONE track to mpv --
 * exactly as a .wma ringtone already falls through to mpv today. */
static nd_err src_open(music_src *s, const char *real_path)
{
    memset(s, 0, sizeof *s);

    if (drmp3_init_file(&s->d.mp3, real_path, NULL) != 0u) {
        s->is_mp3 = true;
        s->rate = s->d.mp3.sampleRate;
        s->channels = s->d.mp3.channels;
    } else if (drwav_init_file(&s->d.wav, real_path, NULL) != 0u) {
        s->is_mp3 = false;
        s->rate = s->d.wav.sampleRate;
        s->channels = s->d.wav.channels;
    } else {
        return ND_ERR_UNSUPPORTED;
    }
    if (s->rate == 0u || s->channels == 0u) {
        if (s->is_mp3)
            drmp3_uninit(&s->d.mp3);
        else
            (void)drwav_uninit(&s->d.wav);
        return ND_ERR_PARSE;
    }
    return ND_OK;
}

static void src_close(music_src *s)
{
    if (s->is_mp3)
        drmp3_uninit(&s->d.mp3);
    else
        (void)drwav_uninit(&s->d.wav);
    memset(s, 0, sizeof *s);
}

/* Frames in the FILE's own channel count. 0 means the end -- and unlike
 * nd_tone_src_read() it stays 0, because a track ends. */
static size_t src_read(music_src *s, int16_t *out, size_t frames)
{
    if (s->is_mp3)
        return (size_t)drmp3_read_pcm_frames_s16(&s->d.mp3, (drmp3_uint64)frames, out);
    return (size_t)drwav_read_pcm_frames_s16(&s->d.wav, (drwav_uint64)frames, out);
}

/* ------------------------------------------------------------------ *
 * The one player
 * ------------------------------------------------------------------ *
 *
 * File-static rather than a field on an app object, for the reason
 * nd_tones_preview_pid() is: app_shutdown() takes no argument, and the
 * SIGTERM teardown contract in nd_app.h requires it to be able to release
 * the sound card from anywhere. There is at most one player in this process.
 */

static struct {
    nd_music_backend backend; /* what _pick_player() chose for the session */
    nd_music_backend running; /* what is playing RIGHT NOW; may be MPV for a
                               * single unsupported track while the session
                               * stays on STREAM */
    pid_t pid;                /* aplay or mpv; -1 when nothing is playing   */
    bool child_gone;          /* the pid has been reaped                    */
    bool paused;

    /* STREAM only. */
    music_src src;
    bool src_open;
    int fd;
    int16_t *buf;
    size_t chunk_frames;
    pthread_t thread;
    bool thread_live;
    volatile sig_atomic_t stop;

    pthread_mutex_t lock;
    bool lock_live;
    uint64_t sent_bytes; /* under lock: written to the player, i.e. heard */
} g = {ND_MUSIC_BACKEND_NONE, ND_MUSIC_BACKEND_NONE, -1, false, false,
       {false, {{0}}, 0u, 0u},  false,  -1,    NULL,  0u,   0,
       false, 0, {{0}}, false,  0u};

/* ------------------------------------------------------------------ *
 * Spawning -- the same shape nd_notify.c and apps/Tones use
 * ------------------------------------------------------------------ */

/* execvp's lookup, which nd_proc_spawn() does not do: it takes a path and
 * execve()s it. A miss is subprocess.Popen's FileNotFoundError. */
static bool which_exec(const char *name, char *out, size_t out_sz)
{
    const char *path;
    const char *seg;

    if (name == NULL || name[0] == '\0' || out == NULL || out_sz == 0u)
        return false;
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) != 0)
            return false;
        return nd_snprintf(out, out_sz, "%s", name) == ND_OK;
    }
    path = getenv("PATH");
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (seg = path; seg != NULL;) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        nd_err rc;

        if (len == 0u)
            rc = nd_snprintf(out, out_sz, "./%s", name);
        else
            rc = nd_snprintf(out, out_sz, "%.*s/%s", (int)len, seg, name);
        if (rc == ND_OK && access(out, X_OK) == 0)
            return true;
        seg = (colon != NULL) ? colon + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}

/* stdout and stderr to /dev/null, as subprocess.DEVNULL puts them, plus an
 * optional stdin for the raw-PCM feed. The descriptor plan is built BEFORE
 * the fork -- CODING-STANDARDS.md section 1.1. */
static bool spawn_quiet(const char *const *argv, int stdin_fd, pid_t *pid_out)
{
    char exe[ND_PATH_MAX];
    nd_proc_spec spec;
    int devnull;
    nd_err rc;

    if (!which_exec(argv[0], exe, sizeof exe)) {
        errno = ENOENT;
        return false;
    }
    /* NOT ND_ROOT-resolved: this is the child's plumbing, not phone data. */
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0)
        return false;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_TONE;
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

static void *feed(void *arg)
{
    ND_UNUSED(arg);

    while (g.stop == 0) {
        size_t got = src_read(&g.src, g.buf, g.chunk_frames);
        size_t bytes;
        size_t sent = 0u;

        if (got == 0u)
            break; /* the track ended -- see the shutdown() below */

        bytes = got * (size_t)g.src.channels * sizeof g.buf[0];
        while (sent < bytes && g.stop == 0) {
            /* MSG_NOSIGNAL, not write(): when stop() kills the player this
             * send is what notices, and on a pipe that would be a
             * process-wide SIGPIPE landing in an app that has no handler. */
            ssize_t n = send(g.fd, (const char *)g.buf + sent, bytes - sent, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return NULL; /* EPIPE: the player is gone. So are we. */
            }
            if (n == 0)
                return NULL;
            sent += (size_t)n;

            /* Counted as the PLAYER accepts it, not as it is decoded: past
             * the first second the socket is full and this loop runs at the
             * speed of the sound card, which is what makes the progress bar
             * track the music instead of the decoder. */
            (void)pthread_mutex_lock(&g.lock);
            g.sent_bytes += (uint64_t)n;
            (void)pthread_mutex_unlock(&g.lock);
        }
    }

    /* The track is over. Close the write half so aplay reaches EOF, DRAINS
     * what is still in the ALSA buffer and exits on its own -- which is what
     * makes is_finished() honest. Killing it here instead would clip the last
     * half-second of every track. */
    if (g.fd >= 0)
        (void)shutdown(g.fd, SHUT_WR);
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Teardown
 * ------------------------------------------------------------------ */

static void release_child(void)
{
    if (g.pid > 0 && !g.child_gone)
        (void)nd_proc_terminate(g.pid, MUSIC_TERM_GRACE, NULL);
    g.pid = -1;
    g.child_gone = false;
}

void nd_music_stop(void)
{
    g.stop = 1;

    /* ORDER MATTERS, and it is nd_notify.c's order for nd_notify.c's reason:
     *   1. the player dies, which is what makes a blocked send() return;
     *   2. our end is shut down as a second way out of that send;
     *   3. join, THEN close -- closing a descriptor another thread is sitting
     *      in send() on is how a number gets recycled underneath it. */
    release_child();
    if (g.fd >= 0)
        (void)shutdown(g.fd, SHUT_RDWR);
    if (g.thread_live)
        (void)pthread_join(g.thread, NULL);
    g.thread_live = false;
    if (g.fd >= 0)
        (void)close(g.fd);
    g.fd = -1;

    if (g.src_open)
        src_close(&g.src);
    g.src_open = false;
    free(g.buf);
    g.buf = NULL;
    g.chunk_frames = 0u;
    g.sent_bytes = 0u;
    g.paused = false;
    g.running = ND_MUSIC_BACKEND_NONE;
    g.stop = 0;
}

/* ------------------------------------------------------------------ *
 * Starting
 * ------------------------------------------------------------------ */

static size_t chunk_frames_from_env(void)
{
    const char *env = getenv(ND_MUSIC_ENV_ABUF_MS);
    long ms = ND_MUSIC_ABUF_MS_DEFAULT;
    unsigned long frames;

    if (env != NULL && env[0] != '\0') {
        char *end = NULL;

        ms = strtol(env, &end, 10);
        if (end == env || ms <= 0)
            ms = ND_MUSIC_ABUF_MS_DEFAULT;
    }
    if (ms > MUSIC_ABUF_MS_MAX)
        ms = MUSIC_ABUF_MS_MAX;

    /* max(16384, RATE * buf_ms * 2 // 1000), then this port's ceiling. */
    frames = ((unsigned long)ND_MUSIC_RATE * (unsigned long)ms * 2ul) / 1000ul;
    if (frames < MUSIC_CHUNK_MIN)
        frames = MUSIC_CHUNK_MIN;
    if (frames > MUSIC_CHUNK_MAX)
        frames = MUSIC_CHUNK_MAX;
    return (size_t)frames;
}

static int32_t abuf_ms(void)
{
    const char *env = getenv(ND_MUSIC_ENV_ABUF_MS);
    long ms = ND_MUSIC_ABUF_MS_DEFAULT;

    if (env != NULL && env[0] != '\0') {
        char *end = NULL;

        ms = strtol(env, &end, 10);
        if (end == env || ms <= 0)
            ms = ND_MUSIC_ABUF_MS_DEFAULT;
    }
    if (ms > MUSIC_ABUF_MS_MAX)
        ms = MUSIC_ABUF_MS_MAX;
    return (int32_t)ms;
}

/* mpv, with the Python's argv verbatim including `nice -n -10` -- playback
 * that stutters because the UI is redrawing is what the nice was for. */
static bool start_mpv(const char *real_path)
{
    const char *argv[ND_MUSIC_MPV_ARGC + 2];
    pid_t pid = -1;
    size_t i;

    for (i = 0u; i < ND_MUSIC_MPV_ARGC; i++)
        argv[i] = nd_music_mpv_cmd[i];
    argv[ND_MUSIC_MPV_ARGC] = real_path;
    argv[ND_MUSIC_MPV_ARGC + 1u] = NULL;

    if (!spawn_quiet(argv, -1, &pid))
        return false;
    g.pid = pid;
    g.child_gone = false;
    g.paused = false;
    g.running = ND_MUSIC_BACKEND_MPV;
    return true;
}

/* The in-process path. `why` receives a short reason for the log. */
static nd_err start_stream(const char *real_path, const char **why)
{
    char rate[16];
    char channels[8];
    char buftime[32];
    const char *argv[14];
    pthread_attr_t attr;
    sigset_t all;
    sigset_t saved;
    int sv[2];
    size_t stack;
    nd_err rc;

    *why = "internal error";

    rc = src_open(&g.src, real_path);
    if (rc != ND_OK) {
        *why = (rc == ND_ERR_UNSUPPORTED) ? "not MP3 or WAV" : nd_strerror(rc);
        return rc;
    }
    g.src_open = true;
    g.chunk_frames = chunk_frames_from_env();

    /* chunk_frames * channels * 2 bytes: 176,400 at the 500 ms default in
     * stereo, capped at 256 kB by MUSIC_CHUNK_MAX. Owned here; freed by
     * nd_music_stop() on every path out. */
    g.buf = malloc(g.chunk_frames * (size_t)g.src.channels * sizeof *g.buf);
    if (g.buf == NULL) {
        *why = "out of memory";
        return ND_ERR_NOMEM;
    }

    if (nd_snprintf(rate, sizeof rate, "%u", (unsigned)g.src.rate) != ND_OK ||
        nd_snprintf(channels, sizeof channels, "%u", (unsigned)g.src.channels) != ND_OK ||
        nd_snprintf(buftime, sizeof buftime, "--buffer-time=%d", abuf_ms() * 1000) != ND_OK) {
        *why = "cannot build the player's arguments";
        return ND_ERR_TOOLONG;
    }

    /* SOCK_STREAM, not a pipe, so the feeder can use MSG_NOSIGNAL. CLOEXEC on
     * both ends: nd_proc_spawn dup2()s the one the child needs, which clears
     * the flag on the copy, and everything else must not leak into aplay. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        *why = strerror(errno);
        return ND_ERR_IO;
    }

    argv[0] = "aplay";
    argv[1] = "-q";
    argv[2] = "-t";
    argv[3] = "raw";
    argv[4] = "-f";
    argv[5] = "S16_LE";
    argv[6] = "-c";
    argv[7] = channels;
    argv[8] = "-r";
    argv[9] = rate;
    argv[10] = buftime;
    argv[11] = NULL;

    if (!spawn_quiet(argv, sv[1], &g.pid)) {
        *why = strerror(errno);
        (void)close(sv[0]);
        (void)close(sv[1]);
        g.pid = -1;
        return ND_ERR_IO;
    }
    (void)close(sv[1]);
    g.fd = sv[0];
    g.child_gone = false;
    /* We never read from the player; shutting the read side down means a
     * player that exits is noticed by the next send() rather than by
     * nothing. */
    (void)shutdown(g.fd, SHUT_RD);

    if (pthread_attr_init(&attr) != 0) {
        *why = "pthread_attr_init";
        return ND_ERR_IO;
    }
    stack = MUSIC_THREAD_STACK;
#ifdef PTHREAD_STACK_MIN
    if (stack < (size_t)PTHREAD_STACK_MIN)
        stack = (size_t)PTHREAD_STACK_MIN;
#endif
    (void)pthread_attr_setstacksize(&attr, stack);

    /* A new thread inherits the creator's signal mask, so blocking everything
     * across pthread_create leaves the feeder unable to receive a signal at
     * all. That is what we want: nd-apprun's SIGTERM handler belongs on the
     * main thread, and a signal landing here would only turn a send() into an
     * EINTR. Python got this free -- CPython delivers to the main thread. */
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &saved);
    if (pthread_create(&g.thread, &attr, feed, NULL) == 0)
        g.thread_live = true;
    (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    (void)pthread_attr_destroy(&attr);

    if (!g.thread_live) {
        *why = "cannot start the feeder thread";
        return ND_ERR_IO;
    }

    g.paused = false;
    g.running = ND_MUSIC_BACKEND_STREAM;
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * The interface
 * ------------------------------------------------------------------ */

void nd_music_player_init(nd_music_backend backend)
{
    if (!g.lock_live) {
        if (pthread_mutex_init(&g.lock, NULL) != 0) {
            /* Without the counter's lock the progress bar would be reading a
             * 64-bit value another thread is writing, which on 32-bit ARM is
             * two halves and no ordering. Refuse the in-process path rather
             * than read it torn. */
            nd_log_err(ND_LOG_MUSIC, "cannot create the position lock; audio disabled");
            g.backend = ND_MUSIC_BACKEND_NONE;
            return;
        }
        g.lock_live = true;
    }
    nd_music_stop();
    g.backend = backend;
}

nd_music_backend nd_music_backend_now(void)
{
    return g.running != ND_MUSIC_BACKEND_NONE ? g.running : g.backend;
}

nd_music_backend nd_music_pick_player(void)
{
    const char *forced = getenv(ND_MUSIC_ENV_AUDIO);

    if (forced == NULL)
        forced = "";

    /* `if forced != "subprocess" and HAS_MINIAUDIO`. HAS_MINIAUDIO is always
     * true here: the decoders are compiled into libneodct, so there is no
     * import to fail. That also makes the Python's third branch --
     * "NEODCT_MUSIC_AUDIO=miniaudio but module missing; audio disabled" --
     * unreachable, and it is left out rather than faked. MU-7. */
    if (strcmp(forced, "subprocess") != 0) {
        nd_log(ND_LOG_MUSIC, "audio: in-process streaming to aplay");
        return ND_MUSIC_BACKEND_STREAM;
    }
    nd_log(ND_LOG_MUSIC, "audio: external mpv processes");
    return ND_MUSIC_BACKEND_MPV;
}

bool nd_music_play(const char *path)
{
    char real[ND_PATH_MAX];
    const char *why = "";
    nd_err rc;

    if (path == NULL || path[0] == '\0')
        return false;
    if (g.backend == ND_MUSIC_BACKEND_NONE) /* `if self.player is None` */
        return false;

    nd_music_stop();

    /* The playlist holds LOGICAL paths; a decoder and a player are both
     * outside the path layer and get real ones. */
    if (nd_path_resolve(real, sizeof real, path) != ND_OK) {
        nd_log(ND_LOG_MUSIC, "playback failed: path too long: %s", path);
        return false;
    }

    if (g.backend == ND_MUSIC_BACKEND_MPV) {
        if (start_mpv(real))
            return true;
        nd_log(ND_LOG_MUSIC, "playback failed: mpv: %s", strerror(errno));
        nd_music_stop();
        return false;
    }

    rc = start_stream(real, &why);
    if (rc == ND_OK)
        return true;

    nd_log(ND_LOG_MUSIC, "playback failed: %s: %s", why, path);
    nd_music_stop();

    if (rc == ND_ERR_UNSUPPORTED) {
        /* miniaudio would have decoded this (.flac, .ogg); dr_mp3 and dr_wav
         * cannot. mpv plays THIS track and the session stays in-process, so
         * one ogg on the card does not put every later mp3 behind a 24 MB
         * process. AUDIO.md, and OPEN-QUESTIONS.md MU-5. */
        nd_log(ND_LOG_MUSIC, "falling back to mpv for this track");
        if (start_mpv(real))
            return true;
        nd_log(ND_LOG_MUSIC, "playback failed: mpv: %s", strerror(errno));
        nd_music_stop();
        return false;
    }
    if (rc == ND_ERR_PARSE) {
        /* miniaudio.DecodeError: the FILE is bad and the backend is fine, so
         * the Python returns false and changes nothing. */
        return false;
    }

    /* Anything else is the Python's "usually device init" branch: the
     * in-process path is unusable, so the session drops to mpv for good. */
    nd_log(ND_LOG_MUSIC, "falling back to mpv");
    g.backend = ND_MUSIC_BACKEND_MPV;
    if (start_mpv(real))
        return true;
    nd_log(ND_LOG_MUSIC, "playback failed: mpv: %s", strerror(errno));
    nd_music_stop();
    return false;
}

/* `self.process.poll() is not None`, and for the streaming path the same
 * question: aplay exits by itself once the feeder has closed the write half
 * and the card has drained. */
static bool child_exited(void)
{
    int status;
    pid_t r;

    if (g.pid <= 0)
        return true;
    if (g.child_gone)
        return true;
    r = waitpid(g.pid, &status, WNOHANG);
    if (r == g.pid) {
        g.child_gone = true;
        return true;
    }
    if (r < 0 && errno == ECHILD) {
        g.child_gone = true;
        return true;
    }
    return false;
}

bool nd_music_is_finished(void)
{
    if (g.running == ND_MUSIC_BACKEND_NONE)
        return true;
    return child_exited();
}

void nd_music_toggle_pause(void)
{
    /* `if not self.device or self._ended: return` for the streaming player;
     * `if not self.process: return` for mpv. Both are "nothing is playing". */
    if (g.pid <= 0 || g.running == ND_MUSIC_BACKEND_NONE)
        return;
    if (g.running == ND_MUSIC_BACKEND_STREAM && nd_music_is_finished())
        return;

    /* SIGSTOP/SIGCONT for both. For mpv that is literally what the Python
     * does; for the streaming path it is the equivalent of miniaudio's
     * device.stop()/device.start(), because a stopped aplay stops draining
     * the socket, the socket fills, and the feeder blocks in send() with its
     * decode position intact -- "the stream generator keeps its position, so
     * start() with the same generator resumes in place".
     *
     * The Python guards this with `except (ProcessLookupError, OSError)`,
     * because mpv can exit between the poll and the signal; kill() returning
     * ESRCH is the same condition and is handled the same way -- the flag
     * goes back to false rather than the app crashing. */
    if (g.paused) {
        if (kill(g.pid, SIGCONT) == 0)
            g.paused = false;
        else
            g.paused = false;
    } else {
        if (kill(g.pid, SIGSTOP) == 0)
            g.paused = true;
        else
            g.paused = false;
    }
}

bool nd_music_is_paused(void)
{
    return g.paused;
}

bool nd_music_position(double *out)
{
    uint64_t bytes;
    size_t frame_bytes;

    if (out == NULL)
        return false;
    /* _MpvPlayer.position() returns None, which is what makes the screen use
     * its wall clock. */
    if (g.running != ND_MUSIC_BACKEND_STREAM || !g.src_open)
        return false;

    frame_bytes = (size_t)g.src.channels * sizeof g.buf[0];
    if (frame_bytes == 0u || g.src.rate == 0u)
        return false;

    (void)pthread_mutex_lock(&g.lock);
    bytes = g.sent_bytes;
    (void)pthread_mutex_unlock(&g.lock);

    *out = (double)(bytes / (uint64_t)frame_bytes) / (double)g.src.rate;
    return true;
}

pid_t nd_music_child_pid(void)
{
    return g.pid;
}

/* ------------------------------------------------------------------ *
 * Duration -- miniaudio.get_file_info(path).duration
 * ------------------------------------------------------------------ */

double nd_music_duration(const char *path)
{
    char real[ND_PATH_MAX];
    music_src s;
    double seconds = 0.0;

    if (path == NULL || path[0] == '\0')
        return 0.0;
    if (nd_path_resolve(real, sizeof real, path) != ND_OK)
        return 0.0;
    if (src_open(&s, real) != ND_OK)
        return 0.0;

    /* dr_mp3 walks the file to count frames and seeks back; dr_wav read the
     * count out of the header. Neither holds the decoded audio, which is the
     * whole difference from the Python's decode_file() and risk R-9. */
    if (s.is_mp3) {
        drmp3_uint64 frames = drmp3_get_pcm_frame_count(&s.d.mp3);

        seconds = (double)frames / (double)s.rate;
    } else {
        seconds = (double)s.d.wav.totalPCMFrameCount / (double)s.rate;
    }
    src_close(&s);
    return seconds;
}
