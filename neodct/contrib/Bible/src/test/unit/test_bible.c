/* test_bible.c -- the Bible app: the pack reader, the paraphraser, reference
 * parsing, and one run of the reader against a framebuffer.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The Gen Z table is ordered longest-`from`-first. genz.c matches
 *     first-hit-wins at each word boundary, so an entry that follows a
 *     shorter prefix of itself can never fire. The failure is silent -- one
 *     phrase quietly stops appearing -- which is exactly the kind of thing a
 *     test has to hold rather than a reviewer.
 *
 *  2. nd_genz() substitutes whole words only, carries the source's
 *     capitalisation, treats an apostrophe as a word end so "man's" becomes
 *     "bro's", leaves a table word embedded in a longer word alone, and
 *     reports truncation the way snprintf does.
 *
 *  3. The output depends only on the text and the seed. This is what lets a
 *     verse scroll off the top of the reader and come back reading the same;
 *     see the note in genz.c.
 *
 *  4. The pack reader round-trips a pack built HERE, in C, with and without a
 *     preset zlib dictionary -- the second is the Z_NEED_DICT path, which is
 *     the one the shipped pack actually takes.
 *
 *  5. Five corrupt packs are refused by nd_bible_open() rather than by
 *     something further in: bad magic, a bad version, a truncated file, a
 *     chapter running off the end of the blob, and a dictionary flag with no
 *     dictionary.
 *
 *  6. bible_parse_ref() handles "John 3:16", "1 Cor 13", a bare book name,
 *     no space before the numbers, and rejects a name that is not a book.
 *
 *  7. bible_read() draws. Driven with scripted keys against the same nd_ui
 *     fixture the other app tests use, it puts ink on the canvas and returns.
 *
 * When a real pack is reachable -- NEODCT_BIBLE_PACK, or
 * neodct/overlay/.../apps/Bible/web.ndb once the app is installed into the
 * image -- it is opened too and known verses are checked. The pack is 1.7 MB
 * of generated data and is not in git, so absent it that block SKIPS and says
 * where it looked. Build one with:
 *
 *     tools/mkbible.py eng-web_vpl.txt web.ndb --name WEB --strict
 *
 * Runs with no arguments.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "smallapp_test.h"

#include "../../apps/Bible/bibleapp.h"

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

static struct {
    nd_err (*open)(nd_bible **, const char *);
    void (*close)(nd_bible *);
    const char *(*translation)(const nd_bible *);
    size_t (*book_count)(const nd_bible *);
    size_t (*total_chapters)(const nd_bible *);
    size_t (*max_raw)(const nd_bible *);
    const char *(*book_name)(const nd_bible *, size_t);
    const char *(*book_abbr)(const nd_bible *, size_t);
    unsigned (*book_section)(const nd_bible *, size_t);
    size_t (*chapter_count)(const nd_bible *, size_t);
    int32_t (*find_book)(const nd_bible *, const char *);
    nd_err (*load)(nd_bible *, size_t, size_t);
    size_t (*verse_count)(const nd_bible *);
    const char *(*verse)(const nd_bible *, size_t);

    size_t (*genz)(char *, size_t, const char *, uint32_t);
    const nd_genz_pair *(*genz_table)(size_t *);
    uint32_t (*genz_seed)(size_t, size_t, size_t);

    bool (*parse_ref)(const nd_bible *, const char *, size_t *, size_t *, size_t *);
    void (*format_ref)(const nd_bible *, size_t, size_t, size_t, char *, size_t);
    void (*read)(bible_app *, size_t, size_t, size_t);
} api;

static bool api_open(void *h)
{
    *(void **)&api.open = sa_sym(h, "nd_bible_open");
    *(void **)&api.close = sa_sym(h, "nd_bible_close");
    *(void **)&api.translation = sa_sym(h, "nd_bible_translation");
    *(void **)&api.book_count = sa_sym(h, "nd_bible_book_count");
    *(void **)&api.total_chapters = sa_sym(h, "nd_bible_total_chapters");
    *(void **)&api.max_raw = sa_sym(h, "nd_bible_max_raw");
    *(void **)&api.book_name = sa_sym(h, "nd_bible_book_name");
    *(void **)&api.book_abbr = sa_sym(h, "nd_bible_book_abbr");
    *(void **)&api.book_section = sa_sym(h, "nd_bible_book_section");
    *(void **)&api.chapter_count = sa_sym(h, "nd_bible_chapter_count");
    *(void **)&api.find_book = sa_sym(h, "nd_bible_find_book");
    *(void **)&api.load = sa_sym(h, "nd_bible_load");
    *(void **)&api.verse_count = sa_sym(h, "nd_bible_verse_count");
    *(void **)&api.verse = sa_sym(h, "nd_bible_verse");
    *(void **)&api.genz = sa_sym(h, "nd_genz");
    *(void **)&api.genz_table = sa_sym(h, "nd_genz_table");
    *(void **)&api.genz_seed = sa_sym(h, "nd_genz_seed");
    *(void **)&api.parse_ref = sa_sym(h, "bible_parse_ref");
    *(void **)&api.format_ref = sa_sym(h, "bible_format_ref");
    *(void **)&api.read = sa_sym(h, "bible_read");

    return api.open != NULL && api.close != NULL && api.translation != NULL &&
           api.book_count != NULL && api.total_chapters != NULL && api.max_raw != NULL &&
           api.book_name != NULL && api.book_abbr != NULL && api.book_section != NULL &&
           api.chapter_count != NULL && api.find_book != NULL && api.load != NULL &&
           api.verse_count != NULL && api.verse != NULL && api.genz != NULL &&
           api.genz_table != NULL && api.genz_seed != NULL && api.parse_ref != NULL &&
           api.format_ref != NULL && api.read != NULL;
}

/* ------------------------------------------------------------------ *
 * Building a pack, in C, so the test needs no Python and no fixture file
 * ------------------------------------------------------------------ */

#define TP_BOOKS 2

typedef struct {
    const char *abbr;
    const char *name;
    unsigned section;
    const char *chapters[3]; /* NUL-terminated list; verses split on '\n' */
} tp_book;

static const tp_book TP[TP_BOOKS] = {
    {"GEN", "Genesis", ND_NDB_OT,
     {"In the beginning.\nAnd the man said to him, Behold.", "Alpha\nBeta\nGamma", NULL}},
    {"JOH", "John", ND_NDB_NT, {"The Word.", NULL, NULL}},
};

static void wr16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static size_t tp_count_verses(const char *raw)
{
    size_t n = 1u;
    const char *p;

    if (raw[0] == '\0')
        return 0u;
    for (p = raw; *p != '\0'; p++) {
        if (*p == '\n')
            n++;
    }
    return n;
}

/* Deflates `raw`, priming with `zdict` when one is given. Returns the length
 * written, or 0 on failure. */
static size_t tp_deflate(uint8_t *out, size_t out_sz, const char *raw, const uint8_t *zdict,
                         size_t zdict_len)
{
    z_stream z;
    size_t len;

    memset(&z, 0, sizeof z);
    if (deflateInit(&z, 9) != Z_OK)
        return 0u;
    if (zdict != NULL && deflateSetDictionary(&z, zdict, (uInt)zdict_len) != Z_OK) {
        (void)deflateEnd(&z);
        return 0u;
    }
    z.next_in = (Bytef *)(uintptr_t)raw;
    z.avail_in = (uInt)strlen(raw);
    z.next_out = out;
    z.avail_out = (uInt)out_sz;
    if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
        (void)deflateEnd(&z);
        return 0u;
    }
    len = z.total_out;
    (void)deflateEnd(&z);
    return len;
}

/* Writes a whole pack to `path`. with_dict selects the Z_NEED_DICT path.
 * Returns the file length, or 0. */
static size_t tp_write(const char *path, bool with_dict)
{
    static const uint8_t ZDICT[] = "In the beginning. And the man said to him, Behold. Alpha";
    uint8_t hdr[ND_NDB_HDR_SZ];
    uint8_t books[TP_BOOKS * ND_NDB_BOOK_SZ];
    uint8_t chaps[8 * ND_NDB_CHAP_SZ];
    uint8_t blob[4096];
    char pool[128];
    size_t pool_len = 0u;
    size_t n_chapters = 0u;
    size_t blob_len = 0u;
    size_t max_raw = 0u;
    size_t zdict_len = with_dict ? (sizeof ZDICT - 1u) : 0u;
    uint32_t book_off = ND_NDB_HDR_SZ;
    uint32_t chap_off;
    uint32_t pool_off;
    uint32_t dict_off;
    uint32_t blob_off;
    size_t i;
    FILE *f;

    for (i = 0u; i < TP_BOOKS; i++) {
        size_t first = n_chapters;
        size_t name_off = pool_len;
        size_t c;

        (void)nd_strlcpy(pool + pool_len, TP[i].name, sizeof pool - pool_len);
        pool_len += strlen(TP[i].name) + 1u;

        for (c = 0u; c < 3u && TP[i].chapters[c] != NULL; c++) {
            const char *raw = TP[i].chapters[c];
            size_t rlen = strlen(raw);
            size_t clen = tp_deflate(blob + blob_len, sizeof blob - blob_len, raw,
                                     with_dict ? ZDICT : NULL, zdict_len);

            if (clen == 0u)
                return 0u;
            wr32(chaps + n_chapters * ND_NDB_CHAP_SZ + 0u, (uint32_t)blob_len);
            wr32(chaps + n_chapters * ND_NDB_CHAP_SZ + 4u, (uint32_t)clen);
            wr16(chaps + n_chapters * ND_NDB_CHAP_SZ + 8u, (unsigned)rlen);
            wr16(chaps + n_chapters * ND_NDB_CHAP_SZ + 10u, (unsigned)tp_count_verses(raw));
            blob_len += clen;
            if (rlen > max_raw)
                max_raw = rlen;
            n_chapters++;
        }

        memset(books + i * ND_NDB_BOOK_SZ, 0, ND_NDB_BOOK_SZ);
        memcpy(books + i * ND_NDB_BOOK_SZ, TP[i].abbr, strlen(TP[i].abbr));
        wr16(books + i * ND_NDB_BOOK_SZ + 4u, (unsigned)name_off);
        wr16(books + i * ND_NDB_BOOK_SZ + 6u, (unsigned)(n_chapters - first));
        wr32(books + i * ND_NDB_BOOK_SZ + 8u, (uint32_t)first);
        books[i * ND_NDB_BOOK_SZ + 12u] = (uint8_t)TP[i].section;
    }

    chap_off = book_off + (uint32_t)(TP_BOOKS * ND_NDB_BOOK_SZ);
    pool_off = chap_off + (uint32_t)(n_chapters * ND_NDB_CHAP_SZ);
    dict_off = pool_off + (uint32_t)pool_len;
    blob_off = dict_off + (uint32_t)zdict_len;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, ND_NDB_MAGIC, ND_NDB_MAGIC_N);
    wr16(hdr + 8u, ND_NDB_VERSION);
    wr16(hdr + 10u, ND_NDB_F_DEFLATE | (with_dict ? ND_NDB_F_DICT : 0u));
    wr16(hdr + 12u, TP_BOOKS);
    wr32(hdr + 16u, (uint32_t)n_chapters);
    wr32(hdr + 20u, book_off);
    wr32(hdr + 24u, chap_off);
    wr32(hdr + 28u, pool_off);
    wr32(hdr + 32u, (uint32_t)pool_len);
    wr32(hdr + 36u, blob_off);
    wr32(hdr + 40u, (uint32_t)blob_len);
    wr32(hdr + 44u, (uint32_t)max_raw);
    wr32(hdr + 48u, dict_off);
    wr32(hdr + 52u, (uint32_t)zdict_len);
    (void)nd_strlcpy((char *)hdr + 56u, "TEST", ND_NDB_NAME_MAX);

    f = fopen(path, "wb");
    if (f == NULL)
        return 0u;
    (void)fwrite(hdr, 1u, sizeof hdr, f);
    (void)fwrite(books, 1u, TP_BOOKS * ND_NDB_BOOK_SZ, f);
    (void)fwrite(chaps, 1u, n_chapters * ND_NDB_CHAP_SZ, f);
    (void)fwrite(pool, 1u, pool_len, f);
    if (zdict_len != 0u)
        (void)fwrite(ZDICT, 1u, zdict_len, f);
    (void)fwrite(blob, 1u, blob_len, f);
    (void)fclose(f);
    return (size_t)blob_off + blob_len;
}

static char tp_path[ND_PATH_MAX];
static char tp_dict_path[ND_PATH_MAX];

/* ------------------------------------------------------------------ *
 * 1 + 2 + 3. Gen Z
 * ------------------------------------------------------------------ */

static void test_genz_table_order(void)
{
    size_t n = 0u;
    const nd_genz_pair *tab = api.genz_table(&n);
    size_t i;
    bool ordered = true;

    CHECK(n > 20u, "the table has entries");
    for (i = 1u; i < n; i++) {
        if (strlen(tab[i].from) > strlen(tab[i - 1u].from)) {
            ordered = false;
            fprintf(stderr, "  \"%s\" follows the shorter \"%s\" and can never match\n",
                    tab[i].from, tab[i - 1u].from);
        }
    }
    CHECK(ordered, "the table is ordered longest-first");

    /* Nothing in the table may be empty: an empty `from` would match at every
     * boundary and the loop would not advance. */
    for (i = 0u; i < n; i++) {
        if (tab[i].from[0] == '\0') {
            CHECK(false, "an entry has an empty `from`");
            break;
        }
    }
}

static void test_genz_substitution(void)
{
    char out[512];

    (void)api.genz(out, sizeof out, "the man said to him, Behold.", 0u);
    CHECK_STR(out, "the bro hit up bro like, Yo peep this.", "phrase beats word, caps carried");

    (void)api.genz(out, sizeof out, "the man's house", 0u);
    CHECK_STR(out, "the bro's house", "an apostrophe ends a word");

    /* "man" inside "manner" and "human" must not fire. */
    (void)api.genz(out, sizeof out, "in a manner unlike any human", 0u);
    CHECK_STR(out, "in a manner unlike any human", "no substitution inside a longer word");

    (void)api.genz(out, sizeof out, "Great and very good", 0u);
    CHECK_STR(out, "Massive and hella bussin", "leading capital is carried, later words are not");

    /* Seed 0 selects PREFIX[0] and SUFFIX[0], both empty, which is what makes
     * every expectation above a bare substitution. Any other seed adds
     * decoration, so check one does. */
    (void)api.genz(out, sizeof out, "Alpha", 1u);
    CHECK(strcmp(out, "Alpha") != 0, "a non-zero seed decorates");
}

static void test_genz_truncation(void)
{
    char small[8];
    size_t want;

    want = api.genz(small, sizeof small, "the wicked man", 0u);
    CHECK(want >= sizeof small, "truncation is reported snprintf-style");
    CHECK_INT(strlen(small), sizeof small - 1u, "the buffer is filled and terminated");
    CHECK_INT(small[sizeof small - 1u], 0, "and NUL-terminated");

    /* An empty verse stays empty rather than becoming a bare interjection. */
    (void)api.genz(small, sizeof small, "", 12345u);
    CHECK_STR(small, "", "an empty verse produces nothing");
}

static void test_genz_deterministic(void)
{
    char a[256];
    char b[256];
    uint32_t s1 = api.genz_seed(43u, 3u, 16u);
    uint32_t s2 = api.genz_seed(43u, 3u, 17u);

    CHECK(api.genz_seed(43u, 3u, 16u) == s1, "the seed is a pure function");
    CHECK(s1 != s2, "adjacent verses get different seeds");

    (void)api.genz(a, sizeof a, "the king said to them, Behold the land.", s1);
    (void)api.genz(b, sizeof b, "the king said to them, Behold the land.", s1);
    CHECK_STR(a, b, "the same seed gives the same words");
}

/* ------------------------------------------------------------------ *
 * 4. The reader
 * ------------------------------------------------------------------ */

static void check_pack_contents(nd_bible *bib, const char *what)
{
    CHECK_STR(api.translation(bib), "TEST", what);
    CHECK_INT(api.book_count(bib), 2, "two books");
    CHECK_INT(api.total_chapters(bib), 3, "three chapters");
    CHECK_INT(api.max_raw(bib), strlen("In the beginning.\nAnd the man said to him, Behold."),
              "max_raw is the longest chapter");

    CHECK_STR(api.book_name(bib, 0u), "Genesis", "book 0 name");
    CHECK_STR(api.book_abbr(bib, 0u), "GEN", "book 0 abbreviation");
    CHECK_INT(api.book_section(bib, 0u), ND_NDB_OT, "book 0 is Old Testament");
    CHECK_INT(api.chapter_count(bib, 0u), 2, "Genesis has two chapters");
    CHECK_STR(api.book_name(bib, 1u), "John", "book 1 name");
    CHECK_INT(api.book_section(bib, 1u), ND_NDB_NT, "book 1 is New Testament");

    /* Out of range answers "" rather than NULL, so a caller may draw it. */
    CHECK_STR(api.book_name(bib, 99u), "", "an out-of-range book name is empty");
    CHECK_INT(api.chapter_count(bib, 99u), 0, "an out-of-range chapter count is zero");

    CHECK_INT(api.find_book(bib, "GEN"), 0, "found by abbreviation");
    CHECK_INT(api.find_book(bib, "gen"), 0, "abbreviation is case-insensitive");
    CHECK_INT(api.find_book(bib, "Joh"), 1, "found by a prefix of the name");
    CHECK_INT(api.find_book(bib, "John"), 1, "found by the whole name");
    CHECK_INT(api.find_book(bib, "Habakkuk"), -1, "a book that is not there is -1");
    CHECK_INT(api.find_book(bib, ""), -1, "an empty needle is -1");

    CHECK_INT(api.load(bib, 0u, 0u), ND_OK, "Genesis 1 loads");
    CHECK_INT(api.verse_count(bib), 2, "Genesis 1 has two verses");
    CHECK_STR(api.verse(bib, 1u), "In the beginning.", "verse 1");
    CHECK_STR(api.verse(bib, 2u), "And the man said to him, Behold.", "verse 2");
    CHECK_STR(api.verse(bib, 0u), "", "verse 0 does not exist; numbering is 1-based");
    CHECK_STR(api.verse(bib, 3u), "", "one past the end is empty");

    CHECK_INT(api.load(bib, 0u, 1u), ND_OK, "Genesis 2 loads");
    CHECK_INT(api.verse_count(bib), 3, "Genesis 2 has three verses");
    CHECK_STR(api.verse(bib, 3u), "Gamma", "the last verse of a chapter");

    /* Reloading the chapter already in the buffer must not disturb it. */
    CHECK_INT(api.load(bib, 0u, 1u), ND_OK, "reloading the current chapter is fine");
    CHECK_STR(api.verse(bib, 1u), "Alpha", "and leaves it intact");

    CHECK_INT(api.load(bib, 1u, 0u), ND_OK, "John 1 loads");
    CHECK_STR(api.verse(bib, 1u), "The Word.", "John 1:1");

    CHECK_INT(api.load(bib, 0u, 9u), ND_ERR_NOTFOUND, "a chapter past the end is NOTFOUND");
    CHECK_INT(api.load(bib, 9u, 0u), ND_ERR_NOTFOUND, "a book past the end is NOTFOUND");
}

static void test_pack_plain(void)
{
    nd_bible *bib = NULL;

    CHECK_INT(api.open(&bib, tp_path), ND_OK, "a pack with no dictionary opens");
    if (bib == NULL)
        return;
    check_pack_contents(bib, "translation label, no dictionary");
    api.close(bib);
}

static void test_pack_with_dictionary(void)
{
    nd_bible *bib = NULL;

    CHECK_INT(api.open(&bib, tp_dict_path), ND_OK, "a pack with a preset dictionary opens");
    if (bib == NULL)
        return;
    /* The whole point: identical contents through the Z_NEED_DICT path. */
    check_pack_contents(bib, "translation label, with dictionary");
    api.close(bib);
}

/* ------------------------------------------------------------------ *
 * 5. Refusing a bad pack
 * ------------------------------------------------------------------ */

/* Copies `src`, pokes `len` bytes in at `off`, and truncates to `keep` when
 * keep is non-zero. */
static bool corrupt_copy(const char *src, const char *dst, size_t off, const uint8_t *patch,
                         size_t len, size_t keep)
{
    uint8_t *buf;
    long n;
    FILE *f = fopen(src, "rb");
    size_t got;

    if (f == NULL)
        return false;
    (void)fseek(f, 0, SEEK_END);
    n = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf == NULL) {
        (void)fclose(f);
        return false;
    }
    got = fread(buf, 1u, (size_t)n, f);
    (void)fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return false;
    }
    if (patch != NULL && off + len <= got)
        memcpy(buf + off, patch, len);
    f = fopen(dst, "wb");
    if (f == NULL) {
        free(buf);
        return false;
    }
    (void)fwrite(buf, 1u, (keep != 0u && keep < got) ? keep : got, f);
    (void)fclose(f);
    free(buf);
    return true;
}

static void expect_refused(const char *path, const char *what)
{
    nd_bible *bib = NULL;
    nd_err rc = api.open(&bib, path);

    CHECK(rc != ND_OK, what);
    CHECK(bib == NULL, "and hands back no handle");
    api.close(bib);
}

static void test_bad_packs(void)
{
    char path[ND_PATH_MAX];
    uint8_t patch[8];

    (void)nd_snprintf(path, sizeof path, "%s/bad.ndb", sa_outdir);

    expect_refused("/nonexistent/nowhere.ndb", "a missing file is refused");

    memcpy(patch, "NOTABIBL", 8u);
    if (corrupt_copy(tp_path, path, 0u, patch, 8u, 0u))
        expect_refused(path, "bad magic is refused");

    wr16(patch, 99u);
    if (corrupt_copy(tp_path, path, 8u, patch, 2u, 0u))
        expect_refused(path, "an unknown version is refused");

    if (corrupt_copy(tp_path, path, 0u, NULL, 0u, ND_NDB_HDR_SZ / 2u))
        expect_refused(path, "a file shorter than the header is refused");

    /* Chapter 0's compressed length, pushed past the end of the blob. */
    wr32(patch, 0xFFFFu);
    if (corrupt_copy(tp_path, path, ND_NDB_HDR_SZ + TP_BOOKS * ND_NDB_BOOK_SZ + 4u, patch, 4u,
                     0u))
        expect_refused(path, "a chapter running off the blob is refused");

    /* The DICTIONARY pack with its dictionary flag cleared. Its streams were
     * deflated with a preset window, so inflate stops at Z_NEED_DICT with
     * nothing to hand it -- which is the only way to reach that branch.
     * Clearing the flag on the plain pack would prove nothing: those streams
     * never ask.
     *
     * The index is still sound, so the pack OPENS and the load is what must
     * fail, leaving no half-inflated chapter behind it. */
    wr16(patch, ND_NDB_F_DEFLATE);
    if (corrupt_copy(tp_dict_path, path, 10u, patch, 2u, 0u)) {
        nd_bible *bib = NULL;

        if (api.open(&bib, path) == ND_OK && bib != NULL) {
            CHECK(api.load(bib, 0u, 0u) != ND_OK, "a chapter needing an absent dictionary fails");
            CHECK_INT(api.verse_count(bib), 0, "and leaves no half-inflated chapter behind");
            CHECK_STR(api.verse(bib, 1u), "", "and reads back empty");
        }
        api.close(bib);
    }
}

/* ------------------------------------------------------------------ *
 * 6. References
 * ------------------------------------------------------------------ */

static void test_parse_ref(void)
{
    nd_bible *bib = NULL;
    size_t book = 99u;
    size_t chapter = 99u;
    size_t verse = 99u;
    char out[64];

    if (api.open(&bib, tp_path) != ND_OK)
        return;

    CHECK(api.parse_ref(bib, "John 3:16", &book, &chapter, &verse), "\"John 3:16\" parses");
    CHECK_INT(book, 1, "  book");
    CHECK_INT(chapter, 3, "  chapter");
    CHECK_INT(verse, 16, "  verse");

    CHECK(api.parse_ref(bib, "Joh3:16", &book, &chapter, &verse), "no space is fine");
    CHECK_INT(chapter, 3, "  chapter without a space");
    CHECK_INT(verse, 16, "  verse without a space");

    CHECK(api.parse_ref(bib, "  gen  2 ", &book, &chapter, &verse), "leading space is skipped");
    CHECK_INT(book, 0, "  book from an abbreviation");
    CHECK_INT(chapter, 2, "  chapter with no verse");
    CHECK_INT(verse, 0, "  and no verse means 0");

    CHECK(api.parse_ref(bib, "Genesis", &book, &chapter, &verse), "a bare book name parses");
    CHECK_INT(chapter, 0, "  with no chapter");

    /* A leading digit belongs to the NAME, which is the whole reason the
     * parser is not a scanf. There is no "1 Genesis", so this must fail
     * rather than quietly reading "1" as a chapter of something. */
    CHECK(!api.parse_ref(bib, "1 Corinthians 13", &book, &chapter, &verse),
          "a book this pack lacks is rejected");
    CHECK(!api.parse_ref(bib, "Nonesuch 1:1", &book, &chapter, &verse),
          "an unknown name is rejected");
    CHECK(!api.parse_ref(bib, "", &book, &chapter, &verse), "an empty reference is rejected");

    api.format_ref(bib, 1u, 3u, 16u, out, sizeof out);
    CHECK_STR(out, "John 3:16", "formatting a full reference");
    api.format_ref(bib, 1u, 3u, 0u, out, sizeof out);
    CHECK_STR(out, "John 3", "verse 0 formats as chapter only");
    api.format_ref(bib, 1u, 0u, 0u, out, sizeof out);
    CHECK_STR(out, "John", "chapter 0 formats as the book alone");

    api.close(bib);
}

/* ------------------------------------------------------------------ *
 * 7. The reader draws
 * ------------------------------------------------------------------ */

static int32_t ink_pixels(const nd_image *img)
{
    int32_t n = 0;
    int32_t x;
    int32_t y;

    for (y = 0; y < img->h; y++) {
        for (x = 0; x < img->w; x++) {
            nd_color c = nd_image_get_px(img, x, y);

            if (c.r > 64u || c.g > 64u || c.b > 64u)
                n++;
        }
    }
    return n;
}

static void run_reader(sa_fixture *fx, bool genz, const char *what)
{
    bible_app app;
    int32_t ink;
    size_t i;

    memset(&app, 0, sizeof app);
    app.ui = &fx->ui;
    app.genz = genz;
    app.state = nd_props_new();
    if (app.state == NULL || api.open(&app.b, tp_path) != ND_OK) {
        CHECK(false, "reader fixture set-up");
        nd_props_free(app.state);
        return;
    }
    (void)nd_snprintf(app.state_path, sizeof app.state_path, "%s/state.prop", sa_outdir);

    /* Scroll, page, step a chapter, then back out. The trailing Clears are
     * belt and braces: every screen in this app treats Clear as Back, so a
     * run of them always unwinds rather than leaving the test blocked in
     * nd_ui_wait_for_key(). */
    (void)sa_send(fx, ND_KEY_DOWN);
    (void)sa_send(fx, ND_KEY_DOWN);
    (void)sa_send(fx, ND_KEY_UP);
    (void)sa_send(fx, ND_KEY_0);
    (void)sa_send(fx, ND_KEY_RIGHT);
    (void)sa_send(fx, ND_KEY_LEFT);
    for (i = 0u; i < 6u; i++)
        (void)sa_send(fx, ND_KEY_CLEAR);

    api.read(&app, 0u, 0u, 1u);

    ink = ink_pixels(fx->canvas);
    CHECK(ink > 200, what);
    if (ink <= 200)
        fprintf(stderr, "  only %d lit pixels; the chapter did not render\n", ink);

    api.close(app.b);
    nd_props_free(app.state);
}

static void test_reader_draws(void)
{
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "the ui fixture came up");
        sa_fx_free(&fx);
        return;
    }
    run_reader(&fx, false, "the reader puts the chapter on the canvas");
    sa_fx_free(&fx);

    if (!sa_fx_init(&fx)) {
        CHECK(false, "the ui fixture came up for Gen Z");
        sa_fx_free(&fx);
        return;
    }
    run_reader(&fx, true, "and does the same in Gen Z mode");
    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * The shipped pack, when it is there
 * ------------------------------------------------------------------ */

static void test_shipped_pack(void)
{
    const char *env = getenv("NEODCT_BIBLE_PACK");
    char path[ND_PATH_MAX];
    nd_bible *bib = NULL;
    int32_t john;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(path, env, sizeof path);
    } else {
        if (!sa_resolve_neodct()) {
            printf("  SKIP real pack: cannot locate the tree\n");
            return;
        }
        (void)nd_snprintf(path, sizeof path, "%s/overlay/NeoDCT/System/apps/Bible/web.ndb",
                          sa_neodct);
    }
    if (!sa_file_exists(path)) {
        printf("  SKIP real pack: %s is not there; set NEODCT_BIBLE_PACK or build one "
               "with tools/mkbible.py\n",
               path);
        return;
    }
    if (api.open(&bib, path) != ND_OK) {
        CHECK(false, "the shipped pack opens");
        return;
    }

    CHECK_STR(api.translation(bib), "WEB", "the shipped pack is the WEB");
    CHECK_INT(api.book_count(bib), 81, "81 books, deuterocanon included");
    CHECK_INT(api.chapter_count(bib, 0u), 50, "Genesis has 50 chapters");

    CHECK_INT(api.load(bib, 0u, 0u), ND_OK, "Genesis 1 loads");
    CHECK_STR(api.verse(bib, 1u), "In the beginning, God created the heavens and the earth.",
              "Genesis 1:1");

    john = api.find_book(bib, "John");
    CHECK(john > 0, "John is found by name");
    if (john >= 0) {
        CHECK_INT(api.chapter_count(bib, (size_t)john), 21, "John has 21 chapters");
        CHECK_INT(api.load(bib, (size_t)john, 2u), ND_OK, "John 3 loads");
        CHECK(strstr(api.verse(bib, 16u), "For God so loved the world") != NULL, "John 3:16");
    }

    /* Psalm 119 is the longest chapter in the canon and the one most likely
     * to expose an off-by-one in the verse index. */
    {
        int32_t psalms = api.find_book(bib, "PSA");

        CHECK(psalms >= 0, "Psalms is found by abbreviation");
        if (psalms >= 0) {
            CHECK_INT(api.load(bib, (size_t)psalms, 118u), ND_OK, "Psalm 119 loads");
            CHECK_INT(api.verse_count(bib), 176, "Psalm 119 has 176 verses");
            CHECK(api.verse(bib, 176u)[0] != '\0', "and its last verse is not empty");
        }
    }

    /* Non-ASCII would render as blank boxes in font.ttf; mkbible.py folds it
     * and this is the assertion that the fold actually ran. */
    {
        size_t bad = 0u;
        size_t c;

        for (c = 0u; c < 50u; c++) {
            size_t v;

            if (api.load(bib, 0u, c) != ND_OK)
                continue;
            for (v = 1u; v <= api.verse_count(bib); v++) {
                const char *p;

                for (p = api.verse(bib, v); *p != '\0'; p++) {
                    if ((unsigned char)*p > 0x7Eu || (unsigned char)*p < 0x20u)
                        bad++;
                }
            }
        }
        CHECK_INT(bad, 0, "all of Genesis is printable ASCII");
    }

    api.close(bib);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("Bible", "ndbible");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    /* The Makefile points NEODCT_ROOT at a scratch directory so that a test
     * cannot write to a developer's real /NeoDCT. Every path this test opens
     * is already an absolute one inside sa_outdir (its own mkdtemp) or inside
     * the checkout, and nd_bible_open() resolves what it is given -- so the
     * prefix would turn "/tmp/ndbible-XXXX/test.ndb" into a path under the
     * scratch root that does not exist. Clearing it is safe here precisely
     * because nothing below names a /NeoDCT path at all. */
    (void)nd_path_set_root("");

    (void)nd_snprintf(tp_path, sizeof tp_path, "%s/test.ndb", sa_outdir);
    (void)nd_snprintf(tp_dict_path, sizeof tp_dict_path, "%s/test-dict.ndb", sa_outdir);
    if (tp_write(tp_path, false) == 0u || tp_write(tp_dict_path, true) == 0u) {
        fprintf(stderr, "test_bible: could not build the fixture packs\n");
        (void)dlclose(h);
        return 1;
    }

    RUN(test_genz_table_order);
    RUN(test_genz_substitution);
    RUN(test_genz_truncation);
    RUN(test_genz_deterministic);
    RUN(test_pack_plain);
    RUN(test_pack_with_dictionary);
    RUN(test_bad_packs);
    RUN(test_parse_ref);
    RUN(test_reader_draws);
    RUN(test_shipped_pack);

    return sa_end(h, "test_bible");
}
