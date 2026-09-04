/* apps/OsmAnd/render.c -- drawing a map the way openstreetmap.org draws
 * one, above the softkey bar every other NeoDCT screen has.
 *
 * ============ TWO STYLES ON ONE SCREEN, DELIBERATELY ============
 *
 * Every row above the softkey bar is OpenStreetMap Carto: the beige land,
 * the blue water, the pink motorways and the yellow primaries, the white
 * residential streets with grey casings, the grey buildings that appear at
 * zoom 16. Every colour below is that stylesheet's, so that a person who
 * knows the web map recognises this one.
 *
 * What is left of NeoDCT on this screen is the softkey bar, and that is on
 * purpose. The title, breadcrumb and divider every list draws would cost
 * thirty-one of the panel's 145 content rows -- more than a fifth of the
 * map -- to say "OsmAnd 13" on a screen whose picture already says which
 * app it is. Koki and the games give the same rows up for the same reason.
 * The map does not bleed into the softkey strip, because the strip is
 * what makes the screen belong to this phone rather than to a web browser.
 *
 * ============ WHY THE MAP HAS ITS OWN SURFACE ============
 *
 * The map is rendered into a caller-owned RGB image the size of the
 * content area and blitted onto the canvas. That is what clips it: a road
 * that runs off the bottom of the map stops at the softkey bar by
 * construction rather than by every primitive knowing where the bar is,
 * and the frame test can look at the map alone.
 *
 * ============ WHY THE POLYGON FILLER IS NOT nd_draw_polygon ============
 *
 * nd_draw_polygon() is Pillow's, and Pillow's takes at most 64 points
 * because nothing in the Python OS ever drew more. A lake has hundreds.
 * The filler here is the same even-odd scanline idea with its edge table in
 * a scratch block the caller allocates once, so the render path still never
 * calls malloc.
 *
 * ============ WHY LINES ARE CLIPPED BEFORE THEY ARE DRAWN ============
 *
 * At zoom 18 a node two kilometres off-screen is at x = -8000. nd_draw_line
 * clips every pixel it plots, but it plots all of them, so an unclipped
 * motorway across the map would cost more than the whole visible frame.
 * Liang-Barsky against the surface plus a margin the width of the road
 * keeps the work proportional to what is on the panel.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "osmand_app.h"

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"

/* ------------------------------------------------------------------ *
 * The palette -- openstreetmap-carto, by hex
 * ------------------------------------------------------------------ */

#define C_LAND        ND_RGB(0xf2, 0xef, 0xe9)
#define C_RESIDENTIAL ND_RGB(0xe0, 0xdf, 0xdf)
#define C_INDUSTRIAL  ND_RGB(0xeb, 0xdb, 0xe8)
#define C_RETAIL      ND_RGB(0xff, 0xd6, 0xd1)
#define C_FARMLAND    ND_RGB(0xee, 0xf0, 0xd5)
#define C_GRASS       ND_RGB(0xcd, 0xeb, 0xb0)
#define C_WOOD        ND_RGB(0xad, 0xd1, 0x9e)
#define C_WATER       ND_RGB(0xaa, 0xd3, 0xdf)
#define C_BUILDING    ND_RGB(0xd9, 0xd0, 0xc9)
#define C_BUILDING_ED ND_RGB(0xc4, 0xb8, 0xac)
#define C_RAIL        ND_RGB(0x70, 0x70, 0x70)
#define C_PATH        ND_RGB(0xc9, 0x7e, 0x6e)
#define C_TRACK       ND_RGB(0x99, 0x66, 0x00)
#define C_WHITE       ND_RGB(0xff, 0xff, 0xff)
#define C_CASING_MIN  ND_RGB(0xbb, 0xbb, 0xbb)
#define C_CASING_TER  ND_RGB(0x8f, 0x8f, 0x8f)
#define C_SECONDARY   ND_RGB(0xf7, 0xfa, 0xbf)
#define C_SECONDARY_C ND_RGB(0x70, 0x7d, 0x05)
#define C_PRIMARY     ND_RGB(0xfc, 0xd6, 0xa4)
#define C_PRIMARY_C   ND_RGB(0xa0, 0x6b, 0x00)
#define C_TRUNK       ND_RGB(0xf9, 0xb2, 0x9c)
#define C_TRUNK_C     ND_RGB(0xc8, 0x4e, 0x2f)
#define C_MOTORWAY    ND_RGB(0xe8, 0x92, 0xa2)
#define C_MOTORWAY_C  ND_RGB(0xdc, 0x2a, 0x67)
#define C_LABEL       ND_RGB(0x22, 0x22, 0x22)
#define C_ROUTE       ND_RGB(0x4b, 0x6b, 0xff)
#define C_ROUTE_C     ND_RGB(0x1a, 0x2a, 0x80)
#define C_START       ND_RGB(0x2a, 0xa0, 0x2a)
#define C_DEST        ND_RGB(0xd0, 0x20, 0x20)
#define C_INK         ND_RGB(0x30, 0x30, 0x30)

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

void nd_osm_map_geometry(const nd_ui *ui, int32_t *top, int32_t *w, int32_t *h)
{
    /* Row 0 down to the softkey bar: 145 rows on this panel. The framework
     * draws no title, breadcrumb or divider here -- see the file header. */
    if (top != NULL)
        *top = 0;
    if (w != NULL)
        *w = nd_ui_width(ui);
    if (h != NULL)
        *h = nd_max32(1, nd_ui_content_bottom(ui));
}

/* Floor division, so a node just left of the centre lands on the pixel to
 * the left rather than on the centre's own column. Truncation would put a
 * one-pixel kink into every road that crosses the middle of the screen. */
static int32_t div_floor(int64_t a, int64_t b)
{
    int64_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return (int32_t)q;
}

void nd_osm_view_to_screen(const nd_osm_view *v, int32_t w, int32_t h, int32_t mx, int32_t my,
                           int32_t *sx, int32_t *sy)
{
    int32_t shift = ND_OSM_MERC_ZOOM - nd_clamp32(v->zoom, ND_OSM_ZOOM_MIN, ND_OSM_ZOOM_MAX);
    int64_t unit = (int64_t)1 << shift;

    if (sx != NULL)
        *sx = div_floor((int64_t)mx - v->cx, unit) + w / 2;
    if (sy != NULL)
        *sy = div_floor((int64_t)my - v->cy, unit) + h / 2;
}

/* The reference-zoom rectangle a w x h surface shows under `v`, grown by a
 * margin in pixels so a wide road just outside still draws its edge. */
static void view_bounds(const nd_osm_view *v, int32_t w, int32_t h, int32_t margin_px, int64_t *x0,
                        int64_t *y0, int64_t *x1, int64_t *y1)
{
    int32_t shift = ND_OSM_MERC_ZOOM - nd_clamp32(v->zoom, ND_OSM_ZOOM_MIN, ND_OSM_ZOOM_MAX);
    int64_t unit = (int64_t)1 << shift;

    *x0 = (int64_t)v->cx - (int64_t)(w / 2 + margin_px) * unit;
    *y0 = (int64_t)v->cy - (int64_t)(h / 2 + margin_px) * unit;
    *x1 = (int64_t)v->cx + (int64_t)(w - w / 2 + margin_px) * unit;
    *y1 = (int64_t)v->cy + (int64_t)(h - h / 2 + margin_px) * unit;
}

int32_t nd_osm_scale_metres(const nd_osm_view *v, int32_t w)
{
    static const int32_t STEPS[] = {1,    2,    5,    10,    20,    50,    100,    200,   500,
                                    1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000};
    int32_t shift = ND_OSM_MERC_ZOOM - nd_clamp32(v->zoom, ND_OSM_ZOOM_MIN, ND_OSM_ZOOM_MAX);
    double per_px = nd_osm_metres_per_unit(v->cy) * (double)((int64_t)1 << shift);
    double budget = (double)(w / 3) * per_px;
    int32_t best = STEPS[0];
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(STEPS); i++) {
        if ((double)STEPS[i] <= budget)
            best = STEPS[i];
    }
    return best;
}

/* ------------------------------------------------------------------ *
 * Scratch -- allocated once by the caller, never by a frame
 * ------------------------------------------------------------------ */

#define SCRATCH_POINTS 4096u /* one more than WAY_REFS_MAX in mapdata.c */
#define LABELS_MAX     10u

typedef struct {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    char text[48];
} label_slot;

struct nd_osm_scratch {
    int32_t *sx; /* screen x per node of the way being drawn */
    int32_t *sy;
    double *ex; /* polygon filler's edge table: x at y0, slope, y range */
    double *edx;
    int32_t *ey0;
    int32_t *ey1;
    double *xs; /* one scanline's crossings */
    label_slot labels[LABELS_MAX];
    size_t n_labels;
};

nd_osm_scratch *nd_osm_scratch_new(void)
{
    /* 4096 * (4 + 4 + 8 + 8 + 4 + 4 + 8) = 160 kB, once per app. */
    nd_osm_scratch *s = calloc(1u, sizeof *s);

    if (s == NULL)
        return NULL;
    s->sx = malloc(SCRATCH_POINTS * sizeof *s->sx);
    s->sy = malloc(SCRATCH_POINTS * sizeof *s->sy);
    s->ex = malloc(SCRATCH_POINTS * sizeof *s->ex);
    s->edx = malloc(SCRATCH_POINTS * sizeof *s->edx);
    s->ey0 = malloc(SCRATCH_POINTS * sizeof *s->ey0);
    s->ey1 = malloc(SCRATCH_POINTS * sizeof *s->ey1);
    s->xs = malloc(SCRATCH_POINTS * sizeof *s->xs);
    if (s->sx == NULL || s->sy == NULL || s->ex == NULL || s->edx == NULL || s->ey0 == NULL ||
        s->ey1 == NULL || s->xs == NULL) {
        nd_osm_scratch_free(s);
        return NULL;
    }
    return s;
}

void nd_osm_scratch_free(nd_osm_scratch *s)
{
    if (s == NULL)
        return;
    free(s->sx);
    free(s->sy);
    free(s->ex);
    free(s->edx);
    free(s->ey0);
    free(s->ey1);
    free(s->xs);
    free(s);
}

/* ------------------------------------------------------------------ *
 * Primitives
 * ------------------------------------------------------------------ */

/* Liang-Barsky against [xmin,xmax] x [ymin,ymax]. false when nothing of the
 * segment is inside. The endpoints are moved onto the boundary. */
static bool clip_segment(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1, int32_t xmin,
                         int32_t ymin, int32_t xmax, int32_t ymax)
{
    double t0 = 0.0;
    double t1 = 1.0;
    double dx = (double)*x1 - (double)*x0;
    double dy = (double)*y1 - (double)*y0;
    double p[4];
    double q[4];
    size_t i;

    p[0] = -dx;
    q[0] = (double)*x0 - (double)xmin;
    p[1] = dx;
    q[1] = (double)xmax - (double)*x0;
    p[2] = -dy;
    q[2] = (double)*y0 - (double)ymin;
    p[3] = dy;
    q[3] = (double)ymax - (double)*y0;

    for (i = 0u; i < 4u; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0)
                return false;
            continue;
        }
        {
            double r = q[i] / p[i];

            if (p[i] < 0.0) {
                if (r > t1)
                    return false;
                if (r > t0)
                    t0 = r;
            } else {
                if (r < t0)
                    return false;
                if (r < t1)
                    t1 = r;
            }
        }
    }
    {
        int32_t nx0 = *x0 + nd_trunc32(t0 * dx + (t0 * dx >= 0.0 ? 0.5 : -0.5));
        int32_t ny0 = *y0 + nd_trunc32(t0 * dy + (t0 * dy >= 0.0 ? 0.5 : -0.5));
        int32_t nx1 = *x0 + nd_trunc32(t1 * dx + (t1 * dx >= 0.0 ? 0.5 : -0.5));
        int32_t ny1 = *y0 + nd_trunc32(t1 * dy + (t1 * dy >= 0.0 ? 0.5 : -0.5));

        *x0 = nx0;
        *y0 = ny0;
        *x1 = nx1;
        *y1 = ny1;
    }
    return true;
}

/* A square of side `w` centred on a vertex, for the joints of a wide
 * polyline: Pillow's wide line is a quadrilateral per segment and two of
 * them meeting at an angle leave a notch on the outside of the bend. Odd
 * widths only, which is every width this file uses above 1. */
static void joint(nd_draw *d, int32_t x, int32_t y, int32_t w, nd_color c)
{
    int32_t r = (w - 1) / 2;

    (void)nd_draw_rect_fill(d, ND_RECT(x - r, y - r, x + r, y + r), c);
}

/* Stroke a polyline held in the scratch as screen coordinates. */
static void stroke(nd_draw *d, const nd_osm_scratch *s, uint32_t n, int32_t w, nd_color c,
                   int32_t surf_w, int32_t surf_h)
{
    uint32_t i;
    int32_t m = w + 2;

    for (i = 0u; i + 1u < n; i++) {
        int32_t x0 = s->sx[i];
        int32_t y0 = s->sy[i];
        int32_t x1 = s->sx[i + 1u];
        int32_t y1 = s->sy[i + 1u];

        if (!clip_segment(&x0, &y0, &x1, &y1, -m, -m, surf_w + m, surf_h + m))
            continue;
        (void)nd_draw_line(d, x0, y0, x1, y1, c, w);
        if (w >= 3 && i + 2u < n)
            joint(d, s->sx[i + 1u], s->sy[i + 1u], w, c);
    }
}

/* Even-odd scanline fill of the ring in the scratch. Edges are stored as
 * (x at their top, dx per row, top row, bottom row); each scanline gathers
 * the crossings of the edges spanning it, sorts them, and fills between
 * pairs. Sampling at pixel centres (y + 0.5) is what keeps a shared edge
 * between two adjacent buildings from being drawn twice or not at all. */
static void fill_ring(nd_image *img, nd_osm_scratch *s, uint32_t n, nd_color c)
{
    uint32_t i;
    uint32_t n_edges = 0u;
    int32_t ymin = INT32_MAX;
    int32_t ymax = INT32_MIN;
    int32_t y;

    if (n < 3u)
        return;
    for (i = 0u; i < n; i++) {
        uint32_t j = (i + 1u) % n;
        int32_t ax = s->sx[i];
        int32_t ay = s->sy[i];
        int32_t bx = s->sx[j];
        int32_t by = s->sy[j];

        if (ay == by)
            continue; /* horizontal edges never cross a scanline centre */
        if (ay > by) {
            int32_t t;

            t = ax;
            ax = bx;
            bx = t;
            t = ay;
            ay = by;
            by = t;
        }
        s->ex[n_edges] = (double)ax;
        s->edx[n_edges] = ((double)bx - (double)ax) / ((double)by - (double)ay);
        s->ey0[n_edges] = ay;
        s->ey1[n_edges] = by;
        n_edges++;
        ymin = nd_min32(ymin, ay);
        ymax = nd_max32(ymax, by);
    }
    if (n_edges < 2u)
        return;
    ymin = nd_max32(ymin, 0);
    ymax = nd_min32(ymax, img->h);

    for (y = ymin; y < ymax; y++) {
        double yc = (double)y + 0.5;
        uint32_t k = 0u;
        uint32_t e;

        for (e = 0u; e < n_edges; e++) {
            double x;
            uint32_t p;

            if (yc < (double)s->ey0[e] || yc >= (double)s->ey1[e])
                continue;
            x = s->ex[e] + (yc - (double)s->ey0[e]) * s->edx[e];
            /* Insertion sort: a scanline crosses a handful of edges. */
            p = k;
            while (p > 0u && s->xs[p - 1u] > x) {
                s->xs[p] = s->xs[p - 1u];
                p--;
            }
            s->xs[p] = x;
            k++;
        }
        for (e = 0u; e + 1u < k; e += 2u) {
            int32_t xa = nd_trunc32(ceil(s->xs[e] - 0.5));
            int32_t xb = nd_trunc32(ceil(s->xs[e + 1u] - 0.5)) - 1;

            if (xb < 0 || xa >= img->w)
                continue;
            (void)nd_image_fill_rect(img, ND_RECT(nd_max32(xa, 0), y, nd_min32(xb, img->w - 1), y),
                                     c);
        }
    }
}

/* Text with a one-pixel white halo, which is how Carto keeps a street name
 * legible over the street. Five draws; there are at most ten labels. */
static void halo_text(nd_draw *d, const nd_font *f, int32_t x, int32_t y, const char *text,
                      nd_color c)
{
    (void)nd_draw_text(d, x - 1, y, text, f, C_WHITE);
    (void)nd_draw_text(d, x + 1, y, text, f, C_WHITE);
    (void)nd_draw_text(d, x, y - 1, text, f, C_WHITE);
    (void)nd_draw_text(d, x, y + 1, text, f, C_WHITE);
    (void)nd_draw_text(d, x, y, text, f, c);
}

/* Reserve a label box if it is on the surface and clear of the others.
 * Returns false when it is not, and nothing is drawn. Also refuses a text
 * already placed: a long street is many ways and wants one name. */
static bool place_label(nd_osm_scratch *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        int32_t surf_w, int32_t surf_h, const char *text)
{
    size_t i;

    if (s->n_labels >= LABELS_MAX)
        return false;
    if (x0 < 0 || y0 < 0 || x1 >= surf_w || y1 >= surf_h)
        return false;
    for (i = 0u; i < s->n_labels; i++) {
        const label_slot *l = &s->labels[i];

        if (strcmp(l->text, text) == 0)
            return false;
        if (x0 <= l->x1 + 2 && x1 >= l->x0 - 2 && y0 <= l->y1 + 2 && y1 >= l->y0 - 2)
            return false;
    }
    s->labels[s->n_labels].x0 = x0;
    s->labels[s->n_labels].y0 = y0;
    s->labels[s->n_labels].x1 = x1;
    s->labels[s->n_labels].y1 = y1;
    (void)nd_strlcpy(s->labels[s->n_labels].text, text, sizeof s->labels[s->n_labels].text);
    s->n_labels++;
    return true;
}

/* ------------------------------------------------------------------ *
 * The style table
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t width; /* 0 for "not at this zoom" */
    nd_color fill;
    nd_color casing;
    bool cased; /* draw casing at width + 2 under the fill */
} line_style;

/* What a road looks like at a zoom, in Carto's spirit if not its exact
 * pixel widths -- those are tuned for a 256 px tile on a monitor and a 240
 * px panel held in a hand wants them a step thinner. Widths are odd so the
 * casing sits evenly either side (nd_draw.h's RULE 2). A width of 1 draws
 * in the casing colour alone: white on beige is invisible. */
static void road_style(uint8_t kind, int32_t zoom, line_style *st)
{
    st->width = 0;
    st->cased = false;
    st->fill = C_WHITE;
    st->casing = C_CASING_MIN;

    switch (kind) {
    case ND_OSM_KIND_MOTORWAY:
    case ND_OSM_KIND_TRUNK:
        st->fill = (kind == ND_OSM_KIND_MOTORWAY) ? C_MOTORWAY : C_TRUNK;
        st->casing = (kind == ND_OSM_KIND_MOTORWAY) ? C_MOTORWAY_C : C_TRUNK_C;
        st->width = (zoom <= 10) ? 1 : (zoom <= 12) ? 3 : (zoom <= 15) ? 5 : 7;
        break;
    case ND_OSM_KIND_PRIMARY:
        st->fill = C_PRIMARY;
        st->casing = C_PRIMARY_C;
        st->width = (zoom <= 11) ? 1 : (zoom <= 13) ? 3 : (zoom <= 15) ? 5 : 7;
        break;
    case ND_OSM_KIND_SECONDARY:
        st->fill = C_SECONDARY;
        st->casing = C_SECONDARY_C;
        st->width = (zoom <= 11) ? 0 : (zoom <= 12) ? 1 : (zoom <= 14) ? 3 : 5;
        break;
    case ND_OSM_KIND_TERTIARY:
        st->casing = C_CASING_TER;
        st->width = (zoom <= 12) ? 0 : (zoom <= 14) ? 1 : (zoom <= 15) ? 3 : 5;
        break;
    case ND_OSM_KIND_RESIDENTIAL:
        st->width = (zoom <= 13) ? 0 : (zoom <= 15) ? 1 : (zoom <= 16) ? 3 : 5;
        break;
    case ND_OSM_KIND_SERVICE:
        st->width = (zoom <= 15) ? 0 : (zoom <= 16) ? 1 : 3;
        break;
    case ND_OSM_KIND_TRACK:
        st->casing = C_TRACK;
        st->width = (zoom <= 14) ? 0 : 1;
        break;
    case ND_OSM_KIND_PATH:
        st->casing = C_PATH;
        st->width = (zoom <= 14) ? 0 : 1;
        break;
    case ND_OSM_KIND_RAILWAY:
        st->casing = C_RAIL;
        st->width = (zoom <= 11) ? 0 : (zoom <= 14) ? 1 : 2;
        break;
    case ND_OSM_KIND_RIVER:
        st->casing = C_WATER;
        st->width = (zoom <= 10) ? 0 : (zoom <= 13) ? 1 : (zoom <= 15) ? 2 : 3;
        break;
    case ND_OSM_KIND_STREAM:
        st->casing = C_WATER;
        st->width = (zoom <= 13) ? 0 : 1;
        break;
    default:
        break;
    }
    if (st->width >= 3 && kind >= ND_OSM_KIND_SERVICE)
        st->cased = true;
    if (st->width < 3)
        st->fill = st->casing;
}

static nd_color area_colour(uint8_t kind)
{
    switch (kind) {
    case ND_OSM_KIND_RESIDENTIAL_LAND:
        return C_RESIDENTIAL;
    case ND_OSM_KIND_INDUSTRIAL:
        return C_INDUSTRIAL;
    case ND_OSM_KIND_RETAIL:
        return C_RETAIL;
    case ND_OSM_KIND_FARMLAND:
        return C_FARMLAND;
    case ND_OSM_KIND_GRASS:
        return C_GRASS;
    case ND_OSM_KIND_WOOD:
        return C_WOOD;
    case ND_OSM_KIND_WATER:
        return C_WATER;
    case ND_OSM_KIND_BUILDING:
        return C_BUILDING;
    default:
        return C_LAND;
    }
}

/* Whether a kind is drawn at all at this zoom. Areas thin out with zoom the
 * way roads do: a building at zoom 14 is a pixel. */
static bool area_visible(uint8_t kind, int32_t zoom)
{
    switch (kind) {
    case ND_OSM_KIND_BUILDING:
        return zoom >= 16;
    case ND_OSM_KIND_GRASS:
    case ND_OSM_KIND_RESIDENTIAL_LAND:
    case ND_OSM_KIND_INDUSTRIAL:
    case ND_OSM_KIND_RETAIL:
    case ND_OSM_KIND_FARMLAND:
        return zoom >= 12;
    default:
        return true;
    }
}

/* ------------------------------------------------------------------ *
 * The layers
 * ------------------------------------------------------------------ */

typedef struct {
    const nd_osm_scene *scene;
    nd_image *img;
    nd_draw draw;
    nd_osm_scratch *s;
    int32_t w;
    int32_t h;
    int64_t vx0; /* the visible rectangle, with margin, reference units */
    int64_t vy0;
    int64_t vx1;
    int64_t vy1;
} frame;

static bool way_visible(const frame *fr, const nd_osm_way *w)
{
    return (int64_t)w->bx1 >= fr->vx0 && (int64_t)w->bx0 <= fr->vx1 && (int64_t)w->by1 >= fr->vy0 &&
           (int64_t)w->by0 <= fr->vy1;
}

/* Project a way's nodes into the scratch. Returns how many, never more
 * than the scratch holds. */
static uint32_t project_way(const frame *fr, const nd_osm_map *m, const nd_osm_way *w)
{
    uint32_t n = (w->n_refs < SCRATCH_POINTS) ? w->n_refs : (uint32_t)SCRATCH_POINTS;
    uint32_t i;

    for (i = 0u; i < n; i++) {
        uint32_t node = m->refs[w->first_ref + i];

        nd_osm_view_to_screen(&fr->scene->view, fr->w, fr->h, m->mx[node], m->my[node],
                              &fr->s->sx[i], &fr->s->sy[i]);
    }
    return n;
}

static void draw_areas(frame *fr, const nd_osm_map *m, uint8_t kind)
{
    uint32_t i;
    int32_t zoom = fr->scene->view.zoom;
    nd_color c = area_colour(kind);

    if (!area_visible(kind, zoom))
        return;
    for (i = m->kind_begin[kind]; i < m->kind_begin[kind + 1u]; i++) {
        const nd_osm_way *w = &m->ways[i];
        uint32_t n;

        if ((w->flags & ND_OSM_FLAG_AREA) == 0u || !way_visible(fr, w))
            continue;
        n = project_way(fr, m, w);
        fill_ring(fr->img, fr->s, n, c);
        if (kind == ND_OSM_KIND_BUILDING && zoom >= 17)
            stroke(&fr->draw, fr->s, n, 1, C_BUILDING_ED, fr->w, fr->h);
    }
}

/* Two passes per kind -- every casing, then every fill -- so that where
 * two residential streets meet the second one's fill covers the first
 * one's casing, which is what makes a junction read as one road rather
 * than a road with a grey bar across it. */
static void draw_lines(frame *fr, const nd_osm_map *m, uint8_t kind)
{
    line_style st;
    uint32_t i;
    int32_t zoom = fr->scene->view.zoom;

    road_style(kind, zoom, &st);
    if (st.width == 0)
        return;

    if (st.cased) {
        for (i = m->kind_begin[kind]; i < m->kind_begin[kind + 1u]; i++) {
            const nd_osm_way *w = &m->ways[i];

            if (!way_visible(fr, w))
                continue;
            stroke(&fr->draw, fr->s, project_way(fr, m, w), st.width + 2, st.casing, fr->w, fr->h);
        }
    }
    for (i = m->kind_begin[kind]; i < m->kind_begin[kind + 1u]; i++) {
        const nd_osm_way *w = &m->ways[i];

        if (!way_visible(fr, w))
            continue;
        stroke(&fr->draw, fr->s, project_way(fr, m, w), st.width, st.fill, fr->w, fr->h);
    }
}

/* Street names, at the midpoint of the longest on-screen segment of a
 * named road. Horizontal only: rotated text is a glyph path this
 * rasteriser does not have, and on a 240 px panel a level name beside
 * its street reads fine. Ten at most, none overlapping. */
static void draw_road_labels(frame *fr, const nd_osm_map *m)
{
    const nd_font *f = fr->scene->ui->font_s;
    int32_t zoom = fr->scene->view.zoom;
    uint8_t kind;

    if (f == NULL || zoom < 16)
        return;
    for (kind = ND_OSM_KIND_MOTORWAY; kind >= ND_OSM_KIND_RESIDENTIAL; kind--) {
        uint32_t i;

        for (i = m->kind_begin[kind]; i < m->kind_begin[kind + 1u]; i++) {
            const nd_osm_way *w = &m->ways[i];
            const char *name;
            uint32_t n;
            uint32_t k;
            int64_t best = 0;
            int32_t bx = 0;
            int32_t by = 0;
            int32_t tw = 0;
            int32_t th = 0;

            if (fr->s->n_labels >= LABELS_MAX)
                return;
            if (!way_visible(fr, w))
                continue;
            name = nd_osm_way_name(m, w);
            if (name[0] == '\0')
                continue;
            n = project_way(fr, m, w);
            for (k = 0u; k + 1u < n; k++) {
                int64_t dx = (int64_t)fr->s->sx[k + 1u] - fr->s->sx[k];
                int64_t dy = (int64_t)fr->s->sy[k + 1u] - fr->s->sy[k];
                int64_t len = dx * dx + dy * dy;
                int32_t mx = (fr->s->sx[k] + fr->s->sx[k + 1u]) / 2;
                int32_t my = (fr->s->sy[k] + fr->s->sy[k + 1u]) / 2;

                if (mx < 0 || my < 0 || mx >= fr->w || my >= fr->h)
                    continue;
                if (len > best) {
                    best = len;
                    bx = mx;
                    by = my;
                }
            }
            /* Shorter than the name is wide: the label would name a road
             * it does not visibly lie along. */
            nd_text_size(f, name, &tw, &th);
            if (best < (int64_t)tw * tw / 2 || tw <= 0)
                continue;
            if (place_label(fr->s, bx - tw / 2, by - th - 3, bx - tw / 2 + tw, by - 3, fr->w, fr->h,
                            name))
                halo_text(&fr->draw, f, bx - tw / 2, by - th - 3 - 1, name, C_LABEL);
        }
    }
}

/* Place names. Bigger places show from further out; a hamlet's name at
 * zoom 11 would cover the town it is next to. */
static void draw_place_labels(frame *fr, const nd_osm_map *m)
{
    const nd_font *f = fr->scene->ui->font_s;
    int32_t zoom = fr->scene->view.zoom;
    int32_t rank;

    if (f == NULL)
        return;
    for (rank = ND_OSM_PLACE_CITY; rank >= ND_OSM_PLACE_NEIGHBOURHOOD; rank--) {
        uint32_t i;
        int32_t zmin;
        int32_t zmax;

        switch (rank) {
        case ND_OSM_PLACE_CITY:
        case ND_OSM_PLACE_TOWN:
            zmin = ND_OSM_ZOOM_MIN;
            zmax = 15;
            break;
        case ND_OSM_PLACE_VILLAGE:
        case ND_OSM_PLACE_SUBURB:
            zmin = 12;
            zmax = 16;
            break;
        default:
            zmin = 14;
            zmax = 17;
            break;
        }
        if (zoom < zmin || zoom > zmax)
            continue;
        for (i = 0u; i < m->n_places; i++) {
            const nd_osm_place *p = &m->places[i];
            const char *name;
            int32_t sx;
            int32_t sy;
            int32_t tw = 0;
            int32_t th = 0;

            if (p->kind != rank)
                continue;
            if (fr->s->n_labels >= LABELS_MAX)
                return;
            nd_osm_view_to_screen(&fr->scene->view, fr->w, fr->h, p->mx, p->my, &sx, &sy);
            if (sx < -60 || sy < -20 || sx > fr->w + 60 || sy > fr->h + 20)
                continue;
            name = nd_osm_place_name(m, p);
            if (name[0] == '\0')
                continue;
            nd_text_size(f, name, &tw, &th);
            if (place_label(fr->s, sx - tw / 2, sy - th / 2, sx - tw / 2 + tw, sy - th / 2 + th,
                            fr->w, fr->h, name)) {
                halo_text(&fr->draw, f, sx - tw / 2, sy - th / 2 - 1, name, C_LABEL);
                (void)nd_draw_ellipse_fill(&fr->draw, ND_RECT(sx - 1, sy - 1, sx + 1, sy + 1),
                                           C_LABEL);
            }
        }
    }
}

static void draw_route(frame *fr)
{
    const nd_osm_scene *sc = fr->scene;
    const nd_osm_map *m;
    uint32_t i;
    uint32_t n;

    if (sc->n_route < 2u || sc->route == NULL || sc->route_map >= sc->n_maps)
        return;
    m = sc->maps[sc->route_map];
    if (m == NULL)
        return;
    n = (sc->n_route < SCRATCH_POINTS) ? sc->n_route : (uint32_t)SCRATCH_POINTS;
    for (i = 0u; i < n; i++) {
        uint32_t node = sc->route[i];

        if (node >= m->n_nodes)
            return;
        nd_osm_view_to_screen(&sc->view, fr->w, fr->h, m->mx[node], m->my[node], &fr->s->sx[i],
                              &fr->s->sy[i]);
    }
    stroke(&fr->draw, fr->s, n, 5, C_ROUTE_C, fr->w, fr->h);
    stroke(&fr->draw, fr->s, n, 3, C_ROUTE, fr->w, fr->h);
}

static void draw_mark(frame *fr, const nd_osm_mark *mk, nd_color c)
{
    int32_t sx;
    int32_t sy;

    if (!mk->set)
        return;
    nd_osm_view_to_screen(&fr->scene->view, fr->w, fr->h, mk->mx, mk->my, &sx, &sy);
    if (sx < -8 || sy < -8 || sx > fr->w + 8 || sy > fr->h + 8)
        return;
    (void)nd_draw_ellipse_fill(&fr->draw, ND_RECT(sx - 6, sy - 6, sx + 6, sy + 6), C_WHITE);
    (void)nd_draw_ellipse_fill(&fr->draw, ND_RECT(sx - 5, sy - 5, sx + 5, sy + 5), c);
    (void)nd_draw_ellipse_fill(&fr->draw, ND_RECT(sx - 2, sy - 2, sx + 2, sy + 2), C_WHITE);
}

/* The one cursor the app has. A gap in the middle so the pixel under it
 * can be seen, and a white underlay so it shows on a motorway as well as
 * on a field. */
static void draw_crosshair(frame *fr)
{
    int32_t cx = fr->w / 2;
    int32_t cy = fr->h / 2;
    nd_draw *d = &fr->draw;

    (void)nd_draw_line(d, cx - 9, cy, cx - 3, cy, C_WHITE, 3);
    (void)nd_draw_line(d, cx + 3, cy, cx + 9, cy, C_WHITE, 3);
    (void)nd_draw_line(d, cx, cy - 9, cx, cy - 3, C_WHITE, 3);
    (void)nd_draw_line(d, cx, cy + 3, cx, cy + 9, C_WHITE, 3);
    (void)nd_draw_line(d, cx - 9, cy, cx - 3, cy, C_INK, 1);
    (void)nd_draw_line(d, cx + 3, cy, cx + 9, cy, C_INK, 1);
    (void)nd_draw_line(d, cx, cy - 9, cx, cy - 3, C_INK, 1);
    (void)nd_draw_line(d, cx, cy + 3, cx, cy + 9, C_INK, 1);
}

static void draw_scale_bar(frame *fr)
{
    const nd_osm_view *v = &fr->scene->view;
    const nd_font *f = fr->scene->ui->font_s;
    int32_t metres = nd_osm_scale_metres(v, fr->w);
    int32_t shift = ND_OSM_MERC_ZOOM - nd_clamp32(v->zoom, ND_OSM_ZOOM_MIN, ND_OSM_ZOOM_MAX);
    double per_px = nd_osm_metres_per_unit(v->cy) * (double)((int64_t)1 << shift);
    int32_t px = nd_max32(2, nd_trunc32((double)metres / per_px));
    int32_t x0 = 5;
    int32_t y = fr->h - 5;
    char text[16];
    int32_t tw = 0;
    int32_t th = 0;

    nd_osm_format_distance((double)metres, text, sizeof text);
    if (f != NULL)
        nd_text_size(f, text, &tw, &th);

    (void)nd_draw_line(&fr->draw, x0, y, x0 + px, y, C_WHITE, 3);
    (void)nd_draw_line(&fr->draw, x0, y - 4, x0, y, C_WHITE, 3);
    (void)nd_draw_line(&fr->draw, x0 + px, y - 4, x0 + px, y, C_WHITE, 3);
    (void)nd_draw_line(&fr->draw, x0, y, x0 + px, y, C_INK, 1);
    (void)nd_draw_line(&fr->draw, x0, y - 4, x0, y, C_INK, 1);
    (void)nd_draw_line(&fr->draw, x0 + px, y - 4, x0 + px, y, C_INK, 1);
    if (f != NULL)
        halo_text(&fr->draw, f, x0 + 2, y - 7 - th - 1, text, C_INK);
}

/* What the screen says when the centre is on no downloaded map: the land
 * colour alone looks like a map that has not finished loading. */
static void draw_no_map(frame *fr)
{
    const nd_font *f = fr->scene->ui->font_s;
    static const char *const LINES[] = {"No map here.", "Options > Download map"};
    int32_t y = fr->h / 2 - 22;
    size_t i;

    if (f == NULL)
        return;
    for (i = 0u; i < ND_ARRAY_LEN(LINES); i++) {
        int32_t tw = 0;

        nd_text_size(f, LINES[i], &tw, NULL);
        halo_text(&fr->draw, f, (fr->w - tw) / 2, y, LINES[i], C_INK);
        y += 18;
    }
}

/* ------------------------------------------------------------------ *
 * The frame
 * ------------------------------------------------------------------ */

void nd_osm_render(const nd_osm_scene *s, nd_image *surface)
{
    frame fr;
    size_t i;
    uint8_t kind;
    bool covered = false;

    if (s == NULL || surface == NULL || s->ui == NULL || s->scratch == NULL)
        return;
    if (nd_draw_bind(&fr.draw, surface) != ND_OK)
        return;
    fr.scene = s;
    fr.img = surface;
    fr.s = s->scratch;
    fr.w = surface->w;
    fr.h = surface->h;
    fr.s->n_labels = 0u;
    view_bounds(&s->view, fr.w, fr.h, 8, &fr.vx0, &fr.vy0, &fr.vx1, &fr.vy1);

    (void)nd_image_fill(surface, C_LAND);

    /* Areas across every map first, then lines across every map, so a road
     * in one map is never buried under a field of the next. */
    for (kind = ND_OSM_KIND_RESIDENTIAL_LAND; kind < ND_OSM_KIND_FIRST_LINE; kind++) {
        for (i = 0u; i < s->n_maps; i++) {
            if (s->maps[i] != NULL)
                draw_areas(&fr, s->maps[i], kind);
        }
    }
    for (kind = ND_OSM_KIND_FIRST_LINE; kind < ND_OSM_KIND_COUNT; kind++) {
        for (i = 0u; i < s->n_maps; i++) {
            if (s->maps[i] != NULL)
                draw_lines(&fr, s->maps[i], kind);
        }
    }

    draw_route(&fr);

    for (i = 0u; i < s->n_maps; i++) {
        if (s->maps[i] != NULL) {
            draw_road_labels(&fr, s->maps[i]);
            draw_place_labels(&fr, s->maps[i]);
            if (nd_osm_map_contains(s->maps[i], s->view.cx, s->view.cy))
                covered = true;
        }
    }
    if (!covered)
        draw_no_map(&fr);

    draw_mark(&fr, &s->start, C_START);
    draw_mark(&fr, &s->dest, C_DEST);
    draw_crosshair(&fr);
    draw_scale_bar(&fr);
}

void nd_osm_scene_draw(const nd_osm_scene *s, nd_image *surface)
{
    nd_ui *ui;
    int32_t top;

    if (s == NULL || s->ui == NULL || surface == NULL)
        return;
    ui = s->ui;
    if (ui->draw == NULL || ui->canvas == NULL)
        return;

    /* The map covers rows 0..content_bottom-1 exactly, so there is no
     * chrome to paint under it and no clear to do: the blit IS the clear.
     * The softkey strip below is the caller's and survives (nd_widgets.h
     * rule 1), which is what lets nd_softkey_update(..., false) and this
     * make one framebuffer write instead of two. */
    nd_osm_render(s, surface);
    nd_osm_map_geometry(ui, &top, NULL, NULL);
    (void)nd_image_blit(ui->canvas, surface, 0, top);

    (void)nd_ui_present(ui);
}
