/* nd_manifest.c -- manifest.json parsing, validation and the compatibility
 * rules, ported from System/core/UpdateService/manifest.py.
 *
 * The header carries the reasoning. What is worth repeating beside the code
 * is the ordering rule: the checks below appear in manifest.py's order and
 * produce manifest.py's exact wording, because a manifest broken in two ways
 * must name the same field the Python would have named -- three test suites
 * match on that field name.
 *
 * Nothing here does I/O. The bytes arrive from nd_package.c, which read them
 * out of the zip, and they are kept verbatim for the signature check.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_json.h"
#include "nd_manifest.h"
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
 * _version_tuple / tuple ordering
 * ------------------------------------------------------------------ */

void nd_kver_parse(const char *text, nd_kver *out)
{
    size_t i = 0u;

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (text == NULL)
        return;

    for (;;) {
        uint64_t value = 0u;
        size_t digits = 0u;

        while (text[i] >= '0' && text[i] <= '9') {
            /* Saturating rather than wrapping: a hostile "999...9" must not
             * be able to alias a small version number. */
            if (value <= UINT32_MAX)
                value = value * 10u + (uint64_t)(text[i] - '0');
            digits++;
            i++;
        }
        /* "if not digits: break" -- a chunk with no leading digit STOPS the
         * whole walk, it does not skip the chunk. "6.x.3" is (6,), not
         * (6, 3), and the difference decides a kernel gate. */
        if (digits == 0u)
            break;

        if (out->n < (uint8_t)ND_KVER_MAX_PARTS) {
            out->part[out->n] = value > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)value;
            out->n = (uint8_t)(out->n + 1u);
        }

        /* Skip the rest of this chunk ("47-rt" -> the "-rt"), then the dot.
         * No dot means the string is finished. */
        while (text[i] != '\0' && text[i] != '.')
            i++;
        if (text[i] != '.')
            break;
        i++;
    }
}

int nd_kver_cmp(const nd_kver *a, const nd_kver *b)
{
    size_t n;
    size_t i;

    if (a == NULL || b == NULL)
        return 0;
    n = a->n < b->n ? (size_t)a->n : (size_t)b->n;
    for (i = 0u; i < n; i++) {
        if (a->part[i] != b->part[i])
            return a->part[i] < b->part[i] ? -1 : 1;
    }
    /* A prefix is smaller than what it is a prefix of: (6,12) < (6,12,47). */
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 * _hex(value, field, length)
 * ------------------------------------------------------------------ */

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Exactly the set bytes.fromhex() skips. */
static bool is_hex_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* bytes.fromhex(value), reduced to the one question manifest.py asks of it:
 * how many BYTES would it have produced? Returns false on the ValueError.
 *
 * Whitespace is skipped between pairs but never inside one -- "aa bb" is two
 * bytes and "a b" is an error, which is what CPython does and is not what a
 * "strip all whitespace then check the length" reader does. */
static bool hex_byte_count(const char *value, size_t *out)
{
    size_t i = 0u;
    size_t bytes = 0u;

    for (;;) {
        while (is_hex_space(value[i]))
            i++;
        if (value[i] == '\0')
            break;
        if (!is_hex_digit(value[i]) || !is_hex_digit(value[i + 1u]))
            return false;
        i += 2u;
        bytes++;
    }
    *out = bytes;
    return true;
}

/* manifest.py's _hex(). `length` is the required BYTE count, or 0 for "any
 * length" (the salt). Returns the value unchanged on success -- the caller
 * copies it verbatim. */
static bool check_hex(const nd_json_val *v, const char *field, size_t length, const char **out,
                      char *why, size_t why_sz)
{
    const char *text = NULL;
    size_t bytes = 0u;

    if (!nd_json_str(v, &text)) {
        say(why, why_sz, "%s must be a hex string", field);
        return false;
    }
    if (!hex_byte_count(text, &bytes)) {
        say(why, why_sz, "%s is not valid hex", field);
        return false;
    }
    if (length != 0u && bytes != length) {
        say(why, why_sz, "%s must be %d hex digits", field, (int)(length * 2u));
        return false;
    }
    *out = text;
    return true;
}

/* ------------------------------------------------------------------ *
 * Python truthiness, for `body.get(k) or ""`
 * ------------------------------------------------------------------ */

static bool json_truthy(const nd_json_val *v)
{
    const char *s = NULL;
    int64_t i = 0;
    double r = 0.0;
    bool b = false;

    if (v == NULL)
        return false;
    switch (nd_json_type_of(v)) {
    case ND_JSON_NULL:
        return false;
    case ND_JSON_BOOL:
        return nd_json_bool(v, &b) && b;
    case ND_JSON_INT:
        return nd_json_int(v, &i) && i != 0;
    case ND_JSON_REAL:
        return nd_json_real(v, &r) && r != 0.0;
    case ND_JSON_STRING:
        return nd_json_str(v, &s) && s[0] != '\0';
    case ND_JSON_ARRAY:
    case ND_JSON_OBJECT:
    default:
        return nd_json_len(v) != 0u;
    }
}

/* ------------------------------------------------------------------ *
 * Bounded copies
 * ------------------------------------------------------------------ *
 *
 * DIVERGENCE (1): Python stores whatever length it is given. Truncating here
 * would be silent and load-bearing -- a truncated version reaches
 * pending.prop, a truncated salt builds the wrong dm-verity table and the
 * phone will not boot -- so an oversized field is a refusal instead.
 */
static bool copy_bounded(char *dst, size_t dst_sz, const char *src, const char *field, char *why,
                         size_t why_sz)
{
    if (nd_strlcpy(dst, src, dst_sz) >= dst_sz) {
        dst[0] = '\0';
        say(why, why_sz, "%s is longer than %d characters", field, (int)(dst_sz - 1u));
        return false;
    }
    return true;
}

/* `body.get(k) or ""` for a field that must be a string when it is there at
 * all. DIVERGENCE (4): Python accepts any truthy object and str()s it later. */
static bool optional_string(const nd_json_val *body, const char *key, const char **out, char *why,
                            size_t why_sz)
{
    const nd_json_val *v = nd_json_get(body, key);

    *out = "";
    if (!json_truthy(v))
        return true;
    if (!nd_json_str(v, out)) {
        *out = "";
        say(why, why_sz, "%s must be a string", key);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * parse()
 * ------------------------------------------------------------------ */

static const char *const REQUIRED[] = {ND_UPD_KEY_VERSION, ND_UPD_KEY_BUILDTIME,
                                       ND_UPD_KEY_PLATFORM, ND_UPD_KEY_SHA256, ND_UPD_KEY_VERITY};

static const char *const REQUIRED_VERITY[] = {ND_UPD_KEY_ROOT_HASH, ND_UPD_KEY_BLOCK_SIZE,
                                              ND_UPD_KEY_IMAGE_BLOCKS};

static bool read_int_field(const nd_json_val *obj, const char *key, int64_t *out)
{
    const nd_json_val *v = nd_json_get(obj, key);

    /* nd_json_int() answers false for ND_JSON_BOOL and ND_JSON_REAL, which is
     * the whole reason those are separate types: "buildtime": true and
     * "buildtime": 1785160800.0 are both refusals. */
    return nd_json_int(v, out);
}

nd_update_err nd_manifest_parse(const uint8_t *raw, size_t len, nd_manifest **out, char *why,
                                size_t why_sz)
{
    nd_update_err rc = ND_UPD_ERR_BAD_MANIFEST;
    nd_json_doc *doc = NULL;
    const nd_json_val *body = NULL;
    const nd_json_val *verity = NULL;
    nd_manifest *m = NULL;
    char json_err[128];
    const char *text = NULL;
    const char *changelog = "";
    const char *min_kernel = "";
    const char *thumb = "";
    size_t i;

    if (out == NULL)
        return ND_UPD_ERR_BAD_MANIFEST;
    *out = NULL;
    if (raw == NULL) {
        say(why, why_sz, "manifest is not valid JSON: no manifest");
        return ND_UPD_ERR_BAD_MANIFEST;
    }

    json_err[0] = '\0';
    if (nd_json_parse(raw, len, &doc, json_err, sizeof json_err) != ND_OK) {
        say(why, why_sz, "manifest is not valid JSON: %s", json_err);
        return ND_UPD_ERR_BAD_MANIFEST;
    }
    body = nd_json_root(doc);
    if (nd_json_type_of(body) != ND_JSON_OBJECT) {
        say(why, why_sz, "manifest must be a JSON object");
        goto done;
    }

    for (i = 0u; i < ND_ARRAY_LEN(REQUIRED); i++) {
        if (nd_json_get(body, REQUIRED[i]) == NULL) {
            say(why, why_sz, "manifest is missing %s", REQUIRED[i]);
            goto done;
        }
    }

    m = calloc(1u, sizeof *m);
    if (m == NULL) {
        say(why, why_sz, "out of memory reading the manifest");
        rc = ND_UPD_ERR_UNREADABLE;
        goto done;
    }

    /* ---- version ---- */
    if (!nd_json_str(nd_json_get(body, ND_UPD_KEY_VERSION), &text) || text[0] == '\0') {
        say(why, why_sz, "version must be a non-empty string");
        goto done;
    }
    if (!copy_bounded(m->version, sizeof m->version, text, ND_UPD_KEY_VERSION, why, why_sz))
        goto done;

    /* ---- platform ---- */
    if (!nd_json_str(nd_json_get(body, ND_UPD_KEY_PLATFORM), &text) || text[0] == '\0') {
        say(why, why_sz, "platform must be a non-empty string");
        goto done;
    }
    if (!copy_bounded(m->platform, sizeof m->platform, text, ND_UPD_KEY_PLATFORM, why, why_sz))
        goto done;

    /* ---- buildtime ---- */
    if (!read_int_field(body, ND_UPD_KEY_BUILDTIME, &m->buildtime)) {
        say(why, why_sz, "buildtime must be a unix timestamp");
        goto done;
    }

    /* ---- sha256 ---- */
    if (!check_hex(nd_json_get(body, ND_UPD_KEY_SHA256), ND_UPD_KEY_SHA256, 32u, &text, why,
                   why_sz))
        goto done;
    if (!copy_bounded(m->sha256, sizeof m->sha256, text, ND_UPD_KEY_SHA256, why, why_sz))
        goto done;

    /* ---- thumbnail_sha256, only when truthy ---- */
    if (json_truthy(nd_json_get(body, ND_UPD_KEY_THUMB_SHA))) {
        if (!check_hex(nd_json_get(body, ND_UPD_KEY_THUMB_SHA), ND_UPD_KEY_THUMB_SHA, 32u, &thumb,
                       why, why_sz))
            goto done;
    }

    /* ---- verity ---- */
    verity = nd_json_get(body, ND_UPD_KEY_VERITY);
    if (nd_json_type_of(verity) != ND_JSON_OBJECT) {
        say(why, why_sz, "verity must be a JSON object");
        goto done;
    }
    for (i = 0u; i < ND_ARRAY_LEN(REQUIRED_VERITY); i++) {
        if (nd_json_get(verity, REQUIRED_VERITY[i]) == NULL) {
            say(why, why_sz, "verity is missing %s", REQUIRED_VERITY[i]);
            goto done;
        }
    }

    if (!check_hex(nd_json_get(verity, ND_UPD_KEY_ROOT_HASH), ND_UPD_KEY_ROOT_HASH, 32u, &text, why,
                   why_sz))
        goto done;
    if (!copy_bounded(m->verity_root_hash, sizeof m->verity_root_hash, text, ND_UPD_KEY_ROOT_HASH,
                      why, why_sz))
        goto done;

    /* verity["salt"] = _hex(verity.get("salt") or "", "salt") -- any length,
     * and written back so the field is always present. "" hexes to zero
     * bytes, which is why an absent salt is not an error. staging then writes
     * verity_salt= (empty) and the shell's verity_table() turns that into a
     * bare "-". */
    text = "";
    if (json_truthy(nd_json_get(verity, ND_UPD_KEY_SALT))) {
        if (!check_hex(nd_json_get(verity, ND_UPD_KEY_SALT), ND_UPD_KEY_SALT, 0u, &text, why,
                       why_sz))
            goto done;
    }
    if (!copy_bounded(m->verity_salt, sizeof m->verity_salt, text, ND_UPD_KEY_SALT, why, why_sz))
        goto done;

    /* ---- block_size ---- */
    if (!read_int_field(verity, ND_UPD_KEY_BLOCK_SIZE, &m->verity_block_size) ||
        m->verity_block_size < 512 || (m->verity_block_size & (m->verity_block_size - 1)) != 0) {
        say(why, why_sz, "block_size must be a power of two >= 512");
        goto done;
    }

    /* ---- image_blocks ---- */
    if (!read_int_field(verity, ND_UPD_KEY_IMAGE_BLOCKS, &m->verity_image_blocks) ||
        m->verity_image_blocks < 1) {
        say(why, why_sz, "image_blocks must be a positive integer");
        goto done;
    }

    /* ---- the optional strings, which Manifest.__init__ reads last ---- */
    if (!optional_string(body, ND_UPD_KEY_MIN_KERNEL, &min_kernel, why, why_sz))
        goto done;
    if (!copy_bounded(m->min_kernel, sizeof m->min_kernel, min_kernel, ND_UPD_KEY_MIN_KERNEL, why,
                      why_sz))
        goto done;
    if (!copy_bounded(m->thumbnail_sha256, sizeof m->thumbnail_sha256, thumb, ND_UPD_KEY_THUMB_SHA,
                      why, why_sz))
        goto done;
    if (!optional_string(body, ND_UPD_KEY_CHANGELOG, &changelog, why, why_sz))
        goto done;

    m->changelog = strdup(changelog);
    if (m->changelog == NULL) {
        say(why, why_sz, "out of memory reading the manifest");
        rc = ND_UPD_ERR_UNREADABLE;
        goto done;
    }

    /* The signed bytes, kept exactly as they arrived. */
    m->raw = malloc(len + 1u);
    if (m->raw == NULL) {
        say(why, why_sz, "out of memory reading the manifest");
        rc = ND_UPD_ERR_UNREADABLE;
        goto done;
    }
    if (len > 0u)
        memcpy(m->raw, raw, len);
    m->raw[len] = '\0'; /* convenience for debuggers only; raw_len is truth */
    m->raw_len = len;

    *out = m;
    m = NULL;
    rc = ND_UPD_OK;

done:
    nd_manifest_free(m);
    nd_json_free(doc);
    return rc;
}

void nd_manifest_free(nd_manifest *m)
{
    if (m == NULL)
        return;
    free(m->changelog);
    free(m->raw);
    free(m);
}

/* ------------------------------------------------------------------ *
 * Derived values
 * ------------------------------------------------------------------ */

uint64_t nd_manifest_image_bytes(const nd_manifest *m)
{
    uint64_t blocks;
    uint64_t size;

    if (m == NULL || m->verity_image_blocks <= 0 || m->verity_block_size <= 0)
        return 0u;
    blocks = (uint64_t)m->verity_image_blocks;
    size = (uint64_t)m->verity_block_size;
    if (blocks > UINT64_MAX / size)
        return UINT64_MAX;
    return blocks * size;
}

uint64_t nd_manifest_hash_offset(const nd_manifest *m)
{
    /* "Byte offset of the verity hash area: straight after the squashfs."
     * Identical arithmetic to image_bytes, and manifest.py spells it twice
     * for the same reason: the two names mean different things to a reader
     * even though they are the same number. */
    return nd_manifest_image_bytes(m);
}

/* ------------------------------------------------------------------ *
 * check_compatible()
 * ------------------------------------------------------------------ */

/* ============ THE ONE ALIAS, AND WHY IT IS SHAPED LIKE THIS ============
 *
 * DECISIONS.md D1 renamed the emulator's compatibility key from qemu-aarch64
 * to qemu-armv7, and every QEMU image ever flashed carries the old one. This
 * table is what stops those images being stranded on their own update line.
 *
 * It is a list of ORDERED PAIRS and it must never become anything else. Not a
 * platform "family", not a prefix compare, not a canonicalising lookup with a
 * fallback bucket -- the last of those is the worst, because a bucket makes
 * two platforms that are BOTH unrecognised compare equal, and
 * ND_SET_OS_PLATFORM_DFLT is "unknown" on any image whose version.prop did
 * not survive. An image that knows nothing about itself must match nothing.
 *
 * Ordered, because the two directions are not the same question:
 *
 *   image qemu-aarch64 + package qemu-armv7 is the one entry. It works only
 *   because that kernel is built with CONFIG_COMPAT, so an armv7 userland
 *   runs on it. What it produces is not pretty -- uname reports armv8l, which
 *   nd_nap_arch_for_machine() maps to "", so the image can install no .nap at
 *   all -- but it BOOTS, and it is a step the owner asked for onto the line
 *   that is still being published.
 *
 *   image qemu-armv7 + package qemu-aarch64 is the reverse and is a BRICK.
 *   There is no kernel in an .ndsw (AGENTS.md; manifest.py only checks
 *   min_kernel), so an armv7 kernel would switch_root into an aarch64
 *   filesystem with no runnable init in it. Nothing in it executes. The
 *   Downgrade app reaches that direction from the network, which is exactly
 *   why the pair is ordered and not symmetric.
 *
 * And luckfox-armv7 is not in the table, in either column, ever. A phone that
 * accepts a QEMU package is unrecoverable without a bench, a cable and
 * upgrade_tool; the whole point of keeping two platform ids after the ABI
 * collapsed is that this comparison still refuses. test_manifest.c walks the
 * full cross-product with only the diagonal and this one pair accepted, and
 * carries three rows -- luckfox-armv7 vs luckfox-armv7x, vs luckfox, and the
 * reverse -- whose only job is to fail the day this becomes a strncmp or a
 * split on '-'.
 *
 * ============ AND THE ONE BUILD THAT MAKES IT REACHABLE ============
 *
 * READ THIS BEFORE BELIEVING THE PAIR RESCUES ANYTHING BY ITSELF. This
 * function runs in the libneodct of the image that is RUNNING. An image
 * flashed before the rename is running the code as it stood then -- a bare
 * strcmp with no table -- so it refuses a qemu-armv7 package however the
 * package reaches it, and nd_remote_asset_name() on it still asks for
 * UPDATE-qemu-aarch64.ndsw, which no release cut after the rename carries.
 * The alias is in the rootfs it cannot reach. Left there, it would widen the
 * one comparison standing between a machine and an unbootable install for a
 * path nothing can take.
 *
 * What makes it reachable is a TRANSITIONAL BUILD, and it is buildable from
 * this tree today because neodct/scripts/platform-id.sh still maps the
 * retired tag:
 *
 *     BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS="... qemu-aarch64"
 *
 * on neodct_qemu_defconfig produces an armv7 image stamped, consistently,
 * with the old key: /NeoDCT/platform says qemu, ND_BUILD_PLATFORM is
 * ND_PLATFORM_QEMU, version.prop says qemu-aarch64. The old image's bare
 * strcmp accepts that package, the aarch64 kernel it is still booting runs
 * the armv7 userland because that kernel has CONFIG_COMPAT, and what comes
 * up carries THIS code while still calling itself qemu-aarch64. That is the
 * one image on which the pair below fires, and from it one qemu-armv7
 * package finishes the move.
 *
 * So the pair is not the migration; it is the second half of one. The first
 * half is a release built under the retired tag, and a release that does not
 * carry one leaves those images where they were. AGENTS.md, release.sh and
 * docs/TESTING_UPDATES.md say the same thing rather than promising a rescue
 * that needs a build nobody made.
 *
 * `image` is what this machine says it is; `package` is what the manifest was
 * built for. Both are whole strings. */
static bool platform_alias_accepts(const char *image, const char *package)
{
    static const struct {
        const char *image;
        const char *package;
    } PAIRS[] = {
        {"qemu-aarch64", "qemu-armv7"},
    };
    size_t i;

    for (i = 0u; i < sizeof PAIRS / sizeof PAIRS[0]; i++) {
        if (strcmp(image, PAIRS[i].image) == 0 && strcmp(package, PAIRS[i].package) == 0)
            return true;
    }
    return false;
}

nd_update_err nd_manifest_check_compatible(const nd_manifest *m, const char *platform,
                                           const char *kernel, char *why, size_t why_sz)
{
    nd_kver want;
    nd_kver have;

    if (m == NULL)
        return ND_UPD_ERR_BAD_MANIFEST;
    if (platform == NULL)
        platform = "";

    /* An exact byte comparison, deliberately, and then one named exception.
     * The Luckfox and QEMU images share a filename and installing the wrong
     * one is unrecoverable without a reflash -- which is why the exception is
     * a single ordered pair with the reasoning above it rather than any kind
     * of matching rule. The refusal SENTENCE is unchanged and pinned
     * byte-for-byte by test_manifest.c and test_c_manifest_matches_python.py:
     * it names both sides, which is what a developer needs.
     *
     * NOTHING ON THE DOWNLOAD SIDE KNOWS ABOUT THIS. nd_remote_asset_name()
     * builds one name from one platform string and never looks for the
     * other's; a fallback there would pull 58 MB over a bearer that can take
     * an hour before this function got a chance to say no. */
    if (strcmp(m->platform, platform) != 0 && !platform_alias_accepts(platform, m->platform)) {
        say(why, why_sz, "update is for %s, this is %s", m->platform, platform);
        return ND_UPD_ERR_INCOMPATIBLE;
    }

    /* "if self.min_kernel and kernel" -- either being empty skips the gate. */
    if (m->min_kernel[0] == '\0' || kernel == NULL || kernel[0] == '\0')
        return ND_UPD_OK;

    nd_kver_parse(m->min_kernel, &want);
    nd_kver_parse(kernel, &have);
    if (nd_kver_cmp(&have, &want) < 0) {
        say(why, why_sz, "update needs kernel %s, running %s", m->min_kernel, kernel);
        return ND_UPD_ERR_INCOMPATIBLE;
    }
    return ND_UPD_OK;
}
