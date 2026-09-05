/* test_fetch_app.c -- Fetch's transport, driven over real pipes.
 *
 * Nothing here is mocked. A stand-in `curl` (neodct/tests/ftp/fake-curl) is
 * put first on PATH; apps/Fetch/ftp.c finds it with its own PATH walk, spawns
 * it with nd_proc_spawn() and cannot tell the difference. So the argv it
 * builds, the pipes it reads, the netrc it writes, the .part file and the
 * rename are all the real ones. Only the network is absent -- which is the
 * same trick test_remote.c plays on nd_remote, for the same reason.
 *
 * ============ THE ASSERTIONS THAT ARE ABOUT SECURITY ============
 *
 * Three of these tests exist because getting them wrong would be quiet:
 *
 *   - the password must NOT appear in argv, which is world-readable through
 *     /proc for as long as curl runs, on a phone that ships a shell app;
 *   - the netrc that carries it instead must be mode 0600 and must be GONE
 *     when the call returns;
 *   - --ssl-reqd and --proto =ftp must be on every invocation, or a server
 *     that declines TLS gets the password in the clear.
 *
 * Each of those is a one-character edit away from being false, and none of
 * them would change what the screen shows.
 */

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_types.h"
#include "platform_test.h"

/* Kept in step with apps/Fetch/fetch_app.h; see test_fetch.c. */
#define ND_FETCH_NAME_MAX 96
#define ND_FETCH_HOST_MAX 128
#define ND_FETCH_PASS_MAX 64
#define ND_FETCH_WHY_MAX  160

typedef struct {
    char name[ND_FETCH_NAME_MAX];
    int64_t size;
    bool is_dir;
} fetch_entry;

typedef struct {
    char host[ND_FETCH_HOST_MAX];
    char user[ND_FETCH_NAME_MAX];
    char pass[ND_FETCH_PASS_MAX];
} fetch_conn;

typedef bool (*fetch_progress_fn)(void *ctx, int64_t done, int64_t total);

static struct {
    void *h;
    nd_err (*list)(const fetch_conn *, const char *, fetch_entry *, size_t, size_t *, char *,
                   size_t);
    nd_err (*download)(const fetch_conn *, const char *, const char *, const char *, int64_t,
                       fetch_progress_fn, void *, char *, size_t);
} api;

/* The password every test types. It is a literal so that the "not in argv"
 * assertion has something specific to grep for. */
#define TEST_PASSWORD "hunter2butlonger"

static char g_fixtures[ND_PATH_MAX]; /* neodct/tests/ftp            */
static char g_ctl[ND_PATH_MAX];      /* the stand-in's control files */
static char g_bindir[ND_PATH_MAX];   /* holds our `curl`             */
static char g_path_keep[ND_PATH_MAX * 2];

/* ------------------------------------------------------------------ *
 * Wiring
 * ------------------------------------------------------------------ */

static bool resolve_app_so(char *out, size_t sz)
{
    char exe[ND_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    char *slash;

    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(out, sz, "%s/../apps/Fetch/app.so", exe) == ND_OK;
}

/* The fixtures sit beside the source tree rather than under a phone root, so
 * this walks up from the test binary the way test_remote.c does. */
static bool find_fixtures(void)
{
    char self[ND_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1u);
    char *slash;

    if (n <= 0)
        return false;
    self[n] = '\0';
    slash = strrchr(self, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(g_fixtures, sizeof g_fixtures, "%s/../../../../tests/ftp", self) == ND_OK;
}

static bool api_open(void)
{
    char so[ND_PATH_MAX];

    if (!resolve_app_so(so, sizeof so))
        return false;
    api.h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (api.h == NULL) {
        fprintf(stderr, "test_fetch_app: dlopen %s: %s -- run `make` first\n", so, dlerror());
        return false;
    }
    *(void **)&api.list = dlsym(api.h, "fetch_list");
    *(void **)&api.download = dlsym(api.h, "fetch_download");
    if (api.list == NULL || api.download == NULL) {
        fprintf(stderr, "test_fetch_app: app.so is missing fetch_list/fetch_download\n");
        return false;
    }
    return true;
}

static bool run(const char *fmt, ...) ND_PRINTF(1, 2);

static bool run(const char *fmt, ...)
{
    char cmd[ND_PATH_MAX * 3];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return false;
    return system(cmd) == 0;
}

/* A control file for the stand-in. */
static void ctl(const char *name, const char *contents)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/%s", g_ctl, name) != ND_OK)
        return;
    f = fopen(path, "w");
    if (f == NULL)
        return;
    (void)fputs(contents, f);
    (void)fclose(f);
}

/* Whatever the stand-in wrote back, or "" if it wrote nothing. */
static const char *readback(const char *name)
{
    static char buf[8192];
    char path[ND_PATH_MAX];
    FILE *f;
    size_t got;

    buf[0] = '\0';
    if (nd_snprintf(path, sizeof path, "%s/%s", g_ctl, name) != ND_OK)
        return buf;
    f = fopen(path, "r");
    if (f == NULL)
        return buf;
    got = fread(buf, 1u, sizeof buf - 1u, f);
    buf[got] = '\0';
    (void)fclose(f);
    return buf;
}

/* A fresh, empty control directory per case, and a `curl` that is ours. Called
 * from every test because pt_new_case() has just thrown the last one away. */
static bool scenario(void)
{
    const char *tmp = getenv("TMPDIR");

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (nd_snprintf(g_ctl, sizeof g_ctl, "%s/ndftp-ctl", tmp) != ND_OK)
        return false;
    if (nd_snprintf(g_bindir, sizeof g_bindir, "%s/ndftp-bin", tmp) != ND_OK)
        return false;
    if (!run("rm -rf '%s' && mkdir -p '%s' '%s'", g_ctl, g_ctl, g_bindir))
        return false;
    if (!run("cp '%s/fake-curl' '%s/curl' && chmod 0755 '%s/curl'", g_fixtures, g_bindir,
             g_bindir))
        return false;
    return setenv("NDFTP_DIR", g_ctl, 1) == 0;
}

static bool path_with_fake_curl(void)
{
    char path[ND_PATH_MAX * 2];

    if (nd_snprintf(path, sizeof path, "%s:%s", g_bindir, g_path_keep) != ND_OK)
        return false;
    return setenv("PATH", path, 1) == 0;
}

static void conn_init(fetch_conn *c)
{
    memset(c, 0, sizeof *c);
    (void)nd_strlcpy(c->host, "10.0.0.1", sizeof c->host);
    (void)nd_strlcpy(c->user, "neodct", sizeof c->user);
    (void)nd_strlcpy(c->pass, TEST_PASSWORD, sizeof c->pass);
}

/* ------------------------------------------------------------------ *
 * Listing
 * ------------------------------------------------------------------ */

static void test_list_reads_what_the_server_said(void)
{
    fetch_conn c;
    fetch_entry got[32];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";
    char listing[ND_PATH_MAX];

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    (void)nd_snprintf(listing, sizeof listing, "%s/listing.txt", g_fixtures);
    ctl("listing", listing);

    conn_init(&c);
    CHECK(api.list(&c, "", got, ND_ARRAY_LEN(got), &n, why, sizeof why) == ND_OK);

    /* Eleven lines in the fixture; the "total" header, a symlink, a device
     * node, "-o" and "../escape.mp3" are all dropped. */
    CHECK_INT(n, 7);
    CHECK_STR(got[0].name, "music");
    CHECK(got[0].is_dir);
    CHECK_STR(got[1].name, "roms");
    CHECK_STR(got[2].name, "A Forest.mp3");
    CHECK_INT(got[2].size, 4194304);
    CHECK_STR(got[3].name, "Bible-qemu-aarch64.nap");
    CHECK_STR(got[4].name, "Crash Bandicoot.bin");
    CHECK_STR(got[5].name, "empty.txt");
    CHECK_INT(got[5].size, 0);
    CHECK_STR(got[6].name, "unreadable-size.mp3");
    CHECK_INT(got[6].size, -1);

    /* The trailing slash is what makes a LIST rather than a RETR. */
    CHECK_STR(readback("urls"), "ftp://10.0.0.1/\n");
}

static void test_a_subdirectory_is_asked_for_by_name(void)
{
    fetch_conn c;
    fetch_entry got[8];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    conn_init(&c);
    (void)api.list(&c, "music/live", got, ND_ARRAY_LEN(got), &n, why, sizeof why);
    CHECK_STR(readback("urls"), "ftp://10.0.0.1/music/live/\n");
}

static void test_the_password_never_reaches_argv(void)
{
    fetch_conn c;
    fetch_entry got[8];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";
    const char *argv;

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    conn_init(&c);
    CHECK(api.list(&c, "", got, ND_ARRAY_LEN(got), &n, why, sizeof why) == ND_OK);

    /* /proc/<pid>/cmdline is readable by anything on this phone, LinuxShell
     * included, for as long as curl runs. */
    argv = readback("argv");
    CHECK(strstr(argv, TEST_PASSWORD) == NULL);
    CHECK(strstr(argv, "neodct") == NULL || strstr(argv, "--user") == NULL);

    /* It went through the netrc instead, which was mode 0600 while curl had
     * it... */
    CHECK(strstr(readback("netrc"), TEST_PASSWORD) != NULL);
    CHECK_STR(readback("netrc.mode"), "600\n");

    /* ...and does not exist any more now that the call has returned. */
    {
        char path[ND_PATH_MAX];
        char *nl;

        (void)nd_strlcpy(path, readback("netrc.path"), sizeof path);
        nl = strchr(path, '\n');
        if (nl != NULL)
            *nl = '\0';
        CHECK(path[0] != '\0');
        CHECK(access(path, F_OK) != 0);
    }
}

static void test_tls_is_required_on_every_call(void)
{
    fetch_conn c;
    fetch_entry got[8];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";
    const char *argv;

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    conn_init(&c);
    CHECK(api.list(&c, "", got, ND_ARRAY_LEN(got), &n, why, sizeof why) == ND_OK);

    argv = readback("argv");
    /* Without this a server that declines AUTH TLS gets the password in the
     * clear, and curl would not say so. */
    CHECK(strstr(argv, "--ssl-reqd") != NULL);
    /* And curl may speak nothing but FTP, so no reply can turn a transfer
     * into a local file read. */
    CHECK(strstr(argv, "--proto\n=ftp\n") != NULL);
}

static void test_a_refused_login_says_so(void)
{
    fetch_conn c;
    fetch_entry got[8];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    ctl("exit", "67\n");
    ctl("stderr", "curl: (67) Access denied: 530\n");

    conn_init(&c);
    /* ND_ERR_PERM rather than a generic failure, and a sentence the owner can
     * act on -- this is the one failure they can fix by typing again. */
    CHECK(api.list(&c, "", got, ND_ARRAY_LEN(got), &n, why, sizeof why) == ND_ERR_PERM);
    /* Naming the USER is the point: the password is the half the owner just
     * typed, and the user name is the half that comes from a default nobody
     * looks at -- which is the way round this actually failed. */
    CHECK_STR(why, "Login refused as \"neodct\".\nCheck the name and password.");
    CHECK_INT(n, 0);
}

static void test_a_network_failure_says_what_curl_said(void)
{
    fetch_conn c;
    fetch_entry got[8];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    ctl("exit", "6\n");
    ctl("stderr", "curl: (6) Could not resolve host: example\n");

    conn_init(&c);
    CHECK(api.list(&c, "", got, ND_ARRAY_LEN(got), &n, why, sizeof why) == ND_ERR_IO);
    CHECK_STR(why, "No network: cannot look up the server.");
}

static void test_no_curl_at_all(void)
{
    fetch_conn c;
    fetch_entry got[8];
    size_t n = 0u;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(setenv("PATH", "", 1) == 0);
    conn_init(&c);
    CHECK(api.list(&c, "", got, ND_ARRAY_LEN(got), &n, why, sizeof why) == ND_ERR_NOTFOUND);
    CHECK_STR(why, "No curl on this phone.");
    CHECK(path_with_fake_curl());
}

/* ------------------------------------------------------------------ *
 * Downloading
 * ------------------------------------------------------------------ */

static int g_progress_calls;
static int64_t g_progress_last;

static bool count_progress(void *ctx, int64_t done, int64_t total)
{
    ND_UNUSED(ctx);
    ND_UNUSED(total);
    g_progress_calls++;
    g_progress_last = done;
    return true;
}

static bool cancel_at_once(void *ctx, int64_t done, int64_t total)
{
    ND_UNUSED(ctx);
    ND_UNUSED(done);
    ND_UNUSED(total);
    g_progress_calls++;
    return false;
}

/* A body of `len` bytes of a repeating pattern, written where the stand-in
 * can find it, and pointed at by the `body` control file. */
static void set_body(size_t len)
{
    char path[ND_PATH_MAX];
    FILE *f;
    size_t i;

    if (nd_snprintf(path, sizeof path, "%s/body.bin", g_ctl) != ND_OK)
        return;
    f = fopen(path, "w");
    if (f == NULL)
        return;
    for (i = 0u; i < len; i++)
        (void)fputc((int)(i & 0xffu), f);
    (void)fclose(f);
    ctl("body", path);
}

static void test_download_lands_where_it_was_asked_to(void)
{
    fetch_conn c;
    char why[ND_FETCH_WHY_MAX] = "";
    char resolved[ND_PATH_MAX];
    struct stat st;

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    set_body(4096u);
    g_progress_calls = 0;

    conn_init(&c);
    CHECK(api.download(&c, "music", "A Forest.mp3", "/card/music/A Forest.mp3", 4096,
                       count_progress, NULL, why, sizeof why) == ND_OK);

    /* The folder was made on the way -- the card has music/ already, but a
     * PSX game's folder does not exist until a disc is downloaded into it. */
    CHECK(nd_path_is_file("/card/music/A Forest.mp3"));
    CHECK(nd_path_resolve(resolved, sizeof resolved, "/card/music/A Forest.mp3") == ND_OK);
    CHECK(stat(resolved, &st) == 0 && st.st_size == 4096);

    /* And the .part it was written through is gone, because nothing that
     * scans music/ should ever see one. */
    CHECK(!nd_path_is_file("/card/music/A Forest.mp3.part"));

    /* Escaped, because a space is not legal in a URL and curl rejects the
     * whole transfer over one. */
    CHECK_STR(readback("urls"), "ftp://10.0.0.1/music/A%20Forest.mp3\n");
}

static void test_progress_is_driven_from_the_growing_file(void)
{
    fetch_conn c;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    set_body(64u * 1024u);
    ctl("chunks", "6\n"); /* six pieces, a tenth of a second apart */
    g_progress_calls = 0;
    g_progress_last = -1;

    conn_init(&c);
    CHECK(api.download(&c, "", "big.bin", "/card/untrusted/big.bin", 64 * 1024, count_progress,
                       NULL, why, sizeof why) == ND_OK);
    /* The bar moved at least once while the file was still arriving, which is
     * the whole reason the loop polls rather than blocking on wait(). */
    CHECK(g_progress_calls > 0);
    CHECK(g_progress_last >= 0);
}

static void test_a_cancelled_download_is_not_saved(void)
{
    fetch_conn c;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    set_body(64u * 1024u);
    ctl("chunks", "10\n");
    g_progress_calls = 0;

    conn_init(&c);
    /* What an incoming call does: the callback says stop, curl is killed and
     * nothing takes the final name. */
    CHECK(api.download(&c, "", "big.bin", "/card/untrusted/big.bin", 64 * 1024, cancel_at_once,
                       NULL, why, sizeof why) == ND_ERR_BUSY);
    CHECK_STR(why, "Cancelled.");
    CHECK(!nd_path_is_file("/card/untrusted/big.bin"));
    CHECK(g_progress_calls > 0);
}

static void test_a_short_file_is_refused(void)
{
    fetch_conn c;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    set_body(4096u);
    ctl("short", "1000\n"); /* curl exits 0 having written 1000 of 4096 */

    conn_init(&c);
    /* Renaming this into music/ would produce a track that plays for four
     * seconds and a bug report about the phone rather than the server. */
    CHECK(api.download(&c, "music", "A Forest.mp3", "/card/music/A Forest.mp3", 4096, NULL, NULL,
                       why, sizeof why) == ND_ERR_IO);
    CHECK_STR(why, "The file arrived short.");
    CHECK(!nd_path_is_file("/card/music/A Forest.mp3"));
}

static void test_a_failed_download_explains_itself(void)
{
    fetch_conn c;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    ctl("exit", "9\n");
    ctl("stderr", "curl: (9) Server denied you access to the resource\n");

    conn_init(&c);
    CHECK(api.download(&c, "music", "A Forest.mp3", "/card/music/A Forest.mp3", 4096, NULL, NULL,
                       why, sizeof why) == ND_ERR_IO);
    /* curl's own words, with its prefix removed -- better than any table of
     * exit codes this app could carry. */
    CHECK_STR(why, "Server denied you access to the resource");
    CHECK(!nd_path_is_file("/card/music/A Forest.mp3"));
}

static void test_an_unsafe_name_never_reaches_curl(void)
{
    fetch_conn c;
    char why[ND_FETCH_WHY_MAX] = "";

    CHECK(scenario());
    CHECK(path_with_fake_curl());
    set_body(16u);
    conn_init(&c);
    CHECK(api.download(&c, "music", "../../etc/passwd", "/card/music/x", 16, NULL, NULL, why,
                       sizeof why) == ND_ERR_INVAL);
    /* Not "curl refused it" -- curl was never started. */
    CHECK_STR(readback("urls"), "");
}

int main(void)
{
    const char *p = getenv("PATH");

    if (!api_open() || !find_fixtures())
        return 1;
    (void)nd_strlcpy(g_path_keep, (p != NULL) ? p : "", sizeof g_path_keep);

    RUN(test_list_reads_what_the_server_said);
    RUN(test_a_subdirectory_is_asked_for_by_name);
    RUN(test_the_password_never_reaches_argv);
    RUN(test_tls_is_required_on_every_call);
    RUN(test_a_refused_login_says_so);
    RUN(test_a_network_failure_says_what_curl_said);
    RUN(test_no_curl_at_all);
    RUN(test_download_lands_where_it_was_asked_to);
    RUN(test_progress_is_driven_from_the_growing_file);
    RUN(test_a_cancelled_download_is_not_saved);
    RUN(test_a_short_file_is_refused);
    RUN(test_a_failed_download_explains_itself);
    RUN(test_an_unsafe_name_never_reaches_curl);

    (void)setenv("PATH", g_path_keep, 1);
    dlclose(api.h);
    return pt_report("test_fetch_app");
}
