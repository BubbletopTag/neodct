/* test_json.c -- the JSON reader and the emitter.
 *
 * Half of this file is about what the parser must REFUSE, because the update
 * manifest's compatibility checks are all negative: a package is rejected
 * when a field is the wrong type, and a parser that coerces cheerfully turns
 * every one of those checks into a no-op.
 *
 * The last case is a small deterministic fuzzer. It is not looking for wrong
 * answers -- it is looking for crashes, hangs and leaks on input nobody wrote
 * on purpose, which is the input this parser gets from an SD card. Run it
 * under `make ASAN=1` and the leak and overflow checks come free.
 */

#include <stdio.h>
#include <string.h>

#include "nd_json.h"

#include "platform_test.h"

static nd_json_doc *parse_ok(const char *text)
{
    nd_json_doc *doc = NULL;
    char err[128];
    nd_err rc = nd_json_parse((const uint8_t *)text, strlen(text), &doc, err, sizeof err);

    g_checks++;
    if (rc != ND_OK || doc == NULL) {
        g_failures++;
        fprintf(stderr, "FAIL parse of <%s>: %s\n", text, err);
        return NULL;
    }
    return doc;
}

static void reject(const char *text)
{
    nd_json_doc *doc = (nd_json_doc *)0x1;
    char err[128];
    nd_err rc = nd_json_parse((const uint8_t *)text, strlen(text), &doc, err, sizeof err);

    g_checks++;
    if (rc == ND_OK) {
        g_failures++;
        fprintf(stderr, "FAIL <%s> should have been rejected\n", text);
        nd_json_free(doc);
        return;
    }
    if (doc != NULL) {
        g_failures++;
        fprintf(stderr, "FAIL <%s> rejected but left a document behind\n", text);
    }
}

static void test_scalars_and_containers(void)
{
    nd_json_doc *doc = parse_ok("{\"s\":\"hi\",\"i\":42,\"r\":1.5,\"b\":true,\"n\":null,"
                                "\"a\":[1,2,3],\"o\":{\"k\":\"v\"}}");
    const nd_json_val *root;
    int64_t i = 0;
    double r = 0.0;
    bool b = false;
    const char *s = NULL;

    if (doc == NULL)
        return;
    root = nd_json_root(doc);

    CHECK_INT(nd_json_type_of(root), ND_JSON_OBJECT);
    CHECK_INT(nd_json_len(root), 7);
    CHECK(nd_json_str(nd_json_get(root, "s"), &s));
    CHECK_STR(s, "hi");
    CHECK(nd_json_int(nd_json_get(root, "i"), &i));
    CHECK_INT(i, 42);
    CHECK(nd_json_real(nd_json_get(root, "r"), &r));
    CHECK(r == 1.5);
    CHECK(nd_json_bool(nd_json_get(root, "b"), &b));
    CHECK(b == true);
    CHECK_INT(nd_json_type_of(nd_json_get(root, "n")), ND_JSON_NULL);

    CHECK_INT(nd_json_len(nd_json_get(root, "a")), 3);
    CHECK(nd_json_int(nd_json_at(nd_json_get(root, "a"), 2u), &i));
    CHECK_INT(i, 3);
    CHECK(nd_json_at(nd_json_get(root, "a"), 3u) == NULL);

    CHECK_STR(nd_json_get_str(nd_json_get(root, "o"), "k", "?"), "v");

    /* Object iteration is in document order. */
    CHECK_STR(nd_json_key_at(root, 0u), "s");
    CHECK_STR(nd_json_key_at(root, 6u), "o");
    CHECK(nd_json_key_at(root, 7u) == NULL);
    CHECK_INT(nd_json_type_of(nd_json_member_at(root, 1u)), ND_JSON_INT);

    nd_json_free(doc);
}

/* An integer is not a float, and a boolean is not a number. The update
 * manifest must reject "buildtime": 1785160800.0 while accepting
 * 1785160800, and must reject "buildtime": true. */
static void test_int_real_and_bool_are_distinct_types(void)
{
    nd_json_doc *doc = parse_ok("{\"whole\":1785160800,\"float\":1785160800.0,"
                                "\"exp\":1e3,\"flag\":true,\"zero\":0,\"neg\":-7}");
    const nd_json_val *root;
    int64_t i = 0;
    double r = 0.0;

    if (doc == NULL)
        return;
    root = nd_json_root(doc);

    CHECK_INT(nd_json_type_of(nd_json_get(root, "whole")), ND_JSON_INT);
    CHECK_INT(nd_json_type_of(nd_json_get(root, "float")), ND_JSON_REAL);
    /* 1e3 has an exponent, so it is a float even with no decimal point. */
    CHECK_INT(nd_json_type_of(nd_json_get(root, "exp")), ND_JSON_REAL);
    CHECK_INT(nd_json_type_of(nd_json_get(root, "flag")), ND_JSON_BOOL);

    CHECK(nd_json_int(nd_json_get(root, "whole"), &i));
    CHECK_INT(i, 1785160800);
    /* No coercion in this direction: this is the assertion the manifest
     * check leans on. */
    CHECK(!nd_json_int(nd_json_get(root, "float"), &i));
    CHECK(!nd_json_int(nd_json_get(root, "flag"), &i));
    /* ...and none from a number to a bool either. */
    {
        bool b = false;

        CHECK(!nd_json_bool(nd_json_get(root, "whole"), &b));
    }
    /* An int where a float is wanted IS accepted; float(1) is 1.0. */
    CHECK(nd_json_real(nd_json_get(root, "whole"), &r));
    CHECK(r == 1785160800.0);

    CHECK_INT(nd_json_get_int(root, "neg", 0), -7);
    CHECK_INT(nd_json_get_int(root, "float", 99), 99);
    CHECK_INT(nd_json_get_int(root, "absent", 99), 99);
    CHECK(nd_json_get_bool(root, "flag", false) == true);
    CHECK(nd_json_get_bool(root, "whole", false) == false);
    CHECK_STR(nd_json_get_str(root, "absent", "dflt"), "dflt");

    nd_json_free(doc);
}

/* Python's json keeps the LAST value and the FIRST position. */
static void test_last_duplicate_key_wins(void)
{
    nd_json_doc *doc = parse_ok("{\"a\":1,\"b\":2,\"a\":3}");
    const nd_json_val *root;

    if (doc == NULL)
        return;
    root = nd_json_root(doc);

    CHECK_INT(nd_json_len(root), 2);
    CHECK_INT(nd_json_get_int(root, "a", 0), 3);
    CHECK_STR(nd_json_key_at(root, 0u), "a");
    CHECK_STR(nd_json_key_at(root, 1u), "b");

    nd_json_free(doc);
}

static void test_string_escapes(void)
{
    nd_json_doc *doc =
        parse_ok("[\"\\\"\\\\\\/\\b\\f\\n\\r\\t\",\"\\u0041\\u00e9\\u20ac\",\"\\ud83d\\ude00\","
                 "\"\\ud800\",\"caf\xc3\xa9\"]");
    const nd_json_val *root;
    const char *s = NULL;

    if (doc == NULL)
        return;
    root = nd_json_root(doc);

    CHECK(nd_json_str(nd_json_at(root, 0u), &s));
    CHECK_STR(s, "\"\\/\b\f\n\r\t");

    CHECK(nd_json_str(nd_json_at(root, 1u), &s));
    CHECK_STR(s, "A\xc3\xa9\xe2\x82\xac");

    /* A surrogate PAIR is one astral character: U+1F600. */
    CHECK(nd_json_str(nd_json_at(root, 2u), &s));
    CHECK_STR(s, "\xf0\x9f\x98\x80");

    /* A LONE surrogate has no UTF-8 encoding, so it becomes U+FFFD rather
     * than producing a string no other part of the system could handle. */
    CHECK(nd_json_str(nd_json_at(root, 3u), &s));
    CHECK_STR(s, "\xef\xbf\xbd");

    /* Raw UTF-8 in the source passes straight through. */
    CHECK(nd_json_str(nd_json_at(root, 4u), &s));
    CHECK_STR(s, "caf\xc3\xa9");

    nd_json_free(doc);
}

static void test_malformed_documents_are_refused(void)
{
    reject("");
    reject("   ");
    reject("{");
    reject("[");
    reject("[1,]");
    reject("{\"a\":}");
    reject("{\"a\" 1}");
    reject("{a:1}");
    reject("{'a':1}");
    reject("[1 2]");
    reject("[1]]");
    reject("{}}");
    reject("nul");
    reject("tru");
    reject("01");
    reject("-");
    reject("1.");
    reject(".5");
    reject("1e");
    reject("+1");
    reject("\"unterminated");
    reject("[\"raw\nnewline\"]");
    reject("{\"a\":1}trailing");
    reject("[1,2] [3]");
    /* Python's json accepts NaN and Infinity; this parser does not, because
     * nothing in the project writes them and accepting them would mean a
     * manifest field could be a quiet NaN. Recorded as a deviation. */
    reject("NaN");
    reject("Infinity");
}

static void test_non_utf8_input_is_refused(void)
{
    static const unsigned char bad[] = {'[', '"', 0xffu, '"', ']'};
    nd_json_doc *doc = (nd_json_doc *)0x1;
    char err[128];

    /* json.loads() on bytes decodes strict UTF-8 first, so the grammar is
     * never even consulted. */
    CHECK_INT(nd_json_parse(bad, sizeof bad, &doc, err, sizeof err), ND_ERR_PARSE);
    CHECK(doc == NULL);
    CHECK(err[0] != '\0');
}

static void test_the_bounds_are_real(void)
{
    char deep[4 * (ND_JSON_MAX_DEPTH + 4) + 8];
    nd_json_doc *doc = NULL;
    char err[128];
    size_t i;
    size_t w = 0u;

    /* Exactly at the limit: accepted. */
    for (i = 0u; i < ND_JSON_MAX_DEPTH; i++)
        deep[w++] = '[';
    for (i = 0u; i < ND_JSON_MAX_DEPTH; i++)
        deep[w++] = ']';
    deep[w] = '\0';
    CHECK_INT(nd_json_parse((const uint8_t *)deep, w, &doc, err, sizeof err), ND_OK);
    nd_json_free(doc);
    doc = NULL;

    /* One past: refused, and refused without having recursed to get there. */
    w = 0u;
    for (i = 0u; i < ND_JSON_MAX_DEPTH + 1u; i++)
        deep[w++] = '[';
    for (i = 0u; i < ND_JSON_MAX_DEPTH + 1u; i++)
        deep[w++] = ']';
    deep[w] = '\0';
    CHECK_INT(nd_json_parse((const uint8_t *)deep, w, &doc, err, sizeof err), ND_ERR_PARSE);
    CHECK(doc == NULL);

    /* Over the byte cap, without allocating a byte of it. */
    CHECK_INT(nd_json_parse((const uint8_t *)"[]", ND_JSON_MAX_BYTES + 1u, &doc, err, sizeof err),
              ND_ERR_TOOLONG);
    CHECK(doc == NULL);
}

static void test_parse_file(void)
{
    nd_json_doc *doc = NULL;
    char err[128];

    pt_write_text("/apps/Koki/manifest.json", "{\n  \"name\": \"Koki\",\n  \"exec\": \"app.so\",\n"
                                              "  \"version\": 3,\n  \"hidden\": false\n}\n");

    CHECK_INT(nd_json_parse_file("/apps/Koki/manifest.json", &doc, err, sizeof err), ND_OK);
    CHECK(doc != NULL);
    if (doc != NULL) {
        CHECK_STR(nd_json_get_str(nd_json_root(doc), "name", "?"), "Koki");
        CHECK_INT(nd_json_get_int(nd_json_root(doc), "version", 0), 3);
        CHECK(nd_json_get_bool(nd_json_root(doc), "hidden", true) == false);
        nd_json_free(doc);
    }

    doc = (nd_json_doc *)0x1;
    CHECK_INT(nd_json_parse_file("/apps/Nope/manifest.json", &doc, err, sizeof err), ND_ERR_IO);
    CHECK(doc == NULL);
}

static void test_writer_compact_and_indented(void)
{
    nd_json_writer *w = nd_json_writer_new(0);
    size_t len = 0u;

    CHECK(w != NULL);
    CHECK_INT(nd_json_begin_object(w), ND_OK);
    CHECK_INT(nd_json_key(w, "keys"), ND_OK);
    CHECK_INT(nd_json_begin_array(w), ND_OK);
    CHECK_INT(nd_json_put_int(w, 1), ND_OK);
    CHECK_INT(nd_json_put_str(w, "two"), ND_OK);
    CHECK_INT(nd_json_end_array(w), ND_OK);
    CHECK_INT(nd_json_key(w, "empty"), ND_OK);
    CHECK_INT(nd_json_begin_object(w), ND_OK);
    CHECK_INT(nd_json_end_object(w), ND_OK);
    CHECK_INT(nd_json_key(w, "ok"), ND_OK);
    CHECK_INT(nd_json_put_bool(w, true), ND_OK);
    CHECK_INT(nd_json_key(w, "nothing"), ND_OK);
    CHECK_INT(nd_json_put_null(w), ND_OK);
    CHECK_INT(nd_json_end_object(w), ND_OK);

    {
        static const char expect[] =
            "{\"keys\":[1,\"two\"],\"empty\":{},\"ok\":true,\"nothing\":null}";

        CHECK_STR(nd_json_writer_text(w, &len), expect);
        CHECK_INT(len, sizeof expect - 1u);
    }
    nd_json_writer_free(w);

    w = nd_json_writer_new(2);
    CHECK_INT(nd_json_begin_object(w), ND_OK);
    CHECK_INT(nd_json_key(w, "a"), ND_OK);
    CHECK_INT(nd_json_begin_array(w), ND_OK);
    CHECK_INT(nd_json_put_int(w, 1), ND_OK);
    CHECK_INT(nd_json_end_array(w), ND_OK);
    CHECK_INT(nd_json_end_object(w), ND_OK);
    CHECK_STR(nd_json_writer_text(w, NULL), "{\n  \"a\": [\n    1\n  ]\n}");
    nd_json_writer_free(w);
}

static void test_writer_escapes_and_numbers(void)
{
    nd_json_writer *w = nd_json_writer_new(0);

    CHECK_INT(nd_json_begin_array(w), ND_OK);
    CHECK_INT(nd_json_put_str(w, "quote\" back\\ tab\t bell\x07"), ND_OK);
    /* json.dumps defaults to ensure_ascii=True, so non-ASCII is escaped and
     * anything beyond the BMP becomes a surrogate pair. */
    CHECK_INT(nd_json_put_str(w, "caf\xc3\xa9 \xf0\x9f\x98\x80"), ND_OK);
    CHECK_INT(nd_json_put_real(w, 1.0), ND_OK);
    CHECK_INT(nd_json_put_real(w, 0.1), ND_OK);
    CHECK_INT(nd_json_put_int(w, -9007199254740993LL), ND_OK);
    CHECK_INT(nd_json_end_array(w), ND_OK);

    CHECK_STR(nd_json_writer_text(w, NULL),
              "[\"quote\\\" back\\\\ tab\\t bell\\u0007\","
              "\"caf\\u00e9 \\ud83d\\ude00\",1.0,0.1,-9007199254740993]");
    nd_json_writer_free(w);
}

static void test_writer_refuses_an_unbalanced_document(void)
{
    nd_json_writer *w = nd_json_writer_new(0);

    CHECK_INT(nd_json_begin_object(w), ND_OK);
    CHECK_INT(nd_json_key(w, "a"), ND_OK);
    /* A key with no value, then a close: that is not a document. */
    CHECK(nd_json_end_object(w) != ND_OK);
    CHECK(nd_json_writer_text(w, NULL) == NULL);
    nd_json_writer_free(w);

    w = nd_json_writer_new(0);
    CHECK_INT(nd_json_begin_array(w), ND_OK);
    /* Closing an array with the object closer must not be papered over. */
    CHECK(nd_json_end_object(w) != ND_OK);
    nd_json_writer_free(w);

    w = nd_json_writer_new(0);
    CHECK_INT(nd_json_begin_array(w), ND_OK);
    CHECK_INT(nd_json_put_int(w, 1), ND_OK);
    CHECK(nd_json_writer_text(w, NULL) == NULL); /* still open */
    CHECK_INT(nd_json_end_array(w), ND_OK);
    CHECK_STR(nd_json_writer_text(w, NULL), "[1]");
    nd_json_writer_free(w);
}

static void test_writer_save_round_trips(void)
{
    nd_json_writer *w = nd_json_writer_new(2);
    nd_json_doc *doc = NULL;
    char err[128];

    CHECK_INT(nd_json_begin_object(w), ND_OK);
    CHECK_INT(nd_json_key(w, "map"), ND_OK);
    CHECK_INT(nd_json_begin_object(w), ND_OK);
    CHECK_INT(nd_json_key(w, "2"), ND_OK);
    CHECK_INT(nd_json_put_str(w, "KEY_UP"), ND_OK);
    CHECK_INT(nd_json_end_object(w), ND_OK);
    CHECK_INT(nd_json_end_object(w), ND_OK);

    CHECK_INT(nd_json_writer_save(w, "/User/keymap.json"), ND_OK);
    CHECK(!nd_path_exists("/User/keymap.json.tmp"));
    nd_json_writer_free(w);

    CHECK_INT(nd_json_parse_file("/User/keymap.json", &doc, err, sizeof err), ND_OK);
    if (doc != NULL) {
        CHECK_STR(nd_json_get_str(nd_json_get(nd_json_root(doc), "map"), "2", "?"), "KEY_UP");
        nd_json_free(doc);
    }
}

/* A tiny deterministic fuzzer. xorshift rather than rand() so a failure
 * reported from the phone reproduces exactly on a desktop. */
static uint32_t rng_state = 0x1234567u;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz_never_crashes(void)
{
    static const char *const seeds[] = {
        "{\"a\":[1,2,{\"b\":\"c\"}],\"d\":null}",
        "[[[[[[\"deep\"]]]]]]",
        "{\"u\":\"\\ud83d\\ude00\",\"n\":-1.5e-3,\"t\":true}",
        "[]",
        "{}",
    };
    char buf[256];
    size_t iter;

    for (iter = 0u; iter < 20000u; iter++) {
        const char *seed = seeds[rng() % ND_ARRAY_LEN(seeds)];
        size_t len = strlen(seed);
        size_t mutations = 1u + (rng() % 4u);
        size_t m;
        nd_json_doc *doc = NULL;
        char err[128];

        memcpy(buf, seed, len);
        for (m = 0u; m < mutations; m++) {
            uint32_t what = rng() % 3u;

            if (what == 0u && len > 0u) {
                buf[rng() % len] = (char)(rng() % 128u); /* substitute */
            } else if (what == 1u && len > 0u) {
                size_t at = rng() % len;

                memmove(buf + at, buf + at + 1u, len - at - 1u); /* delete */
                len--;
            } else if (len + 1u < sizeof buf) {
                size_t at = rng() % (len + 1u);

                memmove(buf + at + 1u, buf + at, len - at); /* insert */
                buf[at] = (char)(rng() % 128u);
                len++;
            }
        }

        /* The only assertion is the contract: either a document comes back or
         * it does not, and nothing is leaked or scribbled on either way.
         * ASAN turns "did not crash" into "did not corrupt anything". */
        if (nd_json_parse((const uint8_t *)buf, len, &doc, err, sizeof err) == ND_OK) {
            g_checks++;
            if (doc == NULL) {
                g_failures++;
                fprintf(stderr, "FAIL fuzz: ND_OK with a NULL document\n");
            }
            (void)nd_json_len(nd_json_root(doc));
            nd_json_free(doc);
        } else {
            g_checks++;
            if (doc != NULL) {
                g_failures++;
                fprintf(stderr, "FAIL fuzz: failure left a document behind\n");
                nd_json_free(doc);
            }
        }
    }
}

int main(void)
{
    RUN(test_scalars_and_containers);
    RUN(test_int_real_and_bool_are_distinct_types);
    RUN(test_last_duplicate_key_wins);
    RUN(test_string_escapes);
    RUN(test_malformed_documents_are_refused);
    RUN(test_non_utf8_input_is_refused);
    RUN(test_the_bounds_are_real);
    RUN(test_parse_file);
    RUN(test_writer_compact_and_indented);
    RUN(test_writer_escapes_and_numbers);
    RUN(test_writer_refuses_an_unbalanced_document);
    RUN(test_writer_save_round_trips);
    RUN(test_fuzz_never_crashes);
    return pt_report("test_json");
}
