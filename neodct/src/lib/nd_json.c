/* nd_json.c -- one JSON reader and one minimal emitter for the whole project.
 *
 * Consumers: app manifests, ui_home.json, keymap.json, update manifests, Koki
 * asset bundles and GitHub's release listing. The last two arrive over the
 * network or on an SD card, so this parser is written as if every document is
 * hostile, which some of them are.
 *
 * ============ THE FOUR THINGS THAT ARE NOT NEGOTIABLE ============
 *
 *   1. An integer is not a float. The update manifest must reject
 *      "buildtime": 1785160800.0 while accepting 1785160800.
 *   2. A boolean is not a number. "buildtime": true must be rejected, and a
 *      parser that stores true as 1 cannot reject it.
 *   3. Last duplicate key wins, which is what Python's json does -- and the
 *      surviving entry keeps the FIRST occurrence's position, because that is
 *      what assigning to an existing dict key does.
 *   4. There is a hard input cap, and a hard nesting cap, and a hard value
 *      count. See SECURITY.md.
 *
 * ============ WHY THERE IS NO RECURSION ============
 *
 * CODING-STANDARDS.md section 1.5: nothing sized by input goes on the stack.
 * A recursive-descent reader with a depth limit would satisfy the letter of
 * that and still put an attacker in charge of how much stack we use. So the
 * parser is a loop over an explicit array of ND_JSON_MAX_DEPTH levels, and
 * the depth limit is enforced by the size of that array rather than by a
 * counter somebody can forget to check.
 *
 * ============ MEMORY ============
 *
 * One arena per document, built from linked blocks so that a value handed out
 * early is not invalidated by a value parsed later. Nothing is individually
 * freed; nd_json_free() drops the whole chain. The temporary vectors the
 * parser builds containers in are separate and go away when parsing finishes.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_json.h"
#include "nd_paths.h"
#include "nd_utf8_priv.h"

/* An object with more members than this is not something this project reads,
 * and the duplicate-key scan below is quadratic in the member count. */
#define ND_JSON_MAX_MEMBERS 4096u

/* Total values in one document. A megabyte of "[1,1,1,..." would otherwise
 * turn into tens of megabytes of parse-time vectors on a 53 MB phone. */
#define ND_JSON_MAX_VALUES 65536u

/* ------------------------------------------------------------------ *
 * Arena
 * ------------------------------------------------------------------ */

/* The flexible array member is max_align_t, not unsigned char, so the block's
 * payload starts on the strictest alignment the platform has. On 32-bit ARM
 * the three-word header is 12 bytes and a char[] payload would begin at
 * offset 12 -- and an int64_t read from there faults, where x86 would have
 * quietly worked. */
typedef struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t cap;
    max_align_t data[];
} arena_block;

#define ARENA_BLOCK_MIN 8192u

typedef struct nd_json_member nd_json_member;

struct nd_json_val {
    nd_json_type type;
    union {
        bool b;
        int64_t i;
        double r;
        const char *s;
        struct {
            nd_json_val *items;
            size_t n;
        } arr;
        struct {
            nd_json_member *members;
            size_t n;
        } obj;
    } u;
};

struct nd_json_member {
    const char *key;
    nd_json_val val;
};

struct nd_json_doc {
    arena_block *blocks;
    nd_json_val *root;
};

static void *arena_alloc(nd_json_doc *doc, size_t n)
{
    arena_block *b = doc->blocks;
    size_t aligned = (n + 15u) & ~(size_t)15u;
    void *p;

    if (aligned < n) /* overflow */
        return NULL;

    if (b == NULL || b->cap - b->used < aligned) {
        size_t cap = aligned > ARENA_BLOCK_MIN ? aligned : ARENA_BLOCK_MIN;

        /* owned by the document; released as a chain in nd_json_free() */
        b = malloc(sizeof *b + cap);
        if (b == NULL)
            return NULL;
        b->next = doc->blocks;
        b->used = 0u;
        b->cap = cap;
        doc->blocks = b;
    }

    p = (unsigned char *)b->data + b->used;
    b->used += aligned;
    return p;
}

static const char *arena_strn(nd_json_doc *doc, const char *s, size_t n)
{
    char *p = arena_alloc(doc, n + 1u);

    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void nd_json_free(nd_json_doc *doc)
{
    arena_block *b;

    if (doc == NULL)
        return;

    b = doc->blocks;
    while (b != NULL) {
        arena_block *next = b->next;

        free(b);
        b = next;
    }
    free(doc);
}

/* ------------------------------------------------------------------ *
 * Parser
 * ------------------------------------------------------------------ */

typedef struct {
    bool is_object;
    const char *pending_key; /* set just before the value it belongs to */
    nd_json_member *items;   /* temporary; key is NULL for arrays        */
    uint64_t *hashes;        /* key hashes, so duplicate lookup is cheap */
    size_t n;
    size_t cap;
} level;

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    nd_json_doc *doc;
    char *scratch; /* string decoding buffer, grown as needed */
    size_t scratch_cap;
    size_t values;
    char *err;
    size_t err_sz;
    level levels[ND_JSON_MAX_DEPTH];
    size_t depth;
} ctx;

static nd_err fail(ctx *c, const char *what)
{
    if (c->err != NULL && c->err_sz > 0u)
        (void)snprintf(c->err, c->err_sz, "%s at byte %zu", what, c->pos);
    return ND_ERR_PARSE;
}

static void skip_ws(ctx *c)
{
    while (c->pos < c->len) {
        uint8_t ch = c->p[c->pos];

        /* Exactly the four JSON whitespace characters. A parser that also
         * skipped vertical tab would accept documents Python rejects. */
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
            c->pos++;
        else
            break;
    }
}

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a */

    while (*s != '\0') {
        h ^= (uint64_t)(unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

static nd_err scratch_reserve(ctx *c, size_t n)
{
    size_t cap = c->scratch_cap;
    char *grown;

    if (n <= cap)
        return ND_OK;
    if (cap == 0u)
        cap = 256u;
    while (cap < n)
        cap *= 2u;

    grown = realloc(c->scratch, cap);
    if (grown == NULL)
        return ND_ERR_NOMEM;
    c->scratch = grown;
    c->scratch_cap = cap;
    return ND_OK;
}

static int hex_nibble(uint8_t ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static size_t encode_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
}

/* Reads a \uXXXX escape (the backslash and u already consumed) and returns
 * the 16-bit value, or -1. */
static int32_t read_u16(ctx *c)
{
    int32_t v = 0;
    size_t i;

    if (c->len - c->pos < 4u)
        return -1;
    for (i = 0u; i < 4u; i++) {
        int nib = hex_nibble(c->p[c->pos + i]);

        if (nib < 0)
            return -1;
        v = (v << 4) | nib;
    }
    c->pos += 4u;
    return v;
}

/* Parses a string literal into the scratch buffer. *out points into the
 * document arena on success. */
static nd_err parse_string(ctx *c, const char **out)
{
    size_t w = 0u;
    nd_err rc;

    if (c->pos >= c->len || c->p[c->pos] != '"')
        return fail(c, "expected a string");
    c->pos++;

    for (;;) {
        uint8_t ch;

        if (c->pos >= c->len)
            return fail(c, "unterminated string");
        ch = c->p[c->pos];

        if (ch == '"') {
            c->pos++;
            break;
        }

        /* Raw control characters are a syntax error in JSON, and Python's
         * json agrees unless strict=False. */
        if (ch < 0x20u)
            return fail(c, "control character in string");

        if (ch != '\\') {
            rc = scratch_reserve(c, w + 1u);
            if (rc != ND_OK)
                return rc;
            c->scratch[w++] = (char)ch;
            c->pos++;
            continue;
        }

        c->pos++;
        if (c->pos >= c->len)
            return fail(c, "unterminated escape");
        ch = c->p[c->pos++];

        rc = scratch_reserve(c, w + 4u);
        if (rc != ND_OK)
            return rc;

        switch (ch) {
        case '"':
            c->scratch[w++] = '"';
            break;
        case '\\':
            c->scratch[w++] = '\\';
            break;
        case '/':
            c->scratch[w++] = '/';
            break;
        case 'b':
            c->scratch[w++] = '\b';
            break;
        case 'f':
            c->scratch[w++] = '\f';
            break;
        case 'n':
            c->scratch[w++] = '\n';
            break;
        case 'r':
            c->scratch[w++] = '\r';
            break;
        case 't':
            c->scratch[w++] = '\t';
            break;
        case 'u': {
            int32_t hi = read_u16(c);
            uint32_t cp;

            if (hi < 0)
                return fail(c, "bad \\u escape");
            cp = (uint32_t)hi;

            if (cp >= 0xD800u && cp <= 0xDBFFu && c->len - c->pos >= 2u && c->p[c->pos] == '\\' &&
                c->p[c->pos + 1u] == 'u') {
                size_t save = c->pos;
                int32_t lo;

                c->pos += 2u;
                lo = read_u16(c);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + ((uint32_t)lo - 0xDC00u);
                else
                    c->pos = save;
            }

            /* A lone surrogate has no UTF-8 encoding. Python keeps it in the
             * str and only fails later, on encode; there is no "later" here,
             * so it becomes U+FFFD. Release notes are the only place this
             * could ever show up and a replacement character is the honest
             * rendering of an unpaired surrogate. */
            if (cp >= 0xD800u && cp <= 0xDFFFu)
                cp = 0xFFFDu;

            w += encode_utf8(cp, c->scratch + w);
            break;
        }
        default:
            return fail(c, "bad escape");
        }
    }

    rc = scratch_reserve(c, w + 1u);
    if (rc != ND_OK)
        return rc;

    *out = arena_strn(c->doc, c->scratch, w);
    return *out != NULL ? ND_OK : ND_ERR_NOMEM;
}

static nd_err parse_number(ctx *c, nd_json_val *v)
{
    size_t start = c->pos;
    bool is_real = false;
    char buf[64];
    size_t n;

    if (c->pos < c->len && c->p[c->pos] == '-')
        c->pos++;

    /* Leading zeros are illegal: "01" is not a number, and Python agrees. */
    if (c->pos < c->len && c->p[c->pos] == '0') {
        c->pos++;
    } else if (c->pos < c->len && c->p[c->pos] >= '1' && c->p[c->pos] <= '9') {
        while (c->pos < c->len && c->p[c->pos] >= '0' && c->p[c->pos] <= '9')
            c->pos++;
    } else {
        return fail(c, "expected a number");
    }

    if (c->pos < c->len && c->p[c->pos] == '.') {
        is_real = true;
        c->pos++;
        if (c->pos >= c->len || c->p[c->pos] < '0' || c->p[c->pos] > '9')
            return fail(c, "expected a digit after the decimal point");
        while (c->pos < c->len && c->p[c->pos] >= '0' && c->p[c->pos] <= '9')
            c->pos++;
    }

    if (c->pos < c->len && (c->p[c->pos] == 'e' || c->p[c->pos] == 'E')) {
        is_real = true;
        c->pos++;
        if (c->pos < c->len && (c->p[c->pos] == '+' || c->p[c->pos] == '-'))
            c->pos++;
        if (c->pos >= c->len || c->p[c->pos] < '0' || c->p[c->pos] > '9')
            return fail(c, "expected a digit in the exponent");
        while (c->pos < c->len && c->p[c->pos] >= '0' && c->p[c->pos] <= '9')
            c->pos++;
    }

    n = c->pos - start;
    if (n >= sizeof buf)
        return fail(c, "number too long");
    memcpy(buf, c->p + start, n);
    buf[n] = '\0';

    if (is_real) {
        v->type = ND_JSON_REAL;
        v->u.r = strtod(buf, NULL);
    } else {
        char *end = NULL;
        long long ll;

        errno = 0;
        ll = strtoll(buf, &end, 10);
        /* Refused rather than silently turned into a double: the whole point
         * of separating INT from REAL is that the update manifest can tell
         * them apart, and a saturated value would be a lie. */
        if (errno == ERANGE)
            return fail(c, "number out of range");
        v->type = ND_JSON_INT;
        v->u.i = (int64_t)ll;
    }
    return ND_OK;
}

static bool lit(ctx *c, const char *word)
{
    size_t n = strlen(word);

    if (c->len - c->pos < n || memcmp(c->p + c->pos, word, n) != 0)
        return false;
    c->pos += n;
    return true;
}

static nd_err level_reserve(level *l)
{
    size_t cap;
    nd_json_member *items;
    uint64_t *hashes;

    if (l->n < l->cap)
        return ND_OK;

    cap = l->cap == 0u ? 8u : l->cap * 2u;
    /* freed by free_levels() on every path out of parse_doc() */
    items = realloc(l->items, cap * sizeof *items);
    if (items == NULL)
        return ND_ERR_NOMEM;
    l->items = items;

    if (l->is_object) {
        hashes = realloc(l->hashes, cap * sizeof *hashes);
        if (hashes == NULL)
            return ND_ERR_NOMEM;
        l->hashes = hashes;
    }
    l->cap = cap;
    return ND_OK;
}

static nd_err level_append(ctx *c, level *l, const nd_json_val *v)
{
    nd_err rc;

    if (l->is_object) {
        uint64_t h = hash_str(l->pending_key);
        size_t i;

        /* Last duplicate wins, but the surviving entry keeps the FIRST
         * occurrence's position -- assigning to an existing dict key does not
         * move it. The hash makes the scan a single integer compare in all
         * but a genuine collision. */
        for (i = 0u; i < l->n; i++) {
            if (l->hashes[i] == h && strcmp(l->items[i].key, l->pending_key) == 0) {
                l->items[i].val = *v;
                return ND_OK;
            }
        }
        if (l->n >= ND_JSON_MAX_MEMBERS)
            return fail(c, "too many members in one object");
    }

    rc = level_reserve(l);
    if (rc != ND_OK)
        return rc;

    if (l->is_object)
        l->hashes[l->n] = hash_str(l->pending_key);
    l->items[l->n].key = l->is_object ? l->pending_key : NULL;
    l->items[l->n].val = *v;
    l->n++;
    return ND_OK;
}

/* Copies a finished level's children into the arena and produces the value
 * that stands for the container. */
static nd_err level_finish(ctx *c, level *l, nd_json_val *out)
{
    size_t i;

    if (l->is_object) {
        nd_json_member *m = NULL;

        if (l->n > 0u) {
            m = arena_alloc(c->doc, l->n * sizeof *m);
            if (m == NULL)
                return ND_ERR_NOMEM;
            memcpy(m, l->items, l->n * sizeof *m);
        }
        out->type = ND_JSON_OBJECT;
        out->u.obj.members = m;
        out->u.obj.n = l->n;
    } else {
        nd_json_val *a = NULL;

        if (l->n > 0u) {
            a = arena_alloc(c->doc, l->n * sizeof *a);
            if (a == NULL)
                return ND_ERR_NOMEM;
            for (i = 0u; i < l->n; i++)
                a[i] = l->items[i].val;
        }
        out->type = ND_JSON_ARRAY;
        out->u.arr.items = a;
        out->u.arr.n = l->n;
    }

    free(l->items);
    free(l->hashes);
    memset(l, 0, sizeof *l);
    return ND_OK;
}

static void free_levels(ctx *c)
{
    size_t i;

    for (i = 0u; i < ND_JSON_MAX_DEPTH; i++) {
        free(c->levels[i].items);
        free(c->levels[i].hashes);
        c->levels[i].items = NULL;
        c->levels[i].hashes = NULL;
    }
}

/* Reads a key and the colon after it, leaving the key on the top level. */
static nd_err parse_key(ctx *c)
{
    level *l = &c->levels[c->depth - 1u];
    nd_err rc;

    skip_ws(c);
    rc = parse_string(c, &l->pending_key);
    if (rc != ND_OK)
        return rc;
    skip_ws(c);
    if (c->pos >= c->len || c->p[c->pos] != ':')
        return fail(c, "expected ':'");
    c->pos++;
    return ND_OK;
}

static nd_err parse_doc(ctx *c, nd_json_val *root)
{
    nd_err rc;

    for (;;) {
        nd_json_val v;
        uint8_t ch;

        memset(&v, 0, sizeof v);

        if (++c->values > ND_JSON_MAX_VALUES)
            return fail(c, "too many values");

        skip_ws(c);
        if (c->pos >= c->len)
            return fail(c, "unexpected end of input");
        ch = c->p[c->pos];

        if (ch == '{' || ch == '[') {
            level *l;

            if (c->depth >= ND_JSON_MAX_DEPTH)
                return fail(c, "nested too deeply");
            c->pos++;

            l = &c->levels[c->depth];
            memset(l, 0, sizeof *l);
            l->is_object = (ch == '{');
            c->depth++;

            skip_ws(c);
            if (c->pos < c->len && c->p[c->pos] == (l->is_object ? '}' : ']')) {
                c->pos++;
                c->depth--;
                rc = level_finish(c, l, &v);
                if (rc != ND_OK)
                    return rc;
                /* fall through to the emit loop with the empty container */
            } else {
                if (l->is_object) {
                    rc = parse_key(c);
                    if (rc != ND_OK)
                        return rc;
                }
                continue;
            }
        } else if (ch == '"') {
            rc = parse_string(c, &v.u.s);
            if (rc != ND_OK)
                return rc;
            v.type = ND_JSON_STRING;
        } else if (lit(c, "true")) {
            v.type = ND_JSON_BOOL;
            v.u.b = true;
        } else if (lit(c, "false")) {
            v.type = ND_JSON_BOOL;
            v.u.b = false;
        } else if (lit(c, "null")) {
            v.type = ND_JSON_NULL;
        } else {
            rc = parse_number(c, &v);
            if (rc != ND_OK)
                return rc;
        }

        /* Emit: attach the value to its parent, then keep closing parents for
         * as long as the next character says so. */
        for (;;) {
            level *l;

            if (c->depth == 0u) {
                *root = v;
                skip_ws(c);
                if (c->pos != c->len)
                    return fail(c, "trailing data");
                return ND_OK;
            }

            l = &c->levels[c->depth - 1u];
            rc = level_append(c, l, &v);
            if (rc != ND_OK)
                return rc;

            skip_ws(c);
            if (c->pos >= c->len)
                return fail(c, "unexpected end of input");
            ch = c->p[c->pos++];

            if (ch == ',') {
                if (l->is_object) {
                    rc = parse_key(c);
                    if (rc != ND_OK)
                        return rc;
                }
                break;
            }
            if (ch == (l->is_object ? '}' : ']')) {
                c->depth--;
                rc = level_finish(c, l, &v);
                if (rc != ND_OK)
                    return rc;
                continue;
            }

            c->pos--;
            return fail(c, l->is_object ? "expected ',' or '}'" : "expected ',' or ']'");
        }
    }
}

nd_err nd_json_parse(const uint8_t *data, size_t len, nd_json_doc **out, char *err_out,
                     size_t err_sz)
{
    ctx c;
    nd_json_doc *doc = NULL;
    nd_json_val root;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;
    if (err_out != NULL && err_sz > 0u)
        err_out[0] = '\0';
    if (data == NULL && len > 0u)
        return ND_ERR_INVAL;

    if (len > ND_JSON_MAX_BYTES) {
        if (err_out != NULL && err_sz > 0u)
            (void)snprintf(err_out, err_sz, "file is too large");
        return ND_ERR_TOOLONG;
    }

    /* json.loads() on bytes decodes strict UTF-8 before it parses, so a
     * document that is not valid UTF-8 is rejected before the grammar is
     * ever consulted. Doing it here means the string parser can treat every
     * non-escape byte as already-good UTF-8 and just copy it. */
    if (!nd_utf8_valid(data, len)) {
        if (err_out != NULL && err_sz > 0u)
            (void)snprintf(err_out, err_sz, "not valid UTF-8 text");
        return ND_ERR_PARSE;
    }

    /* owned by the caller; free with nd_json_free() */
    doc = calloc(1u, sizeof *doc);
    if (doc == NULL)
        return ND_ERR_NOMEM;

    memset(&c, 0, sizeof c);
    memset(&root, 0, sizeof root);
    c.p = data;
    c.len = len;
    c.doc = doc;
    c.err = err_out;
    c.err_sz = err_sz;

    rc = parse_doc(&c, &root);

    free_levels(&c);
    free(c.scratch);

    if (rc != ND_OK) {
        nd_json_free(doc);
        return rc;
    }

    doc->root = arena_alloc(doc, sizeof *doc->root);
    if (doc->root == NULL) {
        nd_json_free(doc);
        return ND_ERR_NOMEM;
    }
    *doc->root = root;
    *out = doc;
    return ND_OK;
}

nd_err nd_json_parse_file(const char *path, nd_json_doc **out, char *err_out, size_t err_sz)
{
    nd_err rc = ND_OK;
    char resolved[ND_PATH_MAX];
    FILE *f = NULL;
    uint8_t *buf = NULL;
    struct stat st;
    size_t len = 0u;

    if (out == NULL || path == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    rc = nd_path_resolve(resolved, sizeof resolved, path);
    if (rc != ND_OK)
        return rc;

    f = fopen(resolved, "rb");
    if (f == NULL)
        return ND_ERR_IO;

    /* Size checked before anything is read, so an enormous file costs one
     * fstat rather than a megabyte of allocation. */
    if (fstat(fileno(f), &st) != 0) {
        rc = ND_ERR_IO;
        goto done;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)ND_JSON_MAX_BYTES) {
        if (err_out != NULL && err_sz > 0u)
            (void)snprintf(err_out, err_sz, "file is too large");
        rc = ND_ERR_TOOLONG;
        goto done;
    }

    len = (size_t)st.st_size;
    /* freed before returning; the parser copies what it keeps into the arena */
    buf = malloc(len + 1u);
    if (buf == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }
    if (len > 0u && fread(buf, 1u, len, f) != len) {
        rc = ND_ERR_IO;
        goto done;
    }
    buf[len] = '\0';

    rc = nd_json_parse(buf, len, out, err_out, err_sz);

done:
    free(buf);
    (void)fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

const nd_json_val *nd_json_root(const nd_json_doc *doc)
{
    return doc != NULL ? doc->root : NULL;
}

nd_json_type nd_json_type_of(const nd_json_val *v)
{
    return v != NULL ? v->type : ND_JSON_NULL;
}

const nd_json_val *nd_json_get(const nd_json_val *v, const char *key)
{
    size_t i;

    if (v == NULL || key == NULL || v->type != ND_JSON_OBJECT)
        return NULL;
    for (i = 0u; i < v->u.obj.n; i++) {
        if (strcmp(v->u.obj.members[i].key, key) == 0)
            return &v->u.obj.members[i].val;
    }
    return NULL;
}

size_t nd_json_len(const nd_json_val *v)
{
    if (v == NULL)
        return 0u;
    if (v->type == ND_JSON_ARRAY)
        return v->u.arr.n;
    if (v->type == ND_JSON_OBJECT)
        return v->u.obj.n;
    return 0u;
}

const nd_json_val *nd_json_at(const nd_json_val *v, size_t i)
{
    if (v == NULL || v->type != ND_JSON_ARRAY || i >= v->u.arr.n)
        return NULL;
    return &v->u.arr.items[i];
}

const char *nd_json_key_at(const nd_json_val *v, size_t i)
{
    if (v == NULL || v->type != ND_JSON_OBJECT || i >= v->u.obj.n)
        return NULL;
    return v->u.obj.members[i].key;
}

const nd_json_val *nd_json_member_at(const nd_json_val *v, size_t i)
{
    if (v == NULL || v->type != ND_JSON_OBJECT || i >= v->u.obj.n)
        return NULL;
    return &v->u.obj.members[i].val;
}

bool nd_json_bool(const nd_json_val *v, bool *out)
{
    if (v == NULL || v->type != ND_JSON_BOOL || out == NULL)
        return false;
    *out = v->u.b;
    return true;
}

bool nd_json_int(const nd_json_val *v, int64_t *out)
{
    /* Deliberately no coercion from ND_JSON_REAL. "buildtime": 1785160800.0
     * must be rejected by the update manifest check, and this is where that
     * happens. */
    if (v == NULL || v->type != ND_JSON_INT || out == NULL)
        return false;
    *out = v->u.i;
    return true;
}

bool nd_json_real(const nd_json_val *v, double *out)
{
    if (v == NULL || out == NULL)
        return false;
    if (v->type == ND_JSON_REAL) {
        *out = v->u.r;
        return true;
    }
    /* An int where a float is wanted is fine in the other direction: Python's
     * float(1) is 1.0 and no manifest check depends on refusing it. */
    if (v->type == ND_JSON_INT) {
        *out = (double)v->u.i;
        return true;
    }
    return false;
}

bool nd_json_str(const nd_json_val *v, const char **out)
{
    if (v == NULL || v->type != ND_JSON_STRING || out == NULL)
        return false;
    *out = v->u.s;
    return true;
}

bool nd_json_get_bool(const nd_json_val *obj, const char *key, bool dflt)
{
    bool v;

    return nd_json_bool(nd_json_get(obj, key), &v) ? v : dflt;
}

int64_t nd_json_get_int(const nd_json_val *obj, const char *key, int64_t dflt)
{
    int64_t v;

    return nd_json_int(nd_json_get(obj, key), &v) ? v : dflt;
}

const char *nd_json_get_str(const nd_json_val *obj, const char *key, const char *dflt)
{
    const char *v;

    return nd_json_str(nd_json_get(obj, key), &v) ? v : dflt;
}

/* ------------------------------------------------------------------ *
 * Writer
 * ------------------------------------------------------------------ */

struct nd_json_writer {
    char *buf;
    size_t len;
    size_t cap;
    int indent;
    bool failed;
    bool expect_key; /* inside an object, and the next put is a key    */
    bool have_key;   /* a key has been written; the value follows      */
    size_t depth;
    bool is_object[ND_JSON_MAX_DEPTH];
    size_t count[ND_JSON_MAX_DEPTH];
    bool done; /* the root value has been written                */
};

nd_json_writer *nd_json_writer_new(int indent)
{
    /* owned by the caller; free with nd_json_writer_free() */
    nd_json_writer *w = calloc(1u, sizeof *w);

    if (w == NULL)
        return NULL;
    w->indent = indent > 0 ? indent : 0;
    return w;
}

void nd_json_writer_free(nd_json_writer *w)
{
    if (w == NULL)
        return;
    free(w->buf);
    free(w);
}

static nd_err w_reserve(nd_json_writer *w, size_t n)
{
    size_t cap = w->cap;
    char *grown;

    if (w->len + n + 1u <= cap)
        return ND_OK;
    if (cap == 0u)
        cap = 512u;
    while (cap < w->len + n + 1u) {
        if (cap >= ND_JSON_MAX_BYTES) {
            w->failed = true;
            return ND_ERR_TOOLONG;
        }
        cap *= 2u;
    }

    grown = realloc(w->buf, cap);
    if (grown == NULL) {
        w->failed = true;
        return ND_ERR_NOMEM;
    }
    w->buf = grown;
    w->cap = cap;
    return ND_OK;
}

static nd_err w_raw(nd_json_writer *w, const char *s, size_t n)
{
    nd_err rc = w_reserve(w, n);

    if (rc != ND_OK)
        return rc;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
    return ND_OK;
}

static nd_err w_str(nd_json_writer *w, const char *s)
{
    return w_raw(w, s, strlen(s));
}

static nd_err w_newline_indent(nd_json_writer *w, size_t depth)
{
    size_t spaces = depth * (size_t)w->indent;
    nd_err rc;

    if (w->indent == 0)
        return ND_OK;
    rc = w_raw(w, "\n", 1u);
    while (rc == ND_OK && spaces > 0u) {
        size_t chunk = spaces > 16u ? 16u : spaces;

        rc = w_raw(w, "                ", chunk);
        spaces -= chunk;
    }
    return rc;
}

/* Emits the separator that must precede the next value or key. */
static nd_err w_before_item(nd_json_writer *w)
{
    nd_err rc = ND_OK;

    if (w->depth == 0u) {
        if (w->done) {
            w->failed = true;
            return ND_ERR_INVAL;
        }
        return ND_OK;
    }

    if (w->have_key)
        return ND_OK; /* the key already emitted its separator */

    if (w->count[w->depth - 1u] > 0u)
        rc = w_raw(w, ",", 1u);
    if (rc == ND_OK)
        rc = w_newline_indent(w, w->depth);
    return rc;
}

static void w_after_item(nd_json_writer *w)
{
    if (w->depth > 0u)
        w->count[w->depth - 1u]++;
    else
        w->done = true;
    w->have_key = false;
}

/* json.dumps defaults to ensure_ascii=True, so every non-ASCII character is
 * escaped. Matching that means output identical to what the Python wrote,
 * byte for byte, which is what makes a keymap.json diff meaningful. */
static nd_err w_quoted(nd_json_writer *w, const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    nd_err rc = w_raw(w, "\"", 1u);

    while (rc == ND_OK && *p != '\0') {
        uint8_t ch = *p;
        char esc[16];

        if (ch == '"') {
            rc = w_raw(w, "\\\"", 2u);
            p++;
        } else if (ch == '\\') {
            rc = w_raw(w, "\\\\", 2u);
            p++;
        } else if (ch == '\b') {
            rc = w_raw(w, "\\b", 2u);
            p++;
        } else if (ch == '\f') {
            rc = w_raw(w, "\\f", 2u);
            p++;
        } else if (ch == '\n') {
            rc = w_raw(w, "\\n", 2u);
            p++;
        } else if (ch == '\r') {
            rc = w_raw(w, "\\r", 2u);
            p++;
        } else if (ch == '\t') {
            rc = w_raw(w, "\\t", 2u);
            p++;
        } else if (ch < 0x20u) {
            (void)snprintf(esc, sizeof esc, "\\u%04x", ch);
            rc = w_raw(w, esc, 6u);
            p++;
        } else if (ch < 0x80u) {
            rc = w_raw(w, (const char *)p, 1u);
            p++;
        } else {
            /* Decode one UTF-8 sequence and re-emit it as \uXXXX, with a
             * surrogate pair beyond the BMP. Malformed input becomes U+FFFD
             * rather than propagating bytes we could not name. */
            uint32_t cp = 0xFFFDu;
            size_t n = 1u;

            if ((ch & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
                cp = ((uint32_t)(ch & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
                n = 2u;
            } else if ((ch & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u &&
                       (p[2] & 0xC0u) == 0x80u) {
                cp = ((uint32_t)(ch & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
                     (uint32_t)(p[2] & 0x3Fu);
                n = 3u;
            } else if ((ch & 0xF8u) == 0xF0u && (p[1] & 0xC0u) == 0x80u &&
                       (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
                cp = ((uint32_t)(ch & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
                     ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
                n = 4u;
            }

            if (cp >= 0x10000u) {
                uint32_t x = cp - 0x10000u;

                (void)snprintf(esc, sizeof esc, "\\u%04x\\u%04x", 0xD800u + (x >> 10),
                               0xDC00u + (x & 0x3FFu));
                rc = w_raw(w, esc, 12u);
            } else {
                (void)snprintf(esc, sizeof esc, "\\u%04x", cp);
                rc = w_raw(w, esc, 6u);
            }
            p += n;
        }
    }

    if (rc == ND_OK)
        rc = w_raw(w, "\"", 1u);
    return rc;
}

nd_err nd_json_begin_object(nd_json_writer *w)
{
    nd_err rc;

    if (w == NULL)
        return ND_ERR_INVAL;
    if (w->failed)
        return ND_ERR_INVAL;
    if (w->depth >= ND_JSON_MAX_DEPTH) {
        w->failed = true;
        return ND_ERR_TOOLONG;
    }

    rc = w_before_item(w);
    if (rc != ND_OK)
        return rc;
    rc = w_raw(w, "{", 1u);
    if (rc != ND_OK)
        return rc;

    w->have_key = false;
    w->is_object[w->depth] = true;
    w->count[w->depth] = 0u;
    w->depth++;
    return ND_OK;
}

nd_err nd_json_begin_array(nd_json_writer *w)
{
    nd_err rc;

    if (w == NULL || w->failed)
        return ND_ERR_INVAL;
    if (w->depth >= ND_JSON_MAX_DEPTH) {
        w->failed = true;
        return ND_ERR_TOOLONG;
    }

    rc = w_before_item(w);
    if (rc != ND_OK)
        return rc;
    rc = w_raw(w, "[", 1u);
    if (rc != ND_OK)
        return rc;

    w->have_key = false;
    w->is_object[w->depth] = false;
    w->count[w->depth] = 0u;
    w->depth++;
    return ND_OK;
}

static nd_err end_container(nd_json_writer *w, bool object)
{
    nd_err rc = ND_OK;

    if (w == NULL || w->failed)
        return ND_ERR_INVAL;
    if (w->depth == 0u || w->is_object[w->depth - 1u] != object || w->have_key) {
        w->failed = true;
        return ND_ERR_INVAL;
    }

    w->depth--;
    /* An empty container stays on one line, which is what json.dump does. */
    if (w->count[w->depth] > 0u)
        rc = w_newline_indent(w, w->depth);
    if (rc == ND_OK)
        rc = w_raw(w, object ? "}" : "]", 1u);
    if (rc == ND_OK)
        w_after_item(w);
    return rc;
}

nd_err nd_json_end_object(nd_json_writer *w)
{
    return end_container(w, true);
}

nd_err nd_json_end_array(nd_json_writer *w)
{
    return end_container(w, false);
}

nd_err nd_json_key(nd_json_writer *w, const char *key)
{
    nd_err rc;

    if (w == NULL || key == NULL || w->failed)
        return ND_ERR_INVAL;
    if (w->depth == 0u || !w->is_object[w->depth - 1u] || w->have_key) {
        w->failed = true;
        return ND_ERR_INVAL;
    }

    rc = w_before_item(w);
    if (rc != ND_OK)
        return rc;
    rc = w_quoted(w, key);
    if (rc != ND_OK)
        return rc;
    rc = w_str(w, w->indent > 0 ? ": " : ":");
    if (rc != ND_OK)
        return rc;

    w->have_key = true;
    return ND_OK;
}

static nd_err put_scalar(nd_json_writer *w, const char *text)
{
    nd_err rc;

    if (w == NULL || w->failed)
        return ND_ERR_INVAL;
    if (w->depth > 0u && w->is_object[w->depth - 1u] && !w->have_key) {
        w->failed = true;
        return ND_ERR_INVAL;
    }

    rc = w_before_item(w);
    if (rc != ND_OK)
        return rc;
    rc = w_str(w, text);
    if (rc == ND_OK)
        w_after_item(w);
    return rc;
}

nd_err nd_json_put_null(nd_json_writer *w)
{
    return put_scalar(w, "null");
}

nd_err nd_json_put_bool(nd_json_writer *w, bool v)
{
    return put_scalar(w, v ? "true" : "false");
}

nd_err nd_json_put_int(nd_json_writer *w, int64_t v)
{
    char buf[32];

    (void)snprintf(buf, sizeof buf, "%lld", (long long)v);
    return put_scalar(w, buf);
}

nd_err nd_json_put_real(nd_json_writer *w, double v)
{
    char buf[40];
    int prec;

    /* repr(float) in Python is the shortest string that round-trips. Trying
     * 15, 16 then 17 significant digits reproduces it for every value that
     * matters here, and 17 always round-trips for IEEE double. */
    if (isnan(v)) {
        return put_scalar(w, "NaN");
    }
    if (isinf(v)) {
        return put_scalar(w, v > 0.0 ? "Infinity" : "-Infinity");
    }

    for (prec = 15; prec <= 17; prec++) {
        (void)snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (strtod(buf, NULL) == v)
            break;
    }

    /* json.dump writes 1.0, not 1: a float stays visibly a float. */
    if (strpbrk(buf, ".eEnN") == NULL)
        (void)nd_strlcat(buf, ".0", sizeof buf);

    return put_scalar(w, buf);
}

nd_err nd_json_put_str(nd_json_writer *w, const char *v)
{
    nd_err rc;

    if (w == NULL || v == NULL || w->failed)
        return ND_ERR_INVAL;
    if (w->depth > 0u && w->is_object[w->depth - 1u] && !w->have_key) {
        w->failed = true;
        return ND_ERR_INVAL;
    }

    rc = w_before_item(w);
    if (rc != ND_OK)
        return rc;
    rc = w_quoted(w, v);
    if (rc == ND_OK)
        w_after_item(w);
    return rc;
}

const char *nd_json_writer_text(const nd_json_writer *w, size_t *len_out)
{
    if (w == NULL || w->failed || w->depth != 0u || !w->done)
        return NULL;
    if (len_out != NULL)
        *len_out = w->len;
    return w->buf != NULL ? w->buf : "";
}

nd_err nd_json_writer_save(const nd_json_writer *w, const char *path)
{
    nd_err rc = ND_OK;
    char tmp_virtual[ND_PATH_MAX];
    char tmp[ND_PATH_MAX];
    char final[ND_PATH_MAX];
    char dir[ND_PATH_MAX];
    const char *text;
    size_t len = 0u;
    const char *slash;
    int fd = -1;
    size_t off = 0u;

    if (w == NULL || path == NULL)
        return ND_ERR_INVAL;

    text = nd_json_writer_text(w, &len);
    if (text == NULL)
        return ND_ERR_INVAL;

    slash = strrchr(path, '/');
    if (slash != NULL && slash != path) {
        size_t dlen = (size_t)(slash - path);

        if (dlen >= sizeof dir)
            return ND_ERR_TOOLONG;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        rc = nd_mkdir_p(dir, 0755u);
        if (rc != ND_OK)
            return rc;
    }

    rc = nd_snprintf(tmp_virtual, sizeof tmp_virtual, "%s.tmp", path);
    if (rc != ND_OK)
        return rc;
    rc = nd_path_resolve(tmp, sizeof tmp, tmp_virtual);
    if (rc != ND_OK)
        return rc;
    rc = nd_path_resolve(final, sizeof final, path);
    if (rc != ND_OK)
        return rc;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return ND_ERR_IO;

    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = ND_ERR_IO;
            goto done;
        }
        off += (size_t)n;
    }

    if (fsync(fd) != 0) {
        rc = ND_ERR_IO;
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        rc = ND_ERR_IO;
        goto done;
    }
    fd = -1;

    if (rename(tmp, final) != 0)
        rc = ND_ERR_IO;

done:
    if (fd >= 0)
        (void)close(fd);
    return rc;
}
