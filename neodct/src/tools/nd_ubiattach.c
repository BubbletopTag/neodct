/* nd_ubiattach.c -- attach an MTD partition to UBI at a stated VID header
 * offset, because busybox cannot.
 *
 *     nd-ubiattach <mtd-num> <ubi-num> <vid-hdr-offset>
 *     exit 0   attached; the ubi number it got is printed
 *     exit 1   the ioctl or the open failed, with errno spelled out
 *     exit 2   the arguments are wrong
 *
 * ============ WHY THIS EXISTS, AND WHY THE NUMBER 2048 IS THE POINT ========
 *
 * The Pico Mini's flash is SPI NAND. drivers/mtd/nand/spi/ never sets
 * `mtd->subpage_sft`, so UBI's default VID header offset there is the whole
 * 2048-byte page -- which is exactly what neodct/tools/mknand.sh writes,
 * `ubinize -O 2048`, and why its LEB_SIZE is 0x1f000 = 126,976 (a 128 KiB
 * PEB less two 2048-byte headers).
 *
 * The emulator's chip is nandsim, which emulates a PARALLEL NAND with four
 * 512-byte ECC steps. nand_scan_tail() therefore sets `subpage_sft = 2`, UBI
 * computes 2048 >> 2 = 512, and attaching mknand.sh's own image without
 * saying otherwise fails outright. Measured, on this kernel, on a real image:
 *
 *     ubi0 error: validate_ec_hdr: bad VID header offset 2048, expected 512
 *     ubi0 error: ubi_attach_mtd_dev: failed to attach mtd5, error -22
 *
 * There is no auto-detect: UBI takes the offset it is given or its own
 * default. So the offset has to be stated at attach time, and the emulator is
 * only faithful here because it is TOLD to be. Attaching without it does not
 * fail loudly -- with a chip somebody formatted in place it succeeds and
 * gives LEB 129,024, a device that looks entirely healthy while computing
 * arithmetic the phone never does. That is the mtdram failure this branch
 * already removed once, wearing new clothes.
 *
 * ============ WHY IT IS NOT A SHELL LINE ============
 *
 * busybox 1.37.0 DOCUMENTS the flag and cannot parse it. miscutils/ubi_tools.c
 * puts `O:+` only in the do_mkvol getopt string --
 *
 *     opts = getopt32(argv, "^" "md:+n:+N:s:a:+t:O:+" "\0" "-1", ...);   mkvol
 *     opts = getopt32(argv, "^" "m:+d:+n:+N:s:a:+t:" "\0" "-1",  ...);   attach
 *
 * -- while `attach_req.vid_hdr_offset = vid_hdr_offset;` is reached only in
 * the do_attach branch, where nothing has ever assigned it. So
 * `ubiattach -O 2048` dies with "invalid option -- 'O'" and the ioctl is
 * issued with an offset of 0 whatever the usage text says. Reading the option
 * out of `ubiattach --help` and believing it is the trap this file removes.
 *
 * The same argument nd_inventory.c makes in its own header: C rather than
 * shell for an ioctl, because shell cannot ask.
 *
 * ============ AND WHY IT IS NOT IN THE IMAGE ============
 *
 * `make install` does not install it. The phone attaches UBI from the kernel
 * command line -- `ubi.mtd=` in the U-Boot environment, consumed at
 * late_initcall -- and has no ubiattach, no ubinize and nothing to attach a
 * partition it did not boot from. A NAND attach tool on a phone whose NAND is
 * already attached is a hazard, not a feature. It is built here because this
 * is where the target toolchain and the coding standards are, and it is
 * packed into the QEMU-only flasher overlay by neodct/tools/run_qemu.sh.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* From include/uapi/mtd/ubi-user.h, copied rather than included: this file is
 * cross-compiled on its own by run_qemu.sh against whatever libc headers the
 * host has, and <mtd/ubi-user.h> is a kernel-headers package that a machine
 * with a working cross compiler still need not carry. The struct is uapi and
 * therefore frozen; the padding is part of it and must stay. */
#define ND_UBI_IOC_MAGIC 'o'
struct nd_ubi_attach_req {
    int32_t ubi_num;
    int32_t mtd_num;
    int32_t vid_hdr_offset;
    int16_t max_beb_per1024;
    int8_t  padding[10];
};
#define ND_UBI_IOCATT _IOW(ND_UBI_IOC_MAGIC, 0x40, struct nd_ubi_attach_req)

#define ND_UBI_CTRL "/dev/ubi_ctrl"

/* strtol with the whole string consumed, because atoi() cannot fail and a
 * mistyped offset that silently becomes 0 is the exact bug above. */
static int parse_num(const char *s, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 0x7fffffffL)
        return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    struct nd_ubi_attach_req req;
    long mtd_num, ubi_num, vid_off;
    int fd, rc;

    if (argc != 4) {
        fprintf(stderr, "usage: nd-ubiattach <mtd-num> <ubi-num> <vid-hdr-offset>\n");
        fprintf(stderr, "  the Pico Mini's flash is ubinize'd with -O 2048; see this file's header\n");
        return 2;
    }
    if (parse_num(argv[1], &mtd_num) != 0 || parse_num(argv[2], &ubi_num) != 0
        || parse_num(argv[3], &vid_off) != 0) {
        fprintf(stderr, "nd-ubiattach: all three arguments must be non-negative integers\n");
        return 2;
    }

    memset(&req, 0, sizeof req);
    req.mtd_num = (int32_t)mtd_num;
    req.ubi_num = (int32_t)ubi_num;
    req.vid_hdr_offset = (int32_t)vid_off;

    fd = open(ND_UBI_CTRL, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "nd-ubiattach: open %s: %s\n", ND_UBI_CTRL, strerror(errno));
        return 1;
    }

    /* UBI_IOCATT returns 0 and PUTS THE ASSIGNED DEVICE NUMBER BACK into the
     * first field of the caller's struct -- drivers/mtd/ubi/cdev.c does
     * `err = put_user(err, (__user int32_t *)argp)` on the success path, and
     * argp is `struct ubi_attach_req *`, whose first member is ubi_num. So
     * the number to report is req.ubi_num after the call and not the return
     * value: printing the return value says "attached as ubi0" for an attach
     * that dmesg records as ubi1, which is exactly the sort of line somebody
     * later builds a cmdline on. Found by booting it. */
    rc = ioctl(fd, ND_UBI_IOCATT, &req);
    if (rc < 0) {
        fprintf(stderr, "nd-ubiattach: UBI_IOCATT mtd%ld: %s\n", mtd_num, strerror(errno));
        (void)close(fd);
        return 1;
    }
    (void)close(fd);

    printf("nd-ubiattach: mtd%ld attached as ubi%d, VID header offset %ld\n",
           mtd_num, (int)req.ubi_num, vid_off);
    return 0;
}
