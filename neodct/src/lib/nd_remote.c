/* nd_remote.c -- finding and fetching an update over the network.
 *
 * A one-to-one port of System/core/UpdateService/remote.py. Read nd_remote.h
 * first: it carries the contract, the constants and the reasoning. This file
 * carries the two things that reasoning does not fit into -- how curl is
 * driven, and how the 206-versus-200 question is answered.
 *
 * ============ HOW curl IS DRIVEN ============
 *
 *     curl -s -S -L --max-redirs 10
 *          --proto =https --proto-redir =https --tlsv1.2
 *          --connect-timeout 20 --speed-limit 1 --speed-time <20|120>
 *          -D /proc/self/fd/3
 *          -H "User-Agent: ..." -H "Accept: application/vnd.github+json"
 *          [-H "Range: bytes=<have>-"]
 *          <url>
 *
 * Three descriptors come back, and the split between them is the whole
 * design:
 *
 *     fd 1  the response BODY and nothing else
 *     fd 3  the dumped response HEADERS, every block of a redirect chain
 *     fd 2  curl's own complaint, which becomes the reason a person reads
 *
 * The headers go to a descriptor of their own rather than to stdout, so
 * there is never a question of where the header block ends and the body
 * begins. With `-D -` that boundary is a guess, and it is a guess about
 * attacker-influenced bytes: a body whose first line happened to read
 * "HTTP/1.1 206 Partial Content" would be swallowed as a header block and
 * would change the answer to the one question this module must not get
 * wrong. On a separate descriptor it cannot.
 *
 * /proc/self/fd/3 rather than /dev/fd/3 because /proc is mounted on this
 * phone and is already relied on elsewhere (nd_proc.c reads /proc/self/exe,
 * the Update app reads /proc/net/route), while /dev/fd is a symlink some
 * images have and some do not.
 *
 * The timeouts are the Python's, mapped correctly. urlopen(timeout=N) is a
 * SOCKET timeout -- it applies to the connect and then to each individual
 * recv, and is not a deadline for the whole transfer. curl's --max-time IS
 * such a deadline and would abort a slow 53 MB download at 120 seconds, so
 * it is deliberately NOT set; --speed-limit 1 --speed-time N is the
 * equivalent of "nothing has arrived for N seconds".
 *
 * --proto/--proto-redir are a hardening the Python does not have: urllib
 * would follow an https->http redirect. Refusing to be downgraded costs
 * nothing here, because the API host is always api.github.com and the asset
 * URL always comes back https.
 *
 * There is no --insecure and there is no way to ask for one. An unverified
 * fetch would make the signature check the only thing standing between the
 * phone and a hostile package, and one line of defence is not enough for
 * something that replaces the rootfs.
 *
 * ============ HOW "DID THE SERVER HONOUR THE RANGE" IS ANSWERED ============
 *
 * By reading the status line off fd 3 and comparing it to 206, which is
 * exactly what remote.py compares response.status to.
 *
 * It matters because a server that ignores a Range header answers 200 WITH
 * THE WHOLE FILE. Appending that to a partial download produces a package
 * whose first N bytes are written twice -- and that package then fails its
 * sha256 much later, with nothing anywhere to say why. So the status decides
 * whether the partial file is appended to or truncated first, and it is read
 * rather than inferred.
 *
 * This is why curl's own `-C -` is NOT used. `-C -` resumes inside curl and
 * hands back no status, so "did the server honour it" would have to be
 * reconstructed afterwards from the length of the file -- which is only
 * possible when the expected size is known, is wrong when the server sends a
 * short 200, and depends on a curl-version-specific decision about whether a
 * 200 answer to a resumed request is an error at all. The Range header is
 * therefore sent explicitly, the body is appended by this file, and the
 * status is read off the wire.
 *
 * ============ WHAT THIS MODULE DOES NOT DECIDE ============
 *
 * Whether the file is trustworthy. It stops at "there is a file on the
 * card"; nd_signing.c decides the rest.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "nd_json.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_remote.h"
#include "nd_types.h"
#include "nd_update.h"

/* The descriptor curl dumps response headers onto, and the path it opens to
 * get there. They are one number in two spellings and must agree. */
#define CURL_HEADER_FD   3
#define CURL_HEADER_PATH "/proc/self/fd/3"

/* Longest header line kept for parsing. Only the status line is read and it
 * is short; a longer line is truncated for parsing and otherwise ignored. */
#define HEADER_LINE_MAX 512

/* How much of curl's own stderr is kept as the reason a person reads. */
#define CURL_ERR_MAX 256

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

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

/* Python's str.strip() strips these six and nothing else that matters here.
 * isspace() from <ctype.h> is locale-dependent and takes an int; this does
 * not need either. */
static bool ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool ascii_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool ascii_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* nd_strlcpy from a run of bytes that is not NUL-terminated. */
static void copy_bounded(char *dst, size_t dst_sz, const char *src, size_t len)
{
    if (dst_sz == 0u)
        return;
    if (len >= dst_sz)
        len = dst_sz - 1u;
    if (len > 0u)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ *
 * The one seam -- remote._sleep
 * ------------------------------------------------------------------ */

static nd_remote_sleep_fn g_sleep_fn;
static void *g_sleep_ctx;

void nd_remote_set_sleep_fn(nd_remote_sleep_fn fn, void *ctx)
{
    g_sleep_fn = fn;
    g_sleep_ctx = ctx;
}

static void back_off(uint32_t seconds)
{
    struct timespec ts;

    if (g_sleep_fn != NULL) {
        g_sleep_fn(g_sleep_ctx, seconds);
        return;
    }
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* ------------------------------------------------------------------ *
 * repo() and asset_name()
 * ------------------------------------------------------------------ */

const char *nd_remote_repo(void)
{
    const char *env = getenv(ND_REMOTE_REPO_ENV);

    /* `os.environ.get(REPO_ENV) or DEFAULT_REPO` -- an empty override is
     * falsy in Python and must fall back here too. */
    return (env != NULL && env[0] != '\0') ? env : ND_REMOTE_DEFAULT_REPO;
}

/* The repo goes into a URL that becomes one argv entry of a spawned program.
 * No shell is involved, so there is nothing to quote, but a name carrying a
 * space, a control character or a leading '-' would either build a URL that
 * means something else or be read by curl as an option. An operator who
 * mistypes the override should see a message, not a different repository. */
static bool repo_is_usable(const char *repo)
{
    size_t i;

    if (repo == NULL || repo[0] == '\0' || repo[0] == '-')
        return false;
    for (i = 0u; repo[i] != '\0'; i++) {
        char c = repo[i];

        if (!ascii_alpha(c) && !ascii_digit(c) && c != '.' && c != '_' && c != '-' && c != '/')
            return false;
    }
    return i < 128u;
}

nd_err nd_remote_asset_name(const char *platform, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    out[0] = '\0';
    /* remote.asset_name() is "UPDATE-%s.ndsw" % platform with no validation
     * at all; a None platform is a TypeError there and an empty name here,
     * which then matches no asset and reads as "nothing published". */
    return nd_snprintf(out, out_sz, "UPDATE-%s.ndsw", platform != NULL ? platform : "");
}

/* ------------------------------------------------------------------ *
 * version_key -- remote.version_key()
 * ------------------------------------------------------------------ */

static void verkey_add(nd_verkey *k, const char *chunk, size_t len)
{
    size_t digits = 0u;
    size_t tail;
    int64_t num = 0;
    bool saturated = false;

    /* re.split leaves an empty piece for "1..2" and for a leading or
     * trailing separator. Python's `elif chunk:` drops them. */
    if (len == 0u)
        return;
    /* DIVERGENCE: Python keeps every component. Past the eighth this drops
     * them, which can only make two versions compare EQUAL that Python would
     * have ordered -- and is_newer() answers false on equal, so the effect is
     * "not offered" rather than "wrongly offered". No published version has
     * more than three components. */
    if (k->n >= ND_VERKEY_MAX_PARTS)
        return;

    while (digits < len && ascii_digit(chunk[digits])) {
        int64_t d = (int64_t)(chunk[digits] - '0');

        /* DIVERGENCE: Python has arbitrary-precision integers. This
         * saturates. A version number with nineteen digits is not a version
         * number, and saturating sorts it at the top of its component --
         * which is why the check below refuses to let a saturated value
         * decide anything it should not: it is recorded honestly and the
         * comparison is left to do its job. */
        if (!saturated) {
            if (num > (INT64_MAX - d) / 10) {
                saturated = true;
                num = INT64_MAX;
            } else {
                num = num * 10 + d;
            }
        }
        digits++;
    }

    if (digits > 0u) {
        tail = digits;
        while (tail < len && ascii_alpha(chunk[tail]))
            tail++;
        if (tail == len) {
            /* ^(\d+)([a-zA-Z]*)$ matched. */
            k->p[k->n].num = num;
            copy_bounded(k->p[k->n].suf, sizeof k->p[k->n].suf, chunk + digits, len - digits);
            k->n++;
            return;
        }
    }

    /* Non-empty and not <digits><letters>: Python emits (-1, chunk), which
     * sorts below every numeric component. */
    k->p[k->n].num = -1;
    copy_bounded(k->p[k->n].suf, sizeof k->p[k->n].suf, chunk, len);
    k->n++;
}

void nd_verkey_parse(const char *text, nd_verkey *out)
{
    const char *s;
    size_t len;
    size_t i;
    size_t start;

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);

    s = (text != NULL) ? text : "";
    while (*s != '\0' && ascii_space(*s))
        s++;
    len = strlen(s);
    while (len > 0u && ascii_space(s[len - 1u]))
        len--;

    start = 0u;
    for (i = 0u;; i++) {
        if (i == len || s[i] == '.' || s[i] == '-' || s[i] == '_') {
            verkey_add(out, s + start, i - start);
            start = i + 1u;
            if (i == len)
                break;
        }
    }
}

int nd_verkey_cmp(const nd_verkey *a, const nd_verkey *b)
{
    size_t i;
    size_t common;

    if (a == NULL || b == NULL)
        return 0;
    common = (a->n < b->n) ? a->n : b->n;
    for (i = 0u; i < common; i++) {
        int c;

        if (a->p[i].num != b->p[i].num)
            return (a->p[i].num < b->p[i].num) ? -1 : 1;
        c = strcmp(a->p[i].suf, b->p[i].suf);
        if (c != 0)
            return (c < 0) ? -1 : 1;
    }
    /* Python tuple ordering: a prefix is smaller than what it is a prefix
     * of, so 0.3 < 0.3.1. */
    if (a->n != b->n)
        return (a->n < b->n) ? -1 : 1;
    return 0;
}

bool nd_remote_is_newer(const char *candidate, const char *installed)
{
    nd_verkey found;
    nd_verkey have;

    /* `if not installed: return True`. A phone with no recorded version
     * should still be offered one. */
    if (installed == NULL || installed[0] == '\0')
        return true;
    nd_verkey_parse(candidate, &found);
    nd_verkey_parse(installed, &have);
    return nd_verkey_cmp(&found, &have) > 0;
}

/* ------------------------------------------------------------------ *
 * enough_space
 * ------------------------------------------------------------------ */

bool nd_remote_enough_space(const char *directory, int64_t size)
{
    char resolved[ND_PATH_MAX];
    struct statvfs st;
    uint64_t have;
    uint64_t want;

    if (directory == NULL || directory[0] == '\0')
        directory = ".";
    if (nd_path_resolve(resolved, sizeof resolved, directory) != ND_OK)
        return true;
    if (statvfs(resolved, &st) != 0)
        return true; /* cannot tell; let the write fail honestly */

    have = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    want = (size > 0) ? (uint64_t)size : 0u;
    /* The margin cannot overflow a real size, but a hostile "size" can. A
     * request that would wrap is one nothing could satisfy. */
    if (want > UINT64_MAX - (uint64_t)ND_REMOTE_SPACE_MARGIN)
        return false;
    return have >= want + (uint64_t)ND_REMOTE_SPACE_MARGIN;
}

/* ------------------------------------------------------------------ *
 * The transport: one curl, three descriptors
 * ------------------------------------------------------------------ */

typedef struct {
    /* Called once, when the final status is known and BEFORE any body byte
     * is handed over. Non-ND_OK aborts the transfer. */
    nd_err (*on_status)(void *ctx, int32_t status);
    nd_err (*on_body)(void *ctx, const uint8_t *buf, size_t len);
    void *ctx;
} http_sink;

typedef struct {
    char line[HEADER_LINE_MAX];
    size_t line_len;
    int32_t last;  /* the last status line seen, redirects included */
    int32_t final; /* the last status that ends a chain            */
    char reason[64];
} header_state;

/* "HTTP/1.1 206 Partial Content" -> 206, "Partial Content".
 * "HTTP/2 206" -> 206, "" -- HTTP/2 has no reason phrase, so a caller that
 * prints one has to cope with its absence. */
static void header_line(header_state *h, const char *line, size_t len)
{
    size_t i = 0u;
    int32_t code = 0;
    size_t digits = 0u;

    while (len > 0u && (line[len - 1u] == '\r' || line[len - 1u] == '\n'))
        len--;
    if (len < 5u || memcmp(line, "HTTP/", 5u) != 0)
        return;

    while (i < len && line[i] != ' ')
        i++;
    while (i < len && line[i] == ' ')
        i++;
    while (i < len && digits < 3u && ascii_digit(line[i])) {
        code = code * 10 + (int32_t)(line[i] - '0');
        i++;
        digits++;
    }
    if (digits != 3u)
        return;

    h->last = code;
    while (i < len && line[i] == ' ')
        i++;
    copy_bounded(h->reason, sizeof h->reason, line + i, len - i);

    /* 1xx is informational and 3xx is followed by another request, so
     * neither ends the chain. Anything else does, and the body that follows
     * it is the body this transfer is about. */
    if (code / 100 != 1 && code / 100 != 3)
        h->final = code;
}

static void header_feed(header_state *h, const char *buf, size_t len)
{
    size_t i;

    for (i = 0u; i < len; i++) {
        if (buf[i] == '\n') {
            header_line(h, h->line, h->line_len);
            h->line_len = 0u;
        } else if (h->line_len + 1u < sizeof h->line) {
            h->line[h->line_len] = buf[i];
            h->line_len++;
        }
        /* An over-long line is truncated for parsing. Nothing this module
         * reads is longer than a status line, and refusing a release because
         * GitHub sent a long Link: header would be absurd. */
    }
}

/* execvp's PATH lookup, which nd_proc_spawn() does not do -- it takes a path
 * and execve()s it. The same shape as apps/MusicPlayer/audio.c's, and it is
 * ALSO the test seam: a stand-in `curl` earlier on PATH is what
 * test/unit/test_remote.c drives the real code against. */
static bool which_curl(char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    const char *seg;

    if (out == NULL || out_sz == 0u)
        return false;
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (seg = path; seg != NULL;) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        nd_err rc;

        if (len == 0u)
            rc = nd_snprintf(out, out_sz, "./curl");
        else
            rc = nd_snprintf(out, out_sz, "%.*s/curl", (int)len, seg);
        if (rc == ND_OK && access(out, X_OK) == 0)
            return true;
        seg = (colon != NULL) ? colon + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}

/* Every URL this module hands to curl either was built here from a validated
 * repository name or came out of GitHub's JSON, which is attacker-influenced
 * text arriving over the network. A URL that is not plainly https, or that
 * carries a space, a quote or a control character, is refused rather than
 * passed on: it would at best confuse curl and at worst be read as an
 * option. There is no shell anywhere in this path, so this is the only place
 * such a thing could get in. */
static bool url_is_usable(const char *url)
{
    size_t i;

    if (url == NULL)
        return false;
    if (strncmp(url, "https://", 8u) != 0)
        return false;
    for (i = 0u; url[i] != '\0'; i++) {
        unsigned char c = (unsigned char)url[i];

        if (c < 0x21u || c > 0x7eu)
            return false;
        if (c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>')
            return false;
    }
    return i < ND_REMOTE_URL_MAX;
}

/* Drains one descriptor into `store`, which may refuse the bytes; the read
 * happens either way so that curl is never blocked writing to a full pipe. */
static bool drain(int fd, char *store, size_t store_sz, size_t *store_len)
{
    char buf[1024];
    ssize_t n;

    do {
        n = read(fd, buf, sizeof buf);
    } while (n < 0 && errno == EINTR);

    if (n <= 0)
        return false; /* EOF, or an error we treat as one */
    if (store != NULL && *store_len + 1u < store_sz) {
        size_t room = store_sz - *store_len - 1u;
        size_t take = ((size_t)n < room) ? (size_t)n : room;

        memcpy(store + *store_len, buf, take);
        *store_len += take;
        store[*store_len] = '\0';
    }
    return true;
}

/* curl's complaint, with its own prefix removed: "curl: (6) Could not
 * resolve host: api.github.com" -> "Could not resolve host: api.github.com",
 * which is the shape of the `reason` urllib hands the Python. */
static const char *curl_reason(const char *stderr_text)
{
    const char *p = stderr_text;

    if (p == NULL || p[0] == '\0')
        return NULL;
    if (strncmp(p, "curl: ", 6u) == 0) {
        p += 6u;
        if (*p == '(') {
            const char *close = strchr(p, ')');

            if (close != NULL) {
                p = close + 1;
                while (*p == ' ')
                    p++;
            }
        }
    }
    return (*p != '\0') ? p : NULL;
}

static void trim_trailing_newlines(char *s)
{
    size_t len = strlen(s);

    while (len > 0u && (s[len - 1u] == '\n' || s[len - 1u] == '\r')) {
        len--;
        s[len] = '\0';
    }
}

/* One HTTP GET. `range_from` < 0 means "no Range header".
 *
 * Returns ND_UPD_OK when curl exited 0 and the final status was 2xx; the
 * body has then been handed to the sink as it arrived. Every other outcome
 * is one of the three refusals remote.py raises, with remote.py's wording.
 *
 * The bytes already handed to the sink are NOT unwound on failure. That is
 * deliberate and is the whole resume design: a connection that dropped after
 * 20 MB leaves 20 MB on the card for the next attempt to carry on from. */
static nd_update_err http_get(const char *url, int32_t speed_time, int64_t range_from,
                              const http_sink *sink, char *why, size_t why_sz)
{
    char exe[ND_PATH_MAX];
    char range_hdr[64];
    char connect_s[16];
    char speed_s[16];
    const char *argv[32];
    size_t argn = 0u;
    nd_proc_spec spec;
    int out_fd[2] = {-1, -1};
    int hdr_fd[2] = {-1, -1};
    int err_fd[2] = {-1, -1};
    pid_t pid = -1;
    header_state hdr;
    char err_text[CURL_ERR_MAX];
    size_t err_len = 0u;
    uint8_t *body = NULL;
    uint8_t *stash = NULL;
    size_t stash_len = 0u;
    bool started = false; /* on_status has been called */
    bool aborted = false; /* the sink refused; stop reading the body */
    bool header_eof = false;
    nd_err sink_rc = ND_OK;
    nd_update_err rc = ND_UPD_ERR_NETWORK;
    nd_proc_status status;
    struct pollfd pfd[3];
    size_t i;

    memset(&hdr, 0, sizeof hdr);
    err_text[0] = '\0';

    if (!url_is_usable(url)) {
        say_why(why, why_sz, "cannot reach GitHub: the release URL is not a plain https one");
        return ND_UPD_ERR_NETWORK;
    }
    if (!which_curl(exe, sizeof exe)) {
        /* subprocess.Popen's FileNotFoundError. On the phone this cannot
         * happen -- curl is in both defconfigs -- so if it is ever seen, it
         * is a broken image and saying so plainly is the useful answer. */
        say_why(why, why_sz, "cannot reach GitHub: no curl on this phone");
        return ND_UPD_ERR_NETWORK;
    }

    if (nd_snprintf(connect_s, sizeof connect_s, "%d", ND_REMOTE_CONNECT_TIMEOUT) != ND_OK ||
        nd_snprintf(speed_s, sizeof speed_s, "%d", (int)speed_time) != ND_OK) {
        say_why(why, why_sz, "network error: cannot build the request");
        return ND_UPD_ERR_NETWORK;
    }
    if (range_from >= 0 && nd_snprintf(range_hdr, sizeof range_hdr, "Range: bytes=%lld-",
                                       (long long)range_from) != ND_OK) {
        say_why(why, why_sz, "network error: cannot build the request");
        return ND_UPD_ERR_NETWORK;
    }

    body = malloc(ND_REMOTE_CHUNK);
    stash = malloc(ND_REMOTE_CHUNK);
    if (body == NULL || stash == NULL) {
        say_why(why, why_sz, "network error: out of memory");
        goto done;
    }

    /* O_CLOEXEC on OUR ends: nd_proc_spawn dup2()s only the descriptors it
     * is given and closes nothing else, so without this the child would
     * inherit all three read ends and the pipes would never report EOF. */
    if (pipe2(out_fd, O_CLOEXEC) != 0 || pipe2(hdr_fd, O_CLOEXEC) != 0 ||
        pipe2(err_fd, O_CLOEXEC) != 0) {
        say_why(why, why_sz, "network error: %s", strerror(errno));
        goto done;
    }

    argv[argn++] = "curl";
    argv[argn++] = "-s"; /* no progress meter on a pipe */
    argv[argn++] = "-S"; /* but do say what went wrong  */
    argv[argn++] = "-L"; /* asset URLs 302 to objects.githubusercontent.com */
    argv[argn++] = "--max-redirs";
    argv[argn++] = "10"; /* urllib's own limit */
    argv[argn++] = "--proto";
    argv[argn++] = "=https";
    argv[argn++] = "--proto-redir";
    argv[argn++] = "=https";
    argv[argn++] = "--tlsv1.2"; /* ssl.create_default_context()'s minimum */
    argv[argn++] = "--connect-timeout";
    argv[argn++] = connect_s;
    /* NOT --max-time. See the header comment: a total deadline would abort a
     * slow 53 MB download that was making perfectly good progress. */
    argv[argn++] = "--speed-limit";
    argv[argn++] = "1";
    argv[argn++] = "--speed-time";
    argv[argn++] = speed_s;
    argv[argn++] = "-D";
    argv[argn++] = CURL_HEADER_PATH;
    argv[argn++] = "-H";
    argv[argn++] = "User-Agent: " ND_REMOTE_USER_AGENT;
    argv[argn++] = "-H";
    argv[argn++] = "Accept: " ND_REMOTE_ACCEPT;
    if (range_from >= 0) {
        argv[argn++] = "-H";
        argv[argn++] = range_hdr;
    }
    argv[argn++] = url;
    argv[argn++] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    /* new_session is deliberately FALSE. curl must not outlive the app that
     * started it: staying in the app's process group means the core's
     * teardown reaches it, and even when it does not, the read ends of these
     * pipes close with this process and curl's next write gets EPIPE. A
     * download that survived its own UI would hold a bearer nobody is
     * watching. */
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = out_fd[1];
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = err_fd[1];
    spec.fds[2].child_fd = CURL_HEADER_FD;
    spec.fds[2].our_fd = hdr_fd[1];
    spec.n_fds = 3u;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        say_why(why, why_sz, "network error: cannot start curl");
        goto done;
    }
    started = false;

    (void)close(out_fd[1]);
    out_fd[1] = -1;
    (void)close(err_fd[1]);
    err_fd[1] = -1;
    (void)close(hdr_fd[1]);
    hdr_fd[1] = -1;

    /* All three are polled together because any one of them can fill its
     * pipe. Draining only stdout would let a hostile server stall the whole
     * transfer by sending 64 kB of headers. */
    for (;;) {
        int live = 0;
        int n;

        pfd[0].fd = hdr_fd[0]; /* headers FIRST -- see below */
        pfd[1].fd = out_fd[0];
        pfd[2].fd = err_fd[0];
        for (i = 0u; i < 3u; i++) {
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
            if (pfd[i].fd >= 0)
                live++;
        }
        if (live == 0)
            break;

        n = poll(pfd, 3u, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            say_why(why, why_sz, "network error: %s", strerror(errno));
            goto reap;
        }

        /* The header descriptor is serviced before the body descriptor in
         * every wakeup, so that a wakeup carrying both gives up the status
         * line before the bytes it describes. curl writes and flushes the
         * header block before it writes any body, which was measured rather
         * than assumed -- see test/unit/test_remote.c. */
        if (pfd[0].fd >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buf[1024];
            ssize_t got;

            do {
                got = read(hdr_fd[0], buf, sizeof buf);
            } while (got < 0 && errno == EINTR);
            if (got > 0) {
                header_feed(&hdr, buf, (size_t)got);
            } else {
                /* No more headers are coming, so whatever status was last
                 * seen is the final one -- this is what makes a chain that
                 * ends on a 3xx work rather than hang. */
                header_eof = true;
                if (hdr.final == 0)
                    hdr.final = hdr.last;
                (void)close(hdr_fd[0]);
                hdr_fd[0] = -1;
            }
        }

        if (pfd[2].fd >= 0 && (pfd[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            if (!drain(err_fd[0], err_text, sizeof err_text, &err_len)) {
                (void)close(err_fd[0]);
                err_fd[0] = -1;
            }
        }

        if (pfd[1].fd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ssize_t got;

            do {
                got = read(out_fd[0], body, ND_REMOTE_CHUNK);
            } while (got < 0 && errno == EINTR);
            if (got <= 0) {
                (void)close(out_fd[0]);
                out_fd[0] = -1;
                continue;
            }
            if (aborted)
                continue;

            if (hdr.final == 0) {
                /* Body before a final status. It has never been observed --
                 * curl flushes the header block first -- but a body byte
                 * must never reach the card on the strength of a status
                 * nobody read, so it waits here instead. */
                if (stash_len + (size_t)got > ND_REMOTE_CHUNK) {
                    say_why(why, why_sz, "network error: no HTTP status before the body");
                    aborted = true;
                    rc = ND_UPD_ERR_NETWORK;
                    continue;
                }
                memcpy(stash + stash_len, body, (size_t)got);
                stash_len += (size_t)got;
                continue;
            }

            if (!started) {
                if (hdr.final == 404) {
                    /* The Python's one 404 branch, and it is not an error
                     * screen: it means "reached GitHub, nothing here". */
                    say_why(why, why_sz, "no published release for %s", nd_remote_repo());
                    rc = ND_UPD_ERR_NO_PACKAGE;
                    aborted = true;
                    continue;
                }
                if (hdr.final >= 400) {
                    if (hdr.reason[0] != '\0')
                        say_why(why, why_sz, "GitHub said %d %s", hdr.final, hdr.reason);
                    else
                        say_why(why, why_sz, "GitHub said %d", hdr.final);
                    rc = ND_UPD_ERR_NETWORK;
                    aborted = true;
                    continue;
                }
                if (sink != NULL && sink->on_status != NULL) {
                    sink_rc = sink->on_status(sink->ctx, hdr.final);
                    if (sink_rc != ND_OK) {
                        aborted = true;
                        continue;
                    }
                }
                started = true;
                if (stash_len > 0u && sink != NULL && sink->on_body != NULL) {
                    sink_rc = sink->on_body(sink->ctx, stash, stash_len);
                    stash_len = 0u;
                    if (sink_rc != ND_OK) {
                        aborted = true;
                        continue;
                    }
                }
            }
            if (sink != NULL && sink->on_body != NULL) {
                sink_rc = sink->on_body(sink->ctx, body, (size_t)got);
                if (sink_rc != ND_OK)
                    aborted = true;
            }
        }
    }

reap:
    trim_trailing_newlines(err_text);

    /* A response with a status and no body at all never reached the branch
     * above, so the status is classified here as well. */
    if (!header_eof && hdr.final == 0)
        hdr.final = hdr.last;
    if (!started && !aborted) {
        if (hdr.final == 404) {
            say_why(why, why_sz, "no published release for %s", nd_remote_repo());
            rc = ND_UPD_ERR_NO_PACKAGE;
            aborted = true;
        } else if (hdr.final >= 400) {
            if (hdr.reason[0] != '\0')
                say_why(why, why_sz, "GitHub said %d %s", hdr.final, hdr.reason);
            else
                say_why(why, why_sz, "GitHub said %d", hdr.final);
            rc = ND_UPD_ERR_NETWORK;
            aborted = true;
        }
    }

    if (pid > 0) {
        memset(&status, 0, sizeof status);
        /* Bounded, not -1. All three pipes are already at EOF, so curl has
         * finished writing and is on its way out -- ten seconds is a tear-
         * down that has gone wrong, and hanging the update screen for ever
         * on it would be worse than killing it. */
        if (nd_proc_wait(pid, 10.0, &status) != ND_OK)
            (void)nd_proc_terminate(pid, 2.0, &status);
        if (aborted) {
            /* Already refused for a reason of our own; curl's exit code adds
             * nothing. */
        } else if (status.signalled) {
            say_why(why, why_sz, "cannot reach GitHub: curl was killed by signal %d", status.signo);
            rc = ND_UPD_ERR_NETWORK;
        } else if (!status.exited || status.exit_status != 0) {
            const char *reason = curl_reason(err_text);

            /* exit 127 is nd_proc_spawn's "execve failed"; every other code
             * is curl's own, and its stderr says what it means far better
             * than a table of curl exit codes would. */
            if (reason != NULL)
                say_why(why, why_sz, "cannot reach GitHub: %s", reason);
            else
                say_why(why, why_sz, "cannot reach GitHub: curl exited %d", status.exit_status);
            rc = ND_UPD_ERR_NETWORK;
        } else if (sink_rc != ND_OK) {
            /* The sink refused -- the card, not the network. */
            say_why(why, why_sz, "could not write the download");
            rc = ND_UPD_ERR_WRITE_FAILED;
        } else if (hdr.final == 0) {
            say_why(why, why_sz, "cannot reach GitHub: no HTTP response");
            rc = ND_UPD_ERR_NETWORK;
        } else {
            /* A 2xx with an empty body never called on_status; the sink has
             * to hear about it either way. */
            if (!started && sink != NULL && sink->on_status != NULL) {
                if (sink->on_status(sink->ctx, hdr.final) != ND_OK) {
                    say_why(why, why_sz, "could not write the download");
                    rc = ND_UPD_ERR_WRITE_FAILED;
                    goto done;
                }
            }
            rc = ND_UPD_OK;
        }
    }
    if (aborted && sink_rc != ND_OK) {
        say_why(why, why_sz, "could not write the download");
        rc = ND_UPD_ERR_WRITE_FAILED;
    }

done:
    for (i = 0u; i < 2u; i++) {
        if (out_fd[i] >= 0)
            (void)close(out_fd[i]);
        if (hdr_fd[i] >= 0)
            (void)close(hdr_fd[i]);
        if (err_fd[i] >= 0)
            (void)close(err_fd[i]);
    }
    free(body);
    free(stash);
    return rc;
}

/* ------------------------------------------------------------------ *
 * all_releases / latest
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    bool too_big;
} body_buf;

static nd_err body_buf_write(void *ctx, const uint8_t *data, size_t len)
{
    body_buf *b = (body_buf *)ctx;

    if (b->too_big)
        return ND_OK; /* keep draining so curl is never blocked */
    if (b->len + len > ND_REMOTE_BODY_MAX) {
        b->too_big = true;
        return ND_OK;
    }
    if (b->len + len + 1u > b->cap) {
        size_t want = b->cap ? b->cap * 2u : ND_REMOTE_CHUNK;
        uint8_t *grown;

        while (want < b->len + len + 1u)
            want *= 2u;
        if (want > ND_REMOTE_BODY_MAX + 1u)
            want = ND_REMOTE_BODY_MAX + 1u;
        grown = realloc(b->buf, want);
        if (grown == NULL)
            return ND_ERR_NOMEM;
        b->buf = grown;
        b->cap = want;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return ND_OK;
}

/* GET the release listing and parse it. *doc is owned by the caller. */
static nd_update_err fetch_releases(int32_t limit, nd_json_doc **doc, char *why, size_t why_sz)
{
    char url[ND_REMOTE_URL_MAX];
    body_buf body;
    http_sink sink;
    nd_update_err rc;
    const nd_json_val *root;

    *doc = NULL;
    memset(&body, 0, sizeof body);

    if (!repo_is_usable(nd_remote_repo())) {
        say_why(why, why_sz, "cannot reach GitHub: %s is not a usable repository name",
                ND_REMOTE_REPO_ENV);
        return ND_UPD_ERR_NETWORK;
    }
    if (limit <= 0 || limit > ND_RELEASES_LIMIT)
        limit = ND_RELEASES_LIMIT;
    if (nd_snprintf(url, sizeof url, ND_REMOTE_API_ALL, nd_remote_repo(), (int)limit) != ND_OK) {
        say_why(why, why_sz, "cannot reach GitHub: the repository name is too long");
        return ND_UPD_ERR_NETWORK;
    }

    memset(&sink, 0, sizeof sink);
    sink.on_body = body_buf_write;
    sink.ctx = &body;

    /* Reaching GitHub is the 20-second job, not the 120-second one. */
    rc = http_get(url, ND_REMOTE_CONNECT_TIMEOUT, -1, &sink, why, why_sz);
    if (rc != ND_UPD_OK) {
        free(body.buf);
        return rc;
    }
    if (body.too_big) {
        free(body.buf);
        say_why(why, why_sz, "GitHub sent something that is not JSON");
        return ND_UPD_ERR_NETWORK;
    }

    if (body.buf == NULL || body.len == 0u ||
        nd_json_parse(body.buf, body.len, doc, NULL, 0u) != ND_OK) {
        free(body.buf);
        *doc = NULL;
        say_why(why, why_sz, "GitHub sent something that is not JSON");
        return ND_UPD_ERR_NETWORK;
    }
    free(body.buf);

    root = nd_json_root(*doc);
    if (nd_json_type_of(root) != ND_JSON_ARRAY) {
        /* The single-release endpoint returns an object. Asking for the list
         * and getting one back means something is wrong. */
        nd_json_free(*doc);
        *doc = NULL;
        say_why(why, why_sz, "GitHub did not send a list of releases");
        return ND_UPD_ERR_NETWORK;
    }
    return ND_UPD_OK;
}

/* One release entry -> one nd_release, or false when this entry carries no
 * asset for this platform.
 *
 * Every field is absent-until-proven. A tag, a URL or a version that does
 * not fit its bound makes the whole entry unusable rather than a truncated
 * one: a truncated URL fetches the wrong thing and a truncated version is
 * compared against the installed one. Notes are the single exception and the
 * header says why. */
static bool entry_to_release(const nd_json_val *entry, const char *wanted, nd_release *out)
{
    const char *tag;
    const char *notes;
    const nd_json_val *assets;
    size_t n;
    size_t i;

    if (nd_json_type_of(entry) != ND_JSON_OBJECT)
        return false;

    /* `tag = entry.get("tag_name") or ""` -- null, false and "" all become
     * the empty string, which then makes an empty version. */
    tag = nd_json_get_str(entry, "tag_name", "");
    assets = nd_json_get(entry, "assets");
    if (nd_json_type_of(assets) != ND_JSON_ARRAY)
        return false;

    n = nd_json_len(assets);
    for (i = 0u; i < n; i++) {
        const nd_json_val *asset = nd_json_at(assets, i);
        const char *name;
        const char *url;
        int64_t size;
        const char *version;

        if (nd_json_type_of(asset) != ND_JSON_OBJECT)
            continue;
        name = nd_json_get_str(asset, "name", "");
        if (strcmp(name, wanted) != 0)
            continue;

        /* The FIRST matching asset wins and the entry is done, exactly as
         * the Python's `break`. */
        memset(out, 0, sizeof *out);

        url = nd_json_get_str(asset, "browser_download_url", "");
        if (!url_is_usable(url))
            return false;
        if (nd_strlcpy(out->url, url, sizeof out->url) >= sizeof out->url)
            return false;
        if (nd_strlcpy(out->tag, tag, sizeof out->tag) >= sizeof out->tag)
            return false;

        /* `tag.lstrip("v")` strips EVERY leading v, not just one. Old tags
         * were v0.1.5a and os-release never carries the v. */
        version = out->tag;
        while (*version == 'v')
            version++;
        if (nd_strlcpy(out->version, version, sizeof out->version) >= sizeof out->version)
            return false;

        /* `int(asset.get("size") or 0)`. A size that is absent, null, a
         * string or negative is 0, which means "unknown" downstream -- the
         * download then cannot check for room or for truncation, which is
         * exactly what Python does with it. */
        size = nd_json_get_int(asset, "size", 0);
        out->size = (size > 0) ? size : 0;

        notes = nd_json_get_str(entry, "body", "");
        (void)nd_strlcpy(out->notes, notes, sizeof out->notes);
        out->prerelease = nd_json_get_bool(entry, "prerelease", false);
        return true;
    }
    return false;
}

nd_update_err nd_remote_all_releases(const char *platform, int32_t limit, nd_release *out,
                                     size_t max_out, size_t *n_out, char *why, size_t why_sz)
{
    char wanted[ND_REMOTE_ASSET_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    nd_update_err rc;
    size_t n;
    size_t i;
    size_t found = 0u;
    size_t kept = 0u;

    if (n_out != NULL)
        *n_out = 0u;
    if (out == NULL || max_out == 0u)
        return ND_UPD_ERR_NETWORK;
    if (nd_remote_asset_name(platform, wanted, sizeof wanted) != ND_OK) {
        say_why(why, why_sz, "no release carries an update for this phone");
        return ND_UPD_ERR_NO_PACKAGE;
    }

    rc = fetch_releases(limit, &doc, why, why_sz);
    if (rc != ND_UPD_OK)
        return rc;

    root = nd_json_root(doc);
    n = nd_json_len(root);
    for (i = 0u; i < n; i++) {
        nd_release entry;

        if (!entry_to_release(nd_json_at(root, i), wanted, &entry))
            continue; /* a release with no asset for this platform is skipped */
        found++;
        if (kept < max_out) {
            out[kept] = entry;
            kept++;
        }
    }
    nd_json_free(doc);

    if (found == 0u) {
        say_why(why, why_sz, "no release carries %s", wanted);
        return ND_UPD_ERR_NO_PACKAGE;
    }
    if (n_out != NULL)
        *n_out = kept;
    return ND_UPD_OK;
}

nd_update_err nd_remote_latest(const char *platform, nd_release *out, char *why, size_t why_sz)
{
    char wanted[ND_REMOTE_ASSET_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    nd_update_err rc;
    nd_verkey best_key;
    bool have_best = false;
    size_t n;
    size_t i;

    if (out == NULL)
        return ND_UPD_ERR_NETWORK;
    memset(out, 0, sizeof *out);
    memset(&best_key, 0, sizeof best_key);

    if (nd_remote_asset_name(platform, wanted, sizeof wanted) != ND_OK) {
        say_why(why, why_sz, "no release carries an update for this phone");
        return ND_UPD_ERR_NO_PACKAGE;
    }

    rc = fetch_releases(ND_RELEASES_LIMIT, &doc, why, why_sz);
    if (rc != ND_UPD_OK)
        return rc;

    /* max(published, key=version_key). Python's max keeps the FIRST maximal
     * element, so the comparison is strictly greater and the earlier entry
     * in GitHub's list wins a tie. Ordering by version rather than by
     * publication date is what stops a re-published old tag being offered as
     * an update. */
    root = nd_json_root(doc);
    n = nd_json_len(root);
    for (i = 0u; i < n; i++) {
        nd_release entry;
        nd_verkey key;

        if (!entry_to_release(nd_json_at(root, i), wanted, &entry))
            continue;
        nd_verkey_parse(entry.version, &key);
        if (!have_best || nd_verkey_cmp(&key, &best_key) > 0) {
            *out = entry;
            best_key = key;
            have_best = true;
        }
    }
    nd_json_free(doc);

    if (!have_best) {
        memset(out, 0, sizeof *out);
        say_why(why, why_sz, "no release carries %s", wanted);
        return ND_UPD_ERR_NO_PACKAGE;
    }
    return ND_UPD_OK;
}

/* ------------------------------------------------------------------ *
 * download
 * ------------------------------------------------------------------ */

typedef struct {
    int fd;
    int64_t have;   /* bytes already on the card when this attempt began */
    int64_t done;   /* bytes in the file now                            */
    int64_t expect; /* the package size, 0 when it is not known         */
    nd_remote_progress_fn progress;
    void *ctx;
    int saved_errno;
} part_sink;

/* THE 206-VERSUS-200 DECISION, and the only place it is made.
 *
 * A server that honours the Range answers 206 and the bytes that follow
 * continue the file. A server that ignores it answers 200 and the bytes that
 * follow are the file FROM THE BEGINNING -- appending those would write the
 * first `have` bytes twice and produce a package that fails its sha256 much
 * later with nothing to explain why. So the partial is truncated and the
 * download starts over, which costs the bytes already fetched and is the
 * only correct answer. */
static nd_err part_on_status(void *ctx, int32_t status)
{
    part_sink *s = (part_sink *)ctx;
    bool resuming = (s->have > 0) && (status == 206);

    if (!resuming) {
        if (ftruncate(s->fd, 0) != 0 || lseek(s->fd, 0, SEEK_SET) == (off_t)-1) {
            s->saved_errno = errno;
            return ND_ERR_IO;
        }
        s->have = 0;
        s->done = 0;
        return ND_OK;
    }
    if (lseek(s->fd, (off_t)s->have, SEEK_SET) == (off_t)-1) {
        s->saved_errno = errno;
        return ND_ERR_IO;
    }
    s->done = s->have;
    return ND_OK;
}

static nd_err part_on_body(void *ctx, const uint8_t *data, size_t len)
{
    part_sink *s = (part_sink *)ctx;
    size_t written = 0u;

    while (written < len) {
        ssize_t n = write(s->fd, data + written, len - written);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            s->saved_errno = errno;
            return ND_ERR_IO;
        }
        written += (size_t)n;
    }
    s->done += (int64_t)len;
    if (s->progress != NULL)
        s->progress(s->ctx, s->done, s->expect);
    return ND_OK;
}

static int64_t file_size(const char *real_path)
{
    struct stat st;

    if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return (int64_t)st.st_size;
}

/* One attempt. Appends to `partial` from byte `have`; *done_out receives the
 * total in the file afterwards, whether or not the attempt succeeded. */
static nd_update_err fetch_into(const char *url, const char *partial_real, int64_t have,
                                int64_t size, nd_remote_progress_fn progress, void *ctx,
                                int64_t *done_out, char *why, size_t why_sz)
{
    part_sink sink_ctx;
    http_sink sink;
    nd_update_err rc;
    int fd;

    memset(&sink_ctx, 0, sizeof sink_ctx);
    sink_ctx.have = have;
    sink_ctx.done = have;
    sink_ctx.expect = size;
    sink_ctx.progress = progress;
    sink_ctx.ctx = ctx;

    /* No O_TRUNC: whether the file is truncated is the status line's
     * decision and it has not been read yet. */
    fd = open(partial_real, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        say_why(why, why_sz, "could not write the download: %s", strerror(errno));
        *done_out = have;
        return ND_UPD_ERR_WRITE_FAILED;
    }
    sink_ctx.fd = fd;

    memset(&sink, 0, sizeof sink);
    sink.on_status = part_on_status;
    sink.on_body = part_on_body;
    sink.ctx = &sink_ctx;

    /* `headers["Range"] = "bytes=%d-" % have` when there is something to
     * resume, and no Range header at all when there is not. */
    rc = http_get(url, ND_REMOTE_DOWNLOAD_TIMEOUT, (have > 0) ? have : -1, &sink, why, why_sz);

    if (rc == ND_UPD_OK || rc == ND_UPD_ERR_NETWORK) {
        /* fsync before the descriptor closes: what is on the card is the
         * whole point of resuming, and a partial that only exists in the
         * page cache does not survive the battery coming out. */
        (void)fsync(fd);
    }
    (void)close(fd);

    if (rc == ND_UPD_ERR_WRITE_FAILED && sink_ctx.saved_errno != 0)
        say_why(why, why_sz, "could not write the download: %s", strerror(sink_ctx.saved_errno));

    *done_out = sink_ctx.done;
    return rc;
}

static void discard(const char *real_path)
{
    if (unlink(real_path) != 0 && errno != ENOENT)
        nd_log_err(ND_LOG_UPDATE, "cannot remove %s: %s", real_path, strerror(errno));
}

nd_update_err nd_remote_download(const char *url, const char *destination, int64_t size,
                                 nd_remote_progress_fn progress, void *ctx, int32_t attempts,
                                 int64_t *written_out, char *why, size_t why_sz)
{
    char dest_real[ND_PATH_MAX];
    char partial_real[ND_PATH_MAX];
    char directory[ND_PATH_MAX];
    char last_why[ND_REMOTE_WHY_MAX];
    nd_update_err last_rc = ND_UPD_ERR_NETWORK;
    int64_t have;
    int32_t total;
    int32_t attempt;
    char *slash;

    if (written_out != NULL)
        *written_out = 0;
    if (destination == NULL || destination[0] == '\0') {
        say_why(why, why_sz, "network error: nowhere to put the download");
        return ND_UPD_ERR_WRITE_FAILED;
    }
    if (nd_path_resolve(dest_real, sizeof dest_real, destination) != ND_OK ||
        nd_snprintf(partial_real, sizeof partial_real, "%s.part", dest_real) != ND_OK) {
        say_why(why, why_sz, "network error: the download path is too long");
        return ND_UPD_ERR_WRITE_FAILED;
    }
    (void)nd_strlcpy(last_why, "download did not finish", sizeof last_why);

    /* os.path.dirname(destination) or "." -- the VIRTUAL path, because
     * nd_remote_enough_space() resolves it itself. */
    (void)nd_strlcpy(directory, destination, sizeof directory);
    slash = strrchr(directory, '/');
    if (slash != NULL)
        *slash = '\0';
    if (directory[0] == '\0')
        (void)nd_strlcpy(directory, ".", sizeof directory);

    have = file_size(partial_real);
    if (size > 0 && have > size) {
        /* Longer than the package: left over from a different, larger one.
         * Resuming from past the end would ask for a range no server can
         * serve. */
        discard(partial_real);
        have = 0;
    }

    /* Only the part still to come needs room. */
    if (size > 0 && !nd_remote_enough_space(directory, (size > have) ? size - have : 0)) {
        say_why(why, why_sz, "not enough room on the card for %lld bytes", (long long)size);
        return ND_UPD_ERR_NO_SPACE;
    }

    total = (attempts > 0) ? attempts : 1;
    for (attempt = 0; attempt < total; attempt++) {
        int64_t done = have;
        nd_update_err rc;

        if (attempt > 0) {
            /* min(RETRY_BACKOFF * 2 ** (attempt - 1), RETRY_BACKOFF_MAX):
             * 5, 10, 20, 40, then 60 for ever. Not a restart -- the next
             * attempt resumes -- so the wait buys the bearer time to come
             * back rather than costing the owner bytes. */
            uint32_t wait = ND_REMOTE_RETRY_BACKOFF_MAX;

            if (attempt <= 24) {
                uint32_t doubled = (uint32_t)ND_REMOTE_RETRY_BACKOFF << (attempt - 1);

                if (doubled < ND_REMOTE_RETRY_BACKOFF_MAX)
                    wait = doubled;
            }
            back_off(wait);
        }

        rc = fetch_into(url, partial_real, have, size, progress, ctx, &done, last_why,
                        sizeof last_why);

        if (rc == ND_UPD_ERR_WRITE_FAILED) {
            /* The card failed, not the network. Bytes that could not be
             * written are not progress, so the partial goes. */
            discard(partial_real);
            say_why(why, why_sz, "%s", last_why);
            return rc;
        }
        if (rc == ND_UPD_ERR_NO_PACKAGE) {
            /* A 404 on the asset itself. remote.NoRelease is not caught by
             * the Python's retry loop either: there is nothing at that URL
             * and waiting will not put anything there. */
            say_why(why, why_sz, "%s", last_why);
            return rc;
        }
        if (rc != ND_UPD_OK && !(size > 0 && done == size)) {
            last_rc = rc;
            /* The bytes already on the card stay where they are. */
            have = file_size(partial_real);
            continue;
        }
        /* The `done == size` half of that condition is not decoration. curl
         * exits 18 when the connection closed with bytes it was still
         * expecting -- and a server that closed after sending the last byte
         * of the package looks exactly like that. Without this, the file
         * would be complete on the card and every remaining attempt would
         * ask for "bytes=<size>-", get nothing, be complained about again,
         * and the download would fail with the whole package sitting beside
         * it. The length is the thing that decides, and the sha256 that
         * follows is what decides whether the length was telling the truth.
         *
         * remote.py has no equivalent branch because its network failures
         * can only come out of _open(), before a single byte is read. */

        if (size > 0 && done != size) {
            /* Short, but the bytes are good as far as they go. Pick it up. */
            (void)snprintf(last_why, sizeof last_why, "download stopped early (%lld of %lld bytes)",
                           (long long)done, (long long)size);
            last_rc = ND_UPD_ERR_NETWORK;
            have = done;
            continue;
        }

        if (rename(partial_real, dest_real) != 0) {
            say_why(why, why_sz, "could not write the download: %s", strerror(errno));
            return ND_UPD_ERR_WRITE_FAILED;
        }
        if (written_out != NULL)
            *written_out = done;
        return ND_UPD_OK;
    }

    /* Out of attempts is not out of luck: the .part file is still there and
     * the next press of the button carries on from it. */
    say_why(why, why_sz, "%s", last_why);
    return last_rc;
}
