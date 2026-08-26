/* koki_cache.c -- engine.py's LRUImages: byte-budgeted, never entry-counted.
 *
 * docs/KOKI_PORT_NOTES.md records why this is byte-budgeted and
 * CODING-STANDARDS.md section 4 repeats it: an entry-counted cache of Koki's
 * costumes is a cache whose size depends on which level you are in, and the
 * level that thrashes is the one on the device rather than the one on the
 * desk. Three instances, all here:
 *
 *   img   path                            -> decoded RGBA
 *   fx    (path, size, flip, ghost, bri)  -> the processed variant
 *   mask  (path, flip)                    -> thresholded 8-bit alpha
 *
 * ============ TWO RULES THAT LOOK LIKE BUGS AND ARE NOT ============
 *
 * 1. put() on a key that is ALREADY PRESENT moves it to the recent end and
 *    keeps the OLD image. The Python does this (`if key in self.map:
 *    move_to_end; return`) and it is load-bearing for the fx cache, whose
 *    callers race to compute the same variant. In C the new image would leak,
 *    so this takes ownership and frees it -- the caller's contract is "put()
 *    consumes the image either way".
 *
 * 2. Eviction stops at ONE entry (`len(self.map) > 1`), so a single image
 *    larger than the whole budget stays cached rather than being evicted
 *    immediately and re-decoded on every frame. Level 2's biggest backdrop
 *    sprite is the case that made the Python grow this guard.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_types.h"

#include "koki.h"

/* LRUImages._cost: width * height * len(getbands()). */
static size_t img_cost(const nd_image *img)
{
    if (img == NULL)
        return 0u;
    return (size_t)img->w * (size_t)img->h * (size_t)img->bpp;
}

static void unlink_entry(koki_lru *c, koki_lru_entry *e)
{
    if (e->prev != NULL)
        e->prev->next = e->next;
    else
        c->head = e->next;
    if (e->next != NULL)
        e->next->prev = e->prev;
    else
        c->tail = e->prev;
    e->prev = NULL;
    e->next = NULL;
}

/* Append at the most-recently-used end. */
static void link_tail(koki_lru *c, koki_lru_entry *e)
{
    e->prev = c->tail;
    e->next = NULL;
    if (c->tail != NULL)
        c->tail->next = e;
    else
        c->head = e;
    c->tail = e;
}

static void drop_entry(koki_lru *c, koki_lru_entry *e)
{
    unlink_entry(c, e);
    c->bytes -= e->cost;
    c->count--;
    nd_image_free(e->img);
    free(e);
}

/* FNV-1a. The cache is a list, not a map -- at the measured peak of a few
 * hundred entries a linear scan is cheaper than a hash table's indirection,
 * PROVIDED the comparison is an integer rather than a 46-character strcmp. */
static uint32_t key_hash(const char *key)
{
    uint32_t h = 2166136261u;

    for (; *key != '\0'; key++) {
        h ^= (uint32_t)(unsigned char)*key;
        h *= 16777619u;
    }
    return h;
}

static koki_lru_entry *find(const koki_lru *c, const char *key, uint32_t hash)
{
    koki_lru_entry *e;

    for (e = c->head; e != NULL; e = e->next) {
        if (e->hash == hash && strcmp(e->key, key) == 0)
            return e;
    }
    return NULL;
}

void koki_lru_init(koki_lru *c, size_t budget_bytes)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    c->budget = budget_bytes;
}

nd_image *koki_lru_get(koki_lru *c, const char *key)
{
    koki_lru_entry *e;

    if (c == NULL || key == NULL)
        return NULL;
    e = find(c, key, key_hash(key));
    if (e == NULL)
        return NULL;
    /* move_to_end: a hit is the most recent use. */
    unlink_entry(c, e);
    link_tail(c, e);
    return e->img;
}

void koki_lru_put(koki_lru *c, const char *key, nd_image *img)
{
    koki_lru_entry *e;
    uint32_t hash;

    if (c == NULL || key == NULL || img == NULL) {
        nd_image_free(img);
        return;
    }
    if (strlen(key) + 1u > sizeof e->key) {
        /* Not a cache miss to recover from silently: a key this long means a
         * costume path longer than the manifest can produce. Drop it rather
         * than truncate, which would alias two variants onto one entry. */
        nd_image_free(img);
        return;
    }

    hash = key_hash(key);
    e = find(c, key, hash);
    if (e != NULL) {
        /* Rule 1: keep the old image, promote the entry, consume the new. */
        unlink_entry(c, e);
        link_tail(c, e);
        nd_image_free(img);
        return;
    }

    e = calloc(1u, sizeof *e);
    if (e == NULL) {
        nd_image_free(img);
        return;
    }
    (void)nd_strlcpy(e->key, key, sizeof e->key);
    e->hash = hash;
    e->img = img;
    e->cost = img_cost(img);
    link_tail(c, e);
    c->bytes += e->cost;
    c->count++;

    /* Rule 2: never evict the last entry. */
    while (c->bytes > c->budget && c->count > 1u && c->head != NULL)
        drop_entry(c, c->head);
}

void koki_lru_clear(koki_lru *c)
{
    if (c == NULL)
        return;
    while (c->head != NULL)
        drop_entry(c, c->head);
    /* teardown() zeroes the byte counter as well as the map; a counter left
     * behind is how the Python's first relaunch used to evict everything on
     * its first frame. */
    c->bytes = 0u;
    c->count = 0u;
}
