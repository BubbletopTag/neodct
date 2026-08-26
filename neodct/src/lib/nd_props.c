/* nd_props.c -- "key=value" files in three dialects, and the atomic writer.
 *
 * The dialects are NOT unified. Each difference between them is pinned by an
 * existing pytest, so "they are basically the same" is a claim a machine can
 * disprove:
 *
 *   B-1  SettingsStorage._parse_settings   settings.prop, version.prop
 *   B-2  Storage._read_state               /run/neodct/sdcard.prop
 *   B-3  RemoteShell._read_props           state.prop, relay.conf
 *
 * See nd_props.h for what each one does differently and why.
 *
 * ============ STORAGE LAYOUT ============
 *
 * Keys and values live in one growable character pool; the index is an array
 * of two offsets per entry, kept sorted by key with strcmp. Two allocations
 * per map instead of two per entry, which matters because nd_settings builds
 * and destroys three of these maps on EVERY get_setting() call (see the
 * write-on-read quirk in nd_settings.h) and get_setting is called per modem
 * ring.
 *
 * Offsets rather than pointers because the pool is realloc'd as it grows; the
 * header already warns that a stored pointer is invalidated by the next set.
 *
 * Sorting is strcmp and never strcoll. Python's sorted() is code-point order,
 * and a locale must never be able to change the byte layout of a file the
 * phone writes.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nd_paths.h"
#include "nd_props.h"
#include "nd_utf8_priv.h"

/* A prop file is bounded input -- one can arrive on an SD card -- so the map
 * is bounded too. Nothing the project ships comes close: the biggest is the
 * merged settings map at roughly twenty-five keys. */
#define ND_PROPS_MAX_ENTRIES 256u

typedef struct {
    size_t key_off;
    size_t val_off;
} prop_entry;

struct nd_props {
    prop_entry *ents; /* sorted by key; owned by the nd_props   */
    size_t n;
    size_t cap;
    char *pool; /* NUL-separated strings; owned likewise  */
    size_t pool_len;
    size_t pool_cap;
};

/* ------------------------------------------------------------------ *
 * The map
 * ------------------------------------------------------------------ */

nd_props *nd_props_new(void)
{
    /* owned by the caller; free with nd_props_free() */
    nd_props *p = calloc(1u, sizeof *p);

    return p;
}

void nd_props_free(nd_props *p)
{
    if (p == NULL)
        return;
    free(p->ents);
    free(p->pool);
    free(p);
}

static const char *pool_str(const nd_props *p, size_t off)
{
    return p->pool + off;
}

/* Binary search. Returns true when the key is present; *pos always receives
 * the index the key does or would occupy. */
static bool find_key(const nd_props *p, const char *key, size_t *pos)
{
    size_t lo = 0u;
    size_t hi = p->n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(key, pool_str(p, p->ents[mid].key_off));

        if (cmp == 0) {
            *pos = mid;
            return true;
        }
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    *pos = lo;
    return false;
}

static nd_err pool_reserve(nd_props *p, size_t extra)
{
    size_t want = p->pool_len + extra;
    size_t cap = p->pool_cap;
    char *grown;

    if (want <= cap)
        return ND_OK;

    if (cap == 0u)
        cap = 512u;
    while (cap < want)
        cap *= 2u;

    grown = realloc(p->pool, cap);
    if (grown == NULL)
        return ND_ERR_NOMEM;

    p->pool = grown;
    p->pool_cap = cap;
    return ND_OK;
}

/* Copies s (with its NUL) onto the end of the pool and returns its offset. */
static nd_err pool_add(nd_props *p, const char *s, size_t *off_out)
{
    size_t n = strlen(s) + 1u;
    nd_err rc = pool_reserve(p, n);

    if (rc != ND_OK)
        return rc;

    memcpy(p->pool + p->pool_len, s, n);
    *off_out = p->pool_len;
    p->pool_len += n;
    return ND_OK;
}

static nd_err ents_reserve(nd_props *p)
{
    size_t cap;
    prop_entry *grown;

    if (p->n < p->cap)
        return ND_OK;

    cap = p->cap == 0u ? 16u : p->cap * 2u;
    if (cap > ND_PROPS_MAX_ENTRIES)
        cap = ND_PROPS_MAX_ENTRIES;
    if (p->n >= cap)
        return ND_ERR_TOOLONG;

    grown = realloc(p->ents, cap * sizeof *grown);
    if (grown == NULL)
        return ND_ERR_NOMEM;

    p->ents = grown;
    p->cap = cap;
    return ND_OK;
}

nd_err nd_props_set(nd_props *p, const char *key, const char *value)
{
    size_t pos;
    size_t key_off;
    size_t val_off;
    nd_err rc;

    if (p == NULL || key == NULL || value == NULL)
        return ND_ERR_INVAL;
    if (strlen(key) >= ND_PROP_KEY_MAX || strlen(value) >= ND_PROP_VALUE_MAX)
        return ND_ERR_TOOLONG;

    if (find_key(p, key, &pos)) {
        char *slot = p->pool + p->ents[pos].val_off;

        /* Reuse the slot when the new value is no longer than the old one.
         * Repeated set() of the same key is the common case (the settings
         * layering does it for every default) and this keeps the pool from
         * growing once per assignment. */
        if (strlen(value) <= strlen(slot)) {
            memcpy(slot, value, strlen(value) + 1u);
            return ND_OK;
        }
        rc = pool_add(p, value, &val_off);
        if (rc != ND_OK)
            return rc;
        p->ents[pos].val_off = val_off;
        return ND_OK;
    }

    rc = ents_reserve(p);
    if (rc != ND_OK)
        return rc;
    rc = pool_add(p, key, &key_off);
    if (rc != ND_OK)
        return rc;
    rc = pool_add(p, value, &val_off);
    if (rc != ND_OK)
        return rc;

    if (pos < p->n)
        memmove(&p->ents[pos + 1u], &p->ents[pos], (p->n - pos) * sizeof p->ents[0]);
    p->ents[pos].key_off = key_off;
    p->ents[pos].val_off = val_off;
    p->n++;
    return ND_OK;
}

const char *nd_props_get(const nd_props *p, const char *key, const char *dflt)
{
    size_t pos;

    if (p == NULL || key == NULL)
        return dflt;
    if (!find_key(p, key, &pos))
        return dflt;
    return pool_str(p, p->ents[pos].val_off);
}

bool nd_props_has(const nd_props *p, const char *key)
{
    size_t pos;

    if (p == NULL || key == NULL)
        return false;
    return find_key(p, key, &pos);
}

nd_err nd_props_remove(nd_props *p, const char *key)
{
    size_t pos;

    if (p == NULL || key == NULL)
        return ND_ERR_INVAL;
    if (!find_key(p, key, &pos))
        return ND_ERR_NOTFOUND;

    /* The strings stay in the pool. Removal happens once per save, on a map
     * that is thrown away immediately afterwards, so compaction would cost
     * more than the bytes it recovered. */
    if (pos + 1u < p->n)
        memmove(&p->ents[pos], &p->ents[pos + 1u], (p->n - pos - 1u) * sizeof p->ents[0]);
    p->n--;
    return ND_OK;
}

size_t nd_props_count(const nd_props *p)
{
    return p == NULL ? 0u : p->n;
}

const char *nd_props_key_at(const nd_props *p, size_t i)
{
    if (p == NULL || i >= p->n)
        return NULL;
    return pool_str(p, p->ents[i].key_off);
}

const char *nd_props_value_at(const nd_props *p, size_t i)
{
    if (p == NULL || i >= p->n)
        return NULL;
    return pool_str(p, p->ents[i].val_off);
}

nd_err nd_props_update(nd_props *dst, const nd_props *src)
{
    size_t i;

    if (dst == NULL)
        return ND_ERR_INVAL;
    if (src == NULL)
        return ND_OK;

    for (i = 0u; i < src->n; i++) {
        nd_err rc = nd_props_set(dst, pool_str(src, src->ents[i].key_off),
                                 pool_str(src, src->ents[i].val_off));

        if (rc != ND_OK)
            return rc;
    }
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Reading files
 * ------------------------------------------------------------------ */

/* Python's str.strip() with no argument removes Unicode whitespace. Every key
 * and value this project stores is ASCII, so the ASCII set is equivalent --
 * including 0x1C..0x1F, which Python does treat as whitespace and a naive
 * isspace() list would miss. U+0085 and U+00A0 are deliberately not handled:
 * they are multi-byte in UTF-8 and no prop file in the project contains one.
 */
static bool is_py_space(char c)
{
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case '\x1c':
    case '\x1d':
    case '\x1e':
    case '\x1f':
        return true;
    default:
        return false;
    }
}

/* Trims in place by moving the start forward and writing a NUL at the end. */
static char *strip_inplace(char *s, size_t len)
{
    size_t start = 0u;

    while (start < len && is_py_space(s[start]))
        start++;
    while (len > start && is_py_space(s[len - 1u]))
        len--;
    s[len] = '\0';
    return s + start;
}

/* One line of dialects B-1 and B-2: strip first, then judge. Mutates the
 * line buffer, which is a scratch copy in every caller. */
static nd_err feed_line_stripped(nd_props *p, char *line, size_t len)
{
    char *s = strip_inplace(line, len);
    char *eq;

    if (s[0] == '\0' || s[0] == '#')
        return ND_OK;

    eq = strchr(s, '=');
    if (eq == NULL)
        return ND_OK;

    *eq = '\0';
    {
        char *key = strip_inplace(s, strlen(s));
        char *val = strip_inplace(eq + 1, strlen(eq + 1));
        nd_err rc = nd_props_set(p, key, val);

        /* A key or value longer than the maxima, or a map already at its
         * entry ceiling, is dropped rather than failing the whole file --
         * Python has no such limits and would have kept the other lines. */
        if (rc == ND_ERR_NOMEM)
            return rc;
    }
    return ND_OK;
}

/* One line of dialect B-3: the line is NOT stripped before the '=' and '#'
 * tests, so " #host=x" is a setting and "#host=x" is a comment. That is a
 * real difference from B-1/B-2 and RemoteShell's relay.conf depends on it. */
static nd_err feed_line_raw(nd_props *p, char *line, size_t len)
{
    char *eq;

    line[len] = '\0';
    eq = strchr(line, '=');
    if (eq == NULL)
        return ND_OK;
    if (line[0] == '#')
        return ND_OK;

    *eq = '\0';
    {
        char *key = strip_inplace(line, strlen(line));
        char *val = strip_inplace(eq + 1, strlen(eq + 1));
        nd_err rc = nd_props_set(p, key, val);

        if (rc == ND_ERR_NOMEM)
            return rc;
    }
    return ND_OK;
}

typedef nd_err (*line_fn)(nd_props *, char *, size_t);

/* Python's str.splitlines() splits on \n, \r and \r\n (and on several Unicode
 * separators that no file here contains). A trailing separator does NOT
 * produce a final empty line, which is why "a=1\n" yields one line. */
static nd_err feed_text(nd_props *p, char *text, size_t len, line_fn fn)
{
    size_t i = 0u;

    while (i < len) {
        size_t start = i;
        size_t end;

        while (i < len && text[i] != '\n' && text[i] != '\r')
            i++;
        end = i;

        if (i < len) {
            if (text[i] == '\r' && i + 1u < len && text[i + 1u] == '\n')
                i += 2u;
            else
                i++;
        }

        {
            /* fn writes a NUL at text[end]; that byte is a separator we have
             * already consumed, or the buffer's own terminator. */
            nd_err rc = fn(p, text + start, end - start);

            if (rc != ND_OK)
                return rc;
        }
    }
    return ND_OK;
}

/* Reads the whole file, NUL-terminated, honouring ND_ROOT. *len_out excludes
 * the terminator. ND_ERR_TOOLONG past ND_PROPS_MAX_BYTES. */
static nd_err read_whole(const char *path, uint8_t **buf_out, size_t *len_out)
{
    nd_err rc = ND_OK;
    char resolved[ND_PATH_MAX];
    FILE *f = NULL;
    uint8_t *buf = NULL;
    size_t len = 0u;
    size_t cap = 4096u;

    rc = nd_path_resolve(resolved, sizeof resolved, path);
    if (rc != ND_OK)
        goto done;

    f = fopen(resolved, "rb");
    if (f == NULL) {
        rc = ND_ERR_IO;
        goto done;
    }

    /* owned by the caller on success; freed here on every failure path.
     * cap + 1 throughout so the NUL terminator always has somewhere to go. */
    buf = malloc(cap + 1u);
    if (buf == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }

    for (;;) {
        size_t got;

        if (len == cap) {
            uint8_t *grown;

            if (cap >= ND_PROPS_MAX_BYTES) {
                rc = ND_ERR_TOOLONG;
                goto done;
            }
            cap = cap * 2u > ND_PROPS_MAX_BYTES ? ND_PROPS_MAX_BYTES : cap * 2u;
            grown = realloc(buf, cap + 1u);
            if (grown == NULL) {
                rc = ND_ERR_NOMEM;
                goto done;
            }
            buf = grown;
        }
        got = fread(buf + len, 1u, cap - len, f);
        len += got;
        if (got == 0u) {
            if (ferror(f)) {
                rc = ND_ERR_IO;
                goto done;
            }
            break;
        }
    }

    buf[len] = '\0';
    *buf_out = buf;
    *len_out = len;
    buf = NULL;

done:
    free(buf);
    if (f != NULL)
        (void)fclose(f);
    return rc;
}

nd_props *nd_props_parse_settings(const char *path)
{
    nd_props *p = nd_props_new();
    uint8_t *buf = NULL;
    size_t len = 0u;

    if (p == NULL)
        return NULL;
    if (path == NULL)
        return p;

    /* Any failure -- missing, unreadable, oversized -- yields the empty map,
     * exactly as the Python's bare `except Exception: return {}` does. */
    if (read_whole(path, &buf, &len) != ND_OK)
        return p;

    /* STRICT UTF-8 over the WHOLE file before a single line is looked at.
     * test_settings_version_layering.py writes b"\x00\xff not a prop file"
     * into version.prop and requires the result to be empty; a decoder that
     * validated line by line would keep the good lines and fail that. */
    if (!nd_utf8_valid(buf, len)) {
        free(buf);
        return p;
    }

    (void)feed_text(p, (char *)buf, len, feed_line_stripped);
    free(buf);
    return p;
}

nd_props *nd_props_parse_lenient(const char *path)
{
    nd_props *p = nd_props_new();
    uint8_t *buf = NULL;
    char *text = NULL;
    size_t len = 0u;
    size_t tlen;

    if (p == NULL)
        return NULL;
    if (path == NULL)
        return p;

    if (read_whole(path, &buf, &len) != ND_OK)
        return p;

    /* errors="replace": each maximal invalid subpart becomes one U+FFFD, so a
     * corrupt file still yields whatever lines parsed. Worst case is 3x, so
     * this buffer can never be too small. */
    text = malloc(3u * len + 1u);
    if (text == NULL) {
        free(buf);
        return p;
    }

    tlen = nd_utf8_replace(buf, len, text, 3u * len + 1u);
    free(buf);
    if (tlen != (size_t)-1)
        (void)feed_text(p, text, tlen, feed_line_stripped);
    free(text);
    return p;
}

nd_err nd_props_parse_raw(const char *path, nd_props **out)
{
    nd_err rc = ND_OK;
    nd_props *p = NULL;
    uint8_t *buf = NULL;
    size_t len = 0u;

    if (out == NULL || path == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    p = nd_props_new();
    if (p == NULL)
        return ND_ERR_NOMEM;

    /* Only an I/O error is swallowed -- the Python catches OSError and
     * nothing else, so a missing relay.conf is an empty map. */
    if (read_whole(path, &buf, &len) != ND_OK) {
        *out = p;
        return ND_OK;
    }

    /* ...but a decode error propagates, because in the Python it escapes
     * settings() and is caught only by launcher.py's blanket handler. */
    if (!nd_utf8_valid(buf, len)) {
        rc = ND_ERR_PARSE;
        goto done;
    }

    rc = feed_text(p, (char *)buf, len, feed_line_raw);

done:
    free(buf);
    if (rc != ND_OK) {
        nd_props_free(p);
        return rc;
    }
    *out = p;
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Writing
 * ------------------------------------------------------------------ */

/* Both Python writers create the parent directory, write "<path>.tmp", fsync
 * the FILE, and rename. NEITHER fsyncs the parent directory; only CrashHandler
 * does, and adding it here would be a behaviour change on a NAND device that
 * already writes this file more often than it should. */
nd_err nd_props_write_atomic(const char *path, const nd_props *p, bool trailing_nl_when_empty)
{
    nd_err rc = ND_OK;
    char dir[ND_PATH_MAX];
    char tmp_virtual[ND_PATH_MAX];
    char tmp[ND_PATH_MAX];
    char final[ND_PATH_MAX];
    char *text = NULL;
    size_t text_len = 0u;
    size_t need = 1u;
    size_t i;
    int fd = -1;
    const char *slash;

    if (path == NULL || p == NULL)
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

    for (i = 0u; i < p->n; i++)
        need += strlen(nd_props_key_at(p, i)) + strlen(nd_props_value_at(p, i)) + 2u;

    /* freed at `done`; one buffer for the whole file so the write is a single
     * syscall and cannot be torn by a signal half way down */
    text = malloc(need);
    if (text == NULL)
        return ND_ERR_NOMEM;

    for (i = 0u; i < p->n; i++) {
        int n = snprintf(text + text_len, need - text_len, "%s=%s\n", nd_props_key_at(p, i),
                         nd_props_value_at(p, i));

        if (n < 0 || (size_t)n >= need - text_len) {
            rc = ND_ERR_TOOLONG;
            goto done;
        }
        text_len += (size_t)n;
    }

    /* The one place the two writers disagree: "\n".join(lines) + "\n" writes a
     * single newline for an empty map, while a per-line loop writes nothing
     * at all. For a non-empty map they produce identical bytes. */
    if (p->n == 0u && trailing_nl_when_empty) {
        text[0] = '\n';
        text_len = 1u;
    }

    /* 0666 is what CPython's open(..., "w") requests; the process umask does
     * the rest, and the result on the phone is 0644. */
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        rc = ND_ERR_IO;
        goto done;
    }

    {
        size_t off = 0u;

        while (off < text_len) {
            ssize_t w = write(fd, text + off, text_len - off);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = ND_ERR_IO;
                goto done;
            }
            off += (size_t)w;
        }
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

    if (rename(tmp, final) != 0) {
        rc = ND_ERR_IO;
        goto done;
    }

done:
    if (fd >= 0)
        (void)close(fd);
    free(text);
    /* A failed write leaves "<path>.tmp" behind, as the Python does. The
     * next successful write truncates it. */
    return rc;
}
