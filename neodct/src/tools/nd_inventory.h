/* nd_inventory.h -- the inventory's private layer: the record buffer, the
 * canonicalisers, and the collectors that fill one.
 *
 * IT LIVES IN tools/ AND NOT IN include/, for the reason nd_bootbar.h and
 * nd_bootfb.h do: nothing in libneodct may depend on it. The direction of the
 * dependency is the whole point -- the inventory reads the library, the
 * library must never learn to read the inventory, or a record format designed
 * to be diffed in a pull request acquires callers on the phone. Exactly two
 * translation units get -Itools: nd_inventory.c and test_inventory.c.
 *
 * IT INCLUDES NOTHING OF OURS, AND THAT IS LOAD-BEARING RATHER THAN TIDY.
 * Everything below is plain C11 plus POSIX, so nd_inventory_collect.c can be
 * cross-compiled on its own against a bare toolchain -- which is how the
 * committed QEMU-side baseline was captured, in a busybox initramfs on the
 * repo's own kernel, months before there is a Buildroot image to capture from.
 * A single #include "nd_types.h" here would have made that impossible and
 * would have been discovered only by somebody trying it. The library-derived
 * records (the compiled platform constant, the modem's cold verdict) are
 * collected in nd_inventory.c, on the other side of that line, and are the
 * only records this file cannot produce.
 *
 * On the split between the two .c files: nd_inventory.c holds main(), the
 * framing and the six library facts; nd_inventory_collect.c holds every
 * collector and every canonicaliser. That is the split BOOTBAR_LIB_OBJS
 * already uses, and for the same reason -- a unit test cannot link a file
 * with a main() in it.
 */

#ifndef ND_INVENTORY_H_INCLUDED
#define ND_INVENTORY_H_INCLUDED

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The format version, in the preamble of every capture and in the host test.
 * A capture whose format does not match the tool's is not old data, it is
 * data whose masks and family table are unknown -- parity_diff.py refuses it
 * rather than diffing two different questions. */
#define ND_INV_FORMAT 1

/* ND_INV_KEY_MAX is generous because the longest keys are sysfs attribute
 * paths ("class.backlight.backlight.max_brightness") and a truncated key is a
 * SILENT diff -- two different records collapsing onto one string. Truncation
 * is refused rather than trimmed everywhere below. */
#define ND_INV_KEY_MAX 192
#define ND_INV_VAL_MAX 1024

/* The column right after the sentinel. A compared record has nothing there;
 * an informational one has a '~'. It is a COLUMN and not part of the key:
 * both sort on the key alone, so `~mem.total_kb` sits beside `mem.total_mib`
 * where a reader looking at one will see the other. Writing the tilde into
 * the key instead would sort every informational record to the bottom of the
 * file, which is precisely where nobody reads it. */
typedef enum {
    INV_CMP = 0, /* compared: a difference here is a difference */
    INV_RAW = 1  /* recorded, never compared: context for a human */
} inv_mark;

typedef struct inv_out inv_out;

/* ------------------------------------------------------------------ *
 * The record buffer
 * ------------------------------------------------------------------ */

inv_out *inv_out_new(void);
void inv_out_free(inv_out *o);

/* Returns false on OOM or on a key/value that would have to be truncated.
 * The caller's contract is that a false here makes the capture INCOMPLETE
 * (exit 1) rather than shorter: a record that silently did not fit is the one
 * failure mode a byte-stable format cannot survive.
 *
 * The key is normalised on the way in: whitespace, control bytes and the
 * backslash itself become \xNN, so a key built out of a device's own name is
 * still one token on a line whose separator is a space. See escape_key() for
 * what that cost before it was there.
 *
 * A DUPLICATE KEY IS ALSO A FALSE, for the same reason and with the same
 * consequence. parity_diff.py refuses a whole capture that carries one, so a
 * tool that emitted duplicates and a comparator that rejected them disagreed
 * about what a legal capture is -- and the operator could only reconcile them
 * by hand-editing the artefact, which is the one thing a baseline may never
 * be. The two producers that could collide key by something that is a
 * function now; the check here is the backstop for the third. */
bool inv_add(inv_out *o, inv_mark mark, const char *key, const char *fmt, ...);

/* Builds a KEY by format into `dst` and, on truncation, does exactly what
 * inv_add() does with a key that does not fit: names it on stderr, counts it
 * in inv_dropped(), returns false. Every collector uses it, because a rule
 * enforced only where the key is USED is a rule bypassed everywhere the key
 * is BUILT -- which is what nineteen `if (!str_fmt(key, ...)) continue;`
 * sites were doing, silently and without moving a counter. */
bool inv_key_fmt(inv_out *o, char *dst, size_t n, const char *fmt, ...);

size_t inv_count(const inv_out *o);
/* How many inv_add() calls were refused. Non-zero means the capture is
 * incomplete and nd_inventory.c exits 1. */
size_t inv_dropped(const inv_out *o);

/* Sorts by key, byte order, as LC_ALL=C sort(1) would. Idempotent. */
void inv_sort(inv_out *o);

/* The body, "INV|" framed, newline terminated, sorted. Caller frees.
 * NULL on OOM. */
char *inv_render(const inv_out *o);

/* The compared subset only, same framing, for the second hash. Caller frees. */
char *inv_render_compared(const inv_out *o);

/* ------------------------------------------------------------------ *
 * Canonicalisers -- pure functions over text. Everything here is unit
 * tested in test/unit/test_inventory.c, because they are the difference
 * between a harness that reports machine differences and one that reports
 * reader differences, and there is no excuse for not testing a pure
 * function over a string.
 * ------------------------------------------------------------------ */

/* Sorts a space-separated token list in place, byte order. NOT locale order:
 * a locale-aware sort orders punctuation differently on musl and on the
 * build host's glibc, so the same machine read by two readers would produce
 * two files. */
void inv_sort_tokens(char *list);

/* A disk serial. Kept verbatim iff it is one of the project's own names --
 * ^ND(SYS|USER|CARD|APPLY)$ -- and otherwise reduced to <serial:N>, N being
 * its length. The length survives because ABSENT and "present but not one of
 * ours" are different facts and a mask that conflated them would hide a card
 * from another phone. Returns false only if `out` is too small. */
bool inv_mask_serial(const char *serial, char *out, size_t n);

/* MemTotal in kB -> MiB bucketed to the nearest 4. Bucketed rather than
 * masked: 53,824 kB against the phone's ~54 MB is the headline claim of this
 * whole branch and deleting it from the artefact deletes the evidence, while
 * comparing it verbatim guarantees one permanent allowlist entry that says
 * nothing. 4 MiB is ~7% of the budget, which is the size of divergence
 * anybody would act on. */
uint32_t inv_bucket_mib(uint64_t kb);

/* A /proc/mounts option list: split, mask, sort, rejoin. Only size=,
 * nr_inodes= and blksize= are masked, because those are a restatement of
 * MemTotal (measured: rootfs and devtmpfs carry MemTotal/2 on both machines).
 * EVERYTHING ELSE IS VERBATIM -- ro, rw, nosuid, nodev, noexec, relatime,
 * mode=, errors= are the options nd-selftest's mount checks reason about and
 * a mask over them would hide the divergence this file exists to find. */
bool inv_mask_mount_options(const char *opts, char *out, size_t n);

/* st_mode -> "0660". Four octal digits, never symbolic: a symbolic mode
 * encodes setuid and sticky ambiguously and busybox's `ls` renders them
 * differently between applet builds, so the prototype shell capture was
 * already recording a reader difference as a machine difference. */
void inv_mode_octal(unsigned int mode, char out[8]);

/* The /dev family this node belongs to, or NULL. Measured on the repo's
 * kernel and readable off the committed capture: `dev.count 622`, 512 of them
 * the legacy BSD pty grid (`pty[a-z][0-f]` n=256 and `tty[a-z][0-f]` n=256).
 * Collapsing them is not cosmetic -- 512 records of identical mode and owner
 * is a file nobody reads, and a file nobody reads is not a review gate. The
 * numbers here used to be 617 and 336, which is how a reader sizing
 * ND_INV_VAL_MAX against them would have budgeted 50% low -- and running past
 * ND_INV_VAL_MAX is exactly how class.tty was silently dropped the first
 * time. */
const char *inv_dev_family(const char *name);

/* Is this kernel cmdline parameter's VALUE worth recording? The names are
 * always compared; the values are partition tables and device paths that
 * come from the U-Boot env on one machine and run_qemu.sh on the other.
 *
 * The POLICY SWITCHES are kept -- neodct.verity above all, which decides
 * whether the immutable-rootfs design is enforced at all. Its name alone is
 * identical whichever mode is set, so while only the name was compared a
 * phone left on `permissive` after recovery diffed clean against an enforcing
 * emulator. */
bool inv_cmdline_keep(const char *name);

/* SHA-256, because the capture comes back down a serial console that turns
 * LF into CRLF and interleaves printk, and a capture is only a baseline if
 * it arrived intact. Out is 65 bytes: 64 lowercase hex plus NUL. */
void inv_sha256_hex(const char *data, size_t len, char out[65]);

/* ------------------------------------------------------------------ *
 * The collectors
 * ------------------------------------------------------------------ */

typedef struct {
    /* "/" for a real capture. Anything else reprefixes /proc, /sys, /dev,
     * /etc and /NeoDCT so the collectors can run against a tree a unit test
     * built -- the hook shape nd-platform already uses with
     * NEODCT_PLATFORM_FILE and uistub.py uses for absolute paths.
     *
     * IT IS ALSO A WAY TO MANUFACTURE A PLAUSIBLE CAPTURE, so it is stamped
     * into the preamble as capture.root= and parity_diff.py refuses any
     * capture whose root is not "/" as a baseline or as either side of a
     * gating comparison. The guard is the comparator's, not the format's;
     * that is a real weakness and it is written down rather than hidden. */
    const char *root;
} inv_cfg;

/* Each collector adds its own records and never sorts: sorting is one
 * operation over the whole file, at the end, so a collector cannot make the
 * order depend on the order collectors ran in. */
void inv_collect_kernel(inv_out *o, const inv_cfg *cfg);
void inv_collect_memory(inv_out *o, const inv_cfg *cfg);
void inv_collect_cmdline(inv_out *o, const inv_cfg *cfg);
void inv_collect_classes(inv_out *o, const inv_cfg *cfg);
void inv_collect_cpufreq(inv_out *o, const inv_cfg *cfg);
void inv_collect_mtd(inv_out *o, const inv_cfg *cfg);
void inv_collect_ubi(inv_out *o, const inv_cfg *cfg);
void inv_collect_dev(inv_out *o, const inv_cfg *cfg);
void inv_collect_mounts(inv_out *o, const inv_cfg *cfg);
void inv_collect_fb(inv_out *o, const inv_cfg *cfg);
void inv_collect_os(inv_out *o, const inv_cfg *cfg);
void inv_collect_proc(inv_out *o, const inv_cfg *cfg);
void inv_collect_input(inv_out *o, const inv_cfg *cfg);
void inv_collect_block(inv_out *o, const inv_cfg *cfg);
void inv_collect_modem_ports(inv_out *o, const inv_cfg *cfg);
void inv_collect_platform_record(inv_out *o, const inv_cfg *cfg);

/* The sections, in one list in one place, exactly as nd_selftest.c keeps
 * its own. NULL terminated. */
extern const char *const INV_SECTIONS[];

/* Runs every named section, or all of them when `sections` is NULL. */
void inv_collect(inv_out *o, const inv_cfg *cfg, const char *const *sections);

/* Exported because the six library-derived records live in nd_inventory.c, on
 * the other side of the libneodct line, and they belong to sections like
 * everything else. Two copies of this predicate would be two answers to
 * `nd-inventory modem` the first time somebody edited one of them. */
bool inv_section_wanted(const char *const *sections, const char *name);

/* VERSION_ID out of /etc/os-release, for the preamble. It is a PRECONDITION
 * and not a record: comparing a 0.5.8b phone against a 0.6.0a emulator is
 * two unrelated observations rather than a parity test, so parity_diff.py
 * refuses the pair instead of printing the difference. Returns false if
 * there is no os-release to read. */
bool inv_os_version_id(const inv_cfg *cfg, char *out, size_t n);

/* Path helper, exposed because every collector and the test need the same
 * reprefixing. Returns false on truncation. */
bool inv_path(const inv_cfg *cfg, char *out, size_t n, const char *abs);

#endif /* ND_INVENTORY_H_INCLUDED */
