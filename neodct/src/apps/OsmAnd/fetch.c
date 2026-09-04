/* apps/OsmAnd/fetch.c -- asking the Overpass API for an area.
 *
 * ============ THE TRANSPORT IS /usr/bin/curl, SPAWNED ============
 *
 * For the reason lib/nd_remote.c gives at length: a download happens a few
 * times in a map's life, and curl's memory -- TLS, the CA bundle, the
 * buffers -- should exist only while one is running. This process is the
 * app, and the app is what draws the progress screen while curl works, so
 * the two run side by side and the app polls the file curl is writing to
 * say how far it has got.
 *
 * ============ WHY THE QUERY IS POSTED FROM A FILE ============
 *
 * Overpass QL for a bounding box is a few hundred bytes with quotes, braces
 * and semicolons in it. There is no shell anywhere in this path, so none of
 * that needs escaping -- but --data-urlencode data@FILE keeps the query out
 * of the process list as well, and lets a test read back exactly what was
 * asked.
 *
 * ============ THE TEST SEAM IS PATH ============
 *
 * The same one as nd_remote.c: a stand-in `curl` earlier on PATH that
 * copies a fixture into the -o path exercises the real argv, the real
 * spawn, the real progress polling and the real import. See
 * test/unit/test_osmand_app.c.
 *
 * ============ WHAT THE PHONE ASKS FOR ============
 *
 * Roads, rails, water, woods, the landuse that colours a town, buildings
 * when the area is small enough for them, and the named places. Not
 * shops, not bus stops, not addresses: each would multiply the answer and
 * none can be shown on 240 pixels in a way that helps. The public Overpass
 * instance is a shared, donated service; the query carries a timeout and
 * the app carries a User-Agent that names this project, as its usage
 * policy asks.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osmand_app.h"

#include "nd_app.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"

#define LOG_TAG "OsmAnd"

nd_err nd_osm_fetch_query(char *out, size_t out_sz, int32_t south, int32_t west, int32_t north,
                          int32_t east, nd_osm_detail detail)
{
    /* The bbox is written from the 1e7 integers digit by digit rather than
     * through %f, so the query says exactly what the header of the map
     * will say. */
    char s[24];
    char w[24];
    char n[24];
    char e[24];
    int32_t vals[4] = {south, west, north, east};
    char *strs[4] = {s, w, n, e};
    size_t i;

    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    for (i = 0u; i < 4u; i++) {
        int32_t v = vals[i];
        int32_t whole = v / ND_OSM_DEG;
        int32_t frac = v % ND_OSM_DEG;

        if (frac < 0)
            frac = -frac;
        if (v < 0 && whole == 0)
            (void)nd_snprintf(strs[i], 24u, "-0.%07d", (int)frac);
        else
            (void)nd_snprintf(strs[i], 24u, "%d.%07d", (int)whole, (int)frac);
    }
    if (detail == ND_OSM_DETAIL_MAIN_ROADS) {
        /* A region: the roads a journey uses, the rivers that say where
         * you are, the towns. No lakes, no rails, no landuse -- see
         * nd_osm_detail. The server is given longer, because a box this
         * size is a real question for it. */
        return nd_snprintf(out, out_sz,
                           "[out:xml][timeout:600][bbox:%s,%s,%s,%s];\n"
                           "(\n"
                           "  way[highway~\"^(motorway|motorway_link|trunk|trunk_link|primary|"
                           "primary_link|secondary|secondary_link|tertiary|tertiary_link)$\"];\n"
                           "  way[waterway=river];\n"
                           "  node[place][name];\n"
                           ");\n"
                           "(._;>;);\n"
                           "out;\n",
                           s, w, n, e);
    }
    return nd_snprintf(out, out_sz,
                       "[out:xml][timeout:180][bbox:%s,%s,%s,%s];\n"
                       "(\n"
                       "  way[highway];\n"
                       "  way[railway~\"^(rail|light_rail|narrow_gauge|tram)$\"];\n"
                       "  way[waterway~\"^(river|stream|canal|riverbank|ditch|drain)$\"];\n"
                       "  way[natural~\"^(water|wood)$\"];\n"
                       "  way[landuse~\"^(forest|grass|meadow|village_green|recreation_ground|"
                       "cemetery|residential|industrial|railway|retail|commercial|farmland|"
                       "orchard|vineyard|farmyard|reservoir|basin)$\"];\n"
                       "  way[leisure~\"^(park|garden|pitch|playground|golf_course|"
                       "nature_reserve|dog_park)$\"];\n"
                       "%s"
                       "  node[place][name];\n"
                       ");\n"
                       "(._;>;);\n"
                       "out;\n",
                       s, w, n, e, (detail == ND_OSM_DETAIL_FULL) ? "  way[building];\n" : "");
}

/* execvp's PATH lookup, which nd_proc_spawn() does not do. The same shape
 * as nd_remote.c's, and the same test seam. */
static bool which_curl(char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    const char *seg;

    if (path == NULL || path[0] == '\0')
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (seg = path; seg != NULL;) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        nd_err rc;

        if (len == 0u)
            rc = nd_snprintf(out, out_sz, "./curl");
        else
            rc = nd_snprintf(out, out_sz, "%.*s/curl", (int)len, seg);
        if (rc == ND_OK && access(out, X_OK) == 0)
            return true;
        seg = (colon != NULL) ? colon + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}

static void say_why(char *why, size_t why_sz, const char *text)
{
    if (why != NULL && why_sz > 0u)
        (void)nd_strlcpy(why, text, why_sz);
}

/* curl's stderr, first line, for the log and -- trimmed of its "curl: (22)"
 * prefix -- for the screen. */
static void read_error(const char *err_path, char *why, size_t why_sz)
{
    FILE *f = fopen(err_path, "rb");
    char line[256];

    if (f == NULL)
        return;
    line[0] = '\0';
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] != '\0' && line[0] != '\n')
            break;
    }
    (void)fclose(f);
    if (line[0] == '\0')
        return;
    line[strcspn(line, "\r\n")] = '\0';
    nd_log_err(LOG_TAG, "curl: %s", line);
    if (strstr(line, "429") != NULL || strstr(line, "504") != NULL) {
        say_why(why, why_sz, "The map server is busy. Try again in a minute.");
    } else if (strstr(line, "resolve") != NULL || strstr(line, "connect") != NULL ||
               strstr(line, "Connection") != NULL) {
        say_why(why, why_sz, "No connection. Mobile data has to be working first.");
    } else {
        say_why(why, why_sz, "The map server did not answer. Try again later.");
    }
}

nd_err nd_osm_fetch(const char *query_path, const char *out_path, nd_osm_fetch_progress_fn fn,
                    void *ctx, char *why, size_t why_sz)
{
    char exe[ND_PATH_MAX];
    char query_real[ND_PATH_MAX];
    char out_real[ND_PATH_MAX];
    char part[ND_PATH_MAX + 8];
    char err_path[ND_PATH_MAX + 8];
    char data_arg[ND_PATH_MAX + 8];
    const char *argv[24];
    size_t argn = 0u;
    nd_proc_spec spec;
    pid_t pid = -1;
    int err_fd = -1;
    nd_proc_status status;
    nd_err rc;

    if (query_path == NULL || out_path == NULL)
        return ND_ERR_INVAL;
    if (nd_path_resolve(query_real, sizeof query_real, query_path) != ND_OK ||
        nd_path_resolve(out_real, sizeof out_real, out_path) != ND_OK ||
        nd_snprintf(part, sizeof part, "%s.part", out_real) != ND_OK ||
        nd_snprintf(err_path, sizeof err_path, "%s.err", out_real) != ND_OK ||
        nd_snprintf(data_arg, sizeof data_arg, "data@%s", query_real) != ND_OK) {
        say_why(why, why_sz, "The download path is too long.");
        return ND_ERR_TOOLONG;
    }
    if (!which_curl(exe, sizeof exe)) {
        /* Cannot happen on the phone -- curl is in both defconfigs -- so
         * if it is ever seen, it is a broken image and saying so plainly
         * is the useful answer. */
        say_why(why, why_sz, "This phone has no downloader.");
        return ND_ERR_NOTFOUND;
    }

    (void)remove(part);
    err_fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (err_fd < 0) {
        say_why(why, why_sz, "The card would not take a file.");
        return ND_ERR_IO;
    }

    argv[argn++] = "curl";
    argv[argn++] = "-s"; /* no progress meter; the app draws its own */
    argv[argn++] = "-S"; /* but do say what went wrong               */
    argv[argn++] = "-f"; /* an HTTP error is a failure, not a body   */
    argv[argn++] = "--proto";
    argv[argn++] = "=https";
    argv[argn++] = "--tlsv1.2";
    argv[argn++] = "--connect-timeout";
    argv[argn++] = "20";
    /* Overpass thinks for up to the query's own 180 s before the first
     * byte, then streams. A stall after that is a dead bearer. */
    argv[argn++] = "--speed-limit";
    argv[argn++] = "1";
    argv[argn++] = "--speed-time";
    argv[argn++] = "240";
    argv[argn++] = "--max-time";
    argv[argn++] = "900";
    argv[argn++] = "-H";
    argv[argn++] = "User-Agent: " ND_OSM_USER_AGENT;
    argv[argn++] = "--data-urlencode";
    argv[argn++] = data_arg;
    argv[argn++] = "-o";
    argv[argn++] = part;
    argv[argn++] = ND_OSM_OVERPASS_URL;
    argv[argn++] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    /* Stays in the app's process group, as nd_remote.c's curl does: a
     * download that outlived the screen showing it would hold a bearer
     * nobody is watching. */
    spec.fds[0].child_fd = 2;
    spec.fds[0].our_fd = err_fd;
    spec.n_fds = 1u;
    spec.close_others = true;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        (void)close(err_fd);
        say_why(why, why_sz, "The downloader could not be started.");
        return ND_ERR_IO;
    }
    (void)close(err_fd);
    err_fd = -1;

    /* Poll rather than block: the progress screen wants the byte count and
     * the teardown contract in nd_app.h wants SIGTERM noticed. An incoming
     * call mid-download stops the download, which is the right trade -- the
     * caller is paying for the bearer. */
    for (;;) {
        struct stat st;

        rc = nd_proc_wait(pid, 0.25, &status);
        if (rc != ND_ERR_TIMEOUT)
            break;
        if (nd_app_should_exit()) {
            (void)nd_proc_terminate(pid, 1.0, &status);
            (void)remove(part);
            say_why(why, why_sz, "The download was interrupted.");
            return ND_ERR_BUSY;
        }
        if (fn != NULL && stat(part, &st) == 0)
            fn(ctx, (int64_t)st.st_size);
    }

    if (rc != ND_OK || !status.exited || status.exit_status != 0) {
        say_why(why, why_sz, "The map server did not answer. Try again later.");
        read_error(err_path, why, why_sz);
        (void)remove(part);
        (void)remove(err_path);
        return ND_ERR_IO;
    }
    (void)remove(err_path);
    if (rename(part, out_real) != 0) {
        nd_log_err(LOG_TAG, "cannot rename %s: %s", part, strerror(errno));
        (void)remove(part);
        say_why(why, why_sz, "The card would not take the download.");
        return ND_ERR_IO;
    }
    return ND_OK;
}
