/* nd_props.h -- "key=value" files, in three dialects that must not be unified.
 *
 * Three different pieces of Python parse prop files and all three do it
 * slightly differently. Each difference is exercised by at least one existing
 * test, so "they are basically the same" is wrong in a way a machine can
 * demonstrate. Pick the right one per call site.
 *
 *   B-1 nd_props_parse_settings  settings.prop, version.prop
 *       Reads the WHOLE file and decodes it as STRICT UTF-8. Any decode error
 *       or any OSError yields an EMPTY map -- test_settings_version_layering
 *       writes b"\x00\xff not a prop file" into version.prop and requires
 *       exactly that. A byte-splitting implementation must therefore validate
 *       UTF-8 over the whole file first.
 *
 *   B-2 nd_props_parse_lenient   /run/neodct/sdcard.prop
 *       Same, but invalid bytes become U+FFFD instead of aborting, so a
 *       corrupt file still yields whatever lines parsed.
 *
 *   B-3 nd_props_parse_raw       RemoteShell state.prop, relay.conf
 *       Iterates LINES without pre-stripping them, so A LEADING SPACE DEFEATS
 *       THE '#' COMMENT CHECK. Only an I/O error is swallowed; a decode error
 *       propagates to the caller.
 *
 * All three: strip whitespace around key and value, skip lines with no '=',
 * split on the FIRST '=', last duplicate wins.
 *
 * ============ AND TWO WRITERS ============
 *
 * They differ in exactly one edge case and it is observable:
 *   trailing_nl_when_empty = true   -> "\n".join(lines) + "\n", so an EMPTY
 *                                      map writes a file of one newline byte
 *                                      (SettingsStorage._format_settings)
 *   trailing_nl_when_empty = false  -> "key=value\n" per line, so an empty map
 *                                      writes a ZERO-BYTE file
 *                                      (RemoteShell._write_props)
 *
 * Both write to path + ".tmp", fsync the FILE, and rename. NEITHER FSYNCS THE
 * PARENT DIRECTORY. Only CrashHandler does that. Do not add it here.
 *
 * Keys are sorted with strcmp, never strcoll: Python's sorted() is code-point
 * order and a locale must never change the file's byte layout.
 */

#ifndef ND_PROPS_H_INCLUDED
#define ND_PROPS_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest key or value we will hold. Settings keys are dotted ASCII; the
 * longest value in the project is a wallpaper path. */
#define ND_PROP_KEY_MAX   128
#define ND_PROP_VALUE_MAX 512

/* Refuses to read more than this, per SECURITY.md -- a prop file can come off
 * an SD card. */
#define ND_PROPS_MAX_BYTES (256u * 1024u)

/* An ordered key/value map. Iteration is in ASCII-sorted key order, which is
 * also the order it writes. */
typedef struct nd_props nd_props;

nd_props *nd_props_new(void);
void nd_props_free(nd_props *p);

/* dflt is returned as-is when the key is absent; the stored value otherwise.
 * The pointer is owned by the map and is invalidated by the next set. */
const char *nd_props_get(const nd_props *p, const char *key, const char *dflt);

/* Copies both strings. ND_ERR_TOOLONG past the maxima above. */
nd_err nd_props_set(nd_props *p, const char *key, const char *value);

bool nd_props_has(const nd_props *p, const char *key);
nd_err nd_props_remove(nd_props *p, const char *key);
size_t nd_props_count(const nd_props *p);

/* Sorted iteration. NULL past the end. */
const char *nd_props_key_at(const nd_props *p, size_t i);
const char *nd_props_value_at(const nd_props *p, size_t i);

/* Copy every entry of src over dst, src winning. This is the layering step. */
nd_err nd_props_update(nd_props *dst, const nd_props *src);

/* ---- the three dialects ---- */

/* B-1 and B-2 return an EMPTY map on any failure, never NULL -- except on
 * allocation failure, which is NULL. That mirrors the Python returning {}. */
nd_props *nd_props_parse_settings(const char *path);
nd_props *nd_props_parse_lenient(const char *path);

/* B-3 reports a malformed file to the caller instead of hiding it. A missing
 * file is still an empty map and ND_OK. */
nd_err nd_props_parse_raw(const char *path, nd_props **out);

/* ---- the writer ---- */

nd_err nd_props_write_atomic(const char *path, const nd_props *p, bool trailing_nl_when_empty);

#ifdef __cplusplus
}
#endif

#endif /* ND_PROPS_H_INCLUDED */
