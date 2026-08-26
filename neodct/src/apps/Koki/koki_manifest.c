/* koki_manifest.c -- reading assets/manifest.json into the runtime's tables.
 *
 * A two-pass load: count the costumes and sounds first so the two arenas can
 * be one allocation each, then fill them. The alternative -- a malloc per
 * costume -- is 262 allocations that live for the whole game and are freed in
 * one go anyway, which is exactly the shape an arena is for.
 *
 * ============ WHAT IS DELIBERATELY NOT VALIDATED ============
 *
 * A missing field takes the Python's default (json .get with a default) and
 * says nothing. A malformed manifest is a build error in build_assets.py,
 * not a runtime condition to recover from -- the file ships inside the same
 * read-only squashfs as this code. What IS checked is anything that would
 * make the runtime index out of bounds: a target with no costumes is
 * rejected, and the arena counts are the ones the first pass measured.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_json.h"
#include "nd_log.h"
#include "nd_types.h"

#include "koki_manifest.h"

void koki_lower(char *out, size_t out_sz, const char *in)
{
    size_t i = 0u;

    if (out == NULL || out_sz == 0u)
        return;
    if (in != NULL) {
        for (; in[i] != '\0' && i + 1u < out_sz; i++) {
            char c = in[i];

            out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    out[i] = '\0';
}

static koki_rot_style parse_rot(const char *s)
{
    /* Only two values occur; anything else is "all around", which is what
     * the Python's spec.get(..., "all around") does. */
    return (s != NULL && strcmp(s, "left-right") == 0) ? KOKI_ROT_LEFT_RIGHT : KOKI_ROT_ALL_AROUND;
}

static double json_real(const nd_json_val *obj, const char *key, double dflt)
{
    const nd_json_val *v = nd_json_get(obj, key);
    double out;

    if (v == NULL)
        return dflt;
    if (nd_json_real(v, &out))
        return out;
    return dflt;
}

/* ------------------------------------------------------------------ *
 * Pass 1 -- how much room the arenas need
 * ------------------------------------------------------------------ */

static void count_arena(const nd_json_val *targets, size_t *n_costumes, size_t *n_sounds)
{
    size_t i;

    *n_costumes = 0u;
    *n_sounds = 0u;
    for (i = 0u; i < nd_json_len(targets); i++) {
        const nd_json_val *t = nd_json_member_at(targets, i);
        const nd_json_val *cs = nd_json_get(t, "costumes");
        const nd_json_val *ss = nd_json_get(t, "sounds");

        *n_costumes += nd_json_len(cs);
        *n_sounds += nd_json_len(ss);
    }
}

/* ------------------------------------------------------------------ *
 * Pass 2 -- fill
 * ------------------------------------------------------------------ */

static void fill_costume(koki_costume *c, const nd_json_val *jc)
{
    const nd_json_val *bbox;

    (void)nd_strlcpy(c->name, nd_json_get_str(jc, "name", ""), sizeof c->name);
    koki_lower(c->lname, sizeof c->lname, c->name);
    (void)nd_strlcpy(c->img, nd_json_get_str(jc, "img", ""), sizeof c->img);
    c->cx = json_real(jc, "cx", 0.0);
    c->cy = json_real(jc, "cy", 0.0);

    /* "bbox": null is the whole image, and 33 of the 262 costumes are that
     * way. Distinguishing null from absent matters not at all here; both
     * mean the same thing to screen_rect(). */
    bbox = nd_json_get(jc, "bbox");
    c->has_bbox = false;
    c->bx0 = c->by0 = c->bx1 = c->by1 = 0;
    if (bbox != NULL && nd_json_type_of(bbox) == ND_JSON_ARRAY && nd_json_len(bbox) == 4u) {
        int64_t v[4];
        size_t k;
        bool ok = true;

        for (k = 0u; k < 4u; k++) {
            if (!nd_json_int(nd_json_at(bbox, k), &v[k]))
                ok = false;
        }
        if (ok) {
            c->has_bbox = true;
            c->bx0 = (int32_t)v[0];
            c->by0 = (int32_t)v[1];
            c->bx1 = (int32_t)v[2];
            c->by1 = (int32_t)v[3];
        }
    }
}

static void fill_target(koki_target *t, const char *name, const nd_json_val *jt,
                        koki_costume **c_next, koki_sound **s_next)
{
    const nd_json_val *cs = nd_json_get(jt, "costumes");
    const nd_json_val *ss = nd_json_get(jt, "sounds");
    size_t i;

    (void)nd_strlcpy(t->name, name, sizeof t->name);
    t->size = json_real(jt, "size", 100.0);
    t->default_size = json_real(jt, "default_size", t->size);
    t->x = json_real(jt, "x", 0.0);
    t->y = json_real(jt, "y", 0.0);
    t->direction = json_real(jt, "direction", 90.0);
    t->rotation_style = parse_rot(nd_json_get_str(jt, "rotation_style", "all around"));
    t->current_costume = (int32_t)nd_json_get_int(jt, "current_costume", 0);

    t->costumes = *c_next;
    t->n_costumes = nd_json_len(cs);
    for (i = 0u; i < t->n_costumes; i++)
        fill_costume(&t->costumes[i], nd_json_at(cs, i));
    *c_next += t->n_costumes;

    t->sounds = *s_next;
    t->n_sounds = nd_json_len(ss);
    for (i = 0u; i < t->n_sounds; i++) {
        const nd_json_val *js = nd_json_member_at(ss, i);
        koki_sound *snd = &t->sounds[i];

        (void)nd_strlcpy(snd->name, nd_json_key_at(ss, i), sizeof snd->name);
        (void)nd_strlcpy(snd->file, nd_json_get_str(js, "file", ""), sizeof snd->file);
        snd->dur = json_real(js, "dur", 0.0);
    }
    *s_next += t->n_sounds;
}

/* ------------------------------------------------------------------ *
 * Load
 * ------------------------------------------------------------------ */

koki_manifest *koki_manifest_load(const char *assets_dir)
{
    char path[ND_PATH_MAX];
    char err[160];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    const nd_json_val *targets;
    koki_manifest *m = NULL;
    koki_costume *c_next;
    koki_sound *s_next;
    size_t i;

    if (assets_dir == NULL)
        return NULL;
    if (nd_snprintf(path, sizeof path, "%s/manifest.json", assets_dir) != ND_OK) {
        nd_log_err(ND_LOG_KOKI, "assets path too long: %s", assets_dir);
        return NULL;
    }
    if (nd_json_parse_file(path, &doc, err, sizeof err) != ND_OK) {
        nd_log_err(ND_LOG_KOKI, "manifest.json: %s", err);
        return NULL;
    }

    root = nd_json_root(doc);
    targets = nd_json_get(root, "targets");
    if (targets == NULL || nd_json_type_of(targets) != ND_JSON_OBJECT) {
        nd_log_err(ND_LOG_KOKI, "manifest.json has no targets object");
        goto fail;
    }
    if (nd_json_len(targets) > KOKI_MAX_TARGETS) {
        nd_log_err(ND_LOG_KOKI, "manifest.json has %zu targets, table holds %d",
                   nd_json_len(targets), KOKI_MAX_TARGETS);
        goto fail;
    }

    m = calloc(1u, sizeof *m);
    if (m == NULL)
        goto fail;
    m->stage_scale = json_real(root, "stage_scale", KOKI_STAGE_SCALE);

    count_arena(targets, &m->n_costumes, &m->n_sounds);
    if (m->n_costumes == 0u) {
        nd_log_err(ND_LOG_KOKI, "manifest.json declares no costumes");
        goto fail;
    }
    /* 262 * 152 = 39,824 bytes, and 110 * 112 = 12,320. Both live for the
     * whole run and are freed in one call each. */
    m->costume_arena = calloc(m->n_costumes, sizeof *m->costume_arena);
    m->sound_arena = (m->n_sounds > 0u) ? calloc(m->n_sounds, sizeof *m->sound_arena) : NULL;
    if (m->costume_arena == NULL || (m->n_sounds > 0u && m->sound_arena == NULL))
        goto fail;

    c_next = m->costume_arena;
    s_next = m->sound_arena;
    for (i = 0u; i < nd_json_len(targets); i++) {
        fill_target(&m->targets[i], nd_json_key_at(targets, i), nd_json_member_at(targets, i),
                    &c_next, &s_next);
    }
    m->n_targets = nd_json_len(targets);

    nd_json_free(doc);
    return m;

fail:
    nd_json_free(doc);
    koki_manifest_free(m);
    return NULL;
}

void koki_manifest_free(koki_manifest *m)
{
    if (m == NULL)
        return;
    free(m->costume_arena);
    free(m->sound_arena);
    free(m);
}

const koki_target *koki_manifest_target(const koki_manifest *m, const char *name)
{
    size_t i;

    if (m == NULL || name == NULL)
        return NULL;
    for (i = 0u; i < m->n_targets; i++) {
        if (strcmp(m->targets[i].name, name) == 0)
            return &m->targets[i];
    }
    return NULL;
}

const koki_sound *koki_sound_find(const koki_sound *sounds, size_t n, const char *name)
{
    size_t i;

    if (sounds == NULL || name == NULL)
        return NULL;
    for (i = 0u; i < n; i++) {
        if (strcmp(sounds[i].name, name) == 0)
            return &sounds[i];
    }
    return NULL;
}
