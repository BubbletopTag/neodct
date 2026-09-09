/* nd-vncd -- a VNC server for the NeoDCT panel.
 *
 * Serves /dev/fb0 -- the phone's framebuffer -- over RFB, so a developer or an
 * agent can watch the real device without a serial cable or a camera. It is
 * the display daemon's skeleton (neodct/src/displayd/neodctDisplay.c) with
 * libvncserver as the sink instead of the ST7789 over SPI: open fb0, read the
 * pixel format from the driver, mmap it read-only, diff frames into a dirty
 * rectangle, and push only that rectangle -- to VNC clients rather than to the
 * panel.
 *
 * fb0 on this phone is the kernel's vfb. nd-core draws into it and
 * neodct_displayd mirrors the changed rectangles to the panel, so fb0 is the
 * ground truth for what is on screen, byte for byte. A second read-only mapping
 * is safe: the display daemon maps it read-only too and neither writes.
 *
 * Geometry is the framebuffer's 240x175, NOT the physical panel's 240x240 --
 * the daemon's Y offset is a panel concern and we serve the framebuffer.
 *
 * View-only in v1. Key injection has to happen at the core's key source, not at
 * /dev/input; the seam for it is marked below. See docs/DEBUG_LAN.md and the
 * NDVNCD writeup.
 *
 * Standalone, like the display daemon: it links libvncserver but not libneodct,
 * and prints to stdout/stderr, which S42debuglan redirects to a log.
 */

/* _GNU_SOURCE is supplied by the build (VNCD_CFLAGS), like the display daemon. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/fb.h>
#include <arpa/inet.h>

/* Header-only use: the ND_KEY_* and path constants. nd-vncd deliberately does
 * not link libneodct (see the header), so nothing here calls a function from
 * either of these. */
#include "nd_keycodes.h"
#include "nd_paths.h"

#include <rfb/rfb.h>

/* The framebuffer geometry, matching neodct_displayd's FB_W/FB_H and nd_ui.h's
 * ND_UI_W/ND_UI_H. Refuse to serve anything else rather than guess. */
#define FB_W       240
#define FB_H       175
#define FB_DEVICE  "/dev/fb0"

/* RGBX intermediate buffer: one fixed format for libvncserver regardless of
 * what the driver hands us. 240 * 175 * 4 = 168 KB. */
#define OUT_BPP    4
#define OUT_STRIDE (FB_W * OUT_BPP)

/* --- framebuffer state (set by init_framebuffer) ------------------------- */
static int                       fb_fd   = -1;
static unsigned char            *fb_data = MAP_FAILED; /* read-only mmap      */
static unsigned char            *prev_fb = NULL;       /* last sent raw frame */
static size_t                    fb_size = 0;
static struct fb_var_screeninfo  vinfo;
static struct fb_fix_screeninfo  finfo;
static uint32_t                  fb_bytespp = 0;       /* 2 or 4             */
static int                       fb_swap_rb = 0;

/* --- the buffer libvncserver encodes from -------------------------------- */
static uint8_t *rgbx_buf = NULL;                       /* FB_W*FB_H*OUT_BPP  */

/* --- options ------------------------------------------------------------- */
static int         opt_swap_rb = 0;
static uint32_t    opt_fps      = 10;          /* the 10 Mbit/s link wants this */
static const char *opt_bind     = "127.0.0.1"; /* SAFE default; see main()     */
static int         opt_port     = 5900;
static const char *opt_desktop  = "NeoDCT";
static int         opt_require_devenv = 0;     /* the gate seam; see main()     */
static int         opt_view_only = 0;          /* refuse to forward keys        */
static const char *opt_devkey = ND_PATH_DEVKEY_SOCK;

/* --- run state ----------------------------------------------------------- */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Open fb0, read its real format, map it. Returns 0 on success.
 *
 * Lifted from neodct_displayd::init_framebuffer, minus the SPI-side force_mode:
 * nd-vncd is a reader and takes the framebuffer as it finds it. */
static int init_framebuffer(void)
{
    fb_fd = open(FB_DEVICE, O_RDONLY);
    if (fb_fd < 0) {
        fprintf(stderr, "nd-vncd: open %s failed: %s\n", FB_DEVICE, strerror(errno));
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "nd-vncd: framebuffer ioctl failed: %s\n", strerror(errno));
        return -1;
    }

    if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "nd-vncd: expected 16 or 32 bpp framebuffer (got %u)\n",
                vinfo.bits_per_pixel);
        return -1;
    }
    fb_bytespp = vinfo.bits_per_pixel / 8u;

    /* ============ WHICH END RED IS ON IS THE DRIVER'S TO SAY ============
     *
     * This is the detail that has already cost this project a release, and it
     * is why nd-vncd reads fb_var_screeninfo instead of assuming a byte order.
     *
     * At 32 bpp the kernel's vfb declares red.offset 0 -- pixels are laid out
     * R G B x. Almost everything else in the world (DRM's fbdev emulation,
     * every desktop) declares red.offset 16, i.e. B G R x. At 16 bpp vfb is
     * likewise blue-last: red 0, green 5, blue 11.
     *
     * NeoDCT once assumed B G R x in BOTH the UI and the display daemon. Being
     * wrong together they looked right -- and every program that read the
     * driver's actual declaration (mpv, NetSurf via libnsfb, the fb console)
     * came out with red and blue swapped. So: follow the struct. --swap-rb
     * inverts whatever was detected, for a driver that fills it in wrongly; it
     * is not the way the order is chosen. */
    fb_swap_rb = (vinfo.red.offset < vinfo.blue.offset) ? 1 : 0;
    if (opt_swap_rb)
        fb_swap_rb = !fb_swap_rb;

    fb_size = (size_t)finfo.line_length * vinfo.yres;   /* stride, never xres*bpp */
    fb_data = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb_data == MAP_FAILED) {
        fprintf(stderr, "nd-vncd: mmap framebuffer failed: %s\n", strerror(errno));
        return -1;
    }

    prev_fb = malloc(fb_size);                          /* freed in cleanup() */
    if (!prev_fb) {
        fprintf(stderr, "nd-vncd: out of memory (prev_fb, %zu bytes)\n", fb_size);
        return -1;
    }
    /* 0xA5 so the first diff differs everywhere and the first frame is full. */
    memset(prev_fb, 0xA5, fb_size);

    printf("nd-vncd: fb0 %ux%u, %u bpp, line_length %u\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);
    printf("nd-vncd: fb channels red@%u green@%u blue@%u -> %s%s\n",
           vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset,
           fb_swap_rb ? "red first" : "blue first",
           opt_swap_rb ? " (--swap-rb inverted it)" : "");

    if (vinfo.xres < FB_W || vinfo.yres < FB_H)
        fprintf(stderr, "nd-vncd: WARNING fb %ux%u smaller than %dx%d; clamping\n",
                vinfo.xres, vinfo.yres, FB_W, FB_H);
    return 0;
}

/* Convert one fb rectangle [x0,x1]x[y0,y1] (inclusive) into rgbx_buf at the
 * same coordinates, as R G B X. Mirrors neodct_displayd::convert_rect, but the
 * sink is an 8888 buffer rather than the panel's big-endian 565. */
static void convert_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    for (uint32_t y = y0; y <= y1; y++) {
        const unsigned char *src = fb_data + (size_t)y * finfo.line_length;
        uint8_t *dst = rgbx_buf + (size_t)y * OUT_STRIDE + (size_t)x0 * OUT_BPP;

        if (fb_bytespp == 4) {
            for (uint32_t x = x0; x <= x1; x++) {
                const unsigned char *p = src + (size_t)x * 4;
                uint8_t b = p[0], g = p[1], r = p[2];
                if (fb_swap_rb) { uint8_t t = r; r = b; b = t; }
                *dst++ = r; *dst++ = g; *dst++ = b; *dst++ = 0;
            }
        } else {
            for (uint32_t x = x0; x <= x1; x++) {
                uint16_t px = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
                if (fb_swap_rb)
                    px = (uint16_t)(((px & 0xF800u) >> 11) | (px & 0x07E0u) |
                                    ((px & 0x001Fu) << 11));
                uint8_t r5 = (uint8_t)((px >> 11) & 0x1Fu);
                uint8_t g6 = (uint8_t)((px >> 5)  & 0x3Fu);
                uint8_t b5 = (uint8_t)( px        & 0x1Fu);
                /* 5/6-bit -> 8-bit, replicating the high bits into the low. */
                *dst++ = (uint8_t)((r5 << 3) | (r5 >> 2));
                *dst++ = (uint8_t)((g6 << 2) | (g6 >> 4));
                *dst++ = (uint8_t)((b5 << 3) | (b5 >> 2));
                *dst++ = 0;
            }
        }
    }
}

/* Diff fb against prev_fb and, if anything changed, convert the dirty bounding
 * rect into rgbx_buf and tell libvncserver. Returns 1 if a rect was sent, 0 if
 * the frame was identical. The two-pass algorithm is neodct_displayd's, proven
 * on this exact framebuffer. */
static int render_dirty(rfbScreenInfoPtr screen)
{
    uint32_t copy_w = vinfo.xres < FB_W ? vinfo.xres : FB_W;
    uint32_t copy_h = vinfo.yres < FB_H ? vinfo.yres : FB_H;
    size_t   row_bytes = (size_t)copy_w * fb_bytespp;

    /* pass 1: dirty row band [ymin, ymax] */
    uint32_t ymin = copy_h, ymax = 0;
    for (uint32_t y = 0; y < copy_h; y++) {
        size_t off = (size_t)y * finfo.line_length;
        if (memcmp(fb_data + off, prev_fb + off, row_bytes) != 0) {
            if (y < ymin) ymin = y;
            ymax = y;
        }
    }
    if (ymin > ymax)
        return 0;                               /* identical frame */

    /* pass 2: dirty column range across the band */
    size_t bmin = row_bytes, bmax = 0;
    for (uint32_t y = ymin; y <= ymax; y++) {
        const unsigned char *a = fb_data + (size_t)y * finfo.line_length;
        const unsigned char *b = prev_fb + (size_t)y * finfo.line_length;
        if (memcmp(a, b, row_bytes) == 0)
            continue;
        size_t i = 0, j = row_bytes - 1;
        while (i < row_bytes && a[i] == b[i]) i++;
        while (j > i && a[j] == b[j]) j--;
        if (i < bmin) bmin = i;
        if (j > bmax) bmax = j;
    }
    uint32_t xmin = (uint32_t)(bmin / fb_bytespp);
    uint32_t xmax = (uint32_t)(bmax / fb_bytespp);

    /* remember what we're about to send */
    for (uint32_t y = ymin; y <= ymax; y++) {
        size_t off = (size_t)y * finfo.line_length;
        memcpy(prev_fb + off, fb_data + off, row_bytes);
    }

    convert_rect(xmin, ymin, xmax, ymax);

    /* libvncserver's rectangle is EXCLUSIVE on the far edge; the display
     * daemon's diff is inclusive. +1 on each far edge, or a one-pixel stale
     * column/row survives every review. */
    rfbMarkRectAsModified(screen, (int)xmin, (int)ymin,
                          (int)xmax + 1, (int)ymax + 1);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Keys, forwarded to the core's own key source
 * ------------------------------------------------------------------ *
 *
 * This is the seam the first version left unwired, now that the thing it was
 * waiting for exists. The reasoning it was left for has not changed and is
 * worth restating, because the obvious implementation is still wrong:
 *
 *   NOT uinput. Apps never read /dev/input -- the core reads the keypad and
 *   hands each app a pipe -- and nd_input's is_our_injector() refuses the
 *   core's own uinput device precisely so a core cannot read back what it
 *   wrote. A uinput device created here would reach nothing.
 *
 *   The devkey channel instead, which IS the core's key source: a datagram
 *   arriving there is merged into the same queue the i2c matrix and the evdev
 *   descriptor feed, so a key sent from a VNC client is indistinguishable
 *   from a key pressed on the phone. Held state, repeat and T9 all behave.
 *
 * The consequence worth stating plainly: with this wired, VNC is no longer a
 * view. It is control. That is the point -- a phone whose keypad expander has
 * lost its solder joint is otherwise undrivable -- but it means the VNC port
 * grants what telnet already grants, which is everything. Both sit behind the
 * same engineering-mode gate and the same bound address, so this adds reach
 * rather than privilege. --view-only declines it.
 *
 * RFB hands us press and release separately, which is exactly what the channel
 * wants: sending only presses would leave keys held, and held state is real
 * state that widgets read.
 */

static int devkey_fd = -1;

/* Connect lazily and re-connect on failure. nd-vncd is started by
 * S42debuglan and the socket is created by the UI, so which comes first is not
 * ours to decide -- a phone where the UI is still starting must not leave this
 * permanently keyless. */
static bool devkey_ready(void)
{
    struct sockaddr_un addr;

    if (devkey_fd >= 0)
        return true;
    if (opt_view_only)
        return false;
    if (strlen(opt_devkey) >= sizeof addr.sun_path)
        return false;

    devkey_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (devkey_fd < 0)
        return false;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof addr.sun_path, "%s", opt_devkey);
    if (connect(devkey_fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        (void)close(devkey_fd);
        devkey_fd = -1;
        return false;
    }
    return true;
}

/* X11 keysym -> NeoDCT keycode, or -1 for a key this phone does not have.
 *
 * Deliberately small. Every entry is a key somebody will actually press at a
 * VNC client, and the phone has sixteen -- mapping a full keyboard onto it
 * would invent keys the hardware cannot produce and make a flow that works
 * over VNC fail on the keypad. LEFT and RIGHT are here because the codes
 * exist and a dev keyboard can send them, not because the phone has them. */
static int32_t keysym_to_code(rfbKeySym k)
{
    switch (k) {
    case 0xFF52: return ND_KEY_UP;      /* XK_Up        */
    case 0xFF54: return ND_KEY_DOWN;    /* XK_Down      */
    case 0xFF51: return ND_KEY_LEFT;    /* XK_Left      */
    case 0xFF53: return ND_KEY_RIGHT;   /* XK_Right     */
    case 0xFF0D:                        /* XK_Return    */
    case 0xFF8D: return ND_KEY_NAVIKEY; /* XK_KP_Enter  */
    case 0xFF08:                        /* XK_BackSpace */
    case 0xFF1B: return ND_KEY_CLEAR;   /* XK_Escape    */
    case 0x0020: return ND_KEY_SPACE;
    case 0x002A: return ND_KEY_STAR;    /* '*' */
    case 0x0023: return ND_KEY_HASH;    /* '#' */
    case 0x002D: return ND_KEY_MINUS;   /* '-' */
    case 0x002E: return ND_KEY_DOT;     /* '.' */
    case 0x002C: return ND_KEY_COMMA;   /* ',' */
    case 0x0031: return ND_KEY_1;
    case 0x0032: return ND_KEY_2;
    case 0x0033: return ND_KEY_3;
    case 0x0034: return ND_KEY_4;
    case 0x0035: return ND_KEY_5;
    case 0x0036: return ND_KEY_6;
    case 0x0037: return ND_KEY_7;
    case 0x0038: return ND_KEY_8;
    case 0x0039: return ND_KEY_9;
    case 0x0030: return ND_KEY_0;
    /* 'm' and 'M' for the menu key, which the phone's keypad does not have
     * either -- the home screen's "Menu" label is NaviKey. Useful from a
     * keyboard, and marked the same way nd-key --list marks it. */
    case 0x006D:
    case 0x004D: return ND_KEY_MENU;
    default: return -1;
    }
}

static void on_key(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    char msg[32];
    int32_t code;
    int n;

    (void)cl;
    if (opt_view_only)
        return;
    code = keysym_to_code(key);
    if (code < 0)
        return;
    if (!devkey_ready())
        return;

    n = snprintf(msg, sizeof msg, "%d %d", (int)code, down ? 1 : 0);
    if (n < 0 || (size_t)n >= sizeof msg)
        return;
    if (send(devkey_fd, msg, (size_t)n, 0) != (ssize_t)n) {
        /* The UI restarted, or the socket went away. Drop the descriptor so
         * the next key reconnects rather than silently going nowhere. */
        (void)close(devkey_fd);
        devkey_fd = -1;
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--swap-rb"))  opt_swap_rb = 1;
        else if (!strcmp(argv[i], "--require-devenv")) opt_require_devenv = 1;
        else if (!strcmp(argv[i], "--view-only")) opt_view_only = 1;
        else if (!strcmp(argv[i], "--devkey") && i + 1 < argc) opt_devkey = argv[++i];
        else if (!strcmp(argv[i], "--fps")  && i + 1 < argc) opt_fps  = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) opt_bind = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) opt_port = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) opt_desktop = argv[++i];
        else {
            fprintf(stderr,
                "nd-vncd: serve /dev/fb0 over VNC (view-only)\n"
                "  --bind ADDR   IPv4 address to listen on (default 127.0.0.1)\n"
                "  --port N      RFB port (default 5900)\n"
                "  --fps N       frames/sec when a client is attached (default 10)\n"
                "  --swap-rb     invert the detected red/blue order\n"
                "  --name STR    VNC desktop name (default NeoDCT)\n"
                "  --view-only   watch without being able to press anything\n"
                "  --devkey PATH the key channel to forward presses to\n"
                "  --require-devenv  refuse to start without /etc/neodct-devenv\n");
            return 2;
        }
    }
    if (opt_fps < 1)  opt_fps = 1;
    if (opt_fps > 60) opt_fps = 60;

    /* ============ THE GATE SEAM ============
     *
     * A VNC server shows the owner's screen, so a release image must not run
     * one. The project's mechanism for "developer image only" is
     * /etc/neodct-devenv, placed only when built with NEODCT_DEVENV_IMAGE=1 and
     * living on the read-only rootfs so a running phone cannot create it
     * (neodct/scripts/post-build-devenv-marker.sh explains why the gate must be
     * something writable storage cannot set).
     *
     * For the proof-of-concept this is OFF by default: nd-vncd runs wherever
     * S42debuglan starts it, the same ungated posture as telnet and ftp on the
     * debug link, and the bound address below is what keeps it safe. When the
     * engineering-mode gate lands, flip the default (or pass --require-devenv
     * from the init script) and this is done -- the check already exists. */
    if (opt_require_devenv && access("/etc/neodct-devenv", F_OK) != 0) {
        fprintf(stderr, "nd-vncd: /etc/neodct-devenv absent and --require-devenv set; refusing\n");
        return 1;
    }

    if (init_framebuffer() != 0)
        return 1;

    rgbx_buf = calloc((size_t)FB_W * FB_H, OUT_BPP);    /* freed in cleanup() */
    if (!rgbx_buf) {
        fprintf(stderr, "nd-vncd: out of memory (rgbx_buf)\n");
        return 1;
    }

    rfbScreenInfoPtr screen = rfbGetScreen(&argc, argv, FB_W, FB_H,
                                           8 /* bits/sample */,
                                           3 /* samples/pixel */,
                                           OUT_BPP);
    if (!screen) {
        fprintf(stderr, "nd-vncd: rfbGetScreen failed\n");
        return 1;
    }

    screen->frameBuffer = (char *)rgbx_buf;
    screen->desktopName = opt_desktop;
    screen->alwaysShared = TRUE;

    /* Match serverFormat to the buffer we fill (R G B X, little-endian), rather
     * than trusting rfbGetScreen's default for a 4-bpp screen. */
    screen->serverFormat.trueColour = TRUE;
    screen->serverFormat.bitsPerPixel = 32;
    screen->serverFormat.depth = 24;
    screen->serverFormat.bigEndian = FALSE;
    screen->serverFormat.redShift   = 0;
    screen->serverFormat.greenShift = 8;
    screen->serverFormat.blueShift  = 16;
    screen->serverFormat.redMax   = 255;
    screen->serverFormat.greenMax = 255;
    screen->serverFormat.blueMax  = 255;

    /* ============ BIND TO ONE ADDRESS, NEVER THE MODEM ============
     *
     * The phone's modem interface is up at the same time and, on an IPv6-only
     * carrier, holds a globally routable address. So:
     *   - listenInterface pins the IPv4 socket to the debug address only.
     *   - ipv6port = 0 disables the IPv6 socket entirely. sockets.c only opens
     *     a TCP6 listener when ipv6port > 0, so this is the whole of it --
     *     there is no ::-bound socket to reach over the modem.
     * The default opt_bind is loopback, so even a hand-run with no --bind
     * cannot expose the screen to the network. */
    screen->port = opt_port;
    screen->autoPort = FALSE;
    screen->ipv6port = 0;
    screen->listenInterface = inet_addr(opt_bind);
    if (screen->listenInterface == INADDR_NONE) {
        fprintf(stderr, "nd-vncd: bad --bind address '%s'\n", opt_bind);
        return 1;
    }

    /* ============ KEYBOARD SEAM -- deliberately unwired (v1 is view-only) ====
     *
     * The obvious `screen->kbdAddEvent = on_key;` feeding a uinput device does
     * NOT work on this system, and the reason is structural: apps do not read
     * /dev/input. nd-core reads the i2c keypad matrix and hands each app a pipe
     * (NEODCT_KEYPAD_FD). nd_input.c:768 (is_our_injector) even refuses to adopt
     * the uinput device the core makes for the Browser, because on the phone
     * /dev/input is empty and that injector lands at event0 -- adopting it would
     * read every injected key straight back out.
     *
     * So key injection belongs at the core's key source: a dev-only channel (a
     * unix socket, gated on /etc/neodct-devenv) that nd_input polls alongside
     * the matrix. When that exists, wiring screen->kbdAddEvent to it is a dozen
     * lines and drives the home screen, the app selector and every app through
     * the one path.
     *
     * THAT CHANNEL NOW EXISTS, so the hook is wired: see on_key() above. There
     * is still no ptrAddEvent -- the phone has no pointer and inventing one
     * would let a flow work over VNC that cannot work on the hardware. */
    if (!opt_view_only)
        screen->kbdAddEvent = on_key;

    rfbInitServer(screen);
    if (!rfbIsActive(screen)) {
        fprintf(stderr, "nd-vncd: rfbInitServer failed to listen on %s:%d\n",
                opt_bind, opt_port);
        rfbScreenCleanup(screen);
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("nd-vncd: serving %dx%d on %s:%d at %u fps (%s)\n",
           FB_W, FB_H, opt_bind, opt_port, opt_fps,
           opt_view_only ? "view-only" : "keys forwarded to the phone");
    if (!opt_view_only)
        printf("nd-vncd: keys go to %s -- arrows, Enter=NaviKey, Backspace=C, "
               "0-9, * and #\n", opt_devkey);
    printf("nd-vncd: note -- fb0 keeps its last image while the panel backlight "
           "is asleep, so a 'frozen' screen here may just be a sleeping phone\n");
    fflush(stdout);

    /* The frame period when a client is attached, and a backed-off period for a
     * screen that has been static for a while (the phone's usual state). */
    long frame_us = (long)(1000000u / opt_fps);
    long idle_poll_us = 1000000;                 /* no client: ~zero CPU      */
    int  backoff_fps = 2;
    long backoff_us  = (long)(1000000 / backoff_fps);
    int  still_frames = 0;
    const int BACKOFF_AFTER = (int)opt_fps * 3;  /* ~3s of no change -> slow  */

    while (!g_stop) {
        int have_client = (screen->clientHead != NULL);

        /* No client: block in select until one connects or the timeout lapses.
         * A connection wakes rfbProcessEvents immediately, so this is idle. */
        long wait_us = have_client
                     ? (still_frames >= BACKOFF_AFTER ? backoff_us : frame_us)
                     : idle_poll_us;

        rfbProcessEvents(screen, wait_us);

        if (screen->clientHead != NULL) {
            if (render_dirty(screen))
                still_frames = 0;                /* change: back to full rate */
            else if (still_frames < BACKOFF_AFTER)
                still_frames++;
        } else {
            still_frames = 0;                    /* reset so next client is crisp */
        }
    }

    printf("nd-vncd: stopping\n");
    rfbShutdownServer(screen, TRUE);
    rfbScreenCleanup(screen);
    if (fb_data != MAP_FAILED) munmap(fb_data, fb_size);
    if (fb_fd >= 0) close(fb_fd);
    free(prev_fb);
    free(rgbx_buf);
    return 0;
}
