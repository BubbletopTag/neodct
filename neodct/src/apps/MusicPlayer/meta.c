/* apps/MusicPlayer/meta.c -- the card scan, the ID3 metadata and the two
 * string helpers the "Now Playing" screen is laid out with.
 *
 * Everything here is a one-to-one port of a named function in
 * System/apps/MusicPlayer/main.py: scan_music, get_metadata, find_folder_art,
 * format_time, and run_now_playing's nested truncate(). The two that look
 * wrong -- the per-directory sort and truncate's off-by-one -- are wrong in
 * the Python and are ported wrong on purpose. README.md already says the
 * sorting and metadata support here are limited.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nd_font.h"
#include "nd_id3.h"
#include "nd_image.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_storage.h"
#include "nd_types.h"

#include "music.h"

/* ------------------------------------------------------------------ *
 * The strings
 * ------------------------------------------------------------------ */

const char *const nd_music_no_card_message = "No SD card.\nMusic is played from a card.";

const char *const nd_music_no_card_help =
    "Music is played from an SD card.\n"
    "\n"
    "Format a card as FAT32, make a folder called \"music\" on it, and copy "
    "your .mp3, .flac, .wav or .ogg files into it.\n"
    "\n"
    "Put the card in the phone and your music shows up here. The phone can "
    "set a blank card up for you from Settings.";

const char *const nd_music_unknown_artist = "Unknown Artist";

/* No `nice -n -10` in front of it any more; music.h says why at length. */
const char *const nd_music_mpv_cmd[ND_MUSIC_MPV_ARGC] = {"mpv", "--no-video", "--audio-buffer=4",
                                                         "--quiet"};

/* ------------------------------------------------------------------ *
 * ASCII case folding
 * ------------------------------------------------------------------ */

/* Python's str.lower() does not consult the locale and tolower() does, so on
 * a machine with a Turkish locale the two would disagree about "I". Every
 * comparison in this file that the Python spells .lower() uses this. */
static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static bool ends_with_lower(const char *name, const char *suffix)
{
    size_t nl;
    size_t sl;
    size_t i;

    if (name == NULL || suffix == NULL)
        return false;
    nl = strlen(name);
    sl = strlen(suffix);
    if (nl < sl)
        return false;
    for (i = 0u; i < sl; i++) {
        if (ascii_lower(name[nl - sl + i]) != suffix[i])
            return false;
    }
    return true;
}

static bool equals_lower(const char *a, const char *b)
{
    size_t i;

    for (i = 0u;; i++) {
        char ca = ascii_lower(a[i]);
        char cb = ascii_lower(b[i]);

        if (ca != cb)
            return false;
        if (ca == '\0')
            return true;
    }
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

/* ------------------------------------------------------------------ *
 * The extension filter
 * ------------------------------------------------------------------ */

/* _MiniaudioPlayer.EXTS and _MpvPlayer.EXTS, in the Python's order. The
 * STREAM list keeps .flac and .ogg although dr_mp3 and dr_wav read neither;
 * see music.h and OPEN-QUESTIONS.md MU-4. */
static const char *const STREAM_EXTS[] = {".mp3", ".wav", ".flac", ".ogg"};
static const char *const MPV_EXTS[] = {".mp3", ".wav", ".aac", ".flac", ".ogg"};

bool nd_music_is_supported(const char *filename, nd_music_backend backend)
{
    const char *const *exts;
    size_t n;
    size_t i;

    if (filename == NULL)
        return false;

    /* `exts = self.player.EXTS if self.player else ()` -- with no player,
     * nothing matches and the playlist comes back empty. */
    if (backend == ND_MUSIC_BACKEND_STREAM) {
        exts = STREAM_EXTS;
        n = ND_ARRAY_LEN(STREAM_EXTS);
    } else if (backend == ND_MUSIC_BACKEND_MPV) {
        exts = MPV_EXTS;
        n = ND_ARRAY_LEN(MPV_EXTS);
    } else {
        return false;
    }

    for (i = 0u; i < n; i++) {
        if (ends_with_lower(filename, exts[i]))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * music_dir() and scan_music()
 * ------------------------------------------------------------------ */

bool nd_music_dir(char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return false;
    /* Storage.folder() answers only once a NeoDCT card is actually mounted,
     * which is why scan_music simply finds nothing without one. */
    return nd_storage_folder(ND_MUSIC_FOLDER, out, out_sz);
}

/* os.walk()'s pending-directory list. Heap, not stack: 64 * 256 is 16 kB and
 * CODING-STANDARDS.md section 1.5 keeps anything sized by input off the
 * stack. */
typedef struct {
    char dir[ND_MUSIC_WALK_MAX][ND_MUSIC_PATH_MAX];
    size_t n;
} walk_stack;

static bool walk_push(walk_stack *w, const char *path)
{
    if (w->n >= ND_MUSIC_WALK_MAX)
        return false;
    if (nd_strlcpy(w->dir[w->n], path, ND_MUSIC_PATH_MAX) >= ND_MUSIC_PATH_MAX)
        return false;
    w->n++;
    return true;
}

/* `for f in sorted(files)`: byte order, over ONE directory's entries. The
 * playlist is therefore NOT globally sorted -- a track in a subdirectory
 * sorts after every track beside its parent, wherever its name falls. That
 * is what README.md means by the sorting being limited, and it is ported.
 * Insertion sort is stable and the slice is bounded at ND_MUSIC_MAX. */
static void sort_slice_by_filename(nd_music_track *t, size_t start, size_t end)
{
    size_t i;

    for (i = start + 1u; i < end; i++) {
        nd_music_track key = t[i];
        size_t j = i;

        while (j > start && strcmp(basename_of(t[j - 1u].path), basename_of(key.path)) > 0) {
            t[j] = t[j - 1u];
            j--;
        }
        t[j] = key;
    }
}

/* One directory of the walk: its files go into `out`, its subdirectories go
 * onto `w`. Returns the new entry count. */
static size_t walk_one(const char *dir, nd_music_track *out, size_t max, size_t n, walk_stack *w,
                       nd_music_backend backend)
{
    char resolved[ND_PATH_MAX];
    size_t start = n;
    size_t sub_base;
    bool warned_full = false;
    DIR *d;
    struct dirent *ent;

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return n;
    d = opendir(resolved);
    if (d == NULL)
        return n; /* os.walk swallows an unreadable directory too */

    sub_base = w->n;
    while ((ent = readdir(d)) != NULL) {
        char child[ND_MUSIC_PATH_MAX];
        char child_real[ND_PATH_MAX];
        struct stat st;
        bool is_dir;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (nd_snprintf(child, sizeof child, "%s/%s", dir, ent->d_name) != ND_OK) {
            nd_log(ND_LOG_MUSIC, "Path too long, skipped: %s/%s", dir, ent->d_name);
            continue;
        }

#ifdef DT_DIR
        if (ent->d_type == DT_DIR)
            is_dir = true;
        else if (ent->d_type != DT_UNKNOWN)
            is_dir = false;
        else
#endif
        {
            /* Some filesystems answer DT_UNKNOWN; os.scandir falls back to
             * stat() in exactly the same case. */
            if (nd_path_resolve(child_real, sizeof child_real, child) != ND_OK)
                continue;
            is_dir = (stat(child_real, &st) == 0) && S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            if (!walk_push(w, child))
                nd_log(ND_LOG_MUSIC, "Too many directories, not scanned: %s", child);
            continue;
        }
        if (!nd_music_is_supported(ent->d_name, backend))
            continue;
        if (n >= max) {
            if (!warned_full) {
                nd_log(ND_LOG_MUSIC, "More than %d tracks; the rest are not listed.", (int)max);
                warned_full = true;
            }
            continue;
        }
        (void)nd_strlcpy(out[n].path, child, sizeof out[n].path);
        n++;
    }
    (void)closedir(d);

    sort_slice_by_filename(out, start, n);

    /* os.walk is depth-first and descends in scandir order. A LIFO pops the
     * last push first, so the segment this directory just added is reversed
     * to put its first subdirectory back on top. */
    if (w->n > sub_base) {
        size_t lo = sub_base;
        size_t hi = w->n - 1u;

        while (lo < hi) {
            char tmp[ND_MUSIC_PATH_MAX];

            memcpy(tmp, w->dir[lo], sizeof tmp);
            memcpy(w->dir[lo], w->dir[hi], sizeof tmp);
            memcpy(w->dir[hi], tmp, sizeof tmp);
            lo++;
            hi--;
        }
    }
    return n;
}

size_t nd_music_scan(nd_music_track *out, size_t max, nd_music_backend backend)
{
    char root[ND_MUSIC_PATH_MAX];
    walk_stack *w;
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;
    /* `if exts and music_dir and os.path.exists(music_dir)` -- all three. */
    if (backend == ND_MUSIC_BACKEND_NONE)
        return 0u;
    if (!nd_music_dir(root, sizeof root))
        return 0u;
    if (!nd_path_exists(root))
        return 0u;

    /* owned here; freed before every return below */
    w = calloc(1u, sizeof *w);
    if (w == NULL)
        return 0u;

    if (!walk_push(w, root)) {
        free(w);
        return 0u;
    }
    while (w->n > 0u) {
        char dir[ND_MUSIC_PATH_MAX];

        w->n--;
        memcpy(dir, w->dir[w->n], sizeof dir);
        n = walk_one(dir, out, max, n, w, backend);
    }
    free(w);
    return n;
}

/* ------------------------------------------------------------------ *
 * find_folder_art()
 * ------------------------------------------------------------------ */

static const char *const ART_STEMS[] = {"cover", "folder", "front", "album", "albumart"};
static const char *const ART_EXTS[] = {".jpg", ".jpeg", ".png"};
#define ART_CANDIDATES (ND_ARRAY_LEN(ART_STEMS) * ND_ARRAY_LEN(ART_EXTS))

nd_image *nd_music_find_folder_art(const char *filepath)
{
    /* 15 * 256 = 3,840 bytes on the stack, fixed at compile time. */
    char found[ART_CANDIDATES][ND_MUSIC_PATH_MAX];
    char folder[ND_MUSIC_PATH_MAX];
    char resolved[ND_PATH_MAX];
    const char *slash;
    DIR *d;
    struct dirent *ent;
    size_t i;
    size_t j;

    if (filepath == NULL)
        return NULL;
    slash = strrchr(filepath, '/');
    if (slash == NULL)
        return NULL; /* os.path.dirname("x.mp3") is "", and listdir("") raises */
    if ((size_t)(slash - filepath) >= sizeof folder)
        return NULL;
    (void)nd_strlcpy(folder, filepath, (size_t)(slash - filepath) + 1u);

    for (i = 0u; i < ART_CANDIDATES; i++)
        found[i][0] = '\0';

    if (nd_path_resolve(resolved, sizeof resolved, folder) != ND_OK)
        return NULL;
    d = opendir(resolved);
    if (d == NULL)
        return NULL; /* `except OSError: return None` */

    /* `entries = {e.lower(): e for e in os.listdir(folder)}` -- a dict
     * comprehension, so with two names differing only in case the LAST one
     * listed wins. One pass, and the assignment below is unconditional for
     * exactly that reason. */
    while ((ent = readdir(d)) != NULL) {
        for (i = 0u; i < ND_ARRAY_LEN(ART_STEMS); i++) {
            for (j = 0u; j < ND_ARRAY_LEN(ART_EXTS); j++) {
                char want[64];

                if (nd_snprintf(want, sizeof want, "%s%s", ART_STEMS[i], ART_EXTS[j]) != ND_OK)
                    continue;
                if (equals_lower(ent->d_name, want)) {
                    size_t rank = i * ND_ARRAY_LEN(ART_EXTS) + j;

                    (void)nd_strlcpy(found[rank], ent->d_name, sizeof found[rank]);
                }
            }
        }
    }
    (void)closedir(d);

    /* The nested loop's order, and its `try/except pass`: a candidate that
     * exists but does not decode does NOT end the search, it just fails and
     * the next name is tried. */
    for (i = 0u; i < ART_CANDIDATES; i++) {
        char path[ND_MUSIC_PATH_MAX + 64];
        nd_image *img;

        if (found[i][0] == '\0')
            continue;
        if (nd_snprintf(path, sizeof path, "%s/%s", folder, found[i]) != ND_OK)
            continue;
        img = nd_image_open(path);
        if (img != NULL)
            return img;
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * get_metadata()
 * ------------------------------------------------------------------ */

/* nd_id3.h's picture callback: `Image.open(io.BytesIO(tag.data))` with the
 * Python's `break` inside the `try`, so a picture that does not decode leaves
 * the loop running and the next APIC frame is tried. */
static bool on_picture(const uint8_t *data, size_t len, void *ctx)
{
    nd_image **slot = (nd_image **)ctx;
    nd_image *img = nd_image_open_mem(data, len);

    if (img == NULL)
        return false; /* keep walking */
    *slot = img;
    return true; /* the `break` */
}

void nd_music_get_metadata(const char *path, nd_music_meta *out)
{
    nd_id3 tag;
    nd_err rc;

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (path == NULL)
        return;

    /* The defaults, before anything is read. */
    (void)nd_strlcpy(out->title, basename_of(path), sizeof out->title);
    (void)nd_strlcpy(out->artist, nd_music_unknown_artist, sizeof out->artist);
    out->album[0] = '\0';
    out->art = NULL;
    out->length = 0.0;

    rc = nd_id3_read(path, &tag, on_picture, &out->art);
    if (rc == ND_OK) {
        /* `if "TIT2" in audio.tags:` -- an ABSENT frame leaves the default,
         * a present but empty one replaces it with an empty string. */
        if (tag.has_title)
            (void)nd_strlcpy(out->title, tag.title, sizeof out->title);
        if (tag.has_artist)
            (void)nd_strlcpy(out->artist, tag.artist, sizeof out->artist);
        if (tag.has_album)
            (void)nd_strlcpy(out->album, tag.album, sizeof out->album);
    } else if (rc != ND_ERR_NOTFOUND) {
        /* `except Exception as e: print(f"[Music] Metadata error: {e}")`.
         * A file with no tag at all is not an error and the Python does not
         * print for it either -- mutagen returns tags=None. */
        nd_log(ND_LOG_MUSIC, "Metadata error: %s: %s", nd_strerror(rc), path);
    }

    /* mutagen's MP3 class only handles mp3; miniaudio reports the duration of
     * anything it can decode. Here both are the same call, because the
     * decoder that plays the file is also the one that measures it. */
    out->length = nd_music_duration(path);

    /* Many rips carry full ID3 text tags but no embedded APIC frame, so fall
     * back to sidecar art sitting next to the track. */
    if (out->art == NULL)
        out->art = nd_music_find_folder_art(path);
}

void nd_music_meta_free(nd_music_meta *m)
{
    if (m == NULL)
        return;
    nd_image_free(m->art);
    m->art = NULL;
}

/* ------------------------------------------------------------------ *
 * format_time() and truncate()
 * ------------------------------------------------------------------ */

void nd_music_format_time(int32_t seconds, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    /* `f"{m:02d}:{s:02d}"` on ints. Python's // and % floor toward negative
     * infinity, so a negative argument would print differently -- but every
     * caller has already passed the value through int(max(0, ...)) or
     * int(elapsed), and elapsed is a duration. Clamped rather than
     * reproduced, because the C division would differ silently. */
    if (seconds < 0)
        seconds = 0;
    (void)nd_snprintf(out, out_sz, "%02d:%02d", seconds / 60, seconds % 60);
}

/* Drop the last UTF-8 codepoint, in place. `t = t[:-1]` in the Python removes
 * one CHARACTER, and a byte-wise chop would leave half a sequence -- which
 * measures a different width and draws a replacement glyph. */
static void chop_last_cp(char *s)
{
    size_t n = strlen(s);

    if (n == 0u)
        return;
    n--;
    while (n > 0u && ((unsigned char)s[n] & 0xC0u) == 0x80u)
        n--;
    s[n] = '\0';
}

const char *nd_music_truncate(char *out, size_t out_sz, const char *text, const nd_font *f,
                              int32_t max_w)
{
    char probe[ND_MUSIC_TEXT_MAX + 8];
    int32_t w = 0;
    int32_t h = 0;
    size_t original;

    if (out == NULL || out_sz == 0u)
        return out;
    if (text == NULL) {
        out[0] = '\0';
        return out;
    }
    (void)nd_strlcpy(out, text, out_sz);
    original = strlen(out);

    /* THE TWO MEASUREMENTS ARE DIFFERENT ON PURPOSE. The first is of `t`
     * alone -- that is the test for "does this need truncating at all", and
     * it is why a string that fits comes back bare. Every measurement after
     * it is of `t + "..."`, because from then on the ellipsis is going to be
     * drawn. A rewrite that measured `t + "..."` on the first pass too would
     * put an ellipsis on strings that already fit, which is a visible change.
     * Measured against Pillow with the real font; the vectors are in
     * test_musicplayer.c. */
    nd_text_size(f, out, &w, &h);
    while (w > max_w && out[0] != '\0') {
        chop_last_cp(out);
        if (nd_snprintf(probe, sizeof probe, "%s...", out) != ND_OK)
            break;
        nd_text_size(f, probe, &w, &h);
    }

    if (strlen(out) < original) {
        (void)nd_strlcat(out, "...", out_sz);
    }
    return out;
}
