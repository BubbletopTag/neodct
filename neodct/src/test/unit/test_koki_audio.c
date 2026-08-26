/* test_koki_audio.c -- the Koki mixer: does music actually play underneath a
 * sound effect, and is the arithmetic the arithmetic engine.py specifies.
 *
 * ============ WHAT THIS CAN AND CANNOT SHOW ============
 *
 * THERE IS NO SOUND CARD ON ANY MACHINE THIS PORT HAS RUN ON. /dev/snd does
 * not exist here and `aplay` is not installed, so nothing below listens to
 * anything. What is checked is everything up to the socket: the saturating
 * fold and the order it folds in, the streaming decode, the looping wrap,
 * the resampler's degenerate case, the voice policy, and the two sums the
 * latency design rests on. Whether 30 ms of ALSA ring is enough on the
 * phone's USB card is not knowable from here and is not claimed.
 *
 * ============ HOW "THEY OVERLAP" IS DEMONSTRATED ============
 *
 * Not by asserting that four voices are registered -- a mixer that dropped
 * three of them on the floor would pass that. Four separate mixers each play
 * ONE of the four sounds and their output is captured; then one mixer plays
 * all four and its output is compared, sample by sample, against the
 * saturating pairwise fold of the four captures. If any voice were missing,
 * silent, or folded in the wrong order, thousands of samples would differ.
 *
 * The assets are the real shipped ones under assets/snd, discovered by
 * scanning the directory rather than by hard-coded md5 filenames, so a
 * re-baked asset set does not silently turn this into a test of nothing.
 *
 * ============ WHY IT dlopen()s app.so ============
 *
 * The same reason test_koki.c does: the Makefile links a test against
 * libneodct and nothing else, and recompiling koki_mixer.c into this binary
 * would test a second copy of the source. Load the artefact that ships.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_types.h"

#include "../../apps/Koki/koki.h"
#include "../../apps/Koki/koki_audio_priv.h"

#define KOKI_ASSETS "/NeoDCT/System/apps/Koki/assets"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
        }                                                                    \
    } while (0)

#define CHECK_INT(got, want, what)                                                              \
    do {                                                                                        \
        long long g_ = (long long)(got);                                                        \
        long long w_ = (long long)(want);                                                       \
        g_checks++;                                                                             \
        if (g_ != w_) {                                                                         \
            g_failures++;                                                                       \
            fprintf(stderr, "FAIL %s:%d  %s: got %lld want %lld\n", __FILE__, __LINE__, (what), \
                    g_, w_);                                                                    \
        }                                                                                       \
    } while (0)

/* ------------------------------------------------------------------ *
 * The slice of app.so this file needs
 * ------------------------------------------------------------------ */

static struct {
    koki_mixer *(*mixer_new)(const char *);
    void (*mixer_free)(koki_mixer *);
    void (*music)(koki_mixer *, const char *);
    void (*sfx)(koki_mixer *, const char *);
    void (*stop_music)(koki_mixer *);
    void (*stop_all)(koki_mixer *);
    size_t (*pull)(koki_mixer *, int16_t *, size_t);
    int32_t (*live_sfx)(koki_mixer *);
    bool (*music_live)(koki_mixer *);
    void (*note_underrun)(koki_mixer *);
    uint32_t (*underruns)(koki_mixer *);
    int16_t (*mix_add)(int16_t, int16_t);
    int32_t (*latency_ms)(int32_t, int32_t);
    bool (*underrun)(double, double, int32_t);
    struct koki_sink *(*sink_start)(koki_mixer *, const char **);
    void (*sink_stop)(struct koki_sink *);
    int32_t (*sink_sock_bytes)(const struct koki_sink *);
    int32_t (*sink_alsa_ms)(const struct koki_sink *);
} api;

static void *g_so_handle;
static char g_so[ND_PATH_MAX];
static char g_stage[ND_PATH_MAX];
static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];
static bool g_stage_is_temp;

static bool api_open(const char *so)
{
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);

    if (h == NULL) {
        fprintf(stderr, "test_koki_audio: dlopen %s: %s\n", so, dlerror());
        return false;
    }
    g_so_handle = h;

    *(void **)&api.mixer_new = dlsym(h, "koki_mixer_new");
    *(void **)&api.mixer_free = dlsym(h, "koki_mixer_free");
    *(void **)&api.music = dlsym(h, "koki_mixer_music");
    *(void **)&api.sfx = dlsym(h, "koki_mixer_sfx");
    *(void **)&api.stop_music = dlsym(h, "koki_mixer_stop_music");
    *(void **)&api.stop_all = dlsym(h, "koki_mixer_stop_all");
    *(void **)&api.pull = dlsym(h, "koki_mixer_pull");
    *(void **)&api.live_sfx = dlsym(h, "koki_mixer_live_sfx");
    *(void **)&api.music_live = dlsym(h, "koki_mixer_music_live");
    *(void **)&api.note_underrun = dlsym(h, "koki_mixer_note_underrun");
    *(void **)&api.underruns = dlsym(h, "koki_mixer_underruns");
    *(void **)&api.mix_add = dlsym(h, "koki_mix_add");
    *(void **)&api.latency_ms = dlsym(h, "koki_mix_latency_ms");
    *(void **)&api.underrun = dlsym(h, "koki_mix_underrun");
    *(void **)&api.sink_start = dlsym(h, "koki_sink_start");
    *(void **)&api.sink_stop = dlsym(h, "koki_sink_stop");
    *(void **)&api.sink_sock_bytes = dlsym(h, "koki_sink_sock_bytes");
    *(void **)&api.sink_alsa_ms = dlsym(h, "koki_sink_alsa_ms");

    return api.mixer_new != NULL && api.mixer_free != NULL && api.music != NULL &&
           api.sfx != NULL && api.stop_music != NULL && api.stop_all != NULL && api.pull != NULL &&
           api.live_sfx != NULL && api.music_live != NULL && api.note_underrun != NULL &&
           api.underruns != NULL && api.mix_add != NULL && api.latency_ms != NULL &&
           api.underrun != NULL && api.sink_start != NULL && api.sink_stop != NULL &&
           api.sink_sock_bytes != NULL && api.sink_alsa_ms != NULL;
}

/* ------------------------------------------------------------------ *
 * The staged root -- test_koki.c's, unchanged
 * ------------------------------------------------------------------ */

static bool file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static bool find_reference_dirs(void)
{
    const char *golden = getenv("NEODCT_GOLDEN");

    if (golden != NULL && golden[0] != '\0')
        (void)nd_strlcpy(g_golden, golden, sizeof g_golden);
    else if (file_exists("../tests/golden/manifest.json"))
        (void)nd_strlcpy(g_golden, "../tests/golden", sizeof g_golden);
    else if (file_exists("neodct/tests/golden/manifest.json"))
        (void)nd_strlcpy(g_golden, "neodct/tests/golden", sizeof g_golden);
    else
        return false;
    return nd_snprintf(g_overlay, sizeof g_overlay, "%s/../../overlay", g_golden) == ND_OK;
}

static bool stage_root(void)
{
    char tmpl[ND_PATH_MAX];
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    char user[ND_PATH_MAX];
    const char *base = getenv("TMPDIR");

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndkokia-XXXXXX", base) != ND_OK)
        return false;
    if (mkdtemp(tmpl) == NULL)
        return false;
    (void)nd_strlcpy(g_stage, tmpl, sizeof g_stage);
    g_stage_is_temp = true;

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        return false;
    (void)mkdir(neodct, 0755);
    if (nd_snprintf(sys_link, sizeof sys_link, "%s/System", neodct) != ND_OK)
        return false;
    if (nd_snprintf(sys_target, sizeof sys_target, "%s/NeoDCT/System", g_overlay) != ND_OK)
        return false;
    if (symlink(sys_target, sys_link) != 0 && errno != EEXIST)
        return false;
    if (nd_snprintf(user, sizeof user, "%s/User", neodct) != ND_OK)
        return false;
    (void)mkdir(user, 0755);
    return nd_path_set_root(g_stage) == ND_OK;
}

static int unlink_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

static void unstage(void)
{
    (void)nd_path_set_root(NULL);
    if (g_stage_is_temp && g_stage[0] != '\0')
        (void)nftw(g_stage, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static bool resolve_app_so(void)
{
    const char *env = getenv("NEODCT_KOKI_SO");
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(g_so, env, sizeof g_so);
        return true;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[(size_t)n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(g_so, sizeof g_so, "%s/../apps/Koki/app.so", exe) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * Picking real assets, by scanning rather than by md5
 * ------------------------------------------------------------------ */

#define PICK_MAX 8

typedef struct {
    char rel[128]; /* "snd/<name>", what the manifest holds */
    long size;
} asset;

static int by_size_then_name(const void *a, const void *b)
{
    const asset *x = (const asset *)a;
    const asset *y = (const asset *)b;

    if (x->size != y->size)
        return (x->size < y->size) ? -1 : 1;
    return strcmp(x->rel, y->rel);
}

/* The `n` smallest files with extension `ext`, smallest first. Smallest so
 * the streaming tests decode whole files quickly; deterministic so a rerun
 * exercises the same bytes. */
static size_t pick_assets(const char *ext, asset *out, size_t n)
{
    char dir_virt[ND_PATH_MAX];
    char dir_real[ND_PATH_MAX];
    asset all[128];
    size_t n_all = 0u;
    size_t elen = strlen(ext);
    DIR *d;
    struct dirent *e;
    size_t i;

    if (nd_snprintf(dir_virt, sizeof dir_virt, "%s/snd", KOKI_ASSETS) != ND_OK)
        return 0u;
    if (nd_path_resolve(dir_real, sizeof dir_real, dir_virt) != ND_OK)
        return 0u;
    d = opendir(dir_real);
    if (d == NULL)
        return 0u;
    while ((e = readdir(d)) != NULL && n_all < ND_ARRAY_LEN(all)) {
        char full[ND_PATH_MAX];
        struct stat st;
        size_t len = strlen(e->d_name);

        if (len <= elen || strcmp(e->d_name + len - elen, ext) != 0)
            continue;
        if (nd_snprintf(full, sizeof full, "%s/%s", dir_real, e->d_name) != ND_OK)
            continue;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (nd_snprintf(all[n_all].rel, sizeof all[n_all].rel, "snd/%s", e->d_name) != ND_OK)
            continue;
        all[n_all].size = (long)st.st_size;
        n_all++;
    }
    (void)closedir(d);
    if (n_all == 0u)
        return 0u;
    qsort(all, n_all, sizeof all[0], by_size_then_name);
    if (n > n_all)
        n = n_all;
    for (i = 0u; i < n; i++)
        out[i] = all[i];
    return n;
}

/* ------------------------------------------------------------------ *
 * A minimal WAV reader, so the mixer's output has an oracle that is not
 * the mixer. Mono 16-bit PCM only, which is what all 38 shipped WAVs are.
 * ------------------------------------------------------------------ */

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* owned by the caller; free(). *frames receives the sample count. */
static int16_t *wav_read(const char *rel, size_t *frames, uint32_t *rate, uint16_t *channels)
{
    char virt[ND_PATH_MAX];
    char real[ND_PATH_MAX];
    uint8_t hdr[8];
    FILE *f;
    int16_t *pcm = NULL;
    uint8_t riff[12];
    bool have_fmt = false;

    *frames = 0u;
    *rate = 0u;
    *channels = 0u;

    if (nd_snprintf(virt, sizeof virt, "%s/%s", KOKI_ASSETS, rel) != ND_OK)
        return NULL;
    if (nd_path_resolve(real, sizeof real, virt) != ND_OK)
        return NULL;
    f = fopen(real, "rb");
    if (f == NULL)
        return NULL;
    if (fread(riff, 1u, sizeof riff, f) != sizeof riff || memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        (void)fclose(f);
        return NULL;
    }
    while (fread(hdr, 1u, sizeof hdr, f) == sizeof hdr) {
        uint32_t sz = rd_le32(hdr + 4);

        if (memcmp(hdr, "fmt ", 4) == 0 && sz >= 16u) {
            uint8_t fmt[16];

            if (fread(fmt, 1u, sizeof fmt, f) != sizeof fmt)
                break;
            *channels = rd_le16(fmt + 2);
            *rate = rd_le32(fmt + 4);
            if (rd_le16(fmt) != 1u || rd_le16(fmt + 14) != 16u)
                break; /* not plain 16-bit PCM */
            have_fmt = true;
            if (sz > 16u && fseek(f, (long)(sz - 16u), SEEK_CUR) != 0)
                break;
        } else if (memcmp(hdr, "data", 4) == 0) {
            size_t n;

            if (!have_fmt || *channels == 0u)
                break;
            n = (size_t)sz / 2u;
            /* owned by the caller */
            pcm = malloc((n == 0u ? 1u : n) * sizeof *pcm);
            if (pcm == NULL)
                break;
            if (fread(pcm, sizeof *pcm, n, f) != n) {
                free(pcm);
                pcm = NULL;
                break;
            }
            *frames = n / *channels;
            break;
        } else {
            if (fseek(f, (long)sz + (sz & 1u ? 1 : 0), SEEK_CUR) != 0)
                break;
        }
    }
    (void)fclose(f);
    return pcm;
}

/* ------------------------------------------------------------------ *
 * 1. The three pure sums
 * ------------------------------------------------------------------ */

static void test_arithmetic(void)
{
    /* audioop.add(a, b, 2). */
    CHECK_INT(api.mix_add(0, 0), 0, "add 0+0");
    CHECK_INT(api.mix_add(100, -100), 0, "add cancels");
    CHECK_INT(api.mix_add(20000, 10000), 30000, "add below the ceiling");
    CHECK_INT(api.mix_add(30000, 30000), 32767, "add saturates high");
    CHECK_INT(api.mix_add(-30000, -30000), -32768, "add saturates low");
    CHECK_INT(api.mix_add(32767, 1), 32767, "add pinned high");
    CHECK_INT(api.mix_add(-32768, -1), -32768, "add pinned low");
    CHECK_INT(api.mix_add(32767, -32768), -1, "add spans the range");

    /* README-PORT.md "Decision 4": pairwise is NOT one wide accumulator, and
     * this is the case that shows it. If someone ever "simplifies" the fold
     * into an int32 sum with one clamp, this line goes red. */
    CHECK_INT(api.mix_add(api.mix_add(30000, 30000), -30000), 2767, "the fold is pairwise");

    /* koki_mix_latency_ms: one chunk, plus the socket's payload, plus the
     * ALSA ring. 128 frames is 5.805 ms; 1024 bytes is 512 frames, 23.2 ms;
     * plus 30 ms of ring is 59 ms, which is the figure in README-PORT.md. */
    CHECK_INT(api.latency_ms(0, 0), 6, "latency: a chunk alone, rounded");
    CHECK_INT(api.latency_ms(1024, 30), 59, "latency: the shipped budget");
    CHECK_INT(api.latency_ms(1024, 0), 29, "latency: chunk plus socket");
    CHECK(api.latency_ms(0, 500) > api.latency_ms(0, 30), "latency grows with the ring");
    /* A default AF_UNIX send buffer, which is what this design exists to
     * avoid: 212,992 bytes is very nearly a second of lag. */
    CHECK(api.latency_ms(212992, 30) > 900, "an untuned socket is a second behind");

    /* koki_mix_underrun: the wall clock must stay behind the audio clock. */
    CHECK(!api.underrun(0.0, 100.0, 30), "fresh start is not an underrun");
    CHECK(!api.underrun(100.0, 100.0, 30), "level with the audio clock is fine");
    CHECK(!api.underrun(125.0, 100.0, 30), "inside the guard is fine");
    CHECK(api.underrun(131.0, 100.0, 30), "past the guard is an underrun");
    CHECK(!api.underrun(131.0, 100.0, 100), "a bigger ring forgives more");
}

/* ------------------------------------------------------------------ *
 * 2. One voice, sample for sample against the file itself
 * ------------------------------------------------------------------ */

/* Every shipped asset is already 22050 Hz mono, so the resampler's frac is
 * zero on every output frame and the whole path must be a bit-exact copy.
 * If it is not, something is interpolating that should not be. */
static void test_passthrough(const asset *wav)
{
    koki_mixer *m;
    int16_t *ref;
    int16_t *got;
    size_t frames = 0u;
    size_t n;
    size_t i;
    size_t bad = 0u;
    uint32_t rate = 0u;
    uint16_t channels = 0u;

    ref = wav_read(wav->rel, &frames, &rate, &channels);
    if (ref == NULL || frames == 0u) {
        CHECK(false, "cannot read the reference WAV");
        free(ref);
        return;
    }
    CHECK_INT(rate, KOKI_MIX_RATE, "the shipped WAV is already at the mix rate");
    CHECK_INT(channels, 1, "the shipped WAV is already mono");

    m = api.mixer_new(KOKI_ASSETS);
    CHECK(m != NULL, "mixer_new");
    if (m == NULL) {
        free(ref);
        return;
    }

    n = frames + 256u; /* run past the end, to see the silence after it */
    got = calloc(n, sizeof *got);
    CHECK(got != NULL, "scratch");
    if (got == NULL) {
        api.mixer_free(m);
        free(ref);
        return;
    }

    api.sfx(m, wav->rel);
    CHECK_INT(api.live_sfx(m), 1, "one effect is live");
    CHECK_INT(api.pull(m, got, n), n, "pull fills the whole request");

    for (i = 0u; i < frames; i++) {
        if (got[i] != ref[i])
            bad++;
    }
    CHECK_INT(bad, 0, "a 22050 Hz mono WAV comes out bit-exact");

    bad = 0u;
    for (i = frames; i < n; i++) {
        if (got[i] != 0)
            bad++;
    }
    CHECK_INT(bad, 0, "a one-shot is silence once it ends");
    CHECK_INT(api.live_sfx(m), 0, "a finished effect is pruned");

    api.mixer_free(m);
    free(got);
    free(ref);
}

/* The same file as MUSIC must wrap seamlessly: the stream is exactly
 * periodic with the file's own length, with no gap and no click at the seam.
 * That is the whole difference between music and an effect here. */
static void test_loop(const asset *wav)
{
    koki_mixer *m;
    int16_t *ref;
    int16_t *got;
    size_t frames = 0u;
    size_t n;
    size_t i;
    size_t bad = 0u;
    uint32_t rate = 0u;
    uint16_t channels = 0u;

    ref = wav_read(wav->rel, &frames, &rate, &channels);
    if (ref == NULL || frames == 0u) {
        CHECK(false, "cannot read the reference WAV");
        free(ref);
        return;
    }

    m = api.mixer_new(KOKI_ASSETS);
    CHECK(m != NULL, "mixer_new");
    if (m == NULL) {
        free(ref);
        return;
    }

    n = frames * 3u + 17u; /* three laps and a bit, so the seam is crossed */
    got = calloc(n, sizeof *got);
    CHECK(got != NULL, "scratch");
    if (got == NULL) {
        api.mixer_free(m);
        free(ref);
        return;
    }

    api.music(m, wav->rel);
    CHECK(api.music_live(m), "music is live");
    (void)api.pull(m, got, n);

    for (i = 0u; i < n; i++) {
        if (got[i] != ref[i % frames])
            bad++;
    }
    CHECK_INT(bad, 0, "music loops seamlessly and sample-exactly");
    CHECK(api.music_live(m), "music is still live after three laps");

    api.stop_music(m);
    CHECK(!api.music_live(m), "stop_music drops the voice");
    (void)memset(got, 0x7f, n * sizeof *got);
    (void)api.pull(m, got, 64u);
    bad = 0u;
    for (i = 0u; i < 64u; i++) {
        if (got[i] != 0)
            bad++;
    }
    CHECK_INT(bad, 0, "nothing playing is silence, not stale samples");

    api.mixer_free(m);
    free(got);
    free(ref);
}

/* ------------------------------------------------------------------ *
 * 3. THE POINT: music and effects in one stream at the same time
 * ------------------------------------------------------------------ */

#define OVERLAP_FRAMES 4000u

static void capture(const asset *a, bool as_music, int16_t *out, size_t n)
{
    koki_mixer *m = api.mixer_new(KOKI_ASSETS);

    (void)memset(out, 0, n * sizeof *out);
    if (m == NULL) {
        CHECK(false, "mixer_new for the reference capture");
        return;
    }
    if (as_music)
        api.music(m, a->rel);
    else
        api.sfx(m, a->rel);
    (void)api.pull(m, out, n);
    api.mixer_free(m);
}

static void test_overlap(const asset *w, size_t n_wav, const asset *mp3, bool have_mp3)
{
    const size_t n = OVERLAP_FRAMES;
    int16_t *ref[4];
    int16_t *mixed;
    int16_t *want;
    const asset *music_src;
    size_t i;
    size_t k;
    size_t bad = 0u;
    size_t nonzero = 0u;
    size_t differs_from_each[4] = {0u, 0u, 0u, 0u};
    koki_mixer *m;

    if (n_wav < 3u) {
        CHECK(false, "need three WAV effects to test overlap");
        return;
    }
    /* Music is an MP3 when there is one, because that is the shipped
     * arrangement -- 19 MP3 tracks and 38 WAV effects -- and because it puts
     * two different decoders in the same sum. */
    music_src = have_mp3 ? mp3 : &w[3];

    for (k = 0u; k < 4u; k++) {
        ref[k] = calloc(n, sizeof *ref[k]);
        CHECK(ref[k] != NULL, "reference buffer");
        if (ref[k] == NULL)
            return;
    }
    mixed = calloc(n, sizeof *mixed);
    want = calloc(n, sizeof *want);
    CHECK(mixed != NULL && want != NULL, "mix buffers");
    if (mixed == NULL || want == NULL)
        return;

    /* Four mixers, one voice each. */
    capture(&w[0], false, ref[0], n);
    capture(&w[1], false, ref[1], n);
    capture(&w[2], false, ref[2], n);
    capture(music_src, true, ref[3], n);

    /* One mixer, all four. The order matters and is the Python's: the
     * effects in the order they started, then music last. */
    m = api.mixer_new(KOKI_ASSETS);
    CHECK(m != NULL, "mixer_new");
    if (m == NULL)
        return;
    api.music(m, music_src->rel);
    api.sfx(m, w[0].rel);
    api.sfx(m, w[1].rel);
    api.sfx(m, w[2].rel);
    CHECK_INT(api.live_sfx(m), 3, "three effects live");
    CHECK(api.music_live(m), "and music underneath them");
    (void)api.pull(m, mixed, n);

    for (i = 0u; i < n; i++) {
        int16_t v = api.mix_add(api.mix_add(ref[0][i], ref[1][i]), ref[2][i]);

        v = api.mix_add(v, ref[3][i]);
        want[i] = v;
        if (mixed[i] != v)
            bad++;
        if (mixed[i] != 0)
            nonzero++;
        for (k = 0u; k < 4u; k++) {
            if (mixed[i] != ref[k][i])
                differs_from_each[k]++;
        }
    }

    CHECK_INT(bad, 0, "the mixed stream IS the pairwise fold of all four voices");
    CHECK(nonzero > n / 4u, "the mixed stream carries real audio, not silence");
    /* Each of these would be zero if the mixer were quietly playing one
     * voice and dropping the rest -- which is the failure this whole test
     * exists to rule out. */
    for (k = 0u; k < 4u; k++)
        CHECK(differs_from_each[k] > 0u, "the mix is not merely one of its voices");

    api.mixer_free(m);
    for (k = 0u; k < 4u; k++)
        free(ref[k]);
    free(mixed);
    free(want);
}

/* ------------------------------------------------------------------ *
 * 4. The voice policy: three, and the fourth is dropped
 * ------------------------------------------------------------------ */

static void test_voice_policy(const asset *w, size_t n_wav)
{
    koki_mixer *m;
    int16_t buf[512];
    size_t i;

    if (n_wav < 4u) {
        CHECK(false, "need four WAVs for the policy test");
        return;
    }
    m = api.mixer_new(KOKI_ASSETS);
    CHECK(m != NULL, "mixer_new");
    if (m == NULL)
        return;

    CHECK_INT(api.live_sfx(m), 0, "a new mixer has no voices");
    CHECK(!api.music_live(m), "and no music");

    for (i = 0u; i < 4u; i++)
        api.sfx(m, w[i].rel);
    CHECK_INT(api.live_sfx(m), KOKI_SND_MAX_SFX, "the fourth effect is dropped, not queued");

    /* Music is a separate voice and is NOT counted against the three. */
    api.music(m, w[0].rel);
    CHECK(api.music_live(m), "music plays alongside three effects");
    CHECK_INT(api.live_sfx(m), KOKI_SND_MAX_SFX, "music did not evict an effect");

    /* play_music REPLACES rather than stacking. */
    api.music(m, w[1].rel);
    CHECK(api.music_live(m), "the second track replaced the first");

    /* An unknown name changes nothing and must not crash. */
    api.sfx(m, "snd/definitely-not-here.wav");
    api.music(m, "snd/definitely-not-here.mp3");
    CHECK_INT(api.live_sfx(m), KOKI_SND_MAX_SFX, "a missing effect changes nothing");
    CHECK(api.music_live(m), "a missing track leaves the current one playing");

    /* The grade screens call stop_all mid-game. */
    api.stop_all(m);
    CHECK_INT(api.live_sfx(m), 0, "stop_all drops every effect");
    CHECK(!api.music_live(m), "stop_all drops the music too");
    (void)memset(buf, 0x5a, sizeof buf);
    (void)api.pull(m, buf, ND_ARRAY_LEN(buf));
    CHECK_INT(buf[0], 0, "and the stream goes silent");
    CHECK_INT(buf[ND_ARRAY_LEN(buf) - 1u], 0, "all the way to the end of the chunk");

    /* Room comes back once the short effects have run out, which is the
     * prune engine.py does at the top of play_sfx. */
    api.sfx(m, w[0].rel);
    CHECK_INT(api.live_sfx(m), 1, "a slot is free again");

    /* NULL and empty must be inert -- app_shutdown() can reach these. */
    api.sfx(m, NULL);
    api.music(m, "");
    api.sfx(NULL, w[0].rel);
    api.stop_all(NULL);
    api.stop_music(NULL);
    api.mixer_free(NULL);
    CHECK_INT(api.pull(NULL, buf, 8u), 0, "pull on a NULL mixer is 0, not a crash");

    CHECK_INT(api.underruns(m), 0, "no underruns without a sink");
    api.note_underrun(m);
    api.note_underrun(m);
    CHECK_INT(api.underruns(m), 2, "the underrun counter counts");

    api.mixer_free(m);
}

/* ------------------------------------------------------------------ *
 * 5. What it costs -- measured, not estimated
 * ------------------------------------------------------------------ */

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long total = 0;
    long resident = 0;

    if (f == NULL)
        return 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2)
        resident = 0;
    (void)fclose(f);
    return resident * (long)sysconf(_SC_PAGESIZE) / 1024L;
}

static void test_footprint(const asset *w, size_t n_wav, const asset *mp3, bool have_mp3)
{
    koki_mixer *m;
    int16_t buf[KOKI_MIX_CHUNK_FRAMES];
    long before;
    long peak;
    long after;
    size_t i;

    if (n_wav < 3u)
        return;

    /* Touch the decoders once first, so what is measured is the mixer's
     * working set and not the first-fault cost of app.so's own pages. */
    m = api.mixer_new(KOKI_ASSETS);
    if (m != NULL) {
        api.sfx(m, w[0].rel);
        if (have_mp3)
            api.music(m, mp3->rel);
        for (i = 0u; i < 40u; i++)
            (void)api.pull(m, buf, ND_ARRAY_LEN(buf));
        api.mixer_free(m);
    }

    before = rss_kb();

    m = api.mixer_new(KOKI_ASSETS);
    CHECK(m != NULL, "mixer_new");
    if (m == NULL)
        return;
    api.music(m, have_mp3 ? mp3->rel : w[2].rel);
    api.sfx(m, w[0].rel);
    api.sfx(m, w[1].rel);
    api.sfx(m, w[2].rel);
    for (i = 0u; i < 200u; i++)
        (void)api.pull(m, buf, ND_ARRAY_LEN(buf));
    peak = rss_kb();

    api.mixer_free(m);
    after = rss_kb();

    printf("test_koki_audio: RSS idle %ld kB, four voices %ld kB (+%ld), after free %ld kB\n",
           before, peak, peak - before, after);
#if defined(__SANITIZE_ADDRESS__)
    /* Under ASAN the number is the sanitizer's, not the phone's: redzones
     * round every allocation up and the quarantine holds freed chunks, so
     * the same four voices measure about 1.1 MB. Print it and move on --
     * asserting on it would only be asserting about ASAN. */
    ND_UNUSED(after);
#else
    /* spec-koki.md budgets ~200 kB for audio; the design table works out at
     * ~146 kB for four voices and the measurement lands near 68 kB, because
     * only the music voice is an MP3 and drwav's state is 408 bytes. Half a
     * megabyte is a loose ceiling that still catches the failure that
     * matters: somebody decoding a whole 183-second track into memory. */
    CHECK(peak - before < 512, "four live voices cost well under half a megabyte");
#endif
}

/* ------------------------------------------------------------------ *
 * 6. The sink -- the half that has a fork and a thread in it
 * ------------------------------------------------------------------ *
 *
 * There is no aplay on this machine and no /dev/snd, so koki_sound_open()
 * would stop long before any of this. The test therefore puts its OWN
 * `aplay` on PATH -- a two-line shell script that keeps the first 64 kB of
 * its stdin and exits -- and drives koki_sink_start() directly through
 * koki_audio_priv.h.
 *
 * What that actually exercises is the risky part: the socketpair and the
 * SO_SNDBUF the whole latency design depends on, a fork that must happen
 * before the feeder thread exists, the feeder unwinding on EPIPE when the
 * player goes away, and a teardown whose order decides whether a descriptor
 * is closed under a thread still sitting in send(). Under ASAN and with the
 * bytes compared against a reference mixer, all four of those are checked at
 * once.
 *
 * What it does NOT check is that a real aplay accepts these arguments, or
 * that 30 ms of ALSA ring survives the phone's USB card. Neither is knowable
 * here.
 */

#define FAKE_PLAYER_BYTES 65536

static char g_fakedir[ND_PATH_MAX];
static char g_capture[ND_PATH_MAX];

/* A stand-in for aplay: read raw PCM on stdin, keep a bounded prefix, exit.
 * Exiting on its own is the point -- it makes the feeder meet a real EPIPE
 * instead of only ever being torn down from the outside. */
static bool fake_player_install(void)
{
    char script[ND_PATH_MAX];
    char path[ND_PATH_MAX * 2];
    const char *old_path = getenv("PATH");
    const char *base = getenv("TMPDIR");
    FILE *f;

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (nd_snprintf(g_fakedir, sizeof g_fakedir, "%s/ndkokibin-XXXXXX", base) != ND_OK)
        return false;
    if (mkdtemp(g_fakedir) == NULL)
        return false;
    if (nd_snprintf(g_capture, sizeof g_capture, "%s/pcm.raw", g_fakedir) != ND_OK)
        return false;
    if (nd_snprintf(script, sizeof script, "%s/aplay", g_fakedir) != ND_OK)
        return false;

    f = fopen(script, "w");
    if (f == NULL)
        return false;
    (void)fprintf(f, "#!/bin/sh\nexec head -c %d > '%s'\n", FAKE_PLAYER_BYTES, g_capture);
    if (fclose(f) != 0)
        return false;
    if (chmod(script, 0755) != 0)
        return false;

    if (old_path == NULL)
        old_path = "/usr/bin:/bin";
    if (nd_snprintf(path, sizeof path, "%s:%s", g_fakedir, old_path) != ND_OK)
        return false;
    return setenv("PATH", path, 1) == 0;
}

static void fake_player_remove(void)
{
    if (g_fakedir[0] != '\0')
        (void)nftw(g_fakedir, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
    g_fakedir[0] = '\0';
}

static long file_size(const char *path)
{
    struct stat st;

    return (stat(path, &st) == 0) ? (long)st.st_size : -1;
}

static void nap_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* NEODCT_KOKI_ABUF_MS is read inside koki_sink_start(), so the clamp is
 * checked through a real start rather than by re-implementing the parse. */
static void check_abuf(const char *value, int32_t want, const char *what, const asset *w)
{
    koki_mixer *m = api.mixer_new(KOKI_ASSETS);
    struct koki_sink *s;
    const char *why = "";

    if (m == NULL) {
        CHECK(false, "mixer_new");
        return;
    }
    if (value != NULL)
        (void)setenv("NEODCT_KOKI_ABUF_MS", value, 1);
    else
        (void)unsetenv("NEODCT_KOKI_ABUF_MS");

    api.sfx(m, w->rel);
    s = api.sink_start(m, &why);
    if (s == NULL) {
        CHECK(false, what);
        api.mixer_free(m);
        return;
    }
    CHECK_INT(api.sink_alsa_ms(s), want, what);
    api.sink_stop(s);
    api.mixer_free(m);
    (void)unsetenv("NEODCT_KOKI_ABUF_MS");
}

static void test_sink(const asset *w, size_t n_wav, const asset *mp3, bool have_mp3)
{
    koki_mixer *m;
    struct koki_sink *s;
    const char *why = "";
    const size_t n = (size_t)FAKE_PLAYER_BYTES / sizeof(int16_t);
    int16_t *want;
    int16_t *got;
    const asset *music_src;
    int32_t sock_bytes;
    int32_t alsa_ms;
    int32_t latency;
    size_t bad = 0u;
    size_t i;
    long waited = 0;
    FILE *f;

    if (n_wav < 3u) {
        CHECK(false, "need three WAVs for the sink test");
        return;
    }
    music_src = have_mp3 ? mp3 : &w[2];

    /* The oracle: what the mixer produces for exactly these voices, with no
     * socket and no thread anywhere near it. */
    want = calloc(n, sizeof *want);
    got = calloc(n, sizeof *got);
    CHECK(want != NULL && got != NULL, "sink buffers");
    if (want == NULL || got == NULL) {
        free(want);
        free(got);
        return;
    }
    m = api.mixer_new(KOKI_ASSETS);
    if (m == NULL) {
        CHECK(false, "mixer_new");
        free(want);
        free(got);
        return;
    }
    api.music(m, music_src->rel);
    api.sfx(m, w[0].rel);
    api.sfx(m, w[1].rel);
    (void)api.pull(m, want, n);
    api.mixer_free(m);

    /* The same voices again, this time through the socket and the thread. */
    m = api.mixer_new(KOKI_ASSETS);
    if (m == NULL) {
        CHECK(false, "mixer_new");
        free(want);
        free(got);
        return;
    }
    api.music(m, music_src->rel);
    api.sfx(m, w[0].rel);
    api.sfx(m, w[1].rel);

    s = api.sink_start(m, &why);
    CHECK(s != NULL, "the sink starts against a player on PATH");
    if (s == NULL) {
        fprintf(stderr, "  (sink_start: %s)\n", why);
        api.mixer_free(m);
        free(want);
        free(got);
        return;
    }

    sock_bytes = api.sink_sock_bytes(s);
    alsa_ms = api.sink_alsa_ms(s);
    latency = api.latency_ms(sock_bytes, alsa_ms);
    printf("test_koki_audio: socket holds %d bytes (%.1f ms), ALSA ring %d ms, total %d ms "
           "(%.1f frames at 30 FPS)\n",
           sock_bytes, (double)sock_bytes / 2.0 * 1000.0 / (double)KOKI_MIX_RATE, alsa_ms, latency,
           (double)latency / (1000.0 / 30.0));

    CHECK(sock_bytes > 0, "the granted send buffer is readable");
    CHECK_INT(alsa_ms, KOKI_MIX_ALSA_MS, "the ALSA ring is the default");
    /* The design's whole claim. If a kernel's SOCK_MIN_SNDBUF floor were
     * ever big enough to break it, this is where it would be found rather
     * than on the phone. */
    CHECK(latency < 100, "end-to-end latency stays inside three frames");

    /* Wait for the player to take its fill and exit on its own. */
    while (file_size(g_capture) < (long)FAKE_PLAYER_BYTES && waited < 10000) {
        nap_ms(10);
        waited += 10;
    }
    CHECK(waited < 10000, "the feeder fed the player without wedging");

    /* THE TEARDOWN ORDER. Under ASAN a descriptor closed under a blocked
     * send(), or a thread left running past the free, is a report rather
     * than a rare flake. */
    api.sink_stop(s);
    api.mixer_free(m);

    CHECK_INT(file_size(g_capture), FAKE_PLAYER_BYTES, "the player received a full buffer");
    f = fopen(g_capture, "rb");
    CHECK(f != NULL, "the captured PCM is readable");
    if (f != NULL) {
        size_t rd = fread(got, sizeof *got, n, f);

        (void)fclose(f);
        CHECK_INT(rd, n, "captured the whole prefix");
        for (i = 0u; i < rd; i++) {
            if (got[i] != want[i])
                bad++;
        }
        CHECK_INT(bad, 0, "what reached the player is exactly the mixed stream");
    }

    free(want);
    free(got);

    /* engine.py's own knob, and the clamp a malloc-sized-by-environment
     * needs (CODING-STANDARDS 1.5). */
    check_abuf(NULL, KOKI_MIX_ALSA_MS, "ABUF_MS unset -> the default", &w[0]);
    check_abuf("77", 77, "ABUF_MS honoured", &w[0]);
    check_abuf("1", KOKI_MIX_ALSA_MS_MIN, "ABUF_MS clamped up", &w[0]);
    check_abuf("100000", KOKI_MIX_ALSA_MS_MAX, "ABUF_MS clamped down", &w[0]);
    check_abuf("banana", KOKI_MIX_ALSA_MS, "ABUF_MS unparseable is ignored", &w[0]);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    asset wav[PICK_MAX];
    asset mp3[PICK_MAX];
    size_t n_wav;
    size_t n_mp3;

    if (!resolve_app_so() || !api_open(g_so)) {
        fprintf(stderr, "test_koki_audio: cannot load apps/Koki/app.so (%s)\n", g_so);
        return 1;
    }
    if (!find_reference_dirs()) {
        fprintf(stderr, "test_koki_audio: cannot find the reference tree\n");
        return 1;
    }
    if (!stage_root()) {
        fprintf(stderr, "test_koki_audio: cannot stage a root\n");
        unstage();
        return 1;
    }

    n_wav = pick_assets(".wav", wav, PICK_MAX);
    n_mp3 = pick_assets(".mp3", mp3, PICK_MAX);
    if (n_wav < 4u) {
        fprintf(stderr, "test_koki_audio: only %zu WAVs under %s/snd\n", n_wav, KOKI_ASSETS);
        unstage();
        return 1;
    }

    test_arithmetic();
    test_passthrough(&wav[0]);
    test_loop(&wav[0]);
    test_overlap(wav, n_wav, &mp3[0], n_mp3 > 0u);
    test_voice_policy(wav, n_wav);
    test_footprint(wav, n_wav, &mp3[0], n_mp3 > 0u);

    if (fake_player_install()) {
        test_sink(wav, n_wav, &mp3[0], n_mp3 > 0u);
        fake_player_remove();
    } else {
        fprintf(stderr, "test_koki_audio: cannot install the fake player; sink not covered\n");
        g_failures++;
    }

    unstage();
    if (g_so_handle != NULL)
        (void)dlclose(g_so_handle);

    printf("test_koki_audio: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
