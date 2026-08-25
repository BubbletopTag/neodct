/* koki_game_boot.c -- register_all(), and game.py's first six sections.
 *
 * The Stage's backdrops and music; the Dynaris logo fading in, animating and
 * handing over to the bobbing Koki title; the flashing START button and the
 * controls panel; the White sprite that does every full-screen fade; the
 * Platform; the Player, which is an invisible physics box; and CharacterAnim,
 * which is the Koki you actually see following it around.
 *
 * ============ THE PLAYER IS NOT THE CHARACTER ============
 *
 * PLAYER is a hitbox at ghost 99 -- present, collidable, and invisible. All
 * the physics (walking, gravity, standing on the Platform) happens to it, and
 * CharacterAnim does goto_sprite(PLAYER) every frame and plays the walk cycle.
 * Five separate scripts run on ANIM at once for that: follow, reset, walk,
 * jump, idle and face. They are separate because Scratch scripts are, and
 * merging them would change which ones "stop other scripts in sprite" kills.
 *
 * ============ WHY register_all IS ONE LONG FUNCTION ============
 *
 * Because game.py's is, and REGISTRATION ORDER IS OBSERVABLE: it fixes each
 * handler's key, which fixes the order scripts run in on the first frame and
 * the grouping stop_other_scripts sees. Splitting the file is fine (the
 * sections are called in order); reordering within it is not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_log.h"
#include "nd_types.h"

#include "koki_game.h"

koki_sprite *KG[KOKI_C_COUNT];
koki_engine *KE;
double KOKI_ATK = 1.35;

/* ------------------------------------------------------------------ *
 * Shared sub-scripts
 * ------------------------------------------------------------------ */

koki_step koki_g_flash(koki_frame *F, koki_sprite *s, int32_t times)
{
    KOKI_BEGIN(F);
    for (F->i = 0; F->i < times; F->i++) {
        s->ghost = 50.0;
        s->brightness = 50.0;
        WAIT(F, 0.05);
        s->ghost = 0.0;
        s->brightness = 0.0;
        WAIT(F, 0.05);
    }
    KOKI_END(F);
}

koki_step koki_g_white_fade_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(WHITE);
    koki_layer_front(WHITE);
    WHITE->ghost = 0.0;
    for (F->i = 0; F->i < 20; F->i++) {
        WHITE->ghost += 5.0;
        YIELD(F);
    }
    koki_hide(WHITE);
    KOKI_END(F);
}

koki_step koki_g_door_flash(koki_frame *F, koki_sprite *door)
{
    KOKI_BEGIN(F);
    for (;;) {
        COSTUME(door, "costume1");
        WAIT(F, 0.3);
        COSTUME(door, "costume2");
        WAIT(F, 0.3);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Stage: backdrops + music
 * ------------------------------------------------------------------ */

static koki_step s_stage_flag(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop1");
    KOKI_END(F);
}

static koki_step s_stage_intro(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_stage_music(KE, "Koki prototype theme");
    KOKI_END(F);
}

static koki_step s_stage_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop5");
    koki_stage_music(KE, "Koki New Level Lobby");
    KOKI_END(F);
}

static koki_step s_stage_lv1(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop3");
    koki_stage_music(KE, "Koki Level 1");
    KOKI_END(F);
}

static koki_step s_stage_lv2(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop4");
    koki_stage_music(KE, "Koki Level 2");
    KOKI_END(F);
}

static koki_step s_stage_lv3(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop6");
    koki_stage_music(KE, "Popi vs Koki");
    KOKI_END(F);
}

static koki_step s_stage_final(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop5");
    koki_stage_music(KE, "Riby boss fight prototype music 1");
    KOKI_END(F);
}

static koki_step s_stage_ending(koki_frame *F)
{
    KOKI_BEGIN(F);
    BACKDROP("backdrop2");
    koki_stage_music(KE, "lobbykoki");
    KOKI_END(F);
}

static koki_step s_stage_bell(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_stage_sfx(KE, "boxing bell");
    KOKI_END(F);
}

/* One body, registered against nine messages -- the original's "stop other
 * scripts in sprite" on the Stage. */
static koki_step s_stage_stop(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_stop_music(KE);
    KOKI_END(F);
}

static koki_step s_stage_finalcut(koki_frame *F)
{
    KOKI_BEGIN(F);
    /* The original slides the music's pitch down 400% over ~80 frames. No
     * external player can do that, so it fades out by simply stopping after
     * a beat. game.py's deviation, kept. */
    WAIT(F, 1.0);
    koki_stop_music(KE);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Dynaris Logo -> intro -> start button -> info panel
 * ------------------------------------------------------------------ */

static koki_step s_logo(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(LOGO, "costume1");
    koki_show(LOGO);
    koki_layer_front(LOGO);
    LOGO->ghost = 100.0;
    for (F->i = 0; F->i < 20; F->i++) {
        LOGO->ghost -= 5.0;
        WAIT(F, 0.01);
    }
    koki_play(LOGO, "Collect Sound Effect");
    WAIT(F, 0.1);
    for (F->i = 0; F->i < 12; F->i++) {
        koki_next_costume(LOGO);
        YIELD(F);
    }
    WAIT(F, 0.5);
    for (F->i = 0; F->i < 20; F->i++) {
        LOGO->ghost += 5.0;
        WAIT(F, 0.01);
    }
    WAIT(F, 1);
    LOGO->ghost = 100.0;
    koki_hide(LOGO);
    BC("start intro");
    KOKI_END(F);
}

static koki_step s_intro(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_goto(INTRO, 0, 0);
    COSTUME(INTRO, "Koki Icon");
    BACKDROP("backdrop2");
    koki_show(INTRO);
    /* Scratch clamps "set size -100%" to a tiny sprite. At size 2 with a
     * baked size of 100, size_q rounds to 0 and the costume really is
     * resized to 1x1 -- see koki_render.c. */
    INTRO->size = 2.0;
    for (F->i = 0; F->i < 5; F->i++) {
        INTRO->size += 5.0;
        YIELD(F);
    }
    for (F->i = 0; F->i < 4; F->i++) {
        INTRO->size += 20.0;
        YIELD(F);
    }
    for (F->i = 0; F->i < 3; F->i++) {
        INTRO->size += 1.0;
        YIELD(F);
    }
    WAIT(F, 0.1);
    for (F->i = 0; F->i < 12; F->i++) {
        INTRO->size -= 1.0;
        YIELD(F);
    }
    INTRO->size = 100.0;
    WAIT(F, 0.5);
    BC("start button enable");
    for (;;) {
        GLIDE(F, INTRO, 0.5, 0, 5);
        GLIDE(F, INTRO, 0.5, 0, 0);
    }
    KOKI_END(F);
}

static koki_step s_intro_out(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(INTRO);
    GLIDE(F, INTRO, 0.3, 0, -298);
    koki_hide(INTRO);
    KOKI_END(F);
}

static koki_step s_btn_anim(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(STARTBTN);
    for (;;) {
        COSTUME(STARTBTN, "costume1");
        WAIT(F, 0.4);
        COSTUME(STARTBTN, "costume2");
        WAIT(F, 0.4);
    }
    KOKI_END(F);
}

static koki_step s_btn_input(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (KEY(ENTER)) {
            STOPOTHER(STARTBTN);
            koki_play(STARTBTN, "startbutton");
            STARTBTN->size = 70.0;
            for (F->i = 0; F->i < 3; F->i++) {
                koki_next_costume(STARTBTN);
                STARTBTN->size += 5.0;
                YIELD(F);
            }
            STARTBTN->size = 70.0;
            for (F->i = 0; F->i < 5; F->i++) {
                COSTUME(STARTBTN, "costume1");
                WAIT(F, 0.05);
                COSTUME(STARTBTN, "costume2");
                WAIT(F, 0.05);
            }
            COSTUME(STARTBTN, "costume1");
            WAIT(F, 0.05);
            BC("start game");
            koki_hide(STARTBTN);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_panel(koki_frame *F)
{
    KOKI_BEGIN(F);
    WAIT(F, 0.5);
    koki_show(PANEL);
    PANEL->size = 100.0;
    COSTUME(PANEL, "costume1");
    koki_goto(PANEL, 0, -394);
    GLIDE(F, PANEL, 0.3, 0, 0);
    WAIT(F, 1);
    COSTUME(PANEL, "costume2");
    for (;;) {
        if (KEY(ENTER)) {
            STOPOTHER(PANEL);
            koki_goto(PANEL, 0, 0);
            koki_play(PANEL, "startbutton");
            for (F->i = 0; F->i < 5; F->i++) {
                COSTUME(PANEL, "costume2");
                WAIT(F, 0.05);
                COSTUME(PANEL, "costume1");
                WAIT(F, 0.05);
            }
            COSTUME(PANEL, "costume2");
            WAIT(F, 0.05);
            koki_goto(PANEL, 0, 0);
            GLIDE(F, PANEL, 0.3, 0, -291);
            BC("go to lobby");
            koki_hide(PANEL);
            KOKI_RETURN(F);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * White: full-screen fades
 * ------------------------------------------------------------------ */

static koki_step s_white_fade(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB(F, koki_g_white_fade_out);
    KOKI_END(F);
}

static koki_step s_white_change(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(WHITE);
    koki_layer_front(WHITE);
    WHITE->ghost = 100.0;
    for (F->i = 0; F->i < 10; F->i++) {
        WHITE->ghost -= 10.0;
        YIELD(F);
    }
    WHITE->ghost = 0.0;
    WAIT(F, 0.05);
    for (F->i = 0; F->i < 20; F->i++) {
        WHITE->ghost += 5.0;
        YIELD(F);
    }
    koki_hide(WHITE);
    KOKI_END(F);
}

static koki_step s_white_end(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(WHITE, "costume2");
    koki_show(WHITE);
    koki_layer_front(WHITE);
    WHITE->ghost = 100.0;
    for (F->i = 0; F->i < 10; F->i++) {
        WHITE->ghost -= 10.0;
        YIELD(F);
    }
    WAIT(F, 1);
    BC("ending score");
    WHITE->ghost = 0.0;
    WAIT(F, 0.05);
    for (F->i = 0; F->i < 20; F->i++) {
        WHITE->ghost += 5.0;
        YIELD(F);
    }
    koki_hide(WHITE);
    COSTUME(WHITE, "costume1");
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Platform
 * ------------------------------------------------------------------ */

static koki_step s_plat_lobby(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_layer_back(PLAT);
    koki_show(PLAT);
    COSTUME(PLAT, "costume2");
    koki_goto(PLAT, 0, -141);
    GLIDE(F, PLAT, 0.5, 0, -50);
    BC("PlayerEnable");
    KOKI_END(F);
}

static koki_step s_plat_lv1(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(PLAT);
    COSTUME(PLAT, "costume3");
    koki_goto(PLAT, 0, -141);
    GLIDE(F, PLAT, 0.2, 0, -50);
    KOKI_END(F);
}

static koki_step s_plat_lv2(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_hide(PLAT);
    KOKI_END(F);
}

static koki_step s_plat_lv3(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(PLAT);
    COSTUME(PLAT, "costume5");
    koki_goto(PLAT, 0, -141);
    GLIDE(F, PLAT, 0.2, 0, -50);
    KOKI_END(F);
}

static koki_step s_plat_end(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(PLAT);
    COSTUME(PLAT, "costume1");
    koki_goto(PLAT, 0, -50);
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Player -- the invisible physics hitbox
 * ------------------------------------------------------------------ */

static bool touch_plat(void)
{
    return TOUCHING(PLAYER, PLAT);
}

static koki_step s_player_physics(koki_frame *F)
{
    double dx;

    KOKI_BEGIN(F);
    koki_show(PLAYER);
    COSTUME(PLAYER, "char2");
    PLAYER->ghost = 99.0;
    koki_goto(PLAYER, -200, 30);
    PLAYER->sy = 0.0;
    for (;;) {
        dx = koki_kdir(KE) * 5.0;
        PLAYER->x += dx;
        /* Scratch's stage fencing kept the player on screen. */
        if (PLAYER->x < -235.0)
            PLAYER->x = -235.0;
        if (PLAYER->x > 235.0)
            PLAYER->x = 235.0;
        if (touch_plat())
            PLAYER->x -= dx;
        PLAYER->sy -= 1.0;
        PLAYER->y += PLAYER->sy;
        if (touch_plat()) {
            PLAYER->y -= PLAYER->sy;
            PLAYER->sy = (PLAYER->sy < 1.0 && KEY(Z)) ? 15.0 : 0.0;
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_player_run(koki_frame *F)
{
    KOKI_BEGIN(F);
    /* Level 3: fixed x, jump only, and a stronger jump. */
    koki_show(PLAYER);
    COSTUME(PLAYER, "char2");
    PLAYER->ghost = 99.0;
    koki_goto(PLAYER, 90, -54);
    PLAYER->sy = 0.0;
    for (;;) {
        PLAYER->sy -= 1.0;
        PLAYER->y += PLAYER->sy;
        if (touch_plat()) {
            PLAYER->y -= PLAYER->sy;
            PLAYER->sy = (PLAYER->sy < 1.0 && KEY(Z)) ? 17.0 : 0.0;
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_player_off(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(PLAYER);
    koki_hide(PLAYER);
    KOKI_END(F);
}

/* The dash step table, shared by both directions. */
static const double DASH_STEPS[9] = {30.0, 30.0, 30.0, 15.0, 15.0, 15.0, 5.0, 5.0, 5.0};

static koki_step s_player_rdash(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(PLAYER, "slide");
    koki_point(PLAYER, 90);
    for (F->i = 0; F->i < (int32_t)ND_ARRAY_LEN(DASH_STEPS); F->i++) {
        koki_move_steps(PLAYER, DASH_STEPS[F->i]);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_player_ldash(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(PLAYER, "slide");
    koki_point(PLAYER, -90);
    for (F->i = 0; F->i < (int32_t)ND_ARRAY_LEN(DASH_STEPS); F->i++) {
        koki_move_steps(PLAYER, DASH_STEPS[F->i]);
        YIELD(F);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * CharacterAnim -- the Koki you see
 * ------------------------------------------------------------------ */

/* The four-frame walk cycle, in the original's order. */
static const char *const WALK_CYCLE[4] = {"costume6", "costume8", "costume4", "costume9"};

static koki_step s_anim_follow(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        koki_goto_sprite(ANIM, PLAYER);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_reset(koki_frame *F)
{
    KOKI_BEGIN(F);
    ANIM->brightness = 0.0;
    ANIM->ghost = 0.0;
    COSTUME(ANIM, "costume10");
    ANIM->rotation_style = KOKI_ROT_LEFT_RIGHT;
    koki_show(ANIM);
    COSTUME(ANIM, "costume2");
    KOKI_END(F);
}

static koki_step s_anim_walk(koki_frame *F)
{
    bool moving;

    KOKI_BEGIN(F);
    for (;;) {
        moving = KEY(LEFT) || KEY(RIGHT);
        if (moving && !(KEY(Z) || !TOUCHING(ANIM, PLAT))) {
            for (;;) {
                if ((!(KEY(LEFT) || KEY(RIGHT))) || KEY(Z) || (KEY(LEFT) && KEY(RIGHT)))
                    break;
                for (F->i = 0; F->i < 4; F->i++) {
                    COSTUME(ANIM, WALK_CYCLE[F->i]);
                    WAIT(F, 0.01);
                }
            }
            COSTUME(ANIM, "costume2");
            WAIT(F, 0.05);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_jump(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (KEY(Z) && !TOUCHING(ANIM, PLAT)) {
            while (!TOUCHING(ANIM, PLAT)) {
                COSTUME(ANIM, "costume10");
                WAIT(F, 0.01);
            }
            COSTUME(ANIM, "costume11");
            WAIT(F, 0.05);
            COSTUME(ANIM, "costume2");
            WAIT(F, 0.01);
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_idle(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        WAIT(F, 10);
        if (!koki_any_key(KE) && TOUCHING(ANIM, PLAT)) {
            COSTUME(ANIM, "costume3");
            WAIT(F, 0.05);
            COSTUME(ANIM, "costume2");
            WAIT(F, 0.01);
        }
    }
    KOKI_END(F);
}

static koki_step s_anim_face(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (KEY(RIGHT))
            koki_point(ANIM, 90);
        if (KEY(LEFT))
            koki_point(ANIM, -90);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_idle_set(koki_frame *F)
{
    KOKI_BEGIN(F);
    COSTUME(ANIM, "costume2");
    KOKI_END(F);
}

static koki_step s_anim_door(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(ANIM);
    COSTUME(ANIM, "costume4");
    WAIT(F, 0.1);
    COSTUME(ANIM, "door");
    WAIT(F, 0.6);
    COSTUME(ANIM, "costume2");
    KOKI_END(F);
}

static koki_step s_anim_oofie(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(ANIM);
    koki_play(ANIM, "hit");
    COSTUME(ANIM, "OOF");
    ANIM->brightness = 0.0;
    ANIM->ghost = 0.0;
    WAIT(F, 1);
    koki_play(ANIM, "Lose sound");
    for (F->i = 0; F->i < 7; F->i++) {
        ANIM->y += 5.0;
        YIELD(F);
    }
    /* glide(0.7, ANIM.x, -204): the x is read when the glide starts, so it
     * is captured into the frame rather than passed as a live expression --
     * the call macro re-evaluates its arguments on every resume. */
    F->s0 = ANIM->x;
    GLIDE(F, ANIM, 0.7, F->s0, -204);
    koki_hide(ANIM);
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

/* 'take damage' is gated through a brief invincibility window and fanned out
 * as 'koki hurt'; the health bar and the flash listen to that instead. */
static koki_step s_dmg_gate(koki_frame *F)
{
    double t;

    KOKI_BEGIN(F);
    t = koki_now(KE);
    if (t - V(HURT_T) >= KOKI_IFRAMES) {
        V(HURT_T) = t;
        BC("koki hurt");
    }
    KOKI_END(F);
}

static koki_step s_anim_dmg_sound(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(ANIM, "hit2");
    for (F->i = 0; F->i < 10; F->i++) {
        COSTUME(ANIM, "OOF");
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_dmg_flash(koki_frame *F)
{
    KOKI_BEGIN(F);
    SUB2(F, koki_g_flash, ANIM, 10);
    KOKI_END(F);
}

static koki_step s_anim_dodge(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(ANIM, "slide");
    COSTUME(ANIM, "costume11");
    SUB2(F, koki_g_flash, ANIM, 10);
    KOKI_END(F);
}

static koki_step s_anim_disable(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(ANIM);
    koki_hide(ANIM);
    KOKI_END(F);
}

static koki_step s_anim_fall(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(ANIM);
    koki_hide(ANIM);
    koki_play(ANIM, "Fall");
    WAIT(F, 1);
    koki_play(ANIM, "Lose sound");
    WAIT(F, 2.7);
    BC("whitechange");
    WAIT(F, 0.05);
    if (V(LIVES) <= 0) {
        BC("game over");
        KOKI_RETURN(F);
    }
    BC("go to lobby");
    KOKI_END(F);
}

static koki_step s_anim_run_follow(koki_frame *F)
{
    KOKI_BEGIN(F);
    ANIM->ghost = 0.0;
    for (;;) {
        koki_goto_sprite(ANIM, PLAYER);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_run_start(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(ANIM);
    BC("RUNNN");
    koki_point(ANIM, 90);
    KOKI_END(F);
}

static koki_step s_anim_runnn_follow(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        koki_goto_sprite(ANIM, PLAYER);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_runnn_anim(koki_frame *F)
{
    KOKI_BEGIN(F);
    ANIM->ghost = 0.0;
    ANIM->brightness = 0.0;
    for (;;) {
        for (F->i = 0; F->i < 4; F->i++) {
            COSTUME(ANIM, WALK_CYCLE[F->i]);
            WAIT(F, 0.01);
        }
    }
    KOKI_END(F);
}

static koki_step s_anim_runnn_jump(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        if (KEY(Z)) {
            ANIM->ghost = 0.0;
            ANIM->brightness = 0.0;
            STOPOTHER(ANIM);
            BC("jumpRUN");
            COSTUME(ANIM, "costume10");
            WAIT(F, 1.1);
            COSTUME(ANIM, "costume11");
            WAIT(F, 0.05);
            STOPOTHER(ANIM);
            BC("RUNNN");
        }
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_jumprun_follow(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        koki_goto_sprite(ANIM, PLAYER);
        YIELD(F);
    }
    KOKI_END(F);
}

static koki_step s_anim_finalcut(koki_frame *F)
{
    KOKI_BEGIN(F);
    ANIM->y = -75.0;
    COSTUME(ANIM, "door");
    WAIT(F, 0.05);
    BC("temporary hit");
    koki_play(ANIM, "hit2");
    COSTUME(ANIM, "OOF");
    GLIDE(F, ANIM, 0.1, 135, -75);
    COSTUME(ANIM, "costume2");
    WAIT(F, 1);
    KOKI_END(F);
}

static koki_step s_anim_cuthit(koki_frame *F)
{
    KOKI_BEGIN(F);
    ANIM->y = -75.0;
    BC("temporaryhit2");
    koki_play(ANIM, "hit2");
    koki_play(ANIM, "slide");
    COSTUME(ANIM, "costume11");
    koki_goto(ANIM, 135, -75);
    GLIDE(F, ANIM, 0.2, -200, -75);
    COSTUME(ANIM, "OOF");
    WAIT(F, 0.05);
    COSTUME(ANIM, "costume2");
    WAIT(F, 1);
    KOKI_END(F);
}

static koki_step s_anim_en4dmg(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_play(ANIM, "Fall2");
    COSTUME(ANIM, "door");
    WAIT(F, 0.1);
    COSTUME(ANIM, "costume2");
    KOKI_END(F);
}

static koki_step s_anim_ending_walk(koki_frame *F)
{
    KOKI_BEGIN(F);
    koki_show(ANIM);
    koki_goto(ANIM, -243, -75);
    GLIDE(F, ANIM, 20, 250, -75);
    STOPOTHER(ANIM);
    BC("the end");
    koki_hide(ANIM);
    KOKI_END(F);
}

static koki_step s_anim_ending_anim(koki_frame *F)
{
    KOKI_BEGIN(F);
    for (;;) {
        for (F->i = 0; F->i < 4; F->i++) {
            COSTUME(ANIM, WALK_CYCLE[F->i]);
            WAIT(F, 0.1);
        }
    }
    KOKI_END(F);
}

static koki_step s_anim_lv3_dance(koki_frame *F)
{
    KOKI_BEGIN(F);
    STOPOTHER(ANIM);
    koki_show(ANIM);
    koki_goto(ANIM, 90, -75);
    for (F->i = 0; F->i < 3; F->i++) {
        COSTUME(ANIM, "costume11");
        WAIT(F, 0.3);
        COSTUME(ANIM, "costume3");
        WAIT(F, 0.3);
    }
    KOKI_END(F);
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

/* The nine messages that stop the Stage's music. */
static const char *const STOPMUSIC_MSGS[9] = {"oofie",       "enemy1 oof", "planeoofie",
                                              "enemy2 oof",  "game over",  "falloofie",
                                              "enemy 4 oof", "the end",    "stopmusic"};

/* The seven messages that put the Player away. */
static const char *const PLAYER_OFF_MSGS[7] = {"turn anim",     "oofie",          "falloofie",
                                               "disableplayer", "final cutscene", "ending cutscene",
                                               "stopmusic"};

void koki_register_boot(koki_engine *eng)
{
    size_t i;

    ON("flag", NULL, s_stage_flag);
    ON("start intro", NULL, s_stage_intro);
    ON("go to lobby", NULL, s_stage_lobby);
    ON("startlv1", NULL, s_stage_lv1);
    ON("startlv2", NULL, s_stage_lv2);
    ON("startlv3", NULL, s_stage_lv3);
    ON("startfinallevel", NULL, s_stage_final);
    ON("ending cutscene", NULL, s_stage_ending);
    ON("boxing bell", NULL, s_stage_bell);
    for (i = 0u; i < ND_ARRAY_LEN(STOPMUSIC_MSGS); i++)
        ON(STOPMUSIC_MSGS[i], NULL, s_stage_stop);
    ON("final cutscene", NULL, s_stage_finalcut);

    ON("flag", LOGO, s_logo);
    ON("start intro", INTRO, s_intro);
    ON("start game", INTRO, s_intro_out);
    ON("start button enable", STARTBTN, s_btn_anim);
    ON("start button enable", STARTBTN, s_btn_input);
    ON("start game", PANEL, s_panel);

    ON("PlayerEnable", WHITE, s_white_fade);
    ON("start intro", WHITE, s_white_fade);
    ON("whitechange", WHITE, s_white_change);
    ON("the end", WHITE, s_white_end);

    ON("go to lobby", PLAT, s_plat_lobby);
    ON("level1", PLAT, s_plat_lv1);
    ON("startlv2", PLAT, s_plat_lv2);
    ON("level3", PLAT, s_plat_lv3);
    ON("ending cutscene", PLAT, s_plat_end);

    ON("PlayerEnable", PLAYER, s_player_physics);
    ON("RUNenable", PLAYER, s_player_run);
    for (i = 0u; i < ND_ARRAY_LEN(PLAYER_OFF_MSGS); i++)
        ON(PLAYER_OFF_MSGS[i], PLAYER, s_player_off);
    ON("rightdash", PLAYER, s_player_rdash);
    ON("leftdash", PLAYER, s_player_ldash);

    ON("PlayerEnable", ANIM, s_anim_follow);
    ON("PlayerEnable", ANIM, s_anim_reset);
    ON("PlayerEnable", ANIM, s_anim_walk);
    ON("PlayerEnable", ANIM, s_anim_jump);
    ON("PlayerEnable", ANIM, s_anim_idle);
    ON("PlayerEnable", ANIM, s_anim_face);
    ON("idleanim", ANIM, s_anim_idle_set);
    ON("turn anim", ANIM, s_anim_door);
    ON("oofie", ANIM, s_anim_oofie);
    ON("take damage", NULL, s_dmg_gate);
    ON("koki hurt", ANIM, s_anim_dmg_sound);
    ON("koki hurt", ANIM, s_anim_dmg_flash);
    ON("dodge", ANIM, s_anim_dodge);
    ON("disableplayer", ANIM, s_anim_disable);
    ON("falloofie", ANIM, s_anim_fall);
    ON("RUNenable", ANIM, s_anim_run_follow);
    ON("RUNenable", ANIM, s_anim_run_start);
    ON("RUNNN", ANIM, s_anim_runnn_follow);
    ON("RUNNN", ANIM, s_anim_runnn_anim);
    ON("RUNNN", ANIM, s_anim_runnn_jump);
    ON("jumpRUN", ANIM, s_anim_jumprun_follow);
    ON("final cutscene", ANIM, s_anim_finalcut);
    ON("cutscenehit", ANIM, s_anim_cuthit);
    ON("enemy4 damage", ANIM, s_anim_en4dmg);
    ON("ending cutscene", ANIM, s_anim_ending_walk);
    ON("ending cutscene", ANIM, s_anim_ending_anim);
    ON("stopmusic", ANIM, s_anim_lv3_dance);
    ND_UNUSED(eng);
}

/* ------------------------------------------------------------------ *
 * register_all
 * ------------------------------------------------------------------ */

/* The sprite creation order, which is also the pre-set_layer_order list
 * order. `Stuff` is a target in the manifest and is NEVER created: creating
 * it would add a 46th sprite to the draw list. */
_Static_assert(KOKI_C_COUNT == 45, "the cast is 45 sprites");

static const char *const CAST_ORDER[KOKI_C_COUNT] = {
    "Player",       "CharacterAnim", "Platform",     "Dynaris Logo",   "intro",      "StartButton",
    "Sprite1",      "White",         "Door1",        "Door2",          "Door3",      "Door4",
    "Enemy 1",      "KokiStats",     "GameOver",     "Shockwave",      "Shockwave2", "Enemy1Stats",
    "Cannon",       "Cannon ball",   "QuickPress",   "KokiPlaneStats", "cutscene1",  "PlaneChar",
    "Enemy2",       "Enemy2Stats",   "Cannon2",      "Gas tank",       "A to Dodge", "Sprite2",
    "Abyss",        "Enemy 3",       "Lives",        "Enemy3Stats",    "Cannon3",    "Cannon ball2",
    "Enemy4Stats",  "EvilCannon",    "Cannon ball3", "Shockwave3",     "Shockwave4", "Shockwave5",
    "Cannon ball4", "Reward",        "Riby",
};

/* The paint order from the original project, back -> front. FORTY-FOUR names
 * for forty-five sprites: Enemy4Stats is missing, so it sorts to 999 and
 * draws in front of everything, White included. Visible on the final-boss
 * screen, and reproduced on purpose. */
static const char *const LAYER_ORDER[44] = {
    "Door4",        "Platform",       "Door1",        "Door2",        "PlaneChar",  "Door3",
    "cutscene1",    "Cannon ball",    "Cannon ball2", "Abyss",        "intro",      "StartButton",
    "Enemy1Stats",  "KokiPlaneStats", "KokiStats",    "Sprite1",      "Shockwave2", "Shockwave5",
    "Shockwave4",   "Shockwave3",     "Shockwave",    "Cannon",       "QuickPress", "Enemy2",
    "Cannon2",      "Gas tank",       "Lives",        "Enemy 3",      "Player",     "Cannon3",
    "Enemy3Stats",  "CharacterAnim",  "GameOver",     "Enemy 1",      "Riby",       "A to Dodge",
    "Enemy2Stats",  "EvilCannon",     "Cannon ball4", "Cannon ball3", "Reward",     "Sprite2",
    "Dynaris Logo", "White",
};

static void read_atk(void)
{
    const char *env = getenv("NEODCT_KOKI_ATTACK_SLOW");
    char *end = NULL;
    double v;

    KOKI_ATK = 1.35;
    if (env == NULL || env[0] == '\0')
        return;
    v = strtod(env, &end);
    if (end == env || (end != NULL && *end != '\0'))
        return; /* float() raised: keep 1.35 */
    /* `if not (ATK > 0)` rejects 0 (which would divide by zero at 20/ATK),
     * negatives, and NaN -- the comparison is written this way round so that
     * NaN falls through it. */
    if (!(v > 0.0))
        return;
    KOKI_ATK = v;
}

void koki_register_all(koki_engine *eng)
{
    size_t i;

    if (eng == NULL)
        return;
    KE = eng;
    memset(KG, 0, sizeof KG);
    read_atk();

    /* "when flag clicked" defaults. The last two are the keys game.py creates
     * lazily with V.get(k, -99); pre-setting them is identical behaviour and
     * removes the only two places a lookup could miss. */
    eng->vars[KOKI_V_LIVES] = 3;
    eng->vars[KOKI_V_DOORS] = 1;
    eng->vars[KOKI_V_TAKEN_DAMAGE] = 0;
    eng->vars[KOKI_V_KNOCKOUTS] = 0;
    eng->vars[KOKI_V_HAS_HEALED] = 0;
    eng->vars[KOKI_V_CANNONDEFEATS] = 1;
    eng->vars[KOKI_V_RIBYDANGER] = 0;
    eng->vars[KOKI_V_EVILCANONBALLDIRECTION] = -90;
    eng->vars[KOKI_V_HEALWAVEDIRECTION] = 2;
    eng->vars[KOKI_V_DAMAGEWAY4] = 2;
    eng->vars[KOKI_V_HURT_T] = -99;
    eng->vars[KOKI_V_PLANE_HURT_T] = -99;

    /* The cast, in creation order -- which is also the pre-set_layer_order
     * list order. koki_cast_id is in the same order as CAST_ORDER, and the
     * assertion above is what keeps them that way. */
    for (i = 0u; i < ND_ARRAY_LEN(CAST_ORDER); i++)
        KG[i] = koki_sprite_get(eng, CAST_ORDER[i]);

    koki_set_layer_order(eng, LAYER_ORDER, ND_ARRAY_LEN(LAYER_ORDER));

    koki_register_boot(eng);
    koki_register_lobby(eng);
    koki_register_lv1(eng);
    koki_register_lv2(eng);
    koki_register_lv3(eng);
    koki_register_final(eng);
    koki_register_ending(eng);
}
