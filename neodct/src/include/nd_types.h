/* nd_types.h -- the vocabulary every other NeoDCT header is written in.
 *
 * Nothing here allocates and nothing here does I/O. It exists so that ten
 * modules written by ten people describe a rectangle, a colour and a failure
 * the same way.
 *
 * Two conventions are fixed here and are not negotiable anywhere else in the
 * project:
 *
 *   1. A function that can fail returns nd_err. A function that returns a
 *      pointer returns NULL on failure. Never both -- see CODING-STANDARDS.md
 *      section 3.
 *   2. nd_rect is INCLUSIVE of both corners, because Pillow's
 *      ImageDraw.rectangle() is, and every coordinate in every spec was
 *      measured against Pillow. (2,3,6,8) covers six columns and six rows.
 *      A half-open rectangle here would move a pixel on almost every screen.
 */

#ifndef ND_TYPES_H_INCLUDED
#define ND_TYPES_H_INCLUDED

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Compiler helpers
 * ------------------------------------------------------------------ */

#if defined(__GNUC__)
#define ND_PRINTF(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#define ND_UNUSED_FN                  __attribute__((unused))
#define ND_WARN_UNUSED                __attribute__((warn_unused_result))
#else
#define ND_PRINTF(fmt_idx, first_arg)
#define ND_UNUSED_FN
#define ND_WARN_UNUSED
#endif

/* Silences -Wunused-parameter for a parameter a stub genuinely does not use. */
#define ND_UNUSED(x) ((void)(x))

#define ND_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ *
 * Errors
 * ------------------------------------------------------------------ */

/* The first seven values are exactly those in CODING-STANDARDS.md section 3
 * and keep their numbering. The rest were added because the modem, the update
 * system and the JSON reader each need to distinguish a failure the original
 * seven flatten together; adding to the tail cannot renumber anything. */
typedef enum {
    ND_OK = 0,
    ND_ERR_NOMEM,      /* allocation failed -- a normal condition on 53 MB   */
    ND_ERR_IO,         /* open/read/write/ioctl failed; errno is meaningful  */
    ND_ERR_INVAL,      /* the caller passed something impossible             */
    ND_ERR_NOTFOUND,   /* the named thing does not exist                     */
    ND_ERR_TOOLONG,    /* would not fit the caller's buffer (snprintf rule)  */
    ND_ERR_HARDWARE,   /* the device answered, but wrongly, or not at all    */
    ND_ERR_BUSY,       /* try again later; the modem is mid-transaction      */
    ND_ERR_TIMEOUT,    /* waited the stated time and nothing arrived         */
    ND_ERR_PARSE,      /* a file or wire message was malformed               */
    ND_ERR_UNSUPPORTED /* understood, deliberately not implemented           */
} nd_err;

/* A short, stable, English description. Never NULL, never allocated -- the
 * returned pointer is a string literal owned by libneodct. */
const char *nd_strerror(nd_err err);

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t x;
    int32_t y;
} nd_point;

typedef struct {
    int32_t w;
    int32_t h;
} nd_size;

/* INCLUSIVE of x1 and y1, matching PIL.ImageDraw.rectangle. A rectangle with
 * x0 == x1 is one pixel wide, not zero. */
typedef struct {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} nd_rect;

static inline int32_t nd_rect_w(nd_rect r) ND_UNUSED_FN;
static inline int32_t nd_rect_w(nd_rect r)
{
    return r.x1 - r.x0 + 1;
}

static inline int32_t nd_rect_h(nd_rect r) ND_UNUSED_FN;
static inline int32_t nd_rect_h(nd_rect r)
{
    return r.y1 - r.y0 + 1;
}

#define ND_RECT(X0, Y0, X1, Y1) \
    ((nd_rect){(int32_t)(X0), (int32_t)(Y0), (int32_t)(X1), (int32_t)(Y1)})

/* ------------------------------------------------------------------ *
 * Colour
 * ------------------------------------------------------------------ */

/* Alpha is carried even for RGB surfaces so one struct covers both; drawing
 * onto an RGB target ignores it. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} nd_color;

#define ND_RGB(R, G, B)     ((nd_color){(uint8_t)(R), (uint8_t)(G), (uint8_t)(B), (uint8_t)255})
#define ND_RGBA(R, G, B, A) ((nd_color){(uint8_t)(R), (uint8_t)(G), (uint8_t)(B), (uint8_t)(A)})

/* Pillow's named colours, which is what the Python source actually writes.
 * "gray" is 128,128,128 -- not 127, and not 0x808080 rounded from anything. */
#define ND_WHITE ND_RGB(255, 255, 255)
#define ND_BLACK ND_RGB(0, 0, 0)
#define ND_GRAY  ND_RGB(128, 128, 128)

/* ------------------------------------------------------------------ *
 * Small numeric helpers with defined rounding
 * ------------------------------------------------------------------ */

static inline int32_t nd_min32(int32_t a, int32_t b) ND_UNUSED_FN;
static inline int32_t nd_min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static inline int32_t nd_max32(int32_t a, int32_t b) ND_UNUSED_FN;
static inline int32_t nd_max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static inline int32_t nd_clamp32(int32_t v, int32_t lo, int32_t hi) ND_UNUSED_FN;
static inline int32_t nd_clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Pillow truncates float coordinates toward zero: 3.6 -> 3, 8.5 -> 8,
 * -2.5 -> -2. Every scrollbar notch position in the UI goes through this, so
 * it is a named function rather than a bare cast at 40 call sites. */
static inline int32_t nd_trunc32(double v) ND_UNUSED_FN;
static inline int32_t nd_trunc32(double v)
{
    return (int32_t)v;
}

/* Python's round(): half-to-even ("banker's rounding"). C's round() rounds
 * half away from zero and gives a different answer for 4.5, 2.5 and 5.5 --
 * which changes the width of the T9 pencil's barrel at three of the sizes it
 * is drawn at. Use this wherever the Python wrote round(). */
double nd_round_half_even(double v);

/* ------------------------------------------------------------------ *
 * Bounded strings
 * ------------------------------------------------------------------ */

/* BSD semantics: always NUL-terminates when dst_sz > 0, and returns the length
 * of the string it WANTED to write. Truncation is therefore
 * (return >= dst_sz), the same test as snprintf. */
size_t nd_strlcpy(char *dst, const char *src, size_t dst_sz);
size_t nd_strlcat(char *dst, const char *src, size_t dst_sz);

/* snprintf that reports truncation as an nd_err instead of a length, for the
 * common "build a path or fail" case. */
nd_err nd_snprintf(char *dst, size_t dst_sz, const char *fmt, ...) ND_PRINTF(3, 4);

#ifdef __cplusplus
}
#endif

#endif /* ND_TYPES_H_INCLUDED */
