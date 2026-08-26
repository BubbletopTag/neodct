/* nd_capture.h -- the two framebuffer backends that are not /dev/fb0, and the
 * golden-frame manifest writer.
 *
 * ADDITION to the frozen header set. nd_fb.h describes exactly one nd_fb: the
 * one that mmaps the panel. Two others are needed and neither belongs in that
 * header.
 *
 *   the MEMORY backend   an nd_fb whose mapping is ordinary heap memory.
 *                        Everything a real framebuffer does -- geometry,
 *                        stride, packing, the centred band -- happens for
 *                        real, and the result can be read back and checked.
 *                        That is what test_nd_fb.c compares against the
 *                        Python driver's own output.
 *
 *   the CAPTURE backend  an nd_fb that never packs anything. It keeps a copy
 *                        of the RGB band the UI handed over, exactly as
 *                        uistub.CapturingFramebuffer does, and advances the
 *                        virtual clock by one tick per committed frame. This
 *                        is what nd-shoot installs as ui->fb.
 *
 * ============ THE DIGEST IS OVER PIXELS, NOT OVER THE PNG ============
 *
 * goldenframe.frame_digest() hashes b"<w>,<h>|" followed by tightly packed
 * RGB rows. Two PNG encoders write different bytes for the same picture, and
 * it is the picture the port has to match. The PNG is written only so that
 * goldenframe's _describe_pixel_diff() can show a human where the difference
 * is when a frame fails.
 *
 * ============ WHY THE FRAME RING IS BOUNDED ============
 *
 * The Python keeps every frame ever drawn in a list, and shoot_docs.py slices
 * it. A capture run of Koki draws hundreds of frames at 126,000 bytes each and
 * saves one. So this keeps the last ND_CAPTURE_RING_DEFAULT of them in
 * buffers it reuses -- after the first few frames the capture path does not
 * allocate at all, which is the same rule the render path follows.
 */

#ifndef ND_CAPTURE_H_INCLUDED
#define ND_CAPTURE_H_INCLUDED

#include "nd_fb.h"
#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The memory backend
 * ------------------------------------------------------------------ */

/* An nd_fb backed by calloc instead of by mmap. line_length may be 0, which
 * means "xres * bpp / 8" -- the same fallback the driver applies, so a caller
 * that passes 0 gets the phone's own geometry rules. Pass a larger value to
 * exercise row padding.
 *
 * The buffer starts zeroed, because the real driver zeroes its mapping once
 * at open and every partial-band write since depends on that.
 *
 * *out is owned by the caller; release with nd_fb_close() like any other. */
nd_err nd_fb_open_mem(nd_fb **out, int32_t xres, int32_t yres, int32_t bpp, size_t line_length);

/* The backing bytes of a memory framebuffer, for a test to inspect. NULL for
 * a device or capture framebuffer. */
const uint8_t *nd_fb_mem_bytes(const nd_fb *fb, size_t *size_out);

/* ------------------------------------------------------------------ *
 * The capture backend
 * ------------------------------------------------------------------ */

#define ND_CAPTURE_RING_DEFAULT 8   /* frames kept; see the header comment  */
#define ND_CAPTURE_FRAMES_MAX   128 /* manifest entries; 49 shots plus room */
#define ND_CAPTURE_NAME_MAX     64  /* longest is "eng-cubebench" today     */

/* The panel the letterboxed device_frame() view describes. uistub centres the
 * band in it; the hardware bottom-aligns instead, and neither side is to be
 * "fixed" -- see spec-core-loop.md section 2. */
#define ND_CAPTURE_PANEL_W 240
#define ND_CAPTURE_PANEL_H 240

typedef struct nd_capture nd_capture;

/* Create the output directory (mkdir -p) and open a capture.
 *
 * THE DIRECTORY IS ND_ROOT-RESOLVED, like every other path this library
 * opens. It has to be: nd_image_save_png() resolves, so a capture directory
 * that did not would have its manifest written in one place and its PNGs in
 * another. With NEODCT_ROOT unset -- which is how nd-shoot runs unless a test
 * harness says otherwise -- resolving is a plain copy and the path means
 * exactly what it says.
 *
 * ring_frames of 0 means ND_CAPTURE_RING_DEFAULT.
 *
 * *out is owned by the caller; release with nd_capture_close(). */
nd_err nd_capture_open(nd_capture **out, const char *dir, size_t ring_frames);
void nd_capture_close(nd_capture *cap);

/* Install this as ui->fb. Owned by the capture; do NOT nd_fb_close() it. */
nd_fb *nd_capture_fb(nd_capture *cap);

/* Frames committed since the capture opened -- which is also the virtual
 * clock's frame number when this is the only framebuffer in the process. */
uint64_t nd_capture_frames_drawn(const nd_capture *cap);

/* A recorded frame: back == 0 is the most recent, 1 the one before it, up to
 * the ring size. NULL when that far back is no longer held. Owned by the
 * capture and overwritten by later frames -- save it or copy it. */
const nd_image *nd_capture_recent(const nd_capture *cap, size_t back);

/* uistub's device_frame(): a panel-sized black image with the band pasted
 * centred, ((pw - bw) / 2, (ph - bh) / 2). Owned by the caller; free with
 * nd_image_free(). NULL when that frame is not held or on allocation failure. */
nd_image *nd_capture_device_frame(const nd_capture *cap, size_t back, int32_t panel_w,
                                  int32_t panel_h);

/* ---- the frame budget ---- */

/* uistub's set_budget(): allow only this many more commits, then refuse.
 * Koki and the games never read the keypad, so a draw count is the only
 * reliable way to stop them. A negative budget means unlimited.
 *
 * A refused commit returns ND_ERR_BUSY from nd_fb_update(), is not recorded,
 * and does NOT advance the clock -- ScriptExhausted does not tick either. */
void nd_capture_set_budget(nd_capture *cap, int64_t frames);
void nd_capture_clear_budget(nd_capture *cap);
bool nd_capture_exhausted(const nd_capture *cap);

/* ---- writing the reference set ---- */

/* Write <dir>/<name>.png and record the manifest entry. img is converted to
 * RGB before hashing and writing, so an RGBA canvas is handled the way
 * frame_digest() handles it. Rejects a duplicate name, a name containing '/',
 * and more than ND_CAPTURE_FRAMES_MAX entries. */
nd_err nd_capture_save(nd_capture *cap, const char *name, const nd_image *img);

/* Convenience: save nd_capture_recent(cap, back). */
nd_err nd_capture_save_recent(nd_capture *cap, const char *name, size_t back);

size_t nd_capture_count(const nd_capture *cap);

/* Write <dir>/manifest.json in goldenframe.write_manifest()'s exact shape:
 * json.dump(indent=2, sort_keys=True), frames sorted by name, and
 * "text_layout": "BASIC" -- compare() rejects a manifest without it, on both
 * sides, because a reference captured with RAQM describes a phone that does
 * not exist. */
nd_err nd_capture_write_manifest(const nd_capture *cap);

/* ------------------------------------------------------------------ *
 * The digest, on its own
 * ------------------------------------------------------------------ */

/* sha256(b"<w>,<h>|" + tightly packed RGB rows), lowercase hex.
 * out_sz must be at least 65. */
nd_err nd_capture_digest(const nd_image *img, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_CAPTURE_H_INCLUDED */
