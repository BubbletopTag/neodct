/* test_remoteshell.c -- the Remote Shell app and the module under it.
 *
 * apps/RemoteShell holds two ports: the app (main.c, from
 * engineering/apps/RemoteShell/main.py) and the module it drives (rshell.c,
 * from System/core/RemoteShell/__init__.py). rshell.h says why the second one
 * is in an app directory rather than in lib/, and what that costs.
 *
 * ============ WHAT THIS TEST WILL NOT DO ============
 *
 * IT NEVER CALLS nd_rs_start(). That forks a real sshd and a real reconnect
 * loop, in their own sessions, and nd_rs_stop() then signals a process GROUP.
 * On a developer's machine or a CI runner that is somebody's ssh server and a
 * daemon left running after the test exits. So the composition is left alone
 * and the pieces underneath it are tested instead -- exactly the hole
 * test_power.c leaves around nd_power_go_down(), and named in remote_app.h as
 * well as here.
 *
 * app_run() is likewise only ever driven with Back on the first screen. Every
 * other answer reaches a blocking dialog or a text field.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The seven menu lines are the Python's, in the Python's order, and are
 *     rebuilt from live state -- so writing a relay address changes line 2 and
 *     line 0 tracks what is actually running. "Dialling" is asserted for both
 *     half-up cases, because "On" there would be a lie.
 *
 *  2. settings()/save_settings(): the defaults, the strip, the two fields that
 *     fall back to a default when they are emptied and the one that does not,
 *     None meaning "leave it alone", and the on-disk format -- key=value,
 *     sorted, no trailing blank, which is nd_props.h's second writer.
 *
 *  3. install_keys_from_card(): the two refusals word for word, the three
 *     files landing at 0600, relay.conf being applied, and the sorted
 *     comma-joined list the "Copied:" dialog is built from.
 *
 *  4. check_ready() refuses in the Python's ORDER, which decides which of five
 *     messages a half-configured phone sees.
 *
 *  5. _quote() and the tunnel command line: an apostrophe in a relay address
 *     comes back as '\'' and cannot end the quoting, which is the
 *     shell-injection case the Python's suite has over real /bin/sh.
 *
 *  6. The generated sshd_config carries every directive that decides who can
 *     get in, and tunnel.sh is a retry loop around the quoted command.
 *
 *  7. _pid_from()/_owns(): a pid file naming a live process that is NOT ours
 *     is not ours. That check is the one that stopped this module killing the
 *     launcher's process group on a real phone.
 *
 *  8. The five dialog strings, and Back on the menu returning 0.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (for the
 * font); the scratch root is this test's own, so nothing it writes can reach
 * a real /NeoDCT/User/.remote.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "smallapp_test.h"

#include "../../apps/RemoteShell/remote_app.h"
#include "nd_remoteshell.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    void (*settings_get)(nd_rs_settings *);
    nd_err (*settings_save)(const char *, const char *, const char *, const bool *);
    nd_err (*install_keys)(const char *, char *, size_t, char *, size_t);
    bool (*have_keys)(void);
    nd_err (*ensure_host_key)(char *, size_t);
    nd_err (*fingerprint)(char *, size_t);
    nd_err (*write_sshd_config)(void);
    nd_err (*write_tunnel_script)(const char *, const char *, const char *);
    nd_err (*write_sshd_script)(void);
    nd_err (*quote)(char *, size_t, const char *);
    nd_err (*tunnel_line)(char *, size_t, const char *, const char *, const char *);
    nd_err (*check_ready)(nd_rs_settings *, char *, size_t);
    void (*status_get)(nd_rs_status *);
    nd_err (*stop)(nd_rs_status *, bool);
    pid_t (*pid_from)(const char *);
    bool (*owns)(pid_t, const char *);
    void (*menu_lines)(char (*)[ND_RSAPP_LINE_MAX], size_t);
    const char *(*running_word)(bool, bool);
    void (*copied_message)(char *, size_t, const char *);
    void (*fingerprint_message)(char *, size_t, const char *);
    const char *const *title;
    const char *const *no_card;
    const char *const *ask_turn_on;
    const char *const *turn_on_button;
    const char *const *now_on;
    const char *const *now_off;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.settings_get = sa_sym(h, "nd_rs_settings_get");
    *(void **)&api.settings_save = sa_sym(h, "nd_rs_settings_save");
    *(void **)&api.install_keys = sa_sym(h, "nd_rs_install_keys_from_card");
    *(void **)&api.have_keys = sa_sym(h, "nd_rs_have_keys");
    *(void **)&api.ensure_host_key = sa_sym(h, "nd_rs_ensure_host_key");
    *(void **)&api.fingerprint = sa_sym(h, "nd_rs_host_fingerprint");
    *(void **)&api.write_sshd_config = sa_sym(h, "nd_rs_write_sshd_config");
    *(void **)&api.write_tunnel_script = sa_sym(h, "nd_rs_write_tunnel_script");
    *(void **)&api.write_sshd_script = dlsym(h, "nd_rs_write_sshd_script");
    *(void **)&api.quote = sa_sym(h, "nd_rs_quote");
    *(void **)&api.tunnel_line = sa_sym(h, "nd_rs_tunnel_command_line");
    *(void **)&api.check_ready = sa_sym(h, "nd_rs_check_ready");
    *(void **)&api.status_get = sa_sym(h, "nd_rs_status_get");
    *(void **)&api.stop = sa_sym(h, "nd_rs_stop");
    *(void **)&api.pid_from = sa_sym(h, "nd_rs_pid_from");
    *(void **)&api.owns = sa_sym(h, "nd_rs_owns");
    *(void **)&api.menu_lines = sa_sym(h, "nd_rsapp_menu_lines");
    *(void **)&api.running_word = sa_sym(h, "nd_rsapp_running_word");
    *(void **)&api.copied_message = sa_sym(h, "nd_rsapp_copied_message");
    *(void **)&api.fingerprint_message = sa_sym(h, "nd_rsapp_fingerprint_message");

    api.title = dlsym(h, "nd_rsapp_title");
    api.no_card = dlsym(h, "nd_rsapp_no_card");
    api.ask_turn_on = dlsym(h, "nd_rsapp_ask_turn_on");
    api.turn_on_button = dlsym(h, "nd_rsapp_turn_on_button");
    api.now_on = dlsym(h, "nd_rsapp_now_on");
    api.now_off = dlsym(h, "nd_rsapp_now_off");

    return api.run != NULL && api.shutdown != NULL && api.settings_get != NULL &&
           api.settings_save != NULL && api.install_keys != NULL && api.have_keys != NULL &&
           api.ensure_host_key != NULL && api.fingerprint != NULL &&
           api.write_sshd_config != NULL && api.write_tunnel_script != NULL && api.quote != NULL &&
           api.tunnel_line != NULL && api.check_ready != NULL && api.status_get != NULL &&
           api.stop != NULL && api.pid_from != NULL && api.owns != NULL && api.menu_lines != NULL &&
           api.running_word != NULL && api.copied_message != NULL &&
           api.fingerprint_message != NULL && api.title != NULL && api.no_card != NULL &&
           api.ask_turn_on != NULL && api.turn_on_button != NULL && api.now_on != NULL &&
           api.now_off != NULL;
}

/* ------------------------------------------------------------------ *
 * The scratch root
 * ------------------------------------------------------------------ */

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];

static void root_to_scratch(void)
{
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);
}

static void root_restore(void)
{
    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
}

/* Everything under /NeoDCT/User/.remote, gone, so each test starts from a
 * phone that has never had Remote Shell turned on. */
static void wipe_state(void)
{
    char dir[ND_PATH_MAX];

    if (nd_snprintf(dir, sizeof dir, "%s%s", g_root, ND_RS_USER_DIR) == ND_OK)
        sa_rmtree(dir);
}

static bool read_whole(const char *virtual_path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    size_t got;

    if (out_sz == 0u)
        return false;
    out[0] = '\0';
    if (nd_path_resolve(resolved, sizeof resolved, virtual_path) != ND_OK)
        return false;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return false;
    got = fread(out, 1u, out_sz - 1u, f);
    out[got] = '\0';
    (void)fclose(f);
    return true;
}

static bool write_whole(const char *virtual_path, const char *text)
{
    char resolved[ND_PATH_MAX];
    FILE *f;

    if (nd_path_resolve(resolved, sizeof resolved, virtual_path) != ND_OK)
        return false;
    f = fopen(resolved, "wb");
    if (f == NULL)
        return false;
    (void)fputs(text, f);
    (void)fclose(f);
    return true;
}

static mode_t mode_of(const char *virtual_path)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, virtual_path) != ND_OK)
        return 0u;
    if (stat(resolved, &st) != 0)
        return 0u;
    return st.st_mode & 07777u;
}

/* ------------------------------------------------------------------ *
 * 2. settings() and save_settings()
 * ------------------------------------------------------------------ */

static void test_settings_defaults(void)
{
    nd_rs_settings s;

    wipe_state();
    api.settings_get(&s);
    CHECK(!s.enabled, "a phone that was never switched on is not enabled");
    CHECK_STR(s.host, "", "and has no relay address");
    CHECK_STR(s.user, "neodct", "DEFAULT_RELAY_USER");
    CHECK_STR(s.port, "2222", "DEFAULT_RELAY_PORT, as a string");
}

static void test_settings_save(void)
{
    nd_rs_settings s;
    char text[512];
    const bool on = true;

    wipe_state();

    /* Each argument is Python's keyword: NULL leaves that field alone. */
    CHECK_INT(api.settings_save("  relay.example.net  ", NULL, NULL, NULL), ND_OK, "host saved");
    api.settings_get(&s);
    CHECK_STR(s.host, "relay.example.net", "and stripped");
    CHECK_STR(s.user, "neodct", "the login was not touched");

    CHECK_INT(api.settings_save(NULL, "operator", NULL, NULL), ND_OK, "user saved");
    api.settings_get(&s);
    CHECK_STR(s.user, "operator", "the new login");
    CHECK_STR(s.host, "relay.example.net", "and the relay survived");

    /* `user.strip() or DEFAULT_RELAY_USER` and the same for the port -- but
     * NOT for the host, which has no default to fall back to. */
    CHECK_INT(api.settings_save(NULL, "   ", "  ", NULL), ND_OK, "emptied login and port");
    api.settings_get(&s);
    CHECK_STR(s.user, "neodct", "an empty login falls back to the default");
    CHECK_STR(s.port, "2222", "and so does an empty port");

    CHECK_INT(api.settings_save("", NULL, NULL, NULL), ND_OK, "emptied host");
    api.settings_get(&s);
    CHECK_STR(s.host, "", "an empty relay address stays empty");

    CHECK_INT(api.settings_save("[2001:db8::1]", "op", "2022", &on), ND_OK, "all four at once");
    api.settings_get(&s);
    CHECK_STR(s.host, "[2001:db8::1]", "an IPv6 literal, which is the likely answer here");
    CHECK_STR(s.user, "op", "login");
    CHECK_STR(s.port, "2022", "port");
    CHECK(s.enabled, "enabled");

    /* _write_props: sorted keys, "key=value\n" per line, and no trailing
     * blank line -- nd_props.h's trailing_nl_when_empty == false writer. */
    CHECK(read_whole(ND_RS_STATE_FILE, text, sizeof text), "state.prop is on disk");
    CHECK_STR(text, "enabled=1\nhost=[2001:db8::1]\nport=2022\nuser=op\n", "state.prop, verbatim");
}

static void test_settings_ignores_a_broken_file(void)
{
    nd_rs_settings s;

    wipe_state();
    CHECK_INT(nd_mkdir_p(ND_RS_USER_DIR, 0700u), ND_OK, "state directory");
    /* A leading space defeats the '#' comment check in the raw dialect
     * (nd_props.h B-3), so this line IS parsed and its key is "#host". */
    CHECK(write_whole(ND_RS_STATE_FILE, " #host=commented\nhost=real.example\nnonsense\n"),
          "wrote a state file with an indented comment in it");
    api.settings_get(&s);
    CHECK_STR(s.host, "real.example", "the real key still wins");
}

/* ------------------------------------------------------------------ *
 * 1. The seven menu lines
 * ------------------------------------------------------------------ */

static void test_running_word(void)
{
    CHECK_STR(api.running_word(true, true), "On", "both up");
    /* "Half up is worth naming. It means the relay refused the tunnel or
     * dropped it, and 'On' would be a lie while nothing can reach you." */
    CHECK_STR(api.running_word(true, false), "Dialling", "sshd only");
    CHECK_STR(api.running_word(false, true), "Dialling", "tunnel only");
    CHECK_STR(api.running_word(false, false), "Off", "neither");
}

static void test_menu_lines(void)
{
    char lines[ND_RSAPP_MENU_ITEMS][ND_RSAPP_LINE_MAX];

    wipe_state();
    api.menu_lines(lines, ND_RSAPP_MENU_ITEMS);

    /* Nothing is running, so this is the whole screen a fresh phone shows. */
    CHECK_STR(lines[ND_RSAPP_STATUS], "Status: Off", "line 0");
    CHECK_STR(lines[ND_RSAPP_TOGGLE], "Turn on", "line 1");
    CHECK_STR(lines[ND_RSAPP_RELAY], "Relay: not set", "line 2, `host or \"not set\"`");
    CHECK_STR(lines[ND_RSAPP_LOGIN], "Login: neodct", "line 3");
    CHECK_STR(lines[ND_RSAPP_PORT], "Port: 2222", "line 4");
    CHECK_STR(lines[ND_RSAPP_KEYS], "Copy keys from card", "line 5");
    CHECK_STR(lines[ND_RSAPP_FINGERPRINT], "This phone's key", "line 6");

    /* Rebuilt from live state every pass: that is what makes the list a
     * status display rather than a menu that has to be left and re-entered. */
    CHECK_INT(api.settings_save("relay.example.net", "op", "2022", NULL), ND_OK, "reconfigured");
    api.menu_lines(lines, ND_RSAPP_MENU_ITEMS);
    CHECK_STR(lines[ND_RSAPP_RELAY], "Relay: relay.example.net", "line 2 followed the setting");
    CHECK_STR(lines[ND_RSAPP_LOGIN], "Login: op", "line 3 followed the setting");
    CHECK_STR(lines[ND_RSAPP_PORT], "Port: 2022", "line 4 followed the setting");
    CHECK_STR(lines[ND_RSAPP_STATUS], "Status: Off", "and nothing is running");

    /* The indices are a contract with run()'s if/elif chain. */
    CHECK_INT(ND_RSAPP_STATUS, 0, "STATUS");
    CHECK_INT(ND_RSAPP_TOGGLE, 1, "TOGGLE");
    CHECK_INT(ND_RSAPP_RELAY, 2, "RELAY");
    CHECK_INT(ND_RSAPP_LOGIN, 3, "LOGIN");
    CHECK_INT(ND_RSAPP_PORT, 4, "PORT");
    CHECK_INT(ND_RSAPP_KEYS, 5, "KEYS");
    CHECK_INT(ND_RSAPP_FINGERPRINT, 6, "FINGERPRINT");
    CHECK_INT(ND_RS_APP_ID, 9990, "APP_ID");
}

/* ------------------------------------------------------------------ *
 * 8. The dialog strings
 * ------------------------------------------------------------------ */

static void test_dialog_strings(void)
{
    char out[ND_RSAPP_MSG_MAX];

    /* "'Remote Shell' is 189px against 136 available; this is 111." */
    CHECK_STR(*api.title, "Remote", "TITLE");
    CHECK_STR(*api.no_card, "No card in the phone.", "no card");
    CHECK_STR(*api.ask_turn_on, "Let this phone be reached over the internet?", "the confirmation");
    CHECK_STR(*api.turn_on_button, "Turn on", "its button");
    CHECK_STR(*api.now_on,
              "Remote Shell is on.\n\nIt stays on across restarts until you turn it off here.",
              "turned on");
    CHECK_STR(*api.now_off, "Remote Shell is off.", "turned off");

    /* A literal double hyphen, not an em dash: this font would draw one
     * wrong and the Python has the two characters. */
    api.copied_message(out, sizeof out, "authorized_keys, id_ed25519, known_hosts");
    CHECK_STR(out,
              "Copied: authorized_keys, id_ed25519, known_hosts.\n\nDelete them from the card now "
              "-- anyone who takes the card out can read them.",
              "the Copied dialog");

    api.fingerprint_message(out, sizeof out, "256 SHA256:abc root@phone (ED25519)");
    CHECK_STR(out, "This phone:\n256 SHA256:abc root@phone (ED25519)", "the fingerprint dialog");
    api.fingerprint_message(out, sizeof out, "");
    CHECK_STR(out, "This phone:\nunknown", "`host_fingerprint() or \"unknown\"`");
}

/* ------------------------------------------------------------------ *
 * 5 and 6. Quoting, the tunnel command and the two generated files
 * ------------------------------------------------------------------ */

static void test_quote(void)
{
    char out[256];

    CHECK_INT(api.quote(out, sizeof out, "plain"), ND_OK, "quoted");
    CHECK_STR(out, "'plain'", "an ordinary word");

    /* The injection case: an apostrophe closes the quoting, so it has to come
     * back as '\'' -- close, escaped literal, reopen. Anything that leaves
     * the quoting open hands the rest of the address to /bin/sh. */
    CHECK_INT(api.quote(out, sizeof out, "a'b"), ND_OK, "quoted an apostrophe");
    CHECK_STR(out, "'a'\\''b'", "the apostrophe cannot end the quoting");
    CHECK_INT(api.quote(out, sizeof out, "'; rm -rf /; echo '"), ND_OK, "quoted a whole payload");
    CHECK_STR(out, "''\\''; rm -rf /; echo '\\'''", "and the payload stays one word");

    CHECK_INT(api.quote(out, sizeof out, ""), ND_OK, "quoted the empty word");
    CHECK_STR(out, "''", "which is still one word to the shell");
    CHECK(api.quote(out, 3u, "toolong") != ND_OK, "a buffer that cannot hold it is refused");
}

static void test_tunnel_command(void)
{
    char line[2048];

    CHECK_INT(api.tunnel_line(line, sizeof line, "relay.example.net", "op", "2022"), ND_OK,
              "built the command");

    /* Every option that keeps the outbound connection honest. */
    CHECK(strstr(line, "'-N' '-T'") != NULL, "no command, no tty");
    CHECK(strstr(line, "'IdentitiesOnly=yes'") != NULL, "only the key we named");
    CHECK(strstr(line, "'BatchMode=yes'") != NULL, "never prompt: nobody is there");
    CHECK(strstr(line, "'StrictHostKeyChecking=yes'") != NULL, "the relay must be the relay");
    CHECK(strstr(line, "'ExitOnForwardFailure=yes'") != NULL, "no tunnel is a failure");
    CHECK(strstr(line, "'ServerAliveInterval=30'") != NULL, "keepalive interval");
    CHECK(strstr(line, "'ServerAliveCountMax=3'") != NULL, "keepalive count");
    CHECK(strstr(line, "'ConnectTimeout=20'") != NULL, "connect timeout");
    CHECK(strstr(line, "'-R' '2022:127.0.0.1:22'") != NULL,
          "the reverse forward lands on the relay's loopback");
    CHECK(strstr(line, "'op@relay.example.net'") != NULL, "user@host is the last word");
    CHECK(strstr(line, "relay_id_ed25519'") != NULL, "and the relay key is named with -i");

    /* A relay address with an apostrophe in it is one word, still. */
    CHECK_INT(api.tunnel_line(line, sizeof line, "a'b", "op", "2022"), ND_OK, "odd host");
    CHECK(strstr(line, "'op@a'\\''b'") != NULL, "the target stays one shell word");
}

static void test_sshd_config(void)
{
    char text[4096];

    wipe_state();
    CHECK_INT(api.write_sshd_config(), ND_OK, "wrote sshd_config");
    CHECK(read_whole(ND_RS_SSHD_CONFIG, text, sizeof text), "and it is on disk");
    CHECK_INT((int)mode_of(ND_RS_SSHD_CONFIG), 0600, "at 0600");

    CHECK(strncmp(text, "# Generated by System/core/RemoteShell. Edits are overwritten.\n", 62) ==
              0,
          "it says it is generated, on the first line");
    /* The four lines that decide who can get in. Losing any one of them is
     * the difference between a phone only its owner can reach and a phone on
     * the internet. */
    CHECK(strstr(text, "\nListenAddress 127.0.0.1\n") != NULL, "loopback only, never an interface");
    CHECK(strstr(text, "\nPasswordAuthentication no\n") != NULL, "no passwords");
    CHECK(strstr(text, "\nKbdInteractiveAuthentication no\n") != NULL, "nor keyboard-interactive");
    CHECK(strstr(text, "\nPubkeyAuthentication yes\n") != NULL, "keys only");
    CHECK(strstr(text, "\nPermitEmptyPasswords no\n") != NULL, "no empty passwords");
    CHECK(strstr(text, "\nPermitRootLogin prohibit-password\n") != NULL, "root by key only");
    CHECK(strstr(text, "\nAllowTcpForwarding no\n") != NULL, "a shell, not a proxy");
    CHECK(strstr(text, "\nAllowAgentForwarding no\n") != NULL, "no agent forwarding");
    CHECK(strstr(text, "\nX11Forwarding no\n") != NULL, "no X11");
    CHECK(strstr(text, "\nStrictModes no\n") != NULL,
          "StrictModes off, because authorized_keys is not in a home directory");
    CHECK(strstr(text, "\nPort 22\n") != NULL, "LOCAL_PORT");
    CHECK(strstr(text, "Subsystem sftp /usr/libexec/sftp-server\n") != NULL, "sftp, for the mount");
    /* "No UsePAM line": this openssh is built without PAM and would warn. */
    CHECK(strstr(text, "UsePAM") == NULL, "and no UsePAM line at all");
}

static void test_tunnel_script(void)
{
    char text[4096];

    wipe_state();
    CHECK_INT(api.write_tunnel_script("relay.example.net", "op", "2022"), ND_OK, "wrote tunnel.sh");
    CHECK(read_whole(ND_RS_TUNNEL_SCRIPT, text, sizeof text), "and it is on disk");
    CHECK_INT((int)mode_of(ND_RS_TUNNEL_SCRIPT), 0700, "executable, by us alone");

    CHECK(strncmp(text, "#!/bin/sh\n", 10) == 0, "a shell script");
    /* "A reconnect loop. Mobile data drops; that is not an error." */
    CHECK(strstr(text, "while :; do\n") != NULL, "loops forever");
    CHECK(strstr(text, "    echo \"[RSHELL] dialling op@relay.example.net\"\n") != NULL,
          "says who it is dialling, with the RSHELL tag the log colours");
    CHECK(strstr(text, "    echo \"[RSHELL] connection ended ($?); retrying in 15\"\n") != NULL,
          "and why it is going round again");
    CHECK(strstr(text, "    sleep 15\n") != NULL, "RETRY_SECONDS");
    CHECK(strstr(text, "\ndone\n") != NULL, "and the loop is closed");
    CHECK(strstr(text, "'-R' '2022:127.0.0.1:22'") != NULL, "the command is the quoted one");
}

/* sshd gets the same retry loop the tunnel has, and for the same reason.
 *
 * The launcher starts Remote Shell at boot, before the network scripts have
 * finished -- deliberately, because the tunnel is a retry loop and mobile data
 * can take a minute to attach. But sshd was started ONCE, and at that point
 * loopback may not be configured yet:
 *
 *     Bind to port 22 on 127.0.0.1 failed: Address not available.
 *     Cannot bind any address.
 *
 * sshd then exits and nothing ever starts it again. The tunnel, meanwhile,
 * comes up perfectly and holds -- so the phone looks reachable from the relay,
 * every hop works, and the connection arrives at a port with nothing behind
 * it. From the other end that is indistinguishable from a rejected key, which
 * is what made this so hard to see. Whether it happens at all depends on
 * whether lo won the race that boot, which is why it worked sometimes.
 *
 * Seen on a fresh boot in QEMU with a real relay: tunnel up, sshd dead, and
 * the phone's own log naming the bind failure in its first two lines. */
static void test_sshd_script(void)
{
    char text[4096];

    wipe_state();
    CHECK(api.write_sshd_script != NULL, "the module can write an sshd runner");
    if (api.write_sshd_script == NULL)
        return;

    CHECK_INT(api.write_sshd_script(), ND_OK, "wrote sshd.sh");
    CHECK(read_whole(ND_RS_SSHD_SCRIPT, text, sizeof text), "and it is on disk");
    CHECK_INT((int)mode_of(ND_RS_SSHD_SCRIPT), 0700, "executable, by us alone");

    CHECK(strncmp(text, "#!/bin/sh\n", 10) == 0, "a shell script");
    CHECK(strstr(text, "while :; do\n") != NULL, "loops forever, as tunnel.sh does");
    CHECK(strstr(text, "'" ND_RS_SSHD "'") != NULL, "runs sshd");
    CHECK(strstr(text, "'-D'") != NULL, "in the foreground, so the loop can wait on it");
    CHECK(strstr(text, "/.remote/sshd_config'") != NULL, "with the generated config");
    CHECK(strstr(text, "    echo \"[RSHELL] sshd ended ($?); retrying in 15\"\n") != NULL,
          "says why it is going round again, with the tag the log colours");
    CHECK(strstr(text, "    sleep 15\n") != NULL, "RETRY_SECONDS, the same as the tunnel");
    CHECK(strstr(text, "\ndone\n") != NULL, "and the loop is closed");
}

/* ------------------------------------------------------------------ *
 * 3. install_keys_from_card()
 * ------------------------------------------------------------------ */

static char g_card[ND_PATH_MAX];

static bool card_file(const char *name, const char *text)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/remote/%s", g_card, name) != ND_OK)
        return false;
    f = fopen(path, "wb");
    if (f == NULL)
        return false;
    (void)fputs(text, f);
    (void)fclose(f);
    return true;
}

static void test_install_keys(void)
{
    char taken[ND_RS_TAKEN_MAX];
    char why[ND_RS_ERRMSG_MAX];
    char card_virtual[ND_PATH_MAX];
    char remote_dir[ND_PATH_MAX];
    nd_rs_settings s;

    wipe_state();

    /* The card is addressed the way the app addresses it: an absolute
     * /NeoDCT path that nd_path_resolve() puts under the scratch root. */
    (void)nd_strlcpy(card_virtual, "/NeoDCT/User/sdcard", sizeof card_virtual);
    (void)nd_snprintf(remote_dir, sizeof remote_dir, "%s/remote", card_virtual);
    CHECK_INT(nd_mkdir_p(card_virtual, 0755u), ND_OK, "a card is mounted");
    (void)nd_snprintf(g_card, sizeof g_card, "%s%s", g_root, card_virtual);

    /* No remote/ folder yet. */
    CHECK(api.install_keys(card_virtual, taken, sizeof taken, why, sizeof why) != ND_OK,
          "a card with no remote/ folder is refused");
    CHECK_STR(why, "No \"remote\" folder on the card.", "and says so, with the folder named");

    CHECK_INT(nd_mkdir_p(remote_dir, 0755u), ND_OK, "the operator made remote/");
    CHECK(api.install_keys(card_virtual, taken, sizeof taken, why, sizeof why) != ND_OK,
          "an empty remote/ folder is refused too");
    CHECK_STR(why, "No keys in remote/ on the card.", "and says which folder is empty");

    /* All three, plus the optional relay.conf. */
    CHECK(card_file("id_ed25519", "PRIVATE KEY\n"), "wrote id_ed25519");
    CHECK(card_file("authorized_keys", "ssh-ed25519 AAAA operator\n"), "wrote authorized_keys");
    CHECK(card_file("known_hosts", "[relay]:2022 ssh-ed25519 AAAA\n"), "wrote known_hosts");
    CHECK(card_file("relay.conf", "host=relay.example.net\nport=2022\n"), "wrote relay.conf");

    CHECK_INT(api.install_keys(card_virtual, taken, sizeof taken, why, sizeof why), ND_OK,
              "the import succeeds");
    /* The app's `", ".join(sorted(taken))`, which is what the dialog shows. */
    CHECK_STR(taken, "authorized_keys, id_ed25519, known_hosts, relay.conf", "sorted and joined");

    /* "ssh refuses a private key the world can read, and it is right to." */
    CHECK_INT((int)mode_of(ND_RS_RELAY_KEY), 0600, "the relay key is 0600");
    CHECK_INT((int)mode_of(ND_RS_AUTHORIZED_KEYS), 0600, "authorized_keys is 0600");
    CHECK_INT((int)mode_of(ND_RS_KNOWN_HOSTS), 0600, "known_hosts is 0600");
    CHECK(api.have_keys(), "and both directions now have what they need");

    /* relay.conf named the host and the port and left the login alone --
     * conf.get("user") is None, which means "do not change it". */
    api.settings_get(&s);
    CHECK_STR(s.host, "relay.example.net", "the card's relay address was applied");
    CHECK_STR(s.port, "2022", "and its port");
    CHECK_STR(s.user, "neodct", "the login it did not name is untouched");
}

/* ------------------------------------------------------------------ *
 * 4. check_ready(), in the Python's order
 * ------------------------------------------------------------------ */

static void test_check_ready_order(void)
{
    nd_rs_settings s;
    char why[ND_RS_ERRMSG_MAX];
    struct stat st;
    bool have_sshd = (stat(ND_RS_SSHD, &st) == 0 && S_ISREG(st.st_mode));

    wipe_state();

    /* The first refusal is about the IMAGE, not the configuration, and it is
     * checked against the real /usr/sbin/sshd -- an executable is never
     * ND_ROOT-resolved (nd_proc.h). On a host without openssh that is the
     * only branch reachable, which is the same hole the Python's own suite
     * has (spec-build-test.md line 1442). */
    if (!have_sshd) {
        CHECK(api.check_ready(&s, why, sizeof why) != ND_OK, "no ssh server -> refused");
        CHECK_STR(why, "This build has no ssh server.", "and says the build has none");
        printf("  (no %s on this host: the later check_ready branches are unreachable)\n",
               ND_RS_SSHD);
        return;
    }

    CHECK(api.check_ready(&s, why, sizeof why) != ND_OK, "no relay address -> refused");
    CHECK_STR(why, "No relay address set.", "the second refusal");

    CHECK_INT(api.settings_save("relay.example.net", NULL, NULL, NULL), ND_OK, "relay set");
    CHECK(api.check_ready(&s, why, sizeof why) != ND_OK, "no relay key -> refused");
    CHECK_STR(why, "No relay key. Copy one from the card.", "the third refusal");

    CHECK(write_whole(ND_RS_RELAY_KEY, "k\n"), "relay key present");
    CHECK(api.check_ready(&s, why, sizeof why) != ND_OK, "no authorized_keys -> refused");
    CHECK_STR(why, "No authorized_keys. Copy one from the card.", "the fourth refusal");

    CHECK(write_whole(ND_RS_AUTHORIZED_KEYS, "k\n"), "authorized_keys present");
    CHECK(api.check_ready(&s, why, sizeof why) != ND_OK, "no known_hosts -> refused");
    CHECK_STR(why, "No known_hosts for the relay. Copy one from the card.", "the fifth refusal");

    CHECK(write_whole(ND_RS_KNOWN_HOSTS, "h\n"), "known_hosts present");
    CHECK_INT(api.check_ready(&s, why, sizeof why), ND_OK, "and then it is ready");
    CHECK_STR(s.host, "relay.example.net", "with the settings handed back");
}

/* ------------------------------------------------------------------ *
 * 7. The pid pair -- the check that stopped this module bricking a boot
 * ------------------------------------------------------------------ */

static void test_pid_and_ownership(void)
{
    char pidtext[32];
    nd_rs_status state;

    wipe_state();
    CHECK_INT(api.pid_from(ND_RS_SSHD_PID), -1, "a missing pid file is None");

    CHECK_INT(nd_mkdir_p(ND_RS_USER_DIR, 0700u), ND_OK, "state directory");
    CHECK(write_whole(ND_RS_SSHD_PID, "not a number\n"), "wrote a junk pid file");
    CHECK_INT(api.pid_from(ND_RS_SSHD_PID), -1, "which is ValueError, and also None");

    (void)nd_snprintf(pidtext, sizeof pidtext, "%ld\n", (long)getpid());
    CHECK(write_whole(ND_RS_SSHD_PID, pidtext), "wrote our own pid");
    CHECK_INT((long)api.pid_from(ND_RS_SSHD_PID), (long)getpid(), "and it reads back");

    /* THIS IS THE ONE THAT MATTERS. The pid is live, so a naive check would
     * signal it -- and on the phone that was the launcher's process group.
     * /proc says this process is a test binary and not sshd, so it is not
     * ours and nothing is signalled. */
    CHECK(!api.owns(getpid(), "sshd"), "a live pid that is not sshd is not ours");
    CHECK(!api.owns(getpid(), "tunnel.sh"), "nor is it the tunnel");
    CHECK(api.owns(getpid(), "test_remoteshell"), "but /proc does name this process");
    CHECK(!api.owns(-1, "sshd"), "a negative pid is not ours");
    CHECK(!api.owns(0, "sshd"), "and neither is pid 0, which is Python's falsy None");

    api.status_get(&state);
    CHECK(!state.sshd, "so status() says sshd is not running");
    CHECK(!state.tunnel, "nor the tunnel");

    /* stop() is safe when it is already down, and forgets the stale file
     * either way -- Python unlinks it whether or not it signalled. */
    /* sshd runs under a loop now, and the loop has its own pid file. Off has
     * to mean off: a loop left behind would start sshd again fifteen seconds
     * after the phone was told to stop. */
    CHECK(write_whole(ND_RS_SSHD_LOOP_PID, "not a number\n"), "wrote a stale loop pid file");

    CHECK_INT(api.stop(&state, true), ND_OK, "stop on an already-stopped phone");
    CHECK(!nd_path_exists(ND_RS_SSHD_PID), "the stale pid file is gone");
    CHECK(!nd_path_exists(ND_RS_SSHD_LOOP_PID), "and so is the loop's");
    CHECK(!state.sshd && !state.tunnel, "and it is still down");
}

/* ------------------------------------------------------------------ *
 * The host key and the fingerprint
 * ------------------------------------------------------------------ */

static void test_host_key(void)
{
    char why[ND_RS_ERRMSG_MAX];
    char fingerprint[ND_RS_ERRMSG_MAX];
    struct stat st;
    bool have_keygen = (stat(ND_RS_KEYGEN, &st) == 0 && S_ISREG(st.st_mode));

    wipe_state();

    /* An empty fingerprint whenever there is no public key, which is what the
     * app turns into "unknown". */
    CHECK_INT(api.fingerprint(fingerprint, sizeof fingerprint), ND_OK, "fingerprint answers");
    CHECK_STR(fingerprint, "", "with nothing, when there is no key");

    if (!have_keygen) {
        /* The Python raises FileNotFoundError out of subprocess.call here and
         * reaches the crash screen; the C reports the message instead. Named
         * in rshell.c note 4. */
        CHECK(api.ensure_host_key(why, sizeof why) != ND_OK, "no ssh-keygen -> refused");
        CHECK_STR(why, "Could not make a host key.", "with the Python's message");
        return;
    }

    CHECK_INT(api.ensure_host_key(why, sizeof why), ND_OK, "the host key is made");
    CHECK_INT((int)mode_of(ND_RS_HOST_KEY), 0600, "at 0600");
    /* "The phone's own identity, made once and kept": a second call must not
     * make a second key. */
    CHECK_INT(api.ensure_host_key(why, sizeof why), ND_OK, "asking again is a no-op");
    CHECK_INT(api.fingerprint(fingerprint, sizeof fingerprint), ND_OK, "fingerprint answers");
    CHECK(strstr(fingerprint, "SHA256:") != NULL, "and now names a SHA256 fingerprint");
    CHECK(strchr(fingerprint, '\n') == NULL, "stripped, so it fits one dialog line");
}

/* An empty host key file is not a host key, and treating it as one leaves the
 * phone unreachable for good.
 *
 * ssh-keygen creates the file before it has any key material to put in it, so
 * an interrupted run leaves nothing behind but a zero-byte file -- and on a
 * first boot it can sit there a long while, because generating a key wants
 * entropy the kernel has not gathered yet. ensure_host_key() asked only
 * whether the file EXISTED, so every start after that skipped regeneration,
 * sshd exited with "no hostkeys available", and the tunnel came up to a phone
 * with nothing listening at the other end of it. Silently: the only thing the
 * person trying to log in sees is a refused connection.
 *
 * Found on a real boot, not by reading this code -- the phone's own remote.log
 * named it in one line while the file listing showed a 0-byte key. */
static void test_empty_host_key_is_replaced(void)
{
    char why[ND_RS_ERRMSG_MAX];
    char body[512];
    struct stat st;

    if (stat(ND_RS_KEYGEN, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("  (no %s on this host: host key regeneration is unreachable)\n", ND_RS_KEYGEN);
        return;
    }

    wipe_state();
    /* save_settings() is what makes the directory; nothing else here does. */
    CHECK_INT(api.settings_save("relay.example.net", NULL, NULL, NULL), ND_OK,
              "a .remote directory to write into");
    CHECK(write_whole(ND_RS_HOST_KEY, ""), "an empty key, as an interrupted keygen leaves it");

    CHECK_INT(api.ensure_host_key(why, sizeof why), ND_OK, "ensure_host_key answers");
    CHECK(read_whole(ND_RS_HOST_KEY, body, sizeof body) && body[0] != '\0',
          "the empty key is replaced, not kept");
    CHECK_INT((int)mode_of(ND_RS_HOST_KEY), 0600, "and what replaces it is 0600");
}

/* Neither is a half-written one. A phone loses power, and ssh-keygen is not
 * atomic: what is left is a file with bytes in it that no longer parses.
 * sshd says "error in libcrypto" and exits exactly as it does for an empty
 * one, so a size check alone still leaves the phone wedged for good -- the
 * key has to be one openssh can actually load. */
static void test_corrupt_host_key_is_replaced(void)
{
    char why[ND_RS_ERRMSG_MAX];
    char body[512];
    struct stat st;
    const char *rubbish = "-----BEGIN OPENSSH PRIVATE KEY-----\ntruncated here\n";

    if (stat(ND_RS_KEYGEN, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("  (no %s on this host: host key validation is unreachable)\n", ND_RS_KEYGEN);
        return;
    }

    wipe_state();
    CHECK_INT(api.settings_save("relay.example.net", NULL, NULL, NULL), ND_OK,
              "a .remote directory to write into");
    CHECK(write_whole(ND_RS_HOST_KEY, rubbish), "a key that stops halfway");

    CHECK_INT(api.ensure_host_key(why, sizeof why), ND_OK, "ensure_host_key answers");
    CHECK(read_whole(ND_RS_HOST_KEY, body, sizeof body) && strcmp(body, rubbish) != 0,
          "the unloadable key is replaced, not kept");
    CHECK_INT((int)mode_of(ND_RS_HOST_KEY), 0600, "and what replaces it is 0600");
}

/* ------------------------------------------------------------------ *
 * 8. app_run()
 * ------------------------------------------------------------------ */

static void test_back_leaves(void)
{
    sa_fixture fx;
    int rc;

    wipe_state();
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* VerticalList does not flush before its first draw, so a queued press is
     * enough -- and Back is the one answer that reaches no dialog. */
    CHECK(sa_send(&fx, ND_KEY_CLEAR), "queued Back");

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    nd_vclock_disable();

    CHECK_INT(rc, 0, "Back on the menu returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame: the menu");
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    api.settings_get(NULL);
    api.status_get(NULL);
    api.menu_lines(NULL, 0u);
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("RemoteShell", "ndrshell");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndrshell-root", g_root, sizeof g_root)) {
        (void)dlclose(h);
        return 1;
    }
    root_to_scratch();

    RUN(test_settings_defaults);
    RUN(test_settings_save);
    RUN(test_settings_ignores_a_broken_file);
    RUN(test_running_word);
    RUN(test_menu_lines);
    RUN(test_dialog_strings);
    RUN(test_quote);
    RUN(test_tunnel_command);
    RUN(test_sshd_config);
    RUN(test_tunnel_script);
    RUN(test_sshd_script);
    RUN(test_install_keys);
    RUN(test_check_ready_order);
    RUN(test_pid_and_ownership);
    RUN(test_host_key);
    RUN(test_empty_host_key_is_replaced);
    RUN(test_corrupt_host_key_is_replaced);
    RUN(test_back_leaves);
    RUN(test_null_safety);

    root_restore();
    rc = sa_end(h, "test_remoteshell");
    sa_rmtree(g_root);
    return rc;
}
