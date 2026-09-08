/* nd_modem_sim.c -- Simulation Mode: the five /tmp/neodct_sim_* hooks.
 *
 * The owner develops on QEMU with no modem passthrough, so this path carries
 * as much weight as the real one. A port of _poll_sim (line 581) plus the two
 * on-demand readers signal_level() and operator_display() reach for.
 *
 *   echo 23 > /tmp/neodct_sim_csq                  drive the signal bars
 *   echo 5551234 > /tmp/neodct_sim_ring            fake an incoming call
 *   rm /tmp/neodct_sim_ring                        the caller gives up
 *   echo Tello > /tmp/neodct_sim_operator          fake the carrier line
 *   echo '5551234|hey there' > /tmp/neodct_sim_sms fake a received SMS
 *   touch /tmp/neodct_sim_fault                    fake a dead modem
 *   rm /tmp/neodct_sim_fault                       and undo it
 *
 * ============ THE RING HOOK IS mtime-EDGE TRIGGERED ============
 *
 * One ring per write or touch, not one per tick: the hook remembers the
 * file's mtime and only rings when it changes. Answering or declining
 * therefore does not instantly re-ring. Deleting the file while it is ringing
 * is the caller hanging up. Both the "no mtime yet" and the "stat failed"
 * states are Python's None and are distinct from any real timestamp, which is
 * why they are carried as a separate bool rather than as a sentinel double.
 *
 * ---- SO WRITE THE FILE ATOMICALLY, OR IT RINGS TWICE ----
 *
 * `echo 5551234 > /tmp/neodct_sim_ring` is TWO mtimes, not one: the shell
 * creates the file empty and then writes into it. The modem thread stats at
 * ten hertz with no regard for what the shell is in the middle of, so it can
 * land between them, ring on the EMPTY file with the fallback caller 5550000
 * and latch the create mtime. The write is then a second edge -- and the
 * latch below is only taken while the state is ND_CALL_IDLE, so it is still
 * pending when the call ends and the phone rings again the instant it goes
 * back to IDLE. It reads as a hangup bug in the dialer, which is exactly how
 * it presented in test_dialer ("End hung it up: got 2 want 0").
 *
 * The fix belongs to the writer, and it is one line:
 * `printf '5551234\n' > f.tmp && mv f.tmp /tmp/neodct_sim_ring`. rename(2) is
 * atomic, so the thread either does not see the file or sees it complete with
 * the only mtime it will ever have. docs/MODEM_BRINGUP.md says so where it
 * tells a bench engineer to use these hooks, and test_dialer.c does it in
 * ring_file_write(). Latching the mtime unconditionally here would swallow a
 * `touch` during a call, which is a hook somebody deliberately used; the
 * asymmetry is on purpose.
 *
 * Every path here is ND_ROOT-resolved, so `make test` drives the hooks inside
 * a scratch directory instead of the developer's real /tmp.
 *
 * ============ AND WHICH HOOKS SURVIVE ON A PHONE ============
 *
 * None of them is gated on the platform, and the gate that IS applied is a
 * better one: "can a leftover file make the phone make a CLAIM ABOUT ITS
 * RADIO?" Sort the five hooks by that question and the policy falls out.
 *
 *   csq, operator   ANSWER A QUESTION ABOUT THE RADIO. Both are read after
 *                   the link check in nd_modem_signal_level() and
 *                   nd_modem_operator_display(), so they are already dead in
 *                   FAULT and UNREACHABLE and are now dead in ABSENT too. A
 *                   phone with no radio cannot be made to draw bars or a
 *                   carrier name by a file somebody forgot to delete.
 *
 *   ring, sms       STAGE AN EVENT. Writing one is a deliberate act by
 *                   somebody with a shell on the phone; the ring it produces
 *                   is theirs, not the service inventing a call. They stay
 *                   live everywhere, including on hardware, because they are
 *                   the only way to exercise the call and message UI on a
 *                   bench -- and /tmp is a tmpfs nothing else writes, so they
 *                   cannot fire by accident.
 *
 *   fault           Staged too, and it must stay ahead of the no-hardware
 *                   early return in nd_modem_poll() for the reason written
 *                   there.
 *
 * A developer with a PHONE IMAGE on the bench and the modem unplugged
 * loses simulated dial and simulated SMS, and there is no way to get them
 * back on that image. That is not an oversight: NEODCT_PLATFORM used to be the
 * escape, and DECISIONS.md D2 closed it on purpose, because the same variable
 * in /NeoDCT/User/env.sh is an update-proof way to make a shipped phone
 * simulate its radio -- which is the failure the owner asked to make
 * impossible. The three staged hooks still work on that bench phone, so the
 * call UI and Messages are still drivable there; everything else belongs on
 * the host build or a QEMU image, where the override still decides and
 * Simulation Mode is whole. docs/MODEM_BRINGUP.md says the same out loud,
 * because otherwise this arrives as a bug report.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_modem_priv.h"
#include "nd_paths.h"
#include "nd_platform.h"
#include "nd_types.h"

/* os.path.getmtime(): seconds with the nanosecond part folded in, because
 * that is the resolution a `touch` two ticks apart differs by. */
static bool file_mtime(const char *path, double *out)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    if (stat(resolved, &st) != 0)
        return false;
    *out = (double)st.st_mtime + (double)st.st_mtim.tv_nsec / 1e9;
    return true;
}

bool nd_modem__sim_read_text(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    size_t n;

    if (out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return false;
    n = fread(out, 1u, out_sz - 1u, f);
    (void)fclose(f);
    out[n] = '\0';
    return true;
}

bool nd_modem__sim_read_int(const char *path, int32_t *out)
{
    char text[64];

    if (!nd_modem__sim_read_text(path, text, sizeof text))
        return false;
    /* int(f.read().strip()) -- parse_int already tolerates the whitespace. */
    return nd_modem__parse_int(text, out);
}

/* str.strip() on a buffer. Duplicated from nd_modem.c rather than exported:
 * it is four lines and exporting a py_strip() invites it into code that
 * should be using nd_strlcpy. */
static bool sim_space(char c)
{
    uint8_t u = (uint8_t)c;

    return u == ' ' || u == '\t' || u == '\n' || u == '\v' || u == '\f' || u == '\r' ||
           u == 0x1cu || u == 0x1du || u == 0x1eu || u == 0x1fu;
}

static void sim_strip(char *s)
{
    size_t start = 0u;
    size_t end = strlen(s);

    while (start < end && sim_space(s[start]))
        start++;
    while (end > start && sim_space(s[end - 1u]))
        end--;
    memmove(s, &s[start], end - start);
    s[end - start] = '\0';
}

/* The Simulation Mode announcement, in one place because there are two moments
 * it can be reached from and they must not say different things:
 * nd_modem_open()'s first probe (which is where it lands whenever the boot
 * grace is 0 -- QEMU, and every fixture in the suite) and the end-of-grace
 * one-shot below (which is where it lands on a phone). Whichever gets there
 * first sets sim_announced, so exactly one of them ever runs.
 *
 * The first line is byte for byte what it has always been, so log scrapers
 * keep working, and neither caller reaches it on a hardware image -- there,
 * announce_absent() has already said the true thing, louder.
 *
 * THE SECOND LINE IS THE UNKNOWN CASE, AND IT IS NOT SILENT. The first line is
 * a verdict about this SERVICE: nothing enumerated, so it is simulating. On an
 * image that has never said what it is, the question of whether the board
 * SHOULD have had a radio was never answered at all, and a reader who takes
 * the line above as a finding about the hardware has been misled by it. So the
 * missing evidence is named rather than papered over.
 *
 * IT QUOTES nd_platform_origin() RATHER THAN COMPOSING A SENTENCE. UNKNOWN is
 * two states, not one -- a build that was told nothing, and an image whose two
 * halves name different machines -- and "this image does not say which machine
 * it is" is FALSE in the second: it says so twice, differently. nd_crash.c was
 * changed away from exactly that mistake, for exactly that reason, and a
 * triage line that misdescribes WHY it cannot name the machine sends the
 * person reading it looking for the wrong fault. The origin string already
 * composes the right clause for both, so there is one sentence and no way for
 * it to be wrong.
 *
 * A console line and not a modal, because the population of unlabelled images
 * is developers' laptops and the unit suite, and a dialog on every run is the
 * denial of service that every latch in this service exists to avoid. */
void nd_modem__announce_simulation(void)
{
    nd_log(ND_LOG_MODEM, "HARDWARE NOT FOUND: Running in Simulation Mode.");
    if (nd_platform() == ND_PLATFORM_UNKNOWN)
        nd_log(ND_LOG_MODEM,
               "(Whether this board should have a radio is unknown: %s. Simulating claims "
               "nothing either way.)",
               nd_platform_origin());
}

/* ------------------------------------------------------------------ *
 * _poll_sim, line 581 -- runs at the FULL tick rate, not the URC cadence
 * ------------------------------------------------------------------ */

static void poll_sim_ring(nd_modem *m)
{
    double mtime = 0.0;
    bool have_mtime;
    bool changed;
    char caller[ND_MODEM_NUMBER_MAX];
    nd_mev e;

    if (!nd_path_exists(ND_MODEM_SIM_RING)) {
        nd_modem__lock(m);
        m->sim_ring_mtime_known = false;
        nd_modem__unlock(m);
        if (nd_modem_state(m) == ND_CALL_RINGING) {
            nd_modem__lock(m);
            m->state = ND_CALL_IDLE;
            nd_modem__unlock(m);
            memset(&e, 0, sizeof e);
            e.kind = ND_MEV_ENDED;
            e.index = -1;
            e.has_detail = true;
            (void)nd_strlcpy(e.text, "sim caller gave up", sizeof e.text);
            nd_modem__queue(m, &e);
        }
        return;
    }

    have_mtime = file_mtime(ND_MODEM_SIM_RING, &mtime);

    nd_modem__lock(m);
    /* Python: `mtime != self._sim_ring_mtime`, where either side may be None. */
    changed = (have_mtime != m->sim_ring_mtime_known) || (have_mtime && mtime != m->sim_ring_mtime);
    if (m->state != ND_CALL_IDLE || !changed) {
        nd_modem__unlock(m);
        return;
    }
    m->sim_ring_mtime_known = have_mtime;
    m->sim_ring_mtime = mtime;
    nd_modem__unlock(m);

    if (!nd_modem__sim_read_text(ND_MODEM_SIM_RING, caller, sizeof caller))
        (void)nd_strlcpy(caller, "5550000", sizeof caller);
    else {
        sim_strip(caller);
        if (caller[0] == '\0')
            (void)nd_strlcpy(caller, "5550000", sizeof caller);
    }

    nd_modem__lock(m);
    (void)nd_strlcpy(m->caller_id, caller, sizeof m->caller_id);
    m->caller_id_known = true;
    m->state = ND_CALL_RINGING;
    nd_modem__unlock(m);

    memset(&e, 0, sizeof e);
    e.kind = ND_MEV_INCOMING;
    e.index = -1;
    e.has_detail = true;
    (void)nd_strlcpy(e.text, caller, sizeof e.text);
    nd_modem__queue(m, &e);
}

static void poll_sim_sms(nd_modem *m)
{
    char content[ND_MODEM_TEXT_MAX];
    char resolved[ND_PATH_MAX];
    char sender[ND_MODEM_NUMBER_MAX];
    char body[ND_MODEM_TEXT_MAX];
    const char *bar;
    nd_mev e;

    if (!nd_path_exists(ND_MODEM_SIM_SMS))
        return;

    if (!nd_modem__sim_read_text(ND_MODEM_SIM_SMS, content, sizeof content))
        content[0] = '\0';
    sim_strip(content);
    /* Both the read and the remove are inside one try in the Python, so a
     * failed read still leaves whatever was read and may skip the remove. */
    if (nd_path_resolve(resolved, sizeof resolved, ND_MODEM_SIM_SMS) == ND_OK)
        (void)unlink(resolved);
    if (content[0] == '\0')
        return;

    /* content.partition("|") -- the FIRST bar only. */
    bar = strchr(content, '|');
    if (bar != NULL) {
        size_t len = (size_t)(bar - content);

        if (len >= sizeof sender)
            len = sizeof sender - 1u;
        memcpy(sender, content, len);
        sender[len] = '\0';
        (void)nd_strlcpy(body, bar + 1, sizeof body);
    } else {
        (void)nd_strlcpy(sender, content, sizeof sender);
        body[0] = '\0';
    }
    if (body[0] == '\0') {
        /* No bar at all -- or a trailing bar with nothing after it. Either
         * way the whole string becomes the body. */
        (void)nd_strlcpy(body, sender, sizeof body);
        (void)nd_strlcpy(sender, "5550000", sizeof sender);
    }
    sim_strip(sender);
    sim_strip(body);

    /* Stash it so that the fetch_sms() the core answers the event with has
     * something to hand back; see OPEN-QUESTIONS.md M-1. */
    nd_modem__lock(m);
    m->sim_sms_pending = true;
    (void)nd_strlcpy(m->sim_sms_sender, sender, sizeof m->sim_sms_sender);
    (void)nd_strlcpy(m->sim_sms_body, body, sizeof m->sim_sms_body);
    nd_modem__unlock(m);

    memset(&e, 0, sizeof e);
    e.kind = ND_MEV_SMS_SIM;
    e.index = ND_MODEM_SMS_IDX_SIM;
    e.has_detail = true;
    (void)nd_strlcpy(e.sender, sender, sizeof e.sender);
    (void)nd_strlcpy(e.text, body, sizeof e.text);
    nd_modem__queue(m, &e);
}

void nd_modem__poll_sim(nd_modem *m, double now)
{
    /* The boot grace ran out with nothing answering: NOW it is Simulation
     * Mode, and the console is told once, where nd_modem_open() used to say
     * it before the modem had a chance.
     *
     * Only when there is genuinely nothing to talk to, though. A phone whose
     * ttyUSB nodes enumerated and could not be opened is
     * ND_MODEM_LINK_UNREACHABLE, and nd_modem__probe_hardware() has already
     * said so in the words that fit; "HARDWARE NOT FOUND" about hardware
     * that is plainly found is how the console came to agree with the
     * carrier line about something neither of them knew. */
    if (!m->sim_announced && now >= m->boot_deadline) {
        bool radio;

        nd_modem__lock(m);
        radio = m->saw_candidates;
        nd_modem__unlock(m);
        /* And not on an image that claims a radio either, where nothing
         * enumerating is ND_MODEM_LINK_ABSENT: announce_absent() has already
         * said it, louder and correctly, and "Running in Simulation Mode" on a
         * phone that is about to refuse every call would be the console
         * contradicting the carrier line about the one thing they both now
         * know.
         *
         * Asked through nd_modem__board_should_have_a_radio() and never as
         * !nd_platform_is_hw(). They are not the same predicate -- a contested
         * image is neither -- and more to the point, a second spelling is a
         * second decision point: the day the gate is narrowed or widened in
         * nd_modem.c, this line has to move with it or the console and the
         * classify start disagreeing again. */
        if (!radio && !nd_modem__board_should_have_a_radio())
            nd_modem__announce_simulation();
        m->sim_announced = true;
    }

    poll_sim_ring(m);
    poll_sim_sms(m);

    /* Re-probe for late or hotplugged hardware: a modem that enumerated after
     * the UI started, or QEMU passthrough attached on the fly. */
    if (now >= m->next_probe && nd_modem__probe_hardware(m)) {
        nd_mev e;

        memset(&e, 0, sizeof e);
        e.kind = ND_MEV_MODEM_FOUND;
        e.index = -1;
        e.has_detail = true;
        nd_modem__lock(m);
        (void)nd_strlcpy(e.text, m->port, sizeof e.text);
        nd_modem__unlock(m);
        nd_modem__queue(m, &e);
    }
}
