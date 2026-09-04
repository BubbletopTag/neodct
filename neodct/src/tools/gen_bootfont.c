/* gen_bootfont.c -- render the real typeface into the committed 1-bit tables
 * the initramfs boot bar draws with.
 *
 * A build-host program, never installed and never run on the phone. It links
 * libneodct so it renders through the SAME nd_font/FreeType path the UI uses;
 * that is the whole point. If the boot screen carried its own hand-drawn 8x8
 * font, the label centred on it would sit a few pixels off from where the
 * Update app puts the same string, and nobody would ever notice why.
 *
 *     ./build/default/bin/gen-bootfont FONT.TTF > tools/nd_bootfont.c
 *
 * The output is committed. neodct/tests/test_bootfont_generated.py re-runs
 * this and compares, the same way neodct/tests/golden/font/fontref.json pins
 * the font renderer -- so a regenerated table that differs from the committed
 * one is a test failure and not a silent drift.
 *
 * Coverage is thresholded at 128. At 14 and 20 px on this face that is close
 * to the outline: the typeface is a pixel font (Nokia Cellphone FC), so most
 * of its coverage is already 0 or 255 and only the diagonals have anything in
 * between.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_bootfont.h"
#include "nd_font.h"
#include "nd_types.h"

/* Enough for every printable ASCII glyph at 20 px with room to spare:
 * (20 + 7) / 8 = 3 bytes a row, at most ~24 rows. */
#define MAX_GLYPH_BYTES 256u

typedef struct {
    nd_bootglyph meta;
    uint8_t bits[MAX_GLYPH_BYTES];
    uint32_t len;
} built;

static bool build_size(const char *path, int32_t px, built *out, uint32_t *total)
{
    nd_font *font = nd_font_load(path, px);
    int32_t cp;

    if (font == NULL) {
        fprintf(stderr, "gen-bootfont: cannot load %s at %d px\n", path, (int)px);
        return false;
    }

    *total = 0u;
    for (cp = ND_BOOTFONT_FIRST; cp <= ND_BOOTFONT_LAST; cp++) {
        built *b = &out[cp - ND_BOOTFONT_FIRST];
        const nd_glyph *g = nd_font_glyph(font, (uint32_t)cp);
        uint32_t row_bytes;
        int32_t y;

        memset(b, 0, sizeof *b);
        if (g == NULL) {
            fprintf(stderr, "gen-bootfont: no glyph for 0x%02x at %d px\n", (unsigned)cp, (int)px);
            nd_font_free(font);
            return false;
        }

        /* Every one of these has to fit the generated struct's widths, and a
         * face that outgrew them would otherwise be truncated in silence. */
        if (g->advance < 0 || g->advance > 255 || g->ink_w < 0 || g->ink_w > 255 || g->ink_h < 0 ||
            g->ink_h > 255 || g->ink_dx < -128 || g->ink_dx > 127 || g->ink_dy < -128 ||
            g->ink_dy > 127) {
            fprintf(stderr, "gen-bootfont: 0x%02x at %d px does not fit nd_bootglyph\n",
                    (unsigned)cp, (int)px);
            nd_font_free(font);
            return false;
        }

        b->meta.advance = (uint8_t)g->advance;
        b->meta.ink_dx = (int8_t)g->ink_dx;
        b->meta.ink_dy = (int8_t)g->ink_dy;

        if (g->coverage == NULL || g->ink_w <= 0 || g->ink_h <= 0)
            continue; /* space and friends: an advance and no ink */

        b->meta.ink_w = (uint8_t)g->ink_w;
        b->meta.ink_h = (uint8_t)g->ink_h;
        row_bytes = ((uint32_t)g->ink_w + 7u) / 8u;
        b->len = row_bytes * (uint32_t)g->ink_h;
        if (b->len > MAX_GLYPH_BYTES) {
            fprintf(stderr, "gen-bootfont: 0x%02x at %d px needs %u bytes\n", (unsigned)cp, (int)px,
                    (unsigned)b->len);
            nd_font_free(font);
            return false;
        }

        for (y = 0; y < g->ink_h; y++) {
            const uint8_t *cov = g->coverage + (size_t)y * (size_t)g->ink_w;
            int32_t x;

            for (x = 0; x < g->ink_w; x++) {
                if (cov[x] < 128u)
                    continue;
                b->bits[(uint32_t)y * row_bytes + (uint32_t)(x / 8)] |= (uint8_t)(0x80u >> (x % 8));
            }
        }
        *total += b->len;
    }

    nd_font_free(font);
    return true;
}

static void emit(const char *name, int32_t px, int32_t ascent, built *g)
{
    uint32_t offset = 0u;
    uint32_t column = 0u;
    int32_t i;

    printf("static const uint8_t bits_%s[] = {", name);
    for (i = 0; i < ND_BOOTFONT_COUNT; i++) {
        uint32_t k;

        for (k = 0u; k < g[i].len; k++) {
            if (column % 12u == 0u)
                printf("\n    ");
            printf("0x%02x,%s", g[i].bits[k], (column % 12u == 11u) ? "" : " ");
            column++;
        }
    }
    if (column == 0u)
        printf("\n    0x00,");
    printf("\n};\n\n");

    printf("static const nd_bootglyph glyphs_%s[ND_BOOTFONT_COUNT] = {\n", name);
    for (i = 0; i < ND_BOOTFONT_COUNT; i++) {
        int32_t cp = ND_BOOTFONT_FIRST + i;

        printf("    {%3u, %2u, %2u, %3d, %3d, %5u},  /* %s%c */\n", (unsigned)g[i].meta.advance,
               (unsigned)g[i].meta.ink_w, (unsigned)g[i].meta.ink_h, (int)g[i].meta.ink_dx,
               (int)g[i].meta.ink_dy, (unsigned)offset, (cp == '\\' || cp == '\'') ? "\\" : "",
               (char)cp);
        offset += g[i].len;
    }
    printf("};\n\n");

    printf("const nd_bootfont nd_bootfont_%d = {\n", (int)px);
    printf("    %d, %d, glyphs_%s, bits_%s, (uint32_t)sizeof bits_%s,\n", (int)px, (int)ascent,
           name, name, name);
    printf("};\n");
}

int main(int argc, char **argv)
{
    static built small[ND_BOOTFONT_COUNT];
    static built mid[ND_BOOTFONT_COUNT];
    static built step[ND_BOOTFONT_COUNT];
    uint32_t small_bytes = 0u;
    uint32_t mid_bytes = 0u;
    uint32_t step_bytes = 0u;
    int32_t small_ascent = 0;
    int32_t mid_ascent = 0;
    int32_t step_ascent = 0;
    nd_font *probe;

    if (argc != 2) {
        fprintf(stderr, "usage: gen-bootfont FONT.TTF > nd_bootfont.c\n");
        return 2;
    }

    if (!build_size(argv[1], ND_FONT_PX_S, small, &small_bytes))
        return 1;
    if (!build_size(argv[1], ND_FONT_PX_MD, mid, &mid_bytes))
        return 1;
    if (!build_size(argv[1], ND_FONT_PX_N, step, &step_bytes))
        return 1;

    probe = nd_font_load(argv[1], ND_FONT_PX_S);
    if (probe == NULL)
        return 1;
    nd_font_metrics(probe, &small_ascent, NULL);
    nd_font_free(probe);

    probe = nd_font_load(argv[1], ND_FONT_PX_MD);
    if (probe == NULL)
        return 1;
    nd_font_metrics(probe, &mid_ascent, NULL);
    nd_font_free(probe);

    probe = nd_font_load(argv[1], ND_FONT_PX_N);
    if (probe == NULL)
        return 1;
    nd_font_metrics(probe, &step_ascent, NULL);
    nd_font_free(probe);

    printf("/* GENERATED by tools/gen_bootfont.c from\n"
           " * neodct/overlay/NeoDCT/System/ui/resources/fonts/font.ttf.\n"
           " * DO NOT EDIT. Regenerate with:\n"
           " *\n"
           " *     ./build/default/bin/gen-bootfont \\\n"
           " *         ../overlay/NeoDCT/System/ui/resources/fonts/font.ttf \\\n"
           " *         > tools/nd_bootfont.c\n"
           " *\n"
           " * neodct/tests/test_bootfont_generated.py re-runs that and compares.\n"
           " * Printable ASCII at %d, %d and %d px -- nd_font_ladder()'s whole\n"
           " * ladder -- with FreeType coverage thresholded at 128;\n"
           " * %u + %u + %u bytes of glyph bits.\n"
           " */\n\n",
           (int)ND_FONT_PX_S, (int)ND_FONT_PX_MD, (int)ND_FONT_PX_N, (unsigned)small_bytes,
           (unsigned)mid_bytes, (unsigned)step_bytes);
    printf("#include \"nd_bootfont.h\"\n\n");

    emit("14", ND_FONT_PX_S, small_ascent, small);
    printf("\n");
    emit("18", ND_FONT_PX_MD, mid_ascent, mid);
    printf("\n");
    emit("20", ND_FONT_PX_N, step_ascent, step);

    return 0;
}
