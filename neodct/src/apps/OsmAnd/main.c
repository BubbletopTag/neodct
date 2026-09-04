/* apps/OsmAnd/main.c -- the OsmAnd app, app id 13.
 *
 * An offline OpenStreetMap viewer and router for a phone with sixteen keys
 * and no GPS. The map itself is render.c, the data is mapdata.c, the
 * routing is route.c and the download is fetch.c; this file is the screens
 * around them and the one entry point nd-apprun calls.
 *
 * ============ WHY THE MAP IS THE APP ============
 *
 * Every other stock app opens on a PagedList of its screens. This one
 * opens on the map, because a map's front page IS the map -- a list whose
 * first row was "View map" would be a door in front of a door. The screens
 * the PagedList would have held are behind the NaviKey instead, which on
 * this phone is where a Nokia puts Options. Calendar made the same choice
 * for the same reason.
 *
 * ============ THE ONE CURSOR ============
 *
 * There is no pointer and no position. The crosshair at the centre of the
 * map is what every verb acts on: "Start here" and "Route to here" mark
 * what is under it, "Download map" fetches the area around it, and "Find"
 * and "Go to" move it. That is one idea to learn rather than four, and it
 * is the idea a GPS fix will slot into when there is one.
 *
 * ============ WHAT IS DELIBERATELY NOT HERE ============
 *
 * No tiles. A raster tile is 256x256 for a screen that is 240x145 of map,
 * the tile servers' policy forbids bulk download, and a tile cannot be
 * routed over. Vector data drawn here serves the map, the search and the
 * router from one download.
 *
 * No online routing and no online search. The phone's modem spends most of
 * its life detached, and a map that quietly fails without a network is
 * worse than one that never claimed to need it. Everything works off the
 * card once a map is on it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osmand_app.h"

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#define LOG_TAG "OsmAnd"

const char *const nd_osmand_app_title = "OsmAnd";

const char *const nd_osmand_options[ND_OSMAND_OPTIONS_COUNT] = {
    "Route to here", "Start here",   "Directions",     "Clear route", "Find",
    "Go to",         "Download map", "Installed maps", "Route mode",
};

const char *const nd_osmand_size_names[ND_OSMAND_SIZES_COUNT] = {
    "Small (2 km)",
    "Medium (5 km)",
    "Large (10 km, no buildings)",
};
const double nd_osmand_size_km[ND_OSMAND_SIZES_COUNT] = {2.0, 5.0, 10.0};
const bool nd_osmand_size_buildings[ND_OSMAND_SIZES_COUNT] = {true, true, false};

const char *const nd_osmand_msg_no_maps = "No maps yet.\n\nDownload the area under the cross.";
const char *const nd_osmand_msg_no_start = "No start point.\n\nChoose Start here first.";
const char *const nd_osmand_msg_no_route = "No road joins those two points on this map.";
const char *const nd_osmand_msg_off_map = "That point is on no downloaded map.";
const char *const nd_osmand_msg_two_maps = "Start and end must be on the same map.";
const char *const nd_osmand_msg_no_card = "No memory card.\n\nMaps are kept on the card.";
const char *const nd_osmand_msg_too_many = "Too many maps loaded.\n\nDelete one first.";
const char *const nd_osmand_msg_not_found = "Nothing by that name on the loaded maps.";

/* ------------------------------------------------------------------ *
 * The key map
 * ------------------------------------------------------------------ */

nd_osm_nav nd_osm_map_key(int32_t key, nd_osm_view *v, int32_t map_w, int32_t map_h,
                          const nd_osm_mark *jump)
{
    int64_t unit;
    int64_t step_x;
    int64_t step_y;
    int64_t world;

    if (v == NULL)
        return ND_OSM_NAV_NONE;
    v->zoom = nd_clamp32(v->zoom, ND_OSM_ZOOM_MIN, ND_OSM_ZOOM_MAX);
    unit = (int64_t)1 << (ND_OSM_MERC_ZOOM - v->zoom);
    world = (int64_t)256 << ND_OSM_MERC_ZOOM;
    /* A quarter of the view per press: far enough that holding a key
     * crosses a town, near enough that a street does not jump out from
     * under the cross. */
    step_x = (int64_t)nd_max32(map_w / 4, 1) * unit;
    step_y = (int64_t)nd_max32(map_h / 4, 1) * unit;

    switch (key) {
    case ND_KEY_ENTER:
        return ND_OSM_NAV_OPTIONS;
    case ND_KEY_CLEAR:
        return ND_OSM_NAV_BACK;

    /* The rocker and the 3x3 block agree; Left and Right exist only on a
     * development keyboard and are folded into 4 and 6 rather than given a
     * meaning the phone could not reach. */
    case ND_KEY_UP:
    case ND_KEY_2:
        v->cy = (int32_t)nd_max32((int32_t)((int64_t)v->cy - step_y), 0);
        return ND_OSM_NAV_MOVED;
    case ND_KEY_DOWN:
    case ND_KEY_8:
        v->cy =
            (int32_t)((int64_t)v->cy + step_y < world - 1 ? (int64_t)v->cy + step_y : world - 1);
        return ND_OSM_NAV_MOVED;
    case ND_KEY_LEFT:
    case ND_KEY_4:
        v->cx = (int32_t)nd_max32((int32_t)((int64_t)v->cx - step_x), 0);
        return ND_OSM_NAV_MOVED;
    case ND_KEY_RIGHT:
    case ND_KEY_6:
        v->cx =
            (int32_t)((int64_t)v->cx + step_x < world - 1 ? (int64_t)v->cx + step_x : world - 1);
        return ND_OSM_NAV_MOVED;

    case ND_KEY_1:
    case ND_KEY_STAR:
        v->zoom = nd_max32(v->zoom - 1, ND_OSM_ZOOM_MIN);
        return ND_OSM_NAV_MOVED;
    case ND_KEY_3:
    case ND_KEY_HASH:
        v->zoom = nd_min32(v->zoom + 1, ND_OSM_ZOOM_MAX);
        return ND_OSM_NAV_MOVED;

    case ND_KEY_5:
        /* The middle of the block is where the journey is: the destination
         * when there is one, the start otherwise. */
        if (jump == NULL || !jump->set)
            return ND_OSM_NAV_NONE;
        v->cx = jump->mx;
        v->cy = jump->my;
        return ND_OSM_NAV_MOVED;

    default:
        return ND_OSM_NAV_NONE;
    }
}

/* ------------------------------------------------------------------ *
 * The app's state
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui *ui;
    nd_osm_map *maps[ND_OSMAND_MAX_MAPS];
    size_t n_maps;
    nd_osm_view view;
    nd_osm_mark start;
    nd_osm_mark dest;
    nd_osm_route route;
    size_t route_map;
    nd_osm_mode mode;
    nd_image *surface; /* the map rows, 240 x 145 RGB888 = 104,400 bytes */
    nd_osm_scratch *scratch;
} app_state;

/* ------------------------------------------------------------------ *
 * Small shared screens
 * ------------------------------------------------------------------ */

static void say(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_osmand_app_title);
    (void)nd_msgdialog_show(&dialog);
}

/* Yes on the NaviKey, no on C -- the two keys the dialog already accepts by
 * default, so nothing has to be taught a third one. */
static bool confirm(nd_ui *ui, const char *message, const char *button)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_osmand_app_title);
    nd_msgdialog_set_button(&dialog, button);
    return nd_msgdialog_show(&dialog) == ND_KEY_ENTER;
}

/* A VerticalList opened on the value already in force, which is what every
 * other list of choices in this OS does. Returns the index, or -1 for Back. */
static int32_t pick(nd_ui *ui, const char *title, const char *const *items, size_t n,
                    size_t current)
{
    nd_vlist menu;
    nd_softkey bar;

    nd_vlist_init(&menu, ui, title, items, n, ND_OSMAND_APP_ID);
    if (current < n)
        menu.selected_index = current;

    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Select", false);

    return nd_vlist_show(&menu);
}

/* ------------------------------------------------------------------ *
 * Settings
 * ------------------------------------------------------------------ */

static bool read_double(const char *key, double *out)
{
    char buf[48];
    char *end;
    double v;

    if (nd_settings_get_copy(key, "", buf, sizeof buf) != ND_OK || buf[0] == '\0')
        return false;
    v = strtod(buf, &end);
    if (end == buf || *end != '\0')
        return false;
    *out = v;
    return true;
}

static void load_settings(app_state *app)
{
    double lat = ND_OSMAND_DEFAULT_LAT;
    double lon = ND_OSMAND_DEFAULT_LON;
    double zoom = ND_OSMAND_DEFAULT_ZOOM;
    double mode = 0.0;
    double l;

    if (read_double(ND_SET_OSMAND_LAT, &l) && l >= -90.0 && l <= 90.0)
        lat = l;
    if (read_double(ND_SET_OSMAND_LON, &l) && l >= -180.0 && l <= 180.0)
        lon = l;
    if (read_double(ND_SET_OSMAND_ZOOM, &l))
        zoom = l;
    if (read_double(ND_SET_OSMAND_MODE, &l))
        mode = l;

    nd_osm_project(nd_trunc32(lat * (double)ND_OSM_DEG), nd_trunc32(lon * (double)ND_OSM_DEG),
                   &app->view.cx, &app->view.cy);
    app->view.zoom = nd_clamp32(nd_trunc32(zoom), ND_OSM_ZOOM_MIN, ND_OSM_ZOOM_MAX);
    app->mode = (mode == 1.0) ? ND_OSM_MODE_FOOT : ND_OSM_MODE_CAR;

    /* The start point defaults to where the map opened. Without a GPS that
     * is the best guess there is for "where I am", and it means "Route to
     * here" works on the first press rather than after a trip to Options. */
    app->start.mx = app->view.cx;
    app->start.my = app->view.cy;
    app->start.set = true;
}

/* Written ONCE, on exit. nd_settings.h's R-24 note: every set is a full
 * rewrite of settings.prop, so the view is not saved per pan. */
static void save_settings(const app_state *app)
{
    char buf[48];
    double lat;
    double lon;

    nd_osm_unproject(app->view.cx, app->view.cy, &lat, &lon);
    (void)nd_snprintf(buf, sizeof buf, "%.6f", lat);
    (void)nd_settings_set(ND_SET_OSMAND_LAT, buf);
    (void)nd_snprintf(buf, sizeof buf, "%.6f", lon);
    (void)nd_settings_set(ND_SET_OSMAND_LON, buf);
    (void)nd_snprintf(buf, sizeof buf, "%d", (int)app->view.zoom);
    (void)nd_settings_set(ND_SET_OSMAND_ZOOM, buf);
    (void)nd_settings_set(ND_SET_OSMAND_MODE, (app->mode == ND_OSM_MODE_FOOT) ? "1" : "0");
}

/* ------------------------------------------------------------------ *
 * The maps on the card
 * ------------------------------------------------------------------ */

static void clear_route(app_state *app)
{
    nd_osm_route_free(&app->route);
    app->route_map = 0u;
}

static void free_maps(app_state *app)
{
    size_t i;

    clear_route(app);
    for (i = 0u; i < app->n_maps; i++)
        nd_osm_map_free(app->maps[i]);
    memset(app->maps, 0, sizeof app->maps);
    app->n_maps = 0u;
}

/* Every .ndmap on the card, up to the ceiling. One that will not load is
 * logged and skipped rather than refused as a whole: a corrupt file must
 * not take the other five maps down with it. */
static void load_maps(app_state *app)
{
    char(*paths)[512];
    size_t n;
    size_t i;

    free_maps(app);
    /* 6 * 512 bytes, briefly; freed below. */
    paths = calloc(ND_OSMAND_MAX_MAPS, sizeof *paths);
    if (paths == NULL)
        return;
    n = nd_osm_list_maps(paths, ND_OSMAND_MAX_MAPS);
    for (i = 0u; i < n; i++) {
        nd_osm_map *m = NULL;

        if (nd_osm_map_load(paths[i], &m) != ND_OK)
            continue;
        app->maps[app->n_maps++] = m;
    }
    free(paths);
    nd_log(LOG_TAG, "%u map(s) loaded", (unsigned)app->n_maps);
}

/* The map a point is on, or SIZE_MAX. */
static size_t map_at(const app_state *app, int32_t mx, int32_t my)
{
    size_t i;

    for (i = 0u; i < app->n_maps; i++) {
        if (nd_osm_map_contains(app->maps[i], mx, my))
            return i;
    }
    return SIZE_MAX;
}

/* ------------------------------------------------------------------ *
 * The map screen
 * ------------------------------------------------------------------ */

static void scene_from(const app_state *app, nd_osm_scene *s)
{
    memset(s, 0, sizeof *s);
    s->ui = app->ui;
    s->scratch = app->scratch;
    s->maps = app->maps;
    s->n_maps = app->n_maps;
    s->view = app->view;
    s->start = app->start;
    s->dest = app->dest;
    s->route = app->route.nodes;
    s->n_route = app->route.n_nodes;
    s->route_map = app->route_map;
}

static void map_repaint(void *ctx)
{
    const app_state *app = (const app_state *)ctx;
    nd_osm_scene s;

    scene_from(app, &s);
    nd_osm_scene_draw(&s, app->surface);
}

/* Draw, loop on keys, and return ND_OSM_NAV_OPTIONS on NaviKey or
 * ND_OSM_NAV_BACK on C (and on SIGTERM). */
static nd_osm_nav show_map(app_state *app)
{
    nd_ui *ui = app->ui;
    nd_softkey bar;
    nd_ui_repaint saved;
    nd_osm_nav out;
    int32_t map_w;
    int32_t map_h;

    nd_osm_map_geometry(ui, NULL, &map_w, &map_h);

    /* Painted, NOT presented: the draw below clears only rows
     * 0..content_bottom, so this label is pushed to the panel as part of the
     * map's own frame. One repaint, not two. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Options", false);
    map_repaint(app);
    saved = nd_ui_set_repaint(ui, map_repaint, app);

    for (;;) {
        int32_t key;
        nd_osm_nav nav;
        const nd_osm_mark *jump;

        /* A poll rather than nd_ui_wait_for_key(), so that the teardown
         * contract in nd_app.h is honoured from the app's OUTERMOST loop:
         * this is the screen an incoming call arrives at. The timeout is
         * nd_ui_widget_timeout() so an animated wallpaper behind the
         * chrome keeps its own rate; it only ever shortens the wait. */
        key = nd_ui_read_keypress(ui, nd_ui_widget_timeout(ui, 0.1));
        if (nd_app_should_exit()) {
            out = ND_OSM_NAV_BACK;
            break;
        }
        if (key == ND_KEY_NONE)
            continue;

        jump = app->dest.set ? &app->dest : &app->start;
        nav = nd_osm_map_key(key, &app->view, map_w, map_h, jump);
        if (nav == ND_OSM_NAV_OPTIONS || nav == ND_OSM_NAV_BACK) {
            out = nav;
            break;
        }
        if (nav == ND_OSM_NAV_MOVED)
            map_repaint(app);
    }

    nd_ui_restore_repaint(ui, saved);
    return out;
}

/* ------------------------------------------------------------------ *
 * Routing
 * ------------------------------------------------------------------ */

static void compute_route(app_state *app)
{
    nd_ui *ui = app->ui;
    size_t ms;
    size_t md;
    uint32_t from;
    uint32_t to;
    double d_from = 0.0;
    double d_to = 0.0;
    nd_osm_route route;
    nd_err rc;
    char dist[16];
    char body[96];
    int32_t minutes;

    clear_route(app);
    if (app->n_maps == 0u) {
        say(ui, nd_osmand_msg_no_maps);
        return;
    }
    if (!app->start.set) {
        say(ui, nd_osmand_msg_no_start);
        return;
    }
    ms = map_at(app, app->start.mx, app->start.my);
    md = map_at(app, app->dest.mx, app->dest.my);
    if (ms == SIZE_MAX || md == SIZE_MAX) {
        say(ui, nd_osmand_msg_off_map);
        return;
    }
    if (ms != md) {
        /* Two overlapping maps can both contain both points; then either
         * will do and the first wins. Different maps cannot be joined. */
        if (nd_osm_map_contains(app->maps[ms], app->dest.mx, app->dest.my))
            md = ms;
        else if (nd_osm_map_contains(app->maps[md], app->start.mx, app->start.my))
            ms = md;
        else {
            say(ui, nd_osmand_msg_two_maps);
            return;
        }
    }

    /* Building the graph and searching it is a second or two on a big
     * town; the screen says so rather than sitting on the options list. */
    {
        /* Drawn, not shown: nd_infoscreen_show() would wait for a key,
         * and nobody should have to press one to let the router run. The
         * progress screen is the one widget that draws without looping,
         * which is why Update uses it for "Checking for updates" too. */
        nd_progress busy;

        nd_progress_init(&busy, ui, "Planning the route", nd_osmand_app_title, NULL, NULL, NULL);
        (void)nd_progress_draw(&busy, 0, 1);
    }
    if (nd_osm_graph_build(app->maps[ms]) != ND_OK) {
        say(ui, "Not enough memory to plan a route on this map.");
        return;
    }
    from = nd_osm_nearest_node(app->maps[ms], app->start.mx, app->start.my, app->mode, &d_from);
    to = nd_osm_nearest_node(app->maps[ms], app->dest.mx, app->dest.my, app->mode, &d_to);
    if (from == UINT32_MAX || to == UINT32_MAX || d_from > 2000.0 || d_to > 2000.0) {
        say(ui, nd_osmand_msg_no_route);
        return;
    }

    rc = nd_osm_route_find(app->maps[ms], from, to, app->mode, &route);
    if (rc == ND_ERR_NOTFOUND) {
        say(ui, nd_osmand_msg_no_route);
        return;
    }
    if (rc != ND_OK) {
        say(ui, "Not enough memory to plan a route on this map.");
        return;
    }
    app->route = route;
    app->route_map = ms;

    nd_osm_format_distance(route.metres, dist, sizeof dist);
    minutes = nd_trunc32(route.seconds / 60.0 + 0.5);
    if (minutes < 1)
        minutes = 1;
    (void)nd_snprintf(body, sizeof body, "%s, about %d min\n%s", dist, (int)minutes,
                      nd_osmand_mode_names[app->mode]);
    nd_log(LOG_TAG, "route: %u nodes, %.0f m, %.0f s", (unsigned)route.n_nodes, route.metres,
           route.seconds);
    say(ui, body);
}

static void route_here(app_state *app)
{
    app->dest.mx = app->view.cx;
    app->dest.my = app->view.cy;
    app->dest.set = true;
    compute_route(app);
}

static void start_here(app_state *app)
{
    app->start.mx = app->view.cx;
    app->start.my = app->view.cy;
    app->start.set = true;
    if (app->dest.set)
        compute_route(app);
}

/* The turn list, one turn per page in big type with the road and distance
 * under it -- the PagedList idiom Clock uses for its rows' values, which
 * here is exactly what a direction is. NaviKey on a page centres the map
 * on that turn. */
typedef struct {
    nd_osm_step steps[ND_OSM_STEPS_MAX];
    char values[ND_OSM_STEPS_MAX][64];
    const char *items[ND_OSM_STEPS_MAX];
    const char *value_ptrs[ND_OSM_STEPS_MAX];
} directions_list;

static void directions(app_state *app)
{
    nd_ui *ui = app->ui;
    directions_list *dl;
    size_t n;
    size_t i;
    nd_pagedlist page;
    int32_t choice;

    if (app->route.n_nodes < 2u || app->route_map >= app->n_maps) {
        say(ui, "No route yet.\n\nChoose Route to here first.");
        return;
    }
    /* 64 * (64 + 64 + 16) bytes is about 9 kB: heap, not an app's stack. */
    dl = calloc(1u, sizeof *dl);
    if (dl == NULL) {
        say(ui, "Not enough memory to list the directions.");
        return;
    }
    n = nd_osm_route_steps(app->maps[app->route_map], &app->route, dl->steps, ND_OSM_STEPS_MAX);
    for (i = 0u; i < n; i++) {
        dl->items[i] = nd_osmand_turn_names[dl->steps[i].turn];
        nd_osm_step_label(&dl->steps[i], dl->values[i], sizeof dl->values[i]);
        dl->value_ptrs[i] = dl->values[i];
    }

    nd_pagedlist_init(&page, ui, "Directions", dl->items, n, ND_OSMAND_APP_ROOT, true);
    nd_pagedlist_set_values(&page, dl->value_ptrs);
    choice = nd_pagedlist_show(&page);
    if (choice >= 0 && (size_t)choice < n) {
        const nd_osm_map *m = app->maps[app->route_map];
        uint32_t node = dl->steps[choice].node;

        if (node < m->n_nodes) {
            app->view.cx = m->mx[node];
            app->view.cy = m->my[node];
            if (app->view.zoom < 16)
                app->view.zoom = 16;
        }
    }
    free(dl);
}

/* ------------------------------------------------------------------ *
 * Find
 * ------------------------------------------------------------------ */

#define FIND_MAX 12

typedef struct {
    size_t map;
    bool is_place;
    uint32_t index; /* place index, or way index */
    char row[ND_OSM_NAME_MAX];
} find_hit;

typedef struct {
    find_hit hits[FIND_MAX];
    const char *rows[FIND_MAX];
    size_t n;
} find_list;

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;

    if (nl == 0u)
        return false;
    for (i = 0u; hay[i] != '\0'; i++) {
        size_t j;

        for (j = 0u; j < nl; j++) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];

            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + 32u);
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + 32u);
            if (a != b)
                break;
        }
        if (j == nl)
            return true;
        if (hay[i + j] == '\0')
            return false;
    }
    return false;
}

static bool already_hit(const find_list *fl, const char *name)
{
    size_t i;

    for (i = 0u; i < fl->n; i++) {
        if (strcmp(fl->hits[i].row, name) == 0)
            return true;
    }
    return false;
}

/* Places first, then streets, across every loaded map, until the list is
 * full. A street is many ways and is listed once. */
static void find_matches(const app_state *app, const char *needle, find_list *fl)
{
    size_t mi;

    fl->n = 0u;
    for (mi = 0u; mi < app->n_maps && fl->n < FIND_MAX; mi++) {
        const nd_osm_map *m = app->maps[mi];
        uint32_t i;

        for (i = 0u; i < m->n_places && fl->n < FIND_MAX; i++) {
            const char *name = nd_osm_place_name(m, &m->places[i]);

            if (!contains_nocase(name, needle) || already_hit(fl, name))
                continue;
            fl->hits[fl->n].map = mi;
            fl->hits[fl->n].is_place = true;
            fl->hits[fl->n].index = i;
            (void)nd_strlcpy(fl->hits[fl->n].row, name, sizeof fl->hits[fl->n].row);
            fl->n++;
        }
    }
    for (mi = 0u; mi < app->n_maps && fl->n < FIND_MAX; mi++) {
        const nd_osm_map *m = app->maps[mi];
        uint32_t i;

        for (i = m->kind_begin[ND_OSM_KIND_FIRST_ROAD]; i < m->n_ways && fl->n < FIND_MAX; i++) {
            const char *name = nd_osm_way_name(m, &m->ways[i]);

            if (name[0] == '\0' || !contains_nocase(name, needle) || already_hit(fl, name))
                continue;
            fl->hits[fl->n].map = mi;
            fl->hits[fl->n].is_place = false;
            fl->hits[fl->n].index = i;
            (void)nd_strlcpy(fl->hits[fl->n].row, name, sizeof fl->hits[fl->n].row);
            fl->n++;
        }
    }
}

static void go_to_hit(app_state *app, const find_hit *h)
{
    const nd_osm_map *m = app->maps[h->map];

    if (h->is_place) {
        const nd_osm_place *p = &m->places[h->index];

        app->view.cx = p->mx;
        app->view.cy = p->my;
        app->view.zoom = (p->kind >= ND_OSM_PLACE_TOWN) ? 13 : 15;
    } else {
        const nd_osm_way *w = &m->ways[h->index];
        uint32_t mid = m->refs[w->first_ref + w->n_refs / 2u];

        app->view.cx = m->mx[mid];
        app->view.cy = m->my[mid];
        app->view.zoom = 16;
    }
}

static void find(app_state *app)
{
    nd_ui *ui = app->ui;
    nd_textinput field;
    char needle[ND_OSM_NAME_MAX];
    find_list *fl;
    nd_vlist list;
    nd_softkey bar;
    int32_t choice;
    size_t i;

    if (app->n_maps == 0u) {
        say(ui, nd_osmand_msg_no_maps);
        return;
    }
    needle[0] = '\0';
    if (nd_textinput_init(&field, ui, nd_osmand_app_title, "Find:", needle, sizeof needle, "",
                          ND_T9_FILTER_ANY) != ND_OK)
        return;
    if (nd_textinput_show(&field) == NULL || needle[0] == '\0')
        return;

    fl = calloc(1u, sizeof *fl);
    if (fl == NULL)
        return;
    find_matches(app, needle, fl);
    if (fl->n == 0u) {
        say(ui, nd_osmand_msg_not_found);
        free(fl);
        return;
    }
    for (i = 0u; i < fl->n; i++) {
        /* VerticalList draws a row as it is given it, so a long name would
         * run out under the scrollbar. 215 px is the width a row has. */
        (void)nd_text_ellipsize(fl->hits[i].row, sizeof fl->hits[i].row, fl->hits[i].row,
                                (ui->font_md != NULL) ? ui->font_md : ui->font_n, 215);
        fl->rows[i] = fl->hits[i].row;
    }

    nd_vlist_init(&list, ui, "Found", fl->rows, fl->n, ND_OSMAND_APP_ID);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Go", false);
    choice = nd_vlist_show(&list);
    if (choice >= 0 && (size_t)choice < fl->n)
        go_to_hit(app, &fl->hits[choice]);
    free(fl);
}

/* ------------------------------------------------------------------ *
 * Go to
 * ------------------------------------------------------------------ */

/* One masked field. false when it was cancelled. Empty on purpose: a masked
 * field that is already full ignores every keypress until something is
 * deleted (see apps/Clock), so nothing is prefilled. */
static bool ask_masked(nd_ui *ui, const char *prompt, const char *mask, char *out, size_t out_sz)
{
    nd_textinput field;

    out[0] = '\0';
    if (nd_textinput_init(&field, ui, nd_osmand_app_title, prompt, out, out_sz, "",
                          ND_T9_FILTER_NUMBERS) != ND_OK)
        return false;
    nd_textinput_set_mask(&field, mask);
    return nd_textinput_show(&field) != NULL;
}

/* Latitude and longitude, each as a hemisphere then digits, because a
 * masked numeric field has no minus sign and a keypad has no way to type
 * one that a person would guess. Four short screens for a thing done once
 * a year. */
static void go_to(app_state *app)
{
    static const char *const NS[] = {"North", "South"};
    static const char *const EW[] = {"East", "West"};
    nd_ui *ui = app->ui;
    char text[16];
    int32_t hemi;
    double lat;
    double lon;

    hemi = pick(ui, "Latitude", NS, 2u, 0u);
    if (hemi < 0)
        return;
    if (!ask_masked(ui, "Latitude:", "##.####", text, sizeof text))
        return;
    lat = strtod(text, NULL);
    if (hemi == 1)
        lat = -lat;
    if (lat < -85.0 || lat > 85.0) {
        say(ui, "Not a latitude.\n\nTwo digits, a point, four digits.");
        return;
    }

    hemi = pick(ui, "Longitude", EW, 2u, 0u);
    if (hemi < 0)
        return;
    if (!ask_masked(ui, "Longitude:", "###.####", text, sizeof text))
        return;
    lon = strtod(text, NULL);
    if (hemi == 1)
        lon = -lon;
    if (lon < -180.0 || lon > 180.0) {
        say(ui, "Not a longitude.\n\nThree digits, a point, four digits.");
        return;
    }

    nd_osm_project(nd_trunc32(lat * (double)ND_OSM_DEG), nd_trunc32(lon * (double)ND_OSM_DEG),
                   &app->view.cx, &app->view.cy);
}

/* ------------------------------------------------------------------ *
 * Download
 * ------------------------------------------------------------------ */

typedef struct {
    nd_progress *bar;
    int64_t estimate; /* what an area this size usually weighs */
    int64_t shown;
} download_ctx;

/* "1.2 MB" beside the percentage. The percentage itself is against an
 * estimate, because Overpass streams with no Content-Length; the bar moves
 * so the phone is visibly not stuck, and the number beside it is the
 * truth. */
static void download_detail(void *ctx, int64_t done, int64_t total, char *out, size_t out_sz)
{
    const download_ctx *dc = (const download_ctx *)ctx;

    ND_UNUSED(done);
    ND_UNUSED(total);
    if (dc->shown < 1024 * 1024)
        (void)nd_snprintf(out, out_sz, "%d kB", (int)(dc->shown / 1024));
    else
        (void)nd_snprintf(out, out_sz, "%.1f MB", (double)dc->shown / (1024.0 * 1024.0));
}

static void download_progress(void *ctx, int64_t bytes)
{
    download_ctx *dc = (download_ctx *)ctx;
    int64_t pct;

    if (bytes < dc->shown + 32 * 1024)
        return;
    dc->shown = bytes;
    pct = bytes * 90 / dc->estimate;
    if (pct > 90)
        pct = 90;
    /* Forced, because the bar redraws only when the percentage moves and
     * the byte count beside it should move regardless. */
    nd_progress_set_step(dc->bar, "Downloading");
    (void)nd_progress_draw(dc->bar, pct, 100);
}

static void download(app_state *app)
{
    nd_ui *ui = app->ui;
    int32_t size;
    int32_t south;
    int32_t west;
    int32_t north;
    int32_t east;
    double lat;
    double lon;
    char body[96];
    char query[1024];
    char query_path[ND_PATH_MAX];
    char xml_path[ND_PATH_MAX];
    char map_path[ND_PATH_MAX];
    char why[128];
    FILE *f;
    char real[ND_PATH_MAX];
    nd_progress bar;
    download_ctx dc;
    nd_osm_import_stats stats;
    nd_err rc;

    size = pick(ui, "Area size", nd_osmand_size_names, ND_OSMAND_SIZES_COUNT, 1u);
    if (size < 0)
        return;

    nd_osm_unproject(app->view.cx, app->view.cy, &lat, &lon);
    (void)nd_snprintf(body, sizeof body, "Download %s around the cross?\n\nUses mobile data.",
                      nd_osmand_size_names[size]);
    if (!confirm(ui, body, "Download"))
        return;

    if (nd_mkdir_p(ND_OSMAND_DATA_DIR, 0755u) != ND_OK) {
        say(ui, nd_osmand_msg_no_card);
        return;
    }
    if (app->n_maps >= ND_OSMAND_MAX_MAPS) {
        say(ui, nd_osmand_msg_too_many);
        return;
    }

    nd_osm_bbox_around(lat, lon, nd_osmand_size_km[size], &south, &west, &north, &east);
    if (nd_osm_fetch_query(query, sizeof query, south, west, north, east,
                           nd_osmand_size_buildings[size]) != ND_OK ||
        nd_snprintf(query_path, sizeof query_path, "%s/.query.txt", ND_OSMAND_DATA_DIR) != ND_OK ||
        nd_snprintf(xml_path, sizeof xml_path, "%s/.download.osm", ND_OSMAND_DATA_DIR) != ND_OK ||
        nd_path_resolve(real, sizeof real, query_path) != ND_OK) {
        say(ui, nd_osmand_msg_no_card);
        return;
    }
    f = fopen(real, "wb");
    if (f == NULL || fputs(query, f) == EOF || fclose(f) != 0) {
        if (f != NULL)
            (void)fclose(f);
        say(ui, nd_osmand_msg_no_card);
        return;
    }

    /* A 2 km square of a town is a megabyte or two of XML; a 10 km one
     * without buildings is about the same. The estimate only drives the
     * bar. */
    dc.bar = &bar;
    dc.estimate = (int64_t)(1.5 * 1024.0 * 1024.0 * nd_osmand_size_km[size] / 2.0);
    dc.shown = 0;
    nd_progress_init(&bar, ui, "Downloading", nd_osmand_app_title, "Press nothing", download_detail,
                     &dc);
    (void)nd_progress_draw(&bar, 0, 100);

    rc = nd_osm_fetch(query_path, xml_path, download_progress, &dc, why, sizeof why);
    (void)remove(real);
    if (rc != ND_OK) {
        if (rc != ND_ERR_BUSY)
            say(ui, why);
        return;
    }

    nd_progress_set_step(&bar, "Reading the map");
    (void)nd_progress_draw(&bar, 95, 100);
    /* Imported under a dot-name the map list ignores, because the file
     * cannot be named until the data says what town it is. Then renamed to
     * that -- with a " 2" when the town is already on the card, so a second
     * download of the same place sits beside the first rather than over it. */
    rc = nd_snprintf(map_path, sizeof map_path, "%s/.import%s", ND_OSMAND_DATA_DIR,
                     ND_OSMAND_MAP_EXT);
    if (rc == ND_OK)
        rc = nd_osm_import(xml_path, map_path, "", south, west, north, east, &stats, why,
                           sizeof why);
    if (nd_path_resolve(real, sizeof real, xml_path) == ND_OK)
        (void)remove(real);
    if (rc != ND_OK) {
        say(ui, why);
        return;
    }
    {
        char named[ND_PATH_MAX];
        char from[ND_PATH_MAX];

        if (nd_osm_new_map_path(stats.name, named, sizeof named) != ND_OK ||
            nd_path_resolve(from, sizeof from, map_path) != ND_OK ||
            nd_path_resolve(real, sizeof real, named) != ND_OK || rename(from, real) != 0) {
            nd_log_err(LOG_TAG, "cannot name the imported map %s", stats.name);
            (void)remove(from);
            say(ui, nd_osmand_msg_no_card);
            return;
        }
    }

    (void)nd_progress_draw(&bar, 100, 100);
    load_maps(app);
    (void)nd_snprintf(body, sizeof body, "Saved %s.\n\n%u streets, %u places", stats.name,
                      (unsigned)stats.n_roads, (unsigned)stats.n_places);
    say(ui, body);
}

/* ------------------------------------------------------------------ *
 * Installed maps
 * ------------------------------------------------------------------ */

static void installed_maps(app_state *app)
{
    nd_ui *ui = app->ui;
    const char *rows[ND_OSMAND_MAX_MAPS];
    size_t i;
    int32_t choice;
    static const char *const ACTIONS[] = {"Go to map", "Delete"};

    if (app->n_maps == 0u) {
        say(ui, nd_osmand_msg_no_maps);
        return;
    }
    for (i = 0u; i < app->n_maps; i++)
        rows[i] = app->maps[i]->name;

    choice = pick(ui, "Maps", rows, app->n_maps, 0u);
    if (choice < 0 || (size_t)choice >= app->n_maps)
        return;

    switch (pick(ui, app->maps[choice]->name, ACTIONS, ND_ARRAY_LEN(ACTIONS), 0u)) {
    case 0: {
        const nd_osm_map *m = app->maps[choice];

        app->view.cx = (int32_t)(((int64_t)m->bx0 + m->bx1) / 2);
        app->view.cy = (int32_t)(((int64_t)m->by0 + m->by1) / 2);
        app->view.zoom = 14;
        break;
    }
    case 1: {
        char body[96];
        char real[ND_PATH_MAX];

        (void)nd_snprintf(body, sizeof body, "Delete %s from the card?", app->maps[choice]->name);
        if (!confirm(ui, body, "Delete"))
            return;
        if (nd_path_resolve(real, sizeof real, app->maps[choice]->path) == ND_OK)
            (void)remove(real);
        nd_log(LOG_TAG, "deleted %s", app->maps[choice]->path);
        load_maps(app);
        break;
    }
    default:
        break;
    }
}

static void route_mode(app_state *app)
{
    int32_t choice =
        pick(app->ui, "Route mode", nd_osmand_mode_names, ND_OSM_MODE_COUNT, (size_t)app->mode);

    if (choice < 0 || choice == (int32_t)app->mode)
        return;
    app->mode = (nd_osm_mode)choice;
    /* A route planned for a car is not a route for a walk. */
    clear_route(app);
    if (app->dest.set)
        compute_route(app);
}

/* ------------------------------------------------------------------ *
 * Options
 * ------------------------------------------------------------------ */

static void options(app_state *app)
{
    int32_t choice =
        pick(app->ui, nd_osmand_app_title, nd_osmand_options, ND_OSMAND_OPTIONS_COUNT, 0u);

    switch (choice) {
    case ND_OSMAND_OPT_ROUTE_HERE:
        route_here(app);
        break;
    case ND_OSMAND_OPT_START_HERE:
        start_here(app);
        break;
    case ND_OSMAND_OPT_DIRECTIONS:
        directions(app);
        break;
    case ND_OSMAND_OPT_CLEAR_ROUTE:
        clear_route(app);
        app->dest.set = false;
        break;
    case ND_OSMAND_OPT_FIND:
        find(app);
        break;
    case ND_OSMAND_OPT_GOTO:
        go_to(app);
        break;
    case ND_OSMAND_OPT_DOWNLOAD:
        download(app);
        break;
    case ND_OSMAND_OPT_MAPS:
        installed_maps(app);
        break;
    case ND_OSMAND_OPT_MODE:
        route_mode(app);
        break;
    default:
        break; /* Back out of the options list, on to the map again */
    }
}

/* ------------------------------------------------------------------ *
 * The entry points
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    app_state *app;
    int32_t map_w;
    int32_t map_h;
    int rc = 0;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* owned here; freed on every exit from this function */
    app = calloc(1u, sizeof *app);
    if (app == NULL)
        return 1;
    app->ui = ui;

    nd_osm_map_geometry(ui, NULL, &map_w, &map_h);
    app->surface = nd_image_new_filled(map_w, map_h, ND_PIXFMT_RGB888, ND_BLACK);
    app->scratch = nd_osm_scratch_new();
    if (app->surface == NULL || app->scratch == NULL) {
        say(ui, "Not enough memory to open the map.");
        rc = 1;
        goto done;
    }

    load_settings(app);
    load_maps(app);

    for (;;) {
        nd_osm_nav nav = show_map(app);

        if (nav == ND_OSM_NAV_BACK)
            break;
        options(app);

        /* nd_app.h: any loop that outlives a frame polls this. The map does
         * its own polling, so this catches a SIGTERM that arrived while a
         * dialog was up. */
        if (nd_app_should_exit())
            break;
    }
    save_settings(app);

done:
    free_maps(app);
    nd_osm_scratch_free(app->scratch);
    nd_image_free(app->surface);
    free(app);
    return rc;
}

/* Nothing held across a frame: the maps and the surface are freed on the
 * way out of app_run(), and the one child this app ever starts -- curl --
 * is stopped by the download loop itself when nd_app_should_exit() goes
 * true. The symbol exists because nd_app.h requires every app to export
 * one. */
void app_shutdown(void) {}
