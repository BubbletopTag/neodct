/* nd_image.h -- the surface type and the fifteen Pillow operations this
 * project actually uses.
 *
 * We are not porting Pillow. Every Pillow call in the whole overlay was
 * counted, and it comes to about fifteen operations: make a picture, load one,
 * paste one onto another, crop, resize, convert, and a couple of oddities that
 * only Koki needs. That is what is here and nothing else.
 *
 * ================== THE TWO BLEND FORMULAS ==================
 *
 * These were MEASURED against Pillow 12.3.0, not recalled from documentation,
 * and they DISAGREE WITH EACH OTHER ON PURPOSE. Anyone who unifies them for
 * tidiness breaks all thirty wallpapered golden frames.
 *
 * (a) Glyph and alpha compositing -- ImageDraw.text(), Koki's alpha pastes:
 *
 *         out = (dst * (255 - mask) + ink * mask + 127) / 255      // truncating
 *
 *     Note the +127 and the TRUNCATING divide. This is NOT the MULDIV255
 *     macro, which differs on 52,910 of the 227,328 possible inputs. Verified
 *     with zero mismatches over the full sweep. Use nd_blend8().
 *
 * (b) Wallpaper dimming -- ImageEnhance.Brightness(img).enhance(0.3):
 *
 *         out = (uint8_t)((double)v * 0.3)                          // truncates
 *
 *     Rounding instead mismatches on 128 of the 256 possible values.
 *     Use nd_image_brightness().
 *
 * ================== RECTANGLES ==================
 *
 * nd_rect is inclusive of both corners, because Pillow's is. See nd_types.h.
 */

#ifndef ND_IMAGE_H_INCLUDED
#define ND_IMAGE_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The surface
 * ------------------------------------------------------------------ */

typedef enum {
    ND_PIXFMT_RGB888 = 0, /* 3 bytes/px, R,G,B -- PIL "RGB"                 */
    ND_PIXFMT_RGBA8888,   /* 4 bytes/px, R,G,B,A -- PIL "RGBA"              */
    ND_PIXFMT_L8          /* 1 byte/px greyscale -- PIL "L", masks and LUTs */
} nd_pixfmt;

/* The struct is public because widget code reads img->w and img->h directly,
 * exactly as the Python reads img.width and img.height. Do not reach into
 * pixels from widget code -- use the functions below, which clip. */
typedef struct nd_image {
    int32_t w;
    int32_t h;
    nd_pixfmt fmt;
    uint8_t bpp;     /* bytes per pixel: 3, 4 or 1. Derived from fmt.     */
    size_t stride;   /* bytes per row. Rows are tightly packed (w * bpp)  */
                     /* for images we allocate, but a borrowed view may   */
                     /* have padding, so always step by stride.           */
    uint8_t *pixels; /* row-major, top row first                          */
    bool borrowed;   /* true == pixels belong to someone else; free()     */
                     /* releases only the header                          */
} nd_image;

static inline uint8_t nd_pixfmt_bpp(nd_pixfmt fmt) ND_UNUSED_FN;
static inline uint8_t nd_pixfmt_bpp(nd_pixfmt fmt)
{
    return fmt == ND_PIXFMT_RGBA8888 ? (uint8_t)4 : (fmt == ND_PIXFMT_L8 ? (uint8_t)1 : (uint8_t)3);
}

/* The measured Pillow compositing arithmetic. Inline because the text
 * renderer calls it once per covered pixel and it must not be a call. */
static inline uint8_t nd_blend8(uint8_t dst, uint8_t ink, uint8_t mask) ND_UNUSED_FN;
static inline uint8_t nd_blend8(uint8_t dst, uint8_t ink, uint8_t mask)
{
    /* +127 then a TRUNCATING divide by 255. See the header comment: this is
     * not MULDIV255, and the difference is visible on 23% of inputs. */
    return (uint8_t)(((uint32_t)dst * (255u - mask) + (uint32_t)ink * mask + 127u) / 255u);
}

/* ------------------------------------------------------------------ *
 * Lifecycle -- PIL Image.new / del
 * ------------------------------------------------------------------ */

/* A new surface, contents UNDEFINED. Owned by the caller; free with
 * nd_image_free(). NULL on allocation failure or a non-positive size.
 *
 * Write the arithmetic next to the call, per CODING-STANDARDS.md section 4:
 * a 240x175 RGB888 frame is 126,000 bytes. */
nd_image *nd_image_new(int32_t w, int32_t h, nd_pixfmt fmt);

/* As nd_image_new(), then filled with colour. This is PIL's
 * Image.new("RGB", (w,h), "black"). */
nd_image *nd_image_new_filled(int32_t w, int32_t h, nd_pixfmt fmt, nd_color colour);

/* A header describing memory the caller owns -- the framebuffer mmap, a
 * static buffer, a sub-rectangle of another image. Nothing is copied and
 * nd_image_free() will not release pixels. stride may exceed w * bpp. */
nd_image *nd_image_borrow(void *pixels, int32_t w, int32_t h, nd_pixfmt fmt, size_t stride);

/* PIL Image.copy(). Owned by the caller. NULL on failure. */
nd_image *nd_image_copy(const nd_image *src);

/* Safe on NULL. */
void nd_image_free(nd_image *img);

/* ------------------------------------------------------------------ *
 * Codecs -- PIL Image.open / Image.save
 * ------------------------------------------------------------------ */

/* Decode by sniffing the file's magic: PNG or JPEG. Returns an RGB888 or
 * RGBA8888 image, owned by the caller, NULL on any failure (a missing file is
 * a failure, not an error to report -- the Python swallows it too).
 *
 * Truncated JPEGs decode as far as they got, because the Python sets
 * ImageFile.LOAD_TRUNCATED_IMAGES globally at import and several shipped
 * assets rely on it. */
nd_image *nd_image_open(const char *path);

/* The individual decoders, for callers that know the format. PNG: 8-bit
 * RGB/RGBA, non-interlaced. bKGD, cHRM, tEXt, tIME and gAMA are IGNORED --
 * all 235 Koki costumes carry bKGD and most carry cHRM, and honouring any of
 * them changes the pixels. */
nd_image *nd_image_load_png(const char *path);
nd_image *nd_image_load_jpeg(const char *path);

/* A GIF decodes to its FIRST FRAME. Anything that wants the animation goes
 * through nd_gif.h instead; a picture is a picture, and every caller of
 * nd_image_open() -- icons, the image cache, Koki costumes -- wants one
 * surface it owns. */
nd_image *nd_image_load_gif(const char *path);

/* Decode from memory, for assets already read (update packages, Koki bundles).
 * The buffer is not retained. */
nd_image *nd_image_open_mem(const uint8_t *data, size_t len);

/* PIL Image.save(). Only PNG is written, and only by the capture tools --
 * nothing on the phone saves an image. */
nd_err nd_image_save_png(const nd_image *img, const char *path);

/* ------------------------------------------------------------------ *
 * Reading pixels out -- PIL Image.tobytes
 * ------------------------------------------------------------------ */

/* Copy the whole surface into out as tightly packed rows in its own format.
 * out_sz must be at least w * h * bpp. This is what the golden-frame hasher
 * consumes: manifest.json holds the SHA-256 of exactly these bytes. */
nd_err nd_image_tobytes(const nd_image *img, uint8_t *out, size_t out_sz);

/* Bounds-checked single-pixel access, for tests and for the handful of places
 * that genuinely plot one pixel. Out-of-range reads return transparent black;
 * out-of-range writes are dropped. */
nd_color nd_image_get_px(const nd_image *img, int32_t x, int32_t y);
void nd_image_set_px(nd_image *img, int32_t x, int32_t y, nd_color c);

/* ------------------------------------------------------------------ *
 * Compositing -- PIL Image.paste
 * ------------------------------------------------------------------ */

/* paste(src, (x,y)) -- a straight copy, alpha ignored. Clipped to dst. */
nd_err nd_image_blit(nd_image *dst, const nd_image *src, int32_t x, int32_t y);

/* paste(src, (x,y), src) -- composite using src's own alpha as the mask, with
 * the nd_blend8() arithmetic. src must be RGBA8888. Clipped to dst. */
nd_err nd_image_blit_alpha(nd_image *dst, const nd_image *src, int32_t x, int32_t y);

/* paste(src, (x,y), mask) -- composite through a separate L8 mask the same
 * size as src. This is the path ImageDraw.text() takes internally. */
nd_err nd_image_blit_mask(nd_image *dst, const nd_image *src, const nd_image *mask, int32_t x,
                          int32_t y);

/* Blit only the given source sub-rectangle. Saves the Python's
 * paste(img.crop(box), box) from allocating a temporary -- the softkey bar's
 * transparent mode does exactly that thirty times a second. */
nd_err nd_image_blit_region(nd_image *dst, const nd_image *src, nd_rect src_rect, int32_t x,
                            int32_t y);

/* paste(colour, box) -- fill the inclusive rectangle. Clipped. */
nd_err nd_image_fill_rect(nd_image *img, nd_rect rect, nd_color colour);

/* The whole surface. */
nd_err nd_image_fill(nd_image *img, nd_color colour);

/* ------------------------------------------------------------------ *
 * Geometry -- crop, resize, thumbnail, transpose
 * ------------------------------------------------------------------ */

/* PIL Image.crop(box). The box is INCLUSIVE (nd_rect), unlike PIL's own
 * half-open tuple -- convert at the call site and say so in a comment, because
 * this is the single easiest place in the port to lose a pixel.
 * Owned by the caller. */
nd_image *nd_image_crop(const nd_image *src, nd_rect box);

/* PIL's crop() semantics for a box that runs off the edge: the outside is
 * filled with zero rather than clipped, so the result is always the requested
 * size. Koki's mask-overlap test depends on the zero padding. */
nd_image *nd_image_crop_zeropad(const nd_image *src, nd_rect box);

/* PIL Image.resize(..., LANCZOS). A port of Pillow's resample.c: 3 lobes,
 * support 3.0, horizontal pass then vertical, the same fixed-point coefficient
 * rounding. "Close enough" is not close enough -- every app icon and the
 * wallpaper go through this, and a different filter shifts every antialiased
 * edge. Owned by the caller. */
nd_image *nd_image_resize_lanczos(const nd_image *src, int32_t w, int32_t h);

/* PIL Image.resize(..., NEAREST), with the verified sampling rule
 *   src_x = min(src_w - 1, (int)((dst_x + 0.5) * src_w / dst_w))
 * Koki scales every costume with this. Owned by the caller. */
nd_image *nd_image_resize_nearest(const nd_image *src, int32_t w, int32_t h);

/* PIL Image.thumbnail((max_w, max_h), LANCZOS): aspect-preserving, in place,
 * and NEVER upscales -- an image already inside the box is left untouched.
 * Reallocates img->pixels when it does shrink. */
nd_err nd_image_thumbnail(nd_image *img, int32_t max_w, int32_t max_h);

/* PIL Image.transpose(FLIP_LEFT_RIGHT) -- row reversal, in place.
 * Koki's only transpose; there is no vertical flip anywhere in the project. */
nd_err nd_image_flip_h(nd_image *img);

/* PIL Image.convert("RGB"/"RGBA"/"L"). Returns a new image even when the
 * format already matches, so the caller's ownership rule stays simple.
 * RGB -> RGBA sets alpha 255. RGBA -> RGB DROPS alpha without compositing,
 * which is what PIL does. */
nd_image *nd_image_convert(const nd_image *src, nd_pixfmt fmt);

/* ------------------------------------------------------------------ *
 * The two Koki oddities
 * ------------------------------------------------------------------ */

/* PIL Image.point(lut) -- map every sample of every listed channel through a
 * 256-entry table, in place. Pass NULL for a channel to leave it alone.
 * Koki uses this three ways: brightness (identity on alpha), the ghost effect
 * (identity on RGB, table on alpha) and an alpha threshold (v > 40 ? 255 : 0). */
nd_err nd_image_point_lut(nd_image *img, const uint8_t lut_r[256], const uint8_t lut_g[256],
                          const uint8_t lut_b[256], const uint8_t lut_a[256]);

/* ImageChops.multiply(a, b).getbbox() != None, which is the only thing Koki
 * ever does with a multiply: "do these two masks overlap anywhere?"
 * On 0/255 inputs (a*b)/255 is 0 or 255, so this is just
 * "is there an index where both are non-zero" -- computed directly, with no
 * intermediate image and no allocation. */
bool nd_image_masks_overlap(const nd_image *a, const nd_image *b);

/* PIL Image.getbbox() on an RGBA image. A pixel counts as empty only when ALL
 * FOUR channels are zero -- that is Pillow's rule, and it is why the battery
 * sprite's "?" label sits where it does. Returns false when the image is
 * entirely empty, in which case *out is untouched. The rectangle is inclusive. */
bool nd_image_alpha_bbox(const nd_image *img, nd_rect *out);

/* ImageEnhance.Brightness(img).enhance(f). Truncating, per the header comment:
 * out = (uint8_t)((double)v * f). Alpha is left alone. In place. */
nd_err nd_image_brightness(nd_image *img, double factor);

/* ------------------------------------------------------------------ *
 * The image cache -- NeoDCT_UI.get_image's 32-entry FIFO
 * ------------------------------------------------------------------ */

/* Entry-counted, not byte-budgeted, to match the Python's eviction order
 * exactly. CODING-STANDARDS.md section 4 wants byte budgets and that is
 * recorded in OPEN-QUESTIONS.md; until it is answered, keep the FIFO. */
#define ND_IMGCACHE_MAX 32

typedef struct nd_imgcache nd_imgcache;

nd_imgcache *nd_imgcache_new(size_t max_entries);
void nd_imgcache_free(nd_imgcache *c);

/* Look up, decoding on a miss. Exactly one of max_size and scale may be
 * non-zero; pass 0 and 0.0 for the plain form.
 *
 *   max_size > 0  -- thumbnail to fit max_size x max_size, never upscaling
 *   scale  > 0.0  -- resize to (max(1, w*scale), max(1, h*scale)), LANCZOS
 *
 * The returned image is OWNED BY THE CACHE. Do not free it, and do not hold it
 * across another call that might evict it -- blit from it and let it go.
 * NULL on any failure, exactly as the Python returns None. */
const nd_image *nd_imgcache_get(nd_imgcache *c, const char *path, int32_t max_size, double scale);

void nd_imgcache_clear(nd_imgcache *c);

#ifdef __cplusplus
}
#endif

#endif /* ND_IMAGE_H_INCLUDED */
