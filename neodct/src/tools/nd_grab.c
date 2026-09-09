/* nd-grab -- a PNG of the panel, or its digest.
 *
 *     nd-grab shot.png              write a PNG
 *     nd-grab -                     write the PNG to stdout
 *     nd-grab --digest              print the sha256 goldenframe.py compares
 *     nd-grab --dev /dev/fb1        somewhere other than /dev/fb0
 *     nd-grab --swap-rb shot.png    invert the detected channel order
 *
 * Reading a plain file instead of a framebuffer device needs the geometry,
 * because a file has no FBIOGET_VSCREENINFO to ask:
 *
 *     nd-grab --dev dump.raw --geom 240x175 --bpp 32 shot.png
 *
 * That is not only for tests. `cat /dev/fb0 > dump.raw` over the debug link
 * and converting it here is the cheapest way to get a frame off a phone whose
 * userspace is too broken to run anything larger.
 *
 * The pixel format is READ, never assumed -- see nd_grab.h for the release
 * that cost.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_grab.h"
#include "nd_image.h"
#include "nd_paths.h"
#include "nd_types.h"

static void usage(FILE *to)
{
    (void)fprintf(to,
                  "nd-grab -- a PNG of the panel, or its digest\n"
                  "\n"
                  "  nd-grab [options] [OUT.png | -]\n"
                  "\n"
                  "  --digest        print the sha256 instead of writing a PNG\n"
                  "  --dev PATH      framebuffer or raw dump (default %s)\n"
                  "  --swap-rb       invert the DETECTED red/blue order\n"
                  "  --geom WxH      geometry, for a raw dump with no ioctl\n"
                  "  --bpp N         16 or 32, for a raw dump\n"
                  "  --stride N      row bytes, for a raw dump (default W*bpp/8)\n",
                  ND_PATH_FB);
}

/* Fill fmt from the driver. Returns false (having complained) when the device
 * cannot describe itself, which is the normal case for a regular file. */
static bool fmt_from_device(int fd, nd_grab_fmt *fmt, bool invert, bool quiet)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        if (!quiet)
            (void)fprintf(stderr, "nd-grab: not a framebuffer (%s); give --geom and --bpp\n",
                          strerror(errno));
        return false;
    }

    if (!nd_grab_fmt_supported(vinfo.bits_per_pixel / 8u)) {
        (void)fprintf(stderr, "nd-grab: expected a 16 or 32 bpp framebuffer, got %u\n",
                      vinfo.bits_per_pixel);
        return false;
    }

    fmt->w = (int32_t)vinfo.xres;
    fmt->h = (int32_t)vinfo.yres;
    fmt->bytespp = vinfo.bits_per_pixel / 8u;
    fmt->stride = finfo.line_length;
    fmt->swap_rb = (vinfo.red.offset < vinfo.blue.offset);
    if (invert)
        fmt->swap_rb = !fmt->swap_rb;

    /* The line the whole diagnosis hangs off when somebody says "the colours
     * are wrong". stderr, so it never lands in a PNG on stdout. */
    (void)fprintf(stderr, "nd-grab: %dx%d %u bpp stride %zu, red@%u blue@%u -> %s%s\n", fmt->w,
                  fmt->h, vinfo.bits_per_pixel, fmt->stride, vinfo.red.offset, vinfo.blue.offset,
                  fmt->swap_rb ? "red first" : "blue first", invert ? " (--swap-rb inverted it)" : "");
    return true;
}

int main(int argc, char **argv)
{
    const char *dev = NULL;
    const char *out = NULL;
    nd_grab_fmt fmt;
    bool want_digest = false;
    bool invert = false;
    long opt_w = 0;
    long opt_h = 0;
    long opt_bpp = 0;
    long opt_stride = 0;
    int fd = -1;
    int rc = 1;
    int i;
    uint8_t *buf = NULL;
    size_t need;
    nd_image *img = NULL;
    char devpath[ND_PATH_MAX];

    memset(&fmt, 0, sizeof fmt);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--digest") == 0) {
            want_digest = true;
        } else if (strcmp(a, "--swap-rb") == 0) {
            invert = true;
        } else if (strcmp(a, "--dev") == 0 && i + 1 < argc) {
            dev = argv[++i];
        } else if (strcmp(a, "--geom") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ldx%ld", &opt_w, &opt_h) != 2 || opt_w <= 0 || opt_h <= 0) {
                (void)fprintf(stderr, "nd-grab: --geom wants WxH\n");
                return 2;
            }
        } else if (strcmp(a, "--bpp") == 0 && i + 1 < argc) {
            opt_bpp = strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--stride") == 0 && i + 1 < argc) {
            opt_stride = strtol(argv[++i], NULL, 10);
        } else if (a[0] == '-' && a[1] != '\0' && strcmp(a, "-") != 0) {
            (void)fprintf(stderr, "nd-grab: unknown option \"%s\"\n", a);
            usage(stderr);
            return 2;
        } else {
            out = a;
        }
    }

    if (out == NULL && !want_digest) {
        (void)fprintf(stderr, "nd-grab: give an output file, \"-\", or --digest\n");
        usage(stderr);
        return 2;
    }

    if (dev != NULL) {
        (void)nd_strlcpy(devpath, dev, sizeof devpath);
    } else if (nd_path_resolve(devpath, sizeof devpath, ND_PATH_FB) != ND_OK) {
        (void)fprintf(stderr, "nd-grab: cannot resolve %s\n", ND_PATH_FB);
        return 1;
    }

    fd = open(devpath, O_RDONLY);
    if (fd < 0) {
        (void)fprintf(stderr, "nd-grab: open %s: %s\n", devpath, strerror(errno));
        return 1;
    }

    /* Ask the driver first, always. The explicit flags are a fallback for
     * something that cannot answer, never a way to override a device that
     * can -- overriding a real framebuffer's own description is how the
     * red/blue bug gets reintroduced by hand. */
    if (!fmt_from_device(fd, &fmt, invert, opt_bpp != 0)) {
        if (opt_bpp == 0 || opt_w == 0) {
            (void)close(fd);
            return 1;
        }
        if (!nd_grab_fmt_supported((uint32_t)opt_bpp / 8u)) {
            (void)fprintf(stderr, "nd-grab: --bpp must be 16 or 32\n");
            (void)close(fd);
            return 2;
        }
        fmt.w = (int32_t)opt_w;
        fmt.h = (int32_t)opt_h;
        fmt.bytespp = (uint32_t)opt_bpp / 8u;
        fmt.stride = (opt_stride > 0) ? (size_t)opt_stride : (size_t)opt_w * fmt.bytespp;
        /* A raw dump carries no channel order, so the flag is the whole of it
         * here rather than an inversion of something detected. */
        fmt.swap_rb = invert;
    }

    need = fmt.stride * (size_t)fmt.h;
    buf = malloc(need); /* owned here; freed before every return below */
    if (buf == NULL) {
        (void)fprintf(stderr, "nd-grab: out of memory (%zu bytes)\n", need);
        (void)close(fd);
        return 1;
    }

    /* read() rather than mmap(): it works identically on a device and on a
     * regular file, and this reads the whole thing exactly once. */
    {
        size_t got = 0u;

        while (got < need) {
            ssize_t n = read(fd, buf + got, need - got);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                (void)fprintf(stderr, "nd-grab: read %s: %s\n", devpath, strerror(errno));
                goto done;
            }
            if (n == 0) {
                (void)fprintf(stderr, "nd-grab: %s ended after %zu of %zu bytes\n", devpath, got,
                              need);
                goto done;
            }
            got += (size_t)n;
        }
    }

    img = nd_grab_to_image(buf, &fmt);
    if (img == NULL) {
        (void)fprintf(stderr, "nd-grab: cannot convert the frame\n");
        goto done;
    }

    if (want_digest) {
        char hex[65];

        if (nd_capture_digest(img, hex, sizeof hex) != ND_OK) {
            (void)fprintf(stderr, "nd-grab: cannot digest the frame\n");
            goto done;
        }
        (void)printf("%s\n", hex);
    }

    if (out != NULL) {
        /* "-" is stdout. /dev/stdout keeps the one PNG writer in libneodct
         * rather than growing a second path that writes to a descriptor. */
        const char *path = (strcmp(out, "-") == 0) ? "/dev/stdout" : out;

        if (nd_image_save_png(img, path) != ND_OK) {
            (void)fprintf(stderr, "nd-grab: cannot write %s\n", path);
            goto done;
        }
    }
    rc = 0;

done:
    nd_image_free(img);
    free(buf);
    if (fd >= 0)
        (void)close(fd);
    return rc;
}
