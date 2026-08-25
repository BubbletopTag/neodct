/* test_manifest.c -- manifest.json parsing, validation and compatibility.
 *
 * The first twenty-two cases are neodct/tests/test_update_manifest.py, one
 * for one and in its order, because that suite is the oracle: it runs against
 * the Python this file is a port of, and a C reader only this file has ever
 * agreed with is worth nothing.
 *
 * The rest are cases Python cannot express or does not need -- bytes.fromhex
 * whitespace rules, bool-versus-int, the length caps that C has and Python
 * does not -- and they are the reason a hostile manifest cannot get past
 * this module by being large rather than by being wrong.
 */

#include <stdio.h>
#include <string.h>

#include "nd_manifest.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Building manifests
 * ------------------------------------------------------------------ *
 *
 * Every field is a JSON TOKEN, not a value: "\"0.3.2a\"" including its
 * quotes, or "4096", or "true". NULL omits the key entirely, which is what
 * the "missing required field" cases need and what a printf template cannot
 * express.
 */
typedef struct {
    const char *version;
    const char *buildtime;
    const char *changelog;
    const char *platform;
    const char *sha256;
    const char *thumbnail_sha256;
    const char *min_kernel;
    const char *verity; /* the whole "verity" value; NULL builds one below */
    const char *root_hash;
    const char *block_size;
    const char *image_blocks;
    const char *salt;
} mf;

#define SHA_A "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""
#define SHA_B "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\""
#define SHA_C "\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\""

static mf mf_good(void)
{
    mf f;

    memset(&f, 0, sizeof f);
    f.version = "\"0.3.2a\"";
    f.buildtime = "1785160800";
    f.changelog = "\"Fixed SMS database sorting bug and updated stock wallpapers.\"";
    f.platform = "\"qemu-aarch64\"";
    f.sha256 = SHA_A;
    f.root_hash = SHA_B;
    f.block_size = "4096";
    f.image_blocks = "30720";
    f.salt = "\"f8e7d6c5b4a3\"";
    return f;
}

/* Appends `"key": value,` when value is not NULL. */
static void put(char *buf, size_t sz, const char *key, const char *value)
{
    char piece[2048];

    if (value == NULL)
        return;
    if (snprintf(piece, sizeof piece, "\"%s\": %s, ", key, value) < 0)
        return;
    (void)nd_strlcat(buf, piece, sz);
}

static size_t mf_json(char *buf, size_t sz, const mf *f)
{
    char verity[3072];
    size_t n;

    buf[0] = '\0';
    (void)nd_strlcat(buf, "{", sz);
    put(buf, sz, "version", f->version);
    put(buf, sz, "buildtime", f->buildtime);
    put(buf, sz, "changelog", f->changelog);
    put(buf, sz, "platform", f->platform);
    put(buf, sz, "sha256", f->sha256);
    put(buf, sz, "thumbnail_sha256", f->thumbnail_sha256);
    put(buf, sz, "min_kernel", f->min_kernel);

    if (f->verity != NULL) {
        put(buf, sz, "verity", f->verity);
    } else if (f->root_hash != NULL || f->block_size != NULL || f->image_blocks != NULL ||
               f->salt != NULL) {
        verity[0] = '\0';
        (void)nd_strlcat(verity, "{", sizeof verity);
        put(verity, sizeof verity, "root_hash", f->root_hash);
        put(verity, sizeof verity, "block_size", f->block_size);
        put(verity, sizeof verity, "image_blocks", f->image_blocks);
        put(verity, sizeof verity, "salt", f->salt);
        n = strlen(verity);
        if (n >= 2u && verity[n - 2u] == ',')
            verity[n - 2u] = '\0';
        (void)nd_strlcat(verity, "}", sizeof verity);
        put(buf, sz, "verity", verity);
    }

    n = strlen(buf);
    if (n >= 2u && buf[n - 2u] == ',')
        buf[n - 2u] = '\0';
    (void)nd_strlcat(buf, "}", sz);
    return strlen(buf);
}

/* ------------------------------------------------------------------ *
 * Assertions
 * ------------------------------------------------------------------ */

static nd_manifest *parse_ok(const char *text)
{
    nd_manifest *m = NULL;
    char why[ND_MANIFEST_WHY_MAX];

    why[0] = '\0';
    g_checks++;
    if (nd_manifest_parse((const uint8_t *)text, strlen(text), &m, why, sizeof why) != ND_UPD_OK ||
        m == NULL) {
        g_failures++;
        fprintf(stderr, "FAIL parse of <%s>: %s\n", text, why);
        return NULL;
    }
    return m;
}

/* Refused, and the refusal names `wanted`. Both halves matter: three test
 * suites match on the field name inside the message. */
static void reject(const char *text, const char *wanted)
{
    nd_manifest *m = (nd_manifest *)0x1;
    char why[ND_MANIFEST_WHY_MAX];

    why[0] = '\0';
    g_checks++;
    if (nd_manifest_parse((const uint8_t *)text, strlen(text), &m, why, sizeof why) == ND_UPD_OK) {
        g_failures++;
        fprintf(stderr, "FAIL <%s> should have been rejected\n", text);
        nd_manifest_free(m);
        return;
    }
    g_checks++;
    if (m != NULL) {
        g_failures++;
        fprintf(stderr, "FAIL <%s> rejected but left a manifest behind\n", text);
    }
    g_checks++;
    if (strstr(why, wanted) == NULL) {
        g_failures++;
        fprintf(stderr, "FAIL <%s> refused with \"%s\", wanted it to name \"%s\"\n", text, why,
                wanted);
    }
}

static void reject_mf(const mf *f, const char *wanted)
{
    char buf[4096];

    (void)mf_json(buf, sizeof buf, f);
    reject(buf, wanted);
}

/* ------------------------------------------------------------------ *
 * test_update_manifest.py, one for one
 * ------------------------------------------------------------------ */

static void test_parses_a_well_formed_manifest(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;

    CHECK_STR(m->version, "0.3.2a");
    CHECK_INT(m->buildtime, 1785160800);
    CHECK_STR(m->platform, "qemu-aarch64");
    CHECK_INT(strlen(m->sha256), 64);
    CHECK(strncmp(m->changelog, "Fixed SMS", 9u) == 0);
    CHECK_INT(m->verity_block_size, 4096);
    CHECK_INT(m->verity_image_blocks, 30720);
    CHECK_STR(m->verity_salt, "f8e7d6c5b4a3");
    nd_manifest_free(m);
}

static void test_keeps_the_exact_bytes_it_was_given(void)
{
    /* The signature covers the file verbatim, so it must never be re-encoded.
     * Trailing newline included: mkupdate.py writes one and openssl signed
     * it. */
    static const char text[] = "{\"version\": \"1\", \"buildtime\": 1, "
                               "\"platform\": \"p\", \"sha256\": " SHA_A ", "
                               "\"verity\": {\"root_hash\": " SHA_B ", "
                               "\"block_size\": 4096, \"image_blocks\": 1}}\n";
    nd_manifest *m = parse_ok(text);

    if (m == NULL)
        return;
    CHECK_INT(m->raw_len, sizeof text - 1u);
    CHECK(memcmp(m->raw, text, sizeof text - 1u) == 0);
    nd_manifest_free(m);
}

static void test_derives_the_hash_offset_from_the_image_size(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    CHECK(nd_manifest_hash_offset(m) == 30720ull * 4096ull);
    CHECK(nd_manifest_image_bytes(m) == 30720ull * 4096ull);
    nd_manifest_free(m);
}

static void test_changelog_is_optional(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    f.changelog = NULL;
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    CHECK_STR(m->changelog, "");
    nd_manifest_free(m);
}

static void test_missing_required_field_is_an_invalid_update(void)
{
    mf f;

    f = mf_good();
    f.version = NULL;
    reject_mf(&f, "version");
    f = mf_good();
    f.buildtime = NULL;
    reject_mf(&f, "buildtime");
    f = mf_good();
    f.platform = NULL;
    reject_mf(&f, "platform");
    f = mf_good();
    f.sha256 = NULL;
    reject_mf(&f, "sha256");
    f = mf_good();
    f.root_hash = NULL;
    f.block_size = NULL;
    f.image_blocks = NULL;
    f.salt = NULL;
    reject_mf(&f, "verity");
}

static void test_missing_verity_field_is_an_invalid_update(void)
{
    mf f;

    f = mf_good();
    f.root_hash = NULL;
    reject_mf(&f, "root_hash");
    f = mf_good();
    f.block_size = NULL;
    reject_mf(&f, "block_size");
    f = mf_good();
    f.image_blocks = NULL;
    reject_mf(&f, "image_blocks");
}

static void test_verity_salt_may_be_absent(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    f.salt = NULL;
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    /* manifest.py writes "" back into verity["salt"] so the field is always
     * there; staging then records verity_salt= and the shell's verity_table()
     * turns that into a bare "-". */
    CHECK_STR(m->verity_salt, "");
    nd_manifest_free(m);
}

static void test_rejects_a_non_hex_image_hash(void)
{
    mf f = mf_good();

    f.sha256 = "\"nope\"";
    reject_mf(&f, "sha256");
}

static void test_rejects_a_non_hex_root_hash(void)
{
    mf f = mf_good();

    f.root_hash = "\"zz\"";
    reject_mf(&f, "root_hash");
}

static void test_rejects_a_non_numeric_buildtime(void)
{
    mf f = mf_good();

    f.buildtime = "\"tuesday\"";
    reject_mf(&f, "buildtime");
}

static void test_rejects_a_block_size_that_is_not_a_power_of_two(void)
{
    mf f = mf_good();

    f.block_size = "3000";
    reject_mf(&f, "block_size");
}

static void test_rejects_malformed_json(void)
{
    reject("{not json", "JSON");
}

static void test_rejects_a_json_document_that_is_not_an_object(void)
{
    reject("[1, 2, 3]", "JSON object");
}

static void test_accepts_a_matching_platform(void)
{
    mf f = mf_good();
    char buf[4096];
    char why[ND_MANIFEST_WHY_MAX];
    nd_manifest *m;

    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_manifest_check_compatible(m, "qemu-aarch64", NULL, why, sizeof why), ND_UPD_OK);
    nd_manifest_free(m);
}

static void test_refuses_an_update_built_for_another_platform(void)
{
    /* The Luckfox and QEMU images share a filename; installing the wrong one
     * on real hardware is unrecoverable without a reflash. */
    mf f = mf_good();
    char buf[4096];
    char why[ND_MANIFEST_WHY_MAX];
    nd_manifest *m;

    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_manifest_check_compatible(m, "luckfox-armv7", NULL, why, sizeof why),
              ND_UPD_ERR_INCOMPATIBLE);
    CHECK_STR(why, "update is for qemu-aarch64, this is luckfox-armv7");

    /* A NULL platform can never match, which is the safe direction. */
    CHECK_INT(nd_manifest_check_compatible(m, NULL, NULL, why, sizeof why),
              ND_UPD_ERR_INCOMPATIBLE);
    nd_manifest_free(m);
}

static void test_refuses_an_update_that_needs_a_newer_kernel(void)
{
    mf f = mf_good();
    char buf[4096];
    char why[ND_MANIFEST_WHY_MAX];
    nd_manifest *m;

    f.min_kernel = "\"6.20.0\"";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_manifest_check_compatible(m, "qemu-aarch64", "6.12.47", why, sizeof why),
              ND_UPD_ERR_INCOMPATIBLE);
    CHECK_STR(why, "update needs kernel 6.20.0, running 6.12.47");
    nd_manifest_free(m);
}

static void test_accepts_an_update_whose_kernel_requirement_is_met(void)
{
    mf f = mf_good();
    char buf[4096];
    char why[ND_MANIFEST_WHY_MAX];
    nd_manifest *m;

    f.min_kernel = "\"6.12.0\"";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_manifest_check_compatible(m, "qemu-aarch64", "6.12.47", why, sizeof why),
              ND_UPD_OK);
    nd_manifest_free(m);
}

static void test_kernel_requirement_is_ignored_when_absent(void)
{
    mf f = mf_good();
    char buf[4096];
    char why[ND_MANIFEST_WHY_MAX];
    nd_manifest *m;

    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    why[0] = '\0';
    CHECK_STR(m->min_kernel, "");
    CHECK_INT(nd_manifest_check_compatible(m, "qemu-aarch64", "5.10.160", why, sizeof why),
              ND_UPD_OK);
    /* And the other half of "if self.min_kernel and kernel": an empty running
     * kernel skips the gate even when the manifest demands one. */
    nd_manifest_free(m);

    f.min_kernel = "\"99.0.0\"";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    CHECK_INT(nd_manifest_check_compatible(m, "qemu-aarch64", NULL, why, sizeof why), ND_UPD_OK);
    CHECK_INT(nd_manifest_check_compatible(m, "qemu-aarch64", "", why, sizeof why), ND_UPD_OK);
    nd_manifest_free(m);
}

static void test_a_thumbnail_hash_is_carried_through(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    f.thumbnail_sha256 = SHA_C;
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    CHECK_INT(strlen(m->thumbnail_sha256), 64);
    CHECK(m->thumbnail_sha256[0] == 'c');
    nd_manifest_free(m);
}

static void test_a_manifest_without_a_thumbnail_reports_no_hash(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    CHECK_STR(m->thumbnail_sha256, "");
    nd_manifest_free(m);

    /* body.get(k) or "": null, false, 0 and "" all collapse to absent. */
    f.thumbnail_sha256 = "null";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m != NULL) {
        CHECK_STR(m->thumbnail_sha256, "");
        nd_manifest_free(m);
    }
    f.thumbnail_sha256 = "\"\"";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m != NULL) {
        CHECK_STR(m->thumbnail_sha256, "");
        nd_manifest_free(m);
    }
    f.thumbnail_sha256 = "false";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m != NULL) {
        CHECK_STR(m->thumbnail_sha256, "");
        nd_manifest_free(m);
    }
}

static void test_rejects_a_thumbnail_hash_that_is_not_a_sha256(void)
{
    mf f = mf_good();

    f.thumbnail_sha256 = "\"deadbeef\"";
    reject_mf(&f, "thumbnail_sha256");
}

static void test_rejects_a_thumbnail_hash_that_is_not_hex(void)
{
    mf f = mf_good();

    f.thumbnail_sha256 = "\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"";
    reject_mf(&f, "thumbnail_sha256");
}

/* ------------------------------------------------------------------ *
 * The bits Python cannot say, and the C-only bounds
 * ------------------------------------------------------------------ */

static void test_hex_follows_bytes_fromhex_to_the_letter(void)
{
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    /* Uppercase is accepted -- and STORED UPPERCASE. A later comparison
     * against a computed digest is a byte comparison and will therefore fail,
     * which is manifest.py's behaviour and is deliberately not corrected. */
    f.sha256 = "\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m != NULL) {
        CHECK(m->sha256[0] == 'A');
        nd_manifest_free(m);
    }

    /* Whitespace BETWEEN pairs is skipped, so this is still 32 bytes. The
     * value keeps its spaces. */
    f = mf_good();
    f.salt = "\"f8 e7\\td6\\nc5\"";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m != NULL) {
        CHECK_STR(m->verity_salt, "f8 e7\td6\nc5");
        nd_manifest_free(m);
    }

    /* Whitespace INSIDE a pair is not: bytes.fromhex("a b") raises. */
    f = mf_good();
    f.salt = "\"a b\"";
    reject_mf(&f, "salt");

    /* An odd number of digits raises too. */
    f = mf_good();
    f.salt = "\"abc\"";
    reject_mf(&f, "salt");

    /* Not a string at all. */
    f = mf_good();
    f.sha256 = "12345";
    reject_mf(&f, "sha256 must be a hex string");

    /* The message names the digit count, not the byte count. */
    f = mf_good();
    f.sha256 = "\"aabb\"";
    reject_mf(&f, "sha256 must be 64 hex digits");
}

static void test_an_integer_is_not_a_float_and_a_bool_is_not_a_number(void)
{
    mf f;

    f = mf_good();
    f.buildtime = "1785160800.0";
    reject_mf(&f, "buildtime");
    f = mf_good();
    f.buildtime = "true";
    reject_mf(&f, "buildtime");
    f = mf_good();
    f.buildtime = "1.785e9";
    reject_mf(&f, "buildtime");

    /* "block_size": true is rejected by Python too, because bool is a
     * subclass of int and True < 512. */
    f = mf_good();
    f.block_size = "true";
    reject_mf(&f, "block_size");

    /* "image_blocks": true is 1 in Python, for the same subclass reason.
     * DIVERGENCE: refused here. nd_json.h keeps ND_JSON_BOOL distinct so
     * that this is possible, and a manifest whose block count arrived as a
     * boolean is not one anybody meant to sign. */
    f = mf_good();
    f.image_blocks = "true";
    reject_mf(&f, "image_blocks");
    f = mf_good();
    f.image_blocks = "4096.0";
    reject_mf(&f, "image_blocks");
}

static void test_the_verity_geometry_bounds(void)
{
    mf f;

    f = mf_good();
    f.block_size = "511";
    reject_mf(&f, "block_size");
    f = mf_good();
    f.block_size = "0";
    reject_mf(&f, "block_size");
    f = mf_good();
    f.block_size = "-4096";
    reject_mf(&f, "block_size");
    f = mf_good();
    f.image_blocks = "0";
    reject_mf(&f, "image_blocks");
    f = mf_good();
    f.image_blocks = "-1";
    reject_mf(&f, "image_blocks");

    /* 512 is the floor and is accepted. */
    f = mf_good();
    f.block_size = "512";
    {
        char buf[4096];
        nd_manifest *m;

        (void)mf_json(buf, sizeof buf, &f);
        m = parse_ok(buf);
        if (m != NULL) {
            CHECK_INT(m->verity_block_size, 512);
            nd_manifest_free(m);
        }
    }
}

static void test_the_derived_size_saturates_instead_of_overflowing(void)
{
    /* Python multiplies two arbitrary-precision integers here; C would
     * overflow. A manifest asking for 2^62 blocks of 4096 bytes is asking for
     * sixteen million times the flash, and it must not be able to wrap the
     * arithmetic into a small, plausible-looking number. */
    mf f = mf_good();
    char buf[4096];
    nd_manifest *m;

    f.image_blocks = "4611686018427387904";
    (void)mf_json(buf, sizeof buf, &f);
    m = parse_ok(buf);
    if (m == NULL)
        return;
    CHECK(nd_manifest_image_bytes(m) == UINT64_MAX);
    nd_manifest_free(m);
}

static void test_oversized_fields_are_refused_not_truncated(void)
{
    /* A truncated version string reaches pending.prop and a truncated salt
     * builds the wrong dm-verity table, so neither may be silently shortened.
     * Python has no bound here at all. */
    mf f;
    char big[1200];
    char quoted[1300];

    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';

    f = mf_good();
    (void)snprintf(quoted, sizeof quoted, "\"%.*s\"", 200, big);
    f.version = quoted;
    reject_mf(&f, "version is longer than");

    f = mf_good();
    f.platform = quoted;
    reject_mf(&f, "platform is longer than");

    f = mf_good();
    f.min_kernel = quoted;
    reject_mf(&f, "min_kernel is longer than");

    /* A hash that is 64 valid hex digits but padded with whitespace passes
     * bytes.fromhex and then does not fit the field it is stored in. */
    f = mf_good();
    f.sha256 = "\"aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa "
               "aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa\"";
    reject_mf(&f, "sha256 is longer than");

    /* verity.py caps the salt at 256 bytes; 1024 hex digits is four times
     * that and is refused rather than cut in half. */
    f = mf_good();
    memset(big, 'a', sizeof big - 1u);
    big[1024] = '\0';
    (void)snprintf(quoted, sizeof quoted, "\"%s\"", big);
    f.salt = quoted;
    reject_mf(&f, "salt is longer than");
}

static void test_changelog_and_min_kernel_must_be_strings(void)
{
    /* DIVERGENCE: Python keeps whatever truthy object it finds and str()s it
     * later. For min_kernel that decides an install, so a non-string is a
     * refusal here rather than a silent "" (which would install) or a silent
     * str() (which would need a formatter this module does not have). */
    mf f;

    f = mf_good();
    f.min_kernel = "99";
    reject_mf(&f, "min_kernel must be a string");
    f = mf_good();
    f.changelog = "[1, 2]";
    reject_mf(&f, "changelog must be a string");

    /* Falsy non-strings still collapse to "", exactly as `or ""` does. */
    f = mf_good();
    f.changelog = "null";
    {
        char buf[4096];
        nd_manifest *m;

        (void)mf_json(buf, sizeof buf, &f);
        m = parse_ok(buf);
        if (m != NULL) {
            CHECK_STR(m->changelog, "");
            nd_manifest_free(m);
        }
    }
}

static void test_verity_must_be_an_object(void)
{
    mf f = mf_good();

    f.verity = "\"nope\"";
    f.root_hash = NULL;
    f.block_size = NULL;
    f.image_blocks = NULL;
    f.salt = NULL;
    reject_mf(&f, "verity must be a JSON object");

    f.verity = "null";
    reject_mf(&f, "verity must be a JSON object");

    f.verity = "[]";
    reject_mf(&f, "verity must be a JSON object");
}

static void test_empty_version_and_platform_are_refused(void)
{
    mf f;

    f = mf_good();
    f.version = "\"\"";
    reject_mf(&f, "version must be a non-empty string");
    f = mf_good();
    f.version = "12";
    reject_mf(&f, "version must be a non-empty string");
    f = mf_good();
    f.platform = "\"\"";
    reject_mf(&f, "platform must be a non-empty string");
    f = mf_good();
    f.platform = "null";
    reject_mf(&f, "platform must be a non-empty string");
}

static void test_version_tuples(void)
{
    struct {
        const char *text;
        uint8_t n;
        uint32_t part[4];
    } cases[] = {
        {"6.12.47", 3u, {6u, 12u, 47u, 0u}},
        {"6.12.47-rt", 3u, {6u, 12u, 47u, 0u}},  /* trailing junk ignored     */
        {"6.x.3", 1u, {6u, 0u, 0u, 0u}},         /* a chunk with no digits    */
        {"", 0u, {0u, 0u, 0u, 0u}},              /* STOPS the walk entirely   */
        {"x.1", 0u, {0u, 0u, 0u, 0u}},
        {"5", 1u, {5u, 0u, 0u, 0u}},
        {"0.0.0", 3u, {0u, 0u, 0u, 0u}},
    };
    size_t i;
    nd_kver a;
    nd_kver b;

    for (i = 0u; i < ND_ARRAY_LEN(cases); i++) {
        size_t j;

        nd_kver_parse(cases[i].text, &a);
        CHECK_INT(a.n, cases[i].n);
        for (j = 0u; j < a.n && j < 4u; j++)
            CHECK_INT(a.part[j], cases[i].part[j]);
    }

    /* Python tuple ordering: a prefix is smaller than what it is a prefix
     * of, so a kernel of "6.12" does not satisfy a demand for "6.12.47". */
    nd_kver_parse("6.12", &a);
    nd_kver_parse("6.12.47", &b);
    CHECK_INT(nd_kver_cmp(&a, &b), -1);
    CHECK_INT(nd_kver_cmp(&b, &a), 1);
    CHECK_INT(nd_kver_cmp(&a, &a), 0);

    nd_kver_parse("6.9.0", &a);
    nd_kver_parse("6.12.0", &b);
    CHECK_INT(nd_kver_cmp(&a, &b), -1); /* numeric, not lexical */

    /* Neither the component count nor a component's magnitude can overflow
     * anything. Ninth and later components are dropped; a component past
     * UINT32_MAX saturates. */
    nd_kver_parse("1.2.3.4.5.6.7.8.9.10", &a);
    CHECK_INT(a.n, ND_KVER_MAX_PARTS);
    nd_kver_parse("999999999999999999999.1", &a);
    CHECK_INT(a.n, 2);
    CHECK(a.part[0] == UINT32_MAX);
    nd_kver_parse(NULL, &a);
    CHECK_INT(a.n, 0);
}

static void test_free_tolerates_null(void)
{
    nd_manifest_free(NULL);
    CHECK(nd_manifest_hash_offset(NULL) == 0u);
    CHECK(nd_manifest_image_bytes(NULL) == 0u);
    CHECK_INT(nd_manifest_check_compatible(NULL, "p", NULL, NULL, 0u), ND_UPD_ERR_BAD_MANIFEST);
    CHECK_INT(nd_manifest_parse(NULL, 0u, NULL, NULL, 0u), ND_UPD_ERR_BAD_MANIFEST);
}

/* ------------------------------------------------------------------ *
 * --parse: the cross-check against the real manifest.py
 * ------------------------------------------------------------------ *
 *
 * neodct/tests/test_c_manifest_matches_python.py feeds the same bytes to both
 * and compares field by field, and the refusal word for word. The escaping is
 * the same as test_package.c's --dump: a changelog holds newlines.
 */

static void put_field(const char *key, const char *value)
{
    printf("mf\t%s\t", key);
    for (; *value != '\0'; value++) {
        switch (*value) {
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        default:
            putchar((int)(unsigned char)*value);
            break;
        }
    }
    putchar('\n');
}

static void put_num(const char *key, long long value)
{
    printf("mf\t%s\t%lld\n", key, value);
}

/* image_bytes saturates at UINT64_MAX; printed signed it would read -1 and
 * the cross-check would be comparing the wrong thing. */
static void put_unum(const char *key, unsigned long long value)
{
    printf("mf\t%s\t%llu\n", key, value);
}

static size_t slurp(const char *path, uint8_t *out, size_t out_sz, bool *ok)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    *ok = false;
    if (f == NULL)
        return 0u;
    n = fread(out, 1u, out_sz, f);
    (void)fclose(f);
    *ok = true;
    return n;
}

static int parse_file(const char *path)
{
    static uint8_t raw[1u << 21];
    nd_manifest *m = NULL;
    char why[ND_MANIFEST_WHY_MAX];
    bool ok = false;
    size_t n = slurp(path, raw, sizeof raw, &ok);

    if (!ok) {
        printf("parse\tERR\n");
        put_field("why", "cannot open the file");
        return 0;
    }
    why[0] = '\0';
    if (nd_manifest_parse(raw, n, &m, why, sizeof why) != ND_UPD_OK) {
        printf("parse\tERR\n");
        put_field("why", why);
        return 0;
    }
    printf("parse\tOK\n");
    put_field("version", m->version);
    put_num("buildtime", (long long)m->buildtime);
    put_field("platform", m->platform);
    put_field("sha256", m->sha256);
    put_field("changelog", m->changelog);
    put_field("min_kernel", m->min_kernel);
    put_field("thumbnail_sha256", m->thumbnail_sha256);
    put_field("root_hash", m->verity_root_hash);
    put_num("block_size", (long long)m->verity_block_size);
    put_num("image_blocks", (long long)m->verity_image_blocks);
    put_field("salt", m->verity_salt);
    put_unum("image_bytes", (unsigned long long)nd_manifest_image_bytes(m));
    put_num("raw_len", (long long)m->raw_len);
    nd_manifest_free(m);
    return 0;
}

static int compat_file(const char *path, const char *platform, const char *kernel)
{
    static uint8_t raw[1u << 21];
    nd_manifest *m = NULL;
    char why[ND_MANIFEST_WHY_MAX];
    bool ok = false;
    size_t n = slurp(path, raw, sizeof raw, &ok);
    nd_update_err rc;

    if (!ok) {
        printf("compat\tPARSE_ERR\n");
        return 0;
    }
    why[0] = '\0';
    if (nd_manifest_parse(raw, n, &m, why, sizeof why) != ND_UPD_OK) {
        printf("compat\tPARSE_ERR\n");
        return 0;
    }
    why[0] = '\0';
    rc = nd_manifest_check_compatible(m, platform, kernel, why, sizeof why);
    printf("compat\t%s\n", rc == ND_UPD_OK ? "OK" : "ERR");
    put_field("why", why);
    nd_manifest_free(m);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--parse") == 0)
        return parse_file(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "--compat") == 0)
        return compat_file(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);

    RUN(test_parses_a_well_formed_manifest);
    RUN(test_keeps_the_exact_bytes_it_was_given);
    RUN(test_derives_the_hash_offset_from_the_image_size);
    RUN(test_changelog_is_optional);
    RUN(test_missing_required_field_is_an_invalid_update);
    RUN(test_missing_verity_field_is_an_invalid_update);
    RUN(test_verity_salt_may_be_absent);
    RUN(test_rejects_a_non_hex_image_hash);
    RUN(test_rejects_a_non_hex_root_hash);
    RUN(test_rejects_a_non_numeric_buildtime);
    RUN(test_rejects_a_block_size_that_is_not_a_power_of_two);
    RUN(test_rejects_malformed_json);
    RUN(test_rejects_a_json_document_that_is_not_an_object);
    RUN(test_accepts_a_matching_platform);
    RUN(test_refuses_an_update_built_for_another_platform);
    RUN(test_refuses_an_update_that_needs_a_newer_kernel);
    RUN(test_accepts_an_update_whose_kernel_requirement_is_met);
    RUN(test_kernel_requirement_is_ignored_when_absent);
    RUN(test_a_thumbnail_hash_is_carried_through);
    RUN(test_a_manifest_without_a_thumbnail_reports_no_hash);
    RUN(test_rejects_a_thumbnail_hash_that_is_not_a_sha256);
    RUN(test_rejects_a_thumbnail_hash_that_is_not_hex);
    RUN(test_hex_follows_bytes_fromhex_to_the_letter);
    RUN(test_an_integer_is_not_a_float_and_a_bool_is_not_a_number);
    RUN(test_the_verity_geometry_bounds);
    RUN(test_the_derived_size_saturates_instead_of_overflowing);
    RUN(test_oversized_fields_are_refused_not_truncated);
    RUN(test_changelog_and_min_kernel_must_be_strings);
    RUN(test_verity_must_be_an_object);
    RUN(test_empty_version_and_platform_are_refused);
    RUN(test_version_tuples);
    RUN(test_free_tolerates_null);
    return pt_report("test_manifest");
}
