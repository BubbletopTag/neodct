/* apps/OsmAnd/directions.c -- the turn-by-turn screen.
 *
 * One turn per page: an arrow drawn for the turn, the distance to it in the
 * big font, and the road it puts you on underneath, fitted to the space
 * rather than cut by it. Up and Down (2 and 8) page, NaviKey shows the
 * turn on the map, C goes back.
 *
 * ============ WHY THIS IS NOT A PagedList ============
 *
 * It was, and the pages came out of the real Mount Vernon to Scio route
 * reading "Coshocton Road, 2" and "at the destinat...": a PagedList wraps
 * its title by hard-trimming and draws its value on one unwrapped line
 * under a scrollbar, and a road name plus a distance is longer than that
 * line as often as not. A direction has three parts of three different
 * weights -- which way, how far, along what -- and a widget built for a
 * menu row with a setting under it has room for two.
 *
 * ============ IT STILL FOLLOWS THE FRAMEWORK'S RULES ============
 *
 * Title at y = 0 in font_xl trimmed against the breadcrumb, the breadcrumb
 * itself counting the pages, the one-pixel divider, text placed by its ink,
 * white on the chrome background. Every row above the softkey bar is
 * cleared and the bar is left to the caller, so the "Map" label is one
 * framebuffer write with the page (nd_widgets.h rule 1).
 *
 * ============ THE ARROWS ============
 *
 * Drawn with lines and one triangle each, in a box the size of two text
 * lines, because the font has no arrow glyphs and a 48 px picture of the
 * turn is read faster than the word for it. They are plotted from a
 * handful of points in a unit box so the same shapes draw at any size the
 * caller asks for; nd_draw_line grows a wide line in its minor axis only
 * (nd_draw.h rule 2), so shafts are odd widths and the heads are polygons.
 */

#include <string.h>

#include "osmand_app.h"

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* ------------------------------------------------------------------ *
 * The arrows
 * ------------------------------------------------------------------ */

/* A thick stroke between two points and an arrowhead at the second,
 * pointing along the stroke. `w` is the shaft width, odd. */
static void shaft(nd_draw *d, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, nd_color c)
{
    (void)nd_draw_line(d, x0, y0, x1, y1, c, w);
    /* Square the joint, as render.c does for roads: two wide segments
     * meeting at a right angle leave a notch on the outside of the bend. */
    (void)nd_draw_rect_fill(
        d, ND_RECT(x1 - (w - 1) / 2, y1 - (w - 1) / 2, x1 + (w - 1) / 2, y1 + (w - 1) / 2), c);
}

/* An arrowhead with its tip at (tx, ty), pointing in (dx, dy), which is one
 * of the four axis directions or a diagonal. `len` is tip to base. */
static void head(nd_draw *d, int32_t tx, int32_t ty, int32_t dx, int32_t dy, int32_t len,
                 nd_color c)
{
    nd_point p[3];
    int32_t bx = tx - dx * len;
    int32_t by = ty - dy * len;
    /* The base runs perpendicular to the direction; for a diagonal the
     * perpendicular is the other diagonal, which is what (-dy, dx) gives
     * in both cases. */
    int32_t px = -dy;
    int32_t py = dx;
    int32_t half = (len * 3) / 4;

    p[0].x = tx;
    p[0].y = ty;
    p[1].x = bx + px * half;
    p[1].y = by + py * half;
    p[2].x = bx - px * half;
    p[2].y = by - py * half;
    (void)nd_draw_polygon(d, p, 3u, c);
}

void nd_osm_arrow_draw(nd_draw *d, nd_osm_turn turn, int32_t x, int32_t y, int32_t size, nd_color c)
{
    /* Everything is placed on a grid of size/8 so the shapes scale. The
     * box is x..x+size-1, y..y+size-1. */
    int32_t u = nd_max32(size / 8, 2);
    int32_t w = nd_max32((u * 5 / 8) | 1, 3); /* odd shaft width: 5 px in a 56 px box */
    int32_t cx = x + size / 2;
    int32_t top = y + u;
    int32_t bottom = y + size - u;
    int32_t left = x + u;
    int32_t right = x + size - u;
    int32_t hl = u * 2; /* head length */

    if (d == NULL || size < 16)
        return;

    switch (turn) {
    case ND_OSM_TURN_START:
        /* A dot where you are, and the road ahead. */
        (void)nd_draw_ellipse_fill(d, ND_RECT(cx - u, bottom - 2 * u, cx + u, bottom), c);
        shaft(d, cx, bottom - 2 * u, cx, top + hl, w, c);
        head(d, cx, top, 0, -1, hl, c);
        break;
    case ND_OSM_TURN_STRAIGHT:
        shaft(d, cx, bottom, cx, top + hl, w, c);
        head(d, cx, top, 0, -1, hl, c);
        break;
    case ND_OSM_TURN_LEFT:
        shaft(d, cx + u, bottom, cx + u, y + size / 2, w, c);
        shaft(d, cx + u, y + size / 2, left + hl, y + size / 2, w, c);
        head(d, left, y + size / 2, -1, 0, hl, c);
        break;
    case ND_OSM_TURN_RIGHT:
        shaft(d, cx - u, bottom, cx - u, y + size / 2, w, c);
        shaft(d, cx - u, y + size / 2, right - hl, y + size / 2, w, c);
        head(d, right, y + size / 2, 1, 0, hl, c);
        break;
    case ND_OSM_TURN_SHARP_LEFT:
        /* Up, then back down and out: the shape of turning almost round. */
        shaft(d, cx + u, bottom, cx + u, top + u, w, c);
        shaft(d, cx + u, top + u, cx - u, top + u, w, c);
        shaft(d, cx - u, top + u, cx - u, bottom - hl - u, w, c);
        head(d, cx - u, bottom - u, 0, 1, hl, c);
        break;
    case ND_OSM_TURN_SHARP_RIGHT:
        shaft(d, cx - u, bottom, cx - u, top + u, w, c);
        shaft(d, cx - u, top + u, cx + u, top + u, w, c);
        shaft(d, cx + u, top + u, cx + u, bottom - hl - u, w, c);
        head(d, cx + u, bottom - u, 0, 1, hl, c);
        break;
    case ND_OSM_TURN_ARRIVE:
    default: {
        /* A flag on a pole. */
        nd_point f[3];

        (void)nd_draw_line(d, left + u, top, left + u, bottom, c, w);
        f[0].x = left + u + w;
        f[0].y = top;
        f[1].x = right;
        f[1].y = top + (size / 4);
        f[2].x = left + u + w;
        f[2].y = top + size / 2;
        (void)nd_draw_polygon(d, f, 3u, c);
        break;
    }
    }
}

/* ------------------------------------------------------------------ *
 * The page
 * ------------------------------------------------------------------ */

/* The geometry, derived from the panel as every screen here derives it. */
typedef struct {
    int32_t screen_w;
    int32_t divider_y;
    int32_t content_bottom;
    int32_t arrow_x;
    int32_t arrow_y;
    int32_t arrow_size;
    int32_t text_x;
    int32_t text_w;
    int32_t dist_y;
    int32_t road_y;
} page_metrics;

static void measure(const nd_ui *ui, page_metrics *g)
{
    g->screen_w = nd_ui_width(ui);
    g->divider_y = nd_ui_header_divider_y(ui);
    g->content_bottom = nd_ui_content_bottom(ui);
    /* The arrow box is two lines of the big font tall and sits with its
     * middle on the middle of the text block, six pixels in from the
     * bezel. */
    g->arrow_size = 56;
    g->arrow_x = 6;
    g->arrow_y = g->divider_y + 12;
    g->text_x = g->arrow_x + g->arrow_size + 8;
    g->text_w = g->screen_w - g->text_x - 6;
    g->dist_y = g->divider_y + 8;
    g->road_y = g->dist_y + 32;
}

/* The road name in the largest face that fits it in one line, or failing
 * that in two lines of the middle face, or failing that two lines of the
 * small one with the second shortened. A name is never cut where a face
 * that would have carried it whole exists. */
static void draw_road(nd_ui *ui, const char *road, int32_t x, int32_t y, int32_t max_w, nd_color c)
{
    const nd_font *faces[3];
    size_t n_faces = 0u;
    size_t i;
    char storage[4][ND_TEXT_LINE_MAX];
    nd_lines lines;

    if (ui->font_n != NULL)
        faces[n_faces++] = ui->font_n;
    if (ui->font_md != NULL)
        faces[n_faces++] = ui->font_md;
    if (ui->font_s != NULL)
        faces[n_faces++] = ui->font_s;
    if (n_faces == 0u || road == NULL || road[0] == '\0')
        return;

    for (i = 0u; i < n_faces; i++) {
        int32_t w = 0;

        nd_text_size(faces[i], road, &w, NULL);
        if (w <= max_w) {
            (void)nd_draw_text(ui->draw, x, y, road, faces[i], c);
            return;
        }
    }
    /* Two lines. The middle face if the name wraps into two there, else
     * the small one; the last line shortened rather than run off. */
    for (i = (n_faces > 1u) ? 1u : 0u; i < n_faces; i++) {
        int32_t line_h = 0;

        nd_lines_init(&lines, storage, ND_ARRAY_LEN(storage));
        nd_text_wrap(&lines, road, faces[i], max_w);
        if (lines.n > 2u && i + 1u < n_faces)
            continue;
        nd_text_size(faces[i], "Ag", NULL, &line_h);
        if (lines.n >= 1u)
            (void)nd_draw_text(ui->draw, x, y, nd_lines_at(&lines, 0u), faces[i], c);
        if (lines.n >= 2u) {
            char last[ND_TEXT_LINE_MAX];

            (void)nd_text_ellipsize(last, sizeof last, nd_lines_at(&lines, 1u), faces[i], max_w);
            (void)nd_draw_text(ui->draw, x, y + line_h + 5, last, faces[i], c);
        }
        return;
    }
}

void nd_osm_directions_draw(nd_ui *ui, const nd_osm_step *steps, size_t n, size_t index)
{
    page_metrics g;
    nd_header header;
    nd_draw *d;
    const nd_osm_step *s;
    char title[ND_TEXT_LINE_MAX];
    char dist[16];
    char count[24];
    int32_t reserved;
    int32_t w = 0;
    int32_t h = 0;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL || steps == NULL || n == 0u)
        return;
    if (index >= n)
        index = n - 1u;
    s = &steps[index];
    d = ui->draw;
    measure(ui, &g);

    /* 1. Rows 0..content_bottom only: the softkey strip is the caller's. */
    nd_ui_paint_chrome_content(ui);

    /* 2. Title and breadcrumb, as nd_vlist_draw() places them. The
     *    breadcrumb counts pages from one, as a PagedList's does. */
    nd_header_init(&header, ui, ND_OSMAND_APP_ROOT);
    reserved = nd_header_width(&header, (int32_t)index + 1);
    if (ui->font_xl != NULL) {
        (void)nd_text_fit(title, sizeof title, "Directions", ui->font_xl,
                          g.screen_w - 5 - reserved - 6);
        (void)nd_draw_text(d, 5, 0, title, ui->font_xl, ND_WHITE);
    }
    nd_header_draw(&header, (int32_t)index + 1);
    (void)nd_draw_line(d, 0, g.divider_y, g.screen_w, g.divider_y, ND_WHITE, 1);

    /* 3. The arrow. */
    nd_osm_arrow_draw(d, s->turn, g.arrow_x, g.arrow_y, g.arrow_size, ND_WHITE);

    /* 4. The distance, big, or the word for a page that has none. */
    if (s->turn == ND_OSM_TURN_ARRIVE)
        (void)nd_strlcpy(dist, "Arrive", sizeof dist);
    else
        nd_osm_format_distance((double)s->metres, dist, sizeof dist);
    if (ui->font_xl != NULL)
        (void)nd_draw_text(d, g.text_x, g.dist_y, dist, ui->font_xl, ND_WHITE);

    /* 5. The road, fitted. On the last page, what the journey ends at. */
    draw_road(ui, (s->turn == ND_OSM_TURN_ARRIVE) ? "End of route" : s->road, g.text_x, g.road_y,
              g.text_w, ND_WHITE);

    /* 6. "4 of 28", small, bottom right, so the breadcrumb's "13-4" has
     *    its other half. */
    if (ui->font_s != NULL) {
        (void)nd_snprintf(count, sizeof count, "%u of %u", (unsigned)(index + 1u), (unsigned)n);
        nd_text_size(ui->font_s, count, &w, &h);
        (void)nd_draw_text(d, g.screen_w - 6 - w, g.content_bottom - 6 - h - 1, count, ui->font_s,
                           ND_WHITE);
    }

    (void)nd_ui_present(ui);
}

nd_osm_dir_nav nd_osm_directions_key(int32_t key, size_t n, size_t *index)
{
    if (index == NULL || n == 0u)
        return ND_OSM_DIR_NONE;
    switch (key) {
    case ND_KEY_ENTER:
        return ND_OSM_DIR_SHOW;
    case ND_KEY_CLEAR:
        return ND_OSM_DIR_BACK;
    /* Up and Down are the rocker; 2 and 8 the same on the number pad;
     * 4 and 6 as well, because the page is a sequence and a thumb on the
     * block expects the horizontal pair to step it too. */
    case ND_KEY_UP:
    case ND_KEY_2:
    case ND_KEY_4:
    case ND_KEY_LEFT:
        if (*index > 0u)
            (*index)--;
        return ND_OSM_DIR_MOVED;
    case ND_KEY_DOWN:
    case ND_KEY_8:
    case ND_KEY_6:
    case ND_KEY_RIGHT:
        if (*index + 1u < n)
            (*index)++;
        return ND_OSM_DIR_MOVED;
    default:
        return ND_OSM_DIR_NONE;
    }
}

int32_t nd_osm_directions_show(nd_ui *ui, const nd_osm_step *steps, size_t n, size_t initial)
{
    nd_softkey bar;
    size_t index = (initial < n) ? initial : 0u;

    if (ui == NULL || steps == NULL || n == 0u)
        return -1;

    /* Painted, not presented: the page's own draw pushes it. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Map", false);
    nd_osm_directions_draw(ui, steps, n, index);

    for (;;) {
        int32_t key = nd_ui_read_keypress(ui, nd_ui_widget_timeout(ui, 0.1));
        nd_osm_dir_nav nav;

        if (nd_app_should_exit())
            return -1;
        if (key == ND_KEY_NONE)
            continue;
        nav = nd_osm_directions_key(key, n, &index);
        if (nav == ND_OSM_DIR_SHOW)
            return (int32_t)index;
        if (nav == ND_OSM_DIR_BACK)
            return -1;
        if (nav == ND_OSM_DIR_MOVED)
            nd_osm_directions_draw(ui, steps, n, index);
    }
}
