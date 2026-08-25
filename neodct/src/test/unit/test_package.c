/* test_package.c -- the .ndsw reader.
 *
 * The positive cases mirror neodct/tests/test_update_package.py. The negative
 * ones are the point of the file: this is the code that reads an
 * attacker-supplied file off a FAT32 card and hands its contents to `dd`, so
 * what matters is not that a good package opens but that every shape of a bad
 * one is refused rather than survived.
 *
 * Every fixture is built here, byte by byte, rather than shelled out to
 * Python. That is deliberate twice over: `make test` has to pass with no
 * arguments on a machine with no Python and no fixtures, and a zip built by a
 * generator that shares no code with the reader is the only way to write a
 * local-header-name mismatch or a member that lies about its own size --
 * zipfile will not emit either.
 *
 * The cross-check against the real Python is a separate, reproducible thing:
 *
 *     build/default/test/test_package --dump PACKAGE.ndsw
 *
 * prints what this reader sees, and neodct/tests/test_c_package_matches_python.py
 * asserts it against what System/core/UpdateService/package.py sees for the
 * same file, over packages built by mkupdate.py and broken by mkbadupdate.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "nd_manifest.h"
#include "nd_package.h"
#include "nd_paths.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * A growable byte buffer, and a zip writer built on it
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t *p;
    size_t n;
    size_t cap;
} buf;

static void buf_free(buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = 0u;
    b->cap = 0u;
}

static void buf_put(buf *b, const void *data, size_t n)
{
    if (b->n + n > b->cap) {
        size_t want = (b->cap == 0u ? 1024u : b->cap);
        uint8_t *bigger;

        while (want < b->n + n)
            want *= 2u;
        bigger = realloc(b->p, want);
        if (bigger == NULL) {
            fprintf(stderr, "out of memory building a fixture\n");
            exit(1);
        }
        b->p = bigger;
        b->cap = want;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void buf_u16(buf *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)(v & 0xffu), (uint8_t)(v >> 8)};

    buf_put(b, x, sizeof x);
}

static void buf_u32(buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v & 0xffu), (uint8_t)((v >> 8) & 0xffu), (uint8_t)((v >> 16) & 0xffu),
                    (uint8_t)(v >> 24)};

    buf_put(b, x, sizeof x);
}

static void buf_u64(buf *b, uint64_t v)
{
    buf_u32(b, (uint32_t)(v & 0xffffffffu));
    buf_u32(b, (uint32_t)(v >> 32));
}

/* Raw deflate, exactly what a zip member holds -- no zlib header. */
static uint8_t *raw_deflate(const uint8_t *data, size_t len, size_t *out_len)
{
    z_stream zs;
    size_t cap = len + len / 2u + 128u;
    uint8_t *out = malloc(cap);

    if (out == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        fprintf(stderr, "deflateInit2 failed\n");
        exit(1);
    }
    zs.next_in = (Bytef *)(uintptr_t)(const void *)data;
    zs.avail_in = (uInt)len;
    zs.next_out = out;
    zs.avail_out = (uInt)cap;
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        fprintf(stderr, "deflate did not finish\n");
        exit(1);
    }
    *out_len = cap - (size_t)zs.avail_out;
    (void)deflateEnd(&zs);
    return out;
}

/* One member, plus every knob a malformed-package case needs. A zero-filled
 * zmember is "sane", and each test turns on exactly the one thing it is
 * about. */
typedef struct {
    const char *name;
    const uint8_t *data;
    size_t len;
    bool deflate;

    const char *local_name; /* NULL: the same name in both headers        */
    uint16_t extra_flags;   /* OR'ed into the general purpose bit flag    */
    int32_t method;         /* -1: as chosen by `deflate`                 */
    int64_t csize;          /* -1: the real compressed size               */
    int64_t usize;          /* -1: the real uncompressed size             */
    int64_t crc;            /* -1: the real CRC-32                        */
    int64_t truncate_data;  /* -1: write it all; else this many bytes     */
} zmember;

typedef struct {
    bool zip64;         /* saturate the 32-bit fields and add the records */
    bool break_cd_sig;  /* corrupt the first central directory signature  */
    bool break_lfh_sig; /* corrupt the first local header signature       */
    int64_t cd_off;     /* -1: the real offset                            */
    int64_t entries;    /* -1: the real count                             */
    size_t chop;        /* bytes to cut off the end of the finished file  */
} zopts;

/* Builds the archive into `out`. Layout is the ordinary one: every member's
 * local header and data, then the central directory, then (for zip64) the
 * zip64 record and its locator, then the end-of-central-directory record. */
static void zip_build(buf *out, const zmember *ms, size_t n, const zopts *opt)
{
    uint64_t lho[128];
    uint32_t crcs[128];
    uint64_t csizes[128];
    uint64_t usizes[128];
    uint16_t methods[128];
    uint64_t cd_start;
    uint64_t cd_size;
    uint64_t eocd_at;
    size_t i;

    if (n > 128u) {
        fprintf(stderr, "too many fixture members\n");
        exit(1);
    }

    for (i = 0u; i < n; i++) {
        const zmember *m = &ms[i];
        uint8_t *payload = (uint8_t *)(uintptr_t)(const void *)m->data;
        size_t payload_len = m->len;
        uint8_t *owned = NULL;
        size_t write_len;

        if (m->deflate) {
            owned = raw_deflate(m->data, m->len, &payload_len);
            payload = owned;
        }
        methods[i] = (uint16_t)(m->method >= 0 ? m->method : (m->deflate ? 8 : 0));
        crcs[i] = m->crc >= 0 ? (uint32_t)m->crc
                              : (uint32_t)crc32(crc32(0uL, NULL, 0u), m->data, (uInt)m->len);
        csizes[i] = m->csize >= 0 ? (uint64_t)m->csize : (uint64_t)payload_len;
        usizes[i] = m->usize >= 0 ? (uint64_t)m->usize : (uint64_t)m->len;

        lho[i] = out->n;
        buf_u32(out, 0x04034b50u);
        buf_u16(out, 20u); /* version needed */
        buf_u16(out, m->extra_flags);
        buf_u16(out, methods[i]);
        buf_u16(out, 0u); /* time */
        buf_u16(out, 0u); /* date */
        buf_u32(out, crcs[i]);
        buf_u32(out, (uint32_t)(csizes[i] & 0xffffffffu));
        buf_u32(out, (uint32_t)(usizes[i] & 0xffffffffu));
        {
            const char *ln = m->local_name != NULL ? m->local_name : m->name;

            buf_u16(out, (uint16_t)strlen(ln));
            buf_u16(out, 0u); /* extra len */
            buf_put(out, ln, strlen(ln));
        }
        write_len = m->truncate_data >= 0 ? (size_t)m->truncate_data : payload_len;
        if (write_len > payload_len)
            write_len = payload_len;
        buf_put(out, payload, write_len);
        free(owned);
    }
    if (opt->break_lfh_sig && n > 0u)
        out->p[lho[0]] = 0xffu;

    cd_start = out->n;
    for (i = 0u; i < n; i++) {
        const zmember *m = &ms[i];
        size_t sig_at = out->n;

        buf_u32(out, 0x02014b50u);
        buf_u16(out, 20u); /* version made by */
        buf_u16(out, 20u); /* version needed  */
        buf_u16(out, m->extra_flags);
        buf_u16(out, methods[i]);
        buf_u16(out, 0u);
        buf_u16(out, 0u);
        buf_u32(out, crcs[i]);
        if (opt->zip64) {
            buf_u32(out, 0xffffffffu);
            buf_u32(out, 0xffffffffu);
        } else {
            buf_u32(out, (uint32_t)(csizes[i] & 0xffffffffu));
            buf_u32(out, (uint32_t)(usizes[i] & 0xffffffffu));
        }
        buf_u16(out, (uint16_t)strlen(m->name));
        buf_u16(out, opt->zip64 ? 28u : 0u); /* extra len */
        buf_u16(out, 0u);                    /* comment len */
        buf_u16(out, 0u);                    /* disk start */
        buf_u16(out, 0u);                    /* internal attrs */
        buf_u32(out, 0u);                    /* external attrs */
        buf_u32(out, opt->zip64 ? 0xffffffffu : (uint32_t)(lho[i] & 0xffffffffu));
        buf_put(out, m->name, strlen(m->name));
        if (opt->zip64) {
            buf_u16(out, 0x0001u);
            buf_u16(out, 24u);
            buf_u64(out, usizes[i]);
            buf_u64(out, csizes[i]);
            buf_u64(out, lho[i]);
        }
        if (opt->break_cd_sig && i == 0u)
            out->p[sig_at] = 0xffu;
    }
    cd_size = out->n - cd_start;

    if (opt->zip64) {
        uint64_t z64_at = out->n;

        buf_u32(out, 0x06064b50u);
        buf_u64(out, 44u); /* size of the record after this field */
        buf_u16(out, 45u);
        buf_u16(out, 45u);
        buf_u32(out, 0u);
        buf_u32(out, 0u);
        buf_u64(out, (uint64_t)n);
        buf_u64(out, (uint64_t)n);
        buf_u64(out, cd_size);
        buf_u64(out, cd_start);

        buf_u32(out, 0x07064b50u);
        buf_u32(out, 0u);
        buf_u64(out, z64_at);
        buf_u32(out, 1u);
    }

    eocd_at = out->n;
    ND_UNUSED(eocd_at);
    buf_u32(out, 0x06054b50u);
    buf_u16(out, 0u);
    buf_u16(out, 0u);
    buf_u16(out, opt->zip64 ? 0xffffu
                            : (uint16_t)(opt->entries >= 0 ? (uint16_t)opt->entries : (uint16_t)n));
    buf_u16(out, opt->zip64 ? 0xffffu
                            : (uint16_t)(opt->entries >= 0 ? (uint16_t)opt->entries : (uint16_t)n));
    buf_u32(out, opt->zip64 ? 0xffffffffu : (uint32_t)cd_size);
    buf_u32(out, opt->zip64 ? 0xffffffffu
                            : (uint32_t)(opt->cd_off >= 0 ? (uint64_t)opt->cd_off : cd_start));
    buf_u16(out, 0u); /* comment length */

    if (opt->chop > 0u)
        out->n = opt->chop >= out->n ? 0u : out->n - opt->chop;
}

/* ------------------------------------------------------------------ *
 * Package fixtures
 * ------------------------------------------------------------------ */

#define IMAGE_BLOCKS 4u
#define BLOCK_SIZE   4096u

static uint8_t g_image[IMAGE_BLOCKS * BLOCK_SIZE];
static uint8_t g_thumb[512];
static char g_image_sha[ND_MANIFEST_HEX_MAX];
static char g_thumb_sha[ND_MANIFEST_HEX_MAX];

static void fixtures_init(void)
{
    size_t i;

    for (i = 0u; i < sizeof g_image; i++)
        g_image[i] = (uint8_t)((i * 37u + (i >> 5)) & 0xffu);
    for (i = 0u; i < sizeof g_thumb; i++)
        g_thumb[i] = (uint8_t)(i & 0x7fu);
    nd_package_sha256_hex(g_image, sizeof g_image, g_image_sha, sizeof g_image_sha);
    nd_package_sha256_hex(g_thumb, sizeof g_thumb, g_thumb_sha, sizeof g_thumb_sha);
}

/* mkupdate.py's manifest, with the fields this fixture can vary. */
static void manifest_text(char *out, size_t out_sz, const char *image_sha, const char *thumb_sha,
                          const char *platform)
{
    char thumb_line[128];

    thumb_line[0] = '\0';
    if (thumb_sha != NULL && thumb_sha[0] != '\0')
        (void)snprintf(thumb_line, sizeof thumb_line, "  \"thumbnail_sha256\": \"%s\",\n",
                       thumb_sha);
    (void)snprintf(out, out_sz,
                   "{\n"
                   "  \"buildtime\": 1785160800,\n"
                   "  \"changelog\": \"Fixed SMS database sorting bug.\",\n"
                   "  \"platform\": \"%s\",\n"
                   "  \"sha256\": \"%s\",\n"
                   "%s"
                   "  \"verity\": {\n"
                   "    \"block_size\": %u,\n"
                   "    \"image_blocks\": %u,\n"
                   "    \"root_hash\": \"%s\",\n"
                   "    \"salt\": \"f8e7d6c5b4a3\"\n"
                   "  },\n"
                   "  \"version\": \"0.3.2a\"\n"
                   "}\n",
                   platform, image_sha, thumb_line, BLOCK_SIZE, IMAGE_BLOCKS,
                   "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
}

/* Writes a package at a VIRTUAL path. Returns the manifest text it used, so a
 * test can check the raw bytes came back byte for byte. */
static void write_package(const char *vpath, const zmember *ms, size_t n, const zopts *opt)
{
    buf b;

    memset(&b, 0, sizeof b);
    zip_build(&b, ms, n, opt);
    pt_write(vpath, b.p, b.n);
    buf_free(&b);
}

static const zopts SANE = {false, false, false, -1, -1, 0u};

/* The ordinary four-member package: the image STORED (a squashfs is already
 * compressed) and everything else DEFLATED, which is what mkupdate.py does. */
static size_t standard_members(zmember *ms, const char *manifest, size_t manifest_len,
                               bool with_thumb, bool with_sig)
{
    static const uint8_t SIGNATURE[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    size_t n = 0u;

    memset(ms, 0, sizeof(zmember) * 4u);
    ms[n].name = ND_PACKAGE_IMAGE_MEMBER;
    ms[n].data = g_image;
    ms[n].len = sizeof g_image;
    ms[n].deflate = false;
    ms[n].method = -1;
    ms[n].csize = -1;
    ms[n].usize = -1;
    ms[n].crc = -1;
    ms[n].truncate_data = -1;
    n++;
    if (with_thumb) {
        ms[n].name = ND_PACKAGE_THUMBNAIL_MEMBER;
        ms[n].data = g_thumb;
        ms[n].len = sizeof g_thumb;
        ms[n].deflate = true;
        ms[n].method = -1;
        ms[n].csize = -1;
        ms[n].usize = -1;
        ms[n].crc = -1;
        ms[n].truncate_data = -1;
        n++;
    }
    ms[n].name = ND_PACKAGE_MANIFEST_MEMBER;
    ms[n].data = (const uint8_t *)manifest;
    ms[n].len = manifest_len;
    ms[n].deflate = true;
    ms[n].method = -1;
    ms[n].csize = -1;
    ms[n].usize = -1;
    ms[n].crc = -1;
    ms[n].truncate_data = -1;
    n++;
    if (with_sig) {
        ms[n].name = ND_PACKAGE_SIGNATURE_MEMBER;
        ms[n].data = SIGNATURE;
        ms[n].len = sizeof SIGNATURE;
        ms[n].deflate = true;
        ms[n].method = -1;
        ms[n].csize = -1;
        ms[n].usize = -1;
        ms[n].crc = -1;
        ms[n].truncate_data = -1;
        n++;
    }
    return n;
}

/* The layout depends on whether there is a thumbnail, and a fixture that
 * tampers with "index 2" instead of "the manifest" is a test that silently
 * stops testing what it says it does. */
static size_t member_index(const zmember *ms, size_t n, const char *name)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (strcmp(ms[i].name, name) == 0)
            return i;
    }
    fprintf(stderr, "fixture has no %s\n", name);
    exit(1);
}

#define MANIFEST_AT  member_index(ms, n, ND_PACKAGE_MANIFEST_MEMBER)
#define SIGNATURE_AT member_index(ms, n, ND_PACKAGE_SIGNATURE_MEMBER)
#define IMAGE_AT     member_index(ms, n, ND_PACKAGE_IMAGE_MEMBER)

/* A member with every knob at its "sane" setting. */
static void init_member(zmember *m, const char *name, const void *data, size_t len, bool deflate)
{
    memset(m, 0, sizeof *m);
    m->name = name;
    m->data = data;
    m->len = len;
    m->deflate = deflate;
    m->method = -1;
    m->csize = -1;
    m->usize = -1;
    m->crc = -1;
    m->truncate_data = -1;
}

static size_t good_package(const char *vpath, char *manifest, size_t manifest_sz, bool with_thumb)
{
    zmember ms[4];
    size_t n;

    manifest_text(manifest, manifest_sz, g_image_sha, with_thumb ? g_thumb_sha : NULL,
                  "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), with_thumb, true);
    write_package(vpath, ms, n, &SANE);
    return strlen(manifest);
}

/* ------------------------------------------------------------------ *
 * Assertions
 * ------------------------------------------------------------------ */

static nd_package *open_ok(const char *vpath)
{
    nd_package *pkg = NULL;
    char why[ND_PACKAGE_WHY_MAX];

    why[0] = '\0';
    g_checks++;
    if (nd_package_open(vpath, &pkg, why, sizeof why) != ND_UPD_OK || pkg == NULL) {
        g_failures++;
        fprintf(stderr, "FAIL open of %s: %s\n", vpath, why);
        return NULL;
    }
    return pkg;
}

static void refuse_open(const char *vpath, nd_update_err want, const char *wanted_text)
{
    nd_package *pkg = (nd_package *)0x1;
    char why[ND_PACKAGE_WHY_MAX];
    nd_update_err rc;

    why[0] = '\0';
    rc = nd_package_open(vpath, &pkg, why, sizeof why);
    g_checks++;
    if (rc == ND_UPD_OK) {
        g_failures++;
        fprintf(stderr, "FAIL %s should have been refused\n", vpath);
        nd_package_close(pkg);
        return;
    }
    g_checks++;
    if (pkg != NULL) {
        g_failures++;
        fprintf(stderr, "FAIL %s refused but left a package behind\n", vpath);
    }
    g_checks++;
    if (rc != want) {
        g_failures++;
        fprintf(stderr, "FAIL %s refused with %d, wanted %d (%s)\n", vpath, (int)rc, (int)want,
                why);
    }
    if (wanted_text != NULL) {
        g_checks++;
        if (strstr(why, wanted_text) == NULL) {
            g_failures++;
            fprintf(stderr, "FAIL %s refused with \"%s\", wanted it to name \"%s\"\n", vpath, why,
                    wanted_text);
        }
    }
}

/* ------------------------------------------------------------------ *
 * sha256, before anything trusts it
 * ------------------------------------------------------------------ */

static void test_sha256_known_answers(void)
{
    char hex[ND_MANIFEST_HEX_MAX];
    uint8_t big[1000];
    size_t i;

    nd_package_sha256_hex("", 0u, hex, sizeof hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    nd_package_sha256_hex("abc", 3u, hex, sizeof hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    nd_package_sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u, hex,
                          sizeof hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* 55, 56 and 64 bytes are the three lengths where the padding block
     * boundary is decided, and they are where a hand-written sha256 is
     * wrong. */
    for (i = 0u; i < sizeof big; i++)
        big[i] = (uint8_t)'a';
    nd_package_sha256_hex(big, 55u, hex, sizeof hex);
    CHECK_STR(hex, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    nd_package_sha256_hex(big, 56u, hex, sizeof hex);
    CHECK_STR(hex, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    nd_package_sha256_hex(big, 64u, hex, sizeof hex);
    CHECK_STR(hex, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    nd_package_sha256_hex(big, 1000u, hex, sizeof hex);
    CHECK_STR(hex, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

/* ------------------------------------------------------------------ *
 * The good path
 * ------------------------------------------------------------------ */

static void test_reads_the_manifest_out_of_a_package(void)
{
    char manifest[2048];
    size_t manifest_len = 0u;
    nd_package *pkg;
    const nd_manifest *m;
    const uint8_t *raw;
    size_t raw_len = 0u;

    manifest_len = good_package("/UPDATE.ndsw", manifest, sizeof manifest, false);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;

    m = nd_package_manifest(pkg);
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK_STR(m->version, "0.3.2a");
        CHECK_STR(m->platform, "qemu-aarch64");
        CHECK_STR(m->sha256, g_image_sha);
    }
    CHECK_STR(nd_package_path(pkg), "/UPDATE.ndsw");
    CHECK_INT(nd_package_image_size(pkg), (int64_t)sizeof g_image);
    CHECK(!nd_package_is_signed(pkg));

    /* The signature is over the manifest bytes EXACTLY AS STORED. */
    raw = nd_package_manifest_raw(pkg, &raw_len);
    CHECK_INT(raw_len, manifest_len);
    CHECK(raw != NULL && memcmp(raw, manifest, manifest_len) == 0);

    nd_package_mark_signed(pkg);
    CHECK(nd_package_is_signed(pkg));
    nd_package_close(pkg);
}

static void test_both_compression_methods_are_mandatory(void)
{
    /* A real package STORES rootfs.squashfs and DEFLATES the rest; the Python
     * test fixtures deflate everything. A reader that supports one method
     * fails half the suite. */
    char manifest[2048];
    zmember ms[4];
    size_t n;
    nd_package *pkg;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");

    n = standard_members(ms, manifest, strlen(manifest), false, true);
    write_package("/stored.ndsw", ms, n, &SANE);
    pkg = open_ok("/stored.ndsw");
    if (pkg != NULL) {
        CHECK_INT(nd_package_member_method(pkg, ND_PACKAGE_IMAGE_MEMBER), 0);
        CHECK_INT(nd_package_member_method(pkg, ND_PACKAGE_MANIFEST_MEMBER), 8);
        nd_package_close(pkg);
    }

    /* Everything deflated, as update_fixtures.py writes them. */
    ms[0].deflate = true;
    write_package("/deflated.ndsw", ms, n, &SANE);
    pkg = open_ok("/deflated.ndsw");
    if (pkg != NULL) {
        CHECK_INT(nd_package_member_method(pkg, ND_PACKAGE_IMAGE_MEMBER), 8);
        CHECK_INT(nd_package_image_size(pkg), (int64_t)sizeof g_image);
        nd_package_close(pkg);
    }
}

static void test_member_listing(void)
{
    char manifest[2048];
    nd_package *pkg;

    (void)good_package("/UPDATE.ndsw", manifest, sizeof manifest, true);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    CHECK_INT(nd_package_member_count(pkg), 4);
    CHECK_STR(nd_package_member_name(pkg, 0u), ND_PACKAGE_IMAGE_MEMBER);
    CHECK_STR(nd_package_member_name(pkg, 1u), ND_PACKAGE_THUMBNAIL_MEMBER);
    CHECK_STR(nd_package_member_name(pkg, 2u), ND_PACKAGE_MANIFEST_MEMBER);
    CHECK_STR(nd_package_member_name(pkg, 3u), ND_PACKAGE_SIGNATURE_MEMBER);
    CHECK(nd_package_member_name(pkg, 4u) == NULL);
    CHECK_INT(nd_package_member_size(pkg, ND_PACKAGE_THUMBNAIL_MEMBER), (int64_t)sizeof g_thumb);
    CHECK_INT(nd_package_member_size(pkg, "nothing.txt"), -1);
    nd_package_close(pkg);
}

static void test_reads_the_signature_member(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;
    nd_package *pkg;
    uint8_t *sig = NULL;
    size_t sig_len = 0u;
    char why[ND_PACKAGE_WHY_MAX];

    (void)good_package("/UPDATE.ndsw", manifest, sizeof manifest, false);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg != NULL) {
        why[0] = '\0';
        CHECK_INT(nd_package_read_signature(pkg, &sig, &sig_len, why, sizeof why), ND_UPD_OK);
        CHECK_INT(sig_len, 8);
        CHECK(sig != NULL && sig[0] == 1u && sig[7] == 8u);
        free(sig);
        nd_package_close(pkg);
    }

    /* An unsigned package is a BAD SIGNATURE, not an invalid one: engineering
     * mode may override the first and may never override the second. */
    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, false);
    write_package("/unsigned.ndsw", ms, n, &SANE);
    pkg = open_ok("/unsigned.ndsw");
    if (pkg != NULL) {
        why[0] = '\0';
        sig = NULL;
        CHECK_INT(nd_package_read_signature(pkg, &sig, &sig_len, why, sizeof why),
                  ND_UPD_ERR_BAD_SIGNATURE);
        CHECK_STR(why, "update is not signed");
        CHECK(sig == NULL);
        nd_package_close(pkg);
    }
}

/* ------------------------------------------------------------------ *
 * read_thumbnail
 * ------------------------------------------------------------------ */

static void test_reads_a_thumbnail_out_of_a_package(void)
{
    char manifest[2048];
    nd_package *pkg;
    uint8_t *art = NULL;
    size_t len = 0u;
    char why[ND_PACKAGE_WHY_MAX];

    (void)good_package("/UPDATE.ndsw", manifest, sizeof manifest, true);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_package_read_thumbnail(pkg, &art, &len, why, sizeof why), ND_UPD_OK);
    CHECK_INT(len, (int64_t)sizeof g_thumb);
    CHECK(art != NULL && memcmp(art, g_thumb, sizeof g_thumb) == 0);
    free(art);
    nd_package_close(pkg);
}

static void test_a_package_without_a_thumbnail_has_none(void)
{
    char manifest[2048];
    nd_package *pkg;
    uint8_t *art = (uint8_t *)0x1;
    size_t len = 99u;
    char why[ND_PACKAGE_WHY_MAX];

    (void)good_package("/UPDATE.ndsw", manifest, sizeof manifest, false);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_package_read_thumbnail(pkg, &art, &len, why, sizeof why), ND_UPD_OK);
    CHECK(art == NULL);
    CHECK_INT(len, 0);
    nd_package_close(pkg);
}

static void test_an_undeclared_thumbnail_is_ignored(void)
{
    /* "Art nobody signed for is not art we display." The signature covers
     * manifest.json alone, so a picture with no hash recorded there is an
     * unsigned attachment. */
    char manifest[2048];
    zmember ms[4];
    size_t n;
    nd_package *pkg;
    uint8_t *art = (uint8_t *)0x1;
    size_t len = 99u;
    char why[ND_PACKAGE_WHY_MAX];

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), true, true);
    write_package("/UPDATE.ndsw", ms, n, &SANE);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    CHECK_INT(nd_package_member_count(pkg), 4); /* the member IS there */
    why[0] = '\0';
    CHECK_INT(nd_package_read_thumbnail(pkg, &art, &len, why, sizeof why), ND_UPD_OK);
    CHECK(art == NULL);
    nd_package_close(pkg);
}

static void test_a_thumbnail_that_does_not_match_its_hash_is_invalid(void)
{
    /* Swapping the picture after signing changes nothing the signature
     * covers, so the manifest's hash is what has to catch it. */
    char manifest[2048];
    zmember ms[4];
    size_t n;
    nd_package *pkg;
    uint8_t *art = (uint8_t *)0x1;
    size_t len = 0u;
    char why[ND_PACKAGE_WHY_MAX];
    uint8_t other[16];

    memset(other, 0x5au, sizeof other);
    manifest_text(manifest, sizeof manifest, g_image_sha, g_thumb_sha, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), true, true);
    ms[1].data = other; /* a different picture, same declared hash */
    ms[1].len = sizeof other;
    write_package("/UPDATE.ndsw", ms, n, &SANE);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_package_read_thumbnail(pkg, &art, &len, why, sizeof why), ND_UPD_ERR_BAD_MANIFEST);
    CHECK_STR(why, "thumbnail does not match the manifest");
    CHECK(art == NULL);
    nd_package_close(pkg);
}

static void test_an_oversized_thumbnail_is_refused_before_it_is_read(void)
{
    /* 64 MB of RAM: a "thumbnail" the size of a photo album never gets
     * decompressed, whatever the manifest claims about it. The member is
     * declared over the cap and its data is a run of zeros that deflates to
     * almost nothing -- which is exactly the shape the cap exists for. */
    char manifest[2048];
    zmember ms[4];
    size_t n;
    nd_package *pkg;
    uint8_t *art = (uint8_t *)0x1;
    size_t len = 0u;
    char why[ND_PACKAGE_WHY_MAX];
    uint8_t *huge;
    size_t huge_len = ND_PACKAGE_MAX_THUMBNAIL_BYTES + 1u;
    char huge_sha[ND_MANIFEST_HEX_MAX];

    huge = calloc(1u, huge_len);
    if (huge == NULL)
        return;
    nd_package_sha256_hex(huge, huge_len, huge_sha, sizeof huge_sha);
    manifest_text(manifest, sizeof manifest, g_image_sha, huge_sha, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), true, true);
    ms[1].data = huge;
    ms[1].len = huge_len;
    write_package("/UPDATE.ndsw", ms, n, &SANE);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg != NULL) {
        why[0] = '\0';
        CHECK_INT(nd_package_read_thumbnail(pkg, &art, &len, why, sizeof why),
                  ND_UPD_ERR_BAD_MANIFEST);
        CHECK(strstr(why, "thumbnail is") != NULL && strstr(why, "byte limit") != NULL);
        CHECK(art == NULL);
        nd_package_close(pkg);
    }
    free(huge);
}

/* ------------------------------------------------------------------ *
 * extract_image
 * ------------------------------------------------------------------ */

static uint64_t g_progress[64][2];
static size_t g_progress_n;

static void note_progress(uint64_t done, uint64_t total, void *user)
{
    ND_UNUSED(user);
    if (g_progress_n < ND_ARRAY_LEN(g_progress)) {
        g_progress[g_progress_n][0] = done;
        g_progress[g_progress_n][1] = total;
        g_progress_n++;
    }
}

static void test_extracts_the_image_and_checks_its_hash(void)
{
    char manifest[2048];
    char why[ND_PACKAGE_WHY_MAX];
    char resolved[ND_PATH_MAX];
    nd_package *pkg;
    uint8_t got[sizeof g_image];
    FILE *f;
    size_t i;

    (void)good_package("/UPDATE.ndsw", manifest, sizeof manifest, false);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;

    g_progress_n = 0u;
    why[0] = '\0';
    CHECK_INT(
        nd_package_extract_image(pkg, "/pending.img", note_progress, NULL, -1, why, sizeof why),
        ND_UPD_OK);
    CHECK(nd_path_resolve(resolved, sizeof resolved, "/pending.img") == ND_OK);
    f = fopen(resolved, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK_INT(fread(got, 1u, sizeof got, f), (int64_t)sizeof g_image);
        CHECK(memcmp(got, g_image, sizeof g_image) == 0);
        (void)fclose(f);
    }

    /* Progress is monotonic and ends exactly at the total. */
    CHECK(g_progress_n > 0u);
    for (i = 1u; i < g_progress_n; i++)
        CHECK(g_progress[i][0] >= g_progress[i - 1u][0]);
    if (g_progress_n > 0u) {
        CHECK(g_progress[g_progress_n - 1u][0] == g_progress[g_progress_n - 1u][1]);
        CHECK(g_progress[g_progress_n - 1u][0] == (uint64_t)sizeof g_image);
    }
    nd_package_close(pkg);
}

static void test_a_corrupted_image_leaves_nothing_behind(void)
{
    /* A truncated or tampered image must never survive as a pending update:
     * whatever is left at that path is what the boot-time applier will find. */
    char manifest[2048];
    char why[ND_PACKAGE_WHY_MAX];
    zmember ms[4];
    size_t n;
    nd_package *pkg;
    uint8_t tampered[sizeof g_image];

    memcpy(tampered, g_image, sizeof tampered);
    tampered[BLOCK_SIZE] ^= 0xffu;

    /* The manifest still records the ORIGINAL image's hash, and the member's
     * own CRC is correct -- so nothing before the sha256 comparison notices. */
    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[0].data = tampered;
    write_package("/UPDATE.ndsw", ms, n, &SANE);

    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_package_extract_image(pkg, "/pending.img", NULL, NULL, -1, why, sizeof why),
              ND_UPD_ERR_BAD_MANIFEST);
    CHECK(strstr(why, "image sha256 does not match the manifest") != NULL);
    CHECK(!nd_path_exists("/pending.img"));
    nd_package_close(pkg);
}

static void test_refuses_to_extract_without_room_on_the_partition(void)
{
    char manifest[2048];
    char why[ND_PACKAGE_WHY_MAX];
    nd_package *pkg;

    (void)good_package("/UPDATE.ndsw", manifest, sizeof manifest, false);
    pkg = open_ok("/UPDATE.ndsw");
    if (pkg == NULL)
        return;
    why[0] = '\0';
    CHECK_INT(nd_package_extract_image(pkg, "/pending.img", NULL, NULL, 1024, why, sizeof why),
              ND_UPD_ERR_NO_SPACE);
    CHECK(strstr(why, "image needs") != NULL && strstr(why, "only 1024 free") != NULL);
    CHECK(!nd_path_exists("/pending.img"));

    /* The margin is real: room for the image alone is not enough. */
    why[0] = '\0';
    CHECK_INT(nd_package_extract_image(pkg, "/pending.img", NULL, NULL,
                                       (int64_t)sizeof g_image + 16, why, sizeof why),
              ND_UPD_ERR_NO_SPACE);
    why[0] = '\0';
    CHECK_INT(nd_package_extract_image(pkg, "/pending.img", NULL, NULL,
                                       (int64_t)sizeof g_image + ND_PACKAGE_SPACE_MARGIN, why,
                                       sizeof why),
              ND_UPD_OK);
    nd_package_close(pkg);
}

/* ------------------------------------------------------------------ *
 * The refusals -- the reason this file exists
 * ------------------------------------------------------------------ */

static void test_a_file_that_is_not_a_zip_is_refused(void)
{
    pt_write_text("/UPDATE.ndsw", "I am not a zip file");
    refuse_open("/UPDATE.ndsw", ND_UPD_ERR_BAD_ZIP, "not a readable zip archive");

    pt_write_text("/empty.ndsw", "");
    refuse_open("/empty.ndsw", ND_UPD_ERR_BAD_ZIP, "too small to be a zip archive");
}

static void test_a_missing_file_is_refused(void)
{
    refuse_open("/nope.ndsw", ND_UPD_ERR_UNREADABLE, "no such update file");
}

static void test_a_truncated_zip_is_refused(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;
    buf b;
    zopts opt = SANE;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, true);

    /* The end of central directory record itself is gone. */
    opt.chop = 22u;
    write_package("/chopped.ndsw", ms, n, &opt);
    refuse_open("/chopped.ndsw", ND_UPD_ERR_BAD_ZIP, "no end of central directory record");

    /* The record survives but half the directory it points at does not. */
    memset(&b, 0, sizeof b);
    zip_build(&b, ms, n, &SANE);
    pt_write("/half.ndsw", b.p, b.n / 2u);
    buf_free(&b);
    refuse_open("/half.ndsw", ND_UPD_ERR_BAD_ZIP, NULL);
}

static void test_a_corrupt_central_directory_is_refused(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;
    zopts opt;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, true);

    opt = SANE;
    opt.break_cd_sig = true;
    write_package("/badsig.ndsw", ms, n, &opt);
    refuse_open("/badsig.ndsw", ND_UPD_ERR_BAD_ZIP, "bad magic number for central directory");

    /* An offset that points past the end of the file. */
    opt = SANE;
    opt.cd_off = 1u << 30;
    write_package("/badoff.ndsw", ms, n, &opt);
    refuse_open("/badoff.ndsw", ND_UPD_ERR_BAD_ZIP, NULL);

    /* An offset that points somewhere inside the member data. */
    opt = SANE;
    opt.cd_off = 40;
    write_package("/insideoff.ndsw", ms, n, &opt);
    refuse_open("/insideoff.ndsw", ND_UPD_ERR_BAD_ZIP, NULL);
}

static void test_a_broken_local_header_is_refused(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;
    zopts opt;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");

    /* The local header's name disagrees with the central directory's. This is
     * what stops one directory entry describing another member's bytes. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].local_name = "somethingelse.json";
    write_package("/namediff.ndsw", ms, n, &SANE);
    refuse_open("/namediff.ndsw", ND_UPD_ERR_BAD_ZIP, "differ");

    /* The local header is not a local header. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    opt = SANE;
    opt.break_lfh_sig = true;
    /* break_lfh_sig corrupts the FIRST member's header, so put the manifest
     * there -- it is the one open() reads. */
    {
        zmember tmp = ms[0];

        ms[0] = ms[MANIFEST_AT];
        ms[1] = tmp;
    }
    write_package("/badlfh.ndsw", ms, n, &opt);
    refuse_open("/badlfh.ndsw", ND_UPD_ERR_BAD_ZIP, "bad magic number for file header");
}

static void test_a_member_that_lies_about_its_size_is_refused(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");

    /* A STORED member is its own compressed form, so two different sizes
     * means the directory is lying about one of them. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].deflate = false;
    ms[MANIFEST_AT].csize = (int64_t)(strlen(manifest) / 2u);
    write_package("/shortstored.ndsw", ms, n, &SANE);
    refuse_open("/shortstored.ndsw", ND_UPD_ERR_BAD_ZIP, "stores");

    /* A DEFLATED member whose compressed data runs out early. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].truncate_data = 4;
    ms[MANIFEST_AT].csize = 4;
    write_package("/shortdeflate.ndsw", ms, n, &SANE);
    refuse_open("/shortdeflate.ndsw", ND_UPD_ERR_BAD_ZIP, "truncated");

    /* A member whose compressed data is declared to reach past the end of the
     * file. The central directory catches the wildly impossible size; the
     * local header catches the one that is merely too big for what is left
     * after the member's own data offset. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].csize = 1u << 20;
    write_package("/runsoff.ndsw", ms, n, &SANE);
    refuse_open("/runsoff.ndsw", ND_UPD_ERR_BAD_ZIP, "claims a size the file cannot hold");

    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].csize = (int64_t)sizeof g_image;
    write_package("/overrun.ndsw", ms, n, &SANE);
    refuse_open("/overrun.ndsw", ND_UPD_ERR_BAD_ZIP, "runs past the end of the file");
}

static void test_a_member_that_expands_past_its_declared_size_is_refused(void)
{
    /* The zip bomb. The member really holds 64 KB of zeros, which deflate
     * squeezes into a couple of hundred bytes, and the directory declares
     * sixteen. A reader that trusts the stream instead of the number
     * decompresses whatever it is given; this one stops at sixteen and then
     * refuses the member for having more to give.
     *
     * The CRC is set to the CRC of those first sixteen bytes, so the member
     * gets all the way past the checksum -- which is the point: an attacker
     * writes the CRC field too. */
    char manifest[2048];
    zmember ms[4];
    size_t n;
    uint8_t *bomb;
    size_t bomb_len = 65536u;

    bomb = calloc(1u, bomb_len);
    if (bomb == NULL)
        return;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].data = bomb;
    ms[MANIFEST_AT].len = bomb_len;
    ms[MANIFEST_AT].deflate = true;
    ms[MANIFEST_AT].usize = 16;
    ms[MANIFEST_AT].crc = (int64_t)(uint32_t)crc32(crc32(0uL, NULL, 0u), bomb, 16u);
    write_package("/bomb.ndsw", ms, n, &SANE);
    refuse_open("/bomb.ndsw", ND_UPD_ERR_BAD_ZIP, "expands past its declared size");
    free(bomb);
}

static void test_a_bad_crc_is_refused(void)
{
    /* Checked at EOF, over every byte, for every member. Python raises
     * BadZipFile("Bad CRC-32 for file %r") and so does this. */
    char manifest[2048];
    zmember ms[4];
    size_t n;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].crc = 0x12345678;
    write_package("/badcrc.ndsw", ms, n, &SANE);
    refuse_open("/badcrc.ndsw", ND_UPD_ERR_BAD_ZIP, "bad CRC-32");

    /* And on the image, where it is found during extraction rather than at
     * open, because nothing reads the image until then. */
    {
        char why[ND_PACKAGE_WHY_MAX];
        nd_package *pkg;

        n = standard_members(ms, manifest, strlen(manifest), false, true);
        ms[IMAGE_AT].crc = 0x9abcdef0;
        write_package("/badimagecrc.ndsw", ms, n, &SANE);
        pkg = open_ok("/badimagecrc.ndsw");
        if (pkg != NULL) {
            why[0] = '\0';
            CHECK_INT(
                nd_package_extract_image(pkg, "/pending.img", NULL, NULL, -1, why, sizeof why),
                ND_UPD_ERR_BAD_ZIP);
            CHECK(strstr(why, "bad CRC-32") != NULL);
            CHECK(!nd_path_exists("/pending.img"));
            nd_package_close(pkg);
        }
    }
}

static void test_unsafe_member_names_are_refused(void)
{
    /* Zip slip. Nothing here extracts by name, so this cannot be exploited
     * today -- it is refused so that the next person to add an extract-all
     * does not have to re-derive it, and because a package carrying such an
     * entry was not written by mkupdate.py. */
    static const char *const EVIL[] = {"../evil",  "a/../../evil", "/etc/passwd",
                                       "..\\evil", "C:/evil",      ".."};
    char manifest[2048];
    zmember ms[4];
    size_t n;
    size_t i;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    for (i = 0u; i < ND_ARRAY_LEN(EVIL); i++) {
        n = standard_members(ms, manifest, strlen(manifest), false, true);
        ms[SIGNATURE_AT].name = EVIL[i];
        write_package("/slip.ndsw", ms, n, &SANE);
        refuse_open("/slip.ndsw", ND_UPD_ERR_BAD_ZIP, "unsafe");
    }
}

static void test_encrypted_and_exotic_members_are_refused(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");

    /* Nothing can decrypt it, and a reader that ignored the flag would hash
     * ciphertext and report a sha256 mismatch instead of the truth. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].extra_flags = 0x0001u;
    write_package("/encrypted.ndsw", ms, n, &SANE);
    refuse_open("/encrypted.ndsw", ND_UPD_ERR_BAD_ZIP, "encrypted");

    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].extra_flags = 0x0040u; /* strong encryption */
    write_package("/strong.ndsw", ms, n, &SANE);
    refuse_open("/strong.ndsw", ND_UPD_ERR_BAD_ZIP, "refuses");

    /* Method 12 is bzip2; method 0 and method 8 are the only two a .ndsw
     * has ever used and the only two this reader implements. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    ms[MANIFEST_AT].method = 12;
    write_package("/bzip2.ndsw", ms, n, &SANE);
    refuse_open("/bzip2.ndsw", ND_UPD_ERR_BAD_ZIP, "compression method 12");
}

static void test_a_package_missing_a_required_member_is_refused(void)
{
    char manifest[2048];
    zmember ms[4];
    size_t n;
    size_t i;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");

    /* Package.__init__ checks manifest.json FIRST, so a package missing both
     * names the manifest. */
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    for (i = MANIFEST_AT; i + 1u < n; i++)
        ms[i] = ms[i + 1u];
    write_package("/nomanifest.ndsw", ms, n - 1u, &SANE);
    refuse_open("/nomanifest.ndsw", ND_UPD_ERR_BAD_MANIFEST, "package has no manifest.json");

    n = standard_members(ms, manifest, strlen(manifest), false, true);
    for (i = IMAGE_AT; i + 1u < n; i++)
        ms[i] = ms[i + 1u];
    write_package("/noimage.ndsw", ms, n - 1u, &SANE);
    refuse_open("/noimage.ndsw", ND_UPD_ERR_BAD_ZIP, "package has no rootfs.squashfs");

    /* An archive with nothing in it at all is a valid zip. */
    write_package("/nothing.ndsw", ms, 0u, &SANE);
    refuse_open("/nothing.ndsw", ND_UPD_ERR_BAD_MANIFEST, "package has no manifest.json");
}

static void test_a_bad_manifest_inside_a_good_zip_is_refused(void)
{
    zmember ms[4];
    size_t n;
    static const char BAD[] = "{not json";
    static const char NOT_OBJECT[] = "[1, 2, 3]";

    n = standard_members(ms, BAD, sizeof BAD - 1u, false, true);
    write_package("/badjson.ndsw", ms, n, &SANE);
    refuse_open("/badjson.ndsw", ND_UPD_ERR_BAD_MANIFEST, "manifest is not valid JSON");

    n = standard_members(ms, NOT_OBJECT, sizeof NOT_OBJECT - 1u, false, true);
    write_package("/notobject.ndsw", ms, n, &SANE);
    refuse_open("/notobject.ndsw", ND_UPD_ERR_BAD_MANIFEST, "manifest must be a JSON object");
}

static void test_an_oversized_manifest_is_refused_before_it_is_read(void)
{
    /* The wall that stops a package declaring a 40 MB manifest.json and
     * asking a 53 MB phone to hold it. Declared over the cap, and its real
     * content is a run of spaces that deflates to nothing. */
    zmember ms[4];
    size_t n;
    size_t big_len = ND_PACKAGE_MAX_WHOLE_MEMBER + 1u;
    char *big = malloc(big_len);

    if (big == NULL)
        return;
    memset(big, ' ', big_len);
    n = standard_members(ms, big, big_len, false, true);
    write_package("/bigmanifest.ndsw", ms, n, &SANE);
    refuse_open("/bigmanifest.ndsw", ND_UPD_ERR_BAD_ZIP, "byte limit");
    free(big);
}

static void test_too_many_members_is_refused(void)
{
    zmember ms[128];
    char names[128][16];
    char manifest[2048];
    size_t n = ND_PACKAGE_MAX_MEMBERS + 2u;
    size_t i;
    buf b;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    memset(ms, 0, sizeof ms);
    for (i = 0u; i < n; i++) {
        (void)snprintf(names[i], sizeof names[i], "f%02zu.txt", i);
        ms[i].name = names[i];
        ms[i].data = (const uint8_t *)"x";
        ms[i].len = 1u;
        ms[i].method = -1;
        ms[i].csize = -1;
        ms[i].usize = -1;
        ms[i].crc = -1;
        ms[i].truncate_data = -1;
    }
    memset(&b, 0, sizeof b);
    zip_build(&b, ms, n, &SANE);
    pt_write("/many.ndsw", b.p, b.n);
    buf_free(&b);
    refuse_open("/many.ndsw", ND_UPD_ERR_BAD_ZIP, "member limit");
}

/* ------------------------------------------------------------------ *
 * The awkward corners of the format
 * ------------------------------------------------------------------ */

static void test_duplicate_names_resolve_to_the_last_entry(void)
{
    /* CPython builds NameToInfo by assignment in directory order, so a later
     * entry silently replaces an earlier one. A reader that kept the FIRST
     * would open a different manifest than Python does over the same bytes --
     * and the signature is checked against whichever one it read. */
    char manifest_a[2048];
    char manifest_b[2048];
    zmember ms[4];
    size_t n;
    nd_package *pkg;
    const nd_manifest *m;

    manifest_text(manifest_a, sizeof manifest_a, g_image_sha, NULL, "luckfox-armv7");
    manifest_text(manifest_b, sizeof manifest_b, g_image_sha, NULL, "qemu-aarch64");

    n = standard_members(ms, manifest_a, strlen(manifest_a), false, true);
    /* A SECOND manifest.json, later in the directory than the first. */
    init_member(&ms[n], ND_PACKAGE_MANIFEST_MEMBER, manifest_b, strlen(manifest_b), true);
    n++;
    write_package("/dup.ndsw", ms, n, &SANE);

    pkg = open_ok("/dup.ndsw");
    if (pkg == NULL)
        return;
    CHECK_INT(nd_package_member_count(pkg), 3);
    m = nd_package_manifest(pkg);
    if (m != NULL)
        CHECK_STR(m->platform, "qemu-aarch64");
    nd_package_close(pkg);
}

static void test_zip64_is_tolerated(void)
{
    /* A 51 MB package never needs Zip64, but Python emits it whenever
     * allowZip64 triggers and a reader that chokes on it is a landmine. */
    char manifest[2048];
    zmember ms[4];
    size_t n;
    zopts opt = SANE;
    nd_package *pkg;

    manifest_text(manifest, sizeof manifest, g_image_sha, NULL, "qemu-aarch64");
    n = standard_members(ms, manifest, strlen(manifest), false, true);
    opt.zip64 = true;
    write_package("/zip64.ndsw", ms, n, &opt);

    pkg = open_ok("/zip64.ndsw");
    if (pkg == NULL)
        return;
    CHECK_INT(nd_package_member_count(pkg), 3);
    CHECK_INT(nd_package_image_size(pkg), (int64_t)sizeof g_image);
    {
        const nd_manifest *m = nd_package_manifest(pkg);

        if (m != NULL)
            CHECK_STR(m->version, "0.3.2a");
    }
    nd_package_close(pkg);
}

static void test_null_arguments_do_not_crash(void)
{
    char why[ND_PACKAGE_WHY_MAX];
    uint8_t *out = NULL;
    size_t len = 0u;

    nd_package_close(NULL);
    CHECK(nd_package_manifest(NULL) == NULL);
    CHECK_STR(nd_package_path(NULL), "");
    CHECK_INT(nd_package_image_size(NULL), -1);
    CHECK(!nd_package_is_signed(NULL));
    nd_package_mark_signed(NULL);
    CHECK(nd_package_manifest_raw(NULL, &len) == NULL);
    CHECK_INT(len, 0);
    CHECK_INT(nd_package_member_count(NULL), 0);
    CHECK(nd_package_member_name(NULL, 0u) == NULL);
    CHECK_INT(nd_package_member_size(NULL, "x"), -1);
    CHECK_INT(nd_package_member_method(NULL, "x"), -1);
    why[0] = '\0';
    CHECK_INT(nd_package_read_signature(NULL, &out, &len, why, sizeof why),
              ND_UPD_ERR_BAD_SIGNATURE);
    CHECK_INT(nd_package_read_thumbnail(NULL, &out, &len, why, sizeof why),
              ND_UPD_ERR_BAD_MANIFEST);
    CHECK_INT(nd_package_extract_image(NULL, "/x", NULL, NULL, -1, why, sizeof why),
              ND_UPD_ERR_BAD_ZIP);
    CHECK_INT(nd_package_read_member(NULL, "x", 16u, &out, &len, why, sizeof why),
              ND_UPD_ERR_BAD_ZIP);
    refuse_open(NULL, ND_UPD_ERR_UNREADABLE, NULL);
    refuse_open("", ND_UPD_ERR_UNREADABLE, NULL);
}

/* ------------------------------------------------------------------ *
 * --dump: the cross-check against the real Python
 * ------------------------------------------------------------------ */

/* A changelog holds newlines and a hostile manifest could hold a tab, so one
 * field per line with the three characters that would break the framing
 * escaped. The Python side reverses it. */
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

static int dump(const char *path)
{
    char why[ND_PACKAGE_WHY_MAX];
    nd_package *pkg = NULL;
    const nd_manifest *m;
    uint8_t *blob = NULL;
    size_t blob_len = 0u;
    size_t i;
    nd_update_err rc;

    /* Not ND_ROOT-resolved: the caller named a real file on disk. */
    (void)nd_path_set_root(NULL);

    why[0] = '\0';
    rc = nd_package_open(path, &pkg, why, sizeof why);
    if (rc != ND_UPD_OK) {
        printf("open\tERR\t%d\t%s\n", (int)rc, why);
        return 0;
    }
    printf("open\tOK\n");
    printf("image_size\t%lld\n", (long long)nd_package_image_size(pkg));
    for (i = 0u; i < nd_package_member_count(pkg); i++) {
        const char *name = nd_package_member_name(pkg, i);

        printf("member\t%s\t%lld\t%d\n", name, (long long)nd_package_member_size(pkg, name),
               (int)nd_package_member_method(pkg, name));
    }
    m = nd_package_manifest(pkg);
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
    printf("manifest_raw_len\t%zu\n", m->raw_len);
    {
        char digest[ND_MANIFEST_HEX_MAX];

        nd_package_sha256_hex(m->raw, m->raw_len, digest, sizeof digest);
        printf("manifest_raw_sha256\t%s\n", digest);
    }

    why[0] = '\0';
    rc = nd_package_read_signature(pkg, &blob, &blob_len, why, sizeof why);
    if (rc != ND_UPD_OK) {
        printf("signature\tERR\t%s\n", why);
    } else {
        char digest[ND_MANIFEST_HEX_MAX];

        nd_package_sha256_hex(blob, blob_len, digest, sizeof digest);
        printf("signature\t%zu\t%s\n", blob_len, digest);
        free(blob);
        blob = NULL;
    }

    why[0] = '\0';
    blob_len = 0u;
    rc = nd_package_read_thumbnail(pkg, &blob, &blob_len, why, sizeof why);
    if (rc != ND_UPD_OK) {
        printf("thumbnail\tERR\t%s\n", why);
    } else if (blob == NULL) {
        printf("thumbnail\tNONE\n");
    } else {
        char digest[ND_MANIFEST_HEX_MAX];

        nd_package_sha256_hex(blob, blob_len, digest, sizeof digest);
        printf("thumbnail\t%zu\t%s\n", blob_len, digest);
        free(blob);
    }

    nd_package_close(pkg);
    return 0;
}

/* extract_image and check_compatible, for the same cross-check. The two
 * variants mkbadupdate calls the brick case (wrong-platform, future-kernel)
 * and the two it calls the corrupt case (corrupt-image, truncated-image) all
 * open cleanly and are only refused HERE, so a cross-check that stops at
 * open() never reaches them. */
static int extract(const char *path, const char *dest)
{
    char why[ND_PACKAGE_WHY_MAX];
    nd_package *pkg = NULL;
    nd_update_err rc;

    (void)nd_path_set_root(NULL);
    why[0] = '\0';
    if (nd_package_open(path, &pkg, why, sizeof why) != ND_UPD_OK) {
        printf("extract\tERR\t%s\n", why);
        return 0;
    }
    rc = nd_package_extract_image(pkg, dest, NULL, NULL, -1, why, sizeof why);
    if (rc == ND_UPD_OK)
        printf("extract\tOK\n");
    else
        printf("extract\tERR\t%s\n", why);
    printf("left_behind\t%d\n", nd_path_exists(dest) ? 1 : 0);
    nd_package_close(pkg);
    return 0;
}

static int compat(const char *path, const char *platform, const char *kernel)
{
    char why[ND_PACKAGE_WHY_MAX];
    nd_package *pkg = NULL;
    nd_update_err rc;

    (void)nd_path_set_root(NULL);
    why[0] = '\0';
    if (nd_package_open(path, &pkg, why, sizeof why) != ND_UPD_OK) {
        printf("compat\tERR\t%s\n", why);
        return 0;
    }
    rc = nd_manifest_check_compatible(nd_package_manifest(pkg), platform, kernel, why, sizeof why);
    printf("compat\t%s\t%s\n", rc == ND_UPD_OK ? "OK" : "ERR", why);
    nd_package_close(pkg);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--dump") == 0)
        return dump(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "--extract") == 0)
        return extract(argv[2], argv[3]);
    if (argc >= 4 && strcmp(argv[1], "--compat") == 0)
        return compat(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);

    fixtures_init();
    RUN(test_sha256_known_answers);
    RUN(test_reads_the_manifest_out_of_a_package);
    RUN(test_both_compression_methods_are_mandatory);
    RUN(test_member_listing);
    RUN(test_reads_the_signature_member);
    RUN(test_reads_a_thumbnail_out_of_a_package);
    RUN(test_a_package_without_a_thumbnail_has_none);
    RUN(test_an_undeclared_thumbnail_is_ignored);
    RUN(test_a_thumbnail_that_does_not_match_its_hash_is_invalid);
    RUN(test_an_oversized_thumbnail_is_refused_before_it_is_read);
    RUN(test_extracts_the_image_and_checks_its_hash);
    RUN(test_a_corrupted_image_leaves_nothing_behind);
    RUN(test_refuses_to_extract_without_room_on_the_partition);
    RUN(test_a_file_that_is_not_a_zip_is_refused);
    RUN(test_a_missing_file_is_refused);
    RUN(test_a_truncated_zip_is_refused);
    RUN(test_a_corrupt_central_directory_is_refused);
    RUN(test_a_broken_local_header_is_refused);
    RUN(test_a_member_that_lies_about_its_size_is_refused);
    RUN(test_a_member_that_expands_past_its_declared_size_is_refused);
    RUN(test_a_bad_crc_is_refused);
    RUN(test_unsafe_member_names_are_refused);
    RUN(test_encrypted_and_exotic_members_are_refused);
    RUN(test_a_package_missing_a_required_member_is_refused);
    RUN(test_a_bad_manifest_inside_a_good_zip_is_refused);
    RUN(test_an_oversized_manifest_is_refused_before_it_is_read);
    RUN(test_too_many_members_is_refused);
    RUN(test_duplicate_names_resolve_to_the_last_entry);
    RUN(test_zip64_is_tolerated);
    RUN(test_null_arguments_do_not_crash);
    return pt_report("test_package");
}
