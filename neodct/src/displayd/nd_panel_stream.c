/* nd_panel_stream.c -- the emulator's transport, and the host tests'.
 *
 * One backend with two configurations, and saying so is the honest way to
 * pick between them. "A file the host can render" and "a null backend" are
 * not alternatives: they are this file with and without an fd.
 *
 *   --panel null            open() succeeds, every DC-framed call is counted
 *                           and discarded. No GPIO, no bus, no allocation.
 *                           This is what the parity probe runs -- see below.
 *   --panel stream:<path>   the same, plus the transcript nd_panel.h pins,
 *                           written to <path>, O_WRONLY|O_NONBLOCK.
 *
 * Under QEMU <path> is /dev/vport0p1, a virtio-console port. Measured in
 * this container: the guest gets /sys/class/virtio-ports/vport0p1 named
 * neodct.panel and /dev/vport0p1 c 252,1; 31 bytes arrived on the host
 * byte-for-byte with no escaping and no line discipline, because it is a byte
 * pipe and not a tty; and MemTotal was 54,812 kB with the device and 54,812 kB
 * without it. The transport is free, and CONFIG_VIRTIO_CONSOLE=y is already
 * in the kernel config.
 *
 * ============ WHY THE NULL CONFIGURATION HAS TO EXIST ============
 *
 * It is not "the stream pointed at /dev/null". The virtio-serial device that
 * carries the transcript CHANGES THE MACHINE: the guest grows /dev/vport0p1
 * and /sys/class/virtio-ports, nd-inventory records both, and dev.count moves.
 * A parity baseline must describe the machine somebody boots, so the probe
 * runs the null configuration and the panel-stream device belongs only in
 * run_qemu.sh's display cases, where a human has asked to watch.
 *
 * ============ WHY THE DECODER IS NOT IN HERE ============
 *
 * The other candidate was a 240x240 GRAM in this file, exporting decoded
 * frames. That is 115,200 bytes against the daemon's measured 283,200 bytes
 * of real buffers (prev_fb 168,000 + out_buf 115,200): +41% RSS, spent by the
 * test harness, on the one machine whose whole claim is that its memory
 * pressure is honest. It also puts the decoder inside the program whose
 * encoder it is meant to check, so the two would agree by construction.
 * neodct/tools/st7789_replay.py does it on the host instead, written from the
 * datasheet, and costs the guest nothing.
 *
 * ============ THE SINK MAY NEVER BECOME THE PRODUCT ============
 *
 * Two properties, both about not letting a diagnostic take the pacer down:
 *
 *   * a record that will not fit is never retried and never half-written.
 *     The frame pacer is the thing being measured; a picture is a
 *     convenience.
 *   * a failed write disables the sink for the life of the process and logs
 *     once. A stream backend that died would take the daemon with it.
 *   * and a record that could not go out ENDS the transcript rather than
 *     leaving a hole in it -- see the drop branch in emit(). A hole is
 *     invisible to every reader of this format, because an ST7789
 *     conversation pairs a command with its parameters by position; the
 *     record framing survives a drop and the meaning does not.
 *
 * ============ AND THE DROP DECISION IS MADE BEFORE ANY BYTE GOES OUT ======
 *
 * "Drop a short write" is not implementable, which a boot found rather than a
 * review: MEASURED on the emulator, one write of a 115,200-byte blanking fill
 * to /dev/vport0p1 returns 32,773 -- virtio-console hands over one 32 KiB
 * buffer per call and comes back for more. Bytes that have gone out cannot be
 * recalled, so a record abandoned half way desynchronises the transcript and
 * every record after it mis-frames. Treating that as a fatal desync killed
 * the sink on the very first frame, every time.
 *
 * So the commitment point is explicit: the five-byte record header is written
 * FIRST and on its own, and it is the affordability test. If it will not go
 * (EAGAIN, nothing written) nothing has been committed, and the sink is
 * disabled there and says so once -- see emit() for why a hole in the
 * conversation is worse than a short transcript. Once the header HAS gone,
 * the payload is finished -- non-blocking, with a bounded wait of
 * ND_PANEL_WRITE_DEADLINE_MS in total.
 *
 * That bound is the whole of the pacer's exposure: a host that has stopped
 * draining the port costs one late frame and then the sink is disabled for
 * the life of the process, so it can never cost a second. The alternative --
 * an unbounded retry -- is the frame pacer parked on a diagnostic, which is
 * what now_ms()'s comment in neodctDisplay.c is the record of.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "nd_panel.h"

struct stream_ctx {
    const char *path;         /* NULL is the null sink */
    unsigned short panel_w;
    unsigned short panel_h;
    int fd;                   /* -1 for the null sink, and once closed */
    int dead;                 /* a write failed or desynchronised */
    unsigned long records;
    unsigned long dropped;
    unsigned long long bytes;
};

static struct stream_ctx stream_state = {
    NULL, 0, 0, -1, 0, 0UL, 0UL, 0ULL
};

static void put_le16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_le32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFFuL);
    p[1] = (unsigned char)((v >> 8) & 0xFFuL);
    p[2] = (unsigned char)((v >> 16) & 0xFFuL);
    p[3] = (unsigned char)((v >> 24) & 0xFFuL);
}

/* How long a record that has already begun may hold the pacer up. One frame
 * at 30 fps is 33 ms; a quarter second is generous enough that an emulator
 * host briefly busy does not lose the picture, and short enough that a host
 * that has stopped reading costs one visibly late frame and no more. */
#define ND_PANEL_WRITE_DEADLINE_MS 250

static double mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void sink_die(struct stream_ctx *c, const char *what)
{
    fprintf(stderr, "panel stream: %s; the transcript ends here and the sink "
                    "is disabled for the life of this process\n", what);
    c->dead = 1;
}

/* The rest of a record that has already been committed to. Non-blocking
 * throughout: EAGAIN sleeps a millisecond and tries again until the deadline,
 * and the deadline disables the sink rather than being extended. */
static int finish(struct stream_ctx *c, const unsigned char *buf, size_t n)
{
    double deadline = mono_ms() + (double)ND_PANEL_WRITE_DEADLINE_MS;
    size_t done = 0u;

    while (done < n) {
        ssize_t w = write(c->fd, buf + done, n - done);

        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            sink_die(c, strerror(errno));
            return -1;
        }
        if (mono_ms() >= deadline) {
            sink_die(c, "the reader stopped draining mid-record");
            return -1;
        }
        usleep(1000);
    }
    return 0;
}

/* One record, or none at all. */
static void emit(struct stream_ctx *c, unsigned char tag,
                 const unsigned char *payload, size_t n)
{
    unsigned char hdr[ND_PANEL_REC_HDR_LEN];
    ssize_t w;

    c->records++;
    c->bytes += (unsigned long long)n;

    if (c->fd < 0)
        return;
    if (c->dead) {
        c->dropped++;
        return;
    }

    hdr[0] = tag;
    put_le32(hdr + 1, (unsigned long)n);

    /* The affordability test, and the commitment point. */
    w = write(c->fd, hdr, sizeof hdr);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* ============ A DROP ENDS THE TRANSCRIPT ============
             *
             * This used to drop the record, count it and CARRY ON, on the
             * grounds that the record framing was still intact. It is -- and
             * that is the wrong layer. Above the framing is an ST7789
             * conversation in which a command and its parameters are paired
             * by position and nothing else: drop the `C 0x2B` and the four
             * RASET bytes that follow are read as CASET's, the window comes
             * out as (105,0)-(105,239) instead of (100,105)-(100,105), and
             * the decoder draws a plausible wrong picture. There is no
             * sequence number and no gap marker for a reader to notice with,
             * and the drop counter is printed only by stream_close() -- into
             * the guest's log, at SIGTERM, where the host holding the file
             * never sees it.
             *
             * So the sink dies here instead. THE PACER IS NOT PAID FOR THIS:
             * a dead sink makes every later emit() an increment and a return,
             * exactly as a drop did, so this costs the frame loop nothing and
             * buys a transcript that is either complete or truthfully ends
             * where it stopped being complete. One late frame was always the
             * documented price of the deadline in finish(); a wrong picture
             * never was. */
            c->dropped++;
            sink_die(c, "the reader could not take a record header "
                        "(the port's buffer is full)");
            return;
        }
        sink_die(c, strerror(errno));
        return;
    }
    if ((size_t)w != sizeof hdr && finish(c, hdr + w, sizeof hdr - (size_t)w) < 0)
        return;
    if (n > 0u)
        (void)finish(c, payload, n);
}

static int stream_open(struct nd_panel *p)
{
    struct stream_ctx *c = p->ctx;
    unsigned char hdr[ND_PANEL_HDR_LEN];

    if (c->path == NULL) {
        printf("panel: null sink (no GPIO, no SPI bus, no transcript)\n");
        return 0;
    }

    /* NO O_CREAT, and a boot is why. With it, a guest whose virtio-serial
     * port had not been attached opened /dev/vport0p1 as a NEW REGULAR FILE
     * in devtmpfs, printed "transcript to /dev/vport0p1" and ran happily with
     * the picture going nowhere anybody would look -- which is precisely the
     * "silently start writing a panel stream to nowhere and report success"
     * that this backend exists not to do. The path must already be there: the
     * emulator's port is created by QEMU and a host test creates its file.
     * O_TRUNC so a rerun does not append to the last run's transcript. */
    c->fd = open(c->path, O_WRONLY | O_NONBLOCK | O_TRUNC);
    if (c->fd < 0) {
        /* Fatal, and deliberately. The backend is chosen by an explicit
         * argument, so a path that cannot be opened is a misconfiguration at
         * startup -- not a reason to degrade quietly. Runtime is lossy;
         * startup is loud. */
        fprintf(stderr, "open %s failed: %s (the panel transcript is written "
                        "to a path that already exists -- QEMU's virtio port, "
                        "or a file you created)\n", c->path, strerror(errno));
        return -1;
    }

    memcpy(hdr, ND_PANEL_MAGIC, ND_PANEL_MAGIC_LEN);
    hdr[4] = ND_PANEL_VERSION;
    hdr[5] = 0;                       /* flags: none defined yet */
    put_le16(hdr + 6, c->panel_w);
    put_le16(hdr + 8, c->panel_h);
    put_le16(hdr + 10, 0);            /* reserved */
    if (write(c->fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        fprintf(stderr, "panel stream: cannot write the transcript header to "
                        "%s: %s\n", c->path, strerror(errno));
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    printf("panel: transcript to %s (ND79 v%d, %ux%u)\n",
           c->path, ND_PANEL_VERSION,
           (unsigned)c->panel_w, (unsigned)c->panel_h);
    return 0;
}

static void stream_reset(struct nd_panel *p)
{
    emit(p->ctx, ND_PANEL_TAG_RESET, NULL, 0);
}

static void stream_cmd(struct nd_panel *p, unsigned char op)
{
    emit(p->ctx, ND_PANEL_TAG_CMD, &op, 1);
}

static void stream_data(struct nd_panel *p, const unsigned char *buf, size_t n)
{
    emit(p->ctx, ND_PANEL_TAG_DATA, buf, n);
}

static void stream_close(struct nd_panel *p)
{
    struct stream_ctx *c = p->ctx;

    /* Printed on both configurations, because on the null sink it is the only
     * evidence that a 240x240 frame was composed at all: the counters are the
     * same counters the transcript would have carried. */
    printf("panel %s: %lu records, %llu payload bytes, %lu dropped%s\n",
           p->name, c->records, c->bytes, c->dropped,
           c->dead ? ", sink disabled" : "");

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static struct nd_panel stream_panel = {
    .name  = "stream",
    .open  = stream_open,
    .reset = stream_reset,
    .cmd   = stream_cmd,
    .data  = stream_data,
    .close = stream_close,
    .ctx   = &stream_state,
};

struct nd_panel *nd_panel_stream(const char *path,
                                 unsigned short panel_w, unsigned short panel_h)
{
    stream_state.path    = path;
    stream_state.panel_w = panel_w;
    stream_state.panel_h = panel_h;
    stream_panel.name    = (path == NULL) ? "null" : "stream";
    return &stream_panel;
}
