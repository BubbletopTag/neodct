/* koki_game_lv1.c -- level 1: the bomb-throwing blob, and the cannon.
 *
 * game.py section 9. Also the home of three sprites the LATER levels borrow:
 * the single Cannon ball (which serves levels 1, 2 and 3 under three
 * different messages), the QuickPress prompt, and Enemy 1 himself, who comes
 * back possessed during the final fight.
 *
 * ============ THE FIGHT, IN ONE PARAGRAPH ============
 *
 * Enemy 1 idles five times, throws a volley of three shockwave pairs, then
 * calls "spawncanon". The cannon appears on the platform; standing on it and
 * pressing x fires the ball, which glides at the boss and takes two costumes
 * off his health bar. He answers with a pounce: "quick tap" puts the
 * QuickPress prompt on screen, and pressing x during it dashes the player
 * clear -- otherwise the pounce lands. Fourteen costume steps later the bar
 * reaches "Oof2" and he dies.
 *
 * ============ ATK ============
 *
 * The pounce is the one attack the tuning knob touches: both of its glides
 * are 0.3 * ATK, so the default 1.35 makes the lunge 35% slower and the
 * quick-press window correspondingly wider. game.py records this as a
 * sanctioned deviation from the 1:1 port rather than a port artefact.
 */

#include <stdio.h>

#include "nd_types.h"

#include "koki_game.h"

/* ------------------------------------------------------------------ *
 * Enemy 1's two shared sequences
 * ------------------------------------------------------------------ */

static koki_step en1_idle_5(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < 5; F->i++) {
        COSTUME(EN1, "costume1");
        WAIT(F, 0.3);
        COSTUME(EN1, "costume2");
        WAIT(F, 0.3);
    }
    KOKI_END(F);
}

static koki_step en1_shockwave_volley(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < 3; F->i++) {
        COSTUME(EN1, "costume3");
        WAIT(F, 0.05);
        COSTUME(EN1, "costume4");
        WAIT(F, 0.05);
        COSTUME(EN1, "costume5");
        WAIT(F, 0.05);
        BC("shockwave");
        for (F->j = 0; F->j < 2; F->j++) {
            COSTUME(EN1, "costume10");
            WAIT(F, 0.05);
            koki_play(EN1, "explbomb");
            COSTUME(EN1, "costume6");
            WAIT(F, 0.05);
            COSTUME(EN1, "costume5");
            WAIT(F, 0.2);
        }
        COSTUME(EN1, "costume3");
        WAIT(F, 0.05);
        COSTUME(EN1, "costume2");
        WAIT(F, 0.05);
        COSTUME(EN1, "costume1");
        WAIT(F, 0.1);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Enemy 1
 * ------------------------------------------------------------------ */

static koki_step s_en1_start(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(EN1, 173, -48);
    koki_show(EN1);
    koki_clear_fx(EN1);
    SUB(F, en1_idle_5);
    SUB(F, en1_shockwave_volley);
    BC("enemy1 idle");
    WAIT(F, 0.2);
    BC("spawncanon");
    KOKI_END(F);
}

static koki_step s_en1_bell(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 2);
    BC("boxing bell");
    BC("hitbox");
    KOKI_END(F);
}

static koki_step s_en1_hitbox(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(EN1, ANIM)) {
            BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_en1_rearm(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 1);
    BC("hitbox");
    KOKI_END(F);
}

static koki_step s_en1_idle(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        COSTUME(EN1, "costume1");
        WAIT(F, 0.3);
        COSTUME(EN1, "costume2");
        WAIT(F, 0.3);
    }
    KOKI_END(F);
}

static koki_step s_en1_damaged(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN1);
    koki_play(EN1, "output (24)");
    COSTUME(EN1, "OOF");
    WAIT(F, 0.5);
    COSTUME(EN1, "costume1");
    WAIT(F, 0.5);
    BC("enemy1 pounce");
    KOKI_END(F);
}

static koki_step s_en1_pounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(EN1, "costume7");
    koki_play(EN1, "output (24)2");
    BC("quickhitbox");
    BC("quick tap");
    GLIDE(F, EN1, 0.3 * KOKI_ATK, 0, 94);
    GLIDE_TO(F, EN1, 0.3 * KOKI_ATK, ANIM);
    koki_play(EN1, "explbomb");
    COSTUME(EN1, "costume8");
    EN1->y = -48.0;
    WAIT(F, 0.05);
    COSTUME(EN1, "costume9");
    WAIT(F, 0.2);
    STOPOTHER(EN1);
    COSTUME(EN1, "costume1");
    GLIDE(F, EN1, 0.3, 173, -48);
    BC("hitbox");
    BC("phaseagain");
    KOKI_END(F);
}

static koki_step s_en1_phaseagain(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB(F, en1_idle_5);
    SUB(F, en1_shockwave_volley);
    BC("enemy1 idle");
    WAIT(F, 0.2);
    BC("spawncanon");
    KOKI_END(F);
}

static koki_step s_en1_quickhitbox(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(EN1, ANIM)) {
            if (KEY(X))
                BC("dodge");
            else
                BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_en1_playerdead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN1);
    KOKI_END(F);
}

static koki_step s_en1_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN1);
    koki_hide(EN1);
    KOKI_END(F);
}

static koki_step s_en1_dead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN1);
    koki_play(EN1, "explbomb2");
    COSTUME(EN1, "OOF");
    for (F->i = 0; F->i < 10; F->i++) {
        EN1->y += 5.0;
        YIELD(F);
    }
    COSTUME(EN1, "OOF2");
    /* glide(0.6, EN1.x, -48): x is read once, when the glide starts. */
    F->s0 = EN1->x;
    GLIDE(F, EN1, 0.6, F->s0, -48);
    EN1->ghost = 0.0;
    for (F->i = 0; F->i < 20; F->i++) {
        EN1->ghost += 5.0;
        YIELD(F);
    }
    koki_hide(EN1);
    WAIT(F, 1);
    BC("whitechange");
    WAIT(F, 0.05);
    BC("unlock door2");
    BC("go to lobby");
    KOKI_END(F);
}

/* -- the final-boss possession phase: Enemy 1 comes back, dark ------------- */

static koki_step s_en1_final_entry(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(EN1);
    koki_clear_fx(EN1);
    koki_goto(EN1, 265, 109);
    GLIDE(F, EN1, 0.1, 84, -48);
    koki_play(EN1, "explbomb");
    COSTUME(EN1, "costume8");
    WAIT(F, 0.05);
    COSTUME(EN1, "costume9");
    WAIT(F, 0.2);
    STOPOTHER(EN1);
    COSTUME(EN1, "costume1");
    SUB(F, en1_idle_5);
    KOKI_END(F);
}

static koki_step s_en1_possessed(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN1);
    koki_play(EN1, "output (24)");
    COSTUME(EN1, "OOF");
    WAIT(F, 0.5);
    COSTUME(EN1, "costume1");
    /* The original tints him purple with a colour effect. The port has only
     * ghost and brightness, so a brightness dip stands in -- game.py's own
     * deviation, and the reason he stays at -50 for the rest of the phase. */
    EN1->brightness = 0.0;
    for (F->i = 0; F->i < 6; F->i++) {
        EN1->brightness -= 25.0;
        WAIT(F, 0.05);
    }
    for (F->i = 0; F->i < 4; F->i++) {
        EN1->brightness += 25.0;
        YIELD(F);
    }
    EN1->brightness = -50.0;
    SUB(F, en1_idle_5);
    for (;;) {
        COSTUME(EN1, "costume7");
        koki_play(EN1, "output (24)2");
        BC("quick tap");
        BC("quickhitbox");
        GLIDE(F, EN1, 0.3 * KOKI_ATK, 0, 94);
        GLIDE_TO(F, EN1, 0.3 * KOKI_ATK, ANIM);
        koki_play(EN1, "explbomb");
        COSTUME(EN1, "costume8");
        EN1->y = -48.0;
        WAIT(F, 0.05);
        COSTUME(EN1, "costume9");
        WAIT(F, 0.2);
        COSTUME(EN1, "costume1");
        GLIDE(F, EN1, 0.3, 173, -48);
        BC("activehitonen1");
        SUB(F, en1_idle_5);
        SUB(F, en1_shockwave_volley);
        BC("activehitonen1");
        SUB(F, en1_idle_5);
    }
    KOKI_END(F);
}

static koki_step s_en1_activehit(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(EN1, ANIM) && KEY(X)) {
            STOPOTHER(EN1);
            koki_play(EN1, "explbomb2");
            COSTUME(EN1, "OOF");
            for (F->i = 0; F->i < 10; F->i++) {
                EN1->y += 5.0;
                YIELD(F);
            }
            COSTUME(EN1, "OOF2");
            F->s0 = EN1->x;
            GLIDE(F, EN1, 0.6, F->s0, -48);
            BC("RibyOUT");
            EN1->ghost = 0.0;
            EN1->brightness = 0.0;
            for (F->i = 0; F->i < 20; F->i++) {
                EN1->ghost += 5.0;
                YIELD(F);
            }
            koki_hide(EN1);
            WAIT(F, 1);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Shockwaves
 * ------------------------------------------------------------------ */

/* _mk_shockwave(spr, delay): two scripts per wave, one that moves it and one
 * that watches for a hit, both offset by the same delay. The second wave
 * trails the first by 0.2 s -- and `if delay:` means the first wave does not
 * wait AT ALL rather than waiting zero, which at 30 fps is a whole frame of
 * difference. */
static koki_step sw_move(koki_frame *F, koki_sprite *spr, double delay)
{
    KOKI_BEGIN(F);
    if (delay != 0.0)
        WAIT(F, delay);
    koki_show(spr);
    koki_goto_sprite(spr, EN1);
    GLIDE(F, spr, 1.0 * KOKI_ATK, -241, -45);
    STOPOTHER(spr);
    koki_hide(spr);
    KOKI_END(F);
}

static koki_step sw_hit(koki_frame *F, koki_sprite *spr, double delay)
{
    KOKI_BEGIN(F);
    if (delay != 0.0)
        WAIT(F, delay);
    for (;;) {
        if (TOUCHING(spr, ANIM)) {
            BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_sw1_move(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, sw_move(&F[1], SW1, 0.0));
    KOKI_END(F);
}

static koki_step s_sw1_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, sw_hit(&F[1], SW1, 0.0));
    KOKI_END(F);
}

static koki_step s_sw2_move(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, sw_move(&F[1], SW2, 0.2));
    KOKI_END(F);
}

static koki_step s_sw2_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, sw_hit(&F[1], SW2, 0.2));
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Enemy 1's health bar
 * ------------------------------------------------------------------ */

static koki_step s_e1stat_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("restoreallhealth");
    koki_show(E1STAT);
    COSTUME(E1STAT, "costume1");
    koki_goto(E1STAT, 308, -144);
    GLIDE(F, E1STAT, 0.3, 150, -144);
    KOKI_END(F);
}

static koki_step s_e1stat_bounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    GLIDE(F, E1STAT, 0.05, 150, -139);
    GLIDE(F, E1STAT, 0.1, 150, -144);
    KOKI_END(F);
}

static koki_step s_e1stat_dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    /* Two costumes per hit -- the cannonball is worth double. */
    for (F->i = 0; F->i < 2; F->i++) {
        koki_next_costume(E1STAT);
        WAIT(F, 0.05);
    }
    if (IS_COSTUME(E1STAT, "Oof2"))
        BC("enemy1 oof");
    KOKI_END(F);
}

static koki_step s_e1stat_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(E1STAT, 150, -144);
    GLIDE(F, E1STAT, 0.3, 308, -144);
    koki_hide(E1STAT);
    KOKI_END(F);
}

static koki_step s_e1stat_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(E1STAT);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Cannon and cannonball
 * ------------------------------------------------------------------ */

static koki_step s_cannon_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(CANNON);
    SUB2(F, koki_g_flash, CANNON, 2);
    KOKI_END(F);
}

static koki_step s_cannon_arm(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON, "recording1");
    koki_goto(CANNON, -79, -69);
    GLIDE(F, CANNON, 0.3, -79, -74);
    for (;;) {
        if (KEY(X) && TOUCHING(CANNON, ANIM)) {
            BC("canonball");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_cannon_fired(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON, "explosion meme");
    SUB2(F, koki_g_flash, CANNON, 2);
    WAIT(F, 0.5);
    koki_hide(CANNON);
    KOKI_END(F);
}

static koki_step s_cannon_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(CANNON);
    koki_hide(CANNON);
    KOKI_END(F);
}

/* One Cannon ball sprite serves all three levels, under three messages. */
static koki_step s_cball_show(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(CBALL);
    koki_layer_back(CBALL);
    SUB2(F, koki_g_flash, CBALL, 2);
    KOKI_END(F);
}

static koki_step s_cball_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(CBALL, -41, -62);
    GLIDE_TO(F, CBALL, 0.7, EN1);
    BC("enemy1damage");
    koki_hide(CBALL);
    KOKI_END(F);
}

static koki_step s_cball_friendly_fire(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        /* The one inset in the whole game: both gate rectangles shrink 15%
         * on each side, so brushing the ball you fired does not hurt. */
        if (koki_touching(CBALL, ANIM, 0.3)) {
            BC("take damage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_cball2_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto_sprite(CBALL, CANNON2);
    GLIDE_TO(F, CBALL, 0.7, EN2);
    BC("enemy2 damage");
    koki_hide(CBALL);
    KOKI_END(F);
}

static koki_step s_cball3_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto_sprite(CBALL, CANNON3);
    GLIDE_TO(F, CBALL, 0.4, EN3);
    BC("enemy 3 damage");
    koki_hide(CBALL);
    KOKI_END(F);
}

static koki_step s_cball_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(CBALL);
    KOKI_END(F);
}

/* Kill any in-flight ball when the player dies: otherwise its damage
 * broadcast lands ~0.7 s later, restarting the boss over the corpse -- or
 * granting door progression on a death. */
static koki_step s_cball_playerdead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(CBALL);
    koki_hide(CBALL);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * The QuickPress prompt
 * ------------------------------------------------------------------ */

static koki_step s_quick_input(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(QUICK, "costume1");
    for (;;) {
        if (KEY(X)) {
            STOPOTHER(QUICK);
            /* Deviation kept from game.py: the original always dashed right.
             * Dashing AWAY from the boss means the escape never lunges into
             * him. */
            BC(ANIM->x < EN1->x ? "leftdash" : "rightdash");
            koki_hide(QUICK);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_quick_show(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(QUICK, "costume1");
    for (F->i = 0; F->i < 4; F->i++) {
        koki_show(QUICK);
        WAIT(F, 0.05);
        koki_hide(QUICK);
        WAIT(F, 0.05);
    }
    STOPOTHER(QUICK);
    KOKI_END(F);
}

static koki_step s_quick_chase(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(QUICK, "costume2");
    for (F->i = 0; F->i < 6; F->i++) {
        koki_show(QUICK);
        WAIT(F, 0.05);
        koki_hide(QUICK);
        WAIT(F, 0.05);
    }
    KOKI_END(F);
}

static koki_step s_quick_final(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(QUICK, "costume3");
    for (F->i = 0; F->i < 5; F->i++) {
        koki_show(QUICK);
        WAIT(F, 0.2);
        koki_hide(QUICK);
        WAIT(F, 0.2);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

static const char *const CBALL_DEAD_MSGS[3] = {"oofie", "planeoofie", "falloofie"};

void koki_register_lv1(koki_engine *eng)
{
    size_t i;

    ON("startlv1", EN1, s_en1_start);
    ON("startlv1", EN1, s_en1_bell);
    ON("hitbox", EN1, s_en1_hitbox);
    ON("take damage", EN1, s_en1_rearm);
    ON("enemy1 idle", EN1, s_en1_idle);
    ON("enemy1damage", EN1, s_en1_damaged);
    ON("enemy1 pounce", EN1, s_en1_pounce);
    ON("phaseagain", EN1, s_en1_phaseagain);
    ON("quickhitbox", EN1, s_en1_quickhitbox);
    ON("oofie", EN1, s_en1_playerdead);
    ON("go to lobby", EN1, s_en1_lobby);
    ON("enemy1 oof", EN1, s_en1_dead);
    ON("en1final", EN1, s_en1_final_entry);
    ON("possessen1", EN1, s_en1_possessed);
    ON("activehitonen1", EN1, s_en1_activehit);

    ON("shockwave", SW1, s_sw1_move);
    ON("shockwave", SW1, s_sw1_hit);
    ON("shockwave", SW2, s_sw2_move);
    ON("shockwave", SW2, s_sw2_hit);

    ON("startlv1", E1STAT, s_e1stat_in);
    ON("enemy1damage", E1STAT, s_e1stat_bounce);
    ON("enemy1damage", E1STAT, s_e1stat_dmg);
    ON("go to lobby", E1STAT, s_e1stat_out);
    ON("game over", E1STAT, s_e1stat_go);

    ON("spawncanon", CANNON, s_cannon_flash);
    ON("spawncanon", CANNON, s_cannon_arm);
    ON("canonball", CANNON, s_cannon_fired);
    ON("go to lobby", CANNON, s_cannon_lobby);

    ON("canonball", CBALL, s_cball_fly);
    ON("canonball", CBALL, s_cball_show);
    ON("canonball", CBALL, s_cball_friendly_fire);
    ON("canonball2", CBALL, s_cball_show);
    ON("canonball2", CBALL, s_cball2_fly);
    ON("cannonball3", CBALL, s_cball_show);
    ON("cannonball3", CBALL, s_cball3_fly);
    ON("go to lobby", CBALL, s_cball_lobby);
    for (i = 0u; i < ND_ARRAY_LEN(CBALL_DEAD_MSGS); i++)
        ON(CBALL_DEAD_MSGS[i], CBALL, s_cball_playerdead);

    ON("quick tap", QUICK, s_quick_input);
    ON("quick tap", QUICK, s_quick_show);
    ON("Chase for the door", QUICK, s_quick_chase);
    ON("startfinallevel", QUICK, s_quick_final);

    ND_UNUSED(eng);
}
