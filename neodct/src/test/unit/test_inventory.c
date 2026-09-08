/* test_inventory.c -- the canonicaliser, which is where the parity harness
 * succeeds or fails.
 *
 * The collectors are not the interesting half. Reading /sys/class/mtd is the
 * same eight lines everywhere and it either works or it does not. What
 * decides whether the artefact is worth committing is everything that happens
 * to the text afterwards, because ALL OF IT IS THE DIFFERENCE BETWEEN A
 * HARNESS THAT REPORTS MACHINE DIFFERENCES AND ONE THAT REPORTS READER
 * DIFFERENCES -- an unsorted directory listing, a locale-aware sort, a
 * symbolic mode, a mask that ate an option nd-selftest reasons about. Every
 * one of those produces a diff line that looks exactly like a real divergence
 * and is not, and after two or three of them the file stops being read.
 *
 * They are also pure functions over strings, so there is no excuse.
 *
 * The last case is the one that would be missed by hand: the same tree, built
 * in two different creation orders, must produce byte-identical output. That
 * is the property the whole design rests on and it cannot be established by
 * reading the code, because readdir order is a property of the filesystem and
 * not of the source.
 *
 * IT ALSO CANNOT BE ESTABLISHED BY COMPARING THE TWO CAPTURES TO EACH OTHER,
 * which is what it used to do and which was vacuous here. ext4 with dir_index
 * and tmpfs both return readdir entries in a hash of the NAME, so both trees
 * hand back the same order whatever order they were created in -- confirmed
 * by building the ten-name /dev tree forward and reversed under /tmp on this
 * box and getting the identical sequence from both. So the case asserts the
 * output against a FIXED STRING as well, and the two-order build stays as the
 * bonus rather than as the whole of it.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_inventory.h"

static int g_fail;
static int g_checks;

#define CHECK(cond, what)                                                          \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            (void)fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
            g_fail++;                                                              \
        }                                                                          \
    } while (0)

#define CHECK_STR(got, want, what)                                                                \
    do {                                                                                          \
        g_checks++;                                                                               \
        if (strcmp((got), (want)) != 0) {                                                         \
            (void)fprintf(stderr, "FAIL %s:%d  %s\n      got  '%s'\n      want '%s'\n", __FILE__, \
                          __LINE__, (what), (got), (want));                                       \
            g_fail++;                                                                             \
        }                                                                                         \
    } while (0)

/* ------------------------------------------------------------------ *
 * Scratch trees
 * ------------------------------------------------------------------ */

static char g_tmp[512];

static void tmpdir_make(char *out, size_t n, const char *tag)
{
    const char *base = getenv("NEODCT_SANDBOX_TMP");

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    (void)snprintf(out, n, "%s/inv-%s-%ld", base, tag, (long)getpid());
    (void)mkdir(base, 0700);
    if (mkdir(out, 0700) != 0 && errno != EEXIST)
        (void)fprintf(stderr, "test_inventory: cannot make %s: %s\n", out, strerror(errno));
}

static void rm_r(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *ent;

    if (d == NULL) {
        (void)unlink(path);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[1024];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        (void)snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rm_r(child);
        else
            (void)unlink(child);
    }
    (void)closedir(d);
    (void)rmdir(path);
}

static void mkdirs(const char *root, const char *rel)
{
    char path[1024];
    char *p;

    (void)snprintf(path, sizeof path, "%s/%s", root, rel);
    for (p = path + strlen(root) + 1u; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        (void)mkdir(path, 0755);
        *p = '/';
    }
    (void)mkdir(path, 0755);
}

static void writef(const char *root, const char *rel, mode_t mode, const char *text)
{
    char path[1024];
    FILE *f;

    (void)snprintf(path, sizeof path, "%s/%s", root, rel);
    f = fopen(path, "we");
    if (f == NULL) {
        (void)fprintf(stderr, "test_inventory: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    (void)fputs(text, f);
    (void)fclose(f);
    (void)chmod(path, mode);
}

/* ------------------------------------------------------------------ *
 * 1. Sorting, under a locale chosen to break a locale-aware sort
 * ------------------------------------------------------------------ */

static void check_sorting(void)
{
    char list[256];

    /* en_US.UTF-8 collation ignores punctuation and case in a way byte order
     * does not: "a-b" sorts before "ab" under LC_ALL=C and after it under a
     * UTF-8 collation, and "B" sorts before "a" bytewise and after it
     * otherwise. This is not hypothetical for us -- musl on the phone and
     * glibc on the build host disagree about exactly this -- and it is why
     * the tool never calls setlocale() and never uses strcoll(). */
    (void)setlocale(LC_ALL, "en_US.UTF-8");

    (void)snprintf(list, sizeof list, "b a-b ab B a");
    inv_sort_tokens(list);
    CHECK_STR(list, "B a a-b ab b", "tokens sort in byte order, not the locale's");

    (void)snprintf(list, sizeof list, "ttyUSB10 ttyUSB2 ttyUSB1");
    inv_sort_tokens(list);
    CHECK_STR(list, "ttyUSB1 ttyUSB10 ttyUSB2",
              "ttyUSB10 before ttyUSB2: a plain byte sort, as nd_modem.c also does");

    (void)snprintf(list, sizeof list, "solo");
    inv_sort_tokens(list);
    CHECK_STR(list, "solo", "a one-token list survives");

    list[0] = '\0';
    inv_sort_tokens(list);
    CHECK_STR(list, "", "an empty list survives");

    (void)setlocale(LC_ALL, "C");
}

/* ------------------------------------------------------------------ *
 * 2. The masks, one case each, including the ones that must NOT fire
 * ------------------------------------------------------------------ */

static void check_masks(void)
{
    char out[256];

    CHECK(inv_mask_serial("NDSYS", out, sizeof out) && strcmp(out, "NDSYS") == 0,
          "a serial in the project's own namespace is verbatim");
    CHECK(inv_mask_serial("NDUSER", out, sizeof out) && strcmp(out, "NDUSER") == 0,
          "NDUSER is verbatim");
    CHECK(inv_mask_serial("QM00013", out, sizeof out) && strcmp(out, "<serial:7>") == 0,
          "a foreign serial keeps only its length -- absent and unrecognised are "
          "different facts");
    CHECK(inv_mask_serial("", out, sizeof out) && out[0] == '\0', "an empty serial stays empty");

    /* The mount-option mask is the one most likely to be widened by somebody
     * in a hurry, so the cases that must NOT be masked are asserted as loudly
     * as the ones that must. nosuid, nodev and ro on /NeoDCT/User are what
     * nd-selftest's mount section fails on; a mask over them would hide the
     * divergence this file exists to find. */
    CHECK(inv_mask_mount_options("rw,nosuid,nodev,noexec,relatime,size=25756k,nr_inodes=6439", out,
                                 sizeof out),
          "a devtmpfs option list masks");
    CHECK_STR(out, "nodev noexec nosuid nr_inodes=<masked> relatime rw size=<masked>",
              "size= and nr_inodes= are masked, everything else is verbatim and sorted");

    CHECK(inv_mask_mount_options("ro,nodev,relatime,errors=remount-ro", out, sizeof out),
          "an ext4 option list masks");
    CHECK_STR(out, "errors=remount-ro nodev relatime ro",
              "ro, nodev and errors= survive untouched");

    CHECK(inv_mask_mount_options("rw,mode=755,blksize=4096", out, sizeof out), "blksize masks");
    CHECK_STR(out, "blksize=<masked> mode=755 rw", "mode= survives; blksize= does not");

    /* MemTotal. 53,824 kB is 52.56 MiB -- the emulator's measured number and
     * the whole memory-parity claim of this branch. It buckets to 52, and the
     * exact kB stays on an uncompared line beside it. */
    CHECK(inv_bucket_mib(53824u) == 52u, "53,824 kB buckets to 52 MiB");
    CHECK(inv_bucket_mib(53820u) == 52u, "53,820 kB -- the same machine with zram -- also 52");
    /* AND THE EDGE, ASSERTED RATHER THAN HOPED FOR. 4 MiB buckets round to
     * nearest, so 52.56 MiB lands in 52 and a machine at exactly 54 MiB lands
     * in 56. Two machines four megabytes apart CAN straddle a boundary and
     * produce one permanent-looking record that is an artefact of the rule
     * rather than of the machines. That is a real weakness of the design and
     * it is pinned here so nobody discovers it by being surprised by a diff. */
    CHECK(inv_bucket_mib(55296u) == 56u, "54.0 MiB rounds up, into the bucket above 52");
    CHECK(inv_bucket_mib(43520u) == 44u, "the multi_v7 kernel's 42.5 MiB does NOT reach 52");
    CHECK(inv_bucket_mib(0u) == 0u, "no MemTotal at all is 0 and not a crash");

    {
        char mode[8];

        inv_mode_octal(0100660u, mode);
        CHECK_STR(mode, "0660", "a mode is four octal digits, never symbolic");
        inv_mode_octal(0104755u, mode);
        CHECK_STR(mode, "4755", "setuid is a digit and not a letter whose case means something");
        inv_mode_octal(0041777u, mode);
        CHECK_STR(mode, "1777", "sticky likewise");
    }
}

/* ------------------------------------------------------------------ *
 * 3. The /dev family table
 * ------------------------------------------------------------------ */

static void check_families(void)
{
    CHECK(inv_dev_family("tty0") != NULL && strcmp(inv_dev_family("tty0"), "tty[N]") == 0,
          "tty0 is in the numbered tty family");
    CHECK(inv_dev_family("tty63") != NULL && strcmp(inv_dev_family("tty63"), "tty[N]") == 0,
          "tty63 too");
    CHECK(inv_dev_family("ptya0") != NULL && strcmp(inv_dev_family("ptya0"), "pty[a-z][0-f]") == 0,
          "ptya0 is the legacy BSD pty master grid");
    CHECK(inv_dev_family("ptyef") != NULL && strcmp(inv_dev_family("ptyef"), "pty[a-z][0-f]") == 0,
          "ptyef, the far corner of it");
    CHECK(inv_dev_family("ttyp0") != NULL && strcmp(inv_dev_family("ttyp0"), "tty[a-z][0-f]") == 0,
          "ttyp0 is the slave grid and NOT the numbered ttys");
    CHECK(inv_dev_family("loop7") != NULL && strcmp(inv_dev_family("loop7"), "loop[N]") == 0,
          "loop7");
    CHECK(inv_dev_family("vcsa2") != NULL && strcmp(inv_dev_family("vcsa2"), "vcsa[N]") == 0,
          "vcsa2 is its own family and not vcs[N] with an 'a2' suffix");

    /* The nodes that must NEVER be collapsed. ttyUSB2 is the modem's AT port
     * and ttyFIQ0 is the phone's console; a family that swallowed either
     * would delete the record somebody is looking for. */
    CHECK(inv_dev_family("ttyUSB2") == NULL, "ttyUSB2 is not in any family");
    CHECK(inv_dev_family("ttyAMA0") == NULL, "ttyAMA0 is not in any family");
    CHECK(inv_dev_family("ttyFIQ0") == NULL, "ttyFIQ0 is not in any family");
    CHECK(inv_dev_family("tty") == NULL, "bare tty is not a family");
    CHECK(inv_dev_family("fb0") == NULL, "fb0 is not a family");
    CHECK(inv_dev_family("i2c-3") == NULL, "i2c-3 is not a family");
}

static void check_cmdline_keep(void)
{
    CHECK(inv_cmdline_keep("video"), "video= decides the framebuffer and is kept");
    CHECK(inv_cmdline_keep("neodct.devenv"), "the env.sh gate is kept");
    CHECK(inv_cmdline_keep("nandsim.first_id_byte"), "every nandsim parameter is kept");
    CHECK(inv_cmdline_keep("mtdram.total_size"), "and every mtdram one");
    /* The ones whose VALUES are partition tables and device paths: the U-Boot
     * env on one machine, run_qemu.sh on the other. The NAMES are always
     * compared; only these values are dropped. */
    CHECK(!inv_cmdline_keep("root"), "root= is a partition and its value is not compared");
    CHECK(!inv_cmdline_keep("console"), "console= likewise");
    CHECK(!inv_cmdline_keep("neodct.user"), "the user-partition hint likewise");
    /* THE POLICY SWITCHES, AND neodct.verity IS THE ONE THAT MATTERED. Its
     * NAME is identical whichever mode is set, so while only the name was
     * compared, a phone booting neodct.verity=permissive -- which is exactly
     * what ndsys-recovery.sh sets for the next boot -- produced zero
     * differing records against an emulator on `enforce`. The full string
     * reaches the file as cmdline.raw, and that record is INV_RAW: never
     * diffed, never in the compared hash. So the one machine in the fleet
     * whose root filesystem is no longer verified was the one this harness
     * reported as being in parity. */
    CHECK(inv_cmdline_keep("neodct.verity"),
          "neodct.verity is a POLICY SWITCH and its value is compared -- enforce, "
          "permissive and off are three different machines");
    CHECK(inv_cmdline_keep("neodct.recovery"), "booting straight into recovery likewise");
    CHECK(inv_cmdline_keep("neodct.unsigned"), "and letting an unsigned update install");
    /* neodct.rectty is deliberately NOT kept: it is a device path, which is
     * the class whose values come from the U-Boot env on one machine and
     * run_qemu.sh on the other. */
    CHECK(!inv_cmdline_keep("neodct.rectty"), "but the recovery console's DEVICE PATH is not");
}

/* ------------------------------------------------------------------ *
 * 4. The record buffer and the two renderings
 * ------------------------------------------------------------------ */

static const char *find_record(const char *body, const char *key)
{
    static char val[512];
    char needle[256];
    const char *at;

    (void)snprintf(needle, sizeof needle, "INV|%s ", key);
    at = strstr(body, needle);
    if (at == NULL) {
        /* An informational record answers to its key too: the ~ is a column. */
        (void)snprintf(needle, sizeof needle, "INV|~%s ", key);
        at = strstr(body, needle);
    }
    if (at == NULL)
        return NULL;
    at += strlen(needle);
    {
        const char *nl = strchr(at, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - at) : strlen(at);

        if (len >= sizeof val)
            len = sizeof val - 1u;
        memcpy(val, at, len);
        val[len] = '\0';
    }
    return val;
}

/* The value of one key in a buffer that has not been rendered yet. Only the
 * duplicate-key case needs it, and it needs it because the point is which of
 * the two records survived. */
static const char *find_record_in(const inv_out *o, const char *key)
{
    static char val[ND_INV_VAL_MAX];
    char *body = inv_render(o);
    const char *found;

    val[0] = '\0';
    if (body == NULL)
        return val;
    found = find_record(body, key);
    if (found != NULL)
        (void)snprintf(val, sizeof val, "%s", found);
    free(body);
    return val;
}

static void check_records(void)
{
    inv_out *o = inv_out_new();
    char *body;
    char *compared;

    CHECK(o != NULL, "a record buffer allocates");
    if (o == NULL)
        return;

    /* Added deliberately out of order, and with the informational record in
     * the middle, because the sort is over the KEY and the ~ is a column. */
    (void)inv_add(o, INV_CMP, "uname.machine", "armv7l");
    (void)inv_add(o, INV_RAW, "mem.total_kb", "53824");
    (void)inv_add(o, INV_CMP, "mem.total_mib", "52");
    (void)inv_add(o, INV_CMP, "class.backlight", "[]");

    inv_sort(o);
    body = inv_render(o);
    compared = inv_render_compared(o);
    CHECK(body != NULL && compared != NULL, "both renderings allocate");
    if (body == NULL || compared == NULL) {
        free(body);
        free(compared);
        inv_out_free(o);
        return;
    }

    CHECK_STR(body,
              "INV|class.backlight []\n"
              "INV|~mem.total_kb 53824\n"
              "INV|mem.total_mib 52\n"
              "INV|uname.machine armv7l\n",
              "records sort by key; ~ is a column and does not sort the "
              "informational record away from the fact it belongs to");
    CHECK_STR(compared,
              "INV|class.backlight []\n"
              "INV|mem.total_mib 52\n"
              "INV|uname.machine armv7l\n",
              "the compared rendering -- what the second hash is over -- drops ~ records");
    CHECK(inv_dropped(o) == 0u, "nothing was dropped");
    free(body);
    free(compared);

    /* A KEY IS ONE WHITESPACE-FREE TOKEN, because the separator on a record
     * line is a space. The first committed capture carried
     * input.byname."QEMU Virtio Keyboard".handlers and the comparator read
     * the key as input.byname."QEMU, merging every device whose name began
     * with QEMU into one record. Normalised in inv_add() rather than at the
     * four collectors that build a key out of a device's own name, because a
     * rule enforced at four call sites is one the fifth collector will not
     * know about. */
    {
        inv_out *k = inv_out_new();

        CHECK(k != NULL, "a second buffer allocates");
        if (k != NULL) {
            char *rendered;

            (void)inv_add(k, INV_CMP, "input.byname.\"QEMU Virtio Keyboard\".handlers",
                          "[event0 kbd]");
            rendered = inv_render(k);
            CHECK_STR(rendered != NULL ? rendered : "",
                      "INV|input.byname.\"QEMU\\x20Virtio\\x20Keyboard\".handlers "
                      "[event0 kbd]\n",
                      "a space inside an identity is escaped, reversibly, so the key "
                      "stays one token");
            CHECK(inv_dropped(k) == 0u, "and the record is not dropped for it");
            free(rendered);
            inv_out_free(k);
        }
    }

    /* A key that does not fit is REFUSED, never truncated: a truncated key
     * silently merges two records, which is the one failure a byte-stable
     * format cannot survive. The refusal is what makes the tool exit 1. */
    {
        char huge[ND_INV_KEY_MAX + 32];

        memset(huge, 'k', sizeof huge - 1u);
        huge[sizeof huge - 1u] = '\0';
        CHECK(!inv_add(o, INV_CMP, huge, "x"), "an over-long key is refused");
        CHECK(inv_dropped(o) == 1u, "and counted, so the capture reports itself incomplete");
    }

    /* A DUPLICATE KEY IS REFUSED THE SAME WAY, because parity_diff.py refuses
     * a whole capture that carries one -- so a tool that emitted duplicates
     * and a comparator that rejected them disagreed about what a legal
     * capture is, and the operator could only reconcile them by hand-editing
     * the artefact. Reproduced on an ordinary container with no contrivance:
     * five virtio disks whose foreign serials were the same length all masked
     * onto block.byserial.<serial:14>.node, /proc/mounts carried /dev/pts and
     * /dev/shm twice, and `nd-inventory --self-check` printed "ok (183
     * records, twice)" and exited 0. --self-check cannot catch it: both
     * collections contain the same duplicates. */
    CHECK(!inv_add(o, INV_CMP, "uname.machine", "aarch64"),
          "a key already in the buffer is refused, whatever the value");
    CHECK(inv_dropped(o) == 2u, "and counted, so the capture reports itself incomplete");
    CHECK(strcmp(find_record_in(o, "uname.machine"), "armv7l") == 0,
          "and the record that was already there is the one that survives");
    inv_out_free(o);

    /* THE KEY BUILDER REPORTS TOO. Nineteen collectors used to build a key
     * with a local snprintf and `continue` on truncation -- dropping the
     * record silently, moving no counter and printing nothing, which is the
     * opposite of what nd_inventory.h promises. Reproduced with a
     * 201-character mount point: the record vanished, stderr said nothing and
     * the tool exited 0. */
    {
        inv_out *k = inv_out_new();
        char key[ND_INV_KEY_MAX];
        char longname[ND_INV_KEY_MAX + 8];

        CHECK(k != NULL, "a third buffer allocates");
        if (k != NULL) {
            memset(longname, 'm', sizeof longname - 1u);
            longname[sizeof longname - 1u] = '\0';
            CHECK(inv_key_fmt(k, key, sizeof key, "mount.%s", "/NeoDCT/User"),
                  "an ordinary key builds");
            CHECK_STR(key, "mount./NeoDCT/User", "into the buffer it was given");
            CHECK(!inv_key_fmt(k, key, sizeof key, "mount.%s", longname),
                  "a key that does not fit is refused rather than truncated");
            CHECK(inv_dropped(k) == 1u,
                  "and counted, so a capture that is quietly one record short exits 1 "
                  "instead of hashing cleanly");
            inv_out_free(k);
        }
    }
}

/* ------------------------------------------------------------------ *
 * 5. SHA-256, against the published vector rather than against itself
 * ------------------------------------------------------------------ */

static void check_sha(void)
{
    char hex[65];

    inv_sha256_hex("abc", 3u, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "FIPS 180-4 vector for \"abc\"");
    inv_sha256_hex("", 0u, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "and for the empty string, which is the message-length padding path");
}

/* ------------------------------------------------------------------ *
 * 6. ABSENT versus [] -- two different facts, on a synthetic tree
 * ------------------------------------------------------------------ */

static char *collect_to_string(const char *root, const char *const *sections)
{
    inv_cfg cfg;
    inv_out *o = inv_out_new();
    char *body;

    cfg.root = root;
    if (o == NULL)
        return NULL;
    inv_collect(o, &cfg, sections);
    inv_sort(o);
    body = inv_render(o);
    inv_out_free(o);
    return body;
}

/* ------------------------------------------------------------------ *
 * 3b. /proc/filesystems has two line shapes and the record needs both
 * ------------------------------------------------------------------ */

static void check_proc_filesystems(void)
{
    static const char *const SECTIONS[] = {"proc", NULL};
    char root[512];
    char *body;
    const char *v;

    tmpdir_make(root, sizeof root, "procfs");
    mkdirs(root, "proc");
    /* Exactly the two shapes the kernel writes: "nodev<TAB>name" for a virtual
     * filesystem and "<TAB>name" for a device-backed one. */
    writef(root, "proc/filesystems", 0444,
           "nodev\tsysfs\n"
           "nodev\ttmpfs\n"
           "\text4\n"
           "\tsquashfs\n"
           "\tvfat\n"
           "nodev\tubifs\n");

    body = collect_to_string(root, SECTIONS);
    CHECK(body != NULL, "the /proc tree collects");
    if (body == NULL) {
        rm_r(root);
        return;
    }
    /* THE RECORD USED TO HOLD THE NODEV ENTRIES AND NOTHING ELSE, because the
     * collector asked for column 1 -- and on a device-backed line the first
     * token IS the name, so the column walk ran off the end and the line was
     * skipped. The record's own comment says it "gates the entire image
     * design: squashfs for the verity root, ext4 for the user partition,
     * ubifs for the phone's NAND", and squashfs and ext4 were exactly the two
     * it could never contain. Measured: deleting ext4, squashfs and vfat from
     * a synthetic file left the record byte-identical, so a kernel config that
     * dropped CONFIG_SQUASHFS and CONFIG_EXT4_FS -- an image that can mount
     * neither its verity root nor its only writable partition -- produced an
     * identical baseline and passed every gate. */
    v = find_record(body, "proc.filesystems");
    CHECK_STR(v != NULL ? v : "", "[ext4 squashfs sysfs tmpfs ubifs vfat]",
              "both line shapes reach the record, and the nodev marker is dropped "
              "rather than selected on");
    free(body);
    rm_r(root);
}

static void check_absent_vs_empty(void)
{
    static const char *const SECTIONS[] = {"class", "cpufreq", "fb", NULL};
    char root[512];
    char *body;
    const char *v;

    tmpdir_make(root, sizeof root, "absent");

    /* backlight exists and is empty; leds does not exist at all. Both states
     * are on the repo's own kernel today -- class.power_supply is the empty
     * one now that CONFIG_TEST_POWER is gone, class.leds the missing one --
     * and the distinction is the whole reason the emulator's backlight took a
     * device tree rather than a kernel symbol: the class was already there. A
     * format that printed an empty directory and a missing one the same way
     * would have hidden which of the two jobs was outstanding. */
    mkdirs(root, "sys/class/backlight");
    mkdirs(root, "sys/class/power_supply/test_battery");
    writef(root, "sys/class/power_supply/test_battery/type", 0444, "Battery\n");
    /* serial_number is NOT in the identity-attribute allowance, and this is
     * the case that proves names-not-values keeps a per-unit identifier out
     * of a git-committed file by construction. */
    writef(root, "sys/class/power_supply/test_battery/serial_number", 0444, "SN-0001-SECRET\n");
    writef(root, "sys/class/power_supply/test_battery/capacity", 0444, "76\n");
    mkdirs(root, "sys/class/tty/tty0");
    mkdirs(root, "sys/class/tty/tty1");
    mkdirs(root, "sys/class/tty/tty2");
    mkdirs(root, "sys/class/tty/ttyAMA0");
    /* A regular file where /dev/fb0 belongs. The framebuffer records are the
     * one thing --root cannot synthesise -- an ioctl needs a real driver --
     * so the collector says WHICH reason it has no numbers rather than
     * omitting the record and shortening the file. */
    mkdirs(root, "dev");
    writef(root, "dev/fb0", 0660, "");

    body = collect_to_string(root, SECTIONS);
    CHECK(body != NULL, "the synthetic tree collects");
    if (body == NULL) {
        rm_r(root);
        return;
    }

    v = find_record(body, "class.backlight");
    CHECK(v != NULL && strcmp(v, "[]") == 0, "a class directory with no devices is []");
    v = find_record(body, "class.leds");
    CHECK(v != NULL && strcmp(v, "ABSENT") == 0, "a class directory that is not there is ABSENT");
    v = find_record(body, "class.power_supply");
    CHECK(v != NULL && strcmp(v, "[test_battery]") == 0, "and one with a device lists its name");
    v = find_record(body, "class.power_supply.test_battery.type");
    CHECK(v != NULL && strcmp(v, "Battery") == 0, "type is in the identity allowance");

    /* The family table applies to class listings too, and it had to: the
     * first capture from the real kernel dropped class.tty outright because
     * the legacy pty grid ran the listing past ND_INV_VAL_MAX. The names that
     * matter survive beside the collapsed family. */
    v = find_record(body, "class.tty");
    CHECK(v != NULL && strcmp(v, "[tty[N]x3 ttyAMA0 ttyp[a-z]]") != 0, "class.tty is recorded");
    CHECK_STR(v != NULL ? v : "", "[tty[N]x3 ttyAMA0]",
              "a family in a class listing arrives as one token carrying its count, "
              "and a name in no family is still spelled out");

    CHECK(strstr(body, "SN-0001-SECRET") == NULL,
          "the serial number's VALUE never reaches the file");
    CHECK(strstr(body, "capacity") == NULL, "and a live reading is not even named");

    v = find_record(body, "cpufreq.cpu0");
    CHECK(v != NULL && strcmp(v, "ABSENT") == 0,
          "ND_CPUFREQ_DIR absent -- the divergence the prototype found on its first run");

    v = find_record(body, "fb0");
    CHECK(v != NULL && strcmp(v, "UNAVAILABLE(not-a-chardev)") == 0,
          "a synthetic root has no framebuffer and the record says which reason");

    free(body);
    rm_r(root);
}

/* ------------------------------------------------------------------ *
 * 7. The family collapse, including the member that escapes it
 * ------------------------------------------------------------------ */

/* `break_name` is the member given a different mode, or NULL for none. It is a
 * NAME and not a flag because WHICH member is broken is the whole of the case
 * below: emit_family() takes the MAJORITY as its reference and spends a
 * documented quadratic scan to do it, and breaking tty1 -- the middle member
 * of tty0/tty1/tty2 -- cannot tell that apart from taking members[0]. */
static void build_dev_tree(const char *root, bool reverse, const char *break_name)
{
    static const char *const NAMES[] = {"tty0",  "tty1", "tty2", "ptya0", "ptya1", "loop0",
                                        "loop1", "fb0",  "null", "i2c-3", NULL};
    size_t n = 0u;
    size_t i;

    mkdirs(root, "dev");
    while (NAMES[n] != NULL)
        n++;
    for (i = 0u; i < n; i++) {
        const char *name = reverse ? NAMES[n - 1u - i] : NAMES[i];
        char rel[256];
        mode_t mode = 0620;

        (void)snprintf(rel, sizeof rel, "dev/%s", name);
        if (break_name != NULL && strcmp(name, break_name) == 0)
            mode = 0666;
        writef(root, rel, mode, "");
    }
}

static void check_dev_families(void)
{
    static const char *const SECTIONS[] = {"dev", NULL};
    char root[512];
    char *body;
    const char *v;

    tmpdir_make(root, sizeof root, "devfam");
    build_dev_tree(root, false, NULL);
    /* The owner NAME comes from the image's own /etc/passwd and /etc/group,
     * not from getpwuid(3). The first probe capture recorded every owner as
     * uid=0(?) because a statically linked glibc needs libnss_files.so.2 at
     * runtime and returns NULL without it -- the same machine read by two
     * builds of the same tool giving two answers, which is precisely the
     * reader difference a parity harness must never report as a machine
     * difference. Reading the files also means --root can put a synthetic
     * users table in front of it, which is what this does. */
    mkdirs(root, "etc");
    writef(root, "etc/passwd", 0644,
           "root:x:0:0:root:/root:/bin/sh\n"
           "ndusr:x:1000:1000:NeoDCT:/:/bin/false\n");
    writef(root, "etc/group", 0644, "root:x:0:\nvideo:x:44:ndusr\n");
    body = collect_to_string(root, SECTIONS);
    CHECK(body != NULL, "the /dev tree collects");
    if (body == NULL) {
        rm_r(root);
        return;
    }
    v = find_record(body, "dev.tty[N]");
    CHECK(v != NULL && strncmp(v, "n=3 ", 4u) == 0,
          "three numbered ttys collapse into one record carrying the count");
    CHECK(v != NULL && strstr(v, "mode=0620") != NULL, "and the mode they share, in octal");
    v = find_record(body, "dev.ptya0");
    CHECK(v == NULL, "a family member has no record of its own");
    v = find_record(body, "dev.fb0");
    CHECK(v != NULL, "a node in no family keeps its own record");
    CHECK(v != NULL && strstr(v, "uid=0(root)") != NULL,
          "the uid's NAME comes out of the tree's own /etc/passwd");
    CHECK(v != NULL && strstr(v, "gid=0(root)") != NULL, "and the gid's out of /etc/group");
    v = find_record(body, "dev.count");
    CHECK(v != NULL && strcmp(v, "10") == 0,
          "the raw node count is recorded too, so a family cannot hide a node");
    free(body);
    rm_r(root);

    /* THE CASE THAT MATTERS: one member with a different mode. It must be
     * broken out under its own key and excluded from the count, so that a
     * single wrong mode cannot hide inside a family of hundreds. */
    tmpdir_make(root, sizeof root, "devbreak");
    build_dev_tree(root, false, "tty1");
    body = collect_to_string(root, SECTIONS);
    CHECK(body != NULL, "the heterogeneous tree collects");
    if (body == NULL) {
        rm_r(root);
        return;
    }
    v = find_record(body, "dev.tty[N]");
    CHECK(v != NULL && strncmp(v, "n=2 ", 4u) == 0,
          "the odd member is excluded from the family's count");
    v = find_record(body, "dev.tty1");
    CHECK(v != NULL && strstr(v, "mode=0666") != NULL,
          "and appears as its own record, carrying the mode that differs");
    free(body);
    rm_r(root);

    /* AND THE SAME CASE WITH THE FIRST MEMBER BROKEN, WHICH IS THE ONE THAT
     * TESTS THE RULE. emit_family() takes the MAJORITY as its reference and
     * spends a documented quadratic scan on it, arguing that taking the first
     * member instead "would mean that one wrong mode on ptya0 broke all 335
     * of its siblings out into their own records: a single-node fault would
     * arrive as a three-hundred-line diff and the one line that mattered
     * would be unfindable in it".
     *
     * Breaking tty1 above cannot see that: it is the middle member, so the
     * majority and members[0] are the same node. Mutation-tested --
     * `ref = &members[best]` replaced by `&members[0]` left all 78 checks
     * passing. With tty0 broken the mutant fails twice, here and on the
     * dev.tty0 record below. */
    tmpdir_make(root, sizeof root, "devfirst");
    build_dev_tree(root, false, "tty0");
    body = collect_to_string(root, SECTIONS);
    CHECK(body != NULL, "the tree with its FIRST family member broken collects");
    if (body == NULL) {
        rm_r(root);
        return;
    }
    v = find_record(body, "dev.tty[N]");
    CHECK(v != NULL && strncmp(v, "n=2 ", 4u) == 0,
          "the family still counts the two that agree, not the one that came first");
    CHECK(v != NULL && strstr(v, "mode=0620") != NULL,
          "and carries the MAJORITY's mode -- taking members[0] would put 0666 here "
          "and break tty1 and tty2 out instead");
    v = find_record(body, "dev.tty0");
    CHECK(v != NULL && strstr(v, "mode=0666") != NULL,
          "the odd member is the one broken out, wherever it sorts");
    free(body);
    rm_r(root);
}

/* ------------------------------------------------------------------ *
 * 8. Two creation orders, one output. The property everything rests on.
 * ------------------------------------------------------------------ */

/* Copies `body`, cutting every line at " uid=". The owner column is the euid
 * of whoever ran the test and the name it resolves to in the SYNTHETIC tree's
 * /etc/passwd, neither of which is what this case is about -- check_dev_
 * families() asserts both against a tree that has a users table. Everything
 * else survives, which is every value the sort decides. */
static void strip_owner(const char *body, char *out, size_t n)
{
    size_t used = 0u;
    const char *p = body;

    out[0] = '\0';
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        const char *cut = strstr(p, " uid=");

        if (cut != NULL && (nl == NULL || cut < nl))
            len = (size_t)(cut - p);
        if (used + len + 2u >= n)
            return;
        memcpy(out + used, p, len);
        used += len;
        out[used++] = '\n';
        out[used] = '\0';
        if (nl == NULL)
            break;
        p = nl + 1;
    }
}

static void check_order_independence(void)
{
    static const char *const SECTIONS[] = {"dev", "class", "mount", NULL};
    char root_a[512];
    char root_b[512];
    char *a;
    char *b;

    tmpdir_make(root_a, sizeof root_a, "orda");
    tmpdir_make(root_b, sizeof root_b, "ordb");
    build_dev_tree(root_a, false, NULL);
    build_dev_tree(root_b, true, NULL);

    /* Built in opposite orders on both sides so that whichever way this
     * filesystem happens to hand names back -- tmpfs returns creation order,
     * ext4 returns a hash order, sysfs returns kernfs rbtree order -- the two
     * captures were not read in the same sequence. */
    mkdirs(root_a, "sys/class/backlight");
    mkdirs(root_b, "sys/class/backlight");
    mkdirs(root_a, "sys/class/net/lo");
    mkdirs(root_a, "sys/class/net/sit0");
    mkdirs(root_b, "sys/class/net/sit0");
    mkdirs(root_b, "sys/class/net/lo");

    /* SIXTEEN NAMES IN ONE DIRECTORY, BECAUSE TWO WERE NOT ENOUGH TO PROVOKE
     * THE BUG. With the qsort() deleted from listdir_sorted(), the ten-name
     * /dev tree above still produced the right answer here: ext4 handed the
     * names back in `fb0 loop1 loop0 ptya0 ptya1 tty2 tty0 tty1 i2c-3 null`
     * -- an order in which every family happens to stay contiguous, so all
     * three collapsed with the right counts anyway, and inv_sort() put the
     * records themselves in order at the end regardless.
     *
     * A CLASS LISTING is the value that carries readdir order straight into
     * the file, and sixteen of them make an accidental agreement with byte
     * order vanishingly unlikely on a hash-ordered filesystem (measured on
     * this one: net02 net07 net11 net01 ...). They are created in OPPOSITE
     * orders in the two trees, which is what covers the other kind: on a
     * creation-ordered filesystem like tmpfs the listing is the shuffle
     * itself, so an unsorted collector cannot agree with byte order there
     * either. `misc` is the class used because it has no identity attribute,
     * so this adds one record and not seventeen. */
    {
        static const char *const MISC[] = {"misc00", "misc01", "misc02", "misc03",
                                           "misc04", "misc05", "misc06", "misc07",
                                           "misc08", "misc09", "misc10", "misc11",
                                           "misc12", "misc13", "misc14", "misc15"};
        size_t m;

        mkdirs(root_a, "sys/class/misc");
        mkdirs(root_b, "sys/class/misc");
        for (m = 0u; m < 16u; m++) {
            char rel[64];

            (void)snprintf(rel, sizeof rel, "sys/class/misc/%s", MISC[15u - m]);
            mkdirs(root_a, rel);
            (void)snprintf(rel, sizeof rel, "sys/class/misc/%s", MISC[m]);
            mkdirs(root_b, rel);
        }
    }
    writef(root_a, "sys/class/net/lo/type", 0444, "772\n");
    writef(root_a, "sys/class/net/sit0/type", 0444, "776\n");
    writef(root_b, "sys/class/net/sit0/type", 0444, "776\n");
    writef(root_b, "sys/class/net/lo/type", 0444, "772\n");

    mkdirs(root_a, "proc");
    mkdirs(root_b, "proc");
    writef(root_a, "proc/mounts", 0444,
           "/dev/root / squashfs ro,relatime 0 0\n"
           "devtmpfs /dev devtmpfs rw,nosuid,size=25756k,nr_inodes=6439,mode=755 0 0\n");
    writef(root_b, "proc/mounts", 0444,
           "/dev/root / squashfs ro,relatime 0 0\n"
           "devtmpfs /dev devtmpfs rw,nosuid,size=41988k,nr_inodes=10497,mode=755 0 0\n");

    a = collect_to_string(root_a, SECTIONS);
    b = collect_to_string(root_b, SECTIONS);
    CHECK(a != NULL && b != NULL, "both trees collect");
    if (a != NULL && b != NULL) {
        CHECK_STR(a, b,
                  "the same tree built in two creation orders produces byte-identical "
                  "output -- including the devtmpfs size=, which is masked because it is "
                  "MemTotal/2 restated and would otherwise differ here");
        /* AND AGAINST A FIXED STRING, BECAUSE a == b IS VACUOUS ON THE
         * FILESYSTEMS THIS PROJECT IS DEVELOPED AND CI'D ON. ext4 with
         * dir_index and tmpfs both return readdir entries in a hash of the
         * NAME, so both trees hand back the same order whatever order they
         * were created in, and the two captures agree even with no sort at
         * all. Mutation-tested: with the qsort() deleted from
         * listdir_sorted() -- the exact regression this case exists to catch
         * -- 77 of 78 checks still passed and the one failure was the
         * incidental class.tty assertion in check_absent_vs_empty(), so
         * anybody who deleted the sort and adjusted that one line would have
         * had a green suite.
         *
         * The expected body below is what a correct sort produces, so the
         * case now fails on any ordering regression regardless of what the
         * host filesystem happens to hand back. Keep the two-order build as
         * well: it is free, and it is the only thing that would catch an
         * order dependence this fixture's shape does not provoke. */
        {
            char got[4096];

            strip_owner(a, got, sizeof got);
            CHECK_STR(got,
                      "INV|class.backlight []\n"
                      "INV|class.block ABSENT\n"
                      "INV|class.gpio ABSENT\n"
                      "INV|class.graphics ABSENT\n"
                      "INV|class.i2c-dev ABSENT\n"
                      "INV|class.input ABSENT\n"
                      "INV|class.leds ABSENT\n"
                      "INV|class.misc [misc00 misc01 misc02 misc03 misc04 misc05 misc06 "
                      "misc07 misc08 misc09 misc10 misc11 misc12 misc13 misc14 misc15]\n"
                      "INV|class.mtd ABSENT\n"
                      "INV|class.net [lo sit0]\n"
                      "INV|class.net.lo.type 772\n"
                      "INV|class.net.sit0.type 776\n"
                      "INV|class.power_supply ABSENT\n"
                      "INV|class.rtc ABSENT\n"
                      "INV|class.thermal ABSENT\n"
                      "INV|class.tty ABSENT\n"
                      "INV|class.ubi ABSENT\n"
                      "INV|dev.count 10\n"
                      "INV|dev.fb0 type=f mode=0620\n"
                      "INV|dev.i2c-3 type=f mode=0620\n"
                      "INV|dev.loop[N] n=2 type=f mode=0620\n"
                      "INV|dev.null type=f mode=0620\n"
                      "INV|dev.pty[a-z][0-f] n=2 type=f mode=0620\n"
                      "INV|dev.tty[N] n=3 type=f mode=0620\n"
                      "INV|mount./ fstype=squashfs options=relatime ro\n"
                      "INV|mount./dev fstype=devtmpfs options=mode=755 nosuid "
                      "nr_inodes=<masked> rw size=<masked>\n",
                      "and the output is the one a correct sort produces -- the record "
                      "order, the token order inside class.net, and the three /dev "
                      "families collapsing with the right counts");
        }
    }
    free(a);
    free(b);
    rm_r(root_a);
    rm_r(root_b);
}

int main(void)
{
    tmpdir_make(g_tmp, sizeof g_tmp, "root");
    rm_r(g_tmp);

    check_sorting();
    check_masks();
    check_families();
    check_cmdline_keep();
    check_proc_filesystems();
    check_records();
    check_sha();
    check_absent_vs_empty();
    check_dev_families();
    check_order_independence();

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_inventory: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_inventory: %d checks passed\n", g_checks);
    return 0;
}
