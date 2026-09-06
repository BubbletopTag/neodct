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

/* Where the browser starts, overridable FOR TESTING ONLY.
 *
 * The browser's address bar takes phone-keypad multi-tap, which no automated
 * harness can drive, so there was no way to point netsurf at a chosen URL and
 * watch what it did. That made the one question worth asking about an
 * untrusted renderer -- "what happens when it reaches for file:///" --
 * answerable only by reading code.
 *
 * This is the same kind of seam as the modem's /tmp/neodct_sim_* hooks: it
 * changes nothing about what netsurf is ALLOWED to do, only which URL it is
 * handed first. The confinement is applied by the core before this app runs
 * (nd_proc.h's UNTRUSTED_APPS), so a URL chosen here is fetched as ndusr_ut,
 * inside the mount namespace, with the hidden paths blanked -- exactly as a
 * URL typed by a user would be.
 *
 * Setting it needs write access to the core's environment, which is root, so
 * it grants nothing to anybody who does not already have everything. */
#define ND_BROWSER_HOME_ENV "NEODCT_BROWSER_URL"
#define ND_BROWSER_TAG  "Browser"

/* libnsfb's own "use exactly this evdev node" override (libnsfb
 * src/surface/linux.c). Nothing in this tree set it before, so netsurf always
 * scanned /dev/input/event0..31 and took the first eight EV_KEY devices --
 * once, at init, with no rescan and no retry. On the phone that scan races
 * udev applying group `input` to the node the core has just made; on a dev
 * box it also means netsurf is reading the developer's real keyboard. Naming
 * the node closes both. */
#define ND_BROWSER_NSFB_DEV_ENV "NSFB_INPUT_DEV"

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

/* What the browser is given an empty view of. SECURITY-PLAN.md section 2.
 *
 * DAC cannot help with any of these, which is the whole reason the list
 * exists: /NeoDCT/System has to be readable by every app, so
 * `file:///NeoDCT/System/...` enumerates the entire system tree no matter
 * who netsurf runs as -- SECURITY-AUDIT.md finding 5. A mount namespace is
 * the answer because it removes a path from EXISTENCE rather than denying
 * access to it: no `..`, no symlink and no /proc/self/root back out.
 *
 * Every entry is somewhere netsurf provably never reads, which is what makes
 * turning this on safe without a phone to try it on:
 *
 *   System/engineering  LinuxShell, raw-AT Modem, RemoteShell and Downgrade
 *                       -- the four apps a compromised browser would most
 *                       like to find, and today `file:///` lists them
 *   System/keys         the release public key
 *   System/tones,       the owner's media, and the wallpaper the phone is
 *   System/wallpapers   currently showing
 *   User/db             contacts, messages, the call log
 *   User/.remote        the ssh keys
 *   User/.ndsys         the update records
 *   User/.seedrng       the entropy seed
 *
 * The four under User/ are already denied by mode bits. They are here anyway
 * because the two mechanisms fail differently -- a mode bit is one chmod
 * away from being wrong, and a path that is not there is not.
 *
 * NOT here, and the reason section 2 is not finished: the minimal /dev.
 * netsurf scans /dev/input/event0..31 and takes every keyboard it finds
 * (libnsfb src/surface/linux.c), so on QEMU it reads the real one -- every
 * keypress on the machine, whichever window has focus. On the phone
 * /dev/input holds only the synthetic bridge the core makes, which is why
 * the plan calls the emulator the more dangerous configuration here. Fixing
 * it means giving the child a /dev containing that bridge and nothing else,
 * and the bridge's own event node has to be discovered from uinput at
 * runtime -- work that cannot be validated without booting.
 */
#define ND_BROWSER_HIDE_PATHS                                                   \
    {                                                                           \
        "/NeoDCT/System/engineering", "/NeoDCT/System/keys",                    \
            "/NeoDCT/System/tones", "/NeoDCT/System/wallpapers",                \
            "/NeoDCT/User/db", "/NeoDCT/User/.remote", "/NeoDCT/User/.ndsys",   \
            "/NeoDCT/User/.seedrng", NULL                                       \
    }

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

/* _log_console(text): write text + "\r\n" to THIS PROCESS'S STDERR. CRLF, not
 * LF -- a serial terminal with no ONLCR needs the carriage return. Every error
 * is swallowed, exactly as the Python's bare except does.
 *
 * ============ IT IS NOT open("/dev/console") ANY MORE ============
 *
 * The Python opened CONSOLE = "/dev/console" per call, and the port carried
 * that over along with an ND_BROWSER_CONSOLE macro naming the path. It could
 * never have worked: /dev/console is created by devtmpfs root:root 0600 and
 * nothing raises it -- eudev's stock rules match ptmx, tty, tty[0-9]*, vcs*
 * and ttyA-Z*, none of which is KERNEL=="console", and this tree's
 * 61-neodct-devices.rules does not name it either -- while the app is
 * ndusr_ut. So the open failed on every boot, the descriptor stayed -1, and
 * -1 means "read and discard" to the pump: netsurf's ENTIRE stderr was thrown
 * away. Every fetch error, every certificate complaint, the neodct-mem lines,
 * neodct-play's exit status, the dmesg tail after an abnormal exit. A browser
 * that this launcher exists to make diagnosable produced no evidence at all
 * on any 0.5.x phone.
 *
 * The fix is deliberately NOT a udev rule. A group the untrusted set holds
 * with write access to /dev/console is a compromised renderer able to scribble
 * over the serial console somebody is using to diagnose the phone. fd 2 needs
 * no permission because this process already has it: nd-apprun inherited it
 * from the core, which inherited it from nd-crashguard.sh, which appends it to
 * /NeoDCT/User/logs/core.log -- the file an owner with no serial cable can
 * read, and where every other subsystem already reports. The macro is gone
 * with the open() that used it; the tests stage a file and point fd 2 at it,
 * which is what the phone really does. */
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
 * `input` may be NULL. When it is not, keypad presses arriving on the
 * inherited channel are read in the same poll and DISCARDED -- the channel is
 * a pipe the core is writing into and a browsing session is long enough to
 * fill it, so it has to be drained by somebody.
 *
 * It used to hand them to an nd_t9_bridge, which typed them into a uinput
 * keyboard this app had created. That is gone: the device is the core's now
 * (nd_proc.h, THE KEY DEVICE) because an ndusr_ut process may not open
 * /dev/uinput, and netsurf reads the node the core made instead. */
void nd_browser_pump(int stderr_fd, int console_fd, nd_browser_cpu *cpu, nd_input *input);

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
