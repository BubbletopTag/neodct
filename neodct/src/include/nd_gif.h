/* nd_gif.h -- animated GIF, decoded one frame at a time.
 *
 * A GIF is the only animated thing the phone renders, and it exists for
 * exactly one reason: an animated wallpaper. That single use case is what
 * decides the shape of this API.
 *
 * ============ WHY THIS STREAMS INSTEAD OF DECODING TO AN ARRAY ============
 *
 * The wallpaper shipped with this feature is 240x175 and 125 frames long.
 * Decoded to RGB888 that is 125 * 126,000 = 15.75 MB, which is 30% of the
 * phone's usable RAM for a picture behind the clock. So nothing is kept: the
 * decoder holds one composed canvas and walks the file, and a frame exists
 * only for as long as it is on screen.
 *
 * The cost of that choice is that seeking backwards means starting over --
 * GIF frames are deltas onto the frame before them, so there is no such thing
 * as decoding frame 90 on its own. nd_gif_rewind() therefore seeks back to
 * the first image descriptor and clears the canvas, and playback is forward
 * only. For a looping wallpaper that is the only motion there is.
 *
 * ============ WHAT IT COSTS ============
 *
 * Per open, for a 240x175 file:
 *   composed canvas   240*175*4 = 168,000 bytes  (RGBA, so an undrawn pixel
 *                                                 is transparent black and
 *                                                 not a stale colour)
 *   index scratch     240*175   =  42,000 bytes
 *   LZW tables        4096*(2+1+1) =  16,384 bytes
 *   disposal backup   168,000 bytes, ALLOCATED ONLY IF A FRAME ASKS FOR IT
 *
 * About 226 KB for a file that never uses disposal method 3, which is most of
 * them. The file itself is never read into memory; it is walked with a FILE*.
 *
 * ============ HOSTILE INPUT ============
 *
 * A wallpaper can come off an SD card, so this parses like SECURITY.md says
 * to: every length is bounded before it is allocated, every table index is
 * checked against the table it indexes, the LZW dictionary cannot grow past
 * 4096 entries, and a frame's rectangle is clipped to the logical screen
 * before a single pixel is written. A malformed file loses its remaining
 * frames; it does not lose the phone.
 */

#ifndef ND_GIF_H_INCLUDED
#define ND_GIF_H_INCLUDED

#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A GIF may declare a logical screen far larger than this panel. The cap is
 * nd_image_priv.h's, restated here because this header is public and a caller
 * deserves to know what will be refused. */
#define ND_GIF_MAX_DIM 16384

/* Frames past this are ignored. 125 is the shipped wallpaper; 4096 is four
 * hundred times more animation than a 240x175 panel has any use for and it
 * bounds the open-time scan against a file that claims to be endless. */
#define ND_GIF_MAX_FRAMES 4096

/* What a frame with a zero or absent delay is shown for. Browsers clamp this
 * way too: a "0 ms" GIF means "as fast as you can", and on a phone drawing
 * 126,000 bytes a frame the honest answer is 100 ms. */
#define ND_GIF_DEFAULT_DELAY_MS 100

/* The floor a nonzero delay is clamped to. GIFs written for desktops often
 * say 10 ms and mean "fast"; the panel cannot show more than it can, and the
 * core loop would spend the whole battery trying. */
#define ND_GIF_MIN_DELAY_MS 20

typedef struct nd_gif nd_gif;

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

/* Open and validate. The header, the global colour table and the whole block
 * structure are read here -- the structural walk counts frames and sums the
 * loop duration without running LZW on any of them, so an unplayable file is
 * refused before a caller builds a UI around it.
 *
 * NULL on any failure, including a file that is not a GIF. Nothing is logged
 * for a missing file, matching nd_image_open(). */
nd_gif *nd_gif_open(const char *path);

/* Decode from memory. The buffer is NOT retained -- it is copied, because the
 * decoder outlives any single call and a caller's buffer may not. */
nd_gif *nd_gif_open_mem(const uint8_t *data, size_t len);

void nd_gif_close(nd_gif *g);

/* ------------------------------------------------------------------ *
 * What it is
 * ------------------------------------------------------------------ */

int32_t nd_gif_width(const nd_gif *g);
int32_t nd_gif_height(const nd_gif *g);

/* Counted by the open-time scan, so this is exact and available immediately.
 * 1 for a still GIF. */
size_t nd_gif_frame_count(const nd_gif *g);

/* One trip through every frame's delay, in milliseconds, with the same
 * clamping nd_gif_next() applies. 0 for a still GIF. */
int32_t nd_gif_duration_ms(const nd_gif *g);

static inline bool nd_gif_animated(const nd_gif *g) ND_UNUSED_FN;
static inline bool nd_gif_animated(const nd_gif *g)
{
    return nd_gif_frame_count(g) > 1u;
}

/* ------------------------------------------------------------------ *
 * Playback
 * ------------------------------------------------------------------ */

/* Compose the next frame and return the decoder's canvas.
 *
 * THE RETURNED IMAGE IS OWNED BY THE DECODER. It is RGBA8888 at the logical
 * screen size, it is the same pointer every call, and its pixels change under
 * the caller on the next call -- blit from it, do not free it, do not keep it
 * across nd_gif_close().
 *
 * *delay_ms, when not NULL, gets how long this frame should be shown, already
 * clamped to [ND_GIF_MIN_DELAY_MS, ...] with a zero delay replaced by
 * ND_GIF_DEFAULT_DELAY_MS.
 *
 * Playback loops: the frame after the last is the first again, with the
 * canvas cleared, so a caller can drive this forever and never check.
 * NULL only when the file is unreadable or the first frame itself is
 * corrupt -- a file that breaks halfway rewinds and keeps playing what it
 * has. */
const nd_image *nd_gif_next(nd_gif *g, int32_t *delay_ms);

/* Back to the first frame, canvas cleared. The next nd_gif_next() returns
 * frame 0. */
void nd_gif_rewind(nd_gif *g);

/* Which frame the last nd_gif_next() returned, zero-based. Before the first
 * call this is 0 and nothing has been decoded. */
size_t nd_gif_position(const nd_gif *g);

/* ------------------------------------------------------------------ *
 * Stills
 * ------------------------------------------------------------------ */

/* The first frame as an ordinary owned image, for callers that want a picture
 * rather than an animation -- nd_image_open()'s GIF branch, an icon that
 * happens to be a GIF, the Settings preview. RGBA8888; a fully transparent
 * pixel is transparent black, so converting to RGB888 shows the panel's own
 * background rather than a stale colour.
 *
 * Owned by the caller. NULL on any failure. */
nd_image *nd_gif_load_first(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ND_GIF_H_INCLUDED */
