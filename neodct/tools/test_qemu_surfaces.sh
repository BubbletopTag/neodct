#!/bin/sh
# test_qemu_surfaces.sh -- boot the emulator and ask the kernel whether the
# four small hardware surfaces are what this tree says they are.
#
# ============ WHY THIS IS A BOOT AND NOT A UNIT TEST ============
#
# `make test` builds a sysfs tree under a case root and drives nd_backlight.c
# and nd_cpufreq.c against it. That checks the code and it cannot check the
# machine: a fixture agrees with whoever wrote it. Three of the things below
# are properties of a running kernel and of nothing else --
#
#   * that /sys/class/backlight has exactly ONE device and it is called
#     `backlight`, because nd_backlight.c takes the lexicographically smallest
#     entry with a brightness file and a second device would silently change
#     which panel dims;
#   * that a min-before-max write while raising is SWALLOWED. nd_cpufreq.h has
#     a paragraph about that failure and test_cpufreq.c says in its header that
#     it cannot check it, because two ordinary files hold both values whichever
#     order they were written in. A driver clamps. This is the only place in
#     the project where the wrong order actually loses a write;
#   * that /sys/class/power_supply and /sys/class/thermal are EMPTY and
#     /sys/class/leds does not exist. Those are deliberate absences with
#     reasons in neodct/tests/parity/allow.txt, and the way they would come
#     back is a kernel config change nobody re-booted -- CONFIG_TEST_POWER
#     alone brings back three fake supplies AND a thermal zone typed
#     `test_battery`.
#
# ============ AND WHY IT TAKES A ROOTFS RATHER THAN AN IMAGE ============
#
# buildroot/output does not exist in a fresh checkout and a full build is
# hours, so this asks for a kernel and a busybox directory, exactly as
# parity_capture_probe.sh does. What it boots is run_qemu.sh's machine,
# run_qemu.sh's device tree AND run_qemu.sh's kernel parameters -- the recipes
# are nd_dtb_build() and nd_qemu_append() in qemu_machine.sh, sourced by all
# three, so they cannot drift apart. The parameters used to be hand-copied
# here and the copy had already lost the nandsim ID bytes, `video=vfb:on`,
# `neodct.devenv=1` and `vt.global_cursor_default=0` while the comment beside
# it still called them "run_qemu.sh's parameters, copied rather than
# invented". AGENTS.md calls this script the thing that notices when the
# parity number moves, and it was measuring MemTotal on a command line
# run_qemu.sh does not use.
#
# Usage:
#   test_qemu_surfaces.sh --kernel <zImage> --rootfs <busybox rootfs dir>
#                         [--work <dir>]
#
# Environment, for the day the kernel config changes and the numbers move:
#   ND_MEM_DTB=54808    expected MemTotal with the device tree
#   ND_MEM_NODTB=53824  expected MemTotal without it
set -eu

KERNEL=""
ROOTFS=""
WORK="${TMPDIR:-/tmp}/nd-surfaces.$$"
HERE="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
DTSI="$HERE/../board/qemu/nd-virt-additions.dtsi"

# The two numbers four documents quote. They are here so that a kernel symbol
# added without a re-boot fails a test instead of quietly making AGENTS.md,
# BUILDING.md, docs/BLUETOOTH.md and the config header wrong at the same time.
# 54,808 and not 54,812: the keypad stage added a bus-number reservation node
# to nd-virt-additions.dtsi, the tree got bigger, and the kernel reserves
# fdt_totalsize(). The 4 kB is the device tree and NOT the i2c device --
# measured, a boot with the vhost-user bus attached and one without it report
# the same number, because `-object memory-backend-memfd,share=on` changes how
# guest RAM is allocated and not how much of it there is.
MEM_DTB="${ND_MEM_DTB:-54808}"
MEM_NODTB="${ND_MEM_NODTB:-53824}"

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --rootfs) ROOTFS="$2"; shift 2 ;;
        --work)   WORK="$2";   shift 2 ;;
        -h|--help) sed -n '2,45p' "$0"; exit 2 ;;
        *) echo "test_qemu_surfaces.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[ -n "$KERNEL" ] && [ -n "$ROOTFS" ] || {
    echo "usage: test_qemu_surfaces.sh --kernel zImage --rootfs DIR" >&2; exit 2; }
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL" >&2; exit 2; }
[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS" >&2; exit 2; }
command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm not found" >&2; exit 2; }

. "$HERE/qemu_machine.sh"

# The trap deletes $WORK recursively, and `mkdir -p` is happy with a directory
# that is already there -- so --work at an existing path used to destroy it.
if [ -e "$WORK" ]; then
    echo "REFUSED: --work $WORK already exists, and this script deletes its work" >&2
    echo "         directory recursively when it finishes. Name one that does not." >&2
    exit 2
fi
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

nd_dtb_build "$DTSI" "$WORK" "$WORK/nd.dtb" || {
    echo "test_qemu_surfaces: could not build the device tree; nothing to test." >&2
    exit 2; }

# The guest REPORTS and the host DECIDES. A guest that made the judgements
# would have to be trusted to have run them all, and a boot that dies halfway
# through looks the same as one that passed -- so every value comes out as a
# framed record and the assertions are below, where a failure can print what
# was actually there.
cp -a "$ROOTFS" "$WORK/root"
cat > "$WORK/root/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
say() { echo "SURF|$1|$2"; }
echo "===SURFACES-BEGIN"
say mem.total_kb "$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"

# The keypad's bus. `[]` here is the machine as it was before Stage 4, and it
# is the shape of an i2c subsystem compiled in with no adapter behind it --
# which is the distinction this whole file exists to make.
if [ -d /sys/class/i2c-dev ]; then
    say class.i2c-dev "[$(ls /sys/class/i2c-dev | tr '\n' ' ')]"
else
    say class.i2c-dev ABSENT
fi
if [ -e /sys/class/i2c-dev/i2c-3/name ]; then
    say i2c3.name "$(cat /sys/class/i2c-dev/i2c-3/name)"
    say i2c3.node "$(ls -l /dev/i2c-3 | awk '{print $1, $3, $4}')"
fi

for c in backlight power_supply thermal leds; do
    if [ -d "/sys/class/$c" ]; then
        say "class.$c" "[$(ls /sys/class/$c | tr '\n' ' ')]"
    else
        say "class.$c" ABSENT
    fi
done

D=/sys/class/backlight/backlight
if [ -d "$D" ]; then
    say bl.max_brightness "$(cat "$D/max_brightness")"
    say bl.bl_power "$(cat "$D/bl_power")"
    printf 5 > "$D/brightness"; say bl.wrote5 "$(cat "$D/brightness")"
    printf 10 > "$D/brightness"
    say bl.type "$(cat "$D/type")"
fi

C=/sys/devices/system/cpu/cpu0/cpufreq
if [ -d "$C" ]; then
    say cpufreq.present yes
    say cpufreq.available "$(cat "$C/scaling_available_frequencies")"
    say cpufreq.driver "$(cat "$C/scaling_driver")"
    say cpufreq.hw_min "$(cat "$C/cpuinfo_min_freq")"
    say cpufreq.hw_max "$(cat "$C/cpuinfo_max_freq")"
    # Pin low the RIGHT way round (min first when lowering), then try to raise
    # the WRONG way round (min first when raising) and report what stuck.
    printf 816000 > "$C/scaling_min_freq"
    printf 816000 > "$C/scaling_max_freq"
    say cpufreq.pinned_min "$(cat "$C/scaling_min_freq")"
    say cpufreq.pinned_cur "$(cat "$C/scaling_cur_freq")"
    printf 1200000 > "$C/scaling_min_freq"
    say cpufreq.min_after_wrong_order "$(cat "$C/scaling_min_freq")"
    printf 1200000 > "$C/scaling_max_freq"
    say cpufreq.min_after_max "$(cat "$C/scaling_min_freq")"
    printf 408000 > "$C/scaling_min_freq"
else
    say cpufreq.present no
fi

for p in 53 56 57; do
    if printf '%s' "$p" > /sys/class/gpio/export 2>/dev/null &&
       printf high > "/sys/class/gpio/gpio$p/direction" 2>/dev/null; then
        say "gpio$p" "dir=$(cat /sys/class/gpio/gpio$p/direction) value=$(cat /sys/class/gpio/gpio$p/value)"
    else
        say "gpio$p" UNAVAILABLE
    fi
done

say rtc0.name "$(cat /sys/class/rtc/rtc0/name 2>/dev/null || echo ABSENT)"
say psy.parent_warnings "$(dmesg | grep -c 'Expected proper parent device')"

# The flash. nandsim.parts= is in nd_qemu_append() and it is the one parameter
# here whose absence is invisible until somebody tries to attach UBI: without
# it the chip is one 128 MB partition, the phone's mtd4 and mtd5 do not exist,
# and neodct.user=ubi1:userdata has nothing to resolve against. writesize is
# the load-bearing number -- UBI reads min_io straight off it -- and 1 instead
# of 2048 is the whole difference between mtdram and the phone's part.
say mtd.count "$(sed -n 's/^mtd\([0-9]*\):.*/\1/p' /proc/mtd | wc -l)"
say mtd.class "[$(ls /sys/class/mtd | tr '\n' ' ')]"
say mtd4.size "$(cat /sys/class/mtd/mtd4/size 2>/dev/null || echo ABSENT)"
say mtd4.writesize "$(cat /sys/class/mtd/mtd4/writesize 2>/dev/null || echo ABSENT)"
say mtd4.erasesize "$(cat /sys/class/mtd/mtd4/erasesize 2>/dev/null || echo ABSENT)"
say mtd5.size "$(cat /sys/class/mtd/mtd5/size 2>/dev/null || echo ABSENT)"
echo "===SURFACES-END"
poweroff -f
INIT
chmod +x "$WORK/root/init"
( cd "$WORK/root" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$WORK/initramfs.cpio.gz"

# run_qemu.sh's machine, and run_qemu.sh's parameters through the function
# they both call rather than through a copy: force-legacy=false is here (it is
# a QEMU flag, not a kernel parameter, and without it virtio_input is refused
# SILENTLY), and everything that decides which DEVICES exist comes from
# nd_qemu_append(). console= and rdinit= are this boot's own.
boot() {   # boot <base name> [extra qemu args...]
    _log="$1.log"; shift
    timeout 300 qemu-system-arm \
        -M virt -cpu cortex-a7 -smp 1 -m 64 -nographic \
        -global virtio-mmio.force-legacy=false \
        -kernel "$KERNEL" -initrd "$WORK/initramfs.cpio.gz" "$@" \
        -append "console=ttyAMA0 rdinit=/init panic=5 $(nd_qemu_append)" \
        > "$_log" 2>&1 || true
    grep -q '===SURFACES-END' "$_log" || {
        echo "test_qemu_surfaces: the guest did not finish; see $_log" >&2
        return 1; }
    tr -d '\r' < "$_log" | sed -n 's/^SURF|//p' > "${_log%.log}.rec"
}

FAILED=0
val() { sed -n "s/^$1|//p" "$2.rec" | head -1; }
check() {   # check <log> <key> <expected> <why>
    _got="$(val "$2" "$1")"
    if [ "$_got" = "$3" ]; then
        echo "ok    $2 = $3"
    else
        echo "FAIL  $2: got '$_got', want '$3'"
        echo "        $4"
        FAILED=$((FAILED + 1))
    fi
}
# Same, but the expected value is a PREFIX. It exists for exactly one record:
# the i2c adapter's name ends in a digit that is not the bus number, and
# allow.txt's own class.i2c-dev.i2c-3.name entry argues at length that the
# digit must not be pinned -- so pinning it here, under an explanation about
# the phone's rk3x controller, would send the next reader to an allowlist that
# says the opposite.
check_prefix() {   # check_prefix <log> <key> <expected prefix> <why>
    _got="$(val "$2" "$1")"
    case "$_got" in
        "$3"*) echo "ok    $2 = $_got" ;;
        *)
            echo "FAIL  $2: got '$_got', want something starting '$3'"
            echo "        $4"
            FAILED=$((FAILED + 1))
            ;;
    esac
}

# ============ THE KEYPAD BUS IS PART OF run_qemu.sh's MACHINE ============
#
# So it is part of this boot. A surfaces run without it would assert against a
# machine nobody boots, which is the same argument the device tree carries
# forty lines up -- and it is the argument that already cost this file once,
# when its hand-copied kernel parameters had lost the nandsim ID bytes.
KEYPADD_PID=""
# `|| true`, because under `set -e` a kill of a pid that has already gone is
# the LAST command of an AND-list and errexit applies to it -- the trap aborts
# and the script exits non-zero after saying every check passed.
trap 'rm -rf "$WORK"; if [ -n "$KEYPADD_PID" ]; then kill "$KEYPADD_PID" 2>/dev/null || true; fi' EXIT
KEYPADD_PID=$(nd_keypadd_start "$HERE" "$WORK/i2c.sock" "$WORK/keys" "$WORK/keypadd.log") || {
    echo "test_qemu_surfaces: the keypad daemon would not start, and QEMU refuses" >&2
    echo "  to boot against a vhost-user socket that is not there." >&2
    exit 2; }
I2C_ARGS=$(nd_qemu_i2c_args "$WORK/i2c.sock" 64) || exit 2

echo "== with the device tree =="
# shellcheck disable=SC2086  # I2C_ARGS is intentionally word-split
boot "$WORK/dtb" -dtb "$WORK/nd.dtb" $I2C_ARGS

check "$WORK/dtb" mem.total_kb "$MEM_DTB" \
    "the kernel reserves fdt_totalsize(); AGENTS.md, BUILDING.md, docs/BLUETOOTH.md and the kernel config header all quote this number"
check "$WORK/dtb" class.backlight "[backlight ]" \
    "exactly one device, named backlight -- nd_backlight.c takes the lexicographically smallest and a second one would change which panel dims"
check "$WORK/dtb" bl.max_brightness 10 \
    "the phone's eleven-entry brightness table, not a 0-255 range; docs/HARDWARE_NOTES.md has the node"
check "$WORK/dtb" bl.bl_power 0 \
    "bl_power 4 here means the .dtsi grew a label AND is being compiled with dtc -@, which is the fault docs/HARDWARE_NOTES.md records"
check "$WORK/dtb" bl.wrote5 5 \
    "sysfs validates on write and nd_backlight.c reads every write back"
check "$WORK/dtb" cpufreq.present yes \
    "ND_CPUFREQ_DIR is the only path nd_cpufreq.c opens"
check "$WORK/dtb" cpufreq.available "408000 600000 816000 1008000 1200000 " \
    "the RV1103's five operating points, in the .dtsi, with the trailing space the kernel really writes"
check "$WORK/dtb" cpufreq.driver cpufreq-dt \
    "the generic driver binding the .dtsi's operating-points-v2"
check "$WORK/dtb" cpufreq.hw_min 408000 "the silicon's floor, what nd_cpufreq_set_range(0,0) unpins to"
check "$WORK/dtb" cpufreq.hw_max 1200000 "the silicon's ceiling"
check "$WORK/dtb" cpufreq.pinned_cur 816000 \
    "scaling_cur_freq tracks the policy even though the stand-in's clock is fixed"
check "$WORK/dtb" cpufreq.min_after_wrong_order 816000 \
    "THE ONE THAT MATTERS: min written before max while RAISING is clamped away. nd_cpufreq_max_first() exists for this and no host test can see it"
check "$WORK/dtb" cpufreq.min_after_max 1200000 \
    "and it lands once the ceiling has moved, which is what makes the line above a swallowed write rather than a broken file"
check "$WORK/dtb" class.power_supply "[]" \
    "deliberately empty: nothing in this tree reads power_supply and the phone's battery is a MAX1704x on i2c-3. CONFIG_TEST_POWER coming back would put test_ac/test_battery/test_usb here"
check "$WORK/dtb" class.thermal "[]" \
    "deliberately empty: the only zone -M virt can produce is registered by power_supply and typed after the supply, i.e. test_battery"
check "$WORK/dtb" class.leds ABSENT \
    "not empty -- absent. LEDS_CLASS is what would create the directory and nothing in this tree reads LEDs"
check "$WORK/dtb" psy.parent_warnings 0 \
    "TEST_POWER printed three __power_supply_register warnings on every boot"
check "$WORK/dtb" gpio53 "dir=out value=1" \
    "the panel's BL wire, ND_BL_GPIO_PIN, granted by S90display; -M virt's own pl061 starts at 512 so this needs gpio-mockup"
check "$WORK/dtb" gpio56 "dir=out value=1" "the panel's RST, driven by neodctDisplay.c"
check "$WORK/dtb" gpio57 "dir=out value=1" "the panel's DC, driven by neodctDisplay.c"

check "$WORK/dtb" class.i2c-dev "[i2c-3 ]" \
    "exactly one adapter and it is bus THREE. ND_I2C_BUS_DEFAULT, ND_KPSETUP_DEFAULT_BUS and ND_BATT_DEFAULT_I2C_BUS are all 3, and the number comes from the reservation node in nd-virt-additions.dtsi, not from luck"
check_prefix "$WORK/dtb" i2c3.name "i2c_virtio at virtio bus " \
    "the vhost-user adapter, whose transfers nd-i2c-keypadd services. The phone's is an rk3x string, which is why this is a permanent allow.txt record. THE TRAILING DIGIT IS DELIBERATELY NOT PINNED: it is i2c-virtio's snprintf of vdev->index, the count of virtio devices QEMU made before this one, so it is a function of THIS BOOT'S DEVICE LIST and not of the adapter -- measured, 0 here, 1 in test_qemu_i2c.sh and 3 in the parity capture"
check "$WORK/dtb" i2c3.node "crw------- 0 0" \
    "devtmpfs makes it root:root 0600 and the udev rule is what changes that -- the window nd_kpsetup_open_keypad_as_root() exists to step over"

check "$WORK/dtb" mtd.count 6 \
    "nandsim.parts=2,2,4,128,64 must give SIX partitions -- five sizes and the remainder. Six sizes gives seven, and the seventh is 3 MiB of bad-block slack the phone's table has no name for"
check "$WORK/dtb" mtd.class "[mtd0 mtd0ro mtd1 mtd1ro mtd2 mtd2ro mtd3 mtd3ro mtd4 mtd4ro mtd5 mtd5ro ]" \
    "the phone's exact MTD key set, which is why class.mtd stopped being an allow.txt record"
check "$WORK/dtb" mtd4.size 8388608 \
    "docs/PARTITIONS.md's userdata partition, at the phone's own mtd number; neodct.user=ubi1:userdata resolves against a UBI device attached to this one"
check "$WORK/dtb" mtd4.writesize 2048 \
    "the Pico Mini's page size. UBI reads min_io straight off it: 2048 gives mknand.sh's LEB of 126,976, and mtdram's 1 gives 130,944 -- arithmetic the phone never does"
check "$WORK/dtb" mtd4.erasesize 131072 \
    "the Pico Mini's erase block, from the 0x15 ID byte's bits[5:4]"
check "$WORK/dtb" mtd5.size 108003328 \
    "the rootfs partition, 3 MiB larger than the phone's 100 MiB because the bad-block slack is folded into it rather than made a seventh partition. Nothing in this tree reads it -- mknand.sh's check_fits uses its own constant"

echo
echo "== without it, which is what a host with no dtc gets =="
boot "$WORK/nodtb"
check "$WORK/nodtb" mem.total_kb "$MEM_NODTB" \
    "no -dtb means no fdt reservation; this is the number the kernel config header quotes for a bare boot"
check "$WORK/nodtb" class.backlight "[]" \
    "the subsystem is built in and the DEVICE comes from the device tree -- which is the distinction the parity allowlist turns on"
check "$WORK/nodtb" cpufreq.present no \
    "cpufreq-dt binds nothing without operating-points-v2 on cpu@0"
check "$WORK/nodtb" class.i2c-dev "[]" \
    "and no keypad bus at all, because this boot attaches no vhost-user device -- which is what the emulator looked like before Stage 4: the subsystem compiled in, the class directory there, and nothing in it"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "test_qemu_surfaces: all checks passed"
    exit 0
fi
echo "test_qemu_surfaces: $FAILED check(s) failed"
exit 1
