NeoDCT's QEMU target. The emulator is a 32-bit Cortex-A7 with 64 MB, because
the phone is, and one ABI on both machines is what puts 32-bit time_t,
size_t, pointer and alignment bugs in front of a developer instead of in front
of the bench.

Run the emulation with:

  qemu-system-arm -M virt -cpu cortex-a7 -smp 1 -m 64 -nographic \
    -global virtio-mmio.force-legacy=false \
    -kernel output/images/zImage \
    -append "rootwait root=/dev/vda console=ttyAMA0 video=vfb:on" \
    -drive file=output/images/rootfs.ext4,if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0

Three of those are not decoration and each was measured:

  zImage, not Image     BR2_LINUX_KERNEL_IMAGE depends on BR2_aarch64, so on
                        arm the choice falls to ZIMAGE. The artefact renamed
                        itself when this target converted.
  force-legacy=false    QEMU's virtio-mmio bus does not offer VIRTIO_F_VERSION_1
                        by default, and virtinput_probe() returns -ENODEV
                        without it -- silently, at any loglevel. The guest then
                        comes up with no keyboard, which looks like a broken
                        input stack rather than a transport option. This did not
                        bite on aarch64 because that machine puts virtio on PCI.
  video=vfb:on          the module-parameter form does not work for a built-in
                        vfb; linux.config beside this file has the reason.

THIS FILE DELIBERATELY CARRIES NO "# <something>_defconfig" TAG, and that is
the one thing not to helpfully add. board/qemu/post-image.sh greps every board
readme for a line tagged with the building defconfig's basename and, when it
finds one, writes it out as $BINARIES_DIR/start-qemu.sh. Today the grep matches
nothing for neodct_qemu_defconfig, the script exits 0, and nothing is
generated. Adding the tag would start generating a second copy of
neodct/tools/run_qemu.sh's command line -- a copy with none of its options,
maintained by nobody, that drifts the first time a flag is added and then
disagrees with the real launcher about how to boot the image. run_qemu.sh is
the launcher; this is documentation.
