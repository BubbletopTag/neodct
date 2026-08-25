/* koki_mixer.c -- engine.py's _MiniaudioMixer, in C: four streaming voices
 * summed into one 22050 Hz mono s16 stream.
 *
 * ============ WHY THIS EXISTS AT ALL ============
 *
 * /etc/init.d/S17audio, generated on the real device, says the phone's ALSA
 * "default" is plain `hw` with no dmix, "because dmix needs an ALSA timer
 * this kernel does not provide. Opening it fails outright." ONE PROGRAM AT A
 * TIME GETS THE CARD. So music and a sound effect at the same time cannot be
 * two processes; the sum has to happen before the samples leave us.
 *
 * That is exactly what engine.py's preferred backend already does, so this
 * is a port of _MiniaudioMixer rather than a new idea. README-PORT.md
 * carries the four decisions -- latency, voice count, mix rate, clipping --
 * and the measurements behind them.
 *
 * ============ WHAT IS IN THIS FILE AND WHAT IS NOT ============
 *
 * The MIXER only: voices, streaming decode, resampling, and the saturating
 * fold. It owns no thread, spawns no process and writes to no device;
 * koki_mixer_pull() hands finished samples back to whoever asked. The one
 * `aplay` and the feeder thread that drives this are in koki_audio.c.
 *
 * The split is not tidiness. There is no sound card on any machine this port
 * has run on, and a mixer that can only be observed through a device is a
 * mixer that cannot be tested. Pulling into a caller's buffer means
 * test_koki_audio.c can start music and three effects from the real shipped
 * assets and check, sample by sample, that all four are present in one
 * stream.
 *
 * ============ THE DECODERS ARE NOT NEW ============
 *
 * lib/vendor/dr_mp3.h and dr_wav.h are already vendored, and lib/nd_notify.c
 * is the one translation unit that defines their implementations --
 * libneodct exports the symbols and apps/MusicPlayer/audio.c already links
 * against them exactly this way. This file includes the headers for their
 * DECLARATIONS only. There is one copy of dr_mp3 in the system, not three.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "koki.h"

/* Declarations only -- no DR_*_IMPLEMENTATION here. The warning set is
 * suspended across the two headers for the reason nd_notify.c gives:
 * -Wconversion on somebody else's DSP header produces hundreds of warnings
 * about arithmetic that is correct, and editing a vendored decoder is how a
 * port acquires a bug nobody can find upstream. Everything this file writes
 * is compiled with the full set. */
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
 * One voice
 * ------------------------------------------------------------------ */

/* A playing sound. Heap-allocated when it starts and freed when it ends, so
 * a silent game holds none of this; the cap is the four slots below.
 * Measured: sizeof(drmp3) is 32,376 and sizeof(drwav) 408, so the union is
 * an MP3-sized 32 KB and the staging buffer another 4 KB. */
typedef struct koki_voice {
    bool is_mp3;
    union {
        drmp3 mp3;
        drwav wav;
    } d;
    uint32_t rate;     /* the FILE's rate, before conversion */
    uint32_t channels; /* the FILE's channel count           */
    bool loop;         /* music loops; an effect does not    */
    bool done;         /* the source is spent                */

    /* Source frames as decoded, in the file's own channel count. */
    int16_t *stage;
    size_t stage_frames;
    size_t stage_pos;

    /* The resampler, integer and therefore drift-free. `frac` counts in
     * units of 1/KOKI_MIX_RATE of one output frame; `a` and `b` are
     * consecutive SOURCE frames, already folded to mono. At 22050 Hz in --
     * which is every one of the 57 shipped assets -- frac is 0 on every
     * output frame and this degenerates to a bit-exact copy. */
    uint32_t frac;
    int16_t a;
    int16_t b;
    bool primed;
} koki_voice;

static size_t voice_decode(koki_voice *v, int16_t *out, size_t frames)
{
    if (v->is_mp3)
        return (size_t)drmp3_read_pcm_frames_s16(&v->d.mp3, (drmp3_uint64)frames, out);
    return (size_t)drwav_read_pcm_frames_s16(&v->d.wav, (drwav_uint64)frames, out);
}

static bool voice_rewind(koki_voice *v)
{
    if (v->is_mp3)
        return drmp3_seek_to_pcm_frame(&v->d.mp3, 0u) != 0u;
    return drwav_seek_to_pcm_frame(&v->d.wav, 0u) != 0u;
}

static void voice_free(koki_voice *v)
{
    if (v == NULL)
        return;
    if (v->is_mp3)
        drmp3_uninit(&v->d.mp3);
    else
        (void)drwav_uninit(&v->d.wav);
    free(v->stage);
    free(v);
}

/* owned by the caller; free with voice_free(). NULL when neither decoder
 * recognises the file, which is what sends koki_audio.c's caller to the
 * external-player ladder for that one asset. */
static koki_voice *voice_open(const char *real_path, bool loop)
{
    koki_voice *v = calloc(1u, sizeof *v);

    if (v == NULL)
        return NULL;

    if (drmp3_init_file(&v->d.mp3, real_path, NULL) != 0u) {
        v->is_mp3 = true;
        v->rate = v->d.mp3.sampleRate;
        v->channels = v->d.mp3.channels;
    } else if (drwav_init_file(&v->d.wav, real_path, NULL) != 0u) {
        v->is_mp3 = false;
        v->rate = v->d.wav.sampleRate;
        v->channels = v->d.wav.channels;
    } else {
        free(v);
        return NULL;
    }
    if (v->rate == 0u || v->channels == 0u || v->channels > 8u) {
        voice_free(v);
        return NULL;
    }

    /* owned by v; freed in voice_free() */
    v->stage = malloc(KOKI_MIX_STAGE_FRAMES * (size_t)v->channels * sizeof *v->stage);
    if (v->stage == NULL) {
        voice_free(v);
        return NULL;
    }
    v->loop = loop;
    return v;
}

/* Refill the staging buffer. For a looping voice the frame after the last is
 * the first, with nothing in between -- THAT is the loop, and it is why
 * music needs no restart from outside.
 *
 * DEVIATION, deliberate: engine.py's _Voice.read() restarts the generator on
 * StopIteration and `continue`s, so a looping file that decodes to nothing
 * spins forever inside the audio callback. Here a rewind that still yields
 * nothing marks the voice done, exactly as lib/nd_notify.c's src_fill()
 * does. No shipped asset is empty; a truncated one on a bad flash write
 * would be. */
static bool voice_fill(koki_voice *v)
{
    size_t got = voice_decode(v, v->stage, KOKI_MIX_STAGE_FRAMES);

    if (got == 0u) {
        if (!v->loop)
            return false;
        if (!voice_rewind(v))
            return false;
        got = voice_decode(v, v->stage, KOKI_MIX_STAGE_FRAMES);
        if (got == 0u)
            return false;
    }
    v->stage_frames = got;
    v->stage_pos = 0u;
    return true;
}

/* One source frame, folded to mono. miniaudio's converter averages the
 * channels for nchannels=1 and so does this; no shipped asset has more than
 * one channel, so the branch never runs on the phone. */
static bool voice_pull(koki_voice *v, int16_t *out)
{
    const int16_t *f;

    if (v->stage_pos >= v->stage_frames) {
        if (!voice_fill(v))
            return false;
    }
    f = v->stage + v->stage_pos * (size_t)v->channels;
    if (v->channels == 1u) {
        *out = f[0];
    } else {
        int32_t sum = 0;
        uint32_t c;

        for (c = 0u; c < v->channels; c++)
            sum += (int32_t)f[c];
        *out = (int16_t)(sum / (int32_t)v->channels);
    }
    v->stage_pos++;
    return true;
}

/* x0 + (x1 - x0) * frac/KOKI_MIX_RATE, in integers. frac < KOKI_MIX_RATE, so
 * the result is always between the two samples and always fits int16. */
static int16_t lerp16(int16_t x0, int16_t x1, uint32_t frac)
{
    int32_t d = (int32_t)x1 - (int32_t)x0;
    int64_t step = ((int64_t)d * (int64_t)frac) / (int64_t)KOKI_MIX_RATE;

    return (int16_t)((int32_t)x0 + (int32_t)step);
}

/* Frames at KOKI_MIX_RATE, mono. A short return means the voice ended inside
 * this chunk; 0 means it had already ended, which is engine.py's
 * `if not data: continue` and keeps the voice out of the fold entirely. */
static size_t voice_read(koki_voice *v, int16_t *out, size_t frames)
{
    size_t i;

    if (v->done)
        return 0u;

    if (!v->primed) {
        if (!voice_pull(v, &v->a) || !voice_pull(v, &v->b)) {
            v->done = true;
            return 0u;
        }
        v->primed = true;
        v->frac = 0u;
    }

    for (i = 0u; i < frames; i++) {
        out[i] = lerp16(v->a, v->b, v->frac);

        v->frac += v->rate;
        while (v->frac >= (uint32_t)KOKI_MIX_RATE) {
            v->frac -= (uint32_t)KOKI_MIX_RATE;
            v->a = v->b;
            if (!voice_pull(v, &v->b)) {
                v->done = true;
                return i + 1u;
            }
        }
    }
    return frames;
}

/* ------------------------------------------------------------------ *
 * The mixer
 * ------------------------------------------------------------------ */

struct koki_mixer {
    pthread_mutex_t lock;
    bool lock_live;
    char base_dir[ND_PATH_MAX];

    /* engine.py's `self.voices` list: INSERTION ORDER, compacted on prune,
     * because the fold is pairwise and therefore order-dependent. */
    koki_voice *sfx[KOKI_SND_MAX_SFX];
    size_t n_sfx;
    koki_voice *music;

    int16_t scratch[KOKI_MIX_CHUNK_FRAMES];
    uint64_t frames_out;
    uint32_t underruns;
};

int16_t koki_mix_add(int16_t a, int16_t b)
{
    int32_t s = (int32_t)a + (int32_t)b;

    if (s > 32767)
        return (int16_t)32767;
    if (s < -32768)
        return (int16_t)(-32768);
    return (int16_t)s;
}

int32_t koki_mix_latency_ms(int32_t sock_bytes, int32_t alsa_ms)
{
    /* One chunk being mixed, plus everything already handed onward: our end
     * of the socket, then aplay's ALSA ring. s16 mono, so a byte is half a
     * frame. See README-PORT.md's table. */
    double chunk_ms = (double)KOKI_MIX_CHUNK_FRAMES * 1000.0 / (double)KOKI_MIX_RATE;
    double sock_ms = (double)sock_bytes / 2.0 * 1000.0 / (double)KOKI_MIX_RATE;

    if (sock_bytes < 0)
        sock_ms = 0.0;
    if (alsa_ms < 0)
        alsa_ms = 0;
    return (int32_t)(chunk_ms + sock_ms + (double)alsa_ms + 0.5);
}

bool koki_mix_underrun(double elapsed_ms, double written_ms, int32_t guard_ms)
{
    /* The card cannot have played more than we wrote, so while the pipeline
     * is fed the wall clock stays BEHIND the audio clock. The guard is one
     * ALSA buffer, because playback really starts about a period after our
     * first write and without it every startup would look like a starve. */
    return elapsed_ms > written_ms + (double)guard_ms;
}

/* owned by the caller; free with koki_mixer_free() */
koki_mixer *koki_mixer_new(const char *base_dir)
{
    koki_mixer *m = calloc(1u, sizeof *m);

    if (m == NULL)
        return NULL;
    if (pthread_mutex_init(&m->lock, NULL) != 0) {
        free(m);
        return NULL;
    }
    m->lock_live = true;
    (void)nd_strlcpy(m->base_dir, (base_dir != NULL) ? base_dir : "", sizeof m->base_dir);
    return m;
}

void koki_mixer_free(koki_mixer *m)
{
    if (m == NULL)
        return;
    koki_mixer_stop_all(m);
    if (m->lock_live)
        (void)pthread_mutex_destroy(&m->lock);
    free(m);
}

/* The decoder is opened OUTSIDE the lock. engine.py builds its _Voice while
 * holding the lock the audio callback needs, so every sound effect stalls
 * playback for the length of a file open; that is a defect of the reference
 * rather than a behaviour of it, and it is not reproduced. */
static koki_voice *open_rel(koki_mixer *m, const char *rel, bool loop)
{
    char virt[ND_PATH_MAX];
    char real[ND_PATH_MAX];

    if (rel == NULL || rel[0] == '\0')
        return NULL;
    if (nd_snprintf(virt, sizeof virt, "%s/%s", m->base_dir, rel) != ND_OK)
        return NULL;
    /* A decoder opens a real file, so this leaves the ND_ROOT namespace --
     * the same reason lib/nd_notify.c resolves before nd_tone_src_open(). */
    if (nd_path_resolve(real, sizeof real, virt) != ND_OK)
        return NULL;
    return voice_open(real, loop);
}

/* Drop every finished effect, keeping the survivors in insertion order.
 * Returns how many pointers it wrote to `dead`; the caller frees them AFTER
 * releasing the lock, because drmp3_uninit() closes a FILE. */
static size_t prune_locked(koki_mixer *m, koki_voice **dead, size_t dead_max)
{
    size_t n_dead = 0u;
    size_t w = 0u;
    size_t i;

    for (i = 0u; i < m->n_sfx; i++) {
        if (m->sfx[i]->done && n_dead < dead_max)
            dead[n_dead++] = m->sfx[i];
        else
            m->sfx[w++] = m->sfx[i];
    }
    m->n_sfx = w;
    if (m->music != NULL && m->music->done && n_dead < dead_max) {
        dead[n_dead++] = m->music;
        m->music = NULL;
    }
    return n_dead;
}

static void free_all(koki_voice **dead, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++)
        voice_free(dead[i]);
}

void koki_mixer_music(koki_mixer *m, const char *rel)
{
    koki_voice *v;
    koki_voice *old;

    if (m == NULL)
        return;
    /* `self.music = self._Voice(self, path, True)` -- play_music REPLACES
     * whatever was there, and a failure to open leaves the old music alone
     * rather than silencing the game. */
    v = open_rel(m, rel, true);
    if (v == NULL) {
        nd_log(ND_LOG_KOKI, "cannot decode music '%s'; leaving the current track", rel);
        return;
    }
    (void)pthread_mutex_lock(&m->lock);
    old = m->music;
    m->music = v;
    (void)pthread_mutex_unlock(&m->lock);
    voice_free(old);
}

void koki_mixer_sfx(koki_mixer *m, const char *rel)
{
    koki_voice *dead[KOKI_SND_MAX_SFX + 1u];
    koki_voice *v;
    size_t n_dead;
    bool room;

    if (m == NULL)
        return;

    /* engine.py: prune the finished voices FIRST, then append only if there
     * is room. A fourth simultaneous effect is DROPPED -- not queued, and
     * nothing is stolen from the oldest. Checking before the open also means
     * a dropped effect costs no file open, which is what the Python's
     * evaluation order gives it for free. */
    (void)pthread_mutex_lock(&m->lock);
    n_dead = prune_locked(m, dead, ND_ARRAY_LEN(dead));
    room = m->n_sfx < (size_t)KOKI_SND_MAX_SFX;
    (void)pthread_mutex_unlock(&m->lock);
    free_all(dead, n_dead);

    if (!room)
        return;

    v = open_rel(m, rel, false);
    if (v == NULL) {
        nd_log(ND_LOG_KOKI, "cannot decode sfx '%s'; skipped", rel);
        return;
    }

    (void)pthread_mutex_lock(&m->lock);
    /* Only this thread appends, and the feeder only removes, so the re-check
     * can only find MORE room than the first one did. It is here anyway
     * because the array index has to be right whatever ran in between. */
    if (m->n_sfx < (size_t)KOKI_SND_MAX_SFX) {
        m->sfx[m->n_sfx++] = v;
        v = NULL;
    }
    (void)pthread_mutex_unlock(&m->lock);
    voice_free(v);
}

void koki_mixer_stop_music(koki_mixer *m)
{
    koki_voice *old;

    if (m == NULL)
        return;
    (void)pthread_mutex_lock(&m->lock);
    old = m->music;
    m->music = NULL;
    (void)pthread_mutex_unlock(&m->lock);
    voice_free(old);
}

void koki_mixer_stop_all(koki_mixer *m)
{
    koki_voice *dead[KOKI_SND_MAX_SFX + 1u];
    size_t n_dead = 0u;
    size_t i;

    if (m == NULL)
        return;
    (void)pthread_mutex_lock(&m->lock);
    for (i = 0u; i < m->n_sfx; i++)
        dead[n_dead++] = m->sfx[i];
    m->n_sfx = 0u;
    if (m->music != NULL) {
        dead[n_dead++] = m->music;
        m->music = NULL;
    }
    (void)pthread_mutex_unlock(&m->lock);
    free_all(dead, n_dead);
}

/* One voice folded into `out`. `mixed` says whether `out` already holds
 * something; the first live voice is COPIED and every later one is added,
 * which is engine.py's `mixed = data if mixed is None else self._mix(...)`.
 * Returns the new value of `mixed`. */
static bool fold_locked(koki_mixer *m, koki_voice *v, int16_t *out, size_t frames, bool mixed)
{
    size_t got = voice_read(v, m->scratch, frames);
    size_t j;

    if (got == 0u)
        return mixed; /* `if not data: continue` */
    if (got < frames)
        memset(m->scratch + got, 0, (frames - got) * sizeof m->scratch[0]);

    if (!mixed) {
        (void)memcpy(out, m->scratch, frames * sizeof *out);
        return true;
    }
    for (j = 0u; j < frames; j++)
        out[j] = koki_mix_add(out[j], m->scratch[j]);
    return true;
}

/* At most KOKI_MIX_CHUNK_FRAMES, because that is what `scratch` holds. */
static void pull_chunk(koki_mixer *m, int16_t *out, size_t frames)
{
    koki_voice *dead[KOKI_SND_MAX_SFX + 1u];
    size_t n_dead;
    bool mixed = false;
    size_t i;

    (void)pthread_mutex_lock(&m->lock);
    /* THE ORDER IS engine.py's: `live = list(self.voices)` and then
     * `live.append(self.music)`. Effects in the order they started, music
     * last. With a pairwise saturating fold that is observable. */
    for (i = 0u; i < m->n_sfx; i++)
        mixed = fold_locked(m, m->sfx[i], out, frames, mixed);
    if (m->music != NULL)
        mixed = fold_locked(m, m->music, out, frames, mixed);

    n_dead = prune_locked(m, dead, ND_ARRAY_LEN(dead));
    m->frames_out += (uint64_t)frames;
    (void)pthread_mutex_unlock(&m->lock);

    if (!mixed)
        (void)memset(out, 0, frames * sizeof *out);
    free_all(dead, n_dead);
}

size_t koki_mixer_pull(koki_mixer *m, int16_t *out, size_t frames)
{
    size_t done = 0u;

    if (m == NULL || out == NULL)
        return 0u;
    while (done < frames) {
        size_t n = frames - done;

        if (n > KOKI_MIX_CHUNK_FRAMES)
            n = KOKI_MIX_CHUNK_FRAMES;
        pull_chunk(m, out + done, n);
        done += n;
    }
    return done;
}

int32_t koki_mixer_live_sfx(koki_mixer *m)
{
    int32_t n;

    if (m == NULL)
        return 0;
    (void)pthread_mutex_lock(&m->lock);
    n = (int32_t)m->n_sfx;
    (void)pthread_mutex_unlock(&m->lock);
    return n;
}

bool koki_mixer_music_live(koki_mixer *m)
{
    bool live;

    if (m == NULL)
        return false;
    (void)pthread_mutex_lock(&m->lock);
    live = m->music != NULL;
    (void)pthread_mutex_unlock(&m->lock);
    return live;
}

/* The counter is under the mixer's lock rather than a bare word shared
 * between the feeder and the game loop: koki_sound_check() reads it every
 * 30 frames from the other thread, and an unsynchronised uint32_t there is a
 * data race whatever the hardware does about it in practice. */
void koki_mixer_note_underrun(koki_mixer *m)
{
    if (m == NULL)
        return;
    (void)pthread_mutex_lock(&m->lock);
    m->underruns++;
    (void)pthread_mutex_unlock(&m->lock);
}

uint32_t koki_mixer_underruns(koki_mixer *m)
{
    uint32_t n;

    if (m == NULL)
        return 0u;
    (void)pthread_mutex_lock(&m->lock);
    n = m->underruns;
    (void)pthread_mutex_unlock(&m->lock);
    return n;
}
