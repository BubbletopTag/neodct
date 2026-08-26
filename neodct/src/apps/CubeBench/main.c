/* apps/CubeBench/main.c -- a spinning wireframe cube and an FPS counter.
 *
 * A one-to-one port of System/engineering/apps/CubeBench/main.py. App id
 * 9998, engineering menu. SESSION-SCOPE.md keeps this one out of the stub
 * set: the owner asked for it as a visible demonstration of the speed
 * difference, so it is real.
 *
 * ============ WHAT THIS APP ACTUALLY SPENDS ITS TIME ON ============
 *
 * docs/c-rewrite/PERFORMANCE.md measured the Python frame before any C was
 * written, and the answer is not the cube:
 *
 *     36 trig calls + 8 projections   0.0061 ms    3%
 *     clear the content rectangle     0.0189 ms    8%
 *     12 wireframe lines              0.0086 ms    4%
 *     the "FPS 60.0" label            0.1821 ms   75%
 *     tobytes() for the blit          0.0261 ms   11%
 *
 * Three quarters of the frame is eight characters of text, because Pillow
 * re-renders every glyph through FreeType on every call and caches nothing.
 * lib/nd_font.c does not: it rasterises all 95 printable ASCII characters
 * into one arena when the face is loaded, so nd_draw_text() is a blend per
 * character and the 75% disappears. That cache is the reason this port is
 * fast, not the native trig -- see the numbers in the work-package report.
 *
 * ============ THE LOOP NEVER BLOCKS ============
 *
 * read_keypress(0) polls with a zero timeout so the animation keeps running
 * while nothing is pressed. That means this app spins a core flat out, which
 * is the point of a benchmark and would be a bug in anything else.
 *
 * ============ HOW IT STOPS ============
 *
 * Three ways, and the first two are the Python's:
 *
 *   1. One of EXIT_KEYS.
 *   2. The frame refuses to commit. Under nd_capture that is the frame
 *      budget, which is uistub's ScriptExhausted -- the exception that
 *      unwinds this loop when shoot_docs.py captures eng-cubebench. The
 *      Python needs no explicit check because fb.update() raises; C has to
 *      look at the return value.
 *   3. nd_app_should_exit(), i.e. SIGTERM from the core's modem thread when
 *      a call arrives. nd_app.h requires any loop longer than a frame to
 *      poll it. The Python had no equivalent -- IncomingCall was raised from
 *      inside read_keypress -- and OPEN-QUESTIONS.md question 1 records the
 *      replacement as a deliberate, approved deviation.
 */

#include <math.h>
#include <stdio.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "cubebench.h"

/* main.py: EXIT_KEYS = {14, 28, 46, 50}. Spelled with the names from
 * nd_keycodes.h; the numbers are the ones the Python set holds. */
static const int32_t EXIT_KEYS[] = {ND_KEY_BACK, ND_KEY_ENTER, ND_KEY_C, ND_KEY_MENU};

/* main.py's edges list, in order. */
const uint8_t nd_cubebench_edges[ND_CUBEBENCH_N_EDGES][2] = {
    {0u, 1u}, {1u, 2u}, {2u, 3u}, {3u, 0u}, {4u, 5u}, {5u, 6u},
    {6u, 7u}, {7u, 4u}, {0u, 4u}, {1u, 5u}, {2u, 6u}, {3u, 7u},
};

bool nd_cubebench_is_exit_key(int32_t code)
{
    size_t i;

    for (i = 0u; i < sizeof EXIT_KEYS / sizeof EXIT_KEYS[0]; i++) {
        if (EXIT_KEYS[i] == code)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

void nd_cubebench_geom_init(nd_cubebench_geom *g, int32_t screen_w, int32_t content_bottom)
{
    /* min(screen_w, content_bottom) is 145 on this panel, and it is the
     * SHORT side deliberately: a cube sized off the long side would clip
     * top and bottom as it turns. */
    int32_t shortest = screen_w < content_bottom ? screen_w : content_bottom;
    double s;
    int32_t i;

    if (g == NULL)
        return;

    g->center_x = screen_w / 2;       /* screen_w // 2       */
    g->center_y = content_bottom / 2; /* content_bottom // 2 */
    g->size = (double)shortest * 0.22;
    g->fov = (double)shortest * 1.1;
    g->view_dist = g->size * 5.5;

    /* The eight corners, in main.py's order. Sign pattern per vertex index:
     * bit 0 is +x on 1,2,5,6; the y and z patterns are the list's own. */
    s = g->size;
    {
        const double corner[ND_CUBEBENCH_N_VERTICES][3] = {
            {-1.0, -1.0, -1.0}, {1.0, -1.0, -1.0}, {1.0, 1.0, -1.0}, {-1.0, 1.0, -1.0},
            {-1.0, -1.0, 1.0},  {1.0, -1.0, 1.0},  {1.0, 1.0, 1.0},  {-1.0, 1.0, 1.0},
        };

        for (i = 0; i < ND_CUBEBENCH_N_VERTICES; i++) {
            /* The Python writes the literal `size` or `-size`, never
             * `1.0 * size`, so multiplying by +/-1.0 has to give the same
             * bits -- it does: IEEE multiplication by one is exact and
             * negation only flips the sign bit. */
            g->vertices[i][0] = corner[i][0] * s;
            g->vertices[i][1] = corner[i][1] * s;
            g->vertices[i][2] = corner[i][2] * s;
        }
    }
}

void nd_cubebench_rotate_x(const double v[3], double a, double out[3])
{
    double x = v[0];
    double y = v[1];
    double z = v[2];
    double ca = cos(a);
    double sa = sin(a);

    out[0] = x;
    out[1] = y * ca - z * sa;
    out[2] = y * sa + z * ca;
}

void nd_cubebench_rotate_y(const double v[3], double a, double out[3])
{
    double x = v[0];
    double y = v[1];
    double z = v[2];
    double ca = cos(a);
    double sa = sin(a);

    out[0] = x * ca + z * sa;
    out[1] = y;
    out[2] = -x * sa + z * ca;
}

void nd_cubebench_rotate_z(const double v[3], double a, double out[3])
{
    double x = v[0];
    double y = v[1];
    double z = v[2];
    double ca = cos(a);
    double sa = sin(a);

    out[0] = x * ca - y * sa;
    out[1] = x * sa + y * ca;
    out[2] = z;
}

void nd_cubebench_project(const double v[3], int32_t center_x, int32_t center_y, double fov,
                          double view_dist, int32_t *sx, int32_t *sy)
{
    double denom = v[2] + view_dist;
    double scale;

    if (denom < 0.1)
        denom = 0.1;
    scale = fov / denom;

    /* int() in Python truncates toward zero, and so does nd_trunc32(). A
     * lround() here would move vertices by a pixel on the negative side. */
    if (sx != NULL)
        *sx = nd_trunc32((double)center_x + v[0] * scale);
    if (sy != NULL)
        *sy = nd_trunc32((double)center_y + v[1] * scale);
}

/* ------------------------------------------------------------------ *
 * The FPS window
 * ------------------------------------------------------------------ */

void nd_cubebench_fps_init(nd_cubebench_fps *f, double now)
{
    if (f == NULL)
        return;
    f->window_start = now;
    f->display = 0.0;
    f->inst = 0.0;
    f->counter = 0;
}

void nd_cubebench_fps_tick(nd_cubebench_fps *f, double now, double dt)
{
    double elapsed;

    if (f == NULL)
        return;

    f->inst = 1.0 / dt;
    f->counter++;
    elapsed = now - f->window_start;

    /* main.py: "Update displayed FPS every 0.5s using measured frames/time
     * window." The window start is set to `now`, not advanced by 0.5, so a
     * long frame shortens the next window rather than accumulating debt. */
    if (elapsed >= 0.5) {
        f->display = (double)f->counter / elapsed;
        f->counter = 0;
        f->window_start = now;
    }
}

/* ------------------------------------------------------------------ *
 * The app
 * ------------------------------------------------------------------ */

/* Python's `//` on a positive divisor floors; C's `/` truncates. They differ
 * only for a negative numerator, which (screen_w - hw) becomes as soon as a
 * hint is wider than the screen. Cheap to get right, invisible until it is
 * not. */
static int32_t floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

int app_run(nd_ui *ui)
{
    nd_cubebench_geom geom;
    nd_cubebench_fps fps;
    nd_softkey softkey;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t proj[ND_CUBEBENCH_N_VERTICES][2];
    double ax = 0.0;
    double ay = 0.0;
    double az = 0.0;
    double last_time;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    nd_cubebench_geom_init(&geom, screen_w, content_bottom);

    /* SoftKeyBar(ui) from inside an app is always the opaque one: by the
     * time any app runs, ui.softkey exists and framework.py's hasattr check
     * has already decided. See the nd_ui.h header comment. */
    nd_softkey_init(&softkey, ui, false);

    last_time = nd_time_monotonic();
    nd_cubebench_fps_init(&fps, last_time);

    for (;;) {
        double now = nd_time_monotonic();
        double dt = now - last_time;
        int32_t key;
        int32_t i;
        char fps_text[32];
        int32_t tw;
        int32_t th;
        int n;

        last_time = now;
        if (dt <= 0.0)
            dt = 0.001;

        /* Zero timeout: check for a keypress without holding up the frame. */
        key = nd_ui_read_keypress(ui, 0.0);
        if (nd_cubebench_is_exit_key(key))
            return 0;
        if (nd_app_should_exit())
            return 0;

        /* main.py: "tuned for visible but smooth motion". Three different
         * rates so the cube never repeats an orientation. */
        ax += 1.30 * dt;
        ay += 1.10 * dt;
        az += 0.85 * dt;

        /* Clears rows 0..content_bottom inclusive, so the softkey strip
         * below keeps whatever was there. */
        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

        for (i = 0; i < ND_CUBEBENCH_N_VERTICES; i++) {
            double r[3];

            nd_cubebench_rotate_x(geom.vertices[i], ax, r);
            nd_cubebench_rotate_y(r, ay, r);
            nd_cubebench_rotate_z(r, az, r);
            nd_cubebench_project(r, geom.center_x, geom.center_y, geom.fov, geom.view_dist,
                                 &proj[i][0], &proj[i][1]);
        }

        for (i = 0; i < ND_CUBEBENCH_N_EDGES; i++) {
            int32_t a = nd_cubebench_edges[i][0];
            int32_t b = nd_cubebench_edges[i][1];

            (void)nd_draw_line(ui->draw, proj[a][0], proj[a][1], proj[b][0], proj[b][1], ND_WHITE,
                               1);
        }

        nd_cubebench_fps_tick(&fps, now, dt);

        /* "FPS %.1f". Both Python's float formatting and C's printf round
         * the decimal correctly, so the same double prints the same string.
         * The buffer cannot truncate -- an inf would be "FPS inf" -- but the
         * return is checked because CODING-STANDARDS.md 1.4 says to. */
        n = snprintf(fps_text, sizeof fps_text, "FPS %.1f", fps.display);
        if (n < 0 || (size_t)n >= sizeof fps_text)
            (void)nd_strlcpy(fps_text, "FPS ?", sizeof fps_text);

        (void)nd_draw_text(ui->draw, 6, 4, "3D Cube", ui->font_s, ND_WHITE);

        nd_ui_text_size(ui, fps_text, ui->font_s, &tw, &th);
        (void)nd_draw_text(ui->draw, screen_w - tw - 6, 16, fps_text, ui->font_s, ND_WHITE);

        nd_ui_text_size(ui, "BACK/OK to exit", ui->font_s, &tw, &th);
        (void)nd_draw_text(ui->draw, floordiv(screen_w - tw, 2), content_bottom - th - 4,
                           "BACK/OK to exit", ui->font_s, ND_GRAY);

        /* present=False: the softkey bar is painted into the same frame the
         * cube is in, and the one commit below puts both on the panel. */
        nd_softkey_update(&softkey, "Exit", false);

        if (nd_ui_present(ui) != ND_OK)
            return 0;
    }
}

/* Nothing is held: no sound card, no child process, no file. Exported
 * anyway, because nd_app.h requires it so that a missing symbol always means
 * the author forgot. */
void app_shutdown(void) {}
