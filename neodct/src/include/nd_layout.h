/* nd_layout.h -- ui_home.json, parsed into a typed array.
 *
 * The home screen is data, not code: /NeoDCT/System/ui/resources/ui_home.json
 * lists elements and the core draws them. There are two kinds, "text" and
 * "icon_set", and four elements in the shipped file: the battery sprite, the
 * carrier name, the clock, and the signal sprite -- IN THAT ORDER, which is
 * also paint order.
 *
 * ============ COORDINATES ARE SCALED, AND ONLY VERTICALLY ============
 *
 *     x = (int)((el.x / 240.0) * 240)   -- i.e. unchanged
 *     y = (int)((el.y / 240.0) * 175)   -- i.e. floor(el.y * 175 / 240)
 *
 * So y 24 becomes 17, y 71 becomes 51, y 12 becomes 8. The layout was authored
 * against a square 240x240 panel and the band is 175 tall; the divide is what
 * squashes it. Port the formula, not the resolved numbers.
 *
 * ============ TWO PLACEHOLDER STRINGS ARE SUBSTITUTED ============
 *
 * Matched literally, not by any marker syntax:
 *   "12:00"      -> strftime("%H:%M"), 24-hour, local time
 *   "No Service" -> the modem's operator name IF IT IS NON-EMPTY; otherwise
 *                   the placeholder stays and really does read "No Service"
 *
 * ============ AND THE STATUS SPRITES OVERHANG ============
 *
 * Both sprites are 26x131 at y 17, so they reach y 148 -- three rows past
 * content_bottom (145). The softkey bar is drawn afterwards and covers them.
 * Reproduce the overlap; it is why the draw order in the file matters.
 */

#ifndef ND_LAYOUT_H_INCLUDED
#define ND_LAYOUT_H_INCLUDED

#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ND_EL_TEXT = 0, ND_EL_ICON_SET } nd_element_type;

/* "center_h" and "right" are tested as SUBSTRINGS of the anchor string, not
 * compared for equality, so "center_h_top" behaves as centred. Reproduce the
 * substring test. */
typedef enum { ND_ANCHOR_LEFT = 0, ND_ANCHOR_CENTER_H, ND_ANCHOR_RIGHT } nd_anchor;

#define ND_LAYOUT_TEXT_MAX    64
#define ND_LAYOUT_PREFIX_MAX  16
#define ND_LAYOUT_ICON_STATES 8 /* count is 5 in the shipped file */

/* Path length inside an element. Kept separate from ND_PATH_MAX so this
 * struct stays small -- there can be eight of them per icon set. */
#define ND_LAYOUT_PATH_MAX 160

typedef struct {
    nd_element_type type;
    int32_t x; /* as authored, before the scale above */
    int32_t y;

    /* ND_EL_TEXT */
    char text[ND_LAYOUT_TEXT_MAX];
    int32_t font_size; /* >= 20 -> font_xl, >= 16 -> font_n, else font_s */
    nd_anchor anchor;
    nd_color color;

    /* ND_EL_ICON_SET */
    int32_t count;
    char prefix[ND_LAYOUT_PREFIX_MAX]; /* "bat" or "sig" drive real values */
    int32_t sim_val;                   /* used when the real value is unknown; default 3 */
    char custom_images[ND_LAYOUT_ICON_STATES][ND_LAYOUT_PATH_MAX];
    bool has_custom[ND_LAYOUT_ICON_STATES];
} nd_element;

#define ND_LAYOUT_MAX_ELEMENTS 32

typedef struct {
    nd_image *background; /* NULL in the shipped layout; owned by the layout */
    nd_element elements[ND_LAYOUT_MAX_ELEMENTS];
    size_t n_elements;
} nd_home_layout;

/* Parse the file. NULL on ANY failure, including a missing file -- the core
 * treats a NULL layout as "draw nothing but the wallpaper", exactly as the
 * Python treats None. Owned by the caller. */
nd_home_layout *nd_layout_load(const char *path);
void nd_layout_free(nd_home_layout *l);

/* Resolve the authored coordinate to a screen coordinate. */
int32_t nd_layout_scale_x(int32_t x, int32_t ui_w);
int32_t nd_layout_scale_y(int32_t y, int32_t ui_h);

/* Draw one element. Declared here rather than in nd_ui.h because the Dialer
 * screens reach for it directly to reuse the clock and status icons at their
 * exact home placement. */
struct nd_ui;
void nd_home_render_element(struct nd_ui *ui, const nd_element *el);

#ifdef __cplusplus
}
#endif

#endif /* ND_LAYOUT_H_INCLUDED */
