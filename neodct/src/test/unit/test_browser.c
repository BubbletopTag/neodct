/* test_browser.c -- the Browser launcher, driven against a fake netsurf.
 *
 * ============ WHY THERE IS A FAKE netsurf ============
 *
 * /usr/bin/netsurf-fb is not installed on any build host and never will be.
 * So the launcher is pointed at a scratch root (NEODCT_ROOT, via
 * nd_path_set_root) in which /usr/bin/netsurf-fb is a shell script that writes
 * a KNOWN set of lines to stderr and exits with a KNOWN status. Everything the
 * launcher does to a real browser -- the argv it builds, the HOME it defaults,
 * the pipe it drains, the exit it describes, the dmesg it dumps -- is then
 * observable in a file, because /dev/console is a scratch file too.
 *
 * That is not a mock of the launcher. It is a mock of NETSURF; the code under
 * test is the shipped app.so, spawning a real process through the real
 * nd_proc_spawn() and reading a real pipe.
 *
 * ============ WHY IT dlopen()s THE APP ============
 *
 * apps/Browser builds to app.so and the Makefile's test rule links a test
 * against libneodct and nothing else. Recompiling main.c into this binary
 * would test a second copy of the source; dlopen()ing the built artefact tests
 * the one that ships. Same arrangement, and same reason, as test_cubebench.c.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. _describe_exit is byte-exact for 0, a code, and signals 6/9/11/31.
 *  2. _classify reproduces the Python's precedence: a memory line is plain
 *     even when it says "error"; an error hint beats a URL.
 *  3. _tagged paints the BRACKETED TAG and not the body, and the space
 *     between them is unpainted.
 *  4. The CPU sampler reads utime+stime from field 14 and 15, returns None on
 *     its first call, and divides by SC_CLK_TCK * elapsed.
 *  5. The pump tags every line onto the console, folds cpu= into the memory
 *     lines, skips blank ones, and DRAINS A PIPE BIGGER THAN THE PIPE. That
 *     last one is the whole reason the pump exists: main.py's comment says
 *     the pipe fills and blocks netsurf if nobody reads it.
 *  6. Keypad presses arriving on the inherited channel while netsurf owns the
 *     screen reach the uinput keyboard, through the BROWSER bridge -- 2 is Up,
 *     4 is Left, 6 is Right, 8 is Down, 5 follows a link.
 *  7. _drain_input empties both the raw descriptor and the decoder's queue,
 *     and tolerates a missing one.
 *  8. app_run end to end: the started line, the stderr, the exit line, and a
 *     dmesg tail after a signal death. Plus the silent return when there is
 *     no browser installed.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "platform_test.h"

#include "../../apps/Browser/browser.h"

/* ------------------------------------------------------------------ *
 * The app.so under test
 * ------------------------------------------------------------------ */

typedef struct {
    int (*app_run)(nd_ui *ui);
    void (*app_shutdown)(void);
    int (*returncode)(const nd_proc_status *st);
    size_t (*describe_exit)(char *out, size_t out_sz, int rc);
    int (*classify)(const char *line);
    bool (*is_mem_line)(const char *line);
    size_t (*tagged)(char *out, size_t out_sz, const char *body, int code);
    void (*log_console)(const char *text);
    void (*cpu_init)(nd_browser_cpu *c, pid_t pid);
    bool (*cpu_percent)(nd_browser_cpu *c, double *out);
    bool (*cpu_percent_at)(nd_browser_cpu *c, double now, double *out);
    void (*pump)(int stderr_fd, int console_fd, nd_browser_cpu *cpu, nd_input *in,
                 nd_t9_bridge *br);
    void (*drain_input)(nd_ui *ui);
    void (*dump_dmesg_tail)(int lines);
    bool (*needs_key_bridge)(const nd_ui *ui);
} browser_api;

static browser_api g_api;
static void *g_handle;
static char g_so[ND_PATH_MAX];

/* build/<variant>/test/test_browser -> build/<variant>/apps/Browser/app.so */
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
    return nd_snprintf(out, sz, "%s/../apps/Browser/app.so", exe) == ND_OK;
}

/* dlsym returns void*, and ISO C has no conversion between an object pointer
 * and a function pointer. The memcpy is the portable spelling POSIX blesses. */
static void bind_sym(void **slot, void *h, const char *name)
{
    void *p = dlsym(h, name);

    if (p == NULL) {
        fprintf(stderr, "test_browser: app.so has no symbol %s\n", name);
        exit(1);
    }
    memcpy(slot, &p, sizeof p);
}

static void load_app(void)
{
    if (!resolve_app_so(g_so, sizeof g_so)) {
        fprintf(stderr, "test_browser: cannot locate app.so\n");
        exit(1);
    }
    g_handle = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (g_handle == NULL) {
        fprintf(stderr, "test_browser: dlopen %s: %s\n", g_so, dlerror());
        exit(1);
    }
#define BIND(field, sym) bind_sym((void **)&g_api.field, g_handle, sym)
    BIND(app_run, "app_run");
    BIND(app_shutdown, "app_shutdown");
    BIND(returncode, "nd_browser_returncode");
    BIND(describe_exit, "nd_browser_describe_exit");
    BIND(classify, "nd_browser_classify");
    BIND(is_mem_line, "nd_browser_is_mem_line");
    BIND(tagged, "nd_browser_tagged");
    BIND(log_console, "nd_browser_log_console");
    BIND(cpu_init, "nd_browser_cpu_init");
    BIND(cpu_percent, "nd_browser_cpu_percent");
    BIND(cpu_percent_at, "nd_browser_cpu_percent_at");
    BIND(pump, "nd_browser_pump");
    BIND(drain_input, "nd_browser_drain_input");
    BIND(dump_dmesg_tail, "nd_browser_dump_dmesg_tail");
    BIND(needs_key_bridge, "nd_browser_needs_key_bridge");
#undef BIND
}

/* ------------------------------------------------------------------ *
 * Fixture helpers
 * ------------------------------------------------------------------ */

/* A big console: the drain-the-pipe case writes three thousand tagged lines
 * through it. Heap, not stack -- 512 KB is not a stack frame. */
#define CONSOLE_CAP (1024u * 1024u)

static char *g_console_buf;

static void write_exec(const char *path, const char *text)
{
    char resolved[ND_PATH_MAX];

    pt_write_text(path, text);
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        fprintf(stderr, "resolve %s failed\n", path);
        exit(1);
    }
    if (chmod(resolved, 0755) != 0) {
        fprintf(stderr, "chmod %s: %s\n", resolved, strerror(errno));
        exit(1);
    }
}

/* /dev/console and /dev/null as ordinary files under the case root. The
 * launcher opens both by their real absolute names; nd_path_resolve is what
 * puts them here instead. */
static void make_devices(void)
{
    pt_write_text("/dev/console", "");
    pt_write_text("/dev/null", "");
}

static const char *console_text(void)
{
    size_t n = pt_read_text("/dev/console", g_console_buf, CONSOLE_CAP);

    if (n == (size_t)-1) {
        g_console_buf[0] = '\0';
        return g_console_buf;
    }
    return g_console_buf;
}

static void check_has(const char *hay, const char *needle)
{
    g_checks++;
    if (hay == NULL || strstr(hay, needle) == NULL) {
        g_failures++;
        fprintf(stderr, "FAIL console does not contain \"%s\"\n", needle);
    }
}

static void check_lacks(const char *hay, const char *needle)
{
    g_checks++;
    if (hay != NULL && strstr(hay, needle) != NULL) {
        g_failures++;
        fprintf(stderr, "FAIL console unexpectedly contains \"%s\"\n", needle);
    }
}

static nd_ui g_ui;

static nd_ui *bare_ui(void)
{
    /* Not nd_ui_init_app(): app_run touches exactly four members and building
     * a real context would drag in fonts, a framebuffer and a canvas none of
     * this exercises. */
    memset(&g_ui, 0, sizeof g_ui);
    g_ui.keypad_fd = -1;
    return &g_ui;
}

/* ------------------------------------------------------------------ *
 * 1. _describe_exit
 * ------------------------------------------------------------------ */

static void check_exit(int rc, const char *want)
{
    char buf[160];
    size_t n = g_api.describe_exit(buf, sizeof buf, rc);

    CHECK_STR(buf, want);
    CHECK_INT(n, strlen(want));
}

static void t_describe_exit(void)
{
    nd_proc_status st;

    check_exit(0, "neodct-browser: exited normally");
    check_exit(1, "neodct-browser: exited with code 1");
    check_exit(127, "neodct-browser: exited with code 127");
    check_exit(-6, "neodct-browser: KILLED by signal 6 (SIGABRT)");
    check_exit(-9, "neodct-browser: KILLED by signal 9 (SIGKILL, possible OOM)");
    check_exit(-11, "neodct-browser: KILLED by signal 11 (SIGSEGV)");
    /* Not in _SIGNAL_NOTES: the bare form, no parenthesis. */
    check_exit(-31, "neodct-browser: KILLED by signal 31");
    check_exit(-15, "neodct-browser: KILLED by signal 15");

    /* Popen.returncode, rebuilt from waitpid's two halves. */
    memset(&st, 0, sizeof st);
    st.exited = true;
    st.exit_status = 0;
    CHECK_INT(g_api.returncode(&st), 0);
    st.exit_status = 7;
    CHECK_INT(g_api.returncode(&st), 7);
    memset(&st, 0, sizeof st);
    st.signalled = true;
    st.signo = 11;
    CHECK_INT(g_api.returncode(&st), -11);
    CHECK_INT(g_api.returncode(NULL), 0);
}

/* ------------------------------------------------------------------ *
 * 2. _classify
 * ------------------------------------------------------------------ */

static void t_classify(void)
{
    static const char *const hints[] = {"ssl",    "tls",    "certificate", "handshake", "verify",
                                        "error",  "failed", "cannot",      "refused",   "timed out",
                                        "unable", "denied", "abort"};
    size_t i;

    /* Ordinary chatter. */
    CHECK_INT(g_api.classify("nsfb: using framebuffer surface"), ND_BROWSER_COLOUR_PLAIN);
    CHECK_INT(g_api.classify(""), ND_BROWSER_COLOUR_PLAIN);
    CHECK_INT(g_api.classify(NULL), ND_BROWSER_COLOUR_PLAIN);

    /* Every hint, and every hint uppercased -- the Python lowercases first. */
    for (i = 0u; i < ND_ARRAY_LEN(hints); i++) {
        char line[128];
        char upper[128];
        size_t k;

        (void)nd_snprintf(line, sizeof line, "netsurf: %s while fetching", hints[i]);
        CHECK_INT(g_api.classify(line), ND_BROWSER_COLOUR_ERROR);
        (void)nd_strlcpy(upper, line, sizeof upper);
        for (k = 0u; upper[k] != '\0'; k++)
            upper[k] = (char)((upper[k] >= 'a' && upper[k] <= 'z') ? upper[k] - 32 : upper[k]);
        CHECK_INT(g_api.classify(upper), ND_BROWSER_COLOUR_ERROR);
    }

    /* A navigation. */
    CHECK_INT(g_api.classify("fetch http://example.com/"), ND_BROWSER_COLOUR_URL);
    CHECK_INT(g_api.classify("fetch https://example.com/"), ND_BROWSER_COLOUR_URL);
    CHECK_INT(g_api.classify("FETCH HTTPS://EXAMPLE.COM/"), ND_BROWSER_COLOUR_URL);

    /* Precedence, both ways round. An error hint beats a URL... */
    CHECK_INT(g_api.classify("https://example.com/ handshake failed"), ND_BROWSER_COLOUR_ERROR);
    /* ...and the memory line beats everything, even the word "error". */
    CHECK_INT(g_api.classify("neodct-mem: rss=20480kB error"), ND_BROWSER_COLOUR_PLAIN);
    CHECK_INT(g_api.classify("NEODCT-MEM: rss=20480kB"), ND_BROWSER_COLOUR_PLAIN);
    /* startswith, not "contains": a memory line mentioned mid-sentence is
     * ordinary text and the word "unable" in it still makes it red. */
    CHECK_INT(g_api.classify("saw neodct-mem: unable"), ND_BROWSER_COLOUR_ERROR);

    CHECK(g_api.is_mem_line("neodct-mem: rss=1kB"));
    CHECK(g_api.is_mem_line("NeoDCT-Mem: rss=1kB"));
    CHECK(!g_api.is_mem_line(" neodct-mem: rss=1kB"));
    CHECK(!g_api.is_mem_line("mem: rss=1kB"));
    CHECK(!g_api.is_mem_line(NULL));
}

/* ------------------------------------------------------------------ *
 * 3. _tagged
 * ------------------------------------------------------------------ */

static void t_tagged(void)
{
    char buf[256];
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(true);
    (void)g_api.tagged(buf, sizeof buf, "hello", 141);
    /* The bracketed tag is bold and coloured; the space and the body are not
     * inside the escape. */
    CHECK_STR(buf, "\033[1m\033[38;5;141m[Browser]\033[0m hello");
    (void)g_api.tagged(buf, sizeof buf, "boom", ND_BROWSER_COLOUR_ERROR);
    CHECK_STR(buf, "\033[1m\033[38;5;196m[Browser]\033[0m boom");

    nd_log_set_colour(false);
    (void)g_api.tagged(buf, sizeof buf, "hello", 141);
    CHECK_STR(buf, "[Browser] hello");
    (void)g_api.tagged(buf, sizeof buf, NULL, 141);
    CHECK_STR(buf, "[Browser] ");

    nd_log_set_colour(was);

    /* 141 is the colour the shared palette already assigns this tag, so the
     * launcher's default and nd_log's agree without either knowing about the
     * other. */
    CHECK_INT(nd_log_colour_for(ND_LOG_BROWSER), ND_BROWSER_COLOUR_PLAIN);
}

/* ------------------------------------------------------------------ *
 * 4. _log_console
 * ------------------------------------------------------------------ */

static void t_log_console(void)
{
    char buf[128];
    size_t n;

    make_devices();
    g_api.log_console("first");
    g_api.log_console("second");
    n = pt_read_text("/dev/console", buf, sizeof buf);
    CHECK_INT(n, 15);
    /* CRLF, not LF: a serial terminal with no ONLCR needs the return. */
    CHECK_STR(buf, "first\r\nsecond\r\n");

    /* No console, no crash and no complaint. */
    pt_new_case();
    g_api.log_console("nowhere");
    g_api.log_console(NULL);
    CHECK(true);
}

/* ------------------------------------------------------------------ *
 * 5. _CpuSampler
 * ------------------------------------------------------------------ */

/* A /proc/<pid>/stat line in the real shape: pid, (comm), state, then the
 * numeric fields. utime and stime are the 14th and 15th, i.e. index 13 and
 * 14 of the whitespace split -- which is what the Python indexes and what
 * BR-1 records as the preserved bug. */
static void write_stat(pid_t pid, unsigned long utime, unsigned long stime)
{
    char path[64];
    char line[512];

    (void)nd_snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    (void)nd_snprintf(line, sizeof line,
                      "%ld (netsurf-fb) S 1 %ld %ld 0 -1 4194560 900 0 3 0 %lu %lu 0 0 20 0 5 0 "
                      "1234 100000000 2000\n",
                      (long)pid, (long)pid, (long)pid, utime, stime);
    pt_write_text(path, line);
}

static void t_cpu_sampler(void)
{
    nd_browser_cpu c;
    double pct = -1.0;
    double tck = (double)sysconf(_SC_CLK_TCK);

    write_stat(4242, 100u, 50u);
    g_api.cpu_init(&c, 4242);
    CHECK_INT(c.pid, 4242);

    /* First call is Python's None: there is nothing to subtract from yet. */
    CHECK(!g_api.cpu_percent_at(&c, 1000.0, &pct));
    CHECK(c.have_last);
    CHECK_INT(c.last_busy, 150);

    /* Busy goes up by exactly one second of ticks over one second: 100%. */
    write_stat(4242, 100u + (unsigned long)tck, 50u);
    CHECK(g_api.cpu_percent_at(&c, 1001.0, &pct));
    CHECK(pct > 99.999 && pct < 100.001);

    /* Half a second of ticks over two seconds: 25%. */
    write_stat(4242, 100u + (unsigned long)tck, 50u + (unsigned long)(tck / 2));
    CHECK(g_api.cpu_percent_at(&c, 1003.0, &pct));
    CHECK(pct > 24.99 && pct < 25.01);

    /* elapsed <= 0 is None, and the sample is still recorded. */
    CHECK(!g_api.cpu_percent_at(&c, 1003.0, &pct));

    /* A vanished process is None, not a crash. */
    g_api.cpu_init(&c, 999999);
    CHECK(!g_api.cpu_percent_at(&c, 1.0, &pct));
    CHECK(!g_api.cpu_percent_at(&c, 2.0, &pct));

    /* A malformed stat file is Python's ValueError -> None. */
    pt_write_text("/proc/4243/stat", "4243 (x) S not a number\n");
    g_api.cpu_init(&c, 4243);
    CHECK(!g_api.cpu_percent_at(&c, 1.0, &pct));

    /* Too few fields is Python's IndexError -> None. */
    pt_write_text("/proc/4244/stat", "4244 (x) S 1 2 3\n");
    g_api.cpu_init(&c, 4244);
    CHECK(!g_api.cpu_percent_at(&c, 1.0, &pct));

    /* The wrapper reads the real monotonic clock and agrees on None-ness. */
    write_stat(4245, 1u, 1u);
    g_api.cpu_init(&c, 4245);
    CHECK(!g_api.cpu_percent(&c, &pct));

    CHECK(!g_api.cpu_percent_at(NULL, 1.0, &pct));
    CHECK(!g_api.cpu_percent_at(&c, 1.0, NULL));
}

/* ------------------------------------------------------------------ *
 * 6. the pump
 * ------------------------------------------------------------------ */

/* Feed `text` down a pipe, close it, and let the pump run to EOF. */
static void pump_text(const char *text, nd_browser_cpu *cpu)
{
    int fds[2];
    char cpath[ND_PATH_MAX];
    int console;

    CHECK_INT(pipe(fds), 0);
    CHECK(nd_path_resolve(cpath, sizeof cpath, "/dev/console") == ND_OK);
    console = open(cpath, O_WRONLY | O_APPEND);
    CHECK(console >= 0);

    CHECK_INT((int)write(fds[1], text, strlen(text)), (int)strlen(text));
    (void)close(fds[1]);

    g_api.pump(fds[0], console, cpu, NULL, NULL);

    (void)close(fds[0]);
    (void)close(console);
}

static void t_pump_lines(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();

    pump_text("nsfb starting\n"
              "\n"
              "   \t \n"
              "fetch https://example.com/\r\n"
              "SSL handshake failed\n"
              "a last line with no newline",
              NULL);

    out = console_text();
    CHECK_STR(out, "[Browser] nsfb starting\r\n"
                   "[Browser] fetch https://example.com/\r\n"
                   "[Browser] SSL handshake failed\r\n"
                   "[Browser] a last line with no newline\r\n");
    nd_log_set_colour(was);
}

static void t_pump_colours(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(true);
    make_devices();

    pump_text("plain\nfetch https://example.com/\nSSL verify failed\n", NULL);

    out = console_text();
    check_has(out, "\033[1m\033[38;5;141m[Browser]\033[0m plain\r\n");
    check_has(out, "\033[1m\033[38;5;117m[Browser]\033[0m fetch https://example.com/\r\n");
    check_has(out, "\033[1m\033[38;5;196m[Browser]\033[0m SSL verify failed\r\n");
    nd_log_set_colour(was);
}

static void t_pump_memory_cpu(void)
{
    nd_browser_cpu c;
    const char *out;
    double tck = (double)sysconf(_SC_CLK_TCK);
    double warm = 0.0;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();

    /* The sampler is warmed by hand so the first memory line already has a
     * reading -- percent()'s first call is always None, whoever makes it. */
    write_stat(4300, 0u, 0u);
    g_api.cpu_init(&c, 4300);
    CHECK(!g_api.cpu_percent(&c, &warm));
    write_stat(4300, (unsigned long)(tck * 1000.0), 0u);

    pump_text("neodct-mem: rss=20480kB\nplain line\n", &c);

    out = console_text();
    /* The exact percentage depends on the real elapsed monotonic time between
     * the two reads, which is microseconds -- so it is enormous and unstable.
     * What is being claimed is that the suffix is THERE and that it is only
     * on the memory line. */
    check_has(out, "[Browser] neodct-mem: rss=20480kB cpu=");
    check_has(out, "[Browser] plain line\r\n");
    check_lacks(out, "plain line cpu=");
    nd_log_set_colour(was);
}

/* THE claim. A pipe is 64 KB; three thousand lines is roughly 200 KB. If the
 * pump were not draining while the writer runs, the writer would block
 * forever and this test would hang -- which is exactly what main.py's comment
 * warns happens if you call wait() before reading. */
static void t_pump_drains_more_than_a_pipe(void)
{
    char script[512];
    const char *out;
    int rc;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();
    (void)nd_snprintf(script, sizeof script,
                      "#!/bin/sh\n"
                      "i=0\n"
                      "while [ $i -lt 3000 ]; do\n"
                      "  echo \"chatter $i padding padding padding padding padding\" >&2\n"
                      "  i=$((i+1))\n"
                      "done\n"
                      "exit 0\n");
    write_exec(ND_BROWSER_BIN, script);

    rc = g_api.app_run(bare_ui());
    CHECK_INT(rc, 0);

    out = console_text();
    check_has(out, "[Browser] chatter 0 padding");
    check_has(out, "[Browser] chatter 2999 padding");
    check_has(out, "[Browser] neodct-browser: exited normally\r\n");
    nd_log_set_colour(was);
}

/* ------------------------------------------------------------------ *
 * 7. the browser key bridge
 * ------------------------------------------------------------------ */

#define EV_KEY_T 0x01

typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} ev_native;

/* Collect the key-down codes the bridge typed into the uinput descriptor. */
static size_t read_downs(int fd, uint16_t *out, size_t max)
{
    uint8_t buf[8192];
    ssize_t got = read(fd, buf, sizeof buf);
    size_t off = 0u;
    size_t n = 0u;

    if (got <= 0)
        return 0u;
    while (off + sizeof(ev_native) <= (size_t)got) {
        ev_native ev;

        memcpy(&ev, buf + off, sizeof ev);
        off += sizeof ev;
        if (ev.type == EV_KEY_T && ev.value == 1 && n < max)
            out[n++] = ev.code;
    }
    return n;
}

static void t_key_bridge_through_pump(void)
{
    nd_input_channel ch;
    nd_input *input = NULL;
    nd_uinput_kbd kbd;
    nd_t9_bridge *bridge;
    int kbd_fds[2];
    int err_fds[2];
    uint16_t downs[16];
    size_t n;
    int flags;

    memset(&ch, 0, sizeof ch);
    CHECK_INT(nd_input_channel_open(&ch), ND_OK);
    CHECK_INT(nd_input_open_pipe(&input, ch.read_fd), ND_OK);

    CHECK_INT(pipe(kbd_fds), 0);
    flags = fcntl(kbd_fds[0], F_GETFL, 0);
    CHECK_INT(fcntl(kbd_fds[0], F_SETFL, flags | O_NONBLOCK), 0);
    CHECK_INT(nd_uinput_attach(&kbd, kbd_fds[1]), ND_OK);

    bridge = nd_t9_bridge_new_for_test(ND_BRIDGE_BROWSER, &kbd);
    CHECK(bridge != NULL);
    /* The browser starts as a d-pad, which is the whole reason it has its own
     * bridge: the enrolment wizard collects no Left and no Right at all. */
    CHECK_STR(nd_t9_bridge_mode_label(bridge), "nav");

    /* Queue the presses BEFORE the pump runs, then close netsurf's stderr so
     * the pump reaches EOF and returns. Both descriptors are ready in the
     * same poll, which is the situation the launcher is built for. */
    CHECK_INT(nd_input_channel_send(&ch, 3, true), ND_OK); /* keypad 2 -> Up    */
    CHECK_INT(nd_input_channel_send(&ch, 3, false), ND_OK);
    CHECK_INT(nd_input_channel_send(&ch, 5, true), ND_OK); /* keypad 4 -> Left  */
    CHECK_INT(nd_input_channel_send(&ch, 5, false), ND_OK);
    CHECK_INT(nd_input_channel_send(&ch, 7, true), ND_OK); /* keypad 6 -> Right */
    CHECK_INT(nd_input_channel_send(&ch, 7, false), ND_OK);
    CHECK_INT(nd_input_channel_send(&ch, 9, true), ND_OK); /* keypad 8 -> Down  */
    CHECK_INT(nd_input_channel_send(&ch, 9, false), ND_OK);
    CHECK_INT(nd_input_channel_send(&ch, 6, true), ND_OK); /* keypad 5 -> Enter */
    CHECK_INT(nd_input_channel_send(&ch, 6, false), ND_OK);

    CHECK_INT(pipe(err_fds), 0);
    CHECK_INT((int)write(err_fds[1], "nsfb ready\n", 11u), 11);
    (void)close(err_fds[1]);

    g_api.pump(err_fds[0], -1, NULL, input, bridge);
    (void)close(err_fds[0]);

    n = read_downs(kbd_fds[0], downs, ND_ARRAY_LEN(downs));
    CHECK_INT(n, 5);
    if (n == 5u) {
        CHECK_INT(downs[0], 103); /* KEY_UP     */
        CHECK_INT(downs[1], 105); /* KEY_LEFT   */
        CHECK_INT(downs[2], 106); /* KEY_RIGHT  */
        CHECK_INT(downs[3], 108); /* KEY_DOWN   */
        CHECK_INT(downs[4], 28);  /* KEY_ENTER  */
    }

    nd_t9_bridge_free_for_test(bridge);
    nd_uinput_close(&kbd); /* closes kbd_fds[1] */
    (void)close(kbd_fds[0]);
    nd_input_channel_close_write(&ch);
    nd_input_close(input); /* owns ch.read_fd */
}

/* With no bridge the presses are still consumed, so the channel cannot fill
 * behind a browsing session. */
static void t_pump_consumes_keys_without_a_bridge(void)
{
    nd_input_channel ch;
    nd_input *input = NULL;
    int err_fds[2];
    int i;

    memset(&ch, 0, sizeof ch);
    CHECK_INT(nd_input_channel_open(&ch), ND_OK);
    CHECK_INT(nd_input_open_pipe(&input, ch.read_fd), ND_OK);

    for (i = 0; i < 20; i++) {
        CHECK_INT(nd_input_channel_send(&ch, ND_KEY_DOWN, true), ND_OK);
        CHECK_INT(nd_input_channel_send(&ch, ND_KEY_DOWN, false), ND_OK);
    }

    /* Close the core's end first, so the channel reports POLLHUP for the rest
     * of the run. POLLHUP is level-triggered: a pump that did not notice would
     * spin at 100% of a core for the whole browsing session, which is the one
     * thing a launcher sitting beside netsurf must not do. Reaching the end of
     * this test at all is the claim. */
    nd_input_channel_close_write(&ch);

    CHECK_INT(pipe(err_fds), 0);
    CHECK_INT((int)write(err_fds[1], "x\n", 2u), 2);
    (void)close(err_fds[1]);
    g_api.pump(err_fds[0], -1, NULL, input, NULL);
    (void)close(err_fds[0]);

    /* Nothing is left for the launcher to replay. */
    CHECK_INT(nd_input_read_key(input, 0.0), ND_KEY_NONE);

    nd_input_close(input);
}

/* ------------------------------------------------------------------ *
 * 8. _drain_input
 * ------------------------------------------------------------------ */

static void t_drain_input(void)
{
    nd_input_channel ch;
    nd_input *input = NULL;
    nd_ui *ui = bare_ui();
    int i;

    /* No fd and no decoder: the Python's getattr(..., None) path. */
    g_api.drain_input(ui);
    g_api.drain_input(NULL);
    CHECK(true);

    memset(&ch, 0, sizeof ch);
    CHECK_INT(nd_input_channel_open(&ch), ND_OK);
    CHECK_INT(nd_input_open_pipe(&input, ch.read_fd), ND_OK);

    for (i = 0; i < 30; i++) {
        CHECK_INT(nd_input_channel_send(&ch, ND_KEY_ENTER, true), ND_OK);
        CHECK_INT(nd_input_channel_send(&ch, ND_KEY_ENTER, false), ND_OK);
    }

    ui->keypad_fd = ch.read_fd;
    ui->input = input;
    g_api.drain_input(ui);

    CHECK_INT(nd_input_read_key(input, 0.0), ND_KEY_NONE);

    /* A closed descriptor is Python's OSError -> pass. */
    ui->input = NULL;
    ui->keypad_fd = 9999;
    g_api.drain_input(ui);
    CHECK(true);

    ui->keypad_fd = -1;
    nd_input_channel_close_write(&ch);
    nd_input_close(input);
    memset(ui, 0, sizeof *ui);
    ui->keypad_fd = -1;
}

/* ------------------------------------------------------------------ *
 * 9. _dump_dmesg_tail
 * ------------------------------------------------------------------ */

static void t_dmesg_tail(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();

    /* Twenty lines out, fifteen in. */
    write_exec("/bin/dmesg", "#!/bin/sh\n"
                             "i=1\n"
                             "while [ $i -le 20 ]; do echo \"kmsg $i\"; i=$((i+1)); done\n");

    g_api.dump_dmesg_tail(ND_BROWSER_DMESG_LINES);

    out = console_text();
    check_lacks(out, "kmsg 5\r\n");
    check_has(out, "kmsg 6\r\n");
    check_has(out, "kmsg 20\r\n");
    /* Untagged: _dump_dmesg_tail calls _log_console directly, so the kernel's
     * own text reaches the console verbatim. */
    check_lacks(out, "[Browser] kmsg");

    /* No dmesg on the system is not an error. */
    pt_new_case();
    make_devices();
    g_api.dump_dmesg_tail(ND_BROWSER_DMESG_LINES);
    CHECK_STR(console_text(), "");

    nd_log_set_colour(was);
}

/* subprocess.run(timeout=5) kills the child and raises; the bare except then
 * discards everything read so far. Costs five seconds of wall clock and is
 * worth it -- an unbounded read here would hang the phone at the exact moment
 * it has already crashed once. */
static void t_dmesg_timeout(void)
{
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();
    write_exec("/bin/dmesg", "#!/bin/sh\necho \"kmsg early\"\nsleep 30\n");

    g_api.dump_dmesg_tail(ND_BROWSER_DMESG_LINES);

    CHECK_STR(console_text(), "");
    nd_log_set_colour(was);
}

/* ------------------------------------------------------------------ *
 * 10. _start_key_bridge's gate
 * ------------------------------------------------------------------ */

static void t_needs_key_bridge(void)
{
    nd_ui *ui = bare_ui();

    /* QEMU and every dev board: no keymap.json, so netsurf already has a real
     * keyboard and bridging one would double every press. */
    CHECK(!g_api.needs_key_bridge(ui));
    CHECK(!g_api.needs_key_bridge(NULL));

    /* Whenever the framework can answer, it wins. */
    ui->has_matrix_keypad = true;
    CHECK(g_api.needs_key_bridge(ui));
    ui->has_matrix_keypad = false;

    /* Keypad-only hardware, recognised the same way the core recognises it. */
    pt_write_text(ND_PATH_KEYMAP, "{\n"
                                  "  \"format\": \"neodct.keymap.v3.matrix.i2c\",\n"
                                  "  \"driver\": \"pcf8575-i2c\",\n"
                                  "  \"i2c_bus\": 3,\n"
                                  "  \"i2c_addr\": 32,\n"
                                  "  \"row_pins\": [0, 1, 2, 3],\n"
                                  "  \"col_pins\": [4, 5, 6, 7],\n"
                                  "  \"keys\": {\n"
                                  "    \"navikey\": {\"row\": 0, \"col\": 0},\n"
                                  "    \"num_2\": {\"row\": 0, \"col\": 1}\n"
                                  "  }\n"
                                  "}\n");
    CHECK(g_api.needs_key_bridge(ui));
}

/* ------------------------------------------------------------------ *
 * 11. run(ui), end to end
 * ------------------------------------------------------------------ */

static void t_run_missing_browser(void)
{
    make_devices();
    /* `if not os.path.exists(browser): return` -- silently, no screen, and
     * nothing on the console at all. */
    CHECK_INT(g_api.app_run(bare_ui()), 0);
    CHECK_STR(console_text(), "");
}

static void t_run_normal_exit(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();
    write_exec(ND_BROWSER_BIN, "#!/bin/sh\n"
                               "echo \"argv1=$1\" >&2\n"
                               "echo \"home=$HOME\" >&2\n"
                               "echo \"fetch https://example.com/\" >&2\n"
                               "echo \"neodct-mem: rss=20480kB\" >&2\n"
                               "exit 0\n");

    unsetenv("HOME");
    CHECK_INT(g_api.app_run(bare_ui()), 0);

    out = console_text();
    check_has(out, "[Browser] neodct-browser: started pid ");
    /* The home page, exactly as spelled in main.py -- a file: URL into the
     * read-only system image. */
    check_has(out, "[Browser] argv1=" ND_BROWSER_HOME "\r\n");
    /* env.setdefault("HOME", "/NeoDCT/User"): the browser writes its cache
     * and cookies onto the only writable partition. */
    check_has(out, "[Browser] home=" ND_BROWSER_HOME_DIR "\r\n");
    check_has(out, "[Browser] fetch https://example.com/\r\n");
    check_has(out, "[Browser] neodct-mem: rss=20480kB");
    check_has(out, "[Browser] neodct-browser: exited normally\r\n");
    nd_log_set_colour(was);
}

static void t_run_keeps_existing_home(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();
    write_exec(ND_BROWSER_BIN, "#!/bin/sh\necho \"home=$HOME\" >&2\nexit 0\n");

    /* setdefault, not assignment: an inherited HOME survives. */
    CHECK_INT(setenv("HOME", "/tmp/somewhere-else", 1), 0);
    CHECK_INT(g_api.app_run(bare_ui()), 0);
    out = console_text();
    check_has(out, "[Browser] home=/tmp/somewhere-else\r\n");
    unsetenv("HOME");
    nd_log_set_colour(was);
}

static void t_run_nonzero_exit(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();
    write_exec(ND_BROWSER_BIN, "#!/bin/sh\necho 'cannot open display' >&2\nexit 3\n");

    CHECK_INT(g_api.app_run(bare_ui()), 0);

    out = console_text();
    check_has(out, "[Browser] cannot open display\r\n");
    check_has(out, "[Browser] neodct-browser: exited with code 3\r\n");
    /* A positive status is not a signal, so no dmesg is dumped. */
    check_lacks(out, "kmsg");
    nd_log_set_colour(was);
}

static void t_run_killed_dumps_dmesg(void)
{
    const char *out;
    bool was = nd_log_colour_enabled();

    nd_log_set_colour(false);
    make_devices();
    write_exec(ND_BROWSER_BIN, "#!/bin/sh\necho 'about to die' >&2\nkill -SEGV $$\n");
    write_exec("/bin/dmesg", "#!/bin/sh\necho 'Out of memory: Killed process 42 (netsurf-fb)'\n");

    CHECK_INT(g_api.app_run(bare_ui()), 0);

    out = console_text();
    check_has(out, "[Browser] about to die\r\n");
    check_has(out, "[Browser] neodct-browser: KILLED by signal 11 (SIGSEGV)\r\n");
    /* The OOM killer's report lands in dmesg even with a quiet console, which
     * is the entire reason this path exists. */
    check_has(out, "Out of memory: Killed process 42 (netsurf-fb)\r\n");
    nd_log_set_colour(was);
}

/* A console that will not open must not stop the browser, and -- unlike the
 * Python, which skipped the pump entirely in this case -- must not leave the
 * pipe unread either. BR-4. */
static void t_run_without_a_console(void)
{
    write_exec(ND_BROWSER_BIN, "#!/bin/sh\n"
                               "i=0\n"
                               "while [ $i -lt 3000 ]; do\n"
                               "  echo \"chatter $i padding padding padding padding\" >&2\n"
                               "  i=$((i+1))\n"
                               "done\n"
                               "exit 0\n");
    pt_write_text("/dev/null", "");
    /* No /dev/console in this case root at all. */
    CHECK(!nd_path_exists("/dev/console"));
    CHECK_INT(g_api.app_run(bare_ui()), 0);
    CHECK(true); /* reaching here at all is the claim: it did not deadlock */
}

/* app_shutdown() with no child is a no-op, and is safe to call twice --
 * nd-apprun calls it on every exit path. */
static void t_shutdown_idempotent(void)
{
    g_api.app_shutdown();
    g_api.app_shutdown();
    CHECK(true);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    /* nd_input_channel_send writes to a pipe whose reader may have gone; the
     * default disposition of SIGPIPE would kill this process before write()
     * could return EPIPE. nd_input.h requires every caller to do this. */
    (void)signal(SIGPIPE, SIG_IGN);

    g_console_buf = malloc(CONSOLE_CAP);
    if (g_console_buf == NULL) {
        fprintf(stderr, "test_browser: out of memory\n");
        return 1;
    }

    load_app();

    RUN(t_describe_exit);
    RUN(t_classify);
    RUN(t_tagged);
    RUN(t_log_console);
    RUN(t_cpu_sampler);
    RUN(t_pump_lines);
    RUN(t_pump_colours);
    RUN(t_pump_memory_cpu);
    RUN(t_pump_drains_more_than_a_pipe);
    RUN(t_key_bridge_through_pump);
    RUN(t_pump_consumes_keys_without_a_bridge);
    RUN(t_drain_input);
    RUN(t_dmesg_tail);
    RUN(t_dmesg_timeout);
    RUN(t_needs_key_bridge);
    RUN(t_run_missing_browser);
    RUN(t_run_normal_exit);
    RUN(t_run_keeps_existing_home);
    RUN(t_run_nonzero_exit);
    RUN(t_run_killed_dumps_dmesg);
    RUN(t_run_without_a_console);
    RUN(t_shutdown_idempotent);

    free(g_console_buf);
    if (g_handle != NULL)
        (void)dlclose(g_handle);
    return pt_report("test_browser");
}
