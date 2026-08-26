/* apps/MusicPlayer/library.c -- artists, albums, and a sort that is a sort.
 *
 * music.h carries the reasoning: why this exists at all when
 * OPEN-QUESTIONS.md MU-11 says the Python's flat, per-directory ordering was
 * deliberately ported; why it reads nd_id3_read() and never
 * nd_music_get_metadata(); and why the tables are sized at ND_MUSIC_MAX so
 * that overflow cannot happen. What follows is how.
 *
 * ============ THE SHAPE, WHICH IS THE WHOLE TRICK ============
 *
 * After the build, the three arrays are sorted so that every run is
 * CONTIGUOUS:
 *
 *     artists   sorted by name
 *     albums    sorted by artist, then by year, then by name
 *     songs     sorted by artist, album, disc, track, title, path
 *
 * so an artist is (first_album, n_albums), an album is (first_song, n_songs),
 * and every screen in the browser is a slice rather than a filter. No screen
 * allocates, nothing is searched, and "the third album of the fifth artist"
 * is two additions.
 *
 * Getting there is one sort and one walk:
 *
 *   1. read each track's tag, resolve its artist and album NAMES
 *   2. intern both, giving each song an artist id and an album id
 *   3. sort the songs on the full key -- the ids are not yet meaningful,
 *      so the comparator sorts on the NAMES the ids point at
 *   4. walk the sorted songs and rebuild the two tables in the order they
 *      now appear, renumbering as it goes
 *
 * Step 4 is what makes the ids contiguous; the ids from step 2 are scratch
 * and are thrown away.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_id3.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "music.h"

const char *const nd_music_unknown_album = "Unknown Album";

/* How often the progress callback fires. Every track would be a redraw per
 * file; a redraw costs more than the tag read it is reporting on. */
#define LIB_PROGRESS_EVERY 8u

struct nd_music_library {
    nd_music_song *songs;
    nd_music_artist *artists;
    nd_music_album *albums;
    /* Indices into songs[], in title order. See the "Songs" note in music.h:
     * an index, not a second copy, because the album runs the browser
     * navigates by have to stay contiguous in the primary order. */
    uint16_t *by_title;
    size_t n_songs;
    size_t n_artists;
    size_t n_albums;
};

/* ------------------------------------------------------------------ *
 * Names
 * ------------------------------------------------------------------ */

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* The two placeholders sort after every real name. Checked by pointer-free
 * comparison because the caller may pass either the shared literal or a copy
 * of it out of the tables. */
static bool is_placeholder(const char *s)
{
    return strcmp(s, nd_music_unknown_artist) == 0 || strcmp(s, nd_music_unknown_album) == 0;
}

int32_t nd_music_name_cmp(const char *a, const char *b)
{
    bool pa;
    bool pb;

    /* NULL is ordered rather than fatal: this is reached from qsort, where a
     * crash would be a corrupted sort rather than a diagnosable fault. */
    if (a == NULL)
        return (b == NULL) ? 0 : -1;
    if (b == NULL)
        return 1;

    pa = is_placeholder(a);
    pb = is_placeholder(b);
    if (pa != pb)
        return pa ? 1 : -1;

    for (;;) {
        char ca = lower_ascii(*a);
        char cb = lower_ascii(*b);

        if (ca != cb)
            return (int32_t)((unsigned char)ca) - (int32_t)((unsigned char)cb);
        if (ca == '\0')
            return 0;
        a++;
        b++;
    }
}

/* ------------------------------------------------------------------ *
 * Sorting
 * ------------------------------------------------------------------ */

int32_t nd_music_album_cmp(const nd_music_album *a, const nd_music_album *b)
{
    /* An album with no year sorts AFTER every dated one rather than in 1900:
     * "the ones I know about, in order, then the rest" is what a
     * discography should read like. */
    if (a->year != b->year) {
        if (a->year == 0u)
            return 1;
        if (b->year == 0u)
            return -1;
        return (a->year < b->year) ? -1 : 1;
    }
    return nd_music_name_cmp(a->name, b->name);
}

int32_t nd_music_song_cmp(const nd_music_song *a, const nd_music_song *b)
{
    if (a->disc != b->disc) {
        /* Disc 0 means "the tag did not say", which for a single-disc album
         * is every track -- so it must sort FIRST, not last, or a set with
         * one tagged disc would put the untagged tracks after it. */
        return (a->disc < b->disc) ? -1 : 1;
    }
    if (a->track != b->track) {
        /* Track 0 is "no number" and goes last, so a bonus track nobody
         * tagged does not open the album. */
        if (a->track == 0u)
            return 1;
        if (b->track == 0u)
            return -1;
        return (a->track < b->track) ? -1 : 1;
    }
    {
        int32_t c = nd_music_name_cmp(a->title, b->title);

        if (c != 0)
            return c;
    }
    /* The path is the last resort and is what makes the whole order total:
     * two files cannot share one, so the sort is deterministic even for two
     * untagged tracks with the same name. */
    return (int32_t)strcmp(a->path, b->path);
}

/* One row while the library is being built: the song, plus the two names and
 * the year the comparator orders by.
 *
 * THE KEY IS INSIDE THE ROW, not in a parallel array. qsort permutes whatever
 * you hand it and knows nothing about anything beside it, so a key array
 * indexed by position stops describing the song at that position the moment
 * the first swap happens -- and the result is a library sorted by a
 * comparator reading somebody else's artist name. Keeping the key in the row
 * makes that unrepresentable, and the comparator needs no context pointer
 * and no file-static to reach it.
 *
 * The cost is one transient copy of the songs: about 215 kB for a full
 * 256-track card, freed before the browser draws anything. */
typedef struct {
    nd_music_song song;
    char artist[ND_MUSIC_TEXT_MAX];
    char album[ND_MUSIC_TEXT_MAX];
    uint16_t year;
} build_row;

/* Albums group by year then name, in the same order nd_music_album_cmp puts
 * them, so that the album table built by the walk comes out in the order the
 * songs are already in. */
static int32_t row_group_cmp(const build_row *a, const build_row *b)
{
    int32_t c = nd_music_name_cmp(a->artist, b->artist);

    if (c != 0)
        return c;
    if (a->year != b->year) {
        if (a->year == 0u)
            return 1;
        if (b->year == 0u)
            return -1;
        return (a->year < b->year) ? -1 : 1;
    }
    return nd_music_name_cmp(a->album, b->album);
}

static int row_qsort_cmp(const void *lhs, const void *rhs)
{
    const build_row *a = lhs;
    const build_row *b = rhs;
    int32_t c = row_group_cmp(a, b);

    if (c == 0)
        c = nd_music_song_cmp(&a->song, &b->song);
    return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
}

/* ------------------------------------------------------------------ *
 * Reading one track
 * ------------------------------------------------------------------ */

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

/* Fills one row from the file's tag.
 *
 * NOTHING here can fail in a way the caller must handle. A file that has
 * gone, or has no tag, or has a tag full of nonsense, becomes an Unknown
 * Artist / Unknown Album track titled after its filename -- which is what
 * the Now Playing screen already does with the same file. */
static void read_track(const nd_music_track *t, build_row *row)
{
    nd_id3 tag;

    memset(row, 0, sizeof *row);
    memset(&tag, 0, sizeof tag);
    (void)nd_strlcpy(row->song.path, t->path, sizeof row->song.path);

    /* on_pic NULL: the tag body is read off the card either way, but no
     * picture is decoded. That is the difference between 15 MB and minutes.
     * The return value is ignored on purpose -- see the comment above. */
    (void)nd_id3_read(t->path, &tag, NULL, NULL);

    if (tag.has_title && tag.title[0] != '\0')
        (void)nd_strlcpy(row->song.title, tag.title, sizeof row->song.title);
    else
        (void)nd_strlcpy(row->song.title, basename_of(t->path), sizeof row->song.title);

    /* THE ALBUM ARTIST WINS. Without this a compilation shatters into one
     * artist per guest vocalist; with it, "Various Artists" is one row with
     * one album under it. TPE1 is the fallback, and the placeholder is the
     * fallback's fallback. */
    if (tag.has_albumartist && tag.albumartist[0] != '\0')
        (void)nd_strlcpy(row->artist, tag.albumartist, sizeof row->artist);
    else if (tag.has_artist && tag.artist[0] != '\0')
        (void)nd_strlcpy(row->artist, tag.artist, sizeof row->artist);
    else
        (void)nd_strlcpy(row->artist, nd_music_unknown_artist, sizeof row->artist);

    if (tag.has_album && tag.album[0] != '\0')
        (void)nd_strlcpy(row->album, tag.album, sizeof row->album);
    else
        (void)nd_strlcpy(row->album, nd_music_unknown_album, sizeof row->album);

    row->song.track = tag.track;
    row->song.disc = tag.disc;
    row->year = tag.year;
}

/* ------------------------------------------------------------------ *
 * The title index
 * ------------------------------------------------------------------ */

/* qsort has no context argument. The library being sorted reaches the
 * comparator through this, which is safe for the same reason the rest of the
 * build is: it runs on the UI thread and there is one library at a time. It
 * is cleared the moment the sort returns. */
static const nd_music_library *g_sort_lib;

static int by_title_cmp(const void *lhs, const void *rhs)
{
    const nd_music_library *lib = g_sort_lib;
    const nd_music_song *a = &lib->songs[*(const uint16_t *)lhs];
    const nd_music_song *b = &lib->songs[*(const uint16_t *)rhs];
    int32_t c = nd_music_name_cmp(a->title, b->title);

    if (c == 0)
        c = nd_music_name_cmp(lib->artists[a->artist].name, lib->artists[b->artist].name);
    /* The path last, so the order is TOTAL: two untagged tracks with the same
     * filename-derived title under the same artist still have an order, and
     * it is the same one on the next build. */
    if (c == 0)
        c = (int32_t)strcmp(a->path, b->path);
    return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
}

/* ------------------------------------------------------------------ *
 * Build
 * ------------------------------------------------------------------ */

void nd_music_library_free(nd_music_library *lib)
{
    if (lib == NULL)
        return;
    free(lib->songs);
    free(lib->artists);
    free(lib->albums);
    free(lib->by_title);
    free(lib);
}

nd_err nd_music_library_build(nd_music_library **out, const nd_music_track *tracks, size_t n,
                              nd_music_progress_fn cb, void *ctx)
{
    nd_music_library *lib;
    build_row *rows = NULL;
    nd_err rc = ND_ERR_NOMEM;
    size_t i;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;
    if (tracks == NULL && n > 0u)
        return ND_ERR_INVAL;
    if (n > (size_t)ND_MUSIC_MAX)
        n = (size_t)ND_MUSIC_MAX;

    lib = calloc(1u, sizeof *lib);
    if (lib == NULL)
        return ND_ERR_NOMEM;

    /* Everything is sized for n, not for ND_MUSIC_MAX: a card with six
     * tracks should not cost 220 kB. The caps in music.h bound n, not this. */
    if (n > 0u) {
        lib->songs = calloc(n, sizeof *lib->songs);
        lib->artists = calloc(n, sizeof *lib->artists);
        lib->albums = calloc(n, sizeof *lib->albums);
        lib->by_title = calloc(n, sizeof *lib->by_title);
        rows = calloc(n, sizeof *rows);
        if (lib->songs == NULL || lib->artists == NULL || lib->albums == NULL ||
            lib->by_title == NULL || rows == NULL)
            goto done;
    }

    /* ---- 1: read every tag ---- */
    for (i = 0u; i < n; i++) {
        read_track(&tracks[i], &rows[i]);

        if (cb != NULL && ((i + 1u) % LIB_PROGRESS_EVERY == 0u || i + 1u == n)) {
            if (!cb(ctx, i + 1u, n)) {
                rc = ND_ERR_BUSY;
                goto done;
            }
        }
    }

    /* ---- 2: one sort, on rows that carry their own key ---- */
    if (n > 1u)
        qsort(rows, n, sizeof *rows, row_qsort_cmp);

    /* ---- 3: walk the sorted rows and build the two tables ----
     *
     * Every run is contiguous by construction, because the rows are already
     * in exactly the order the tables describe. A new artist starts an artist
     * row; a new (album, year) within that artist starts an album row. */
    for (i = 0u; i < n; i++) {
        const build_row *r = &rows[i];
        /* A run ends where the name changes. The comparison is
         * nd_music_name_cmp and not strcmp so that "ABBA" and "Abba" are one
         * artist rather than two -- the sort has already put them together,
         * and splitting the run here would produce two rows with the same
         * name a screen apart. */
        bool new_artist = (i == 0u) || nd_music_name_cmp(rows[i - 1u].artist, r->artist) != 0;
        /* An album break is a new artist, a different album name, or the same
         * name in a different year -- a band that re-recorded a record is two
         * albums, not one with the tracks interleaved. */
        bool new_album = new_artist || nd_music_name_cmp(rows[i - 1u].album, r->album) != 0 ||
                         rows[i - 1u].year != r->year;

        if (new_artist) {
            nd_music_artist *a = &lib->artists[lib->n_artists++];

            (void)nd_strlcpy(a->name, r->artist, sizeof a->name);
            a->first_album = (uint16_t)lib->n_albums;
            a->n_albums = 0u;
            a->n_songs = 0u;
        }
        if (new_album) {
            nd_music_album *al = &lib->albums[lib->n_albums++];

            (void)nd_strlcpy(al->name, r->album, sizeof al->name);
            al->artist = (uint16_t)(lib->n_artists - 1u);
            al->year = r->year;
            al->first_song = (uint16_t)i;
            al->n_songs = 0u;
            lib->artists[lib->n_artists - 1u].n_albums++;
        }

        lib->songs[i] = r->song;
        lib->songs[i].artist = (uint16_t)(lib->n_artists - 1u);
        lib->songs[i].album = (uint16_t)(lib->n_albums - 1u);
        lib->albums[lib->n_albums - 1u].n_songs++;
        lib->artists[lib->n_artists - 1u].n_songs++;
    }
    lib->n_songs = n;

    /* ---- 4: the title index ----
     *
     * Built from the finished songs array, so the tie-break on artist reads
     * the artist NAME through the table rather than comparing the artist ids
     * the walk has just assigned -- ids order by the primary sort, and using
     * them here would make the tie-break depend on the very thing this index
     * exists to ignore. */
    for (i = 0u; i < n; i++)
        lib->by_title[i] = (uint16_t)i;
    if (n > 1u) {
        g_sort_lib = lib;
        qsort(lib->by_title, n, sizeof *lib->by_title, by_title_cmp);
        g_sort_lib = NULL;
    }

    rc = ND_OK;
    *out = lib;
    lib = NULL;

done:
    free(rows);
    nd_music_library_free(lib);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Reading it back
 * ------------------------------------------------------------------ */

size_t nd_music_library_n_songs(const nd_music_library *lib)
{
    return (lib != NULL) ? lib->n_songs : 0u;
}

size_t nd_music_library_n_artists(const nd_music_library *lib)
{
    return (lib != NULL) ? lib->n_artists : 0u;
}

size_t nd_music_library_n_albums(const nd_music_library *lib)
{
    return (lib != NULL) ? lib->n_albums : 0u;
}

const nd_music_song *nd_music_library_song(const nd_music_library *lib, size_t i)
{
    if (lib == NULL || i >= lib->n_songs)
        return NULL;
    return &lib->songs[i];
}

const nd_music_artist *nd_music_library_artist(const nd_music_library *lib, size_t i)
{
    if (lib == NULL || i >= lib->n_artists)
        return NULL;
    return &lib->artists[i];
}

const nd_music_album *nd_music_library_album(const nd_music_library *lib, size_t i)
{
    if (lib == NULL || i >= lib->n_albums)
        return NULL;
    return &lib->albums[i];
}

const nd_music_song *nd_music_library_song_by_title(const nd_music_library *lib, size_t i)
{
    if (lib == NULL || i >= lib->n_songs || lib->by_title == NULL)
        return NULL;
    return &lib->songs[lib->by_title[i]];
}
