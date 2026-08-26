/* koki_sched.h -- Python generators, in C, for 500 suspension points.
 *
 * The whole port hangs off this file; README-PORT.md argues the choice and
 * this header is the mechanism. A Scratch script stops in the middle of a
 * statement and carries on next frame. Python spells that `yield`. C has
 * nothing, so a script becomes an ordinary function that RETURNS at each
 * suspension and re-enters at the same place next time, using a switch whose
 * case labels the compiler numbers for us:
 *
 *     static koki_step s_logo(koki_frame *F)
 *     {
 *         KOKI_BEGIN(F);
 *         LOGO->ghost = 100;
 *         for (F->i = 0; F->i < 20; F->i++) { LOGO->ghost -= 5; WAIT(F, 0.01); }
 *         KOKI_END(F);
 *     }
 *
 * ============ THE ONE RULE ============
 *
 * NO C STACK LOCAL MAY SURVIVE A SUSPENSION. Every loop counter, saved
 * coordinate and temporary that is read after a KOKI_YIELD/KOKI_WAIT/
 * KOKI_CALL lives in koki_frame, never in a local. A local that breaks this
 * is not a compile error -- it silently resets to garbage on resume, and the
 * symptom is a boss that occasionally skips an attack. That is why the frame
 * has NAMED fields rather than a void* blob: `F->i` reads as a loop counter
 * at the use site, and a reviewer can see the rule being followed.
 *
 * ============ THE FRAME STACK ============
 *
 * `F` is not one frame, it is the base of a small array. F[0] is this
 * function's own state; F[1] belongs to whatever it calls; F[2] to that
 * function's callee. So the engine's own generators (wait, glide,
 * play_until_done) and the game's shared sub-scripts (_flash,
 * _white_fade_out, _door_interact) are simply protothreads invoked at F + 1,
 * and Python's `yield from` becomes KOKI_CALL.
 *
 * Measured maximum nesting in game.py is 3
 * (_riby_damaged -> _riby_dash_sweeps -> _riby_laugh -> wait).
 * KOKI_STACK_DEPTH is 5.
 *
 * ============ WHY KOKI_FALLTHROUGH EXISTS ============
 *
 * spec-koki.md section 4.4 sketches KOKI_CALL without it, and that sketch
 * does not build here: -Wextra turns on -Wimplicit-fallthrough, and in the
 * call macro the `case` label follows a plain assignment rather than a
 * `return`, so control really can fall into it. KOKI_YIELD is unaffected
 * because a `return` sits immediately before its label. The attribute says
 * "yes, on purpose".
 *
 * ============ WHAT A SCRIPT MAY NOT CONTAIN ============
 *
 * A `switch` around a suspension point -- Duff's device does not nest. All
 * 304 handlers in game.py are if/while/for only, so nothing had to be bent.
 */

#ifndef KOKI_SCHED_H_INCLUDED
#define KOKI_SCHED_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { KOKI_YIELDED = 0, KOKI_DONE = 1 } koki_step;

/* One suspension point's worth of state, for one function.
 *
 * The fields are deliberately general: `i`/`j` are the two repeat counters a
 * script can need at once, `t0`/`x0`/`y0`/`tx`/`ty` are exactly what wait()
 * and glide() keep, and `s0`/`s1` are the two spare doubles a handful of
 * scripts need for a saved coordinate. A frame is never simultaneously its
 * own script's and a child's -- the child gets F[1] -- so the reuse is safe.
 *
 * 4 + 4 + 4 + 7*8 = 68 bytes, 72 with padding, so one script's whole frame
 * stack is 360 bytes and the 304 handlers together are 107 KB -- allocated
 * once at registration and never grown. */
typedef struct {
    int32_t pc; /* resume point: the __LINE__ that suspended, 0 = start */
    int32_t i;  /* outer repeat counter                                */
    int32_t j;  /* inner repeat counter                                */
    double t0;  /* wait: deadline. glide: start time.                  */
    double x0;  /* glide: start x                                      */
    double y0;  /* glide: start y                                      */
    double tx;  /* glide: target x                                     */
    double ty;  /* glide: target y                                     */
    double s0;  /* script scratch                                      */
    double s1;  /* script scratch                                      */
} koki_frame;

#define KOKI_STACK_DEPTH 5

#if defined(__GNUC__)
#define KOKI_FALLTHROUGH __attribute__((fallthrough))
#else
#define KOKI_FALLTHROUGH ((void)0)
#endif

/* ------------------------------------------------------------------ *
 * The trampoline
 * ------------------------------------------------------------------ */

#define KOKI_BEGIN(F)  \
    switch ((F)->pc) { \
    case 0:

/* A bare `yield`: one frame. */
#define KOKI_YIELD(F)        \
    do {                     \
        (F)->pc = __LINE__;  \
        return KOKI_YIELDED; \
    case __LINE__:;          \
    } while (0)

/* Falling off the end of a generator is StopIteration, which the scheduler
 * turns into dead. pc = -1 makes a re-entry after completion land on no case
 * label and run the tail again -- which cannot happen, because the scheduler
 * never steps a finished script, and if it ever did the -1 would be visible
 * in a debugger rather than silently restarting from the top. */
#define KOKI_END(F) \
    }               \
    (F)->pc = -1;   \
    return KOKI_DONE

/* An early `return` from the middle of a script. */
#define KOKI_RETURN(F)    \
    do {                  \
        (F)->pc = -1;     \
        return KOKI_DONE; \
    } while (0)

/* `yield from child(...)`. The child runs on F[1]; its arguments are
 * re-evaluated on every resume, so pass constants or stable pointers only
 * (README-PORT.md, cost 4). */
#define KOKI_CALL(F, CALL_EXPR)          \
    do {                                 \
        (F)[1].pc = 0;                   \
        (F)->pc = __LINE__;              \
        KOKI_FALLTHROUGH;                \
    case __LINE__:                       \
        if ((CALL_EXPR) == KOKI_YIELDED) \
            return KOKI_YIELDED;         \
    } while (0)

/* There is deliberately NO "repeat n times" macro. Python's
 * `for _ in range(n)` becomes a plain
 *
 *     for (F->i = 0; F->i < n; F->i++) { ... }
 *
 * at every one of its sixty-six call sites, because the counter living in
 * the FRAME rather than on the stack is exactly the thing a reader must not
 * miss, and a macro would hide it. F->j is the inner counter when two loops
 * nest around a suspension.
 *
 */

#ifdef __cplusplus
}
#endif

#endif /* KOKI_SCHED_H_INCLUDED */
