/* nd_evdev.c -- finding, opening and decoding a kernel input device.
 *
 * Ported from core/main.py:_event_device_name, _discover_keypad_path and the
 * evdev half of read_keypress().
 *
 * ============ THE TWO RECORD LAYOUTS ============
 *
 * struct input_event carries a struct timeval, so it is 24 bytes on a 64-bit
 * host and 16 on 32-bit ARM. Both are decoded here, because the layout is
 * decided by the kernel that created the device rather than by the program
 * reading it: the same binary reads 16-byte records off the phone's keypad
 * and 24-byte records off a desktop's USB keyboard.
 *
 * The struct is declared locally rather than included from <linux/input.h>:
 * the acceptance gate compiles every source under musl-gcc, which does not
 * see /usr/include/linux, and the layout is fixed ABI that has not moved in
 * twenty years.
 *
 * ============ ND_ROOT ============
 *
 * Device discovery globs and opens real paths, so it all goes through
 * nd_path_resolve(). With NEODCT_ROOT unset that is a plain copy; with it set
 * a host test can build a fake /dev/input tree and exercise the whole
 * six-step priority order without root and without a keyboard. The paths this
 * module RETURNS are in the unresolved namespace, so a caller can hand one
 * straight back to nd_evdev_open().
 */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_input.h"
#include "nd_log.h"
#include "nd_paths.h"

#include "nd_input_priv.h"

#ifdef __GLIBC__
#define IOCTL_REQ(x) ((unsigned long)(x))
#else
#define IOCTL_REQ(x) ((int)(unsigned int)(x))
#endif

/* EVIOCGNAME(len) == _IOC(_IOC_READ, 'E', 0x06, len) on every Linux
 * architecture this project targets. Spelled out so no kernel header is
 * needed; the 0x80000000 bit is _IOC_READ. */
#define EVIOCGNAME_REQ(len) (0x80000000u | (((unsigned)(len) & 0x3FFFu) << 16) | ('E' << 8) | 0x06u)

#define ND_EV_KEY 0x01

/* Discovery works in its own small buffers rather than ND_PATH_MAX ones: two
 * 80-entry arrays of 512-byte paths is 80 KB of stack, and CODING-STANDARDS
 * section 1.5 exists because the thread stacks on this device are small. A
 * /dev/input/by-id name plus an ND_ROOT prefix fits in 160 bytes with room to
 * spare, and thirty-two candidates is more input devices than the phone or
 * QEMU has ever presented. */
#define DEVPATH_MAX 160
#define DEVLIST_MAX 32

/* One record, in whichever of the two shapes the device speaks. */
typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} evdev_record;

static double monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Turn a resolved (real) path back into one in the ND_ROOT namespace, so the
 * value we hand out can be passed straight to nd_evdev_open(). */
static void devirtualise(const char *resolved, char *out, size_t out_sz)
{
    const char *root = nd_path_root();
    size_t rlen = strlen(root);

    if (rlen > 0u && strncmp(resolved, root, rlen) == 0 && resolved[rlen] == '/')
        (void)nd_strlcpy(out, resolved + rlen, out_sz);
    else
        (void)nd_strlcpy(out, resolved, out_sz);
}

/* realpath() over a virtual path, answering in the virtual namespace. Falls
 * back to the input when the path cannot be resolved, which is what
 * os.path.realpath does for a dangling name. */
static void virtual_realpath(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    char real[PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        (void)nd_strlcpy(out, path, out_sz);
        return;
    }
    if (realpath(resolved, real) == NULL) {
        (void)nd_strlcpy(out, path, out_sz);
        return;
    }
    devirtualise(real, out, out_sz);
}

static int cmp_str(const void *a, const void *b)
{
    /* strcmp, not strcoll: Python's sorted() is code-point order, and a
     * locale must never be able to reorder the device list. */
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* glob a virtual pattern, sorted by strcmp, results in the virtual namespace.
 * Returns the number written. */
static size_t glob_virtual(const char *pattern, char out[][DEVPATH_MAX], size_t max)
{
    char resolved[ND_PATH_MAX];
    glob_t g;
    size_t n = 0u;
    size_t i;
    char *ptrs[DEVLIST_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, pattern) != ND_OK)
        return 0u;

    memset(&g, 0, sizeof g);
    /* GLOB_NOSORT because glibc's own sort is strcoll-based. */
    if (glob(resolved, GLOB_NOSORT, NULL, &g) != 0) {
        globfree(&g);
        return 0u;
    }

    for (i = 0u; i < g.gl_pathc && n < max && n < ND_ARRAY_LEN(ptrs); i++)
        ptrs[n++] = g.gl_pathv[i];

    qsort(ptrs, n, sizeof ptrs[0], cmp_str);
    for (i = 0u; i < n; i++)
        devirtualise(ptrs[i], out[i], DEVPATH_MAX);

    globfree(&g);
    return n;
}

nd_err nd_evdev_device_name(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    char sysfs[ND_PATH_MAX];
    char real[ND_PATH_MAX];
    const char *base;
    FILE *f;
    int fd;
    size_t len;

    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    out[0] = '\0';
    if (path == NULL)
        return ND_ERR_INVAL;

    /* EVIOCGNAME first: it is one ioctl and it works on a device that sysfs
     * has not caught up with. */
    if (nd_path_resolve(resolved, sizeof resolved, path) == ND_OK) {
        fd = open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            int n = ioctl(fd, IOCTL_REQ(EVIOCGNAME_REQ(out_sz)), out);

            (void)close(fd);
            if (n > 0) {
                out[out_sz - 1u] = '\0';
                len = strlen(out);
                while (len > 0u && (out[len - 1u] == '\n' || out[len - 1u] == ' '))
                    out[--len] = '\0';
                if (out[0] != '\0')
                    return ND_OK;
            }
            out[0] = '\0';
        }
    }

    /* The Python's path: /sys/class/input/<eventN>/device/name. Kept as the
     * fallback because it is what a test can fake and what the log lines in
     * the field were produced by. */
    virtual_realpath(path, real, sizeof real);
    base = strrchr(real, '/');
    base = (base != NULL) ? base + 1 : real;
    if (snprintf(sysfs, sizeof sysfs, "/sys/class/input/%s/device/name", base) < 0)
        return ND_ERR_TOOLONG;
    if (nd_path_resolve(resolved, sizeof resolved, sysfs) != ND_OK)
        return ND_ERR_TOOLONG;

    f = fopen(resolved, "rb");
    if (f == NULL)
        return ND_ERR_NOTFOUND;
    if (fgets(out, (int)out_sz, f) == NULL)
        out[0] = '\0';
    (void)fclose(f);

    len = strlen(out);
    while (len > 0u && (unsigned char)out[len - 1u] <= ' ')
        out[--len] = '\0';
    return (out[0] != '\0') ? ND_OK : ND_ERR_NOTFOUND;
}

/* The "(name)" suffix the Python prints, which is the literal word "unknown"
 * when nothing could be read. */
static void device_name_or_unknown(const char *path, char *out, size_t out_sz)
{
    if (nd_evdev_device_name(path, out, out_sz) != ND_OK || out[0] == '\0')
        (void)nd_strlcpy(out, "unknown", out_sz);
}

nd_err nd_evdev_discover(char *out_path, size_t out_sz)
{
    char candidates[DEVLIST_MAX][DEVPATH_MAX];
    char seen[DEVLIST_MAX][DEVPATH_MAX];
    char name[128];
    const char *override;
    size_t n_cand = 0u;
    size_t n_seen = 0u;
    size_t got;
    size_t i;
    size_t j;

    if (out_path == NULL || out_sz == 0u)
        return ND_ERR_INVAL;

    /* 1. the explicit override, but only when it actually exists. */
    override = getenv(ND_ENV_KEYPAD_DEVICE);
    if (override != NULL && override[0] != '\0') {
        if (nd_path_exists(override)) {
            char selected[DEVPATH_MAX];

            virtual_realpath(override, selected, sizeof selected);
            device_name_or_unknown(selected, name, sizeof name);
            nd_log(ND_LOG_INPUT, "Using %s: %s (%s)", ND_ENV_KEYPAD_DEVICE, selected, name);
            (void)nd_strlcpy(out_path, selected, out_sz);
            return ND_OK;
        }
        nd_log(ND_LOG_INPUT, "%s not found: %s", ND_ENV_KEYPAD_DEVICE, override);
    }

    /* 2, 3 and 4, concatenated in that order. */
    got = glob_virtual("/dev/input/by-path/*-kbd", candidates, ND_ARRAY_LEN(candidates));
    n_cand = got;
    if (n_cand < ND_ARRAY_LEN(candidates)) {
        got = glob_virtual("/dev/input/by-id/*-kbd", &candidates[n_cand],
                           ND_ARRAY_LEN(candidates) - n_cand);
        n_cand += got;
    }
    if (n_cand < ND_ARRAY_LEN(candidates) && nd_path_exists(ND_PATH_KEYPAD)) {
        (void)nd_strlcpy(candidates[n_cand], ND_PATH_KEYPAD, DEVPATH_MAX);
        n_cand++;
    }

    for (i = 0u; i < n_cand; i++) {
        char resolved[DEVPATH_MAX];
        bool dup = false;

        virtual_realpath(candidates[i], resolved, sizeof resolved);
        for (j = 0u; j < n_seen && !dup; j++) {
            if (strcmp(seen[j], resolved) == 0)
                dup = true;
        }
        if (dup)
            continue;
        if (n_seen < ND_ARRAY_LEN(seen))
            (void)nd_strlcpy(seen[n_seen++], resolved, DEVPATH_MAX);
        if (!nd_path_exists(resolved))
            continue;

        device_name_or_unknown(resolved, name, sizeof name);
        nd_log(ND_LOG_INPUT, "Selected keyboard device: %s (%s)", resolved, name);
        (void)nd_strlcpy(out_path, resolved, out_sz);
        return ND_OK;
    }

    /* 5. whatever event device exists, lowest-numbered first. */
    got = glob_virtual("/dev/input/event*", candidates, ND_ARRAY_LEN(candidates));
    if (got > 0u) {
        char fallback[DEVPATH_MAX];

        virtual_realpath(candidates[0], fallback, sizeof fallback);
        device_name_or_unknown(fallback, name, sizeof name);
        nd_log(ND_LOG_INPUT, "Fallback input device: %s (%s)", fallback, name);
        (void)nd_strlcpy(out_path, fallback, out_sz);
        return ND_OK;
    }

    /* 6. nothing at all. Returning the legacy path rather than failing is
     * deliberate: the caller's open() then produces the one error message
     * that says what is actually wrong. */
    nd_log(ND_LOG_INPUT, "No input event device found; defaulting to %s", ND_PATH_KEYPAD);
    (void)nd_strlcpy(out_path, ND_PATH_KEYPAD, out_sz);
    return ND_ERR_NOTFOUND;
}

int nd_evdev_open(const char *path)
{
    char resolved[ND_PATH_MAX];

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        errno = ENAMETOOLONG;
        return -1;
    }
    /* O_NONBLOCK because every read is preceded by a select() with its own
     * timeout; a blocking descriptor would hold the UI thread past it. */
    return open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* One record, decoded. Returns false on timeout or on anything unreadable.
 * Shared with nd_input.c, which needs releases as well as presses. */
bool nd_evdev_read_record(int fd, double timeout_s, uint16_t *type, uint16_t *code, int32_t *value)
{
    uint8_t buf[24];
    struct timespec ts;
    struct timespec *tsp = &ts;
    struct pollfd pfd;
    ssize_t got;
    evdev_record ev;
    int rc;

    if (fd < 0)
        return false;

    /* ppoll(), not select(). Two reasons: select()'s FD_SET is a macro that
     * cannot be written warning-clean under -Wconversion on musl, which is
     * the target's libc; and ppoll takes a timespec, so a sub-millisecond
     * slice is not silently rounded to zero the way poll()'s int would do it. */
    if (timeout_s < 0.0) {
        tsp = NULL; /* block */
    } else {
        if (timeout_s > 1000000.0)
            timeout_s = 1000000.0;
        ts.tv_sec = (time_t)timeout_s;
        ts.tv_nsec = (long)((timeout_s - (double)ts.tv_sec) * 1e9);
        if (ts.tv_nsec < 0)
            ts.tv_nsec = 0;
        if (ts.tv_nsec > 999999999L)
            ts.tv_nsec = 999999999L;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = ppoll(&pfd, 1u, tsp, NULL);
    if (rc <= 0)
        return false; /* any poll error reads as "nothing", as in the Python */

    got = read(fd, buf, sizeof buf);
    if (got == 24) {
        memcpy(&ev, buf, sizeof ev);
    } else if (got == 16) {
        /* 32-bit timeval: two 32-bit words, then the same tail. */
        uint32_t sec;
        uint32_t usec;

        memcpy(&sec, buf, 4u);
        memcpy(&usec, buf + 4, 4u);
        ev.tv_sec = (long)sec;
        ev.tv_usec = (long)usec;
        memcpy(&ev.type, buf + 8, 2u);
        memcpy(&ev.code, buf + 10, 2u);
        memcpy(&ev.value, buf + 12, 4u);
    } else {
        return false;
    }

    if (type != NULL)
        *type = ev.type;
    if (code != NULL)
        *code = ev.code;
    if (value != NULL)
        *value = ev.value;
    return true;
}

int32_t nd_evdev_read_key(int fd, double timeout_s)
{
    double deadline = (timeout_s >= 0.0) ? monotonic_now() + timeout_s : 0.0;

    for (;;) {
        uint16_t type = 0u;
        uint16_t code = 0u;
        int32_t value = 0;
        double remaining = -1.0;

        if (timeout_s >= 0.0) {
            remaining = deadline - monotonic_now();
            if (remaining < 0.0)
                remaining = 0.0;
        }
        if (!nd_evdev_read_record(fd, remaining, &type, &code, &value))
            return ND_KEY_NONE;

        /* Only a press. Value 2 is the kernel's own autorepeat, which this
         * path has always ignored -- the core synthesises its own from held
         * state so the i2c keypad and a USB keyboard behave the same. */
        if (type == ND_EV_KEY && value == 1)
            return (int32_t)code;

        if (timeout_s >= 0.0 && monotonic_now() >= deadline)
            return ND_KEY_NONE;
    }
}
