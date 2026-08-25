/* koki_sched.c -- Scratch's scheduler: registration, broadcast, one frame.
 *
 * The second thing in this port that does not survive a naive translation.
 * Scratch's rules, all four of them load-bearing:
 *
 *   1. broadcast(m) RESTARTS every handler listening for m, from the top,
 *      even one that is halfway through.
 *   2. `active` is a Python dict, and its INSERTION ORDER IS THE EXECUTION
 *      ORDER. Assigning to a key that is present keeps its position;
 *      a key that died and comes back is appended at the END. Run order is
 *      therefore dynamic and the game depends on it.
 *   3. The per-frame pass steps a SNAPSHOT taken before anything runs, so a
 *      script started during frame N first runs during frame N+1. Every
 *      broadcast in the game costs one frame of latency, including the
 *      "instant" handlers written as `...; return; yield`.
 *   4. stop_other_scripts(sprite) kills every script whose SPRITE matches,
 *      whatever message started it, except the one currently running.
 *
 * ============ WHY EACH HANDLER GETS EXACTLY ONE SLOT ============
 *
 * Python allocates a fresh Script object per start, and rule 3's snapshot
 * holds the OLD object while `active[key]` already points at the new one --
 * so "is this the instance I snapshotted?" is object identity. One slot per
 * handler is much cheaper (no allocation in a broadcast, and a broadcast can
 * restart thirty scripts) but loses that identity, and losing it would step
 * the restarted instance in the frame that started it, collapsing rule 3.
 *
 * A GENERATION COUNTER restores it: every (re)start bumps it, and the
 * snapshot records the generation it saw. That is object identity, in 4
 * bytes, and it is the reason a restart mid-frame behaves exactly as the
 * Python's does.
 */

#include <stdio.h>
#include <string.h>

#include "nd_log.h"
#include "nd_types.h"

#include "koki.h"

/* ------------------------------------------------------------------ *
 * The active list -- insertion-ordered, threaded through the slots
 * ------------------------------------------------------------------ */

static void active_append(koki_engine *eng, koki_slot *s)
{
    s->prev = eng->active_tail;
    s->next = NULL;
    if (eng->active_tail != NULL)
        eng->active_tail->next = s;
    else
        eng->active_head = s;
    eng->active_tail = s;
    s->live = true;
}

static void active_unlink(koki_engine *eng, koki_slot *s)
{
    if (s->prev != NULL)
        s->prev->next = s->next;
    else
        eng->active_head = s->next;
    if (s->next != NULL)
        s->next->prev = s->prev;
    else
        eng->active_tail = s->prev;
    s->prev = NULL;
    s->next = NULL;
    s->live = false;
}

size_t koki_active_count(const koki_engine *eng)
{
    const koki_slot *s;
    size_t n = 0u;

    if (eng == NULL)
        return 0u;
    for (s = eng->active_head; s != NULL; s = s->next)
        n++;
    return n;
}

/* ------------------------------------------------------------------ *
 * Registration
 * ------------------------------------------------------------------ */

static int32_t event_index(koki_engine *eng, const char *event)
{
    size_t i;

    for (i = 0u; i < eng->n_events; i++) {
        if (strcmp(eng->events[i], event) == 0)
            return (int32_t)i;
    }
    if (eng->n_events >= KOKI_MAX_EVENTS)
        return -1;
    (void)nd_strlcpy(eng->events[eng->n_events], event, KOKI_NAME_MAX);
    eng->event_first[eng->n_events] = -1;
    eng->event_last[eng->n_events] = -1;
    eng->n_events++;
    return (int32_t)(eng->n_events - 1u);
}

int32_t koki_on(koki_engine *eng, const char *event, koki_sprite *sprite, koki_script_fn fn)
{
    int32_t ev;
    int32_t idx;
    koki_slot *s;

    if (eng == NULL || event == NULL || fn == NULL)
        return -1;
    if (eng->n_slots >= KOKI_MAX_HANDLERS) {
        nd_log_err(ND_LOG_KOKI, "handler table full at %d", KOKI_MAX_HANDLERS);
        return -1;
    }
    ev = event_index(eng, event);
    if (ev < 0) {
        nd_log_err(ND_LOG_KOKI, "event table full at %d", KOKI_MAX_EVENTS);
        return -1;
    }

    idx = (int32_t)eng->n_slots;
    s = &eng->slots[idx];
    memset(s, 0, sizeof *s);
    s->key = idx + 1; /* _hkey starts at 1 */
    s->event = ev;
    s->sprite = sprite;
    s->fn = fn;
    s->next_in_event = -1;
    eng->n_slots++;

    /* Appended, so a chain is in registration order -- which is what decides
     * the initial run order and the stop_other_scripts grouping. */
    if (eng->event_last[ev] < 0)
        eng->event_first[ev] = idx;
    else
        eng->slots[eng->event_last[ev]].next_in_event = idx;
    eng->event_last[ev] = idx;

    return s->key;
}

/* ------------------------------------------------------------------ *
 * Broadcast
 * ------------------------------------------------------------------ */

void koki_broadcast(koki_engine *eng, const char *event)
{
    int32_t ev;
    int32_t idx;
    size_t i;

    if (eng == NULL || event == NULL)
        return;

    ev = -1;
    for (i = 0u; i < eng->n_events; i++) {
        if (strcmp(eng->events[i], event) == 0) {
            ev = (int32_t)i;
            break;
        }
    }
    if (ev < 0)
        return; /* handlers.get(event, ()) -- an unheard message is not an error */

    for (idx = eng->event_first[ev]; idx >= 0; idx = eng->slots[idx].next_in_event) {
        koki_slot *s = &eng->slots[idx];

        /* Restart: the running instance dies, a new one takes its place in
         * the SAME position in `active`. A slot that is not live is appended
         * at the end, which is Python's dict-insert rule.
         *
         * HAZARD, for anyone adding a script: this zeroes the frame stack
         * IMMEDIATELY, so a script that broadcasts a message it is itself a
         * handler for loses its own loop counters and saved coordinates at
         * that instant -- Python's old generator keeps its locals, this
         * one does not. Both scripts in game.py that do it (jumpatkriby,
         * RUNNN) touch nothing afterwards, so it does not bite; a new one
         * that read F->i after such a broadcast would read zero. */
        s->generation++;
        s->dead = false;
        memset(s->stack, 0, sizeof s->stack);
        if (!s->live)
            active_append(eng, s);
    }
}

void koki_start_flag(koki_engine *eng)
{
    koki_broadcast(eng, "flag");
}

void koki_stop_other_scripts(koki_engine *eng, const koki_sprite *sprite)
{
    koki_slot *s;

    if (eng == NULL)
        return;
    for (s = eng->active_head; s != NULL; s = s->next) {
        if (s->sprite == sprite && s != eng->current)
            s->dead = true;
    }
}

void koki_stop_all_scripts(koki_engine *eng)
{
    koki_slot *s;

    if (eng == NULL)
        return;
    for (s = eng->active_head; s != NULL; s = s->next)
        s->dead = true;
}

koki_sprite *koki_script_sprite(const koki_engine *eng)
{
    if (eng == NULL || eng->current == NULL)
        return NULL;
    return eng->current->sprite;
}

const char *koki_script_event(const koki_engine *eng)
{
    if (eng == NULL || eng->current == NULL)
        return "";
    if (eng->current->event < 0 || (size_t)eng->current->event >= eng->n_events)
        return "";
    return eng->events[eng->current->event];
}

/* ------------------------------------------------------------------ *
 * One frame
 * ------------------------------------------------------------------ */

void koki_step_frame(koki_engine *eng)
{
    /* The snapshot is FUNCTION-STATIC rather than on the stack: 400 entries
     * is 4.8 KB, and CODING-STANDARDS.md section 1.5 keeps anything this
     * size off a stack that also has to hold the render path. Static is safe
     * here because this function is never re-entered -- a script cannot step
     * a frame -- and two engines alternating frames each finish with the
     * buffer before the other touches it. */
    static koki_slot *snap_slot[KOKI_MAX_HANDLERS];
    static uint32_t snap_gen[KOKI_MAX_HANDLERS];
    size_t n = 0u;
    size_t i;
    koki_slot *s;
    koki_slot *next;

    if (eng == NULL)
        return;

    for (s = eng->active_head; s != NULL && n < KOKI_MAX_HANDLERS; s = s->next) {
        snap_slot[n] = s;
        snap_gen[n] = s->generation;
        n++;
    }

    for (i = 0u; i < n; i++) {
        uint32_t gen_before;
        koki_step rc;

        s = snap_slot[i];
        /* Three ways this entry is stale, and all three are the Python's
         * `if sc.dead: continue` in disguise: the script was stopped, it was
         * restarted by a broadcast earlier in this very pass (generation
         * moved), or the slot left `active` altogether. */
        if (s->dead || s->generation != snap_gen[i] || !s->live)
            continue;

        gen_before = s->generation;
        eng->current = s;
        rc = s->fn(s->stack);

        if (s->generation != gen_before) {
            /* THE SCRIPT RESTARTED ITSELF. It broadcast a message it is
             * itself a handler for, which in Python put a BRAND-NEW Script
             * object at this key -- and everything the old generator did
             * afterwards (its yield, or its return) landed on the OLD
             * object, leaving the new one untouched and ready to run from
             * the top next frame.
             *
             * One slot per handler cannot hold two instances, so the restart
             * wins: whatever the old body wrote into the frame stack after
             * the broadcast -- the resume line from its KOKI_YIELD, the -1
             * from its KOKI_END -- is thrown away, and StopIteration is NOT
             * applied to the slot, because it belongs to an instance that no
             * longer exists.
             *
             * This is not a corner case. koki_game_final.c's
             * s_riby_jump_attacks ends by broadcasting "jumpatkriby", which
             * is the message it is registered against, and game.py's comment
             * says why: without that self-restart the final fight soft-locks
             * with Riby inert. Marking the slot dead here would put the
             * soft-lock straight back. */
            memset(s->stack, 0, sizeof s->stack);
        } else if (rc == KOKI_DONE) {
            s->dead = true; /* StopIteration */
        }
    }
    eng->current = NULL;

    /* The sweep. Python builds the dead key list and re-checks each one,
     * because a restart may already have replaced it; with one slot per key
     * a restart clears `dead`, so a single pass removing the still-dead
     * entries is the same thing. */
    for (s = eng->active_head; s != NULL; s = next) {
        next = s->next;
        if (s->dead)
            active_unlink(eng, s);
    }
}

/* ------------------------------------------------------------------ *
 * The engine's two waiting generators
 * ------------------------------------------------------------------ */

koki_step koki_wait(koki_frame *F, koki_engine *eng, double secs)
{
    KOKI_BEGIN(F);
    F->t0 = koki_now(eng) + secs;
    for (;;) {
        /* ALWAYS at least one frame: W(0) is a frame, and at 30 fps W(0.05)
         * is two (0.0333 < 0.05 <= 0.0667). Several boss patterns are timed
         * off exactly that. */
        KOKI_YIELD(F);
        if (koki_now(eng) >= F->t0)
            KOKI_RETURN(F);
    }
    KOKI_END(F);
}

koki_step koki_wait_until(koki_frame *F, koki_engine *eng, koki_pred_fn pred)
{
    KOKI_BEGIN(F);
    /* The predicate is evaluated FIRST, so wait_until can complete with zero
     * yields and let the caller's next statement run in the same frame. */
    while (!pred(eng))
        KOKI_YIELD(F);
    KOKI_END(F);
}
