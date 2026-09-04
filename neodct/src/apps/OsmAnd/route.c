/* apps/OsmAnd/route.c -- the routing graph, Dijkstra over it, and the
 * turn-by-turn list a route folds into.
 *
 * ============ WHAT THE GRAPH IS ============
 *
 * Every node of every road way is a vertex and every consecutive pair of
 * refs in a road way is an edge, stored in BOTH directions with a flag on
 * the one that runs against the way. The flag rather than a missing edge is
 * what lets one graph serve both modes: a car honours the flag on a one-way
 * street and a pedestrian ignores it, and neither needs the graph rebuilt.
 *
 * The graph is built the first time a route is asked for, not at load,
 * because most launches of a map app are a look and not a journey, and
 * building it is a pass over every road plus two allocations the size of
 * the road network.
 *
 * ============ WHY DIJKSTRA AND NOT A* ============
 *
 * The maps are small -- a town, capped by mapdata.c -- and the phone is
 * slow at exactly the thing A* saves (the search) and not at the thing A*
 * costs (a heuristic per pop that needs a square root). Plain Dijkstra with
 * a binary heap over a 100,000-node town finishes in well under a second on
 * the Cortex-A7 and is one less thing to get subtly wrong.
 *
 * ============ NO GPS ============
 *
 * There is no "current position" anywhere in this file. A route runs from a
 * node to a node, and the app decides which nodes from its marks. When a
 * receiver arrives it will supply a mark; this file will not change.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "osmand_app.h"

#include "nd_types.h"

#define PI_D 3.14159265358979323846

/* The high bit of an edge's way index marks an edge that runs against the
 * way's stored direction. Way indices are far below 2^31. */
#define EDGE_REVERSE 0x80000000u
#define EDGE_WAY(w)  ((w) & ~EDGE_REVERSE)

const char *const nd_osmand_mode_names[ND_OSM_MODE_COUNT] = {"By car", "On foot"};

const char *const nd_osmand_turn_names[ND_OSM_TURN_ARRIVE + 1] = {
    "Start", "Continue", "Turn left", "Turn right", "Sharp left", "Sharp right", "Arrive"};

/* ------------------------------------------------------------------ *
 * Policy
 * ------------------------------------------------------------------ */

double nd_osm_speed(nd_osm_mode mode, uint8_t kind, uint8_t flags)
{
    if (kind < ND_OSM_KIND_FIRST_ROAD || kind >= ND_OSM_KIND_COUNT)
        return 0.0;
    if (mode == ND_OSM_MODE_FOOT) {
        if ((flags & ND_OSM_FLAG_NO_FOOT) != 0u || kind == ND_OSM_KIND_MOTORWAY)
            return 0.0;
        /* Everything is a walk at 5 km/h. A motorway is the one road a
         * person may not be on; a trunk road usually has a verge. */
        return 5.0 / 3.6;
    }
    if ((flags & ND_OSM_FLAG_NO_CAR) != 0u)
        return 0.0;
    switch (kind) {
    case ND_OSM_KIND_MOTORWAY:
        return 100.0 / 3.6;
    case ND_OSM_KIND_TRUNK:
        return 80.0 / 3.6;
    case ND_OSM_KIND_PRIMARY:
        return 60.0 / 3.6;
    case ND_OSM_KIND_SECONDARY:
        return 50.0 / 3.6;
    case ND_OSM_KIND_TERTIARY:
        return 40.0 / 3.6;
    case ND_OSM_KIND_RESIDENTIAL:
        return 30.0 / 3.6;
    case ND_OSM_KIND_SERVICE:
        return 15.0 / 3.6;
    case ND_OSM_KIND_TRACK:
        return 10.0 / 3.6;
    default:
        return 0.0; /* a path is not for a car */
    }
}

/* ------------------------------------------------------------------ *
 * Building the graph
 * ------------------------------------------------------------------ */

nd_err nd_osm_graph_build(nd_osm_map *m)
{
    uint32_t *deg = NULL;
    uint32_t i;
    uint32_t n_edges = 0u;
    uint32_t road_begin;
    uint32_t road_end;

    if (m == NULL)
        return ND_ERR_INVAL;
    if (m->adj_start != NULL)
        return ND_OK;

    road_begin = m->kind_begin[ND_OSM_KIND_FIRST_ROAD];
    road_end = m->kind_begin[ND_OSM_KIND_COUNT];

    /* owned by the map; released by nd_osm_map_free() */
    m->adj_start = calloc((size_t)m->n_nodes + 1u, sizeof *m->adj_start);
    deg = calloc((size_t)m->n_nodes + 1u, sizeof *deg);
    if (m->adj_start == NULL || deg == NULL)
        goto nomem;

    for (i = road_begin; i < road_end; i++) {
        const nd_osm_way *w = &m->ways[i];
        uint32_t j;

        for (j = 0u; j + 1u < w->n_refs; j++) {
            deg[m->refs[w->first_ref + j]]++;
            deg[m->refs[w->first_ref + j + 1u]]++;
            n_edges += 2u;
        }
    }
    for (i = 0u; i < m->n_nodes; i++)
        m->adj_start[i + 1u] = m->adj_start[i] + deg[i];
    m->n_adj = n_edges;

    /* Eight bytes per directed edge: a town's 200,000 of them are 1.6 MB. */
    m->adj = malloc(((size_t)n_edges > 0u ? (size_t)n_edges : 1u) * sizeof *m->adj);
    if (m->adj == NULL)
        goto nomem;
    memset(deg, 0, ((size_t)m->n_nodes + 1u) * sizeof *deg);

    for (i = road_begin; i < road_end; i++) {
        const nd_osm_way *w = &m->ways[i];
        uint32_t j;

        for (j = 0u; j + 1u < w->n_refs; j++) {
            uint32_t a = m->refs[w->first_ref + j];
            uint32_t b = m->refs[w->first_ref + j + 1u];
            nd_osm_edge *ea = &m->adj[m->adj_start[a] + deg[a]++];
            nd_osm_edge *eb = &m->adj[m->adj_start[b] + deg[b]++];

            ea->to = b;
            ea->way = i;
            eb->to = a;
            eb->way = i | EDGE_REVERSE;
        }
    }
    free(deg);
    return ND_OK;

nomem:
    free(deg);
    free(m->adj_start);
    free(m->adj);
    m->adj_start = NULL;
    m->adj = NULL;
    m->n_adj = 0u;
    return ND_ERR_NOMEM;
}

/* The cost of an edge in seconds, or a negative number when it may not be
 * used in this mode. */
static double edge_cost(const nd_osm_map *m, uint32_t from, const nd_osm_edge *e, nd_osm_mode mode,
                        double *metres_out)
{
    const nd_osm_way *w = &m->ways[EDGE_WAY(e->way)];
    double speed = nd_osm_speed(mode, w->kind, w->flags);
    double dx;
    double dy;
    double metres;

    if (speed <= 0.0)
        return -1.0;
    if (mode == ND_OSM_MODE_CAR && (w->flags & ND_OSM_FLAG_ONEWAY) != 0u &&
        (e->way & EDGE_REVERSE) != 0u)
        return -1.0;
    dx = (double)m->mx[e->to] - (double)m->mx[from];
    dy = (double)m->my[e->to] - (double)m->my[from];
    metres = sqrt(dx * dx + dy * dy) * m->metres_per_unit;
    if (metres_out != NULL)
        *metres_out = metres;
    return metres / speed;
}

static bool node_usable(const nd_osm_map *m, uint32_t node, nd_osm_mode mode)
{
    uint32_t e;

    for (e = m->adj_start[node]; e < m->adj_start[node + 1u]; e++) {
        if (edge_cost(m, node, &m->adj[e], mode, NULL) >= 0.0)
            return true;
    }
    return false;
}

uint32_t nd_osm_nearest_node(const nd_osm_map *m, int32_t mx, int32_t my, nd_osm_mode mode,
                             double *dist_out)
{
    uint32_t best = UINT32_MAX;
    double best_d2 = 0.0;
    uint32_t i;

    if (m == NULL || m->adj_start == NULL)
        return UINT32_MAX;
    for (i = 0u; i < m->n_nodes; i++) {
        double dx;
        double dy;
        double d2;

        if (m->adj_start[i] == m->adj_start[i + 1u])
            continue;
        dx = (double)m->mx[i] - (double)mx;
        dy = (double)m->my[i] - (double)my;
        d2 = dx * dx + dy * dy;
        if (best != UINT32_MAX && d2 >= best_d2)
            continue;
        if (!node_usable(m, i, mode))
            continue;
        best = i;
        best_d2 = d2;
    }
    if (dist_out != NULL)
        *dist_out = (best == UINT32_MAX) ? 0.0 : sqrt(best_d2) * m->metres_per_unit;
    return best;
}

/* ------------------------------------------------------------------ *
 * Dijkstra
 * ------------------------------------------------------------------ */

typedef struct {
    double cost;
    uint32_t node;
} heap_item;

typedef struct {
    heap_item *items;
    size_t n;
    size_t cap;
} heap;

static bool heap_push(heap *h, double cost, uint32_t node)
{
    size_t i;

    if (h->n == h->cap) {
        size_t want = (h->cap == 0u) ? 1024u : h->cap * 2u;
        heap_item *grown = realloc(h->items, want * sizeof *grown);

        if (grown == NULL)
            return false;
        h->items = grown;
        h->cap = want;
    }
    i = h->n++;
    while (i > 0u) {
        size_t parent = (i - 1u) / 2u;

        if (h->items[parent].cost <= cost)
            break;
        h->items[i] = h->items[parent];
        i = parent;
    }
    h->items[i].cost = cost;
    h->items[i].node = node;
    return true;
}

static bool heap_pop(heap *h, heap_item *out)
{
    heap_item last;
    size_t i = 0u;

    if (h->n == 0u)
        return false;
    *out = h->items[0];
    last = h->items[--h->n];
    while (2u * i + 1u < h->n) {
        size_t child = 2u * i + 1u;

        if (child + 1u < h->n && h->items[child + 1u].cost < h->items[child].cost)
            child++;
        if (h->items[child].cost >= last.cost)
            break;
        h->items[i] = h->items[child];
        i = child;
    }
    if (h->n > 0u)
        h->items[i] = last;
    return true;
}

nd_err nd_osm_route_find(nd_osm_map *m, uint32_t from, uint32_t to, nd_osm_mode mode,
                         nd_osm_route *out)
{
    double *dist = NULL;
    double *metres = NULL;
    uint32_t *prev = NULL;
    heap h = {NULL, 0u, 0u};
    nd_err rc = ND_ERR_NOTFOUND;
    uint32_t i;
    heap_item top;
    bool reached = false;

    if (m == NULL || out == NULL)
        return ND_ERR_INVAL;
    memset(out, 0, sizeof *out);
    rc = nd_osm_graph_build(m);
    if (rc != ND_OK)
        return rc;
    if (from >= m->n_nodes || to >= m->n_nodes)
        return ND_ERR_INVAL;

    /* 8 + 8 + 4 bytes per node: 8 MB for the largest map the file format
     * allows, 2 MB for a town. Freed before this returns. */
    dist = malloc((size_t)m->n_nodes * sizeof *dist);
    metres = malloc((size_t)m->n_nodes * sizeof *metres);
    prev = malloc((size_t)m->n_nodes * sizeof *prev);
    if (dist == NULL || metres == NULL || prev == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }
    for (i = 0u; i < m->n_nodes; i++) {
        dist[i] = -1.0;
        metres[i] = 0.0;
        prev[i] = UINT32_MAX;
    }
    dist[from] = 0.0;
    if (!heap_push(&h, 0.0, from)) {
        rc = ND_ERR_NOMEM;
        goto done;
    }

    while (heap_pop(&h, &top)) {
        uint32_t e;

        if (top.cost > dist[top.node])
            continue; /* a stale entry; the node was settled cheaper */
        if (top.node == to) {
            reached = true;
            break;
        }
        for (e = m->adj_start[top.node]; e < m->adj_start[top.node + 1u]; e++) {
            const nd_osm_edge *edge = &m->adj[e];
            double len = 0.0;
            double cost = edge_cost(m, top.node, edge, mode, &len);
            double total;

            if (cost < 0.0)
                continue;
            total = top.cost + cost;
            if (dist[edge->to] >= 0.0 && dist[edge->to] <= total)
                continue;
            dist[edge->to] = total;
            metres[edge->to] = metres[top.node] + len;
            prev[edge->to] = top.node;
            if (!heap_push(&h, total, edge->to)) {
                rc = ND_ERR_NOMEM;
                goto done;
            }
        }
    }

    if (!reached) {
        rc = ND_ERR_NOTFOUND;
        goto done;
    }

    /* Walk back to count, allocate exactly, walk back again to fill. */
    {
        uint32_t n = 0u;
        uint32_t at = to;

        while (at != UINT32_MAX) {
            n++;
            at = prev[at];
        }
        /* owned by the caller; released by nd_osm_route_free() */
        out->nodes = malloc((size_t)n * sizeof *out->nodes);
        if (out->nodes == NULL) {
            rc = ND_ERR_NOMEM;
            goto done;
        }
        out->n_nodes = n;
        at = to;
        while (n > 0u) {
            out->nodes[--n] = at;
            at = prev[at];
        }
        out->metres = metres[to];
        out->seconds = dist[to];
        rc = ND_OK;
    }

done:
    free(dist);
    free(metres);
    free(prev);
    free(h.items);
    return rc;
}

void nd_osm_route_free(nd_osm_route *r)
{
    if (r == NULL)
        return;
    free(r->nodes);
    r->nodes = NULL;
    r->n_nodes = 0u;
    r->metres = 0.0;
    r->seconds = 0.0;
}

/* ------------------------------------------------------------------ *
 * Directions
 * ------------------------------------------------------------------ */

nd_osm_turn nd_osm_turn_for(double delta_deg)
{
    double a = fabs(delta_deg);

    if (a < 25.0)
        return ND_OSM_TURN_STRAIGHT;
    if (a <= 110.0)
        return (delta_deg > 0.0) ? ND_OSM_TURN_RIGHT : ND_OSM_TURN_LEFT;
    return (delta_deg > 0.0) ? ND_OSM_TURN_SHARP_RIGHT : ND_OSM_TURN_SHARP_LEFT;
}

/* Compass bearing in degrees from a to b: 0 north, 90 east. Screen y grows
 * downward, hence the sign on dy. */
static double bearing(const nd_osm_map *m, uint32_t a, uint32_t b)
{
    double dx = (double)m->mx[b] - (double)m->mx[a];
    double dy = (double)m->my[b] - (double)m->my[a];

    return atan2(dx, -dy) * 180.0 / PI_D;
}

/* The way carrying the edge a -> b, or NULL. Any road way joining the two
 * will do; a route never uses an edge that is not in the graph. */
static const nd_osm_way *edge_way(const nd_osm_map *m, uint32_t a, uint32_t b)
{
    uint32_t e;

    if (m->adj_start == NULL)
        return NULL;
    for (e = m->adj_start[a]; e < m->adj_start[a + 1u]; e++) {
        if (m->adj[e].to == b)
            return &m->ways[EDGE_WAY(m->adj[e].way)];
    }
    return NULL;
}

/* What an unnamed road is called in a direction. */
static const char *kind_word(uint8_t kind)
{
    /* Capitalised, because it stands where a road name stands: as the
     * page's own title line. */
    switch (kind) {
    case ND_OSM_KIND_PATH:
        return "Path";
    case ND_OSM_KIND_TRACK:
        return "Track";
    case ND_OSM_KIND_SERVICE:
        return "Service road";
    case ND_OSM_KIND_MOTORWAY:
        return "Motorway";
    default:
        return "Road";
    }
}

static void road_label(const nd_osm_map *m, const nd_osm_way *w, char *out, size_t out_sz)
{
    const char *name = (w != NULL) ? nd_osm_way_name(m, w) : "";
    size_t i;

    if (name[0] == '\0') {
        (void)nd_strlcpy(out, kind_word((w != NULL) ? w->kind : ND_OSM_KIND_NONE), out_sz);
        return;
    }
    (void)nd_strlcpy(out, name, out_sz);
    /* A road carrying two route numbers is tagged "US 36;SR 83". The
     * semicolon is OSM's list separator, not a spelling; a sign says it
     * with a slash. */
    for (i = 0u; out[i] != '\0'; i++) {
        if (out[i] == ';')
            out[i] = '/';
    }
}

size_t nd_osm_route_steps(const nd_osm_map *m, const nd_osm_route *r, nd_osm_step *out, size_t max)
{
    size_t n = 0u;
    uint32_t i;
    char cur_road[48];
    double cur_metres = 0.0;
    double last_bearing = 0.0;
    uint32_t cur_start;

    if (m == NULL || r == NULL || out == NULL || max < 2u || r->n_nodes < 2u ||
        m->adj_start == NULL)
        return 0u;

    road_label(m, edge_way(m, r->nodes[0], r->nodes[1]), cur_road, sizeof cur_road);
    cur_start = r->nodes[0];
    out[0].turn = ND_OSM_TURN_START;

    for (i = 0u; i + 1u < r->n_nodes; i++) {
        uint32_t a = r->nodes[i];
        uint32_t b = r->nodes[i + 1u];
        const nd_osm_way *w = edge_way(m, a, b);
        char road[48];
        double dx = (double)m->mx[b] - (double)m->mx[a];
        double dy = (double)m->my[b] - (double)m->my[a];
        double len = sqrt(dx * dx + dy * dy) * m->metres_per_unit;
        double br = bearing(m, a, b);

        road_label(m, w, road, sizeof road);
        if (strcmp(road, cur_road) != 0) {
            double delta;

            /* Close the step that just ended. When the list is full the
             * rest of the journey folds into the last written step, so the
             * distances still add up to the route's. */
            if (n + 1u < max) {
                out[n].node = cur_start;
                out[n].metres = nd_trunc32(cur_metres + 0.5);
                (void)nd_strlcpy(out[n].road, cur_road, sizeof out[n].road);
                n++;
                delta = br - last_bearing;
                while (delta > 180.0)
                    delta -= 360.0;
                while (delta <= -180.0)
                    delta += 360.0;
                out[n].turn = nd_osm_turn_for(delta);
                cur_metres = 0.0;
                cur_start = a;
            }
            (void)nd_strlcpy(cur_road, road, sizeof cur_road);
        }
        cur_metres += len;
        last_bearing = br;
    }

    out[n].node = cur_start;
    out[n].metres = nd_trunc32(cur_metres + 0.5);
    (void)nd_strlcpy(out[n].road, cur_road, sizeof out[n].road);
    n++;

    out[n].turn = ND_OSM_TURN_ARRIVE;
    out[n].node = r->nodes[r->n_nodes - 1u];
    out[n].metres = 0;
    out[n].road[0] = '\0';
    n++;
    return n;
}

void nd_osm_format_distance(double metres, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    if (metres < 0.0)
        metres = 0.0;
    if (metres < 1000.0) {
        /* To the nearest ten metres above a hundred: "437 m" is precision
         * a map cut to a pixel does not have. */
        int32_t v = nd_trunc32(metres + 0.5);

        if (v >= 100)
            v = (v + 5) / 10 * 10;
        (void)nd_snprintf(out, out_sz, "%d m", (int)v);
    } else if (metres < 10000.0) {
        (void)nd_snprintf(out, out_sz, "%.1f km", metres / 1000.0);
    } else {
        (void)nd_snprintf(out, out_sz, "%.0f km", metres / 1000.0);
    }
}

void nd_osm_step_label(const nd_osm_step *s, char *out, size_t out_sz)
{
    char dist[16];

    if (s == NULL || out == NULL || out_sz == 0u)
        return;
    /* The turn in a word or two, then the distance: short enough to fit the
     * one 20 px line the PagedList gives a value, which is 223 px. The
     * road is the page's ITEM, which the widget wraps over two lines at
     * 24 px -- so "Scio Bowerstown Road" is read whole rather than as
     * "Scio Bow..." with the distance lost off the end, which is what
     * putting the road on this line did.
     *
     * Measured against that width in the phone's font: "Turn right, 28 km"
     * is 230 px and "Sharp right, 1.2 km" 244, so the words here are the
     * short ones and the comma is gone. "Hard right 1.2 km" is 223 exactly;
     * only a hard turn followed by a hundred kilometres of the same road
     * would still be shortened, and no road does that. */
    static const char *const WORDS[ND_OSM_TURN_ARRIVE + 1] = {
        "Start", "Continue", "Left", "Right", "Hard left", "Hard right", "Arrive"};

    if (s->turn == ND_OSM_TURN_ARRIVE) {
        (void)nd_strlcpy(out, "End of route", out_sz);
        return;
    }
    nd_osm_format_distance((double)s->metres, dist, sizeof dist);
    (void)nd_snprintf(out, out_sz, "%s %s",
                      WORDS[nd_clamp32((int32_t)s->turn, 0, ND_OSM_TURN_ARRIVE)], dist);
}
