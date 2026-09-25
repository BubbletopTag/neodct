/* nd-watchd -- the panel, the speaker and the keypad over one TCP socket.
 *
 * `ndlink watch` used to point a stock VNC client at nd-vncd. That is still
 * there and still the answer for a client that is not ndlink, but RFB has no
 * audio, and a phone you cannot hear is half a phone: ringtones, the music
 * player, a call's far end, a game's effects. So this is the second, narrower
 * server, spoken to only by neodct/tools/ndlink-watch:
 *
 *   - the framebuffer, diffed at --fps (30 by default) and sent as the dirty
 *     rectangle in RGB565 -- the panel's own depth, so the viewer shows what
 *     the glass shows and a full-screen change costs 84 KB, ~20 Mbit/s at
 *     30 fps, a fifth of the 100 Mbit debug link;
 *   - whatever is being played, relayed from the ALSA tap (see --tap below);
 *   - keys back from the viewer into the devkey channel, exactly as nd-vncd
 *     forwards them, and for the reasons its on_key() gives.
 *
 * ============ WHERE THE AUDIO COMES FROM ============
 *
 * There is no loopback card in this kernel and kernel changes cannot ship over
 * the air, so the audio is taken in userspace, on the way to the card. When the
 * debug link is up, S42debuglan asks S17audio to put ALSA's `file` plugin in
 * the playback half of "default" (plug -> file -> hw), with its output piped
 * to `nd-watchd --tap`. alsa-lib popen()s that once per PCM open, writes a WAV
 * header, then every frame the app plays -- AFTER plug's conversion, so it is
 * always the card's own format (S16_LE stereo on the C-Media) whatever the app
 * asked for. The tap cuts that into datagrams and throws them at this daemon's
 * abstract socket. It never blocks and never exits before EOF; the next
 * section says why that is the whole design.
 *
 * pcm_file hands the bytes over one slave buffer behind the application, which
 * is roughly when the card plays them, so the tap is in step with the speaker
 * rather than ahead of it.
 *
 * ============ THE TAP MUST NEVER HURT PLAYBACK ============
 *
 * The tap sits in the write path of every sound this phone makes. If it
 * blocks, playback stalls; if it exits early, alsa-lib's next write() gets
 * EPIPE and the app sees a failed PCM -- a ringtone that does not ring because
 * a debugging tool was installed. So: the tap reads stdin until EOF no matter
 * what, sends with MSG_DONTWAIT, ignores every send error (no daemon, daemon
 * busy, queue full all read as "drop this"), and ignores SIGPIPE. With no
 * viewer connected the daemon still drains its socket, so the tap's cost is a
 * copy and a failed or discarded sendto.
 *
 * musl's popen() makes the parent's end O_CLOEXEC, so a child the player
 * forks and execs does not keep the pipe open behind its back -- which would
 * leave pclose() waiting for an EOF that never comes. nd_proc also closes
 * every descriptor above 2 in anything the core spawns.
 *
 * ============ WHAT IT DOES NOT HAVE ============
 *
 * Capture. The tap is on playback only; the microphone is not streamed. It is
 * a speaker you are listening to, not a bug in the room. It does carry a
 * call's far end when one is playing through "default", which is the point of
 * listening to a phone -- and is also why it binds the debug address only and
 * starts only in engineering mode, like the telnet shell beside it that
 * already grants all of this and more.
 *
 * Standalone, like nd-vncd and the display daemon: libc only, not libneodct.
 */

/* _GNU_SOURCE is supplied by the build (WATCHD_CFLAGS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/fb.h>
#include <sound/asound.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* Header-only, as in nd-vncd: the ND_KEY_* and path constants. */
#include "nd_keycodes.h"
#include "nd_paths.h"

#define FB_W 240
#define FB_H 175

/* ============ THE WIRE ============
 *
 * Every message, both directions: an 8-byte header, then `len` bytes.
 *
 *     u8 type   u8 0   u16 0   u32 len          (all little-endian)
 *
 * server -> viewer
 *   'H' hello   "NDW1" u16 w  u16 h  u8 pixfmt(1=RGB565LE)  u8 keys  u16 fps
 *   'V' video   u16 x  u16 y  u16 w  u16 h  then w*h RGB565LE pixels
 *   'A' audio   u32 rate  u16 channels  u16 bits  then interleaved LE PCM
 *   'k' keyecho i32 NeoDCT keycode  u8 down  u8[3] 0   -- ANY key the phone
 *               took in: its own keypad, a viewer's, nd-key's
 *   'S' stats   u16 cpu permille (0xFFFF: not measured yet)  u16 0
 *               u32 MemTotal  u32 MemAvailable  u32 SwapTotal  u32 SwapFree
 *               (kB), every STATS_EVERY_US while a viewer is attached
 * viewer -> server
 *   'K' key     i32 NeoDCT keycode  u8 down  u8[3] 0
 *   'O' options u8 flags: bit 0 = mute the phone's speaker while I watch
 *
 * A viewer skips a type it does not know; so does this, up to MAX_IN_MSG.
 * The protocol is ours at both ends and versioned by the hello's magic. */
#define MSG_HDR    8
#define MAX_IN_MSG 256

/* The tap's datagram: "NDA1" u32 rate u16 channels u16 bits, then PCM. */
#define TAP_HDR   12
#define TAP_CHUNK 8192

/* How much may queue for a slow viewer before we stop adding to it. Video is
 * cut first and loses nothing by it -- the diff simply grows to cover the
 * frames that were skipped. Audio is cut later and does lose samples, which is
 * the right way round: a late picture is still the right picture, and late
 * audio is only more latency. */
#define VIDEO_HIGHWATER (128u * 1024u)
#define AUDIO_HIGHWATER (512u * 1024u)
#define OUT_CAP         (1024u * 1024u)

/* --- options ------------------------------------------------------------- */
static const char *opt_bind      = "127.0.0.1"; /* safe default, as nd-vncd */
static int         opt_port      = 5901;
static unsigned    opt_fps       = 30;
static int         opt_view_only = 0;
static int         opt_swap_rb   = 0;
static const char *opt_devkey    = ND_PATH_DEVKEY_SOCK;
static const char *opt_audio     = "nd-watchd";  /* abstract socket name    */
static const char *opt_fb        = "/dev/fb0";
static const char *opt_mute_card = "usb";        /* "usb", a number, "none" */
static const char *opt_mute_state = "/run/nd-watchd.muted";
static const char *opt_keyecho   = ND_PATH_KEYECHO_SOCK;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ *
 * little-endian packing
 * ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The abstract address both halves use. Abstract rather than a path because
 * the tap runs inside whatever plays audio -- including mpv and NetSurf as
 * ndusr_ut in a private mount namespace, where a socket file under /run may
 * not be visible and would need a mode that lets them write it. Abstract
 * sockets live in the network namespace, which nothing here unshares. */
static socklen_t audio_addr(struct sockaddr_un *a)
{
    size_t n = strlen(opt_audio);
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (n > sizeof a->sun_path - 1)
        n = sizeof a->sun_path - 1;
    memcpy(a->sun_path + 1, opt_audio, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

/* ------------------------------------------------------------------ *
 * --tap: the ALSA file plugin's pipe reader
 * ------------------------------------------------------------------ */

static int read_full(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static void drain_stdin(void)
{
    uint8_t junk[4096];
    for (;;) {
        ssize_t r = read(0, junk, sizeof junk);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
    }
}

static int run_tap(void)
{
    uint8_t wav[44];
    static uint8_t buf[TAP_HDR + TAP_CHUNK];
    struct sockaddr_un addr;
    socklen_t alen;
    uint32_t rate, channels, bits, frame;
    size_t have = 0;
    int fd;

    signal(SIGPIPE, SIG_IGN);

    /* pcm_file writes a fixed 44-byte header: RIFF, WAVE, a 16-byte fmt
     * chunk, then data. Anything else is not something we can describe to the
     * viewer, so it is swallowed -- never refused, see the header. */
    if (read_full(0, wav, sizeof wav) != 0)
        return 0;
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVEfmt ", 8) != 0 ||
        get16(wav + 20) != 1 || memcmp(wav + 36, "data", 4) != 0) {
        drain_stdin();
        return 0;
    }
    channels = get16(wav + 22);
    rate     = get32(wav + 24);
    bits     = get16(wav + 34);
    frame    = channels * (bits / 8u);
    if (channels == 0 || channels > 8 || (bits != 8 && bits != 16 &&
        bits != 24 && bits != 32) || rate == 0 || frame == 0) {
        drain_stdin();
        return 0;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        drain_stdin();
        return 0;
    }
    alen = audio_addr(&addr);

    memcpy(buf, "NDA1", 4);
    put32(buf + 4, rate);
    put16(buf + 8, channels);
    put16(buf + 10, bits);

    /* Whole frames per datagram, the remainder carried to the next read, so
     * the daemon never has to reassemble a sample split across two. */
    const size_t cap = TAP_CHUNK - (TAP_CHUNK % frame);
    for (;;) {
        ssize_t r = read(0, buf + TAP_HDR + have, cap - have);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        have += (size_t)r;
        size_t whole = have - (have % frame);
        if (whole == 0)
            continue;
        (void)sendto(fd, buf, TAP_HDR + whole, MSG_DONTWAIT | MSG_NOSIGNAL,
                     (const struct sockaddr *)&addr, alen);
        memmove(buf + TAP_HDR, buf + TAP_HDR + whole, have - whole);
        have -= whole;
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ *
 * The framebuffer -- nd-vncd's reader, with RGB565 as the sink
 * ------------------------------------------------------------------ */

static int                      fb_fd = -1;
static unsigned char           *fb_data = MAP_FAILED;
static unsigned char           *prev_fb = NULL;
static size_t                   fb_size = 0;
static uint32_t                 fb_line = 0;
static uint32_t                 fb_bytespp = 0;
static uint32_t                 fb_w = 0, fb_h = 0;
static int                      fb_swap_rb = 0;

static int init_framebuffer(void)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    struct stat st;

    fb_fd = open(opt_fb, O_RDONLY | O_CLOEXEC);
    if (fb_fd < 0) {
        fprintf(stderr, "nd-watchd: open %s: %s\n", opt_fb, strerror(errno));
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
        if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32) {
            fprintf(stderr, "nd-watchd: %u bpp framebuffer not supported\n",
                    vinfo.bits_per_pixel);
            return -1;
        }
        fb_bytespp = vinfo.bits_per_pixel / 8u;
        fb_line = finfo.line_length;
        fb_w = vinfo.xres < FB_W ? vinfo.xres : FB_W;
        fb_h = vinfo.yres < FB_H ? vinfo.yres : FB_H;
        /* The driver says which end red is on; nd-vncd's init_framebuffer()
         * has the story of the release it cost to assume. */
        fb_swap_rb = vinfo.red.offset < vinfo.blue.offset;
        fb_size = (size_t)fb_line * vinfo.yres;
    } else if (fstat(fb_fd, &st) == 0 && S_ISREG(st.st_mode) &&
               (size_t)st.st_size >= (size_t)FB_W * FB_H * 4u) {
        /* A plain file of vfb's shape: 240x175, 32 bpp, red first. Only the
         * host tests use this -- they have no fb0 and should not need one. */
        fb_bytespp = 4;
        fb_line = FB_W * 4u;
        fb_w = FB_W;
        fb_h = FB_H;
        fb_swap_rb = 1;
        fb_size = (size_t)fb_line * FB_H;
    } else {
        fprintf(stderr, "nd-watchd: %s is not a framebuffer\n", opt_fb);
        return -1;
    }
    if (opt_swap_rb)
        fb_swap_rb = !fb_swap_rb;

    fb_data = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb_data == MAP_FAILED) {
        fprintf(stderr, "nd-watchd: mmap %s: %s\n", opt_fb, strerror(errno));
        return -1;
    }
    prev_fb = malloc(fb_size);
    if (!prev_fb)
        return -1;
    memset(prev_fb, 0xA5, fb_size);
    return 0;
}

/* One row of fb pixels [x0, x1] into dst as RGB565LE. The format branches
 * are taken once per row rather than once per pixel: at 30 fps a whole-screen
 * change is 1.26 M pixels a second, and on the Cortex-A7 the per-pixel
 * version cost the daemon ~14% of the CPU while nd-core's own animation took
 * 50. The 32-bpp case reads whole words -- the fb is 4-byte aligned. */
static void convert_row(const unsigned char *src, uint8_t *dst, uint32_t x0, uint32_t x1)
{
    if (fb_bytespp == 4) {
        const uint32_t rs = fb_swap_rb ? 0 : 16, bs = fb_swap_rb ? 16 : 0;
        for (uint32_t x = x0; x <= x1; x++) {
            uint32_t w;
            memcpy(&w, src + (size_t)x * 4u, 4);
            uint32_t v = (((w >> rs) & 0xF8u) << 8) | (((w >> 8) & 0xFCu) << 3) |
                         (((w >> bs) & 0xFFu) >> 3);
            *dst++ = (uint8_t)v;
            *dst++ = (uint8_t)(v >> 8);
        }
    } else if (!fb_swap_rb) {
        memcpy(dst, src + (size_t)x0 * 2u, (size_t)(x1 - x0 + 1) * 2u);
    } else {
        for (uint32_t x = x0; x <= x1; x++) {
            uint32_t px = (uint32_t)src[x * 2] | ((uint32_t)src[x * 2 + 1] << 8);
            px = ((px & 0xF800u) >> 11) | (px & 0x07E0u) | ((px & 0x001Fu) << 11);
            *dst++ = (uint8_t)px;
            *dst++ = (uint8_t)(px >> 8);
        }
    }
}

/* ------------------------------------------------------------------ *
 * The viewer
 * ------------------------------------------------------------------ */

static int      cl_fd = -1;
static uint8_t *out_buf = NULL;         /* OUT_CAP bytes                     */
static size_t   out_len = 0;
static uint8_t  in_buf[MSG_HDR + MAX_IN_MSG];
static size_t   in_len = 0;
static int      devkey_fd = -1;
static int      echo_fd = -1;
static int64_t  stats_next = 0;

static int64_t now_us(void);

/* Keys the viewer holds down right now. A viewer that vanishes mid-press --
 * window closed, cable pulled -- would otherwise leave the key held on the
 * phone, and held state is real state that widgets read. */
static bool     held[256];

static void *out_reserve(size_t n)
{
    if (out_len + n > OUT_CAP)
        return NULL;
    void *p = out_buf + out_len;
    out_len += n;
    return p;
}

static void put_hdr(uint8_t *p, char type, uint32_t len)
{
    p[0] = (uint8_t)type; p[1] = 0; p[2] = 0; p[3] = 0;
    put32(p + 4, len);
}

/* Same connect-lazily-and-retry as nd-vncd: the UI creates the socket, and a
 * phone whose UI is restarting must not leave the viewer keyless for good. */
static bool devkey_ready(void)
{
    struct sockaddr_un a;

    if (devkey_fd >= 0)
        return true;
    if (opt_view_only || strlen(opt_devkey) >= sizeof a.sun_path)
        return false;
    devkey_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (devkey_fd < 0)
        return false;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, opt_devkey, strlen(opt_devkey));
    if (connect(devkey_fd, (const struct sockaddr *)&a, sizeof a) != 0) {
        close(devkey_fd);
        devkey_fd = -1;
        return false;
    }
    return true;
}

/* The keys this phone has, plus the few a dev keyboard reaches that nd-key
 * also knows (MENU, space and the punctuation T9 accepts). NOT left and right:
 * the phone has no such keys, and a flow that needs them over the viewer
 * would be one nobody can repeat on the keypad. Anything else is dropped here
 * rather than trusted from the wire. */
static bool key_allowed(int32_t c)
{
    switch (c) {
    case ND_KEY_UP: case ND_KEY_DOWN:
    case ND_KEY_NAVIKEY: case ND_KEY_CLEAR: case ND_KEY_SPACE:
    case ND_KEY_STAR: case ND_KEY_HASH: case ND_KEY_MINUS: case ND_KEY_DOT:
    case ND_KEY_COMMA: case ND_KEY_MENU:
        return true;
    default:
        return nd_key_is_digit(c);
    }
}

static void send_key(int32_t code, bool down)
{
    char msg[32];
    int n;

    if (!devkey_ready())
        return;
    n = snprintf(msg, sizeof msg, "%d %d", (int)code, down ? 1 : 0);
    if (n <= 0 || (size_t)n >= sizeof msg)
        return;
    if (send(devkey_fd, msg, (size_t)n, MSG_NOSIGNAL) != (ssize_t)n) {
        close(devkey_fd);
        devkey_fd = -1;
        return;
    }
    held[code & 0xFF] = down;
}

/* ------------------------------------------------------------------ *
 * Muting the phone while it is watched
 * ------------------------------------------------------------------ *
 *
 * Somebody listening through the viewer does not want the phone playing the
 * same sound a few hundred milliseconds earlier on the desk beside them. So a
 * viewer can ask ('O', bit 0) and ndlink-watch does unless given --no-mute.
 *
 * The playback switches are turned off, NOT the stream: the tap sits in front
 * of the card and the card still has to run for the app's writes to be paced,
 * so the app, the tap and the viewer carry on exactly as before and only the
 * speaker is silent. Every "... Playback Switch" on the USB card is saved and
 * cleared through the kernel's control interface -- the same thing amixer does
 * -- and put back as it was, not switched on, when the viewer goes.
 *
 * What was saved is also written to /run, because the one outcome this must
 * never have is a phone left silent: a daemon killed while muted restores it
 * when it next starts, and /run is gone after a reboot, as is the mute.
 *
 * A view-only daemon does not mute. View-only is the promise not to disturb
 * the phone, and silencing its owner's ringtone disturbs it. */
#define MUTE_MAX 16

struct saved_switch {
    unsigned numid;
    unsigned count;
    long     v[8];
};
static struct saved_switch mute_saved[MUTE_MAX];
static int  mute_n = 0;
static bool muted = false;

/* The card S17audio routes "default" to: the first with a usbid. */
static int mute_card(void)
{
    char path[64];

    if (!strcmp(opt_mute_card, "none"))
        return -1;
    if (strcmp(opt_mute_card, "usb") != 0)
        return atoi(opt_mute_card);
    for (int c = 0; c < 8; c++) {
        (void)snprintf(path, sizeof path, "/proc/asound/card%d/usbid", c);
        if (access(path, F_OK) == 0)
            return c;
    }
    return -1;
}

static int ctl_open(void)
{
    char path[32];
    int card = mute_card();

    if (card < 0)
        return -1;
    (void)snprintf(path, sizeof path, "/dev/snd/controlC%d", card);
    return open(path, O_RDWR | O_CLOEXEC);
}

static void ctl_write(int fd, const struct saved_switch *sw, bool restore)
{
    struct snd_ctl_elem_value val;

    memset(&val, 0, sizeof val);
    val.id.numid = sw->numid;
    for (unsigned i = 0; i < sw->count; i++)
        val.value.integer.value[i] = restore ? sw->v[i] : 0;
    (void)ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val);
}

static void save_mute_state(void)
{
    FILE *f = fopen(opt_mute_state, "w");
    if (!f)
        return;
    for (int i = 0; i < mute_n; i++) {
        fprintf(f, "%u %u", mute_saved[i].numid, mute_saved[i].count);
        for (unsigned j = 0; j < mute_saved[i].count; j++)
            fprintf(f, " %ld", mute_saved[i].v[j]);
        fputc('\n', f);
    }
    fclose(f);
}

static void phone_mute(bool on)
{
    int fd;

    if (on == muted)
        return;
    if (!on) {
        fd = ctl_open();
        if (fd >= 0) {
            for (int i = 0; i < mute_n; i++)
                ctl_write(fd, &mute_saved[i], true);
            close(fd);
        }
        mute_n = 0;
        muted = false;
        (void)unlink(opt_mute_state);
        printf("nd-watchd: phone speaker restored\n");
        fflush(stdout);
        return;
    }

    fd = ctl_open();
    if (fd < 0) {
        printf("nd-watchd: cannot mute the phone: no USB sound card\n");
        fflush(stdout);
        return;
    }
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof list);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0 || list.count == 0) {
        close(fd);
        return;
    }
    struct snd_ctl_elem_id *ids = calloc(list.count, sizeof *ids);
    if (!ids) {
        close(fd);
        return;
    }
    list.space = list.count;
    list.pids = ids;
    mute_n = 0;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
        for (unsigned i = 0; i < list.used && mute_n < MUTE_MAX; i++) {
            struct snd_ctl_elem_info info;
            struct snd_ctl_elem_value val;
            const char *name = (const char *)ids[i].name;
            size_t nl = strlen(name), sl = strlen(" Playback Switch");

            if (nl < sl || strcmp(name + nl - sl, " Playback Switch") != 0)
                continue;
            memset(&info, 0, sizeof info);
            info.id = ids[i];
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) != 0 ||
                info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN || info.count > 8)
                continue;
            memset(&val, 0, sizeof val);
            val.id = ids[i];
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) != 0)
                continue;
            struct saved_switch *sw = &mute_saved[mute_n++];
            sw->numid = ids[i].numid;
            sw->count = info.count;
            for (unsigned j = 0; j < info.count; j++)
                sw->v[j] = val.value.integer.value[j];
        }
    }
    free(ids);
    /* Saved to /run BEFORE anything is switched off, so there is no moment
     * at which the phone is silent and nothing records how to undo it. */
    save_mute_state();
    for (int i = 0; i < mute_n; i++)
        ctl_write(fd, &mute_saved[i], false);
    close(fd);
    muted = true;
    printf("nd-watchd: phone speaker muted (%d switches) while watched\n", mute_n);
    fflush(stdout);
}

/* A previous nd-watchd died while muted: put the speaker back. */
static void restore_stale_mute(void)
{
    FILE *f = fopen(opt_mute_state, "r");
    if (!f)
        return;
    mute_n = 0;
    while (mute_n < MUTE_MAX) {
        struct saved_switch *sw = &mute_saved[mute_n];
        if (fscanf(f, "%u %u", &sw->numid, &sw->count) != 2 || sw->count > 8)
            break;
        unsigned j = 0;
        while (j < sw->count && fscanf(f, "%ld", &sw->v[j]) == 1)
            j++;
        if (j != sw->count)
            break;
        mute_n++;
    }
    fclose(f);
    muted = true;
    phone_mute(false);
}

static void drop_client(const char *why)
{
    if (cl_fd < 0)
        return;
    printf("nd-watchd: viewer gone (%s)\n", why);
    fflush(stdout);
    close(cl_fd);
    cl_fd = -1;
    out_len = 0;
    in_len = 0;
    for (int c = 0; c < 256; c++)
        if (held[c])
            send_key(c, false);
    phone_mute(false);
}

#define HELLO_LEN (MSG_HDR + 12)

static void fill_hello(uint8_t *p)
{
    put_hdr(p, 'H', 12);
    memcpy(p + 8, "NDW1", 4);
    put16(p + 12, fb_w);
    put16(p + 14, fb_h);
    p[16] = 1;
    p[17] = opt_view_only ? 0 : 1;
    put16(p + 18, opt_fps);
}

/* ------------------------------------------------------------------ *
 * The key echo: what the phone's own keypad is doing
 * ------------------------------------------------------------------ *
 *
 * nd-core sends every key it queues to ND_PATH_KEYECHO_SOCK (nd_input.c's
 * key_echo() has the gate and the why). This end binds it, as root, inside a
 * directory ndusr owns -- so it is done carefully:
 *
 *   - unlink first, which removes whatever is there without following it;
 *   - bind under umask 0177, so the socket is 0600 from the moment it
 *     exists rather than after a chmod that would follow a swapped-in link;
 *   - lchown to the directory's owner, never chown: if ndusr raced a symlink
 *     into the name, lchown changes the link and nothing it points at.
 *
 * Bound when a viewer connects, not at start: S42debuglan runs before
 * run_neodct.sh has made the directory. Re-bound on every connect, so a
 * restarted UI or a vanished file never leaves the window blind for long. */
static void echo_bind(void)
{
    struct sockaddr_un a;
    struct stat dir;
    char dpath[sizeof a.sun_path];
    const char *slash = strrchr(opt_keyecho, '/');

    if (echo_fd >= 0) {
        close(echo_fd);
        echo_fd = -1;
    }
    if (!slash || strlen(opt_keyecho) >= sizeof a.sun_path)
        return;
    memcpy(dpath, opt_keyecho, (size_t)(slash - opt_keyecho));
    dpath[slash - opt_keyecho] = '\0';
    if (lstat(dpath, &dir) != 0 || !S_ISDIR(dir.st_mode))
        return;                                   /* no UI yet; next viewer */

    (void)unlink(opt_keyecho);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, opt_keyecho, strlen(opt_keyecho));
    mode_t old = umask(0177);
    int rc = bind(fd, (const struct sockaddr *)&a, sizeof a);
    umask(old);
    if (rc != 0) {
        close(fd);
        return;
    }
    if (geteuid() == 0)
        (void)lchown(opt_keyecho, dir.st_uid, dir.st_gid);
    echo_fd = fd;
}

static void read_echo(void)
{
    char msg[32];
    for (;;) {
        ssize_t n = recv(echo_fd, msg, sizeof msg - 1, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        msg[n] = '\0';
        int code, down;
        if (cl_fd < 0 || sscanf(msg, "%d %d", &code, &down) != 2)
            continue;
        uint8_t *p = out_reserve(MSG_HDR + 8);
        if (!p)
            continue;
        put_hdr(p, 'k', 8);
        put32(p + 8, (uint32_t)code);
        p[12] = down ? 1 : 0;
        p[13] = p[14] = p[15] = 0;
    }
}

/* ------------------------------------------------------------------ *
 * CPU and RAM, for the viewer's --verbose window
 * ------------------------------------------------------------------ *
 *
 * Every ten seconds while a viewer is attached, and nothing at all without
 * one. CPU is the whole machine's busy share since the last sample, from
 * /proc/stat -- the first sample comes a second after connecting so the
 * window is not blank for ten. RAM is MemAvailable, the kernel's own answer
 * to "how much could still be had", which on a 64 MB phone with zram is the
 * number that predicts the OOM killer; MemFree does not. */
#define STATS_EVERY_US (10 * 1000000LL)

static uint64_t cpu_total_prev, cpu_idle_prev;

static bool read_cpu(uint64_t *total, uint64_t *idle)
{
    unsigned long long v[8] = {0};
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return false;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return false;
    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += v[i];
    *idle = v[3] + v[4];
    return true;
}

static void read_mem(uint32_t kb[4])
{
    static const char *keys[4] = {"MemTotal:", "MemAvailable:", "SwapTotal:", "SwapFree:"};
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");

    memset(kb, 0, 4 * sizeof kb[0]);
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        for (int i = 0; i < 4; i++) {
            size_t kl = strlen(keys[i]);
            if (!strncmp(line, keys[i], kl))
                kb[i] = (uint32_t)strtoul(line + kl, NULL, 10);
        }
    }
    fclose(f);
}

static void queue_stats(void)
{
    uint64_t total, idle;
    uint32_t permille = 0xFFFF, kb[4];

    if (read_cpu(&total, &idle)) {
        uint64_t dt = total - cpu_total_prev, di = idle - cpu_idle_prev;
        if (cpu_total_prev && dt > 0)
            permille = (uint32_t)(1000u * (dt - di) / dt);
        cpu_total_prev = total;
        cpu_idle_prev = idle;
    }
    read_mem(kb);

    uint8_t *p = out_reserve(MSG_HDR + 20);
    if (!p)
        return;
    put_hdr(p, 'S', 20);
    put16(p + 8, permille);
    put16(p + 10, 0);
    for (int i = 0; i < 4; i++)
        put32(p + 12 + 4 * i, kb[i]);
}

/* ============ WHO IS THE VIEWER ============
 *
 * One viewer at a time, and the newest wins -- the likeliest second
 * connection is the same person reopening a window. But "newest connection"
 * is not "newest viewer": `ndlink watch` checks the port is open before it
 * launches anything, and so does anything else that asks whether nd-watchd
 * is up. When a bare connect took the phone, every such check knocked the
 * open window off it, and the window said "reconnecting" over and over.
 *
 * So a connection is PENDING until it speaks: it gets the hello at once (a
 * viewer waits for that before it says anything), and it becomes the viewer
 * only when it sends a viewer's message -- ndlink-watch's options straight
 * after the hello, or a key. A probe that connects and closes, or sends
 * nothing for PENDING_US, is dropped and the open viewer never notices. */
#define PENDING_US (5 * 1000000LL)

static int      pend_fd = -1;
static int64_t  pend_since = 0;
static struct in_addr pend_addr;

static void close_pending(void)
{
    if (pend_fd >= 0)
        close(pend_fd);
    pend_fd = -1;
}

static void new_client(int lfd)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    uint8_t hello[HELLO_LEN];
    int fd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;

    int one = 1, sndbuf = 512 * 1024;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    fill_hello(hello);
    if (send(fd, hello, sizeof hello, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)sizeof hello) {
        close(fd);
        return;
    }
    close_pending();                        /* newest pending wins too */
    pend_fd = fd;
    pend_since = now_us();
    pend_addr = peer.sin_addr;
}

static void promote_pending(void)
{
    if (cl_fd >= 0)
        drop_client("replaced by a new viewer");
    cl_fd = pend_fd;
    pend_fd = -1;
    memset(prev_fb, 0xA5, fb_size);         /* first frame is a whole one */
    echo_bind();
    /* RAM now, CPU once there is a second of /proc/stat to compare. */
    cpu_total_prev = cpu_idle_prev = 0;
    queue_stats();
    stats_next = now_us() + 1000000;
    printf("nd-watchd: viewer %s\n", inet_ntoa(pend_addr));
    fflush(stdout);
}

/* The pending connection said something, or went away. Peeked, not read:
 * once promoted, read_client() parses the same bytes as any other message. */
static void check_pending(void)
{
    uint8_t type;
    ssize_t n = recv(pend_fd, &type, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return;
    if (n == 1 && (type == 'O' || type == 'K'))
        promote_pending();
    else
        close_pending();
}

static void flush_out(void)
{
    while (cl_fd >= 0 && out_len > 0) {
        ssize_t n = send(cl_fd, out_buf, out_len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop_client(strerror(errno));
            return;
        }
        memmove(out_buf, out_buf + n, out_len - (size_t)n);
        out_len -= (size_t)n;
    }
}

static void read_client(void)
{
    for (;;) {
        ssize_t n = recv(cl_fd, in_buf + in_len, sizeof in_buf - in_len, MSG_DONTWAIT);
        if (n == 0) { drop_client("closed"); return; }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop_client(strerror(errno));
            return;
        }
        in_len += (size_t)n;

        while (in_len >= MSG_HDR) {
            uint32_t len = get32(in_buf + 4);
            if (len > MAX_IN_MSG) { drop_client("oversized message"); return; }
            if (in_len < MSG_HDR + len)
                break;
            if (in_buf[0] == 'K' && len >= 5 && !opt_view_only) {
                int32_t code = (int32_t)get32(in_buf + 8);
                if (key_allowed(code))
                    send_key(code, in_buf[12] != 0);
            }
            if (in_buf[0] == 'O' && len >= 1 && !opt_view_only)
                phone_mute((in_buf[8] & 1) != 0);
            memmove(in_buf, in_buf + MSG_HDR + len, in_len - MSG_HDR - len);
            in_len -= MSG_HDR + len;
        }
    }
}

/* Diff the framebuffer and queue the dirty bounding rectangle. The two-pass
 * scan is neodct_displayd's, as in nd-vncd. */
static void queue_frame(void)
{
    size_t row = (size_t)fb_w * fb_bytespp;
    uint32_t ymin = fb_h, ymax = 0;

    for (uint32_t y = 0; y < fb_h; y++) {
        size_t off = (size_t)y * fb_line;
        if (memcmp(fb_data + off, prev_fb + off, row) != 0) {
            if (y < ymin) ymin = y;
            ymax = y;
        }
    }
    if (ymin > ymax)
        return;

    size_t bmin = row, bmax = 0;
    for (uint32_t y = ymin; y <= ymax; y++) {
        const unsigned char *a = fb_data + (size_t)y * fb_line;
        const unsigned char *b = prev_fb + (size_t)y * fb_line;
        if (memcmp(a, b, row) == 0)
            continue;
        size_t i = 0, j = row - 1;
        while (i < row && a[i] == b[i]) i++;
        while (j > i && a[j] == b[j]) j--;
        if (i < bmin) bmin = i;
        if (j > bmax) bmax = j;
    }
    uint32_t x0 = (uint32_t)(bmin / fb_bytespp), x1 = (uint32_t)(bmax / fb_bytespp);
    uint32_t w = x1 - x0 + 1, h = ymax - ymin + 1;

    uint8_t *p = out_reserve(MSG_HDR + 8 + (size_t)w * h * 2);
    if (!p)
        return;                     /* prev_fb untouched: next tick retries */
    put_hdr(p, 'V', 8 + w * h * 2);
    put16(p + 8, x0); put16(p + 10, ymin); put16(p + 12, w); put16(p + 14, h);
    uint8_t *px = p + 16;
    for (uint32_t y = ymin; y <= ymax; y++) {
        const unsigned char *src = fb_data + (size_t)y * fb_line;
        convert_row(src, px, x0, x1);
        px += (size_t)w * 2u;
        memcpy(prev_fb + (size_t)y * fb_line, src, row);
    }
}

static void read_audio(int afd)
{
    static uint8_t dg[TAP_HDR + TAP_CHUNK];
    for (;;) {
        ssize_t n = recv(afd, dg, sizeof dg, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return;
        /* Drained even with no viewer, so the tap's sends succeed into an
         * empty queue rather than piling up to be delivered, stale, to the
         * next viewer that connects. */
        if (cl_fd < 0 || n <= TAP_HDR || memcmp(dg, "NDA1", 4) != 0)
            continue;
        if (out_len > AUDIO_HIGHWATER)
            continue;
        size_t pcm = (size_t)n - TAP_HDR;
        uint8_t *p = out_reserve(MSG_HDR + 8 + pcm);
        if (!p)
            continue;
        put_hdr(p, 'A', (uint32_t)(8 + pcm));
        memcpy(p + 8, dg + 4, 8);           /* rate, channels, bits */
        memcpy(p + 16, dg + TAP_HDR, pcm);
    }
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int usage(void)
{
    fprintf(stderr,
        "nd-watchd: the panel, the speaker and the keys, for ndlink watch\n"
        "  --bind ADDR    IPv4 address to listen on (default 127.0.0.1)\n"
        "  --port N       TCP port (default 5901)\n"
        "  --fps N        frames/sec while a viewer is attached (default 30)\n"
        "  --view-only    never forward keys\n"
        "  --devkey PATH  the key channel (default " ND_PATH_DEVKEY_SOCK ")\n"
        "  --audio NAME   abstract socket the tap sends to (default nd-watchd)\n"
        "  --fb PATH      framebuffer (default /dev/fb0)\n"
        "  --swap-rb      invert the detected red/blue order\n"
        "  --mute-card C  card whose speaker a viewer may mute: usb (default),\n"
        "                 a number, or none\n"
        "  --mute-state P where a mute is recorded (default /run/nd-watchd.muted)\n"
        "  --keyecho PATH where nd-core echoes keys (default " ND_PATH_KEYECHO_SOCK ")\n"
        "nd-watchd --tap [--audio NAME]\n"
        "  read a WAV stream from ALSA's file plugin on stdin and relay it\n");
    return 2;
}

int main(int argc, char **argv)
{
    int tap = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--tap"))       tap = 1;
        else if (!strcmp(argv[i], "--view-only")) opt_view_only = 1;
        else if (!strcmp(argv[i], "--swap-rb"))   opt_swap_rb = 1;
        else if (!strcmp(argv[i], "--bind")   && i + 1 < argc) opt_bind = argv[++i];
        else if (!strcmp(argv[i], "--port")   && i + 1 < argc) opt_port = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--fps")    && i + 1 < argc) opt_fps = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--devkey") && i + 1 < argc) opt_devkey = argv[++i];
        else if (!strcmp(argv[i], "--audio")  && i + 1 < argc) opt_audio = argv[++i];
        else if (!strcmp(argv[i], "--fb")     && i + 1 < argc) opt_fb = argv[++i];
        else if (!strcmp(argv[i], "--mute-card")  && i + 1 < argc) opt_mute_card = argv[++i];
        else if (!strcmp(argv[i], "--mute-state") && i + 1 < argc) opt_mute_state = argv[++i];
        else if (!strcmp(argv[i], "--keyecho") && i + 1 < argc) opt_keyecho = argv[++i];
        else if (tap) {
            /* The tap must never fail its pipe, not even on a bad argument. */
            drain_stdin();
            return 0;
        } else
            return usage();
    }
    if (tap)
        return run_tap();

    if (opt_fps < 1)  opt_fps = 1;
    if (opt_fps > 60) opt_fps = 60;

    restore_stale_mute();
    if (init_framebuffer() != 0)
        return 1;
    out_buf = malloc(OUT_CAP);
    if (!out_buf)
        return 1;

    /* ============ ONE ADDRESS, NEVER THE MODEM ============
     * nd-vncd's rule, for nd-vncd's reason: IPv4 only, bound to the address
     * S42debuglan has just confirmed is on eth0, loopback when run by hand. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)opt_port);
    if (inet_pton(AF_INET, opt_bind, &sa.sin_addr) != 1) {
        fprintf(stderr, "nd-watchd: bad --bind address '%s'\n", opt_bind);
        return 1;
    }
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int one = 1;
    if (lfd < 0 ||
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(lfd, 2) != 0) {
        fprintf(stderr, "nd-watchd: listen %s:%d: %s\n", opt_bind, opt_port, strerror(errno));
        return 1;
    }

    struct sockaddr_un ua;
    socklen_t ualen = audio_addr(&ua);
    int afd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int rcvbuf = 256 * 1024;
    if (afd >= 0)
        (void)setsockopt(afd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    if (afd < 0 || bind(afd, (struct sockaddr *)&ua, ualen) != 0) {
        /* Not fatal: the picture and the keys are most of the value, and a
         * second nd-watchd holding the name is the likeliest cause. */
        fprintf(stderr, "nd-watchd: audio socket @%s: %s -- no sound\n",
                opt_audio, strerror(errno));
        if (afd >= 0) close(afd);
        afd = -1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("nd-watchd: %ux%u on %s:%d at %u fps, audio @%s, %s\n",
           fb_w, fb_h, opt_bind, opt_port, opt_fps, opt_audio,
           opt_view_only ? "view-only" : "keys forwarded");
    fflush(stdout);

    const int64_t period = 1000000 / (int64_t)opt_fps;
    int64_t next = now_us();

    while (!g_stop) {
        struct pollfd pf[5];
        int n = 0, ci = -1, ai = -1, ei = -1, pi = -1;
        pf[n].fd = lfd; pf[n].events = POLLIN; n++;
        if (afd >= 0) { ai = n; pf[n].fd = afd; pf[n].events = POLLIN; n++; }
        if (echo_fd >= 0) { ei = n; pf[n].fd = echo_fd; pf[n].events = POLLIN; n++; }
        if (pend_fd >= 0) { pi = n; pf[n].fd = pend_fd; pf[n].events = POLLIN; n++; }
        if (cl_fd >= 0) {
            ci = n;
            pf[n].fd = cl_fd;
            pf[n].events = (short)(POLLIN | (out_len ? POLLOUT : 0));
            n++;
        }

        /* No viewer: sleep until something happens. With one: wake for the
         * next frame. */
        int timeout = -1;
        if (cl_fd >= 0) {
            int64_t left = next - now_us();
            timeout = left <= 0 ? 0 : (int)((left + 999) / 1000);
        }
        if (pend_fd >= 0 && (timeout < 0 || timeout > 1000))
            timeout = 1000;                 /* to time the pending one out */
        if (poll(pf, (nfds_t)n, timeout) < 0 && errno != EINTR)
            break;

        if (pi >= 0 && pend_fd >= 0 && pf[pi].revents)
            check_pending();
        if (pend_fd >= 0 && now_us() - pend_since > PENDING_US)
            close_pending();
        if (pf[0].revents & POLLIN)
            new_client(lfd);
        if (ai >= 0 && (pf[ai].revents & POLLIN))
            read_audio(afd);
        if (ei >= 0 && echo_fd >= 0 && (pf[ei].revents & POLLIN))
            read_echo();
        if (ci >= 0 && cl_fd >= 0 && pf[ci].fd == cl_fd) {
            if (pf[ci].revents & (POLLERR | POLLHUP))
                drop_client("hung up");
            else if (pf[ci].revents & POLLIN)
                read_client();
        }

        if (cl_fd >= 0) {
            int64_t t = now_us();
            if (t >= next) {
                if (out_len < VIDEO_HIGHWATER)
                    queue_frame();
                next += period;
                if (next < t)           /* fell behind: do not burst to catch up */
                    next = t + period;
            }
            if (t >= stats_next) {
                queue_stats();
                stats_next = t + STATS_EVERY_US;
            }
            flush_out();
        } else {
            next = now_us();
        }
    }

    drop_client("stopping");
    close_pending();
    if (echo_fd >= 0)
        (void)unlink(opt_keyecho);
    printf("nd-watchd: stopping\n");
    return 0;
}
