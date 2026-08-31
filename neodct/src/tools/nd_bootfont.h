/* nd_bootfont.h -- the shape of the glyph tables in the generated
 * nd_bootfont.c, and nothing else.
 *
 * The initramfs has no FreeType and no font.ttf: putting them there would
 * cost roughly a megabyte of RAM on a 64 MB device, unpacked from a cpio that
 * on the Luckfox is built into the kernel image. So the real typeface is
 * rendered at build time by tools/gen_bootfont.c -- which links libneodct and
 * therefore uses the phone's own nd_font -- thresholded to one bit, and
 * committed as C.
 *
 * The point of generating rather than hand-drawing is the metrics: advance,
 * ink_dx and ink_dy come from the same FreeType faces the UI measures with,
 * so a string centred on the boot screen lands on the same pixel column it
 * would land on in the Update app. The glyphs are 1-bit where the OS's are
 * antialiased, so the two screens are not pixel-identical. The layout is.
 */

#ifndef ND_BOOTFONT_H_INCLUDED
#define ND_BOOTFONT_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Printable ASCII. Boot strings are ours and there are eleven of them. */
#define ND_BOOTFONT_FIRST 0x20
#define ND_BOOTFONT_LAST  0x7E
#define ND_BOOTFONT_COUNT (ND_BOOTFONT_LAST - ND_BOOTFONT_FIRST + 1)

typedef struct {
    uint8_t advance; /* whole pixels; nd_font.h fact 2 -- never fractional */
    uint8_t ink_w;
    uint8_t ink_h;
    int8_t ink_dx; /* pen x + ink_dx == left edge of the ink   */
    int8_t ink_dy; /* ascender + ink_dy == top of the ink      */
    /* Byte offset into `bits` of this glyph's rows. Each row is
     * (ink_w + 7) / 8 bytes, MSB first, left to right; ink_h rows. */
    uint32_t offset;
} nd_bootglyph;

typedef struct {
    uint8_t px;
    uint8_t ascent;
    const nd_bootglyph *glyphs; /* ND_BOOTFONT_COUNT entries */
    const uint8_t *bits;
    uint32_t bits_len;
} nd_bootfont;

/* ND_FONT_PX_S, ND_FONT_PX_MD and ND_FONT_PX_N -- the whole ladder
 * nd_font_ladder() offers, in the order nd_fit_font() tries it. A label that
 * does not fit at 20 px drops to 18 and then to 14 on the boot screen for the
 * same reason and by the same arithmetic as it does in the Update app, so
 * "Checking the update" is one size on both. Defined in the generated
 * nd_bootfont.c. */
extern const nd_bootfont nd_bootfont_14;
extern const nd_bootfont nd_bootfont_18;
extern const nd_bootfont nd_bootfont_20;

#ifdef __cplusplus
}
#endif

#endif /* ND_BOOTFONT_H_INCLUDED */
