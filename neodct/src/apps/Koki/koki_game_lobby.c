/* koki_game_lobby.c -- lives, the health bar, GAME OVER, and the four doors.
 *
 * game.py sections 7 and 8.
 *
 * ============ THE HEALTH BAR IS A COSTUME COUNTER ============
 *
 * KokiStats has no number in it. Taking a hit calls next_costume(), and the
 * bar is "empty" when the costume it lands on is called "Oof" -- at which
 * point it broadcasts "oofie", which is what actually kills the player. Every
 * heal is likewise a costume walked backwards by name (partialrestore). This
 * is how Scratch does it and it is why the manifest's costume ORDER for the
 * stats sprites is load-bearing rather than cosmetic.
 *
 * ============ ONE DOOR SCRIPT, FOUR DOORS ============
 *
 * Python builds these with factory functions that close over a door
 * (_mk_hide(_d), _mk_cut(_d)). C has no closures, but it does not need one:
 * the scheduler already records which sprite a handler was registered
 * against -- stop_other_scripts compares exactly that pointer -- so
 * koki_script_sprite() hands the running script its own door. One function,
 * four registrations, no duplicated bodies and no table to keep in step.
 *
 * ============ WHY DOOR 4 DOES NOT BROADCAST "doorinteracted" ============
 *
 * The other three do, and it is what hides the doors and the lives icon. Door
 * 4 opens the final CUTSCENE rather than a level, and the cutscene wants the
 * other doors still on screen flashing behind it. Faithful to game.py, which
 * simply omits the third broadcast from door 4's enter().
 */

#include <stdio.h>

#include "nd_types.h"

#include "koki_game.h"

/* ------------------------------------------------------------------ *
 * Lives icon
 * ------------------------------------------------------------------ */

static koki_step s_lives(koki_frame *F)
{
    KOKI_BEGIN(F);
    if (V(LIVES) >= 3)
        COSTUME(LIVES, "costume1");
    else if (V(LIVES) == 2)
        COSTUME(LIVES, "costume2");
    else if (V(LIVES) == 1)
        COSTUME(LIVES, "costume3");
    /* No branch for 0: the icon keeps whatever it had, and "game over" is
     * about to cover it anyway. */
    WAIT(F, 1);
    koki_show(LIVES);
    KOKI_END(F);
}

static koki_step s_lives_hide(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(LIVES);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * KokiStats -- the health bar
 * ------------------------------------------------------------------ */

static koki_step s_kstat_lv1(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(KSTAT, "costume1");
    KOKI_END(F);
}

static koki_step s_kstat_in(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("restoreallhealth");
    koki_show(KSTAT);
    koki_goto(KSTAT, -308, -144);
    GLIDE(F, KSTAT, 0.3, -150, -144);
    KOKI_END(F);
}

static koki_step s_kstat_dmg_bounce(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(TAKEN_DAMAGE) = 1;
    GLIDE(F, KSTAT, 0.05, -150, -139);
    GLIDE(F, KSTAT, 0.1, -150, -144);
    KOKI_END(F);
}

static koki_step s_kstat_dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_next_costume(KSTAT);
    if (IS_COSTUME(KSTAT, "Oof")) {
        BC("oofie");
        V(KNOCKOUTS) += 1;
        WAIT(F, 0.05);
        koki_hide(KSTAT);
    }
    KOKI_END(F);
}

static koki_step s_kstat_restore(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(KSTAT, "costume1");
    KOKI_END(F);
}

static koki_step s_kstat_newgame(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(LIVES) = 3;
    KOKI_END(F);
}

static koki_step s_kstat_oofie(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(LIVES) -= 1;
    KOKI_END(F);
}

static koki_step s_kstat_fall(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(LIVES) -= 1;
    koki_hide(KSTAT);
    KOKI_END(F);
}

static koki_step s_kstat_lv2(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(KSTAT);
    koki_goto(KSTAT, -150, -144);
    GLIDE(F, KSTAT, 0.3, -308, -144);
    koki_hide(KSTAT);
    KOKI_END(F);
}

static koki_step s_kstat_lv3(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("restoreallhealth");
    koki_show(KSTAT);
    koki_goto(KSTAT, -308, -144);
    GLIDE(F, KSTAT, 0.3, -150, -144);
    KOKI_END(F);
}

static koki_step s_kstat_temphit(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(KSTAT, "costume2");
    GLIDE(F, KSTAT, 0.05, -150, -139);
    GLIDE(F, KSTAT, 0.1, -150, -144);
    KOKI_END(F);
}

static koki_step s_kstat_temphit2(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(KSTAT, "costume3");
    GLIDE(F, KSTAT, 0.05, -150, -139);
    GLIDE(F, KSTAT, 0.1, -150, -144);
    KOKI_END(F);
}

static koki_step s_kstat_partial(koki_frame *F)
{
    KOKI_BEGIN(F);
    /* One step back up the costume list, by name. */
    if (IS_COSTUME(KSTAT, "costume2"))
        COSTUME(KSTAT, "costume1");
    else if (IS_COSTUME(KSTAT, "costume3"))
        COSTUME(KSTAT, "costume2");
    else if (IS_COSTUME(KSTAT, "costume4"))
        COSTUME(KSTAT, "costume3");
    KOKI_END(F);
}

static koki_step s_kstat_end(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(KSTAT);
    koki_hide(KSTAT);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * GAME OVER
 * ------------------------------------------------------------------ */

static koki_step s_gameover_music(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_sprite_music(GOVER, "KokiPrototypelOBBY");
    KOKI_END(F);
}

static koki_step s_gameover(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_layer_front(GOVER);
    koki_show(GOVER);
    COSTUME(GOVER, "costume1");
    WAIT(F, 0.5);
    for (;;) {
        COSTUME(GOVER, "costume2");
        if (KEY(ENTER)) {
            koki_play(GOVER, "startbutton");
            koki_stop_music(KE);
            for (F->i = 0; F->i < 5; F->i++) {
                COSTUME(GOVER, "costume1");
                WAIT(F, 0.05);
                COSTUME(GOVER, "costume2");
                WAIT(F, 0.05);
            }
            COSTUME(GOVER, "costume2");
            WAIT(F, 0.05);
            BC("whitechange");
            BC("go to lobby");
            BC("lockAlldoors");
            V(LIVES) = 3;
            koki_hide(GOVER);
            STOPOTHER(GOVER);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * The doors
 * ------------------------------------------------------------------ */

/* _door_interact(door, on_enter, unlock_check): hide, wait, show if unlocked,
 * then watch for x/up while the character is standing on it.
 *
 * unlock_min of 0 means "no check" -- door 1 is always open.
 * announce_interacted is false for door 4 only; see the header comment. */
static koki_step door_interact(koki_frame *F, koki_sprite *door, double unlock_min,
                               const char *level_msg, bool announce_interacted)
{
    KOKI_BEGIN(F);
    koki_hide(door);
    WAIT(F, 0.5);
    if (unlock_min > 0.0 && !(V(DOORS) >= unlock_min))
        KOKI_RETURN(F);
    koki_show(door);
    for (;;) {
        if (TOUCHING(door, ANIM) && (KEY(X) || KEY(UP))) {
            BC("turn anim");
            BC(level_msg);
            if (announce_interacted)
                BC("doorinteracted");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_door1_watch(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, door_interact(&F[1], DOOR1, 0.0, "level1", true));
    KOKI_END(F);
}

static koki_step s_door2_watch(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, door_interact(&F[1], DOOR2, 2.0, "level2", true));
    KOKI_END(F);
}

static koki_step s_door3_watch(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, door_interact(&F[1], DOOR3, 3.0, "level3", true));
    KOKI_END(F);
}

static koki_step s_door4_watch(koki_frame *F)
{
    KOKI_BEGIN(F);
    KOKI_CALL(F, door_interact(&F[1], DOOR4, 4.0, "level 4", false));
    KOKI_END(F);
}

/* One flashing body for all four doors and for both of door 4's messages. */
static koki_step s_door_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB1(F, koki_g_door_flash, koki_script_sprite(KE));
    KOKI_END(F);
}

static koki_step s_door1_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("whitechange");
    WAIT(F, 0.05);
    BC("startlv1");
    BC("PlayerEnable");
    KOKI_END(F);
}

static koki_step s_door2_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("whitechange");
    WAIT(F, 0.05);
    BC("startlv2");
    BC("disableplayer");
    BC("planecutscene");
    KOKI_END(F);
}

static koki_step s_door3_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    BC("whitechange");
    WAIT(F, 0.05);
    BC("startlv3");
    BC("RUNenable");
    KOKI_END(F);
}

static koki_step s_door2_unlock(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(DOORS) = 2;
    KOKI_END(F);
}

static koki_step s_doors_lock(koki_frame *F)
{
    KOKI_BEGIN(F);
    V(DOORS) = 1;
    KOKI_END(F);
}

static koki_step s_door4_go(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(DOOR4);
    BC("final cutscene");
    koki_show(DOOR4);
    SUB1(F, koki_g_door_flash, DOOR4);
    KOKI_END(F);
}

static koki_step s_door4_again_watch(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (TOUCHING(DOOR4, ANIM) && (KEY(X) || KEY(UP))) {
            BC("turn anim");
            BC("whitechange");
            WAIT(F, 0.5);
            BC("ending cutscene");
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* _mk_hide(spr) and _mk_hide(spr, stop=True). The door comes from the slot. */
static koki_step s_door_hide(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(koki_script_sprite(KE));
    KOKI_END(F);
}

static koki_step s_door_hide_stop(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(koki_script_sprite(KE));
    koki_hide(koki_script_sprite(KE));
    KOKI_END(F);
}

/* _mk_cut(door): during the final cutscene the other doors flash
 * decoratively. */
static koki_step s_door_cut(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(koki_script_sprite(KE));
    koki_show(koki_script_sprite(KE));
    SUB1(F, koki_g_door_flash, koki_script_sprite(KE));
    KOKI_END(F);
}

/* _mk_still(door): and they stop flashing when the fight starts. */
static koki_step s_door_still(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(koki_script_sprite(KE));
    COSTUME(koki_script_sprite(KE), "costume1");
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration -- game.py's order exactly
 * ------------------------------------------------------------------ */

void koki_register_lobby(koki_engine *eng)
{
    koki_sprite *doors[4];
    size_t i;

    ON("go to lobby", LIVES, s_lives);
    ON("doorinteracted", LIVES, s_lives_hide);
    ON("ending cutscene", LIVES, s_lives_hide);

    ON("startlv1", KSTAT, s_kstat_lv1);
    ON("PlayerEnable", KSTAT, s_kstat_in);
    ON("koki hurt", KSTAT, s_kstat_dmg_bounce);
    ON("koki hurt", KSTAT, s_kstat_dmg);
    ON("restoreallhealth", KSTAT, s_kstat_restore);
    ON("start game", KSTAT, s_kstat_newgame);
    ON("oofie", KSTAT, s_kstat_oofie);
    ON("falloofie", KSTAT, s_kstat_fall);
    ON("startlv2", KSTAT, s_kstat_lv2);
    ON("startlv3", KSTAT, s_kstat_lv3);
    ON("temporary hit", KSTAT, s_kstat_temphit);
    ON("temporaryhit2", KSTAT, s_kstat_temphit2);
    ON("partialrestore", KSTAT, s_kstat_partial);
    ON("ending cutscene", KSTAT, s_kstat_end);

    ON("game over", GOVER, s_gameover_music);
    ON("game over", GOVER, s_gameover);

    ON("go to lobby", DOOR1, s_door1_watch);
    ON("go to lobby", DOOR1, s_door_flash);
    ON("level1", DOOR1, s_door1_go);

    ON("go to lobby", DOOR2, s_door2_watch);
    ON("go to lobby", DOOR2, s_door_flash);
    ON("level2", DOOR2, s_door2_go);
    ON("unlock door2", DOOR2, s_door2_unlock);
    ON("lockAlldoors", DOOR2, s_doors_lock);

    ON("go to lobby", DOOR3, s_door3_watch);
    ON("go to lobby", DOOR3, s_door_flash);
    ON("level3", DOOR3, s_door3_go);

    ON("go to lobby", DOOR4, s_door4_watch);
    ON("go to lobby", DOOR4, s_door_flash);
    ON("level 4", DOOR4, s_door4_go);
    ON("door4openagain", DOOR4, s_door_flash);
    ON("door4openagain", DOOR4, s_door4_again_watch);

    doors[0] = DOOR1;
    doors[1] = DOOR2;
    doors[2] = DOOR3;
    doors[3] = DOOR4;
    for (i = 0u; i < 4u; i++) {
        ON("doorinteracted", doors[i], s_door_hide);
        ON("level2", doors[i], s_door_hide);
    }
    for (i = 0u; i < 3u; i++)
        ON("level3", doors[i], s_door_hide);
    ON("level3", DOOR4, s_door_hide);
    for (i = 0u; i < 4u; i++)
        ON("ending cutscene", doors[i], s_door_hide_stop);

    /* Both registrations happen inside ONE loop iteration in game.py, so the
     * order is (cut, still) per door rather than all the cuts then all the
     * stills. Registration order fixes the handler keys, so this matters. */
    for (i = 0u; i < 3u; i++) {
        ON("final cutscene", doors[i], s_door_cut);
        ON("startfinallevel", doors[i], s_door_still);
    }
    ON("startfinallevel", DOOR4, s_door_still);

    ND_UNUSED(eng);
}
