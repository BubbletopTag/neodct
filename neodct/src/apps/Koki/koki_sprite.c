/* koki_sprite.c -- the Sprite: state, costumes, motion, and the two motion
 * generators.
 *
 * A one-to-one port of engine.py's Costume and Sprite. Nothing here is
 * clever; what it has to be is exact, because every coordinate in game.py is
 * fed through these functions and a rounding choice made here moves a
 * character on screen.
 *
 * ============ THE THREE THINGS THAT LOOK ARBITRARY ============
 *
 * 1. visible ALWAYS starts false, whatever the manifest's pose says. Every
 *    sprite in the original project begins "when flag clicked: hide", and the
 *    baker records the editor pose rather than the runtime one.
 * 2. Costume lookup is case-INSENSITIVE and resolves to the FIRST costume
 *    with that lower-cased name. Python builds it with setdefault, so a
 *    duplicate name keeps the EARLIER index; scanning forwards and stopping
 *    at the first hit is the same rule.
 * 3. point_towards uses atan2 with its ARGUMENTS SWAPPED -- atan2(dx, dy),
 *    not atan2(dy, dx). That is Scratch's convention (0 degrees is up, 90 is
 *    right) and it is not a transcription slip.
 *
 * ============ WHEN A GLIDE SAMPLES ITS TARGET ============
 *
 * A Python generator runs no code until its first next(). `yield from
 * spr.glide(...)` therefore captures the start position and the start time on
 * the FIRST STEP -- in the same frame the caller reached the statement -- and
 * then suspends without moving anything. glide_to_sprite samples the other
 * sprite's position at that same instant, once, which is Scratch's rule and
 * is observable whenever the target is itself moving. The protothreads below
 * capture on pc == 0 for exactly this reason.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_log.h"
#include "nd_types.h"

#include "koki_manifest.h"

/* ------------------------------------------------------------------ *
 * Creation
 * ------------------------------------------------------------------ */

koki_sprite *koki_sprite_get(koki_engine *eng, const char *name)
{
    koki_sprite *s;
    const koki_target *t;
    size_t i;

    if (eng == NULL || name == NULL)
        return NULL;
    for (i = 0u; i < eng->n_sprites; i++) {
        if (strcmp(eng->sprites[i]->name, name) == 0)
            return eng->sprites[i];
    }

    t = koki_manifest_target(eng->manifest, name);
    if (t == NULL) {
        nd_log_err(ND_LOG_KOKI, "no such target in manifest: %s", name);
        return NULL;
    }
    if (t->n_costumes == 0u) {
        nd_log_err(ND_LOG_KOKI, "target %s has no costumes", name);
        return NULL;
    }
    if (eng->n_sprites >= KOKI_MAX_SPRITES) {
        nd_log_err(ND_LOG_KOKI, "sprite table full at %d", KOKI_MAX_SPRITES);
        return NULL;
    }

    /* One allocation per sprite, 45 of them, freed in koki_engine_teardown().
     * The costume and sound arrays are borrowed from the manifest arenas. */
    s = calloc(1u, sizeof *s);
    if (s == NULL)
        return NULL;

    s->eng = eng;
    (void)nd_strlcpy(s->name, t->name, sizeof s->name);
    s->costumes = t->costumes;
    s->n_costumes = t->n_costumes;
    s->sounds = t->sounds;
    s->n_sounds = t->n_sounds;
    s->baked_size = t->size;
    s->x = t->x;
    s->y = t->y;
    s->direction = t->direction;
    s->rotation_style = t->rotation_style;
    s->visible = false; /* see the header comment */
    s->size = t->default_size;
    s->ghost = 0.0;
    s->brightness = 0.0;
    s->costume_i =
        (size_t)(((t->current_costume % (int32_t)t->n_costumes) + (int32_t)t->n_costumes) %
                 (int32_t)t->n_costumes);
    s->sy = 0.0;

    eng->sprites[eng->n_sprites++] = s;
    eng->layers[eng->n_layers++] = s;
    return s;
}

/* ------------------------------------------------------------------ *
 * Layers
 * ------------------------------------------------------------------ */

static void layer_remove(koki_engine *eng, const koki_sprite *s)
{
    size_t i;

    for (i = 0u; i < eng->n_layers; i++) {
        if (eng->layers[i] == s) {
            memmove(&eng->layers[i], &eng->layers[i + 1u],
                    (eng->n_layers - i - 1u) * sizeof eng->layers[0]);
            eng->n_layers--;
            return;
        }
    }
}

void koki_layer_front(koki_sprite *s)
{
    koki_engine *eng;

    if (s == NULL)
        return;
    eng = s->eng;
    layer_remove(eng, s);
    eng->layers[eng->n_layers++] = s;
}

void koki_layer_back(koki_sprite *s)
{
    koki_engine *eng;

    if (s == NULL)
        return;
    eng = s->eng;
    layer_remove(eng, s);
    memmove(&eng->layers[1], &eng->layers[0], eng->n_layers * sizeof eng->layers[0]);
    eng->layers[0] = s;
    eng->n_layers++;
}

/* set_layer_order(names): a STABLE sort by the index of each sprite's name in
 * `names`, with 999 for a name that is not listed.
 *
 * game.py passes 44 names for 45 sprites: Enemy4Stats is missing, gets 999
 * and therefore renders in FRONT OF EVERYTHING, White included. That is
 * visible on the final-boss screen and it is reproduced deliberately --
 * "port the bug too" (CODING-STANDARDS.md section 9.4). */
void koki_set_layer_order(koki_engine *eng, const char *const *names, size_t n)
{
    int32_t rank[KOKI_MAX_SPRITES];
    size_t i;
    size_t j;

    if (eng == NULL || names == NULL)
        return;
    for (i = 0u; i < eng->n_layers; i++) {
        rank[i] = 999;
        for (j = 0u; j < n; j++) {
            if (strcmp(eng->layers[i]->name, names[j]) == 0) {
                rank[i] = (int32_t)j;
                break;
            }
        }
    }

    /* Insertion sort: stable by construction, and 45 elements makes the
     * quadratic term irrelevant. Python's list.sort() is stable, and with
     * 999 shared by more than one sprite the tie order is observable. */
    for (i = 1u; i < eng->n_layers; i++) {
        koki_sprite *s = eng->layers[i];
        int32_t r = rank[i];

        j = i;
        while (j > 0u && rank[j - 1u] > r) {
            eng->layers[j] = eng->layers[j - 1u];
            rank[j] = rank[j - 1u];
            j--;
        }
        eng->layers[j] = s;
        rank[j] = r;
    }
}

/* ------------------------------------------------------------------ *
 * Looks
 * ------------------------------------------------------------------ */

void koki_set_costume(koki_sprite *s, const char *name)
{
    char want[KOKI_NAME_MAX];
    size_t i;

    if (s == NULL || name == NULL)
        return;
    koki_lower(want, sizeof want, name);
    for (i = 0u; i < s->n_costumes; i++) {
        if (strcmp(s->costumes[i].lname, want) == 0) {
            s->costume_i = i;
            return;
        }
    }
    nd_log(ND_LOG_KOKI, "%s: unknown costume '%s'", s->name, name);
}

void koki_set_costume_i(koki_sprite *s, int32_t index)
{
    int32_t n;

    if (s == NULL || s->n_costumes == 0u)
        return;
    n = (int32_t)s->n_costumes;
    /* Python's % is never negative; C's can be. */
    s->costume_i = (size_t)(((index % n) + n) % n);
}

void koki_next_costume(koki_sprite *s)
{
    if (s == NULL || s->n_costumes == 0u)
        return;
    s->costume_i = (s->costume_i + 1u) % s->n_costumes;
}

const char *koki_costume_name(const koki_sprite *s)
{
    if (s == NULL || s->n_costumes == 0u)
        return "";
    return s->costumes[s->costume_i].name;
}

int32_t koki_costume_number(const koki_sprite *s)
{
    if (s == NULL)
        return 0;
    return (int32_t)s->costume_i + 1; /* Scratch is 1-based */
}

bool koki_costume_is(const koki_sprite *s, const char *name)
{
    char want[KOKI_NAME_MAX];

    if (s == NULL || name == NULL || s->n_costumes == 0u)
        return false;
    koki_lower(want, sizeof want, name);
    return strcmp(s->costumes[s->costume_i].lname, want) == 0;
}

void koki_show(koki_sprite *s)
{
    if (s != NULL)
        s->visible = true;
}

void koki_hide(koki_sprite *s)
{
    if (s != NULL)
        s->visible = false;
}

void koki_clear_fx(koki_sprite *s)
{
    if (s != NULL) {
        s->ghost = 0.0;
        s->brightness = 0.0;
    }
}

/* ------------------------------------------------------------------ *
 * Motion
 * ------------------------------------------------------------------ */

void koki_goto(koki_sprite *s, double x, double y)
{
    if (s != NULL) {
        s->x = x;
        s->y = y;
    }
}

void koki_goto_sprite(koki_sprite *s, const koki_sprite *other)
{
    if (s != NULL && other != NULL) {
        s->x = other->x;
        s->y = other->y;
    }
}

void koki_point(koki_sprite *s, double direction)
{
    if (s != NULL)
        s->direction = direction;
}

void koki_point_towards(koki_sprite *s, const koki_sprite *other)
{
    double dx;
    double dy;

    if (s == NULL || other == NULL)
        return;
    dx = other->x - s->x;
    dy = other->y - s->y;
    /* Scratch's swapped-argument atan2: 0 degrees is up. */
    s->direction = atan2(dx, dy) * 180.0 / M_PI;
}

void koki_move_steps(koki_sprite *s, double steps)
{
    double rad;

    if (s == NULL)
        return;
    rad = s->direction * M_PI / 180.0;
    s->x += steps * sin(rad);
    s->y += steps * cos(rad);
}

/* ------------------------------------------------------------------ *
 * Sound, per sprite
 * ------------------------------------------------------------------ */

void koki_play(koki_sprite *s, const char *sound_name)
{
    const koki_sound *snd;

    if (s == NULL)
        return;
    snd = koki_sound_find(s->sounds, s->n_sounds, sound_name);
    if (snd == NULL) {
        nd_log(ND_LOG_KOKI, "%s: unknown sound '%s'", s->name, sound_name);
        return;
    }
    koki_sound_sfx(&s->eng->sound, snd->file);
}

void koki_sprite_music(koki_sprite *s, const char *sound_name)
{
    const koki_sound *snd;

    if (s == NULL)
        return;
    snd = koki_sound_find(s->sounds, s->n_sounds, sound_name);
    if (snd == NULL) {
        nd_log(ND_LOG_KOKI, "%s: unknown music '%s'", s->name, sound_name);
        return;
    }
    koki_sound_music(&s->eng->sound, snd->file);
}

/* ------------------------------------------------------------------ *
 * The motion generators
 * ------------------------------------------------------------------ */

koki_step koki_glide(koki_frame *F, koki_sprite *s, double secs, double tx, double ty)
{
    double k;

    KOKI_BEGIN(F);
    F->x0 = s->x;
    F->y0 = s->y;
    F->tx = tx;
    F->ty = ty;
    F->t0 = koki_now(s->eng);
    for (;;) {
        /* The first step moves nothing: Python yields before it measures. */
        KOKI_YIELD(F);
        k = (secs > 0.0) ? (koki_now(s->eng) - F->t0) / secs : 1.0;
        if (k >= 1.0) {
            s->x = F->tx;
            s->y = F->ty;
            KOKI_RETURN(F);
        }
        s->x = F->x0 + (F->tx - F->x0) * k;
        s->y = F->y0 + (F->ty - F->y0) * k;
    }
    KOKI_END(F);
}

koki_step koki_glide_to(koki_frame *F, koki_sprite *s, double secs, const koki_sprite *other)
{
    /* Scratch samples the target ONCE, on the first step. Sampling it every
     * frame would turn a glide into a chase, which is a different block. */
    if (F->pc == 0) {
        F->s0 = other->x;
        F->s1 = other->y;
    }
    return koki_glide(F, s, secs, F->s0, F->s1);
}

koki_step koki_play_until_done(koki_frame *F, koki_sprite *s, const char *sound_name)
{
    const koki_sound *snd;

    KOKI_BEGIN(F);
    snd = koki_sound_find(s->sounds, s->n_sounds, sound_name);
    if (snd == NULL) {
        /* Unknown: the Python returns without yielding at all, so the caller
         * carries straight on in the same frame. */
        nd_log(ND_LOG_KOKI, "%s: unknown sound '%s'", s->name, sound_name);
        KOKI_RETURN(F);
    }
    koki_sound_sfx(&s->eng->sound, snd->file);
    F->s0 = snd->dur;
    KOKI_CALL(F, koki_wait(&F[1], s->eng, F->s0));
    KOKI_END(F);
}
