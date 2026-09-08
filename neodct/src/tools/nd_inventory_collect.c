/* nd_inventory_collect.c -- the collectors and the canonicalisers.
 *
 * Everything here is plain C11 + POSIX and links nothing of ours, for the
 * reason nd_inventory.h gives at length: this file has to cross-compile on
 * its own against a bare toolchain, because the QEMU-side baseline was
 * captured by booting the repo's kernel with a busybox initramfs long before
 * there is a Buildroot image to capture from.
 *
 * ============ THE RULES THIS FILE ENFORCES, AS RULES ============
 *
 * They are here rather than in the allowlist on purpose, and that is the
 * single biggest weakness of the design: widening a mask silences a whole
 * class of divergence and there is no automated check that a mask is not too
 * wide. What holds it is that widening one takes a code diff AND a baseline
 * recapture, which is two visible things in one pull request.
 *
 *   NO CLOCK AND NO SENSOR. Not "masked" -- not read. No uptime, no btime,
 *   no atime/mtime/ctime, no RTC value, no capacity, no voltage_now, no
 *   temp, no brightness. A mask over a live reading still opens the file;
 *   a rule against reading it cannot be got round by a future collector.
 *
 *   NAMES, NOT VALUES, in sysfs -- with a fixed identity-attribute allowance
 *   per class. This is what makes power_supply/<dev>/serial_number safe BY
 *   CONSTRUCTION rather than by a blocklist somebody has to maintain: it
 *   appears in the file as an attribute NAME and never as a value, so a
 *   per-unit identifier cannot reach a git-committed file even if a future
 *   kernel invents a new one.
 *
 *   NOTHING UNDER /NeoDCT/User. It is the only writable partition and it
 *   holds the phonebook, the SMS databases and Remote Shell's private key.
 *   The mount POINT is recorded; nothing inside it is. Same for /dev/shm
 *   and /dev/pts, whose contents are somebody's runtime state rather than a
 *   property of the machine.
 *
 *   NO ADDRESSES. /proc/iomem, /proc/ioports, /proc/kallsyms, MAC and IP.
 *
 *   AND NO WRITE, NO STATE-CHANGING IOCTL, NO SIDE-EFFECTING OPEN. The rule
 *   is one line and it is what makes this safe to run on a phone in the
 *   owner's pocket: THE INVENTORY OPENS /dev/fb0 O_RDONLY FOR TWO GET
 *   IOCTLS AND OPENS NOTHING ELSE UNDER /dev. Opening /dev/ttyUSB2 raises
 *   DTR on a real SIM7600; /home/user/k/fbprobe.c gets its framebuffer
 *   numbers with a PUT first, which is an instrument that changes the
 *   machine it is measuring. This does GET only and pins WHEN it runs
 *   instead -- the capture script refuses a capture taken before
 *   S90display's force_mode() rather than silently recording 640x480x8.
 *
 * ============ readdir ORDER IS NEVER TRUSTED, ANYWHERE ============
 *
 * Every directory listing here is sorted byte-wise before it is used, and
 * every list VALUE is sorted too. sysfs readdir is kernfs order -- an rbtree
 * keyed on a name hash -- so it is neither alphabetical nor creation order
 * and it moves with the kernel build. nd_modem.c's sorted_listdir() carries
 * the scar: a truncation applied before the sort made a phone report "no
 * candidate AT ports" for ever with the modem sitting there enumerated.
 *
 * nd_selftest.c's first_matching() deliberately DOES use readdir order, and
 * says why; the divergence in habit between the two tools is not an
 * oversight. It is asking "can this user open some i2c bus" and any bus will
 * do; this is asking "what is on this machine" and the answer has to be the
 * same file twice.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "nd_inventory.h"

/* ------------------------------------------------------------------ *
 * Small text helpers
 * ------------------------------------------------------------------ */

#define INV_PATH_MAX 512
#define INV_LIST_MAX 4096

static bool str_copy(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);

    if (n == 0u || len >= n)
        return false;
    memcpy(dst, src, len + 1u);
    return true;
}

/* snprintf with the truncation check the coding standards demand at every
 * call site. Truncation is a hard false here and never a shortened string:
 * a truncated KEY silently merges two records, which is the one failure a
 * byte-stable format cannot survive. */
static bool str_fmt(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    int written;

    va_start(ap, fmt);
    written = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return written >= 0 && (size_t)written < n;
}

static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0u;

    while (len > 0u && (s[len - 1u] == '\n' || s[len - 1u] == '\r' || s[len - 1u] == ' ' ||
                        s[len - 1u] == '\t'))
        len--;
    s[len] = '\0';
    while (s[start] == ' ' || s[start] == '\t')
        start++;
    if (start > 0u)
        memmove(s, s + start, len - start + 1u);
}

/* Newlines and the sentinel's own separator would break one record into two
 * on the way down a serial line, so a value that contains either is not
 * recorded as itself. This has never fired on a real capture; it exists so
 * that the day a kernel invents an attribute with a newline in it, the
 * capture stays parseable and says what happened. */
static void sanitise_value(char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n' || c == '\r' || c == '\t')
            s[i] = ' ';
        else if (c < 0x20u || c == 0x7fu)
            s[i] = '?';
    }
}

bool inv_path(const inv_cfg *cfg, char *out, size_t n, const char *abs)
{
    const char *root = (cfg != NULL && cfg->root != NULL) ? cfg->root : "/";

    if (root[0] == '/' && root[1] == '\0')
        return str_copy(out, n, abs);
    return str_fmt(out, n, "%s%s", root, abs);
}

/* Whole small file into a buffer, trimmed. False for anything that could not
 * be opened OR read -- the caller decides between ABSENT and UNREADABLE from
 * errno, because those are different facts and a format that conflated them
 * would report a permissions change as a shorter file. */
static bool read_text(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t got;

    if (fd < 0)
        return false;
    got = read(fd, out, n - 1u);
    (void)close(fd);
    if (got < 0)
        return false;
    out[(size_t)got] = '\0';
    trim(out);
    return true;
}

static bool read_attr(const inv_cfg *cfg, char *out, size_t n, const char *fmt, ...)
{
    char abs[INV_PATH_MAX];
    char path[INV_PATH_MAX];
    va_list ap;
    int written;

    /* So that a caller reading errno after a false is reading THIS call. The
     * two paths that fail before any syscall runs -- a format that does not
     * fit, a --root that does not fit -- left whatever the last syscall
     * anywhere in the process had set, and the class collector then decided
     * between ABSENT and UNREADABLE from it. */
    errno = 0;
    va_start(ap, fmt);
    written = vsnprintf(abs, sizeof abs, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= sizeof abs)
        return false;
    if (!inv_path(cfg, path, sizeof path, abs))
        return false;
    return read_text(path, out, n);
}

/* ------------------------------------------------------------------ *
 * The record buffer
 * ------------------------------------------------------------------ */

typedef struct {
    char key[ND_INV_KEY_MAX];
    char val[ND_INV_VAL_MAX];
    inv_mark mark;
} inv_rec;

struct inv_out {
    inv_rec *rec;
    size_t n;
    size_t cap;
    size_t dropped;
};

inv_out *inv_out_new(void)
{
    inv_out *o = calloc(1u, sizeof *o);

    if (o == NULL)
        return NULL;
    o->cap = 256u;
    /* owned by inv_out; freed by inv_out_free() */
    o->rec = calloc(o->cap, sizeof *o->rec);
    if (o->rec == NULL) {
        free(o);
        return NULL;
    }
    return o;
}

void inv_out_free(inv_out *o)
{
    if (o == NULL)
        return;
    free(o->rec);
    free(o);
}

/* A RECORD LINE IS `INV|<key> <value>` AND THE KEY IS THEREFORE ONE
 * WHITESPACE-FREE TOKEN. That is not a style rule, it is what makes the
 * format parseable, and it was found by the reader rather than by the writer:
 * the first committed capture carried
 * `input.byname."QEMU Virtio Keyboard".handlers`, and a comparator splitting
 * on the first space read the key as `input.byname."QEMU` and merged three
 * different devices into one record.
 *
 * The fix is here, in the one place every key passes through, rather than at
 * the four collectors that build a key out of a device's own name -- because
 * a rule enforced at four call sites is a rule the fifth collector will not
 * know about. A space becomes \x20 and a backslash \x5c, so the escaping is
 * reversible, sorts stably, and survives any transport that would otherwise
 * fold whitespace. Values are the rest of the line and need none of this.
 *
 * Refused rather than trimmed if the escaped form does not fit: a truncated
 * key silently merges two records, which is the one failure a byte-stable
 * format cannot survive. */
static bool escape_key(char *dst, size_t n, const char *key)
{
    size_t used = 0u;
    size_t i;

    for (i = 0u; key[i] != '\0'; i++) {
        unsigned char c = (unsigned char)key[i];

        if (c > 0x20u && c != 0x7fu && c != (unsigned char)'\\') {
            if (used + 1u >= n)
                return false;
            dst[used++] = (char)c;
            continue;
        }
        if (used + 4u >= n)
            return false;
        (void)snprintf(dst + used, n - used, "\\x%02x", (unsigned int)c);
        used += 4u;
    }
    if (used >= n)
        return false;
    dst[used] = '\0';
    return true;
}

bool inv_add(inv_out *o, inv_mark mark, const char *key, const char *fmt, ...)
{
    inv_rec *r;
    va_list ap;
    int written;
    size_t i;

    if (o == NULL)
        return false;
    if (o->n == o->cap) {
        size_t cap = o->cap * 2u;
        /* owned by inv_out; freed by inv_out_free() */
        inv_rec *grown = realloc(o->rec, cap * sizeof *grown);

        if (grown == NULL) {
            o->dropped++;
            return false;
        }
        o->rec = grown;
        o->cap = cap;
    }
    r = &o->rec[o->n];
    /* A refusal SAYS WHICH RECORD, on stderr, where it cannot contaminate the
     * capture on stdout. The first capture from the real kernel dropped one
     * record and the message did not name it, which cost a boot and a grep to
     * find; an instrument that reports "something is missing" without saying
     * what is an instrument nobody can act on. */
    if (!escape_key(r->key, sizeof r->key, key)) {
        (void)fprintf(stderr, "nd-inventory: record key too long, refused: %.64s...\n", key);
        o->dropped++;
        return false;
    }
    /* A DUPLICATE KEY IS REFUSED HERE, because parity_diff.py refuses the
     * whole capture for one -- and the two halves of a harness may not
     * disagree about what a legal capture is. Measured on an ordinary
     * container: five virtio disks whose foreign serials are the same length
     * all masked onto `block.byserial.<serial:14>.node`, and /proc/mounts
     * carried /dev/pts and /dev/shm twice; nd-inventory printed "--self-check
     * ok (183 records, twice)" and exited 0, and the comparator then rejected
     * the file with an error the operator could only act on by hand-editing
     * the artefact -- which is the one thing a baseline may never be.
     *
     * --self-check structurally cannot catch this: both collections contain
     * the same duplicates, so they compare equal. The two producers that
     * could collide have been fixed to key by something that is a function
     * (see inv_collect_mounts() and inv_collect_block()); this is the
     * backstop, in the one place every key passes through, so the third
     * producer to key on something many-to-one is caught by the tool rather
     * than by the comparator. It costs a linear scan over a few hundred
     * records, once per record, which is less than emit_family()'s
     * already-documented quadratic. */
    for (i = 0u; i < o->n; i++) {
        if (strcmp(o->rec[i].key, r->key) == 0) {
            (void)fprintf(stderr,
                          "nd-inventory: duplicate record key '%s', refused -- the key is "
                          "not a function of the machine here\n",
                          r->key);
            o->dropped++;
            return false;
        }
    }
    va_start(ap, fmt);
    written = vsnprintf(r->val, sizeof r->val, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= sizeof r->val) {
        (void)fprintf(stderr, "nd-inventory: value too long for '%s' (%d bytes), refused\n", key,
                      written);
        o->dropped++;
        return false;
    }
    sanitise_value(r->val);
    r->mark = mark;
    o->n++;
    return true;
}

/* THE KEY BUILDER EVERY COLLECTOR USES, and the reason it exists is that
 * inv_add()'s contract was enforced in the one place every key passes THROUGH
 * and then bypassed everywhere a key was BUILT. Nineteen call sites did
 * `if (!str_fmt(key, ...)) continue;` -- dropping the record silently, moving
 * no counter and printing nothing, which is the exact opposite of what
 * nd_inventory.h promises two paragraphs above inv_add(): "a false here makes
 * the capture INCOMPLETE (exit 1) rather than shorter".
 *
 * Reproduced with a 201-character mount point: the record vanished, stderr
 * said nothing and the tool exited 0. The same shape reaches the keys built
 * out of a device's own name -- an mtd partition or a sysfs attribute whose
 * name pushes the key past ND_INV_KEY_MAX -- so a capture five records short
 * still hashed cleanly and the diff simply said ABSENT on the other side.
 *
 * A call site may still choose to skip; what it may no longer do is skip
 * without saying so. */
bool inv_key_fmt(inv_out *o, char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    int written;

    va_start(ap, fmt);
    written = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (written >= 0 && (size_t)written < n)
        return true;
    (void)fprintf(stderr, "nd-inventory: record key too long to build, refused: %.64s...\n",
                  (written > 0) ? dst : fmt);
    if (o != NULL)
        o->dropped++;
    return false;
}

size_t inv_count(const inv_out *o)
{
    return (o != NULL) ? o->n : 0u;
}

size_t inv_dropped(const inv_out *o)
{
    return (o != NULL) ? o->dropped : 0u;
}

static int cmp_rec(const void *a, const void *b)
{
    const inv_rec *x = a;
    const inv_rec *y = b;
    int by_key = strcmp(x->key, y->key);

    /* The tie-break is the value, so that two records that somehow share a
     * key still land in one order rather than in qsort's. There should never
     * be two, and a harness whose output depends on qsort's internals for a
     * case that "cannot happen" is a harness that fails once, on somebody
     * else's machine, at the worst moment. */
    return (by_key != 0) ? by_key : strcmp(x->val, y->val);
}

void inv_sort(inv_out *o)
{
    if (o == NULL || o->n == 0u)
        return;
    qsort(o->rec, o->n, sizeof *o->rec, cmp_rec);
}

static char *render(const inv_out *o, bool compared_only)
{
    size_t need = 1u;
    size_t i;
    char *buf;
    size_t used = 0u;

    if (o == NULL)
        return NULL;
    for (i = 0u; i < o->n; i++) {
        if (compared_only && o->rec[i].mark != INV_CMP)
            continue;
        need += strlen(o->rec[i].key) + strlen(o->rec[i].val) + 8u;
    }
    /* owned by the caller; free with free() */
    buf = malloc(need);
    if (buf == NULL)
        return NULL;
    for (i = 0u; i < o->n; i++) {
        int written;

        if (compared_only && o->rec[i].mark != INV_CMP)
            continue;
        written = snprintf(buf + used, need - used, "INV|%s%s %s\n",
                           o->rec[i].mark == INV_RAW ? "~" : "", o->rec[i].key, o->rec[i].val);
        if (written < 0 || (size_t)written >= need - used) {
            free(buf);
            return NULL;
        }
        used += (size_t)written;
    }
    buf[used] = '\0';
    return buf;
}

char *inv_render(const inv_out *o)
{
    return render(o, false);
}

char *inv_render_compared(const inv_out *o)
{
    return render(o, true);
}

/* ------------------------------------------------------------------ *
 * Canonicalisers
 * ------------------------------------------------------------------ */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void inv_sort_tokens(char *list)
{
    char *tok[512];
    size_t n = 0u;
    char *save = NULL;
    char *p;
    size_t i;
    size_t used = 0u;
    size_t len;

    if (list == NULL || list[0] == '\0')
        return;
    len = strlen(list);
    for (p = strtok_r(list, " ", &save); p != NULL && n < 512u; p = strtok_r(NULL, " ", &save))
        tok[n++] = p;
    qsort(tok, n, sizeof tok[0], cmp_str);
    /* Rebuilt through a scratch buffer because strtok_r has already written
     * NULs over the separators of the ORIGINAL order; writing back in the
     * new order in place would overwrite tokens not yet copied. */
    {
        char *scratch = malloc(len + 2u);

        if (scratch == NULL)
            return; /* the list stays NUL-riddled but the caller sees no partial sort */
        for (i = 0u; i < n; i++) {
            size_t tl = strlen(tok[i]);

            if (i > 0u)
                scratch[used++] = ' ';
            memcpy(scratch + used, tok[i], tl);
            used += tl;
        }
        scratch[used] = '\0';
        memcpy(list, scratch, used + 1u);
        free(scratch);
    }
}

bool inv_mask_serial(const char *serial, char *out, size_t n)
{
    static const char *const OURS[] = {"NDSYS", "NDUSER", "NDCARD", "NDAPPLY", NULL};
    size_t i;

    if (serial == NULL || serial[0] == '\0')
        return str_copy(out, n, "");
    for (i = 0u; OURS[i] != NULL; i++) {
        if (strcmp(serial, OURS[i]) == 0)
            return str_copy(out, n, serial);
    }
    return str_fmt(out, n, "<serial:%zu>", strlen(serial));
}

uint32_t inv_bucket_mib(uint64_t kb)
{
    uint64_t mib = kb / 1024u;

    /* Nearest, not floor: 53,824 kB is 52.56 MiB and the honest bucket for
     * it is 52, while a floor at a bucket edge would put two machines four
     * megabytes apart into the same bucket half the time. The edge case is
     * real and is written down as a risk -- two machines either side of a
     * boundary produce one permanent-looking record that is an artefact of
     * the boundary rather than of the machines. */
    return (uint32_t)(((mib + 2u) / 4u) * 4u);
}

bool inv_mask_mount_options(const char *opts, char *out, size_t n)
{
    static const char *const MASKED[] = {"size=", "nr_inodes=", "blksize=", NULL};
    char work[INV_LIST_MAX];
    char built[INV_LIST_MAX];
    char *save = NULL;
    char *p;
    size_t used = 0u;

    if (!str_copy(work, sizeof work, opts))
        return false;
    built[0] = '\0';
    for (p = strtok_r(work, ",", &save); p != NULL; p = strtok_r(NULL, ",", &save)) {
        const char *emit = p;
        char masked[64];
        size_t i;
        int written;

        for (i = 0u; MASKED[i] != NULL; i++) {
            size_t plen = strlen(MASKED[i]);

            if (strncmp(p, MASKED[i], plen) == 0) {
                if (!str_fmt(masked, sizeof masked, "%s<masked>", MASKED[i]))
                    return false;
                emit = masked;
                break;
            }
        }
        written = snprintf(built + used, sizeof built - used, "%s%s", used > 0u ? " " : "", emit);
        if (written < 0 || (size_t)written >= sizeof built - used)
            return false;
        used += (size_t)written;
    }
    inv_sort_tokens(built);
    return str_copy(out, n, built);
}

void inv_mode_octal(unsigned int mode, char out[8])
{
    (void)snprintf(out, 8u, "%04o", mode & 07777u);
}

static bool all_digits(const char *s)
{
    size_t i;

    if (s[0] == '\0')
        return false;
    for (i = 0u; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

const char *inv_dev_family(const char *name)
{
    /* The legacy BSD pty grid. `pty` or `tty` + one of "pqrstuvwxyzabcde" +
     * one hex digit: 16 * 16 * 2 = 512 possible names, and on the repo's
     * kernel ALL 512 are there -- the committed capture carries
     * `dev.pty[a-z][0-f] n=256` beside `dev.tty[a-z][0-f] n=256`. This said
     * 336, which is not a number anything measured. */
    static const char *const PTY_LETTERS = "pqrstuvwxyzabcde";

    if (name == NULL || name[0] == '\0')
        return NULL;

    if (strlen(name) == 5u && strchr(PTY_LETTERS, name[3]) != NULL &&
        isxdigit((unsigned char)name[4]) != 0) {
        if (strncmp(name, "pty", 3u) == 0)
            return "pty[a-z][0-f]";
        if (strncmp(name, "tty", 3u) == 0)
            return "tty[a-z][0-f]";
    }
    if (strncmp(name, "tty", 3u) == 0 && all_digits(name + 3))
        return "tty[N]";
    if (strncmp(name, "loop", 4u) == 0 && all_digits(name + 4))
        return "loop[N]";
    if (strncmp(name, "ram", 3u) == 0 && all_digits(name + 3))
        return "ram[N]";
    if (strncmp(name, "vcsa", 4u) == 0 && all_digits(name + 4))
        return "vcsa[N]";
    if (strncmp(name, "vcsu", 4u) == 0 && all_digits(name + 4))
        return "vcsu[N]";
    if (strncmp(name, "vcs", 3u) == 0 && all_digits(name + 3))
        return "vcs[N]";
    return NULL;
}

bool inv_cmdline_keep(const char *name)
{
    /* THE POLICY SWITCHES ARE HERE AND THEY WERE NOT, which mattered most for
     * the one that decides whether the immutable-rootfs design is enforced at
     * all. `neodct.verity` appeared in cmdline.keys -- identical whichever
     * mode is set -- and its VALUE reached the file only as cmdline.raw,
     * which is INV_RAW and therefore never diffed and never in the compared
     * hash. So a phone that has been through recovery, which sets
     * neodct.verity=permissive for the next boot (ndsys-recovery.sh), or a
     * field unit left on `off`, produced ZERO differing records against an
     * emulator on `enforce`: the one machine in the fleet whose root is no
     * longer verified was the one this harness called in parity.
     * `neodct.recovery` and `neodct.unsigned` are the same shape -- boot
     * straight into recovery, and let an unsigned update install.
     *
     * `neodct.rectty` deliberately stays out with root= and console=: it is a
     * device path, which is the class whose values come from the U-Boot env
     * on one machine and run_qemu.sh on the other. */
    static const char *const KEEP[] = {"video",           "neodct.devenv", "panic",
                                       "loglevel",        "neodct.verity", "neodct.recovery",
                                       "neodct.unsigned", NULL};
    static const char *const KEEP_PREFIX[] = {"nandsim.", "mtdram.", NULL};
    size_t i;

    for (i = 0u; KEEP[i] != NULL; i++) {
        if (strcmp(name, KEEP[i]) == 0)
            return true;
    }
    for (i = 0u; KEEP_PREFIX[i] != NULL; i++) {
        if (strncmp(name, KEEP_PREFIX[i], strlen(KEEP_PREFIX[i])) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * SHA-256
 *
 * Written out rather than taken from libcrypto because this file has to
 * cross-compile against a bare toolchain (see the header), and because
 * nd-verify already demonstrates what linking OpenSSL costs a small tool:
 * 4.3 MB static. Ninety lines against four megabytes.
 * ------------------------------------------------------------------ */

static uint32_t rotr32(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32u - n));
}

void inv_sha256_hex(const char *data, size_t len, char out[65])
{
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint64_t bits = (uint64_t)len * 8u;
    size_t total = len + 1u;
    size_t padded;
    unsigned char *msg;
    size_t off;
    size_t i;

    while ((total % 64u) != 56u)
        total++;
    padded = total + 8u;
    /* owned here; freed before return */
    msg = calloc(padded, 1u);
    if (msg == NULL) {
        (void)str_copy(out, 65u, "UNAVAILABLE");
        return;
    }
    memcpy(msg, data, len);
    msg[len] = 0x80u;
    for (i = 0u; i < 8u; i++)
        msg[padded - 1u - i] = (unsigned char)((bits >> (8u * i)) & 0xffu);

    for (off = 0u; off < padded; off += 64u) {
        uint32_t w[64];
        uint32_t a, b, c, d, e, f, g, hh;
        size_t t;

        for (t = 0u; t < 16u; t++)
            w[t] = ((uint32_t)msg[off + t * 4u] << 24) | ((uint32_t)msg[off + t * 4u + 1u] << 16) |
                   ((uint32_t)msg[off + t * 4u + 2u] << 8) | (uint32_t)msg[off + t * 4u + 3u];
        for (t = 16u; t < 64u; t++) {
            uint32_t s0 = rotr32(w[t - 15u], 7u) ^ rotr32(w[t - 15u], 18u) ^ (w[t - 15u] >> 3);
            uint32_t s1 = rotr32(w[t - 2u], 17u) ^ rotr32(w[t - 2u], 19u) ^ (w[t - 2u] >> 10);

            w[t] = w[t - 16u] + s0 + w[t - 7u] + s1;
        }
        a = h[0];
        b = h[1];
        c = h[2];
        d = h[3];
        e = h[4];
        f = h[5];
        g = h[6];
        hh = h[7];
        for (t = 0u; t < 64u; t++) {
            uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + s1 + ch + K[t] + w[t];
            uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    free(msg);
    for (i = 0u; i < 8u; i++)
        (void)snprintf(out + i * 8u, 9u, "%08x", h[i]);
}

/* ------------------------------------------------------------------ *
 * Sorted directory listing -- the one primitive every collector shares
 * ------------------------------------------------------------------ */

typedef struct {
    char (*name)[64];
    size_t n;
    size_t cap;
} namelist;

/* strcmp cannot be handed to qsort directly: the cast between the two
 * function types is undefined behaviour and -Wcast-function-type says so. */
static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void namelist_free(namelist *nl)
{
    free(nl->name);
    nl->name = NULL;
    nl->n = 0u;
    nl->cap = 0u;
}

static bool namelist_push(namelist *nl, const char *name)
{
    if (nl->n == nl->cap) {
        size_t cap = (nl->cap == 0u) ? 32u : nl->cap * 2u;
        /* owned by the namelist; freed with namelist_free() */
        char(*grown)[64] = realloc(nl->name, cap * sizeof *grown);

        if (grown == NULL)
            return false;
        nl->name = grown;
        nl->cap = cap;
    }
    if (!str_copy(nl->name[nl->n], 64u, name))
        return false;
    nl->n++;
    return true;
}

/* -1 no such directory, -2 present but unreadable, otherwise the count.
 * Three outcomes and not two, because ABSENT and UNREADABLE are different
 * facts about a machine and a collector that collapsed them would report a
 * permissions change as a missing device. */
static int listdir_sorted(const char *path, namelist *nl)
{
    DIR *d = opendir(path);
    struct dirent *ent;

    nl->name = NULL;
    nl->n = 0u;
    nl->cap = 0u;
    if (d == NULL)
        return (errno == ENOENT || errno == ENOTDIR) ? -1 : -2;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!namelist_push(nl, ent->d_name)) {
            (void)closedir(d);
            namelist_free(nl);
            return -2;
        }
    }
    (void)closedir(d);
    /* An empty directory leaves nl->name NULL, and qsort(NULL, 0, ...) is
     * undefined behaviour that UBSan reports rather than tolerates -- found
     * by `make ASAN=1 test` on /sys/class/backlight, which is the empty
     * directory this whole harness has the most to say about. */
    if (nl->n > 0u)
        qsort(nl->name, nl->n, sizeof nl->name[0], cmp_name);
    return (int)nl->n;
}

/* "[a b c]" for a listing, "[]" for an empty one. The two are DIFFERENT
 * FACTS and the format must never conflate them, and the repo's own kernel
 * now shows all three states at once: class.power_supply is [] (the subsystem
 * is compiled in and CONFIG_TEST_POWER was removed, so no device), class.leds
 * is ABSENT (LEDS_CLASS is not built, so no such path), and class.backlight
 * is [backlight] (the subsystem was always there and a device tree finally
 * gave it a device). That last one used to be [] and is the worked example
 * for the whole distinction -- the classes were there, so the work was
 * providing devices rather than enabling subsystems -- and a format that
 * printed an empty directory and a missing one the same way would have hidden
 * which of the two jobs was outstanding.
 *
 * FAMILIES COLLAPSE HERE TOO, AND THAT WAS FOUND BY BOOTING RATHER THAN BY
 * READING. The first capture from the real kernel dropped class.tty entirely:
 * the tty class holds the whole legacy BSD pty grid, so the listing ran past
 * ND_INV_VAL_MAX and the record was refused. Raising the cap would have been
 * the wrong fix -- a 2,500-character line is not a record anybody diffs -- so
 * the /dev family table is applied to class listings as well and a family
 * arrives as one token carrying its count: `pty[a-z][0-f]x256`. The count is
 * still in the record, so a class that gains or loses a member is still a
 * diff; what is gone is 300 tokens of noise around the four names that
 * matter (console, ptmx, ttyAMA0, ttyS0). */
static bool joined(const namelist *nl, char *out, size_t n)
{
    size_t used = 0u;
    size_t i;
    int written;

    written = snprintf(out, n, "[");
    if (written < 0 || (size_t)written >= n)
        return false;
    used = (size_t)written;
    for (i = 0u; i < nl->n;) {
        const char *fam = inv_dev_family(nl->name[i]);
        size_t j = i + 1u;

        if (fam != NULL) {
            while (j < nl->n) {
                const char *f2 = inv_dev_family(nl->name[j]);

                if (f2 == NULL || strcmp(f2, fam) != 0)
                    break;
                j++;
            }
            written = snprintf(out + used, n - used, "%s%sx%zu", used > 1u ? " " : "", fam, j - i);
        } else {
            written = snprintf(out + used, n - used, "%s%s", used > 1u ? " " : "", nl->name[i]);
        }
        if (written < 0 || (size_t)written >= n - used)
            return false;
        used += (size_t)written;
        i = j;
    }
    written = snprintf(out + used, n - used, "]");
    return written >= 0 && (size_t)written < n - used;
}

/* ------------------------------------------------------------------ *
 * Collectors
 * ------------------------------------------------------------------ */

/* uname(2) is the ONE collector --root cannot reprefix, and that is honest
 * rather than a gap: it is a property of the running kernel and there is no
 * file to substitute. A synthetic-root capture therefore carries the host's
 * kernel identity, which is exactly why parity_diff.py refuses a non-"/"
 * root as a baseline. */
void inv_collect_kernel(inv_out *o, const inv_cfg *cfg)
{
    struct utsname u;

    (void)cfg;
    if (uname(&u) != 0) {
        (void)inv_add(o, INV_CMP, "uname.sysname", "UNREADABLE(%d)", errno);
        return;
    }
    (void)inv_add(o, INV_CMP, "uname.sysname", "%s", u.sysname);
    (void)inv_add(o, INV_CMP, "uname.machine", "%s", u.machine);
    /* COMPARED, not masked and not exempted. It differs -- 6.12.47 here
     * against the SDK's 5.10.110 -- and it is a `permanent` allowlist record
     * whose reason is that a kernel cannot ship in a .ndsw. That is the
     * point: a kernel bump then shows up as a diff in TWO committed files
     * that have to move in one commit and get reviewed. uname.machine is
     * compared and must MATCH: armv7l is the whole of Stage 1's claim.
     *
     * uname.version is deliberately absent. It is a build date and a
     * hostname wearing a version's clothes, and it would differ on every
     * rebuild of the same source. */
    (void)inv_add(o, INV_CMP, "uname.release", "%s", u.release);
}

void inv_collect_memory(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char buf[256];
    unsigned long long kb = 0ull;
    FILE *f;

    if (!inv_path(cfg, path, sizeof path, "/proc/meminfo")) {
        (void)inv_add(o, INV_CMP, "mem.total_mib", "UNREADABLE(path)");
        return;
    }
    f = fopen(path, "re");
    if (f == NULL) {
        (void)inv_add(o, INV_CMP, "mem.total_mib", "ABSENT");
        return;
    }
    while (fgets(buf, (int)sizeof buf, f) != NULL) {
        if (sscanf(buf, "MemTotal: %llu kB", &kb) == 1)
            break;
    }
    (void)fclose(f);
    (void)inv_add(o, INV_CMP, "mem.total_mib", "%u", inv_bucket_mib((uint64_t)kb));
    (void)inv_add(o, INV_RAW, "mem.total_kb", "%llu", kb);
}

void inv_collect_cmdline(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char raw[INV_LIST_MAX];
    char work[INV_LIST_MAX];
    char names[INV_LIST_MAX];
    char *save = NULL;
    char *tok;
    size_t used = 0u;

    if (!inv_path(cfg, path, sizeof path, "/proc/cmdline") || !read_text(path, raw, sizeof raw)) {
        (void)inv_add(o, INV_CMP, "cmdline.keys", "ABSENT");
        return;
    }
    if (!str_copy(work, sizeof work, raw)) {
        (void)inv_add(o, INV_CMP, "cmdline.keys", "UNREADABLE(toolong)");
        return;
    }
    names[0] = '\0';
    for (tok = strtok_r(work, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save)) {
        char name[128];
        const char *eq = strchr(tok, '=');
        size_t nlen = (eq != NULL) ? (size_t)(eq - tok) : strlen(tok);
        int written;

        if (nlen >= sizeof name)
            nlen = sizeof name - 1u;
        memcpy(name, tok, nlen);
        name[nlen] = '\0';
        written = snprintf(names + used, sizeof names - used, "%s%s", used > 0u ? " " : "", name);
        if (written < 0 || (size_t)written >= sizeof names - used)
            break;
        used += (size_t)written;
        /* The names are the parity question; the values are partition
         * tables and device paths that come from the U-Boot env on one
         * machine and run_qemu.sh on the other. The KEEP list is the set
         * whose VALUE decides something the two images share. */
        if (inv_cmdline_keep(name)) {
            char key[ND_INV_KEY_MAX];

            if (inv_key_fmt(o, key, sizeof key, "cmdline.%s", name))
                (void)inv_add(o, INV_CMP, key, "%s", (eq != NULL) ? eq + 1 : "");
        }
    }
    inv_sort_tokens(names);
    (void)inv_add(o, INV_CMP, "cmdline.keys", "[%s]", names);
    (void)inv_add(o, INV_RAW, "cmdline.raw", "%s", raw);
}

/* The identity-attribute allowance, per class. Names are always recorded;
 * these are the only VALUES that come with them, and the list is short on
 * purpose. `brightness` is absent while `max_brightness` is present, because
 * one is a live reading and the other is a property of the panel; every
 * other class's omissions are the same distinction. */
typedef struct {
    const char *class_name;
    const char *attrs[5];
} class_spec;

static const class_spec CLASSES[] = {
    {"backlight", {"type", "max_brightness", NULL, NULL, NULL}},
    {"power_supply", {"type", NULL, NULL, NULL, NULL}},
    {"thermal", {"type", NULL, NULL, NULL, NULL}},
    {"i2c-dev", {"name", NULL, NULL, NULL, NULL}},
    {"input", {"name", NULL, NULL, NULL, NULL}},
    {"graphics", {"name", NULL, NULL, NULL, NULL}},
    /* No attribute, and the listing is deliberately the whole directory --
     * export and unexport are files rather than devices, and they are the two
     * that say whether the LEGACY interface is compiled in at all. That is the
     * fact nd_backlight.c's GPIO tier and S90display both depend on, and
     * GPIO_SYSFS is `bool ... if EXPERT`, so it is one olddefconfig away from
     * vanishing without a word. The chips themselves cannot match across the
     * two machines and are not meant to: see the class.gpio record in
     * neodct/tests/parity/allow.txt. */
    {"gpio", {NULL, NULL, NULL, NULL, NULL}},
    {"rtc", {"name", NULL, NULL, NULL, NULL}},
    {"mtd", {NULL, NULL, NULL, NULL, NULL}}, /* geometry: inv_collect_mtd */
    {"net", {"type", NULL, NULL, NULL, NULL}},
    {"leds", {NULL, NULL, NULL, NULL, NULL}},
    {"block", {NULL, NULL, NULL, NULL, NULL}}, /* size/ro/removable: inv_collect_block */
    {"misc", {NULL, NULL, NULL, NULL, NULL}},
    {"tty", {NULL, NULL, NULL, NULL, NULL}},
    {"ubi", {NULL, NULL, NULL, NULL, NULL}},
    {NULL, {NULL, NULL, NULL, NULL, NULL}}};

void inv_collect_classes(inv_out *o, const inv_cfg *cfg)
{
    size_t c;

    for (c = 0u; CLASSES[c].class_name != NULL; c++) {
        char abs[INV_PATH_MAX];
        char path[INV_PATH_MAX];
        char key[ND_INV_KEY_MAX];
        char list[INV_LIST_MAX];
        namelist nl;
        int n;
        size_t i;

        if (!str_fmt(abs, sizeof abs, "/sys/class/%s", CLASSES[c].class_name) ||
            !inv_path(cfg, path, sizeof path, abs) ||
            !inv_key_fmt(o, key, sizeof key, "class.%s", CLASSES[c].class_name))
            continue;
        n = listdir_sorted(path, &nl);
        if (n == -1) {
            (void)inv_add(o, INV_CMP, key, "ABSENT");
            continue;
        }
        if (n == -2) {
            (void)inv_add(o, INV_CMP, key, "UNREADABLE(%d)", errno);
            continue;
        }
        if (joined(&nl, list, sizeof list))
            (void)inv_add(o, INV_CMP, key, "%s", list);
        for (i = 0u; i < nl.n; i++) {
            size_t a;

            for (a = 0u; a < 5u && CLASSES[c].attrs[a] != NULL; a++) {
                char val[256];
                char akey[ND_INV_KEY_MAX];

                if (!inv_key_fmt(o, akey, sizeof akey, "class.%s.%s.%s", CLASSES[c].class_name,
                                 nl.name[i], CLASSES[c].attrs[a]))
                    continue;
                if (read_attr(cfg, val, sizeof val, "/sys/class/%s/%s/%s", CLASSES[c].class_name,
                              nl.name[i], CLASSES[c].attrs[a]))
                    (void)inv_add(o, INV_CMP, akey, "%s", val);
                /* THE NUMBER, NOT A GUESS AT WHICH ONE IT IS. This said
                 * UNREADABLE(EACCES) for every errno that was not ENOENT,
                 * naming a permission failure it had not established --
                 * reproduced with a sysfs attribute that is a directory,
                 * where the read fails EISDIR and the record claimed EACCES.
                 * On the phone the equivalent is any show() returning -EIO,
                 * -ENODEV or -ENXIO, and because the record is INV_CMP a
                 * phone failing EIO and an emulator failing EACCES produced
                 * identical text and the divergence was invisible. errno is
                 * zeroed at the top of read_attr(), so a 0 here means the
                 * path did not fit rather than a stale value from an earlier
                 * call -- the same cause inv_collect_memory() already calls
                 * UNAVAILABLE(path). */
                else if (errno == ENOENT)
                    (void)inv_add(o, INV_CMP, akey, "ABSENT");
                else if (errno == 0)
                    (void)inv_add(o, INV_CMP, akey, "UNAVAILABLE(path)");
                else
                    (void)inv_add(o, INV_CMP, akey, "UNREADABLE(%d)", errno);
            }
        }
        namelist_free(&nl);
    }
}

/* THE WORKED EXAMPLE FOR WHY THIS TOOL EARNS ITS KEEP, and it was found by
 * the prototype on its first run rather than reasoned about.
 *
 * There is no /sys/class/cpufreq on this kernel. The tree is
 * /sys/devices/system/cpu/cpufreq/policy* and /sys/devices/system/cpu/cpu0/
 * cpufreq -- and the latter is ND_CPUFREQ_DIR, the ONLY path nd_cpufreq.c
 * ever opens.
 *
 * IT USED TO BE ABSENT UNDER QEMU AND THAT WAS THIS COLLECTOR'S WORKED
 * EXAMPLE: -M virt declares no operating points, so CPUFREQ_DT bound no
 * policy and a whole subsystem the phone has was missing from the emulator,
 * invisible to every other test in the tree. run_qemu.sh now appends
 * neodct/board/qemu/nd-virt-additions.dtsi to QEMU's generated device tree
 * and it is present on both machines -- which is why the TABLE is recorded
 * here as well as the directory. "Both have cpufreq" is a much weaker
 * statement than "both offer these five operating points", and after the
 * allowlist records for this surface went away it was the only statement
 * left.
 *
 * The string is recorded exactly as the kernel wrote it, trailing space and
 * all, because that is what nd_cpufreq_parse_table() is handed. It is NOT
 * sorted here: nd_cpufreq.c sorts because the rockchip driver emits OPP-table
 * order, so an ordering difference between the two machines is harmless to
 * the code and is still a difference somebody should have to write a line
 * about rather than one this tool quietly normalises away. */
void inv_collect_cpufreq(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char list[INV_LIST_MAX];
    char table[ND_INV_VAL_MAX];
    namelist nl;
    int n;

    if (inv_path(cfg, path, sizeof path, "/sys/devices/system/cpu/cpu0/cpufreq")) {
        struct stat st;

        (void)inv_add(o, INV_CMP, "cpufreq.cpu0", "%s",
                      (stat(path, &st) == 0) ? "present" : "ABSENT");
    }
    if (read_attr(cfg, table, sizeof table,
                  "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies"))
        (void)inv_add(o, INV_CMP, "cpufreq.available", "%s", table);
    else
        (void)inv_add(o, INV_CMP, "cpufreq.available", "ABSENT");
    if (!inv_path(cfg, path, sizeof path, "/sys/devices/system/cpu/cpufreq"))
        return;
    n = listdir_sorted(path, &nl);
    if (n == -1) {
        (void)inv_add(o, INV_CMP, "cpufreq.policies", "ABSENT");
        return;
    }
    if (n == -2) {
        (void)inv_add(o, INV_CMP, "cpufreq.policies", "UNREADABLE(%d)", errno);
        return;
    }
    if (joined(&nl, list, sizeof list))
        (void)inv_add(o, INV_CMP, "cpufreq.policies", "%s", list);
    namelist_free(&nl);
}

/* Finding 11 in record form. mtdram reports write=1 and nandsim write=2048,
 * and every LEB size, VID header offset and `ubinize -O` argument in the
 * update path is arithmetic over these five numbers. Keyed by NAME and not
 * by index, with the index recorded the other way round, because an index in
 * a device node name is not an identity -- measured, twice, in this
 * container. */
void inv_collect_mtd(inv_out *o, const inv_cfg *cfg)
{
    static const char *const GEOM[] = {"type", "size", "erasesize", "writesize", "oobsize", NULL};
    char path[INV_PATH_MAX];
    namelist nl;
    int n;
    size_t i;

    if (!inv_path(cfg, path, sizeof path, "/sys/class/mtd"))
        return;
    n = listdir_sorted(path, &nl);
    if (n < 0)
        return;
    for (i = 0u; i < nl.n; i++) {
        char name[256];
        char key[ND_INV_KEY_MAX];
        size_t g;

        /* mtdNro is the read-only alias of mtdN and carries the same name.
         * Skipping it keeps the by-name index a function rather than a
         * relation; the alias itself is still visible in class.mtd. */
        if (strstr(nl.name[i], "ro") != NULL && strlen(nl.name[i]) > 4u)
            continue;
        if (!read_attr(cfg, name, sizeof name, "/sys/class/mtd/%s/name", nl.name[i]))
            continue;
        if (inv_key_fmt(o, key, sizeof key, "mtd.node.%s.name", nl.name[i]))
            (void)inv_add(o, INV_CMP, key, "%s", name);
        for (g = 0u; GEOM[g] != NULL; g++) {
            char val[128];

            if (!inv_key_fmt(o, key, sizeof key, "mtd.byname.\"%s\".%s", name, GEOM[g]))
                continue;
            if (read_attr(cfg, val, sizeof val, "/sys/class/mtd/%s/%s", nl.name[i], GEOM[g]))
                (void)inv_add(o, INV_CMP, key, "%s", val);
            else
                (void)inv_add(o, INV_CMP, key, "ABSENT");
        }
    }
    namelist_free(&nl);
}

/* Where the two machines have already cost the project a shipped feature:
 * every UBI number here is derived from the MTD geometry above, so an
 * emulator on mtdram computes volume sizes the phone will never compute. */
void inv_collect_ubi(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    namelist nl;
    int n;
    size_t i;

    if (!inv_path(cfg, path, sizeof path, "/sys/class/ubi"))
        return;
    n = listdir_sorted(path, &nl);
    if (n < 0)
        return;
    for (i = 0u; i < nl.n; i++) {
        static const char *const DEV_ATTRS[] = {"eraseblock_size", "min_io_size", "volumes_count",
                                                NULL};
        static const char *const VOL_ATTRS[] = {"type", "data_bytes", NULL};
        char key[ND_INV_KEY_MAX];
        char val[128];
        size_t a;

        if (strchr(nl.name[i], '_') != NULL) {
            /* A volume: ubi0_0. Keyed by the volume's NAME, because the
             * index is assigned in creation order by ubinize and the name is
             * what the cmdline and the initramfs actually ask for. */
            char vname[128];

            if (!read_attr(cfg, vname, sizeof vname, "/sys/class/ubi/%s/name", nl.name[i]))
                continue;
            for (a = 0u; VOL_ATTRS[a] != NULL; a++) {
                if (!inv_key_fmt(o, key, sizeof key, "ubi.vol.\"%s\".%s", vname, VOL_ATTRS[a]))
                    continue;
                if (read_attr(cfg, val, sizeof val, "/sys/class/ubi/%s/%s", nl.name[i],
                              VOL_ATTRS[a]))
                    (void)inv_add(o, INV_CMP, key, "%s", val);
            }
            continue;
        }
        for (a = 0u; DEV_ATTRS[a] != NULL; a++) {
            if (!inv_key_fmt(o, key, sizeof key, "ubi.%s.%s", nl.name[i], DEV_ATTRS[a]))
                continue;
            if (read_attr(cfg, val, sizeof val, "/sys/class/ubi/%s/%s", nl.name[i], DEV_ATTRS[a]))
                (void)inv_add(o, INV_CMP, key, "%s", val);
        }
    }
    namelist_free(&nl);
}

/* ---- /dev, and the family collapse ---------------------------------- */

typedef struct {
    char name[128];
    char mode[8];
    char owner[96];
    char type;
} devnode;

static char type_char(mode_t m)
{
    if (S_ISCHR(m))
        return 'c';
    if (S_ISBLK(m))
        return 'b';
    if (S_ISDIR(m))
        return 'd';
    if (S_ISLNK(m))
        return 'l';
    if (S_ISSOCK(m))
        return 's';
    if (S_ISFIFO(m))
        return 'p';
    return 'f';
}

/* ---- names for uid and gid, read out of the image's own files ------- *
 *
 * NOT getpwuid(3), and the reason is measured rather than stylistic. The
 * first probe capture from the real kernel came back with every owner as
 * `uid=0(?) gid=0(?)`: the cross-compiled build is statically linked against
 * glibc, whose getpwuid() needs libnss_files.so.2 AT RUNTIME and silently
 * returns NULL without it. That is a READER difference -- the same machine
 * read by two builds of the same tool -- and reporting one of those as a
 * machine difference is the exact failure this whole file exists to avoid.
 *
 * Reading /etc/passwd and /etc/group directly also makes the lookup honest in
 * two other ways: it follows --root, so a unit test can put a synthetic
 * users table in front of it, and it reads the SAME files nd_priv_lookup()
 * reads, so the name this records is the name the phone's own privilege drop
 * would find.
 *
 * The table is loaded once. The 622 nodes the committed capture counts would
 * otherwise mean 1,244 file reads for an answer that cannot change during a
 * capture. */

#define INV_IDS_MAX 128

typedef struct {
    unsigned long id;
    char name[40];
} idname;

typedef struct {
    idname ent[INV_IDS_MAX];
    size_t n;
    bool loaded;
} idtable;

static void idtable_load(idtable *t, const inv_cfg *cfg, const char *file)
{
    char path[INV_PATH_MAX];
    char line[512];
    FILE *f;

    t->loaded = true;
    t->n = 0u;
    if (!inv_path(cfg, path, sizeof path, file))
        return;
    f = fopen(path, "re");
    if (f == NULL)
        return;
    while (fgets(line, (int)sizeof line, f) != NULL && t->n < INV_IDS_MAX) {
        char name[40];
        unsigned long id;
        char *save = NULL;
        char *nm;
        char *field;

        trim(line);
        nm = strtok_r(line, ":", &save);
        if (nm == NULL || !str_copy(name, sizeof name, nm))
            continue;
        field = strtok_r(NULL, ":", &save); /* the password placeholder */
        if (field == NULL)
            continue;
        field = strtok_r(NULL, ":", &save); /* uid, or gid in /etc/group */
        if (field == NULL)
            continue;
        id = strtoul(field, NULL, 10);
        t->ent[t->n].id = id;
        (void)str_copy(t->ent[t->n].name, sizeof t->ent[t->n].name, name);
        t->n++;
    }
    (void)fclose(f);
}

static const char *idtable_name(idtable *t, const inv_cfg *cfg, const char *file, unsigned long id)
{
    size_t i;

    if (!t->loaded)
        idtable_load(t, cfg, file);
    for (i = 0u; i < t->n; i++) {
        if (t->ent[i].id == id)
            return t->ent[i].name;
    }
    return "?";
}

/* uid/gid as BOTH the number and the name. The kernel enforces the number;
 * users-table.txt asks for the name; a disagreement between them across two
 * machines is precisely the class of bug this project keeps finding, and it
 * is invisible unless both are on the page. "1000(?)" where the name does
 * not resolve, which is itself a fact worth a diff. */
static void owner_text(const inv_cfg *cfg, idtable *users, idtable *groups, uid_t uid, gid_t gid,
                       char *out, size_t n)
{
    (void)snprintf(out, n, "uid=%lu(%s) gid=%lu(%s)", (unsigned long)uid,
                   idtable_name(users, cfg, "/etc/passwd", (unsigned long)uid), (unsigned long)gid,
                   idtable_name(groups, cfg, "/etc/group", (unsigned long)gid));
}

static void emit_dev_record(inv_out *o, const devnode *d)
{
    char key[ND_INV_KEY_MAX];

    if (!inv_key_fmt(o, key, sizeof key, "dev.%s", d->name))
        return;
    (void)inv_add(o, INV_CMP, key, "type=%c mode=%s %s", d->type, d->mode, d->owner);
}

static bool same_shape(const devnode *a, const devnode *b)
{
    return a->type == b->type && strcmp(a->mode, b->mode) == 0 && strcmp(a->owner, b->owner) == 0;
}

/* The family record ASSERTS HOMOGENEITY. It carries the mode and owner shared
 * by the family, and any member that differs is broken out as its own record
 * and excluded from the count -- so one wrong mode can never hide inside a
 * family of 336. The count is part of the record, so a family that swallows a
 * different number of nodes is a diff too.
 *
 * THE REFERENCE IS THE MAJORITY AND NOT THE FIRST MEMBER, which is worth the
 * quadratic scan over at most a few hundred nodes. Taking the first would
 * mean that one wrong mode on ptya0 broke all 335 of its siblings out into
 * their own records: a single-node fault would arrive as a three-hundred-line
 * diff and the one line that mattered would be unfindable in it.
 *
 * The label carries no range. The design sketch said `dev.tty[0-63]`; the
 * range is already in `n=` and a label that also encoded it would have to be
 * hand-edited every time CONFIG_VT's maximum moved, which makes the label a
 * second place to get the same fact wrong. */
static void emit_family(inv_out *o, const char *family, const devnode *members, size_t n)
{
    char key[ND_INV_KEY_MAX];
    size_t i;
    size_t j;
    size_t best = 0u;
    size_t best_count = 0u;
    size_t homogeneous = 0u;
    const devnode *ref;

    for (i = 0u; i < n; i++) {
        size_t count = 0u;

        for (j = 0u; j < n; j++) {
            if (same_shape(&members[i], &members[j]))
                count++;
        }
        if (count > best_count) {
            best_count = count;
            best = i;
        }
    }
    ref = &members[best];
    for (i = 0u; i < n; i++) {
        if (same_shape(&members[i], ref))
            homogeneous++;
        else
            emit_dev_record(o, &members[i]);
    }
    if (!inv_key_fmt(o, key, sizeof key, "dev.%s", family))
        return;
    (void)inv_add(o, INV_CMP, key, "n=%zu type=%c mode=%s %s", homogeneous, ref->type, ref->mode,
                  ref->owner);
}

typedef struct {
    devnode *node;
    size_t n;
    size_t cap;
} devlist;

static bool devlist_push(devlist *dl, const devnode *d)
{
    if (dl->n == dl->cap) {
        size_t cap = (dl->cap == 0u) ? 128u : dl->cap * 2u;
        /* owned by the devlist; freed at the end of inv_collect_dev() */
        devnode *grown = realloc(dl->node, cap * sizeof *grown);

        if (grown == NULL)
            return false;
        dl->node = grown;
        dl->cap = cap;
    }
    dl->node[dl->n++] = *d;
    return true;
}

static void walk_dev(const inv_cfg *cfg, const char *rel, unsigned int depth, devlist *dl,
                     idtable *users, idtable *groups)
{
    char abs[INV_PATH_MAX];
    char path[INV_PATH_MAX];
    namelist nl;
    int n;
    size_t i;

    if (depth > 3u)
        return;
    if (!str_fmt(abs, sizeof abs, "/dev%s%s", rel[0] != '\0' ? "/" : "", rel) ||
        !inv_path(cfg, path, sizeof path, abs))
        return;
    n = listdir_sorted(path, &nl);
    if (n < 0)
        return;
    for (i = 0u; i < nl.n; i++) {
        devnode d;
        char child[INV_PATH_MAX];
        char full[INV_PATH_MAX];
        struct stat st;

        if (!str_fmt(child, sizeof child, "%s%s%s", rel, rel[0] != '\0' ? "/" : "", nl.name[i]))
            continue;
        if (!str_fmt(full, sizeof full, "%s/%s", path, nl.name[i]))
            continue;
        if (lstat(full, &st) != 0)
            continue;
        if (!str_copy(d.name, sizeof d.name, child))
            continue;
        inv_mode_octal((unsigned int)st.st_mode, d.mode);
        owner_text(cfg, users, groups, st.st_uid, st.st_gid, d.owner, sizeof d.owner);
        d.type = type_char(st.st_mode);
        (void)devlist_push(dl, &d);
        /* /dev/pts and /dev/shm hold somebody's runtime state -- open ptys
         * and whatever a process mapped -- not a property of the machine.
         * The DIRECTORY is recorded; its contents are not. */
        if (S_ISDIR(st.st_mode) && strcmp(nl.name[i], "pts") != 0 &&
            strcmp(nl.name[i], "shm") != 0 && strcmp(nl.name[i], "mqueue") != 0)
            walk_dev(cfg, child, depth + 1u, dl, users, groups);
    }
    namelist_free(&nl);
}

void inv_collect_dev(inv_out *o, const inv_cfg *cfg)
{
    devlist dl = {NULL, 0u, 0u};
    idtable users;
    idtable groups;
    size_t i;

    users.loaded = false;
    users.n = 0u;
    groups.loaded = false;
    groups.n = 0u;
    walk_dev(cfg, "", 0u, &dl, &users, &groups);
    if (dl.n == 0u) {
        (void)inv_add(o, INV_CMP, "dev.count", "ABSENT");
        return;
    }
    (void)inv_add(o, INV_CMP, "dev.count", "%zu", dl.n);
    for (i = 0u; i < dl.n;) {
        const char *fam = inv_dev_family(dl.node[i].name);
        size_t j;

        if (fam == NULL) {
            emit_dev_record(o, &dl.node[i]);
            i++;
            continue;
        }
        /* The list is sorted, so a family's members are contiguous only when
         * their names sort together -- which they do for every family in the
         * table (one prefix each). Scanning forward for members of the SAME
         * family rather than assuming contiguity keeps that from becoming a
         * silent assumption. */
        for (j = i + 1u; j < dl.n; j++) {
            const char *f2 = inv_dev_family(dl.node[j].name);

            if (f2 == NULL || strcmp(f2, fam) != 0)
                break;
        }
        emit_family(o, fam, &dl.node[i], j - i);
        i = j;
    }
    free(dl.node);
}

/* ---- mounts --------------------------------------------------------- */

/* THE KEY IS THE MOUNT POINT, AND A MOUNT POINT IS NOT UNIQUE. /proc/mounts
 * legitimately carries several lines with the same one -- an over-mount, a
 * self-bind, a devpts or a tmpfs mounted twice -- and this keyed on the point
 * alone, so on such a machine the tool emitted the same key twice and
 * parity_diff.py refused the whole capture. Measured on an ordinary
 * container: /dev/pts and /dev/shm each appeared twice.
 *
 * So the OCCURRENCE is part of the key when a point repeats: `mount./dev/pts`
 * and then `mount./dev/pts#2`. The plain key keeps its meaning for the
 * overwhelmingly common case, the second mount stops being invisible, and a
 * machine that gains or loses an over-mount is a diff rather than a refusal.
 * Dropping the later lines was the other candidate and is worse: the record
 * that decides what is actually on top of a directory would be the one
 * discarded. */
void inv_collect_mounts(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char line[INV_LIST_MAX];
    namelist seen = {NULL, 0u, 0u};
    FILE *f;

    if (!inv_path(cfg, path, sizeof path, "/proc/mounts")) {
        (void)inv_add(o, INV_CMP, "mount.count", "UNREADABLE(path)");
        return;
    }
    f = fopen(path, "re");
    if (f == NULL) {
        (void)inv_add(o, INV_CMP, "mount.count", "ABSENT");
        return;
    }
    while (fgets(line, (int)sizeof line, f) != NULL) {
        char src[256];
        char point[256];
        char fstype[64];
        char opts[512];
        char masked[512];
        char key[ND_INV_KEY_MAX];
        size_t nth = 1u;
        size_t i;

        if (sscanf(line, "%255s %255s %63s %511s", src, point, fstype, opts) != 4)
            continue;
        if (!inv_mask_mount_options(opts, masked, sizeof masked))
            continue;
        /* namelist holds 64-byte names; a longer point simply never matches a
         * previous one, which costs an unqualified duplicate key that inv_add()
         * then names and counts. That is the loud failure, not the silent one. */
        for (i = 0u; i < seen.n; i++) {
            if (strcmp(seen.name[i], point) == 0)
                nth++;
        }
        (void)namelist_push(&seen, point);
        if (nth == 1u) {
            if (!inv_key_fmt(o, key, sizeof key, "mount.%s", point))
                continue;
        } else if (!inv_key_fmt(o, key, sizeof key, "mount.%s#%zu", point, nth)) {
            continue;
        }
        /* The SOURCE is not recorded. It is a device node whose name is an
         * enumeration index on one machine (/dev/vda) and a UBI volume on
         * the other, and the same fact is already recorded by identity in
         * block.byserial and ubi.vol. */
        (void)inv_add(o, INV_CMP, key, "fstype=%s options=%s", fstype, masked);
    }
    (void)fclose(f);
    namelist_free(&seen);
}

/* ---- the framebuffer ioctls ----------------------------------------- */

/* THE RECORD THAT CANNOT BE TAKEN FROM A SHELL, and the reason this is a C
 * program. EMPIRICAL-FINDINGS 12: the panel divergence neodctDisplay.c:423
 * documents is the pixel format, and after force_mode() both machines read
 * back 240x175 bpp=32 line_len=960 with red at offset 0. That is the whole
 * Stage 3 claim in eleven numbers, and nothing is masked anywhere near it.
 *
 * O_RDONLY and two GET ioctls. Never a PUT: an instrument that changes the
 * machine it is measuring is not an instrument. */
void inv_collect_fb(inv_out *o, const inv_cfg *cfg)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    char path[INV_PATH_MAX];
    struct stat st;
    int fd;

    if (!inv_path(cfg, path, sizeof path, "/dev/fb0"))
        return;
    if (lstat(path, &st) != 0) {
        (void)inv_add(o, INV_CMP, "fb0", "ABSENT");
        return;
    }
    if (!S_ISCHR(st.st_mode)) {
        /* A synthetic --root tree has a regular file here at best. Saying so
         * is better than an ioctl error, because it names the reason. */
        (void)inv_add(o, INV_CMP, "fb0", "UNAVAILABLE(not-a-chardev)");
        return;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        (void)inv_add(o, INV_CMP, "fb0", "UNREADABLE(%d)", errno);
        return;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        (void)inv_add(o, INV_CMP, "fb0", "UNREADABLE(ioctl %d)", errno);
        (void)close(fd);
        return;
    }
    (void)close(fd);
    (void)inv_add(o, INV_CMP, "fb0", "present");
    (void)inv_add(o, INV_CMP, "fb0.var.xres", "%u", var.xres);
    (void)inv_add(o, INV_CMP, "fb0.var.yres", "%u", var.yres);
    (void)inv_add(o, INV_CMP, "fb0.var.xres_virtual", "%u", var.xres_virtual);
    (void)inv_add(o, INV_CMP, "fb0.var.yres_virtual", "%u", var.yres_virtual);
    (void)inv_add(o, INV_CMP, "fb0.var.bits_per_pixel", "%u", var.bits_per_pixel);
    (void)inv_add(o, INV_CMP, "fb0.var.red", "%u/%u", var.red.offset, var.red.length);
    (void)inv_add(o, INV_CMP, "fb0.var.green", "%u/%u", var.green.offset, var.green.length);
    (void)inv_add(o, INV_CMP, "fb0.var.blue", "%u/%u", var.blue.offset, var.blue.length);
    (void)inv_add(o, INV_CMP, "fb0.var.transp", "%u/%u", var.transp.offset, var.transp.length);
    (void)inv_add(o, INV_CMP, "fb0.fix.id", "%.16s", fix.id);
    (void)inv_add(o, INV_CMP, "fb0.fix.type", "%u", fix.type);
    (void)inv_add(o, INV_CMP, "fb0.fix.visual", "%u", fix.visual);
    (void)inv_add(o, INV_CMP, "fb0.fix.line_length", "%u", fix.line_length);
    (void)inv_add(o, INV_CMP, "fb0.fix.smem_len", "%u", fix.smem_len);
}

/* ---- os-release, and the platform record ---------------------------- */

static bool osrelease_field(const inv_cfg *cfg, const char *field, char *out, size_t n)
{
    char path[INV_PATH_MAX];
    char line[512];
    FILE *f;
    bool found = false;
    size_t flen = strlen(field);

    if (!inv_path(cfg, path, sizeof path, "/etc/os-release"))
        return false;
    f = fopen(path, "re");
    if (f == NULL)
        return false;
    while (fgets(line, (int)sizeof line, f) != NULL) {
        trim(line);
        if (strncmp(line, field, flen) != 0 || line[flen] != '=')
            continue;
        {
            const char *v = line + flen + 1u;
            size_t vlen;

            if (v[0] == '"')
                v++;
            vlen = strlen(v);
            if (vlen > 0u && v[vlen - 1u] == '"')
                vlen--;
            if (vlen < n) {
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                found = true;
            }
        }
        break;
    }
    (void)fclose(f);
    return found;
}

bool inv_os_version_id(const inv_cfg *cfg, char *out, size_t n)
{
    return osrelease_field(cfg, "VERSION_ID", out, n);
}

void inv_collect_os(inv_out *o, const inv_cfg *cfg)
{
    char val[256];

    /* ID and NAME go in the body; VERSION_ID does NOT. It is a precondition
     * in the preamble: comparing a 0.5.8b phone against a 0.6.0a emulator is
     * two unrelated observations rather than a parity test, and the right
     * answer is a refusal from the comparator, not a diff line. */
    (void)inv_add(o, INV_CMP, "os.id", "%s",
                  osrelease_field(cfg, "ID", val, sizeof val) ? val : "ABSENT");
    (void)inv_add(o, INV_CMP, "os.name", "%s",
                  osrelease_field(cfg, "NAME", val, sizeof val) ? val : "ABSENT");
}

/* /NeoDCT/platform, the RECORD half of D1/D2's discriminator. The COMPILED
 * half cannot be read from here -- it is inside libneodct -- and is added by
 * nd_inventory.c, which is the only reason that file exists as well as this
 * one. nd-platform's own header already documents what re-deriving costs:
 * "it will happily answer hw on an image the C has already refused to call
 * anything." */
void inv_collect_platform_record(inv_out *o, const inv_cfg *cfg)
{
    static const char *const FIELDS[] = {"platform", "board", "image", NULL};
    char path[INV_PATH_MAX];
    char line[512];
    FILE *f;
    size_t i;
    bool seen[3] = {false, false, false};

    if (!inv_path(cfg, path, sizeof path, "/NeoDCT/platform"))
        return;
    f = fopen(path, "re");
    if (f != NULL) {
        while (fgets(line, (int)sizeof line, f) != NULL) {
            trim(line);
            for (i = 0u; FIELDS[i] != NULL; i++) {
                size_t flen = strlen(FIELDS[i]);
                char key[ND_INV_KEY_MAX];

                if (strncmp(line, FIELDS[i], flen) != 0 || line[flen] != '=')
                    continue;
                if (inv_key_fmt(o, key, sizeof key, "platform.record.%s", FIELDS[i])) {
                    (void)inv_add(o, INV_CMP, key, "%s", line + flen + 1u);
                    seen[i] = true;
                }
            }
        }
        (void)fclose(f);
    }
    for (i = 0u; FIELDS[i] != NULL; i++) {
        char key[ND_INV_KEY_MAX];

        if (seen[i])
            continue;
        if (inv_key_fmt(o, key, sizeof key, "platform.record.%s", FIELDS[i]))
            (void)inv_add(o, INV_CMP, key, "ABSENT");
    }
}

/* ---- /proc, the cheap records that explain the expensive ones -------- */

/* One name per line, sorted, in brackets.
 *
 * `drop_nodev` exists for /proc/filesystems and is the reason this function
 * no longer takes a column NUMBER. That file has two shapes -- a virtual
 * filesystem is "nodev\tsysfs" and a device-backed one is just "\text4" --
 * so asking for column 1 got the nodev entries AND NOTHING ELSE. Measured:
 * the record read [bdev devpts ... ubifs] on a kernel whose /proc/filesystems
 * also held ext2, ext3, ext4, squashfs and vfat, and the same string came
 * back after squashfs and ext4 were deleted from the file. The record's own
 * comment says it gates "squashfs for the verity root, ext4 for the user
 * partition", and those were exactly the two it could never contain: a kernel
 * built without either produced a byte-identical baseline. */
static void collect_token_per_line(inv_out *o, const inv_cfg *cfg, const char *file,
                                   const char *key, bool drop_nodev)
{
    char path[INV_PATH_MAX];
    char line[1024];
    char list[INV_LIST_MAX];
    FILE *f;
    size_t used = 0u;

    if (!inv_path(cfg, path, sizeof path, file))
        return;
    f = fopen(path, "re");
    if (f == NULL) {
        (void)inv_add(o, INV_CMP, key, "ABSENT");
        return;
    }
    list[0] = '\0';
    while (fgets(line, (int)sizeof line, f) != NULL) {
        char *save = NULL;
        char *tok = strtok_r(line, " \t\n", &save);
        int written;

        if (tok != NULL && drop_nodev && strcmp(tok, "nodev") == 0)
            tok = strtok_r(NULL, " \t\n", &save);
        if (tok == NULL || tok[0] == '\0')
            continue;
        written = snprintf(list + used, sizeof list - used, "%s%s", used > 0u ? " " : "", tok);
        if (written < 0 || (size_t)written >= sizeof list - used)
            break;
        used += (size_t)written;
    }
    (void)fclose(f);
    inv_sort_tokens(list);
    (void)inv_add(o, INV_CMP, key, "[%s]", list);
}

void inv_collect_proc(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char line[512];
    char chars[INV_LIST_MAX];
    char blocks[INV_LIST_MAX];
    FILE *f;
    struct stat st;

    /* Which filesystems the kernel can mount AT ALL, which gates the entire
     * image design: squashfs for the verity root, ext4 for the user
     * partition, ubifs for the phone's NAND. The nodev column is dropped
     * rather than selected on -- see collect_token_per_line(); selecting on
     * it is how this record came to hold neither squashfs nor ext4. */
    collect_token_per_line(o, cfg, "/proc/filesystems", "proc.filesystems", true);
    /* fbcon is gone from this kernel, so /proc/consoles lists ttyAMA0 alone
     * and ndsys-recovery.sh's text menu falls through to /dev/console. One
     * line that would have saved an afternoon. */
    collect_token_per_line(o, cfg, "/proc/consoles", "proc.consoles", false);
    collect_token_per_line(o, cfg, "/proc/modules", "proc.modules", false);

    /* The char and block major tables: the thing that explains a missing
     * node. Names only -- the major NUMBER is dynamic for most of them and
     * would churn between kernels for no reason anybody would act on. */
    chars[0] = '\0';
    blocks[0] = '\0';
    if (inv_path(cfg, path, sizeof path, "/proc/devices")) {
        f = fopen(path, "re");
        if (f != NULL) {
            char *into = NULL;
            size_t used_c = 0u;
            size_t used_b = 0u;

            while (fgets(line, (int)sizeof line, f) != NULL) {
                char name[128];
                unsigned int major;
                int written;
                size_t *used;

                trim(line);
                if (strcmp(line, "Character devices:") == 0) {
                    into = chars;
                    continue;
                }
                if (strcmp(line, "Block devices:") == 0) {
                    into = blocks;
                    continue;
                }
                if (into == NULL || sscanf(line, "%u %127s", &major, name) != 2)
                    continue;
                used = (into == chars) ? &used_c : &used_b;
                written = snprintf(into + *used, (size_t)INV_LIST_MAX - *used, "%s%s",
                                   *used > 0u ? " " : "", name);
                if (written < 0 || (size_t)written >= (size_t)INV_LIST_MAX - *used)
                    continue;
                *used += (size_t)written;
            }
            (void)fclose(f);
        }
    }
    inv_sort_tokens(chars);
    inv_sort_tokens(blocks);
    (void)inv_add(o, INV_CMP, "proc.devices.char", "[%s]", chars);
    (void)inv_add(o, INV_CMP, "proc.devices.block", "[%s]", blocks);

    /* /dev/mapper/control, and NOT `grep verity /proc/devices`: verity is a
     * DM target rather than a device, so the obvious check reports failure
     * on a working kernel. EMPIRICAL-FINDINGS says so in as many words. */
    if (inv_path(cfg, path, sizeof path, "/dev/mapper/control"))
        (void)inv_add(o, INV_CMP, "dm.control", "%s",
                      (lstat(path, &st) == 0) ? "present" : "ABSENT");

    /* S16zram asks for a 28 MB compressed swap and, without the symbol,
     * exits 0 in silence -- the emulator quietly getting MORE headroom than
     * the phone, which is the memory pressure this whole branch exists to
     * reproduce going missing. */
    {
        char val[64];

        (void)inv_add(o, INV_CMP, "zram0.disksize", "%s",
                      read_attr(cfg, val, sizeof val, "/sys/block/zram0/disksize") ? val
                                                                                   : "ABSENT");
    }
}

/* THE RECORD THAT MAKES FINDING 10 IMPOSSIBLE TO MISS AGAIN. A virtio_input
 * silently refused for want of VIRTIO_F_VERSION_1 produces NO LINE HERE at
 * all -- -ENODEV from a probe prints nothing at any loglevel -- so the diff
 * is a whole missing device rather than a warning nobody saw.
 *
 * Both directions are recorded. The index is not an identity (measured: the
 * keyboard given FIRST on the command line enumerated SECOND), but it is
 * load-bearing, because ND_PATH_KEYPAD is the literal string
 * /dev/input/event0. So identity->node and node->identity both go in, and
 * "event0 is a tablet here and an i2c keypad there" becomes a line in the
 * allowlist instead of an afternoon on the bench. */
void inv_collect_input(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char line[1024];
    FILE *f;
    char name[256] = "";
    char handlers[512] = "";
    char ev[64] = "";
    char key[ND_INV_KEY_MAX];

    if (!inv_path(cfg, path, sizeof path, "/proc/bus/input/devices"))
        return;
    f = fopen(path, "re");
    if (f == NULL) {
        (void)inv_add(o, INV_CMP, "input.devices", "ABSENT");
        return;
    }
    while (fgets(line, (int)sizeof line, f) != NULL) {
        trim(line);
        if (strncmp(line, "N: Name=", 8u) == 0) {
            const char *v = line + 8;
            size_t vlen;

            if (v[0] == '"')
                v++;
            vlen = strlen(v);
            if (vlen > 0u && v[vlen - 1u] == '"')
                vlen--;
            if (vlen < sizeof name) {
                memcpy(name, v, vlen);
                name[vlen] = '\0';
            }
        } else if (strncmp(line, "H: Handlers=", 12u) == 0) {
            (void)str_copy(handlers, sizeof handlers, line + 12);
            inv_sort_tokens(handlers);
        } else if (strncmp(line, "B: EV=", 6u) == 0) {
            (void)str_copy(ev, sizeof ev, line + 6);
        } else if (line[0] == '\0' && name[0] != '\0') {
            char *tok;
            char *save = NULL;
            char scan[512];

            if (inv_key_fmt(o, key, sizeof key, "input.byname.\"%s\".handlers", name))
                (void)inv_add(o, INV_CMP, key, "[%s]", handlers);
            if (inv_key_fmt(o, key, sizeof key, "input.byname.\"%s\".ev", name))
                (void)inv_add(o, INV_CMP, key, "%s", ev);
            (void)str_copy(scan, sizeof scan, handlers);
            for (tok = strtok_r(scan, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save)) {
                if (strncmp(tok, "event", 5u) != 0)
                    continue;
                if (inv_key_fmt(o, key, sizeof key, "input.node.%s.name", tok))
                    (void)inv_add(o, INV_CMP, key, "%s", name);
            }
            name[0] = '\0';
            handlers[0] = '\0';
            ev[0] = '\0';
        }
    }
    (void)fclose(f);
}

/* Measured, and it is the reason the by-identity keys above exist: reorder
 * the -drive arguments and vda/vdb swap. So the SERIAL is the identity and
 * the node is an attribute of it, recorded both ways round. */
void inv_collect_block(inv_out *o, const inv_cfg *cfg)
{
    static const char *const ATTRS[] = {"size", "ro", "removable", NULL};
    char path[INV_PATH_MAX];
    namelist nl;
    int n;
    size_t i;

    if (!inv_path(cfg, path, sizeof path, "/sys/class/block"))
        return;
    n = listdir_sorted(path, &nl);
    if (n < 0)
        return;
    for (i = 0u; i < nl.n; i++) {
        char key[ND_INV_KEY_MAX];
        char val[128];
        char serial[128];
        char masked[128];
        size_t a;

        for (a = 0u; ATTRS[a] != NULL; a++) {
            if (!inv_key_fmt(o, key, sizeof key, "block.%s.%s", nl.name[i], ATTRS[a]))
                continue;
            if (read_attr(cfg, val, sizeof val, "/sys/class/block/%s/%s", nl.name[i], ATTRS[a]))
                (void)inv_add(o, INV_CMP, key, "%s", val);
        }
        if (!read_attr(cfg, serial, sizeof serial, "/sys/class/block/%s/serial", nl.name[i]) &&
            !read_attr(cfg, serial, sizeof serial, "/sys/class/block/%s/device/serial", nl.name[i]))
            continue;
        if (!inv_mask_serial(serial, masked, sizeof masked))
            continue;
        if (inv_key_fmt(o, key, sizeof key, "block.%s.serial", nl.name[i]))
            (void)inv_add(o, INV_CMP, key, "%s", masked);
        /* THE REVERSE INDEX ONLY EXISTS FOR OUR OWN NAMES, and that is the
         * same argument inv_collect_mtd() already makes about mtdNro: a key
         * has to be a function of the machine. inv_mask_serial() maps every
         * foreign serial of one length onto the single token <serial:N> by
         * construction, so N disks with equal-length serials collapse onto
         * one key -- measured on an ordinary container, five virtio disks all
         * claiming block.byserial.<serial:14>.node, a capture the comparator
         * then refused outright. A many-to-one index is not an index anyway:
         * the record could only ever name whichever disk happened to be
         * enumerated last. A serial kept VERBATIM is one of ND(SYS|USER|CARD|
         * APPLY), which is unique by design, and those are the only ones the
         * initramfs ever looks a partition up by. */
        if (strcmp(masked, serial) != 0)
            continue;
        if (inv_key_fmt(o, key, sizeof key, "block.byserial.%s.node", masked))
            (void)inv_add(o, INV_CMP, key, "%s", nl.name[i]);
    }
    namelist_free(&nl);
}

/* The candidate AT ports, as the INPUT to nd_modem's decision rather than
 * as its output. This deliberately records the raw sorted ttyUSB* set from
 * /sys/class/tty and NOT nd_modem__candidate_ports(), which needs a live
 * nd_modem: constructing one starts a thread and the preference ordering it
 * applies is nd_modem's policy rather than a fact about the machine. The set
 * is the fact; the ordering belongs to the code being tested, not to the
 * instrument testing it. */
void inv_collect_modem_ports(inv_out *o, const inv_cfg *cfg)
{
    char path[INV_PATH_MAX];
    char list[INV_LIST_MAX];
    namelist nl;
    namelist usb = {NULL, 0u, 0u};
    int n;
    size_t i;

    if (!inv_path(cfg, path, sizeof path, "/sys/class/tty"))
        return;
    n = listdir_sorted(path, &nl);
    if (n < 0) {
        (void)inv_add(o, INV_CMP, "modem.candidate_ports", "ABSENT");
        return;
    }
    for (i = 0u; i < nl.n; i++) {
        if (strncmp(nl.name[i], "ttyUSB", 6u) == 0)
            (void)namelist_push(&usb, nl.name[i]);
    }
    namelist_free(&nl);
    if (joined(&usb, list, sizeof list))
        (void)inv_add(o, INV_CMP, "modem.candidate_ports", "%s", list);
    namelist_free(&usb);
}

/* ------------------------------------------------------------------ *
 * The section list, in ONE place, exactly as nd_selftest.c keeps its own
 * ------------------------------------------------------------------ */

const char *const INV_SECTIONS[] = {"kernel", "memory", "cmdline", "class", "cpufreq", "mtd",
                                    "ubi",    "dev",    "mount",   "fb",    "os",      "proc",
                                    "input",  "block",  "modem",   NULL};

bool inv_section_wanted(const char *const *sections, const char *name)
{
    size_t i;

    if (sections == NULL)
        return true;
    for (i = 0u; sections[i] != NULL; i++) {
        if (strcmp(sections[i], name) == 0)
            return true;
    }
    return false;
}

void inv_collect(inv_out *o, const inv_cfg *cfg, const char *const *sections)
{
    if (inv_section_wanted(sections, "kernel"))
        inv_collect_kernel(o, cfg);
    if (inv_section_wanted(sections, "memory"))
        inv_collect_memory(o, cfg);
    if (inv_section_wanted(sections, "cmdline"))
        inv_collect_cmdline(o, cfg);
    if (inv_section_wanted(sections, "class"))
        inv_collect_classes(o, cfg);
    if (inv_section_wanted(sections, "cpufreq"))
        inv_collect_cpufreq(o, cfg);
    if (inv_section_wanted(sections, "mtd"))
        inv_collect_mtd(o, cfg);
    if (inv_section_wanted(sections, "ubi"))
        inv_collect_ubi(o, cfg);
    if (inv_section_wanted(sections, "dev"))
        inv_collect_dev(o, cfg);
    if (inv_section_wanted(sections, "mount"))
        inv_collect_mounts(o, cfg);
    if (inv_section_wanted(sections, "fb"))
        inv_collect_fb(o, cfg);
    if (inv_section_wanted(sections, "os")) {
        inv_collect_os(o, cfg);
        inv_collect_platform_record(o, cfg);
    }
    if (inv_section_wanted(sections, "proc"))
        inv_collect_proc(o, cfg);
    if (inv_section_wanted(sections, "input"))
        inv_collect_input(o, cfg);
    if (inv_section_wanted(sections, "block"))
        inv_collect_block(o, cfg);
    if (inv_section_wanted(sections, "modem"))
        inv_collect_modem_ports(o, cfg);
}
