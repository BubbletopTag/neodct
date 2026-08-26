/* nd_json.h -- one JSON reader for the whole project.
 *
 * Consumers: app manifests, ui_home.json, keymap.json, update manifests, Koki
 * asset bundles and GitHub's release listing. They are written twice at your
 * peril -- the update system's manifest validation depends on distinctions
 * that a casual parser throws away.
 *
 * ============ THE FOUR THINGS THAT ARE NOT NEGOTIABLE ============
 *
 *   1. AN INTEGER IS NOT A FLOAT. The update manifest must reject
 *      "buildtime": 1785160800.0 while accepting 1785160800. ND_JSON_INT and
 *      ND_JSON_REAL are separate types for exactly this reason.
 *   2. A BOOLEAN IS NOT A NUMBER. "buildtime": true must be rejected, and it
 *      is not rejected by a parser that stores true as 1.
 *   3. LAST DUPLICATE KEY WINS, which is what Python's json does.
 *   4. THERE IS A HARD INPUT CAP. This parser reads files off an SD card that
 *      arrived from who-knows-where; see SECURITY.md.
 *
 * \uXXXX escapes decode to UTF-8, surrogate pairs included -- GitHub release
 * notes contain arbitrary text.
 *
 * ============ MEMORY ============
 *
 * One arena per document. Values point into it; nothing is individually
 * freed and nothing is individually allocated. Free the document and every
 * value from it becomes invalid at once. That is the only ownership rule.
 */

#ifndef ND_JSON_H_INCLUDED
#define ND_JSON_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Biggest document we will look at. The largest thing in the project is a
 * GitHub release listing; Koki's asset manifest is next. 1 MB is generous and
 * bounded, which is the point. */
#define ND_JSON_MAX_BYTES (1024u * 1024u)

/* Deepest nesting. Recursive descent on untrusted input needs a ceiling --
 * CODING-STANDARDS.md section 1.5. */
#define ND_JSON_MAX_DEPTH 32

typedef enum {
    ND_JSON_NULL = 0,
    ND_JSON_BOOL,
    ND_JSON_INT,  /* no '.', no exponent -- a whole number as written */
    ND_JSON_REAL, /* had a '.' or an exponent                         */
    ND_JSON_STRING,
    ND_JSON_ARRAY,
    ND_JSON_OBJECT
} nd_json_type;

typedef struct nd_json_doc nd_json_doc;
typedef struct nd_json_val nd_json_val;

/* ------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------ */

/* Parse a buffer. *out is owned by the caller; free with nd_json_free().
 * On failure *out is NULL and, when err_out is non-NULL, it receives a short
 * message naming the byte offset -- these strings reach the user on the
 * update screens, so keep them terse and free of jargon. */
nd_err nd_json_parse(const uint8_t *data, size_t len, nd_json_doc **out, char *err_out,
                     size_t err_sz);

/* Read a file and parse it. Files larger than ND_JSON_MAX_BYTES are refused
 * with ND_ERR_TOOLONG before any of it is read into memory. */
nd_err nd_json_parse_file(const char *path, nd_json_doc **out, char *err_out, size_t err_sz);

void nd_json_free(nd_json_doc *doc);

/* The document's root value. NULL only for a NULL document. */
const nd_json_val *nd_json_root(const nd_json_doc *doc);

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

nd_json_type nd_json_type_of(const nd_json_val *v); /* ND_JSON_NULL for NULL */

/* Object lookup. NULL when v is not an object or the key is absent. */
const nd_json_val *nd_json_get(const nd_json_val *v, const char *key);

/* Array access. nd_json_len() is 0 for anything that is not an array or an
 * object; for an object it is the number of members. */
size_t nd_json_len(const nd_json_val *v);
const nd_json_val *nd_json_at(const nd_json_val *v, size_t i);

/* Object iteration, in document order after duplicate resolution. */
const char *nd_json_key_at(const nd_json_val *v, size_t i);
const nd_json_val *nd_json_member_at(const nd_json_val *v, size_t i);

/* Typed readers. Each returns false and leaves *out alone when the value is
 * missing or of the wrong type -- there is no coercion anywhere, deliberately.
 * The string from nd_json_str() points into the document's arena. */
bool nd_json_bool(const nd_json_val *v, bool *out);
bool nd_json_int(const nd_json_val *v, int64_t *out);
bool nd_json_real(const nd_json_val *v, double *out);
bool nd_json_str(const nd_json_val *v, const char **out);

/* The convenience forms every manifest reader wants: look up a key and read
 * it in one step, with a default. Absent, wrong type or malformed all give
 * the default -- which is the Python's data.get("x", default) behaviour. */
bool nd_json_get_bool(const nd_json_val *obj, const char *key, bool dflt);
int64_t nd_json_get_int(const nd_json_val *obj, const char *key, int64_t dflt);
const char *nd_json_get_str(const nd_json_val *obj, const char *key, const char *dflt);

/* ------------------------------------------------------------------ *
 * Writing
 * ------------------------------------------------------------------ */

/* A minimal emitter. Only three things in the project write JSON -- the
 * keypad wizard's keymap.json, the update staging records and the test
 * tooling -- so this is a builder, not a general serialiser.
 *
 * Keys are emitted in insertion order. Output is compact (no spaces) unless
 * indent > 0, in which case it matches json.dump(..., indent=N). */
typedef struct nd_json_writer nd_json_writer;

nd_json_writer *nd_json_writer_new(int indent);
void nd_json_writer_free(nd_json_writer *w);

nd_err nd_json_begin_object(nd_json_writer *w);
nd_err nd_json_end_object(nd_json_writer *w);
nd_err nd_json_begin_array(nd_json_writer *w);
nd_err nd_json_end_array(nd_json_writer *w);
nd_err nd_json_key(nd_json_writer *w, const char *key);
nd_err nd_json_put_null(nd_json_writer *w);
nd_err nd_json_put_bool(nd_json_writer *w, bool v);
nd_err nd_json_put_int(nd_json_writer *w, int64_t v);
nd_err nd_json_put_real(nd_json_writer *w, double v);
nd_err nd_json_put_str(nd_json_writer *w, const char *v);

/* The finished text, NUL-terminated, owned by the writer. NULL if the
 * document is unbalanced. */
const char *nd_json_writer_text(const nd_json_writer *w, size_t *len_out);

/* Write it out with the atomic temp-file-and-rename dance. */
nd_err nd_json_writer_save(const nd_json_writer *w, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ND_JSON_H_INCLUDED */
