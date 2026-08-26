/* nd_imgcache.c -- NeoDCT_UI.get_image's 32-entry FIFO, ported literally.
 *
 * From System/core/main.py:714 (`get_image`) and :754 (`_cache_put`). The
 * Python keeps a dict, relies on dict insertion order, and evicts with
 * `self.image_cache.pop(next(iter(self.image_cache)))` -- the OLDEST INSERTED
 * entry, never the least recently used. A cache HIT does not reorder anything.
 * Decision C-4 in OPEN-QUESTIONS.md keeps that policy exactly: eviction order
 * is observable as a decode stall, and 32 icons at 82x82 RGBA is 860 KB
 * against a 53 MB budget.
 *
 * ============ THE ONE ADDITION C-4 ASKED FOR ============
 *
 * A hard PER-IMAGE ceiling, so one enormous PNG cannot take a slot at full
 * size. It is not an eviction-policy change -- it decides whether a decode is
 * admitted at all, and it is set (8 MiB of pixels, i.e. 2048x1024 RGBA) far
 * above anything the project ships. The largest asset in the overlay is a
 * 27 KB Koki costume; the biggest thing that reaches this cache is an 82x82
 * icon at 27 KB of pixels. Nothing shipped can trip it.
 *
 * An image over the ceiling is freed and reported as a miss -- NULL, exactly
 * as the Python returns None when a decode fails -- because the contract in
 * nd_image.h says the cache owns what it hands back, and handing back
 * something nobody owns is worse than not drawing an absurd picture.
 *
 * ============ THE KEY ============
 *
 * Three shapes, matching the Python's f-strings byte for byte, because the
 * same file at two scales is two entries and must not collide:
 *
 *     "<path>"                plain
 *     "<path>@<max_size>"     f"{clean_path}@{int(max_size)}"
 *     "<path>@x<scale:%g>"    f"{clean_path}@x{scale:g}"
 *
 * C's "%g" and Python's "%g" agree on the only value the phone ever uses,
 * 175/240 -> "0.729167".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

/* Path plus the "@x0.729167" suffix, with room to spare. */
#define IMGCACHE_KEY_MAX (ND_PATH_MAX + 32)

/* 8 MiB of decoded pixels. See the header comment: this is an admission
 * ceiling, not an eviction rule. */
#define IMGCACHE_IMAGE_MAX_BYTES ((size_t)8u * 1024u * 1024u)

typedef struct {
    char key[IMGCACHE_KEY_MAX];
    nd_image *img; /* owned by the cache */
} imgcache_entry;

struct nd_imgcache {
    imgcache_entry *slots; /* insertion-ordered; slot 0 is the oldest */
    size_t n;
    size_t max;
};

/* owned by the caller; free with nd_imgcache_free() */
nd_imgcache *nd_imgcache_new(size_t max_entries)
{
    nd_imgcache *c;

    if (max_entries == 0u)
        max_entries = ND_IMGCACHE_MAX;

    c = calloc(1u, sizeof *c);
    if (c == NULL)
        return NULL;

    c->slots = calloc(max_entries, sizeof *c->slots);
    if (c->slots == NULL) {
        free(c);
        return NULL;
    }
    c->max = max_entries;
    return c;
}

void nd_imgcache_clear(nd_imgcache *c)
{
    size_t i;

    if (c == NULL)
        return;
    for (i = 0u; i < c->n; i++) {
        nd_image_free(c->slots[i].img);
        c->slots[i].img = NULL;
        c->slots[i].key[0] = '\0';
    }
    c->n = 0u;
}

void nd_imgcache_free(nd_imgcache *c)
{
    if (c == NULL)
        return;
    nd_imgcache_clear(c);
    free(c->slots);
    free(c);
}

static nd_err imgcache_key(char *out, size_t out_sz, const char *path, int32_t max_size,
                           double scale)
{
    if (max_size > 0)
        return nd_snprintf(out, out_sz, "%s@%d", path, (int)max_size);
    if (scale > 0.0)
        return nd_snprintf(out, out_sz, "%s@x%g", path, scale);
    return nd_snprintf(out, out_sz, "%s", path);
}

/* FIFO on insertion order: slot 0 leaves first, which is what
 * `pop(next(iter(dict)))` does. */
static void imgcache_put(nd_imgcache *c, const char *key, nd_image *img)
{
    if (c->n >= c->max) {
        nd_image_free(c->slots[0].img);
        memmove(&c->slots[0], &c->slots[1], (c->max - 1u) * sizeof c->slots[0]);
        c->n = c->max - 1u;
    }
    (void)nd_strlcpy(c->slots[c->n].key, key, sizeof c->slots[c->n].key);
    c->slots[c->n].img = img;
    c->n++;
}

static size_t image_bytes(const nd_image *img)
{
    return (size_t)img->w * (size_t)img->h * (size_t)img->bpp;
}

/* Owned by the cache. NULL on any failure, exactly as the Python's bare
 * `except: return None`. */
const nd_image *nd_imgcache_get(nd_imgcache *c, const char *path, int32_t max_size, double scale)
{
    char key[IMGCACHE_KEY_MAX];
    nd_image *raw = NULL;
    nd_image *rgba = NULL;
    size_t i;

    if (c == NULL || path == NULL || path[0] == '\0')
        return NULL;
    if (imgcache_key(key, sizeof key, path, max_size, scale) != ND_OK)
        return NULL;

    for (i = 0u; i < c->n; i++) {
        if (strcmp(c->slots[i].key, key) == 0)
            return c->slots[i].img;
    }

    raw = nd_image_open(path);
    if (raw == NULL)
        return NULL;

    /* Image.open(path).convert("RGBA") -- always a new surface, so the
     * ownership rule stays "the cache owns exactly one image". */
    rgba = nd_image_convert(raw, ND_PIXFMT_RGBA8888);
    nd_image_free(raw);
    if (rgba == NULL)
        return NULL;

    if (max_size > 0) {
        /* img.thumbnail((n, n), LANCZOS) -- only when it does not already
         * fit, which is the Python's explicit `if img.width > max_size or
         * img.height > max_size`. nd_image_thumbnail never upscales, so the
         * guard is belt and braces rather than a behaviour change. */
        if (rgba->w > max_size || rgba->h > max_size) {
            if (nd_image_thumbnail(rgba, max_size, max_size) != ND_OK) {
                nd_image_free(rgba);
                return NULL;
            }
        }
    } else if (scale > 0.0) {
        int32_t w = nd_max32(1, nd_trunc32((double)rgba->w * scale));
        int32_t h = nd_max32(1, nd_trunc32((double)rgba->h * scale));

        if (w != rgba->w || h != rgba->h) {
            nd_image *scaled = nd_image_resize_lanczos(rgba, w, h);

            nd_image_free(rgba);
            if (scaled == NULL)
                return NULL;
            rgba = scaled;
        }
    }

    if (image_bytes(rgba) > IMGCACHE_IMAGE_MAX_BYTES) {
        /* Decision C-4's ceiling. Nothing the project ships reaches it, so
         * say so loudly rather than silently dropping a picture. */
        nd_log_err(ND_LOG_UI, "image too large to cache (%dx%d): %s", rgba->w, rgba->h, path);
        nd_image_free(rgba);
        return NULL;
    }

    imgcache_put(c, key, rgba);
    return rgba;
}
