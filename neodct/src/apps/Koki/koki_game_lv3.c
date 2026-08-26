/* koki_game_lv3.c -- level 3: running away from the popcorn machine.
 *
 * game.py section 11. The player's x is fixed and only jumping is left
 * (RUNenable, in koki_game_boot.c, is the physics for that); Popi chases from
 * the left, spitting projectiles, and the cannon slides past on a timer.
 * Hitting him four times ends the level -- the count is V["cannondefeats"],
 * which starts at 1, so 2 and 3 send him back for another phase and 4 is the
 * end.
 *
 * ============ THE ABYSS ============
 *
 * The one place in the game where falling kills you. ABYSS is a full-width
 * sprite along the bottom that watches PLAYER -- the invisible hitbox, not
 * the visible character -- and broadcasts "falloofie", which is a different
 * death from "oofie" and plays a different sound.
 *
 * ============ ATK, AGAIN ============
 *
 * Two uses here, and note that they go OPPOSITE WAYS: Popi's projectile
 * glide is `0.2 * ATK` (a longer glide is slower), while the level-3
 * shockwave moves `7 / ATK` per frame (a smaller step is slower). Both make
 * the attack gentler as ATK rises, which is the point.
 */

#include <stdio.h>

#include "nd_types.h"

#include "koki_game.h"

/* ------------------------------------------------------------------ *
 * Enemy 3 -- Popi
 * ------------------------------------------------------------------ */

static const char *const EN3_RUN_CYCLE[4] = {"costume7", "costume6", "costume5", "costume4"};
static const char *const EN3_HIT_CYCLE[6] = {"costume8",  "costume9",  "costume10",
                                             "costume11", "costume12", "costume4"};

static koki_step s_en3_place(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(EN3);
    koki_goto(EN3, -169, -55);
    KOKI_END(F);
}

static koki_step s_en3_hitbox_arm(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 2);
    BC("hitbox");
    KOKI_END(F);
}

static koki_step s_en3_hitbox(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(EN3, ANIM)) {
            BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_en3_rearm(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 1);
    BC("hitbox");
    KOKI_END(F);
}

static koki_step s_en3_run_start(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("enemy3 run");
    KOKI_END(F);
}

static koki_step s_en3_run(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        for (F->i = 0; F->i < 4; F->i++) {
            COSTUME(EN3, EN3_RUN_CYCLE[F->i]);
            WAIT(F, 0.01);
        }
    }
    KOKI_END(F);
}

static koki_step s_en3_begin_atk(koki_frame *F)
{
    KOKI_BEGIN(F);
    EN3->brightness = 0.0;
    V(CANNONDEFEATS) = 1;
    WAIT(F, 3);
    BC("en3atk");
    KOKI_END(F);
}

static koki_step s_en3_attack(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        for (F->i = 0; F->i < 5; F->i++) {
            koki_play(EN3, "output (24)2");
            EN3->brightness = 0.0;
            /* The wind-up: he brightens over ten steps, and the flash is the
             * tell that the shot is coming. */
            for (F->j = 0; F->j < 10; F->j++) {
                EN3->brightness += 10.0;
                WAIT(F, 0.05);
            }
            koki_play(EN3, "recording2");
            BC("shootenemyball");
            EN3->brightness -= 50.0;
            WAIT(F, 0.05);
            EN3->brightness -= 50.0;
            WAIT(F, 0.05);
            EN3->brightness = 0.0;
            SUB2(F, koki_g_flash, EN3, 5);
        }
        BC("spawncannon333");
        WAIT(F, 3);
    }
    KOKI_END(F);
}

static koki_step s_en3_damaged(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN3);
    BC("enemy3hit");
    koki_play(EN3, "output (24)");
    SUB2(F, koki_g_flash, EN3, 10);
    KOKI_END(F);
}

static koki_step s_en3_hit_anim(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < 6; F->i++) {
        COSTUME(EN3, EN3_HIT_CYCLE[F->i]);
        WAIT(F, 0.05);
    }
    BC("enemy3 run");
    WAIT(F, 1);
    BC("en3atk");
    KOKI_END(F);
}

static koki_step s_en3_phase_down(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN3);
    V(CANNONDEFEATS) += 1;
    koki_play(EN3, "output (24)");
    /* The same hit cycle, minus its last frame: he does not go back to
     * running here, he leaves. */
    for (F->i = 0; F->i < 5; F->i++) {
        COSTUME(EN3, EN3_HIT_CYCLE[F->i]);
        WAIT(F, 0.05);
    }
    koki_goto(EN3, -169, -55);
    GLIDE(F, EN3, 0.2, -293, -55);
    koki_hide(EN3);
    if (V(CANNONDEFEATS) == 2 || V(CANNONDEFEATS) == 3) {
        BC("Chase for the door");
        BC("startshockwaves333");
        BC("enemy3 run");
        BC("restoreEN3");
        koki_show(EN3);
        koki_goto(EN3, -293, -55);
        GLIDE(F, EN3, 1, -169, -55);
        WAIT(F, 1);
        BC("en3atk");
    } else if (V(CANNONDEFEATS) == 4) {
        BC("stopmusic");
        WAIT(F, 3);
        V(DOORS) = 4;
        BC("go to lobby");
    }
    KOKI_END(F);
}

static koki_step s_en3_playerdead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN3);
    KOKI_END(F);
}

static koki_step s_en3_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN3);
    koki_hide(EN3);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Popi's projectiles
 * ------------------------------------------------------------------ */

static koki_step s_eball_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(CBALL2, -122, -41);
    GLIDE(F, CBALL2, 0.2 * KOKI_ATK, 250, -41);
    koki_hide(CBALL2);
    KOKI_END(F);
}

static koki_step s_eball_show(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(CBALL2);
    koki_layer_back(CBALL2);
    SUB2(F, koki_g_flash, CBALL2, 2);
    KOKI_END(F);
}

static koki_step s_eball_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(CBALL2, ANIM)) {
            BC("take damage");
            koki_hide(CBALL2);
            STOPOTHER(CBALL2);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_eball_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(CBALL2);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Cannon 3
 * ------------------------------------------------------------------ */

static koki_step s_cannon3_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(CANNON3);
    SUB2(F, koki_g_flash, CANNON3, 2);
    KOKI_END(F);
}

static koki_step s_cannon3_arm(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON3, "recording1");
    for (;;) {
        if (KEY(X) && TOUCHING(CANNON3, ANIM)) {
            BC("cannonball3");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_cannon3_slide(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(CANNON3, 262, -72);
    GLIDE(F, CANNON3, 2, -263, -72);
    STOPOTHER(CANNON3);
    koki_hide(CANNON3);
    KOKI_END(F);
}

static koki_step s_cannon3_fired(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON3, "explosion meme");
    SUB2(F, koki_g_flash, CANNON3, 2);
    WAIT(F, 0.5);
    koki_hide(CANNON3);
    KOKI_END(F);
}

static koki_step s_cannon3_hide(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(CANNON3);
    koki_hide(CANNON3);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Level 3's shockwaves, and the abyss
 * ------------------------------------------------------------------ */

static koki_step s_sw3_random(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        /* W(R(5, 30)): the wait is an INTEGER number of seconds drawn from
         * the private MT19937, so the sequence is reproducible run to run. */
        F->s0 = (double)RND(5, 30);
        WAIT(F, F->s0);
        BC("shock3");
    }
    KOKI_END(F);
}

static koki_step s_sw3_move(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(SW3);
    koki_goto(SW3, 241, -75);
    while (SW3->x > -241.0) {
        SW3->x -= 7.0 / KOKI_ATK;
        YIELD(F);
    }
    STOPOTHER(SW3);
    koki_hide(SW3);
    KOKI_END(F);
}

static koki_step s_sw3_hit(koki_frame *F)
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

static koki_step s_sw3_off(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(SW3);
    koki_hide(SW3);
    KOKI_END(F);
}

static koki_step s_abyss(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_layer_back(ABYSS);
    koki_show(ABYSS);
    for (;;) {
        /* PLAYER, not ANIM: the hitbox is what the physics moves, and the
         * visible character lags it by a frame. */
        if (TOUCHING(ABYSS, PLAYER)) {
            BC("falloofie");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Enemy 3's health bar
 * ------------------------------------------------------------------ */

static koki_step s_e3stat_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("restorecanon");
    WAIT(F, 0.05);
    koki_show(E3STAT);
    COSTUME(E3STAT, "costume3");
    koki_goto(E3STAT, 308, -144);
    GLIDE(F, E3STAT, 0.3, 150, -144);
    KOKI_END(F);
}

static koki_step s_e3stat_bounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    GLIDE(F, E3STAT, 0.05, 150, -139);
    GLIDE(F, E3STAT, 0.1, 150, -144);
    KOKI_END(F);
}

static koki_step s_e3stat_dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_next_costume(E3STAT);
    WAIT(F, 0.05);
    if (IS_COSTUME(E3STAT, "Oof2"))
        BC("enemy 3 oof");
    KOKI_END(F);
}

static koki_step s_e3stat_restore(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(E3STAT, "costume3");
    GLIDE(F, E3STAT, 0.05, 150, -139);
    GLIDE(F, E3STAT, 0.1, 150, -144);
    KOKI_END(F);
}

static koki_step s_e3stat_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(E3STAT, 150, -144);
    GLIDE(F, E3STAT, 0.3, 308, -144);
    koki_hide(E3STAT);
    KOKI_END(F);
}

static koki_step s_e3stat_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(E3STAT);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

void koki_register_lv3(koki_engine *eng)
{
    ON("startlv3", EN3, s_en3_place);
    ON("startlv3", EN3, s_en3_hitbox_arm);
    ON("hitbox", EN3, s_en3_hitbox);
    ON("take damage", EN3, s_en3_rearm);
    ON("startlv3", EN3, s_en3_run_start);
    ON("enemy3 run", EN3, s_en3_run);
    ON("startlv3", EN3, s_en3_begin_atk);
    ON("en3atk", EN3, s_en3_attack);
    ON("enemy 3 damage", EN3, s_en3_damaged);
    ON("enemy3hit", EN3, s_en3_hit_anim);
    ON("enemy 3 oof", EN3, s_en3_phase_down);
    ON("oofie", EN3, s_en3_playerdead);
    ON("go to lobby", EN3, s_en3_lobby);

    ON("shootenemyball", CBALL2, s_eball_fly);
    ON("shootenemyball", CBALL2, s_eball_show);
    ON("shootenemyball", CBALL2, s_eball_hit);
    ON("go to lobby", CBALL2, s_eball_lobby);

    ON("spawncannon333", CANNON3, s_cannon3_flash);
    ON("spawncannon333", CANNON3, s_cannon3_arm);
    ON("spawncannon333", CANNON3, s_cannon3_slide);
    ON("cannonball3", CANNON3, s_cannon3_fired);
    ON("hidecannon3", CANNON3, s_cannon3_hide);
    ON("go to lobby", CANNON3, s_cannon3_hide);

    ON("startshockwaves333", SW3, s_sw3_random);
    ON("shock3", SW3, s_sw3_move);
    ON("shock3", SW3, s_sw3_hit);
    ON("enemy 3 oof", SW3, s_sw3_off);

    ON("level3", ABYSS, s_abyss);

    ON("startlv3", E3STAT, s_e3stat_in);
    ON("enemy 3 damage", E3STAT, s_e3stat_bounce);
    ON("enemy 3 damage", E3STAT, s_e3stat_dmg);
    ON("restoreEN3", E3STAT, s_e3stat_restore);
    ON("go to lobby", E3STAT, s_e3stat_out);
    ON("game over", E3STAT, s_e3stat_go);

    ND_UNUSED(eng);
}
