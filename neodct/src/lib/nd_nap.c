/* nd_nap.c -- reading and installing .nap packages. See nd_nap.h.
 *
 * ============ ONE READER, TWO PASSES ============
 *
 * walk() reads the archive from the front, one 512-byte header at a time,
 * and hands every entry to a callback with its name already validated and
 * classified. nd_nap_inspect() is that walk with a callback that only
 * looks; nd_nap_install() is nd_nap_inspect() followed by the same walk with
 * a callback that writes. Doing the whole validation pass BEFORE the first
 * byte is written is what makes "refused" mean "nothing happened": a package
 * with a bad entry as its last member is refused without a staging directory
 * ever being created.
 *
 * ============ THE ARCHIVE IS NEVER TRUSTED TO BE WHAT IT SAYS ============
 *
 * Every number comes out of an ASCII octal field in a file off a removable
 * card. A size is checked against the bound before it is used to seek, a
 * checksum is verified before any field is believed, and a name is rebuilt
 * component by component rather than handed to open(). The seeks are what
 * keep this cheap: a 1.7 MB data file is skipped over during the inspect
 * pass in one fseek, not read.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "nd_json.h"
#include "nd_log.h"
#include "nd_nap.h"
#include "nd_paths.h"
#include "nd_storage.h"

/* ------------------------------------------------------------------ *
 * The ustar header
 * ------------------------------------------------------------------ */

#define TAR_BLOCK 512u

/* POSIX.1-1988 ustar, byte for byte. The GNU format differs only in fields
 * this file never reads, and in the "ustar  " magic, which is accepted
 * because `tar` on a developer's machine writes it by default. */
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} tar_header;

typedef enum {
    ENT_DIR = 0,   /* a directory: made, never a problem                  */
    ENT_FILE,      /* an ordinary file of the app's                       */
    ENT_MANIFEST,  /* manifest.json, written last                         */
    ENT_APPSO_TOP, /* app.so at the root: the one-phone shape             */
    ENT_APPSO_LIB  /* lib/<arch>/app.so: the universal shape              */
} entry_kind;

typedef struct {
    char name[ND_PATH_MAX]; /* relative, normalised: no "./", no trailing "/" */
    entry_kind kind;
    char arch[ND_NAP_ARCH_MAX]; /* ENT_APPSO_LIB only */
    uint64_t size;
    long data_offset; /* where the bytes start in the file */
} tar_entry;

/* A callback sees every entry once, in archive order, with `f` positioned
 * at the entry's first data byte. It may read the data; walk() reseeks
 * afterwards regardless. Returning anything but ND_OK stops the walk. */
typedef nd_err (*entry_fn)(FILE *f, const tar_entry *e, void *ctx, char *why, size_t why_sz);

static void say(char *why, size_t why_sz, const char *text)
{
    if (why != NULL && why_sz > 0u)
        (void)nd_strlcpy(why, text, why_sz);
}

/* An octal field: leading spaces, digits, then NUL or space. Anything else
 * -- including the base-256 form GNU tar uses past 8 GB, which starts with
 * a byte >= 0x80 -- is refused. */
static bool parse_octal(const char *field, size_t len, uint64_t *out)
{
    uint64_t v = 0u;
    size_t i = 0u;
    bool any = false;

    while (i < len && field[i] == ' ')
        i++;
    for (; i < len; i++) {
        char c = field[i];

        if (c == '\0' || c == ' ')
            break;
        if (c < '0' || c > '7')
            return false;
        if (v > (UINT64_MAX >> 3))
            return false;
        v = (v << 3) | (uint64_t)(c - '0');
        any = true;
    }
    if (!any)
        return false;
    *out = v;
    return true;
}

static bool block_is_zero(const uint8_t *b)
{
    size_t i;

    for (i = 0u; i < TAR_BLOCK; i++) {
        if (b[i] != 0u)
            return false;
    }
    return true;
}

/* The checksum is the byte sum of the header with the checksum field itself
 * read as eight spaces. Some old writers summed SIGNED chars; both are
 * accepted, as every reader does, because a header that matches either was
 * written by a tar and not by chance. */
static bool checksum_ok(const uint8_t *b)
{
    uint64_t want;
    uint64_t unsigned_sum = 0u;
    int64_t signed_sum = 0;
    size_t i;

    if (!parse_octal((const char *)b + 148, 8u, &want))
        return false;
    for (i = 0u; i < TAR_BLOCK; i++) {
        uint8_t byte = (i >= 148u && i < 156u) ? (uint8_t)' ' : b[i];

        unsigned_sum += byte;
        signed_sum += (int8_t)byte;
    }
    return want == unsigned_sum || (signed_sum >= 0 && want == (uint64_t)signed_sum);
}

/* ------------------------------------------------------------------ *
 * Names
 * ------------------------------------------------------------------ */

/* Rebuilds the entry's path one component at a time into `out`, refusing
 * everything nd_nap.h lists. "./a/b/" comes out as "a/b". */
static bool normalise_name(const char *raw, char *out, size_t out_sz)
{
    const char *p = raw;
    size_t n = 0u;

    if (raw == NULL || raw[0] == '/' || strchr(raw, '\\') != NULL)
        return false;

    while (*p != '\0') {
        const char *end = strchr(p, '/');
        size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);

        if (len == 0u) {
            /* "a//b" is not a name any packer writes. */
            if (end == NULL || (n > 0u && *(end + 1) != '\0'))
                return false;
            p = end + 1;
            continue;
        }
        if ((len == 1u && p[0] == '.') || (len == 2u && p[0] == '.' && p[1] == '.')) {
            /* A leading "./" is what `tar cf x .` writes and is harmless; a
             * "." anywhere else and ".." anywhere at all are refused. */
            if (len == 2u || n > 0u)
                return false;
        } else {
            if (n + len + 2u > out_sz)
                return false;
            if (n > 0u)
                out[n++] = '/';
            memcpy(out + n, p, len);
            n += len;
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
    out[n] = '\0';
    return true;
}

static bool arch_tag_ok(const char *tag)
{
    size_t i;

    if (tag == NULL || tag[0] == '\0' || strlen(tag) >= ND_NAP_ARCH_MAX)
        return false;
    for (i = 0u; tag[i] != '\0'; i++) {
        char c = tag[i];

        if (!isalnum((unsigned char)c) && c != '-' && c != '_')
            return false;
    }
    return true;
}

/* Which of nd_nap.h's five kinds this name is, or false for a name the
 * layout reserves and the package may not use. */
static bool classify(const char *name, bool is_dir, tar_entry *e, char *why, size_t why_sz)
{
    static const char DATA[] = ND_PATH_APP_DATA_NAME;
    static const char LIB[] = "lib";

    e->arch[0] = '\0';

    if (strcmp(name, DATA) == 0 ||
        (strncmp(name, DATA, sizeof DATA - 1u) == 0 && name[sizeof DATA - 1u] == '/')) {
        say(why, why_sz, "Package tries to write the\napp's data folder.");
        return false;
    }
    if (is_dir) {
        e->kind = ENT_DIR;
        return true;
    }
    if (strcmp(name, "manifest.json") == 0) {
        e->kind = ENT_MANIFEST;
        return true;
    }
    if (strcmp(name, "app.so") == 0) {
        e->kind = ENT_APPSO_TOP;
        return true;
    }
    if (strcmp(name, LIB) == 0 ||
        (strncmp(name, LIB, sizeof LIB - 1u) == 0 && name[sizeof LIB - 1u] == '/')) {
        /* lib/<tag>/app.so and nothing else. */
        const char *tag = name + sizeof LIB;
        const char *slash = strchr(tag, '/');
        size_t tag_len;

        if (name[sizeof LIB - 1u] != '/' || slash == NULL || strcmp(slash + 1, "app.so") != 0) {
            say(why, why_sz, "Package misuses its lib folder.");
            return false;
        }
        tag_len = (size_t)(slash - tag);
        if (tag_len == 0u || tag_len >= ND_NAP_ARCH_MAX) {
            say(why, why_sz, "Package misuses its lib folder.");
            return false;
        }
        memcpy(e->arch, tag, tag_len);
        e->arch[tag_len] = '\0';
        if (!arch_tag_ok(e->arch)) {
            say(why, why_sz, "Package misuses its lib folder.");
            return false;
        }
        e->kind = ENT_APPSO_LIB;
        return true;
    }
    e->kind = ENT_FILE;
    return true;
}

/* ------------------------------------------------------------------ *
 * The walk
 * ------------------------------------------------------------------ */

static nd_err walk(const char *path, entry_fn fn, void *ctx, char *why, size_t why_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    uint64_t file_len;
    size_t n_entries = 0u;
    nd_err rc = ND_OK;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        say(why, why_sz, "Package name is too long.");
        return ND_ERR_TOOLONG;
    }
    f = fopen(resolved, "rb");
    if (f == NULL) {
        nd_log_err(ND_LOG_OS, "nap: cannot open %s: %s", path, strerror(errno));
        say(why, why_sz, "Cannot read the package.");
        return ND_ERR_IO;
    }
    /* The archive's real length, so that a header claiming more data than
     * the file holds is refused HERE, in the inspection, and not discovered
     * by the installer half way through writing. fseek() past the end of a
     * file succeeds silently, which is why the size cannot be left to it. */
    {
        struct stat st;

        if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
            (void)fclose(f);
            say(why, why_sz, "Cannot read the package.");
            return ND_ERR_IO;
        }
        file_len = (uint64_t)st.st_size;
    }

    for (;;) {
        uint8_t block[TAR_BLOCK];
        const tar_header *h = (const tar_header *)block;
        tar_entry e;
        char raw[256];
        uint64_t size;
        long header_at;
        size_t got;
        bool is_dir;

        header_at = ftell(f);
        got = fread(block, 1u, TAR_BLOCK, f);
        if (got == 0u)
            break; /* EOF with no end-of-archive marker: tar(1) accepts it too */
        if (got != TAR_BLOCK) {
            say(why, why_sz, "Package is cut short.");
            rc = ND_ERR_PARSE;
            break;
        }
        if (block_is_zero(block))
            break; /* the end-of-archive marker; the second block is not needed */

        if (n_entries >= ND_NAP_MAX_ENTRIES) {
            say(why, why_sz, "Package has too many files.");
            rc = ND_ERR_PARSE;
            break;
        }
        n_entries++;

        if (!checksum_ok(block)) {
            say(why, why_sz, n_entries == 1u ? "Not a .nap package." : "Package is damaged.");
            rc = ND_ERR_PARSE;
            break;
        }
        if (!parse_octal(h->size, sizeof h->size, &size)) {
            say(why, why_sz, "Package is damaged.");
            rc = ND_ERR_PARSE;
            break;
        }

        /* The name: prefix + "/" + name when the prefix field is in use,
         * which POSIX permits only under a ustar magic. Neither field is
         * necessarily NUL-terminated at its full width. */
        {
            size_t name_len = strnlen(h->name, sizeof h->name);
            size_t prefix_len = 0u;

            if (memcmp(h->magic, "ustar", 5u) == 0)
                prefix_len = strnlen(h->prefix, sizeof h->prefix);
            if (prefix_len > 0u) {
                memcpy(raw, h->prefix, prefix_len);
                raw[prefix_len] = '/';
                memcpy(raw + prefix_len + 1u, h->name, name_len);
                raw[prefix_len + 1u + name_len] = '\0';
            } else {
                memcpy(raw, h->name, name_len);
                raw[name_len] = '\0';
            }
        }

        switch (h->typeflag) {
        case '0':
        case '\0':
        case '7': /* "high performance" file: a regular file to everyone */
            is_dir = false;
            break;
        case '5':
            is_dir = true;
            break;
        default:
            /* Links, devices, fifos, pax and GNU extension headers. nd_nap.h
             * has the list and the reasons; the sentence is the one thing
             * they have in common from the phone's side. */
            say(why, why_sz, "Package contains something\nthat is not a file.");
            rc = ND_ERR_UNSUPPORTED;
            break;
        }
        if (rc != ND_OK)
            break;

        if (!is_dir && size > ND_NAP_MAX_FILE_BYTES) {
            say(why, why_sz, "A file in the package is\ntoo big.");
            rc = ND_ERR_PARSE;
            break;
        }
        if (is_dir)
            size = 0u; /* a directory entry carries no data, whatever it says */

        memset(&e, 0, sizeof e);
        if (!normalise_name(raw, e.name, sizeof e.name) || (e.name[0] == '\0' && !is_dir)) {
            say(why, why_sz, "Package contains an unsafe\nfile name.");
            rc = ND_ERR_INVAL;
            break;
        }
        /* "./" -- the archive's own root, which `tar cf x .` writes first.
         * There is nothing to make: the staging directory is that root. */
        if (e.name[0] == '\0')
            continue;
        if (!classify(e.name, is_dir, &e, why, why_sz)) {
            rc = ND_ERR_INVAL;
            break;
        }
        e.size = size;
        e.data_offset = header_at + (long)TAR_BLOCK;
        if (size > file_len || (uint64_t)e.data_offset > file_len - size) {
            say(why, why_sz, "Package is cut short.");
            rc = ND_ERR_PARSE;
            break;
        }

        rc = fn(f, &e, ctx, why, why_sz);
        if (rc != ND_OK)
            break;

        /* Past the data, rounded up to the block, wherever the callback
         * left the stream. A size that does not fit in a long is a package
         * bigger than any card; the cap above already refused it. */
        {
            uint64_t padded = (size + (TAR_BLOCK - 1u)) & ~(uint64_t)(TAR_BLOCK - 1u);

            if (padded > (uint64_t)LONG_MAX - (uint64_t)e.data_offset ||
                fseek(f, e.data_offset + (long)padded, SEEK_SET) != 0) {
                say(why, why_sz, "Package is cut short.");
                rc = ND_ERR_PARSE;
                break;
            }
        }
    }

    (void)fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ *
 * The manifest
 * ------------------------------------------------------------------ */

/* nd_ui.c's manifest_id(), which accepts "13" and 13 alike; an app that
 * would not show in the menu must not install either. */
static bool manifest_id(const nd_json_val *o, int32_t *out)
{
    const nd_json_val *v = nd_json_get(o, "id");
    int64_t n;
    const char *s;

    if (v == NULL) {
        *out = 999;
        return true;
    }
    if (nd_json_int(v, &n)) {
        if (n < 0 || n > INT32_MAX)
            return false;
        *out = (int32_t)n;
        return true;
    }
    if (nd_json_str(v, &s) && s != NULL) {
        char *end = NULL;
        long parsed;

        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (*s == '\0')
            return false;
        parsed = strtol(s, &end, 10);
        if (end == s || parsed < 0 || parsed > INT32_MAX)
            return false;
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
            end++;
        if (*end != '\0')
            return false;
        *out = (int32_t)parsed;
        return true;
    }
    return false;
}

typedef struct {
    nd_nap_info info;
    uint8_t *manifest; /* owned here; freed by inspect_free()          */
    size_t manifest_len;
    bool saw_manifest;
    bool saw_top_so;
    char icon[ND_PATH_MAX]; /* the manifest's icon name, once parsed        */
} inspect_ctx;

static void inspect_free(inspect_ctx *c)
{
    free(c->manifest);
    c->manifest = NULL;
}

static bool add_arch(nd_nap_info *info, const char *tag)
{
    size_t i;

    for (i = 0u; i < info->n_arches; i++) {
        if (strcmp(info->arches[i], tag) == 0)
            return true;
    }
    if (info->n_arches >= ND_NAP_ARCHES_MAX)
        return false;
    (void)nd_strlcpy(info->arches[info->n_arches], tag, ND_NAP_ARCH_MAX);
    info->n_arches++;
    return true;
}

static nd_err inspect_entry(FILE *f, const tar_entry *e, void *ctx, char *why, size_t why_sz)
{
    inspect_ctx *c = ctx;

    if (e->kind == ENT_DIR)
        return ND_OK;

    c->info.n_files++;
    c->info.bytes += e->size;

    switch (e->kind) {
    case ENT_MANIFEST:
        if (c->saw_manifest) {
            say(why, why_sz, "Package has two manifests.");
            return ND_ERR_INVAL;
        }
        if (e->size == 0u || e->size > ND_NAP_MANIFEST_MAX) {
            say(why, why_sz, "Package manifest is the\nwrong size.");
            return ND_ERR_INVAL;
        }
        c->manifest = malloc((size_t)e->size);
        if (c->manifest == NULL) {
            say(why, why_sz, "Out of memory.");
            return ND_ERR_NOMEM;
        }
        if (fread(c->manifest, 1u, (size_t)e->size, f) != (size_t)e->size) {
            say(why, why_sz, "Package is cut short.");
            return ND_ERR_PARSE;
        }
        c->manifest_len = (size_t)e->size;
        c->saw_manifest = true;
        return ND_OK;
    case ENT_APPSO_TOP:
        c->saw_top_so = true;
        return ND_OK;
    case ENT_APPSO_LIB:
        if (!add_arch(&c->info, e->arch)) {
            say(why, why_sz, "Package names too many phones.");
            return ND_ERR_INVAL;
        }
        return ND_OK;
    case ENT_FILE:
    case ENT_DIR:
    default:
        return ND_OK;
    }
}

/* The second half of the inspection, once the manifest bytes are in hand:
 * parse them, and reconcile "arch" with what the walk found. */
static nd_err inspect_manifest(inspect_ctx *c, char *why, size_t why_sz)
{
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    const nd_json_val *v;
    const char *name;
    const char *arch;
    nd_err rc = ND_OK;

    if (!c->saw_manifest) {
        say(why, why_sz, "Package has no manifest.json.");
        return ND_ERR_NOTFOUND;
    }
    if (nd_json_parse(c->manifest, c->manifest_len, &doc, NULL, 0u) != ND_OK) {
        say(why, why_sz, "Package manifest is not\nvalid JSON.");
        return ND_ERR_PARSE;
    }
    root = nd_json_root(doc);
    if (root == NULL || nd_json_type_of(root) != ND_JSON_OBJECT) {
        say(why, why_sz, "Package manifest is not\nan app manifest.");
        rc = ND_ERR_PARSE;
        goto done;
    }

    name = nd_json_get_str(root, "name", NULL);
    if (name == NULL || name[0] == '\0' || strlen(name) >= ND_APP_NAME_MAX) {
        say(why, why_sz, "Package manifest has no\nusable name.");
        rc = ND_ERR_INVAL;
        goto done;
    }
    (void)nd_strlcpy(c->info.name, name, sizeof c->info.name);
    if (!nd_nap_dir_from_name(name, c->info.dir, sizeof c->info.dir)) {
        say(why, why_sz, "Package manifest has no\nusable name.");
        rc = ND_ERR_INVAL;
        goto done;
    }
    if (!manifest_id(root, &c->info.id)) {
        say(why, why_sz, "Package manifest has a bad id.");
        rc = ND_ERR_INVAL;
        goto done;
    }
    (void)nd_strlcpy(c->icon, nd_json_get_str(root, "icon", "icon.png"), sizeof c->icon);

    /* The one-phone shape: app.so at the root is meaningless without a tag
     * saying which phone it is for, and a tag without the file is a lie. */
    v = nd_json_get(root, "arch");
    arch = nd_json_get_str(root, "arch", NULL);
    if (c->saw_top_so) {
        if (v == NULL || arch == NULL || !arch_tag_ok(arch)) {
            say(why, why_sz, "Package does not say which\nphone its app.so is for.");
            rc = ND_ERR_INVAL;
            goto done;
        }
        if (c->info.n_arches > 0u) {
            say(why, why_sz, "Package mixes both layouts.");
            rc = ND_ERR_INVAL;
            goto done;
        }
        (void)add_arch(&c->info, arch);
    } else if (v != NULL) {
        say(why, why_sz, "Package names a phone but\nhas no app.so at its root.");
        rc = ND_ERR_INVAL;
        goto done;
    }
    if (c->info.n_arches == 0u) {
        say(why, why_sz, "Package has no app.so.");
        rc = ND_ERR_INVAL;
        goto done;
    }

done:
    nd_json_free(doc);
    return rc;
}

/* A second, cheap walk that answers one question: is the icon the manifest
 * names in the package? Not fatal either way -- the menu draws a placeholder
 * for an app with no icon -- but the picker can say so. */
typedef struct {
    const char *icon;
    bool found;
} icon_ctx;

static nd_err icon_entry(FILE *f, const tar_entry *e, void *ctx, char *why, size_t why_sz)
{
    icon_ctx *c = ctx;

    ND_UNUSED(f);
    ND_UNUSED(why);
    ND_UNUSED(why_sz);
    if (e->kind == ENT_FILE && strcmp(e->name, c->icon) == 0)
        c->found = true;
    return ND_OK;
}

static nd_err inspect(const char *path, inspect_ctx *c, char *why, size_t why_sz)
{
    nd_err rc;

    memset(c, 0, sizeof *c);
    rc = walk(path, inspect_entry, c, why, why_sz);
    if (rc == ND_OK)
        rc = inspect_manifest(c, why, why_sz);
    if (rc == ND_OK) {
        icon_ctx ic;

        ic.icon = c->icon;
        ic.found = false;
        if (walk(path, icon_entry, &ic, NULL, 0u) == ND_OK)
            c->info.has_icon = ic.found;
    }
    return rc;
}

nd_err nd_nap_inspect(const char *path, nd_nap_info *out, char *why, size_t why_sz)
{
    inspect_ctx c;
    nd_err rc;

    if (path == NULL || out == NULL)
        return ND_ERR_INVAL;
    rc = inspect(path, &c, why, why_sz);
    if (rc == ND_OK)
        *out = c.info;
    else
        memset(out, 0, sizeof *out);
    inspect_free(&c);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Installing
 * ------------------------------------------------------------------ */

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

/* rm -rf on a RESOLVED path. FTW_PHYS so a symlink somebody planted inside
 * an app directory is removed as a link and never followed. */
static void rm_rf(const char *resolved)
{
    if (access(resolved, F_OK) != 0)
        return;
    (void)nftw(resolved, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* mkdir -p below a resolved staging root, 0755 every level. */
static bool mkdir_under(const char *root, const char *rel)
{
    char full[ND_PATH_MAX];
    size_t i;
    size_t base;

    if (nd_snprintf(full, sizeof full, "%s/%s", root, rel) != ND_OK)
        return false;
    base = strlen(root) + 1u;
    for (i = base; full[i] != '\0'; i++) {
        if (full[i] == '/') {
            full[i] = '\0';
            if (mkdir(full, 0755u) != 0 && errno != EEXIST)
                return false;
            full[i] = '/';
        }
    }
    if (mkdir(full, 0755u) != 0 && errno != EEXIST)
        return false;
    return true;
}

/* Streams `size` bytes from f to `rel` under root, 0644, refusing to
 * overwrite: the same name twice in one archive is two entries claiming one
 * file, and the second is not an update of the first. */
static nd_err write_under(FILE *f, const char *root, const char *rel, uint64_t size, char *why,
                          size_t why_sz)
{
    char full[ND_PATH_MAX];
    uint8_t buf[8192];
    const char *slash;
    FILE *out;
    int fd;

    if (nd_snprintf(full, sizeof full, "%s/%s", root, rel) != ND_OK) {
        say(why, why_sz, "A file name in the package\nis too long.");
        return ND_ERR_TOOLONG;
    }
    slash = strrchr(rel, '/');
    if (slash != NULL) {
        char dir[ND_PATH_MAX];

        (void)nd_strlcpy(dir, rel, (size_t)(slash - rel) + 1u);
        if (!mkdir_under(root, dir)) {
            say(why, why_sz, "Could not write to the card.");
            return ND_ERR_IO;
        }
    }

    fd = open(full, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            say(why, why_sz, "Package has the same file\ntwice.");
            return ND_ERR_INVAL;
        }
        nd_log_err(ND_LOG_OS, "nap: cannot create %s: %s", full, strerror(errno));
        say(why, why_sz, "Could not write to the card.");
        return ND_ERR_IO;
    }
    out = fdopen(fd, "wb");
    if (out == NULL) {
        (void)close(fd);
        say(why, why_sz, "Could not write to the card.");
        return ND_ERR_IO;
    }

    while (size > 0u) {
        size_t want = (size > sizeof buf) ? sizeof buf : (size_t)size;
        size_t got = fread(buf, 1u, want, f);

        if (got == 0u) {
            (void)fclose(out);
            say(why, why_sz, "Package is cut short.");
            return ND_ERR_PARSE;
        }
        if (fwrite(buf, 1u, got, out) != got) {
            (void)fclose(out);
            nd_log_err(ND_LOG_OS, "nap: short write to %s: %s", full, strerror(errno));
            say(why, why_sz, "Could not write to the card.\nIt may be full.");
            return ND_ERR_IO;
        }
        size -= got;
    }
    if (fclose(out) != 0) {
        say(why, why_sz, "Could not write to the card.\nIt may be full.");
        return ND_ERR_IO;
    }
    /* The mode is set explicitly rather than trusted to the umask, because
     * these are the modes apply_layout() will restate and the app has to be
     * able to read its own files the moment the install finishes. */
    (void)chmod(full, 0644u);
    return ND_OK;
}

typedef struct {
    const char *staging; /* resolved */
    const char *arch;
    bool wrote_so;
} install_ctx;

static nd_err install_entry(FILE *f, const tar_entry *e, void *ctx, char *why, size_t why_sz)
{
    install_ctx *c = ctx;

    switch (e->kind) {
    case ENT_DIR:
        /* lib/ is never installed, and neither are its subdirectories. */
        if (strcmp(e->name, "lib") == 0 || strncmp(e->name, "lib/", 4u) == 0)
            return ND_OK;
        if (!mkdir_under(c->staging, e->name)) {
            say(why, why_sz, "Could not write to the card.");
            return ND_ERR_IO;
        }
        return ND_OK;
    case ENT_MANIFEST:
        /* Written last, by install() itself, from the bytes the inspection
         * already validated. */
        return ND_OK;
    case ENT_APPSO_TOP:
        c->wrote_so = true;
        return write_under(f, c->staging, "app.so", e->size, why, why_sz);
    case ENT_APPSO_LIB:
        if (strcmp(e->arch, c->arch) != 0)
            return ND_OK; /* another phone's; skipped, not installed */
        c->wrote_so = true;
        return write_under(f, c->staging, "app.so", e->size, why, why_sz);
    case ENT_FILE:
    default:
        return write_under(f, c->staging, e->name, e->size, why, why_sz);
    }
}

bool nd_nap_is_installed(const char *apps_dir, const char *dir)
{
    char manifest[ND_PATH_MAX];

    if (apps_dir == NULL || dir == NULL || dir[0] == '\0')
        return false;
    if (nd_snprintf(manifest, sizeof manifest, "%s/%s/manifest.json", apps_dir, dir) != ND_OK)
        return false;
    return nd_path_is_file(manifest);
}

nd_err nd_nap_install(const char *path, const char *apps_dir, const char *arch, nd_nap_info *out,
                      char *why, size_t why_sz)
{
    inspect_ctx c;
    install_ctx ic;
    char apps_res[ND_PATH_MAX];
    char staging[ND_PATH_MAX];
    char final_dir[ND_PATH_MAX];
    char old_dir[ND_PATH_MAX];
    char manifest_rel[] = "manifest.json";
    bool have_old = false;
    bool moved_old = false;
    nd_err rc;

    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (path == NULL || apps_dir == NULL || arch == NULL || arch[0] == '\0')
        return ND_ERR_INVAL;

    rc = inspect(path, &c, why, why_sz);
    if (rc != ND_OK)
        goto done;

    if (!nd_nap_info_has_arch(&c.info, arch)) {
        say(why, why_sz, "This package is not for\nthis phone.");
        rc = ND_ERR_UNSUPPORTED;
        goto done;
    }

    if (nd_path_resolve(apps_res, sizeof apps_res, apps_dir) != ND_OK ||
        nd_snprintf(staging, sizeof staging, "%s/.%s.installing", apps_res, c.info.dir) != ND_OK ||
        nd_snprintf(final_dir, sizeof final_dir, "%s/%s", apps_res, c.info.dir) != ND_OK ||
        nd_snprintf(old_dir, sizeof old_dir, "%s/.%s.replaced", apps_res, c.info.dir) != ND_OK) {
        say(why, why_sz, "Package name is too long.");
        rc = ND_ERR_TOOLONG;
        goto done;
    }
    if (!nd_path_is_dir(apps_dir)) {
        say(why, why_sz, "The card has no apps folder.");
        rc = ND_ERR_NOTFOUND;
        goto done;
    }

    /* Whatever an earlier attempt left behind. The staging name is ours and
     * holds nothing an owner put there. The "replaced" name is different:
     * it is the owner's previous app, stepped aside, and if the step back
     * failed last time it is the only copy -- so it goes back where it was
     * when nothing has taken the name since, and is discarded only when
     * something has. */
    rm_rf(staging);
    if (access(old_dir, F_OK) == 0) {
        if (access(final_dir, F_OK) != 0 && rename(old_dir, final_dir) == 0)
            nd_log(ND_LOG_OS, "nap: restored %s from an interrupted replacement", final_dir);
        else
            rm_rf(old_dir);
    }

    if (mkdir(staging, 0755u) != 0) {
        nd_log_err(ND_LOG_OS, "nap: cannot create %s: %s", staging, strerror(errno));
        say(why, why_sz, "Could not write to the card.");
        rc = ND_ERR_IO;
        goto done;
    }
    (void)chmod(staging, 0755u);

    memset(&ic, 0, sizeof ic);
    ic.staging = staging;
    ic.arch = arch;
    rc = walk(path, install_entry, &ic, why, why_sz);
    if (rc != ND_OK)
        goto fail;
    if (!ic.wrote_so) {
        /* Cannot happen after the has_arch check above, but a package
         * without a program is the one thing that must never be installed,
         * so it is checked on the writing side too. */
        say(why, why_sz, "Package has no app.so.");
        rc = ND_ERR_INVAL;
        goto fail;
    }

    /* The manifest, last. From here the staged directory IS an app. */
    {
        char full[ND_PATH_MAX];
        FILE *mf;

        if (nd_snprintf(full, sizeof full, "%s/%s", staging, manifest_rel) != ND_OK) {
            rc = ND_ERR_TOOLONG;
            goto fail;
        }
        mf = fopen(full, "wb");
        if (mf == NULL || fwrite(c.manifest, 1u, c.manifest_len, mf) != c.manifest_len ||
            fclose(mf) != 0) {
            if (mf != NULL)
                (void)fclose(mf);
            say(why, why_sz, "Could not write to the card.\nIt may be full.");
            rc = ND_ERR_IO;
            goto fail;
        }
        (void)chmod(full, 0644u);
    }

    /* Replace. The old directory steps aside first so that a failure at any
     * point can step it back; its data/ moves into the new one by rename,
     * which on one filesystem is a metadata change however big it is. */
    have_old = access(final_dir, F_OK) == 0;
    if (have_old) {
        char old_data[ND_PATH_MAX];
        char new_data[ND_PATH_MAX];

        if (rename(final_dir, old_dir) != 0) {
            nd_log_err(ND_LOG_OS, "nap: cannot move %s aside: %s", final_dir, strerror(errno));
            say(why, why_sz, "Could not replace the old\nversion.");
            rc = ND_ERR_IO;
            goto fail;
        }
        moved_old = true;
        if (nd_snprintf(old_data, sizeof old_data, "%s/%s", old_dir, ND_PATH_APP_DATA_NAME) ==
                ND_OK &&
            nd_snprintf(new_data, sizeof new_data, "%s/%s", staging, ND_PATH_APP_DATA_NAME) ==
                ND_OK &&
            access(old_data, F_OK) == 0) {
            if (rename(old_data, new_data) != 0) {
                nd_log_err(ND_LOG_OS, "nap: cannot keep %s: %s", old_data, strerror(errno));
                say(why, why_sz, "Could not keep the app's\nsaved data.");
                rc = ND_ERR_IO;
                goto fail;
            }
        }
    }
    if (rename(staging, final_dir) != 0) {
        nd_log_err(ND_LOG_OS, "nap: cannot rename %s: %s", staging, strerror(errno));
        say(why, why_sz, "Could not write to the card.");
        rc = ND_ERR_IO;
        if (moved_old) {
            char old_data[ND_PATH_MAX];
            char new_data[ND_PATH_MAX];

            /* data/ went across; bring it home before the old app does. */
            if (nd_snprintf(old_data, sizeof old_data, "%s/%s", old_dir, ND_PATH_APP_DATA_NAME) ==
                    ND_OK &&
                nd_snprintf(new_data, sizeof new_data, "%s/%s", staging, ND_PATH_APP_DATA_NAME) ==
                    ND_OK &&
                access(new_data, F_OK) == 0)
                (void)rename(new_data, old_data);
        }
        goto fail;
    }
    if (moved_old)
        rm_rf(old_dir);

    nd_log(ND_LOG_OS, "nap: installed %s (id %d, %s) from %s%s", c.info.name, (int)c.info.id, arch,
           path, have_old ? ", replacing the earlier version" : "");
    if (out != NULL)
        *out = c.info;
    rc = ND_OK;
    goto done;

fail:
    if (moved_old)
        (void)rename(old_dir, final_dir);
    rm_rf(staging);
    nd_log_err(ND_LOG_OS, "nap: install of %s failed: %s", path, nd_strerror(rc));

done:
    inspect_free(&c);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Names and phones
 * ------------------------------------------------------------------ */

bool nd_nap_dir_from_name(const char *name, char *out, size_t out_sz)
{
    size_t n = 0u;
    size_t i;

    if (name == NULL || out == NULL || out_sz == 0u)
        return false;
    for (i = 0u; name[i] != '\0'; i++) {
        char c = name[i];

        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
            continue;
        if (n + 1u >= out_sz)
            return false;
        out[n++] = c;
    }
    out[n] = '\0';
    /* A leading '-' would make a directory some shell tools read as an
     * option; a name that is nothing but punctuation was refused above. */
    return n > 0u && out[0] != '-';
}

const char *nd_nap_display_name(const char *path, char *out, size_t out_sz)
{
    const char *slash;
    const char *base;
    size_t len;

    if (out == NULL || out_sz == 0u)
        return out;
    if (path == NULL) {
        out[0] = '\0';
        return out;
    }
    slash = strrchr(path, '/');
    base = (slash != NULL) ? slash + 1 : path;
    len = strlen(base);
    if (len > sizeof ND_NAP_SUFFIX - 1u &&
        strcasecmp(base + len - (sizeof ND_NAP_SUFFIX - 1u), ND_NAP_SUFFIX) == 0)
        len -= sizeof ND_NAP_SUFFIX - 1u;
    if (len + 1u > out_sz)
        len = out_sz - 1u;
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

bool nd_nap_info_has_arch(const nd_nap_info *info, const char *arch)
{
    size_t i;

    if (info == NULL || arch == NULL)
        return false;
    for (i = 0u; i < info->n_arches; i++) {
        if (strcmp(info->arches[i], arch) == 0)
            return true;
    }
    return false;
}

const char *nd_nap_arch_for_machine(const char *machine)
{
    if (machine == NULL)
        return "";
    /* armv7l is what the RV1103's kernel reports; "armv7" alone would be a
     * kernel this tree has never met, and is accepted for the day it does. */
    if (strcmp(machine, "armv7l") == 0 || strcmp(machine, "armv7") == 0)
        return ND_NAP_ARCH_LUCKFOX;
    if (strcmp(machine, "aarch64") == 0)
        return ND_NAP_ARCH_QEMU;
    if (strcmp(machine, "x86_64") == 0)
        return ND_NAP_ARCH_HOST;
    return "";
}

const char *nd_nap_phone_arch(void)
{
    static char cached[ND_NAP_ARCH_MAX];
    static bool known;

    if (!known) {
        struct utsname u;

        if (uname(&u) == 0)
            (void)nd_strlcpy(cached, nd_nap_arch_for_machine(u.machine), sizeof cached);
        else
            cached[0] = '\0';
        known = true;
    }
    return cached;
}

/* ------------------------------------------------------------------ *
 * The card
 * ------------------------------------------------------------------ */

static bool ends_with_nap(const char *name)
{
    size_t n = strlen(name);

    return n > sizeof ND_NAP_SUFFIX - 1u &&
           strcasecmp(name + n - (sizeof ND_NAP_SUFFIX - 1u), ND_NAP_SUFFIX) == 0;
}

static int nap_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    const char *bx = strrchr(x, '/');
    const char *by = strrchr(y, '/');
    int c;

    bx = (bx != NULL) ? bx + 1 : x;
    by = (by != NULL) ? by + 1 : y;
    c = strcasecmp(bx, by);
    if (c != 0)
        return c;
    return strcmp(x, y);
}

static size_t find_in(const char *dir, char ***names, size_t *n, size_t *cap)
{
    char resolved[ND_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    size_t added = 0u;

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return 0u;
    d = opendir(resolved);
    if (d == NULL)
        return 0u;
    while ((ent = readdir(d)) != NULL) {
        char full[ND_STORAGE_PATH_MAX];
        char *copy;

        if (*n >= ND_NAP_MAX_FOUND)
            break;
        if (!ends_with_nap(ent->d_name))
            continue;
        if (nd_snprintf(full, sizeof full, "%s/%s", dir, ent->d_name) != ND_OK)
            continue;
        if (!nd_path_is_file(full))
            continue;
        if (*n == *cap) {
            size_t grow = (*cap == 0u) ? 16u : *cap * 2u;
            /* freed by nd_nap_find() */
            char **bigger = realloc(*names, grow * sizeof *bigger);

            if (bigger == NULL)
                break;
            *names = bigger;
            *cap = grow;
        }
        copy = strdup(full);
        if (copy == NULL)
            break;
        (*names)[(*n)++] = copy;
        added++;
    }
    (void)closedir(d);
    return added;
}

size_t nd_nap_find(char out[][ND_STORAGE_PATH_MAX], size_t max)
{
    static const char *const SUBDIRS[] = {"apps", ND_SD_UNTRUSTED_NAME};
    char mount[ND_PATH_MAX];
    char **names = NULL;
    size_t n = 0u;
    size_t cap = 0u;
    size_t written = 0u;
    size_t i;

    if (out == NULL || max == 0u)
        return 0u;
    /* nd_storage_folder() is the mount point plus a name, gated on the card
     * being ready -- so asking it for "." is asking for the root of a ready
     * card and nothing at all otherwise. */
    if (!nd_storage_is_ready() || !nd_storage_folder(".", mount, sizeof mount))
        return 0u;
    mount[strlen(mount) - 2u] = '\0'; /* drop the "/." */

    (void)find_in(mount, &names, &n, &cap);
    for (i = 0u; i < ND_ARRAY_LEN(SUBDIRS); i++) {
        char dir[ND_PATH_MAX];

        if (nd_snprintf(dir, sizeof dir, "%s/%s", mount, SUBDIRS[i]) == ND_OK)
            (void)find_in(dir, &names, &n, &cap);
    }

    if (n > 0u)
        qsort(names, n, sizeof *names, nap_cmp);
    for (i = 0u; i < n; i++) {
        if (written < max &&
            nd_strlcpy(out[written], names[i], ND_STORAGE_PATH_MAX) < ND_STORAGE_PATH_MAX)
            written++;
        free(names[i]);
    }
    free(names);
    return written;
}
