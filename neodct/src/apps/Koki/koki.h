/* koki.h -- the Koki engine: a small Scratch 3 runtime, in C.
 *
 * A one-to-one port of System/apps/Koki/engine.py (1,153 lines). The game
 * itself lives in koki_game*.c and is a port of game.py; this file is the
 * runtime it is written against, and it is the half that has to be right
 * first, because every one of the 304 game scripts is expressed in these
 * calls.
 *
 * ============ WHAT SCRATCH IS, IN FIVE LINES ============
 *
 * A 480x360 stage with (0,0) at its centre, x right and y UP. Everything on
 * it is a sprite: a position, a direction, a size percentage, a visible flag,
 * a ghost (transparency) and brightness effect, and a list of costumes.
 * Sprites run scripts; a script runs one step per frame and suspends.
 * Scripts talk by broadcasting messages, and a broadcast RESTARTS a handler
 * that is already running. Two sprites touch when their opaque PIXELS
 * overlap, not their rectangles.
 *
 * Our screen is 240x175, so the stage is drawn at half size with 2.5 stage
 * units cropped off the top and the bottom.
 *
 * ============ THE ENGINE POINTER IS EXPLICIT ============
 *
 * engine.py's game code closes over one `eng`. The C passes it, because a
 * hidden singleton is untestable and koki_engine is exactly the object a
 * unit test wants two of. Sprites carry ->eng so sprite calls need no extra
 * argument; only the handful of engine-level calls (broadcast, backdrop,
 * randint) take one, and the game files keep it in one file-scope `KE`.
 *
 * ============ LIFETIME AND OWNERSHIP ============
 *
 * koki_engine owns: the parsed manifest, every koki_sprite, the three
 * caches and everything in them, and the backdrop surface. It does NOT own
 * ui->canvas (the core lends it) and it does not own the keypad descriptor.
 * koki_engine_teardown() releases all of it and must run even on the error
 * path -- the audio device is in there.
 */

#ifndef KOKI_H_INCLUDED
#define KOKI_H_INCLUDED

#include <sys/types.h> /* pid_t, for the external audio players */

#include "nd_image.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "koki_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Constants -- engine.py's module scope, verbatim
 * ------------------------------------------------------------------ */

#define KOKI_STAGE_SCALE 0.5
#define KOKI_SCREEN_W    240
#define KOKI_SCREEN_H    175
#define KOKI_CENTER_X    120.0 /* SCREEN_W / 2.0 */
#define KOKI_CENTER_Y    87.5  /* SCREEN_H / 2.0 */
#define KOKI_FPS         30
#define KOKI_FRAME_DT    (1.0 / 30.0)

/* alpha > 40 counts as solid for collision (Scratch-like). */
#define KOKI_ALPHA_SOLID 40

/* Fixed capacities, all measured against assets/manifest.json rather than
 * guessed: 47 targets (Stage + 46 sprites), the widest sprite has 15
 * costumes, the Stage declares 11 sounds. Slack, but bounded -- nothing here
 * is sized by input at run time. */
#define KOKI_MAX_SPRITES  48
#define KOKI_MAX_HANDLERS 400 /* 304 registered */
#define KOKI_MAX_EVENTS   160 /* 107 distinct   */
#define KOKI_NAME_MAX     40
#define KOKI_PATH_REL_MAX 64

/* ------------------------------------------------------------------ *
 * Costumes, sounds, sprites
 * ------------------------------------------------------------------ */

typedef enum { KOKI_ROT_ALL_AROUND = 0, KOKI_ROT_LEFT_RIGHT } koki_rot_style;

typedef struct {
    char name[KOKI_NAME_MAX];
    char lname[KOKI_NAME_MAX]; /* name lowercased: lookup is case-insensitive */
    char img[KOKI_PATH_REL_MAX];
    double cx, cy; /* rotation centre, already scaled at bake time */
    bool has_bbox; /* false == the box is the whole image          */
    int32_t bx0, by0, bx1, by1;
} koki_costume;

typedef struct {
    char name[KOKI_NAME_MAX];
    char file[KOKI_PATH_REL_MAX];
    double dur; /* seconds, from the manifest -- play_until_done waits it */
} koki_sound;

typedef struct koki_sprite {
    struct koki_engine *eng;
    char name[KOKI_NAME_MAX];

    koki_costume *costumes; /* owned by the engine's manifest arena */
    size_t n_costumes;
    koki_sound *sounds;
    size_t n_sounds;

    double baked_size; /* manifest "size": the % the costumes were baked at */

    /* Live state. x/y/direction/rotation_style/costume_i come from the
     * manifest's editor-left pose; size starts at "default_size"; visible
     * ALWAYS starts false, whatever the manifest says, because the original
     * project's every sprite begins "when flag clicked: hide". */
    double x, y;
    double direction; /* degrees, Scratch convention: 90 = right */
    koki_rot_style rotation_style;
    bool visible;
    double size;       /* percent */
    double ghost;      /* 0..100  */
    double brightness; /* -100..100 */
    size_t costume_i;

    /* Player only: vertical velocity. On the sprite because the physics
     * script needs it across suspensions and Python put it there too. */
    double sy;
} koki_sprite;

/* ------------------------------------------------------------------ *
 * Scripts
 * ------------------------------------------------------------------ */

struct koki_engine;

/* A script body. F is the base of a KOKI_STACK_DEPTH frame array. */
typedef koki_step (*koki_script_fn)(koki_frame *F);

/* One registered handler. There is exactly one slot per registration, and
 * the slot IS the entry in `active` -- see the generation counter below. */
typedef struct koki_slot {
    int32_t key;         /* _hkey: 1-based, registration order  */
    int32_t event;       /* index into the engine's event table */
    koki_sprite *sprite; /* NULL for a Stage-level handler      */
    koki_script_fn fn;
    koki_frame stack[KOKI_STACK_DEPTH];

    /* Scheduler state. `live` says the slot is in the active list; `dead`
     * marks it for the end-of-frame sweep. `generation` increments on every
     * (re)start, which is how the frame snapshot tells a restarted script
     * from the instance it was holding -- Python compares object identity,
     * and reusing one slot per key would otherwise lose that. */
    bool live;
    bool dead;
    uint32_t generation;
    struct koki_slot *prev, *next; /* the active list, insertion-ordered */

    int32_t next_in_event; /* -1 terminated; registration order per event */
} koki_slot;

/* ------------------------------------------------------------------ *
 * Input -- eight logical keys
 * ------------------------------------------------------------------ */

typedef enum {
    KOKI_KEY_LEFT = 0,
    KOKI_KEY_RIGHT,
    KOKI_KEY_UP,
    KOKI_KEY_DOWN,
    KOKI_KEY_Z,
    KOKI_KEY_X,
    KOKI_KEY_ENTER,
    KOKI_KEY_BACK,
    KOKI_KEY_COUNT
} koki_key_id;

typedef struct {
    int fd; /* the inherited keypad channel, or -1 */
    bool held[KOKI_KEY_COUNT];
    bool pressed[KOKI_KEY_COUNT]; /* edges seen this frame */
} koki_input;

/* ------------------------------------------------------------------ *
 * The byte-budgeted LRU -- engine.py's LRUImages
 * ------------------------------------------------------------------ */

/* A cache key is a path for the image cache and a flattened tuple for the
 * other two; both become one bounded string so a single implementation
 * serves all three. Longest in practice: an fx key is a 46-character image
 * path plus five quantised numbers.
 *
 * Entries are heap-allocated rather than drawn from a fixed pool, so the
 * cache is bounded in BYTES and unbounded in COUNT -- which is what
 * LRUImages is, and a pool would have added an eviction rule the Python does
 * not have. An entry is 112 bytes beside images of 4 KB and up. */
#define KOKI_CACHE_KEY_MAX 96

typedef struct koki_lru_entry {
    char key[KOKI_CACHE_KEY_MAX];
    uint32_t hash; /* FNV-1a of key: the list is scanned, and comparing an
                    * int first turns 200 strcmps a frame into 200 loads */
    nd_image *img; /* owned by the cache */
    size_t cost;   /* w * h * channels, exactly as _cost() computes it */
    struct koki_lru_entry *prev, *next;
} koki_lru_entry;

typedef struct {
    size_t budget;
    size_t bytes;
    size_t count;
    koki_lru_entry *head; /* least recently used */
    koki_lru_entry *tail; /* most recently used  */
} koki_lru;

/* ------------------------------------------------------------------ *
 * Sound
 * ------------------------------------------------------------------ */

/* _MiniaudioMixer.MAX_SFX. Three effects over one looping music voice; a
 * fourth effect is DROPPED rather than queued, and nothing is stolen from
 * the oldest. See README-PORT.md, "Decision 2". */
#define KOKI_SND_MAX_SFX 3

/* ---- the in-process mixer's four numbers ----
 *
 * All of README-PORT.md's "Decision 1" lands here. Read that table before
 * moving any of them: the two buffer sizes are a latency budget, not a
 * performance tuning, and the owner's complaint about the audio is latency.
 *
 * KOKI_MIX_RATE is _MiniaudioMixer.RATE, and it is also, measured, what
 * every one of the 57 files in assets/snd already is: 22050 Hz, mono, s16.
 * A source that disagrees is resampled per voice before the fold, so the
 * output format never depends on which file started first. */
#define KOKI_MIX_RATE 22050

/* The mix granularity AND the write size: 128 frames = 256 bytes = 5.805 ms
 * at 22050 Hz. An effect started at any instant is in the next chunk, so
 * quantisation costs 5.8 ms rather than a buffer. */
#define KOKI_MIX_CHUNK_FRAMES 128u

/* Source frames decoded per refill, per voice: 1024 x channels x 2 B. */
#define KOKI_MIX_STAGE_FRAMES 1024u

/* What SO_SNDBUF is asked for on our end of the socket to aplay. THIS IS THE
 * ONE THAT BITES: a default AF_UNIX SOCK_STREAM send buffer is 212,992
 * bytes, which at 44,100 B/s is 969 ms of audio already handed onward --
 * a second of lag, from a line of code nobody writes. The kernel clamps
 * sk_sndbuf up to SOCK_MIN_SNDBUF (4608 here) and charges skb truesize
 * rather than payload, so 2048 buys about 1024 bytes of real payload =
 * 23.2 ms. koki_audio.c reads the granted value back and logs it. */
#define KOKI_MIX_SOCK_BYTES 2048

/* aplay's ALSA ring, in milliseconds; --period-time is one chunk.
 * NEODCT_KOKI_ABUF_MS moves it, clamped to [MIN, MAX]. engine.py's default
 * for that variable is 150; 30 is deliberately five times tighter, and
 * raising it is the first thing to try if the phone crackles. */
#define KOKI_MIX_ALSA_MS     30
#define KOKI_MIX_ALSA_MS_MIN 10
#define KOKI_MIX_ALSA_MS_MAX 500

/* Opaque: koki_mixer.c owns the voices, koki_audio.c owns the one aplay and
 * the feeder thread. Neither type puts pthread.h in this header. */
typedef struct koki_mixer koki_mixer;
struct koki_sink;

typedef struct {
    bool enabled;
    char disabled_reason[128];

    /* The in-process path. Both NULL means the external players below, or
     * silence; they are never live at the same time, which is also what
     * keeps fork() away from a threaded process (CODING-STANDARDS 1.1). */
    koki_mixer *mixer;
    struct koki_sink *sink;
    int32_t alsa_ms;    /* what aplay was actually asked for */
    int32_t latency_ms; /* the end-to-end figure that was logged */
    uint32_t underruns; /* last count reported by koki_sound_check() */

    /* The external-player fallback, unchanged. */
    bool have_wav_player;
    bool have_mp3_player;
    char wav_player[64];
    char mp3_player[64];
    bool mpv_trim_ok; /* this mpv accepts the memory-trim flags */
    pid_t music_pid;
    pid_t sfx_pid[KOKI_SND_MAX_SFX];
    int max_sfx;
    bool music_death_logged;
    char base_dir[ND_PATH_MAX];
} koki_sound_mgr;

/* ------------------------------------------------------------------ *
 * Randomness -- CPython's MT19937, bit for bit
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t mt[624];
    int32_t index;
} koki_rng;

/* ------------------------------------------------------------------ *
 * Game variables
 * ------------------------------------------------------------------ */

/* game.py's `V` dict. Every key is known at compile time and every value is
 * a number, so it is an array rather than a map -- behaviour-identical, and
 * it removes a hash lookup from the middle of the boss fights. The two keys
 * the damage gates create lazily are pre-set to their -99 default, which is
 * what V.get(k, -99) would have returned. */
typedef enum {
    KOKI_V_LIVES = 0,
    KOKI_V_DOORS,
    KOKI_V_TAKEN_DAMAGE,
    KOKI_V_KNOCKOUTS,
    KOKI_V_HAS_HEALED,
    KOKI_V_CANNONDEFEATS,
    KOKI_V_RIBYDANGER,
    KOKI_V_EVILCANONBALLDIRECTION,
    KOKI_V_HEALWAVEDIRECTION,
    KOKI_V_DAMAGEWAY4,
    KOKI_V_HURT_T,
    KOKI_V_PLANE_HURT_T,
    KOKI_V_COUNT
} koki_varid;

/* ------------------------------------------------------------------ *
 * The engine
 * ------------------------------------------------------------------ */

typedef struct koki_engine {
    nd_ui *ui;
    nd_image *canvas; /* ui->canvas, borrowed */
    char app_dir[ND_PATH_MAX];
    char assets[ND_PATH_MAX];

    struct koki_manifest *manifest; /* owned */

    koki_input input;
    koki_sound_mgr sound;
    koki_rng rng;
    double vars[KOKI_V_COUNT];

    koki_lru img_cache;  /* path -> decoded RGBA        */
    koki_lru fx_cache;   /* variant key -> processed RGBA */
    koki_lru mask_cache; /* (path, flip) -> 8-bit alpha  */

    koki_sprite *sprites[KOKI_MAX_SPRITES]; /* by creation order, owned */
    size_t n_sprites;
    koki_sprite *layers[KOKI_MAX_SPRITES]; /* draw order, back -> front */
    size_t n_layers;

    nd_image *backdrop_img; /* owned; 240x175 RGB888 = 126,000 bytes */
    char backdrop_name[KOKI_NAME_MAX];

    char events[KOKI_MAX_EVENTS][KOKI_NAME_MAX];
    int32_t event_first[KOKI_MAX_EVENTS]; /* head of the slot chain, -1 */
    int32_t event_last[KOKI_MAX_EVENTS];
    size_t n_events;

    koki_slot slots[KOKI_MAX_HANDLERS];
    size_t n_slots;
    koki_slot *active_head;
    koki_slot *active_tail;
    koki_slot *current; /* the slot being stepped; stop_other_scripts spares it */

    bool quit;
    bool perf;

    /* The virtual clock the headless harness uses. engine.py sets _vtime to
     * 0.0 BEFORE start_flag(), so every script sees one timestamp per frame;
     * on the device now() is read per call and two scripts in a frame can
     * see different times. Both are reproduced. */
    bool have_vtime;
    double vtime;
    int64_t headless_frames; /* < 0 == not headless */
} koki_engine;

/* ------------------------------------------------------------------ *
 * Engine lifecycle
 * ------------------------------------------------------------------ */

/* Build an engine over `ui`, reading assets from <app_dir>/assets.
 * *out is owned by the caller; release with koki_engine_free(), which runs
 * koki_engine_teardown() first. NULL on any failure, with the reason
 * logged. */
koki_engine *koki_engine_new(nd_ui *ui, const char *app_dir);

/* engine.py's teardown(): stop everything, empty all three caches (map AND
 * byte counter), drop the sprites, release the audio device, malloc_trim.
 * Idempotent, and safe to call on the error path. */
void koki_engine_teardown(koki_engine *eng);
void koki_engine_free(koki_engine *eng);

/* engine.py's run(): the frame loop, the pause menu and the teardown.
 * Returns 0 for a normal exit. */
int koki_engine_run(koki_engine *eng);

/* Game clock: wall time normally, virtual time when headless. */
double koki_now(const koki_engine *eng);

/* The harness hook: run exactly n frames on a virtual clock, then stop.
 * Must be called before koki_engine_run(). */
void koki_set_headless(koki_engine *eng, int64_t frames);

/* ------------------------------------------------------------------ *
 * Sprites and layers
 * ------------------------------------------------------------------ */

/* eng.sprite(name): creates on first use and appends to the draw list.
 * NULL if the manifest has no such target or the table is full. */
koki_sprite *koki_sprite_get(koki_engine *eng, const char *name);

void koki_set_layer_order(koki_engine *eng, const char *const *names, size_t n);
void koki_layer_front(koki_sprite *s);
void koki_layer_back(koki_sprite *s);

/* Looks. koki_costume() is a case-insensitive name lookup that resolves to
 * the FIRST costume with that lower-cased name; an unknown name logs and
 * leaves the costume alone, exactly as the Python does. */
void koki_set_costume(koki_sprite *s, const char *name);
void koki_set_costume_i(koki_sprite *s, int32_t index); /* Scratch is 0-based here */
void koki_next_costume(koki_sprite *s);
const char *koki_costume_name(const koki_sprite *s);
int32_t koki_costume_number(const koki_sprite *s); /* 1-based, as Scratch */
bool koki_costume_is(const koki_sprite *s, const char *name);
void koki_show(koki_sprite *s);
void koki_hide(koki_sprite *s);
void koki_clear_fx(koki_sprite *s);

/* Motion. */
void koki_goto(koki_sprite *s, double x, double y);
void koki_goto_sprite(koki_sprite *s, const koki_sprite *other);
void koki_point(koki_sprite *s, double direction);
void koki_point_towards(koki_sprite *s, const koki_sprite *other);
void koki_move_steps(koki_sprite *s, double steps);

/* Sound, per sprite. */
void koki_play(koki_sprite *s, const char *sound_name);
void koki_sprite_music(koki_sprite *s, const char *sound_name);

/* ------------------------------------------------------------------ *
 * The engine's own generators -- child protothreads, run at F + 1
 * ------------------------------------------------------------------ */

/* wait(secs): ALWAYS yields at least once, so W(0) is one frame. At 30 fps
 * W(0.05) is two frames. */
koki_step koki_wait(koki_frame *F, koki_engine *eng, double secs);

/* wait_until(pred): evaluates the predicate FIRST, so it can complete with
 * zero yields and let the caller carry on in the same frame.
 *
 * Ported for completeness and NOT USED by the shipped game.py, which is the
 * same for koki_point_towards() below; both are part of what engine.py is,
 * and --gc-sections drops them from app.so if they stay unreferenced. */
typedef bool (*koki_pred_fn)(koki_engine *eng);
koki_step koki_wait_until(koki_frame *F, koki_engine *eng, koki_pred_fn pred);

/* glide(secs, tx, ty). t0 is captured on the FIRST STEP, not at creation,
 * and the first step moves nothing. */
koki_step koki_glide(koki_frame *F, koki_sprite *s, double secs, double tx, double ty);

/* glide_to_sprite(secs, other): samples other's position once, on the first
 * step, which is Scratch's rule and is observable. */
koki_step koki_glide_to(koki_frame *F, koki_sprite *s, double secs, const koki_sprite *other);

/* play_until_done(name): start the sfx, then wait its manifest duration. */
koki_step koki_play_until_done(koki_frame *F, koki_sprite *s, const char *sound_name);

/* ------------------------------------------------------------------ *
 * Collision
 * ------------------------------------------------------------------ */

/* Scratch-style: visible-pixel rectangles as the cheap gate, then a real
 * alpha-mask overlap. `inset` shrinks both gate rectangles by inset/2 on
 * each side; game.py passes 0 everywhere except _cball_friendly_fire. */
bool koki_touching(koki_sprite *s, koki_sprite *other, double inset);

/* Exposed for the unit tests: the two geometry helpers the collision test
 * is built on. */
void koki_screen_rect(koki_sprite *s, double inset, double out[4]);
void koki_paste_origin(koki_sprite *s, double out[2]);

/* ------------------------------------------------------------------ *
 * Rendering
 * ------------------------------------------------------------------ */

void koki_render(koki_engine *eng);
void koki_backdrop(koki_engine *eng, const char *name);

/* The current costume with flip/size/ghost/brightness applied. The returned
 * image is OWNED BY THE ENGINE (either the image cache or the fx cache);
 * blit from it and let it go. Writes the paste centre to *cx, *cy. */
const nd_image *koki_costume_variant(koki_engine *eng, koki_sprite *s, double *cx, double *cy);

/* The decoded costume, from the image cache. Owned by the cache. */
const nd_image *koki_load_image(koki_engine *eng, const char *rel);

/* ------------------------------------------------------------------ *
 * Scheduling
 * ------------------------------------------------------------------ */

/* eng.on(event, sprite)(fn). Returns the handler key, or -1 if the tables
 * are full (which is a build-time-sized condition, not a runtime one). */
int32_t koki_on(koki_engine *eng, const char *event, koki_sprite *sprite, koki_script_fn fn);

void koki_broadcast(koki_engine *eng, const char *event);
void koki_start_flag(koki_engine *eng);
void koki_stop_other_scripts(koki_engine *eng, const koki_sprite *sprite);
void koki_stop_all_scripts(koki_engine *eng);

/* The sprite of the script currently being stepped -- the C equivalent of
 * the Python factories that close over a door. NULL outside a step. */
koki_sprite *koki_script_sprite(const koki_engine *eng);

/* The message that started the script currently being stepped. The other
 * half of the same trick: game.py's five grade screens are one factory
 * closing over a costume and a music track, and the grade IS the message
 * ("a".."f"), so one handler plus a five-row table replaces five bodies.
 * "" outside a step. */
const char *koki_script_event(const koki_engine *eng);

/* One frame's worth of script stepping plus the dead sweep. Exposed so a
 * test can drive frames without the render or the sleep. */
void koki_step_frame(koki_engine *eng);

/* Live scripts, for the tests and the memory report. */
size_t koki_active_count(const koki_engine *eng);

/* ------------------------------------------------------------------ *
 * Stage-level sound, variables, input, randomness
 * ------------------------------------------------------------------ */

void koki_stage_music(koki_engine *eng, const char *name);
void koki_stage_sfx(koki_engine *eng, const char *name);
void koki_stop_music(koki_engine *eng);

bool koki_key(const koki_engine *eng, koki_key_id k);
bool koki_pressed(const koki_engine *eng, koki_key_id k);
bool koki_any_key(const koki_engine *eng);
double koki_kdir(const koki_engine *eng); /* right - left, as -1/0/1 */
void koki_input_open(koki_input *in, nd_ui *ui);
void koki_input_poll(koki_input *in);

/* randint(a, b), inclusive, order-normalised, through the private MT19937 */
int32_t koki_randint(koki_engine *eng, int32_t a, int32_t b);
void koki_rng_init(koki_rng *r); /* seeds from OS entropy, as Random() does */
void koki_rng_seed(koki_rng *r, uint32_t seed);
uint32_t koki_rng_u32(koki_rng *r);
uint32_t koki_rng_getrandbits(koki_rng *r, int32_t k);
uint32_t koki_rng_below(koki_rng *r, uint32_t n);

/* ------------------------------------------------------------------ *
 * Caches -- exposed for the tests and the teardown
 * ------------------------------------------------------------------ */

void koki_lru_init(koki_lru *c, size_t budget_bytes);
nd_image *koki_lru_get(koki_lru *c, const char *key);
void koki_lru_put(koki_lru *c, const char *key, nd_image *img); /* takes ownership */
void koki_lru_clear(koki_lru *c);

/* ------------------------------------------------------------------ *
 * Sound manager
 * ------------------------------------------------------------------ */

void koki_sound_open(koki_sound_mgr *sm, const char *assets_dir);
void koki_sound_music(koki_sound_mgr *sm, const char *rel);
void koki_sound_sfx(koki_sound_mgr *sm, const char *rel);
void koki_sound_stop_music(koki_sound_mgr *sm);
void koki_sound_stop_all(koki_sound_mgr *sm);
void koki_sound_check(koki_sound_mgr *sm);
void koki_sound_shutdown(koki_sound_mgr *sm);

/* ------------------------------------------------------------------ *
 * The mixer -- engine.py's _MiniaudioMixer
 * ------------------------------------------------------------------ *
 *
 * Deliberately free of any device: koki_mixer_pull() hands finished samples
 * to whoever asked, and koki_audio.c is the only caller that puts them in a
 * socket. That is what lets test_koki_audio.c prove four voices really do
 * end up in one stream on a machine with no sound card, which is every
 * machine this port has run on.
 *
 * `rel` is a path under the assets directory given to koki_mixer_new(), the
 * same "snd/<md5>.wav" the manifest carries.
 */

/* owned by the caller; free with koki_mixer_free() */
koki_mixer *koki_mixer_new(const char *base_dir);
void koki_mixer_free(koki_mixer *m);

void koki_mixer_music(koki_mixer *m, const char *rel); /* loops; REPLACES */
void koki_mixer_sfx(koki_mixer *m, const char *rel);   /* one-shot; may be dropped */
void koki_mixer_stop_music(koki_mixer *m);
void koki_mixer_stop_all(koki_mixer *m);

/* Always produces exactly `frames` frames of 22050 Hz mono s16 -- silence
 * when nothing is playing. Returns `frames`. Safe from any one thread at a
 * time; the mixer's lock covers the voice list. */
size_t koki_mixer_pull(koki_mixer *m, int16_t *out, size_t frames);

int32_t koki_mixer_live_sfx(koki_mixer *m);
bool koki_mixer_music_live(koki_mixer *m);
void koki_mixer_note_underrun(koki_mixer *m);
uint32_t koki_mixer_underruns(koki_mixer *m);

/* The three pure sums the design rests on, exported so a test can check the
 * arithmetic rather than the prose. */

/* audioop.add(a, b, 2): saturating s16, applied PAIRWISE and in voice order.
 * Not the same as one wide accumulator -- see README-PORT.md, "Decision 4". */
int16_t koki_mix_add(int16_t a, int16_t b);

/* One chunk + the socket's payload capacity + aplay's ring, in ms. */
int32_t koki_mix_latency_ms(int32_t sock_bytes, int32_t alsa_ms);

/* True when the sink must have run dry: the wall clock has overtaken the
 * audio clock by more than one ALSA buffer. */
bool koki_mix_underrun(double elapsed_ms, double written_ms, int32_t guard_ms);

/* ------------------------------------------------------------------ *
 * The game
 * ------------------------------------------------------------------ */

/* game.py's register_all(eng): creates all 45 sprites, sets the layer order
 * and registers all 304 handlers, in the Python's textual order. */
void koki_register_all(koki_engine *eng);

#ifdef __cplusplus
}
#endif

#endif /* KOKI_H_INCLUDED */
