/* koki_game_final.c -- the final boss: Riby, an evil Koki.
 *
 * game.py section 12, and the most tangled part of the game: Riby has six
 * attack patterns that chain into one another by broadcast, and which one
 * comes next is drawn from the private MT19937 at the moment he is hit.
 *
 * ============ RibyDanger ============
 *
 * One variable decides whether touching Riby hurts YOU or hurts HIM.
 * _danger(1) means he is mid-attack and contact is damage; _danger(0) means
 * he is recovering and contact plus x is a hit. Every call also rebroadcasts
 * "playerfinalenable", which RESTARTS the touch-watching script from the top
 * -- that restart is the whole point, because a script already past its
 * `if` would otherwise keep the old reading for a frame.
 *
 * ============ THE SOFT-LOCK FIX IS game.py's ============
 *
 * _riby_jump_attacks ends by broadcasting itself. Its comment explains why:
 * every other pattern chains onward, and without the self-broadcast the
 * fight dies quietly -- Riby inert, the player able neither to hurt him nor
 * to be hurt -- if every pound window was missed. Kept.
 *
 * ============ WHERE THE RANDOM DRAWS HAPPEN ============
 *
 * Python evaluates `range(R(2, 6))` ONCE, when the loop starts. KOKI_CALL
 * re-evaluates its arguments on every resume, so a literal transcription of
 * `yield from _riby_cannon_volley(R(1, 3))` would redraw the generator every
 * frame and desynchronise the whole RNG stream. Every such count is drawn
 * into the frame first and the frame value is passed. This is the trap
 * README-PORT.md lists as cost 4, and this file is where it actually bites.
 */

#include <stdio.h>
#include <string.h>

#include "nd_types.h"

#include "koki_game.h"

/* ------------------------------------------------------------------ *
 * Riby's shared pieces
 * ------------------------------------------------------------------ */

static const char *const RIBY_RUN_CYCLE[4] = {"costume6", "costume8", "costume4", "costume9"};

static koki_step riby_run_anim(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        for (F->i = 0; F->i < 4; F->i++) {
            COSTUME(RIBY, RIBY_RUN_CYCLE[F->i]);
            WAIT(F, 0.01);
        }
    }
    KOKI_END(F);
}

static koki_step riby_laugh(koki_frame *F, int32_t times)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < times; F->i++) {
        COSTUME(RIBY, "costume16");
        WAIT(F, 0.05);
        COSTUME(RIBY, "costume15");
        WAIT(F, 0.05);
    }
    KOKI_END(F);
}

/* Not a protothread: it sets one variable and rebroadcasts, both instant. */
static void danger(double v)
{
    V(RIBYDANGER) = v;
    BC("playerfinalenable");
}

/* Aim the evil cannon at Koki and fire, `times` times. */
static koki_step riby_cannon_volley(koki_frame *F, int32_t times)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < times; F->i++) {
        danger(1);
        COSTUME(RIBY, "costume2");
        BC("aim canon evil");
        for (F->j = 0; F->j < 10; F->j++) {
            koki_point(RIBY, ANIM->x > RIBY->x ? 90 : -90);
            WAIT(F, 0.05);
        }
        BC("shootEVILcanonball");
    }
    BC("hideEvilCanon");
    danger(0);
    WAIT(F, 2);
    BC("jumpatkriby");
    KOKI_END(F);
}

/* Back-and-forth floor dashes, then settle at the centre. */
static koki_step riby_dash_sweeps(koki_frame *F)
{
    KOKI_BEGIN(F);
    /* range(R(2, 6)) is drawn ONCE. */
    F->s0 = (double)RND(2, 6);
    for (F->i = 0; F->i < (int32_t)F->s0; F->i++) {
        KOKI_CALL(F, riby_laugh(&F[1], 7));
        danger(1);
        koki_point(RIBY, -90);
        koki_play(RIBY, "slide");
        COSTUME(RIBY, "costume11");
        GLIDE(F, RIBY, 0.2, -200, -75);
        KOKI_CALL(F, riby_laugh(&F[1], 7));
        danger(1);
        koki_point(RIBY, 90);
        koki_play(RIBY, "slide");
        COSTUME(RIBY, "costume11");
        GLIDE(F, RIBY, 0.2, 200, -75);
    }
    COSTUME(RIBY, "costume13");
    WAIT(F, 1);
    danger(0);
    BC("RibyRun");
    koki_point(RIBY, -90);
    GLIDE(F, RIBY, 1, 0, -75);
    STOPOTHER(RIBY);
    COSTUME(RIBY, "costume2");
    danger(0);
    WAIT(F, 1);
    BC("jumpatkriby");
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Riby
 * ------------------------------------------------------------------ */

static koki_step s_riby_cutscene(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(RIBY);
    koki_point(RIBY, 90);
    koki_goto(RIBY, 198, -75);
    COSTUME(RIBY, "door");
    WAIT(F, 0.1);
    COSTUME(RIBY, "costume2");
    WAIT(F, 1);
    COSTUME(RIBY, "costume3");
    WAIT(F, 0.05);
    COSTUME(RIBY, "costume13");
    WAIT(F, 0.7);
    COSTUME(RIBY, "costume3");
    WAIT(F, 0.05);
    COSTUME(RIBY, "costume14");
    WAIT(F, 0.05);
    KOKI_CALL(F, riby_laugh(&F[1], 10));
    koki_point(RIBY, -90);
    COSTUME(RIBY, "costume2");
    WAIT(F, 0.2);
    COSTUME(RIBY, "costume3");
    WAIT(F, 0.05);
    COSTUME(RIBY, "costume4");
    WAIT(F, 0.05);
    koki_goto(RIBY, 198, -75);
    GLIDE(F, RIBY, 0.05, 176, -51);
    BC("cutscenehit");
    GLIDE(F, RIBY, 0.05, 143, -38);
    GLIDE(F, RIBY, 0.05, 126, -75);
    COSTUME(RIBY, "costume11");
    WAIT(F, 0.05);
    COSTUME(RIBY, "costume2");
    WAIT(F, 1);
    BC("startfinallevel");
    BC("PlayerEnable");
    BC("playerfinalenable");
    KOKI_END(F);
}

static koki_step s_riby_fight_start(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(RIBY);
    koki_goto(RIBY, 126, -75);
    koki_point(RIBY, -90);
    COSTUME(RIBY, "costume2");
    WAIT(F, 1);
    BC("phase1riby");
    KOKI_END(F);
}

static koki_step s_riby_run(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB(F, riby_run_anim);
    KOKI_END(F);
}

static koki_step s_riby_phase1(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(RIBYDANGER) = 1;
    BC("RibyRun");
    BC("playerfinalenable");
    koki_goto(RIBY, 126, -75);
    GLIDE(F, RIBY, 0.5, 0, -75);
    STOPOTHER(RIBY);
    BC("playerfinalenable");
    COSTUME(RIBY, "costume10");
    GLIDE(F, RIBY, 0.3, -99, 30);
    GLIDE(F, RIBY, 0.1, -146, -75);
    BC("shockwaveriby");
    koki_play(RIBY, "groundpound");
    COSTUME(RIBY, "costume11");
    GLIDE(F, RIBY, 0.1, -151, -75);
    danger(0);
    KOKI_CALL(F, riby_laugh(&F[1], 20));
    danger(1);
    koki_point(RIBY, 90);
    BC("RibyRun");
    GLIDE(F, RIBY, 0.5, 126, -75);
    STOPOTHER(RIBY);
    BC("playerfinalenable");
    COSTUME(RIBY, "costume10");
    koki_goto(RIBY, 126, -75);
    GLIDE(F, RIBY, 0.1, 126, 30);
    GLIDE(F, RIBY, 0.1, 126, -75);
    BC("shockwaveriby");
    koki_play(RIBY, "groundpound");
    COSTUME(RIBY, "costume11");
    WAIT(F, 0.1);
    danger(0);
    KOKI_CALL(F, riby_laugh(&F[1], 20));
    danger(1);
    koki_point(RIBY, -90);
    BC("RibyRun");
    GLIDE(F, RIBY, 0.5, 0, -75);
    STOPOTHER(RIBY);
    WAIT(F, 1);
    KOKI_CALL(F, riby_cannon_volley(&F[1], 1));
    KOKI_END(F);
}

static koki_step s_riby_touch(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(RIBY, ANIM)) {
            if (V(RIBYDANGER) == 1) {
                BC("take damage");
                KOKI_RETURN(F);
            }
            if (V(RIBYDANGER) == 0 && KEY(X)) {
                BC("enemy4 damage");
                KOKI_RETURN(F);
            }
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_riby_damaged(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(DAMAGEWAY4) = RND(1, 3);
    STOPOTHER(RIBY);
    koki_play(RIBY, "hit2");
    COSTUME(RIBY, "OOF");
    SUB2(F, koki_g_flash, RIBY, 10);
    RIBY->ghost = 0.0;
    danger(1);
    koki_point(RIBY, -90);
    BC("RibyRun");
    GLIDE(F, RIBY, 0.5, 0, -75);
    STOPOTHER(RIBY);
    danger(1);
    COSTUME(RIBY, "costume2");
    WAIT(F, 0.7);
    COSTUME(RIBY, "costume13");
    WAIT(F, 0.3);
    COSTUME(RIBY, "costume2");
    WAIT(F, 0.5);
    /* damageway4 was drawn above -- unless the health bar has since pushed
     * it to 4, which is how the Enemy-1 possession phase is reached. */
    if (V(DAMAGEWAY4) == 1) {
        F->s0 = (double)RND(1, 3); /* drawn once, not once per resume */
        KOKI_CALL(F, riby_cannon_volley(&F[1], (int32_t)F->s0));
    } else if (V(DAMAGEWAY4) == 2) {
        BC("jumpatkriby");
    } else if (V(DAMAGEWAY4) == 3) {
        BC("RibyRun");
        koki_point(RIBY, 90);
        GLIDE(F, RIBY, 0.5, 200, -75);
        STOPOTHER(RIBY);
        danger(1);
        SUB(F, riby_dash_sweeps);
    } else if (V(DAMAGEWAY4) == 4) {
        STOPOTHER(RIBY);
        danger(1);
        koki_point(RIBY, 90);
        COSTUME(RIBY, "costume2");
        WAIT(F, 0.3);
        BC("RibyRun");
        GLIDE(F, RIBY, 0.6, -56, -75);
        STOPOTHER(RIBY);
        COSTUME(RIBY, "costume2");
        WAIT(F, 0.3);
        BC("en1final");
        WAIT(F, 1);
        COSTUME(RIBY, "costume10");
        GLIDE(F, RIBY, 0.1, -1, -4);
        GLIDE(F, RIBY, 0.1, 35, -22);
        GLIDE(F, RIBY, 0.1, 83, -72);
        koki_hide(RIBY);
        STOPOTHER(RIBY);
        BC("possessen1");
    }
    KOKI_END(F);
}

static koki_step s_riby_jump_attacks(koki_frame *F)
{
    KOKI_BEGIN(F);
    F->s1 = (double)RND(3, 6); /* range(R(3, 6)), drawn once */
    for (F->i = 0; F->i < (int32_t)F->s1; F->i++) {
        COSTUME(RIBY, "costume10");
        koki_play(RIBY, "jump");
        koki_point(RIBY, RIBY->x < 0.0 ? 90 : -90);
        GLIDE(F, RIBY, 0.2, 0, 33);
        COSTUME(RIBY, "costume17");
        WAIT(F, 0.1);
        koki_point(RIBY, ANIM->x > RIBY->x ? 90 : -90);
        WAIT(F, 0.2);
        danger(1);
        GLIDE_TO(F, RIBY, 0.5 * KOKI_ATK, ANIM);
        koki_play(RIBY, "groundpound");
        F->s0 = RIBY->x; /* glide(0.01, RIBY.x, -75) */
        GLIDE(F, RIBY, 0.01, F->s0, -75);
        danger(0);
        BC("shockwaveriby");
        COSTUME(RIBY, "costume11");
        WAIT(F, 0.05);
        COSTUME(RIBY, "costume2");
        WAIT(F, 2);
        danger(1);
    }
    /* See the header comment: without this the fight soft-locks. */
    WAIT(F, 1);
    BC("jumpatkriby");
    KOKI_END(F);
}

static koki_step s_riby_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(RIBY);
    koki_goto(RIBY, EN1->x, -75);
    danger(0);
    KOKI_CALL(F, riby_laugh(&F[1], 15));
    danger(1);
    BC("RibyRun");
    koki_point(RIBY, 90);
    GLIDE(F, RIBY, 0.5, 200, -75);
    STOPOTHER(RIBY);
    danger(1);
    SUB(F, riby_dash_sweeps);
    KOKI_END(F);
}

static koki_step s_riby_dead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(RIBY);
    koki_play(RIBY, "hit");
    COSTUME(RIBY, "OOF");
    koki_clear_fx(RIBY);
    WAIT(F, 1);
    for (F->i = 0; F->i < 7; F->i++) {
        RIBY->y += 5.0;
        YIELD(F);
    }
    F->s0 = RIBY->x;
    GLIDE(F, RIBY, 0.7, F->s0, -204);
    koki_hide(RIBY);
    BC("door4openagain");
    KOKI_END(F);
}

static koki_step s_riby_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(RIBY);
    koki_hide(RIBY);
    KOKI_END(F);
}

static koki_step s_riby_playerdead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(RIBY);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * The evil cannon and its two balls
 * ------------------------------------------------------------------ */

static koki_step s_evilc_aim(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(EVILC, "recording1");
    koki_layer_front(EVILC);
    koki_clear_fx(EVILC);
    koki_show(EVILC);
    koki_goto_sprite(EVILC, RIBY);
    EVILC->y = -74.0;
    for (F->i = 0; F->i < 30; F->i++) {
        koki_point(EVILC, ANIM->x > EVILC->x ? 90 : -90);
        WAIT(F, 0.05);
    }
    V(EVILCANONBALLDIRECTION) = (EVILC->direction > 0.0) ? 90 : -90;
    KOKI_END(F);
}

static koki_step s_evilc_fire(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(EVILC, "explosion meme");
    SUB2(F, koki_g_flash, EVILC, 2);
    koki_clear_fx(EVILC);
    KOKI_END(F);
}

static koki_step s_evilc_hide(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EVILC);
    koki_hide(EVILC);
    KOKI_END(F);
}

/* _mk_evil_ball(spr, step, edge): CBALL3 goes right at 20/ATK to x = 240,
 * CBALL4 goes left at -20/ATK to x = -240. */
static koki_step evil_ball_fly(koki_frame *F, koki_sprite *spr, double step, double edge)
{
    KOKI_BEGIN(F);
    koki_goto_sprite(spr, EVILC);
    koki_show(spr);
    if (step > 0.0) {
        while (spr->x < edge) {
            spr->x += step;
            YIELD(F);
        }
    } else {
        while (spr->x > edge) {
            spr->x += step;
            YIELD(F);
        }
    }
    koki_hide(spr);
    STOPOTHER(spr);
    KOKI_END(F);
}

static koki_step s_cball3_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, evil_ball_fly(&F[1], CBALL3, 20.0 / KOKI_ATK, 240.0));
    KOKI_END(F);
}

static koki_step s_cball4_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, evil_ball_fly(&F[1], CBALL4, -20.0 / KOKI_ATK, -240.0));
    KOKI_END(F);
}

static koki_step s_evil_ball_show(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(koki_script_sprite(KE));
    koki_layer_front(koki_script_sprite(KE));
    SUB2(F, koki_g_flash, koki_script_sprite(KE), 2);
    KOKI_END(F);
}

static koki_step s_evil_ball_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(koki_script_sprite(KE), ANIM)) {
            BC("take damage");
            koki_hide(koki_script_sprite(KE));
            STOPOTHER(koki_script_sprite(KE));
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_evil_ball_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(koki_script_sprite(KE));
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Riby's shockwaves -- two damaging, one healing
 * ------------------------------------------------------------------ */

static koki_step s_rsw_left(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(SW3);
    koki_goto_sprite(SW3, RIBY);
    while (SW3->x > -241.0) {
        SW3->x -= 10.0 / KOKI_ATK;
        YIELD(F);
    }
    STOPOTHER(SW3);
    koki_hide(SW3);
    KOKI_END(F);
}

static koki_step s_rsw_left_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(SW3, ANIM)) {
            BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_rsw_right(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(SW4);
    koki_goto_sprite(SW4, RIBY);
    while (SW4->x < 241.0) {
        SW4->x += 10.0 / KOKI_ATK;
        YIELD(F);
    }
    STOPOTHER(SW4);
    koki_hide(SW4);
    KOKI_END(F);
}

static koki_step s_rsw_right_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(SW4, ANIM)) {
            BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* The heal wave: same shape, but touching it restores a costume of health
 * instead of taking one, and it moves at a flat 10 -- ATK does not slow the
 * thing that helps you. */
static koki_step s_rsw_heal(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 0.7);
    V(HEALWAVEDIRECTION) = RND(1, 2);
    koki_show(SW5);
    koki_goto_sprite(SW5, RIBY);
    if (V(HEALWAVEDIRECTION) == 1) {
        while (SW5->x < 241.0) {
            SW5->x += 10.0;
            YIELD(F);
        }
    } else {
        while (SW5->x > -241.0) {
            SW5->x -= 10.0;
            YIELD(F);
        }
    }
    STOPOTHER(SW5);
    koki_hide(SW5);
    KOKI_END(F);
}

static koki_step s_rsw_heal_touch(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(SW5, ANIM)) {
            BC("partialrestore");
            koki_play(SW5, "pop");
            koki_hide(SW5);
            STOPOTHER(SW5);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Riby's health bar
 * ------------------------------------------------------------------ */

static koki_step s_e4stat_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("restorecanon");
    WAIT(F, 0.05);
    koki_show(E4STAT);
    COSTUME(E4STAT, "costume1");
    koki_goto(E4STAT, 308, -144);
    GLIDE(F, E4STAT, 0.3, 150, -144);
    KOKI_END(F);
}

static koki_step s_e4stat_bounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    GLIDE(F, E4STAT, 0.05, 150, -139);
    GLIDE(F, E4STAT, 0.1, 150, -144);
    KOKI_END(F);
}

static koki_step s_e4stat_dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_next_costume(E4STAT);
    WAIT(F, 0.05);
    /* Three costumes in the middle of the bar force damageway4 to 4, which
     * is the ONLY route into the Enemy-1 possession phase -- _riby_damaged
     * only ever draws 1..3 for itself. */
    if (IS_COSTUME(E4STAT, "costume5") || IS_COSTUME(E4STAT, "costume6") ||
        IS_COSTUME(E4STAT, "costume7"))
        V(DAMAGEWAY4) = 4;
    if (IS_COSTUME(E4STAT, "Oof2"))
        BC("enemy 4 oof");
    KOKI_END(F);
}

static koki_step s_e4stat_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(E4STAT, 150, -144);
    GLIDE(F, E4STAT, 0.3, 308, -144);
    koki_hide(E4STAT);
    KOKI_END(F);
}

static koki_step s_e4stat_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(E4STAT);
    KOKI_END(F);
}

static koki_step s_e4stat_end(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(E4STAT);
    koki_hide(E4STAT);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

void koki_register_final(koki_engine *eng)
{
    ON("final cutscene", RIBY, s_riby_cutscene);
    ON("startfinallevel", RIBY, s_riby_fight_start);
    ON("RibyRun", RIBY, s_riby_run);
    ON("phase1riby", RIBY, s_riby_phase1);
    ON("playerfinalenable", RIBY, s_riby_touch);
    ON("enemy4 damage", RIBY, s_riby_damaged);
    ON("jumpatkriby", RIBY, s_riby_jump_attacks);
    ON("RibyOUT", RIBY, s_riby_out);
    ON("enemy 4 oof", RIBY, s_riby_dead);
    ON("go to lobby", RIBY, s_riby_lobby);
    ON("oofie", RIBY, s_riby_playerdead);

    ON("aim canon evil", EVILC, s_evilc_aim);
    ON("shootEVILcanonball", EVILC, s_evilc_fire);
    ON("hideEvilCanon", EVILC, s_evilc_hide);
    ON("go to lobby", EVILC, s_evilc_hide);

    /* Per sprite, all four in one loop iteration -- so CBALL3's four come
     * before any of CBALL4's, which is the Python's registration order. */
    ON("shootEVILcanonball", CBALL3, s_cball3_fly);
    ON("shootEVILcanonball", CBALL3, s_evil_ball_show);
    ON("shootEVILcanonball", CBALL3, s_evil_ball_hit);
    ON("go to lobby", CBALL3, s_evil_ball_lobby);
    ON("shootEVILcanonball", CBALL4, s_cball4_fly);
    ON("shootEVILcanonball", CBALL4, s_evil_ball_show);
    ON("shootEVILcanonball", CBALL4, s_evil_ball_hit);
    ON("go to lobby", CBALL4, s_evil_ball_lobby);

    ON("shockwaveriby", SW3, s_rsw_left);
    ON("shockwaveriby", SW3, s_rsw_left_hit);
    ON("shockwaveriby", SW4, s_rsw_right);
    ON("shockwaveriby", SW4, s_rsw_right_hit);
    ON("shockwaveriby", SW5, s_rsw_heal);
    ON("shockwaveriby", SW5, s_rsw_heal_touch);

    ON("startfinallevel", E4STAT, s_e4stat_in);
    ON("enemy4 damage", E4STAT, s_e4stat_bounce);
    ON("enemy4 damage", E4STAT, s_e4stat_dmg);
    ON("go to lobby", E4STAT, s_e4stat_out);
    ON("game over", E4STAT, s_e4stat_go);
    ON("ending cutscene", E4STAT, s_e4stat_end);

    ND_UNUSED(eng);
}
