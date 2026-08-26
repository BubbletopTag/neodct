/* nd_layout.c -- ui_home.json, and the one function that draws an element of it.
 *
 * Ported from System/core/main.py:677 (`load_layout`) and :765
 * (`render_element`). The home screen is data: four elements in one JSON file,
 * drawn in array order, and that order is paint order.
 *
 * ============ THE COORDINATE SCALE IS VERTICAL ONLY ============
 *
 *     x = (int)((el.x / 240.0) * W)     W is 240, so x never moves
 *     y = (int)((el.y / 240.0) * H)     H is 175, so y = floor(el.y * 175/240)
 *
 * The layout was authored against a square 240x240 panel. Both divides are
 * written out rather than folded into a constant, because the Python does
 * them and because a future panel changes only H.
 *
 * ============ THE SPRITES OVERHANG THE CONTENT AREA ============
 *
 * Both status sprites are 26x131 pasted at y 17, so they reach y 148 -- three
 * rows past content_bottom (145). The softkey bar is drawn afterwards and
 * covers them. That overlap is reproduced, not corrected; it is why the file's
 * element order matters.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_json.h"
#include "nd_layout.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"

/* The two placeholder strings render_element() matches LITERALLY. There is no
 * marker syntax; these exact strings are the whole mechanism. */
#define LAYOUT_CLOCK_PLACEHOLDER   "12:00"
#define LAYOUT_CARRIER_PLACEHOLDER "No Service"

/* ------------------------------------------------------------------ *
 * Colours
 * ------------------------------------------------------------------ */

/* Pillow's colour strings, restricted to what this project actually writes.
 * The shipped layout uses only "white"; "#333333" comes from the drawn-bars
 * fallback in render_element and is spelled in C there, not here. */
static bool hexdigit(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

static bool parse_hex_colour(const char *s, nd_color *out)
{
    uint8_t d[6];
    size_t len;
    size_t i;

    len = strlen(s);
    if (len == 7u) {
        for (i = 0u; i < 6u; i++) {
            if (!hexdigit(s[i + 1u], &d[i]))
                return false;
        }
        *out = ND_RGB((d[0] << 4) | d[1], (d[2] << 4) | d[3], (d[4] << 4) | d[5]);
        return true;
    }
    if (len == 4u) {
        for (i = 0u; i < 3u; i++) {
            if (!hexdigit(s[i + 1u], &d[i]))
                return false;
        }
        *out = ND_RGB((d[0] << 4) | d[0], (d[1] << 4) | d[1], (d[2] << 4) | d[2]);
        return true;
    }
    return false;
}

static nd_color layout_colour(const char *s)
{
    static const struct {
        const char *name;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    } named[] = {
        {"white", 255u, 255u, 255u}, {"black", 0u, 0u, 0u},      {"red", 255u, 0u, 0u},
        {"green", 0u, 128u, 0u},     {"blue", 0u, 0u, 255u},     {"yellow", 255u, 255u, 0u},
        {"gray", 128u, 128u, 128u},  {"grey", 128u, 128u, 128u},
    };
    size_t i;
    nd_color c;

    if (s == NULL || s[0] == '\0')
        return ND_WHITE;
    if (s[0] == '#' && parse_hex_colour(s, &c))
        return c;
    for (i = 0u; i < ND_ARRAY_LEN(named); i++) {
        if (strcmp(s, named[i].name) == 0)
            return ND_RGB(named[i].r, named[i].g, named[i].b);
    }
    return ND_WHITE;
}

/* ------------------------------------------------------------------ *
 * Loading
 * ------------------------------------------------------------------ */

int32_t nd_layout_scale_x(int32_t x, int32_t ui_w)
{
    return nd_trunc32(((double)x / 240.0) * (double)ui_w);
}

int32_t nd_layout_scale_y(int32_t y, int32_t ui_h)
{
    return nd_trunc32(((double)y / 240.0) * (double)ui_h);
}

/* "center_h" and "right" are SUBSTRING tests in the Python, so "center_h_top"
 * behaves as centred. Reproduce the substring test, and its precedence. */
static nd_anchor parse_anchor(const char *s)
{
    if (s == NULL)
        return ND_ANCHOR_LEFT;
    if (strstr(s, "center_h") != NULL)
        return ND_ANCHOR_CENTER_H;
    if (strstr(s, "right") != NULL)
        return ND_ANCHOR_RIGHT;
    return ND_ANCHOR_LEFT;
}

static bool load_text_element(const nd_json_val *o, nd_element *el)
{
    const char *s;

    el->type = ND_EL_TEXT;
    s = nd_json_get_str(o, "text", "");
    (void)nd_strlcpy(el->text, s, sizeof el->text);
    el->font_size = (int32_t)nd_json_get_int(o, "font_size", 0);
    el->anchor = parse_anchor(nd_json_get_str(o, "anchor", ""));
    el->color = layout_colour(nd_json_get_str(o, "color", "white"));
    return true;
}

static bool load_icon_set_element(const nd_json_val *o, nd_element *el)
{
    const nd_json_val *custom;
    int32_t i;

    el->type = ND_EL_ICON_SET;
    /* el.get("count", 5) and int(el.get("sim_val", 3)) -- the shipped file
     * carries 5 and 4, but the defaults are the Python's. */
    el->count = (int32_t)nd_json_get_int(o, "count", 5);
    el->sim_val = (int32_t)nd_json_get_int(o, "sim_val", 3);
    (void)nd_strlcpy(el->prefix, nd_json_get_str(o, "prefix", ""), sizeof el->prefix);

    custom = nd_json_get(o, "custom_images");
    if (custom == NULL || nd_json_type_of(custom) != ND_JSON_OBJECT)
        return true;

    for (i = 0; i < (int32_t)ND_LAYOUT_ICON_STATES; i++) {
        char name[4];
        const char *path;

        if (nd_snprintf(name, sizeof name, "%d", i) != ND_OK)
            continue;
        path = nd_json_get_str(custom, name, NULL);
        if (path == NULL || path[0] == '\0')
            continue;
        (void)nd_strlcpy(el->custom_images[i], path, ND_LAYOUT_PATH_MAX);
        el->has_custom[i] = true;
    }
    return true;
}

/* The Python builds `self._home_bg` lazily on the first render:
 * get_image(bg).convert("RGB").resize((W,H), LANCZOS), cached on the instance.
 * nd_home_layout.background is an nd_image, so the work happens here instead.
 * The only difference is a background file that appears AFTER boot, which the
 * Python would pick up on a later frame; the shipped layout has
 * "background": null, so the branch is dormant either way. Recorded in
 * OPEN-QUESTIONS.md. */
static nd_image *load_background(const char *path)
{
    nd_image *raw;
    nd_image *rgba;
    nd_image *rgb;
    nd_image *scaled;

    raw = nd_image_open(path);
    if (raw == NULL)
        return NULL;
    rgba = nd_image_convert(raw, ND_PIXFMT_RGBA8888); /* get_image()'s convert */
    nd_image_free(raw);
    if (rgba == NULL)
        return NULL;
    rgb = nd_image_convert(rgba, ND_PIXFMT_RGB888);
    nd_image_free(rgba);
    if (rgb == NULL)
        return NULL;
    if (rgb->w == ND_UI_W && rgb->h == ND_UI_H)
        return rgb;
    scaled = nd_image_resize_lanczos(rgb, ND_UI_W, ND_UI_H);
    nd_image_free(rgb);
    return scaled;
}

/* owned by the caller; free with nd_layout_free() */
nd_home_layout *nd_layout_load(const char *path)
{
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    const nd_json_val *elements;
    const nd_json_val *bg;
    nd_home_layout *l = NULL;
    size_t n;
    size_t i;

    if (path == NULL)
        return NULL;
    /* `except: return None` -- a missing or malformed file is not an error to
     * report, it is a home screen with no elements. */
    if (nd_json_parse_file(path, &doc, NULL, 0u) != ND_OK)
        return NULL;

    root = nd_json_root(doc);
    if (root == NULL || nd_json_type_of(root) != ND_JSON_OBJECT)
        goto fail;

    l = calloc(1u, sizeof *l);
    if (l == NULL)
        goto fail;

    bg = nd_json_get(root, "background");
    if (bg != NULL && nd_json_type_of(bg) == ND_JSON_STRING) {
        const char *bg_path = NULL;

        if (nd_json_str(bg, &bg_path) && bg_path != NULL && bg_path[0] != '\0')
            l->background = load_background(bg_path);
    }

    elements = nd_json_get(root, "elements");
    if (elements == NULL || nd_json_type_of(elements) != ND_JSON_ARRAY) {
        nd_json_free(doc);
        return l;
    }

    n = nd_json_len(elements);
    for (i = 0u; i < n && l->n_elements < ND_LAYOUT_MAX_ELEMENTS; i++) {
        const nd_json_val *o = nd_json_at(elements, i);
        nd_element *el = &l->elements[l->n_elements];
        const char *type;

        if (o == NULL || nd_json_type_of(o) != ND_JSON_OBJECT)
            continue;
        type = nd_json_get_str(o, "type", "");

        memset(el, 0, sizeof *el);
        el->x = (int32_t)nd_json_get_int(o, "x", 0);
        el->y = (int32_t)nd_json_get_int(o, "y", 0);

        if (strcmp(type, "text") == 0) {
            if (!load_text_element(o, el))
                continue;
        } else if (strcmp(type, "icon_set") == 0) {
            if (!load_icon_set_element(o, el))
                continue;
        } else {
            /* render_element() handles exactly two types and silently ignores
             * anything else, so an unknown element is dropped at load. */
            continue;
        }
        l->n_elements++;
    }

    nd_json_free(doc);
    return l;

fail:
    nd_json_free(doc);
    free(l);
    return NULL;
}

void nd_layout_free(nd_home_layout *l)
{
    if (l == NULL)
        return;
    nd_image_free(l->background);
    free(l);
}

/* ------------------------------------------------------------------ *
 * Drawing one element
 * ------------------------------------------------------------------ */

static const nd_font *font_for_size(const nd_ui *ui, int32_t font_size)
{
    if (font_size >= 20)
        return ui->font_xl;
    if (font_size >= 16)
        return ui->font_n;
    return ui->font_s;
}

/* _draw_status_label: a small value just LEFT of the icon's opaque region and
 * vertically centred on it. vis_box arrives in Pillow's tuple shape -- right
 * and bottom EXCLUSIVE -- because that is what getbbox() returns and what the
 * arithmetic below was measured against. */
static void draw_status_label(nd_ui *ui, const char *text, int32_t icon_x, int32_t icon_y,
                              int32_t left, int32_t top, int32_t bottom)
{
    int32_t tw = 0;
    int32_t th = 0;
    int32_t tx;
    int32_t ty;

    if (ui->font_s == NULL)
        return;
    nd_ui_text_size(ui, text, ui->font_s, &tw, &th);
    tx = nd_max32(0, icon_x + left - tw - 4);
    ty = icon_y + top + nd_max32(0, ((bottom - top) - th) / 2);
    (void)nd_draw_text(ui->draw, tx, ty, text, ui->font_s, ND_WHITE);
}

static void render_text_element(nd_ui *ui, const nd_element *el)
{
    char buf[ND_LAYOUT_TEXT_MAX];
    const char *text = el->text;
    const nd_font *f;
    int32_t w = 0;
    int32_t h = 0;
    int32_t x;
    int32_t y;

    if (strcmp(text, LAYOUT_CLOCK_PLACEHOLDER) == 0) {
        struct tm tmv;
        nd_time_localtime(nd_time_now(), &tmv);
        if (strftime(buf, sizeof buf, "%H:%M", &tmv) == 0u)
            buf[0] = '\0';
        text = buf;
    } else if (strcmp(text, LAYOUT_CARRIER_PLACEHOLDER) == 0) {
        const char *carrier = nd_ui_status_carrier(ui);

        /* "if carrier:" -- an empty name leaves the placeholder standing, and
         * the home screen really does read "No Service". */
        if (carrier != NULL && carrier[0] != '\0') {
            (void)nd_strlcpy(buf, carrier, sizeof buf);
            text = buf;
        }
    }

    f = font_for_size(ui, el->font_size);
    if (f == NULL)
        return;

    nd_ui_text_size(ui, text, f, &w, &h);
    x = nd_layout_scale_x(el->x, nd_ui_width(ui));
    y = nd_layout_scale_y(el->y, nd_ui_height(ui));

    if (el->anchor == ND_ANCHOR_CENTER_H)
        x -= w / 2;
    else if (el->anchor == ND_ANCHOR_RIGHT)
        x -= w;

    (void)nd_draw_text(ui->draw, x, y, text, f, el->color);
}

static void render_icon_set_element(nd_ui *ui, const nd_element *el)
{
    const char *bat_label = NULL;
    const char *custom = NULL;
    int32_t val;
    int32_t x;
    int32_t y;
    /* Pillow's (left, top, right, bottom) with right/bottom exclusive. */
    bool have_box = false;
    int32_t box_left = 0;
    int32_t box_top = 0;
    int32_t box_bottom = 0;

    if (strcmp(el->prefix, "bat") == 0) {
        val = nd_ui_status_battery_level(ui);
        if (!nd_ui_status_battery_hardware(ui))
            bat_label = "?";
    } else if (strcmp(el->prefix, "sig") == 0) {
        int32_t bars = nd_ui_status_signal_level(ui);
        val = bars < 0 ? el->sim_val : bars;
    } else {
        val = el->sim_val;
    }

    /* custom_images.get(str(val)) -- an out-of-range value silently falls
     * through to the drawn-bars path. */
    if (val >= 0 && val < (int32_t)ND_LAYOUT_ICON_STATES && el->has_custom[val])
        custom = el->custom_images[val];

    x = nd_layout_scale_x(el->x, nd_ui_width(ui));
    y = nd_layout_scale_y(el->y, nd_ui_height(ui));

    if (custom != NULL) {
        /* _get_status_icon: home-layout coords are authored for a 240px-tall
         * UI, so the sprite is scaled by H/240 and cached at display size. */
        const nd_image *img = nd_ui_get_image_scaled(ui, custom, (double)nd_ui_height(ui) / 240.0);

        if (img != NULL) {
            (void)nd_image_blit_alpha(ui->canvas, img, x, y);
            if (bat_label != NULL) {
                nd_rect bb;

                if (nd_image_alpha_bbox(img, &bb)) {
                    box_left = bb.x0;
                    box_top = bb.y0;
                    box_bottom = bb.y1 + 1; /* nd_rect is inclusive; PIL's is not */
                } else {
                    box_left = 0;
                    box_top = 0;
                    box_bottom = img->h;
                }
                have_box = true;
            }
        }
    } else {
        int32_t step = nd_max32(3, nd_trunc32((double)nd_ui_width(ui) * 0.021));
        int32_t i;

        for (i = 0; i < el->count; i++) {
            int32_t bh = (i + 1) * 3;
            /* `i <= val`, so val 3 lights FOUR of five bars. */
            nd_color c = i <= val ? ND_WHITE : ND_RGB(0x33, 0x33, 0x33);
            int32_t bx = x + (i * step);

            (void)nd_draw_rect_fill(ui->draw, ND_RECT(bx, y + 15 - bh, bx + 3, y + 15), c);
        }
        box_left = 0;
        box_top = 0;
        box_bottom = 15;
        have_box = true;
    }

    /* Drawn even when the sprite failed to load, so a missing asset cannot
     * silently hide the "no battery" state. */
    if (bat_label != NULL) {
        if (have_box)
            draw_status_label(ui, bat_label, x, y, box_left, box_top, box_bottom);
        else
            draw_status_label(ui, bat_label, x, y, 0, 0, 15);
    }
}

void nd_home_render_element(nd_ui *ui, const nd_element *el)
{
    if (ui == NULL || el == NULL || ui->draw == NULL)
        return;
    if (el->type == ND_EL_TEXT)
        render_text_element(ui, el);
    else if (el->type == ND_EL_ICON_SET)
        render_icon_set_element(ui, el);
}
