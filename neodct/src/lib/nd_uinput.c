/* nd_uinput.c -- a minimal virtual keyboard on /dev/uinput.
 *
 * Ported from System/hw/t9_uinput.py, class UInputKeyboard.
 *
 * On keypad-only hardware the console on /dev/tty2 has no keyboard, and
 * neither does netsurf. This creates one: the kernel is told about a device
 * that can produce the keycodes we need, and from then on writing an
 * input_event to the descriptor is indistinguishable, to everything above,
 * from somebody pressing a key.
 *
 * ============ THE fd= HOOK IS NOT OPTIONAL ============
 *
 * nd_uinput_attach() wraps a descriptor the caller already has and issues no
 * ioctls at all, so a test can point the keyboard at a pipe and read back the
 * exact bytes it emitted. That is how all 27 of the Python's uinput tests
 * work, and the C port keeps it for the same reason.
 *
 * The uinput structs are declared here rather than included from
 * <linux/uinput.h>: the acceptance gate compiles every source under musl-gcc,
 * which does not see /usr/include/linux. The layouts are fixed ABI.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "nd_input.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_t9.h"

#ifdef __GLIBC__
#define IOCTL_REQ(x) ((unsigned long)(x))
#else
#define IOCTL_REQ(x) ((int)(unsigned int)(x))
#endif

#define ND_EV_SYN     0x00
#define ND_EV_KEY     0x01
#define ND_SYN_REPORT 0

#define UI_SET_EVBIT   0x40045564u /* _IOW('U', 100, int) */
#define UI_SET_KEYBIT  0x40045565u /* _IOW('U', 101, int) */
#define UI_DEV_CREATE  0x5501u     /* _IO('U', 1)         */
#define UI_DEV_DESTROY 0x5502u     /* _IO('U', 2)         */

/* _IOC(_IOC_READ, 'U', 44, len) -- UI_GET_SYSNAME, which answers "inputN" for
 * the device this descriptor just created. Spelled out for the same reason
 * the four above are: the acceptance gate compiles every source under
 * musl-gcc, which does not see /usr/include/linux. */
#define UI_GET_SYSNAME(len) ((unsigned)(2u << 30) | ((unsigned)(len) << 16) | (0x55u << 8) | 44u)

/* What UI_GET_SYSNAME writes: "input" plus a small number. 32 is four times
 * what any kernel produces and keeps the whole thing on the stack. */
#define SYSNAME_MAX 32

#define BUS_VIRTUAL 0x06

/* The kernel needs a moment to bind the new device to the console before the
 * first keystroke will land anywhere. Measured on the target; shorter and the
 * first character of a shell command goes missing. */
#define UINPUT_SETTLE_US 200000

#define ND_KEY_BACKSPACE 14
#define ND_KEY_LEFTSHIFT 42

typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} uinput_record;

/* struct uinput_user_dev, verbatim. 80 + 8 + 4 + 4*64*4 bytes. */
#define UINPUT_MAX_NAME_SIZE 80
#define UINPUT_ABS_CNT       64

typedef struct {
    char name[UINPUT_MAX_NAME_SIZE];
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    uint32_t ff_effects_max;
    int32_t absmax[UINPUT_ABS_CNT];
    int32_t absmin[UINPUT_ABS_CNT];
    int32_t absfuzz[UINPUT_ABS_CNT];
    int32_t absflat[UINPUT_ABS_CNT];
} uinput_user_dev;

/* US layout, unshifted. */
static const struct {
    char ch;
    uint16_t code;
} PLAIN[] = {
    {'a', 30},  {'b', 48},  {'c', 46},  {'d', 32}, {'e', 18},  {'f', 33}, {'g', 34}, {'h', 35},
    {'i', 23},  {'j', 36},  {'k', 37},  {'l', 38}, {'m', 50},  {'n', 49}, {'o', 24}, {'p', 25},
    {'q', 16},  {'r', 19},  {'s', 31},  {'t', 20}, {'u', 22},  {'v', 47}, {'w', 17}, {'x', 45},
    {'y', 21},  {'z', 44},  {'1', 2},   {'2', 3},  {'3', 4},   {'4', 5},  {'5', 6},  {'6', 7},
    {'7', 8},   {'8', 9},   {'9', 10},  {'0', 11}, {' ', 57},  {'-', 12}, {'=', 13}, {'[', 26},
    {']', 27},  {';', 39},  {'\'', 40}, {'`', 41}, {'\\', 43}, {',', 51}, {'.', 52}, {'/', 53},
    {'\t', 15}, {'\n', 28},
};

/* US layout, with shift held. */
static const struct {
    char ch;
    uint16_t code;
} SHIFTED[] = {
    {'!', 2},  {'@', 3},  {'#', 4},  {'$', 5},  {'%', 6},  {'^', 7},  {'&', 8},
    {'*', 9},  {'(', 10}, {')', 11}, {'_', 12}, {'+', 13}, {'{', 26}, {'}', 27},
    {':', 39}, {'"', 40}, {'~', 41}, {'|', 43}, {'<', 51}, {'>', 52}, {'?', 53},
};

/* NeoDCT keypad codes forwarded to the console unchanged -- enter, clear and
 * the four arrows. NeoDCT keycodes ARE Linux keycodes, so there is no
 * translation table here and there does not need to be one. */
static const uint16_t PASSTHROUGH_CODES[] = {28, 14, 103, 105, 106, 108};

/* What the BROWSER bridge sends that is not a character and not an arrow: the
 * two page keys, and the four that say which mode the pad is in. nd_t9.h is
 * where they are chosen and what netsurf makes of them; they are listed again
 * here because declaring a keycode and sending it are two different acts and
 * the kernel does the first.
 *
 * UI_SET_KEYBIT IS NOT A FORMALITY. A uinput device may only emit codes it
 * declared before UI_DEV_CREATE; write() an undeclared one afterwards and the
 * kernel drops the event inside evdev, with no error on the descriptor and
 * nothing in dmesg. So a keycode missing from this list is a key that works
 * perfectly in every host test -- which writes to a pipe and never asks the
 * kernel anything -- and does nothing at all on the phone.
 *
 * They are declared on EVERY keyboard this module makes, including the shell's
 * and the raw one, rather than only the browser's. The list is what the device
 * CAN carry, the bridge decides what it does carry, and a keycode nobody sends
 * costs one ioctl at startup. Splitting it by consumer would buy nothing and
 * would put the failure above one refactor away. */
static const uint16_t BRIDGE_SIGNAL_CODES[] = {
    ND_T9_BROWSER_KEY_PAGE_UP,  ND_T9_BROWSER_KEY_PAGE_DOWN,  ND_T9_BROWSER_KEY_MODE_NAV,
    ND_T9_BROWSER_KEY_MODE_ABC, ND_T9_BROWSER_KEY_MODE_UPPER, ND_T9_BROWSER_KEY_MODE_123,
};

bool nd_uinput_char_to_keypress(char c, uint16_t *code, bool *shift)
{
    size_t i;

    if (code == NULL || shift == NULL)
        return false;

    for (i = 0u; i < ND_ARRAY_LEN(PLAIN); i++) {
        if (PLAIN[i].ch == c) {
            *code = PLAIN[i].code;
            *shift = false;
            return true;
        }
    }
    for (i = 0u; i < ND_ARRAY_LEN(SHIFTED); i++) {
        if (SHIFTED[i].ch == c) {
            *code = SHIFTED[i].code;
            *shift = true;
            return true;
        }
    }
    /* An upper-case letter is the lower-case key plus shift. */
    if (isalpha((unsigned char)c) != 0) {
        char lower = (char)tolower((unsigned char)c);

        for (i = 0u; i < ND_ARRAY_LEN(PLAIN); i++) {
            if (PLAIN[i].ch == lower) {
                *code = PLAIN[i].code;
                *shift = true;
                return true;
            }
        }
    }
    return false;
}

/* Every keycode the device must declare before the kernel will deliver it. */
static bool needed_keycode(uint16_t code)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(PLAIN); i++) {
        if (PLAIN[i].code == code)
            return true;
    }
    for (i = 0u; i < ND_ARRAY_LEN(SHIFTED); i++) {
        if (SHIFTED[i].code == code)
            return true;
    }
    for (i = 0u; i < ND_ARRAY_LEN(PASSTHROUGH_CODES); i++) {
        if (PASSTHROUGH_CODES[i] == code)
            return true;
    }
    for (i = 0u; i < ND_ARRAY_LEN(BRIDGE_SIGNAL_CODES); i++) {
        if (BRIDGE_SIGNAL_CODES[i] == code)
            return true;
    }
    return code == ND_KEY_LEFTSHIFT;
}

nd_err nd_uinput_open(nd_uinput_kbd *k, const char *path, const char *name)
{
    nd_err rc = ND_OK;
    char resolved[ND_PATH_MAX];
    uinput_user_dev dev;
    struct timespec settle;
    uint16_t code;

    if (k == NULL)
        return ND_ERR_INVAL;
    k->fd = -1;
    k->owns_device = false;

    if (path == NULL)
        path = "/dev/uinput";
    if (name == NULL)
        name = "neodct-t9-keypad";

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return ND_ERR_TOOLONG;

    k->fd = open(resolved, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (k->fd < 0) {
        /* nd_log_err, not nd_log. This was informational for a year, which
         * put the ONE line explaining a browser with no keys on stdout --
         * and nd-core's stdout is /dev/tty1, the framebuffer VT the UI is
         * painting over. It never reached core.log, which is the file an
         * owner without a serial cable can actually read. An unopenable
         * /dev/uinput is never routine: it is either a missing kernel module
         * or a process that is not allowed to inject keys, and both of those
         * are somebody's afternoon. */
        nd_log_err(ND_LOG_INPUT, "uinput keyboard unavailable: %s: %s", path, strerror(errno));
        return ND_ERR_IO;
    }
    k->owns_device = true;

    if (ioctl(k->fd, IOCTL_REQ(UI_SET_EVBIT), (unsigned long)ND_EV_KEY) < 0) {
        rc = ND_ERR_HARDWARE;
        goto fail;
    }
    /* Ascending, which is what sorted(set(...)) produced. The order does not
     * matter to the kernel, but a diff against the Python is then the six
     * BRIDGE_SIGNAL_CODES and nothing else -- the Python had no browser
     * chrome to talk to and declared only what it could type. */
    for (code = 0u; code < 256u; code++) {
        if (!needed_keycode(code))
            continue;
        if (ioctl(k->fd, IOCTL_REQ(UI_SET_KEYBIT), (unsigned long)code) < 0) {
            rc = ND_ERR_HARDWARE;
            goto fail;
        }
    }

    memset(&dev, 0, sizeof dev);
    (void)nd_strlcpy(dev.name, name, sizeof dev.name);
    dev.bustype = BUS_VIRTUAL;
    dev.vendor = 0x1;
    dev.product = 0x1;
    dev.version = 1;
    dev.ff_effects_max = 0;
    if (write(k->fd, &dev, sizeof dev) != (ssize_t)sizeof dev) {
        rc = ND_ERR_IO;
        goto fail;
    }
    if (ioctl(k->fd, IOCTL_REQ(UI_DEV_CREATE)) < 0) {
        rc = ND_ERR_HARDWARE;
        goto fail;
    }

    settle.tv_sec = UINPUT_SETTLE_US / 1000000;
    settle.tv_nsec = (UINPUT_SETTLE_US % 1000000) * 1000;
    while (nanosleep(&settle, &settle) != 0 && errno == EINTR)
        ;
    return ND_OK;

fail:
    (void)close(k->fd);
    k->fd = -1;
    k->owns_device = false;
    return rc;
}

nd_err nd_uinput_attach(nd_uinput_kbd *k, int fd)
{
    if (k == NULL || fd < 0)
        return ND_ERR_INVAL;
    k->fd = fd;
    k->owns_device = false;
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Where the device the kernel just made can be READ
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THIS IS NOT A SLEEP ============
 *
 * The whole synchronisation between "the device exists" and "the program we
 * are about to start can open it" used to be UINPUT_SETTLE_US above: 200 ms,
 * measured once for LinuxShell's console binding on one board, and spent in
 * the PARENT before the child was even forked.
 *
 * The two facts it was standing in for are not the same fact and do not
 * happen at the same time. devtmpfs creates /dev/input/eventN the instant
 * UI_DEV_CREATE returns, root:root 0600. It becomes 0660 root:input only
 * when udevd gets round to the hotplug event -- eudev supplies that mode
 * implicitly from the bare `SUBSYSTEM=="input", GROUP="input"` in its stock
 * rules -- and udevd is a separate process on a single-core RV1103 that may
 * be several hundred milliseconds behind, or seconds behind while S11udevall
 * is still coldplugging. Until then every reader gets EACCES.
 *
 * That matters more than it looks, because the program on the far side gets
 * exactly one chance: libnsfb's evdev_open_inputs() (src/surface/linux.c)
 * walks /dev/input/event0..31 ONCE from linux_initialise() and never rescans.
 * A netsurf that starts inside that window is keyless for the whole session,
 * silently, with no retry and no message. So the answer is not a longer
 * sleep, it is waiting for the thing we actually need -- and then telling the
 * child which node it is, so it does not have to guess either.
 */

/* Is `name` an "eventN" directory entry? */
static bool is_event_name(const char *name)
{
    size_t i;

    if (strncmp(name, "event", 5u) != 0 || name[5] == '\0')
        return false;
    for (i = 5u; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return i < 16u;
}

/* /sys/class/input/<sysname>/eventN -> "eventN", or false. */
static bool event_under_sysname(const char *sysname, char *out, size_t out_sz)
{
    char dir[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    DIR *d;
    struct dirent *e;
    bool found = false;

    if (nd_snprintf(dir, sizeof dir, "/sys/class/input/%s", sysname) != ND_OK)
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return false;
    d = opendir(resolved);
    if (d == NULL)
        return false;
    while ((e = readdir(d)) != NULL) {
        if (!is_event_name(e->d_name))
            continue;
        found = nd_snprintf(out, out_sz, "%s", e->d_name) == ND_OK;
        break;
    }
    (void)closedir(d);
    return found;
}

nd_err nd_uinput_event_node(const nd_uinput_kbd *k, char *out, size_t out_sz)
{
    char sysname[SYSNAME_MAX];
    char event[16];

    if (k == NULL || k->fd < 0 || out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    out[0] = '\0';

    memset(sysname, 0, sizeof sysname);
    /* UI_GET_SYSNAME rather than a scan of every /sys/class/input/event*
     * device/name: two bridges with the same name can exist at once -- an
     * engineering LinuxShell and the core's -- and matching on the name would
     * hand one of them the other's node. The ioctl answers about THIS
     * descriptor and cannot be confused. */
    if (ioctl(k->fd, IOCTL_REQ(UI_GET_SYSNAME(SYSNAME_MAX)), sysname) < 0) {
        nd_log_err(ND_LOG_INPUT, "uinput: UI_GET_SYSNAME: %s", strerror(errno));
        return ND_ERR_HARDWARE;
    }
    sysname[SYSNAME_MAX - 1u] = '\0';
    if (sysname[0] == '\0')
        return ND_ERR_NOTFOUND;

    /* The eventN directory appears under the inputN one as part of the same
     * device registration, so this is not a second race -- but it is read
     * from sysfs rather than composed, because inputN and eventN are two
     * different counters and assuming they match is how a bridge ends up
     * pointing at somebody else's keyboard. */
    if (!event_under_sysname(sysname, event, sizeof event))
        return ND_ERR_NOTFOUND;
    return nd_snprintf(out, out_sz, "/dev/input/%s", event);
}

bool nd_uinput_wait_readable(const char *node, double timeout_s)
{
    char resolved[ND_PATH_MAX];
    double waited = 0.0;

    if (node == NULL || node[0] == '\0')
        return false;
    if (nd_path_resolve(resolved, sizeof resolved, node) != ND_OK)
        return false;

    for (;;) {
        struct timespec step;

        if (access(resolved, R_OK) == 0)
            return true;
        if (waited >= timeout_s)
            return false;
        /* 20 ms: forty probes covers the udev latency measured on this board
         * with room to spare, and a probe is one stat. */
        step.tv_sec = 0;
        step.tv_nsec = 20 * 1000 * 1000;
        while (nanosleep(&step, &step) != 0 && errno == EINTR) {}
        waited += 0.02;
    }
}

static nd_err emit(nd_uinput_kbd *k, uint16_t type, uint16_t code, int32_t value)
{
    uinput_record rec;
    struct timespec ts;

    if (k == NULL || k->fd < 0)
        return ND_ERR_INVAL;

    /* time.time(), not monotonic: the Python stamps wall-clock and evdev
     * consumers expect a real timeval here. */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    memset(&rec, 0, sizeof rec);
    rec.tv_sec = (long)ts.tv_sec;
    rec.tv_usec = (long)(ts.tv_nsec / 1000);
    rec.type = type;
    rec.code = code;
    rec.value = value;

    if (write(k->fd, &rec, sizeof rec) != (ssize_t)sizeof rec)
        return ND_ERR_IO;
    return ND_OK;
}

static nd_err syn(nd_uinput_kbd *k)
{
    return emit(k, ND_EV_SYN, ND_SYN_REPORT, 0);
}

nd_err nd_uinput_send_key(nd_uinput_kbd *k, uint16_t code, bool shift)
{
    nd_err rc;

    if (shift) {
        rc = emit(k, ND_EV_KEY, ND_KEY_LEFTSHIFT, 1);
        if (rc == ND_OK)
            rc = syn(k);
        if (rc != ND_OK)
            return rc;
    }
    rc = emit(k, ND_EV_KEY, code, 1);
    if (rc == ND_OK)
        rc = syn(k);
    if (rc == ND_OK)
        rc = emit(k, ND_EV_KEY, code, 0);
    if (rc == ND_OK)
        rc = syn(k);
    if (rc != ND_OK)
        return rc;

    if (shift) {
        rc = emit(k, ND_EV_KEY, ND_KEY_LEFTSHIFT, 0);
        if (rc == ND_OK)
            rc = syn(k);
    }
    return rc;
}

/* One HALF of a keystroke, unchanged.
 *
 * nd_uinput_send_key() above is a whole press-and-release, which is right for
 * a bridge TYPING text into a console: the caller has a character, not a
 * finger. It is wrong for forwarding the phone's own keypad, because a
 * program on the far side that wants to know whether a key is still down --
 * an emulator holding a d-pad direction, mpv seeking while a key is held, any
 * game -- would see every press end a microsecond after it began.
 *
 * So the raw route mirrors what the core saw: press when the key went down,
 * release when it came up, one SYN_REPORT each, and nothing invented in
 * between. */
nd_err nd_uinput_send_raw(nd_uinput_kbd *k, uint16_t code, bool pressed)
{
    nd_err rc = emit(k, ND_EV_KEY, code, pressed ? 1 : 0);

    if (rc == ND_OK)
        rc = syn(k);
    return rc;
}

bool nd_uinput_type_char(nd_uinput_kbd *k, char c)
{
    uint16_t code = 0u;
    bool shift = false;

    if (!nd_uinput_char_to_keypress(c, &code, &shift))
        return false;
    return nd_uinput_send_key(k, code, shift) == ND_OK;
}

nd_err nd_uinput_backspace(nd_uinput_kbd *k)
{
    return nd_uinput_send_key(k, ND_KEY_BACKSPACE, false);
}

void nd_uinput_close(nd_uinput_kbd *k)
{
    if (k == NULL || k->fd < 0)
        return;
    if (k->owns_device)
        (void)ioctl(k->fd, IOCTL_REQ(UI_DEV_DESTROY));
    /* An attached descriptor is closed too, exactly as UInputKeyboard.close()
     * does: the bridge owns the keyboard's lifetime either way, and a caller
     * that wants the fd back can dup() it before attaching. */
    (void)close(k->fd);
    k->fd = -1;
    k->owns_device = false;
}
