/* test_osmand_app.c -- the OsmAnd app, app id 13.
 *
 * dlopen()s the BUILT app.so so the test exercises the artefact that ships
 * rather than a second copy compiled with different flags.
 *
 * ============ WHAT THIS PINS, AND WHY EACH ONE ============
 *
 *  1. The rows and the strings, in the order the screens page through them.
 *  2. The projection: a round trip through Web Mercator, the metre scale,
 *     and the bounding box a download asks for.
 *  3. The tag classifier -- the whole of what decides how a street is
 *     drawn and routed, checkable without a file.
 *  4. THE KEY MAP. The phone's keypad is sixteen keys with no left and no
 *     right, and the test machine has neither that keypad nor a way to
 *     simulate a person using one. So the map is a pure function and this
 *     drives it directly, including a case asserting every movement is
 *     reachable WITHOUT the two keycodes the hardware does not have.
 *  5. The import: neodct/tests/osmand/town.osm through the real parser
 *     into a real .ndmap, loaded back, with every count derived from the
 *     fixture by hand.
 *  6. Routing over that town: one-way streets honoured by a car and not by
 *     a walker, a footpath taken on foot and not by car, and the directions
 *     a route folds into.
 *  7. The download, through a stand-in `curl` first on PATH -- the same
 *     seam test_remote.c uses -- so the real argv, spawn and progress poll
 *     run, and then the real import on what arrived.
 *  8. The frame: the town rendered at the panel's size has water where the
 *     lake is, grass where the park is, and the chrome above and below it
 *     is NeoDCT's, not the map's.
 *  9. That C leaves the map, and the SIGTERM teardown contract (nd_app.h).
 *
 * There is no golden frame for the map and there should not be one:
 * CODING-STANDARDS.md section 7 is explicit that a new screen's test is its
 * unit test and not a picture of itself that can only ever agree with it.
 */

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_paths.h"
#include "nd_widgets.h"

#include "smallapp_test.h"

#include "../../apps/OsmAnd/osmand_app.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    const char *const *title;
    const char *const *options;
    const char *const *size_names;
    const char *const *mode_names;
    const char *const *turn_names;
    void (*project)(int32_t, int32_t, int32_t *, int32_t *);
    void (*unproject)(int32_t, int32_t, double *, double *);
    double (*metres_per_unit)(int32_t);
    void (*bbox_around)(double, double, double, int32_t *, int32_t *, int32_t *, int32_t *);
    void (*classify_tag)(nd_osm_classify *, const char *, const char *);
    void (*classify_finish)(nd_osm_classify *);
    nd_osm_place_kind (*place_rank)(const char *);
    nd_osm_nav (*map_key)(int32_t, nd_osm_view *, int32_t, int32_t, const nd_osm_mark *);
    void (*map_geometry)(const nd_ui *, int32_t *, int32_t *, int32_t *);
    int32_t (*scale_metres)(const nd_osm_view *, int32_t);
    void (*view_to_screen)(const nd_osm_view *, int32_t, int32_t, int32_t, int32_t, int32_t *,
                           int32_t *);
    nd_err (*import)(const char *, const char *, const char *, int32_t, int32_t, int32_t, int32_t,
                     nd_osm_import_stats *, char *, size_t);
    nd_err (*map_load)(const char *, nd_osm_map **);
    void (*map_free)(nd_osm_map *);
    nd_err (*map_peek)(const char *, char *, size_t, int32_t *, int32_t *, int32_t *, int32_t *);
    size_t (*list_maps)(char (*)[512], size_t);
    nd_err (*new_map_path)(const char *, char *, size_t);
    const char *(*way_name)(const nd_osm_map *, const nd_osm_way *);
    const char *(*place_name)(const nd_osm_map *, const nd_osm_place *);
    nd_err (*graph_build)(nd_osm_map *);
    uint32_t (*nearest_node)(const nd_osm_map *, int32_t, int32_t, nd_osm_mode, double *);
    nd_err (*route_find)(nd_osm_map *, uint32_t, uint32_t, nd_osm_mode, nd_osm_route *);
    void (*route_free)(nd_osm_route *);
    double (*speed)(nd_osm_mode, uint8_t, uint8_t);
    nd_osm_turn (*turn_for)(double);
    size_t (*route_steps)(const nd_osm_map *, const nd_osm_route *, nd_osm_step *, size_t);
    void (*step_label)(const nd_osm_step *, char *, size_t);
    void (*format_distance)(double, char *, size_t);
    nd_err (*fetch_query)(char *, size_t, int32_t, int32_t, int32_t, int32_t, bool);
    nd_err (*fetch)(const char *, const char *, nd_osm_fetch_progress_fn, void *, char *, size_t);
    nd_osm_scratch *(*scratch_new)(void);
    void (*scratch_free)(nd_osm_scratch *);
    void (*render)(const nd_osm_scene *, nd_image *);
    void (*scene_draw)(const nd_osm_scene *, nd_image *);
} api;

#define SYM(field, name)                        \
    do {                                        \
        *(void **)&api.field = sa_sym(h, name); \
        if (api.field == NULL)                  \
            ok = false;                         \
    } while (0)

static bool api_open(void *h)
{
    bool ok = true;

    SYM(run, "app_run");
    SYM(shutdown, "app_shutdown");
    api.title = dlsym(h, "nd_osmand_app_title");
    api.options = dlsym(h, "nd_osmand_options");
    api.size_names = dlsym(h, "nd_osmand_size_names");
    api.mode_names = dlsym(h, "nd_osmand_mode_names");
    api.turn_names = dlsym(h, "nd_osmand_turn_names");
    SYM(project, "nd_osm_project");
    SYM(unproject, "nd_osm_unproject");
    SYM(metres_per_unit, "nd_osm_metres_per_unit");
    SYM(bbox_around, "nd_osm_bbox_around");
    SYM(classify_tag, "nd_osm_classify_tag");
    SYM(classify_finish, "nd_osm_classify_finish");
    SYM(place_rank, "nd_osm_place_rank");
    SYM(map_key, "nd_osm_map_key");
    SYM(map_geometry, "nd_osm_map_geometry");
    SYM(scale_metres, "nd_osm_scale_metres");
    SYM(view_to_screen, "nd_osm_view_to_screen");
    SYM(import, "nd_osm_import");
    SYM(map_load, "nd_osm_map_load");
    SYM(map_free, "nd_osm_map_free");
    SYM(map_peek, "nd_osm_map_peek");
    SYM(list_maps, "nd_osm_list_maps");
    SYM(new_map_path, "nd_osm_new_map_path");
    SYM(way_name, "nd_osm_way_name");
    SYM(place_name, "nd_osm_place_name");
    SYM(graph_build, "nd_osm_graph_build");
    SYM(nearest_node, "nd_osm_nearest_node");
    SYM(route_find, "nd_osm_route_find");
    SYM(route_free, "nd_osm_route_free");
    SYM(speed, "nd_osm_speed");
    SYM(turn_for, "nd_osm_turn_for");
    SYM(route_steps, "nd_osm_route_steps");
    SYM(step_label, "nd_osm_step_label");
    SYM(format_distance, "nd_osm_format_distance");
    SYM(fetch_query, "nd_osm_fetch_query");
    SYM(fetch, "nd_osm_fetch");
    SYM(scratch_new, "nd_osm_scratch_new");
    SYM(scratch_free, "nd_osm_scratch_free");
    SYM(render, "nd_osm_render");
    SYM(scene_draw, "nd_osm_scene_draw");
    return ok && api.title != NULL && api.options != NULL && api.size_names != NULL &&
           api.mode_names != NULL && api.turn_names != NULL;
}

/* ------------------------------------------------------------------ *
 * A scratch root for the whole test
 * ------------------------------------------------------------------ *
 *
 * Every path the app opens goes through nd_path_resolve(), so pointing the
 * root at a fresh directory is what keeps the maps this test writes, and
 * the settings.prop app_run() saves on exit, out of the developer's
 * /NeoDCT and out of the source tree. The fixture's wallpaper code saves
 * and restores the root around its own overlay reads, so the two coexist.
 */

static char g_root[ND_PATH_MAX];
static char g_fixture[ND_PATH_MAX];
static char g_bindir[ND_PATH_MAX];
static char g_ctl[ND_PATH_MAX];
static char g_path_keep[ND_PATH_MAX * 2];

static bool scratch_root_begin(void)
{
    if (!sa_tmpdir("ndosm", g_root, sizeof g_root))
        return false;
    /* Traversable, like the phone's own /: an app process under a 0700
     * root cannot resolve its own path. See references/testing.md. */
    (void)chmod(g_root, 0711);
    return nd_path_set_root(g_root) == ND_OK;
}

static void scratch_root_end(void)
{
    (void)nd_path_set_root(NULL);
    sa_rmtree(g_root);
}

static bool find_fixture(void)
{
    if (sa_neodct[0] == '\0' && !sa_resolve_neodct())
        return false;
    if (nd_snprintf(g_fixture, sizeof g_fixture, "%s/tests/osmand/town.osm", sa_neodct) != ND_OK)
        return false;
    return sa_file_exists(g_fixture);
}

/* The fixture's own coordinates, in 1e7 degrees. */
#define LAT(y) ((int32_t)((y) * 10000000.0 + ((y) >= 0 ? 0.5 : -0.5)))
#define LON(x) ((int32_t)((x) * 10000000.0 + ((x) >= 0 ? 0.5 : -0.5)))

#define TOWN_SOUTH LAT(53.3440)
#define TOWN_WEST  LON(-6.2760)
#define TOWN_NORTH LAT(53.3560)
#define TOWN_EAST  LON(-6.2440)

/* ------------------------------------------------------------------ *
 * 1. The rows
 * ------------------------------------------------------------------ */

static void test_rows(void)
{
    CHECK_STR(*api.title, "OsmAnd", "the title the header draws");
    CHECK_INT(ND_OSMAND_APP_ID, 13, "app id 13");
    CHECK_STR(ND_OSMAND_APP_ROOT, "13", "and the breadcrumb agrees with it");
    CHECK_STR(ND_OSMAND_DATA_DIR, "/NeoDCT/User/sdcard/apps/OsmAnd/data",
              "maps live on the card, where the owner asked for them");

    /* "Route to here" is row 0 so that NaviKey, NaviKey plans a route to
     * the cross: two presses on one key with no navigation between them. */
    CHECK_STR(api.options[ND_OSMAND_OPT_ROUTE_HERE], "Route to here", "options row 1");
    CHECK_STR(api.options[ND_OSMAND_OPT_START_HERE], "Start here", "options row 2");
    CHECK_STR(api.options[ND_OSMAND_OPT_DIRECTIONS], "Directions", "options row 3");
    CHECK_STR(api.options[ND_OSMAND_OPT_CLEAR_ROUTE], "Clear route", "options row 4");
    CHECK_STR(api.options[ND_OSMAND_OPT_FIND], "Find", "options row 5");
    CHECK_STR(api.options[ND_OSMAND_OPT_GOTO], "Go to", "options row 6");
    CHECK_STR(api.options[ND_OSMAND_OPT_DOWNLOAD], "Download map", "options row 7");
    CHECK_STR(api.options[ND_OSMAND_OPT_MAPS], "Installed maps", "options row 8");
    CHECK_STR(api.options[ND_OSMAND_OPT_MODE], "Route mode", "options row 9");
    /* Nine rows: every one reachable by its digit shortcut on nd_vlist. */
    CHECK_INT(ND_OSMAND_OPTIONS_COUNT, 9, "nine options, one per digit");

    CHECK_STR(api.size_names[0], "Small (2 km)", "size row 1");
    CHECK_STR(api.size_names[2], "Large (10 km, no buildings)", "size row 3");
    CHECK_STR(api.mode_names[ND_OSM_MODE_CAR], "By car", "mode row 1");
    CHECK_STR(api.mode_names[ND_OSM_MODE_FOOT], "On foot", "mode row 2");
    CHECK_STR(api.turn_names[ND_OSM_TURN_LEFT], "Turn left", "the turn a page shows");
    CHECK_STR(api.turn_names[ND_OSM_TURN_ARRIVE], "Arrive", "and the last one");
}

/* ------------------------------------------------------------------ *
 * 2. The projection
 * ------------------------------------------------------------------ */

static void test_projection_round_trips(void)
{
    int32_t mx;
    int32_t my;
    double lat;
    double lon;
    int32_t world = (int32_t)(256 << ND_OSM_MERC_ZOOM);

    api.project(0, 0, &mx, &my);
    CHECK_INT(mx, world / 2, "the prime meridian is the middle of the world");
    CHECK_INT(my, world / 2, "and so is the equator");

    api.project(LAT(53.3498), LON(-6.2603), &mx, &my);
    CHECK(mx < world / 2, "Dublin is west of Greenwich");
    CHECK(my < world / 2, "and north of the equator");
    api.unproject(mx, my, &lat, &lon);
    CHECK(fabs(lat - 53.3498) < 1e-5, "latitude survives the round trip");
    CHECK(fabs(lon + 6.2603) < 1e-5, "longitude survives the round trip");

    /* The equator: the circumference over the world's width in pixels. */
    CHECK(fabs(api.metres_per_unit(world / 2) - 40075016.686 / (double)world) < 1e-9,
          "a reference unit at the equator is the circumference over 2^28");
    CHECK(api.metres_per_unit(my) < api.metres_per_unit(world / 2),
          "and it shrinks with latitude, as Mercator does");
}

static void test_the_download_box(void)
{
    int32_t s;
    int32_t w;
    int32_t n;
    int32_t e;

    api.bbox_around(53.35, -6.26, 2.0, &s, &w, &n, &e);
    /* Two kilometres is 0.01797 degrees of latitude anywhere. */
    CHECK(abs((n - s) - 179700) < 200, "2 km is 0.018 degrees north to south");
    /* And more degrees of longitude this far north. */
    CHECK(e - w > n - s, "the box is wider in degrees than it is tall");
    CHECK(s < LAT(53.35) && n > LAT(53.35), "centred on the latitude");
    CHECK(w < LON(-6.26) && e > LON(-6.26), "centred on the longitude");

    api.bbox_around(89.0, 0.0, 1000.0, &s, &w, &n, &e);
    CHECK(n <= LAT(85.06), "clamped to what Mercator can draw");
}

/* ------------------------------------------------------------------ *
 * 3. The classifier
 * ------------------------------------------------------------------ */

static void classify(nd_osm_classify *c, bool closed, const char *k1, const char *v1,
                     const char *k2, const char *v2)
{
    memset(c, 0, sizeof *c);
    c->closed = closed;
    api.classify_tag(c, k1, v1);
    if (k2 != NULL)
        api.classify_tag(c, k2, v2);
    api.classify_finish(c);
}

static void test_tags_become_kinds(void)
{
    nd_osm_classify c;

    classify(&c, false, "highway", "motorway", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_MOTORWAY, "motorway");
    CHECK((c.flags & ND_OSM_FLAG_NO_FOOT) != 0u, "which nobody walks along");

    classify(&c, false, "highway", "footway", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_PATH, "footway is a path");
    CHECK((c.flags & ND_OSM_FLAG_NO_CAR) != 0u, "which no car drives along");

    classify(&c, false, "highway", "residential", "oneway", "yes");
    CHECK_INT(c.kind, ND_OSM_KIND_RESIDENTIAL, "residential");
    CHECK((c.flags & ND_OSM_FLAG_ONEWAY) != 0u, "one way");

    classify(&c, true, "highway", "residential", "junction", "roundabout");
    CHECK((c.flags & ND_OSM_FLAG_ONEWAY) != 0u, "a roundabout is one way");
    CHECK((c.flags & ND_OSM_FLAG_AREA) == 0u, "and a closed road is still a line");

    classify(&c, true, "highway", "pedestrian", "area", "yes");
    CHECK_INT(c.kind, ND_OSM_KIND_PATH, "a pedestrian square is a place to walk");
    CHECK((c.flags & ND_OSM_FLAG_AREA) == 0u, "not a thing to fill");

    classify(&c, true, "landuse", "forest", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_WOOD, "a closed forest");
    CHECK((c.flags & ND_OSM_FLAG_AREA) != 0u, "is filled");

    classify(&c, false, "landuse", "forest", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_NONE, "an open forest edge draws nothing");

    classify(&c, true, "building", "yes", "natural", "water");
    CHECK_INT(c.kind, ND_OSM_KIND_WATER, "water beats a building");
    classify(&c, true, "landuse", "grass", "building", "house");
    CHECK_INT(c.kind, ND_OSM_KIND_BUILDING, "and a building beats landuse");
    classify(&c, true, "building", "no", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_NONE, "building=no is not a building");

    classify(&c, false, "highway", "service", "access", "private");
    CHECK((c.flags & ND_OSM_FLAG_NO_CAR) != 0u && (c.flags & ND_OSM_FLAG_NO_FOOT) != 0u,
          "private is private for everybody");

    classify(&c, false, "highway", "proposed", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_NONE, "a proposed road is not a road");

    classify(&c, false, "railway", "rail", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_RAILWAY, "rail");
    classify(&c, false, "waterway", "stream", NULL, NULL);
    CHECK_INT(c.kind, ND_OSM_KIND_STREAM, "stream");

    CHECK_INT(api.place_rank("city"), ND_OSM_PLACE_CITY, "city outranks");
    CHECK_INT(api.place_rank("hamlet"), ND_OSM_PLACE_HAMLET, "hamlet");
    CHECK_INT(api.place_rank("continent"), ND_OSM_PLACE_OTHER, "a continent is not drawn");
    CHECK_INT(api.place_rank(NULL), ND_OSM_PLACE_OTHER, "NULL is nothing");
}

/* ------------------------------------------------------------------ *
 * 4. The key map -- the case a golden frame could not have covered
 * ------------------------------------------------------------------ */

#define MAP_W 240
#define MAP_H 114

static nd_osm_view centred_view(int32_t zoom)
{
    nd_osm_view v;

    api.project(LAT(53.35), LON(-6.26), &v.cx, &v.cy);
    v.zoom = zoom;
    return v;
}

static void test_the_3x3_block_is_the_d_pad(void)
{
    nd_osm_view v = centred_view(15);
    nd_osm_view w;
    int64_t unit = (int64_t)1 << (ND_OSM_MERC_ZOOM - 15);

    w = v;
    CHECK_INT(api.map_key(ND_KEY_2, &w, MAP_W, MAP_H, NULL), ND_OSM_NAV_MOVED, "2 moves");
    CHECK_INT(w.cy, v.cy - (MAP_H / 4) * unit, "2 pans up a quarter of the map");
    CHECK_INT(w.cx, v.cx, "and not sideways");

    w = v;
    (void)api.map_key(ND_KEY_8, &w, MAP_W, MAP_H, NULL);
    CHECK_INT(w.cy, v.cy + (MAP_H / 4) * unit, "8 pans down");
    w = v;
    (void)api.map_key(ND_KEY_4, &w, MAP_W, MAP_H, NULL);
    CHECK_INT(w.cx, v.cx - (MAP_W / 4) * unit, "4 pans left");
    w = v;
    (void)api.map_key(ND_KEY_6, &w, MAP_W, MAP_H, NULL);
    CHECK_INT(w.cx, v.cx + (MAP_W / 4) * unit, "6 pans right");

    w = v;
    CHECK_INT(api.map_key(ND_KEY_1, &w, MAP_W, MAP_H, NULL), ND_OSM_NAV_MOVED, "1 zooms");
    CHECK_INT(w.zoom, 14, "out");
    CHECK_INT(w.cx, v.cx, "about the centre");
    w = v;
    (void)api.map_key(ND_KEY_3, &w, MAP_W, MAP_H, NULL);
    CHECK_INT(w.zoom, 16, "3 zooms in");
}

static void test_the_rocker_and_the_outer_keys_agree_with_the_block(void)
{
    nd_osm_view v = centred_view(15);
    nd_osm_view a;
    nd_osm_view b;

    a = v;
    b = v;
    (void)api.map_key(ND_KEY_UP, &a, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_2, &b, MAP_W, MAP_H, NULL);
    CHECK(a.cy == b.cy && a.cx == b.cx, "Up is 2");
    a = v;
    b = v;
    (void)api.map_key(ND_KEY_DOWN, &a, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_8, &b, MAP_W, MAP_H, NULL);
    CHECK(a.cy == b.cy, "Down is 8");
    a = v;
    b = v;
    (void)api.map_key(ND_KEY_STAR, &a, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_1, &b, MAP_W, MAP_H, NULL);
    CHECK(a.zoom == b.zoom, "* is 1");
    a = v;
    b = v;
    (void)api.map_key(ND_KEY_HASH, &a, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_3, &b, MAP_W, MAP_H, NULL);
    CHECK(a.zoom == b.zoom, "# is 3");

    /* Left and Right exist only on a development QWERTY keyboard. They are
     * folded onto 4 and 6 rather than given a meaning the phone could not
     * reach. */
    a = v;
    b = v;
    (void)api.map_key(ND_KEY_LEFT, &a, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_4, &b, MAP_W, MAP_H, NULL);
    CHECK(a.cx == b.cx, "Left is 4");
    a = v;
    b = v;
    (void)api.map_key(ND_KEY_RIGHT, &a, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_6, &b, MAP_W, MAP_H, NULL);
    CHECK(a.cx == b.cx, "Right is 6");
}

/* THE CASE THIS FILE EXISTS FOR. Every movement the map can make has to be
 * reachable from the sixteen keys the 5190 actually has. If a future edit
 * moved one of them onto Left or Right the phone would silently lose it,
 * and nothing else in the suite would notice -- the development keyboard
 * has both and every other test runs on that. */
static void test_every_movement_is_reachable_without_left_or_right(void)
{
    static const int32_t HARDWARE_KEYS[] = {
        ND_KEY_ENTER, ND_KEY_CLEAR, ND_KEY_UP,   ND_KEY_DOWN, ND_KEY_1, ND_KEY_2,
        ND_KEY_3,     ND_KEY_4,     ND_KEY_5,    ND_KEY_6,    ND_KEY_7, ND_KEY_8,
        ND_KEY_9,     ND_KEY_0,     ND_KEY_STAR, ND_KEY_HASH,
    };
    nd_osm_view v = centred_view(15);
    nd_osm_mark jump;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool in = false;
    bool out = false;
    bool jumped = false;
    bool options = false;
    bool back = false;
    size_t i;

    CHECK_INT(ND_ARRAY_LEN(HARDWARE_KEYS), 16, "the 5190 has sixteen keys");
    jump.mx = v.cx + 12345;
    jump.my = v.cy - 6789;
    jump.set = true;

    for (i = 0u; i < ND_ARRAY_LEN(HARDWARE_KEYS); i++) {
        nd_osm_view w = v;
        nd_osm_nav nav = api.map_key(HARDWARE_KEYS[i], &w, MAP_W, MAP_H, &jump);

        if (nav == ND_OSM_NAV_OPTIONS)
            options = true;
        else if (nav == ND_OSM_NAV_BACK)
            back = true;
        if (nav != ND_OSM_NAV_MOVED)
            continue;
        if (w.cx == jump.mx && w.cy == jump.my)
            jumped = true;
        else if (w.cy < v.cy)
            up = true;
        else if (w.cy > v.cy)
            down = true;
        else if (w.cx < v.cx)
            left = true;
        else if (w.cx > v.cx)
            right = true;
        else if (w.zoom > v.zoom)
            in = true;
        else if (w.zoom < v.zoom)
            out = true;
    }

    CHECK(up && down, "up and down, from the keypad alone");
    CHECK(left && right, "left and right, from the keypad alone");
    CHECK(in && out, "in and out, from the keypad alone");
    CHECK(jumped, "the jump to the mark, from the keypad alone");
    CHECK(options && back, "and Options and Back");
}

static void test_the_edges_of_the_map_are_steady(void)
{
    nd_osm_view v = centred_view(ND_OSM_ZOOM_MAX);
    nd_osm_mark none;

    none.set = false;
    /* A zoom already at its limit still reports MOVED, so holding the key
     * against the end of the range is a steady screen rather than a dead
     * one -- nd_vlist_show() makes the same choice at the ends of a list. */
    CHECK_INT(api.map_key(ND_KEY_3, &v, MAP_W, MAP_H, NULL), ND_OSM_NAV_MOVED, "3 at max zoom");
    CHECK_INT(v.zoom, ND_OSM_ZOOM_MAX, "stays at max");
    v.zoom = ND_OSM_ZOOM_MIN;
    (void)api.map_key(ND_KEY_1, &v, MAP_W, MAP_H, NULL);
    CHECK_INT(v.zoom, ND_OSM_ZOOM_MIN, "and at min");

    v.cx = 0;
    v.cy = 0;
    (void)api.map_key(ND_KEY_4, &v, MAP_W, MAP_H, NULL);
    (void)api.map_key(ND_KEY_2, &v, MAP_W, MAP_H, NULL);
    CHECK(v.cx == 0 && v.cy == 0, "the world has an edge");

    CHECK_INT(api.map_key(ND_KEY_5, &v, MAP_W, MAP_H, &none), ND_OSM_NAV_NONE,
              "5 with nothing marked does nothing");
    CHECK_INT(api.map_key(ND_KEY_5, &v, MAP_W, MAP_H, NULL), ND_OSM_NAV_NONE, "nor with NULL");
    CHECK_INT(api.map_key(ND_KEY_0, &v, MAP_W, MAP_H, NULL), ND_OSM_NAV_NONE, "0 is unmapped");
    CHECK_INT(api.map_key(ND_KEY_7, &v, MAP_W, MAP_H, NULL), ND_OSM_NAV_NONE, "so is 7");
    CHECK_INT(api.map_key(ND_KEY_4, NULL, MAP_W, MAP_H, NULL), ND_OSM_NAV_NONE, "NULL view");
}

/* ------------------------------------------------------------------ *
 * 5. The import
 * ------------------------------------------------------------------ */

static char g_map_path[512];

/* The importer opens its input through nd_path_resolve(), as the download
 * lands under the root, so the fixture is copied to a virtual path first
 * -- exactly where a real download would be. */
#define STAGED_XML ND_OSMAND_DATA_DIR "/.fixture.osm"

static bool stage_fixture(void)
{
    char real[ND_PATH_MAX];
    FILE *in;
    FILE *out;
    char buf[4096];
    size_t n;
    bool ok = true;

    if (nd_mkdir_p(ND_OSMAND_DATA_DIR, 0755u) != ND_OK)
        return false;
    if (nd_path_resolve(real, sizeof real, STAGED_XML) != ND_OK)
        return false;
    in = fopen(g_fixture, "rb");
    if (in == NULL)
        return false;
    out = fopen(real, "wb");
    if (out == NULL) {
        (void)fclose(in);
        return false;
    }
    while ((n = fread(buf, 1u, sizeof buf, in)) > 0u) {
        if (fwrite(buf, 1u, n, out) != n)
            ok = false;
    }
    (void)fclose(in);
    return fclose(out) == 0 && ok;
}

static bool import_town(nd_osm_import_stats *stats)
{
    char why[128];
    nd_err rc;

    if (!stage_fixture())
        return false;
    if (api.new_map_path("", g_map_path, sizeof g_map_path) != ND_OK)
        return false;
    why[0] = '\0';
    rc = api.import(STAGED_XML, g_map_path, "", TOWN_SOUTH, TOWN_WEST, TOWN_NORTH, TOWN_EAST, stats,
                    why, sizeof why);
    if (rc != ND_OK)
        fprintf(stderr, "import: %s (%s)\n", nd_strerror(rc), why);
    return rc == ND_OK;
}

/* The way with a given name, or NULL. Names are per way, so a street of
 * two ways -- Link Street -- returns whichever sorts first. */
static const nd_osm_way *way_named(const nd_osm_map *m, const char *name)
{
    uint32_t i;

    for (i = 0u; i < m->n_ways; i++) {
        if (strcmp(api.way_name(m, &m->ways[i]), name) == 0)
            return &m->ways[i];
    }
    return NULL;
}

/* The node at a fixture coordinate, or UINT32_MAX. Projection is
 * deterministic, so the fixture's own numbers find their nodes exactly. */
static uint32_t node_at(const nd_osm_map *m, double lat, double lon)
{
    int32_t mx;
    int32_t my;
    uint32_t i;

    api.project(LAT(lat), LON(lon), &mx, &my);
    for (i = 0u; i < m->n_nodes; i++) {
        if (m->mx[i] == mx && m->my[i] == my)
            return i;
    }
    return UINT32_MAX;
}

static void test_the_town_imports(void)
{
    nd_osm_import_stats stats;
    nd_osm_map *m = NULL;
    char name[ND_OSM_NAME_MAX];
    int32_t s = 0;
    int32_t w = 0;
    int32_t n = 0;
    int32_t e = 0;
    char paths[2][512];
    uint32_t i;
    bool ordered = true;
    const nd_osm_way *way;

    memset(&stats, 0, sizeof stats);
    if (!import_town(&stats)) {
        CHECK(false, "the fixture imports");
        return;
    }
    /* Every one of these is counted by hand from town.osm; see its README. */
    CHECK_INT(stats.n_nodes, 38, "38 nodes are referenced by a kept way");
    CHECK_INT(stats.n_ways, 18,
              "18 ways survive (the open landuse, the ghost and the proposed do not)");
    CHECK_INT(stats.n_roads, 12, "12 of them are roads");
    CHECK_INT(stats.n_places, 2, "2 named places (a continent is not one)");
    CHECK_STR(stats.name, "Ballytown", "the town names the map");
    CHECK(strstr(g_map_path, "/Map.ndmap") != NULL, "a nameless import is called Map");

    CHECK_INT(api.map_peek(g_map_path, name, sizeof name, &s, &w, &n, &e), ND_OK, "peek");
    CHECK_STR(name, "Ballytown", "the header carries the name");
    CHECK_INT(s, TOWN_SOUTH, "and the box that was asked for");
    CHECK_INT(e, TOWN_EAST, "to the east as well");

    CHECK_INT(api.list_maps(paths, 2u), 1, "one map on the card");
    CHECK_STR(paths[0], g_map_path, "listed by its virtual path");

    CHECK_INT(api.map_load(g_map_path, &m), ND_OK, "loads");
    if (m == NULL)
        return;
    CHECK_INT(m->n_nodes, 38, "38 nodes loaded");
    CHECK_INT(m->n_ways, 18, "18 ways loaded");
    CHECK_INT(m->n_places, 2, "2 places loaded");
    CHECK_STR(m->name, "Ballytown", "named");

    for (i = 1u; i < m->n_ways; i++) {
        if (m->ways[i].kind < m->ways[i - 1u].kind)
            ordered = false;
    }
    CHECK(ordered, "ways are sorted by kind, areas first, motorway last");
    CHECK_INT(m->kind_begin[ND_OSM_KIND_COUNT], 18, "the kind index covers every way");
    CHECK_INT(m->kind_begin[ND_OSM_KIND_MOTORWAY + 1] - m->kind_begin[ND_OSM_KIND_MOTORWAY], 1,
              "one motorway");
    CHECK_INT(m->kind_begin[ND_OSM_KIND_RESIDENTIAL + 1] - m->kind_begin[ND_OSM_KIND_RESIDENTIAL],
              8, "eight residential ways, Link Street counted twice");

    way = way_named(m, "Mid Lane");
    CHECK(way != NULL, "Mid Lane is there");
    if (way != NULL) {
        CHECK((way->flags & ND_OSM_FLAG_ONEWAY) != 0u, "and one way");
        CHECK_INT(way->n_refs, 3, "with three nodes");
    }
    way = way_named(m, "M50");
    CHECK(way != NULL, "a road with only a ref is known by its ref");
    way = way_named(m, "Round Lake");
    CHECK(way != NULL && (way->flags & ND_OSM_FLAG_AREA) != 0u, "the lake is an area");
    way = way_named(m, "Green Park");
    CHECK(way != NULL && way->kind == ND_OSM_KIND_GRASS, "the park is grass");
    CHECK(way_named(m, "Ghost Street") == NULL, "a road with one real node is dropped");

    CHECK_STR(api.place_name(m, &m->places[0]), "Ballytown", "place 1");
    CHECK_STR(api.place_name(m, &m->places[1]), "Northside & Co", "place 2, entity decoded");
    CHECK_INT(m->places[0].kind, ND_OSM_PLACE_TOWN, "a town");

    CHECK(node_at(m, 53.3480, -6.2650) != UINT32_MAX, "the grid's corner is a node");
    CHECK(node_at(m, 53.3450, -6.2600) == UINT32_MAX, "the dropped landuse's node is not");

    api.map_free(m);
}

static void test_a_second_import_gets_a_new_name(void)
{
    char path[512];

    CHECK_INT(api.new_map_path("Ballytown", path, sizeof path), ND_OK, "a free name");
    CHECK(strstr(path, "/Ballytown.ndmap") != NULL, "is used as it is");
    CHECK_INT(api.new_map_path("", path, sizeof path), ND_OK, "the name Map is taken");
    CHECK(strstr(path, "/Map 2.ndmap") != NULL, "so the next one is Map 2");
    CHECK_INT(api.new_map_path("Dún Laoghaire/Rathdown", path, sizeof path), ND_OK,
              "a name with bytes a file cannot carry");
    CHECK(strstr(path, "/D__n Laoghaire_Rathdown.ndmap") != NULL,
          "is spelled with underscores; the header keeps the real one");
}

static void test_a_file_that_is_not_a_map_is_refused(void)
{
    nd_osm_map *m = NULL;

    CHECK_INT(api.map_load(STAGED_XML, &m), ND_ERR_PARSE, "XML is not a map");
    CHECK(m == NULL, "and nothing is returned");
    CHECK_INT(api.map_load("/NeoDCT/User/sdcard/apps/OsmAnd/data/none.ndmap", &m), ND_ERR_NOTFOUND,
              "a missing file");
}

/* ------------------------------------------------------------------ *
 * 6. Routing
 * ------------------------------------------------------------------ */

static bool route_has(const nd_osm_route *r, uint32_t node)
{
    uint32_t i;

    for (i = 0u; i < r->n_nodes; i++) {
        if (r->nodes[i] == node)
            return true;
    }
    return false;
}

static void test_routes_honour_one_way_streets_by_car_only(void)
{
    nd_osm_map *m = NULL;
    nd_osm_route r;
    uint32_t sw;
    uint32_t nw;
    uint32_t mid;
    uint32_t n_mid;
    uint32_t s_mid;
    uint32_t path_node;
    uint32_t ne;
    double d = 0.0;

    if (api.map_load(g_map_path, &m) != ND_OK || m == NULL) {
        CHECK(false, "the town loads for routing");
        return;
    }
    CHECK_INT(api.graph_build(m), ND_OK, "the graph builds");
    CHECK_INT(api.graph_build(m), ND_OK, "and building it twice is a no-op");
    CHECK(m->n_adj > 0u, "with edges");

    sw = node_at(m, 53.3480, -6.2650);
    nw = node_at(m, 53.3520, -6.2650);
    mid = node_at(m, 53.3500, -6.2600);
    s_mid = node_at(m, 53.3480, -6.2600);
    n_mid = node_at(m, 53.3520, -6.2600);
    ne = node_at(m, 53.3520, -6.2550);
    path_node = node_at(m, 53.3510, -6.2575);
    if (sw == UINT32_MAX || nw == UINT32_MAX || mid == UINT32_MAX || s_mid == UINT32_MAX ||
        n_mid == UINT32_MAX || ne == UINT32_MAX || path_node == UINT32_MAX) {
        CHECK(false, "the fixture's nodes are found");
        api.map_free(m);
        return;
    }

    /* Nearest node: a point just off the south-west corner snaps to it. */
    CHECK_INT(api.nearest_node(m, m->mx[sw] + 3, m->my[sw] - 3, ND_OSM_MODE_CAR, &d), sw,
              "the nearest routable node");
    CHECK(d < 5.0, "a few metres away");

    /* West Lane, straight up: 444 m at 30 km/h. */
    CHECK_INT(api.route_find(m, sw, nw, ND_OSM_MODE_CAR, &r), ND_OK, "a route up West Lane");
    CHECK_INT(r.n_nodes, 3, "three nodes");
    CHECK(fabs(r.metres - 444.8) < 5.0, "444 m long");
    CHECK(fabs(r.seconds - 444.8 / (30.0 / 3.6)) < 2.0, "at 30 km/h");
    api.route_free(&r);

    /* Mid Lane runs north only. Southbound by car has to go round; on foot
     * it is the direct 444 m. */
    CHECK_INT(api.route_find(m, n_mid, s_mid, ND_OSM_MODE_CAR, &r), ND_OK, "south by car");
    CHECK(!route_has(&r, mid), "avoids the one-way lane");
    CHECK(r.metres > 1000.0, "and goes the long way round");
    api.route_free(&r);
    CHECK_INT(api.route_find(m, n_mid, s_mid, ND_OSM_MODE_FOOT, &r), ND_OK, "south on foot");
    CHECK(route_has(&r, mid), "walks straight down it");
    CHECK(fabs(r.metres - 444.8) < 5.0, "444 m");
    api.route_free(&r);

    /* Park Path cuts the corner from the middle to the north-east. A walker
     * takes it; a car cannot. */
    CHECK_INT(api.route_find(m, mid, ne, ND_OSM_MODE_FOOT, &r), ND_OK, "corner on foot");
    CHECK(route_has(&r, path_node), "takes the footpath");
    api.route_free(&r);
    CHECK_INT(api.route_find(m, mid, ne, ND_OSM_MODE_CAR, &r), ND_OK, "corner by car");
    CHECK(!route_has(&r, path_node), "stays on the road");
    api.route_free(&r);

    /* The motorway is not joined to the town. */
    {
        uint32_t m50 = node_at(m, 53.3540, -6.2700);

        CHECK(m50 != UINT32_MAX, "the motorway's node exists");
        if (m50 != UINT32_MAX) {
            CHECK_INT(api.route_find(m, sw, m50, ND_OSM_MODE_CAR, &r), ND_ERR_NOTFOUND,
                      "nothing reaches it");
            CHECK(r.nodes == NULL, "and nothing is allocated");
        }
    }

    CHECK_INT(api.route_find(m, sw, 999999u, ND_OSM_MODE_CAR, &r), ND_ERR_INVAL, "a bad node");
    api.map_free(m);
}

static void test_speed_is_the_whole_policy(void)
{
    CHECK(api.speed(ND_OSM_MODE_CAR, ND_OSM_KIND_MOTORWAY, 0u) >
              api.speed(ND_OSM_MODE_CAR, ND_OSM_KIND_RESIDENTIAL, 0u),
          "a motorway is faster than a street");
    CHECK_DBL(api.speed(ND_OSM_MODE_CAR, ND_OSM_KIND_PATH, 0u), 0.0, "a car takes no path");
    CHECK_DBL(api.speed(ND_OSM_MODE_FOOT, ND_OSM_KIND_MOTORWAY, 0u), 0.0, "nobody walks the M50");
    CHECK(api.speed(ND_OSM_MODE_FOOT, ND_OSM_KIND_PATH, 0u) > 0.0, "a walker takes a path");
    CHECK_DBL(api.speed(ND_OSM_MODE_CAR, ND_OSM_KIND_RESIDENTIAL, ND_OSM_FLAG_NO_CAR), 0.0,
              "access=no keeps a car out");
    CHECK_DBL(api.speed(ND_OSM_MODE_FOOT, ND_OSM_KIND_PATH, ND_OSM_FLAG_NO_FOOT), 0.0,
              "foot=no keeps a walker out");
    CHECK_DBL(api.speed(ND_OSM_MODE_CAR, ND_OSM_KIND_WATER, 0u), 0.0, "water is not a road");
}

static void test_directions(void)
{
    nd_osm_map *m = NULL;
    nd_osm_route r;
    nd_osm_step steps[ND_OSM_STEPS_MAX];
    size_t n;
    size_t i;
    int32_t total = 0;
    uint32_t sw;
    uint32_t ne;
    char label[64];

    CHECK_INT(api.turn_for(0.0), ND_OSM_TURN_STRAIGHT, "0 degrees is straight on");
    CHECK_INT(api.turn_for(20.0), ND_OSM_TURN_STRAIGHT, "so is a gentle bend");
    CHECK_INT(api.turn_for(90.0), ND_OSM_TURN_RIGHT, "90 is right");
    CHECK_INT(api.turn_for(-90.0), ND_OSM_TURN_LEFT, "-90 is left");
    CHECK_INT(api.turn_for(170.0), ND_OSM_TURN_SHARP_RIGHT, "170 is sharp right");
    CHECK_INT(api.turn_for(-150.0), ND_OSM_TURN_SHARP_LEFT, "-150 is sharp left");

    api.format_distance(57.0, label, sizeof label);
    CHECK_STR(label, "57 m", "metres");
    api.format_distance(437.0, label, sizeof label);
    CHECK_STR(label, "440 m", "to the nearest ten above a hundred");
    api.format_distance(1234.0, label, sizeof label);
    CHECK_STR(label, "1.2 km", "kilometres with a decimal");
    api.format_distance(12345.0, label, sizeof label);
    CHECK_STR(label, "12 km", "and without one past ten");

    if (api.map_load(g_map_path, &m) != ND_OK || m == NULL) {
        CHECK(false, "the town loads for directions");
        return;
    }
    sw = node_at(m, 53.3480, -6.2650);
    ne = node_at(m, 53.3520, -6.2550);
    if (api.route_find(m, sw, ne, ND_OSM_MODE_CAR, &r) != ND_OK) {
        CHECK(false, "corner to corner by car");
        api.map_free(m);
        return;
    }
    n = api.route_steps(m, &r, steps, ND_OSM_STEPS_MAX);
    CHECK(n >= 3u, "at least a start, a turn and an arrival");
    CHECK_INT(steps[0].turn, ND_OSM_TURN_START, "the first step starts");
    CHECK_INT(steps[n - 1u].turn, ND_OSM_TURN_ARRIVE, "the last arrives");
    CHECK_INT(steps[n - 1u].node, ne, "at the destination");
    for (i = 0u; i < n; i++)
        total += steps[i].metres;
    CHECK(abs(total - nd_trunc32(r.metres + 0.5)) <= (int32_t)n, "the steps add up to the route");
    CHECK(steps[0].road[0] != '\0', "the first road is named");
    {
        bool turned = false;

        /* The fastest way corner to corner is out along Link Street, along
         * the primary Main Road at 60 km/h, and back up the other Link
         * Street onto East Lane -- which is a straight continuation, so a
         * "Continue" is right. But nothing on a rectangular grid is sharp. */
        for (i = 1u; i + 1u < n; i++) {
            CHECK(steps[i].turn == ND_OSM_TURN_LEFT || steps[i].turn == ND_OSM_TURN_RIGHT ||
                      steps[i].turn == ND_OSM_TURN_STRAIGHT,
                  "on a rectangular grid no turn is sharp");
            if (steps[i].turn == ND_OSM_TURN_LEFT || steps[i].turn == ND_OSM_TURN_RIGHT)
                turned = true;
        }
        CHECK(turned, "and getting to the opposite corner takes at least one turn");
        CHECK(r.metres > 1300.0, "the fast road is worth the extra distance");
    }

    api.step_label(&steps[0], label, sizeof label);
    CHECK(strstr(label, steps[0].road) == label, "the label opens with the road");
    CHECK(strstr(label, " m") != NULL, "and ends with the distance");
    api.step_label(&steps[n - 1u], label, sizeof label);
    CHECK_STR(label, "at the destination", "the arrival's label");

    api.route_free(&r);
    api.map_free(m);
}

/* ------------------------------------------------------------------ *
 * 7. The download, through a stand-in curl
 * ------------------------------------------------------------------ */

/* Written here rather than committed: it is eight lines, and what it has
 * to do -- read -o and --data-urlencode, note the query, copy the fixture
 * -- is specific to this app's argv. NDOSM_FAIL makes it fail the way a
 * phone with no bearer does. */
static bool install_fake_curl(void)
{
    char script[ND_PATH_MAX];
    FILE *f;
    const char *old = getenv("PATH");
    char path[ND_PATH_MAX * 2];

    if (nd_snprintf(g_bindir, sizeof g_bindir, "%s/bin", g_root) != ND_OK ||
        nd_snprintf(g_ctl, sizeof g_ctl, "%s/ctl", g_root) != ND_OK)
        return false;
    if (mkdir(g_bindir, 0755) != 0 || mkdir(g_ctl, 0755) != 0)
        return false;
    if (nd_snprintf(script, sizeof script, "%s/curl", g_bindir) != ND_OK)
        return false;
    f = fopen(script, "w");
    if (f == NULL)
        return false;
    (void)fputs("#!/bin/sh\n"
                "out=''; data=''\n"
                "while [ $# -gt 0 ]; do\n"
                "  case \"$1\" in\n"
                "    -o) out=\"$2\"; shift;;\n"
                "    --data-urlencode) data=\"$2\"; shift;;\n"
                "  esac\n"
                "  shift\n"
                "done\n"
                "printf '%s\\n' \"$data\" > \"$NDOSM_DIR/data\"\n"
                "if [ -n \"$NDOSM_FAIL\" ]; then\n"
                "  echo 'curl: (6) Could not resolve host: overpass-api.de' >&2; exit 6\n"
                "fi\n"
                "cp \"$NDOSM_FIXTURE\" \"$out\"\n",
                f);
    if (fclose(f) != 0 || chmod(script, 0755) != 0)
        return false;

    (void)nd_strlcpy(g_path_keep, (old != NULL) ? old : "/usr/bin:/bin", sizeof g_path_keep);
    if (nd_snprintf(path, sizeof path, "%s:%s", g_bindir, g_path_keep) != ND_OK)
        return false;
    return setenv("PATH", path, 1) == 0 && setenv("NDOSM_DIR", g_ctl, 1) == 0 &&
           setenv("NDOSM_FIXTURE", g_fixture, 1) == 0;
}

static void restore_path(void)
{
    (void)setenv("PATH", g_path_keep, 1);
    (void)unsetenv("NDOSM_FAIL");
}

static int64_t g_progress_bytes;
static int g_progress_calls;

static void on_progress(void *ctx, int64_t bytes)
{
    ND_UNUSED(ctx);
    g_progress_calls++;
    if (bytes > g_progress_bytes)
        g_progress_bytes = bytes;
}

static void test_the_query_names_the_box(void)
{
    char q[1024];

    CHECK_INT(api.fetch_query(q, sizeof q, TOWN_SOUTH, TOWN_WEST, TOWN_NORTH, TOWN_EAST, true),
              ND_OK, "a query");
    CHECK(strstr(q, "[bbox:53.3440000,-6.2760000,53.3560000,-6.2440000]") != NULL,
          "with the box spelled from the integers, south west north east");
    CHECK(strstr(q, "way[highway]") != NULL, "asking for roads");
    CHECK(strstr(q, "way[building]") != NULL, "and buildings");
    CHECK(strstr(q, "node[place][name]") != NULL, "and named places");
    CHECK(strstr(q, "(._;>;);") != NULL, "with every way's nodes");
    CHECK_INT(api.fetch_query(q, sizeof q, LAT(-0.5), LON(-0.25), 0, 0, false), ND_OK,
              "a box across the equator");
    CHECK(strstr(q, "[bbox:-0.5000000,-0.2500000,0.0000000,0.0000000]") != NULL,
          "keeps its minus signs on a negative fraction");
    CHECK(strstr(q, "way[building]") == NULL, "and leaves buildings out when asked");
    CHECK_INT(api.fetch_query(q, 16u, 0, 0, 0, 0, true), ND_ERR_TOOLONG, "a short buffer");
}

static void test_download_then_import(void)
{
    char query_path[512];
    char xml_path[512];
    char real[ND_PATH_MAX];
    char data[512];
    char why[128];
    FILE *f;
    nd_osm_import_stats stats;
    char map_path[512];
    char q[1024];
    nd_err rc;

    if (!install_fake_curl()) {
        CHECK(false, "the stand-in curl installs");
        return;
    }
    (void)nd_snprintf(query_path, sizeof query_path, "%s/.query.txt", ND_OSMAND_DATA_DIR);
    (void)nd_snprintf(xml_path, sizeof xml_path, "%s/.download.osm", ND_OSMAND_DATA_DIR);
    (void)api.fetch_query(q, sizeof q, TOWN_SOUTH, TOWN_WEST, TOWN_NORTH, TOWN_EAST, true);
    if (nd_path_resolve(real, sizeof real, query_path) != ND_OK || (f = fopen(real, "w")) == NULL) {
        CHECK(false, "the query file");
        restore_path();
        return;
    }
    (void)fputs(q, f);
    (void)fclose(f);

    g_progress_bytes = 0;
    g_progress_calls = 0;
    why[0] = '\0';
    rc = api.fetch(query_path, xml_path, on_progress, NULL, why, sizeof why);
    CHECK_INT(rc, ND_OK, "the download succeeds");
    if (rc != ND_OK)
        fprintf(stderr, "fetch: %s\n", why);
    CHECK(nd_path_is_file(xml_path), "and the answer is where it was asked to go");
    {
        char part[ND_PATH_MAX];

        (void)nd_snprintf(part, sizeof part, "%s.part", xml_path);
        CHECK(!nd_path_is_file(part), "the .part was renamed");
    }

    /* What curl was told. */
    (void)nd_snprintf(real, sizeof real, "%s/data", g_ctl);
    f = fopen(real, "r");
    data[0] = '\0';
    if (f != NULL) {
        if (fgets(data, sizeof data, f) == NULL)
            data[0] = '\0';
        (void)fclose(f);
    }
    data[strcspn(data, "\n")] = '\0';
    CHECK(strncmp(data, "data@", 5) == 0, "the query is posted from a file");
    CHECK(strstr(data, ".query.txt") != NULL, "the one that was written");

    /* And the real importer on what arrived. */
    memset(&stats, 0, sizeof stats);
    CHECK_INT(api.new_map_path("", map_path, sizeof map_path), ND_OK, "a path");
    rc = api.import(xml_path, map_path, "", TOWN_SOUTH, TOWN_WEST, TOWN_NORTH, TOWN_EAST, &stats,
                    why, sizeof why);
    CHECK_INT(rc, ND_OK, "imports");
    CHECK_INT(stats.n_roads, 12, "with the town's twelve roads");
    if (nd_path_resolve(real, sizeof real, xml_path) == ND_OK)
        (void)remove(real);
    if (nd_path_resolve(real, sizeof real, map_path) == ND_OK)
        (void)remove(real);

    /* A phone with no bearer. */
    (void)setenv("NDOSM_FAIL", "1", 1);
    why[0] = '\0';
    CHECK_INT(api.fetch(query_path, xml_path, NULL, NULL, why, sizeof why), ND_ERR_IO,
              "a failed transfer is reported");
    CHECK_STR(why, "No connection. Mobile data has to be working first.", "in the phone's words");
    CHECK(!nd_path_is_file(xml_path), "and leaves nothing behind");
    (void)unsetenv("NDOSM_FAIL");

    /* A phone with no curl at all. */
    (void)setenv("PATH", "/nonexistent-so-there-is-no-curl", 1);
    CHECK_INT(api.fetch(query_path, xml_path, NULL, NULL, why, sizeof why), ND_ERR_NOTFOUND,
              "no downloader");
    CHECK_STR(why, "This phone has no downloader.", "says so plainly");
    restore_path();

    if (nd_path_resolve(real, sizeof real, query_path) == ND_OK)
        (void)remove(real);
}

/* ------------------------------------------------------------------ *
 * 8. The frame
 * ------------------------------------------------------------------ */

static bool same_colour(nd_color c, uint8_t r, uint8_t g, uint8_t b)
{
    return c.r == r && c.g == g && c.b == b;
}

static int32_t count_colour(const nd_image *img, uint8_t r, uint8_t g, uint8_t b)
{
    int32_t n = 0;
    int32_t x;
    int32_t y;

    for (y = 0; y < img->h; y++) {
        for (x = 0; x < img->w; x++) {
            if (same_colour(nd_image_get_px(img, x, y), r, g, b))
                n++;
        }
    }
    return n;
}

static void test_the_town_renders_in_carto_colours(void)
{
    sa_fixture fx;
    nd_osm_map *m = NULL;
    nd_osm_map *maps[1];
    nd_osm_scene s;
    nd_image *surface;
    int32_t top;
    int32_t w;
    int32_t h;
    int32_t land;
    int32_t water;
    int32_t grass;
    int32_t white;
    int32_t px;
    int32_t py;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    if (api.map_load(g_map_path, &m) != ND_OK || m == NULL) {
        CHECK(false, "the town loads for rendering");
        sa_fx_free(&fx);
        return;
    }
    api.map_geometry(&fx.ui, &top, &w, &h);
    CHECK_INT(top, 31, "the map starts under the divider");
    CHECK_INT(w, 240, "the panel's width");
    CHECK_INT(h, 114, "down to the softkey bar");

    surface = nd_image_new_filled(w, h, ND_PIXFMT_RGB888, ND_BLACK);
    maps[0] = m;
    memset(&s, 0, sizeof s);
    s.ui = &fx.ui;
    s.scratch = api.scratch_new();
    s.maps = maps;
    s.n_maps = 1u;
    api.project(LAT(53.3500), LON(-6.2620), &s.view.cx, &s.view.cy);
    s.view.zoom = 14;
    if (surface == NULL || s.scratch == NULL) {
        CHECK(false, "surface and scratch");
        goto done;
    }

    /* Zoom 14 is 5.7 m a pixel here: the whole grid, the park and the
     * lake's eastern half fit on 240 px. */
    api.render(&s, surface);
    land = count_colour(surface, 0xf2, 0xef, 0xe9);
    water = count_colour(surface, 0xaa, 0xd3, 0xdf);
    grass = count_colour(surface, 0xcd, 0xeb, 0xb0);
    CHECK(water > 200, "the lake is blue");
    CHECK(grass > 100, "the park is green");
    CHECK(count_colour(surface, 0xe0, 0xdf, 0xdf) > 1000, "the town is residential grey");
    CHECK(land + water + grass < w * h, "and there is more on it than three colours");

    /* Zoom 16 is where the streets get their white fill and grey casing,
     * and where the building appears. */
    s.view.zoom = 16;
    api.render(&s, surface);
    white = count_colour(surface, 0xff, 0xff, 0xff);
    CHECK(white > 200, "the streets are white at zoom 16");
    CHECK(count_colour(surface, 0xbb, 0xbb, 0xbb) > 100, "with grey casings");
    CHECK(count_colour(surface, 0xd9, 0xd0, 0xc9) > 4, "and the building is drawn");
    s.view.zoom = 14;
    api.render(&s, surface);

    /* The lake's eastern shore is at -6.2680: 0.006 degrees west of the
     * view, which at zoom 14 and this latitude is about 70 px, so it is
     * well inside the left half of the surface. */
    api.project(LAT(53.3500), LON(-6.2685), &px, &py);
    {
        int32_t sx;
        int32_t sy;

        api.view_to_screen(&s.view, w, h, px, py, &sx, &sy);
        CHECK(sx >= 0 && sx < w && sy >= 0 && sy < h, "the shore is on the surface");
        CHECK(same_colour(nd_image_get_px(surface, sx, sy), 0xaa, 0xd3, 0xdf),
              "and the pixel there is water");
    }

    /* The whole screen: chrome above, map in the middle, softkey below. */
    {
        nd_softkey bar;
        const nd_image *frame;
        int32_t x;
        int32_t white_title = 0;
        int32_t black_strip = 0;

        nd_softkey_init(&bar, &fx.ui, false);
        nd_softkey_update(&bar, "Options", false);
        api.scene_draw(&s, surface);
        frame = nd_capture_recent(fx.cap, 0u);
        CHECK(frame != NULL, "a frame was presented");
        if (frame != NULL) {
            for (x = 0; x < frame->w; x++) {
                nd_color c = nd_image_get_px(frame, x, 30);

                if (c.r == 255u && c.g == 255u && c.b == 255u)
                    white_title++;
            }
            CHECK_INT(white_title, 240, "the divider is one white row at y = 30");
            /* The strip is the app's opaque bar over the chrome background
             * -- the wallpaper here, as in every stock app's frame -- with
             * "Options" on it. What it must not be is map. */
            for (x = 0; x < frame->w; x++) {
                nd_color c = nd_image_get_px(frame, x, 160);

                if (same_colour(c, 0xf2, 0xef, 0xe9) || same_colour(c, 0xe0, 0xdf, 0xdf))
                    black_strip++;
            }
            CHECK_INT(black_strip, 0, "no map colour reaches the softkey strip");
            {
                int32_t label = 0;

                for (x = 0; x < frame->w; x++) {
                    nd_color c = nd_image_get_px(frame, x, 160);

                    if (c.r == 255u && c.g == 255u && c.b == 255u)
                        label++;
                }
                CHECK(label > 10, "and the softkey label is on it");
            }
            CHECK(same_colour(nd_image_get_px(frame, 120, 31 + 3), 0xf2, 0xef, 0xe9) ||
                      !same_colour(nd_image_get_px(frame, 120, 31 + 3), 0, 0, 0),
                  "the map begins on the row under the divider");
        }
    }

done:
    api.scratch_free(s.scratch);
    nd_image_free(surface);
    api.map_free(m);
    sa_fx_free(&fx);
}

static void test_the_scale_bar(void)
{
    nd_osm_view v = centred_view(15);
    int32_t at15 = api.scale_metres(&v, 240);
    int32_t at16;

    v.zoom = 16;
    at16 = api.scale_metres(&v, 240);
    CHECK(at15 == 100 || at15 == 200 || at15 == 500, "a round number of metres at zoom 15");
    CHECK(at16 < at15, "and a shorter one a zoom in");
    v.zoom = ND_OSM_ZOOM_MIN;
    CHECK(api.scale_metres(&v, 240) >= 2000, "kilometres at the widest");
}

/* ------------------------------------------------------------------ *
 * 9. The map loop, and teardown
 * ------------------------------------------------------------------ */

static void test_clear_leaves_the_map(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* Held rather than queued: the map's loop polls with a timeout, so a
     * held key and its synthesised repeat both arrive. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }
    nd_vclock_enable();
    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run returns 0 on C");
    CHECK(nd_capture_frames_drawn(fx.cap) >= 1u, "the map was drawn");
    nd_vclock_disable();
    /* The view is saved on the way out, under the scratch root. */
    CHECK(nd_path_is_file(ND_PATH_SETTINGS_PROP), "and the view was saved");
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    sa_checks++;
}

/* THE SIGTERM TEARDOWN CONTRACT (nd_app.h). The flag is raised BEFORE
 * app_run() so the test is deterministic: the first time the map's loop
 * reaches its poll it returns, whatever the key channel holds.
 *
 * MUST RUN LAST. There is no way to lower g_should_exit again. */
static void test_sigterm_leaves_the_map(void)
{
    sa_fixture fx;
    int rc;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* A key that would otherwise loop for ever: 6 pans east a quarter of a
     * screen at a time and never leaves. */
    if (!sa_hold(&fx, ND_KEY_6)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        return;
    }

    CHECK_INT(nd_app_install_signal_handlers(), ND_OK, "handlers install");
    CHECK(!nd_app_should_exit(), "not yet");
    if (kill(getpid(), SIGTERM) != 0) {
        CHECK(false, "raise SIGTERM");
        sa_fx_free(&fx);
        return;
    }
    CHECK(nd_app_should_exit(), "the handler set the flag");

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    CHECK_INT(rc, 0, "app_run returns rather than panning for ever");
    nd_vclock_disable();

    sa_fx_free(&fx);
}

int main(void)
{
    void *h = sa_begin("OsmAnd", "ndosmand");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!find_fixture()) {
        fprintf(stderr, "test_osmand_app: cannot find neodct/tests/osmand/town.osm\n");
        (void)dlclose(h);
        return 1;
    }
    if (!scratch_root_begin()) {
        fprintf(stderr, "test_osmand_app: cannot make a scratch root\n");
        (void)dlclose(h);
        return 1;
    }

    RUN(test_rows);
    RUN(test_projection_round_trips);
    RUN(test_the_download_box);
    RUN(test_tags_become_kinds);

    RUN(test_the_3x3_block_is_the_d_pad);
    RUN(test_the_rocker_and_the_outer_keys_agree_with_the_block);
    RUN(test_every_movement_is_reachable_without_left_or_right);
    RUN(test_the_edges_of_the_map_are_steady);

    RUN(test_the_town_imports);
    RUN(test_a_second_import_gets_a_new_name);
    RUN(test_a_file_that_is_not_a_map_is_refused);

    RUN(test_routes_honour_one_way_streets_by_car_only);
    RUN(test_speed_is_the_whole_policy);
    RUN(test_directions);

    RUN(test_the_query_names_the_box);
    RUN(test_download_then_import);

    RUN(test_the_town_renders_in_carto_colours);
    RUN(test_the_scale_bar);

    RUN(test_clear_leaves_the_map);
    RUN(test_null_safety);

    /* Last: nd_app_should_exit() cannot be lowered again. */
    RUN(test_sigterm_leaves_the_map);

    scratch_root_end();
    return sa_end(h, "test_osmand_app");
}
