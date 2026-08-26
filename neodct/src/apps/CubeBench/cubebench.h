/* cubebench.h -- the pieces of CubeBench a unit test can reach.
 *
 * System/engineering/apps/CubeBench/main.py keeps its rotation, projection
 * and FPS-window arithmetic in four module-level helpers (_rotate_x,
 * _rotate_y, _rotate_z, _project) with the rest inline in run(). Those four
 * are the whole numeric surface of the app and they are the part a test can
 * pin down without rendering anything, so they are declared here rather than
 * left static inside main.c: test/unit/test_cubebench.c dlopen()s the built
 * app.so and dlsym()s them, which means the test exercises the SHIPPED
 * binary and not a second copy compiled with different flags.
 *
 * Names follow CODING-STANDARDS.md section 2 (nd_<module>_<verb>); the
 * Python name each one came from is on the declaration.
 */

#ifndef ND_CUBEBENCH_H_INCLUDED
#define ND_CUBEBENCH_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_CUBEBENCH_N_VERTICES 8
#define ND_CUBEBENCH_N_EDGES    12

/* The constants run() derives before its loop, in one place so a test can
 * check the arithmetic without driving 60 frames. Every number here is
 * main.py's, unchanged:
 *
 *     size      = min(screen_w, content_bottom) * 0.22
 *     fov       = min(screen_w, content_bottom) * 1.1
 *     view_dist = size * 5.5
 *     center    = (screen_w // 2, content_bottom // 2)
 *
 * At 240x145 that is size 31.9, fov 159.5, view_dist 175.45, centre (120,72).
 * They look arbitrary because they are: they were tuned by eye. Do not
 * "simplify" 0.22 and 1.1 into one factor -- fov is derived from the screen,
 * not from the cube, and the two only look related. */
typedef struct {
    int32_t center_x;
    int32_t center_y;
    double size;
    double fov;
    double view_dist;
    double vertices[ND_CUBEBENCH_N_VERTICES][3];
} nd_cubebench_geom;

/* The twelve edges, in main.py's order. Drawing order is visible: later
 * lines overwrite earlier ones where they cross. */
extern const uint8_t nd_cubebench_edges[ND_CUBEBENCH_N_EDGES][2];

void nd_cubebench_geom_init(nd_cubebench_geom *g, int32_t screen_w, int32_t content_bottom);

/* _rotate_x / _rotate_y / _rotate_z. out may alias v. */
void nd_cubebench_rotate_x(const double v[3], double a, double out[3]);
void nd_cubebench_rotate_y(const double v[3], double a, double out[3]);
void nd_cubebench_rotate_z(const double v[3], double a, double out[3]);

/* _project. The perspective divide is clamped at 0.1 so a vertex that has
 * rotated behind the camera scales enormously instead of dividing by zero --
 * the resulting off-screen coordinate is clipped by the rasteriser, which is
 * why the Python never noticed it needed anything better. */
void nd_cubebench_project(const double v[3], int32_t center_x, int32_t center_y, double fov,
                          double view_dist, int32_t *sx, int32_t *sy);

/* The FPS window: frames counted over a wall-clock window that rolls every
 * 0.5 s. `display` is what the label shows, and it stays at its previous
 * value -- 0.0 for the first half second -- until a window closes. */
typedef struct {
    double window_start;
    double display;
    double inst;
    int32_t counter;
} nd_cubebench_fps;

void nd_cubebench_fps_init(nd_cubebench_fps *f, double now);

/* One frame: count it, and close the window if 0.5 s has passed. dt is the
 * frame time the instantaneous reading is taken from. */
void nd_cubebench_fps_tick(nd_cubebench_fps *f, double now, double dt);

/* True for the four codes in main.py's EXIT_KEYS. */
bool nd_cubebench_is_exit_key(int32_t code);

#ifdef __cplusplus
}
#endif

#endif /* ND_CUBEBENCH_H_INCLUDED */
