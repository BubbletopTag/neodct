/* osmand_app.h -- the shape of the OsmAnd app, app id 13.
 *
 * An offline map of OpenStreetMap data, drawn in OpenStreetMap's own colours
 * inside NeoDCT's own chrome, with a downloader and a router. Everything a
 * test needs to reach is declared here rather than left static, so
 * test/unit/test_osmand_app.c can dlopen() the BUILT app.so and assert on the
 * artefact that ships.
 *
 * ============ WHAT THE APP IS, IN ONE PARAGRAPH ============
 *
 * OsmAnd on a phone with a real GPS draws vector map data it downloaded
 * earlier and routes over it without a network. This is the same idea cut to
 * a 240x175 panel, sixteen keys, no GPS and 64 MB: an area around the map's
 * centre is fetched from the Overpass API once, boiled down to a compact
 * binary file on the memory card, and from then on every pan, zoom, search
 * and route runs entirely off that file. The Overpass XML never stays on the
 * card and is never parsed twice.
 *
 * ============ WHERE THE DATA LIVES ============
 *
 *     /NeoDCT/User/sdcard/apps/OsmAnd/data/<name>.ndmap
 *
 * On the card and not on /NeoDCT/User, for the reason nd_paths.h gives about
 * installed apps: the user partition is eight megabytes on the Luckfox and a
 * town is a few of them. The directory is created by this app the first
 * time it is needed, through nd_mkdir_p() so the host tests land in their
 * scratch root rather than in a developer's /NeoDCT.
 *
 * ============ SIXTEEN KEYS, AND NO LEFT OR RIGHT ============
 *
 * The map needs two axes and a zoom, and the rocker has one axis. So the
 * number pad is the d-pad, the way MusicPlayer, Messages and Calendar
 * already use it:
 *
 *     1  zoom out        2  pan up          3  zoom in
 *     4  pan left        5  jump to mark    6  pan right
 *     7  (nothing)       8  pan down        9  (nothing)
 *     *  zoom out        0  (nothing)       #  zoom in
 *
 * Up and Down do what 2 and 8 do. Left and Right exist only on a development
 * QWERTY keyboard and do what 4 and 6 do. NaviKey opens the options list; C
 * leaves the app. The whole map is reachable from the phone's keys and the
 * QEMU keyboard is merely comfortable.
 *
 * ============ NO GPS YET ============
 *
 * There is no position. The map's centre is the only cursor: "Start here"
 * and "Route to here" mark whatever is under the crosshair, and the start
 * point defaults to wherever the map was when the app opened. When a
 * receiver arrives, its fix becomes a third mark and the default start, and
 * nothing else about this file has to change.
 */

#ifndef ND_OSMAND_APP_H_INCLUDED
#define ND_OSMAND_APP_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App id 13 -- manifest.json, and the "13" the options list's header draws.
 * The first free slot after the stock 1..12. A string as well as a number
 * because the widgets take the root id as text and the two must not be able
 * to drift. */
#define ND_OSMAND_APP_ID   13
#define ND_OSMAND_APP_ROOT "13"

extern const char *const nd_osmand_app_title;

/* The one directory this app writes. Absolute and load-bearing (AGENTS.md);
 * every open goes through nd_path_resolve(). */
#define ND_OSMAND_DATA_DIR "/NeoDCT/User/sdcard/apps/OsmAnd/data"
#define ND_OSMAND_MAP_EXT  ".ndmap"

/* How many maps are loaded at once. Each is held whole in memory, so this is
 * a RAM budget rather than a preference: six towns is the most this phone
 * should be asked to hold beside its own UI. */
#define ND_OSMAND_MAX_MAPS 6

/* ------------------------------------------------------------------ *
 * The projection
 * ------------------------------------------------------------------ *
 *
 * Web Mercator, as every slippy map uses, with one fixed reference zoom so
 * that a node's position is an integer computed once at load and every
 * zoom level after that is a shift. Zoom 20 makes the world 2^28 pixels
 * wide, which fits an int32 with room for a view that hangs off its edge.
 * A pixel at zoom z is (1 << (20 - z)) of these units.
 */

#define ND_OSM_MERC_ZOOM 20
#define ND_OSM_ZOOM_MIN  10
#define ND_OSM_ZOOM_MAX  18

/* Degrees are carried as integers scaled by 1e7 -- about a centimetre --
 * because that is what OSM itself stores and it survives a round trip
 * through a file without any question about float formatting. */
#define ND_OSM_DEG 10000000

/* lat/lon (1e7 degrees) -> reference-zoom world pixel. Latitude is clamped
 * to Mercator's +-85.05 degrees, which no map this app downloads reaches. */
void nd_osm_project(int32_t lat, int32_t lon, int32_t *mx, int32_t *my);

/* The inverse, in floating degrees, for the settings file and the bounding
 * box of a download. */
void nd_osm_unproject(int32_t mx, int32_t my, double *lat, double *lon);

/* Metres per reference-zoom unit at a given world y. Mercator stretches
 * with latitude, so a route's length is measured with this at the map's own
 * latitude rather than at the equator. */
double nd_osm_metres_per_unit(int32_t my);

/* A square of `km` kilometres a side centred on a point, as a bounding box
 * in 1e7 degrees, clamped to the world. What "Download this area" asks
 * Overpass for. */
void nd_osm_bbox_around(double lat, double lon, double km, int32_t *south, int32_t *west,
                        int32_t *north, int32_t *east);

/* ------------------------------------------------------------------ *
 * What is on the map
 * ------------------------------------------------------------------ *
 *
 * A way is one polyline or one closed ring with a kind, a few flags and a
 * name. Kinds are ordered as they are drawn: every line kind after every
 * area kind, and every road from the least important to the most, so that
 * a motorway is painted over the residential street it crosses -- which is
 * the order OpenStreetMap's own Carto stylesheet uses.
 */

typedef enum {
    ND_OSM_KIND_NONE = 0,

    /* Areas, filled. */
    ND_OSM_KIND_RESIDENTIAL_LAND, /* landuse=residential                     */
    ND_OSM_KIND_INDUSTRIAL,       /* landuse=industrial                      */
    ND_OSM_KIND_RETAIL,           /* landuse=retail|commercial               */
    ND_OSM_KIND_FARMLAND,         /* landuse=farmland                        */
    ND_OSM_KIND_GRASS,            /* leisure=park|garden|pitch, landuse=grass|meadow */
    ND_OSM_KIND_WOOD,             /* natural=wood, landuse=forest            */
    ND_OSM_KIND_WATER,            /* natural=water, waterway=riverbank, ... */
    ND_OSM_KIND_BUILDING,         /* building=*                              */

    /* Lines, stroked. */
    ND_OSM_KIND_STREAM,      /* waterway=stream                          */
    ND_OSM_KIND_RIVER,       /* waterway=river|canal                     */
    ND_OSM_KIND_RAILWAY,     /* railway=rail                             */
    ND_OSM_KIND_PATH,        /* highway=path|footway|cycleway|steps|...  */
    ND_OSM_KIND_TRACK,       /* highway=track                            */
    ND_OSM_KIND_SERVICE,     /* highway=service                          */
    ND_OSM_KIND_RESIDENTIAL, /* highway=residential|unclassified|living_street */
    ND_OSM_KIND_TERTIARY,
    ND_OSM_KIND_SECONDARY,
    ND_OSM_KIND_PRIMARY,
    ND_OSM_KIND_TRUNK,
    ND_OSM_KIND_MOTORWAY,

    ND_OSM_KIND_COUNT
} nd_osm_kind;

#define ND_OSM_KIND_FIRST_LINE ND_OSM_KIND_STREAM
#define ND_OSM_KIND_FIRST_ROAD ND_OSM_KIND_PATH

/* Per-way flags. */
#define ND_OSM_FLAG_ONEWAY  0x01u /* oneway=yes, or junction=roundabout          */
#define ND_OSM_FLAG_AREA    0x02u /* a closed ring to fill                       */
#define ND_OSM_FLAG_NO_CAR  0x04u /* access=no|private, motor_vehicle=no         */
#define ND_OSM_FLAG_NO_FOOT 0x08u /* foot=no, or a motorway                      */

/* One way, as loaded. The bounding box is in reference-zoom units and is
 * computed at load rather than stored: it exists so a frame can skip a way
 * with one comparison instead of projecting every node of it. */
typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint32_t first_ref; /* index into refs[]                           */
    uint32_t n_refs;
    uint32_t name_off; /* into names[], 0 for no name (names[0] is "") */
    int32_t bx0;
    int32_t by0;
    int32_t bx1;
    int32_t by1;
} nd_osm_way;

/* A named point: place=city|town|village|suburb|hamlet|neighbourhood. The
 * only kind of point of interest this app downloads, because it is what a
 * map at low zoom is for and what "Find" needs to be useful before a
 * street name is known. */
typedef enum {
    ND_OSM_PLACE_OTHER = 0,
    ND_OSM_PLACE_NEIGHBOURHOOD,
    ND_OSM_PLACE_HAMLET,
    ND_OSM_PLACE_SUBURB,
    ND_OSM_PLACE_VILLAGE,
    ND_OSM_PLACE_TOWN,
    ND_OSM_PLACE_CITY,
    ND_OSM_PLACE_COUNT
} nd_osm_place_kind;

typedef struct {
    int32_t mx;
    int32_t my;
    uint8_t kind;
    uint32_t name_off;
} nd_osm_place;

/* A directed edge of the routing graph, built lazily by nd_osm_graph_build().
 * `way` rather than a length because the length depends on the mode and the
 * name is wanted for directions; both come off the way. */
typedef struct {
    uint32_t to;
    uint32_t way;
} nd_osm_edge;

#define ND_OSM_NAME_MAX 64

/* One loaded map. Owned by the caller of nd_osm_map_load(); free with
 * nd_osm_map_free(). */
typedef struct {
    char name[ND_OSM_NAME_MAX];
    char path[512];

    /* The area asked of Overpass, 1e7 degrees, and the same in projection. */
    int32_t south;
    int32_t west;
    int32_t north;
    int32_t east;
    int32_t bx0;
    int32_t by0;
    int32_t bx1;
    int32_t by1;

    uint32_t n_nodes;
    int32_t *mx; /* reference-zoom world pixel, per node   */
    int32_t *my;

    uint32_t n_ways;
    nd_osm_way *ways; /* sorted by kind at load; see kind_begin */
    uint32_t kind_begin[ND_OSM_KIND_COUNT + 1];

    uint32_t n_refs;
    uint32_t *refs;

    uint32_t n_places;
    nd_osm_place *places;

    uint32_t names_len;
    char *names;

    /* The graph. NULL until a route is asked for. */
    uint32_t *adj_start; /* n_nodes + 1 */
    nd_osm_edge *adj;
    uint32_t n_adj;
    double metres_per_unit;
} nd_osm_map;

/* The name of a way or a place, never NULL: "" for an unnamed one. */
const char *nd_osm_way_name(const nd_osm_map *m, const nd_osm_way *w);
const char *nd_osm_place_name(const nd_osm_map *m, const nd_osm_place *p);

/* True when a reference-zoom point falls inside the map's downloaded area. */
bool nd_osm_map_contains(const nd_osm_map *m, int32_t mx, int32_t my);

/* ------------------------------------------------------------------ *
 * The file
 * ------------------------------------------------------------------ *
 *
 * .ndmap is little-endian, explicit-byte, fixed-layout: a header, the node
 * table, the way table, the reference table, the place table and a string
 * pool. No compression -- the card has room and the phone has no CPU to
 * spare on decompressing a town every time the app opens. The layout is in
 * mapdata.c beside the writer.
 */

#define ND_OSM_FILE_MAGIC   "NDMP"
#define ND_OSM_FILE_VERSION 1u

/* Ceilings on what one map may hold. Overpass answers for a large city
 * exceed these; the download sizes offered are chosen so that a town does
 * not. A file over any of them is refused rather than truncated, because a
 * map with its northern half missing is worse than a message. */
#define ND_OSM_MAX_NODES  400000u
#define ND_OSM_MAX_WAYS   120000u
#define ND_OSM_MAX_REFS   1000000u
#define ND_OSM_MAX_PLACES 4096u
#define ND_OSM_MAX_NAMES  (1024u * 1024u)

/* What an import produced, for the "Saved" dialog and the tests. */
typedef struct {
    uint32_t n_nodes;
    uint32_t n_ways;
    uint32_t n_roads;
    uint32_t n_places;
    char name[ND_OSM_NAME_MAX]; /* the best place name found, or "" */
} nd_osm_import_stats;

/* Read an Overpass API XML answer and write it as a .ndmap. `name` is the
 * map's display name, stored in the header; an empty one is replaced by the
 * most important place in the data, and failing that by "Map". `south` ..
 * `east` are the bounding box that was asked for. Paths are ND_ROOT-resolved.
 *
 * Streaming: the XML is read line by line, twice (nodes, then ways), and
 * never held in memory. `why` receives a sentence for the screen. */
nd_err nd_osm_import(const char *xml_path, const char *out_path, const char *name, int32_t south,
                     int32_t west, int32_t north, int32_t east, nd_osm_import_stats *stats,
                     char *why, size_t why_sz);

/* Load a .ndmap whole. ND_ERR_PARSE for a file that is not one. */
nd_err nd_osm_map_load(const char *path, nd_osm_map **out);
void nd_osm_map_free(nd_osm_map *m);

/* Just the name and bounds from the header, for the installed-maps list. */
nd_err nd_osm_map_peek(const char *path, char *name, size_t name_sz, int32_t *south, int32_t *west,
                       int32_t *north, int32_t *east);

/* The .ndmap files in ND_OSMAND_DATA_DIR, as "<dir>/<file>" paths, sorted by
 * name. Returns how many were written; never more than `max`. */
size_t nd_osm_list_maps(char (*paths)[512], size_t max);

/* A path under the data directory for a new map called `name`, with the
 * characters a filename cannot carry replaced and a " 2", " 3" suffix when
 * that name is already taken. */
nd_err nd_osm_new_map_path(const char *name, char *out, size_t out_sz);

/* Turn one Overpass way's tags into a kind and flags. Exposed because it is
 * the whole of what decides how a street is drawn and routed, and it is
 * checkable without a file. `k` and `v` are one tag; call it once per tag
 * on the same struct, then nd_osm_classify_finish(). */
typedef struct {
    uint8_t kind;
    uint8_t flags;
    bool closed;   /* first ref == last ref; set by the caller     */
    bool area_tag; /* area=yes                                      */
    bool highway;  /* any highway=* seen, closed or not             */
} nd_osm_classify;

void nd_osm_classify_tag(nd_osm_classify *c, const char *k, const char *v);
void nd_osm_classify_finish(nd_osm_classify *c);

/* place=<v> -> a rank, ND_OSM_PLACE_OTHER for one this app does not draw. */
nd_osm_place_kind nd_osm_place_rank(const char *v);

/* ------------------------------------------------------------------ *
 * The view, and the key map
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t cx; /* the centre, reference-zoom units */
    int32_t cy;
    int32_t zoom;
} nd_osm_view;

typedef struct {
    int32_t mx;
    int32_t my;
    bool set;
} nd_osm_mark;

typedef enum {
    ND_OSM_NAV_NONE = 0, /* the key meant nothing here; do not redraw */
    ND_OSM_NAV_MOVED,    /* the view changed                          */
    ND_OSM_NAV_OPTIONS,  /* NaviKey                                   */
    ND_OSM_NAV_BACK      /* C                                         */
} nd_osm_nav;

/* One key against the view. The header block above is the specification;
 * this is it in one function, so the map can be tested without a panel.
 * `map_w` and `map_h` are the map surface in pixels -- a pan is a quarter of
 * it -- and `jump` is where 5 goes, or NULL for nowhere.
 *
 * ND_OSM_NAV_MOVED is returned even when a zoom was already at its limit,
 * so that holding a key against the end of the range is a steady screen
 * rather than a dead one. */
nd_osm_nav nd_osm_map_key(int32_t key, nd_osm_view *v, int32_t map_w, int32_t map_h,
                          const nd_osm_mark *jump);

/* The map surface: every row above the softkey bar, the full panel width.
 * There is no title, breadcrumb or divider on this screen -- render.c's
 * header says why -- so `top` is 0 on every panel. */
void nd_osm_map_geometry(const nd_ui *ui, int32_t *top, int32_t *w, int32_t *h);

/* Reference-zoom point -> pixel on a map surface of w x h under `v`. Pure. */
void nd_osm_view_to_screen(const nd_osm_view *v, int32_t w, int32_t h, int32_t mx, int32_t my,
                           int32_t *sx, int32_t *sy);

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

/* The renderer's working memory: a way's projected points, the polygon
 * filler's edge table, the frame's label boxes. About 160 kB, allocated
 * ONCE by the app and lent to every frame, so the render path never calls
 * malloc (CODING-STANDARDS.md section 4). */
typedef struct nd_osm_scratch nd_osm_scratch;
nd_osm_scratch *nd_osm_scratch_new(void);
void nd_osm_scratch_free(nd_osm_scratch *s);

/* Everything one frame of the map needs, gathered so that the animated-
 * wallpaper repainter and the key loop draw the same thing. */
typedef struct {
    nd_ui *ui;
    nd_osm_scratch *scratch;
    nd_osm_map *const *maps;
    size_t n_maps;
    nd_osm_view view;
    nd_osm_mark start;
    nd_osm_mark dest;
    /* The route, as nodes of maps[route_map]; n_route 0 for none. */
    const uint32_t *route;
    uint32_t n_route;
    size_t route_map;
} nd_osm_scene;

/* Draw the whole map screen -- the map, the marks, the route, the
 * crosshair, the scale bar -- and present it. Paints rows
 * 0..content_bottom-1 ONLY, so a caller's nd_softkey_update(..., false)
 * survives into the frame, the same contract nd_vlist_draw() offers.
 * `surface` is the caller's map-sized RGB image, so nothing is allocated
 * per frame. */
void nd_osm_scene_draw(const nd_osm_scene *s, nd_image *surface);

/* Draw only the map layers into `surface` -- no chrome. What the frame
 * test renders. */
void nd_osm_render(const nd_osm_scene *s, nd_image *surface);

/* The scale bar's length in metres for a view: the largest of 1, 2, 5 x 10^n
 * that fits in a third of the surface width. Pure. */
int32_t nd_osm_scale_metres(const nd_osm_view *v, int32_t w);

/* ------------------------------------------------------------------ *
 * Routing
 * ------------------------------------------------------------------ */

typedef enum { ND_OSM_MODE_CAR = 0, ND_OSM_MODE_FOOT, ND_OSM_MODE_COUNT } nd_osm_mode;
extern const char *const nd_osmand_mode_names[ND_OSM_MODE_COUNT];

/* Build the adjacency lists. Idempotent; ND_ERR_NOMEM is the only failure. */
nd_err nd_osm_graph_build(nd_osm_map *m);

/* The routable node nearest a point, for `mode`, or UINT32_MAX when the map
 * has no roads. *dist_out receives the distance in metres. */
uint32_t nd_osm_nearest_node(const nd_osm_map *m, int32_t mx, int32_t my, nd_osm_mode mode,
                             double *dist_out);

/* A route: node indices from start to end, and its length. Heap; free with
 * nd_osm_route_free(). */
typedef struct {
    uint32_t *nodes;
    uint32_t n_nodes;
    double metres;
    double seconds;
} nd_osm_route;

/* Dijkstra from `from` to `to`. ND_ERR_NOTFOUND when nothing connects them,
 * which on a map cut from a bounding box is an ordinary answer. */
nd_err nd_osm_route_find(nd_osm_map *m, uint32_t from, uint32_t to, nd_osm_mode mode,
                         nd_osm_route *out);
void nd_osm_route_free(nd_osm_route *r);

/* Speed, in metres per second, for a way kind under a mode; 0 when that kind
 * may not be used at all. Pure, and the whole of the routing policy. */
double nd_osm_speed(nd_osm_mode mode, uint8_t kind, uint8_t flags);

/* ---- directions ---- */

typedef enum {
    ND_OSM_TURN_START = 0,
    ND_OSM_TURN_STRAIGHT,
    ND_OSM_TURN_LEFT,
    ND_OSM_TURN_RIGHT,
    ND_OSM_TURN_SHARP_LEFT,
    ND_OSM_TURN_SHARP_RIGHT,
    ND_OSM_TURN_ARRIVE
} nd_osm_turn;

typedef struct {
    nd_osm_turn turn;
    uint32_t node;  /* where the step begins                  */
    int32_t metres; /* how far this step runs                 */
    char road[48];  /* the way's name, or "" when unnamed     */
} nd_osm_step;

#define ND_OSM_STEPS_MAX 64

/* One per nd_osm_turn, in that order: the big line of a directions page. */
extern const char *const nd_osmand_turn_names[ND_OSM_TURN_ARRIVE + 1];

/* Heading change in degrees (-180, 180] -> a turn. Pure. */
nd_osm_turn nd_osm_turn_for(double delta_deg);

/* Fold a route into steps, one per change of road name, capped. The last
 * step is always ND_OSM_TURN_ARRIVE. Returns how many were written. */
size_t nd_osm_route_steps(const nd_osm_map *m, const nd_osm_route *r, nd_osm_step *out, size_t max);

/* "Right 450 m" -- the line under a directions page's title, which is the
 * road. The turn and the distance are short and go on the one line the
 * PagedList draws a value on; the road is the item, which it wraps. */
void nd_osm_step_label(const nd_osm_step *s, char *out, size_t out_sz);
void nd_osm_format_distance(double metres, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * Downloading
 * ------------------------------------------------------------------ */

#define ND_OSM_OVERPASS_URL "https://overpass-api.de/api/interpreter"
#define ND_OSM_USER_AGENT   "NeoDCT-OsmAnd/1.0 (+https://github.com/BubbletopTag/neodct)"

/* How much of an area to ask for. Three levels, because they are three
 * different sizes of answer for the same box, and the ceilings above are
 * what decide which is possible:
 *
 *   FULL          everything the app draws, buildings included: a town
 *   NO_BUILDINGS  the same without building outlines: a larger town
 *   MAIN_ROADS    motorway to tertiary, rivers and place names only: a
 *                 region. Measured on a 140 x 33 km corridor of rural Ohio,
 *                 tertiary roads and above were 66,000 nodes and lakes
 *                 another 47,000, which is what puts the lakes and the
 *                 railways out of a box four times that size. */
typedef enum {
    ND_OSM_DETAIL_FULL = 0,
    ND_OSM_DETAIL_NO_BUILDINGS,
    ND_OSM_DETAIL_MAIN_ROADS
} nd_osm_detail;

/* The Overpass QL for a bounding box at a level of detail. */
nd_err nd_osm_fetch_query(char *out, size_t out_sz, int32_t south, int32_t west, int32_t north,
                          int32_t east, nd_osm_detail detail);

/* Bytes landed so far. `bytes` only grows. */
typedef void (*nd_osm_fetch_progress_fn)(void *ctx, int64_t bytes);

/* POST the query in `query_path` to Overpass and write the answer to
 * `out_path`, through /usr/bin/curl found on PATH -- the same transport and
 * the same test seam as lib/nd_remote.c. Both paths are ND_ROOT-resolved.
 * Returns ND_ERR_IO with `why` filled in for anything curl refused, and
 * ND_ERR_BUSY when SIGTERM arrived mid-transfer and curl was stopped. */
nd_err nd_osm_fetch(const char *query_path, const char *out_path, nd_osm_fetch_progress_fn fn,
                    void *ctx, char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * The lists
 * ------------------------------------------------------------------ */

/* NaviKey on the map. */
#define ND_OSMAND_OPTIONS_COUNT 9
typedef enum {
    ND_OSMAND_OPT_ROUTE_HERE = 0,
    ND_OSMAND_OPT_START_HERE,
    ND_OSMAND_OPT_DIRECTIONS,
    ND_OSMAND_OPT_CLEAR_ROUTE,
    ND_OSMAND_OPT_FIND,
    ND_OSMAND_OPT_GOTO,
    ND_OSMAND_OPT_DOWNLOAD,
    ND_OSMAND_OPT_MAPS,
    ND_OSMAND_OPT_MODE
} nd_osmand_option;
extern const char *const nd_osmand_options[ND_OSMAND_OPTIONS_COUNT];

/* The four download sizes: a side in kilometres and the detail asked for.
 * The fourth is the one that makes a journey possible: a town map cannot
 * route to the next town, and a 140 km square of main roads can -- Mount
 * Vernon to Scio, Ohio, is 140 km of US 36 and US 250 and fits one. */
#define ND_OSMAND_SIZES_COUNT 4
extern const char *const nd_osmand_size_names[ND_OSMAND_SIZES_COUNT];
extern const double nd_osmand_size_km[ND_OSMAND_SIZES_COUNT];
extern const nd_osm_detail nd_osmand_size_detail[ND_OSMAND_SIZES_COUNT];

/* Settings keys. The view is saved on exit so the map opens where it was
 * left; a map that forgot would open on a town you were not looking at. */
#define ND_SET_OSMAND_LAT  "osmand.lat"
#define ND_SET_OSMAND_LON  "osmand.lon"
#define ND_SET_OSMAND_ZOOM "osmand.zoom"
#define ND_SET_OSMAND_MODE "osmand.mode"

/* Where a phone with nothing saved opens. Somewhere real rather than 0,0
 * in the Gulf of Guinea, so the first "Download this area" fetches a
 * town and the blank screen has a reason. */
#define ND_OSMAND_DEFAULT_LAT  53.3498
#define ND_OSMAND_DEFAULT_LON  (-6.2603)
#define ND_OSMAND_DEFAULT_ZOOM 15

/* What it says. Every one is written to the MessageDialog's budget -- a
 * title line and three at 14 px -- because it truncates rather than
 * scrolling. */
extern const char *const nd_osmand_msg_no_maps;
extern const char *const nd_osmand_msg_no_start;
extern const char *const nd_osmand_msg_no_route;
extern const char *const nd_osmand_msg_off_map;
extern const char *const nd_osmand_msg_two_maps;
extern const char *const nd_osmand_msg_no_card;
extern const char *const nd_osmand_msg_too_many;
extern const char *const nd_osmand_msg_not_found;

#ifdef __cplusplus
}
#endif

#endif /* ND_OSMAND_APP_H_INCLUDED */
