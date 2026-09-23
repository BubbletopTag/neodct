/* koki_engine.c -- construction, the frame loop, the pause menu, teardown.
 *
 * engine.py's Engine.__init__, now(), run(), _pause_menu() and teardown().
 *
 * ============ THE FRAME ============
 *
 *     poll input
 *     if BACK was pressed -> the quit dialog, and poll again on resume
 *     step every live script once (a SNAPSHOT of them: koki_sched.c)
 *     sweep the dead
 *     render
 *     sleep out the rest of 1/30 s
 *
 * Note the order: input is read BEFORE the scripts run and the frame is
 * rendered AFTER they have, so a key pressed during frame N is visible to
 * frame N's scripts and on frame N's picture. Moving the render earlier
 * would put every sprite one frame behind its own logic.
 *
 * ============ TIME GOES THROUGH nd_time_monotonic() ============
 *
 * Not clock_gettime(). goldenframe.py replaces Python's time.monotonic with a
 * virtual clock that advances exactly one tick per COMMITTED frame, and
 * nd_vclock.h is the C substitution for it. Every wait, every glide and every
 * invincibility window in this game is measured against that clock, so a
 * captured frame is the same bytes on every machine -- which is the only
 * reason golden/app-koki.png can be an oracle for a real-time game.
 *
 * ============ WHY THE CACHES ARE TIERED BY /proc/meminfo ============
 *
 * The Luckfox has ~53 MB for the whole of userspace and QEMU tests run with
 * 64. A level's working set is well under a megabyte now that oversized
 * static art is cropped at bake time, so the small budgets do not thrash --
 * but a budget sized for the desktop would, on the device, evict a costume
 * that is about to be drawn again. The tiers and the two environment
 * overrides are engine.py's, unchanged.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"

#include "koki_manifest.h"

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */

double koki_now(const koki_engine *eng)
{
    if (eng != NULL && eng->have_vtime)
        return eng->vtime;
    return nd_time_monotonic();
}

void koki_set_headless(koki_engine *eng, int64_t frames)
{
    if (eng == NULL)
        return;
    eng->headless_frames = frames;
}

/* ------------------------------------------------------------------ *
 * Cache budgets
 * ------------------------------------------------------------------ */

static long meminfo_kb(void)
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

static long env_kb(const char *name, long dflt)
{
    const char *v = getenv(name);
    char *end = NULL;
    long n;

    if (v == NULL || v[0] == '\0')
        return dflt;
    n = strtol(v, &end, 10);
    if (end == v || (end != NULL && *end != '\0') || n < 0) {
        /* The Python lets int() raise here, which kills the app on a typo.
         * Falling back to the default and saying so is strictly better and
         * changes nothing when the variable is well-formed or absent. */
        nd_log(ND_LOG_KOKI, "ignoring unparseable %s=%s", name, v);
        return dflt;
    }
    return n;
}

static void init_caches(koki_engine *eng)
{
    long img_kb = 3072;
    long fx_kb = 1024;
    long total = meminfo_kb();

    if (total > 0) {
        if (total < 40L * 1024L) {
            img_kb = 1024;
            fx_kb = 384;
        } else if (total < 72L * 1024L) {
            img_kb = 1536;
            fx_kb = 512;
        }
        if (img_kb != 3072 || fx_kb != 1024)
            nd_log(ND_LOG_KOKI, "small RAM (%ldMB): cache budgets %ld/%ldKB", total / 1024, img_kb,
                   fx_kb);
    }
    img_kb = env_kb("NEODCT_KOKI_IMG_CACHE_KB", img_kb);
    fx_kb = env_kb("NEODCT_KOKI_FX_CACHE_KB", fx_kb);

    koki_lru_init(&eng->img_cache, (size_t)img_kb * 1024u);
    koki_lru_init(&eng->fx_cache, (size_t)fx_kb * 1024u);
    /* The mask cache is fixed: measured peak is 54 KB during levels 2 and 3,
     * well inside 256 KB, and nothing has ever needed to tune it. */
    koki_lru_init(&eng->mask_cache, 256u * 1024u);
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

koki_engine *koki_engine_new(nd_ui *ui, const char *app_dir)
{
    koki_engine *eng;
    size_t i;

    if (ui == NULL || app_dir == NULL)
        return NULL;

    eng = calloc(1u, sizeof *eng);
    if (eng == NULL)
        return NULL;

    eng->ui = ui;
    eng->canvas = ui->canvas; /* borrowed: 240x175 RGB888 = 126,000 bytes */
    eng->headless_frames = -1;
    (void)nd_strlcpy(eng->app_dir, app_dir, sizeof eng->app_dir);
    if (nd_snprintf(eng->assets, sizeof eng->assets, "%s/assets", app_dir) != ND_OK) {
        free(eng);
        return NULL;
    }

    eng->manifest = koki_manifest_load(eng->assets);
    if (eng->manifest == NULL) {
        free(eng);
        return NULL;
    }

    koki_input_open(&eng->input, ui);
    koki_sound_open(&eng->sound, eng->assets);
    koki_rng_init(&eng->rng);
    init_caches(eng);

    for (i = 0u; i < (size_t)KOKI_MAX_EVENTS; i++) {
        eng->event_first[i] = -1;
        eng->event_last[i] = -1;
    }

    eng->perf = (getenv("NEODCT_KOKI_PERF") != NULL);
    return eng;
}

/* ------------------------------------------------------------------ *
 * Teardown
 * ------------------------------------------------------------------ */

void koki_engine_teardown(koki_engine *eng)
{
    size_t i;

    if (eng == NULL)
        return;

    koki_stop_all_scripts(eng);
    eng->active_head = NULL;
    eng->active_tail = NULL;
    eng->current = NULL;
    for (i = 0u; i < eng->n_slots; i++) {
        eng->slots[i].live = false;
        eng->slots[i].prev = NULL;
        eng->slots[i].next = NULL;
    }
    eng->n_slots = 0u;
    eng->n_events = 0u;

    koki_lru_clear(&eng->img_cache);
    koki_lru_clear(&eng->fx_cache);
    koki_lru_clear(&eng->mask_cache);

    for (i = 0u; i < eng->n_sprites; i++)
        free(eng->sprites[i]);
    eng->n_sprites = 0u;
    eng->n_layers = 0u;

    nd_image_free(eng->backdrop_img);
    eng->backdrop_img = NULL;

    koki_sound_shutdown(&eng->sound);

    koki_manifest_free(eng->manifest);
    eng->manifest = NULL;

#ifdef __GLIBC__
    /* Hand freed heap pages back to the kernel so the rest of the OS is not
     * squeezed after the game exits. musl has no equivalent and does not need
     * one here: the app is its own process now, so exit returns everything.
     * The call is kept because a future in-core Koki would want it and
     * because it costs nothing. */
    (void)malloc_trim(0);
#endif
}

void koki_engine_free(koki_engine *eng)
{
    if (eng == NULL)
        return;
    koki_engine_teardown(eng);
    free(eng);
}

/* ------------------------------------------------------------------ *
 * Stage-level sound
 * ------------------------------------------------------------------ */

static const koki_sound *stage_sound(koki_engine *eng, const char *name)
{
    const koki_target *stage = koki_manifest_target(eng->manifest, "Stage");
    const koki_sound *s;

    if (stage == NULL)
        return NULL;
    s = koki_sound_find(stage->sounds, stage->n_sounds, name);
    if (s == NULL)
        nd_log(ND_LOG_KOKI, "Stage: unknown sound '%s'", name);
    return s;
}

void koki_stage_music(koki_engine *eng, const char *name)
{
    const koki_sound *s;

    if (eng == NULL)
        return;
    s = stage_sound(eng, name);
    if (s != NULL)
        koki_sound_music(&eng->sound, s->file);
}

void koki_stage_sfx(koki_engine *eng, const char *name)
{
    const koki_sound *s;

    if (eng == NULL)
        return;
    s = stage_sound(eng, name);
    if (s != NULL)
        koki_sound_sfx(&eng->sound, s->file);
}

void koki_stop_music(koki_engine *eng)
{
    if (eng != NULL)
        koki_sound_stop_music(&eng->sound);
}

/* ------------------------------------------------------------------ *
 * The pause menu
 * ------------------------------------------------------------------ */

static void nap(double secs)
{
    struct timespec ts;

    if (secs <= 0.0)
        return;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0)
        ts.tv_nsec = 0;
    if (ts.tv_nsec > 999999999L)
        ts.tv_nsec = 999999999L;
    (void)nanosleep(&ts, NULL);
}

/* Back key: a small confirm overlay drawn straight onto the shared canvas.
 * It does NOT re-render the game, so the frame underneath stays frozen behind
 * the box and the next render paints over it. Returns true to quit. */
static bool pause_menu(koki_engine *eng)
{
    nd_ui *ui = eng->ui;

    if (ui == NULL || ui->draw == NULL)
        return true;

    /* PIL's rectangle(fill=..., outline=...) is a fill and then a 1 px
     * border, both inclusive of the corners. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(30, 55, 210, 120), ND_BLACK);
    (void)nd_draw_rect_outline(ui->draw, ND_RECT(30, 55, 210, 120), ND_WHITE, 1);
    (void)nd_draw_text(ui->draw, 45, 62, "Quit Koki?", ui->font_n, ND_WHITE);
    (void)nd_draw_text(ui->draw, 45, 90, "Enter=Yes  C=No", ui->font_s, ND_WHITE);
    if (ui->fb != NULL && nd_fb_update(ui->fb, eng->canvas) != ND_OK)
        return true; /* the frame budget ran out: end the run, as ScriptExhausted does */

    /* Swallow the held BACK key, or the loop below would see it immediately
     * and resume in the same breath. */
    while (koki_key(eng, KOKI_KEY_BACK)) {
        if (nd_app_should_exit())
            return true;
        koki_input_poll(&eng->input);
        nap(0.02);
    }
    for (;;) {
        /* nd_app.h requires any loop longer than a frame to poll this: an
         * incoming call arrives as SIGTERM, and the Python had no equivalent
         * because IncomingCall was raised from inside read_keypress. */
        if (nd_app_should_exit())
            return true;
        koki_input_poll(&eng->input);
        if (koki_pressed(eng, KOKI_KEY_ENTER))
            return true;
        if (koki_pressed(eng, KOKI_KEY_BACK))
            return false;
        nap(0.02);
    }
}

/* ------------------------------------------------------------------ *
 * The frame loop
 * ------------------------------------------------------------------ */

int koki_engine_run(koki_engine *eng)
{
    int64_t frames = 0;
    double busy_acc = 0.0;
    double t_report;
    bool headless;

    if (eng == NULL)
        return 1;

    headless = (eng->headless_frames >= 0);
    if (headless) {
        /* The virtual clock must exist BEFORE any script runs, so that every
         * script in a frame sees one timestamp. On the device now() is read
         * per call and two scripts in a frame can see different times; both
         * behaviours are the Python's. */
        eng->have_vtime = true;
        eng->vtime = 0.0;
    }

    koki_start_flag(eng);
    t_report = nd_time_monotonic();

    while (!eng->quit) {
        double t0 = nd_time_monotonic();
        double busy;

        if (nd_app_should_exit())
            break;

        koki_input_poll(&eng->input);
        if (koki_pressed(eng, KOKI_KEY_BACK)) {
            if (pause_menu(eng))
                break;
            koki_input_poll(&eng->input);
        }

        koki_step_frame(eng);
        koki_render(eng);

        busy = nd_time_monotonic() - t0;
        busy_acc += busy;
        frames++;
        if ((frames % 30) == 0)
            koki_sound_check(&eng->sound);
        if (eng->perf && nd_time_monotonic() - t_report >= 5.0) {
            double elapsed = nd_time_monotonic() - t_report;

            nd_log(ND_LOG_KOKI, "avg frame %.1fms (%.1f fps)", busy_acc / (double)frames * 1000.0,
                   (double)frames / elapsed);
            frames = 0;
            busy_acc = 0.0;
            t_report = nd_time_monotonic();
        }

        if (headless) {
            eng->vtime += KOKI_FRAME_DT;
            eng->headless_frames--;
            if (eng->headless_frames <= 0)
                break;
        } else {
            nap(KOKI_FRAME_DT - busy);
        }
    }

    koki_engine_teardown(eng);
    return 0;
}
