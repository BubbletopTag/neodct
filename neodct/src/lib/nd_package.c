/* nd_package.c -- the .ndsw reader: a zip parser, a streaming extractor and
 * package.py's four operations on top of them.
 *
 * The header says what this refuses and why. What is worth saying beside the
 * code is where the zip parsing came from: it is CPython's zipfile, function
 * by function, because the packages this reads are written by CPython's
 * zipfile (mkupdate.py, mkbadupdate.py, tests/update_fixtures.py) and the
 * suite that decides whether this port is correct feeds both sides the same
 * bytes. Where a comment below says "CPython", the behaviour was read out of
 * Lib/zipfile/__init__.py and matched deliberately -- including the parts
 * that look like historical accidents, such as taking the LAST end-of-central-
 * directory signature in the file rather than a consistent one.
 *
 * ============ THE ZIP READER IS IN THIS FILE ON PURPOSE ============
 *
 * spec-update-system.md gives it a file of its own (nd_zip.c). It is here
 * instead because it has exactly one caller and no independent contract: the
 * only archives NeoDCT will ever open are four-member .ndsw packages, every
 * entry point below is one of package.py's, and a general-purpose zip API
 * exported from libneodct would be a promise this project does not want to
 * keep. The section markers below are where it splits if that ever changes.
 *
 * ============ MEMORY ============
 *
 * 51 MB of image against 53 MB of usable RAM. Nothing here allocates in
 * proportion to a member's size except nd_package_read_member(), which takes
 * its ceiling as an argument and refuses before allocating. The image is
 * never read into memory at all -- nd_package_extract_image() is the only
 * way to get at it and it streams in ND_PACKAGE_CHUNK pieces.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <zlib.h>

#include "nd_manifest.h"
#include "nd_package.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_update.h"

/* ------------------------------------------------------------------ *
 * Refusals
 * ------------------------------------------------------------------ */

static void say(char *why, size_t why_sz, const char *fmt, ...) ND_PRINTF(3, 4);

static void say(char *why, size_t why_sz, const char *fmt, ...)
{
    va_list ap;

    if (why == NULL || why_sz == 0u)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(why, why_sz, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ *
 * sha256
 * ------------------------------------------------------------------ *
 *
 * A private copy. lib/nd_capture.c has one too, and the RSA verifier will
 * want a third; when that lands all three want lifting into one nd_sha256.c.
 * It is duplicated rather than exported today because exporting a symbol the
 * signature work package is about to define is how two agents collide in the
 * same link.
 */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t block[64];
    size_t have;
} sha256_ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t rotr32(uint32_t v, unsigned int n)
{
    return (v >> n) | (v << (32u - n));
}

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    unsigned int i;

    for (i = 0u; i < 16u; i++) {
        w[i] = ((uint32_t)p[i * 4u] << 24) | ((uint32_t)p[i * 4u + 1u] << 16) |
               ((uint32_t)p[i * 4u + 2u] << 8) | (uint32_t)p[i * 4u + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);

        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];

    for (i = 0u; i < 64u; i++) {
        uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u;
    c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu;
    c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu;
    c->h[7] = 0x5be0cd19u;
    c->len = 0u;
    c->have = 0u;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;

    c->len += (uint64_t)n;
    while (n > 0u) {
        size_t take = 64u - c->have;

        if (take > n)
            take = n;
        memcpy(c->block + c->have, p, take);
        c->have += take;
        p += take;
        n -= take;
        if (c->have == 64u) {
            sha256_block(c, c->block);
            c->have = 0u;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8u;
    uint8_t tail[72];
    size_t pad;
    unsigned int i;

    memset(tail, 0, sizeof tail);
    tail[0] = 0x80u;
    /* 56 is where the length must start inside the final block. */
    pad = (c->have < 56u) ? (56u - c->have) : (120u - c->have);
    for (i = 0u; i < 8u; i++)
        tail[pad + i] = (uint8_t)(bits >> (56u - 8u * i));
    sha256_update(c, tail, pad + 8u);

    for (i = 0u; i < 8u; i++) {
        out[i * 4u] = (uint8_t)(c->h[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(c->h[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(c->h[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)c->h[i];
    }
}

static void hex_of(const uint8_t *digest, char *out, size_t out_sz)
{
    static const char DIGITS[] = "0123456789abcdef";
    size_t i;

    if (out == NULL || out_sz == 0u)
        return;
    if (out_sz < 65u) {
        out[0] = '\0';
        return;
    }
    for (i = 0u; i < 32u; i++) {
        out[i * 2u] = DIGITS[digest[i] >> 4];
        out[i * 2u + 1u] = DIGITS[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

void nd_package_sha256_hex(const void *data, size_t len, char *out, size_t out_sz)
{
    sha256_ctx ctx;
    uint8_t digest[32];

    sha256_init(&ctx);
    if (data != NULL && len > 0u)
        sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
    hex_of(digest, out, out_sz);
}

/* ------------------------------------------------------------------ *
 * ==================== the zip reader starts here ====================
 * ------------------------------------------------------------------ */

#define SIG_EOCD     0x06054b50u
#define SIG_EOCD64   0x06064b50u
#define SIG_LOCATOR  0x07064b50u
#define SIG_CENTRAL  0x02014b50u
#define SIG_LOCAL    0x04034b50u

#define EOCD_SIZE     22u
#define EOCD64_SIZE   56u
#define LOCATOR_SIZE  20u
#define CENTRAL_SIZE  46u
#define LOCAL_SIZE    30u
#define MAX_COMMENT   65535u

/* General-purpose bit flags that decide whether a member can be read at all. */
#define FLAG_ENCRYPTED        0x0001u
#define FLAG_COMPRESSED_PATCH 0x0020u
#define FLAG_STRONG_ENCRYPT   0x0040u

#define METHOD_STORED  0u
#define METHOD_DEFLATE 8u

/* Compressed bytes fed to inflate at a time. Fixed, not input-sized. */
#define ZIP_IN_CHUNK 32768u

typedef struct {
    char name[ND_PACKAGE_NAME_MAX];
    uint16_t flags;
    uint16_t method;
    uint32_t crc;
    uint64_t csize;
    uint64_t usize;
    uint64_t lho; /* local header offset, with `concat` already applied */
} zip_entry;

struct nd_package {
    char path[ND_PATH_MAX]; /* the VIRTUAL path, which is what staging records */
    int fd;
    uint64_t file_size;
    zip_entry entries[ND_PACKAGE_MAX_MEMBERS];
    size_t n_entries;
    nd_manifest *manifest;
    bool is_signed;
};

/* ---- little-endian readers. No unaligned access, no endianness
 * assumption: ARM faults where x86 quietly works. ---- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Reads exactly `n` bytes at `off`, looping over short reads. false means the
 * file ended early or the read failed -- both are "this zip is truncated". */
static bool read_at(int fd, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = buf;

    while (n > 0u) {
        ssize_t got;

        if (off > (uint64_t)INT64_MAX)
            return false;
        got = pread(fd, p, n, (off_t)off);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        p += (size_t)got;
        n -= (size_t)got;
        off += (uint64_t)got;
    }
    return true;
}

/* ---- member names ---- */

/* An .ndsw has four flat ASCII names. Anything that could be read as a path
 * out of the archive's own directory is refused for the whole package: this
 * reader never extracts by name, so it cannot be exploited today, and it is
 * refused anyway so that the next person to add an extract-all does not have
 * to re-derive it. Nothing mkupdate.py writes comes near this. */
static bool name_is_safe(const char *name)
{
    size_t i = 0u;

    if (name[0] == '\0' || name[0] == '/')
        return false;
    if (name[0] != '\0' && name[1] == ':') /* a Windows drive letter */
        return false;
    for (i = 0u; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20u || c == 0x7fu || c == '\\')
            return false;
    }
    /* Any component that is exactly "..". */
    i = 0u;
    for (;;) {
        const char *slash = strchr(name + i, '/');
        size_t len = slash != NULL ? (size_t)(slash - (name + i)) : strlen(name + i);

        if (len == 2u && name[i] == '.' && name[i + 1u] == '.')
            return false;
        if (slash == NULL)
            break;
        i += len + 1u;
    }
    return true;
}

static const zip_entry *find_entry(const nd_package *pkg, const char *name)
{
    size_t i;

    for (i = 0u; i < pkg->n_entries; i++) {
        if (strcmp(pkg->entries[i].name, name) == 0)
            return &pkg->entries[i];
    }
    return NULL;
}

/* ---- the Zip64 extra field (header id 0x0001) ----
 *
 * CPython's ZipInfo._decodeExtra, including its error cases: a field that
 * overruns the extra area and a counts block too small for the 0xFFFFFFFF
 * placeholders actually present are both BadZipFile. */
static bool apply_zip64(zip_entry *e, const uint8_t *extra, size_t extra_len, char *why,
                        size_t why_sz)
{
    size_t p = 0u;

    while (extra_len - p >= 4u) {
        uint16_t tp = rd16(extra + p);
        uint16_t ln = rd16(extra + p + 2u);

        if ((size_t)ln + 4u > extra_len - p) {
            say(why, why_sz, "corrupt extra field %04x (size=%u)", tp, (unsigned int)ln);
            return false;
        }
        p += 4u;
        if (tp == 0x0001u) {
            uint64_t counts[3];
            size_t ncounts;
            size_t idx = 0u;
            size_t k;

            if (ln >= 24u)
                ncounts = 3u;
            else if (ln == 16u)
                ncounts = 2u;
            else if (ln == 8u)
                ncounts = 1u;
            else if (ln == 0u)
                ncounts = 0u;
            else {
                say(why, why_sz, "corrupt extra field %04x (size=%u)", tp, (unsigned int)ln);
                return false;
            }
            for (k = 0u; k < ncounts; k++)
                counts[k] = rd64(extra + p + k * 8u);

            /* The order is fixed by the format: uncompressed, compressed,
             * local header offset -- and only the ones that were 0xFFFFFFFF
             * consume a slot. */
            if (e->usize == 0xffffffffu) {
                if (idx >= ncounts) {
                    say(why, why_sz, "corrupt zip64 extra field");
                    return false;
                }
                e->usize = counts[idx++];
            }
            if (e->csize == 0xffffffffu) {
                if (idx >= ncounts) {
                    say(why, why_sz, "corrupt zip64 extra field");
                    return false;
                }
                e->csize = counts[idx++];
            }
            if (e->lho == 0xffffffffu) {
                if (idx >= ncounts) {
                    say(why, why_sz, "corrupt zip64 extra field");
                    return false;
                }
                e->lho = counts[idx++];
            }
        }
        p += (size_t)ln;
    }
    return true;
}

/* ---- the end of central directory, and its Zip64 counterpart ----
 *
 * CPython scans backwards over at most 22 + 65535 bytes and takes the LAST
 * occurrence of the signature (bytes.rfind), without checking that the
 * comment length that follows is consistent. Matched deliberately: a package
 * whose trailing bytes happen to contain the signature must be read the same
 * way by both sides or the two disagree about which archive they are looking
 * at. */
static bool find_eocd(nd_package *pkg, uint64_t *eocd_at, uint8_t out[EOCD_SIZE], char *why,
                      size_t why_sz)
{
    size_t window;
    uint64_t base;
    uint8_t *buf = NULL;
    bool found = false;
    size_t i;

    if (pkg->file_size < EOCD_SIZE) {
        say(why, why_sz, "file is too small to be a zip archive");
        return false;
    }
    window = (size_t)((pkg->file_size < (uint64_t)(EOCD_SIZE + MAX_COMMENT))
                          ? pkg->file_size
                          : (uint64_t)(EOCD_SIZE + MAX_COMMENT));
    base = pkg->file_size - (uint64_t)window;

    buf = malloc(window);
    if (buf == NULL) {
        say(why, why_sz, "out of memory");
        return false;
    }
    if (!read_at(pkg->fd, base, buf, window)) {
        say(why, why_sz, "cannot read the end of the file");
        free(buf);
        return false;
    }

    i = window - EOCD_SIZE + 1u;
    while (i > 0u) {
        i--;
        if (rd32(buf + i) == SIG_EOCD) {
            memcpy(out, buf + i, EOCD_SIZE);
            *eocd_at = base + (uint64_t)i;
            found = true;
            break;
        }
    }
    free(buf);
    if (!found)
        say(why, why_sz, "no end of central directory record");
    return found;
}

/* CPython's _EndRecData64: always looks for the locator, whether or not the
 * 32-bit fields were saturated, and quietly keeps the 32-bit record when it
 * is not there. */
static bool read_eocd64(nd_package *pkg, uint64_t eocd_at, uint64_t *entries, uint64_t *cd_size,
                        uint64_t *cd_off, bool *was_zip64, char *why, size_t why_sz)
{
    uint8_t loc[LOCATOR_SIZE];
    uint8_t rec[EOCD64_SIZE];
    uint64_t rel;

    *was_zip64 = false;
    if (eocd_at < (uint64_t)LOCATOR_SIZE)
        return true;
    if (!read_at(pkg->fd, eocd_at - LOCATOR_SIZE, loc, LOCATOR_SIZE))
        return true;
    if (rd32(loc) != SIG_LOCATOR)
        return true;

    if (rd32(loc + 4u) != 0u || rd32(loc + 16u) > 1u) {
        say(why, why_sz, "zipfiles that span multiple disks are not supported");
        return false;
    }
    rel = rd64(loc + 8u);
    if (rel > pkg->file_size || pkg->file_size - rel < (uint64_t)EOCD64_SIZE) {
        say(why, why_sz, "zip64 end of central directory is out of range");
        return false;
    }
    if (!read_at(pkg->fd, rel, rec, EOCD64_SIZE)) {
        say(why, why_sz, "cannot read the zip64 end of central directory");
        return false;
    }
    if (rd32(rec) != SIG_EOCD64) {
        say(why, why_sz, "bad magic number for the zip64 end of central directory");
        return false;
    }
    *entries = rd64(rec + 32u);
    *cd_size = rd64(rec + 40u);
    *cd_off = rd64(rec + 48u);
    *was_zip64 = true;
    return true;
}

static bool read_central_directory(nd_package *pkg, char *why, size_t why_sz)
{
    uint8_t eocd[EOCD_SIZE];
    uint64_t eocd_at = 0u;
    uint64_t entries;
    uint64_t cd_size;
    uint64_t cd_off;
    int64_t concat;
    uint64_t cd_start;
    uint8_t *cd = NULL;
    bool zip64 = false;
    bool ok = false;
    size_t p = 0u;

    if (!find_eocd(pkg, &eocd_at, eocd, why, why_sz))
        return false;

    entries = rd16(eocd + 10u);
    cd_size = rd32(eocd + 12u);
    cd_off = rd32(eocd + 16u);

    if (!read_eocd64(pkg, eocd_at, &entries, &cd_size, &cd_off, &zip64, why, why_sz))
        return false;

    if (!zip64 && (rd16(eocd + 4u) != 0u || rd16(eocd + 6u) != 0u)) {
        say(why, why_sz, "zipfiles that span multiple disks are not supported");
        return false;
    }

    /* CPython: `concat` is zero unless the zip was appended to another file
     * (a self-extracting archive). Every offset in the directory is relative
     * to the start of the zip, not the start of the file. */
    concat = (int64_t)eocd_at - (int64_t)cd_size - (int64_t)cd_off;
    if (zip64)
        concat -= (int64_t)(EOCD64_SIZE + LOCATOR_SIZE);
    if (concat < 0) {
        say(why, why_sz, "bad offset for central directory");
        return false;
    }
    cd_start = (uint64_t)concat + cd_off;
    if (cd_start > pkg->file_size || pkg->file_size - cd_start < cd_size) {
        say(why, why_sz, "central directory runs past the end of the file");
        return false;
    }
    if (cd_size > (uint64_t)ND_PACKAGE_MAX_FILE_BYTES) {
        say(why, why_sz, "central directory is implausibly large");
        return false;
    }
    if (entries > (uint64_t)ND_PACKAGE_MAX_MEMBERS) {
        say(why, why_sz, "package has %llu members, over the %d member limit",
            (unsigned long long)entries, ND_PACKAGE_MAX_MEMBERS);
        return false;
    }

    /* An archive with an empty central directory is a valid zip with no
     * members; the "package has no manifest.json" check upstairs is what
     * refuses it, and with the right words. */
    if (cd_size == 0u)
        return true;
    cd = malloc((size_t)cd_size);
    if (cd == NULL) {
        say(why, why_sz, "out of memory");
        return false;
    }
    if (!read_at(pkg->fd, cd_start, cd, (size_t)cd_size)) {
        say(why, why_sz, "truncated central directory");
        goto done;
    }

    while ((uint64_t)p < cd_size) {
        zip_entry e;
        uint16_t name_len;
        uint16_t extra_len;
        uint16_t comment_len;
        uint64_t need;
        size_t i;
        bool replaced = false;

        if (cd_size - (uint64_t)p < (uint64_t)CENTRAL_SIZE) {
            say(why, why_sz, "truncated central directory");
            goto done;
        }
        if (rd32(cd + p) != SIG_CENTRAL) {
            say(why, why_sz, "bad magic number for central directory");
            goto done;
        }
        memset(&e, 0, sizeof e);
        e.flags = rd16(cd + p + 8u);
        e.method = rd16(cd + p + 10u);
        e.crc = rd32(cd + p + 16u);
        e.csize = rd32(cd + p + 20u);
        e.usize = rd32(cd + p + 24u);
        name_len = rd16(cd + p + 28u);
        extra_len = rd16(cd + p + 30u);
        comment_len = rd16(cd + p + 32u);
        e.lho = rd32(cd + p + 42u);

        need = (uint64_t)CENTRAL_SIZE + (uint64_t)name_len + (uint64_t)extra_len +
               (uint64_t)comment_len;
        if (cd_size - (uint64_t)p < need) {
            say(why, why_sz, "truncated central directory");
            goto done;
        }
        if ((size_t)name_len >= sizeof e.name) {
            say(why, why_sz, "package member name is longer than %d bytes",
                ND_PACKAGE_NAME_MAX - 1);
            goto done;
        }
        memcpy(e.name, cd + p + CENTRAL_SIZE, (size_t)name_len);
        e.name[name_len] = '\0';
        if (strlen(e.name) != (size_t)name_len) {
            say(why, why_sz, "package member name contains a NUL");
            goto done;
        }
        if (!name_is_safe(e.name)) {
            say(why, why_sz, "package member name is unsafe: %s", e.name);
            goto done;
        }
        if (!apply_zip64(&e, cd + p + CENTRAL_SIZE + name_len, (size_t)extra_len, why,
                         why_sz))
            goto done;

        if (e.lho > (uint64_t)INT64_MAX - (uint64_t)concat) {
            say(why, why_sz, "bad local header offset for %s", e.name);
            goto done;
        }
        e.lho += (uint64_t)concat;
        if (e.lho > pkg->file_size || pkg->file_size - e.lho < (uint64_t)LOCAL_SIZE) {
            say(why, why_sz, "local header for %s is past the end of the file", e.name);
            goto done;
        }
        if (e.csize > pkg->file_size || e.usize > (uint64_t)ND_PACKAGE_MAX_FILE_BYTES) {
            say(why, why_sz, "%s claims a size the file cannot hold", e.name);
            goto done;
        }

        /* "Duplicate names: the last occurrence in the central directory
         * wins." CPython builds NameToInfo by assignment in directory order,
         * so a later entry silently replaces an earlier one -- and a reader
         * that kept the first would open a different member than Python
         * does over the same bytes. */
        for (i = 0u; i < pkg->n_entries; i++) {
            if (strcmp(pkg->entries[i].name, e.name) == 0) {
                pkg->entries[i] = e;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (pkg->n_entries >= ND_ARRAY_LEN(pkg->entries)) {
                say(why, why_sz, "package has more than %d members", ND_PACKAGE_MAX_MEMBERS);
                goto done;
            }
            pkg->entries[pkg->n_entries++] = e;
        }
        p += (size_t)need;
    }
    ok = true;

done:
    free(cd);
    return ok;
}

/* ------------------------------------------------------------------ *
 * Reading a member
 * ------------------------------------------------------------------ */

typedef struct {
    nd_package *pkg;
    const zip_entry *e;
    uint64_t at;       /* next compressed byte to read, absolute */
    uint64_t in_left;  /* compressed bytes not yet read          */
    uint64_t produced; /* uncompressed bytes handed out          */
    uint32_t crc;
    bool zs_open;
    z_stream zs;
    uint8_t in[ZIP_IN_CHUNK];
} zip_reader;

/* Resolves the member's data offset from its LOCAL header -- never from the
 * central directory's name and extra lengths, which routinely differ from
 * the local ones. Getting this wrong is the classic hand-written-zip bug and
 * it reads the wrong bytes silently. */
static bool reader_open(nd_package *pkg, const zip_entry *e, zip_reader *r, char *why,
                        size_t why_sz)
{
    uint8_t head[LOCAL_SIZE];
    char local_name[ND_PACKAGE_NAME_MAX];
    uint16_t name_len;
    uint16_t extra_len;
    uint64_t data_off;

    memset(r, 0, sizeof *r);
    r->pkg = pkg;
    r->e = e;

    if ((e->flags & FLAG_ENCRYPTED) != 0u) {
        say(why, why_sz, "%s is encrypted", e->name);
        return false;
    }
    if ((e->flags & (FLAG_COMPRESSED_PATCH | FLAG_STRONG_ENCRYPT)) != 0u) {
        say(why, why_sz, "%s uses a zip feature this reader refuses", e->name);
        return false;
    }
    if (e->method != METHOD_STORED && e->method != METHOD_DEFLATE) {
        say(why, why_sz, "%s uses compression method %u", e->name, (unsigned int)e->method);
        return false;
    }
    if (!read_at(pkg->fd, e->lho, head, LOCAL_SIZE)) {
        say(why, why_sz, "truncated file header for %s", e->name);
        return false;
    }
    if (rd32(head) != SIG_LOCAL) {
        say(why, why_sz, "bad magic number for file header");
        return false;
    }
    name_len = rd16(head + 26u);
    extra_len = rd16(head + 28u);
    if ((size_t)name_len >= sizeof local_name) {
        say(why, why_sz, "file name in the header for %s is too long", e->name);
        return false;
    }
    if (!read_at(pkg->fd, e->lho + LOCAL_SIZE, local_name, (size_t)name_len)) {
        say(why, why_sz, "truncated file header for %s", e->name);
        return false;
    }
    local_name[name_len] = '\0';
    /* CPython raises BadZipFile("File name in directory %r and header %r
     * differ"). It is what stops one directory entry describing another
     * member's bytes. */
    if (strcmp(local_name, e->name) != 0) {
        say(why, why_sz, "file name in directory '%s' and header '%s' differ", e->name, local_name);
        return false;
    }

    data_off = e->lho + LOCAL_SIZE + (uint64_t)name_len + (uint64_t)extra_len;
    if (data_off > pkg->file_size || pkg->file_size - data_off < e->csize) {
        say(why, why_sz, "%s runs past the end of the file", e->name);
        return false;
    }
    /* A stored member IS its own compressed form. Two different sizes means
     * the directory is lying about one of them; Python would return the
     * shorter of the two and let the CRC decide. */
    if (e->method == METHOD_STORED && e->csize != e->usize) {
        say(why, why_sz, "%s declares %llu bytes but stores %llu", e->name,
            (unsigned long long)e->usize, (unsigned long long)e->csize);
        return false;
    }

    r->at = data_off;
    r->in_left = e->csize;
    r->crc = (uint32_t)crc32(0uL, NULL, 0u);

    if (e->method == METHOD_DEFLATE) {
        /* Raw deflate: no zlib header, which is what a zip member is. */
        if (inflateInit2(&r->zs, -MAX_WBITS) != Z_OK) {
            say(why, why_sz, "cannot start decompression for %s", e->name);
            return false;
        }
        r->zs_open = true;
    }
    return true;
}

static void reader_close(zip_reader *r)
{
    if (r == NULL)
        return;
    if (r->zs_open) {
        (void)inflateEnd(&r->zs);
        r->zs_open = false;
    }
}

static bool reader_fill(zip_reader *r)
{
    size_t n;

    if (r->zs.avail_in != 0u || r->in_left == 0u)
        return true;
    n = r->in_left < (uint64_t)sizeof r->in ? (size_t)r->in_left : sizeof r->in;
    if (!read_at(r->pkg->fd, r->at, r->in, n))
        return false;
    r->at += (uint64_t)n;
    r->in_left -= (uint64_t)n;
    r->zs.next_in = r->in;
    r->zs.avail_in = (uInt)n;
    return true;
}

/* Produces up to `cap` bytes, never more than the member's DECLARED
 * uncompressed size in total. *got == 0 means the member is finished. */
static bool reader_read(zip_reader *r, uint8_t *out, size_t cap, size_t *got, char *why,
                        size_t why_sz)
{
    uint64_t left = r->e->usize - r->produced;
    size_t want;

    *got = 0u;
    if (left == 0u || cap == 0u)
        return true;
    want = (uint64_t)cap < left ? cap : (size_t)left;

    if (r->e->method == METHOD_STORED) {
        if (r->in_left < (uint64_t)want) {
            say(why, why_sz, "%s is truncated", r->e->name);
            return false;
        }
        if (!read_at(r->pkg->fd, r->at, out, want)) {
            say(why, why_sz, "%s is truncated", r->e->name);
            return false;
        }
        r->at += (uint64_t)want;
        r->in_left -= (uint64_t)want;
        *got = want;
    } else {
        r->zs.next_out = out;
        r->zs.avail_out = (uInt)want;
        while (r->zs.avail_out != 0u) {
            int rc;

            if (!reader_fill(r)) {
                say(why, why_sz, "%s is truncated", r->e->name);
                return false;
            }
            if (r->zs.avail_in == 0u) {
                say(why, why_sz, "%s is truncated", r->e->name);
                return false;
            }
            rc = inflate(&r->zs, Z_NO_FLUSH);
            if (rc == Z_STREAM_END)
                break;
            if (rc != Z_OK) {
                say(why, why_sz, "%s is not valid deflate data", r->e->name);
                return false;
            }
        }
        *got = want - (size_t)r->zs.avail_out;
        if (*got == 0u) {
            say(why, why_sz, "%s is truncated", r->e->name);
            return false;
        }
    }

    r->produced += (uint64_t)*got;
    r->crc = (uint32_t)crc32(r->crc, out, (uInt)*got);
    return true;
}

/* Everything that can only be judged once the last byte is out. */
static bool reader_finish(zip_reader *r, char *why, size_t why_sz)
{
    if (r->produced != r->e->usize) {
        say(why, why_sz, "%s is truncated", r->e->name);
        return false;
    }
    if (r->crc != r->e->crc) {
        say(why, why_sz, "bad CRC-32 for file '%s'", r->e->name);
        return false;
    }
    if (r->e->method == METHOD_DEFLATE) {
        /* The declared size is a ceiling and inflate stopped there. If the
         * stream still has output to give, the member lied about its size --
         * which is how a 4 KB entry turns into the whole of RAM on a reader
         * that trusts the stream instead of the number. */
        uint8_t probe;

        for (;;) {
            int rc;

            if (r->zs.avail_in == 0u) {
                if (r->in_left == 0u)
                    break;
                if (!reader_fill(r)) {
                    say(why, why_sz, "%s is truncated", r->e->name);
                    return false;
                }
            }
            r->zs.next_out = &probe;
            r->zs.avail_out = 1u;
            rc = inflate(&r->zs, Z_NO_FLUSH);
            if (r->zs.avail_out == 0u) {
                say(why, why_sz, "%s expands past its declared size of %llu bytes", r->e->name,
                    (unsigned long long)r->e->usize);
                return false;
            }
            if (rc != Z_OK)
                break;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * ==================== package.py starts here ====================
 * ------------------------------------------------------------------ */

static nd_update_err read_member_locked(nd_package *pkg, const zip_entry *e, size_t max,
                                        uint8_t **out, size_t *len, char *why, size_t why_sz)
{
    zip_reader *r = NULL;
    uint8_t *buf = NULL;
    size_t done = 0u;
    nd_update_err rc = ND_UPD_ERR_BAD_ZIP;

    if (e->usize > (uint64_t)max) {
        say(why, why_sz, "%s is %llu bytes, over the %llu byte limit", e->name,
            (unsigned long long)e->usize, (unsigned long long)max);
        return ND_UPD_ERR_BAD_ZIP;
    }

    r = malloc(sizeof *r);
    if (r == NULL) {
        say(why, why_sz, "out of memory");
        return ND_UPD_ERR_UNREADABLE;
    }
    /* +1 so text members come back NUL-terminated; *len stays the truth. */
    buf = malloc((size_t)e->usize + 1u);
    if (buf == NULL) {
        say(why, why_sz, "out of memory");
        free(r);
        return ND_UPD_ERR_UNREADABLE;
    }

    if (!reader_open(pkg, e, r, why, why_sz))
        goto done;
    while (done < (size_t)e->usize) {
        size_t got = 0u;

        if (!reader_read(r, buf + done, (size_t)e->usize - done, &got, why, why_sz))
            goto done;
        if (got == 0u)
            break;
        done += got;
    }
    if (!reader_finish(r, why, why_sz))
        goto done;

    buf[done] = '\0';
    *out = buf;
    *len = done;
    buf = NULL;
    rc = ND_UPD_OK;

done:
    reader_close(r);
    free(r);
    free(buf);
    return rc;
}

nd_update_err nd_package_read_member(nd_package *pkg, const char *name, size_t max, uint8_t **out,
                                     size_t *len, char *why, size_t why_sz)
{
    const zip_entry *e;

    if (out != NULL)
        *out = NULL;
    if (len != NULL)
        *len = 0u;
    if (pkg == NULL || name == NULL || out == NULL || len == NULL) {
        say(why, why_sz, "no package");
        return ND_UPD_ERR_BAD_ZIP;
    }
    e = find_entry(pkg, name);
    if (e == NULL) {
        say(why, why_sz, "package has no %s", name);
        return ND_UPD_ERR_BAD_ZIP;
    }
    return read_member_locked(pkg, e, max, out, len, why, why_sz);
}

/* ---- open ---- */

nd_update_err nd_package_open(const char *path, nd_package **out, char *why, size_t why_sz)
{
    char resolved[ND_PATH_MAX];
    char zip_why[ND_PACKAGE_WHY_MAX];
    nd_package *pkg = NULL;
    struct stat st;
    uint8_t *raw = NULL;
    size_t raw_len = 0u;
    nd_update_err rc;

    if (out == NULL)
        return ND_UPD_ERR_UNREADABLE;
    *out = NULL;
    if (path == NULL || path[0] == '\0') {
        say(why, why_sz, "no such update file: ");
        return ND_UPD_ERR_UNREADABLE;
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        say(why, why_sz, "no such update file: %s", path);
        return ND_UPD_ERR_UNREADABLE;
    }

    pkg = calloc(1u, sizeof *pkg);
    if (pkg == NULL) {
        say(why, why_sz, "out of memory");
        return ND_UPD_ERR_UNREADABLE;
    }
    pkg->fd = -1;
    if (nd_strlcpy(pkg->path, path, sizeof pkg->path) >= sizeof pkg->path) {
        say(why, why_sz, "update path is too long");
        rc = ND_UPD_ERR_UNREADABLE;
        goto fail;
    }

    pkg->fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (pkg->fd < 0) {
        /* package.py splits FileNotFoundError from every other OSError and
         * the two say different things on screen. */
        if (errno == ENOENT)
            say(why, why_sz, "no such update file: %s", path);
        else
            say(why, why_sz, "not a readable zip archive: %s", strerror(errno));
        rc = ND_UPD_ERR_UNREADABLE;
        goto fail;
    }
    if (fstat(pkg->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        say(why, why_sz, "not a readable zip archive: %s", strerror(errno));
        rc = ND_UPD_ERR_UNREADABLE;
        goto fail;
    }
    pkg->file_size = (uint64_t)st.st_size;
    if (pkg->file_size > ND_PACKAGE_MAX_FILE_BYTES) {
        say(why, why_sz, "not a readable zip archive: %llu bytes is over the limit",
            (unsigned long long)pkg->file_size);
        rc = ND_UPD_ERR_BAD_ZIP;
        goto fail;
    }

    zip_why[0] = '\0';
    if (!read_central_directory(pkg, zip_why, sizeof zip_why)) {
        say(why, why_sz, "not a readable zip archive: %s", zip_why);
        rc = ND_UPD_ERR_BAD_ZIP;
        goto fail;
    }

    /* Package.__init__ checks manifest.json FIRST and rootfs.squashfs second,
     * so a package missing both names the manifest. Three screens quote the
     * message. */
    if (find_entry(pkg, ND_PACKAGE_MANIFEST_MEMBER) == NULL) {
        say(why, why_sz, "package has no %s", ND_PACKAGE_MANIFEST_MEMBER);
        rc = ND_UPD_ERR_BAD_MANIFEST;
        goto fail;
    }
    if (find_entry(pkg, ND_PACKAGE_IMAGE_MEMBER) == NULL) {
        say(why, why_sz, "package has no %s", ND_PACKAGE_IMAGE_MEMBER);
        rc = ND_UPD_ERR_BAD_ZIP;
        goto fail;
    }

    rc = nd_package_read_member(pkg, ND_PACKAGE_MANIFEST_MEMBER, ND_PACKAGE_MAX_WHOLE_MEMBER, &raw,
                                &raw_len, zip_why, sizeof zip_why);
    if (rc != ND_UPD_OK) {
        say(why, why_sz, "not a readable zip archive: %s", zip_why);
        goto fail;
    }

    rc = nd_manifest_parse(raw, raw_len, &pkg->manifest, why, why_sz);
    free(raw);
    raw = NULL;
    if (rc != ND_UPD_OK)
        goto fail;

    *out = pkg;
    return ND_UPD_OK;

fail:
    free(raw);
    nd_package_close(pkg);
    return rc;
}

void nd_package_close(nd_package *pkg)
{
    if (pkg == NULL)
        return;
    if (pkg->fd >= 0)
        (void)close(pkg->fd);
    nd_manifest_free(pkg->manifest);
    free(pkg);
}

/* ---- accessors ---- */

const nd_manifest *nd_package_manifest(const nd_package *pkg)
{
    return pkg != NULL ? pkg->manifest : NULL;
}

const char *nd_package_path(const nd_package *pkg)
{
    return pkg != NULL ? pkg->path : "";
}

int64_t nd_package_image_size(const nd_package *pkg)
{
    const zip_entry *e;

    if (pkg == NULL)
        return -1;
    e = find_entry(pkg, ND_PACKAGE_IMAGE_MEMBER);
    if (e == NULL || e->usize > (uint64_t)INT64_MAX)
        return -1;
    return (int64_t)e->usize;
}

bool nd_package_is_signed(const nd_package *pkg)
{
    return pkg != NULL && pkg->is_signed;
}

void nd_package_mark_signed(nd_package *pkg)
{
    if (pkg != NULL)
        pkg->is_signed = true;
}

const uint8_t *nd_package_manifest_raw(const nd_package *pkg, size_t *len_out)
{
    if (len_out != NULL)
        *len_out = 0u;
    if (pkg == NULL || pkg->manifest == NULL)
        return NULL;
    if (len_out != NULL)
        *len_out = pkg->manifest->raw_len;
    return pkg->manifest->raw;
}

size_t nd_package_member_count(const nd_package *pkg)
{
    return pkg != NULL ? pkg->n_entries : 0u;
}

const char *nd_package_member_name(const nd_package *pkg, size_t i)
{
    if (pkg == NULL || i >= pkg->n_entries)
        return NULL;
    return pkg->entries[i].name;
}

int64_t nd_package_member_size(const nd_package *pkg, const char *name)
{
    const zip_entry *e;

    if (pkg == NULL || name == NULL)
        return -1;
    e = find_entry(pkg, name);
    if (e == NULL || e->usize > (uint64_t)INT64_MAX)
        return -1;
    return (int64_t)e->usize;
}

int32_t nd_package_member_method(const nd_package *pkg, const char *name)
{
    const zip_entry *e;

    if (pkg == NULL || name == NULL)
        return -1;
    e = find_entry(pkg, name);
    return e == NULL ? -1 : (int32_t)e->method;
}

/* ---- the signature member ---- */

nd_update_err nd_package_read_signature(nd_package *pkg, uint8_t **out, size_t *len, char *why,
                                        size_t why_sz)
{
    char zip_why[ND_PACKAGE_WHY_MAX];
    nd_update_err rc;

    if (out != NULL)
        *out = NULL;
    if (len != NULL)
        *len = 0u;
    if (pkg == NULL || out == NULL || len == NULL) {
        say(why, why_sz, "update is not signed");
        return ND_UPD_ERR_BAD_SIGNATURE;
    }
    /* package.py catches KeyError from _zip.read() and raises BadSignature,
     * not InvalidUpdate: engineering mode may override a missing signature
     * and may never override a broken archive. */
    if (find_entry(pkg, ND_PACKAGE_SIGNATURE_MEMBER) == NULL) {
        say(why, why_sz, "update is not signed");
        return ND_UPD_ERR_BAD_SIGNATURE;
    }
    zip_why[0] = '\0';
    rc = nd_package_read_member(pkg, ND_PACKAGE_SIGNATURE_MEMBER, ND_PACKAGE_MAX_WHOLE_MEMBER, out,
                                len, zip_why, sizeof zip_why);
    if (rc != ND_UPD_OK) {
        say(why, why_sz, "cannot read the signature: %s", zip_why);
        return ND_UPD_ERR_BAD_SIGNATURE;
    }
    return ND_UPD_OK;
}

/* ---- read_thumbnail ---- */

nd_update_err nd_package_read_thumbnail(nd_package *pkg, uint8_t **out, size_t *len, char *why,
                                        size_t why_sz)
{
    char zip_why[ND_PACKAGE_WHY_MAX];
    char digest[ND_MANIFEST_HEX_MAX];
    const zip_entry *e;
    uint8_t *data = NULL;
    size_t n = 0u;
    nd_update_err rc;

    if (out != NULL)
        *out = NULL;
    if (len != NULL)
        *len = 0u;
    if (pkg == NULL || pkg->manifest == NULL || out == NULL || len == NULL)
        return ND_UPD_ERR_BAD_MANIFEST;

    /* "Only art the manifest vouches for comes back: the signature covers
     * manifest.json alone, so a thumbnail with no hash recorded there is an
     * unsigned attachment and is treated as absent." */
    if (pkg->manifest->thumbnail_sha256[0] == '\0')
        return ND_UPD_OK;
    e = find_entry(pkg, ND_PACKAGE_THUMBNAIL_MEMBER);
    if (e == NULL)
        return ND_UPD_OK;

    /* Refused before a byte is read: a 64x64 PNG is a couple of kilobytes and
     * a 64 MB phone must not be asked to decompress a photo album. */
    if (e->usize > (uint64_t)ND_PACKAGE_MAX_THUMBNAIL_BYTES) {
        say(why, why_sz, "thumbnail is %llu bytes, over the %llu byte limit",
            (unsigned long long)e->usize, (unsigned long long)ND_PACKAGE_MAX_THUMBNAIL_BYTES);
        return ND_UPD_ERR_BAD_MANIFEST;
    }

    zip_why[0] = '\0';
    rc = read_member_locked(pkg, e, ND_PACKAGE_MAX_THUMBNAIL_BYTES, &data, &n, zip_why,
                            sizeof zip_why);
    if (rc != ND_UPD_OK) {
        say(why, why_sz, "cannot read the thumbnail: %s", zip_why);
        return rc;
    }

    nd_package_sha256_hex(data, n, digest, sizeof digest);
    /* A byte comparison, case-sensitive. An uppercase hash in the manifest
     * therefore never matches -- see nd_manifest.h on why that is kept. */
    if (strcmp(digest, pkg->manifest->thumbnail_sha256) != 0) {
        say(why, why_sz, "thumbnail does not match the manifest");
        free(data);
        return ND_UPD_ERR_BAD_MANIFEST;
    }
    *out = data;
    *len = n;
    return ND_UPD_OK;
}

/* ---- extract_image ---- */

/* package.py's _free_space(): statvfs, and None (meaning "skip the check")
 * on any failure. */
static bool free_space_of(const char *dir, uint64_t *out)
{
    struct statvfs vfs;

    if (statvfs(dir, &vfs) != 0)
        return false;
    *out = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    return true;
}

static void dir_of(const char *path, char *out, size_t out_sz)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        (void)nd_strlcpy(out, ".", out_sz);
        return;
    }
    if (slash == path) {
        (void)nd_strlcpy(out, "/", out_sz);
        return;
    }
    (void)nd_strlcpy(out, path, (size_t)(slash - path) + 1u);
}

nd_update_err nd_package_extract_image(nd_package *pkg, const char *dest,
                                       nd_package_progress_fn progress, void *user,
                                       int64_t free_bytes, char *why, size_t why_sz)
{
    char resolved[ND_PATH_MAX];
    char dir[ND_PATH_MAX];
    char digest[ND_MANIFEST_HEX_MAX];
    const zip_entry *e;
    zip_reader *r = NULL;
    uint8_t *chunk = NULL;
    FILE *target = NULL;
    sha256_ctx ctx;
    uint8_t raw_digest[32];
    uint64_t total;
    uint64_t done = 0u;
    uint64_t have = 0u;
    nd_update_err rc = ND_UPD_ERR_WRITE_FAILED;

    if (pkg == NULL || pkg->manifest == NULL || dest == NULL) {
        say(why, why_sz, "no package");
        return ND_UPD_ERR_BAD_ZIP;
    }
    e = find_entry(pkg, ND_PACKAGE_IMAGE_MEMBER);
    if (e == NULL) {
        say(why, why_sz, "package has no %s", ND_PACKAGE_IMAGE_MEMBER);
        return ND_UPD_ERR_BAD_ZIP;
    }
    total = e->usize;

    if (nd_path_resolve(resolved, sizeof resolved, dest) != ND_OK) {
        say(why, why_sz, "cannot write %s", dest);
        return ND_UPD_ERR_WRITE_FAILED;
    }
    dir_of(resolved, dir, sizeof dir);

    if (free_bytes < 0) {
        /* A statvfs that fails skips the check entirely, which is what the
         * Python's `free_bytes is not None` guard does. */
        if (free_space_of(dir, &have))
            free_bytes = have > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)have;
    }
    if (free_bytes >= 0 && (uint64_t)free_bytes < total + ND_PACKAGE_SPACE_MARGIN) {
        say(why, why_sz, "image needs %llu bytes, only %lld free", (unsigned long long)total,
            (long long)free_bytes);
        return ND_UPD_ERR_NO_SPACE;
    }

    r = malloc(sizeof *r);
    chunk = malloc(ND_PACKAGE_CHUNK);
    if (r == NULL || chunk == NULL) {
        say(why, why_sz, "out of memory");
        rc = ND_UPD_ERR_UNREADABLE;
        goto done;
    }
    if (!reader_open(pkg, e, r, why, why_sz)) {
        rc = ND_UPD_ERR_BAD_ZIP;
        goto done;
    }

    target = fopen(resolved, "wb");
    if (target == NULL) {
        say(why, why_sz, "cannot write %s: %s", dest, strerror(errno));
        rc = ND_UPD_ERR_WRITE_FAILED;
        goto done;
    }

    sha256_init(&ctx);
    for (;;) {
        size_t got = 0u;

        if (!reader_read(r, chunk, ND_PACKAGE_CHUNK, &got, why, why_sz)) {
            rc = ND_UPD_ERR_BAD_ZIP;
            goto done;
        }
        if (got == 0u)
            break;
        if (fwrite(chunk, 1u, got, target) != got) {
            say(why, why_sz, "cannot write %s: %s", dest, strerror(errno));
            rc = ND_UPD_ERR_WRITE_FAILED;
            goto done;
        }
        sha256_update(&ctx, chunk, got);
        done += (uint64_t)got;
        if (progress != NULL)
            progress(done, total, user);
    }
    if (!reader_finish(r, why, why_sz)) {
        rc = ND_UPD_ERR_BAD_ZIP;
        goto done;
    }
    if (fflush(target) != 0 || fsync(fileno(target)) != 0) {
        say(why, why_sz, "cannot write %s: %s", dest, strerror(errno));
        rc = ND_UPD_ERR_WRITE_FAILED;
        goto done;
    }
    if (fclose(target) != 0) {
        target = NULL;
        say(why, why_sz, "cannot write %s: %s", dest, strerror(errno));
        rc = ND_UPD_ERR_WRITE_FAILED;
        goto done;
    }
    target = NULL;

    sha256_final(&ctx, raw_digest);
    hex_of(raw_digest, digest, sizeof digest);
    if (strcmp(digest, pkg->manifest->sha256) != 0) {
        say(why, why_sz, "image sha256 does not match the manifest (%s != %s)", digest,
            pkg->manifest->sha256);
        rc = ND_UPD_ERR_BAD_MANIFEST;
        goto done;
    }
    rc = ND_UPD_OK;

done:
    reader_close(r);
    free(r);
    free(chunk);
    if (target != NULL)
        (void)fclose(target);
    /* "On any mismatch the partial file is removed: a half-written image must
     * never be left behind where the applier could find it." */
    if (rc != ND_UPD_OK)
        (void)unlink(resolved);
    return rc;
}
