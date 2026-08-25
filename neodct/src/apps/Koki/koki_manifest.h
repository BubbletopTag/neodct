/* koki_manifest.h -- assets/manifest.json, decoded once.
 *
 * 52,758 bytes of JSON describing 47 targets, 262 costumes and 110 sound
 * entries. It is the contract between tools/build_assets.py (which runs on a
 * developer's machine and pre-scales every costume to its final size) and
 * this runtime, so nothing here recomputes anything the baker already
 * decided -- cx/cy arrive already scaled, and a costume PNG is blitted, not
 * resized, unless a script changes the sprite's size at run time.
 *
 * Everything is read into two flat arenas at load and never reallocated: a
 * sprite's costume list is a pointer into the costume arena, not its own
 * allocation. One malloc per arena, one free at teardown, no per-costume
 * bookkeeping.
 */

#ifndef KOKI_MANIFEST_H_INCLUDED
#define KOKI_MANIFEST_H_INCLUDED

#include "koki.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KOKI_MAX_TARGETS 64 /* 47 in the shipped manifest */

typedef struct {
    char name[KOKI_NAME_MAX];
    koki_costume *costumes; /* into the arena */
    size_t n_costumes;
    koki_sound *sounds; /* into the arena */
    size_t n_sounds;
    double size;         /* the % the costumes were BAKED at */
    double default_size; /* the sprite's runtime starting size % */
    double x, y, direction;
    koki_rot_style rotation_style;
    int32_t current_costume; /* 0-based */
} koki_target;

typedef struct koki_manifest {
    double stage_scale;
    koki_target targets[KOKI_MAX_TARGETS];
    size_t n_targets;

    koki_costume *costume_arena; /* owned */
    size_t n_costumes;
    koki_sound *sound_arena; /* owned */
    size_t n_sounds;
} koki_manifest;

/* Parse <assets_dir>/manifest.json. Owned by the caller; free with
 * koki_manifest_free(). NULL on any failure, with the reason logged. */
koki_manifest *koki_manifest_load(const char *assets_dir);
void koki_manifest_free(koki_manifest *m);

/* Exact-name lookup; NULL when absent. */
const koki_target *koki_manifest_target(const koki_manifest *m, const char *name);

/* Exact-name sound lookup within a target, as Python's dict.get() is.
 * Costume lookup is case-INSENSITIVE and lives on the sprite. */
const koki_sound *koki_sound_find(const koki_sound *sounds, size_t n, const char *name);

/* ASCII lower-casing, which is all the manifest ever needs: every costume
 * name in the file is ASCII. Writes at most out_sz-1 characters. */
void koki_lower(char *out, size_t out_sz, const char *in);

#ifdef __cplusplus
}
#endif

#endif /* KOKI_MANIFEST_H_INCLUDED */
