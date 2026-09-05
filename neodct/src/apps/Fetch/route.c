/* route.c -- what a downloaded file is, and where it belongs.
 *
 * Everything here is pure but for fetch_prepare_dir() and fetch_write_cue(),
 * which is why the destination rules take `mount` and `psx_installed` as
 * arguments rather than asking nd_storage: the whole of this file is
 * exercised by test_fetch.c against a scratch directory, with no card, no
 * network and no PSX app.
 *
 * ============ THE FOUR DESTINATIONS, AND WHO READS THEM ============
 *
 *   music/                     MusicPlayer scans it. 0750 ndusr:ndusr.
 *   apps/PSX/games/<T>/<T>.bin the PSX app's coverflow reads exactly this
 *                              shape -- one folder per disc, named after the
 *                              disc. See ../neodct-pcsxrearmed/README.md.
 *   untrusted/                 nd_nap_find() scans the card root, apps/ and
 *                              untrusted/, so a .nap dropped here appears in
 *                              Settings -> Install apps with nothing further
 *                              to do. It is also where the browser downloads,
 *                              so it is the right home for anything else.
 *
 * There is deliberately no "roms" folder. neodct-sdcard's CARD_LAYOUT is the
 * list of directories that exist on a card, adding one means every card in
 * the field grows it at the next mount, and nothing on the phone would read
 * it -- PSX only looks inside its own app directory, because a confined app
 * cannot see anything else. A .bin therefore goes where it will actually be
 * played, or, when PSX is not installed, to untrusted/ where it can wait.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fetch_app.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_storage.h"
#include "nd_types.h"

/* A raw PlayStation disc is a run of these. mkcue.sh checks the same number
 * for the same reason: a file that is not a multiple of it is not a raw
 * image, and a cue sheet claiming otherwise fails later and further away. */
#define FETCH_PSX_SECTOR 2352

/* ------------------------------------------------------------------ *
 * Names
 * ------------------------------------------------------------------ */

/* The extension, lowercased, or "" when there is none. `out` is small on
 * purpose: an "extension" longer than this is not one. */
static void extension_of(const char *name, char *out, size_t out_sz)
{
    const char *dot = strrchr(name, '.');
    size_t i;

    out[0] = '\0';
    if (dot == NULL || dot == name || dot[1] == '\0')
        return;
    dot++;
    if (strlen(dot) >= out_sz)
        return;
    for (i = 0u; dot[i] != '\0'; i++) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = '\0';
}

static bool ext_in(const char *ext, const char *const *set, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (strcmp(ext, set[i]) == 0)
            return true;
    }
    return false;
}

fetch_dest_kind fetch_classify(const char *name)
{
    /* What MusicPlayer and mpv between them will open. */
    static const char *const MUSIC[] = {"mp3", "m4a", "aac", "wav", "flac", "ogg", "opus"};
    /* What PCSX-ReARMed loads. .cue and .m3u come with a .bin and are routed
     * to the same folder so that uploading a two-file disc works. */
    static const char *const GAME[] = {"bin", "cue", "img", "chd", "pbp", "m3u"};
    char ext[8];

    if (name == NULL)
        return FETCH_DEST_OTHER;
    /* The console's own ROM, checked by whole name before its .bin extension
     * can send it to games/. It belongs in the emulator's bios/ instead. */
    if (strcasecmp(name, "bios.bin") == 0)
        return FETCH_DEST_BIOS;
    extension_of(name, ext, sizeof ext);
    if (ext[0] == '\0')
        return FETCH_DEST_OTHER;
    if (strcmp(ext, "nap") == 0)
        return FETCH_DEST_NAP;
    if (ext_in(ext, MUSIC, ND_ARRAY_LEN(MUSIC)))
        return FETCH_DEST_MUSIC;
    if (ext_in(ext, GAME, ND_ARRAY_LEN(GAME)))
        return FETCH_DEST_GAME;
    return FETCH_DEST_OTHER;
}

bool fetch_name_is_safe(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0')
        return false;
    if (strlen(name) >= ND_FETCH_NAME_MAX)
        return false;
    /* "-o" as a file name would become an option the moment it reached an
     * argv, and this app builds argv from listings. Refuse it here, once,
     * rather than remembering to write "./%s" at four call sites. */
    if (name[0] == '-')
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (i = 0u; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20u || c == 0x7fu)
            return false;
        if (c == '/' || c == '\\')
            return false;
    }
    return true;
}

/* The file name without its extension -- the disc's title, and therefore the
 * name of the folder it gets. Falls back to the whole name when there is no
 * extension, which fetch_classify() has already made impossible for a game. */
static nd_err stem_of(const char *name, char *out, size_t out_sz)
{
    const char *dot = strrchr(name, '.');
    size_t len = (dot != NULL && dot != name) ? (size_t)(dot - name) : strlen(name);

    if (len == 0u || len >= out_sz)
        return ND_ERR_TOOLONG;
    memcpy(out, name, len);
    out[len] = '\0';
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Destinations
 * ------------------------------------------------------------------ */

nd_err fetch_dest_path(const char *mount, const char *name, bool psx_installed, char *out,
                       size_t out_sz, fetch_dest_kind *kind)
{
    fetch_dest_kind k;

    if (mount == NULL || name == NULL || out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    if (!fetch_name_is_safe(name))
        return ND_ERR_INVAL;

    k = fetch_classify(name);
    /* A disc image or a BIOS with nowhere to be played is not one yet. It
     * lands in untrusted/ under its own name, and moving it in later is the
     * PSX app's business rather than this one's. */
    if ((k == FETCH_DEST_GAME || k == FETCH_DEST_BIOS) && !psx_installed)
        k = FETCH_DEST_OTHER;

    if (kind != NULL)
        *kind = k;

    switch (k) {
    case FETCH_DEST_MUSIC:
        return nd_snprintf(out, out_sz, "%s/music/%s", mount, name);
    case FETCH_DEST_GAME: {
        char stem[ND_FETCH_NAME_MAX];
        nd_err rc = stem_of(name, stem, sizeof stem);

        if (rc != ND_OK)
            return rc;
        return nd_snprintf(out, out_sz, "%s/apps/PSX/games/%s/%s", mount, stem, name);
    }
    case FETCH_DEST_BIOS:
        /* Under the one name the core is sure to find (bios = auto scans the
         * system directory), so an uploaded bios.bin is usable with nothing
         * further to do. */
        return nd_snprintf(out, out_sz, "%s/apps/PSX/bios/scph1001.bin", mount);
    case FETCH_DEST_NAP:
    case FETCH_DEST_OTHER:
    default:
        return nd_snprintf(out, out_sz, "%s/" ND_SD_UNTRUSTED_NAME "/%s", mount, name);
    }
}

/* mkdir every component of `dir` that is missing. Bounded by the caller's
 * buffer and by the fact that every path here is built from constants and one
 * vetted name, so there is no recursion and no unbounded walk. */
static nd_err mkdir_p(const char *dir)
{
    char work[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    size_t i;

    if (nd_strlcpy(work, dir, sizeof work) >= sizeof work)
        return ND_ERR_TOOLONG;

    for (i = 1u; work[i] != '\0'; i++) {
        if (work[i] != '/')
            continue;
        work[i] = '\0';
        if (nd_path_resolve(resolved, sizeof resolved, work) == ND_OK) {
            if (mkdir(resolved, 0755) != 0 && errno != EEXIST)
                return ND_ERR_IO;
        }
        work[i] = '/';
    }
    if (nd_path_resolve(resolved, sizeof resolved, work) != ND_OK)
        return ND_ERR_TOOLONG;
    if (mkdir(resolved, 0755) != 0 && errno != EEXIST)
        return ND_ERR_IO;
    return ND_OK;
}

nd_err fetch_prepare_dir(const char *path)
{
    char dir[ND_PATH_MAX];
    char *slash;

    if (path == NULL)
        return ND_ERR_INVAL;
    if (nd_strlcpy(dir, path, sizeof dir) >= sizeof dir)
        return ND_ERR_TOOLONG;
    slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir)
        return ND_OK; /* nothing above it to make */
    *slash = '\0';
    return mkdir_p(dir);
}

/* ------------------------------------------------------------------ *
 * Cue sheets
 * ------------------------------------------------------------------ */

nd_err fetch_write_cue(const char *bin_path)
{
    char cue[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    const char *base;
    const char *dot;
    struct stat st;
    FILE *f;
    size_t len;

    if (bin_path == NULL)
        return ND_ERR_INVAL;
    dot = strrchr(bin_path, '.');
    if (dot == NULL)
        return ND_ERR_INVAL;
    len = (size_t)(dot - bin_path);
    if (nd_snprintf(cue, sizeof cue, "%.*s.cue", (int)len, bin_path) != ND_OK)
        return ND_ERR_TOOLONG;
    if (nd_path_is_file(cue))
        return ND_OK; /* the owner uploaded a real one; never overwrite it */

    if (nd_path_resolve(resolved, sizeof resolved, bin_path) != ND_OK)
        return ND_ERR_TOOLONG;
    if (stat(resolved, &st) != 0)
        return ND_ERR_NOTFOUND;
    if (st.st_size <= 0 || (st.st_size % FETCH_PSX_SECTOR) != 0) {
        nd_log(ND_LOG_FETCH, "no cue for %s: not a whole number of %d-byte sectors", bin_path,
               FETCH_PSX_SECTOR);
        return ND_ERR_UNSUPPORTED;
    }

    base = strrchr(bin_path, '/');
    base = (base != NULL) ? base + 1 : bin_path;

    if (nd_path_resolve(resolved, sizeof resolved, cue) != ND_OK)
        return ND_ERR_TOOLONG;
    f = fopen(resolved, "we");
    if (f == NULL)
        return ND_ERR_IO;
    if (fprintf(f, "FILE \"%s\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n", base) <
        0) {
        (void)fclose(f);
        (void)unlink(resolved);
        return ND_ERR_IO;
    }
    if (fclose(f) != 0) {
        (void)unlink(resolved);
        return ND_ERR_IO;
    }
    nd_log(ND_LOG_FETCH, "wrote %s", cue);
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Sizes on screen
 * ------------------------------------------------------------------ */

void fetch_format_size(int64_t bytes, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    if (bytes < 0) {
        (void)nd_strlcpy(out, "?", out_sz);
    } else if (bytes >= 1024 * 1024) {
        /* One decimal, computed in integers: this phone has no FPU worth
         * using and a tenth of a megabyte is (bytes * 10 / MB) % 10. */
        int64_t tenths = (bytes * 10) / (1024 * 1024);

        (void)nd_snprintf(out, out_sz, "%lld.%lld MB", (long long)(tenths / 10),
                          (long long)(tenths % 10));
    } else if (bytes >= 1024) {
        (void)nd_snprintf(out, out_sz, "%lld kB", (long long)(bytes / 1024));
    } else {
        (void)nd_snprintf(out, out_sz, "%lld B", (long long)bytes);
    }
}
