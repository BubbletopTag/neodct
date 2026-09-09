/* uifont_test.h -- which typeface a rendering fixture must open.
 *
 * ============ WHY THIS EXISTS ============
 *
 * The phone ships two faces (nd_paths.h): font.ttf, the pixel face that
 * fontref.json pins by SHA-256 and that the initramfs boot bar's glyph tables
 * are baked from; and aero.ttf, what the UI actually draws with.
 *
 * A fixture that renders a SCREEN has to open the second, or its frames will
 * not match the reference set and the mismatch will look like a layout bug
 * rather than a font one -- which is exactly what happened when the theme
 * landed: fifteen fixtures each carried their own copy of a resolver with
 * "font.ttf" spelled into it, every one of them kept rendering in the pixel
 * face, and eng-cubebench came back "8.14% of pixels differ" with a picture of
 * a perfectly correct cube.
 *
 * A fixture that tests the RENDERER -- test_font, test_draw, test_bootbar,
 * test_keypadsetup -- must keep opening font.ttf, because the thing it is
 * checking is pinned against that file. Those four do not include this header
 * and must not.
 *
 * ============ THE BOLD PAIR ============
 *
 * nd_ui carries font_n_b and font_xl_b, and nd_ui_font_bold() falls back to
 * the regular weight when either is NULL. A fixture that leaves them NULL
 * therefore renders correctly-laid-out screens in the wrong weight -- no
 * crash, no warning, and every title one stroke too light. So loading them is
 * not optional for a fixture that compares against a reference frame, and
 * ui_bold_face_path() is here to make it one line.
 */

#ifndef ND_UIFONT_TEST_H_INCLUDED
#define ND_UIFONT_TEST_H_INCLUDED

#include <stdio.h>
#include <string.h>

#include "nd_types.h"

/* Relative to the repo's neodct/ directory, which is what every fixture's own
 * resolver searches from. */
#define ND_TEST_UI_FONT_REL      "overlay/NeoDCT/System/ui/resources/fonts/aero.ttf"
#define ND_TEST_UI_FONT_ABS      "/NeoDCT/System/ui/resources/fonts/aero.ttf"
#define ND_TEST_UI_FONT_BOLD_REL "overlay/NeoDCT/System/ui/resources/fonts/aero-bold.ttf"

/* The bold companion of an already-resolved regular face: the same directory,
 * with "-bold" before the extension. Derived rather than searched for so that
 * a fixture pointed at a face by $NEODCT_FONT gets that face's bold cut and
 * not the tree's.
 *
 * False when there is no such file, which every caller treats as "no bold" --
 * nd_ui_font_bold() then answers the regular weight and the screen is a shade
 * lighter, which is the same graceful degradation a phone missing the file
 * gets. */
static bool ui_bold_face_path(const char *regular, char *out, size_t out_sz) ND_UNUSED_FN;
static bool ui_bold_face_path(const char *regular, char *out, size_t out_sz)
{
    const char *dot;
    size_t stem;
    FILE *f;

    if (regular == NULL || regular[0] == '\0' || out == NULL || out_sz == 0u)
        return false;

    dot = strrchr(regular, '.');
    /* A dot in a parent directory is not an extension. Without this check
     * "/opt/my.fonts/aero" would become "/opt/my-bold.fonts/aero". */
    if (dot == NULL || strchr(dot, '/') != NULL)
        stem = strlen(regular);
    else
        stem = (size_t)(dot - regular);

    if (nd_snprintf(out, out_sz, "%.*s-bold%s", (int)stem, regular,
                    (dot != NULL && strchr(dot, '/') == NULL) ? dot : "") != ND_OK)
        return false;

    f = fopen(out, "rb");
    if (f == NULL) {
        out[0] = '\0';
        return false;
    }
    (void)fclose(f);
    return true;
}

/* ============ THE REFERENCE WALLPAPER ============
 *
 * nd-shoot renders every wallpapered group against the phone's shipped
 * default (ND_SHOOT_WALLPAPER, which is ND_SET_UI_WALLPAPER_DFLT). A fixture
 * that sets a different one renders a correct screen over the wrong
 * photograph and fails the frame comparison for a reason the diff will not
 * make obvious -- the pixels differ everywhere and nothing is wrong with the
 * widget.
 *
 * Named here so the fixtures and nd-shoot cannot drift. If the shipped
 * default changes, this is the second of the two places to edit and the
 * frames have to be re-cut anyway. */
#define ND_TEST_REF_WALLPAPER      "Fruitiger Aero.jpg"
#define ND_TEST_REF_WALLPAPER_PATH "/NeoDCT/System/wallpapers/" ND_TEST_REF_WALLPAPER

#endif /* ND_UIFONT_TEST_H_INCLUDED */
