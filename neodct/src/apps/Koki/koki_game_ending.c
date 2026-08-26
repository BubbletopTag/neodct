/* koki_game_ending.c -- the trophy walk and the grade screen.
 *
 * game.py section 13, and the shortest one: Koki walks the length of the
 * lobby carrying the Reward, the screen whites out, and a grade from A to F
 * appears with its own picture and its own music.
 *
 * ============ HOW THE GRADE IS DECIDED ============
 *
 * Not by a score. By four counters, in a nest of ifs that is easier to read
 * as a list of the cases it actually produces:
 *
 *   no knockouts, no damage at all ................................. A
 *   no knockouts, some damage, more than 2 lives, healed ........... C
 *   no knockouts, some damage, more than 2 lives, never healed ..... D
 *   no knockouts, some damage, 2 lives or fewer .................... B
 *   knocked out, more than 2 lives, healed ......................... C
 *   knocked out, more than 2 lives, never healed ................... D
 *   knocked out once or twice ...................................... C
 *   knocked out more than six times ................................ F
 *   anything else .................................................. B
 *
 * "More than 2 lives" beating "knocked out" looks wrong and is the Python's;
 * ported as-is, per CODING-STANDARDS.md section 9.4.
 *
 * ============ FIVE SCREENS, ONE HANDLER ============
 *
 * Python builds these with a factory closing over a costume and a track, and
 * registers a pair against each of the five messages "a".."f". The message IS
 * the grade, so koki_script_event() plus a five-row table does the same job
 * without five near-identical bodies -- the same trick the doors use with
 * koki_script_sprite().
 */

#include <stdio.h>
#include <string.h>

#include "nd_types.h"

#include "koki_game.h"

/* ------------------------------------------------------------------ *
 * The trophy
 * ------------------------------------------------------------------ */

static koki_step s_reward(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(REWARD);
    koki_layer_front(REWARD);
    for (;;) {
        koki_goto_sprite(REWARD, ANIM);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_reward_off(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(REWARD);
    koki_hide(REWARD);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * The score screen
 * ------------------------------------------------------------------ */

static koki_step s_score(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_layer_front(SCORE);
    koki_show(SCORE);
    COSTUME(SCORE, "costume1");
    /* The one play_until_done in the whole game: 4.05 s, from the manifest. */
    PLAY_UNTIL(F, SCORE, "output (33)");
    WAIT(F, 0.6);
    if (V(KNOCKOUTS) == 0) {
        if (V(TAKEN_DAMAGE) == 0) {
            BC("a");
        } else if (V(LIVES) > 2) {
            BC(V(HAS_HEALED) > 0 ? "c" : "d");
        } else {
            BC("b");
        }
        KOKI_RETURN(F);
    }
    if (V(LIVES) > 2) {
        BC(V(HAS_HEALED) > 0 ? "c" : "d");
        KOKI_RETURN(F);
    }
    if (V(KNOCKOUTS) == 1 || V(KNOCKOUTS) == 2) {
        BC("c");
        KOKI_RETURN(F);
    }
    if (V(KNOCKOUTS) > 6) {
        BC("f");
        KOKI_RETURN(F);
    }
    BC("b");
    KOKI_END(F);
}

/* _grades: the picture and the music track for each letter. Dict order in
 * Python is insertion order, and the registration loop walks it, so this
 * table is also the registration order. */
static const struct {
    const char *grade;
    const char *costume;
    const char *track;
} GRADES[5] = {
    {"a", "costume5", "Koki A score music"},
    {"b", "costume7", "lowatrezzo"},
    {"c", "costume9", "Koki C Ending score music"},
    {"d", "costume10", "Koki D score"},
    {"f", "costume11", "Koki F score"},
};

static koki_step s_grade_show(koki_frame *F)
{
    const char *ev = koki_script_event(KE);
    size_t i;

    KOKI_BEGIN(F);
    for (i = 0u; i < ND_ARRAY_LEN(GRADES); i++) {
        if (strcmp(GRADES[i].grade, ev) == 0) {
            COSTUME(SCORE, GRADES[i].costume);
            /* "Koki D score" is 14.46 s -- under the asset builder's 15 s
             * music threshold, so it was baked as a WAV and is then used as
             * looping music. That is the one asset the aplay path cannot
             * loop; see koki_audio.c. */
            koki_sprite_music(SCORE, GRADES[i].track);
            break;
        }
    }
    KOKI_END(F);
}

static koki_step s_grade_input(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 1);
    for (;;) {
        if (KEY(ENTER)) {
            STOPOTHER(SCORE);
            koki_sound_stop_all(&KE->sound);
            BC("whitechange");
            WAIT(F, 0.05);
            BC("go to lobby");
            koki_hide(SCORE);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

void koki_register_ending(koki_engine *eng)
{
    size_t i;

    ON("ending cutscene", REWARD, s_reward);
    ON("the end", REWARD, s_reward_off);
    ON("ending score", SCORE, s_score);
    for (i = 0u; i < ND_ARRAY_LEN(GRADES); i++) {
        ON(GRADES[i].grade, SCORE, s_grade_show);
        ON(GRADES[i].grade, SCORE, s_grade_input);
    }

    ND_UNUSED(eng);
}
