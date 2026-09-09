#!/bin/sh
# test_qemu_i2c.sh -- the emulator really has an i2c bus with a PCF8575 on it,
# asserted by booting one.
#
#     neodct/tools/test_qemu_i2c.sh --kernel <zImage> --rootfs <busybox dir>
#
# It belongs beside test_qemu_surfaces.sh in the parity README's "needs a
# kernel, needs no image" row, and it answers the questions no host test can:
#
#   1. an adapter registers at all, and it is numbered THREE -- the number
#      nd_keymap.c, nd_keypadsetup.c, nd_battery.c and nd_selftest.c all
#      default to. An emulator on /dev/i2c-0 would need a keymap fork, a
#      NEODCT_KEYPAD_SETUP_BUS and a settings override to work, which is
#      exactly the per-machine configuration this branch keeps deleting;
#   2. /dev/i2c-3 is char 89:3 and lands root:root 0600 out of devtmpfs, which
#      is the state the udev rule then has to change;
#   3. I2C_FUNCS carries I2C_FUNC_I2C. THIS IS THE SINGLE ASSERTION THAT WOULD
#      HAVE CAUGHT i2c-stub, which gives a perfectly good adapter whose every
#      plain transfer is EOPNOTSUPP because its algorithm declares no
#      .master_xfer;
#   4. the phone's OWN two-byte write and two-byte read round-trip -- the
#      repository's nd_pcf8575.c and nd_matrix.c, cross-compiled for armv7 and
#      run against the node, not a probe that models them;
#   5. a key pressed on the host reaches nd_matrix_scan_once() as exactly one
#      PRESS at the expected row and column, and nd_matrix_is_held()'s
#      debounce then reports it HELD;
#   6. of 0x20..0x27, EXACTLY ONE address answers, so nd_kpsetup_probe()'s
#      eight-address sweep finds the chip and not the first thing it tried;
#   7. nd_input_open() CHOOSES THE MATRIX on a machine that has both it and an
#      evdev keyboard. This boot attaches a virtio keyboard as well, which is
#      the phone's own arrangement -- nd_input opens the matrix first and then
#      opens evdev REGARDLESS, "so a developer with a USB keyboard plugged
#      into a real phone can still type" -- and it is the first machine
#      anywhere where that CHOICE has run at all;
#   8. nd_battery reaches ND_BATT_SRC_LIVE on the same adapter. That is not a
#      bonus chip: the moment /dev/i2c-3 exists the gauge can never go back to
#      SIM, because nd_battery.c says "past open() the node demonstrably
#      exists, so every remaining failure is UNREADABLE by construction". A
#      keypad daemon that NAKed 0x36 would turn the emulator's battery meter
#      into a fault rather than leaving it simulating;
#   9. THE THREE-STATE OWNERSHIP PROBE, which is the test that would have
#      caught the keypad EACCES. See its own block below.
#
# ============ WHAT THIS CANNOT SEE, STATED UP FRONT ============
#
# There is no eudev in a busybox initramfs, so this script does not prove that
# 61-neodct-devices.rules really turns the node into root:i2c 0660 -- it
# proves that the node arrives in the state the rule has to change, that its
# SUBSYSTEM is the rule's exact match key, and that the kernel's answer for
# each of the three possible outcomes is what the rule assumes. The last mile
# is an image boot and nd-selftest, which is the same `until-image` status the
# parity README already gives nine records.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../src"
DTSI="$HERE/../board/qemu/nd-virt-additions.dtsi"
CROSS="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
KERNEL=""
ROOTFS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --rootfs) ROOTFS="$2"; shift 2 ;;
        -h|--help)
            echo "usage: $0 --kernel <zImage> --rootfs <busybox rootfs dir>"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$KERNEL" ] && [ -n "$ROOTFS" ] || { echo "usage: $0 --kernel <zImage> --rootfs <dir>" >&2; exit 2; }
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL" >&2; exit 2; }
[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS" >&2; exit 2; }
command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm not found" >&2; exit 2; }
command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found" >&2; exit 2; }

. "$HERE/qemu_machine.sh"

WORK="${TMPDIR:-/tmp}/nd-qemu-i2c.$$"
mkdir -p "$WORK"
KEYPADD_PID=""
KEYS_PID=""
cleanup() {
    # `|| true` ON EVERY LINE, AND IT IS NOT DEFENSIVE PADDING. Under `set -e`
    # a bare `[ -n "$P" ] && kill "$P"` ABORTS THE TRAP when the kill fails,
    # because the kill is the last command of an AND-list and errexit applies
    # to it -- so the script exits 1 after printing "all assertions passed",
    # which is the worst failure a gate can have. Measured: the key writer
    # exits seconds before the boot does, its pid is stale by the time the
    # trap runs, and every green run reported failure.
    if [ -n "$KEYPADD_PID" ]; then kill "$KEYPADD_PID" 2>/dev/null || true; fi
    # The key writer waits on a marker in the console and would otherwise
    # outlive the work directory it writes into, which is a confusing error
    # printed after the verdict.
    if [ -n "$KEYS_PID" ]; then kill "$KEYS_PID" 2>/dev/null || true; fi
    # ND_KEEP_WORK=1 leaves the console, the daemon's log and the merged device
    # tree behind. Debugging a failure here means reading all three together,
    # and re-running to get them back changes the timing that produced it.
    [ "${ND_KEEP_WORK:-0}" = "1" ] || rm -rf "$WORK"
}
# An EXIT trap and not a line after QEMU: under `set -e` a non-zero QEMU exit
# terminates the script before any cleanup, and a killed script orphans both
# QEMU and the daemon. Both were measured on the storage stage's userdata lift.
trap cleanup EXIT

fail=0
ok()   { echo "ok   $*"; }
bad()  { echo "FAIL $*"; fail=1; }

nd_dtb_build "$DTSI" "$WORK" "$WORK/nd.dtb" || {
    echo "REFUSED: no device tree, and the bus NUMBER comes from it -- without" >&2
    echo "         the reservation node the adapter is /dev/i2c-0 and this test" >&2
    echo "         would be asserting against a machine nobody boots." >&2
    exit 1; }

# ------------------------------------------------------------------ #
# The guest side: the repository's own driver, cross-compiled
# ------------------------------------------------------------------ #
#
# nd_pcf8575.c and nd_matrix.c and nothing standing in for them. That is the
# whole point of the exercise: a probe written for this test could agree with
# a wrong driver, and the two files below are what the phone runs.
WARN="-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes \
-Wmissing-prototypes -Wvla -O2 -D_GNU_SOURCE"

cat > "$WORK/i2cprobe.c" <<'PROBE'
/* Cross-compiled into the guest. Every i2c transaction below is made by the
 * repository's own nd_pcf8575.c and nd_matrix.c. */
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include "nd_battery.h"
#include "nd_input.h"
#include "nd_keypad.h"

#define I2C_FUNCS 0x0705
#define FUNC_I2C  0x00000001

/* nd_kpsetup_probe()'s loop, replicated rather than linked: nd_keypadsetup.c
 * pulls in nd_draw, nd_fb, nd_font and nd_image, which do not cross-compile
 * without freetype and libpng for the target. The loop is four lines and is
 * quoted from that function; what matters is that it uses the same
 * nd_pcf8575_open/write16/read16 the wizard uses, so an address that answers
 * here answers there. */
static void probe_addresses(int bus)
{
    int addr;
    int found = 0;

    for (addr = 0x20; addr <= 0x27; addr++) {
        nd_pcf8575 chip;
        uint16_t v = 0u;
        if (nd_pcf8575_open(&chip, bus, addr) != ND_OK)
            continue;
        if (nd_pcf8575_write16(&chip, 0xFFFFu) == ND_OK &&
            nd_pcf8575_read16(&chip, &v) == ND_OK) {
            printf("I2C-PROBE-ANSWER 0x%02X 0x%04X\n", (unsigned)addr, v);
            found++;
        }
        nd_pcf8575_close(&chip);
    }
    printf("I2C-PROBE-COUNT %d\n", found);
}

/* chown(2) is warn_unused_result under -Werror, and a chown that failed would
 * make the next assertion measure a state nobody set. Say so rather than
 * casting it to void. */
static void set_owner(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
    if (chown(path, uid, gid) != 0 || chmod(path, mode) != 0)
        printf("I2C-ACL-SETUP-FAILED %s %s\n", path, strerror(errno));
}

/* The three-state ownership probe. It really forks, really setgroups,
 * setgid and setuid, and reports what the KERNEL decided -- not what a rules
 * file says it should have decided. */
static void open_as(const char *label, const char *path, uid_t uid, gid_t gid)
{
    pid_t pid = fork();

    if (pid == 0) {
        gid_t only = gid;
        int fd;
        if (setgroups(1u, &only) != 0 || setgid(gid) != 0 || setuid(uid) != 0)
            _exit(70);
        fd = open(path, O_RDWR);
        if (fd < 0)
            _exit(errno == EACCES ? 13 : 71);
        (void)close(fd);
        _exit(0);
    }
    if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
        st = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
        printf("I2C-ACL %s %s\n", label, st == 0 ? "OPEN-OK" : (st == 13 ? "EACCES" : "OTHER"));
    }
}

int main(int argc, char **argv)
{
    static const uint8_t rows[4] = {0, 1, 2, 3};
    static const uint8_t cols[4] = {4, 5, 6, 7};
    const char *dev = "/dev/i2c-3";
    int bus = 3;
    int passes = (argc > 1) ? atoi(argv[1]) : 40;
    nd_matrix_scanner s;
    nd_pcf8575 raw;
    struct stat st;
    unsigned long funcs = 0ul;
    int fd;
    int i;

    if (stat(dev, &st) == 0)
        printf("I2C-NODE %s mode=%04o uid=%u gid=%u major=%u minor=%u\n", dev,
               (unsigned)(st.st_mode & 07777u), (unsigned)st.st_uid, (unsigned)st.st_gid,
               (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
    else
        printf("I2C-NODE %s ABSENT\n", dev);

    fd = open(dev, O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, I2C_FUNCS, &funcs) == 0)
            printf("I2C-FUNCS 0x%08lx I2C_FUNC_I2C=%s\n", funcs,
                   (funcs & FUNC_I2C) ? "yes" : "no");
        (void)close(fd);
    }

    /* The phone's own transaction, through the phone's own driver. */
    if (nd_pcf8575_open(&raw, bus, 0x20) == ND_OK) {
        uint16_t v = 0u;
        nd_err w = nd_pcf8575_write16(&raw, 0xFFFEu);
        nd_err r = nd_pcf8575_read16(&raw, &v);
        printf("I2C-XFER write=%d read=%d value=0x%04X stage=%s errno=%d\n", (int)w, (int)r, v,
               nd_pcf8575_stage_name(raw.last_stage), raw.last_errno);
        nd_pcf8575_close(&raw);
    } else {
        printf("I2C-XFER open-failed stage=%s errno=%d\n", nd_pcf8575_stage_name(raw.last_stage),
               raw.last_errno);
    }

    probe_addresses(bus);

    /* ============ THE OTHER CHIP ON THE PHONE'S BUS ============
     *
     * nd_battery.c opens the MAX17048 at 0x36 a few lines BEFORE the keypad in
     * nd_ui_init(), on this same node. Until this stage the emulator had no
     * /dev/i2c-3 at all, so the gauge got ENOENT, nd_battery_errno_means_absent()
     * was true and the meter simulated. The moment the node exists that stops
     * being true and there is no going back to SIM: nd_battery.c says in its own
     * words that "past open() the node demonstrably exists, so every remaining
     * failure is UNREADABLE by construction". So this assertion is not a bonus
     * chip -- it is the check that this stage did not turn the emulator's battery
     * meter into a fault. */
    {
        nd_battery *b = NULL;
        if (nd_battery_open(&b, bus, 0x36) == ND_OK) {
            const char *warn = nd_battery_poll(b, true);
            double v = 0.0;
            (void)warn;
            (void)nd_battery_vcell(b, &v);
            printf("I2C-GAUGE source=%d hardware=%d vcell=%.3f fault=%s\n",
                   (int)nd_battery_source_of(b), nd_battery_has_hardware(b) ? 1 : 0, v,
                   nd_battery_fault(b));
            nd_battery_close(b);
        } else {
            printf("I2C-GAUGE open-failed\n");
        }
    }

    /* THE OWNERSHIP PROBE, in all three states the boot can produce. uid 1000
     * and gid 1002 are ndusr and i2c out of users-table.txt. */
    set_owner(dev, 0u, 0u, 0600);
    open_as("devtmpfs-0600-root", dev, 1000u, 1002u);
    set_owner(dev, 0u, 1002u, 0660);
    open_as("udev-0660-i2c", dev, 1000u, 1002u);
    set_owner(dev, 0u, 0u, 0660);
    open_as("udev-0660-unknown-group", dev, 1000u, 1002u);
    /* Put it back the way the kernel made it, so the scan below is not testing
     * a mode this function invented. */
    set_owner(dev, 0u, 0u, 0600);

    /* ============ nd_input's OWN BACKEND SELECTION ============
     *
     * This is the first machine anywhere with BOTH an i2c matrix and an evdev
     * keyboard, which is the phone's own arrangement -- nd_input_open() opens
     * the matrix first and then opens an evdev device REGARDLESS, "so a
     * developer with a USB keyboard plugged into a real phone can still
     * type". Until now the emulator had only the keyboard and the phone had
     * only the matrix, so the CHOICE between them had never run.
     *
     * NEODCT_ROOT points at a small tree this boot writes, holding the
     * keymap.json the wizard would have enrolled. Everything below it is the
     * shipping nd_input.c, nd_keymap.c, nd_evdev.c and nd_matrix.c. */
    {
        nd_input *in = NULL;
        if (nd_input_open(&in) == ND_OK) {
            printf("I2C-BACKEND matrix=%d\n", nd_input_has_matrix(in) ? 1 : 0);
            nd_input_close(in);
        } else {
            printf("I2C-BACKEND open-failed\n");
        }
    }

    if (nd_matrix_scanner_init(&s, rows, 4u, cols, 4u, bus, 0x20) != ND_OK) {
        printf("I2C-SCAN init-failed\n");
        return 1;
    }
    printf("I2C-SCAN-READY\n");
    fflush(stdout);
    for (i = 0; i < passes; i++) {
        nd_matrix_pos p;
        bool found = false;
        nd_err rc = nd_matrix_scan_once(&s, &p, &found);
        if (rc != ND_OK)
            printf("I2C-SCAN pass=%d rc=%d stage=%s errno=%d\n", i, (int)rc,
                   nd_pcf8575_stage_name(s.chip.last_stage), s.chip.last_errno);
        else if (found)
            printf("I2C-SCAN pass=%d PRESS row=%u col=%u\n", i, (unsigned)p.row, (unsigned)p.col);
        else {
            int r;
            int c;
            int held = 0;
            for (r = 0; r < 4; r++) {
                for (c = 0; c < 4; c++) {
                    if (nd_matrix_is_held(&s, (uint8_t)r, (uint8_t)c)) {
                        held = 1;
                        printf("I2C-SCAN pass=%d HELD row=%d col=%d\n", i, r, c);
                    }
                }
            }
            if (held == 0)
                printf("I2C-SCAN pass=%d no key\n", i);
        }
        fflush(stdout);
        usleep(100000);
    }
    nd_matrix_scanner_close(&s);
    printf("I2C-SCAN-DONE\n");
    return 0;
}
PROBE

# The source list is the LINK CLOSURE of what the probe calls and not a
# selection. nd_matrix.c calls nd_input_errno_is_transient(), which is the
# shared table deciding whether a bus failure is worth retrying -- the one
# this transport's errno gap makes answer differently here from on the phone
# -- and nd_battery.c reaches nd_settings and nd_props for the two overrides a
# phone can carry. Nothing here is a stub or a shim: every i2c transaction the
# guest makes below is made by a file that ships. -lm is nd_util.c's
# round-half-even.
"${CROSS}gcc" $WARN -I"$SRC/include" -static -o "$WORK/i2cprobe" \
    "$WORK/i2cprobe.c" "$SRC/lib/nd_pcf8575.c" "$SRC/lib/nd_matrix.c" \
    "$SRC/lib/nd_input.c" "$SRC/lib/nd_evdev.c" "$SRC/lib/nd_keymap.c" \
    "$SRC/lib/nd_keycodes.c" "$SRC/lib/nd_json.c" "$SRC/lib/nd_utf8.c" \
    "$SRC/lib/nd_battery.c" "$SRC/lib/nd_settings.c" "$SRC/lib/nd_vclock.c" \
    "$SRC/lib/nd_props.c" \
    "$SRC/lib/nd_log.c" "$SRC/lib/nd_path.c" "$SRC/lib/nd_util.c" -lm 2>"$WORK/cc.log" || {
    echo "cross build of the i2c probe failed:" >&2; cat "$WORK/cc.log" >&2; exit 1; }
"${CROSS}strip" "$WORK/i2cprobe" 2>/dev/null || true

cp -a "$ROOTFS" "$WORK/root"
cp "$WORK/i2cprobe" "$WORK/root/bin/i2cprobe"

cat > "$WORK/root/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
# The keymap the first-boot wizard would have enrolled -- rows P00-P03,
# columns P04-P07, address 0x20, bus 3 -- so nd_input_open() has something to
# load. It goes at the REAL path and NOT under a NEODCT_ROOT, and that is not
# a shortcut: nd_path_resolve() prefixes EVERY path, /dev/i2c-3 included, so a
# root pointed at a fixture makes nd_pcf8575_open() look for /nd/dev/i2c-3 and
# the whole boot then measures a bus that is not there. Measured once in this
# file, and nd_pcf8575.c's own header records the same trap from the host
# suite. It is written by the guest rather than shipped in the rootfs because
# keymap.json lives on /NeoDCT/User on a real phone -- the one partition an
# .ndsw never replaces -- and a keymap baked into a rootfs is the arrangement
# the wizard exists to avoid.
mkdir -p /NeoDCT/User
cat > /NeoDCT/User/keymap.json <<'KM'
{
  "by_code": {},
  "by_matrix": {"1,1": "num_2"},
  "col_pins": [4, 5, 6, 7],
  "driver": "pcf8575-i2c",
  "format": "neodct.keymap.v3.matrix.i2c",
  "i2c_addr": 32,
  "i2c_bus": 3,
  "keys": {
    "num_2": {"col": 1, "col_pin": 5, "label": "2", "row": 1, "row_pin": 1}
  },
  "output": "/NeoDCT/User/keymap.json",
  "row_pins": [0, 1, 2, 3]
}
KM
echo "===I2C-BEGIN"
echo "I2C-EVDEV $(ls /dev/input 2>/dev/null | tr '\n' ' ')"
for d in /sys/class/i2c-dev/*; do
    [ -e "$d" ] || continue
    echo "I2C-ADAPTER $(basename "$d") name=$(cat "$d/name")"
    echo "I2C-SUBSYSTEM $(basename "$d") $(basename "$(readlink -f "$d/subsystem")")"
    echo "I2C-DEVLINK $(basename "$d") $(readlink -f "$d/device")"
done
ls /dev/i2c-* 2>/dev/null | while read -r n; do echo "I2C-DEVNODE $n"; done
/bin/i2cprobe 40
echo "===I2C-MEM $(grep MemTotal /proc/meminfo)"
echo "===I2C-END"
poweroff -f
INIT
chmod +x "$WORK/root/init"
( cd "$WORK/root" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$WORK/initramfs.cpio.gz"

# ------------------------------------------------------------------ #
# The host side: the daemon, then QEMU, then keys
# ------------------------------------------------------------------ #
KEYPADD_PID=$(nd_keypadd_start "$HERE" "$WORK/i2c.sock" "$WORK/keys" "$WORK/keypadd.log") || {
    echo "REFUSED: the keypad daemon would not start; QEMU would refuse to boot" >&2
    # `>&2` FIRST. Redirections apply left to right, so the old
    # `2>/dev/null >&2` pointed fd 2 at /dev/null and then duplicated
    # THAT into fd 1 -- the daemon's log went to /dev/null on both
    # descriptors, and this refusal printed the path of a file the EXIT
    # trap was about to delete. Measured on a socket path too long for a
    # sockaddr_un: the one line explaining it was unrecoverable.
    cat "$WORK/keypadd.log" >&2 2>/dev/null
    exit 1; }
I2C_ARGS=$(nd_qemu_i2c_args "$WORK/i2c.sock" 64) || exit 1

# The keys are scripted rather than typed, because this is a gate. `num_2` is
# row 1 column 1 in the enrolment-order layout nd-i2c-keypadd carries, and the
# assertion below names those two numbers so that a layout change fails here
# rather than in somebody's fingers.
#
# IT WAITS FOR THE GUEST TO SAY IT IS SCANNING, and does not sleep. Measured
# why: a `sleep 12` pressed the key long after the probe's forty passes had
# finished, so the run reported "no PRESS" -- a green-looking transport and a
# red-looking test, which is the worst way round. The guest prints
# I2C-SCAN-READY on the console the instant nd_matrix_scanner_init() returns,
# and the console is this file.
( waited=0
  while ! grep -q 'I2C-SCAN-READY' "$WORK/boot.log" 2>/dev/null; do
      waited=$((waited + 1))
      [ "$waited" -gt 1200 ] && break
      sleep 0.1
  done
  echo "press num_2" > "$WORK/keys"
  sleep 1.5
  echo "release all" > "$WORK/keys" ) &
KEYS_PID=$!

timeout 180 qemu-system-arm \
    -M virt -cpu cortex-a7 -smp 1 -m 64 -nographic \
    -global virtio-mmio.force-legacy=false \
    -kernel "$KERNEL" \
    -initrd "$WORK/initramfs.cpio.gz" \
    -dtb "$WORK/nd.dtb" \
    $I2C_ARGS \
    -device virtio-keyboard-device \
    -append "console=ttyAMA0 rdinit=/init panic=5 video=vfb:on $(nd_qemu_append)" \
    > "$WORK/boot.log" 2>&1 || true

tr -d '\r' < "$WORK/boot.log" > "$WORK/boot.txt"

grep -q '===I2C-END' "$WORK/boot.txt" || {
    echo "REFUSED: the guest never finished. Last of the console:" >&2
    tail -30 "$WORK/boot.txt" >&2
    exit 1; }

# ------------------------------------------------------------------ #
# The assertions
# ------------------------------------------------------------------ #

adapters=$(grep -c '^I2C-ADAPTER ' "$WORK/boot.txt" || true)
[ "$adapters" = "1" ] && ok "exactly one i2c adapter" || bad "expected 1 adapter, got $adapters"

if grep -q '^I2C-ADAPTER i2c-3 ' "$WORK/boot.txt"; then
    ok "the adapter is i2c-3 -- $(grep '^I2C-ADAPTER i2c-3 ' "$WORK/boot.txt")"
else
    bad "the adapter is not i2c-3: $(grep '^I2C-ADAPTER ' "$WORK/boot.txt" || echo none).
     Every default in this tree -- ND_I2C_BUS_DEFAULT, ND_KPSETUP_DEFAULT_BUS,
     ND_BATT_DEFAULT_I2C_BUS -- is 3, so a bus anywhere else needs three
     per-machine overrides. The number comes from the reservation node in
     nd-virt-additions.dtsi."
fi

grep -q '^I2C-SUBSYSTEM i2c-3 i2c-dev' "$WORK/boot.txt" \
    && ok "its subsystem is i2c-dev, which is 61-neodct-devices.rules' exact match key" \
    || bad "the node's subsystem is not i2c-dev, so the udev rule cannot match it"

node=$(grep '^I2C-NODE ' "$WORK/boot.txt" | head -1)
case "$node" in
    *"mode=0600 uid=0 gid=0 major=89 minor=3")
        ok "devtmpfs made it char 89:3 root:root 0600 -- $node" ;;
    *) bad "the node is not char 89:3 root:root 0600: $node" ;;
esac

funcs=$(grep '^I2C-FUNCS ' "$WORK/boot.txt" | head -1)
case "$funcs" in
    *"I2C_FUNC_I2C=yes") ok "$funcs" ;;
    *) bad "the adapter has no I2C_FUNC_I2C: $funcs.
     This is what i2c-stub looks like: a perfectly good adapter whose every
     plain transfer returns EOPNOTSUPP, because its algorithm declares
     .smbus_xfer and never .master_xfer." ;;
esac

xfer=$(grep '^I2C-XFER ' "$WORK/boot.txt" | head -1)
case "$xfer" in
    "I2C-XFER write=0 read=0 value=0xfffe"*|"I2C-XFER write=0 read=0 value=0xFFFE"*)
        ok "the phone's own write16/read16 round-tripped -- $xfer" ;;
    *) bad "nd_pcf8575's two-byte write and read did not round-trip: $xfer" ;;
esac

answers=$(grep -c '^I2C-PROBE-ANSWER ' "$WORK/boot.txt" || true)
count=$(sed -n 's/^I2C-PROBE-COUNT \([0-9]*\)$/\1/p' "$WORK/boot.txt" | head -1)
if [ "$answers" = "1" ] && [ "${count:-x}" = "1" ] &&
   grep -q '^I2C-PROBE-ANSWER 0x20 ' "$WORK/boot.txt"; then
    ok "of 0x20..0x27 exactly one address answered, and it is 0x20"
else
    bad "the eight-address probe found $count chips, not one:
$(grep '^I2C-PROBE-ANSWER ' "$WORK/boot.txt" || echo '     (none)')
     nd_kpsetup_probe() takes the FIRST address that answers, so a model that
     ACKs everything enrols 0x20 by accident and one that ACKs nothing leaves
     the phone keyless."
fi

# ============ THE CHOICE, ON A MACHINE THAT HAS BOTH ============
#
# This boot attaches a virtio keyboard as well as the keypad bus, which is
# what run_qemu.sh does by default and what a phone with a USB keyboard
# plugged in looks like. The assertion is that the MATRIX wins -- if evdev
# ever won silently, every text widget in the system would take the
# DEV_KEYMAP path and T9 would vanish with nothing saying so.
grep -q '^I2C-EVDEV .*event0' "$WORK/boot.txt" \
    && ok "the guest also has /dev/input/event0, so the choice is a real one" \
    || bad "no evdev device in this boot, so nd_input had nothing to choose between:
     $(grep '^I2C-EVDEV ' "$WORK/boot.txt" || echo '     (no line)')"
backend=$(grep '^I2C-BACKEND ' "$WORK/boot.txt" | head -1)
[ "$backend" = "I2C-BACKEND matrix=1" ] \
    && ok "nd_input_open() chose the i2c matrix over evdev" \
    || bad "nd_input_open() did not choose the matrix: '$backend'.
     The boot log's 'Input backend selected:' line says why it fell back."

gauge=$(grep '^I2C-GAUGE ' "$WORK/boot.txt" | head -1)
case "$gauge" in
    "I2C-GAUGE source=1 hardware=1 "*)
        ok "the fuel gauge on the same adapter is LIVE -- $gauge" ;;
    *) bad "nd_battery is not LIVE on the new bus: $gauge.
     source=0 is SIM and source=2 is UNREADABLE. Once /dev/i2c-3 exists the
     gauge can never go back to SIM -- nd_battery.c: 'past open() the node
     demonstrably exists, so every remaining failure is UNREADABLE by
     construction' -- so a keypad daemon that NAKs 0x36 does not leave the
     meter simulating, it turns it into a fault." ;;
esac

# ============ THE TEST THAT WOULD HAVE CAUGHT THE EACCES ============
#
# Three states, and the kernel's answer for each. The third is the one no
# other test in the tree can reach: it is what eudev produces when GROUP=
# names a group that does not exist -- it falls back to gid 0 -- and from the
# UI's side it is indistinguishable from no rule at all. A missing `i2c` line
# in users-table.txt is a permission bug on the phone and not a build error;
# this is the assertion that turns it back into a build failure.
acl_check() {
    want="$2"
    got=$(sed -n "s/^I2C-ACL $1 \(.*\)$/\1/p" "$WORK/boot.txt" | head -1)
    [ "$got" = "$want" ] && ok "ownership $1 -> $got" \
        || bad "ownership $1 gave '$got', expected '$want'"
}
acl_check devtmpfs-0600-root        EACCES
acl_check udev-0660-i2c             OPEN-OK
acl_check udev-0660-unknown-group   EACCES

# ============ AND A HOST KEYSTROKE REACHING THE SHIPPING SCANNER ============
presses=$(grep -c '^I2C-SCAN .* PRESS ' "$WORK/boot.txt" || true)
if [ "$presses" = "1" ] && grep -q 'PRESS row=1 col=1' "$WORK/boot.txt"; then
    ok "one press, at row 1 col 1 -- $(grep 'PRESS row=' "$WORK/boot.txt")"
else
    bad "expected exactly one PRESS at row 1 col 1 (num_2), got $presses:
$(grep '^I2C-SCAN ' "$WORK/boot.txt" | grep PRESS || echo '     (none)')"
fi
held=$(grep -c 'HELD row=1 col=1' "$WORK/boot.txt" || true)
[ "${held:-0}" -ge 1 ] \
    && ok "nd_matrix_is_held() reported it held for $held further scans" \
    || bad "the key was never reported HELD, so the debounce did not run"

mem=$(sed -n 's/^===I2C-MEM MemTotal: *\([0-9]*\) kB/\1/p' "$WORK/boot.txt" | head -1)
# 54,808 kB and not 54,812: the reservation node makes the device tree bigger
# and the kernel reserves fdt_totalsize(). The number is here, in
# test_qemu_surfaces.sh and in the parity capture, so moving it means typing
# it three times.
if [ "${mem:-0}" = "${ND_MEM_I2C:-54808}" ]; then
    ok "MemTotal ${mem} kB"
else
    bad "MemTotal is ${mem:-nothing} kB, expected ${ND_MEM_I2C:-54808}.
     The 64 MB machine's memory parity with the phone's ~54 MB is the claim
     the kernel config header makes; a move here is that claim changing."
fi

echo
if [ "$fail" = "0" ]; then
    echo "test_qemu_i2c: all assertions passed"
else
    cp "$WORK/boot.txt" "${TMPDIR:-/tmp}/nd-qemu-i2c-failed.log" 2>/dev/null || true
    cp "$WORK/keypadd.log" "${TMPDIR:-/tmp}/nd-qemu-i2c-failed-keypadd.log" 2>/dev/null || true
    echo "test_qemu_i2c: FAILURES above. The guest console is in"
    echo "  ${TMPDIR:-/tmp}/nd-qemu-i2c-failed.log and the daemon's log in"
    echo "  ${TMPDIR:-/tmp}/nd-qemu-i2c-failed-keypadd.log. ND_KEEP_WORK=1 keeps"
    echo "  the whole work directory, which is the only way to see the device tree."
fi
exit "$fail"
