/* apps/OsmAnd/mapdata.c -- the projection, the Overpass importer and the
 * .ndmap file.
 *
 * Three things that belong together because they agree on one layout:
 * what a node is (a point in Web Mercator at a fixed reference zoom), what
 * a way is (a run of node indices with a kind), and how both are written to
 * the memory card and read back.
 *
 * ============ WHY THE XML IS PARSED BY LINE AND NOT BY A PARSER ============
 *
 * An Overpass answer for a town is several megabytes of XML with a shape
 * that never varies: one element per line, attributes double-quoted, the
 * nodes before the ways. lib/nd_json.c would build a tree of it that costs
 * more than the phone has. A line scanner reads it in two passes -- nodes
 * first, so that the ways in the second pass can resolve their references
 * against a sorted table -- and holds the answer, not the document. That is
 * the difference between a 5 MB import and a 50 MB one on a 64 MB phone.
 *
 * ============ WHY THE FILE STORES DEGREES AND NOT PIXELS ============
 *
 * The node table is lat/lon in 1e7 degrees, which is what OpenStreetMap
 * stores and what survives a file byte-for-byte. The projection to the
 * reference zoom happens at load: 400,000 nodes cost about a fifth of a
 * second on the Cortex-A7, once per app launch, and in exchange the file
 * never depends on ND_OSM_MERC_ZOOM and nothing has to be re-downloaded if
 * that constant changes.
 *
 * ============ LIFETIMES ============
 *
 * nd_osm_map_load() allocates every table of the map and nd_osm_map_free()
 * releases every one, including the routing graph route.c hangs off it.
 * Nothing here allocates per frame; the renderer only reads.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osmand_app.h"

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

#define LOG_TAG "OsmAnd"

/* ------------------------------------------------------------------ *
 * The projection
 * ------------------------------------------------------------------ */

#define WORLD_PX (256.0 * (double)(1 << ND_OSM_MERC_ZOOM))
#define MAX_LAT  85.05112878
#define PI_D     3.14159265358979323846

void nd_osm_project(int32_t lat, int32_t lon, int32_t *mx, int32_t *my)
{
    double la = (double)lat / (double)ND_OSM_DEG;
    double lo = (double)lon / (double)ND_OSM_DEG;
    double s;
    double x;
    double y;

    if (la > MAX_LAT)
        la = MAX_LAT;
    if (la < -MAX_LAT)
        la = -MAX_LAT;
    if (lo > 180.0)
        lo = 180.0;
    if (lo < -180.0)
        lo = -180.0;

    s = sin(la * PI_D / 180.0);
    x = (lo + 180.0) / 360.0 * WORLD_PX;
    y = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * PI_D)) * WORLD_PX;

    if (x < 0.0)
        x = 0.0;
    if (x > WORLD_PX - 1.0)
        x = WORLD_PX - 1.0;
    if (y < 0.0)
        y = 0.0;
    if (y > WORLD_PX - 1.0)
        y = WORLD_PX - 1.0;

    if (mx != NULL)
        *mx = nd_trunc32(x);
    if (my != NULL)
        *my = nd_trunc32(y);
}

void nd_osm_unproject(int32_t mx, int32_t my, double *lat, double *lon)
{
    double n = PI_D - 2.0 * PI_D * (double)my / WORLD_PX;

    if (lon != NULL)
        *lon = (double)mx / WORLD_PX * 360.0 - 180.0;
    if (lat != NULL)
        *lat = 180.0 / PI_D * atan(0.5 * (exp(n) - exp(-n)));
}

double nd_osm_metres_per_unit(int32_t my)
{
    double lat;

    nd_osm_unproject(0, my, &lat, NULL);
    /* The equatorial circumference, WGS84. */
    return 40075016.686 * cos(lat * PI_D / 180.0) / WORLD_PX;
}

void nd_osm_bbox_around(double lat, double lon, double km, int32_t *south, int32_t *west,
                        int32_t *north, int32_t *east)
{
    /* One degree of latitude is 111.32 km everywhere; a degree of longitude
     * shrinks with the cosine of the latitude. Good to a fraction of a
     * percent, which is all a download box needs. */
    double half = km / 2.0;
    double dlat = half / 111.32;
    double c = cos(lat * PI_D / 180.0);
    double dlon = (c > 0.01) ? half / (111.32 * c) : 180.0;
    double s = lat - dlat;
    double n = lat + dlat;
    double w = lon - dlon;
    double e = lon + dlon;

    if (s < -MAX_LAT)
        s = -MAX_LAT;
    if (n > MAX_LAT)
        n = MAX_LAT;
    if (w < -180.0)
        w = -180.0;
    if (e > 180.0)
        e = 180.0;

    if (south != NULL)
        *south = nd_trunc32(s * (double)ND_OSM_DEG);
    if (west != NULL)
        *west = nd_trunc32(w * (double)ND_OSM_DEG);
    if (north != NULL)
        *north = nd_trunc32(n * (double)ND_OSM_DEG);
    if (east != NULL)
        *east = nd_trunc32(e * (double)ND_OSM_DEG);
}

/* ------------------------------------------------------------------ *
 * Names and containment
 * ------------------------------------------------------------------ */

const char *nd_osm_way_name(const nd_osm_map *m, const nd_osm_way *w)
{
    if (m == NULL || w == NULL || m->names == NULL || w->name_off >= m->names_len)
        return "";
    return m->names + w->name_off;
}

const char *nd_osm_place_name(const nd_osm_map *m, const nd_osm_place *p)
{
    if (m == NULL || p == NULL || m->names == NULL || p->name_off >= m->names_len)
        return "";
    return m->names + p->name_off;
}

bool nd_osm_map_contains(const nd_osm_map *m, int32_t mx, int32_t my)
{
    if (m == NULL)
        return false;
    return mx >= m->bx0 && mx <= m->bx1 && my >= m->by0 && my <= m->by1;
}

/* ------------------------------------------------------------------ *
 * Classifying a way from its tags
 * ------------------------------------------------------------------ */

static bool in_list(const char *v, const char *const *list)
{
    size_t i;

    for (i = 0u; list[i] != NULL; i++) {
        if (strcmp(v, list[i]) == 0)
            return true;
    }
    return false;
}

static bool is_area_kind(uint8_t kind)
{
    return kind >= ND_OSM_KIND_RESIDENTIAL_LAND && kind < ND_OSM_KIND_FIRST_LINE;
}

nd_osm_place_kind nd_osm_place_rank(const char *v)
{
    if (v == NULL)
        return ND_OSM_PLACE_OTHER;
    if (strcmp(v, "city") == 0)
        return ND_OSM_PLACE_CITY;
    if (strcmp(v, "town") == 0)
        return ND_OSM_PLACE_TOWN;
    if (strcmp(v, "village") == 0)
        return ND_OSM_PLACE_VILLAGE;
    if (strcmp(v, "suburb") == 0 || strcmp(v, "quarter") == 0)
        return ND_OSM_PLACE_SUBURB;
    if (strcmp(v, "hamlet") == 0 || strcmp(v, "isolated_dwelling") == 0)
        return ND_OSM_PLACE_HAMLET;
    if (strcmp(v, "neighbourhood") == 0 || strcmp(v, "locality") == 0)
        return ND_OSM_PLACE_NEIGHBOURHOOD;
    return ND_OSM_PLACE_OTHER;
}

/* Area tags never override a highway: a pedestrian square carries both and
 * is a place to walk before it is a thing to fill. Among the area tags
 * water beats everything and a building beats landuse, so that a pond in a
 * park is blue and a house on farmland is a house. */
static void set_area(nd_osm_classify *c, uint8_t kind)
{
    if (c->highway)
        return;
    if (c->kind == ND_OSM_KIND_WATER)
        return;
    if (c->kind == ND_OSM_KIND_BUILDING && kind != ND_OSM_KIND_WATER)
        return;
    c->kind = kind;
}

void nd_osm_classify_tag(nd_osm_classify *c, const char *k, const char *v)
{
    static const char *const RESIDENTIAL[] = {"residential", "unclassified", "living_street",
                                              "road", NULL};
    static const char *const PATH[] = {"path",       "footway",   "cycleway", "steps",
                                       "pedestrian", "bridleway", "corridor", NULL};
    static const char *const RAIL[] = {"rail", "light_rail", "narrow_gauge", "tram", NULL};
    static const char *const GRASS_LANDUSE[] = {
        "grass", "meadow", "village_green", "recreation_ground", "cemetery", NULL};
    static const char *const GRASS_LEISURE[] = {
        "park", "garden", "pitch", "playground", "golf_course", "nature_reserve", "dog_park", NULL};
    static const char *const FARM[] = {"farmland", "orchard", "vineyard", "farmyard", NULL};

    if (c == NULL || k == NULL || v == NULL)
        return;

    if (strcmp(k, "highway") == 0) {
        uint8_t kind = ND_OSM_KIND_NONE;

        if (strcmp(v, "motorway") == 0 || strcmp(v, "motorway_link") == 0) {
            kind = ND_OSM_KIND_MOTORWAY;
            c->flags |= ND_OSM_FLAG_NO_FOOT;
        } else if (strcmp(v, "trunk") == 0 || strcmp(v, "trunk_link") == 0) {
            kind = ND_OSM_KIND_TRUNK;
        } else if (strcmp(v, "primary") == 0 || strcmp(v, "primary_link") == 0) {
            kind = ND_OSM_KIND_PRIMARY;
        } else if (strcmp(v, "secondary") == 0 || strcmp(v, "secondary_link") == 0) {
            kind = ND_OSM_KIND_SECONDARY;
        } else if (strcmp(v, "tertiary") == 0 || strcmp(v, "tertiary_link") == 0) {
            kind = ND_OSM_KIND_TERTIARY;
        } else if (in_list(v, RESIDENTIAL)) {
            kind = ND_OSM_KIND_RESIDENTIAL;
        } else if (strcmp(v, "service") == 0) {
            kind = ND_OSM_KIND_SERVICE;
        } else if (strcmp(v, "track") == 0) {
            kind = ND_OSM_KIND_TRACK;
        } else if (in_list(v, PATH)) {
            kind = ND_OSM_KIND_PATH;
            c->flags |= ND_OSM_FLAG_NO_CAR;
        }
        if (kind != ND_OSM_KIND_NONE) {
            c->kind = kind;
            c->highway = true;
        }
        return;
    }
    if (strcmp(k, "railway") == 0) {
        if (in_list(v, RAIL) && !c->highway && c->kind == ND_OSM_KIND_NONE)
            c->kind = ND_OSM_KIND_RAILWAY;
        return;
    }
    if (strcmp(k, "waterway") == 0) {
        if (strcmp(v, "river") == 0 || strcmp(v, "canal") == 0) {
            if (!c->highway && !is_area_kind(c->kind))
                c->kind = ND_OSM_KIND_RIVER;
        } else if (strcmp(v, "stream") == 0 || strcmp(v, "ditch") == 0 || strcmp(v, "drain") == 0) {
            if (!c->highway && !is_area_kind(c->kind))
                c->kind = ND_OSM_KIND_STREAM;
        } else if (strcmp(v, "riverbank") == 0) {
            set_area(c, ND_OSM_KIND_WATER);
        }
        return;
    }
    if (strcmp(k, "natural") == 0) {
        if (strcmp(v, "water") == 0)
            set_area(c, ND_OSM_KIND_WATER);
        else if (strcmp(v, "wood") == 0)
            set_area(c, ND_OSM_KIND_WOOD);
        return;
    }
    if (strcmp(k, "landuse") == 0) {
        if (strcmp(v, "forest") == 0)
            set_area(c, ND_OSM_KIND_WOOD);
        else if (in_list(v, GRASS_LANDUSE))
            set_area(c, ND_OSM_KIND_GRASS);
        else if (strcmp(v, "residential") == 0)
            set_area(c, ND_OSM_KIND_RESIDENTIAL_LAND);
        else if (strcmp(v, "industrial") == 0 || strcmp(v, "railway") == 0)
            set_area(c, ND_OSM_KIND_INDUSTRIAL);
        else if (strcmp(v, "retail") == 0 || strcmp(v, "commercial") == 0)
            set_area(c, ND_OSM_KIND_RETAIL);
        else if (in_list(v, FARM))
            set_area(c, ND_OSM_KIND_FARMLAND);
        else if (strcmp(v, "reservoir") == 0 || strcmp(v, "basin") == 0)
            set_area(c, ND_OSM_KIND_WATER);
        return;
    }
    if (strcmp(k, "leisure") == 0) {
        if (in_list(v, GRASS_LEISURE))
            set_area(c, ND_OSM_KIND_GRASS);
        return;
    }
    if (strcmp(k, "building") == 0) {
        if (strcmp(v, "no") != 0)
            set_area(c, ND_OSM_KIND_BUILDING);
        return;
    }
    if (strcmp(k, "oneway") == 0) {
        if (strcmp(v, "yes") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0)
            c->flags |= ND_OSM_FLAG_ONEWAY;
        return;
    }
    if (strcmp(k, "junction") == 0) {
        if (strcmp(v, "roundabout") == 0 || strcmp(v, "circular") == 0)
            c->flags |= ND_OSM_FLAG_ONEWAY;
        return;
    }
    if (strcmp(k, "access") == 0) {
        if (strcmp(v, "no") == 0 || strcmp(v, "private") == 0)
            c->flags |= ND_OSM_FLAG_NO_CAR | ND_OSM_FLAG_NO_FOOT;
        return;
    }
    if (strcmp(k, "motor_vehicle") == 0 || strcmp(k, "vehicle") == 0 ||
        strcmp(k, "motorcar") == 0) {
        if (strcmp(v, "no") == 0 || strcmp(v, "private") == 0)
            c->flags |= ND_OSM_FLAG_NO_CAR;
        return;
    }
    if (strcmp(k, "foot") == 0) {
        if (strcmp(v, "no") == 0)
            c->flags |= ND_OSM_FLAG_NO_FOOT;
        return;
    }
    if (strcmp(k, "area") == 0) {
        if (strcmp(v, "yes") == 0)
            c->area_tag = true;
        return;
    }
}

void nd_osm_classify_finish(nd_osm_classify *c)
{
    if (c == NULL)
        return;
    if (c->highway) {
        /* A road is a line even when it is a ring: a roundabout, a closed
         * crescent, a pedestrian square. Filling any of them would paint
         * over what is inside. */
        c->flags &= (uint8_t)~ND_OSM_FLAG_AREA;
        return;
    }
    if (is_area_kind(c->kind)) {
        if (c->closed)
            c->flags |= ND_OSM_FLAG_AREA;
        else
            c->kind = ND_OSM_KIND_NONE; /* an open landuse edge draws nothing */
    }
}

/* ------------------------------------------------------------------ *
 * A growable table with a ceiling
 * ------------------------------------------------------------------ */

typedef struct {
    void *data;
    size_t n;
    size_t cap;
    size_t elem;
    size_t max;
} table;

static void table_init(table *t, size_t elem, size_t max)
{
    t->data = NULL;
    t->n = 0u;
    t->cap = 0u;
    t->elem = elem;
    t->max = max;
}

/* Room for one more; ND_ERR_TOOLONG at the ceiling, ND_ERR_NOMEM otherwise. */
static nd_err table_reserve(table *t)
{
    size_t want;
    void *grown;

    if (t->n < t->cap)
        return ND_OK;
    if (t->n >= t->max)
        return ND_ERR_TOOLONG;
    want = (t->cap == 0u) ? 1024u : t->cap * 2u;
    if (want > t->max)
        want = t->max;
    /* owned by the table; released by table_free */
    grown = realloc(t->data, want * t->elem);
    if (grown == NULL)
        return ND_ERR_NOMEM;
    t->data = grown;
    t->cap = want;
    return ND_OK;
}

static void table_free(table *t)
{
    free(t->data);
    t->data = NULL;
    t->n = 0u;
    t->cap = 0u;
}

/* ------------------------------------------------------------------ *
 * The XML line scanner
 * ------------------------------------------------------------------ */

#define LINE_MAX 4096

/* Read one line, dropping whatever of an over-long one does not fit. An
 * Overpass line is a few dozen bytes; anything longer is a name nobody
 * needs the end of. false at EOF. */
static bool read_line(FILE *f, char *buf, size_t sz)
{
    size_t len;

    if (fgets(buf, (int)sz, f) == NULL)
        return false;
    len = strlen(buf);
    if (len > 0u && buf[len - 1u] == '\n') {
        buf[len - 1u] = '\0';
        return true;
    }
    if (len + 1u >= sz) {
        int ch;

        while ((ch = fgetc(f)) != EOF && ch != '\n') {}
    }
    return true;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* XML's five named entities and numeric ones. Anything else is copied as
 * it is -- a stray '&' in a street name is a street name. */
static size_t decode_entity(const char *in, char *out, size_t out_left)
{
    static const struct {
        const char *ent;
        char ch;
    } NAMED[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(NAMED); i++) {
        size_t n = strlen(NAMED[i].ent);

        if (strncmp(in, NAMED[i].ent, n) == 0) {
            if (out_left > 0u)
                *out = NAMED[i].ch;
            return n;
        }
    }
    if (in[1] == '#') {
        const char *p = in + 2;
        uint32_t cp = 0u;
        int base = 10;
        size_t n;

        if (*p == 'x' || *p == 'X') {
            base = 16;
            p++;
        }
        cp = (uint32_t)strtoul(p, (char **)&p, base);
        if (*p != ';')
            return 0u;
        n = (size_t)(p - in) + 1u;
        /* UTF-8 encode, up to four bytes, into whatever room is left. */
        if (cp < 0x80u) {
            if (out_left >= 1u)
                out[0] = (char)cp;
            else
                return 0u;
        } else if (cp < 0x800u) {
            if (out_left < 2u)
                return 0u;
            out[0] = (char)(0xC0u | (cp >> 6));
            out[1] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (out_left < 3u)
                return 0u;
            out[0] = (char)(0xE0u | (cp >> 12));
            out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[2] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (out_left < 4u)
                return 0u;
            out[0] = (char)(0xF0u | (cp >> 18));
            out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[3] = (char)(0x80u | (cp & 0x3Fu));
        }
        return n;
    }
    return 0u;
}

/* How many bytes decode_entity() wrote for a return of n > 0. */
static size_t entity_out_len(const char *in)
{
    if (in[1] != '#')
        return 1u;
    {
        const char *p = in + 2;
        int base = 10;
        uint32_t cp;

        if (*p == 'x' || *p == 'X') {
            base = 16;
            p++;
        }
        cp = (uint32_t)strtoul(p, NULL, base);
        return (cp < 0x80u) ? 1u : (cp < 0x800u) ? 2u : (cp < 0x10000u) ? 3u : 4u;
    }
}

/* The value of attribute `key` in `line`, decoded, or false. Truncates to
 * out_sz silently: the only attribute that can be long is a name, and a
 * name cut at 63 bytes is still the name. */
static bool attr(const char *line, const char *key, char *out, size_t out_sz)
{
    char pat[32];
    const char *p;
    size_t o = 0u;

    if (nd_snprintf(pat, sizeof pat, " %s=\"", key) != ND_OK)
        return false;
    p = strstr(line, pat);
    if (p == NULL)
        return false;
    p += strlen(pat);
    while (*p != '\0' && *p != '"') {
        if (*p == '&') {
            size_t used = decode_entity(p, out + o, (o < out_sz) ? out_sz - 1u - o : 0u);

            if (used > 0u) {
                size_t wrote = entity_out_len(p);

                if (o + wrote < out_sz)
                    o += wrote;
                p += used;
                continue;
            }
        }
        if (o + 1u < out_sz)
            out[o++] = *p;
        p++;
    }
    if (out_sz > 0u)
        out[o] = '\0';
    return true;
}

static bool attr_i64(const char *line, const char *key, int64_t *out)
{
    char buf[32];
    char *end;
    long long v;

    if (!attr(line, key, buf, sizeof buf))
        return false;
    errno = 0;
    v = strtoll(buf, &end, 10);
    if (end == buf || errno != 0)
        return false;
    *out = (int64_t)v;
    return true;
}

/* "53.3498123" -> 533498123, without going through a double, so that the
 * file holds exactly the digits Overpass sent. */
static bool attr_deg(const char *line, const char *key, int32_t *out)
{
    char buf[32];
    const char *p;
    bool neg = false;
    int64_t whole = 0;
    int64_t frac = 0;
    int32_t digits = 0;

    if (!attr(line, key, buf, sizeof buf))
        return false;
    p = buf;
    if (*p == '-') {
        neg = true;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (!isdigit((unsigned char)*p))
        return false;
    while (isdigit((unsigned char)*p)) {
        whole = whole * 10 + (*p - '0');
        if (whole > 180)
            return false;
        p++;
    }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            if (digits < 7) {
                frac = frac * 10 + (*p - '0');
                digits++;
            }
            p++;
        }
    }
    while (digits < 7) {
        frac *= 10;
        digits++;
    }
    {
        int64_t v = whole * ND_OSM_DEG + frac;

        if (neg)
            v = -v;
        *out = (int32_t)v;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * The importer
 * ------------------------------------------------------------------ */

typedef struct {
    int64_t id;
    int32_t lat;
    int32_t lon;
} raw_node;

typedef struct {
    int32_t lat;
    int32_t lon;
    uint8_t kind;
    uint32_t name_off;
} raw_place;

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint32_t first_ref;
    uint32_t n_refs;
    uint32_t name_off;
} raw_way;

typedef struct {
    table nodes;  /* raw_node  */
    table places; /* raw_place */
    table ways;   /* raw_way   */
    table refs;   /* uint32_t  */
    table names;  /* char      */
} import_state;

static int cmp_raw_node(const void *a, const void *b)
{
    const raw_node *x = (const raw_node *)a;
    const raw_node *y = (const raw_node *)b;

    return (x->id < y->id) ? -1 : (x->id > y->id) ? 1 : 0;
}

static nd_err names_add(table *names, const char *s, uint32_t *off_out)
{
    size_t len = strlen(s) + 1u;
    size_t i;

    if (s[0] == '\0') {
        *off_out = 0u;
        return ND_OK;
    }
    if (names->n + len > names->max)
        return ND_ERR_TOOLONG;
    for (i = 0u; i < len; i++) {
        nd_err rc = table_reserve(names);

        if (rc != ND_OK)
            return rc;
        ((char *)names->data)[names->n++] = s[i];
    }
    *off_out = (uint32_t)(names->n - len);
    return ND_OK;
}

static void say_why(char *why, size_t why_sz, const char *text)
{
    if (why != NULL && why_sz > 0u)
        (void)nd_strlcpy(why, text, why_sz);
}

static const char *cap_reason(nd_err rc)
{
    return (rc == ND_ERR_TOOLONG) ? "That area has too much in it. Try a smaller one."
                                  : "Not enough memory to import that area.";
}

/* Pass 1: every node's position, and the named places among them. */
static nd_err import_nodes(FILE *f, import_state *st, char *line, char *why, size_t why_sz)
{
    bool in_node = false;
    int32_t cur_lat = 0;
    int32_t cur_lon = 0;
    nd_osm_place_kind cur_rank = ND_OSM_PLACE_OTHER;
    char cur_name[ND_OSM_NAME_MAX];
    char k[64];
    char v[ND_OSM_NAME_MAX];
    bool any = false;

    cur_name[0] = '\0';

    while (read_line(f, line, LINE_MAX)) {
        const char *p = skip_ws(line);

        if (strncmp(p, "<node ", 6) == 0) {
            raw_node *n;
            int64_t id;
            nd_err rc;

            if (!attr_i64(p, "id", &id) || !attr_deg(p, "lat", &cur_lat) ||
                !attr_deg(p, "lon", &cur_lon))
                continue;
            rc = table_reserve(&st->nodes);
            if (rc != ND_OK) {
                say_why(why, why_sz, cap_reason(rc));
                return rc;
            }
            n = &((raw_node *)st->nodes.data)[st->nodes.n++];
            n->id = id;
            n->lat = cur_lat;
            n->lon = cur_lon;
            any = true;

            in_node = (strstr(p, "/>") == NULL);
            cur_rank = ND_OSM_PLACE_OTHER;
            cur_name[0] = '\0';
            continue;
        }
        if (in_node && strncmp(p, "<tag ", 5) == 0) {
            if (attr(p, "k", k, sizeof k) && attr(p, "v", v, sizeof v)) {
                if (strcmp(k, "place") == 0)
                    cur_rank = nd_osm_place_rank(v);
                else if (strcmp(k, "name") == 0)
                    (void)nd_strlcpy(cur_name, v, sizeof cur_name);
            }
            continue;
        }
        if (in_node && strncmp(p, "</node>", 7) == 0) {
            in_node = false;
            if (cur_rank != ND_OSM_PLACE_OTHER && cur_name[0] != '\0' &&
                st->places.n < st->places.max) {
                raw_place *pl;
                uint32_t off;

                if (names_add(&st->names, cur_name, &off) != ND_OK)
                    continue;
                if (table_reserve(&st->places) != ND_OK)
                    continue;
                pl = &((raw_place *)st->places.data)[st->places.n++];
                pl->lat = cur_lat;
                pl->lon = cur_lon;
                pl->kind = (uint8_t)cur_rank;
                pl->name_off = off;
            }
            continue;
        }
        if (strncmp(p, "<way ", 5) == 0)
            break; /* the node block is over; the second pass starts here */
        if (strncmp(p, "<remark>", 8) == 0) {
            /* Overpass reports a query it could not run inside the body
             * with a 200, so this is the only place the refusal shows. */
            say_why(why, why_sz, "The map server refused that area. Try a smaller one.");
            return ND_ERR_IO;
        }
    }
    if (!any) {
        say_why(why, why_sz, "The map server sent nothing for that area.");
        return ND_ERR_PARSE;
    }
    return ND_OK;
}

#define WAY_REFS_MAX 4000

static uint32_t find_node(const import_state *st, int64_t id)
{
    const raw_node *nodes = (const raw_node *)st->nodes.data;
    size_t lo = 0u;
    size_t hi = st->nodes.n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;

        if (nodes[mid].id < id)
            lo = mid + 1u;
        else if (nodes[mid].id > id)
            hi = mid;
        else
            return (uint32_t)mid;
    }
    return UINT32_MAX;
}

/* Pass 2: the ways. Refs that name a node outside the answer are dropped;
 * Overpass's (._;>;) makes that rare, and a road with one node missing is
 * still a road. */
static nd_err import_ways(FILE *f, import_state *st, char *line, int64_t *ref_buf, char *why,
                          size_t why_sz)
{
    bool in_way = false;
    size_t n_ref = 0u;
    bool overflow = false;
    bool reverse = false;
    nd_osm_classify cls;
    char cur_name[ND_OSM_NAME_MAX];
    char cur_ref[ND_OSM_NAME_MAX];
    char k[64];
    char v[ND_OSM_NAME_MAX];

    memset(&cls, 0, sizeof cls);
    cur_name[0] = '\0';
    cur_ref[0] = '\0';

    do {
        const char *p = skip_ws(line);

        if (strncmp(p, "<way ", 5) == 0) {
            in_way = (strstr(p, "/>") == NULL);
            n_ref = 0u;
            overflow = false;
            reverse = false;
            memset(&cls, 0, sizeof cls);
            cur_name[0] = '\0';
            cur_ref[0] = '\0';
            continue;
        }
        if (!in_way)
            continue;
        if (strncmp(p, "<nd ", 4) == 0) {
            int64_t id;

            if (!attr_i64(p, "ref", &id))
                continue;
            if (n_ref >= WAY_REFS_MAX) {
                overflow = true;
                continue;
            }
            ref_buf[n_ref++] = id;
            continue;
        }
        if (strncmp(p, "<tag ", 5) == 0) {
            if (attr(p, "k", k, sizeof k) && attr(p, "v", v, sizeof v)) {
                if (strcmp(k, "name") == 0)
                    (void)nd_strlcpy(cur_name, v, sizeof cur_name);
                else if (strcmp(k, "ref") == 0)
                    (void)nd_strlcpy(cur_ref, v, sizeof cur_ref);
                else if (strcmp(k, "oneway") == 0 && strcmp(v, "-1") == 0)
                    reverse = true;
                else
                    nd_osm_classify_tag(&cls, k, v);
            }
            continue;
        }
        if (strncmp(p, "</way>", 6) == 0) {
            raw_way *w;
            uint32_t first;
            uint32_t kept = 0u;
            uint32_t off;
            size_t i;
            nd_err rc;

            in_way = false;
            if (overflow || n_ref < 2u)
                continue;
            cls.closed = (ref_buf[0] == ref_buf[n_ref - 1u]);
            nd_osm_classify_finish(&cls);
            if (cls.kind == ND_OSM_KIND_NONE)
                continue;
            if (reverse) {
                /* oneway=-1 means "against the way's direction". Storing
                 * it reversed lets the router treat every one-way road the
                 * same way. */
                for (i = 0u; i < n_ref / 2u; i++) {
                    int64_t t = ref_buf[i];

                    ref_buf[i] = ref_buf[n_ref - 1u - i];
                    ref_buf[n_ref - 1u - i] = t;
                }
                cls.flags |= ND_OSM_FLAG_ONEWAY;
            }

            first = (uint32_t)st->refs.n;
            for (i = 0u; i < n_ref; i++) {
                uint32_t idx = find_node(st, ref_buf[i]);

                if (idx == UINT32_MAX)
                    continue;
                rc = table_reserve(&st->refs);
                if (rc != ND_OK) {
                    say_why(why, why_sz, cap_reason(rc));
                    return rc;
                }
                ((uint32_t *)st->refs.data)[st->refs.n++] = idx;
                kept++;
            }
            if (kept < 2u) {
                st->refs.n = first;
                continue;
            }
            /* A road with no name but a number is known by its number. */
            rc = names_add(&st->names, (cur_name[0] != '\0') ? cur_name : cur_ref, &off);
            if (rc != ND_OK) {
                if (rc == ND_ERR_NOMEM) {
                    say_why(why, why_sz, cap_reason(rc));
                    return rc;
                }
                off = 0u; /* the pool is full: the road stays, unnamed */
            }
            rc = table_reserve(&st->ways);
            if (rc != ND_OK) {
                say_why(why, why_sz, cap_reason(rc));
                return rc;
            }
            w = &((raw_way *)st->ways.data)[st->ways.n++];
            w->kind = cls.kind;
            w->flags = cls.flags;
            w->first_ref = first;
            w->n_refs = kept;
            w->name_off = off;
            continue;
        }
    } while (read_line(f, line, LINE_MAX));

    return ND_OK;
}

/* ---- little-endian writers ---- */

static bool put_u8(FILE *f, uint8_t v)
{
    return fputc((int)v, f) != EOF;
}

static bool put_u16(FILE *f, uint16_t v)
{
    return put_u8(f, (uint8_t)(v & 0xFFu)) && put_u8(f, (uint8_t)(v >> 8));
}

static bool put_u32(FILE *f, uint32_t v)
{
    return put_u16(f, (uint16_t)(v & 0xFFFFu)) && put_u16(f, (uint16_t)(v >> 16));
}

static bool put_i32(FILE *f, int32_t v)
{
    return put_u32(f, (uint32_t)v);
}

/* The header. 108 bytes; every reader below agrees on the offsets. */
#define HDR_SIZE 108u

static bool write_header(FILE *f, const char *name, int32_t south, int32_t west, int32_t north,
                         int32_t east, uint32_t n_nodes, uint32_t n_ways, uint32_t n_refs,
                         uint32_t n_places, uint32_t names_len)
{
    char nm[ND_OSM_NAME_MAX];

    memset(nm, 0, sizeof nm);
    (void)nd_strlcpy(nm, name, sizeof nm);
    return fwrite(ND_OSM_FILE_MAGIC, 1u, 4u, f) == 4u && put_u32(f, ND_OSM_FILE_VERSION) &&
           put_i32(f, south) && put_i32(f, west) && put_i32(f, north) && put_i32(f, east) &&
           put_u32(f, n_nodes) && put_u32(f, n_ways) && put_u32(f, n_refs) &&
           put_u32(f, n_places) && put_u32(f, names_len) &&
           fwrite(nm, 1u, sizeof nm, f) == sizeof nm;
}

static nd_err write_map(const char *out_path, const char *name, int32_t south, int32_t west,
                        int32_t north, int32_t east, const import_state *st, const uint32_t *renum,
                        uint32_t n_kept)
{
    char real[ND_PATH_MAX];
    char tmp[ND_PATH_MAX + 8];
    FILE *f;
    const raw_node *nodes = (const raw_node *)st->nodes.data;
    const raw_way *ways = (const raw_way *)st->ways.data;
    const uint32_t *refs = (const uint32_t *)st->refs.data;
    const raw_place *places = (const raw_place *)st->places.data;
    size_t i;
    bool ok;

    if (nd_path_resolve(real, sizeof real, out_path) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_snprintf(tmp, sizeof tmp, "%s.part", real) != ND_OK)
        return ND_ERR_TOOLONG;

    f = fopen(tmp, "wb");
    if (f == NULL) {
        nd_log_err(LOG_TAG, "cannot write %s: %s", tmp, strerror(errno));
        return ND_ERR_IO;
    }

    ok = write_header(f, name, south, west, north, east, n_kept, (uint32_t)st->ways.n,
                      (uint32_t)st->refs.n, (uint32_t)st->places.n, (uint32_t)st->names.n);
    for (i = 0u; ok && i < st->nodes.n; i++) {
        if (renum[i] == UINT32_MAX)
            continue;
        ok = put_i32(f, nodes[i].lat) && put_i32(f, nodes[i].lon);
    }
    for (i = 0u; ok && i < st->ways.n; i++) {
        ok = put_u8(f, ways[i].kind) && put_u8(f, ways[i].flags) && put_u16(f, 0u) &&
             put_u32(f, ways[i].first_ref) && put_u32(f, ways[i].n_refs) &&
             put_u32(f, ways[i].name_off);
    }
    for (i = 0u; ok && i < st->refs.n; i++)
        ok = put_u32(f, renum[refs[i]]);
    for (i = 0u; ok && i < st->places.n; i++) {
        ok = put_i32(f, places[i].lat) && put_i32(f, places[i].lon) && put_u8(f, places[i].kind) &&
             put_u8(f, 0u) && put_u16(f, 0u) && put_u32(f, places[i].name_off);
    }
    if (ok && st->names.n > 0u)
        ok = fwrite(st->names.data, 1u, st->names.n, f) == st->names.n;

    if (ok)
        ok = fflush(f) == 0;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        nd_log_err(LOG_TAG, "short write to %s", tmp);
        (void)remove(tmp);
        return ND_ERR_IO;
    }
    if (rename(tmp, real) != 0) {
        nd_log_err(LOG_TAG, "cannot rename %s: %s", tmp, strerror(errno));
        (void)remove(tmp);
        return ND_ERR_IO;
    }
    return ND_OK;
}

nd_err nd_osm_import(const char *xml_path, const char *out_path, const char *name, int32_t south,
                     int32_t west, int32_t north, int32_t east, nd_osm_import_stats *stats,
                     char *why, size_t why_sz)
{
    char real[ND_PATH_MAX];
    char best_name[ND_OSM_NAME_MAX];
    FILE *f = NULL;
    char *line = NULL;
    int64_t *ref_buf = NULL;
    uint32_t *renum = NULL;
    import_state st;
    nd_err rc;
    size_t i;
    uint32_t n_kept = 0u;
    uint32_t n_roads = 0u;
    int32_t best_rank = -1;
    const raw_way *ways;
    const uint32_t *refs;
    const raw_place *places;

    if (xml_path == NULL || out_path == NULL)
        return ND_ERR_INVAL;
    if (nd_path_resolve(real, sizeof real, xml_path) != ND_OK)
        return ND_ERR_TOOLONG;

    table_init(&st.nodes, sizeof(raw_node), ND_OSM_MAX_NODES);
    table_init(&st.places, sizeof(raw_place), ND_OSM_MAX_PLACES);
    table_init(&st.ways, sizeof(raw_way), ND_OSM_MAX_WAYS);
    table_init(&st.refs, sizeof(uint32_t), ND_OSM_MAX_REFS);
    table_init(&st.names, 1u, ND_OSM_MAX_NAMES);
    best_name[0] = '\0';

    /* owned here; freed at done */
    line = malloc(LINE_MAX);
    ref_buf = malloc(WAY_REFS_MAX * sizeof *ref_buf);
    if (line == NULL || ref_buf == NULL) {
        say_why(why, why_sz, cap_reason(ND_ERR_NOMEM));
        rc = ND_ERR_NOMEM;
        goto done;
    }

    /* names[0] is "", so that name_off 0 means "no name" everywhere. */
    rc = table_reserve(&st.names);
    if (rc != ND_OK)
        goto done;
    ((char *)st.names.data)[st.names.n++] = '\0';

    f = fopen(real, "rb");
    if (f == NULL) {
        nd_log_err(LOG_TAG, "cannot read %s: %s", real, strerror(errno));
        say_why(why, why_sz, "The downloaded file could not be read.");
        rc = ND_ERR_IO;
        goto done;
    }

    rc = import_nodes(f, &st, line, why, why_sz);
    if (rc != ND_OK)
        goto done;
    qsort(st.nodes.data, st.nodes.n, sizeof(raw_node), cmp_raw_node);

    /* import_nodes() stopped on the first <way line, which is still in
     * `line`; import_ways() starts by looking at it. */
    rc = import_ways(f, &st, line, ref_buf, why, why_sz);
    if (rc != ND_OK)
        goto done;

    /* Keep only the nodes something refers to. Overpass's (._;>;) returns
     * every node of every matching way, but the tags this app ignores
     * still bring nodes it never draws, and they are the bulk of a big
     * answer. */
    renum = malloc((st.nodes.n > 0u ? st.nodes.n : 1u) * sizeof *renum);
    if (renum == NULL) {
        say_why(why, why_sz, cap_reason(ND_ERR_NOMEM));
        rc = ND_ERR_NOMEM;
        goto done;
    }
    for (i = 0u; i < st.nodes.n; i++)
        renum[i] = UINT32_MAX;
    refs = (const uint32_t *)st.refs.data;
    for (i = 0u; i < st.refs.n; i++)
        renum[refs[i]] = 0u;
    for (i = 0u; i < st.nodes.n; i++) {
        if (renum[i] == 0u)
            renum[i] = n_kept++;
    }

    ways = (const raw_way *)st.ways.data;
    for (i = 0u; i < st.ways.n; i++) {
        if (ways[i].kind >= ND_OSM_KIND_FIRST_ROAD)
            n_roads++;
    }
    if (st.ways.n == 0u) {
        say_why(why, why_sz, "There are no streets in that area.");
        rc = ND_ERR_NOTFOUND;
        goto done;
    }

    /* The most important place is the map's name, which is what the list
     * of installed maps shows and what a person would have typed anyway. */
    places = (const raw_place *)st.places.data;
    for (i = 0u; i < st.places.n; i++) {
        if ((int32_t)places[i].kind > best_rank) {
            best_rank = (int32_t)places[i].kind;
            (void)nd_strlcpy(best_name, (const char *)st.names.data + places[i].name_off,
                             sizeof best_name);
        }
    }
    if (name == NULL || name[0] == '\0')
        name = (best_name[0] != '\0') ? best_name : "Map";

    rc = write_map(out_path, name, south, west, north, east, &st, renum, n_kept);
    if (rc != ND_OK) {
        say_why(why, why_sz, "The map could not be written to the card.");
        goto done;
    }

    if (stats != NULL) {
        stats->n_nodes = n_kept;
        stats->n_ways = (uint32_t)st.ways.n;
        stats->n_roads = n_roads;
        stats->n_places = (uint32_t)st.places.n;
        (void)nd_strlcpy(stats->name, name, sizeof stats->name);
    }
    nd_log(LOG_TAG, "imported %s: %u nodes, %u ways, %u places", name, (unsigned)n_kept,
           (unsigned)st.ways.n, (unsigned)st.places.n);

done:
    if (f != NULL)
        (void)fclose(f);
    free(line);
    free(ref_buf);
    free(renum);
    table_free(&st.nodes);
    table_free(&st.places);
    table_free(&st.ways);
    table_free(&st.refs);
    table_free(&st.names);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Reading a map back
 * ------------------------------------------------------------------ */

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t get_i32(const uint8_t *p)
{
    return (int32_t)get_u32(p);
}

typedef struct {
    int32_t south;
    int32_t west;
    int32_t north;
    int32_t east;
    uint32_t n_nodes;
    uint32_t n_ways;
    uint32_t n_refs;
    uint32_t n_places;
    uint32_t names_len;
    char name[ND_OSM_NAME_MAX];
} map_header;

static nd_err read_header(FILE *f, map_header *h)
{
    uint8_t buf[HDR_SIZE];

    if (fread(buf, 1u, sizeof buf, f) != sizeof buf)
        return ND_ERR_PARSE;
    if (memcmp(buf, ND_OSM_FILE_MAGIC, 4u) != 0)
        return ND_ERR_PARSE;
    if (get_u32(buf + 4) != ND_OSM_FILE_VERSION)
        return ND_ERR_UNSUPPORTED;
    h->south = get_i32(buf + 8);
    h->west = get_i32(buf + 12);
    h->north = get_i32(buf + 16);
    h->east = get_i32(buf + 20);
    h->n_nodes = get_u32(buf + 24);
    h->n_ways = get_u32(buf + 28);
    h->n_refs = get_u32(buf + 32);
    h->n_places = get_u32(buf + 36);
    h->names_len = get_u32(buf + 40);
    memcpy(h->name, buf + 44, ND_OSM_NAME_MAX);
    h->name[ND_OSM_NAME_MAX - 1] = '\0';
    if (h->n_nodes > ND_OSM_MAX_NODES || h->n_ways > ND_OSM_MAX_WAYS ||
        h->n_refs > ND_OSM_MAX_REFS || h->n_places > ND_OSM_MAX_PLACES ||
        h->names_len > ND_OSM_MAX_NAMES || h->names_len == 0u)
        return ND_ERR_PARSE;
    return ND_OK;
}

nd_err nd_osm_map_peek(const char *path, char *name, size_t name_sz, int32_t *south, int32_t *west,
                       int32_t *north, int32_t *east)
{
    char real[ND_PATH_MAX];
    FILE *f;
    map_header h;
    nd_err rc;

    if (path == NULL)
        return ND_ERR_INVAL;
    if (nd_path_resolve(real, sizeof real, path) != ND_OK)
        return ND_ERR_TOOLONG;
    f = fopen(real, "rb");
    if (f == NULL)
        return ND_ERR_NOTFOUND;
    rc = read_header(f, &h);
    (void)fclose(f);
    if (rc != ND_OK)
        return rc;
    if (name != NULL)
        (void)nd_strlcpy(name, h.name, name_sz);
    if (south != NULL)
        *south = h.south;
    if (west != NULL)
        *west = h.west;
    if (north != NULL)
        *north = h.north;
    if (east != NULL)
        *east = h.east;
    return ND_OK;
}

/* Read `n` records of `rec` bytes through one 8 kB buffer, handing each to
 * `decode`. The tables are read this way rather than fread() into the typed
 * arrays because the file is explicit little-endian and the target's
 * alignment rules are not the host's. */
typedef void (*decode_fn)(void *ctx, uint32_t i, const uint8_t *rec);

static nd_err read_records(FILE *f, uint32_t n, size_t rec, decode_fn decode, void *ctx)
{
    uint8_t buf[8192];
    uint32_t done = 0u;
    size_t per = sizeof buf / rec;

    while (done < n) {
        uint32_t want = n - done;
        size_t got;
        size_t i;

        if ((size_t)want > per)
            want = (uint32_t)per;
        got = fread(buf, rec, (size_t)want, f);
        if (got != (size_t)want)
            return ND_ERR_PARSE;
        for (i = 0u; i < got; i++)
            decode(ctx, done + (uint32_t)i, buf + i * rec);
        done += (uint32_t)got;
    }
    return ND_OK;
}

static void decode_node(void *ctx, uint32_t i, const uint8_t *rec)
{
    nd_osm_map *m = (nd_osm_map *)ctx;

    nd_osm_project(get_i32(rec), get_i32(rec + 4), &m->mx[i], &m->my[i]);
}

static void decode_way(void *ctx, uint32_t i, const uint8_t *rec)
{
    nd_osm_map *m = (nd_osm_map *)ctx;
    nd_osm_way *w = &m->ways[i];

    w->kind = rec[0];
    w->flags = rec[1];
    w->first_ref = get_u32(rec + 4);
    w->n_refs = get_u32(rec + 8);
    w->name_off = get_u32(rec + 12);
}

static void decode_ref(void *ctx, uint32_t i, const uint8_t *rec)
{
    ((nd_osm_map *)ctx)->refs[i] = get_u32(rec);
}

static void decode_place(void *ctx, uint32_t i, const uint8_t *rec)
{
    nd_osm_map *m = (nd_osm_map *)ctx;
    nd_osm_place *p = &m->places[i];

    nd_osm_project(get_i32(rec), get_i32(rec + 4), &p->mx, &p->my);
    p->kind = rec[8];
    p->name_off = get_u32(rec + 12);
}

static int cmp_way_kind(const void *a, const void *b)
{
    const nd_osm_way *x = (const nd_osm_way *)a;
    const nd_osm_way *y = (const nd_osm_way *)b;

    if (x->kind != y->kind)
        return (x->kind < y->kind) ? -1 : 1;
    return (x->first_ref < y->first_ref) ? -1 : (x->first_ref > y->first_ref) ? 1 : 0;
}

/* Everything that is derived rather than stored: the bounding boxes, the
 * kind ordering and the kind index. Also where a corrupt file is caught --
 * a ref past the node table or a way past the ref table is refused here,
 * so the renderer never bounds-checks. */
static nd_err finish_map(nd_osm_map *m)
{
    uint32_t i;
    uint32_t kind;
    uint32_t at = 0u;

    for (i = 0u; i < m->n_refs; i++) {
        if (m->refs[i] >= m->n_nodes)
            return ND_ERR_PARSE;
    }
    for (i = 0u; i < m->n_ways; i++) {
        nd_osm_way *w = &m->ways[i];
        uint32_t j;

        if (w->kind >= ND_OSM_KIND_COUNT || w->n_refs < 2u || w->first_ref > m->n_refs ||
            w->n_refs > m->n_refs - w->first_ref || w->name_off >= m->names_len)
            return ND_ERR_PARSE;
        w->bx0 = INT32_MAX;
        w->by0 = INT32_MAX;
        w->bx1 = INT32_MIN;
        w->by1 = INT32_MIN;
        for (j = 0u; j < w->n_refs; j++) {
            uint32_t n = m->refs[w->first_ref + j];

            w->bx0 = nd_min32(w->bx0, m->mx[n]);
            w->by0 = nd_min32(w->by0, m->my[n]);
            w->bx1 = nd_max32(w->bx1, m->mx[n]);
            w->by1 = nd_max32(w->by1, m->my[n]);
        }
    }
    for (i = 0u; i < m->n_places; i++) {
        if (m->places[i].name_off >= m->names_len)
            return ND_ERR_PARSE;
    }
    m->names[m->names_len - 1u] = '\0';

    qsort(m->ways, m->n_ways, sizeof *m->ways, cmp_way_kind);
    for (kind = 0u; kind < ND_OSM_KIND_COUNT; kind++) {
        m->kind_begin[kind] = at;
        while (at < m->n_ways && m->ways[at].kind == kind)
            at++;
    }
    m->kind_begin[ND_OSM_KIND_COUNT] = at;

    nd_osm_project(m->north, m->west, &m->bx0, &m->by0);
    nd_osm_project(m->south, m->east, &m->bx1, &m->by1);
    m->metres_per_unit = nd_osm_metres_per_unit((int32_t)(((int64_t)m->by0 + m->by1) / 2));
    return ND_OK;
}

nd_err nd_osm_map_load(const char *path, nd_osm_map **out)
{
    char real[ND_PATH_MAX];
    FILE *f = NULL;
    nd_osm_map *m = NULL;
    map_header h;
    nd_err rc;

    if (path == NULL || out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;
    if (nd_path_resolve(real, sizeof real, path) != ND_OK)
        return ND_ERR_TOOLONG;

    f = fopen(real, "rb");
    if (f == NULL)
        return ND_ERR_NOTFOUND;
    rc = read_header(f, &h);
    if (rc != ND_OK)
        goto fail;

    /* owned by the caller; released by nd_osm_map_free() */
    m = calloc(1u, sizeof *m);
    if (m == NULL) {
        rc = ND_ERR_NOMEM;
        goto fail;
    }
    (void)nd_strlcpy(m->name, h.name, sizeof m->name);
    (void)nd_strlcpy(m->path, path, sizeof m->path);
    m->south = h.south;
    m->west = h.west;
    m->north = h.north;
    m->east = h.east;
    m->n_nodes = h.n_nodes;
    m->n_ways = h.n_ways;
    m->n_refs = h.n_refs;
    m->n_places = h.n_places;
    m->names_len = h.names_len;

    /* Two int32 per node, 16 bytes per way (with the derived box it is 24),
     * 4 per ref, 16 per place: a 100,000-node town is about 2 MB. */
    m->mx = malloc((size_t)(h.n_nodes > 0u ? h.n_nodes : 1u) * sizeof *m->mx);
    m->my = malloc((size_t)(h.n_nodes > 0u ? h.n_nodes : 1u) * sizeof *m->my);
    m->ways = malloc((size_t)(h.n_ways > 0u ? h.n_ways : 1u) * sizeof *m->ways);
    m->refs = malloc((size_t)(h.n_refs > 0u ? h.n_refs : 1u) * sizeof *m->refs);
    m->places = malloc((size_t)(h.n_places > 0u ? h.n_places : 1u) * sizeof *m->places);
    m->names = malloc((size_t)h.names_len);
    if (m->mx == NULL || m->my == NULL || m->ways == NULL || m->refs == NULL || m->places == NULL ||
        m->names == NULL) {
        rc = ND_ERR_NOMEM;
        goto fail;
    }

    rc = read_records(f, h.n_nodes, 8u, decode_node, m);
    if (rc == ND_OK)
        rc = read_records(f, h.n_ways, 16u, decode_way, m);
    if (rc == ND_OK)
        rc = read_records(f, h.n_refs, 4u, decode_ref, m);
    if (rc == ND_OK)
        rc = read_records(f, h.n_places, 16u, decode_place, m);
    if (rc == ND_OK && fread(m->names, 1u, (size_t)h.names_len, f) != (size_t)h.names_len)
        rc = ND_ERR_PARSE;
    if (rc != ND_OK)
        goto fail;
    (void)fclose(f);
    f = NULL;

    rc = finish_map(m);
    if (rc != ND_OK)
        goto fail;

    *out = m;
    return ND_OK;

fail:
    if (f != NULL)
        (void)fclose(f);
    nd_osm_map_free(m);
    nd_log_err(LOG_TAG, "cannot load %s: %s", real, nd_strerror(rc));
    return rc;
}

void nd_osm_map_free(nd_osm_map *m)
{
    if (m == NULL)
        return;
    free(m->mx);
    free(m->my);
    free(m->ways);
    free(m->refs);
    free(m->places);
    free(m->names);
    free(m->adj_start);
    free(m->adj);
    free(m);
}

/* ------------------------------------------------------------------ *
 * The data directory
 * ------------------------------------------------------------------ */

static bool has_map_ext(const char *name)
{
    size_t n = strlen(name);
    size_t e = strlen(ND_OSMAND_MAP_EXT);

    return n > e && strcmp(name + n - e, ND_OSMAND_MAP_EXT) == 0;
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

size_t nd_osm_list_maps(char (*paths)[512], size_t max)
{
    char real[ND_PATH_MAX];
    DIR *d;
    struct dirent *e;
    size_t n = 0u;

    if (paths == NULL || max == 0u)
        return 0u;
    if (nd_path_resolve(real, sizeof real, ND_OSMAND_DATA_DIR) != ND_OK)
        return 0u;
    d = opendir(real);
    if (d == NULL)
        return 0u;
    while (n < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !has_map_ext(e->d_name))
            continue;
        if (nd_snprintf(paths[n], 512u, "%s/%s", ND_OSMAND_DATA_DIR, e->d_name) != ND_OK)
            continue;
        n++;
    }
    (void)closedir(d);
    qsort(paths, n, 512u, cmp_path);
    return n;
}

nd_err nd_osm_new_map_path(const char *name, char *out, size_t out_sz)
{
    char clean[ND_OSM_NAME_MAX];
    size_t o = 0u;
    size_t i;
    int32_t suffix;

    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    if (name == NULL)
        name = "";

    /* Letters, digits, space, dash and underscore; anything else, which
     * includes every byte of a non-ASCII name, becomes an underscore. The
     * display name in the header keeps the real spelling. */
    for (i = 0u; name[i] != '\0' && o + 1u < sizeof clean; i++) {
        unsigned char ch = (unsigned char)name[i];

        if (isalnum(ch) || ch == ' ' || ch == '-' || ch == '_')
            clean[o++] = (char)ch;
        else
            clean[o++] = '_';
    }
    clean[o] = '\0';
    while (o > 0u && (clean[o - 1u] == ' ' || clean[o - 1u] == '_'))
        clean[--o] = '\0';
    if (o == 0u)
        (void)nd_strlcpy(clean, "Map", sizeof clean);

    for (suffix = 1; suffix < 100; suffix++) {
        nd_err rc;

        if (suffix == 1)
            rc = nd_snprintf(out, out_sz, "%s/%s%s", ND_OSMAND_DATA_DIR, clean, ND_OSMAND_MAP_EXT);
        else
            rc = nd_snprintf(out, out_sz, "%s/%s %d%s", ND_OSMAND_DATA_DIR, clean, (int)suffix,
                             ND_OSMAND_MAP_EXT);
        if (rc != ND_OK)
            return rc;
        if (!nd_path_exists(out))
            return ND_OK;
    }
    return ND_ERR_BUSY;
}
