/* nd_broker.c -- see nd_broker.h for why any of this exists.
 *
 * Two verbs, SPAWN and WAIT, because those are the only two operations that
 * measurement showed nd-core could not do once it stopped being root.
 *
 * ============ THE RECORD IS FIXED-SIZE AND THE STRINGS ARE NOT ============
 *
 * Same shape as nd_svc: a fixed struct so a short read is a malformed record
 * rather than a buffer overrun, with the variable-length parts in one
 * NUL-separated blob whose length is carried explicitly. The blob is READ back
 * with the count, never scanned for a terminator that a hostile sender could
 * omit -- nd-core is unprivileged now, so everything arriving here is
 * untrusted input, including the lengths.
 *
 * ============ THE FDS ARRIVE SEPARATELY FROM THEIR NUMBERS ============
 *
 * SCM_RIGHTS carries descriptors; it does not carry what the child should see
 * them AS. So the request names the child_fd numbers in order and the fds come
 * in the same order, and the broker pairs them up. A mismatch in the count is
 * a malformed record.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nd_broker.h"
#include "nd_clock.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_priv.h"
#include "nd_svc.h"

/* ------------------------------------------------------------------ *
 * The wire
 * ------------------------------------------------------------------ */

typedef enum { REQ_SPAWN = 1, REQ_WAIT = 2, REQ_HALT = 3, REQ_CLOCK = 4 } nd_broker_op;

typedef struct {
    uint32_t op;
    /* SPAWN */
    char path[ND_PATH_MAX];
    char user[32]; /* "" means do not drop */
    uint32_t n_argv;
    uint32_t n_envp;
    uint32_t n_hide;
    uint32_t n_fds;
    int32_t child_fd[ND_PROC_MAX_FDS];
    uint8_t no_new_privs;
    uint8_t new_session;
    uint8_t close_others;
    uint8_t private_mounts;
    uint32_t owner;
    uint32_t blob_len;
    char blob[ND_BROKER_BLOB_MAX];
    /* WAIT */
    int64_t pid;
    double timeout_s;
    /* HALT / CLOCK */
    uint8_t reboot;
    int64_t when;
} nd_broker_req;

typedef struct {
    int32_t err; /* nd_err */
    int64_t pid;
    uint8_t exited;
    int32_t exit_status;
    uint8_t signalled;
    int32_t signo;
} nd_broker_rep;

struct nd_broker {
    int fd;    /* our end; the broker holds the other */
    pid_t pid; /* the broker process */
    bool ok;
};

/* ------------------------------------------------------------------ *
 * Sending and receiving, with descriptors
 * ------------------------------------------------------------------ */

static bool send_msg(int fd, const void *buf, size_t len, const int *fds, size_t n_fds)
{
    struct msghdr msg;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int) * ND_PROC_MAX_FDS)];
    ssize_t n;

    memset(&msg, 0, sizeof msg);
    memset(control, 0, sizeof control);
    iov.iov_base = (void *)buf;
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (n_fds > 0u) {
        struct cmsghdr *c;

        if (n_fds > (size_t)ND_PROC_MAX_FDS)
            return false;
        msg.msg_control = control;
        msg.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * n_fds);
        c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * n_fds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * n_fds);
        msg.msg_controllen = c->cmsg_len;
    }

    do {
        n = sendmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)len;
}

/* Returns the payload length, 0 on EOF, -1 on error. Any descriptors that
 * arrive are written to fds_out and *n_fds_out; a caller that did not expect
 * them must close them, because a descriptor leaked into the broker is a
 * descriptor the broker holds open forever. */
static ssize_t recv_msg(int fd, void *buf, size_t len, int *fds_out, size_t *n_fds_out)
{
    struct msghdr msg;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int) * ND_PROC_MAX_FDS)];
    struct cmsghdr *c;
    ssize_t n;

    memset(&msg, 0, sizeof msg);
    memset(control, 0, sizeof control);
    iov.iov_base = buf;
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;

    if (n_fds_out != NULL)
        *n_fds_out = 0u;

    do {
        n = recvmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n <= 0)
        return n;

    /* ONE control block, examined without CMSG_NXTHDR.
     *
     * musl's CMSG_NXTHDR compares a POINTER DIFFERENCE -- signed -- against an
     * unsigned sum, so it cannot survive -Wsign-conversion -Werror, which is
     * what the cross build uses and the host glibc build does not. That
     * difference is worth stating plainly: this file compiled cleanly on the
     * host and failed on the target, and because `make ... all` was launched
     * without reading its exit code, a stale image was tested and briefly
     * looked like a bug in the drop.
     *
     * Iterating was never needed anyway. Both ends of this socketpair are
     * ours and every request sends at most one SCM_RIGHTS block, so the first
     * header is the only one there is. MSG_CTRUNC catches the case where the
     * kernel had to drop control data -- if that happens the descriptors are
     * gone or partial and the record is not safe to act on.
     */
    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
        nd_log_err(ND_LOG_OS, "broker: control data truncated; refusing the record");
        errno = EPROTO;
        return -1;
    }

    c = CMSG_FIRSTHDR(&msg);
    if (c != NULL && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        size_t hdr = (size_t)((const char *)CMSG_DATA(c) - (const char *)c);
        size_t got;

        if ((size_t)c->cmsg_len < hdr)
            return n;
        got = ((size_t)c->cmsg_len - hdr) / sizeof(int);
        if (got > (size_t)ND_PROC_MAX_FDS)
            got = (size_t)ND_PROC_MAX_FDS;
        if (fds_out != NULL && n_fds_out != NULL) {
            memcpy(fds_out, CMSG_DATA(c), sizeof(int) * got);
            *n_fds_out = got;
        }
    }
    return n;
}

static void close_all(int *fds, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (fds[i] >= 0)
            (void)close(fds[i]);
    }
}

/* ------------------------------------------------------------------ *
 * The blob
 * ------------------------------------------------------------------ */

/* Append one NUL-terminated string. False when it would not fit, which the
 * caller turns into a refused launch rather than a truncated argv. */
static bool blob_put(char *blob, uint32_t *len, const char *s)
{
    size_t n = strlen(s) + 1u;

    if (s == NULL || (size_t)*len + n > (size_t)ND_BROKER_BLOB_MAX)
        return false;
    memcpy(blob + *len, s, n);
    *len += (uint32_t)n;
    return true;
}

/* Point `out` at the next `count` strings in the blob and NULL-terminate it.
 * Bounded by blob_len, never by a terminator: the sender chose both. */
static bool blob_take(const char *blob, uint32_t blob_len, uint32_t *off, uint32_t count,
                      const char **out, size_t out_cap)
{
    uint32_t i;

    if ((size_t)count + 1u > out_cap)
        return false;
    for (i = 0u; i < count; i++) {
        const char *s = blob + *off;
        uint32_t rest;

        if (*off >= blob_len)
            return false;
        rest = blob_len - *off;
        if (memchr(s, '\0', rest) == NULL)
            return false; /* unterminated: refuse the whole record */
        out[i] = s;
        *off += (uint32_t)strlen(s) + 1u;
    }
    out[count] = NULL;
    return true;
}

/* ------------------------------------------------------------------ *
 * The broker side
 * ------------------------------------------------------------------ */

static bool user_is_allowed(const char *user)
{
    static const char *const allowed[] = ND_BROKER_USERS;
    size_t i;

    /* An empty name is "do not drop", which is NOT waved through here any more
     * -- see root_exec_allowed(), which the caller consults instead. */
    if (user == NULL || user[0] == '\0')
        return true;
    for (i = 0u; allowed[i] != NULL; i++) {
        if (strcmp(user, allowed[i]) == 0)
            return true;
    }
    return false;
}

/* True when `path` may run WITHOUT being dropped, i.e. as root.
 *
 * This is the check that makes the broker a boundary rather than a root-exec
 * service. ND_BROKER_ROOT_EXEC has the argument for the list; the extra
 * condition on nd-apprun is here because it needs argv.
 *
 * Exact string comparison, deliberately. Not a prefix, not a realpath, not
 * "does it start with /NeoDCT/System" -- a prefix test invites "/NeoDCT/System/
 * ../../tmp/x" and a realpath here would be a second answer to a question the
 * mount table already answers. The paths that may run as root are three
 * literals, and a literal cannot be traversed into. */
static bool root_exec_allowed(const char *path, const char *const *argv, uint32_t n_argv)
{
    static const char *const allowed[] = ND_BROKER_ROOT_EXEC;
    size_t i;
    bool listed = false;

    if (path == NULL)
        return false;
    for (i = 0u; allowed[i] != NULL; i++) {
        if (strcmp(path, allowed[i]) == 0) {
            listed = true;
            break;
        }
    }
    if (!listed)
        return false;

    /* nd-apprun as root is only ever for an ENGINEERING app, and which app it
     * runs is argv[1]. Without this, one allowed path would mean every app on
     * the phone could be asked for as root. */
    if (strcmp(path, ND_PATH_ND_APPRUN) == 0) {
        static const char ENG[] = ND_PATH_ENG_APPS_DIR "/";

        if (n_argv < 2u || argv == NULL || argv[1] == NULL)
            return false;
        if (strncmp(argv[1], ENG, sizeof ENG - 1u) != 0)
            return false;
        /* And nothing may climb back out of it. */
        if (strstr(argv[1], "/../") != NULL || strstr(argv[1], "/..") == argv[1])
            return false;
    }
    return true;
}

/* `fds` is MUTATED: the descriptors are moved above the targets below, and
 * the caller closes whatever the array holds afterwards. */
static void do_spawn(const nd_broker_req *req, int *fds, size_t n_fds, nd_broker_rep *rep)
{
    const char *argv[8];
    const char *envp[24];
    const char *hide[ND_PROC_MAX_HIDE + 1];
    nd_proc_spec spec;
    uint32_t off = 0u;
    pid_t pid = -1;
    size_t i;

    memset(&spec, 0, sizeof spec);

    if (req->blob_len > (uint32_t)ND_BROKER_BLOB_MAX || n_fds != (size_t)req->n_fds ||
        req->n_fds > (size_t)ND_PROC_MAX_FDS || req->n_hide > (uint32_t)ND_PROC_MAX_HIDE) {
        rep->err = ND_ERR_INVAL;
        return;
    }
    if (!user_is_allowed(req->user)) {
        /* The one policy decision here, and it is a refusal. */
        nd_log_err(ND_LOG_OS, "broker: REFUSING to spawn as '%s'", req->user);
        rep->err = ND_ERR_PERM;
        return;
    }
    if (!blob_take(req->blob, req->blob_len, &off, req->n_argv, argv, ND_ARRAY_LEN(argv)) ||
        !blob_take(req->blob, req->blob_len, &off, req->n_envp, envp, ND_ARRAY_LEN(envp)) ||
        !blob_take(req->blob, req->blob_len, &off, req->n_hide, hide, ND_ARRAY_LEN(hide))) {
        rep->err = ND_ERR_PARSE;
        return;
    }

    /* AFTER the blob is parsed, because the nd-apprun rule needs argv[1].
     *
     * A no-drop spawn is the only way anything stays root on the far side of
     * this socket, so it is the one request that has to be argued for rather
     * than merely well-formed. */
    if (req->user[0] == '\0' && !root_exec_allowed(req->path, argv, req->n_argv)) {
        nd_log_err(ND_LOG_OS, "broker: REFUSING to run '%s' as root: not on the list", req->path);
        rep->err = ND_ERR_PERM;
        return;
    }

    spec.argv = argv;
    spec.envp = (req->n_envp > 0u) ? envp : NULL;
    spec.owner = (nd_proc_owner)req->owner;
    spec.no_new_privs = req->no_new_privs != 0u;
    spec.new_session = req->new_session != 0u;
    spec.close_others = req->close_others != 0u;
    spec.private_mounts = req->private_mounts != 0u;
    spec.hide_paths = (req->n_hide > 0u) ? hide : NULL;

    /* The uid is looked up HERE, from a name checked against a fixed list,
     * rather than taken off the wire. */
    if (req->user[0] != '\0') {
        if (!nd_priv_lookup(req->user, &spec.run_as))
            nd_log_err(ND_LOG_OS, "broker: no '%s' in this image", req->user);
    }

    /* ============ MOVE EVERY SOURCE ABOVE EVERY TARGET ============
     *
     * nd_proc_spawn() dup2()s the pairs in order and does not defend against a
     * source descriptor being clobbered by an earlier dup2. That was safe for
     * its whole life because the core always passed child_fd == our_fd -- it
     * owned the descriptors and simply kept their numbers, so every pair took
     * the fcntl no-op branch and nothing moved.
     *
     * Through the broker they are DIFFERENT numbers: SCM_RIGHTS gives the
     * receiver whatever is free in ITS table, which has no relation to what
     * the core had. So dup2(our=5 -> child=8) followed by dup2(our=8 -> child=5)
     * hands the child the same descriptor twice and loses the other.
     *
     * The phone said so, and not in the way the code reads: the browser and the
     * clock opened, but the Power app reported "cannot reach the core: Not a
     * socket" -- its NEODCT_SERVICE_FD number was pointing at the framebuffer,
     * because the two had been swapped on the way in.
     *
     * Lifting every source above every target makes the order irrelevant. */
    {
        int highest = 0;

        for (i = 0u; i < n_fds; i++) {
            if ((int)req->child_fd[i] > highest)
                highest = (int)req->child_fd[i];
        }
        for (i = 0u; i < n_fds; i++) {
            int moved;

            if (fds[i] > highest)
                continue; /* already clear of every target */
            moved = fcntl(fds[i], F_DUPFD, highest + 1);
            if (moved < 0) {
                nd_log_err(ND_LOG_OS, "broker: F_DUPFD: %s", strerror(errno));
                rep->err = ND_ERR_IO;
                return;
            }
            (void)close(fds[i]);
            fds[i] = moved;
        }
    }

    for (i = 0u; i < n_fds; i++) {
        spec.fds[i].child_fd = (int)req->child_fd[i];
        spec.fds[i].our_fd = fds[i];
    }
    spec.n_fds = n_fds;

    rep->err = (int32_t)nd_proc_spawn(req->path, &spec, &pid);
    rep->pid = (int64_t)pid;
}

static void do_wait(const nd_broker_req *req, nd_broker_rep *rep)
{
    nd_proc_status st;

    memset(&st, 0, sizeof st);
    rep->err = (int32_t)nd_proc_wait((pid_t)req->pid, req->timeout_s, &st);
    rep->pid = (int64_t)st.pid;
    rep->exited = st.exited ? 1u : 0u;
    rep->exit_status = st.exit_status;
    rep->signalled = st.signalled ? 1u : 0u;
    rep->signo = st.signo;
}

/* Never returns. */
/* The syscall and nothing else. Deliberately NOT nd_clock_set(): that looks for
 * a broker, and in the broker that would be a loop. The policy -- bounds, the
 * log line, the NEODCT_ROOT sandbox check -- has already run on the core's
 * side before the request was sent. */
static bool clock_set_raw(time_t when)
{
    struct timespec ts;

    ts.tv_sec = when;
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        nd_log_err(ND_LOG_CLOCK, "broker: clock_settime: %s", strerror(errno));
        return false;
    }
    return true;
}

static void broker_loop(int fd)
{
    /* Whatever nd-core sets AFTER the fork cannot reach here, but make the
     * invariant explicit rather than depending on call order: inside the
     * broker there is no broker to delegate to. */
    nd_broker_set_default(NULL);

    /* SIGPIPE would kill the broker the first time nd-core died mid-reply,
     * and the broker outliving the core by one message is the whole point of
     * noticing EOF below. */
    (void)signal(SIGPIPE, SIG_IGN);

    for (;;) {
        nd_broker_req req;
        nd_broker_rep rep;
        int fds[ND_PROC_MAX_FDS];
        size_t n_fds = 0u;
        ssize_t n;

        memset(fds, -1, sizeof fds);
        n = recv_msg(fd, &req, sizeof req, fds, &n_fds);
        if (n <= 0)
            break; /* EOF: nd-core is gone, and so is the reason to exist */

        memset(&rep, 0, sizeof rep);
        if ((size_t)n != sizeof req) {
            rep.err = ND_ERR_PARSE;
            close_all(fds, n_fds);
        } else if (req.op == REQ_SPAWN) {
            do_spawn(&req, fds, n_fds, &rep);
            /* Our copies go the moment the child has them, exactly as
             * nd_proc_launch_app closes its own. */
            close_all(fds, n_fds);
        } else if (req.op == REQ_WAIT) {
            close_all(fds, n_fds);
            do_wait(&req, &rep);
        } else if (req.op == REQ_HALT) {
            close_all(fds, n_fds);
            rep.err = nd_svc_halt_now(req.reboot != 0u) ? (int32_t)ND_OK : (int32_t)ND_ERR_IO;
        } else if (req.op == REQ_CLOCK) {
            close_all(fds, n_fds);
            /* The bounds check already ran on the core's side; this is the
             * syscall it could no longer make. */
            rep.err = clock_set_raw((time_t)req.when) ? (int32_t)ND_OK : (int32_t)ND_ERR_IO;
        } else {
            close_all(fds, n_fds);
            rep.err = ND_ERR_INVAL;
        }

        if (!send_msg(fd, &rep, sizeof rep, NULL, 0u))
            break;
    }
    _exit(0);
}

/* ------------------------------------------------------------------ *
 * The core side
 * ------------------------------------------------------------------ */

nd_broker *nd_broker_start(void)
{
    nd_broker *b;
    int sv[2];
    pid_t pid;

    b = (nd_broker *)calloc(1u, sizeof *b);
    if (b == NULL)
        return NULL;
    b->fd = -1;
    b->pid = -1;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        nd_log_err(ND_LOG_OS, "broker: socketpair: %s", strerror(errno));
        free(b);
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        nd_log_err(ND_LOG_OS, "broker: fork: %s", strerror(errno));
        (void)close(sv[0]);
        (void)close(sv[1]);
        free(b);
        return NULL;
    }
    if (pid == 0) {
        (void)close(sv[0]);
        broker_loop(sv[1]);
        _exit(0); /* not reached */
    }

    (void)close(sv[1]);
    b->fd = sv[0];
    b->pid = pid;
    b->ok = true;
    nd_log(ND_LOG_OS, "broker: pid %ld holds the privilege; the UI will not", (long)pid);
    return b;
}

bool nd_broker_ok(const nd_broker *b)
{
    return b != NULL && b->ok && b->fd >= 0;
}

static nd_err round_trip(nd_broker *b, const nd_broker_req *req, const int *fds, size_t n_fds,
                         nd_broker_rep *rep)
{
    ssize_t n;

    if (!nd_broker_ok(b))
        return ND_ERR_IO;
    if (!send_msg(b->fd, req, sizeof *req, fds, n_fds)) {
        nd_log_err(ND_LOG_OS, "broker: send: %s", strerror(errno));
        b->ok = false;
        return ND_ERR_IO;
    }
    n = recv_msg(b->fd, rep, sizeof *rep, NULL, NULL);
    if (n != (ssize_t)sizeof *rep) {
        nd_log_err(ND_LOG_OS, "broker: no answer (%s)", n == 0 ? "it exited" : strerror(errno));
        b->ok = false;
        return ND_ERR_IO;
    }
    return ND_OK;
}

nd_err nd_broker_spawn(nd_broker *b, const char *path, const nd_proc_spec *spec, const char *user,
                       pid_t *pid_out)
{
    nd_broker_req req;
    nd_broker_rep rep;
    int fds[ND_PROC_MAX_FDS];
    size_t i;
    nd_err rc;

    if (b == NULL || path == NULL || spec == NULL || pid_out == NULL)
        return ND_ERR_INVAL;

    memset(&req, 0, sizeof req);
    req.op = REQ_SPAWN;
    if (nd_strlcpy(req.path, path, sizeof req.path) >= sizeof req.path)
        return ND_ERR_TOOLONG;
    if (user != NULL && nd_strlcpy(req.user, user, sizeof req.user) >= sizeof req.user)
        return ND_ERR_TOOLONG;

    for (i = 0u; spec->argv != NULL && spec->argv[i] != NULL; i++) {
        if (!blob_put(req.blob, &req.blob_len, spec->argv[i]))
            return ND_ERR_TOOLONG;
        req.n_argv++;
    }
    for (i = 0u; spec->envp != NULL && spec->envp[i] != NULL; i++) {
        if (!blob_put(req.blob, &req.blob_len, spec->envp[i]))
            return ND_ERR_TOOLONG;
        req.n_envp++;
    }
    for (i = 0u; spec->hide_paths != NULL && spec->hide_paths[i] != NULL; i++) {
        if (!blob_put(req.blob, &req.blob_len, spec->hide_paths[i]))
            return ND_ERR_TOOLONG;
        req.n_hide++;
    }

    req.owner = (uint32_t)spec->owner;
    req.no_new_privs = spec->no_new_privs ? 1u : 0u;
    req.new_session = spec->new_session ? 1u : 0u;
    req.close_others = spec->close_others ? 1u : 0u;
    req.private_mounts = spec->private_mounts ? 1u : 0u;

    if (spec->n_fds > (size_t)ND_PROC_MAX_FDS)
        return ND_ERR_INVAL;
    for (i = 0u; i < spec->n_fds; i++) {
        req.child_fd[i] = (int32_t)spec->fds[i].child_fd;
        fds[i] = spec->fds[i].our_fd;
    }
    req.n_fds = (uint32_t)spec->n_fds;

    memset(&rep, 0, sizeof rep);
    rc = round_trip(b, &req, fds, spec->n_fds, &rep);
    if (rc != ND_OK)
        return rc;
    *pid_out = (pid_t)rep.pid;
    return (nd_err)rep.err;
}

nd_err nd_broker_wait(nd_broker *b, pid_t pid, double timeout_s, nd_proc_status *out)
{
    nd_broker_req req;
    nd_broker_rep rep;
    nd_err rc;

    if (b == NULL || out == NULL)
        return ND_ERR_INVAL;

    memset(&req, 0, sizeof req);
    req.op = REQ_WAIT;
    req.pid = (int64_t)pid;
    req.timeout_s = timeout_s;

    memset(&rep, 0, sizeof rep);
    rc = round_trip(b, &req, NULL, 0u, &rep);
    if (rc != ND_OK)
        return rc;

    memset(out, 0, sizeof *out);
    out->pid = (pid_t)rep.pid;
    out->exited = rep.exited != 0u;
    out->exit_status = rep.exit_status;
    out->signalled = rep.signalled != 0u;
    out->signo = rep.signo;
    return (nd_err)rep.err;
}

static nd_broker *g_default = NULL;

void nd_broker_set_default(nd_broker *b)
{
    g_default = b;
}

nd_broker *nd_broker_default(void)
{
    return g_default;
}

bool nd_broker_halt(nd_broker *b, bool reboot)
{
    nd_broker_req req;
    nd_broker_rep rep;

    memset(&req, 0, sizeof req);
    req.op = REQ_HALT;
    req.reboot = reboot ? 1u : 0u;
    memset(&rep, 0, sizeof rep);
    if (round_trip(b, &req, NULL, 0u, &rep) != ND_OK)
        return false;
    return rep.err == (int32_t)ND_OK;
}

bool nd_broker_set_clock(nd_broker *b, int64_t when)
{
    nd_broker_req req;
    nd_broker_rep rep;

    memset(&req, 0, sizeof req);
    req.op = REQ_CLOCK;
    req.when = when;
    memset(&rep, 0, sizeof rep);
    if (round_trip(b, &req, NULL, 0u, &rep) != ND_OK)
        return false;
    return rep.err == (int32_t)ND_OK;
}

void nd_broker_stop(nd_broker *b)
{
    if (b == NULL)
        return;
    if (b->fd >= 0)
        (void)close(b->fd); /* EOF is how the broker is told to go */
    if (b->pid > 0) {
        nd_proc_status st;

        (void)nd_proc_wait(b->pid, 2.0, &st);
    }
    free(b);
}
