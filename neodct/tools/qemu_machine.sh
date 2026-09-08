# qemu_machine.sh -- the two halves of the machine the NeoDCT emulator boots:
# the device tree, and the kernel parameters that decide which devices exist.
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
