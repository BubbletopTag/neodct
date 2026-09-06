/* test_nap.c -- .nap packages: what is accepted, what is refused, and what an
 * install leaves on the card.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The two shapes nd_nap.h describes both inspect correctly: one phone
 *     (app.so at the root, "arch" in the manifest) and any phone
 *     (lib/<tag>/app.so), and the manifest's name, id and icon come through.
 *
 *  2. EVERYTHING ON THE REFUSAL LIST IS REFUSED, with nothing written: a
 *     missing manifest, an app.so with no tag, a tag with no app.so, both
 *     layouts at once, an entry under data/, a misuse of lib/, a symlink, a
 *     ".." name, an absolute name, a pax header, a file that claims more
 *     bytes than the archive holds, a checksum that does not match, and a
 *     file that is not a tar at all.
 *
 *  3. An install lands the right app.so for the phone and nothing from
 *     lib/, with 0644 files and 0755 directories, manifest included -- and a
 *     package for another phone is refused before a directory exists.
 *
 *  4. Replacing an installed app keeps its data/ and drops its old files;
 *     an install that fails half way through a replacement puts the old app
 *     back, data included.
 *
 *  5. nd_nap_find() looks in the three places an owner would put a package,
 *     only on a ready card, and sorts what it finds.
 *
 * The archives are built here, by a writer of a dozen lines, because a test
 * that reads its fixtures from files nobody can see is a test nobody can
 * read. Every path is virtual: ND_ROOT is a scratch directory per case.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_nap.h"
#include "nd_storage.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * A ustar writer
 * ------------------------------------------------------------------ */

#define TW_MAX (256u * 1024u)

typedef struct {
    uint8_t *buf; /* owned; tw_free() */
    size_t len;
} tw;

static void tw_init(tw *t)
{
    t->buf = calloc(1u, TW_MAX);
    t->len = 0u;
    if (t->buf == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
}

static void tw_free(tw *t)
{
    free(t->buf);
    t->buf = NULL;
}

static void tw_header(uint8_t *h, const char *name, const char *prefix, char type, size_t size,
                      unsigned mode)
{
    unsigned sum = 0u;
    size_t i;

    memset(h, 0, 512u);
    memcpy(h, name, strlen(name));
    (void)snprintf((char *)h + 100, 8u, "%07o", mode);
    (void)snprintf((char *)h + 108, 8u, "%07o", 0u);
    (void)snprintf((char *)h + 116, 8u, "%07o", 0u);
    (void)snprintf((char *)h + 124, 12u, "%011o", (unsigned)size);
    (void)snprintf((char *)h + 136, 12u, "%011o", 0u);
    memset(h + 148, ' ', 8u);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar", 6u);
    memcpy(h + 263, "00", 2u);
    if (prefix != NULL)
        memcpy(h + 345, prefix, strlen(prefix));
    for (i = 0u; i < 512u; i++)
        sum += h[i];
    (void)snprintf((char *)h + 148, 8u, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
}

/* An entry with `type`, `data` bytes, and -- optionally -- a prefix field,
 * which is how ustar spells a long path and which the reader must join. */
static void tw_add_full(tw *t, const char *name, const char *prefix, char type, const void *data,
                        size_t size, unsigned mode)
{
    size_t padded = (size + 511u) & ~(size_t)511u;

    if (t->len + 512u + padded > TW_MAX) {
        fprintf(stderr, "test archive too big\n");
        exit(1);
    }
    tw_header(t->buf + t->len, name, prefix, type, size, mode);
    t->len += 512u;
    if (size > 0u)
        memcpy(t->buf + t->len, data, size);
    t->len += padded;
}

static void tw_file(tw *t, const char *name, const char *text)
{
    tw_add_full(t, name, NULL, '0', text, strlen(text), 0644u);
}

static void tw_dir(tw *t, const char *name)
{
    tw_add_full(t, name, NULL, '5', NULL, 0u, 0755u);
}

/* Two zero blocks, then the archive goes to a virtual path. */
static void tw_write(tw *t, const char *path)
{
    if (t->len + 1024u > TW_MAX) {
        fprintf(stderr, "test archive too big\n");
        exit(1);
    }
    memset(t->buf + t->len, 0, 1024u);
    pt_write(path, t->buf, t->len + 1024u);
}

/* The manifest most cases share. */
#define MANIFEST_LUCKFOX \
    "{\"name\": \"Demo App\", \"id\": \"13\", \"icon\": \"icon.png\", \"arch\": \"luckfox-armv7\"}"
#define MANIFEST_PLAIN "{\"name\": \"Demo App\", \"id\": 13, \"icon\": \"icon.png\"}"

/* A complete one-phone package: manifest, icon, code, one data file. */
static void write_single(const char *path)
{
    tw t;

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "icon.png", "PNG");
    tw_file(&t, "app.so", "SO-LUCKFOX");
    tw_file(&t, "web.ndb", "the text");
    tw_write(&t, path);
    tw_free(&t);
}

/* A universal package, with a subdirectory of the app's own. */
static void write_universal(const char *path)
{
    tw t;

    tw_init(&t);
    tw_dir(&t, "./");
    tw_file(&t, "./manifest.json", MANIFEST_PLAIN);
    tw_file(&t, "./icon.png", "PNG");
    tw_dir(&t, "./lib/");
    tw_dir(&t, "./lib/luckfox-armv7/");
    tw_file(&t, "./lib/luckfox-armv7/app.so", "SO-LUCKFOX");
    tw_dir(&t, "./lib/qemu-aarch64/");
    tw_file(&t, "./lib/qemu-aarch64/app.so", "SO-QEMU");
    tw_dir(&t, "./art/");
    tw_file(&t, "./art/big.png", "PNG2");
    tw_write(&t, path);
    tw_free(&t);
}

static size_t read_file(const char *path, char *out, size_t out_sz)
{
    return pt_read_text(path, out, out_sz);
}

static unsigned mode_of(const char *path)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK || stat(resolved, &st) != 0)
        return 0u;
    return (unsigned)(st.st_mode & 07777u);
}

/* ------------------------------------------------------------------ *
 * 0. The pure functions
 * ------------------------------------------------------------------ */

static void test_machine_to_arch(void)
{
    CHECK_STR(nd_nap_arch_for_machine("armv7l"), ND_NAP_ARCH_LUCKFOX);
    CHECK_STR(nd_nap_arch_for_machine("aarch64"), ND_NAP_ARCH_QEMU);
    CHECK_STR(nd_nap_arch_for_machine("x86_64"), ND_NAP_ARCH_HOST);
    CHECK_STR(nd_nap_arch_for_machine("mips"), "");
    CHECK_STR(nd_nap_arch_for_machine(NULL), "");
    /* Whatever this host is, the answer is one of the three or nothing --
     * never a string a package could not have named. */
    {
        const char *mine = nd_nap_phone_arch();

        CHECK(mine != NULL);
        CHECK(strcmp(mine, "") == 0 || strcmp(mine, ND_NAP_ARCH_LUCKFOX) == 0 ||
              strcmp(mine, ND_NAP_ARCH_QEMU) == 0 || strcmp(mine, ND_NAP_ARCH_HOST) == 0);
    }
}

static void test_dir_from_name(void)
{
    char out[ND_NAP_DIR_MAX];

    CHECK(nd_nap_dir_from_name("Bible", out, sizeof out));
    CHECK_STR(out, "Bible");
    CHECK(nd_nap_dir_from_name("Phone Book", out, sizeof out));
    CHECK_STR(out, "PhoneBook");
    CHECK(nd_nap_dir_from_name("  T9 (beta) v2.1!  ", out, sizeof out));
    CHECK_STR(out, "T9betav21");
    CHECK(nd_nap_dir_from_name("my-app_2", out, sizeof out));
    CHECK_STR(out, "my-app_2");
    /* Nothing usable, or a leading dash, or too long. */
    CHECK(!nd_nap_dir_from_name("...", out, sizeof out));
    CHECK(!nd_nap_dir_from_name("", out, sizeof out));
    CHECK(!nd_nap_dir_from_name("-rf", out, sizeof out));
    CHECK(!nd_nap_dir_from_name("/etc/../shadow", out, 4u));
    CHECK(!nd_nap_dir_from_name(NULL, out, sizeof out));
}

static void test_display_name(void)
{
    char out[64];

    CHECK_STR(nd_nap_display_name("/NeoDCT/User/sdcard/Bible.nap", out, sizeof out), "Bible");
    CHECK_STR(nd_nap_display_name("GAME.NAP", out, sizeof out), "GAME");
    CHECK_STR(nd_nap_display_name("noext", out, sizeof out), "noext");
    CHECK_STR(nd_nap_display_name(".nap", out, sizeof out), ".nap");
    CHECK_STR(nd_nap_display_name(NULL, out, sizeof out), "");
}

/* ------------------------------------------------------------------ *
 * 1. Inspection
 * ------------------------------------------------------------------ */

static void test_inspect_single(void)
{
    nd_nap_info info;
    char why[ND_NAP_WHY_MAX];

    write_single("/card/Demo.nap");
    why[0] = '\0';
    CHECK_INT(nd_nap_inspect("/card/Demo.nap", &info, why, sizeof why), ND_OK);
    CHECK_STR(why, "");
    CHECK_STR(info.name, "Demo App");
    CHECK_STR(info.dir, "DemoApp");
    CHECK_INT(info.id, 13);
    CHECK_INT(info.n_arches, 1);
    CHECK_STR(info.arches[0], ND_NAP_ARCH_LUCKFOX);
    CHECK(nd_nap_info_has_arch(&info, ND_NAP_ARCH_LUCKFOX));
    CHECK(!nd_nap_info_has_arch(&info, ND_NAP_ARCH_QEMU));
    CHECK(info.has_icon);
    CHECK_INT(info.n_files, 4);
    CHECK_INT(info.bytes, strlen(MANIFEST_LUCKFOX) + 3u + 10u + 8u);
}

static void test_inspect_universal(void)
{
    nd_nap_info info;

    write_universal("/card/Uni.nap");
    CHECK_INT(nd_nap_inspect("/card/Uni.nap", &info, NULL, 0u), ND_OK);
    CHECK_STR(info.dir, "DemoApp");
    CHECK_INT(info.id, 13);
    CHECK_INT(info.n_arches, 2);
    CHECK(nd_nap_info_has_arch(&info, ND_NAP_ARCH_LUCKFOX));
    CHECK(nd_nap_info_has_arch(&info, ND_NAP_ARCH_QEMU));
    CHECK(!nd_nap_info_has_arch(&info, ND_NAP_ARCH_HOST));
    CHECK(info.has_icon);
    CHECK_INT(info.n_files, 5);
}

/* The prefix field: a ustar writer splits a long path across prefix and
 * name, and a reader that ignores the prefix installs "app.so" where
 * "lib/qemu-aarch64/app.so" was meant. */
static void test_inspect_joins_the_prefix(void)
{
    nd_nap_info info;
    tw t;

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_PLAIN);
    tw_add_full(&t, "app.so", "lib/qemu-aarch64", '0', "SO-QEMU", 7u, 0644u);
    tw_write(&t, "/card/P.nap");
    tw_free(&t);

    CHECK_INT(nd_nap_inspect("/card/P.nap", &info, NULL, 0u), ND_OK);
    CHECK_INT(info.n_arches, 1);
    CHECK_STR(info.arches[0], ND_NAP_ARCH_QEMU);
    CHECK(!info.has_icon);
}

/* One refusal, checked the same way every time: an error, a reason, and
 * nothing on the card. */
static void expect_refused(const char *path, const char *fragment)
{
    nd_nap_info info;
    char why[ND_NAP_WHY_MAX];
    nd_err rc;

    memset(&info, 0x55, sizeof info);
    why[0] = '\0';
    rc = nd_nap_inspect(path, &info, why, sizeof why);
    CHECK(rc != ND_OK);
    if (rc == ND_OK)
        fprintf(stderr, "  %s was accepted (expected \"%s\")\n", path, fragment);
    CHECK(strstr(why, fragment) != NULL);
    if (strstr(why, fragment) == NULL)
        fprintf(stderr, "  %s: why=\"%s\" (expected \"%s\")\n", path, why, fragment);
    CHECK_INT(info.n_arches, 0);

    /* And the installer refuses the same package the same way, creating
     * nothing. */
    pt_mkdir("/card/apps");
    why[0] = '\0';
    CHECK(nd_nap_install(path, "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why, sizeof why) != ND_OK);
    CHECK(strstr(why, fragment) != NULL);
    CHECK(!nd_path_exists("/card/apps/DemoApp"));
    CHECK(!nd_path_exists("/card/apps/.DemoApp.installing"));
}

/* version, author and description are optional; present, they reach the info
 * the install screen draws from, and absent they are empty rather than junk.
 * The icon's own name is carried too, so nd_nap_extract_icon() knows what to
 * pull. */
static void test_inspect_reads_optional_metadata(void)
{
    nd_nap_info info;
    tw t;

    tw_init(&t);
    tw_file(&t, "manifest.json",
            "{\"name\":\"Demo App\",\"id\":13,\"icon\":\"icon.png\","
            "\"arch\":\"luckfox-armv7\",\"version\":\"2.3\","
            "\"author\":\"Someone\",\"description\":\"Does a thing.\"}");
    tw_file(&t, "icon.png", "PNG");
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/Meta.nap");
    tw_free(&t);
    CHECK_INT(nd_nap_inspect("/card/Meta.nap", &info, NULL, 0u), ND_OK);
    CHECK_STR(info.version, "2.3");
    CHECK_STR(info.author, "Someone");
    CHECK_STR(info.description, "Does a thing.");
    CHECK_STR(info.icon, "icon.png");

    /* The package the other tests use carries none of the three. */
    write_single("/card/Bare.nap");
    CHECK_INT(nd_nap_inspect("/card/Bare.nap", &info, NULL, 0u), ND_OK);
    CHECK_STR(info.version, "");
    CHECK_STR(info.author, "");
    CHECK_STR(info.description, "");
    CHECK_STR(info.icon, "icon.png");
}

/* nd_nap_extract_icon() copies just the icon out, so the install screen can
 * show it before anything is unpacked. It writes a real path (no ND_ROOT
 * redirect, since the caller hands it a scratch file), so the test resolves
 * one; a package whose manifest names an icon it does not contain reports so
 * rather than writing a truncated file. */
static void test_extract_icon(void)
{
    static const char ICONBYTES[] = "PNGDATA-and-more-1234567890";
    char dest[ND_PATH_MAX];
    char got[64];
    tw t;

    tw_init(&t);
    tw_file(&t, "manifest.json",
            "{\"name\":\"Demo App\",\"id\":13,\"icon\":\"icon.png\",\"arch\":\"luckfox-armv7\"}");
    tw_file(&t, "icon.png", ICONBYTES);
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/Ico.nap");
    tw_free(&t);

    CHECK_INT(nd_path_resolve(dest, sizeof dest, "/card/out.png"), ND_OK);
    CHECK_INT(nd_nap_extract_icon("/card/Ico.nap", dest), ND_OK);
    CHECK_INT((int)read_file("/card/out.png", got, sizeof got), (int)strlen(ICONBYTES));
    CHECK_STR(got, ICONBYTES);

    /* A manifest naming an icon the package does not hold: no file written. */
    tw_init(&t);
    tw_file(&t, "manifest.json",
            "{\"name\":\"Demo App\",\"id\":13,\"icon\":\"icon.png\",\"arch\":\"luckfox-armv7\"}");
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/NoIco.nap");
    tw_free(&t);
    CHECK_INT(nd_path_resolve(dest, sizeof dest, "/card/none.png"), ND_OK);
    CHECK(nd_nap_extract_icon("/card/NoIco.nap", dest) != ND_OK);
    CHECK(!nd_path_exists("/card/none.png"));
}

static void test_refusals(void)
{
    tw t;
    static const char JUNK[] = "This is not a tar file, it is a text file that is long enough "
                               "to be mistaken for one if nobody checked the checksum. Which "
                               "somebody does, so it is not, and this padding takes it past "
                               "one block so that a short read is not the reason either. "
                               "..........................................................."
                               "..........................................................."
                               "..........................................................."
                               "..........................................................."
                               "..........................................................."
                               "..........................................................."
                               "...........................................................";

    /* Not a tar at all. */
    pt_write_text("/card/junk.nap", JUNK);
    expect_refused("/card/junk.nap", "Not a .nap package");

    /* A header short of a block. */
    pt_write_text("/card/short.nap", "ustar");
    expect_refused("/card/short.nap", "cut short");

    /* No manifest. */
    tw_init(&t);
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/nomanifest.nap");
    tw_free(&t);
    expect_refused("/card/nomanifest.nap", "no manifest");

    /* A manifest with no name. */
    tw_init(&t);
    tw_file(&t, "manifest.json", "{\"id\": 13, \"arch\": \"luckfox-armv7\"}");
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/noname.nap");
    tw_free(&t);
    expect_refused("/card/noname.nap", "usable name");

    /* A manifest that is not JSON. */
    tw_init(&t);
    tw_file(&t, "manifest.json", "{name: Demo");
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/badjson.nap");
    tw_free(&t);
    expect_refused("/card/badjson.nap", "valid JSON");

    /* app.so at the root and no tag: which phone? */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_PLAIN);
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/notag.nap");
    tw_free(&t);
    expect_refused("/card/notag.nap", "which");

    /* A tag and no app.so at the root. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "lib/luckfox-armv7/app.so", "SO");
    tw_write(&t, "/card/tagnoso.nap");
    tw_free(&t);
    expect_refused("/card/tagnoso.nap", "no app.so at its root");

    /* Both layouts at once. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_file(&t, "lib/qemu-aarch64/app.so", "SO");
    tw_write(&t, "/card/mixed.nap");
    tw_free(&t);
    expect_refused("/card/mixed.nap", "both layouts");

    /* No app.so anywhere. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_PLAIN);
    tw_file(&t, "icon.png", "PNG");
    tw_write(&t, "/card/noso.nap");
    tw_free(&t);
    expect_refused("/card/noso.nap", "no app.so");

    /* The app's data directory is the core's to make. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_file(&t, "data/state.prop", "x=1");
    tw_write(&t, "/card/data.nap");
    tw_free(&t);
    expect_refused("/card/data.nap", "data folder");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_dir(&t, "data/");
    tw_write(&t, "/card/datadir.nap");
    tw_free(&t);
    expect_refused("/card/datadir.nap", "data folder");

    /* lib/ holds app.so per phone and nothing else. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_file(&t, "lib/helper.so", "SO");
    tw_write(&t, "/card/libmisuse.nap");
    tw_free(&t);
    expect_refused("/card/libmisuse.nap", "lib folder");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_PLAIN);
    tw_file(&t, "lib/luckfox-armv7/extra.so", "SO");
    tw_write(&t, "/card/libextra.nap");
    tw_free(&t);
    expect_refused("/card/libextra.nap", "lib folder");

    /* A symlink, a hard link, a pax header. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_add_full(&t, "icon.png", NULL, '2', NULL, 0u, 0777u);
    tw_write(&t, "/card/symlink.nap");
    tw_free(&t);
    expect_refused("/card/symlink.nap", "not a file");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_add_full(&t, "app.so", NULL, '1', NULL, 0u, 0644u);
    tw_write(&t, "/card/hardlink.nap");
    tw_free(&t);
    expect_refused("/card/hardlink.nap", "not a file");

    tw_init(&t);
    tw_add_full(&t, "./PaxHeaders/x", NULL, 'x', "30 path=manifest.json\n", 22u, 0644u);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/pax.nap");
    tw_free(&t);
    expect_refused("/card/pax.nap", "not a file");

    /* Names that climb out, or start from the root. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_file(&t, "../Settings/app.so", "SO");
    tw_write(&t, "/card/dotdot.nap");
    tw_free(&t);
    expect_refused("/card/dotdot.nap", "unsafe");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "art/../../x", "SO");
    tw_write(&t, "/card/dotdot2.nap");
    tw_free(&t);
    expect_refused("/card/dotdot2.nap", "unsafe");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "/etc/passwd", "root::0:0");
    tw_write(&t, "/card/absolute.nap");
    tw_free(&t);
    expect_refused("/card/absolute.nap", "unsafe");

    /* A header claiming more than the archive holds. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_add_full(&t, "app.so", NULL, '0', "SO", 2u, 0644u);
    /* Rewrite that header's size to a megabyte and fix its checksum. */
    tw_header(t.buf + 1024u, "app.so", NULL, '0', 1024u * 1024u, 0644u);
    tw_write(&t, "/card/truncated.nap");
    tw_free(&t);
    expect_refused("/card/truncated.nap", "cut short");

    /* A header whose checksum is wrong: one flipped byte in the name. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    t.buf[1024u + 3u] ^= 0x01u;
    tw_write(&t, "/card/corrupt.nap");
    tw_free(&t);
    expect_refused("/card/corrupt.nap", "damaged");

    /* Two manifests. */
    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO");
    tw_file(&t, "manifest.json", MANIFEST_PLAIN);
    tw_write(&t, "/card/twomanifests.nap");
    tw_free(&t);
    expect_refused("/card/twomanifests.nap", "two manifests");

    /* A name that reduces to nothing on the card. */
    tw_init(&t);
    tw_file(&t, "manifest.json", "{\"name\": \"!!!\", \"arch\": \"luckfox-armv7\"}");
    tw_file(&t, "app.so", "SO");
    tw_write(&t, "/card/punct.nap");
    tw_free(&t);
    expect_refused("/card/punct.nap", "usable name");

    /* A file that is not there. */
    CHECK(nd_nap_inspect("/card/missing.nap", &(nd_nap_info){0}, NULL, 0u) == ND_ERR_IO);
}

/* ------------------------------------------------------------------ *
 * 3. Installing
 * ------------------------------------------------------------------ */

static void test_install_single(void)
{
    nd_nap_info info;
    char why[ND_NAP_WHY_MAX];
    char text[128];

    write_single("/card/Demo.nap");
    pt_mkdir("/card/apps");
    CHECK(!nd_nap_is_installed("/card/apps", "DemoApp"));

    why[0] = '\0';
    CHECK_INT(
        nd_nap_install("/card/Demo.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, &info, why, sizeof why),
        ND_OK);
    CHECK_STR(why, "");
    CHECK_STR(info.name, "Demo App");
    CHECK(nd_nap_is_installed("/card/apps", "DemoApp"));

    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 10u);
    CHECK_STR(text, "SO-LUCKFOX");
    CHECK(read_file("/card/apps/DemoApp/web.ndb", text, sizeof text) == 8u);
    CHECK(read_file("/card/apps/DemoApp/icon.png", text, sizeof text) == 3u);
    CHECK(read_file("/card/apps/DemoApp/manifest.json", text, sizeof text) ==
          strlen(MANIFEST_LUCKFOX));
    CHECK_STR(text, MANIFEST_LUCKFOX);

    /* The modes apply_layout() will restate, already in place. */
    CHECK_INT(mode_of("/card/apps/DemoApp"), 0755u);
    CHECK_INT(mode_of("/card/apps/DemoApp/app.so"), 0644u);
    CHECK_INT(mode_of("/card/apps/DemoApp/manifest.json"), 0644u);

    /* Nothing left over, and no data/ -- that is the core's. */
    CHECK(!nd_path_exists("/card/apps/.DemoApp.installing"));
    CHECK(!nd_path_exists("/card/apps/.DemoApp.replaced"));
    CHECK(!nd_path_exists("/card/apps/DemoApp/data"));

    /* Another phone's package is refused, and the phone is named. */
    why[0] = '\0';
    CHECK_INT(
        nd_nap_install("/card/Demo.nap", "/card/apps", ND_NAP_ARCH_QEMU, &info, why, sizeof why),
        ND_ERR_UNSUPPORTED);
    CHECK(strstr(why, "not for") != NULL);
    /* ...and the install that was there is untouched by the refusal. */
    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 10u);

    /* No apps directory means no card: nothing is conjured up. */
    CHECK_INT(nd_nap_install("/card/Demo.nap", "/card/nowhere", ND_NAP_ARCH_LUCKFOX, NULL, why,
                             sizeof why),
              ND_ERR_NOTFOUND);
    CHECK(!nd_path_exists("/card/nowhere"));
}

static void test_install_universal_picks_this_phone(void)
{
    char text[128];
    char why[ND_NAP_WHY_MAX];

    write_universal("/card/Uni.nap");
    pt_mkdir("/card/apps");

    CHECK_INT(
        nd_nap_install("/card/Uni.nap", "/card/apps", ND_NAP_ARCH_QEMU, NULL, why, sizeof why),
        ND_OK);
    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 7u);
    CHECK_STR(text, "SO-QEMU");
    /* lib/ is the package's, not the app's. */
    CHECK(!nd_path_exists("/card/apps/DemoApp/lib"));
    /* The app's own subdirectory came across, with its mode. */
    CHECK(read_file("/card/apps/DemoApp/art/big.png", text, sizeof text) == 4u);
    CHECK_INT(mode_of("/card/apps/DemoApp/art"), 0755u);

    /* The same package, for the other phone, replaces it. */
    CHECK_INT(
        nd_nap_install("/card/Uni.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why, sizeof why),
        ND_OK);
    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 10u);
    CHECK_STR(text, "SO-LUCKFOX");

    /* And the host tag, which this package does not carry. */
    CHECK_INT(
        nd_nap_install("/card/Uni.nap", "/card/apps", ND_NAP_ARCH_HOST, NULL, why, sizeof why),
        ND_ERR_UNSUPPORTED);
}

/* ------------------------------------------------------------------ *
 * 4. Replacing
 * ------------------------------------------------------------------ */

static void test_replace_keeps_data(void)
{
    char text[128];
    char why[ND_NAP_WHY_MAX];
    tw t;

    /* Version 1, run once: it has saved something. Plus a file version 2
     * does not carry, which must not survive -- an upgrade is the new
     * package, not the union. */
    write_single("/card/Demo.nap");
    pt_mkdir("/card/apps");
    CHECK_INT(
        nd_nap_install("/card/Demo.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why, sizeof why),
        ND_OK);
    pt_write_text("/card/apps/DemoApp/data/state.prop", "chapter=3\n");
    pt_write_text("/card/apps/DemoApp/data/notes/one.txt", "kept\n");
    pt_write_text("/card/apps/DemoApp/old.txt", "gone\n");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO-LUCKFOX-2");
    tw_file(&t, "web.ndb", "the text, revised");
    tw_write(&t, "/card/Demo2.nap");
    tw_free(&t);

    CHECK(nd_nap_is_installed("/card/apps", "DemoApp"));
    CHECK_INT(
        nd_nap_install("/card/Demo2.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why, sizeof why),
        ND_OK);

    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 12u);
    CHECK_STR(text, "SO-LUCKFOX-2");
    CHECK(read_file("/card/apps/DemoApp/web.ndb", text, sizeof text) == 17u);
    CHECK(read_file("/card/apps/DemoApp/data/state.prop", text, sizeof text) == 10u);
    CHECK_STR(text, "chapter=3\n");
    CHECK(read_file("/card/apps/DemoApp/data/notes/one.txt", text, sizeof text) == 5u);
    CHECK(!nd_path_exists("/card/apps/DemoApp/old.txt"));
    CHECK(!nd_path_exists("/card/apps/DemoApp/icon.png"));
    CHECK(!nd_path_exists("/card/apps/.DemoApp.replaced"));
    CHECK(!nd_path_exists("/card/apps/.DemoApp.installing"));
}

/* A package whose flaw only shows while WRITING -- the same file twice, which
 * the inspection does not see and O_EXCL does -- fails half way through a
 * replacement, and the old app comes back with its data. */
static void test_a_failed_replacement_puts_the_old_app_back(void)
{
    char text[128];
    char why[ND_NAP_WHY_MAX];
    tw t;

    write_single("/card/Demo.nap");
    pt_mkdir("/card/apps");
    CHECK_INT(
        nd_nap_install("/card/Demo.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why, sizeof why),
        ND_OK);
    pt_write_text("/card/apps/DemoApp/data/state.prop", "chapter=3\n");

    tw_init(&t);
    tw_file(&t, "manifest.json", MANIFEST_LUCKFOX);
    tw_file(&t, "app.so", "SO-LUCKFOX-2");
    tw_file(&t, "web.ndb", "one");
    tw_file(&t, "web.ndb", "two");
    tw_write(&t, "/card/Dup.nap");
    tw_free(&t);

    why[0] = '\0';
    CHECK(nd_nap_install("/card/Dup.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why,
                         sizeof why) != ND_OK);
    CHECK(strstr(why, "twice") != NULL);

    CHECK(nd_nap_is_installed("/card/apps", "DemoApp"));
    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 10u);
    CHECK_STR(text, "SO-LUCKFOX");
    CHECK(read_file("/card/apps/DemoApp/data/state.prop", text, sizeof text) == 10u);
    CHECK(!nd_path_exists("/card/apps/.DemoApp.installing"));
    CHECK(!nd_path_exists("/card/apps/.DemoApp.replaced"));
}

/* A directory with no manifest is a dead install and not an app: it is not
 * "installed", and the next install of that name simply takes its place. */
static void test_a_dead_install_is_replaced_quietly(void)
{
    char text[128];
    char why[ND_NAP_WHY_MAX];

    pt_write_text("/card/apps/DemoApp/app.so", "half");
    pt_write_text("/card/apps/.DemoApp.installing/app.so", "half");
    CHECK(!nd_nap_is_installed("/card/apps", "DemoApp"));

    write_single("/card/Demo.nap");
    CHECK_INT(
        nd_nap_install("/card/Demo.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, why, sizeof why),
        ND_OK);
    CHECK(read_file("/card/apps/DemoApp/app.so", text, sizeof text) == 10u);
    CHECK(!nd_path_exists("/card/apps/.DemoApp.installing"));
}

/* ------------------------------------------------------------------ *
 * 5. The card
 * ------------------------------------------------------------------ */

#define MOUNT "/sdcard"
#define STATE "/sdcard.prop"

static void card_ready(void)
{
    static const char *const FOLDERS[] = {"wallpapers", "tones", "backup_db", "music",
                                          "update",     "apps",  "untrusted"};
    size_t i;

    nd_storage_set_paths(MOUNT, STATE);
    pt_write_text(STATE, "state=mounted\ndevice=/dev/vdc\nfstype=ext4\nlabel=NEODCT\n");
    for (i = 0u; i < ND_ARRAY_LEN(FOLDERS); i++) {
        char path[ND_PATH_MAX];

        CHECK_INT(nd_snprintf(path, sizeof path, "%s/%s", MOUNT, FOLDERS[i]), ND_OK);
        pt_mkdir(path);
    }
}

static void test_find(void)
{
    char found[ND_NAP_MAX_FOUND][ND_STORAGE_PATH_MAX];
    size_t n;

    /* No card: nothing, and nothing is looked at. */
    nd_storage_set_paths(MOUNT, STATE);
    CHECK_INT(nd_nap_find(found, ND_NAP_MAX_FOUND), 0);

    card_ready();
    pt_write_text(MOUNT "/bible.nap", "x");
    pt_write_text(MOUNT "/apps/Chess.NAP", "x");
    pt_write_text(MOUNT "/untrusted/aardvark.nap", "x");
    pt_write_text(MOUNT "/notes.txt", "x");
    pt_write_text(MOUNT "/music/song.nap", "x"); /* not a place we look */
    pt_mkdir(MOUNT "/folder.nap");               /* not a file */
    pt_write_text(MOUNT "/apps/Chess/manifest.json", "{}");

    n = nd_nap_find(found, ND_NAP_MAX_FOUND);
    CHECK_INT(n, 3);
    if (n == 3u) {
        CHECK_STR(found[0], MOUNT "/untrusted/aardvark.nap");
        CHECK_STR(found[1], MOUNT "/bible.nap");
        CHECK_STR(found[2], MOUNT "/apps/Chess.NAP");
    }

    /* A caller with room for one gets the first one. */
    CHECK_INT(nd_nap_find(found, 1u), 1);
    CHECK_STR(found[0], MOUNT "/untrusted/aardvark.nap");

    nd_storage_set_paths(NULL, NULL);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Menu ids: the band, and the collision nobody was checking for
 * ------------------------------------------------------------------ *
 *
 * nd_ui.c sorts the app list by manifest "id" with a STABLE insertion sort,
 * so two apps claiming the same id keep the order readdir returned them in --
 * install order on ext4, which is nothing an owner can see or predict. There
 * was no allocation scheme and no check, and the two .nap packages that exist
 * today BOTH claim 13, because 13 is the next number after Update's 12 and
 * both authors counted the same way. Neither the band nor the collision
 * refuses an install; both have to be VISIBLE, which is what this pins.
 */
static void test_id_conflicts(void)
{
    char clash[64];

    pt_mkdir("/card/apps");

    /* Nothing installed: nothing to collide with, and an apps directory that
     * is not there is not an error -- Settings asks before the card is set
     * up. */
    CHECK(!nd_nap_id_conflict("/card/apps", 13, NULL, clash, sizeof clash));
    CHECK_STR(clash, "");
    CHECK(!nd_nap_id_conflict("/card/nosuch", 13, NULL, clash, sizeof clash));
    CHECK(!nd_nap_id_conflict(NULL, 13, NULL, clash, sizeof clash));

    write_single("/card/Demo.nap");
    CHECK_INT(nd_nap_install("/card/Demo.nap", "/card/apps", ND_NAP_ARCH_LUCKFOX, NULL, NULL, 0u),
              ND_OK);

    /* THE ACTUAL BUG: a second package that also picked 13. The name comes
     * back so the install screen can say WHICH app it will sit beside. */
    CHECK(nd_nap_id_conflict("/card/apps", 13, NULL, clash, sizeof clash));
    CHECK_STR(clash, "Demo App");

    /* A different id is not a collision, and neither is the app colliding
     * with ITSELF -- re-installing the same package must not warn. */
    CHECK(!nd_nap_id_conflict("/card/apps", 14, NULL, clash, sizeof clash));
    CHECK(!nd_nap_id_conflict("/card/apps", 13, "DemoApp", clash, sizeof clash));

    /* A directory with no manifest is a dead install, not an app, so it
     * claims no id at all -- the same rule nd_nap_is_installed() uses. */
    pt_mkdir("/card/apps/Dead");
    CHECK(!nd_nap_id_conflict("/card/apps", 999, NULL, clash, sizeof clash));

    /* And the band itself, which docs/NAP-PACKAGES.md publishes to package
     * authors. Stock apps are 1-12 today and the 9xx block is reserved for
     * the two that must sort last, so an installed app belongs strictly
     * between them. 13 -- what both real packages chose -- is outside it, and
     * that is the point: it sorts in among the stock apps. */
    CHECK(ND_NAP_ID_MIN > 99);
    CHECK(ND_NAP_ID_MAX < 900);
    CHECK(ND_NAP_ID_MIN < ND_NAP_ID_MAX);
    CHECK(13 < ND_NAP_ID_MIN);
}

int main(void)
{
    RUN(test_machine_to_arch);
    RUN(test_dir_from_name);
    RUN(test_display_name);
    RUN(test_inspect_single);
    RUN(test_inspect_universal);
    RUN(test_inspect_joins_the_prefix);
    RUN(test_inspect_reads_optional_metadata);
    RUN(test_extract_icon);
    RUN(test_refusals);
    RUN(test_install_single);
    RUN(test_install_universal_picks_this_phone);
    RUN(test_replace_keeps_data);
    RUN(test_a_failed_replacement_puts_the_old_app_back);
    RUN(test_a_dead_install_is_replaced_quietly);
    RUN(test_id_conflicts);
    RUN(test_find);
    return pt_report("test_nap");
}
