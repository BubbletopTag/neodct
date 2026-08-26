/* rshell.h -- System/core/RemoteShell/__init__.py, ported.
 *
 * ============ THIS FILE IS IN THE WRONG DIRECTORY, ON PURPOSE ============
 *
 * In the Python this is a CORE module: System/core/RemoteShell. Two things
 * import it -- launcher.py, which calls start_if_enabled() at boot, and the
 * engineering app, which is the switch and the address book. The C plan says
 * the same thing: spec-storage-settings.md line 148 puts it in the core binary
 * as nd_remoteshell.c/.h, and core/nd_main.c already calls
 * nd_rs_start_if_enabled() through a weak reference, logging
 *
 *     [RSHELL] remote shell unavailable: not linked in this build
 *
 * when nothing defines it.
 *
 * It is here because this work package may not add files to lib/ or core/, and
 * the app is unportable without it: every one of the app's seven menu lines is
 * a call into this module. So the module is ported verbatim, under the names
 * spec-storage-settings.md line 1476 already chose for it, and lives beside
 * the only caller that can currently reach it.
 *
 * WHAT THAT COSTS, PLAINLY: nd-core links libneodct, not an app's app.so, so
 * the weak reference in nd_main.c STILL resolves to NULL and the phone still
 * does not bring the tunnel back up after a reboot. The app's own
 * "It stays on across restarts until you turn it off here" is therefore not
 * true in this build. Moving this pair of files to lib/nd_remoteshell.{c,h}
 * is the whole fix; nothing in them needs editing to do it.
 *
 * It also settles cross-agent point 5 (spec-storage-settings.md line 1709) in
 * the direction the spec calls "simpler, but then a crashing app can orphan an
 * sshd" -- the app forks the children itself -- because with the module in an
 * app's .so there is no other option. That is a decision for the owner, not
 * for this port, and it is written down here so it is not discovered later.
 *
 * ============ WHAT THE MODULE IS ============
 *
 * The Python's docstring is the specification and is reproduced in rshell.c.
 * The shape, in one picture:
 *
 *     phone  ---- ssh -R, key auth ---->  relay (a VPS with a public address)
 *                                           ^
 *     laptop ---- ssh -J -------------------'
 *
 * Two processes: sshd bound to 127.0.0.1 only, and a shell script that keeps
 * the outbound tunnel up. Both are started in their own session so that "off"
 * can mean off, and both are checked against /proc before they are signalled.
 */

#ifndef ND_RSHELL_H_INCLUDED
#define ND_RSHELL_H_INCLUDED

#include <sys/types.h>

#include "nd_paths.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APP_ID = 9990 -- manifest.json, and the menu VerticalList's app_id. */
#define ND_RS_APP_ID 9990

/* ------------------------------------------------------------------ *
 * The files, all under /NeoDCT/User/.remote
 * ------------------------------------------------------------------ *
 *
 * Spelled out rather than composed at runtime because every one of them is
 * also a string in a generated config file, and a path that is built two
 * different ways is a path that will one day disagree with itself.
 *
 * ND_PATH_REMOTE_DIR is nd_paths.h's name for the directory; it is repeated
 * literally below only so the constants read as the Python's do.
 */
#define ND_RS_USER_DIR        ND_PATH_REMOTE_DIR
#define ND_RS_STATE_FILE      "/NeoDCT/User/.remote/state.prop"
#define ND_RS_SSHD_CONFIG     "/NeoDCT/User/.remote/sshd_config"
#define ND_RS_HOST_KEY        "/NeoDCT/User/.remote/ssh_host_ed25519_key"
#define ND_RS_HOST_KEY_PUB    "/NeoDCT/User/.remote/ssh_host_ed25519_key.pub"
#define ND_RS_AUTHORIZED_KEYS "/NeoDCT/User/.remote/authorized_keys"
#define ND_RS_RELAY_KEY       "/NeoDCT/User/.remote/relay_id_ed25519"
#define ND_RS_KNOWN_HOSTS     "/NeoDCT/User/.remote/known_hosts"
#define ND_RS_TUNNEL_SCRIPT   "/NeoDCT/User/.remote/tunnel.sh"
/* sshd runs under a retry loop of its own, for the reason tunnel.sh has one:
 * it is started at boot, before the network scripts have finished, and a bind
 * to 127.0.0.1 fails outright when loopback is not up yet. */
#define ND_RS_SSHD_SCRIPT   "/NeoDCT/User/.remote/sshd.sh"
#define ND_RS_SSHD_LOOP_PID "/NeoDCT/User/.remote/sshd_loop.pid"
#define ND_RS_TUNNEL_PID    "/NeoDCT/User/.remote/tunnel.pid"
#define ND_RS_SSHD_PID      "/NeoDCT/User/.remote/sshd.pid"
#define ND_RS_LOG_FILE      "/NeoDCT/User/.remote/remote.log"

/* Where the operator drops the three files, on the card, from a PC. */
#define ND_RS_CARD_DIR  "remote"
#define ND_RS_CARD_CONF "relay.conf"

/* The three names taken off the card, in the Python dict's insertion order.
 * The app shows them sorted; see nd_rs_install_keys_from_card(). */
#define ND_RS_CARD_FILE_COUNT 3
extern const char *const nd_rs_card_file_names[ND_RS_CARD_FILE_COUNT];
extern const char *const nd_rs_card_file_dests[ND_RS_CARD_FILE_COUNT];

/* The programs. NOT ND_ROOT-resolved anywhere: nd_proc.h is explicit that an
 * executable is not, and the Python is checking the real /usr/sbin/sshd. */
#define ND_RS_SSHD        "/usr/sbin/sshd"
#define ND_RS_SSH         "/usr/bin/ssh"
#define ND_RS_KEYGEN      "/usr/bin/ssh-keygen"
#define ND_RS_SFTP_SERVER "/usr/libexec/sftp-server"

/* "The port sshd listens on, on loopback. Not 22: nothing else on this phone
 * wants 22, but a number nobody guesses costs nothing either." -- and then it
 * is 22 anyway. The comment and the value disagree in the Python; the VALUE is
 * what the config and the -R forward are built from, so 22 it is. */
#define ND_RS_LOCAL_PORT 22

/* DEFAULT_RELAY_PORT is the integer 2222 in the Python and is only ever used
 * through str(), so it is a string here. */
#define ND_RS_DEFAULT_RELAY_PORT "2222"
#define ND_RS_DEFAULT_RELAY_USER "neodct"

#define ND_RS_RETRY_SECONDS 15

/* sshd writes its PidFile a moment after it starts, and the app wants to say
 * "on" the moment it is: twenty tenth-second looks, then trust the file. */
#define ND_RS_PID_WAIT_TRIES 20
#define ND_RS_PID_WAIT_SLICE 0.1

/* ------------------------------------------------------------------ *
 * The two records
 * ------------------------------------------------------------------ *
 *
 * The Python's dicts are unbounded strings. These are the sizes
 * spec-storage-settings.md line 1477 chose: a host name or an IPv6 literal
 * fits 255, a login fits 63, a port fits 15. A longer value in state.prop is
 * truncated rather than rejected -- there is no screen for "your relay address
 * is too long", and the connection failing with a wrong name is the same
 * outcome as it failing with a missing one.
 */
typedef struct {
    bool enabled;
    char host[256];
    char user[64];
    char port[16];
} nd_rs_settings;

typedef struct {
    bool sshd;
    bool tunnel;
    bool enabled;
} nd_rs_status;

/* Every message this module puts on the phone's screen fits in this. */
#define ND_RS_ERRMSG_MAX 160

/* "Copied: %s." -- three file names and relay.conf, comma-joined. */
#define ND_RS_TAKEN_MAX 128

/* ------------------------------------------------------------------ *
 * settings() and save_settings()
 * ------------------------------------------------------------------ */

/* settings(): what the phone has been told about the relay. Never fails; an
 * unreadable or malformed state.prop reads as the defaults, which is what the
 * Python's `except OSError: pass` amounts to for every field. */
void nd_rs_settings_get(nd_rs_settings *out);

/* save_settings(host=, user=, port=, enabled=). A NULL argument is Python's
 * None: leave that field as it is. Each string is stripped of surrounding
 * whitespace; an empty user or port falls back to the default, an empty host
 * does not (there is no default relay). */
nd_err nd_rs_settings_save(const char *host, const char *user, const char *port,
                           const bool *enabled);

/* ------------------------------------------------------------------ *
 * Keys
 * ------------------------------------------------------------------ */

/* install_keys_from_card(card_root). Copies whichever of id_ed25519,
 * authorized_keys and known_hosts are in <card_root>/remote/ onto the user
 * partition at 0600, and applies <card_root>/remote/relay.conf if it is there.
 *
 * `taken` receives the names it took, comma-space joined and SORTED -- the
 * sort is the app's `", ".join(sorted(taken))` in the Python, done here
 * because the C returns one string rather than a list.
 *
 * ND_OK, or an error with `errmsg` set to the RemoteShellError message the
 * phone shows: no folder on the card, or nothing in it. */
nd_err nd_rs_install_keys_from_card(const char *card_root, char *taken, size_t n, char *errmsg,
                                    size_t errn);

/* have_keys(): true when both directions have what they need. */
bool nd_rs_have_keys(void);

/* ensure_host_key(): the phone's own identity, made once with ssh-keygen and
 * kept. ND_OK when the key exists afterwards. */
nd_err nd_rs_ensure_host_key(char *errmsg, size_t n);

/* host_fingerprint(): `ssh-keygen -lf <key>.pub`, stripped. Writes "" and
 * still returns ND_OK when there is no public key or ssh-keygen fails -- the
 * Python returns "" for both and the app prints "unknown". */
nd_err nd_rs_host_fingerprint(char *out, size_t n);

/* ------------------------------------------------------------------ *
 * The generated files
 * ------------------------------------------------------------------ */

/* write_sshd_config(): rewritten on every start, deliberately. See rshell.c
 * for the whole of the Python's reasoning, which is the security argument for
 * this module and is reproduced rather than summarised. */
nd_err nd_rs_write_sshd_config(void);

/* write_tunnel_script(host, user, port): the reconnect loop. */
nd_err nd_rs_write_tunnel_script(const char *host, const char *user, const char *port);

/* write_sshd_script(): the same retry loop, around sshd. It is started at boot
 * before the network scripts have finished, and a bind to 127.0.0.1 fails
 * outright when loopback is not up yet -- once, silently, leaving a phone that
 * the tunnel reaches and nothing answers on. */
nd_err nd_rs_write_sshd_script(void);

/* _quote(word): "'" + word.replace("'", "'\\''") + "'". Exported because the
 * Python's test suite has a real-`sh` shell-injection case over it. */
nd_err nd_rs_quote(char *out, size_t n, const char *word);

/* The single line tunnel.sh runs, already quoted word by word. Exported for
 * the same reason. */
nd_err nd_rs_tunnel_command_line(char *out, size_t n, const char *host, const char *user,
                                 const char *port);

/* ------------------------------------------------------------------ *
 * Running it
 * ------------------------------------------------------------------ */

/* check_ready(): ND_OK and `out` filled when this phone can actually do it;
 * otherwise `errmsg` is the reason, in the Python's order -- no ssh server, no
 * relay address, no relay key, no authorized_keys, no known_hosts. */
nd_err nd_rs_check_ready(nd_rs_settings *out, char *errmsg, size_t n);

/* status(): what is actually RUNNING, not what was asked for. Each half is a
 * pid file plus a /proc check; see nd_rs_owns(). */
void nd_rs_status_get(nd_rs_status *out);

/* start(): bring it up. ND_OK and `out` filled, or an error with `errmsg` set
 * to something worth reading on a 240 px screen. */
nd_err nd_rs_start(nd_rs_status *out, char *errmsg, size_t n);

/* stop(): take it down. Safe to call when it is already down. `remember`
 * false is the internal call inside start(), which must not record "off". */
nd_err nd_rs_stop(nd_rs_status *out, bool remember);

/* start_if_enabled(): the boot path. Silent when it was never turned on, and
 * logs "[RSHELL] not starting: <why>" rather than failing the boot.
 *
 * NOTHING CALLS THIS TODAY. core/nd_main.c wants it, but it references the
 * symbol weakly and links libneodct, not this .so. See the header comment. */
void nd_rs_start_if_enabled(void);

/* ------------------------------------------------------------------ *
 * The pid pair, exported because the bug they exist for bricked a phone
 * ------------------------------------------------------------------ */

/* _pid_from(path): the number in a pid file, or -1 for anything that is not
 * one. Python's `except (OSError, ValueError): return None`. */
pid_t nd_rs_pid_from(const char *path);

/* _owns(pid, needle): true when `pid` is running AND is the process we
 * started, judged by /proc/<pid>/cmdline containing `needle`.
 *
 * "A pid file outlives the boot that wrote it. On the next boot that number
 * belongs to whatever init happened to start in its place, and signalling it
 * -- or worse, its whole process group -- kills something chosen at random.
 *
 * That is not hypothetical. This phone left sshd.pid=244 and tunnel.pid=246
 * behind, came back up, and Remote Shell killed the process group those
 * numbers had been handed to: its own launcher. The UI never started and the
 * serial log simply stopped mid-boot.
 *
 * So the number is never enough. /proc says what the process actually is, and
 * only a match gets signalled." -- RemoteShell/__init__.py:371, copied
 * verbatim as PORT-PLAN.md WP-45 requires. */
bool nd_rs_owns(pid_t pid, const char *needle);

#ifdef __cplusplus
}
#endif

#endif /* ND_RSHELL_H_INCLUDED */
