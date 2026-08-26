/* test_koki.c -- the Koki engine: arithmetic, the scheduler, and six levels
 * of the game checked frame by frame against the Python.
 *
 * ============ WHERE THE REFERENCE NUMBERS COME FROM ============
 *
 * Two places, and both are the SHIPPED PYTHON rather than a description of
 * it:
 *
 *   - the RNG vectors are CPython's random.Random(seed), reproduced in
 *     spec-koki.md section 12 and re-measured on this checkout;
 *   - the frame digests are System/apps/Koki/tools/smoke.py's six scenarios,
 *     run against overlay/.../engine.py and game.py with the same seed, the
 *     same virtual clock and the same scripted key holds, one digest per
 *     frame, sampled every fifty frames.
 *
 * That second set is the real content of this file. Koki is 3,000 lines of
 * game logic where every constant is load-bearing and almost nothing is
 * reachable from a unit test in isolation: the only honest way to check it is
 * to run it and compare the pixels with the original's. 6,400 frames across
 * six scenarios do that here, and test_koki_frame.c does the stored golden
 * frame separately.
 *
 * ============ WHY IT dlopen()s app.so ============
 *
 * The Makefile links a test against libneodct and nothing else, and
 * recompiling the engine into this binary would test a second copy of the
 * source. test_cubebench.c set the precedent: load the artefact that ships.
 * Everything the engine exports is available that way, which is also how the
 * scheduler probes below register their own scripts.
 *
 * ============ WHAT smoke.py's HARNESS DOES, REPRODUCED HERE ============
 *
 * Its scenarios do not play the game -- they jump into it. Frame 1 stops
 * every script and hides the Dynaris logo, killing the boot sequence, and
 * from there the scenario injects broadcasts at fixed frames and holds keys
 * over fixed spans. The clock is the headless one (a flat 1/30 s per frame,
 * NOT the 0.1 s golden-frame tick), and the RNG is seeded with 42 so the
 * bosses make the same choices every run.
 */

#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_capture.h"
#include "nd_draw.h"
#include "nd_image.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "../../apps/Koki/koki.h"

#define KOKI_APP_DIR "/NeoDCT/System/apps/Koki"

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

#define CHECK_STR(got, want, what)                                                          \
    do {                                                                                    \
        const char *g_ = (got);                                                             \
        const char *w_ = (want);                                                            \
        g_checks++;                                                                         \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                                            \
            g_failures++;                                                                   \
            fprintf(stderr, "FAIL %s:%d  %s: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                    (what), g_ != NULL ? g_ : "(null)", w_);                                \
        }                                                                                   \
    } while (0)

#define CHECK_EXACT(got, want, what)                                                              \
    do {                                                                                          \
        double g_ = (got);                                                                        \
        double w_ = (want);                                                                       \
        g_checks++;                                                                               \
        if (g_ != w_) {                                                                           \
            g_failures++;                                                                         \
            fprintf(stderr, "FAIL %s:%d  %s: got %.17g want %.17g\n", __FILE__, __LINE__, (what), \
                    g_, w_);                                                                      \
        }                                                                                         \
    } while (0)

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

static struct {
    void *handle;
    koki_engine *(*engine_new)(nd_ui *, const char *);
    void (*engine_free)(koki_engine *);
    void (*register_all)(koki_engine *);
    void (*start_flag)(koki_engine *);
    void (*step_frame)(koki_engine *);
    void (*render)(koki_engine *);
    void (*broadcast)(koki_engine *, const char *);
    void (*stop_all)(koki_engine *);
    void (*stop_other)(koki_engine *, const koki_sprite *);
    int32_t (*on)(koki_engine *, const char *, koki_sprite *, koki_script_fn);
    koki_sprite *(*sprite_get)(koki_engine *, const char *);
    void (*hide)(koki_sprite *);
    size_t (*active_count)(const koki_engine *);
    void (*rng_seed)(koki_rng *, uint32_t);
    uint32_t (*rng_u32)(koki_rng *);
    int32_t (*randint)(koki_engine *, int32_t, int32_t);
    void (*lru_init)(koki_lru *, size_t);
    nd_image *(*lru_get)(koki_lru *, const char *);
    void (*lru_put)(koki_lru *, const char *, nd_image *);
    void (*lru_clear)(koki_lru *);
    void (*paste_origin)(koki_sprite *, double *);
    void (*screen_rect)(koki_sprite *, double, double *);
} api;

static bool api_open(const char *so)
{
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);

    if (h == NULL) {
        fprintf(stderr, "test_koki: dlopen %s: %s\n", so, dlerror());
        return false;
    }
    api.handle = h;
    *(void **)&api.engine_new = dlsym(h, "koki_engine_new");
    *(void **)&api.engine_free = dlsym(h, "koki_engine_free");
    *(void **)&api.register_all = dlsym(h, "koki_register_all");
    *(void **)&api.start_flag = dlsym(h, "koki_start_flag");
    *(void **)&api.step_frame = dlsym(h, "koki_step_frame");
    *(void **)&api.render = dlsym(h, "koki_render");
    *(void **)&api.broadcast = dlsym(h, "koki_broadcast");
    *(void **)&api.stop_all = dlsym(h, "koki_stop_all_scripts");
    *(void **)&api.stop_other = dlsym(h, "koki_stop_other_scripts");
    *(void **)&api.on = dlsym(h, "koki_on");
    *(void **)&api.sprite_get = dlsym(h, "koki_sprite_get");
    *(void **)&api.hide = dlsym(h, "koki_hide");
    *(void **)&api.active_count = dlsym(h, "koki_active_count");
    *(void **)&api.rng_seed = dlsym(h, "koki_rng_seed");
    *(void **)&api.rng_u32 = dlsym(h, "koki_rng_u32");
    *(void **)&api.randint = dlsym(h, "koki_randint");
    *(void **)&api.lru_init = dlsym(h, "koki_lru_init");
    *(void **)&api.lru_get = dlsym(h, "koki_lru_get");
    *(void **)&api.lru_put = dlsym(h, "koki_lru_put");
    *(void **)&api.lru_clear = dlsym(h, "koki_lru_clear");
    *(void **)&api.paste_origin = dlsym(h, "koki_paste_origin");
    *(void **)&api.screen_rect = dlsym(h, "koki_screen_rect");

    return api.engine_new && api.engine_free && api.register_all && api.start_flag &&
           api.step_frame && api.render && api.broadcast && api.stop_all && api.stop_other &&
           api.on && api.sprite_get && api.hide && api.active_count && api.rng_seed &&
           api.rng_u32 && api.randint && api.lru_init && api.lru_get && api.lru_put &&
           api.lru_clear && api.paste_origin && api.screen_rect;
}

/* ------------------------------------------------------------------ *
 * Staging -- test_ui.c's symlink farm
 * ------------------------------------------------------------------ */

static char g_stage[ND_PATH_MAX];
static bool g_stage_is_temp;
static char g_golden[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];
static char g_so[ND_PATH_MAX];

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
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
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndkokiu-XXXXXX", base) != ND_OK)
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
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(g_so, sizeof g_so, "%s/../apps/Koki/app.so", exe) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * 1. The private Mersenne Twister
 * ------------------------------------------------------------------ */

/* CPython's random.Random(seed), measured on this checkout and identical to
 * the table in spec-koki.md section 12. Koki's RNG must agree BIT FOR BIT
 * or a boss picks a different attack and every frame after it diverges. */
static void test_rng(void)
{
    static const struct {
        uint32_t seed;
        uint32_t bits[4];
        int32_t r13[8];
        int32_t r530[6];
        int32_t r240[5];
    } VEC[] = {
        {42u,
         {2746317213u, 478163327u, 107420369u, 3184935163u},
         {3, 1, 1, 3, 2, 1, 1, 1},
         {25, 8, 5, 28, 13, 12},
         {87, -183, -228, 139, -100}},
        {1234u,
         {4150886329u, 3342196574u, 1892932127u, 501869158u},
         {2, 1, 1, 1, 3, 1, 3, 3},
         {29, 19, 8, 5, 7, 30},
         {158, -15, -181, -237, -194}},
        {20240101u,
         {868755655u, 1962355856u, 4178678115u, 1712989311u},
         {1, 2, 2, 2, 3, 3, 3, 2},
         {11, 19, 17, 17, 24, 22},
         {-137, -7, -36, -43, 67}},
    };
    koki_engine eng;
    size_t v;
    size_t i;

    memset(&eng, 0, sizeof eng);
    for (v = 0u; v < ND_ARRAY_LEN(VEC); v++) {
        api.rng_seed(&eng.rng, VEC[v].seed);
        for (i = 0u; i < 4u; i++)
            CHECK_INT(api.rng_u32(&eng.rng), VEC[v].bits[i], "getrandbits(32)");

        api.rng_seed(&eng.rng, VEC[v].seed);
        for (i = 0u; i < 8u; i++)
            CHECK_INT(api.randint(&eng, 1, 3), VEC[v].r13[i], "randint(1, 3)");

        api.rng_seed(&eng.rng, VEC[v].seed);
        for (i = 0u; i < 6u; i++)
            CHECK_INT(api.randint(&eng, 5, 30), VEC[v].r530[i], "randint(5, 30)");

        api.rng_seed(&eng.rng, VEC[v].seed);
        for (i = 0u; i < 5u; i++)
            CHECK_INT(api.randint(&eng, -240, 240), VEC[v].r240[i], "randint(-240, 240)");
    }

    /* randint normalises its arguments, so a reversed range draws the same
     * value from the same stream as the forward one. */
    api.rng_seed(&eng.rng, 42u);
    CHECK_INT(api.randint(&eng, 3, 1), 3, "randint(3, 1) == randint(1, 3)");
    /* A zero-width range consumes nothing and returns the endpoint. */
    CHECK_INT(api.randint(&eng, 7, 7), 7, "randint(n, n) is n");
}

/* ------------------------------------------------------------------ *
 * 2. LRUImages
 * ------------------------------------------------------------------ */

static nd_image *tile(int32_t w, int32_t h)
{
    return nd_image_new_filled(w, h, ND_PIXFMT_RGBA8888, ND_RGBA(1, 2, 3, 4));
}

static void test_cache(void)
{
    koki_lru c;
    nd_image *a;
    nd_image *b;

    /* cost is w * h * channels: a 10x10 RGBA tile is 400 bytes. */
    api.lru_init(&c, 1000u);
    a = tile(10, 10);
    api.lru_put(&c, "a", a);
    CHECK_INT(c.bytes, 400, "cost is w * h * channels");
    CHECK_INT(c.count, 1, "one entry");
    CHECK(api.lru_get(&c, "a") == a, "get returns what was put");
    CHECK(api.lru_get(&c, "missing") == NULL, "a miss is NULL");

    /* put() on a key that is already present KEEPS THE OLD IMAGE and
     * consumes the new one -- engine.py's `if key in self.map: move_to_end;
     * return`, with C's ownership rule bolted on. */
    b = tile(10, 10);
    api.lru_put(&c, "a", b);
    CHECK(api.lru_get(&c, "a") == a, "put on an existing key keeps the old image");
    CHECK_INT(c.count, 1, "and does not add an entry");
    CHECK_INT(c.bytes, 400, "and does not double-count");

    /* Eviction is least-recently-USED, not inserted: touching "a" saves it. */
    api.lru_put(&c, "b", tile(10, 10));
    CHECK_INT(c.count, 2, "two entries fit in 1000 bytes");
    (void)api.lru_get(&c, "a");
    api.lru_put(&c, "c", tile(10, 10));
    CHECK_INT(c.count, 2, "1200 bytes does not fit, so one went");
    CHECK(api.lru_get(&c, "a") != NULL, "the recently used entry survived");
    CHECK(api.lru_get(&c, "b") == NULL, "the least recently used entry was evicted");
    api.lru_clear(&c);
    CHECK_INT(c.bytes, 0, "clear zeroes the byte counter, not just the map");
    CHECK_INT(c.count, 0, "clear empties the map");

    /* An image larger than the whole budget is kept anyway: the eviction
     * loop stops at one entry, so a huge costume is not re-decoded every
     * single frame. */
    api.lru_init(&c, 100u);
    api.lru_put(&c, "big", tile(20, 20)); /* 1600 bytes */
    CHECK_INT(c.count, 1, "an over-budget entry is kept when it is the only one");
    CHECK(api.lru_get(&c, "big") != NULL, "and is still readable");
    api.lru_clear(&c);
}

/* ------------------------------------------------------------------ *
 * 3. The scheduler
 * ------------------------------------------------------------------ */

/* Probes: three scripts that record the frame numbers they ran on. */
static int32_t g_frame;
static int32_t g_log[64];
static size_t g_log_n;

static void logit(int32_t id)
{
    if (g_log_n < ND_ARRAY_LEN(g_log))
        g_log[g_log_n++] = id * 1000 + g_frame;
}

static koki_step probe_a(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        logit(1);
        KOKI_YIELD(F);
    }
    KOKI_END(F);
}

static koki_step probe_b(koki_frame *F)
{
    KOKI_BEGIN(F);
    logit(2);
    KOKI_YIELD(F);
    logit(2);
    KOKI_END(F);
}

/* Another forever script, on the other sprite, so stop_other_scripts has
 * something to spare. */
static koki_step probe_c(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        logit(4);
        KOKI_YIELD(F);
    }
    KOKI_END(F);
}

/* A script that broadcasts its OWN message and then returns. Python answers
 * that by putting a brand-new Script at the key and letting the old
 * generator's StopIteration land on the object it came from; the C has one
 * slot per handler and has to reproduce it deliberately. game.py's
 * jumpatkriby depends on this and says so. */
static koki_engine *g_probe_eng;

static koki_step probe_self(koki_frame *F)
{
    KOKI_BEGIN(F);
    logit(5);
    KOKI_YIELD(F);
    logit(5);
    api.broadcast(g_probe_eng, "self");
    KOKI_END(F);
}

static koki_step probe_instant(koki_frame *F)
{
    KOKI_BEGIN(F);
    logit(3);
    KOKI_END(F);
}

static void test_scheduler(koki_engine *eng)
{
    koki_sprite *s1;
    koki_sprite *s2;

    s1 = api.sprite_get(eng, "Player");
    s2 = api.sprite_get(eng, "Riby");
    CHECK(s1 != NULL && s2 != NULL, "two sprites for the scheduler probes");

    /* probe_b is registered FIRST on purpose: it is the one that finishes,
     * and watching it come back at the END of the run order is the whole
     * point of test 3 below. */
    CHECK_INT(api.on(eng, "ping", s2, probe_b), 1, "the first handler key is 1");
    CHECK_INT(api.on(eng, "ping", s1, probe_a), 2, "keys increase in registration order");
    CHECK_INT(api.on(eng, "pong", NULL, probe_instant), 3, "a Stage handler has a NULL sprite");
    CHECK_INT(api.on(eng, "pung", s2, probe_c), 4, "and the fourth key is 4");

    /* 1. A broadcast starts a script but does NOT step it: the per-frame
     * pass works off a snapshot taken before anything ran, so the first
     * statement lands one frame later. Every broadcast in the game pays
     * this, including the "instant" handlers. */
    g_log_n = 0u;
    g_frame = 0;
    api.broadcast(eng, "ping");
    CHECK_INT(api.active_count(eng), 2, "two scripts are live");
    CHECK_INT(g_log_n, 0, "a broadcast runs nothing by itself");

    /* 2. Registration order decides the initial run order. */
    g_frame = 1;
    api.step_frame(eng);
    CHECK_INT(g_log_n, 2, "both ran on the next frame");
    CHECK_INT(g_log[0], 2001, "probe_b was registered first, so it runs first");
    CHECK_INT(g_log[1], 1001, "probe_a second");

    /* probe_b finishes on its second step; probe_a loops forever. */
    g_log_n = 0u;
    g_frame = 2;
    api.step_frame(eng);
    CHECK_INT(api.active_count(eng), 1, "a finished script is swept out of active");

    /* 3. A key that died and comes back is APPENDED, so the run order
     * flips. This is Python dict-insert ordering and the game depends on
     * it. */
    g_log_n = 0u;
    api.broadcast(eng, "ping");
    g_frame = 3;
    api.step_frame(eng);
    CHECK_INT(g_log_n, 2, "both ran again");
    CHECK_INT(g_log[0], 1003, "probe_a kept its position");
    CHECK_INT(g_log[1], 2003, "probe_b was re-appended AFTER it");

    /* 4. Restarting a script that is still alive keeps its position. */
    g_log_n = 0u;
    api.broadcast(eng, "ping");
    g_frame = 4;
    api.step_frame(eng);
    CHECK_INT(g_log[0], 1004, "a live script restarted in place keeps its slot");
    CHECK_INT(g_log[1], 2004, "and so does the one after it");

    /* 5. An instant handler -- no yield before its return -- still costs a
     * frame of latency, and is gone by the end of the frame it ran in. */
    g_log_n = 0u;
    api.broadcast(eng, "pong");
    CHECK_INT(g_log_n, 0, "an instant handler does not run at broadcast time");
    g_frame = 5;
    api.step_frame(eng);
    CHECK_INT(g_log_n, 3, "it ran on the following frame, alongside the others");
    CHECK_INT(g_log[2], 3005, "and last, because it was started last");
    /* probe_b reached its end on this same frame, so BOTH finishers left:
     * only probe_a, which loops forever, is still live. */
    CHECK_INT(api.active_count(eng), 1, "every script that ended left active in that frame");

    /* 6. stop_other_scripts kills by SPRITE, whatever message started the
     * script. Called from outside a step nothing is "current", so every
     * script on that sprite dies -- and no other sprite's does. */
    api.broadcast(eng, "pung");
    g_frame = 6;
    api.step_frame(eng);
    CHECK_INT(api.active_count(eng), 2, "one forever script on each of the two sprites");
    api.stop_other(eng, s1);
    g_frame = 7;
    api.step_frame(eng);
    CHECK_INT(api.active_count(eng), 1, "the Player's script died and Riby's did not");

    api.stop_all(eng);
    g_frame = 8;
    api.step_frame(eng);
    CHECK_INT(api.active_count(eng), 0, "stop_all_scripts empties active");

    /* 7. An unheard message is not an error. */
    api.broadcast(eng, "nobody is listening to this");
    CHECK_INT(api.active_count(eng), 0, "broadcasting an unknown event does nothing");

    /* 8. A script that RESTARTS ITSELF and then ends. The instance that
     * returned StopIteration is not the instance that is now in the slot, so
     * the slot must survive and must begin again from the top. */
    g_probe_eng = eng;
    CHECK_INT(api.on(eng, "self", s1, probe_self), 5, "the self-restarting handler");
    g_log_n = 0u;
    api.broadcast(eng, "self");
    g_frame = 9;
    api.step_frame(eng);
    CHECK_INT(g_log_n, 1, "first step: one log");
    g_frame = 10;
    api.step_frame(eng); /* second step: logs, broadcasts itself, returns */
    CHECK_INT(g_log_n, 2, "second step: the other log");
    CHECK_INT(api.active_count(eng), 1, "the self-restart survived StopIteration");
    g_frame = 11;
    api.step_frame(eng);
    CHECK_INT(g_log_n, 3, "and the new instance ran");
    CHECK_INT(g_log[2], 5011, "from the TOP -- its first statement, not its second");
    api.stop_all(eng);
    g_frame = 12;
    api.step_frame(eng);
}

/* ------------------------------------------------------------------ *
 * 4. The manifest, the cast and the layer order
 * ------------------------------------------------------------------ */

static void test_registration(koki_engine *eng)
{
    size_t i;
    bool found_stuff = false;

    /* Measured by instrumenting register_all() in the Python: 304 handlers
     * over 107 distinct messages. A grep of "@eng.on" gives 243 and is
     * wrong -- six loops install the rest. */
    CHECK_INT(eng->n_slots, 304, "304 handlers registered");
    CHECK_INT(eng->n_events, 107, "107 distinct messages");
    CHECK_INT(eng->n_sprites, 45, "45 sprites created");
    CHECK_INT(eng->n_layers, 45, "45 sprites in the draw list");

    /* The manifest has 47 targets: the Stage, the 45 sprites the game
     * creates, and `Stuff`, which no script ever references and which must
     * NOT be instantiated -- creating it would add a sprite to the draw
     * list and change what is on screen. */
    for (i = 0u; i < eng->n_sprites; i++) {
        if (strcmp(eng->sprites[i]->name, "Stuff") == 0)
            found_stuff = true;
    }
    CHECK(!found_stuff, "the unreferenced target 'Stuff' is never created");

    /* set_layer_order gets 44 names for 45 sprites. Enemy4Stats is the one
     * that is missing, so it sorts to 999 and draws in FRONT of everything,
     * White included. Visible on the final-boss screen; reproduced on
     * purpose. */
    CHECK_STR(eng->layers[eng->n_layers - 1u]->name, "Enemy4Stats",
              "Enemy4Stats renders in front of everything, White included");
    CHECK_STR(eng->layers[eng->n_layers - 2u]->name, "White", "White is second from the front");
    CHECK_STR(eng->layers[0]->name, "Door4", "Door4 is at the back");

    /* Every sprite starts hidden whatever the manifest's editor pose says:
     * the original project's every sprite begins "when flag clicked: hide". */
    for (i = 0u; i < eng->n_sprites; i++) {
        if (eng->sprites[i]->visible) {
            CHECK(false, "every sprite starts hidden");
            break;
        }
    }
    g_checks++;

    /* game.py's "when flag clicked" defaults, including the two the damage
     * gates would otherwise create lazily. */
    CHECK_EXACT(eng->vars[KOKI_V_LIVES], 3.0, "lives starts at 3");
    CHECK_EXACT(eng->vars[KOKI_V_DOORS], 1.0, "doors starts at 1");
    CHECK_EXACT(eng->vars[KOKI_V_CANNONDEFEATS], 1.0, "cannondefeats starts at 1");
    CHECK_EXACT(eng->vars[KOKI_V_EVILCANONBALLDIRECTION], -90.0,
                "evilcanonballdirection starts at -90");
    CHECK_EXACT(eng->vars[KOKI_V_HURT_T], -99.0, "_hurt_t defaults to -99");
    CHECK_EXACT(eng->vars[KOKI_V_PLANE_HURT_T], -99.0, "_plane_hurt_t defaults to -99");
}

/* ------------------------------------------------------------------ *
 * 5. Half-to-even rounding at the paste origin
 * ------------------------------------------------------------------ */

/* spec-koki.md section 1's worked example. CharacterAnim/costume2 has
 * cy = 19.5, so the paste row is 87.5 - y*0.5 - 19.5 = 68 - y*0.5; at
 * y = -75 that is 105.5, which Python's round() sends to 106, and at y = -73
 * it is 104.5, which goes to 104. C's round() would give 106 and 105, and
 * (int)(v + 0.5) would give 106 and 105 as well -- both wrong by a pixel on
 * one of them. */
static void test_rounding(koki_engine *eng)
{
    koki_sprite *anim = api.sprite_get(eng, "CharacterAnim");
    double o[2];

    CHECK(anim != NULL, "CharacterAnim exists");
    if (anim == NULL)
        return;

    /* costume2 is the sprite's resting pose; the manifest gives it cy 19.5. */
    anim->costume_i = 0u;
    while (anim->costume_i < anim->n_costumes &&
           strcmp(anim->costumes[anim->costume_i].name, "costume2") != 0)
        anim->costume_i++;
    CHECK(anim->costume_i < anim->n_costumes, "costume2 is in the list");
    if (anim->costume_i >= anim->n_costumes)
        return;
    CHECK_EXACT(anim->costumes[anim->costume_i].cy, 19.5, "costume2 has cy 19.5");

    anim->x = 0.0;
    anim->y = -75.0;
    api.paste_origin(anim, o);
    CHECK_EXACT(o[1], 105.5, "paste origin y is exactly 105.5 at y = -75");
    CHECK_INT((int32_t)nd_round_half_even(o[1]), 106, "105.5 rounds to 106 (even)");

    anim->y = -73.0;
    api.paste_origin(anim, o);
    CHECK_EXACT(o[1], 104.5, "paste origin y is exactly 104.5 at y = -73");
    CHECK_INT((int32_t)nd_round_half_even(o[1]), 104, "104.5 rounds to 104 (even), not 105");
}

/* ------------------------------------------------------------------ *
 * 6. Six scenarios, frame by frame, against the Python
 * ------------------------------------------------------------------ */

typedef struct {
    int frame;
    const char *msgs[3];
    koki_varid var; /* KOKI_V_COUNT when there is no variable to set */
    double val;
} sc_action;

typedef struct {
    koki_key_id k;
    int a, b;
} sc_hold;

typedef struct {
    const char *name;
    int frames;
    sc_action actions[4];
    sc_hold holds[4];
} scenario;

/* tools/smoke.py's S table, verbatim. */
static const scenario SCENARIOS[] = {
    {"lv1",
     1000,
     {{2, {"level1", NULL}, KOKI_V_COUNT, 0},
      {500, {"enemy1damage", NULL}, KOKI_V_COUNT, 0},
      {800, {"enemy1damage", NULL}, KOKI_V_COUNT, 0},
      {-1, {NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_Z, 150, 152},
      {KOKI_KEY_Z, 180, 182},
      {KOKI_KEY_Z, 210, 212},
      {KOKI_KEY_COUNT, 0, 0}}},
    {"lv2",
     1200,
     {{2, {"startlv2", "planecutscene", NULL}, KOKI_V_COUNT, 0},
      {700, {"enemy2 damage", NULL}, KOKI_V_COUNT, 0},
      {1000, {"enemy2 damage", NULL}, KOKI_V_COUNT, 0},
      {-1, {NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_UP, 200, 240},
      {KOKI_KEY_UP, 400, 430},
      {KOKI_KEY_DOWN, 300, 340},
      {KOKI_KEY_COUNT, 0, 0}}},
    {"lv3",
     1000,
     {{2, {"level3", NULL}, KOKI_V_COUNT, 0},
      {500, {"enemy 3 damage", NULL}, KOKI_V_COUNT, 0},
      {-1, {NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_Z, 200, 205},
      {KOKI_KEY_Z, 300, 305},
      {KOKI_KEY_Z, 600, 605},
      {KOKI_KEY_COUNT, 0, 0}}},
    {"final",
     1400,
     {{2, {"go to lobby", NULL}, KOKI_V_COUNT, 0},
      {40, {NULL}, KOKI_V_DOORS, 4},
      {60, {"final cutscene", NULL}, KOKI_V_COUNT, 0},
      {-1, {NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_COUNT, 0, 0}}},
    {"gameover",
     400,
     {{2, {NULL}, KOKI_V_LIVES, 0},
      {3, {"game over", NULL}, KOKI_V_COUNT, 0},
      {-1, {NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_ENTER, 200, 205}, {KOKI_KEY_COUNT, 0, 0}}},
    {"ending",
     1400,
     {{2, {"ending cutscene", NULL}, KOKI_V_COUNT, 0}, {-1, {NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_COUNT, 0, 0}}},
    /* NOT one of smoke.py's: added here because it is the regression test for
     * the self-restarting broadcast (see test_scheduler's probe_self). Riby's
     * jump-attack script ends by broadcasting the message it is itself
     * registered against, and 2,200 frames is long enough for that to happen
     * five times. Nothing else is broadcast into "jumpatkriby" in this
     * scenario, so every restart after the first is the script's own. Get the
     * scheduler wrong and the fight goes inert at frame 438. */
    {"jumpatk",
     2200,
     {{2, {"go to lobby", NULL}, KOKI_V_COUNT, 0},
      {40, {"PlayerEnable", NULL}, KOKI_V_COUNT, 0},
      {50, {"playerfinalenable", NULL}, KOKI_V_COUNT, 0},
      {60, {"jumpatkriby", NULL}, KOKI_V_COUNT, 0}},
     {{KOKI_KEY_COUNT, 0, 0}}},
};

/* Every fiftieth frame of every scenario, from the Python. */
static const struct {
    const char *scenario;
    int frame;
    const char *sha256;
} CHECKPOINTS[] = {
    {"lv1", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"lv1", 50, "623852ab6ff4d098a47fd71860ac6535a9cf7f462550f397505c0c4905e25eec"},
    {"lv1", 100, "cdf2e12e061a52e4b3c209dc3fb8d46688d03cc10594369068177ccb81f56dbe"},
    {"lv1", 150, "ebb505055ace84c5ba6ec7c66d56d78fa0e6db997212dfb255d4c4d8792107b7"},
    {"lv1", 200, "ba412087badddf0dad035fbc4e23469567ca4e7abf904b1d7ac499e2b5183d89"},
    {"lv1", 250, "efa90ca4a2f3998cde01c7ad1b4e1583028c3e941cfeb4ea56b39e4dcad76b86"},
    {"lv1", 300, "6d69c5c7771a30a058725365f586a8b152e151d019e4c6f9da800dd84c78099f"},
    {"lv1", 350, "0c8a77796e3655eb1a09ab72d385f6d353dd7aa31bba3ce12d3aca3b63482150"},
    {"lv1", 400, "6d69c5c7771a30a058725365f586a8b152e151d019e4c6f9da800dd84c78099f"},
    {"lv1", 450, "0c8a77796e3655eb1a09ab72d385f6d353dd7aa31bba3ce12d3aca3b63482150"},
    {"lv1", 500, "6d69c5c7771a30a058725365f586a8b152e151d019e4c6f9da800dd84c78099f"},
    {"lv1", 550, "471f9dd7751a0867b811ad686ce9f6a86e23d8e7a44358f52593fc1d14be3712"},
    {"lv1", 600, "477bf6f9280ea7c0d6ec43c601b17f04115d32c850c632df5920697d80990198"},
    {"lv1", 650, "517ed4e74d802471a383313398cd4fcbd82d1afb2c675f7198e9aed2d41de1f3"},
    {"lv1", 700, "03caed8e8fc6b9553bfb7e4456245649b5df4eaa8b8bb32caf6c42c01300388b"},
    {"lv1", 750, "7b4e6ca6fdd492a645acde312d2e490f94b1a60994350e29a12f0a091b7acbdc"},
    {"lv1", 800, "90a8d9b5010043bcac3229603af822f863efbe49ac4dc03ff4d762fe870f0ec5"},
    {"lv1", 850, "7b517bb20aa000346e872b99530874dac4e2b4919975d268f3d80e0e4628cdb6"},
    {"lv1", 900, "b54d8255c97252a8fa3c115e1562325c95540d05a6de9e1d29453889927a3a45"},
    {"lv1", 950, "4097a5c33f1da968cea87b7b78616cde2f198fcb4dc634f160f504bac40918e7"},
    {"lv1", 999, "74c9254aac9863a5215f7730f3b8927ec1085a4ad542a22f486627175be146e3"},
    {"lv2", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"lv2", 50, "553788d5efc06825198e2141525fb7f6ca9a95f59703754f988ad17226afb3f9"},
    {"lv2", 100, "22ec2bbff3b643bf49d7dc7435ac8235022df09aa83ff6bfe5e82400353696c0"},
    {"lv2", 150, "9aa7cdafe6a2e31c4c396ec1cbda9b7a5362c3ca00d056c3eeb0955789f03d86"},
    {"lv2", 200, "6e67b5eccdd65d6837996ca528e265dbff166d34343536dd38e50cc796504895"},
    {"lv2", 250, "3de60833075f15fe59aa01bc47a114b7e2b32827fc49e7f8fd72f4ef1dbfa409"},
    {"lv2", 300, "7b8a7da93bb46996dacefd61b8d2435a2b6c6f10bd56f4e0f4daf1d77ba92892"},
    {"lv2", 350, "97045b921be78d728945e7a27cec6ee1de4e27248786b92907f135ffe1c03acb"},
    {"lv2", 400, "5f98d024b602f5d4eaea5c2325dbda6001f996fb237cc7b526b66472a46abcf9"},
    {"lv2", 450, "b5cd97c27ed77a0a0fc01785792d444f7ca853d3146974ee31064d5e18cf1fda"},
    {"lv2", 500, "87e42d3948efbb1c7efaac13266d15febedca4224a9ea06879906491dfbc28f6"},
    {"lv2", 550, "a4df7b7163b3411c561db43f630a5614f556d42ec41a673b1443a79bbc299e69"},
    {"lv2", 600, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 650, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 700, "e76d3866404803c7b695f6037dafc1ce48428b2cdb3940dac55f1e8642753766"},
    {"lv2", 750, "e76d3866404803c7b695f6037dafc1ce48428b2cdb3940dac55f1e8642753766"},
    {"lv2", 800, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 850, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 900, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 950, "e76d3866404803c7b695f6037dafc1ce48428b2cdb3940dac55f1e8642753766"},
    {"lv2", 1000, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 1050, "e76d3866404803c7b695f6037dafc1ce48428b2cdb3940dac55f1e8642753766"},
    {"lv2", 1100, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv2", 1150, "e76d3866404803c7b695f6037dafc1ce48428b2cdb3940dac55f1e8642753766"},
    {"lv2", 1199, "c2b1b70c0550ffac46516c4c26af046bf286cba5fcef5d65778672f6c8cf02dd"},
    {"lv3", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"lv3", 50, "7b61c44add1f6387ad47508b00e38f86860ff5c7fab5c98267122fcb522f086f"},
    {"lv3", 100, "80e25c0d2566315c7dddc2c985ca82a2c52e542ee91b6e340f82041b0d84547a"},
    {"lv3", 150, "e0f8bcb3bf68caec8461c79cac4d760ecc6fc813c8f8a44bb847694270a1205b"},
    {"lv3", 200, "42dd08219b17f4880f4078e8d6686128c7d85b241a55d3cc348802347b210dd2"},
    {"lv3", 250, "7d17d8093769d8d9d6ae8836350dfc3909ce43b42a9d13ea004ef4946cff562f"},
    {"lv3", 300, "23db4e12d8c626f09c5b46902e09f6bd2f05e588af31f4f1c66ca5b9964112b4"},
    {"lv3", 350, "a7d1fc866843fb93744bd1081b34a6d516a25e5bdb9a01bc0c33e7d3c38e402c"},
    {"lv3", 400, "e9c3f14d9d2b587b7c98f84624bcd7857416f576b0a2996b01e79759334077a2"},
    {"lv3", 450, "7ddd05b9ce6be144edb660ca21e409248afe38e953f3526cb4535ac7bcf592e2"},
    {"lv3", 500, "4260bc07708abe4dc90794c004ada157a10fa03c2c31e2cfffc431785684b913"},
    {"lv3", 550, "4260bc07708abe4dc90794c004ada157a10fa03c2c31e2cfffc431785684b913"},
    {"lv3", 600, "e68d61eec398c288e46423f19cbf6fb5a72736339fbe6b64f5e2f908b5cb790e"},
    {"lv3", 650, "e68d61eec398c288e46423f19cbf6fb5a72736339fbe6b64f5e2f908b5cb790e"},
    {"lv3", 700, "a1f2e6f4b636eacd8715af964ed533771c701ce4154861f1acc718cd4667aaa7"},
    {"lv3", 750, "1cfe233c2bced115ff6415fe63be5d2afff63a6e83e3032f525cb6a6ebf43820"},
    {"lv3", 800, "bd14bc5c5a59a4e990cb4eb5742867f8ebdae07fa614556aa7a439635cb02bb4"},
    {"lv3", 850, "e68d61eec398c288e46423f19cbf6fb5a72736339fbe6b64f5e2f908b5cb790e"},
    {"lv3", 900, "e68d61eec398c288e46423f19cbf6fb5a72736339fbe6b64f5e2f908b5cb790e"},
    {"lv3", 950, "4260bc07708abe4dc90794c004ada157a10fa03c2c31e2cfffc431785684b913"},
    {"lv3", 999, "e68d61eec398c288e46423f19cbf6fb5a72736339fbe6b64f5e2f908b5cb790e"},
    {"final", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"final", 50, "8a6515d435cf585eb0e0dc2feceb6a2c7d522647365ed7b2f8c73f09aa3eab90"},
    {"final", 100, "3fa5fba724f2945a4848b556874203ee2bef5dcd1fd78f25b07c4d24cb9c9541"},
    {"final", 150, "69062c2167c555b22b7a9d7f83e1915dbb3f963adf9c0760e65ff6cb003395a7"},
    {"final", 200, "22fceb2547689bf5ae89dcca0b64e872bfab388c0717ec62ff82bcea1d74b81b"},
    {"final", 250, "fee3e58ebbd05f047117cfa2682eff8ec5662202b51e840e4a2aa214d8235665"},
    {"final", 300, "c676ec7d25996158f940b768cd1b6dd86f6c9ee33f303decf34cc77683400fce"},
    {"final", 350, "9b340cc729bcff77dc172f7ddd3f58d486a76fd5bafa906ef033d6c6a7ec512d"},
    {"final", 400, "622d3cf12a02cd804fec2779ace60de51c204dd4ae71a3fdea3a276611004fda"},
    {"final", 450, "c650bf9d36ac4357aef442b95a3dc93ea9f433ca0e4afb454c60efc3e0d23ba3"},
    {"final", 500, "7db0c73b88c344a00a17774136eb2039c920597a74c2ac6e6ce35ce9f41479a3"},
    {"final", 550, "c9d4765a6bc9def359fb14f17764f8f65da60fb4ef1bcdfa830d29a68d0b29cb"},
    {"final", 600, "54d0811793885f11a0d7f57f6c56bc9410f48b6f4a57a561889c69cf121cafa8"},
    {"final", 650, "f8b5741d9a239b5361229de8b02f672b133ae919b3fdfbf718c4ac7c70b5000e"},
    {"final", 700, "05dcbe79b1e0d840fcdbcf05d097cc78c53e22220332f38443e991ad54b32ee1"},
    {"final", 750, "ad3d4badb83ba59ee201668466ad7c4e6aef9ceb0c062e6547371d51fda1ff82"},
    {"final", 800, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"final", 850, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"final", 900, "bfe50c9b4883a48f9c838de2732c4f22d76bb47d2aeb9c8b2671f7d875837748"},
    {"final", 950, "bfe50c9b4883a48f9c838de2732c4f22d76bb47d2aeb9c8b2671f7d875837748"},
    {"final", 1000, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"final", 1050, "bfe50c9b4883a48f9c838de2732c4f22d76bb47d2aeb9c8b2671f7d875837748"},
    {"final", 1100, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"final", 1150, "bfe50c9b4883a48f9c838de2732c4f22d76bb47d2aeb9c8b2671f7d875837748"},
    {"final", 1200, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"final", 1250, "bfe50c9b4883a48f9c838de2732c4f22d76bb47d2aeb9c8b2671f7d875837748"},
    {"final", 1300, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"final", 1350, "bfe50c9b4883a48f9c838de2732c4f22d76bb47d2aeb9c8b2671f7d875837748"},
    {"final", 1399, "c0e45d0087cbe43161f7b5e9f16988163864803a88a44aab97678dac91779cac"},
    {"gameover", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"gameover", 50, "f1c58b2e9f8fb9bbec5c90ab1fa31ede6f0ac8847aa8b9d9b4e2b62cf1f59a95"},
    {"gameover", 100, "f1c58b2e9f8fb9bbec5c90ab1fa31ede6f0ac8847aa8b9d9b4e2b62cf1f59a95"},
    {"gameover", 150, "f1c58b2e9f8fb9bbec5c90ab1fa31ede6f0ac8847aa8b9d9b4e2b62cf1f59a95"},
    {"gameover", 200, "8dd18cad1596c4cfc518b291c8c4af4175460a704ef16e8d62c519f4d71fbfbd"},
    {"gameover", 250, "51efc6f0c725d87d6b28baac57a8c76573cf04b25754fb881dac5cf59fa86b4f"},
    {"gameover", 300, "8a6515d435cf585eb0e0dc2feceb6a2c7d522647365ed7b2f8c73f09aa3eab90"},
    {"gameover", 350, "c690f5fc58016adb0ed30a427292e2359d6a2f739e30f5058935f24fa4198c02"},
    {"gameover", 399, "8a6515d435cf585eb0e0dc2feceb6a2c7d522647365ed7b2f8c73f09aa3eab90"},
    {"ending", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"ending", 50, "6eaab211da9294c0f809d33caa6f8b6845979dadc86f665a62168deb258750ce"},
    {"ending", 100, "88a0695cd29c8f9161ce27c23129667bfdb2ef86fb2e451d77bdbf78fe6de603"},
    {"ending", 150, "86f734122dc22fe98559b451d67d623179bcab7e4e2c51cb1cccca19f8bf452e"},
    {"ending", 200, "d5020871002dec26be34363df3c46c9918e5fc49c2388c2ca642eaf06274824d"},
    {"ending", 250, "46f482dd10c28cc1e6f9db37c73484a6f5d23b40c7639622b1532d9c59868a20"},
    {"ending", 300, "39c57afcf30942a668c5475f61aa08bc0abfa2bbe27eade7d9b7d6871a9e4b34"},
    {"ending", 350, "eacb0915ab0fddd104bcbeba469349b97a9db492b686d2704399c37ff326a430"},
    {"ending", 400, "1c4c810a70a6334490d706d50675a82107c2882e600d431e4a69b5bcd655f2b0"},
    {"ending", 450, "8a85542e1184489c18a3a9924b5a6f29c6b4286764013fc9df1a6e563bb53aea"},
    {"ending", 500, "dc8eaedd7a7a4eeec2c64f13a82912e149bce6e6c9fb81d6dc87c4321ccfc906"},
    {"ending", 550, "b1a72422395e3f1c09590cf87fb05f0936bbfbf35f3bff1840b1e3e9d4456f5f"},
    {"ending", 600, "e6dbc7221d59da92c71014b2a93ecf645e11fb2c5502bc231be872fbb6712961"},
    {"ending", 650, "8174ddee6048a08ab689b9aadf29f33f589776fdf660f87ed88961a824cd5c68"},
    {"ending", 700, "8174ddee6048a08ab689b9aadf29f33f589776fdf660f87ed88961a824cd5c68"},
    {"ending", 750, "8174ddee6048a08ab689b9aadf29f33f589776fdf660f87ed88961a824cd5c68"},
    {"ending", 800, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 850, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 900, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 950, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1000, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1050, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1100, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1150, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1200, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1250, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1300, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1350, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"ending", 1399, "2941e94f1c69857f7fd537aeb48bc6ec9620b37b8c621c4e943640d296ccf433"},
    {"jumpatk", 0, "08dc1b945254ab7d09ab496ce6c1ec25d42a9ed28bb84da59504eafa1d5a976b"},
    {"jumpatk", 50, "5c78c30e2affeedaaa21a0a5ac2214d4e02fd08efb7b52e25daca8a9a4a16391"},
    {"jumpatk", 100, "c690f5fc58016adb0ed30a427292e2359d6a2f739e30f5058935f24fa4198c02"},
    {"jumpatk", 150, "98f10b82a22434b2e600fe7bdc2242f8a29e62ca1dbbaed4c072065077248843"},
    {"jumpatk", 200, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 250, "5e6803c54db9cd0f6e73635ee937b43fec7eacb5bd3a8fa4e71d2e2579d39274"},
    {"jumpatk", 300, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 350, "5e3a7a43830940299dbdf97ab855d20c522e1b64e067984a1bc18aeb8b25f12d"},
    {"jumpatk", 400, "c690f5fc58016adb0ed30a427292e2359d6a2f739e30f5058935f24fa4198c02"},
    {"jumpatk", 450, "d4a07827165e4b65ded43fe3dbf047954fb7f77420064ff5f9a635f8b30eb9fb"},
    {"jumpatk", 500, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 550, "a1334a88bf210a665296b7d1d453b85b599d39b0e4300141fc103e053c504298"},
    {"jumpatk", 600, "efd00f96bdbb8e5f8289223f0f8af624f7d4b16adaaf4fbfb2999941550bfc11"},
    {"jumpatk", 650, "c529f4ddee35aac9f2aed6370e40f5fe34f1aef05f4a3538ee408a6a1ebc3703"},
    {"jumpatk", 700, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 750, "43f548209ad1b7da7b3621c249ef5f404f00111c62c4178d4b7f8205e12a38c2"},
    {"jumpatk", 800, "efd00f96bdbb8e5f8289223f0f8af624f7d4b16adaaf4fbfb2999941550bfc11"},
    {"jumpatk", 850, "efd00f96bdbb8e5f8289223f0f8af624f7d4b16adaaf4fbfb2999941550bfc11"},
    {"jumpatk", 900, "b24b4f29e977831cee6bde3e29628dc81ed0a2839318195c840a789a400dc684"},
    {"jumpatk", 950, "c690f5fc58016adb0ed30a427292e2359d6a2f739e30f5058935f24fa4198c02"},
    {"jumpatk", 1000, "2a392f8bf9735311da045082d3a8398b4a81fda8be76edc16f15455a50a2b12f"},
    {"jumpatk", 1050, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 1100, "c20459bbe42adf44010d19b786e2664f72921fab508469188a73f03955333900"},
    {"jumpatk", 1150, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 1200, "efd00f96bdbb8e5f8289223f0f8af624f7d4b16adaaf4fbfb2999941550bfc11"},
    {"jumpatk", 1250, "76448d19079a28602bad9cdb4f6411d8a68da7c0ad95019bdbfe505c54f35eeb"},
    {"jumpatk", 1300, "8a6515d435cf585eb0e0dc2feceb6a2c7d522647365ed7b2f8c73f09aa3eab90"},
    {"jumpatk", 1350, "611c1e518f98014469f24f8b8e4250f2b61ef8165864878904944021e5f94d02"},
    {"jumpatk", 1400, "efd00f96bdbb8e5f8289223f0f8af624f7d4b16adaaf4fbfb2999941550bfc11"},
    {"jumpatk", 1450, "d38ab4303e6c2e58b0ae757b5aa2af7c6e796582b1761bcf3f7b652ab7d35496"},
    {"jumpatk", 1500, "efd00f96bdbb8e5f8289223f0f8af624f7d4b16adaaf4fbfb2999941550bfc11"},
    {"jumpatk", 1550, "8e5e622161cabf0e852d4a1ed35e97f8ed5da2f3cfbb110ee7b110a819f086d0"},
    {"jumpatk", 1600, "8a6515d435cf585eb0e0dc2feceb6a2c7d522647365ed7b2f8c73f09aa3eab90"},
    {"jumpatk", 1650, "b4b105f6d386eb04ab26077255981620b09705e0b969a55ec7bc784686f348df"},
    {"jumpatk", 1700, "b9f2cd7b2c4c38d832b8002c210838a7076482c87f6bb926262166ed3329731c"},
    {"jumpatk", 1750, "0080ae8bd074c964e0b3ba04f2ed2a2246242bc30dd67d1a7937615e4cd6117f"},
    {"jumpatk", 1800, "f8cd32dde7372e3b81becaa12873847b66ff685407bc6124b9c486bf36f67a6e"},
    {"jumpatk", 1850, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 1900, "98f10b82a22434b2e600fe7bdc2242f8a29e62ca1dbbaed4c072065077248843"},
    {"jumpatk", 1950, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 2000, "5e6803c54db9cd0f6e73635ee937b43fec7eacb5bd3a8fa4e71d2e2579d39274"},
    {"jumpatk", 2050, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 2100, "50d932da8b2b35f3cd7cbfc6810e0f310fd8cd5eb78c2a02798e6069cc95f3e0"},
    {"jumpatk", 2150, "546ad18cd7d6f01f657055b0bfa7d39cab5b3f113e0720b4447602adfafab058"},
    {"jumpatk", 2199, "efbd3f685aa6672775fedb1233bcaba9daf177857d4a4c7d5a8ba50037c06666"},
};

static void run_scenario(const scenario *sc)
{
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;
    koki_engine *eng;
    int f;
    int mismatches = 0;
    int compared = 0;

    memset(&ui, 0, sizeof ui);
    /* 240 * 175 * 3 = 126,000 bytes. No framebuffer: the digest is taken
     * from the canvas, which is what the Python hashes too. */
    canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (canvas == NULL || nd_draw_bind(&draw, canvas) != ND_OK) {
        nd_image_free(canvas);
        g_failures++;
        return;
    }
    ui.w = ND_UI_W;
    ui.h = ND_UI_H;
    ui.softkey_h = ND_SOFTKEY_H;
    ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    ui.canvas = canvas;
    ui.draw = &draw;
    ui.fb = NULL;
    ui.keypad_fd = -1;

    eng = api.engine_new(&ui, KOKI_APP_DIR);
    if (eng == NULL) {
        fprintf(stderr, "test_koki: engine_new failed for %s\n", sc->name);
        nd_image_free(canvas);
        g_failures++;
        return;
    }
    api.register_all(eng);
    api.rng_seed(&eng->rng, 42u);
    /* headless: a flat 1/30 s per frame, set before any script runs so every
     * script in a frame sees one timestamp. */
    eng->have_vtime = true;
    eng->vtime = 0.0;
    api.start_flag(eng);

    for (f = 0; f < sc->frames; f++) {
        size_t h;
        size_t a;
        char digest[80];

        for (h = 0u; h < (size_t)KOKI_KEY_COUNT; h++) {
            bool cur = false;
            size_t j;

            for (j = 0u; j < ND_ARRAY_LEN(sc->holds); j++) {
                if (sc->holds[j].k == KOKI_KEY_COUNT)
                    break;
                if (sc->holds[j].k == (koki_key_id)h && f >= sc->holds[j].a && f <= sc->holds[j].b)
                    cur = true;
            }
            eng->input.pressed[h] = cur && !eng->input.held[h];
            eng->input.held[h] = cur;
        }

        api.step_frame(eng);

        /* smoke.py kills the boot sequence at frame 1 and jumps straight
         * into the scenario. */
        if (f == 1) {
            api.stop_all(eng);
            api.hide(api.sprite_get(eng, "Dynaris Logo"));
        }
        for (a = 0u; a < ND_ARRAY_LEN(sc->actions); a++) {
            size_t m;

            if (sc->actions[a].frame < 0)
                break;
            if (sc->actions[a].frame != f)
                continue;
            if (sc->actions[a].var != KOKI_V_COUNT)
                eng->vars[sc->actions[a].var] = sc->actions[a].val;
            for (m = 0u; m < ND_ARRAY_LEN(sc->actions[a].msgs) && sc->actions[a].msgs[m] != NULL;
                 m++)
                api.broadcast(eng, sc->actions[a].msgs[m]);
        }

        api.render(eng);

        if (nd_capture_digest(canvas, digest, sizeof digest) == ND_OK) {
            size_t k;

            for (k = 0u; k < ND_ARRAY_LEN(CHECKPOINTS); k++) {
                if (CHECKPOINTS[k].frame != f || strcmp(CHECKPOINTS[k].scenario, sc->name) != 0)
                    continue;
                compared++;
                if (strcmp(CHECKPOINTS[k].sha256, digest) != 0) {
                    mismatches++;
                    if (mismatches == 1)
                        fprintf(stderr,
                                "FAIL %s frame %d: got %s\n"
                                "                  want %s\n",
                                sc->name, f, digest, CHECKPOINTS[k].sha256);
                }
            }
        }
        eng->vtime += KOKI_FRAME_DT;
    }

    /* The peak concurrency the caches and the frame array were sized
     * against; spec-koki.md measured 32 over a full tour. */
    printf("  %-9s %4d frames, %2d checkpoints, %zu scripts live at the end\n", sc->name,
           sc->frames, compared, api.active_count(eng));

    CHECK(compared > 0, "the scenario had checkpoints to compare");
    CHECK_INT(mismatches, 0, "every checkpoint matches the Python frame for frame");
    api.engine_free(eng);
    nd_image_free(canvas);
}

static void test_scenarios(void)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(SCENARIOS); i++)
        run_scenario(&SCENARIOS[i]);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    koki_engine *eng;
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;

    /* No child audio players and no /dev/snd probing: sound reaches no pixel
     * and no timing, because play_until_done waits the manifest's declared
     * duration rather than the device's. */
    (void)setenv("NEODCT_KOKI_NOSOUND", "1", 1);
    /* The tuning knob has to be at its default or every boss glide moves. */
    (void)unsetenv("NEODCT_KOKI_ATTACK_SLOW");

    if (!find_reference_dirs()) {
        fprintf(stderr, "test_koki: NEODCT_GOLDEN is not set and the reference set was not "
                        "found; skipping\n");
        return 0;
    }
    if (!resolve_app_so() || !api_open(g_so)) {
        fprintf(stderr, "test_koki: cannot load apps/Koki/app.so\n");
        return 1;
    }
    if (!stage_root()) {
        fprintf(stderr, "test_koki: cannot stage a root\n");
        return 1;
    }
    if (nd_app_set_dir(KOKI_APP_DIR) != ND_OK) {
        unstage();
        return 1;
    }

    test_rng();
    test_cache();

    /* One engine for the structural tests. It gets a canvas because
     * koki_sprite_get() decodes costumes through the image cache. */
    memset(&ui, 0, sizeof ui);
    canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (canvas == NULL || nd_draw_bind(&draw, canvas) != ND_OK) {
        unstage();
        return 1;
    }
    ui.w = ND_UI_W;
    ui.h = ND_UI_H;
    ui.canvas = canvas;
    ui.draw = &draw;
    ui.keypad_fd = -1;

    eng = api.engine_new(&ui, KOKI_APP_DIR);
    if (eng == NULL) {
        fprintf(stderr, "test_koki: engine_new failed -- are the assets staged?\n");
        nd_image_free(canvas);
        unstage();
        return 1;
    }
    api.register_all(eng);
    test_registration(eng);
    test_rounding(eng);
    api.engine_free(eng);

    /* A SECOND engine, with no game registered, for the scheduler probes:
     * they need to be the only scripts in `active` for the run order to mean
     * anything. */
    eng = api.engine_new(&ui, KOKI_APP_DIR);
    if (eng != NULL) {
        test_scheduler(eng);
        api.engine_free(eng);
    } else {
        g_failures++;
    }
    nd_image_free(canvas);

    test_scenarios();

    unstage();
    (void)dlclose(api.handle);
    printf("test_koki: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
