/* koki_game.h -- what the 304 game scripts are written against.
 *
 * game.py is one 2,817-line function, register_all(eng), that closes over
 * `eng`, forty-five sprite handles, a variable table and two tuning knobs.
 * A C file cannot close over anything, so those become file-scope objects
 * shared by the seven koki_game_*.c files, and the macros below make the
 * transcription line-for-line checkable against the Python beside it:
 *
 *      Python                                C
 *      yield from W(0.3)                     WAIT(F, 0.3);
 *      yield                                 YIELD(F);
 *      yield from EN1.glide(0.4, 0, -60)     GLIDE(F, EN1, 0.4, 0, -60);
 *      eng.broadcast("startlv1")             BC("startlv1");
 *      if key("z"):                          if (KEY(Z)) {
 *      V["lives"] -= 1                       V(LIVES) -= 1;
 *      EN1.set_costume("costume3")           COSTUME(EN1, "costume3");
 *
 * That correspondence is the whole review strategy for this half of the port,
 * so the macros are deliberately short and deliberately shaped like the
 * source. They are confined to the Koki app's own translation units.
 *
 * ============ WHY THE SPRITE NAMES ARE MACROS ============
 *
 * PLAYER, ANIM, EN1 and the rest are macros onto one array of sprite
 * pointers, so there is a single definition and a single initialisation site,
 * and they keep the Python's short names -- a reviewer reading the two files
 * side by side is not also translating identifiers. The cost is 45 very
 * ordinary words that must not be used as local variables anywhere in
 * apps/Koki, which the compiler points out loudly the moment anyone tries.
 *
 * ============ THE FRAME ARGUMENT ============
 *
 * Every script is `static koki_step name(koki_frame *F)`. F[0] is its own
 * suspension state; the WAIT/GLIDE/SUB macros run their callee on F[1], and
 * a sub-script that itself waits uses F[2]. See koki_sched.h for the rule
 * that no C local may survive a suspension.
 */

#ifndef KOKI_GAME_H_INCLUDED
#define KOKI_GAME_H_INCLUDED

#include "koki.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The cast
 * ------------------------------------------------------------------ */

/* The cast is an ARRAY with named indices rather than a struct of 45
 * pointers, so that koki_register_all() can fill it from one name table with
 * a loop and the two lists cannot drift apart. Indexing across the members of
 * a struct would have been undefined behaviour even though the members are
 * all the same type; this is the honest version of that trick. */
typedef enum {
    KOKI_C_PLAYER = 0,
    KOKI_C_ANIM,
    KOKI_C_PLAT,
    KOKI_C_LOGO,
    KOKI_C_INTRO,
    KOKI_C_STARTBTN,
    KOKI_C_PANEL,
    KOKI_C_WHITE,
    KOKI_C_DOOR1,
    KOKI_C_DOOR2,
    KOKI_C_DOOR3,
    KOKI_C_DOOR4,
    KOKI_C_EN1,
    KOKI_C_KSTAT,
    KOKI_C_GOVER,
    KOKI_C_SW1,
    KOKI_C_SW2,
    KOKI_C_E1STAT,
    KOKI_C_CANNON,
    KOKI_C_CBALL,
    KOKI_C_QUICK,
    KOKI_C_KPSTAT,
    KOKI_C_CUT1,
    KOKI_C_PLANE,
    KOKI_C_EN2,
    KOKI_C_E2STAT,
    KOKI_C_CANNON2,
    KOKI_C_GAS,
    KOKI_C_DODGE,
    KOKI_C_SCORE,
    KOKI_C_ABYSS,
    KOKI_C_EN3,
    KOKI_C_LIVES,
    KOKI_C_E3STAT,
    KOKI_C_CANNON3,
    KOKI_C_CBALL2,
    KOKI_C_E4STAT,
    KOKI_C_EVILC,
    KOKI_C_CBALL3,
    KOKI_C_SW3,
    KOKI_C_SW4,
    KOKI_C_SW5,
    KOKI_C_CBALL4,
    KOKI_C_REWARD,
    KOKI_C_RIBY,
    KOKI_C_COUNT
} koki_cast_id;

extern koki_sprite *KG[KOKI_C_COUNT];
extern koki_engine *KE;

extern double KOKI_ATK;
#define KOKI_IFRAMES 0.9

#define PLAYER   (KG[KOKI_C_PLAYER])
#define ANIM     (KG[KOKI_C_ANIM])
#define PLAT     (KG[KOKI_C_PLAT])
#define LOGO     (KG[KOKI_C_LOGO])
#define INTRO    (KG[KOKI_C_INTRO])
#define STARTBTN (KG[KOKI_C_STARTBTN])
#define PANEL    (KG[KOKI_C_PANEL])
#define WHITE    (KG[KOKI_C_WHITE])
#define DOOR1    (KG[KOKI_C_DOOR1])
#define DOOR2    (KG[KOKI_C_DOOR2])
#define DOOR3    (KG[KOKI_C_DOOR3])
#define DOOR4    (KG[KOKI_C_DOOR4])
#define EN1      (KG[KOKI_C_EN1])
#define KSTAT    (KG[KOKI_C_KSTAT])
#define GOVER    (KG[KOKI_C_GOVER])
#define SW1      (KG[KOKI_C_SW1])
#define SW2      (KG[KOKI_C_SW2])
#define E1STAT   (KG[KOKI_C_E1STAT])
#define CANNON   (KG[KOKI_C_CANNON])
#define CBALL    (KG[KOKI_C_CBALL])
#define QUICK    (KG[KOKI_C_QUICK])
#define KPSTAT   (KG[KOKI_C_KPSTAT])
#define CUT1     (KG[KOKI_C_CUT1])
#define PLANE    (KG[KOKI_C_PLANE])
#define EN2      (KG[KOKI_C_EN2])
#define E2STAT   (KG[KOKI_C_E2STAT])
#define CANNON2  (KG[KOKI_C_CANNON2])
#define GAS      (KG[KOKI_C_GAS])
#define DODGE    (KG[KOKI_C_DODGE])
#define SCORE    (KG[KOKI_C_SCORE])
#define ABYSS    (KG[KOKI_C_ABYSS])
#define EN3      (KG[KOKI_C_EN3])
#define LIVES    (KG[KOKI_C_LIVES])
#define E3STAT   (KG[KOKI_C_E3STAT])
#define CANNON3  (KG[KOKI_C_CANNON3])
#define CBALL2   (KG[KOKI_C_CBALL2])
#define E4STAT   (KG[KOKI_C_E4STAT])
#define EVILC    (KG[KOKI_C_EVILC])
#define CBALL3   (KG[KOKI_C_CBALL3])
#define SW3      (KG[KOKI_C_SW3])
#define SW4      (KG[KOKI_C_SW4])
#define SW5      (KG[KOKI_C_SW5])
#define CBALL4   (KG[KOKI_C_CBALL4])
#define REWARD   (KG[KOKI_C_REWARD])
#define RIBY     (KG[KOKI_C_RIBY])

/* ------------------------------------------------------------------ *
 * The transcription vocabulary
 * ------------------------------------------------------------------ */

#define YIELD(F)             KOKI_YIELD(F)
#define WAIT(F, T)           KOKI_CALL(F, koki_wait(&(F)[1], KE, (T)))
#define WAIT_UNTIL(F, P)     KOKI_CALL(F, koki_wait_until(&(F)[1], KE, (P)))
#define GLIDE(F, S, T, X, Y) KOKI_CALL(F, koki_glide(&(F)[1], (S), (T), (X), (Y)))
#define GLIDE_TO(F, S, T, O) KOKI_CALL(F, koki_glide_to(&(F)[1], (S), (T), (O)))
#define PLAY_UNTIL(F, S, N)  KOKI_CALL(F, koki_play_until_done(&(F)[1], (S), (N)))

/* A shared sub-script, run one frame deeper. SUB1/SUB2 pass its arguments;
 * remember that those are re-evaluated on every resume, so they must be
 * constants or stable pointers (README-PORT.md, cost 4). */
#define SUB(F, FN)        KOKI_CALL(F, FN(&(F)[1]))
#define SUB1(F, FN, A)    KOKI_CALL(F, FN(&(F)[1], (A)))
#define SUB2(F, FN, A, B) KOKI_CALL(F, FN(&(F)[1], (A), (B)))

#define BC(M)            koki_broadcast(KE, (M))
#define STOPOTHER(S)     koki_stop_other_scripts(KE, (S))
#define KEY(K)           koki_key(KE, KOKI_KEY_##K)
#define RND(A, B)        koki_randint(KE, (A), (B))
#define V(N)             (KE->vars[KOKI_V_##N])
#define COSTUME(S, N)    koki_set_costume((S), (N))
#define IS_COSTUME(S, N) koki_costume_is((S), (N))
#define TOUCHING(A, B)   koki_touching((A), (B), 0.0)
#define BACKDROP(N)      koki_backdrop(KE, (N))
#define ON(EV, SPR, FN)  (void)koki_on(KE, (EV), (SPR), (FN))

/* ------------------------------------------------------------------ *
 * Shared sub-scripts (game.py's module-level helpers)
 * ------------------------------------------------------------------ */

/* The flash game.py writes out longhand in six places: alternate
 * ghost/brightness 50 and 0, `times` times, half a frame-pair apart. Shared
 * here rather than copied; the two assignments per step are order-
 * independent, so this is byte-identical to every one of those six. */
koki_step koki_g_flash(koki_frame *F, koki_sprite *s, int32_t times);

/* "_white_fade_out": show White opaque at the front, fade it out over 20
 * frames, hide it. */
koki_step koki_g_white_fade_out(koki_frame *F);

/* "_door_flash": forever, alternate a door's two costumes every 0.3 s. */
koki_step koki_g_door_flash(koki_frame *F, koki_sprite *door);

/* Per-section registration, called in game.py's textual order. That order
 * decides the initial run order and the stop_other_scripts grouping, so it
 * is part of the contract. */
void koki_register_boot(koki_engine *eng);  /* Stage, logo, intro, button, panel,
                                              * White, Platform, Player, CharacterAnim */
void koki_register_lobby(koki_engine *eng); /* lives, health bar, game over, doors */
void koki_register_lv1(koki_engine *eng);
void koki_register_lv2(koki_engine *eng);
void koki_register_lv3(koki_engine *eng);
void koki_register_final(koki_engine *eng);
void koki_register_ending(koki_engine *eng);

#ifdef __cplusplus
}
#endif

#endif /* KOKI_GAME_H_INCLUDED */
