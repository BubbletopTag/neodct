/* browser.h -- the pieces of the Browser launcher a unit test can reach.
 *
 * System/apps/Browser/main.py keeps its whole surface in module-level
 * helpers: _describe_exit, _classify, _tagged, _CpuSampler, _pump_browser_log
 * and _drain_input. Two of them (_describe_exit and _drain_input) are called
 * out in spec-apps-core.md as unit-tested and byte-exact, and the rest are
 * pure arithmetic over strings that a test can pin down without netsurf being
 * installed. So they are declared here rather than left static inside main.c,
 * and test/unit/test_browser.c dlopen()s the BUILT app.so and dlsym()s them --
 * the same arrangement test_cubebench.c uses, and for the same reason: the
 * test then exercises the artefact that ships rather than a second copy of the
 * source compiled with different flags.
 *
 * Names follow CODING-STANDARDS.md section 2 (nd_<module>_<verb>); the Python
 * name each one came from is on the declaration.
 */

#ifndef ND_BROWSER_H_INCLUDED
#define ND_BROWSER_H_INCLUDED

#include <sys/types.h>

#include "nd_input.h"
#include "nd_proc.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py's four constants. BROWSER_BIN is netsurf's FRAMEBUFFER front-end,
 * drawing straight to /dev/fb0 -- not cage, not WebKitGTK, whatever
 * ARCHITECTURE.md says. Both defconfigs set BR2_PACKAGE_NETSURF_FRAMEBUFFER=y
 * and neither has a cage or webkit package. */
#define ND_BROWSER_BIN  "/usr/bin/netsurf-fb"
#define ND_BROWSER_HOME "file:///NeoDCT/System/apps/Browser/home.html"
#define ND_BROWSER_TAG  "Browser"

/* CONSOLE in main.py. The browser's stderr goes to the serial console so
 * memory pressure can be watched from the host. */
#define ND_BROWSER_CONSOLE "/dev/console"

/* The browser's HOME. Not ND_ROOT-resolved: it is handed to another program,
 * not opened by us.
 *
 * It used to be /NeoDCT/User -- the ROOT of the whole writable partition,
 * which is where the Python put it. That was a reasonable default when
 * everything was root and there was nothing to separate; it is not one now.
 * netsurf writes its cookie jar, its URL database and its cache into HOME,
 * and /NeoDCT/User also holds the SMS databases, the ssh keys and the update
 * records. SECURITY-AUDIT.md section 2.5.
 *
 * /NeoDCT/User/browser is created by S00userdata as 0770 ndusr:ndusr_ut --
 * the one directory the untrusted set may write, and the reason the parent
 * is 0751 rather than 0750: ndusr_ut has to be able to REACH this by name
 * without being able to list what else is up there. */
#define ND_BROWSER_HOME_DIR "/NeoDCT/User/browser"

/* subprocess.run(["dmesg"], ...) is a PATH lookup; execve is not, so the
 * launcher has to name the file. Busybox and util-linux both install it
 * here. See BR-6 in OPEN-QUESTIONS.md. */
#define ND_BROWSER_DMESG "/bin/dmesg"

/* _dump_dmesg_tail(lines=15) and its timeout=5. */
#define ND_BROWSER_DMESG_LINES   15
#define ND_BROWSER_DMESG_TIMEOUT 5.0

/* The colours _classify hands back. 141 is the purple this tag owns in
 * logstyle's palette; 196 is the red every failing line in the system is
 * painted; 117 is the pale blue reserved here for a navigation. */
#define ND_BROWSER_COLOUR_PLAIN 141
#define ND_BROWSER_COLOUR_ERROR 196
#define ND_BROWSER_COLOUR_URL   117

/* Longest line the pump will assemble before it emits what it has and treats
 * the remainder as a new line. Python's readline() has no bound at all; a
 * fixed buffer is the price of not allocating per line on a 53 MB phone, and
 * netsurf's longest real line -- a certificate chain complaint carrying a URL
 * -- is well inside it. */
#define ND_BROWSER_LINE_MAX 1024

/* ------------------------------------------------------------------ *
 * _describe_exit
 * ------------------------------------------------------------------ */

/* Python's Popen.returncode convention: >= 0 is an exit status, < 0 is
 * -signal. nd_proc_status carries the two apart, so this folds them back. */
int nd_browser_returncode(const nd_proc_status *st);

/* One serial-log line describing how netsurf-fb ended. Byte for byte:
 *
 *     0   -> "neodct-browser: exited normally"
 *     > 0 -> "neodct-browser: exited with code %d"
 *     < 0 -> "neodct-browser: KILLED by signal %d"        (unknown signals)
 *            "neodct-browser: KILLED by signal 6 (SIGABRT)"
 *            "neodct-browser: KILLED by signal 9 (SIGKILL, possible OOM)"
 *            "neodct-browser: KILLED by signal 11 (SIGSEGV)"
 *
 * Returns the length it wanted, snprintf-style. */
size_t nd_browser_describe_exit(char *out, size_t out_sz, int returncode);

/* ------------------------------------------------------------------ *
 * _classify and _tagged
 * ------------------------------------------------------------------ */

/* The colour for one line of netsurf stderr. Order matters and is the
 * Python's: a "neodct-mem:" line is plain even when it contains the word
 * "error", and an _ERROR_HINTS match beats a URL, so a failing navigation is
 * red rather than blue. */
int nd_browser_classify(const char *line);

/* True when the line is one of the NeoDCT build's periodic RSS reports, i.e.
 * when its lowercase form starts "neodct-mem:". The pump asks twice, once
 * inside _classify and once to decide whether to fold the CPU figure in;
 * both are the same test. */
bool nd_browser_is_mem_line(const char *line);

/* _tagged(body, code): a bold purple "[Browser]", a space, then the body.
 * The space belongs to the body, not to the painted tag. */
size_t nd_browser_tagged(char *out, size_t out_sz, const char *body, int code);

/* _log_console(text): open the console, write text + "\r\n", close. CRLF, not
 * LF -- a serial terminal with no ONLCR needs the carriage return. Every
 * error is swallowed, exactly as the Python's bare except does. */
void nd_browser_log_console(const char *text);

/* ------------------------------------------------------------------ *
 * _CpuSampler
 * ------------------------------------------------------------------ */

/* Percent CPU for one pid, between calls. Read from /proc/<pid>/stat rather
 * than shelling out to top: netsurf is the heaviest thing this phone runs and
 * the sampler must not be part of the problem. */
typedef struct {
    pid_t pid;
    bool have_last;
    unsigned long long last_busy;
    double last_now;
} nd_browser_cpu;

void nd_browser_cpu_init(nd_browser_cpu *c, pid_t pid);

/* _CpuSampler.percent(). false is Python's None: the first call, an
 * unreadable or malformed stat file, or a non-positive interval. */
bool nd_browser_cpu_percent(nd_browser_cpu *c, double *out);

/* The same with the clock supplied. percent() is this with
 * nd_time_monotonic(); a test drives it directly so the arithmetic is
 * checkable without sleeping. */
bool nd_browser_cpu_percent_at(nd_browser_cpu *c, double now, double *out);

/* ------------------------------------------------------------------ *
 * _pump_browser_log
 * ------------------------------------------------------------------ */

/* Read `stderr_fd` to EOF, tagging every line onto `console_fd` as it goes.
 *
 * THE PIPE MUST BE DRAINED WHILE NETSURF IS ALIVE. main.py says so in as many
 * words -- "the pipe has to be drained while netsurf is alive or it fills and
 * blocks it" -- and a 64 KB pipe against a browser that logs every fetch is
 * minutes, not hours. So this is called BEFORE the wait, never after, and it
 * returns only at EOF (or when SIGTERM arrives).
 *
 * console_fd < 0 means "read and discard". The Python skipped the pump
 * entirely when /dev/console would not open, which left exactly the full pipe
 * its own comment warns about; see BR-4 in OPEN-QUESTIONS.md.
 *
 * `input` and `bridge` may both be NULL. When they are not, keypad presses
 * arriving on the inherited channel are read in the same poll and handed to
 * the bridge, which types them into the uinput keyboard netsurf reads. That
 * is the one structural difference from the Python, where the bridge owned a
 * thread and scanned the i2c expander itself -- an app process no longer
 * touches the bus. See BR-2. */
void nd_browser_pump(int stderr_fd, int console_fd, nd_browser_cpu *cpu, nd_input *input,
                     nd_t9_bridge *bridge);

/* ------------------------------------------------------------------ *
 * _drain_input and _dump_dmesg_tail
 * ------------------------------------------------------------------ */

/* Swallow every keypress queued up while the browser owned the screen, so the
 * launcher does not replay them as menu actions. Both halves of the Python:
 * the raw descriptor (select with a 0 timeout, read 4096 -- note the size,
 * which is not the 24-byte read used everywhere else) and then up to 64
 * read_key(0) calls to empty the decoder's own queue. */
void nd_browser_drain_input(nd_ui *ui);

/* After an abnormal exit, surface the kernel's view -- OOM killer reports land
 * in dmesg even with a quiet console. The last `lines` lines of dmesg's
 * stdout, each through _log_console. A dmesg that has not finished within
 * ND_BROWSER_DMESG_TIMEOUT logs NOTHING, which is what Python's
 * TimeoutExpired-into-a-bare-except does. */
void nd_browser_dump_dmesg_tail(int lines);

/* True on hardware whose only input device is the i2c keypad, i.e. where
 * netsurf cannot see a keyboard unless one is bridged for it. See BR-3. */
bool nd_browser_needs_key_bridge(const nd_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* ND_BROWSER_H_INCLUDED */
