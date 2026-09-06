/* nd_modem_sim.c -- Simulation Mode: the four /tmp/neodct_sim_* hooks.
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
 * Every path here is ND_ROOT-resolved, so `make test` drives the hooks inside
 * a scratch directory instead of the developer's real /tmp.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_modem_priv.h"
#include "nd_paths.h"
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
        if (!radio)
            nd_log(ND_LOG_MODEM, "HARDWARE NOT FOUND: Running in Simulation Mode.");
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
