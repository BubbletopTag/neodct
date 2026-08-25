/* rshell.c -- ssh and sftp to the phone, over the internet, on purpose.
 *
 * A one-to-one port of System/core/RemoteShell/__init__.py (487 lines). That
 * module's docstring is the specification for the whole subsystem and is
 * reproduced rather than summarised, because every line of it is a decision
 * about who can get into this phone:
 *
 *     Development on this phone means a serial console, which means two wires
 *     soldered to pads with no strain relief, which means they come off. This
 *     is the replacement: an ssh session and an sftp mount, reached from
 *     anywhere, without opening the phone.
 *
 *     The phone is behind carrier-grade NAT and its mobile data is IPv6, so
 *     nothing can connect *to* it. So it connects out, and carries a way back
 *     with it:
 *
 *         phone  ---- ssh -R, key auth ---->  relay (a VPS with a public
 *                                               address)
 *                                               ^
 *         laptop ---- ssh -J -------------------'
 *
 *     What that buys, and why it is shaped this way:
 *
 *       * sshd binds 127.0.0.1 and nothing else. Not the modem's interface,
 *         not wlan, not "0.0.0.0 but the firewall will save us". The only
 *         route in is the tunnel the phone itself dialled, so a phone with
 *         Remote Shell on and no relay reachable is a phone with no way in at
 *         all.
 *       * The forwarded port lands on the relay's loopback (the relay keeps
 *         GatewayPorts off), so reaching the phone means getting into the
 *         relay first. The phone is not sitting on a public port waiting for
 *         the internet to find it.
 *       * Keys only, both directions. Password authentication is off, and so
 *         is keyboard-interactive -- turning one off and leaving the other is
 *         a classic way to think you did this and not have.
 *       * The relay's host key is checked. Dialling out with
 *         StrictHostKeyChecking=no would mean anything that can answer on
 *         that address gets offered the tunnel.
 *       * It is off until somebody turns it on, and it says so on the screen
 *         while it is running.
 *
 *     Nothing here is subtle enough to deserve being clever. Everything that
 *     decides who can get in is written into the config this module
 *     generates, and generated fresh every time it starts, so editing it by
 *     hand on the phone cannot quietly weaken it and survive.
 *
 * rshell.h says why this file is in an app directory instead of lib/, and what
 * that costs. Read that first.
 *
 * ============ FIVE PLACES THE C HAD TO SAY SOMETHING ============
 *
 * 1. start_new_session=True HAS NO nd_proc_spawn EQUIVALENT. nd_proc_spec
 *    carries argv, envp, a descriptor plan and an owner tag, and no way to ask
 *    for setsid(). It is not optional here: stop() signals the process GROUP,
 *    and without a new session that group is the core's own -- which is
 *    exactly the failure the _owns() comment in rshell.h describes, except
 *    self-inflicted and every time. So this file forks sshd and the tunnel
 *    itself, doing only setsid/dup2/execve/_exit in the child, which is the
 *    sequence CODING-STANDARDS.md 1.1 and spec-storage-settings.md risk R-7
 *    both spell out. The clean fix is a `new_session` flag on nd_proc_spec;
 *    that is a shared header and this work package may not touch it, so it is
 *    reported instead of added.
 *
 * 2. GENERATED PATHS ARE ND_ROOT-RESOLVED, INCLUDING INSIDE THE FILES. On the
 *    phone ND_ROOT is empty and every string below is byte-for-byte the
 *    Python's. Under the host test harness it is a scratch directory, and a
 *    config naming /NeoDCT/User/.remote while the key sits under the scratch
 *    root would be a file that disagrees with itself. Resolving both keeps the
 *    generated config pointing at the tree the keys were actually written to.
 *
 * 3. /proc IS NOT ND_ROOT-RESOLVED. It is the kernel's, not the phone's
 *    layout, and _owns() has to read the real one or it answers about nothing.
 *
 * 4. THREE OSErrors THAT ARE CRASHES IN PYTHON ARE MESSAGES HERE. os.open() in
 *    write_sshd_config/write_tunnel_script, open(LOG_FILE) in start(), and
 *    subprocess.call([KEYGEN, ...]) in ensure_host_key() when ssh-keygen is
 *    missing all raise an OSError that no caller catches -- the app catches
 *    RemoteShellError only, so each reaches the crash screen. Reproducing a
 *    crash screen for "the user partition is read-only" would be worse than
 *    the Python, so the C reports the reason in the dialog the app already
 *    has. Named here rather than left to be discovered.
 *
 * 5. THE FILE COPY IS CHUNKED. install_keys_from_card() reads each card file
 *    whole into memory in the Python. These come off an SD card, so the C
 *    streams them in 8 KB pieces instead (SECURITY.md: nothing off a card gets
 *    to size an allocation). The bytes written are identical.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_props.h"
#include "nd_types.h"

#include "rshell.h"

extern char **environ;

/* CARD_FILES, in the Python dict's insertion order. */
const char *const nd_rs_card_file_names[ND_RS_CARD_FILE_COUNT] = {"id_ed25519", "authorized_keys",
                                                                  "known_hosts"};
const char *const nd_rs_card_file_dests[ND_RS_CARD_FILE_COUNT] = {
    ND_RS_RELAY_KEY, ND_RS_AUTHORIZED_KEYS, ND_RS_KNOWN_HOSTS};

/* One generated file's worth of text. sshd_config is ~700 bytes and tunnel.sh
 * is ~600 with a 255-character relay address in it; 4 KB is the round number
 * above both that still costs nothing on the stack. */
#define RS_TEXT_MAX 4096

/* /proc/<pid>/cmdline for anything this module started. sshd's is short; the
 * tunnel's is "/bin/sh\0/NeoDCT/User/.remote/tunnel.sh\0". */
#define RS_CMDLINE_MAX 4096

#define RS_COPY_CHUNK 8192

/* ------------------------------------------------------------------ *
 * Small local helpers
 * ------------------------------------------------------------------ */

static void say(char *out, size_t n, const char *msg)
{
    if (out != NULL && n > 0u)
        (void)nd_strlcpy(out, msg, n);
}

/* Python's str.strip(): both ends, the ASCII whitespace set. */
static void strip_into(char *out, size_t n, const char *in)
{
    const char *start;
    const char *end;

    if (out == NULL || n == 0u)
        return;
    out[0] = '\0';
    if (in == NULL)
        return;

    start = in;
    while (*start == ' ' || (*start >= '\t' && *start <= '\r'))
        start++;
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || (end[-1] >= '\t' && end[-1] <= '\r')))
        end--;

    {
        size_t len = (size_t)(end - start);

        if (len >= n)
            len = n - 1u;
        memcpy(out, start, len);
        out[len] = '\0';
    }
}

/* Appends to `out`, reporting truncation the way nd_snprintf does. Used to
 * build the two generated files line by line without a format string per
 * line. */
static nd_err append(char *out, size_t n, const char *text)
{
    size_t len = nd_strlcat(out, text, n);

    return (len < n) ? ND_OK : ND_ERR_TOOLONG;
}

/* mkdir -p USER_DIR and chmod 0700, which every writer in the Python does
 * before it writes. The chmod is separate because makedirs() is subject to the
 * umask and 0700 is not a suggestion here -- ssh refuses a private key whose
 * directory the world can read, and it is right to. */
static nd_err ensure_user_dir(void)
{
    char resolved[ND_PATH_MAX];
    nd_err rc;

    rc = nd_mkdir_p(ND_RS_USER_DIR, 0700u);
    if (rc != ND_OK)
        return rc;
    if (nd_path_resolve(resolved, sizeof resolved, ND_RS_USER_DIR) != ND_OK)
        return ND_ERR_TOOLONG;
    (void)chmod(resolved, 0700);
    return ND_OK;
}

/* os.open(path, O_WRONLY|O_CREAT|O_TRUNC, mode) + write + close, on an
 * ND_ROOT-resolved path. The mode is set at creation rather than chmod'ed
 * afterwards: "between the two there is a moment where the key is readable,
 * and on a phone that moment is as long as the flash is slow." */
static nd_err write_file_mode(const char *path, const char *text, mode_t mode)
{
    char resolved[ND_PATH_MAX];
    size_t len;
    ssize_t wrote;
    int fd;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return ND_ERR_TOOLONG;
    fd = open(resolved, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return ND_ERR_IO;

    len = strlen(text);
    wrote = write(fd, text, len);
    (void)close(fd);
    if (wrote < 0 || (size_t)wrote != len)
        return ND_ERR_IO;
    /* The Python chmods after writing as well, which matters when the file
     * already existed: O_CREAT's mode is ignored then. */
    (void)chmod(resolved, mode);
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * settings() and save_settings()
 * ------------------------------------------------------------------ */

void nd_rs_settings_get(nd_rs_settings *out)
{
    nd_props *values = NULL;

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    (void)nd_strlcpy(out->user, ND_RS_DEFAULT_RELAY_USER, sizeof out->user);
    (void)nd_strlcpy(out->port, ND_RS_DEFAULT_RELAY_PORT, sizeof out->port);

    /* B-3, the raw dialect: state.prop is one of the two files nd_props.h
     * names for it. A malformed file is ND_ERR_PARSE and `values` is NULL,
     * which lands on the defaults above -- in the Python that decode error
     * escapes settings() and is caught only by launcher.py's blanket handler,
     * i.e. the phone carries on with a broken state file either way. */
    if (nd_props_parse_raw(ND_RS_STATE_FILE, &values) != ND_OK || values == NULL)
        return;

    out->enabled = strcmp(nd_props_get(values, "enabled", "0"), "1") == 0;
    (void)nd_strlcpy(out->host, nd_props_get(values, "host", ""), sizeof out->host);
    (void)nd_strlcpy(out->user, nd_props_get(values, "user", ND_RS_DEFAULT_RELAY_USER),
                     sizeof out->user);
    (void)nd_strlcpy(out->port, nd_props_get(values, "port", ND_RS_DEFAULT_RELAY_PORT),
                     sizeof out->port);
    nd_props_free(values);
}

nd_err nd_rs_settings_save(const char *host, const char *user, const char *port,
                           const bool *enabled)
{
    nd_rs_settings current;
    nd_props *p;
    nd_err rc;

    nd_rs_settings_get(&current);

    if (host != NULL)
        strip_into(current.host, sizeof current.host, host);
    if (user != NULL) {
        strip_into(current.user, sizeof current.user, user);
        /* `user.strip() or DEFAULT_RELAY_USER` -- an empty login is not a
         * login, and there is no screen that could ask again. */
        if (current.user[0] == '\0')
            (void)nd_strlcpy(current.user, ND_RS_DEFAULT_RELAY_USER, sizeof current.user);
    }
    if (port != NULL) {
        strip_into(current.port, sizeof current.port, port);
        if (current.port[0] == '\0')
            (void)nd_strlcpy(current.port, ND_RS_DEFAULT_RELAY_PORT, sizeof current.port);
    }
    if (enabled != NULL)
        current.enabled = *enabled;

    p = nd_props_new();
    if (p == NULL)
        return ND_ERR_NOMEM;

    rc = nd_props_set(p, "enabled", current.enabled ? "1" : "0");
    if (rc == ND_OK)
        rc = nd_props_set(p, "host", current.host);
    if (rc == ND_OK)
        rc = nd_props_set(p, "user", current.user);
    if (rc == ND_OK)
        rc = nd_props_set(p, "port", current.port);
    if (rc == ND_OK) {
        /* trailing_nl_when_empty=false: _write_props writes "key=value\n" per
         * line, so an empty map would be a zero-byte file. nd_props.h keeps
         * the two writers apart for exactly this. */
        rc = nd_props_write_atomic(ND_RS_STATE_FILE, p, false);
    }
    nd_props_free(p);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Keys
 * ------------------------------------------------------------------ */

bool nd_rs_have_keys(void)
{
    return nd_path_is_file(ND_RS_RELAY_KEY) && nd_path_is_file(ND_RS_AUTHORIZED_KEYS);
}

/* One card file onto the user partition, at 0600 from the first byte. */
static nd_err copy_at_0600(const char *src, const char *dest)
{
    char src_r[ND_PATH_MAX];
    char dest_r[ND_PATH_MAX];
    char buf[RS_COPY_CHUNK];
    nd_err rc = ND_OK;
    int in = -1;
    int outfd = -1;

    if (nd_path_resolve(src_r, sizeof src_r, src) != ND_OK ||
        nd_path_resolve(dest_r, sizeof dest_r, dest) != ND_OK)
        return ND_ERR_TOOLONG;

    in = open(src_r, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return ND_ERR_IO;
    outfd = open(dest_r, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (outfd < 0) {
        rc = ND_ERR_IO;
        goto done;
    }

    for (;;) {
        ssize_t got = read(in, buf, sizeof buf);
        ssize_t put;

        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            rc = ND_ERR_IO;
            goto done;
        }
        put = write(outfd, buf, (size_t)got);
        if (put != got) {
            rc = ND_ERR_IO;
            goto done;
        }
    }
    /* os.chmod(destination, 0o600) after the write: O_CREAT's mode does
     * nothing when the file was already there from a previous import. */
    (void)fchmod(outfd, 0600);

done:
    if (outfd >= 0)
        (void)close(outfd);
    (void)close(in);
    return rc;
}

/* strcmp order, which is what Python's sorted() gives for these names. */
static void sort_names(const char **names, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        const char *key = names[i];
        size_t j = i;

        while (j > 0u && strcmp(names[j - 1u], key) > 0) {
            names[j] = names[j - 1u];
            j--;
        }
        names[j] = key;
    }
}

nd_err nd_rs_install_keys_from_card(const char *card_root, char *taken, size_t n, char *errmsg,
                                    size_t errn)
{
    char source[ND_PATH_MAX];
    char path[ND_PATH_MAX];
    const char *names[ND_RS_CARD_FILE_COUNT + 1u];
    size_t n_taken = 0u;
    nd_props *conf = NULL;
    size_t i;

    if (taken != NULL && n > 0u)
        taken[0] = '\0';
    if (errmsg != NULL && errn > 0u)
        errmsg[0] = '\0';
    if (card_root == NULL)
        return ND_ERR_INVAL;

    if (nd_snprintf(source, sizeof source, "%s/%s", card_root, ND_RS_CARD_DIR) != ND_OK)
        return ND_ERR_TOOLONG;

    if (!nd_path_is_dir(source)) {
        say(errmsg, errn, "No \"" ND_RS_CARD_DIR "\" folder on the card.");
        return ND_ERR_NOTFOUND;
    }

    if (ensure_user_dir() != ND_OK) {
        say(errmsg, errn, "Cannot write to the user partition.");
        return ND_ERR_IO;
    }

    for (i = 0u; i < (size_t)ND_RS_CARD_FILE_COUNT; i++) {
        if (nd_snprintf(path, sizeof path, "%s/%s", source, nd_rs_card_file_names[i]) != ND_OK)
            continue;
        if (!nd_path_is_file(path))
            continue;
        if (copy_at_0600(path, nd_rs_card_file_dests[i]) != ND_OK) {
            /* Python lets the OSError out; there is no partial-import screen.
             * Reporting it beats a crash screen -- see note 4 in the header. */
            say(errmsg, errn, "Could not copy the keys off the card.");
            return ND_ERR_IO;
        }
        names[n_taken] = nd_rs_card_file_names[i];
        n_taken++;
    }

    /* "The relay's address, if the card names it." relay.conf is the other
     * file nd_props.h's raw dialect exists for. A malformed one is
     * ND_ERR_PARSE; the Python raises UnicodeDecodeError here, which the app
     * does not catch, so it reaches the crash screen. Reported instead. */
    if (nd_snprintf(path, sizeof path, "%s/%s", source, ND_RS_CARD_CONF) == ND_OK) {
        if (nd_props_parse_raw(path, &conf) != ND_OK) {
            say(errmsg, errn, "The card's " ND_RS_CARD_CONF " is not readable.");
            return ND_ERR_PARSE;
        }
    }
    if (conf != NULL && nd_props_count(conf) > 0u) {
        /* conf.get(k) is None for a key the file does not carry, and None
         * leaves that setting alone. */
        (void)nd_rs_settings_save(
            nd_props_has(conf, "host") ? nd_props_get(conf, "host", "") : NULL,
            nd_props_has(conf, "user") ? nd_props_get(conf, "user", "") : NULL,
            nd_props_has(conf, "port") ? nd_props_get(conf, "port", "") : NULL, NULL);
        names[n_taken] = ND_RS_CARD_CONF;
        n_taken++;
    }
    nd_props_free(conf);

    if (n_taken == 0u) {
        say(errmsg, errn, "No keys in " ND_RS_CARD_DIR "/ on the card.");
        return ND_ERR_NOTFOUND;
    }

    /* The app's `", ".join(sorted(taken))`, done here because the C hands back
     * one string rather than a list. */
    sort_names(names, n_taken);
    if (taken != NULL && n > 0u) {
        for (i = 0u; i < n_taken; i++) {
            if (i > 0u && append(taken, n, ", ") != ND_OK)
                return ND_ERR_TOOLONG;
            if (append(taken, n, names[i]) != ND_OK)
                return ND_ERR_TOOLONG;
        }
    }
    return ND_OK;
}

/* subprocess.call/check_output: spawn, optionally capture stdout, wait.
 * `capture` NULL means both streams go to /dev/null, which is what
 * ensure_host_key asks for. Returns the child's exit status, or -1 when it
 * could not be started at all (Python's FileNotFoundError). */
static int run_tool(const char *const *argv, char *capture, size_t cap_n)
{
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int devnull = -1;
    int pipefd[2] = {-1, -1};
    int status = -1;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;

    devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0)
        return -1;

    if (capture != NULL) {
        if (cap_n > 0u)
            capture[0] = '\0';
        /* pipe2 with O_CLOEXEC, not pipe(): the child must not inherit the
         * READ end, or the read below never sees end-of-file and the
         * fingerprint screen hangs until the app is killed. */
        if (pipe2(pipefd, O_CLOEXEC) != 0) {
            (void)close(devnull);
            return -1;
        }
        spec.fds[0].child_fd = 1;
        spec.fds[0].our_fd = pipefd[1];
        spec.fds[1].child_fd = 2;
        spec.fds[1].our_fd = devnull;
        spec.n_fds = 2u;
    } else {
        spec.fds[0].child_fd = 1;
        spec.fds[0].our_fd = devnull;
        spec.fds[1].child_fd = 2;
        spec.fds[1].our_fd = devnull;
        spec.n_fds = 2u;
    }

    if (nd_proc_spawn(argv[0], &spec, &pid) != ND_OK)
        goto done;

    if (capture != NULL) {
        size_t used = 0u;

        (void)close(pipefd[1]);
        pipefd[1] = -1;
        for (;;) {
            ssize_t got;

            if (cap_n == 0u || used + 1u >= cap_n)
                break;
            got = read(pipefd[0], capture + used, cap_n - 1u - used);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            used += (size_t)got;
        }
        if (cap_n > 0u)
            capture[used] = '\0';
    }

    memset(&st, 0, sizeof st);
    if (nd_proc_wait(pid, -1.0, &st) == ND_OK)
        status = st.exited ? st.exit_status : 128 + st.signo;

done:
    if (pipefd[0] >= 0)
        (void)close(pipefd[0]);
    if (pipefd[1] >= 0)
        (void)close(pipefd[1]);
    (void)close(devnull);
    return status;
}

nd_err nd_rs_ensure_host_key(char *errmsg, size_t n)
{
    char key[ND_PATH_MAX];
    const char *argv[9];

    if (errmsg != NULL && n > 0u)
        errmsg[0] = '\0';

    if (nd_path_is_file(ND_RS_HOST_KEY))
        return ND_OK;
    if (ensure_user_dir() != ND_OK) {
        say(errmsg, n, "Could not make a host key.");
        return ND_ERR_IO;
    }
    if (nd_path_resolve(key, sizeof key, ND_RS_HOST_KEY) != ND_OK) {
        say(errmsg, n, "Could not make a host key.");
        return ND_ERR_TOOLONG;
    }

    argv[0] = ND_RS_KEYGEN;
    argv[1] = "-q";
    argv[2] = "-t";
    argv[3] = "ed25519";
    argv[4] = "-N";
    argv[5] = ""; /* no passphrase: nobody is there to type one */
    argv[6] = "-f";
    argv[7] = key;
    argv[8] = NULL;
    /* subprocess.call ignores the return code here; the file's existence is
     * the test. A missing ssh-keygen is a FileNotFoundError in the Python and
     * a crash screen -- see note 4 in the header -- and lands on the same
     * message below in the C. */
    (void)run_tool(argv, NULL, 0u);

    if (!nd_path_is_file(ND_RS_HOST_KEY)) {
        say(errmsg, n, "Could not make a host key.");
        return ND_ERR_IO;
    }
    (void)chmod(key, 0600);
    return ND_OK;
}

nd_err nd_rs_host_fingerprint(char *out, size_t n)
{
    char pub[ND_PATH_MAX];
    char buf[256];
    const char *argv[4];
    int status;

    if (out == NULL || n == 0u)
        return ND_ERR_INVAL;
    out[0] = '\0';

    if (!nd_path_is_file(ND_RS_HOST_KEY_PUB))
        return ND_OK; /* `return ""` */
    if (nd_path_resolve(pub, sizeof pub, ND_RS_HOST_KEY_PUB) != ND_OK)
        return ND_OK;

    argv[0] = ND_RS_KEYGEN;
    argv[1] = "-lf";
    argv[2] = pub;
    argv[3] = NULL;
    status = run_tool(argv, buf, sizeof buf);
    /* CalledProcessError and OSError are both caught and both return "". */
    if (status != 0)
        return ND_OK;

    strip_into(out, n, buf);
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * The generated sshd_config
 * ------------------------------------------------------------------ */

nd_err nd_rs_write_sshd_config(void)
{
    /* "Rewritten on every start, deliberately. A config file that persists is
     * a config file somebody edits at 2am to get past something, and then it
     * stays edited. This one is generated from here every time, so what the
     * source says is what the phone runs." */
    char text[RS_TEXT_MAX];
    char host_key[ND_PATH_MAX];
    char auth[ND_PATH_MAX];
    char pidfile[ND_PATH_MAX];
    char line[ND_PATH_MAX + 32];
    nd_err rc;

    rc = ensure_user_dir();
    if (rc != ND_OK)
        return rc;

    if (nd_path_resolve(host_key, sizeof host_key, ND_RS_HOST_KEY) != ND_OK ||
        nd_path_resolve(auth, sizeof auth, ND_RS_AUTHORIZED_KEYS) != ND_OK ||
        nd_path_resolve(pidfile, sizeof pidfile, ND_RS_SSHD_PID) != ND_OK)
        return ND_ERR_TOOLONG;

    text[0] = '\0';
    rc = append(text, sizeof text,
                "# Generated by System/core/RemoteShell. Edits are overwritten.\n"
                /* never a public interface */
                "ListenAddress 127.0.0.1\n");
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line, "Port %d\n", ND_RS_LOCAL_PORT);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line, "HostKey %s\n", host_key);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line, "AuthorizedKeysFile %s\n", auth);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK) {
        rc = append(text, sizeof text,
                    "PermitRootLogin prohibit-password\n"
                    "PasswordAuthentication no\n"
                    "KbdInteractiveAuthentication no\n"
                    "ChallengeResponseAuthentication no\n"
                    "PermitEmptyPasswords no\n"
                    /* authorized_keys lives on the user partition, not in a
                     * home directory, because / and /NeoDCT/System are
                     * read-only squashfs and there is nowhere else to put a
                     * writable file. StrictModes walks every directory above
                     * it and refuses the file if any of them is group- or
                     * world-writable -- and /NeoDCT/User is the root of a
                     * filesystem whose mode is whatever mkfs left, which a
                     * reflash resets. It refuses silently, reporting nothing
                     * but "Permission denied (publickey)" to the person trying
                     * to log in.
                     *
                     * What StrictModes protects against is another user
                     * editing your authorized_keys. This phone has one user
                     * and it is root, so there is no other user to protect
                     * against. */
                    "StrictModes no\n"
                    /* No UsePAM line. This openssh is built without PAM, so
                     * sshd warns "Unsupported option" and carries on -- noise
                     * in a log that should be worth reading. Its own default
                     * is no, and the two directives above already refuse every
                     * non-key path. */
                    "PubkeyAuthentication yes\n"
                    "X11Forwarding no\n"
                    "AllowAgentForwarding no\n"
                    "AllowTcpForwarding no\n" /* a shell, not a proxy */
        );
    }
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line, "PidFile %s\n", pidfile);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK)
        rc = append(text, sizeof text, "Subsystem sftp " ND_RS_SFTP_SERVER "\n");
    if (rc != ND_OK)
        return rc;

    return write_file_mode(ND_RS_SSHD_CONFIG, text, 0600);
}

/* ------------------------------------------------------------------ *
 * The tunnel, and the shell script that keeps dialling it
 * ------------------------------------------------------------------ */

nd_err nd_rs_quote(char *out, size_t n, const char *word)
{
    size_t used = 0u;
    size_t i;

    if (out == NULL || n == 0u || word == NULL)
        return ND_ERR_INVAL;

    /* "'" + word.replace("'", "'\\''") + "'" -- close the quote, escape one
     * literal apostrophe, open it again. */
    if (used + 1u >= n)
        return ND_ERR_TOOLONG;
    out[used++] = '\'';
    for (i = 0u; word[i] != '\0'; i++) {
        const char *piece = (word[i] == '\'') ? "'\\''" : NULL;

        if (piece != NULL) {
            size_t len = strlen(piece);

            if (used + len + 1u >= n)
                return ND_ERR_TOOLONG;
            memcpy(out + used, piece, len);
            used += len;
        } else {
            if (used + 2u >= n)
                return ND_ERR_TOOLONG;
            out[used++] = word[i];
        }
    }
    if (used + 2u > n)
        return ND_ERR_TOOLONG;
    out[used++] = '\'';
    out[used] = '\0';
    return ND_OK;
}

nd_err nd_rs_tunnel_command_line(char *out, size_t n, const char *host, const char *user,
                                 const char *port)
{
    char relay_key[ND_PATH_MAX];
    char known[ND_PATH_MAX];
    char known_opt[ND_PATH_MAX + 32];
    char forward[64];
    char target[512];
    const char *words[24];
    size_t n_words = 0u;
    size_t i;

    if (out == NULL || n == 0u || host == NULL || user == NULL || port == NULL)
        return ND_ERR_INVAL;
    out[0] = '\0';

    if (nd_path_resolve(relay_key, sizeof relay_key, ND_RS_RELAY_KEY) != ND_OK ||
        nd_path_resolve(known, sizeof known, ND_RS_KNOWN_HOSTS) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(known_opt, sizeof known_opt, "UserKnownHostsFile=%s", known) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(forward, sizeof forward, "%s:127.0.0.1:%d", port, ND_RS_LOCAL_PORT) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(target, sizeof target, "%s@%s", user, host) != ND_OK)
        return ND_ERR_TOOLONG;

    /* tunnel_command(): "the outbound connection, and every option that keeps
     * it honest". */
    words[n_words++] = ND_RS_SSH;
    words[n_words++] = "-N";
    words[n_words++] = "-T";
    words[n_words++] = "-i";
    words[n_words++] = relay_key;
    words[n_words++] = "-o";
    words[n_words++] = "IdentitiesOnly=yes";
    words[n_words++] = "-o";
    words[n_words++] = "BatchMode=yes"; /* never prompt: nobody is there */
    words[n_words++] = "-o";
    words[n_words++] = "StrictHostKeyChecking=yes"; /* the relay must be the relay */
    words[n_words++] = "-o";
    words[n_words++] = known_opt;
    words[n_words++] = "-o";
    words[n_words++] = "ExitOnForwardFailure=yes"; /* no tunnel is a failure, not a shell */
    words[n_words++] = "-o";
    words[n_words++] = "ServerAliveInterval=30";
    words[n_words++] = "-o";
    words[n_words++] = "ServerAliveCountMax=3";
    words[n_words++] = "-o";
    words[n_words++] = "ConnectTimeout=20";
    words[n_words++] = "-R";
    words[n_words++] = forward;
    words[n_words++] = target;

    for (i = 0u; i < n_words; i++) {
        char quoted[ND_PATH_MAX + 64];

        if (nd_rs_quote(quoted, sizeof quoted, words[i]) != ND_OK)
            return ND_ERR_TOOLONG;
        if (i > 0u && append(out, n, " ") != ND_OK)
            return ND_ERR_TOOLONG;
        if (append(out, n, quoted) != ND_OK)
            return ND_ERR_TOOLONG;
    }
    return ND_OK;
}

nd_err nd_rs_write_tunnel_script(const char *host, const char *user, const char *port)
{
    /* "A reconnect loop. Mobile data drops; that is not an error." */
    char command[RS_TEXT_MAX];
    char text[RS_TEXT_MAX];
    char line[RS_TEXT_MAX];
    nd_err rc;

    if (host == NULL || user == NULL || port == NULL)
        return ND_ERR_INVAL;

    rc = ensure_user_dir();
    if (rc != ND_OK)
        return rc;
    rc = nd_rs_tunnel_command_line(command, sizeof command, host, user, port);
    if (rc != ND_OK)
        return rc;

    text[0] = '\0';
    rc = append(text, sizeof text,
                "#!/bin/sh\n"
                "# Generated by System/core/RemoteShell. Edits are overwritten.\n"
                "while :; do\n");
    if (rc == ND_OK) {
        /* user and host go into a double-quoted shell string UNQUOTED here,
         * unlike every word of the command below. That is the Python's, and
         * it means a relay address containing `"` or `$` is interpolated by
         * the shell. Ported as-is: the address is typed by the operator on
         * the phone or copied off their own card. */
        (void)nd_snprintf(line, sizeof line, "    echo \"[RSHELL] dialling %s@%s\"\n", user, host);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line, "    %s\n", command);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line,
                          "    echo \"[RSHELL] connection ended ($?); retrying in %d\"\n",
                          ND_RS_RETRY_SECONDS);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK) {
        (void)nd_snprintf(line, sizeof line, "    sleep %d\n", ND_RS_RETRY_SECONDS);
        rc = append(text, sizeof text, line);
    }
    if (rc == ND_OK)
        rc = append(text, sizeof text, "done\n");
    if (rc != ND_OK)
        return rc;

    return write_file_mode(ND_RS_TUNNEL_SCRIPT, text, 0700);
}

/* ------------------------------------------------------------------ *
 * check_ready()
 * ------------------------------------------------------------------ */

nd_err nd_rs_check_ready(nd_rs_settings *out, char *errmsg, size_t n)
{
    nd_rs_settings current;
    struct stat st;

    if (errmsg != NULL && n > 0u)
        errmsg[0] = '\0';

    /* NOT nd_path_is_file: an executable is never ND_ROOT-resolved, and the
     * question is whether THIS IMAGE has an ssh server. */
    if (stat(ND_RS_SSHD, &st) != 0 || !S_ISREG(st.st_mode)) {
        say(errmsg, n, "This build has no ssh server.");
        return ND_ERR_NOTFOUND;
    }

    nd_rs_settings_get(&current);
    if (current.host[0] == '\0') {
        say(errmsg, n, "No relay address set.");
        return ND_ERR_NOTFOUND;
    }
    if (!nd_path_is_file(ND_RS_RELAY_KEY)) {
        say(errmsg, n, "No relay key. Copy one from the card.");
        return ND_ERR_NOTFOUND;
    }
    if (!nd_path_is_file(ND_RS_AUTHORIZED_KEYS)) {
        say(errmsg, n, "No authorized_keys. Copy one from the card.");
        return ND_ERR_NOTFOUND;
    }
    if (!nd_path_is_file(ND_RS_KNOWN_HOSTS)) {
        say(errmsg, n, "No known_hosts for the relay. Copy one from the card.");
        return ND_ERR_NOTFOUND;
    }

    if (out != NULL)
        *out = current;
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * The pid pair
 * ------------------------------------------------------------------ */

pid_t nd_rs_pid_from(const char *path)
{
    char resolved[ND_PATH_MAX];
    char buf[32];
    ssize_t got;
    long value;
    char *end;
    int fd;

    if (path == NULL || nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return -1;
    fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1; /* `except OSError: return None` */
    got = read(fd, buf, sizeof buf - 1u);
    (void)close(fd);
    if (got <= 0)
        return -1;
    buf[got] = '\0';

    errno = 0;
    value = strtol(buf, &end, 10);
    /* int(text.strip()) -- anything that is not a whole number is ValueError,
     * which the Python also turns into None. Trailing whitespace is allowed
     * because .strip() removes it; trailing anything else is not. */
    if (end == buf || errno != 0 || value <= 0 || value > 0x7fffffffL)
        return -1;
    while (*end == ' ' || (*end >= '\t' && *end <= '\r'))
        end++;
    if (*end != '\0')
        return -1;
    return (pid_t)value;
}

bool nd_rs_owns(pid_t pid, const char *needle)
{
    char path[64];
    char cmdline[RS_CMDLINE_MAX];
    size_t used = 0u;
    size_t needle_len;
    size_t i;
    int fd;

    if (pid <= 0 || needle == NULL || needle[0] == '\0')
        return false;

    /* The real /proc, never ND_ROOT-resolved. */
    (void)snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    for (;;) {
        ssize_t got;

        if (used >= sizeof cmdline)
            break;
        got = read(fd, cmdline + used, sizeof cmdline - used);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    (void)close(fd);

    /* `needle in cmdline`, where cmdline is the WHOLE file decoded -- argv
     * separators are NUL bytes and Python keeps them in the string, so the
     * search crosses them. An ASCII needle can only match ASCII bytes, and
     * UTF-8 decoding leaves those alone, so a byte-wise search over the raw
     * buffer is the same answer for every needle this module uses. */
    needle_len = strlen(needle);
    if (needle_len == 0u || needle_len > used)
        return false;
    for (i = 0u; i + needle_len <= used; i++) {
        if (memcmp(cmdline + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

/* _stop_pid(path, needle): stop the process named in `path`, if it is still
 * the right one, and then forget the file either way. */
static void stop_pid(const char *path, const char *needle)
{
    char resolved[ND_PATH_MAX];
    pid_t pid = nd_rs_pid_from(path);

    if (nd_rs_owns(pid, needle)) {
        pid_t group = getpgid(pid);

        /* "Both are started with start_new_session=True, so the group is ours
         * alone and killing it takes the ssh the tunnel script is waiting on
         * with it. 'Off' has to mean off." */
        if (group <= 0 || kill(-group, SIGTERM) != 0)
            (void)kill(pid, SIGTERM);
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) == ND_OK)
        (void)unlink(resolved);
}

void nd_rs_status_get(nd_rs_status *out)
{
    nd_rs_settings current;

    if (out == NULL)
        return;
    /* "What is actually running, not what was asked for." */
    out->sshd = nd_rs_owns(nd_rs_pid_from(ND_RS_SSHD_PID), "sshd");
    out->tunnel = nd_rs_owns(nd_rs_pid_from(ND_RS_TUNNEL_PID), "tunnel.sh");
    nd_rs_settings_get(&current);
    out->enabled = current.enabled;
}

/* ------------------------------------------------------------------ *
 * start() and stop()
 * ------------------------------------------------------------------ */

/* subprocess.Popen(argv, stdout=log, stderr=log, start_new_session=True).
 *
 * See note 1 in the header for why this is not nd_proc_spawn(): there is no
 * way to ask that function for a new session, and stop() signals the process
 * GROUP. Everything the child touches is a local by the time fork() is
 * called, and the child does only setsid, dup2, execve and _exit -- all four
 * async-signal-safe, CODING-STANDARDS.md 1.1. */
static nd_err spawn_session(const char *path, const char *const *argv, int log_fd, pid_t *pid_out)
{
    char *const *cargv = (char *const *)(uintptr_t)(const void *)argv;
    int fd = log_fd;
    pid_t pid;

    if (path == NULL || argv == NULL || pid_out == NULL)
        return ND_ERR_INVAL;

    (void)fflush(NULL); /* BEFORE the fork, so the child inherits empty buffers */

    pid = fork();
    if (pid < 0) {
        nd_log_err(ND_LOG_RSHELL, "fork: %s", strerror(errno));
        return ND_ERR_IO;
    }
    if (pid == 0) {
        /* ==== ASYNC-SIGNAL-SAFE ONLY FROM HERE TO THE execve ==== */
        if (setsid() < 0)
            _exit(126);
        if (fd >= 0) {
            if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
                _exit(126);
        }
        (void)execve(path, cargv, environ);
        _exit(127); /* only reached if execve failed */
    }

    *pid_out = pid;
    return ND_OK;
}

/* time.sleep(0.1), twenty times, waiting for sshd to write its PidFile. */
static void nap(double seconds)
{
    struct timespec req;

    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    (void)nanosleep(&req, NULL);
}

nd_err nd_rs_start(nd_rs_status *out, char *errmsg, size_t n)
{
    nd_rs_settings current;
    char config[ND_PATH_MAX];
    char script[ND_PATH_MAX];
    char log_path[ND_PATH_MAX];
    char pidtext[32];
    const char *sshd_argv[6];
    const char *tunnel_argv[3];
    const bool yes = true;
    nd_err rc;
    pid_t pid = -1;
    int log_fd = -1;
    int i;

    if (errmsg != NULL && n > 0u)
        errmsg[0] = '\0';

    rc = nd_rs_check_ready(&current, errmsg, n);
    if (rc != ND_OK)
        return rc;

    /* "The user partition's root arrives with whatever mode mkfs gave it, and
     * a reflash resets it. sshd will not read an authorized_keys file under a
     * group- or world-writable directory. StrictModes is off in the config
     * below, but fixing the mode costs nothing and means a phone is not
     * relying on that one line." The Python swallows any OSError here. */
    {
        char parent[ND_PATH_MAX];
        struct stat st;

        if (nd_path_resolve(parent, sizeof parent, ND_PATH_USER) == ND_OK &&
            stat(parent, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0022u) != 0u)
            (void)chmod(parent, 0755);
    }

    rc = nd_rs_ensure_host_key(errmsg, n);
    if (rc != ND_OK)
        return rc;
    rc = nd_rs_write_sshd_config();
    if (rc != ND_OK) {
        say(errmsg, n, "Could not write sshd_config.");
        return rc;
    }
    rc = nd_rs_write_tunnel_script(current.host, current.user, current.port);
    if (rc != ND_OK) {
        say(errmsg, n, "Could not write tunnel.sh.");
        return rc;
    }

    /* "Open the log first: everything after this is worth a record, and a
     * start that dies silently is a start nobody can debug." */
    if (nd_path_resolve(log_path, sizeof log_path, ND_RS_LOG_FILE) != ND_OK) {
        say(errmsg, n, "Could not open the log.");
        return ND_ERR_TOOLONG;
    }
    log_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (log_fd < 0) {
        say(errmsg, n, "Could not open the log.");
        return ND_ERR_IO;
    }

    (void)nd_rs_stop(NULL, false); /* never end up with two of either */

    if (nd_path_resolve(config, sizeof config, ND_RS_SSHD_CONFIG) != ND_OK ||
        nd_path_resolve(script, sizeof script, ND_RS_TUNNEL_SCRIPT) != ND_OK) {
        (void)close(log_fd);
        say(errmsg, n, "Could not start: path too long");
        return ND_ERR_TOOLONG;
    }

    sshd_argv[0] = ND_RS_SSHD;
    sshd_argv[1] = "-f";
    sshd_argv[2] = config;
    sshd_argv[3] = "-D";
    sshd_argv[4] = "-e";
    sshd_argv[5] = NULL;
    rc = spawn_session(ND_RS_SSHD, sshd_argv, log_fd, &pid);
    if (rc != ND_OK) {
        (void)close(log_fd);
        say(errmsg, n, "Could not start: cannot fork");
        return rc;
    }

    /* "sshd writes its own PidFile, but not instantly, and the app wants to
     * say 'on' the moment it is. Give it a beat, then trust the file." */
    for (i = 0; i < ND_RS_PID_WAIT_TRIES; i++) {
        if (nd_rs_owns(nd_rs_pid_from(ND_RS_SSHD_PID), "sshd"))
            break;
        nap(ND_RS_PID_WAIT_SLICE);
    }

    tunnel_argv[0] = "/bin/sh";
    tunnel_argv[1] = script;
    tunnel_argv[2] = NULL;
    rc = spawn_session("/bin/sh", tunnel_argv, log_fd, &pid);
    if (rc != ND_OK) {
        (void)close(log_fd);
        say(errmsg, n, "Could not start: cannot fork");
        return rc;
    }
    (void)nd_snprintf(pidtext, sizeof pidtext, "%ld\n", (long)pid);
    (void)write_file_mode(ND_RS_TUNNEL_PID, pidtext, 0644);

    /* The Python never closes this handle; the C has to. Both children hold
     * their own dup of it, so the log keeps being written. */
    (void)close(log_fd);

    (void)nd_rs_settings_save(NULL, NULL, NULL, &yes);
    nd_rs_status_get(out);
    return ND_OK;
}

nd_err nd_rs_stop(nd_rs_status *out, bool remember)
{
    const bool no = false;

    stop_pid(ND_RS_TUNNEL_PID, "tunnel.sh");
    stop_pid(ND_RS_SSHD_PID, "sshd");
    if (remember)
        (void)nd_rs_settings_save(NULL, NULL, NULL, &no);
    nd_rs_status_get(out);
    return ND_OK;
}

void nd_rs_start_if_enabled(void)
{
    nd_rs_settings current;
    char why[ND_RS_ERRMSG_MAX];

    /* "Called at boot. Silent when it was never turned on." */
    nd_rs_settings_get(&current);
    if (!current.enabled)
        return;
    if (nd_rs_start(NULL, why, sizeof why) != ND_OK) {
        /* print("[RSHELL] not starting: %s") -- the same bytes on the serial
         * console, painted by the same tag table. */
        nd_log(ND_LOG_RSHELL, "not starting: %s", why);
    }
}
