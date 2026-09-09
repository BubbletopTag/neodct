# qemu_machine.sh -- the machine the NeoDCT emulator boots: the device tree,
# the kernel parameters that decide which devices exist, and the QEMU
# arguments for the keypad's i2c bus.
#
# Sourced, not run. run_qemu.sh sources it to boot the phone;
# test_qemu_surfaces.sh and parity_capture_probe.sh source it to boot a probe
# at the same devices. It is one file rather than three copies because the
# copies would drift and the drift would be invisible: the test would go on
# asserting a backlight against a recipe run_qemu.sh no longer uses.
#
# IT WAS CALLED qemu_dtb.sh AND HELD ONLY THE FIRST HALF, WHICH WAS HALF A
# FIX. The device tree was shared for exactly the reason above while the
# KERNEL COMMAND LINE -- which decides as many records as the tree does -- was
# hand-copied into all three, and the three copies had already drifted: the
# surfaces test was missing the nandsim ID bytes and its comment nonetheless
# said these were "run_qemu.sh's parameters, copied rather than invented".
# Measured consequence: delete `gpio-mockup.gpio_mockup_ranges=0,64` from
# run_qemu.sh as an obsolete-looking parameter and the emulator loses gpio53,
# 56 and 57 -- the backlight's GPIO tier and the panel's RST and DC -- while
# test_qemu_surfaces.sh goes on printing `ok gpio53 = dir=out value=1`,
# because it supplied its own copy.
#
# ============ WHY A CONCATENATION AND NOT AN OVERLAY ============
#
# `-M virt` generates its own tree and QEMU will not merge anything into it.
# The proper mechanism is a .dtbo applied with fdtoverlay, and it does not
# work here: `dtc -@` refuses a value reference to a base-tree node by path --
#
#     FATAL ERROR: Can't generate fixup for reference to path
#                  &{/pl061@9030000}
#
# -- because an overlay's references are resolved against __symbols__ at apply
# time, and QEMU's generated blob carries no __symbols__ for a label form to
# resolve against either. Decompiling the base, appending the .dtsi and
# compiling the whole thing is a full compile rather than an overlay fixup, so
# dtc resolves the pl061 phandle itself. Measured working end to end; the
# overlay route was measured failing.
#
# ============ AND NOTHING IS EVER COMMITTED OR CACHED ============
#
# `-M virt` is a versioned machine. A blob cut today still boots on next
# year's QEMU -- only /memory and /chosen are patched -- so a committed one
# would freeze the guest's device set to whatever QEMU produced the day it was
# cut, with nothing anywhere saying so. Regenerating from the installed binary
# every run cannot drift.
#
# The dump costs nothing extra: measured, a tree dumped with only
# -M/-cpu/-smp/-m is byte-identical to one dumped with the whole device set
# attached, apart from rng-seed and kaslr-seed.

# nd_dtb_build <dtsi> <work dir> <output .dtb>
#
# Returns 0 having written the tree, or non-zero having said why on stderr.
# The caller decides what a failure means -- run_qemu.sh boots without it and
# loses the backlight; the surfaces test fails.
nd_dtb_build() {
    _dtsi="$1"
    _work="$2"
    _out="$3"

    if ! command -v dtc >/dev/null 2>&1; then
        echo "qemu_machine: dtc not found (package device-tree-compiler)." >&2
        return 1
    fi
    if [ ! -f "$_dtsi" ]; then
        echo "qemu_machine: $_dtsi missing." >&2
        return 1
    fi
    mkdir -p "$_work" || return 1

    # $$ throughout and an atomic rename at the end, so two sessions on one
    # machine cannot hand a half-written tree to a booting kernel. They write
    # the same bytes anyway apart from the two seeds, which the kernel
    # replaces.
    qemu-system-arm -M "virt,dumpdtb=$_work/base.$$.dtb" \
        -cpu cortex-a7 -smp 1 -m 64 -nographic >/dev/null 2>&1 || {
        echo "qemu_machine: qemu-system-arm would not dump a device tree." >&2
        return 1; }
    dtc -I dtb -O dts -o "$_work/base.$$.dts" "$_work/base.$$.dtb" 2>/dev/null || {
        echo "qemu_machine: dtc could not decompile QEMU's tree." >&2
        return 1; }
    cat "$_work/base.$$.dts" "$_dtsi" > "$_work/nd.$$.dts" || return 1
    # 2>/dev/null: dtc warns about QEMU's own numeric `clocks` cells on every
    # run -- they are artefacts of decompiling a blob that has no phandles for
    # them -- and four warnings nobody can act on would train people to ignore
    # the fifth.
    dtc -I dts -O dtb -o "$_work/nd.$$.dtb" "$_work/nd.$$.dts" 2>/dev/null || {
        echo "qemu_machine: dtc could not compile the merged tree." >&2
        return 1; }
    mv -f "$_work/nd.$$.dtb" "$_out" || return 1
    rm -f "$_work/base.$$.dtb" "$_work/base.$$.dts" "$_work/nd.$$.dts"
    return 0
}

# nd_qemu_append
#
# The kernel parameters that decide WHICH DEVICES THE GUEST HAS -- which is
# exactly the set an nd-inventory capture records, and therefore the set that
# must be identical in every recipe or the capture describes a machine nobody
# boots. Echoed as one space-separated string; every caller prepends its own.
#
# What is deliberately NOT here, so that the omissions are a decision rather
# than an oversight:
#
#   console=, rdinit=, panic=, quiet, loglevel=   how this particular boot is
#       driven and where its output goes. run_qemu.sh boots an image and the
#       two probe scripts boot an initramfs; they cannot share these.
#   video=vfb:on   a DISPLAY decision, and run_qemu.sh makes a different one
#       per NEODCT_DISPLAY mode -- `none` is -nographic with no panel at all.
#       parity_capture_probe.sh adds it because a capture is only worth taking
#       of a session that has a framebuffer.
#   neodct.verity=, neodct.devenv=, neodct.recovery=, neodct.unsigned=
#       image policy. There is no image behind either probe script.
nd_qemu_append() {
    # nandsim at the Pico Mini's ID bytes -- 0x20/0xa1 is a 1 Gbit x8 part and
    # 0x15 is a 2 KiB page with a 128 KiB erase block -- and mtdram switched
    # off, because a built-in mtdram vmallocs its 4 MiB default out of a 64 MB
    # machine on every boot and reports write=1, so every LEB size computed on
    # it is arithmetic the phone never does.
    #
    # THE `gpio-mockup.` PREFIX IS REQUIRED AND ITS ABSENCE IS SILENT: for a
    # built-in driver the bare parameter name is handed to userspace, on one
    # line, at the end of a boot nobody reads, and the chip never appears.
    #
    # AND `nandsim.parts=` GIVES THAT CHIP THE PHONE'S PARTITION TABLE.
    # docs/PARTITIONS.md's table in 128 KiB erase blocks is 2,2,4,128,64,800 --
    # env, idblock, uboot, boot, userdata, rootfs -- and the value here stops
    # at FIVE on purpose. nandsim gives whatever is left over to a final
    # partition, so five sizes produce exactly six partitions at the phone's
    # numbers, with mtd4 the 8 MiB userdata and mtd5 the rootfs.
    #
    # Writing all six produces SEVEN. Measured: the 3 MiB of bad-block slack
    # becomes an mtd6 that the phone's table has no name for, `class.mtd` then
    # has a cardinality the phone can never match, and every mtd.byname family
    # record covers a seventh key that is pure emulator. The price of stopping
    # at five is an mtd5 of 103 MiB where the phone's is 100 -- a size nothing
    # in this tree reads, because mknand.sh's check_fits uses its own constant.
    #
    # Measured with it, on this kernel:
    #   mtd0 00040000  mtd1 00040000  mtd2 00080000
    #   mtd3 01000000  mtd4 00800000  mtd5 06700000
    #   class.mtd = [mtd0 mtd0ro mtd1 mtd1ro ... mtd5 mtd5ro]
    #
    # `nandsim.cache_file=` is deliberately NOT here, and the reason is worse
    # than "the probe has no cache disk". It names a literal /dev/vda that only
    # run_qemu.sh's NAND storage mode attaches, and nandsim opens it with
    # O_CREAT: measured, a cache_file naming a path that does not exist gets a
    # REGULAR FILE created there instead, in the initramfs, which is RAM. The
    # chip then works, writes run at 5 MB/s, no slab appears and nothing says a
    # word -- while the pages sit in the guest's own memory, which is the one
    # thing a cache file exists to prevent.
    echo "vt.global_cursor_default=0 mtdram.total_size=0 \
nandsim.first_id_byte=0x20 nandsim.second_id_byte=0xa1 \
nandsim.third_id_byte=0x00 nandsim.fourth_id_byte=0x15 \
nandsim.parts=2,2,4,128,64 \
gpio-mockup.gpio_mockup_ranges=0,64"
}

# nd_qemu_i2c_args <socket path> [guest MB]
#
# The QEMU arguments that give the guest a real i2c bus with a PCF8575 on it.
# Echoed as one space-separated string, empty when the emulator's QEMU cannot
# do it.
#
# ============ WHY THIS IS A SECOND FUNCTION AND NOT nd_qemu_append ============
#
# Everything nd_qemu_append() emits is a KERNEL PARAMETER. These are -object,
# -machine, -chardev and -device arguments, which the kernel never sees. Two
# different things in one string would be one string nobody could reuse: the
# capture scripts want the devices without necessarily wanting the same
# console, and a caller that wants neither still wants the nandsim bytes.
#
# It lives in THIS file for the reason the file exists: three recipes that
# hand-copy a machine drift, and the drift is invisible. The measured
# precedent is in this file's own header.
#
# ============ THE memfd IS NOT OPTIONAL ============
#
# vhost-user hands the backend a file descriptor for guest RAM and the backend
# mmaps it. QEMU's default anonymous guest memory is not shareable, so the
# backend maps nothing and every transfer silently does nothing --
# `-object memory-backend-memfd,share=on` plus `-machine memory-backend=` is
# what makes the RAM shareable. MEASURED that it costs nothing: MemTotal at
# -m 64 is 54,808 kB with it and without it, byte for byte.
#
# ============ AND A MISSING DAEMON IS A BOOT FAILURE, NOT A DEGRADED BOOT ===
#
# QEMU REFUSES TO START when the vhost-user socket is not there -- measured,
# `Failed to connect to '.../i2c.sock': No such file or directory` -- so the
# caller must start nd-i2c-keypadd BEFORE qemu and reap it on an EXIT trap.
# This function does not start anything; it only says what to put on the line.
nd_qemu_i2c_args() {
    _sock="$1"
    _mem="${2:-64}"

    # -M virt is a versioned machine and so is its device list. The tree is
    # already regenerated from the installed binary on every run for exactly
    # this reason; checking for the device rather than failing obscurely is
    # the same discipline one layer up.
    if ! qemu-system-arm -M virt -device help 2>/dev/null | grep -q '^name "vhost-user-i2c-device"'; then
        echo "qemu_machine: this qemu-system-arm has no vhost-user-i2c-device." >&2
        return 1
    fi
    echo "-object memory-backend-memfd,id=ndmem,size=${_mem}M,share=on \
-machine memory-backend=ndmem \
-chardev socket,id=ndi2c,path=$_sock \
-device vhost-user-i2c-device,chardev=ndi2c"
}

# nd_keypadd_start <tools dir> <socket> <fifo> <log>
#
# Starts nd-i2c-keypadd, waits for it to be listening, and echoes its pid.
# Returns non-zero having said why on stderr.
#
# The tools directory is an ARGUMENT and not `dirname $0`, because in a
# sourced file $0 is the SOURCING script and the three callers do not all live
# in the same directory. Every one of them already computes that path in order
# to source this file, so passing it costs a word and cannot be wrong.
#
# THE WAIT IS A READY FILE AND NOT A SLEEP. QEMU's refusal to start against a
# missing socket is immediate and total, so a `sleep 1` is a race that passes
# on the machine it was written on and turns a slow host into "the emulator
# does not boot".
nd_keypadd_start() {
    _kp_dir="$1"
    _kp_sock="$2"
    _kp_fifo="$3"
    _kp_log="$4"
    _kp_bin="$_kp_dir/nd-i2c-keypadd"
    _kp_src="$_kp_dir/nd-i2c-keypadd.c"
    _kp_ready="$_kp_sock.ready"

    if [ ! -x "$_kp_bin" ] || [ "$_kp_src" -nt "$_kp_bin" ]; then
        # Built on demand rather than committed, and rebuilt when the source
        # is newer. It is a HOST tool -- it never goes near the image and
        # never crosses the cross-compiler -- so requiring a make in another
        # directory before the emulator boots would be a step people work
        # around, and a stale binary is the bug that step would hide.
        if ! cc -O2 -o "$_kp_bin" "$_kp_src"; then
            echo "qemu_machine: cannot build nd-i2c-keypadd." >&2
            return 1
        fi
    fi
    # The READY file only. The SOCKET is deliberately left alone: it used to be
    # removed here, which is how a second session on one host silently took a
    # live session's socket away -- measured, the incumbent stayed alive with
    # its path gone. The daemon decides, because only it can tell a stale
    # socket file from one somebody is serving (it connects to it first).
    rm -f "$_kp_ready"
    # >/dev/null AND NOT >/dev/null 2>&1. stdout has to go, because this
    # function is called in a command substitution to capture the pid and a
    # background child holding that pipe open means `$(nd_keypadd_start ...)`
    # never returns -- measured, it hung until the qemu timeout. STDERR must
    # NOT go: the daemon's ordinary output goes to --log, so the only things
    # it ever writes to stderr are the failures that happen BEFORE the log is
    # open, and those are precisely the ones a caller cannot otherwise find.
    "$_kp_bin" --socket "$_kp_sock" --keys "$_kp_fifo" --ready "$_kp_ready" \
        --log "$_kp_log" >/dev/null &
    _kp_pid=$!
    _kp_wait=0
    while [ ! -e "$_kp_ready" ]; do
        # A daemon that has ALREADY EXITED is the common failure -- a socket
        # somebody else is serving, a path too long for a sockaddr_un, an
        # unwritable log -- and waiting ten seconds for a ready file that can
        # never arrive turns a one-line reason into a timeout. It said its
        # piece on stderr on the way out; noticing at once is what puts that
        # line next to this one.
        if ! kill -0 "$_kp_pid" 2>/dev/null; then
            echo "qemu_machine: nd-i2c-keypadd exited before it was listening" >&2
            echo "  (its reason is above, or in $_kp_log)." >&2
            return 1
        fi
        _kp_wait=$((_kp_wait + 1))
        if [ "$_kp_wait" -gt 200 ]; then
            echo "qemu_machine: nd-i2c-keypadd never started listening; see $_kp_log" >&2
            kill "$_kp_pid" 2>/dev/null || true
            return 1
        fi
        sleep 0.05
    done
    echo "$_kp_pid"
}
