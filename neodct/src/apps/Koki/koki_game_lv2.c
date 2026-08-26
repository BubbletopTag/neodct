/* koki_game_lv2.c -- level 2: a biplane against a dragon.
 *
 * game.py section 10. The player stops walking and starts flying: up and
 * down move the plane, z is a boost, and x fires the cannon that drops past
 * on a timer. The dragon lashes with its tongue on a cycle that gets faster
 * once its health bar is half gone.
 *
 * ============ TWO DEVIATIONS THAT ARE game.py's, NOT THIS PORT's ============
 *
 * 1. THE BOOST. The original needed z held together with an arrow -- a chord.
 *    The phone's i2c keypad reports one key at a time (nd_input.h says so in
 *    as many words), and plenty of keyboards ghost the same combination, so
 *    z alone boosts in the last vertical direction tapped within two
 *    seconds, double-tap-dash style. With no recent tap it boosts toward
 *    open space. Chords still work for anyone who has them.
 * 2. THE DAMAGE GATE. "takeplanedamage" is funnelled through the same
 *    invincibility window the on-foot player has, and only then fanned out as
 *    "plane hurt". Without it the dragon's tongue lands on several frames in
 *    a row and takes the whole bar at once.
 *
 * ============ ONE ATTACK BODY, TWO SPEEDS ============
 *
 * _en2_attack(track_secs, track_reps, pace) is the whole tongue lash, and the
 * two cycle scripts differ only in what they pass it: (0.1, 5, 0.05) before
 * the halfway point and (0.05, 1, 0.03) after. The switch happens in
 * _e2stat_ping, which reads the health bar's COSTUME NUMBER -- five or more
 * means cycle 2.
 */

#include <stdio.h>

#include "nd_types.h"

#include "koki_game.h"

/* ------------------------------------------------------------------ *
 * The cutscene and the plane
 * ------------------------------------------------------------------ */

static koki_step s_cut1_fall(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 0.5);
    koki_show(CUT1);
    koki_layer_back(CUT1);
    COSTUME(CUT1, "falling");
    koki_goto(CUT1, -187, 180);
    GLIDE(F, CUT1, 0.5, -187, 0);
    BC("startplane");
    BC("enableplane");
    koki_hide(CUT1);
    KOKI_END(F);
}

static koki_step s_plane_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(PLANE);
    koki_goto(PLANE, -332, 0);
    GLIDE(F, PLANE, 1, -195, 0);
    KOKI_END(F);
}

static koki_step s_plane_prop(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < 10; F->i++) {
        COSTUME(PLANE, "costume1");
        WAIT(F, 0.01);
        COSTUME(PLANE, "costume3");
        WAIT(F, 0.01);
    }
    BC("planeANIM");
    KOKI_END(F);
}

static koki_step s_plane_anim(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        COSTUME(PLANE, "costume2");
        WAIT(F, 0.01);
        COSTUME(PLANE, "costume5");
        WAIT(F, 0.01);
    }
    KOKI_END(F);
}

static koki_step s_plane_up(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        while (KEY(UP)) {
            PLANE->y += 7.0;
            YIELD(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_plane_down(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        while (KEY(DOWN)) {
            PLANE->y -= 7.0;
            YIELD(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_plane_bounds(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (PLANE->y < -70.0)
            PLANE->y += 7.0;
        if (PLANE->y > 180.0)
            PLANE->y -= 7.0;
        YIELD(F);
    }
    KOKI_END(F);
}

/* The frame carries three things across the boost's suspensions: s0 is the
 * time of the last vertical tap, s1 its direction (1 = up), and tx the step
 * size the six-frame lunge is using. All three would be lost as C locals --
 * that is the one rule protothreads impose (koki_sched.h). */
static koki_step s_plane_boost(koki_frame *F)
{
    KOKI_BEGIN(F);
    F->s1 = 1.0;   /* last_dir = "up" */
    F->s0 = -99.0; /* last_t                 */
    for (;;) {
        if (KEY(UP)) {
            F->s1 = 1.0;
            F->s0 = koki_now(KE);
        }
        if (KEY(DOWN)) {
            F->s1 = 0.0;
            F->s0 = koki_now(KE);
        }
        if (KEY(Z)) {
            bool boost_up;

            /* Within two seconds of a tap, boost the way that tap pointed;
             * otherwise boost toward whichever half of the screen is emptier. */
            if (koki_now(KE) - F->s0 <= 2.0)
                boost_up = (F->s1 != 0.0);
            else
                boost_up = (PLANE->y < 55.0);
            koki_play(PLANE, "slide");
            F->tx = boost_up ? 20.0 : -20.0;
            for (F->i = 0; F->i < 6; F->i++) {
                PLANE->y += F->tx;
                YIELD(F);
            }
            BC("rechargeeffect");
            WAIT(F, 1);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_plane_recharge(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB2(F, koki_g_flash, PLANE, 5);
    KOKI_END(F);
}

static koki_step s_plane_dmg_gate(koki_frame *F)
{
    double t;

    KOKI_BEGIN(F);
    t = koki_now(KE);
    if (t - V(PLANE_HURT_T) >= KOKI_IFRAMES) {
        V(PLANE_HURT_T) = t;
        BC("plane hurt");
    }
    KOKI_END(F);
}

static const char *const PLANE_HIT_CYCLE[4] = {"costume4", "costume6", "costume9", "costume10"};

static koki_step s_plane_hit(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(PLANE);
    BC("enableplane");
    koki_play(PLANE, "hit sound");
    for (F->i = 0; F->i < 4; F->i++) {
        COSTUME(PLANE, PLANE_HIT_CYCLE[F->i]);
        WAIT(F, 0.05);
    }
    koki_play(PLANE, "beep");
    for (F->i = 0; F->i < 8; F->i++) {
        COSTUME(PLANE, "costume7");
        WAIT(F, 0.01);
        COSTUME(PLANE, "costume8");
        WAIT(F, 0.01);
    }
    PLANE->ghost = 0.0;
    BC("planeANIM");
    KOKI_END(F);
}

static koki_step s_plane_hit_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB2(F, koki_g_flash, PLANE, 10);
    KOKI_END(F);
}

static koki_step s_plane_dead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(PLANE);
    koki_play(PLANE, "hit sound");
    koki_play(PLANE, "hit");
    BC("planeoofanim");
    WAIT(F, 1);
    koki_play(PLANE, "Lose sound");
    GLIDE(F, PLANE, 1, 37, -246);
    koki_play(PLANE, "explbomb2");
    koki_hide(PLANE);
    WAIT(F, 2);
    BC("whitechange");
    WAIT(F, 0.05);
    if (V(LIVES) <= 0) {
        BC("game over");
        KOKI_RETURN(F);
    }
    BC("go to lobby");
    KOKI_END(F);
}

static koki_step s_plane_oofanim(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        COSTUME(PLANE, "Oofie");
        WAIT(F, 0.01);
        COSTUME(PLANE, "Oofie2");
        WAIT(F, 0.01);
    }
    KOKI_END(F);
}

static koki_step s_plane_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(PLANE);
    koki_hide(PLANE);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * The plane's health bar
 * ------------------------------------------------------------------ */

static koki_step s_kpstat_reset(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(KPSTAT, "costume1");
    KOKI_END(F);
}

static koki_step s_kpstat_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(KPSTAT);
    koki_goto(KPSTAT, -308, -144);
    GLIDE(F, KPSTAT, 0.3, -150, -144);
    BC("restoreplane");
    KOKI_END(F);
}

static koki_step s_kpstat_bounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(TAKEN_DAMAGE) += 1;
    GLIDE(F, KPSTAT, 0.05, -150, -139);
    GLIDE(F, KPSTAT, 0.1, -150, -144);
    KOKI_END(F);
}

static koki_step s_kpstat_restore(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(KPSTAT, "costume1");
    KOKI_END(F);
}

static koki_step s_kpstat_dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_next_costume(KPSTAT);
    WAIT(F, 0.05);
    if (IS_COSTUME(KPSTAT, "Oof")) {
        BC("planeoofie");
        V(KNOCKOUTS) += 1;
        WAIT(F, 0.05);
        koki_hide(KPSTAT);
    }
    KOKI_END(F);
}

static koki_step s_kpstat_oof(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(LIVES) -= 1;
    KOKI_END(F);
}

static koki_step s_kpstat_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(KPSTAT, -150, -144);
    GLIDE(F, KPSTAT, 0.3, -308, -144);
    koki_hide(KPSTAT);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Enemy 2 -- the dragon
 * ------------------------------------------------------------------ */

static koki_step s_en2_entry(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(EN2);
    COSTUME(EN2, "costume1");
    koki_goto(EN2, 170, 227);
    GLIDE(F, EN2, 1, 170, 0);
    WAIT(F, 0.2);
    COSTUME(EN2, "costume2");
    WAIT(F, 0.05);
    COSTUME(EN2, "costume3");
    WAIT(F, 0.05);
    koki_play(EN2, "grrr");
    for (F->i = 0; F->i < 5; F->i++) {
        COSTUME(EN2, "costume4");
        WAIT(F, 0.05);
        COSTUME(EN2, "costume5");
        WAIT(F, 0.05);
    }
    COSTUME(EN2, "costume3");
    WAIT(F, 0.05);
    COSTUME(EN2, "costume2");
    WAIT(F, 0.05);
    COSTUME(EN2, "costume1");
    WAIT(F, 0.05);
    BC("en2 cycle 1");
    KOKI_END(F);
}

static koki_step s_en2_hitbox(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(EN2, PLANE)) {
            BC("takeplanedamage");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* The retract sequence, in the original's order. */
static const char *const EN2_RETRACT[5] = {"costume11", "costume10", "costume9", "costume8",
                                           "costume7"};

/* One tongue attack: wind up, track the plane, lash out and retract. */
static koki_step en2_attack(koki_frame *F, double track_secs, int32_t track_reps, double pace)
{
    KOKI_BEGIN(F);
    BC("hitbox2");
    COSTUME(EN2, "costume6");
    WAIT(F, pace);
    for (F->i = 0; F->i < 5; F->i++) {
        koki_next_costume(EN2);
        WAIT(F, pace);
    }
    BC("enemy bright");
    for (F->i = 0; F->i < track_reps; F->i++) {
        /* The plane's y is sampled once per glide, at its first step: the
         * tongue leads the plane rather than following it. */
        GLIDE(F, EN2, track_secs, 170, PLANE->y);
    }
    koki_play(EN2, "output (27)");
    COSTUME(EN2, "costume12");
    WAIT(F, 0.05);
    for (F->i = 0; F->i < 10; F->i++) {
        koki_next_costume(EN2);
        WAIT(F, 0.05);
    }
    for (F->i = 0; F->i < 5; F->i++) {
        COSTUME(EN2, EN2_RETRACT[F->i]);
        WAIT(F, 0.05);
    }
    COSTUME(EN2, "costume6");
    WAIT(F, 0.2);
    KOKI_END(F);
}

static koki_step s_en2_cycle1(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        for (F->i = 0; F->i < 4; F->i++) {
            KOKI_CALL(F, en2_attack(&F[1], 0.1, 5, 0.05));
        }
        BC("spawncanon222");
        WAIT(F, 1);
    }
    KOKI_END(F);
}

static koki_step s_en2_cycle2(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        for (F->i = 0; F->i < 4; F->i++) {
            KOKI_CALL(F, en2_attack(&F[1], 0.05, 1, 0.03));
        }
        BC("spawncanon222");
        WAIT(F, 0.5);
    }
    KOKI_END(F);
}

static koki_step s_en2_bright(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB2(F, koki_g_flash, EN2, 6);
    KOKI_END(F);
}

static koki_step s_en2_playerdead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN2);
    KOKI_END(F);
}

static koki_step s_en2_damaged(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN2);
    BC("enemy bright");
    koki_play(EN2, "output (24)");
    koki_play(EN2, "output (31)");
    COSTUME(EN2, "Damage");
    WAIT(F, 0.05);
    for (F->i = 0; F->i < 9; F->i++) {
        koki_next_costume(EN2);
        WAIT(F, 0.05);
    }
    koki_play(EN2, "grrr");
    for (F->i = 0; F->i < 5; F->i++) {
        COSTUME(EN2, "costume5");
        WAIT(F, 0.05);
        COSTUME(EN2, "costume4");
        WAIT(F, 0.05);
    }
    COSTUME(EN2, "costume3");
    WAIT(F, 0.05);
    COSTUME(EN2, "costume2");
    WAIT(F, 0.05);
    COSTUME(EN2, "costume1");
    WAIT(F, 0.05);
    BC("ping enemy2 stats");
    KOKI_END(F);
}

static koki_step s_en2_dmg_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB2(F, koki_g_flash, EN2, 10);
    KOKI_END(F);
}

static koki_step s_en2_dead(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN2);
    koki_clear_fx(EN2);
    COSTUME(EN2, "OOF");
    koki_play(EN2, "output (29)");
    koki_play(EN2, "output (28)");
    for (F->i = 0; F->i < 15; F->i++) {
        /* Two draws from the private MT19937, and the ORDER MATTERS: C
         * leaves argument evaluation order unspecified, Python fixes it
         * left to right, so they are drawn into the frame first. */
        F->s0 = (double)RND(-240, 240);
        F->s1 = (double)RND(-180, 180);
        koki_goto(EN2, F->s0, F->s1);
        YIELD(F);
    }
    koki_play(EN2, "output (30)");
    COSTUME(EN2, "OOF2");
    WAIT(F, 0.05);
    COSTUME(EN2, "OOF3");
    WAIT(F, 0.05);
    koki_hide(EN2);
    V(DOORS) = 3;
    WAIT(F, 3);
    BC("go to lobby");
    KOKI_END(F);
}

static koki_step s_en2_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(EN2);
    koki_hide(EN2);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Enemy 2's health bar
 * ------------------------------------------------------------------ */

static koki_step s_e2stat_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("restoreplane");
    koki_show(E2STAT);
    koki_layer_front(E2STAT);
    COSTUME(E2STAT, "costume1");
    koki_goto(E2STAT, 308, -144);
    GLIDE(F, E2STAT, 0.3, 150, -144);
    KOKI_END(F);
}

static koki_step s_e2stat_bounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    GLIDE(F, E2STAT, 0.05, 150, -139);
    GLIDE(F, E2STAT, 0.1, 150, -144);
    KOKI_END(F);
}

static koki_step s_e2stat_dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_next_costume(E2STAT);
    WAIT(F, 0.05);
    if (IS_COSTUME(E2STAT, "Oof2"))
        BC("enemy2 oof");
    KOKI_END(F);
}

static koki_step s_e2stat_ping(koki_frame *F)
{
    KOKI_BEGIN(F);
    /* Costume NUMBER, 1-based as Scratch counts: past halfway the dragon
     * switches to the faster attack cycle. */
    if (koki_costume_number(E2STAT) >= 5)
        BC("en2 cycle 2");
    else
        BC("en2 cycle 1");
    KOKI_END(F);
}

static koki_step s_e2stat_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(E2STAT, 150, -144);
    GLIDE(F, E2STAT, 0.3, 308, -144);
    koki_hide(E2STAT);
    KOKI_END(F);
}

static koki_step s_e2stat_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(E2STAT);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Cannon 2 and the gas tank
 * ------------------------------------------------------------------ */

static koki_step s_cannon2_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(CANNON2);
    SUB2(F, koki_g_flash, CANNON2, 2);
    KOKI_END(F);
}

static koki_step s_cannon2_arm(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON2, "recording1");
    for (;;) {
        if (KEY(X) && TOUCHING(CANNON2, PLANE)) {
            BC("canonball2");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_cannon2_drop(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON2, "recording1");
    koki_show(CANNON2);
    koki_goto(CANNON2, -100, 186);
    GLIDE(F, CANNON2, 1.5, -100, -193);
    STOPOTHER(CANNON2);
    koki_hide(CANNON2);
    KOKI_END(F);
}

static koki_step s_cannon2_fired(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(CANNON2, "explosion meme");
    SUB2(F, koki_g_flash, CANNON2, 2);
    KOKI_END(F);
}

static koki_step s_cannon2_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(CANNON2);
    koki_hide(CANNON2);
    KOKI_END(F);
}

static koki_step s_gas_trigger(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("spawnfuel");
    KOKI_END(F);
}

static koki_step s_gas_fly(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(GAS);
    koki_goto(GAS, 242, (double)RND(-88, 150));
    /* glide(1.5, -243, GAS.y): the y is the one just chosen, read again at
     * the glide's first step -- which is this same frame. */
    F->s0 = GAS->y;
    GLIDE(F, GAS, 1.5, -243, F->s0);
    STOPOTHER(GAS);
    koki_hide(GAS);
    KOKI_END(F);
}

static koki_step s_gas_touch(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(GAS, PLANE)) {
            BC("restoreplane");
            koki_play(GAS, "heal");
            V(HAS_HEALED) = 1;
            STOPOTHER(GAS);
            koki_hide(GAS);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_gas_off(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(GAS);
    koki_hide(GAS);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * The "A to dodge" hint
 * ------------------------------------------------------------------ */

static koki_step s_dodge_hint(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(DODGE);
    koki_layer_front(DODGE);
    for (F->i = 0; F->i < 5; F->i++) {
        COSTUME(DODGE, "costume1");
        WAIT(F, 0.1);
        COSTUME(DODGE, "costume2");
        WAIT(F, 0.1);
    }
    koki_hide(DODGE);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

void koki_register_lv2(koki_engine *eng)
{
    ON("planecutscene", CUT1, s_cut1_fall);

    ON("planecutscene", PLANE, s_plane_in);
    ON("planecutscene", PLANE, s_plane_prop);
    ON("planeANIM", PLANE, s_plane_anim);
    ON("enableplane", PLANE, s_plane_up);
    ON("enableplane", PLANE, s_plane_down);
    ON("enableplane", PLANE, s_plane_bounds);
    ON("enableplane", PLANE, s_plane_boost);
    ON("rechargeeffect", PLANE, s_plane_recharge);
    ON("takeplanedamage", NULL, s_plane_dmg_gate);
    ON("plane hurt", PLANE, s_plane_hit);
    ON("plane hurt", PLANE, s_plane_hit_flash);
    ON("planeoofie", PLANE, s_plane_dead);
    ON("planeoofanim", PLANE, s_plane_oofanim);
    ON("go to lobby", PLANE, s_plane_lobby);

    ON("startlv2", KPSTAT, s_kpstat_reset);
    ON("startplane", KPSTAT, s_kpstat_in);
    ON("plane hurt", KPSTAT, s_kpstat_bounce);
    ON("restoreplane", KPSTAT, s_kpstat_restore);
    ON("plane hurt", KPSTAT, s_kpstat_dmg);
    ON("planeoofie", KPSTAT, s_kpstat_oof);
    ON("go to lobby", KPSTAT, s_kpstat_out);

    ON("startlv2", EN2, s_en2_entry);
    ON("hitbox2", EN2, s_en2_hitbox);
    ON("en2 cycle 1", EN2, s_en2_cycle1);
    ON("en2 cycle 2", EN2, s_en2_cycle2);
    ON("enemy bright", EN2, s_en2_bright);
    ON("planeoofie", EN2, s_en2_playerdead);
    ON("enemy2 damage", EN2, s_en2_damaged);
    ON("enemy2 damage", EN2, s_en2_dmg_flash);
    ON("enemy2 oof", EN2, s_en2_dead);
    ON("go to lobby", EN2, s_en2_lobby);

    ON("startlv2", E2STAT, s_e2stat_in);
    ON("enemy2 damage", E2STAT, s_e2stat_bounce);
    ON("enemy2 damage", E2STAT, s_e2stat_dmg);
    ON("ping enemy2 stats", E2STAT, s_e2stat_ping);
    ON("go to lobby", E2STAT, s_e2stat_out);
    ON("game over", E2STAT, s_e2stat_go);

    ON("spawncanon222", CANNON2, s_cannon2_flash);
    ON("spawncanon222", CANNON2, s_cannon2_arm);
    ON("spawncanon222", CANNON2, s_cannon2_drop);
    ON("canonball2", CANNON2, s_cannon2_fired);
    ON("go to lobby", CANNON2, s_cannon2_lobby);

    ON("en2 cycle 2", GAS, s_gas_trigger);
    ON("spawnfuel", GAS, s_gas_fly);
    ON("spawnfuel", GAS, s_gas_touch);
    ON("planeoofie", GAS, s_gas_off);
    ON("go to lobby", GAS, s_gas_off);

    ON("level2", DODGE, s_dodge_hint);

    ND_UNUSED(eng);
}
