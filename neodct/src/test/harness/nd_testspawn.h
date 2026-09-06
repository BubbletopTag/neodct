/* nd_testspawn.h -- did that child ever reach execve, or did it die in the
 * window between the fork and the exec?
 *
 * Header-only, `static inline`, no object file: it has to be usable from a
 * test binary, from a test app under test/apps, and from tools/nd_selftest.c
 * without any of them acquiring a new link dependency, and the Makefile names
 * test/harness/nd_testguard.o by hand rather than globbing this directory.
 * Include it by relative path, the way test_notify.c already includes
 * ../../lib/nd_notify_priv.h:
 *
 *     #include "../harness/nd_testspawn.h"
 *
 * ============ THE BUG THIS EXISTS FOR ============
 *
 * nd_proc_spawn() returns ND_OK when fork() succeeded. Its own header says
 * so, in as many words: "an execve failure is reported by the child exiting
 * 127, which the caller sees at waitpid". Every caller in this tree read that
 * ND_OK as "the program is running", and for eight releases one of them was
 * wrong on every phone -- nd_notify.c asked its aplay to become ndusr from a
 * core that was already ndusr, setgroups(2) wanted CAP_SETGID, and the child
 * did _exit(122) before execve. The phone made no sound at all and the log
 * said "Ringing:".
 *
 * The suite could not see it, and the reason is worth stating plainly because
 * it is the whole design constraint here: A TEST THAT ASSERTS ON THE SPAWN'S
 * RETURN VALUE CANNOT FAIL. The information about a pre-exec death is in the
 * WAIT STATUS and nowhere else, and until something looks at the wait status
 * the failure is not merely undetected, it is unobservable.
 *
 * ============ WHY THIS DOES NOT USE nd_proc_wait() ============
 *
 * Deliberately. nd_proc.c's collect() answers ECHILD -- "that pid is not a
 * child of mine" -- by synthesising `exited = true, exit_status = 0`, so
 * nd_proc_wait() reports a clean exit for a process this program never
 * forked. That is the same shape of lie as the one above, one layer down, and
 * an observer built on it would inherit it: ask about a pid that was never
 * spawned and be told it finished successfully.
 *
 * So this calls waitpid(2) directly and reports ND_TESTSPAWN_NOT_OURS as its
 * own distinct answer. When nd_proc.c's ECHILD branch is fixed this becomes
 * redundant rather than wrong, which is the right way round.
 *
 * ============ HOW A TEST USES IT ============
 *
 *     pid_t pid;
 *     int code;
 *
 *     CHECK(nd_proc_spawn(exe, &spec, &pid) == ND_OK);   // says only "fork() worked"
 *
 *     // ...and this says whether anything ran:
 *     nd_testspawn_fate f = nd_testspawn_wait(pid, 2.0, &code);
 *     if (f != ND_TESTSPAWN_EXECED && f != ND_TESTSPAWN_RUNNING)
 *         printf("the player never started: %s\n", nd_testspawn_why(f, code));
 *     CHECK(f == ND_TESTSPAWN_EXECED || f == ND_TESTSPAWN_RUNNING);
 *
 * A long-lived child (a ringer, a media player) is ND_TESTSPAWN_RUNNING after
 * a short wait, and that is a PASS: still being alive is the only positive
 * proof there is that execve happened. A one-shot child is EXECED with its
 * own status in `code`. Anything else is a death before the program started,
 * and `code` says at which of nd_priv_become()'s steps.
 */

#ifndef ND_TESTSPAWN_H_INCLUDED
#define ND_TESTSPAWN_H_INCLUDED

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "nd_types.h"

/* The 120..127 decoder, in libneodct so that there is exactly one table:
 * nd_notify.c logs from it, tools/nd_selftest.c prints from it, and this
 * reads from it. A second copy would drift the first time nd_proc.c reserved
 * another number. */
#include "../../lib/nd_notify_priv.h"

typedef enum {
    /* Still alive when the wait ran out. For anything meant to keep running
     * this is the success case: a process cannot be alive without having got
     * past execve. */
    ND_TESTSPAWN_RUNNING = 0,
    /* Exited with a status a real program can produce. It ran. */
    ND_TESTSPAWN_EXECED,
    /* Exited 120..127 -- one of nd_proc.c's reserved pre-exec codes. Nothing
     * that actually started can produce one, so this is proof the program
     * never ran. `code` is the number; nd_tone_pre_exec_reason() names it. */
    ND_TESTSPAWN_PRE_EXEC,
    /* Killed by a signal before anybody waited. `code` is the signal. */
    ND_TESTSPAWN_SIGNALLED,
    /* waitpid said ECHILD: this process never forked that pid, or somebody
     * else has already reaped it. NOT an exit, and specifically not a clean
     * one -- see the header. */
    ND_TESTSPAWN_NOT_OURS,
    /* waitpid failed for some other reason, or the pid was nonsense. */
    ND_TESTSPAWN_ERROR
} nd_testspawn_fate;

static inline void nd_testspawn__nap_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* Classify one already-collected wait status. Separated out so a caller that
 * did its own waitpid -- tools/nd_selftest.c does, from a child that has
 * dropped privilege -- gets the same reading of the same number. */
static inline nd_testspawn_fate nd_testspawn_classify(int status, int *code_out)
{
    int code = 0;
    nd_testspawn_fate f;

    if (WIFSIGNALED(status)) {
        code = WTERMSIG(status);
        f = ND_TESTSPAWN_SIGNALLED;
    } else if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
        /* The reserved band. nd_proc.c hands out 120 + the nd_priv_become()
         * step, 126 for a dup2 and 127 for the execve itself; aplay uses 0
         * and 1, mpv 0..4, and nothing in this tree exits in that range on
         * purpose. Treating them as ordinary exits is exactly the mistake
         * that hid the silent phone. */
        f = (nd_tone_pre_exec_reason(code) != NULL) ? ND_TESTSPAWN_PRE_EXEC
                                                    : ND_TESTSPAWN_EXECED;
    } else {
        /* Stopped or continued: WUNTRACED is not passed below, so this cannot
         * normally happen, and reporting it as an exit would be a guess. */
        f = ND_TESTSPAWN_ERROR;
    }
    if (code_out != NULL)
        *code_out = code;
    return f;
}

/* Wait up to `wait_s` for `pid` and say what became of it. wait_s of 0.0 is
 * one non-blocking probe; negative blocks. */
static inline nd_testspawn_fate nd_testspawn_wait(pid_t pid, double wait_s, int *code_out)
{
    long waited_ms = 0;
    long limit_ms = (wait_s < 0.0) ? -1L : (long)(wait_s * 1000.0);

    if (code_out != NULL)
        *code_out = 0;
    if (pid <= 0)
        return ND_TESTSPAWN_ERROR;

    for (;;) {
        int status = 0;
        pid_t r;

        do {
            r = waitpid(pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);

        if (r == pid)
            return nd_testspawn_classify(status, code_out);
        if (r < 0)
            return (errno == ECHILD) ? ND_TESTSPAWN_NOT_OURS : ND_TESTSPAWN_ERROR;
        if (limit_ms >= 0 && waited_ms >= limit_ms)
            return ND_TESTSPAWN_RUNNING;
        nd_testspawn__nap_ms(5);
        waited_ms += 5;
    }
}

static inline const char *nd_testspawn_fate_name(nd_testspawn_fate f)
{
    switch (f) {
    case ND_TESTSPAWN_RUNNING:   return "still running";
    case ND_TESTSPAWN_EXECED:    return "ran and exited";
    case ND_TESTSPAWN_PRE_EXEC:  return "never started";
    case ND_TESTSPAWN_SIGNALLED: return "killed by a signal";
    case ND_TESTSPAWN_NOT_OURS:  return "not a child of this process";
    default:                     return "unknown";
    }
}

/* One sentence a person can read, with the pre-exec step named. Returns a
 * string literal or a pointer into a small static buffer -- fine for a test
 * or a diagnostic, not for two concurrent callers. */
static inline const char *nd_testspawn_why(nd_testspawn_fate f, int code)
{
    static char buf[160];
    const char *reason;

    switch (f) {
    case ND_TESTSPAWN_RUNNING:
        return "still running -- which is proof it got past execve";
    case ND_TESTSPAWN_EXECED:
        (void)nd_snprintf(buf, sizeof buf, "ran, and exited %d", code);
        return buf;
    case ND_TESTSPAWN_PRE_EXEC:
        reason = nd_tone_pre_exec_reason(code);
        (void)nd_snprintf(buf, sizeof buf, "never started: exit %d, %s", code,
                          reason != NULL ? reason : "a reserved pre-exec code");
        return buf;
    case ND_TESTSPAWN_SIGNALLED:
        (void)nd_snprintf(buf, sizeof buf, "killed by signal %d", code);
        return buf;
    case ND_TESTSPAWN_NOT_OURS:
        return "waitpid says ECHILD: nothing here forked that pid, so its "
               "status is unknown and is NOT a clean exit";
    default:
        return "waitpid failed";
    }
}

#endif /* ND_TESTSPAWN_H_INCLUDED */
