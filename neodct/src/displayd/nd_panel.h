/* nd_panel.h -- how panel bytes reach a panel, and nothing else.
 *
 * neodct_displayd is two programs wearing one coat. One half composes: it
 * diffs /dev/fb0 against the last frame, finds the dirty rectangle, packs
 * 8888 down to big-endian RGB565 and works out the CASET/RASET window that
 * places the 240x175 UI band on the 240x240 panel at y=65. The other half is
 * a transport: sysfs GPIO for DC and RESET, spidev for the bytes. Every
 * function in neodctDisplay.c belonged to exactly one of those halves
 * already, which is why this seam is a move rather than a redesign.
 *
 * This header is the transport half. The composing half stays in
 * neodctDisplay.c and is SHARED by every backend -- if any of it moved down
 * here, the emulator would be testing a second implementation instead of the
 * phone's.
 *
 * ============ WHY THE SEAM IS AT THE DC-FRAMED BYTE ============
 *
 * The obvious alternative is present(x0, y0, x1, y1, rgb565be, n), which
 * would spare a non-SPI backend an ST7789 decoder. It is the wrong place.
 * The partial-window optimisation is not the integers xmin..ymax -- it is the
 * six bytes set_window() builds out of them and the `+ opt_yoff` it folds in.
 * Above a rect seam those bytes are produced by code no test on either machine
 * ever reads back, so a wrong CASET is invisible everywhere. Below this seam a
 * decoder written independently from the datasheet reads them, and an
 * independent second implementation is the only kind that can disagree with a
 * wrong encoder.
 *
 * ============ AND WHY IT IS NOT LOWER, AT spi_send() ============
 *
 * spi_chunk comes from /sys/module/spidev/parameters/bufsiz and is 4096 or
 * 65536 depending on a kernel cmdline the emulator does not have. A transcript
 * that recorded chunk boundaries would bake a phone-and-cmdline-specific
 * number into a committed artefact: adding spidev.bufsiz=65536 would change
 * every golden byte and the diff would say nothing about the panel. So the
 * backend receives LOGICAL DC-framed calls and the spidev backend chunks
 * internally.
 *
 * NAMED HOLE, so nobody mistakes this for coverage: the chunk loop inside
 * spi_send() is exercised on hardware and nowhere else. It was that way
 * before this seam existed and it still is. Closing it would cost more than
 * it buys, so it is written down instead of papered over.
 *
 * ============ THE VTABLE IS NOT nd_fb's ============
 *
 * libneodct has an nd_fb_backend enum and an nd_fb_sink_fn with a similar
 * shape. Do not reuse either. That seam is INSIDE the library and means
 * "where the UI's composed image goes"; this one is outside it and means "how
 * panel bytes reach a panel". neodct_displayd deliberately links no libneodct
 * -- the Makefile spends thirty lines on why, and mkinitramfs.py copies this
 * binary into the initramfs where /NeoDCT/System is by definition not mounted
 * -- so sharing a name would suggest a dependency that must never exist.
 *
 * Six indirect calls per frame (two CASET, two RASET, RAMWR, the payload).
 * Not per pixel and not per chunk, so the dispatch cost on a Cortex-A7 is
 * unmeasurable.
 */

#ifndef ND_PANEL_H_INCLUDED
#define ND_PANEL_H_INCLUDED

#include <stddef.h>

struct nd_panel {
    const char *name; /* "spidev" | "stream" | "null" -- appears in the log */

    /* Acquire the transport. Non-zero means the daemon must not start: the
     * backend is CHOSEN and never detected, so a failure here is loud and
     * fatal rather than a quiet fall back to a sink that goes nowhere. */
    int (*open)(struct nd_panel *p);

    /* The panel's hardware reset pulse train. A backend with no reset line
     * records the event and returns; it must never shorten the phone's. */
    void (*reset)(struct nd_panel *p);

    /* One command byte, DC low. */
    void (*cmd)(struct nd_panel *p, unsigned char op);

    /* n data bytes, DC high. n is up to a whole 240x240 frame (115,200). */
    void (*data)(struct nd_panel *p, const unsigned char *buf, size_t n);

    void (*close)(struct nd_panel *p);

    void *ctx;
};

/* The phone. Talks to /dev/spidev0.0 and the sysfs GPIO class, at speed_hz.
 *
 * ctx is NULL and that is deliberate: there is exactly one SPI bus, one DC
 * pin and one panel on this board, so the state stays in the file-scope
 * statics it was already in. Keeping them makes the move out of
 * neodctDisplay.c readable as a cut-and-paste, which is the review property
 * the whole refactor rests on -- this is the one file in the tree with no
 * host test, no ASAN coverage and a -Wconversion exemption granted on the
 * strength of being proven on hardware nobody here has. */
struct nd_panel *nd_panel_spidev(int speed_hz);

/* The emulator, and the host tests. path NULL is the null sink: open()
 * succeeds, every call is counted and discarded, no GPIO, no bus, no
 * allocation. Otherwise every DC-framed call is also written to path as the
 * transcript described below. panel_w/panel_h go in the transcript header and
 * are the caller's, not this file's -- the geometry that composes the frame
 * and the geometry that describes it can then never disagree. */
struct nd_panel *nd_panel_stream(const char *path,
                                 unsigned short panel_w, unsigned short panel_h);

/* ============ THE TRANSCRIPT, PINNED HERE BECAUSE IT IS AN INTERFACE ======
 *
 * neodct/tests/test_displayd_stream.py asserts these bytes and
 * neodct/tools/st7789_replay.py decodes them, so the format is a committed
 * artefact rather than an implementation detail. Get it wrong once and every
 * later change invalidates the baseline; hence the version byte, which the
 * decoder REFUSES rather than half-understanding.
 *
 * Header, once, 12 bytes:
 *
 *     "ND79"  version:u8  flags:u8  panel_w:u16  panel_h:u16  reserved:u16
 *
 * Then records, until the stream ends:
 *
 *     tag:u8  len:u32  payload[len]
 *
 *     'C'  one command byte      (len 1)
 *     'D'  data bytes            (len >= 1)
 *     'R'  the reset pulse train (len 0)
 *
 * Record boundaries are the DC transitions and NOTHING else -- see the note
 * about spi_send() above. Every multi-byte field is little-endian and is
 * written a byte at a time, because CODING-STANDARDS section 6 forbids an
 * endianness assumption in a wire format and this one is read on x86-64 and
 * written on armv7.
 *
 * WHAT THE SINK DOES UNDER LOAD, because somebody will report it as a bug:
 * the fd is O_NONBLOCK and a record that will not fit is never retried. The
 * frame pacer is the product -- now_ms()'s comment in neodctDisplay.c is the
 * record of what a parked pacer costs -- and a picture for a human to look at
 * is not worth missing a deadline for.
 *
 * A record is written WHOLE or not at all, and that decision is made before
 * any byte of it goes out. The distinction is not pedantry: measured on the
 * emulator, one write of the 115,200-byte blanking fill to /dev/vport0p1
 * returns 32,773, because virtio-console hands over one 32 KiB buffer per
 * call. Bytes already gone cannot be recalled, so a record abandoned half way
 * would mis-frame everything after it. nd_panel_stream.c writes the five-byte
 * record header first and alone -- that is the affordability test -- and
 * finishes a committed payload under a bounded deadline.
 *
 * AND A RECORD THAT COULD NOT GO OUT ENDS THE TRANSCRIPT. It used to be
 * dropped, counted and followed by the next one, which keeps the RECORD
 * framing intact and breaks the layer above it: a command and its parameters
 * are paired by position in this format, so a missing `C 0x2B` makes the four
 * bytes after it read as CASET's and the window come out somewhere else
 * entirely. There is no sequence number to notice that with. A transcript is
 * therefore either complete or truthfully short, and the sink is disabled for
 * the life of the process and says so once -- which costs the pacer nothing,
 * because a dead sink is an increment and a return. */

#define ND_PANEL_MAGIC       "ND79"
#define ND_PANEL_MAGIC_LEN   4
#define ND_PANEL_VERSION     1
#define ND_PANEL_HDR_LEN     12
#define ND_PANEL_REC_HDR_LEN 5

#define ND_PANEL_TAG_CMD   'C'
#define ND_PANEL_TAG_DATA  'D'
#define ND_PANEL_TAG_RESET 'R'

#endif /* ND_PANEL_H_INCLUDED */
