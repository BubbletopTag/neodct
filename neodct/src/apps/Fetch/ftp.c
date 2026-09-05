/* ftp.c -- the transport: two curl invocations, and the parsing of what one
 * of them prints.
 *
 * fetch_app.h holds the reasoning about WHY this spawns curl and why it does
 * not reuse nd_remote. What is here is how.
 *
 * ============ THE TWO INVOCATIONS ============
 *
 * A LISTING is `curl ftp://host/dir/` with a trailing slash, which makes curl
 * send LIST and print its reply on stdout. The reply is ls -l text, because
 * that is what every unix FTP server emits and MLSD is not universal; it is
 * read into a bounded heap buffer and parsed line by line. Nothing about that
 * text is trusted: see fetch_parse_list_line().
 *
 * A DOWNLOAD is `curl -o <path>.part ftp://host/dir/name`. curl writes the
 * file itself rather than piping it through this process, which removes the
 * whole read-and-append loop nd_remote needs -- and progress then comes from
 * stat()ing the partial file five times a second, against the size the
 * listing already gave. That is a cruder progress bar than counting bytes as
 * they arrive and it is exactly as accurate, because it is the same bytes.
 *
 * ============ WHY .part AND A RENAME ============
 *
 * The destination directories are read by other things: MusicPlayer scans
 * music/, Settings scans untrusted/ for .nap files, PSX scans its games/. A
 * half-downloaded file with its final name in any of those is a file one of
 * them will try to open. So curl writes a sibling ending in ".part" and the
 * file only takes its real name once curl has exited 0 and the length looks
 * right; a failed or cancelled transfer leaves a .part behind, which nothing
 * scans and the next attempt overwrites.
 *
 * ============ CANCELLING ============
 *
 * The progress callback returns false when the app should exit -- an incoming
 * call, mostly. curl is then SIGTERMed with a one-second grace through
 * nd_proc_terminate(), which is the same sequence the core uses on an app,
 * and the .part is left where it is. A download that outlived its own screen
 * would hold a bearer nobody is watching, which is the failure nd_remote's
 * header describes and this avoids the same way: new_session stays false, so
 * curl is in this app's process group and dies with it regardless.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fetch_app.h"
#include "nd_app.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"

/* How often the download loop wakes to move the bar and ask whether it should
 * still be running. Five times a second is under the eye's threshold for a
 * bar that is moving and cheap enough that stat() does not show up. */
#define FETCH_POLL_MS 200

/* curl's exit codes, the three this app can say something useful about. */
#define CURL_E_COULDNT_RESOLVE 6
#define CURL_E_COULDNT_CONNECT 7
#define CURL_E_LOGIN_DENIED    67

static void say_why(char *why, size_t why_sz, const char *fmt, ...) ND_PRINTF(3, 4);
static void say_why(char *why, size_t why_sz, const char *fmt, ...)
{
    va_list ap;

    if (why == NULL || why_sz == 0u)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(why, why_sz, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ *
 * Finding curl
 * ------------------------------------------------------------------ */

/* The same PATH walk nd_remote does, and for the same reason: it is ALSO the
 * test seam. A stand-in `curl` earlier on PATH is what test_fetch.c uses, so
 * the real argv, the real pipes and the real parsing are exercised. */
static bool which_curl(char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    const char *seg;

    if (path == NULL || path[0] == '\0')
        return false;
    for (seg = path; seg != NULL; ) {
        const char *end = strchr(seg, ':');
        size_t len = (end != NULL) ? (size_t)(end - seg) : strlen(seg);
        char cand[ND_PATH_MAX];
        nd_err rc;

        if (len == 0u)
            rc = nd_snprintf(cand, sizeof cand, "./curl");
        else
            rc = nd_snprintf(cand, sizeof cand, "%.*s/curl", (int)len, seg);
        if (rc == ND_OK && access(cand, X_OK) == 0)
            return nd_strlcpy(out, cand, out_sz) < out_sz;
        seg = (end != NULL) ? end + 1 : NULL;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * The credentials, off the command line
 * ------------------------------------------------------------------ */

/* Write a netrc for this one transfer and hand back its path.
 *
 * /tmp is tmpfs, so this never touches the card and never survives a reboot;
 * 0600 and O_EXCL mean nothing else on the phone can read it or be made to
 * hand us somebody else's file. It is unlinked as soon as curl has exited,
 * which is a window of one transfer rather than of one session. */
static nd_err write_netrc(const fetch_conn *c, char *out, size_t out_sz)
{
    int fd;
    FILE *f;

    if (nd_snprintf(out, out_sz, "/tmp/neodct-fetch-%ld.netrc", (long)getpid()) != ND_OK)
        return ND_ERR_TOOLONG;
    (void)unlink(out);
    fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return ND_ERR_IO;
    f = fdopen(fd, "w");
    if (f == NULL) {
        (void)close(fd);
        (void)unlink(out);
        return ND_ERR_IO;
    }
    /* No quoting is needed and none is possible: netrc is whitespace
     * separated, so a password with a space in it cannot be expressed. The
     * password field is built by the owner on a phone keypad and the login
     * screen refuses a space for exactly this reason. */
    if (fprintf(f, "machine %s login %s password %s\n", c->host, c->user, c->pass) < 0) {
        (void)fclose(f);
        (void)unlink(out);
        return ND_ERR_IO;
    }
    if (fclose(f) != 0) {
        (void)unlink(out);
        return ND_ERR_IO;
    }
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * URLs
 * ------------------------------------------------------------------ */

/* ============ ESCAPING, AND WHY REFUSING WAS THE WRONG CALL ============
 *
 * This used to refuse any path that would need escaping, on the reasoning
 * that everything here was built by the app from names it had already vetted.
 * That reasoning was wrong about one character: a SPACE. File names have
 * spaces in them -- "A Forest.mp3", "Crash Bandicoot.bin" -- and there is
 * nothing unsafe about one; it is simply not legal in a URL. The result was
 * an app that listed a music folder perfectly and then answered "URL
 * rejected: Error" for every track in it.
 *
 * So the rule is now the correct one: anything outside the unreserved set of
 * RFC 3986 is percent-encoded, which is what a URL is for. Refusal is kept
 * for the one thing escaping cannot make safe -- a ".." component, which is a
 * traversal whether it is encoded or not, and which no listing this app
 * accepts can produce anyway.
 */
static bool is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

/* Percent-encode `in` into `out`. '/' is kept when `keep_slash`, which is how
 * a multi-segment directory ("music/live") escapes each segment without
 * losing the separators between them. */
static nd_err url_escape(const char *in, char *out, size_t out_sz, bool keep_slash)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = 0u;
    size_t i;

    for (i = 0u; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];

        if (is_unreserved(c) || (keep_slash && c == '/')) {
            if (n + 1u >= out_sz)
                return ND_ERR_TOOLONG;
            out[n++] = (char)c;
        } else {
            /* Three bytes, and the NUL still has to fit after them. */
            if (n + 4u > out_sz)
                return ND_ERR_TOOLONG;
            out[n++] = '%';
            out[n++] = HEX[(c >> 4) & 0x0fu];
            out[n++] = HEX[c & 0x0fu];
        }
    }
    if (n >= out_sz)
        return ND_ERR_TOOLONG;
    out[n] = '\0';
    return ND_OK;
}

/* The remote directory the app is showing, as "music" or "roms/psx". Only
 * two things are refused, because escaping handles everything else: an
 * absolute path, and a ".." component. Control bytes cannot get here -- every
 * segment came from fetch_parse_list_line() -- but they are refused anyway,
 * since a byte that cannot appear is a byte worth refusing cheaply. */
static bool dir_is_safe(const char *dir)
{
    size_t i;

    if (dir == NULL)
        return false;
    if (dir[0] == '/')
        return false;
    for (i = 0u; dir[i] != '\0'; i++) {
        unsigned char c = (unsigned char)dir[i];

        if (c < 0x20u || c == 0x7fu)
            return false;
    }
    /* ".." at the start, as a whole component, or at the end. */
    if (strcmp(dir, "..") == 0 || strncmp(dir, "../", 3u) == 0 || strstr(dir, "/../") != NULL)
        return false;
    {
        size_t len = strlen(dir);

        if (len >= 3u && strcmp(dir + len - 3u, "/..") == 0)
            return false;
    }
    return true;
}

/* A host is not escaped -- it is compared against the netrc's `machine` line
 * and has to match byte for byte -- so it is checked instead. An IPv4 or IPv6
 * literal or a host name, and nothing that could end a URL authority early. */
static bool host_is_safe(const char *host)
{
    size_t i;

    if (host == NULL || host[0] == '\0')
        return false;
    for (i = 0u; host[i] != '\0'; i++) {
        unsigned char c = (unsigned char)host[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == ':' || c == '[' || c == ']'))
            return false;
    }
    return true;
}

nd_err fetch_build_url(const char *host, const char *dir, const char *name, char *out,
                       size_t out_sz)
{
    char dir_esc[ND_FETCH_URL_MAX];
    char name_esc[ND_FETCH_NAME_MAX * 3u + 1u];

    if (host == NULL || dir == NULL || out == NULL)
        return ND_ERR_INVAL;
    if (!host_is_safe(host) || !dir_is_safe(dir))
        return ND_ERR_INVAL;
    if (name != NULL && !fetch_name_is_safe(name))
        return ND_ERR_INVAL;
    if (url_escape(dir, dir_esc, sizeof dir_esc, true) != ND_OK)
        return ND_ERR_TOOLONG;
    if (name != NULL && url_escape(name, name_esc, sizeof name_esc, false) != ND_OK)
        return ND_ERR_TOOLONG;

    if (name == NULL) {
        /* The trailing slash is what makes curl LIST rather than RETR. */
        if (dir_esc[0] == '\0')
            return nd_snprintf(out, out_sz, "ftp://%s/", host);
        return nd_snprintf(out, out_sz, "ftp://%s/%s/", host, dir_esc);
    }
    if (dir_esc[0] == '\0')
        return nd_snprintf(out, out_sz, "ftp://%s/%s", host, name_esc);
    return nd_snprintf(out, out_sz, "ftp://%s/%s/%s", host, dir_esc, name_esc);
}

/* ------------------------------------------------------------------ *
 * Listing text
 * ------------------------------------------------------------------ */

/* Advance past spaces and tabs, then to the end of the next field. Returns
 * the start of the field and writes its length; NULL at the end of the
 * line. */
static const char *next_field(const char *p, size_t *len_out)
{
    const char *start;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return NULL;
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        p++;
    *len_out = (size_t)(p - start);
    return start;
}

bool fetch_parse_list_line(const char *line, fetch_entry *out)
{
    const char *p = line;
    size_t len = 0u;
    size_t field;
    int64_t size = -1;
    char mode0;

    if (line == NULL || out == NULL)
        return false;
    memset(out, 0, sizeof *out);
    out->size = -1;

    p = next_field(p, &len);
    if (p == NULL || len < 10u)
        return false;
    mode0 = p[0];
    /* Only plain files and directories. A symlink is how a listing points at
     * something outside the folder the owner is looking at, and a device node
     * has no business on a file server. */
    if (mode0 != '-' && mode0 != 'd')
        return false;
    out->is_dir = (mode0 == 'd');
    p += len;

    /* links, owner, group, size -- fields two to five. */
    for (field = 2u; field <= 5u; field++) {
        p = next_field(p, &len);
        if (p == NULL)
            return false;
        if (field == 5u) {
            char buf[24];
            char *endp = NULL;
            long long v;

            if (len >= sizeof buf)
                return false;
            memcpy(buf, p, len);
            buf[len] = '\0';
            errno = 0;
            v = strtoll(buf, &endp, 10);
            /* -1 rather than 0 for a size that will not parse: 0 is a real
             * file length and the progress bar has to tell them apart. */
            size = (errno == 0 && endp != NULL && *endp == '\0' && v >= 0) ? (int64_t)v : -1;
        }
        p += len;
    }

    /* month, day, time-or-year -- fields six to eight. */
    for (field = 6u; field <= 8u; field++) {
        p = next_field(p, &len);
        if (p == NULL)
            return false;
        p += len;
    }

    /* Everything left, minus the leading space, is the name -- so a file with
     * spaces in it survives. */
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return false;
    if (nd_strlcpy(out->name, p, sizeof out->name) >= sizeof out->name)
        return false;
    if (!fetch_name_is_safe(out->name))
        return false;

    out->size = out->is_dir ? -1 : size;
    return true;
}

/* Directories first, then names ascending. strcasecmp so that "Zelda" and
 * "abba" sort the way a person expects rather than the way ASCII does. */
static int entry_cmp(const void *a, const void *b)
{
    const fetch_entry *x = (const fetch_entry *)a;
    const fetch_entry *y = (const fetch_entry *)b;

    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* One pass over the text, taking only directories or only files, appending
 * from `n`. Returns the new count. */
static size_t scan_listing(const char *text, fetch_entry *out, size_t max, bool want_dirs,
                           size_t n)
{
    const char *p = text;

    while (*p != '\0' && n < max) {
        char line[ND_FETCH_LINE_MAX];
        fetch_entry e;
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

        if (len > 0u && p[len - 1u] == '\r')
            len--;
        if (len > 0u && len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';
            if (fetch_parse_list_line(line, &e) && e.is_dir == want_dirs)
                out[n++] = e;
        }
        if (nl == NULL)
            break;
        p = nl + 1;
    }
    return n;
}

/* ============ WHY THIS IS TWO PASSES AND NOT ONE ============
 *
 * The array is bounded and a folder is not, so a big enough folder is
 * truncated -- and truncation must never cost a DIRECTORY. LIST comes back in
 * whatever order the server's readdir gave, so a one-pass fill would drop
 * whichever entries happened to come last, and a subfolder sitting at the end
 * of a directory of nine hundred tracks would become unreachable: not merely
 * missing from a screen, but with no way to navigate into it at all.
 *
 * Taking directories first means the only thing a full array can cost is
 * files, which the owner can still get at by making another folder on the
 * server. Sorting afterwards is unaffected -- entry_cmp puts directories
 * first anyway, so the two passes and the sort agree.
 */
size_t fetch_parse_listing(const char *text, fetch_entry *out, size_t max)
{
    size_t n;

    if (text == NULL || out == NULL || max == 0u)
        return 0u;
    n = scan_listing(text, out, max, true, 0u);
    n = scan_listing(text, out, max, false, n);
    if (n > 1u)
        qsort(out, n, sizeof out[0], entry_cmp);
    return n;
}

/* ------------------------------------------------------------------ *
 * Running curl
 * ------------------------------------------------------------------ */

/* The options both invocations share, appended to argv. Returns the new
 * argument count. `argv` has room; the callers size it. */
static size_t common_args(const char **argv, size_t argn, const char *netrc,
                          const char *connect_s, const char *stall_s)
{
    argv[argn++] = "-s"; /* no progress meter -- there is one on the panel */
    argv[argn++] = "-S"; /* but do say what went wrong                     */
    /* FTP and nothing else. curl will not be talked into file:// or scp://
     * by a server, and there are no redirects in FTP to follow anyway. */
    argv[argn++] = "--proto";
    argv[argn++] = "=ftp";
    /* Explicit AUTH TLS, REQUIRED: a server that will not negotiate it is
     * refused rather than spoken to in the clear. */
    argv[argn++] = "--ssl-reqd";
    argv[argn++] = "--tlsv1.2";
    /* And the certificate is not checked, because it is self-signed on an IP
     * address. fetch_app.h says what that is and is not worth. */
    argv[argn++] = "-k";
    argv[argn++] = "--netrc-file";
    argv[argn++] = netrc;
    argv[argn++] = "--connect-timeout";
    argv[argn++] = connect_s;
    /* NOT --max-time: a total deadline would abort a slow transfer that was
     * making perfectly good progress. A stall is the thing worth giving up
     * on, so it is a speed floor over a window instead. */
    argv[argn++] = "--speed-limit";
    argv[argn++] = "1";
    argv[argn++] = "--speed-time";
    argv[argn++] = stall_s;
    return argn;
}

/* curl's complaint with its own prefix removed, so the screen says "Access
 * denied" rather than "curl: (67) Access denied". */
static const char *curl_reason(const char *stderr_text)
{
    const char *p = stderr_text;

    while (*p == ' ' || *p == '\n' || *p == '\r')
        p++;
    if (strncmp(p, "curl: ", 6u) == 0) {
        const char *close_paren = strchr(p, ')');

        if (close_paren != NULL) {
            p = close_paren + 1;
            while (*p == ' ')
                p++;
        }
    }
    return p;
}

/* Turn an exit code and whatever curl said into the one line a person reads.
 * The three codes named here are the ones with a specific fix; everything
 * else gets curl's own words, which are better than a table would be. */
static void explain(int exit_status, const char *err_text, const char *user, char *why,
                    size_t why_sz)
{
    const char *reason = curl_reason(err_text);

    switch (exit_status) {
    case CURL_E_LOGIN_DENIED:
        /* Curl 67 is "the server said no to this login", and it does not say
         * which half was wrong. Naming the USER is what makes that useful:
         * the password is the half the owner just typed and is thinking
         * about, and the user name is the half that comes from a default
         * nobody looks at -- which is exactly how a server account renamed
         * from neodct to ndftp cost an evening of retyping a correct
         * password. */
        say_why(why, why_sz, "Login refused as \"%s\".\nCheck the name and password.", user);
        return;
    case CURL_E_COULDNT_RESOLVE:
        say_why(why, why_sz, "No network: cannot look up the server.");
        return;
    case CURL_E_COULDNT_CONNECT:
        say_why(why, why_sz, "Cannot reach the server.");
        return;
    default:
        break;
    }
    if (reason[0] != '\0') {
        /* curl's stderr ends in a newline. Left on, it becomes a blank line
         * inside the dialog and, worse, pushes a two-line message over the
         * threshold where MessageDialog switches to its paragraph layout --
         * so the trailing whitespace silently changes how the screen looks. */
        char trimmed[ND_FETCH_WHY_MAX];
        size_t len;

        (void)nd_strlcpy(trimmed, reason, sizeof trimmed);
        len = strlen(trimmed);
        while (len > 0u && (trimmed[len - 1u] == '\n' || trimmed[len - 1u] == '\r' ||
                            trimmed[len - 1u] == ' ' || trimmed[len - 1u] == '\t'))
            trimmed[--len] = '\0';
        say_why(why, why_sz, "%s", trimmed);
    } else {
        say_why(why, why_sz, "Transfer failed (curl %d).", exit_status);
    }
}

/* Read a whole descriptor into a bounded, NUL-terminated heap buffer. Keeps
 * reading past `cap` and throws the excess away, so the child is never
 * blocked writing to a pipe nobody is draining. */
static char *slurp(int fd, size_t cap)
{
    char *buf = malloc(cap + 1u);
    size_t used = 0u;

    if (buf == NULL)
        return NULL;
    for (;;) {
        char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof chunk);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        if (used < cap) {
            size_t room = cap - used;
            size_t take = ((size_t)got < room) ? (size_t)got : room;

            memcpy(buf + used, chunk, take);
            used += take;
        }
    }
    buf[used] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ *
 * Listing
 * ------------------------------------------------------------------ */

nd_err fetch_list(const fetch_conn *c, const char *dir, fetch_entry *out, size_t max, size_t *n_out,
                  char *why, size_t why_sz)
{
    const char *argv[24];
    size_t argn = 0u;
    char exe[ND_PATH_MAX];
    char url[ND_FETCH_URL_MAX];
    char netrc[ND_PATH_MAX];
    char connect_s[16];
    char stall_s[16];
    int out_fd[2] = {-1, -1};
    int err_fd[2] = {-1, -1};
    nd_proc_spec spec;
    nd_proc_status status;
    pid_t pid = -1;
    char *body = NULL;
    char *err_text = NULL;
    nd_err rc = ND_ERR_IO;

    if (c == NULL || dir == NULL || out == NULL || n_out == NULL)
        return ND_ERR_INVAL;
    *n_out = 0u;

    if (!which_curl(exe, sizeof exe)) {
        say_why(why, why_sz, "No curl on this phone.");
        return ND_ERR_NOTFOUND;
    }
    if (fetch_build_url(c->host, dir, NULL, url, sizeof url) != ND_OK) {
        say_why(why, why_sz, "That folder name cannot be asked for.");
        return ND_ERR_INVAL;
    }
    if (write_netrc(c, netrc, sizeof netrc) != ND_OK) {
        say_why(why, why_sz, "Cannot write the login file.");
        return ND_ERR_IO;
    }
    (void)nd_snprintf(connect_s, sizeof connect_s, "%d", ND_FETCH_CONNECT_TIMEOUT);
    (void)nd_snprintf(stall_s, sizeof stall_s, "%d", ND_FETCH_STALL_TIMEOUT);

    /* O_CLOEXEC on our ends: nd_proc_spawn dup2()s only what it is given and
     * closes nothing else, so without this the child holds the read ends open
     * and the pipes never reach EOF. */
    if (pipe2(out_fd, O_CLOEXEC) != 0 || pipe2(err_fd, O_CLOEXEC) != 0) {
        say_why(why, why_sz, "%s", strerror(errno));
        goto done;
    }

    argv[argn++] = "curl";
    argn = common_args(argv, argn, netrc, connect_s, stall_s);
    argv[argn++] = url;
    argv[argn++] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = out_fd[1];
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = err_fd[1];
    spec.n_fds = 2u;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        say_why(why, why_sz, "Cannot start curl.");
        goto done;
    }
    (void)close(out_fd[1]);
    out_fd[1] = -1;
    (void)close(err_fd[1]);
    err_fd[1] = -1;

    body = slurp(out_fd[0], ND_FETCH_LIST_MAX);
    err_text = slurp(err_fd[0], ND_FETCH_WHY_MAX * 2u);
    if (body == NULL || err_text == NULL) {
        say_why(why, why_sz, "Out of memory.");
        rc = ND_ERR_NOMEM;
        (void)nd_proc_terminate(pid, 1.0, &status);
        pid = -1;
        goto done;
    }

    /* Both pipes are at EOF, so curl has closed them or is gone; a bounded
     * wait here can only be a curl that has stopped without exiting. */
    if (nd_proc_wait(pid, 10.0, &status) != ND_OK) {
        (void)nd_proc_terminate(pid, 1.0, &status);
        say_why(why, why_sz, "The server stopped answering.");
        pid = -1;
        rc = ND_ERR_TIMEOUT;
        goto done;
    }
    pid = -1;
    if (!status.exited || status.exit_status != 0) {
        explain(status.exited ? status.exit_status : -1, err_text, c->user, why, why_sz);
        rc = (status.exited && status.exit_status == CURL_E_LOGIN_DENIED) ? ND_ERR_PERM
                                                                         : ND_ERR_IO;
        goto done;
    }

    *n_out = fetch_parse_listing(body, out, max);
    rc = ND_OK;

done:
    if (pid > 0)
        (void)nd_proc_terminate(pid, 1.0, &status);
    if (out_fd[0] >= 0)
        (void)close(out_fd[0]);
    if (out_fd[1] >= 0)
        (void)close(out_fd[1]);
    if (err_fd[0] >= 0)
        (void)close(err_fd[0]);
    if (err_fd[1] >= 0)
        (void)close(err_fd[1]);
    free(body);
    free(err_text);
    (void)unlink(netrc);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Downloading
 * ------------------------------------------------------------------ */

static int64_t file_size(const char *path)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return -1;
    if (stat(resolved, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
}

nd_err fetch_download(const fetch_conn *c, const char *dir, const char *name,
                      const char *local_path, int64_t total, fetch_progress_fn on_progress,
                      void *ctx, char *why, size_t why_sz)
{
    const char *argv[26];
    size_t argn = 0u;
    char exe[ND_PATH_MAX];
    char url[ND_FETCH_URL_MAX];
    char netrc[ND_PATH_MAX];
    char part[ND_PATH_MAX];
    char part_resolved[ND_PATH_MAX];
    char final_resolved[ND_PATH_MAX];
    char connect_s[16];
    char stall_s[16];
    int err_fd[2] = {-1, -1};
    nd_proc_spec spec;
    nd_proc_status status;
    pid_t pid = -1;
    char *err_text = NULL;
    nd_err rc = ND_ERR_IO;
    bool cancelled = false;

    if (c == NULL || dir == NULL || name == NULL || local_path == NULL)
        return ND_ERR_INVAL;

    if (!which_curl(exe, sizeof exe)) {
        say_why(why, why_sz, "No curl on this phone.");
        return ND_ERR_NOTFOUND;
    }
    if (fetch_build_url(c->host, dir, name, url, sizeof url) != ND_OK) {
        say_why(why, why_sz, "That file cannot be asked for.");
        return ND_ERR_INVAL;
    }
    if (nd_snprintf(part, sizeof part, "%s.part", local_path) != ND_OK ||
        nd_path_resolve(part_resolved, sizeof part_resolved, part) != ND_OK ||
        nd_path_resolve(final_resolved, sizeof final_resolved, local_path) != ND_OK) {
        say_why(why, why_sz, "That path is too long for the card.");
        return ND_ERR_TOOLONG;
    }
    if (fetch_prepare_dir(local_path) != ND_OK) {
        say_why(why, why_sz, "Cannot make the folder on the card.");
        return ND_ERR_IO;
    }
    /* Whatever a previous attempt left. curl would truncate it anyway; doing
     * it here means the progress bar starts at zero rather than at whatever
     * the last attempt reached. */
    (void)unlink(part_resolved);

    if (write_netrc(c, netrc, sizeof netrc) != ND_OK) {
        say_why(why, why_sz, "Cannot write the login file.");
        return ND_ERR_IO;
    }
    (void)nd_snprintf(connect_s, sizeof connect_s, "%d", ND_FETCH_CONNECT_TIMEOUT);
    (void)nd_snprintf(stall_s, sizeof stall_s, "%d", ND_FETCH_STALL_TIMEOUT);

    if (pipe2(err_fd, O_CLOEXEC) != 0) {
        say_why(why, why_sz, "%s", strerror(errno));
        goto done;
    }

    argv[argn++] = "curl";
    argn = common_args(argv, argn, netrc, connect_s, stall_s);
    argv[argn++] = "-o";
    argv[argn++] = part_resolved;
    argv[argn++] = url;
    argv[argn++] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    spec.fds[0].child_fd = 2;
    spec.fds[0].our_fd = err_fd[1];
    spec.n_fds = 1u;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        say_why(why, why_sz, "Cannot start curl.");
        goto done;
    }
    (void)close(err_fd[1]);
    err_fd[1] = -1;

    /* The progress loop. curl owns the file; this only watches it grow. */
    for (;;) {
        nd_err w = nd_proc_wait(pid, (double)FETCH_POLL_MS / 1000.0, &status);

        if (w == ND_OK) {
            pid = -1;
            break;
        }
        if (w != ND_ERR_TIMEOUT) {
            say_why(why, why_sz, "Lost track of the download.");
            goto done;
        }
        if (on_progress != NULL && !on_progress(ctx, file_size(part), total)) {
            cancelled = true;
            (void)nd_proc_terminate(pid, 1.0, &status);
            pid = -1;
            break;
        }
    }

    err_text = slurp(err_fd[0], ND_FETCH_WHY_MAX * 2u);
    if (cancelled) {
        say_why(why, why_sz, "Cancelled.");
        rc = ND_ERR_BUSY;
        goto done;
    }
    if (!status.exited || status.exit_status != 0) {
        explain(status.exited ? status.exit_status : -1, (err_text != NULL) ? err_text : "",
                c->user, why, why_sz);
        rc = (status.exited && status.exit_status == CURL_E_LOGIN_DENIED) ? ND_ERR_PERM
                                                                         : ND_ERR_IO;
        goto done;
    }

    /* curl exited 0 with a file shorter than the listing said. That is a
     * server that changed its mind mid-transfer, and renaming it into music/
     * would produce a track that plays for four seconds. */
    if (total > 0 && file_size(part) != total) {
        say_why(why, why_sz, "The file arrived short.");
        rc = ND_ERR_IO;
        goto done;
    }
    if (rename(part_resolved, final_resolved) != 0) {
        say_why(why, why_sz, "Cannot save it: %s", strerror(errno));
        rc = ND_ERR_IO;
        goto done;
    }
    nd_log(ND_LOG_FETCH, "downloaded %s -> %s", name, local_path);
    rc = ND_OK;

done:
    if (pid > 0)
        (void)nd_proc_terminate(pid, 1.0, &status);
    if (err_fd[0] >= 0)
        (void)close(err_fd[0]);
    if (err_fd[1] >= 0)
        (void)close(err_fd[1]);
    free(err_text);
    (void)unlink(netrc);
    return rc;
}
