/* test_musicplayer.c -- the Music app (id 970) and lib/nd_id3.c.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The strings are the Python's, byte for byte -- NO_CARD_HELP, the
 *     no-card dialog message, MPV_CMD and "Unknown Artist". Three of them
 *     reach the panel and the fourth is an argv.
 *
 *  2. _MiniaudioPlayer.EXTS and _MpvPlayer.EXTS, ASCII-case-insensitively and
 *     with no locale. `.flac` and `.ogg` stay in the STREAM list even though
 *     dr_mp3 and dr_wav read neither; a track that vanishes from the list is
 *     a worse answer than one that plays through the mpv fallback.
 *
 *  3. _pick_player() reads NEODCT_MUSIC_AUDIO exactly as the Python spells
 *     it: only the literal "subprocess" forces mpv.
 *
 *  4. format_time() is "%02d:%02d" of (s // 60, s % 60) on an INT, so a track
 *     over 99 minutes grows the field rather than wrapping.
 *
 *  5. truncate() AGAINST PILLOW. The five vectors below were computed by
 *     running the Python's own truncate() over ImageDraw.textbbox with
 *     System/ui/resources/fonts/font.ttf at 14 px and 20 px. They are the
 *     oracle, not a guess, and they pin the one asymmetry in the function --
 *     the bare first measurement -- which is what lets a string that fits
 *     come back with no ellipsis.
 *
 *  6. nd_id3.c: four text encodings, ID3v2.2/2.3/2.4 frame walks, the
 *     picture callback's "keep going after a decode failure" contract, and
 *     the bounds. THE HOSTILE CASES ARE THE POINT: a frame claiming
 *     0xFFFFFFFF bytes inside a 40-byte tag, a header claiming a 256 MB tag,
 *     a size of zero. Every one of those lengths came out of the file, and
 *     an SD card is a file somebody else wrote.
 *
 *  7. scan_music() walks os.walk order with each directory's own files sorted
 *     by BYTE VALUE and no global sort -- so a track in a subdirectory sorts
 *     after every track beside its parent whatever its name is. README.md
 *     calls the sorting limited; this pins the limitation rather than fixing
 *     it.
 *
 *  8. find_folder_art()'s nested name x extension order, matched
 *     case-insensitively against the real directory entries.
 *
 *  9. get_metadata()'s defaults, and that a tagged file overrides them.
 *
 * 10. Playback really spawns and really stops, driven against stub `aplay`
 *     and `nice` binaries on $PATH -- so no sound card, no mpv, and nothing
 *     is played. The claims are that a child appears, that the fallback
 *     ladder sends ONE unsupported track to mpv without moving the session
 *     off the in-process path, that pause and resume reach the child, and
 *     that app_shutdown() releases the sound card. That last one is the
 *     SIGTERM teardown contract in nd_app.h and the reason the phone does not
 *     ring silently.
 *
 * 11. THE GOLDEN FRAME. app-musicplayer is the "No SD card." MessageDialog,
 *     judged by the SHA-256 over raw RGB that goldenframe.py compares. It is
 *     captured with a budget of ONE frame, which is exactly how nd-shoot
 *     takes it: run(ui) walks straight from the dialog into the TextScroller
 *     with no key in between, so the recording has to stop at the dialog the
 *     way uistub's ScriptExhausted stops the Python's.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <math.h>
#include <time.h>

#include "nd_id3.h"
#include "nd_settings.h"
#include "nd_storage.h"

#include "smallapp_test.h"

#include "../../apps/MusicPlayer/music.h"

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    nd_music_backend (*pick_player)(void);
    nd_music_backend (*backend_now)(void);
    void (*player_init)(nd_music_backend);
    bool (*is_supported)(const char *, nd_music_backend);
    bool (*dir)(char *, size_t);
    size_t (*scan)(nd_music_track *, size_t, nd_music_backend);
    void (*get_metadata)(const char *, nd_music_meta *);
    void (*meta_free)(nd_music_meta *);
    nd_image *(*find_folder_art)(const char *);
    double (*duration)(const char *);
    void (*format_time)(int32_t, char *, size_t);
    const char *(*truncate)(char *, size_t, const char *, const nd_font *, int32_t);
    bool (*play)(const char *);
    void (*stop)(void);
    void (*toggle_pause)(void);

    /* ---- the library: new in this revision, not a port ---- */
    nd_err (*lib_build)(nd_music_library **, const nd_music_track *, size_t,
                        nd_music_progress_fn, void *);
    void (*lib_free)(nd_music_library *);
    size_t (*lib_n_songs)(const nd_music_library *);
    size_t (*lib_n_artists)(const nd_music_library *);
    size_t (*lib_n_albums)(const nd_music_library *);
    const nd_music_song *(*lib_song)(const nd_music_library *, size_t);
    const nd_music_song *(*lib_song_by_title)(const nd_music_library *, size_t);
    const nd_music_artist *(*lib_artist)(const nd_music_library *, size_t);
    const nd_music_album *(*lib_album)(const nd_music_library *, size_t);
    int32_t (*name_cmp)(const char *, const char *);
    const char *const *unknown_album;

    /* ---- volume: new in this revision, not a port ---- */
    int32_t (*volume)(void);
    void (*set_volume)(int32_t);
    void (*volume_load)(void);
    int32_t (*gain_q15)(int32_t);
    int16_t (*apply_gain)(int16_t, int32_t);
    void (*gain_buffer)(int16_t *, size_t, int32_t);
    bool (*is_paused)(void);
    bool (*is_finished)(void);
    bool (*position)(double *);
    pid_t (*child_pid)(void);
    const char *const *no_card_help;
    const char *const *no_card_message;
    const char *const *unknown_artist;
    const char *const *mpv_cmd;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.pick_player = sa_sym(h, "nd_music_pick_player");
    *(void **)&api.backend_now = sa_sym(h, "nd_music_backend_now");
    *(void **)&api.player_init = sa_sym(h, "nd_music_player_init");
    *(void **)&api.is_supported = sa_sym(h, "nd_music_is_supported");
    *(void **)&api.dir = sa_sym(h, "nd_music_dir");
    *(void **)&api.scan = sa_sym(h, "nd_music_scan");
    *(void **)&api.get_metadata = sa_sym(h, "nd_music_get_metadata");
    *(void **)&api.meta_free = sa_sym(h, "nd_music_meta_free");
    *(void **)&api.find_folder_art = sa_sym(h, "nd_music_find_folder_art");
    *(void **)&api.duration = sa_sym(h, "nd_music_duration");
    *(void **)&api.format_time = sa_sym(h, "nd_music_format_time");
    *(void **)&api.truncate = sa_sym(h, "nd_music_truncate");
    *(void **)&api.play = sa_sym(h, "nd_music_play");
    *(void **)&api.stop = sa_sym(h, "nd_music_stop");
    *(void **)&api.toggle_pause = sa_sym(h, "nd_music_toggle_pause");
    *(void **)&api.is_paused = sa_sym(h, "nd_music_is_paused");
    *(void **)&api.is_finished = sa_sym(h, "nd_music_is_finished");
    *(void **)&api.position = sa_sym(h, "nd_music_position");
    *(void **)&api.child_pid = sa_sym(h, "nd_music_child_pid");
    api.no_card_help = dlsym(h, "nd_music_no_card_help");
    api.no_card_message = dlsym(h, "nd_music_no_card_message");
    api.unknown_artist = dlsym(h, "nd_music_unknown_artist");
    api.mpv_cmd = dlsym(h, "nd_music_mpv_cmd");

    *(void **)&api.lib_build = sa_sym(h, "nd_music_library_build");
    *(void **)&api.lib_free = sa_sym(h, "nd_music_library_free");
    *(void **)&api.lib_n_songs = sa_sym(h, "nd_music_library_n_songs");
    *(void **)&api.lib_n_artists = sa_sym(h, "nd_music_library_n_artists");
    *(void **)&api.lib_n_albums = sa_sym(h, "nd_music_library_n_albums");
    *(void **)&api.lib_song = sa_sym(h, "nd_music_library_song");
    *(void **)&api.lib_song_by_title = sa_sym(h, "nd_music_library_song_by_title");
    *(void **)&api.lib_artist = sa_sym(h, "nd_music_library_artist");
    *(void **)&api.lib_album = sa_sym(h, "nd_music_library_album");
    *(void **)&api.name_cmp = sa_sym(h, "nd_music_name_cmp");
    api.unknown_album = dlsym(h, "nd_music_unknown_album");

    *(void **)&api.volume = sa_sym(h, "nd_music_volume");
    *(void **)&api.set_volume = sa_sym(h, "nd_music_set_volume");
    *(void **)&api.volume_load = sa_sym(h, "nd_music_volume_load");
    *(void **)&api.gain_q15 = sa_sym(h, "nd_music_gain_q15");
    *(void **)&api.apply_gain = sa_sym(h, "nd_music_apply_gain");
    *(void **)&api.gain_buffer = sa_sym(h, "nd_music_gain_buffer");

    return api.lib_build != NULL && api.lib_free != NULL && api.lib_n_songs != NULL &&
           api.lib_n_artists != NULL && api.lib_n_albums != NULL && api.lib_song != NULL &&
           api.lib_artist != NULL && api.lib_album != NULL && api.name_cmp != NULL &&
           api.lib_song_by_title != NULL &&
           api.unknown_album != NULL &&
           api.volume != NULL && api.set_volume != NULL && api.volume_load != NULL &&
           api.gain_q15 != NULL && api.apply_gain != NULL && api.gain_buffer != NULL &&
           api.run != NULL && api.shutdown != NULL && api.pick_player != NULL &&
           api.backend_now != NULL && api.player_init != NULL && api.is_supported != NULL &&
           api.dir != NULL && api.scan != NULL && api.get_metadata != NULL &&
           api.meta_free != NULL && api.find_folder_art != NULL && api.duration != NULL &&
           api.format_time != NULL && api.truncate != NULL && api.play != NULL &&
           api.stop != NULL && api.toggle_pause != NULL && api.is_paused != NULL &&
           api.is_finished != NULL && api.position != NULL && api.child_pid != NULL &&
           api.no_card_help != NULL && api.no_card_message != NULL &&
           api.unknown_artist != NULL && api.mpv_cmd != NULL;
}

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];
static char g_bindir[ND_PATH_MAX];

#define MOUNT "/sdcard"
#define STATE "/sdcard.prop"
#define MUSIC MOUNT "/music"

/* ------------------------------------------------------------------ *
 * 1. The strings
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    CHECK_STR(*api.no_card_message, "No SD card.\nMusic is played from a card.",
              "the dialog behind app-musicplayer");
    CHECK_STR(*api.unknown_artist, "Unknown Artist", "the artist default");

    CHECK_STR(*api.no_card_help,
              "Music is played from an SD card.\n"
              "\n"
              "Format a card as FAT32, make a folder called \"music\" on it, and copy "
              "your .mp3, .flac, .wav or .ogg files into it.\n"
              "\n"
              "Put the card in the phone and your music shows up here. The phone can "
              "set a blank card up for you from Settings.",
              "NO_CARD_HELP");

    CHECK_STR(api.mpv_cmd[0], "nice", "MPV_CMD[0]");
    CHECK_STR(api.mpv_cmd[1], "-n", "MPV_CMD[1]");
    CHECK_STR(api.mpv_cmd[2], "-10", "MPV_CMD[2] -- the nice level is negative on purpose");
    CHECK_STR(api.mpv_cmd[3], "mpv", "MPV_CMD[3]");
    CHECK_STR(api.mpv_cmd[4], "--no-video", "MPV_CMD[4]");
    CHECK_STR(api.mpv_cmd[5], "--audio-buffer=4", "MPV_CMD[5]");
    CHECK_STR(api.mpv_cmd[6], "--quiet", "MPV_CMD[6]");

    /* manifest.json says 970, and the track list says 4 anyway. */
    CHECK_INT(ND_MUSIC_APP_ID, 970, "the manifest's id");
    CHECK_INT(ND_MUSIC_LIST_APP_ID, 4, "VerticalList(app_id=4) -- Settings' root id, on screen");
    CHECK_STR(ND_MUSIC_FOLDER, "music", "MUSIC_FOLDER is a folder NAME, not a path");
    CHECK_INT(ND_MUSIC_RATE, 44100, "_MiniaudioPlayer.RATE");
    CHECK_INT(ND_MUSIC_ABUF_MS_DEFAULT, 500, "NEODCT_MUSIC_ABUF_MS's default");
}

/* ------------------------------------------------------------------ *
 * 2. The extension filter
 * ------------------------------------------------------------------ */

static void test_is_supported(void)
{
    /* _MiniaudioPlayer.EXTS = (".mp3", ".wav", ".flac", ".ogg") */
    CHECK(api.is_supported("a.mp3", ND_MUSIC_BACKEND_STREAM), "stream .mp3");
    CHECK(api.is_supported("a.wav", ND_MUSIC_BACKEND_STREAM), "stream .wav");
    CHECK(api.is_supported("a.flac", ND_MUSIC_BACKEND_STREAM), "stream .flac -- see MU-4");
    CHECK(api.is_supported("a.ogg", ND_MUSIC_BACKEND_STREAM), "stream .ogg -- see MU-4");
    CHECK(!api.is_supported("a.aac", ND_MUSIC_BACKEND_STREAM), "aac is MPV-only, as in the Python");

    /* _MpvPlayer.EXTS adds ".aac" and nothing else. */
    CHECK(api.is_supported("a.aac", ND_MUSIC_BACKEND_MPV), "mpv .aac");
    CHECK(api.is_supported("a.mp3", ND_MUSIC_BACKEND_MPV), "mpv .mp3");
    CHECK(!api.is_supported("a.wma", ND_MUSIC_BACKEND_MPV), "wma is in neither list");

    /* `exts = self.player.EXTS if self.player else ()` -- with no player,
     * nothing matches at all. */
    CHECK(!api.is_supported("a.mp3", ND_MUSIC_BACKEND_NONE), "no player, no extensions");

    /* str.lower() does not consult the locale and tolower() does. */
    CHECK(api.is_supported("A.MP3", ND_MUSIC_BACKEND_STREAM), "upper case");
    CHECK(api.is_supported("A.Mp3", ND_MUSIC_BACKEND_STREAM), "mixed case");

    CHECK(!api.is_supported("mp3", ND_MUSIC_BACKEND_STREAM), "endswith(\".mp3\") needs the dot");
    CHECK(!api.is_supported("a.mp3.txt", ND_MUSIC_BACKEND_STREAM), "the suffix is the END");
    CHECK(!api.is_supported("", ND_MUSIC_BACKEND_STREAM), "an empty name");
    CHECK(!api.is_supported(NULL, ND_MUSIC_BACKEND_STREAM), "a NULL name");
    CHECK(api.is_supported(".mp3", ND_MUSIC_BACKEND_STREAM), "a file that is only a suffix");
}

/* ------------------------------------------------------------------ *
 * 3. _pick_player()
 * ------------------------------------------------------------------ */

static void test_pick_player(void)
{
    (void)unsetenv(ND_MUSIC_ENV_AUDIO);
    CHECK_INT(api.pick_player(), ND_MUSIC_BACKEND_STREAM, "unset picks the in-process path");

    (void)setenv(ND_MUSIC_ENV_AUDIO, "", 1);
    CHECK_INT(api.pick_player(), ND_MUSIC_BACKEND_STREAM, "empty is not \"subprocess\"");

    (void)setenv(ND_MUSIC_ENV_AUDIO, "subprocess", 1);
    CHECK_INT(api.pick_player(), ND_MUSIC_BACKEND_MPV, "\"subprocess\" forces mpv");

    /* `if forced != "subprocess" and HAS_MINIAUDIO` -- HAS_MINIAUDIO is
     * always true here, so "miniaudio" is not a third answer. MU-7. */
    (void)setenv(ND_MUSIC_ENV_AUDIO, "miniaudio", 1);
    CHECK_INT(api.pick_player(), ND_MUSIC_BACKEND_STREAM, "\"miniaudio\" is the same as unset");

    (void)setenv(ND_MUSIC_ENV_AUDIO, "Subprocess", 1);
    CHECK_INT(api.pick_player(), ND_MUSIC_BACKEND_STREAM, "the comparison is exact, not folded");

    (void)unsetenv(ND_MUSIC_ENV_AUDIO);
}

/* ------------------------------------------------------------------ *
 * 4. format_time()
 * ------------------------------------------------------------------ */

static void test_format_time(void)
{
    char out[32];

#define FT(secs, want)                          \
    do {                                        \
        api.format_time((secs), out, sizeof out); \
        CHECK_STR(out, (want), "format_time(" #secs ")"); \
    } while (0)

    FT(0, "00:00");
    FT(9, "00:09");
    FT(59, "00:59");
    FT(60, "01:00");
    FT(61, "01:01");
    FT(599, "09:59");
    FT(600, "10:00");
    FT(3599, "59:59");
    FT(3600, "60:00");
    /* f"{m:02d}" pads to two and does not TRUNCATE to two, so a 100-minute
     * track widens the label and the right-aligned "-MM:SS" moves left. */
    FT(6000, "100:00");
    FT(35999, "599:59");
    /* Clamped rather than reproduced: Python's // floors and C's / truncates,
     * so a negative would differ silently. No caller can reach it -- both go
     * through int(max(0, ...)) or a duration. MU-9. */
    FT(-1, "00:00");
#undef FT

    /* Must not write through a NULL or into nothing. */
    api.format_time(61, NULL, 8u);
    api.format_time(61, out, 0u);
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * 5. truncate(), against Pillow
 * ------------------------------------------------------------------ */

/* Every `want` below is what System/apps/MusicPlayer/main.py's truncate()
 * returns when run over PIL.ImageDraw.textbbox with
 * System/ui/resources/fonts/font.ttf. 116 is the real text_width on this
 * panel: max(30, 240 - 116 - 8).
 *
 * THE ORACLE MUST BE ASKED WITH layout_engine=ImageFont.Layout.BASIC, and
 * this comment exists because the first cut of these vectors was not and two
 * of them came out one character too long. A desktop Pillow with libraqm
 * available uses the COMPLEX engine, which kerns; python-pillow on the phone
 * is built with `-Craqm=disable` and never does. "Now Playing" at 14 px is
 * 105 px kerned and 108 px not, and 108 is what the panel has always drawn --
 * see nd_font.h and tools/fontref.py, which forces BASIC for the same
 * reason. The C was right and the vectors were wrong. */
static void test_truncate(void)
{
    sa_fixture fx;
    char out[ND_MUSIC_TEXT_MAX + 8];

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }

    /* Fits: returned bare, with NO ellipsis. This is the case the bare first
     * measurement exists for. */
    CHECK_STR(api.truncate(out, sizeof out, "Short", fx.font_n, 116), "Short",
              "a title that fits keeps every character and gains nothing");

    CHECK_STR(api.truncate(out, sizeof out, "A Very Long Track Title Indeed", fx.font_n, 116),
              "A Very...", "20 px title at 116 px");

    CHECK_STR(api.truncate(out, sizeof out, "Unknown Artist", fx.font_s, 116), "Unknown A...",
              "14 px artist at 116 px -- the DEFAULT artist does not fit");

    CHECK_STR(api.truncate(out, sizeof out,
                           "An Extremely Long Artist Name That Will Not Fit", fx.font_s, 116),
              "An Extrem...", "14 px artist, long");

    CHECK_STR(api.truncate(out, sizeof out, "Greatest Hits Volume Two", fx.font_s, 116),
              "Greatest Hi...", "14 px album at 116 px");

    /* `while w > max_w and len(t) > 0` -- the length guard is what ends this
     * one, and the result is WIDER than max_w. Ported, because the alternative
     * is an empty string where the Python draws three dots. */
    CHECK_STR(api.truncate(out, sizeof out, "W", fx.font_n, 1), "...",
              "an impossible width empties the string and keeps the ellipsis");

    CHECK_STR(api.truncate(out, sizeof out, "", fx.font_n, 116), "", "an empty string");

    /* t[:-1] drops a CHARACTER. A byte-wise chop would leave half of a
     * two-byte sequence, which measures differently and draws a replacement
     * glyph. Six two-byte codepoints; the width forces several chops. */
    (void)api.truncate(out, sizeof out, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9",
                       fx.font_n, 20);
    CHECK(strlen(out) >= 3u, "something came back");
    {
        size_t i;
        bool clean = true;

        for (i = 0u; i < strlen(out); i++) {
            unsigned char c = (unsigned char)out[i];

            /* No lead byte may be the last byte, and no continuation byte may
             * stand alone. */
            if ((c & 0xE0u) == 0xC0u && (i + 1u >= strlen(out) ||
                                         ((unsigned char)out[i + 1u] & 0xC0u) != 0x80u))
                clean = false;
        }
        CHECK(clean, "the chop landed on a codepoint boundary");
    }

    CHECK_STR(api.truncate(out, sizeof out, NULL, fx.font_n, 116), "", "a NULL string");
    CHECK(api.truncate(NULL, 0u, "x", fx.font_n, 116) == NULL, "no buffer");

    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 6. nd_id3.c
 * ------------------------------------------------------------------ */

/* nd_id3_text_utf8() takes a frame body with the ENCODING BYTE ALREADY
 * REMOVED, so every `in` below starts at the first byte of the value. */
static void test_id3_text(void)
{
    char out[64];

    /* 0 -- ISO-8859-1. 0xE9 is U+00E9, not a UTF-8 lead byte. */
    {
        static const uint8_t in[] = {'C', 'a', 'f', 0xE9};

        CHECK_INT(nd_id3_text_utf8(out, sizeof out, 0u, in, sizeof in), 5,
                  "latin-1 widens 0xE9 to two bytes");
        CHECK_STR(out, "Caf\xc3\xa9", "Cafe with an acute");
    }

    /* 0 -- the value stops at the first terminator. mutagen joins a
     * multi-value frame with U+0000, which a C string cannot hold, so the
     * FIRST value is what is kept. */
    {
        static const uint8_t in[] = {'A', 0x00, 'B'};

        CHECK_INT(nd_id3_text_utf8(out, sizeof out, 0u, in, sizeof in), 1, "stops at the NUL");
        CHECK_STR(out, "A", "the first value only");
    }

    /* 1 -- UTF-16 with a little-endian BOM. */
    {
        static const uint8_t in[] = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};

        CHECK_INT(nd_id3_text_utf8(out, sizeof out, 1u, in, sizeof in), 2, "UTF-16LE with a BOM");
        CHECK_STR(out, "Hi", "Hi");
    }

    /* 1 -- UTF-16 with a big-endian BOM. */
    {
        static const uint8_t in[] = {0xFE, 0xFF, 0x00, 'H', 0x00, 'i'};

        CHECK_STR(out, "Hi", "(previous)");
        (void)nd_id3_text_utf8(out, sizeof out, 1u, in, sizeof in);
        CHECK_STR(out, "Hi", "UTF-16BE with a BOM");
    }

    /* 1 -- NO BOM. The spec demands one; mutagen's codec reads it
     * little-endian rather than raising, and files in the wild rely on it. */
    {
        static const uint8_t in[] = {'H', 0x00, 'i', 0x00};

        (void)nd_id3_text_utf8(out, sizeof out, 1u, in, sizeof in);
        CHECK_STR(out, "Hi", "a missing BOM reads little-endian, as mutagen does");
    }

    /* 2 -- UTF-16BE, no BOM, and a surrogate pair. U+1F3B5 MUSICAL NOTE. */
    {
        static const uint8_t in[] = {0xD8, 0x3C, 0xDF, 0xB5};

        (void)nd_id3_text_utf8(out, sizeof out, 2u, in, sizeof in);
        CHECK_STR(out, "\xf0\x9f\x8e\xb5", "a surrogate pair becomes one 4-byte sequence");
    }

    /* 3 -- UTF-8, copied. */
    {
        static const uint8_t in[] = {0xE2, 0x99, 0xAA, 'x'};

        (void)nd_id3_text_utf8(out, sizeof out, 3u, in, sizeof in);
        CHECK_STR(out, "\xe2\x99\xaax", "UTF-8 passes through");
    }

    /* 3 -- a truncation must land on a sequence boundary, never inside one. */
    {
        static const uint8_t in[] = {0xE2, 0x99, 0xAA, 0xE2, 0x99, 0xAA};
        char small[5];

        (void)nd_id3_text_utf8(small, sizeof small, 3u, in, sizeof in);
        CHECK_STR(small, "\xe2\x99\xaa", "only the whole sequence that fits");
    }

    /* An encoding byte no version of ID3 defines writes nothing. */
    {
        static const uint8_t in[] = {'x', 'y'};

        CHECK_INT(nd_id3_text_utf8(out, sizeof out, 9u, in, sizeof in), 0, "encoding 9");
        CHECK_STR(out, "", "and leaves an empty string");
    }

    CHECK_INT(nd_id3_text_utf8(NULL, 8u, 0u, (const uint8_t *)"x", 1u), 0, "NULL out");
    CHECK_INT(nd_id3_text_utf8(out, 0u, 0u, (const uint8_t *)"x", 1u), 0, "zero capacity");
}

/* ---- building tags ------------------------------------------------ */

/* A tag is a ten-byte header and a body. `size` is syncsafe: seven bits per
 * byte, so no byte of it can be 0xFF. */
static size_t id3_header(uint8_t *out, uint8_t ver, uint8_t flags, size_t body_len)
{
    out[0] = 'I';
    out[1] = 'D';
    out[2] = '3';
    out[3] = ver;
    out[4] = 0u;
    out[5] = flags;
    out[6] = (uint8_t)((body_len >> 21) & 0x7Fu);
    out[7] = (uint8_t)((body_len >> 14) & 0x7Fu);
    out[8] = (uint8_t)((body_len >> 7) & 0x7Fu);
    out[9] = (uint8_t)(body_len & 0x7Fu);
    return ND_ID3_HEADER_LEN;
}

/* A v2.3 frame: four characters, a plain big-endian size, two flag bytes. */
static size_t id3_frame_v23(uint8_t *out, const char *id, const uint8_t *body, size_t blen)
{
    memcpy(out, id, 4u);
    out[4] = (uint8_t)((blen >> 24) & 0xFFu);
    out[5] = (uint8_t)((blen >> 16) & 0xFFu);
    out[6] = (uint8_t)((blen >> 8) & 0xFFu);
    out[7] = (uint8_t)(blen & 0xFFu);
    out[8] = 0u;
    out[9] = 0u;
    memcpy(out + 10, body, blen);
    return 10u + blen;
}

/* A v2.4 frame differs only in that the size is syncsafe. */
static size_t id3_frame_v24(uint8_t *out, const char *id, const uint8_t *body, size_t blen)
{
    memcpy(out, id, 4u);
    out[4] = (uint8_t)((blen >> 21) & 0x7Fu);
    out[5] = (uint8_t)((blen >> 14) & 0x7Fu);
    out[6] = (uint8_t)((blen >> 7) & 0x7Fu);
    out[7] = (uint8_t)(blen & 0x7Fu);
    out[8] = 0u;
    out[9] = 0u;
    memcpy(out + 10, body, blen);
    return 10u + blen;
}

/* A v2.2 frame: three characters, a 24-bit size, no flags. */
static size_t id3_frame_v22(uint8_t *out, const char *id, const uint8_t *body, size_t blen)
{
    memcpy(out, id, 3u);
    out[3] = (uint8_t)((blen >> 16) & 0xFFu);
    out[4] = (uint8_t)((blen >> 8) & 0xFFu);
    out[5] = (uint8_t)(blen & 0xFFu);
    memcpy(out + 6, body, blen);
    return 6u + blen;
}

/* One latin-1 text body: the encoding byte and then the string. */
static size_t text_body(uint8_t *out, const char *s)
{
    out[0] = 0u;
    memcpy(out + 1, s, strlen(s));
    return 1u + strlen(s);
}

/* APIC: encoding, "image/png", type, description, then the data. */
static size_t apic_body(uint8_t *out, const uint8_t *data, size_t len)
{
    size_t n = 0u;

    out[n++] = 0u; /* latin-1 description */
    memcpy(out + n, "image/png", 9u);
    n += 9u;
    out[n++] = 0u; /* the MIME terminator */
    out[n++] = 3u; /* picture type: cover (front) */
    out[n++] = 0u; /* an empty description, terminated */
    memcpy(out + n, data, len);
    return n + len;
}

typedef struct {
    size_t calls;
    size_t accept_at; /* the call that returns true; SIZE_MAX for none */
    uint8_t first[16];
    size_t first_len;
} pic_log;

static bool pic_cb(const uint8_t *data, size_t len, void *ctx)
{
    pic_log *lg = (pic_log *)ctx;

    if (lg->calls == 0u) {
        lg->first_len = (len < sizeof lg->first) ? len : sizeof lg->first;
        memcpy(lg->first, data, lg->first_len);
    }
    lg->calls++;
    return lg->calls - 1u == lg->accept_at;
}

static void test_id3_frames(void)
{
    uint8_t tag[512];
    uint8_t body[128];
    nd_id3 out;
    pic_log lg;
    size_t n;
    size_t b;

    /* ---- ID3v2.3, the version nearly every ripper writes ---- */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Blue Monday");
    n += id3_frame_v23(tag + n, "TIT2", body, b);
    b = text_body(body, "New Order");
    n += id3_frame_v23(tag + n, "TPE1", body, b);
    b = text_body(body, "Power, Corruption & Lies");
    n += id3_frame_v23(tag + n, "TALB", body, b);
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a v2.3 tag parses");
    CHECK_INT(out.version, 3, "version 3");
    CHECK_STR(out.title, "Blue Monday", "TIT2");
    CHECK_STR(out.artist, "New Order", "TPE1");
    CHECK_STR(out.album, "Power, Corruption & Lies", "TALB");
    CHECK(out.has_title && out.has_artist && out.has_album, "all three frames present");

    /* ---- ID3v2.4: the same walk with syncsafe frame sizes ---- */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Untrue");
    n += id3_frame_v24(tag + n, "TALB", body, b);
    (void)id3_header(tag, 4u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a v2.4 tag parses");
    CHECK_INT(out.version, 4, "version 4");
    CHECK_STR(out.album, "Untrue", "TALB, syncsafe size");
    /* An ABSENT frame leaves the caller's default standing; the Python's
     * `if "TIT2" in audio.tags` is exactly this flag. */
    CHECK(!out.has_title, "no TIT2 means the filename stays on screen");
    CHECK_STR(out.title, "", "and the field is empty, not garbage");

    /* ---- ID3v2.2: three-character identifiers ---- */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Windowlicker");
    n += id3_frame_v22(tag + n, "TT2", body, b);
    b = text_body(body, "Aphex Twin");
    n += id3_frame_v22(tag + n, "TP1", body, b);
    (void)id3_header(tag, 2u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a v2.2 tag parses");
    CHECK_INT(out.version, 2, "version 2");
    CHECK_STR(out.title, "Windowlicker", "TT2");
    CHECK_STR(out.artist, "Aphex Twin", "TP1");

    /* ---- a text frame with no encoding byte at all ---- */
    n = ND_ID3_HEADER_LEN;
    n += id3_frame_v23(tag + n, "TIT2", body, 0u);
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);
    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "an empty TIT2 parses");
    CHECK(out.has_title, "mutagen gives an empty string rather than dropping the frame");
    CHECK_STR(out.title, "", "so an empty line is drawn, not the filename");

    /* ---- APIC, and the callback contract ---- */
    {
        static const uint8_t png[] = {0x89, 'P', 'N', 'G', 1, 2, 3, 4};
        static const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 9, 9, 9, 9};

        n = ND_ID3_HEADER_LEN;
        b = apic_body(body, png, sizeof png);
        n += id3_frame_v23(tag + n, "APIC", body, b);
        b = apic_body(body, jpg, sizeof jpg);
        n += id3_frame_v23(tag + n, "APIC", body, b);
        b = text_body(body, "Two Pictures");
        n += id3_frame_v23(tag + n, "TIT2", body, b);
        (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

        /* Accepting the first is the Python's `break`, and the walk stops
         * there -- so TIT2, which comes AFTER, is never read. That is what
         * `for tag in audio.tags.values(): ... break` does too. */
        memset(&lg, 0, sizeof lg);
        lg.accept_at = 0u;
        CHECK_INT(nd_id3_parse(tag, n, &out, pic_cb, &lg), ND_OK, "a tag with two pictures");
        CHECK_INT(lg.calls, 1, "the first picture ends the walk");
        CHECK_INT(lg.first_len, sizeof png, "the whole payload, MIME and description stripped");
        CHECK(memcmp(lg.first, png, sizeof png) == 0, "and it is the picture's own bytes");

        /* Refusing every picture is `except Exception: meta["art"] = None`
         * with the `break` INSIDE the try -- the loop keeps running and the
         * next APIC is offered. */
        memset(&lg, 0, sizeof lg);
        lg.accept_at = (size_t)-1;
        CHECK_INT(nd_id3_parse(tag, n, &out, pic_cb, &lg), ND_OK, "no picture decodes");
        CHECK_INT(lg.calls, 2, "the second picture is tried after the first fails");
        CHECK_STR(out.title, "Two Pictures", "and the walk runs on to the text frame");

        /* on_pic NULL skips pictures without copying them anywhere. */
        CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "no callback");
        CHECK_STR(out.title, "Two Pictures", "text is still read");
    }

    /* ---- a value longer than the buffer is truncated, not overrun ---- */
    {
        static uint8_t big[ND_ID3_TEXT_MAX + 64];
        uint8_t *tagbuf = malloc(sizeof big + 64u);

        if (tagbuf == NULL) {
            CHECK(false, "allocation");
            return;
        }
        memset(big, 'A', sizeof big);
        big[0] = 0u; /* the encoding byte */
        n = ND_ID3_HEADER_LEN;
        n += id3_frame_v23(tagbuf + n, "TIT2", big, sizeof big);
        (void)id3_header(tagbuf, 3u, 0u, n - ND_ID3_HEADER_LEN);

        CHECK_INT(nd_id3_parse(tagbuf, n, &out, NULL, NULL), ND_OK, "an over-long title");
        CHECK_INT(strlen(out.title), ND_ID3_TEXT_MAX - 1u, "truncated to the buffer, terminated");
        free(tagbuf);
    }
}

/* THE HOSTILE CASES. Every length in an ID3 tag comes out of the file, the
 * file comes off a FAT32 card, and the card comes from whoever handed it to
 * the user. */
/* ------------------------------------------------------------------ *
 * 6b. The four frames the library browser needs
 * ------------------------------------------------------------------ */

/* TPE2, TRCK, TPOS and the year. None of these reach the Now Playing screen;
 * every one of them decides where a track lands in the browser, which is why
 * they are read at all. See the nd_id3.h header comment. */
static void test_id3_library_frames(void)
{
    uint8_t tag[512];
    uint8_t body[128];
    nd_id3 out;
    size_t n;
    size_t b;

    /* ---- v2.3: the spelling nearly every ripper writes ---- */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Talking Heads");
    n += id3_frame_v23(tag + n, "TPE2", body, b);
    b = text_body(body, "7/11");
    n += id3_frame_v23(tag + n, "TRCK", body, b);
    b = text_body(body, "2/2");
    n += id3_frame_v23(tag + n, "TPOS", body, b);
    b = text_body(body, "1980");
    n += id3_frame_v23(tag + n, "TYER", body, b);
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a v2.3 library tag parses");
    CHECK_STR(out.albumartist, "Talking Heads", "TPE2 is the album artist");
    CHECK(out.has_albumartist, "and it is marked present");
    CHECK_INT(out.track, 7, "TRCK \"7/11\" is track 7, not 711");
    CHECK_INT(out.disc, 2, "TPOS \"2/2\" is disc 2");
    CHECK_INT(out.year, 1980, "TYER is the year");

    /* ---- v2.4 spells the year TDRC, and it is a whole ISO timestamp ---- */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "2013-05-13T09:00:00");
    n += id3_frame_v24(tag + n, "TDRC", body, b);
    b = text_body(body, "3");
    n += id3_frame_v24(tag + n, "TRCK", body, b);
    (void)id3_header(tag, 4u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a v2.4 library tag parses");
    CHECK_INT(out.year, 2013, "TDRC yields the year out of an ISO timestamp");
    CHECK_INT(out.track, 3, "a bare TRCK with no slash");
    CHECK_INT(out.disc, 0, "an absent TPOS is zero, not one");

    /* ---- v2.2's three-letter spellings ---- */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Various Artists");
    n += id3_frame_v22(tag + n, "TP2", body, b);
    b = text_body(body, "12");
    n += id3_frame_v22(tag + n, "TRK", body, b);
    b = text_body(body, "1994");
    n += id3_frame_v22(tag + n, "TYE", body, b);
    (void)id3_header(tag, 2u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a v2.2 library tag parses");
    CHECK_STR(out.albumartist, "Various Artists", "TP2");
    CHECK_INT(out.track, 12, "TRK");
    CHECK_INT(out.year, 1994, "TYE");

    /* ---- what a corrupt or eccentric tag does ---- *
     *
     * Every one of these came off somebody's card. None may put a track in
     * the wrong place or, worse, wrap a uint16 into a plausible-looking one. */
    {
        static const struct {
            const char *raw;
            unsigned expect;
            const char *what;
        } TRACKS[] = {
            {"0", 0u, "track 0 is treated as absent"},
            {"", 0u, "an empty TRCK is absent"},
            {"/12", 0u, "a leading slash with no number is absent"},
            {"A5", 0u, "a track number that is not a number is absent"},
            {"5A", 5u, "a number with trailing junk still yields the number"},
            {"  9", 9u, "leading spaces are skipped"},
            {"9999", 9999u, "the largest allowed track"},
            {"10000", ND_ID3_NUM_MAX, "one past it clamps rather than wrapping"},
            {"65536", ND_ID3_NUM_MAX, "and a value that would wrap a uint16 clamps"},
            {"4294967296", ND_ID3_NUM_MAX, "so does one that would wrap a uint32"},
            {"-3", 0u, "a negative track number is absent, not 65533"},
        };
        size_t i;

        for (i = 0u; i < ND_ARRAY_LEN(TRACKS); i++) {
            n = ND_ID3_HEADER_LEN;
            b = text_body(body, TRACKS[i].raw);
            n += id3_frame_v23(tag + n, "TRCK", body, b);
            (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);
            (void)nd_id3_parse(tag, n, &out, NULL, NULL);
            CHECK_INT(out.track, TRACKS[i].expect, TRACKS[i].what);
        }
    }

    {
        static const struct {
            const char *raw;
            unsigned expect;
            const char *what;
        } YEARS[] = {
            {"1980", 1980u, "a plain year"},
            {"0", 0u, "year 0 is not a year"},
            {"999", 0u, "below the floor is dropped"},
            {"1000", 1000u, "the floor itself is kept"},
            {"2999", 2999u, "and the ceiling"},
            {"3000", 0u, "above the ceiling is dropped, not clamped"},
            {"43000", 0u, "a corrupt year cannot sort to the top"},
            {"nineteen eighty", 0u, "words are not a year"},
            {"", 0u, "an empty year frame is absent"},
        };
        size_t i;

        for (i = 0u; i < ND_ARRAY_LEN(YEARS); i++) {
            n = ND_ID3_HEADER_LEN;
            b = text_body(body, YEARS[i].raw);
            n += id3_frame_v23(tag + n, "TYER", body, b);
            (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);
            (void)nd_id3_parse(tag, n, &out, NULL, NULL);
            CHECK_INT(out.year, YEARS[i].expect, YEARS[i].what);
        }
    }

    /* A tag carrying only the Python's three frames must leave every new
     * field at zero -- that is what makes the browser's fallbacks fire
     * instead of it sorting on uninitialised memory. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Just A Title");
    n += id3_frame_v23(tag + n, "TIT2", body, b);
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);
    (void)nd_id3_parse(tag, n, &out, NULL, NULL);
    CHECK_INT(out.track, 0, "no TRCK");
    CHECK_INT(out.disc, 0, "no TPOS");
    CHECK_INT(out.year, 0, "no year");
    CHECK(!out.has_albumartist, "no TPE2");
    CHECK_STR(out.albumartist, "", "and the field is empty, not garbage");
}

static void test_id3_hostile(void)
{
    uint8_t tag[256];
    uint8_t body[64];
    nd_id3 out;
    size_t n;
    size_t b;

    /* No magic. */
    CHECK_INT(nd_id3_parse((const uint8_t *)"NOTATAG___", 10u, &out, NULL, NULL), ND_ERR_NOTFOUND,
              "no \"ID3\" magic");

    /* Shorter than a header. */
    CHECK_INT(nd_id3_parse((const uint8_t *)"ID3", 3u, &out, NULL, NULL), ND_ERR_NOTFOUND,
              "shorter than the ten-byte header");

    /* A version that does not exist. 2.5 was never published and 0xFF is a
     * version marker, not a version. */
    (void)id3_header(tag, 5u, 0u, 16u);
    CHECK_INT(nd_id3_parse(tag, 32u, &out, NULL, NULL), ND_ERR_NOTFOUND, "ID3v2.5");
    (void)id3_header(tag, 0xFFu, 0u, 16u);
    CHECK_INT(nd_id3_parse(tag, 32u, &out, NULL, NULL), ND_ERR_NOTFOUND, "version 0xFF");

    /* A size of zero: self-contradictory, because a tag that exists has
     * frames in it. */
    (void)id3_header(tag, 3u, 0u, 0u);
    CHECK_INT(nd_id3_parse(tag, 32u, &out, NULL, NULL), ND_ERR_PARSE, "a tag claiming zero bytes");

    /* A syncsafe size can express 256 MB. THE CAP IS CHECKED AGAINST THE
     * HEADER'S OWN FIELD, not against how much was read, so this is refused
     * before a single byte is allocated. */
    (void)id3_header(tag, 3u, 0u, ND_ID3_TAG_MAX + 1u);
    CHECK_INT(nd_id3_parse(tag, 32u, &out, NULL, NULL), ND_ERR_PARSE,
              "a tag larger than the cap is refused before allocating");
    (void)id3_header(tag, 3u, 0u, 0x0FFFFFFFu); /* the largest a syncsafe int holds */
    CHECK_INT(nd_id3_parse(tag, 32u, &out, NULL, NULL), ND_ERR_PARSE, "256 MB is refused too");

    /* A FRAME claiming more than the tag holds. The check is `claimed >
     * remaining`, never `pos + claimed > len` -- which overflows for a
     * claimed size near SIZE_MAX and then compares true when it should
     * compare false. The frames BEFORE it are still read. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Read Me");
    n += id3_frame_v23(tag + n, "TIT2", body, b);
    memcpy(tag + n, "TPE1", 4u);
    tag[n + 4u] = 0xFFu;
    tag[n + 5u] = 0xFFu;
    tag[n + 6u] = 0xFFu;
    tag[n + 7u] = 0xFFu;
    tag[n + 8u] = 0u;
    tag[n + 9u] = 0u;
    n += 10u;
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a 4 GB frame ends the walk");
    CHECK_STR(out.title, "Read Me", "and the frame before it survived");
    CHECK(!out.has_artist, "the impossible frame contributed nothing");

    /* A truncated file: the header promises more than the buffer holds. The
     * frames that ARE there still parse, which is what mutagen does. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Half A Tag");
    n += id3_frame_v23(tag + n, "TIT2", body, b);
    (void)id3_header(tag, 3u, 0u, 4096u); /* a lie */
    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a truncated tag still parses");
    CHECK_STR(out.title, "Half A Tag", "as far as it goes");

    /* A frame identifier that is not upper-case letters and digits means the
     * stream is no longer where we think it is. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Before");
    n += id3_frame_v23(tag + n, "TIT2", body, b);
    memcpy(tag + n, "\x01\x02\x03\x04", 4u);
    memset(tag + n + 4u, 0, 6u);
    n += 10u;
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);
    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a junk identifier ends the walk");
    CHECK_STR(out.title, "Before", "cleanly");

    /* v2.2's 0x40 flag means "the whole tag is compressed" and the scheme was
     * never specified. mutagen refuses the tag; so does this, rather than
     * guessing at somebody else's bytes. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Nope");
    n += id3_frame_v22(tag + n, "TT2", body, b);
    (void)id3_header(tag, 2u, 0x40u, n - ND_ID3_HEADER_LEN);
    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_ERR_UNSUPPORTED,
              "a compressed v2.2 tag is abandoned whole");

    /* v2.4 per-frame compression: RECOGNISED, then the frame is dropped.
     * Inflating an attacker's zlib stream to learn an album name is not a
     * trade this phone should make. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Kept");
    n += id3_frame_v24(tag + n, "TIT2", body, b);
    b = text_body(body, "Dropped");
    {
        size_t at = n;

        n += id3_frame_v24(tag + n, "TALB", body, b);
        tag[at + 9u] = 0x08u; /* the compression flag */
    }
    (void)id3_header(tag, 4u, 0u, n - ND_ID3_HEADER_LEN);
    CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "a compressed frame is skipped");
    CHECK_STR(out.title, "Kept", "the plain frame is read");
    CHECK(!out.has_album, "and the compressed one is not");

    /* Unsynchronisation: "FF 00" is "FF", and for 2.2/2.3 the whole tag body
     * is decoded BEFORE the frames are walked -- so a frame's declared size
     * is its size AFTER the zero bytes come out, which is how mutagen reads
     * one. Five bytes on disk, four in the size field. */
    {
        n = ND_ID3_HEADER_LEN;
        memcpy(tag + n, "TIT2", 4u);
        tag[n + 4u] = 0u;
        tag[n + 5u] = 0u;
        tag[n + 6u] = 0u;
        tag[n + 7u] = 4u; /* the DECODED length */
        tag[n + 8u] = 0u;
        tag[n + 9u] = 0u;
        tag[n + 10u] = 0u; /* latin-1 */
        tag[n + 11u] = 'A';
        tag[n + 12u] = 0xFFu;
        tag[n + 13u] = 0x00u; /* the inserted zero */
        tag[n + 14u] = 'B';
        n += 15u;
        (void)id3_header(tag, 3u, 0x80u, n - ND_ID3_HEADER_LEN);
        CHECK_INT(nd_id3_parse(tag, n, &out, NULL, NULL), ND_OK, "an unsynchronised tag");
        /* "A", U+00FF, "B" -- the 0xFF widened to two UTF-8 bytes. */
        CHECK_STR(out.title, "A\xc3\xbf" "B", "the inserted 0x00 is removed before decoding");
    }

    CHECK_INT(nd_id3_parse(NULL, 0u, &out, NULL, NULL), ND_ERR_NOTFOUND, "a NULL buffer");
    CHECK_INT(nd_id3_parse(tag, n, NULL, NULL, NULL), ND_ERR_INVAL, "a NULL result");
    CHECK_INT(nd_id3_read(NULL, &out, NULL, NULL), ND_ERR_INVAL, "a NULL path");
    CHECK_INT(nd_id3_read("/nowhere/at/all.mp3", &out, NULL, NULL), ND_ERR_IO, "a missing file");
}

/* ------------------------------------------------------------------ *
 * 11. The golden frame -- taken BEFORE any card exists
 * ------------------------------------------------------------------ */

static void test_no_card(void)
{
    char dir[ND_MUSIC_PATH_MAX];

    CHECK(!nd_storage_is_ready(), "no SD card in this test yet");
    CHECK(!api.dir(dir, sizeof dir), "music_dir() is None with no card");
    CHECK(!api.dir(NULL, 8u), "NULL out");
    CHECK(!api.dir(dir, 0u), "zero capacity");
    CHECK_INT(api.scan(NULL, 4u, ND_MUSIC_BACKEND_STREAM), 0, "scan with NULL out");
}

/* MessageDialog draws DEFAULT_WARNING_ICON, which is a real file under
 * /NeoDCT/System. This test's scratch root has no such file, so the root is
 * pointed at the overlay for the length of the capture -- exactly what
 * test_clock_app.c does for the same icon. There is still no card under it,
 * which is the state the frame is taken in. */
static bool root_to_overlay(void)
{
    char overlay[ND_PATH_MAX];

    if (!sa_overlay_root(overlay, sizeof overlay))
        return false;
    return nd_path_set_root(overlay) == ND_OK;
}

static void root_restore(void)
{
    (void)nd_path_set_root(g_root);
}

static void test_golden_frame(void)
{
    sa_fixture fx;
    int rc;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay for the warning icon");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* MessageDialog drains the channel before its first draw, so the key that
     * lets the app out has to arrive as a repeat rather than be queued. */
    if (!sa_hold(&fx, ND_KEY_ENTER)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    /* ONE frame, which is what nd-shoot allows this case too. run(ui) goes
     * from the dialog straight into the TextScroller with no key in between,
     * so the recording has to stop at the dialog -- the Python's stops there
     * because ScriptExhausted comes out of MessageDialog's first
     * read_keypress with that frame already committed. */
    nd_capture_set_budget(fx.cap, 1);

    nd_vclock_enable();
    rc = api.run(&fx.ui);
    nd_capture_clear_budget(fx.cap);

    CHECK_INT(rc, 0, "the no-card path returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "one frame committed: the dialog");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "app-musicplayer");

    nd_vclock_disable();
    sa_fx_free(&fx);
    root_restore();
}

/* ------------------------------------------------------------------ *
 * 7. The card, and scan_music()
 * ------------------------------------------------------------------ */

static bool write_text(const char *logical, const char *content)
{
    char real[ND_PATH_MAX];
    FILE *f;

    if (nd_path_resolve(real, sizeof real, logical) != ND_OK)
        return false;
    f = fopen(real, "wb");
    if (f == NULL)
        return false;
    (void)fputs(content, f);
    (void)fclose(f);
    return true;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* A canonical 44-byte PCM WAV of `frames` silent stereo frames. Written byte
 * by byte rather than by struct: the header is little-endian on every host
 * and CODING-STANDARDS.md section 6 forbids assuming this one is. */
static bool write_wav(const char *logical, uint32_t frames, uint32_t rate)
{
    uint8_t hdr[44];
    uint32_t data = frames * 2u * 2u;
    char real[ND_PATH_MAX];
    uint8_t zero[512];
    uint32_t left = data;
    FILE *f;

    memcpy(hdr, "RIFF", 4u);
    put_le32(hdr + 4, 36u + data);
    memcpy(hdr + 8, "WAVEfmt ", 8u);
    put_le32(hdr + 16, 16u);          /* the fmt chunk's length */
    put_le16(hdr + 20, 1u);           /* PCM */
    put_le16(hdr + 22, 2u);           /* stereo */
    put_le32(hdr + 24, rate);
    put_le32(hdr + 28, rate * 4u);    /* bytes per second */
    put_le16(hdr + 32, 4u);           /* block align */
    put_le16(hdr + 34, 16u);          /* bits per sample */
    memcpy(hdr + 36, "data", 4u);
    put_le32(hdr + 40, data);

    if (nd_path_resolve(real, sizeof real, logical) != ND_OK)
        return false;
    f = fopen(real, "wb");
    if (f == NULL)
        return false;
    memset(zero, 0, sizeof zero);
    if (fwrite(hdr, 1u, sizeof hdr, f) != sizeof hdr) {
        (void)fclose(f);
        return false;
    }
    while (left > 0u) {
        size_t chunk = (left > sizeof zero) ? sizeof zero : (size_t)left;

        if (fwrite(zero, 1u, chunk, f) != chunk) {
            (void)fclose(f);
            return false;
        }
        left -= (uint32_t)chunk;
    }
    (void)fclose(f);
    return true;
}

static void build_card(void)
{
    static const char *const FOLDERS[5] = {"wallpapers", "tones", "backup_db", "music", "update"};
    size_t i;

    nd_storage_set_paths(MOUNT, STATE);
    CHECK(write_text(STATE, "state=mounted\ndevice=/dev/vdc\nfstype=vfat\nlabel=NEODCT\n"),
          "the card state file");
    for (i = 0u; i < 5u; i++) {
        char path[ND_PATH_MAX];

        CHECK_INT(nd_snprintf(path, sizeof path, "%s/%s", MOUNT, FOLDERS[i]), ND_OK, "folder path");
        CHECK_INT(nd_mkdir_p(path, 0755u), ND_OK, "a card folder");
    }
    CHECK(nd_storage_is_ready(), "the card is now ready");

    /* One directory of tracks and one subdirectory under it. The
     * subdirectory's file is named so that a GLOBAL sort would put it first
     * and the Python's per-directory sort puts it last -- that difference is
     * the whole claim. */
    CHECK_INT(nd_mkdir_p(MUSIC "/sub/deep", 0755u), ND_OK, "a nested music directory");
    CHECK(write_text(MUSIC "/b.mp3", "x"), "b.mp3");
    CHECK(write_text(MUSIC "/a.MP3", "x"), "a.MP3");
    CHECK(write_text(MUSIC "/c.txt", "x"), "c.txt");
    CHECK(write_text(MUSIC "/d.wav", "x"), "d.wav");
    CHECK(write_text(MUSIC "/e.flac", "x"), "e.flac");
    CHECK(write_text(MUSIC "/f.aac", "x"), "f.aac");
    CHECK(write_text(MUSIC "/sub/0-first.mp3", "x"), "sub/0-first.mp3");
    CHECK(write_text(MUSIC "/sub/z.mp3", "x"), "sub/z.mp3");
    CHECK(write_text(MUSIC "/sub/deep/m.mp3", "x"), "sub/deep/m.mp3");
}

static void test_music_dir(void)
{
    char dir[ND_MUSIC_PATH_MAX];

    CHECK(api.dir(dir, sizeof dir), "music_dir() answers once a card is mounted");
    CHECK_STR(dir, MUSIC, "Storage.folder(\"music\")");
}

static void test_scan(void)
{
    nd_music_track *t = calloc((size_t)ND_MUSIC_MAX, sizeof *t);
    size_t n;

    if (t == NULL) {
        CHECK(false, "allocation");
        return;
    }

    n = api.scan(t, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_STREAM);

    /* c.txt is not audio and f.aac is mpv-only. Seven remain: four beside the
     * music folder, two in sub/ and one in sub/deep/. */
    CHECK_INT(n, 7, "seven streamable tracks");
    if (n == 7u) {
        /* The top directory's own files, sorted by BYTE value... */
        CHECK_STR(t[0].path, MUSIC "/a.MP3", "a.MP3");
        CHECK_STR(t[1].path, MUSIC "/b.mp3", "b.mp3");
        CHECK_STR(t[2].path, MUSIC "/d.wav", "d.wav");
        CHECK_STR(t[3].path, MUSIC "/e.flac", "e.flac -- listed even though dr_* cannot read it");
        /* ...and only THEN the subdirectory, whose "0-first.mp3" would sort
         * before every one of them under a global sort. os.walk is
         * depth-first and sorted() is applied per directory. */
        CHECK_STR(t[4].path, MUSIC "/sub/0-first.mp3",
                  "a subdirectory's track sorts AFTER its parent's, whatever its name");
        CHECK_STR(t[5].path, MUSIC "/sub/z.mp3", "and its own directory is sorted");
        /* os.walk is depth-first: sub/ is finished before sub/deep/ is
         * opened, so the deeper track comes last however it is named. */
        CHECK_STR(t[6].path, MUSIC "/sub/deep/m.mp3", "the walk descends more than one level");
    }

    /* MPV's list adds .aac and nothing else. */
    n = api.scan(t, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_MPV);
    CHECK_INT(n, 8, "the mpv list also takes f.aac");

    /* `exts = self.player.EXTS if self.player else ()` -- no player, no
     * playlist, and the app draws "No Music Found". */
    CHECK_INT(api.scan(t, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_NONE), 0,
              "with no player nothing is listed");

    /* The cap is this port's, not the Python's, and it is honoured rather
     * than overrun. ND_MUSIC_MAX itself would need 257 files to reach. */
    CHECK_INT(api.scan(t, 3u, ND_MUSIC_BACKEND_STREAM), 3, "a cap of three stops at three");
    CHECK_STR(t[0].path, MUSIC "/a.MP3", "and keeps the first three in walk order");
    CHECK_INT(api.scan(t, 0u, ND_MUSIC_BACKEND_STREAM), 0, "zero capacity");

    free(t);
}

/* ------------------------------------------------------------------ *
 * 8. find_folder_art()
 * ------------------------------------------------------------------ */

static bool write_png(const char *logical, uint8_t r, uint8_t g, uint8_t b)
{
    nd_image *img = nd_image_new_filled(4, 4, ND_PIXFMT_RGB888, ND_RGB(r, g, b));
    nd_err rc;

    if (img == NULL)
        return false;
    rc = nd_image_save_png(img, logical);
    nd_image_free(img);
    return rc == ND_OK;
}

static void test_folder_art(void)
{
    nd_image *art;

    /* Nothing there yet. */
    art = api.find_folder_art(MUSIC "/a.MP3");
    CHECK(art == NULL, "no sidecar art, no image");
    nd_image_free(art);

    /* "albumart" is LAST in the stem list; "cover" is first. Both exist, so
     * the nested loop's order is what decides -- and it is name-major,
     * extension-minor. The names are written in the WRONG case on purpose:
     * `{e.lower(): e for e in os.listdir(folder)}` matches case-insensitively
     * against the real entry. */
    CHECK(write_png(MUSIC "/AlbumArt.PNG", 10u, 20u, 30u), "AlbumArt.PNG");
    art = api.find_folder_art(MUSIC "/a.MP3");
    CHECK(art != NULL, "albumart.png is found through a case-folded match");
    if (art != NULL) {
        nd_color c = nd_image_get_px(art, 0, 0);

        CHECK_INT(c.r, 10, "and it is that file");
    }
    nd_image_free(art);

    CHECK(write_png(MUSIC "/Cover.jpeg", 40u, 50u, 60u), "Cover.jpeg");
    art = api.find_folder_art(MUSIC "/a.MP3");
    CHECK(art != NULL, "cover.* beats albumart.*");
    if (art != NULL) {
        nd_color c = nd_image_get_px(art, 0, 0);

        /* .jpeg is second in the extension list and albumart is fifth in the
         * stem list; the loop is `for name: for ext:`, so the STEM wins. */
        CHECK_INT(c.r, 40, "the stem is the outer loop");
    }
    nd_image_free(art);

    CHECK(write_png(MUSIC "/cover.jpg", 70u, 80u, 90u), "cover.jpg");
    art = api.find_folder_art(MUSIC "/a.MP3");
    CHECK(art != NULL, "cover.jpg beats cover.jpeg");
    if (art != NULL) {
        nd_color c = nd_image_get_px(art, 0, 0);

        CHECK_INT(c.r, 70, ".jpg is first in the extension list");
    }
    nd_image_free(art);

    /* A candidate that exists but does not decode does NOT end the search --
     * the Python's `try: ... except: pass` is inside the loop. */
    CHECK(write_text(MUSIC "/front.png", "I am not a PNG"), "a corrupt front.png");
    art = api.find_folder_art(MUSIC "/a.MP3");
    CHECK(art != NULL, "a corrupt candidate does not end the search");
    nd_image_free(art);

    /* os.path.dirname("x.mp3") is "", and listdir("") raises. */
    CHECK(api.find_folder_art("x.mp3") == NULL, "a bare filename has no folder");
    CHECK(api.find_folder_art(NULL) == NULL, "a NULL path");
}

/* ------------------------------------------------------------------ *
 * 9. get_metadata(), and 10. duration
 * ------------------------------------------------------------------ */

static void test_metadata(void)
{
    nd_music_meta m;
    uint8_t tag[256];
    uint8_t body[64];
    char real[ND_PATH_MAX];
    size_t n;
    size_t b;
    FILE *f;

    /* The defaults, on a file with no tag and no duration. */
    api.get_metadata(MUSIC "/b.mp3", &m);
    CHECK_STR(m.title, "b.mp3", "the title defaults to the basename");
    CHECK_STR(m.artist, "Unknown Artist", "the artist default");
    CHECK_STR(m.album, "", "the album default is empty, so the third line is not drawn");
    CHECK_DBL(m.length, 0.0, "nothing readable, no length");
    /* cover.jpg is in this directory from test_folder_art, so the sidecar
     * fallback fires -- which is the `if meta["art"] is None` branch. */
    CHECK(m.art != NULL, "sidecar art stands in for a missing APIC");
    api.meta_free(&m);
    CHECK(m.art == NULL, "meta_free clears the pointer it released");

    /* A file with a real ID3v2.3 tag in front of it. */
    n = ND_ID3_HEADER_LEN;
    b = text_body(body, "Teardrop");
    n += id3_frame_v23(tag + n, "TIT2", body, b);
    b = text_body(body, "Massive Attack");
    n += id3_frame_v23(tag + n, "TPE1", body, b);
    b = text_body(body, "Mezzanine");
    n += id3_frame_v23(tag + n, "TALB", body, b);
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

    CHECK_INT(nd_path_resolve(real, sizeof real, MUSIC "/tagged.mp3"), ND_OK, "resolve");
    f = fopen(real, "wb");
    if (f == NULL) {
        CHECK(false, "tagged.mp3");
        return;
    }
    CHECK_INT(fwrite(tag, 1u, n, f), (long long)n, "the tag was written");
    (void)fclose(f);

    api.get_metadata(MUSIC "/tagged.mp3", &m);
    CHECK_STR(m.title, "Teardrop", "TIT2 replaces the basename");
    CHECK_STR(m.artist, "Massive Attack", "TPE1 replaces \"Unknown Artist\"");
    CHECK_STR(m.album, "Mezzanine", "TALB, so the third line IS drawn");
    api.meta_free(&m);

    /* A file that is not there at all: the Python's `except Exception:
     * print(...)` leaves every default standing rather than failing the
     * screen, and so does this. */
    api.get_metadata(MUSIC "/gone.mp3", &m);
    CHECK_STR(m.title, "gone.mp3", "a missing file keeps the defaults");
    CHECK_STR(m.artist, "Unknown Artist", "and the artist default");
    api.meta_free(&m);

    /* Nothing may be written through a NULL. */
    api.get_metadata(NULL, &m);
    api.get_metadata(MUSIC "/b.mp3", NULL);
    api.meta_free(NULL);
    sa_checks++;
}

static void test_duration(void)
{
    nd_music_meta m;

    /* 4410 frames at 44100 Hz is exactly a tenth of a second, and the
     * arithmetic is frames / rate with no rounding anywhere in it. */
    CHECK(write_wav(MUSIC "/tone.wav", 4410u, 44100u), "a 0.1 s WAV");
    CHECK_DBL(api.duration(MUSIC "/tone.wav"), 0.1, "duration comes out of the header");

    /* 22050 Hz proves the rate is read rather than assumed: the same frame
     * count is twice as long. */
    CHECK(write_wav(MUSIC "/slow.wav", 4410u, 22050u), "a 0.2 s WAV");
    CHECK_DBL(api.duration(MUSIC "/slow.wav"), 0.2, "the file's own rate is used");

    /* miniaudio.get_file_info() raises for anything it cannot decode and the
     * Python swallows it; 0 is the same answer. */
    CHECK_DBL(api.duration(MUSIC "/c.txt"), 0.0, "not audio, no duration");
    CHECK_DBL(api.duration(MUSIC "/gone.wav"), 0.0, "a missing file");
    CHECK_DBL(api.duration(NULL), 0.0, "a NULL path");
    CHECK_DBL(api.duration(""), 0.0, "an empty path");

    /* And it reaches the screen through get_metadata, which is where
     * `if not meta["length"]` falls through to it. */
    api.get_metadata(MUSIC "/tone.wav", &m);
    CHECK_DBL(m.length, 0.1, "get_metadata picks the duration up");
    CHECK_STR(m.title, "tone.wav", "a WAV has no ID3 tag, so the basename stands");
    api.meta_free(&m);
}

/* ------------------------------------------------------------------ *
 * 10. Playback
 * ------------------------------------------------------------------ */

/* Stub players on $PATH, so nothing needs a sound card and nothing is played.
 * `aplay` is what the in-process path feeds; `nice` is MPV_CMD's argv[0] and
 * therefore the only program the mpv path has to find. Both block, so a
 * started child stays started until it is stopped. */
/* A stub that RECORDS its argv before sleeping, so a test can assert on what
 * was actually passed to the player. Written with printf rather than "$@" so
 * that one argument per line survives arguments containing spaces. */
static bool make_recording_stub(const char *name, const char *logfile)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/%s", g_bindir, name) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fprintf(f, "#!/bin/sh\nfor a in \"$@\"; do printf '%%s\\n' \"$a\"; done > '%s'\n"
                     "exec sleep 30\n",
                  logfile);
    (void)fclose(f);
    return chmod(path, 0755) == 0;
}

/* `nice -n -10 mpv ...` means the process that is actually exec'd is NICE,
 * and the plain sleeping stub above never reaches mpv at all. This one does
 * what nice does: drop its own two arguments and exec the rest. */
static bool make_passthrough_stub(const char *name)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/%s", g_bindir, name) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fputs("#!/bin/sh\nshift 2\nexec \"$@\"\n", f);
    (void)fclose(f);
    return chmod(path, 0755) == 0;
}

/* True when `needle` is one of the recorded argv entries, exactly. */
static bool stub_argv_has(const char *logfile, const char *needle)
{
    char line[512];
    FILE *f = fopen(logfile, "r");
    bool found = false;

    if (f == NULL)
        return false;
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);

        while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r'))
            line[--n] = '\0';
        if (strcmp(line, needle) == 0) {
            found = true;
            break;
        }
    }
    (void)fclose(f);
    return found;
}

static bool make_stub(const char *name)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(path, sizeof path, "%s/%s", g_bindir, name) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fputs("#!/bin/sh\nexec sleep 30\n", f);
    (void)fclose(f);
    return chmod(path, 0755) == 0;
}

static void test_playback(void)
{
    char keep[ND_PATH_MAX];
    const char *saved = getenv("PATH");
    pid_t first;
    double pos = -1.0;

    (void)nd_strlcpy(keep, (saved != NULL) ? saved : "", sizeof keep);
    if (!make_stub("aplay") || !make_stub("nice")) {
        CHECK(false, "stub players");
        return;
    }
    (void)setenv("PATH", g_bindir, 1);

    /* `if self.player is None: return False`. */
    api.player_init(ND_MUSIC_BACKEND_NONE);
    CHECK(!api.play(MUSIC "/tone.wav"), "with no player, play_file returns False");
    CHECK_INT(api.child_pid(), -1, "and nothing was spawned");

    /* ---- the in-process path ---- */
    api.player_init(ND_MUSIC_BACKEND_STREAM);
    CHECK_INT(api.backend_now(), ND_MUSIC_BACKEND_STREAM, "the session is in-process");
    CHECK(api.is_finished(), "nothing is playing yet");
    CHECK(!api.play(NULL), "a NULL path plays nothing");
    CHECK(!api.play(""), "an empty path plays nothing");

    CHECK(api.play(MUSIC "/tone.wav"), "a WAV plays through the in-process decoder");
    first = api.child_pid();
    CHECK(first > 0, "a player process was started");
    CHECK(!api.is_paused(), "and it is not paused");
    /* The Now Playing screen tests this before its first draw, and the queue
     * added in this revision treats it as "go to the next track" -- so an
     * is_finished() that is true the instant a track starts would skip the
     * whole album without drawing a frame. */
    CHECK(!api.is_finished(), "and it is NOT finished the instant it starts");
    CHECK_INT(api.backend_now(), ND_MUSIC_BACKEND_STREAM, "still in-process");

    /* _MiniaudioPlayer.position() is a real decoded-frame position, so the
     * progress bar tracks the music rather than the wall clock. */
    CHECK(api.position(&pos), "the in-process path reports a position");
    CHECK(pos >= 0.0, "and it is not negative");

    /* SIGSTOP/SIGCONT, which for aplay is the equivalent of miniaudio's
     * device.stop()/device.start(): a stopped player stops draining the
     * socket, the socket fills, and the feeder blocks with its decode
     * position intact. */
    api.toggle_pause();
    CHECK(api.is_paused(), "toggle_pause pauses");
    api.toggle_pause();
    CHECK(!api.is_paused(), "and resumes");

    /* "self.stop()" is the first line of play(): a second track replaces the
     * first rather than playing over it. */
    CHECK(api.play(MUSIC "/slow.wav"), "a second track");
    CHECK(api.child_pid() > 0, "started");
    CHECK(api.child_pid() != first, "and it is a different process");

    api.stop();
    CHECK_INT(api.child_pid(), -1, "stop reaps it");
    CHECK(api.is_finished(), "and nothing is playing");
    CHECK(!api.position(&pos), "with nothing playing there is no position");

    /* ---- the per-track fallback ---- *
     *
     * miniaudio decodes .flac; dr_mp3 and dr_wav do not. THIS ONE TRACK goes
     * to mpv and the session stays in-process, so one ogg on the card does
     * not put every later mp3 behind a 24 MB process. AUDIO.md, MU-5. */
    CHECK(api.play(MUSIC "/e.flac"), "an unreadable format falls through to mpv");
    CHECK(api.child_pid() > 0, "mpv was started for it");
    api.stop();
    CHECK_INT(api.backend_now(), ND_MUSIC_BACKEND_STREAM,
              "and the SESSION is still in-process afterwards");

    /* ---- mpv for the whole session ---- */
    api.player_init(ND_MUSIC_BACKEND_MPV);
    CHECK_INT(api.backend_now(), ND_MUSIC_BACKEND_MPV, "the session is external");
    CHECK(api.play(MUSIC "/tone.wav"), "mpv plays it");
    CHECK(api.child_pid() > 0, "a process was started");
    /* _MpvPlayer.position() returns None, which is what makes the screen fall
     * back to its wall clock. */
    CHECK(!api.position(&pos), "mpv reports no position");
    api.toggle_pause();
    CHECK(api.is_paused(), "SIGSTOP, as the Python sends");
    api.toggle_pause();
    CHECK(!api.is_paused(), "SIGCONT");

    /* app_shutdown() is what runs when the modem thread signals an incoming
     * call. If it did not release the sound card the phone would ring
     * silently -- nd_app.h's whole reason for making the symbol mandatory. */
    api.shutdown();
    CHECK_INT(api.child_pid(), -1, "app_shutdown() releases the sound card");

    /* Stopping nothing, twice, is not an error. */
    api.stop();
    api.stop();
    api.toggle_pause();
    CHECK(!api.is_paused(), "toggling a pause with nothing playing does nothing");

    api.player_init(ND_MUSIC_BACKEND_NONE);
    (void)setenv("PATH", keep, 1);
}

/* ------------------------------------------------------------------ *
 * 12. Volume -- NOT a port; see the music.h header comment
 * ------------------------------------------------------------------ */

/* The ladder, computed here rather than copied from the implementation.
 * 3 dB a step means each level is 10^(-3/20) of the one above it, and the
 * expectation is that arithmetic done in double and rounded -- so a table
 * that drifted, or a curve that quietly became linear, fails here. */
static int32_t expected_gain_q15(int32_t level)
{
    double db;

    if (level <= 0)
        return 0;
    if (level >= ND_MUSIC_VOLUME_MAX)
        return 32768;
    db = -3.0 * (double)(ND_MUSIC_VOLUME_MAX - level);
    return (int32_t)(32768.0 * pow(10.0, db / 20.0) + 0.5);
}

static void test_gain_ladder(void)
{
    int32_t level;

    CHECK_INT(api.gain_q15(ND_MUSIC_VOLUME_MAX), 32768, "level 10 is unity");
    CHECK_INT(api.gain_q15(0), 0, "level 0 is silence");

    for (level = 0; level <= ND_MUSIC_VOLUME_MAX; level++) {
        char what[64];

        (void)nd_snprintf(what, sizeof what, "level %d is 3 dB below level %d", level, level + 1);
        CHECK_INT(api.gain_q15(level), expected_gain_q15(level), what);
    }

    /* Monotonic, which a table typo would break without changing either end. */
    for (level = 1; level <= ND_MUSIC_VOLUME_MAX; level++)
        CHECK(api.gain_q15(level) > api.gain_q15(level - 1), "the ladder rises at every step");

    /* Out of range clamps rather than indexing off the table. */
    CHECK_INT(api.gain_q15(-1), 0, "below zero is silence");
    CHECK_INT(api.gain_q15(-1000), 0, "far below zero is still silence");
    CHECK_INT(api.gain_q15(ND_MUSIC_VOLUME_MAX + 1), 32768, "above the top is unity");
    CHECK_INT(api.gain_q15(1000000), 32768, "far above the top is still unity");
}

static void test_apply_gain(void)
{
    CHECK_INT(api.apply_gain(1000, ND_MUSIC_VOLUME_MAX), 1000, "unity passes a sample through");
    CHECK_INT(api.apply_gain(-1000, ND_MUSIC_VOLUME_MAX), -1000, "including a negative one");
    CHECK_INT(api.apply_gain(1000, 0), 0, "silence is silence");
    CHECK_INT(api.apply_gain(-1000, 0), 0, "for a negative sample too");

    /* The extremes of the sample range, at unity: the whole point of the
     * saturating cast is that neither wraps. */
    CHECK_INT(api.apply_gain(32767, ND_MUSIC_VOLUME_MAX), 32767, "the largest sample survives");
    CHECK_INT(api.apply_gain(-32768, ND_MUSIC_VOLUME_MAX), -32768, "and the smallest");

    /* Halfway down the ladder the sample is scaled by the table, and the
     * arithmetic is (sample * gain) >> 15 with the sign kept. */
    {
        int32_t g = api.gain_q15(5);

        CHECK_INT(api.apply_gain(20000, 5), (int16_t)((20000 * g) / 32768),
                  "a positive sample is scaled by the table");
        CHECK_INT(api.apply_gain(-20000, 5), (int16_t)((-20000 * g) / 32768),
                  "and a negative sample scales symmetrically");
    }

    /* A gain of at most unity cannot amplify, so nothing can ever clip. */
    {
        int32_t level;

        for (level = 0; level <= ND_MUSIC_VOLUME_MAX; level++) {
            CHECK(api.apply_gain(32767, level) <= 32767, "no positive overflow at any level");
            CHECK(api.apply_gain(-32768, level) >= -32768, "no negative overflow at any level");
        }
    }
}

static void test_gain_buffer(void)
{
    int16_t buf[8];
    int16_t original[8];
    size_t i;
    static const int16_t SEED[8] = {0, 1, -1, 100, -100, 32767, -32768, 12345};

    memcpy(original, SEED, sizeof SEED);

    /* Unity is a no-op, and it is a no-op WITHOUT touching the buffer -- that
     * is the fast path the common case takes. */
    memcpy(buf, SEED, sizeof SEED);
    api.gain_buffer(buf, 8u, ND_MUSIC_VOLUME_MAX);
    CHECK_INT(memcmp(buf, original, sizeof buf), 0, "unity leaves the buffer alone");

    /* Zero silences every sample. */
    memcpy(buf, SEED, sizeof SEED);
    api.gain_buffer(buf, 8u, 0);
    for (i = 0u; i < 8u; i++)
        CHECK_INT(buf[i], 0, "level 0 silences every sample");

    /* An intermediate level agrees with apply_gain(), sample for sample --
     * which is what stops the buffer path and the single-sample path from
     * drifting apart. */
    memcpy(buf, SEED, sizeof SEED);
    api.gain_buffer(buf, 8u, 4);
    for (i = 0u; i < 8u; i++)
        CHECK_INT(buf[i], api.apply_gain(SEED[i], 4), "the buffer path matches apply_gain");

    /* Bounds: n_samples of 0, and a NULL buffer, must both be safe. */
    memcpy(buf, SEED, sizeof SEED);
    api.gain_buffer(buf, 0u, 0);
    CHECK_INT(memcmp(buf, original, sizeof buf), 0, "zero samples touches nothing");
    api.gain_buffer(NULL, 8u, 4);
    sa_checks++; /* reaching here without faulting is the claim */

    /* It must scale only the range it was given. */
    memcpy(buf, SEED, sizeof SEED);
    api.gain_buffer(buf, 4u, 0);
    for (i = 0u; i < 4u; i++)
        CHECK_INT(buf[i], 0, "the first four are silenced");
    for (i = 4u; i < 8u; i++)
        CHECK_INT(buf[i], SEED[i], "and the rest are untouched");
}

static void test_volume_state(void)
{
    char saved[32];

    (void)nd_strlcpy(saved, "", sizeof saved);

    api.set_volume(5);
    CHECK_INT(api.volume(), 5, "set_volume takes");

    api.set_volume(0);
    CHECK_INT(api.volume(), 0, "zero is a legal level");
    api.set_volume(ND_MUSIC_VOLUME_MAX);
    CHECK_INT(api.volume(), ND_MUSIC_VOLUME_MAX, "so is the top");

    /* Clamped, not wrapped and not rejected: the UI steps this with a key
     * and holding the key at either end must simply stop. */
    api.set_volume(-1);
    CHECK_INT(api.volume(), 0, "below zero clamps to zero");
    api.set_volume(ND_MUSIC_VOLUME_MAX + 5);
    CHECK_INT(api.volume(), ND_MUSIC_VOLUME_MAX, "above the top clamps to the top");

    /* It survives a reload, because it is written to settings.prop. */
    api.set_volume(3);
    api.volume_load();
    CHECK_INT(api.volume(), 3, "the level persists across a reload");

    /* An absent setting gives the default rather than silence -- a phone
     * whose first launch plays nothing looks broken. */
    (void)nd_settings_set(ND_MUSIC_VOLUME_SETTING, "");
    api.volume_load();
    CHECK_INT(api.volume(), ND_MUSIC_VOLUME_DEFAULT, "an empty setting gives the default");

    /* AND LOADING DOES NOT WRITE IT BACK.
     *
     * app_run() loads the level before it knows whether there is a card, so a
     * load that persisted would rewrite the whole of settings.prop -- temp
     * file, fsync, rename, on the phone's only writable flash -- every time
     * the app was opened, and would leave a trace in it merely for having
     * been opened. It also, concretely, wrote into the repo's own
     * overlay/NeoDCT/User/settings.prop when test_golden_frame pointed the
     * path root at the real overlay to find the warning icon. */
    CHECK_STR(nd_settings_get(ND_MUSIC_VOLUME_SETTING, "ABSENT"), "",
              "loading the default leaves the stored value alone");

    (void)nd_settings_set(ND_MUSIC_VOLUME_SETTING, "not-a-number");
    api.volume_load();
    CHECK_INT(api.volume(), ND_MUSIC_VOLUME_DEFAULT, "so does a value that is not a number");

    (void)nd_settings_set(ND_MUSIC_VOLUME_SETTING, "99");
    api.volume_load();
    CHECK_INT(api.volume(), ND_MUSIC_VOLUME_MAX, "an out-of-range setting clamps");

    api.set_volume(ND_MUSIC_VOLUME_DEFAULT);
}

/* ------------------------------------------------------------------ *
 * 13. The level reaches the player
 * ------------------------------------------------------------------ */

/* The in-process path scales samples itself and is covered by the three
 * tests above. mpv decodes in another process, so the ONLY way the level can
 * reach it is its argv -- and that is what this checks, against a stub that
 * records what it was given.
 *
 * The asymmetry is real and is documented in music.h: an mpv track is told
 * its level once, when it starts. */
static void test_mpv_gets_the_volume(void)
{
    char keep[ND_PATH_MAX];
    char logfile[ND_PATH_MAX];
    const char *saved = getenv("PATH");

    (void)nd_strlcpy(keep, (saved != NULL) ? saved : "", sizeof keep);
    if (nd_snprintf(logfile, sizeof logfile, "%s/mpv-argv.txt", g_bindir) != ND_OK) {
        CHECK(false, "log path");
        return;
    }
    if (!make_recording_stub("mpv", logfile) || !make_passthrough_stub("nice")) {
        CHECK(false, "recording stub");
        return;
    }
    (void)setenv("PATH", g_bindir, 1);

    api.player_init(ND_MUSIC_BACKEND_MPV);

    api.set_volume(4);
    CHECK(api.play(MUSIC "/tone.wav"), "mpv plays a track");
    /* The stub writes the file before exec'ing sleep, and play() has already
     * waited for the spawn, but the write is in the CHILD -- give it a
     * moment rather than racing it. */
    {
        int tries = 0;

        while (tries < 200 && !stub_argv_has(logfile, "--volume=40")) {
            struct timespec ts = {0, 10 * 1000 * 1000};

            (void)nanosleep(&ts, NULL);
            tries++;
        }
    }
    CHECK(stub_argv_has(logfile, "--volume=40"), "level 4 reaches mpv as --volume=40");
    CHECK(stub_argv_has(logfile, "--no-video"), "and the Python's own arguments survive");
    api.stop();

    /* Level 10 is 100, not 130: mpv's --volume is a percentage of unity and
     * this player never amplifies. */
    api.set_volume(ND_MUSIC_VOLUME_MAX);
    CHECK(api.play(MUSIC "/tone.wav"), "mpv plays again");
    {
        int tries = 0;

        while (tries < 200 && !stub_argv_has(logfile, "--volume=100")) {
            struct timespec ts = {0, 10 * 1000 * 1000};

            (void)nanosleep(&ts, NULL);
            tries++;
        }
    }
    CHECK(stub_argv_has(logfile, "--volume=100"), "the top of the ladder is --volume=100");
    api.stop();

    api.set_volume(0);
    CHECK(api.play(MUSIC "/tone.wav"), "and at zero");
    {
        int tries = 0;

        while (tries < 200 && !stub_argv_has(logfile, "--volume=0")) {
            struct timespec ts = {0, 10 * 1000 * 1000};

            (void)nanosleep(&ts, NULL);
            tries++;
        }
    }
    CHECK(stub_argv_has(logfile, "--volume=0"), "silence is --volume=0");
    api.stop();

    api.set_volume(ND_MUSIC_VOLUME_DEFAULT);
    api.player_init(ND_MUSIC_BACKEND_NONE);
    (void)setenv("PATH", keep, 1);
}


/* ------------------------------------------------------------------ *
 * 14. The library -- NOT a port; see the music.h header comment
 * ------------------------------------------------------------------ */

/* Writes a real MP3-ish file: an ID3v2.3 tag followed by enough bytes that
 * the scan sees a file. The library reads ONLY the tag, so the audio after
 * it is never touched -- which is the point being relied on, and is why
 * these fixtures can be four bytes of nothing. */
static bool write_tagged(const char *logical, const char *title, const char *artist,
                         const char *albumartist, const char *album, const char *trck,
                         const char *year)
{
    uint8_t tag[1024];
    uint8_t body[256];
    char real[ND_PATH_MAX];
    size_t n = ND_ID3_HEADER_LEN;
    size_t b;
    FILE *f;

    if (title != NULL) {
        b = text_body(body, title);
        n += id3_frame_v23(tag + n, "TIT2", body, b);
    }
    if (artist != NULL) {
        b = text_body(body, artist);
        n += id3_frame_v23(tag + n, "TPE1", body, b);
    }
    if (albumartist != NULL) {
        b = text_body(body, albumartist);
        n += id3_frame_v23(tag + n, "TPE2", body, b);
    }
    if (album != NULL) {
        b = text_body(body, album);
        n += id3_frame_v23(tag + n, "TALB", body, b);
    }
    if (trck != NULL) {
        b = text_body(body, trck);
        n += id3_frame_v23(tag + n, "TRCK", body, b);
    }
    if (year != NULL) {
        b = text_body(body, year);
        n += id3_frame_v23(tag + n, "TYER", body, b);
    }
    (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

    if (nd_path_resolve(real, sizeof real, logical) != ND_OK)
        return false;
    f = fopen(real, "wb");
    if (f == NULL)
        return false;
    (void)fwrite(tag, 1u, n, f);
    (void)fputs("audio-goes-here", f);
    (void)fclose(f);
    return true;
}

#define LIBDIR MUSIC "/lib"

/* A deliberately awkward card:
 *
 *   - two albums by one artist, out of chronological order on disk
 *   - track numbers that disagree with filenames, which is the whole
 *     complaint the browser answers
 *   - a compilation whose four tracks have four different TPE1s and one
 *     shared TPE2, which must collapse to ONE artist
 *   - two different artists with an album of the SAME NAME
 *   - a file with no tag at all
 *   - a two-disc set
 */
static void build_library_card(void)
{
    CHECK_INT(nd_mkdir_p(LIBDIR, 0755u), ND_OK, "the library fixture directory");

    /* Portishead, two albums, filenames in the wrong order on purpose. */
    CHECK(write_tagged(LIBDIR "/zzz.mp3", "Mysterons", "Portishead", NULL, "Dummy", "1", "1994"),
          "Dummy 1");
    CHECK(write_tagged(LIBDIR "/aaa.mp3", "Sour Times", "Portishead", NULL, "Dummy", "2", "1994"),
          "Dummy 2");
    CHECK(write_tagged(LIBDIR "/mmm.mp3", "Cowboys", "Portishead", NULL, "Portishead", "1",
                       "1997"),
          "Portishead 1");

    /* A compilation: four different TPE1s, one TPE2. */
    CHECK(write_tagged(LIBDIR "/c1.mp3", "Track One", "Alice", "Various Artists", "Now 42", "1",
                       "2001"),
          "compilation 1");
    CHECK(write_tagged(LIBDIR "/c2.mp3", "Track Two", "Bob", "Various Artists", "Now 42", "2",
                       "2001"),
          "compilation 2");
    CHECK(write_tagged(LIBDIR "/c3.mp3", "Track Three", "Carol", "Various Artists", "Now 42", "3",
                       "2001"),
          "compilation 3");

    /* Two artists, one album title. These must not merge. */
    CHECK(write_tagged(LIBDIR "/g1.mp3", "Hit A", "Queen", NULL, "Greatest Hits", "1", "1981"),
          "Queen greatest hits");
    CHECK(write_tagged(LIBDIR "/g2.mp3", "Hit B", "Abba", NULL, "Greatest Hits", "1", "1975"),
          "Abba greatest hits");

    /* A two-disc set whose disc 2 track 1 must not jump ahead of disc 1. */
    CHECK(write_tagged(LIBDIR "/d2t1.mp3", "Disc Two Opener", "Sufjan", NULL, "Illinois", "1",
                       "2005"),
          "disc 2 track 1");
    CHECK(write_tagged(LIBDIR "/d1t9.mp3", "Disc One Closer", "Sufjan", NULL, "Illinois", "9",
                       "2005"),
          "disc 1 track 9");

    /* No tag at all: the browser has to invent both names. */
    CHECK(write_text(LIBDIR "/untagged.mp3", "no tag here"), "an untagged file");
}

/* Finds an artist by name, or (size_t)-1. */
static size_t find_artist(const nd_music_library *lib, const char *name)
{
    size_t i;

    for (i = 0u; i < api.lib_n_artists(lib); i++) {
        if (strcmp(api.lib_artist(lib, i)->name, name) == 0)
            return i;
    }
    return (size_t)-1;
}

static void test_name_cmp(void)
{
    CHECK(api.name_cmp("abba", "Beatles") < 0, "case-insensitive: a before B");
    CHECK(api.name_cmp("ABBA", "abba") == 0, "the same name in any case is the same name");
    CHECK(api.name_cmp("Zappa", "abba") > 0, "and Z after a");
    CHECK(api.name_cmp("", "") == 0, "two empties");

    /* The placeholders sort LAST whatever letter they start with, so an
     * untagged file does not land between Tycho and U2. */
    CHECK(api.name_cmp(*api.unknown_artist, "Zappa") > 0, "Unknown Artist sorts after Z");
    CHECK(api.name_cmp("Abba", *api.unknown_artist) < 0, "and after A");
    CHECK(api.name_cmp(*api.unknown_album, "Zeppelin") > 0, "Unknown Album too");
    CHECK(api.name_cmp(*api.unknown_artist, *api.unknown_artist) == 0,
          "and a placeholder equals itself");

    CHECK(api.name_cmp(NULL, "a") != 0, "NULL is ordered, not a crash");
    CHECK(api.name_cmp("a", NULL) != 0, "either way round");
    CHECK(api.name_cmp(NULL, NULL) == 0, "and two NULLs are equal");
}

static void test_library_groups(void)
{
    nd_music_track *tracks = calloc((size_t)ND_MUSIC_MAX, sizeof *tracks);
    nd_music_library *lib = NULL;
    size_t n;
    size_t idx;

    if (tracks == NULL) {
        CHECK(false, "allocation");
        return;
    }
    n = api.scan(tracks, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_STREAM);
    CHECK(n > 0u, "the card scanned");

    CHECK_INT(api.lib_build(&lib, tracks, n, NULL, NULL), ND_OK, "the library builds");
    if (lib == NULL) {
        free(tracks);
        CHECK(false, "no library");
        return;
    }

    CHECK_INT(api.lib_n_songs(lib), n, "every scanned track is in the library");

    /* ---- the compilation collapses to ONE artist ---- */
    idx = find_artist(lib, "Various Artists");
    CHECK(idx != (size_t)-1, "TPE2 gives the compilation a single artist");
    if (idx != (size_t)-1) {
        CHECK_INT(api.lib_artist(lib, idx)->n_albums, 1, "with one album");
        CHECK_INT(api.lib_artist(lib, idx)->n_songs, 3, "and three songs");
    }
    CHECK_INT(find_artist(lib, "Alice"), (size_t)-1,
              "and the per-track TPE1s do NOT become artists of their own");
    CHECK_INT(find_artist(lib, "Bob"), (size_t)-1, "none of them");

    /* ---- an untagged file gets both placeholders ---- */
    idx = find_artist(lib, *api.unknown_artist);
    CHECK(idx != (size_t)-1, "an untagged track lands under Unknown Artist");
    if (idx != (size_t)-1) {
        const nd_music_artist *a = api.lib_artist(lib, idx);
        const nd_music_album *alb = api.lib_album(lib, a->first_album);
        size_t j;
        bool found = false;

        /* build_card() already left several untagged files on the card, so
         * they ALL land here -- which is the claim: one Unknown Artist with
         * one Unknown Album, not one row per untagged file. */
        CHECK_INT(a->n_albums, 1, "all untagged tracks share one album");
        CHECK_STR(alb->name, *api.unknown_album, "which is Unknown Album");
        CHECK_INT(alb->year, 0, "with no year");

        /* With no TIT2 the title falls back to the filename, which is what
         * the Now Playing screen already does with the same file. */
        for (j = 0u; j < alb->n_songs; j++) {
            const nd_music_song *sg = api.lib_song(lib, alb->first_song + j);

            if (strcmp(sg->title, "untagged.mp3") == 0) {
                found = true;
                CHECK(strstr(sg->path, "/lib/untagged.mp3") != NULL,
                      "and it is the file we wrote");
                CHECK_INT(sg->track, 0, "with no track number");
            }
        }
        CHECK(found, "an untagged track is titled after its filename");
    }

    /* ---- two artists with the same album title stay two albums ---- */
    {
        size_t queen = find_artist(lib, "Queen");
        size_t abba = find_artist(lib, "Abba");

        CHECK(queen != (size_t)-1 && abba != (size_t)-1, "both artists exist");
        if (queen != (size_t)-1 && abba != (size_t)-1) {
            const nd_music_album *qa = api.lib_album(lib, api.lib_artist(lib, queen)->first_album);
            const nd_music_album *aa = api.lib_album(lib, api.lib_artist(lib, abba)->first_album);

            CHECK_STR(qa->name, "Greatest Hits", "Queen's");
            CHECK_STR(aa->name, "Greatest Hits", "Abba's");
            CHECK(qa != aa, "two albums, not one merged by title");
            CHECK_INT(qa->n_songs, 1, "one song each");
            CHECK_INT(aa->n_songs, 1, "each");
        }
    }

    api.lib_free(lib);
    free(tracks);
}

static void test_library_order(void)
{
    nd_music_track *tracks = calloc((size_t)ND_MUSIC_MAX, sizeof *tracks);
    nd_music_library *lib = NULL;
    size_t n;
    size_t i;
    size_t idx;

    if (tracks == NULL) {
        CHECK(false, "allocation");
        return;
    }
    n = api.scan(tracks, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_STREAM);
    CHECK_INT(api.lib_build(&lib, tracks, n, NULL, NULL), ND_OK, "the library builds");
    if (lib == NULL) {
        free(tracks);
        return;
    }

    /* ---- artists are sorted, and the placeholder is last ---- */
    for (i = 1u; i < api.lib_n_artists(lib); i++) {
        CHECK(api.name_cmp(api.lib_artist(lib, i - 1u)->name, api.lib_artist(lib, i)->name) < 0,
              "artists are in ascending order with no duplicates");
    }
    if (api.lib_n_artists(lib) > 0u) {
        CHECK_STR(api.lib_artist(lib, api.lib_n_artists(lib) - 1u)->name, *api.unknown_artist,
                  "Unknown Artist is last");
    }

    /* ---- an artist's albums are CHRONOLOGICAL, not alphabetical ---- *
     *
     * Portishead's two albums are "Dummy" (1994) and "Portishead" (1997),
     * which happen to be in the same order either way -- so the claim is
     * checked on the years, which is what actually decides it. */
    idx = find_artist(lib, "Portishead");
    CHECK(idx != (size_t)-1, "Portishead is in the library");
    if (idx != (size_t)-1) {
        const nd_music_artist *a = api.lib_artist(lib, idx);

        CHECK_INT(a->n_albums, 2, "two albums");
        if (a->n_albums == 2u) {
            const nd_music_album *first = api.lib_album(lib, a->first_album);
            const nd_music_album *second = api.lib_album(lib, a->first_album + 1u);

            CHECK_INT(first->year, 1994, "the 1994 album is first");
            CHECK_INT(second->year, 1997, "and the 1997 album second");
            CHECK_STR(first->name, "Dummy", "Dummy");
            CHECK_STR(second->name, "Portishead", "then Portishead");
        }

        /* ---- and the tracks are in TRACK NUMBER order, not filename order.
         *
         * THIS IS THE WHOLE POINT. On disk "Dummy"'s two tracks are aaa.mp3
         * (track 2) and zzz.mp3 (track 1); the old list showed them a, z. */
        if (a->n_albums >= 1u) {
            const nd_music_album *dummy = api.lib_album(lib, a->first_album);

            CHECK_INT(dummy->n_songs, 2, "Dummy has two songs");
            if (dummy->n_songs == 2u) {
                const nd_music_song *s1 = api.lib_song(lib, dummy->first_song);
                const nd_music_song *s2 = api.lib_song(lib, dummy->first_song + 1u);

                CHECK_STR(s1->title, "Mysterons", "track 1 comes first");
                CHECK_STR(s2->title, "Sour Times", "track 2 second");
                CHECK(strstr(s1->path, "zzz.mp3") != NULL,
                      "even though its filename sorts LAST");
                CHECK(strstr(s2->path, "aaa.mp3") != NULL, "and the other sorts first");
            }
        }
    }

    /* ---- a compilation keeps its running order ---- */
    idx = find_artist(lib, "Various Artists");
    if (idx != (size_t)-1) {
        const nd_music_album *alb = api.lib_album(lib, api.lib_artist(lib, idx)->first_album);

        if (alb->n_songs == 3u) {
            CHECK_STR(api.lib_song(lib, alb->first_song)->title, "Track One", "1");
            CHECK_STR(api.lib_song(lib, alb->first_song + 1u)->title, "Track Two", "2");
            CHECK_STR(api.lib_song(lib, alb->first_song + 2u)->title, "Track Three", "3");
        } else {
            CHECK(false, "the compilation has three songs");
        }
    }

    /* ---- every album's song run is inside the song array and contiguous, and
     * every artist's album run likewise. A browser indexes straight into
     * these, so a bad range is an out-of-bounds read on a real phone. ---- */
    {
        size_t songs_seen = 0u;
        size_t albums_seen = 0u;

        for (i = 0u; i < api.lib_n_artists(lib); i++) {
            const nd_music_artist *a = api.lib_artist(lib, i);
            size_t j;
            size_t counted = 0u;

            CHECK_INT(a->first_album, albums_seen, "an artist's albums start where the last ended");
            CHECK(a->n_albums > 0u, "an artist with no albums cannot exist");
            for (j = 0u; j < a->n_albums; j++) {
                const nd_music_album *alb = api.lib_album(lib, a->first_album + j);

                CHECK_INT(alb->artist, i, "an album points back at its artist");
                CHECK_INT(alb->first_song, songs_seen, "songs are contiguous across the library");
                CHECK(alb->n_songs > 0u, "an album with no songs cannot exist");
                songs_seen += alb->n_songs;
                counted += alb->n_songs;
            }
            albums_seen += a->n_albums;
            CHECK_INT(a->n_songs, counted, "an artist's song count is its albums'");
        }
        CHECK_INT(albums_seen, api.lib_n_albums(lib), "every album belongs to an artist");
        CHECK_INT(songs_seen, api.lib_n_songs(lib), "and every song to an album");
    }

    api.lib_free(lib);
    free(tracks);
}

/* The flat "Songs" view: every song exactly once, in title order. */
static void test_library_by_title(void)
{
    nd_music_track *tracks = calloc((size_t)ND_MUSIC_MAX, sizeof *tracks);
    nd_music_library *lib = NULL;
    size_t n;
    size_t i;
    size_t seen = 0u;

    if (tracks == NULL) {
        CHECK(false, "allocation");
        return;
    }
    n = api.scan(tracks, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_STREAM);
    CHECK_INT(api.lib_build(&lib, tracks, n, NULL, NULL), ND_OK, "the library builds");
    if (lib == NULL) {
        free(tracks);
        return;
    }

    /* Ascending by title, with ties allowed -- two tracks really can share a
     * name, and the tie-break is checked below rather than here. */
    for (i = 1u; i < api.lib_n_songs(lib); i++) {
        const nd_music_song *prev = api.lib_song_by_title(lib, i - 1u);
        const nd_music_song *cur = api.lib_song_by_title(lib, i);

        CHECK(api.name_cmp(prev->title, cur->title) <= 0, "titles ascend");
    }

    /* It is a PERMUTATION: every song appears exactly once. A sort that
     * dropped or duplicated a row would still pass the ordering check above,
     * which is why this one exists. */
    for (i = 0u; i < api.lib_n_songs(lib); i++) {
        const nd_music_song *target = api.lib_song(lib, i);
        size_t j;
        size_t hits = 0u;

        for (j = 0u; j < api.lib_n_songs(lib); j++) {
            if (api.lib_song_by_title(lib, j) == target)
                hits++;
        }
        if (hits != 1u) {
            CHECK_INT(hits, 1, "every song appears exactly once in title order");
            break;
        }
        seen++;
    }
    CHECK_INT(seen, api.lib_n_songs(lib), "and all of them appear");

    /* The index is stable across two builds of the same card: a browser that
     * reordered itself between visits would be worse than one that did not
     * sort at all. */
    {
        nd_music_library *again = NULL;

        if (api.lib_build(&again, tracks, n, NULL, NULL) == ND_OK && again != NULL) {
            bool same = true;

            for (i = 0u; i < api.lib_n_songs(lib); i++) {
                if (strcmp(api.lib_song_by_title(lib, i)->path,
                           api.lib_song_by_title(again, i)->path) != 0) {
                    same = false;
                    break;
                }
            }
            CHECK(same, "two builds of one card give the same title order");
            api.lib_free(again);
        }
    }

    CHECK(api.lib_song_by_title(lib, api.lib_n_songs(lib)) == NULL, "past the end is NULL");
    CHECK(api.lib_song_by_title(NULL, 0u) == NULL, "and a NULL library is NULL");

    api.lib_free(lib);
    free(tracks);
}

static void test_library_discs(void)
{
    nd_music_track one[2];
    nd_music_library *lib = NULL;

    /* Two tracks of one album: disc 1 track 9, and disc 2 track 1. Sorted by
     * track number alone the disc-2 opener would jump ahead of the disc-1
     * closer, which is the bug this pins. The fixture writes TPOS through a
     * second tag write, so build them here rather than in build_library_card. */
    CHECK_INT(nd_mkdir_p(MUSIC "/discs", 0755u), ND_OK, "a directory for the two-disc set");
    {
        uint8_t tag[512];
        uint8_t body[128];
        char real[ND_PATH_MAX];
        static const struct {
            const char *file;
            const char *title;
            const char *trck;
            const char *tpos;
        } SET[2] = {
            {MUSIC "/discs/b.mp3", "Disc Two Opener", "1", "2"},
            {MUSIC "/discs/a.mp3", "Disc One Closer", "9", "1"},
        };
        size_t k;

        for (k = 0u; k < 2u; k++) {
            size_t n = ND_ID3_HEADER_LEN;
            size_t b;
            FILE *f;

            b = text_body(body, SET[k].title);
            n += id3_frame_v23(tag + n, "TIT2", body, b);
            b = text_body(body, "Sufjan");
            n += id3_frame_v23(tag + n, "TPE1", body, b);
            b = text_body(body, "Illinois");
            n += id3_frame_v23(tag + n, "TALB", body, b);
            b = text_body(body, SET[k].trck);
            n += id3_frame_v23(tag + n, "TRCK", body, b);
            b = text_body(body, SET[k].tpos);
            n += id3_frame_v23(tag + n, "TPOS", body, b);
            (void)id3_header(tag, 3u, 0u, n - ND_ID3_HEADER_LEN);

            CHECK_INT(nd_path_resolve(real, sizeof real, SET[k].file), ND_OK, "path");
            f = fopen(real, "wb");
            if (f == NULL) {
                CHECK(false, "write the disc fixture");
                return;
            }
            (void)fwrite(tag, 1u, n, f);
            (void)fclose(f);
            (void)nd_strlcpy(one[k].path, SET[k].file, sizeof one[k].path);
        }
    }

    CHECK_INT(api.lib_build(&lib, one, 2u, NULL, NULL), ND_OK, "the two-disc library builds");
    if (lib == NULL)
        return;
    CHECK_INT(api.lib_n_albums(lib), 1, "one album");
    if (api.lib_n_albums(lib) == 1u) {
        const nd_music_album *alb = api.lib_album(lib, 0u);

        CHECK_INT(alb->n_songs, 2, "with both songs");
        CHECK_STR(api.lib_song(lib, alb->first_song)->title, "Disc One Closer",
                  "disc 1 track 9 comes before disc 2 track 1");
        CHECK_STR(api.lib_song(lib, alb->first_song + 1u)->title, "Disc Two Opener",
                  "and the disc 2 opener is second");
    }
    api.lib_free(lib);
}

/* The progress callback, and cancelling. */
static size_t g_progress_calls;
static size_t g_progress_last_done;
static size_t g_progress_total;
static size_t g_progress_stop_after;

static bool progress_cb(void *ctx, size_t done, size_t total)
{
    CHECK(ctx == (void *)&g_progress_calls, "the context is handed back");
    g_progress_calls++;
    g_progress_last_done = done;
    g_progress_total = total;
    if (g_progress_stop_after != 0u && g_progress_calls >= g_progress_stop_after)
        return false;
    return true;
}

static void test_library_progress(void)
{
    nd_music_track *tracks = calloc((size_t)ND_MUSIC_MAX, sizeof *tracks);
    nd_music_library *lib = NULL;
    size_t n;

    if (tracks == NULL) {
        CHECK(false, "allocation");
        return;
    }
    n = api.scan(tracks, (size_t)ND_MUSIC_MAX, ND_MUSIC_BACKEND_STREAM);

    g_progress_calls = 0u;
    g_progress_last_done = 0u;
    g_progress_total = 0u;
    g_progress_stop_after = 0u;
    CHECK_INT(api.lib_build(&lib, tracks, n, progress_cb, &g_progress_calls), ND_OK,
              "a build with a callback succeeds");
    CHECK(g_progress_calls > 0u, "the callback was called");
    CHECK_INT(g_progress_total, n, "and told the real total");
    CHECK_INT(g_progress_last_done, n, "and reached it");
    api.lib_free(lib);
    lib = NULL;

    /* Cancelling frees everything and reports it, rather than returning a
     * half-built library the browser would then draw. */
    g_progress_calls = 0u;
    g_progress_stop_after = 1u;
    CHECK_INT(api.lib_build(&lib, tracks, n, progress_cb, &g_progress_calls), ND_ERR_BUSY,
              "a cancelled build says so");
    CHECK(lib == NULL, "and hands back nothing to free");

    g_progress_stop_after = 0u;
    free(tracks);
}

static void test_library_edges(void)
{
    nd_music_library *lib = NULL;
    nd_music_track one;

    /* Zero tracks is a library, not an error: the browser draws "no music"
     * rather than an error screen. */
    CHECK_INT(api.lib_build(&lib, NULL, 0u, NULL, NULL), ND_OK, "an empty library builds");
    if (lib != NULL) {
        CHECK_INT(api.lib_n_songs(lib), 0, "no songs");
        CHECK_INT(api.lib_n_artists(lib), 0, "no artists");
        CHECK_INT(api.lib_n_albums(lib), 0, "no albums");
        CHECK(api.lib_song(lib, 0u) == NULL, "and indexing it gives NULL, not a dummy row");
        CHECK(api.lib_artist(lib, 0u) == NULL, "for artists");
        CHECK(api.lib_album(lib, 0u) == NULL, "and albums");
        api.lib_free(lib);
        lib = NULL;
    } else {
        CHECK(false, "an empty library is still a library");
    }

    /* A track that has since vanished must not take the build down with it;
     * it gets the placeholders like any other untagged file. */
    (void)nd_strlcpy(one.path, MUSIC "/does-not-exist.mp3", sizeof one.path);
    CHECK_INT(api.lib_build(&lib, &one, 1u, NULL, NULL), ND_OK, "a missing file still builds");
    if (lib != NULL) {
        CHECK_INT(api.lib_n_songs(lib), 1, "and is listed");
        CHECK_STR(api.lib_artist(lib, 0u)->name, *api.unknown_artist, "under Unknown Artist");
        api.lib_free(lib);
        lib = NULL;
    }

    CHECK_INT(api.lib_build(NULL, NULL, 0u, NULL, NULL), ND_ERR_INVAL, "a NULL out is refused");
    CHECK_INT(api.lib_build(&lib, NULL, 5u, NULL, NULL), ND_ERR_INVAL,
              "so are tracks that are NULL with a non-zero count");
    api.lib_free(NULL); /* must be safe */
    sa_checks++;

    /* Past the end is NULL on a non-empty library too. */
    {
        nd_music_track t;

        (void)nd_strlcpy(t.path, MUSIC "/does-not-exist.mp3", sizeof t.path);
        if (api.lib_build(&lib, &t, 1u, NULL, NULL) == ND_OK && lib != NULL) {
            CHECK(api.lib_song(lib, 1u) == NULL, "one past the last song");
            CHECK(api.lib_artist(lib, 1u) == NULL, "one past the last artist");
            CHECK(api.lib_album(lib, 1u) == NULL, "one past the last album");
            CHECK(api.lib_song(lib, (size_t)-1) == NULL, "and a wrapped index");
            api.lib_free(lib);
        }
    }
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* must be safe with nothing held */
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h = sa_begin("MusicPlayer", "ndmusic");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndmusic-root", g_root, sizeof g_root) ||
        !sa_tmpdir("ndmusic-bin", g_bindir, sizeof g_bindir)) {
        (void)dlclose(h);
        return 1;
    }

    /* Everything below /NeoDCT is this test's own. Set before anything is
     * written, because every helper here goes through nd_path_resolve(). */
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);

    RUN(test_strings);
    RUN(test_is_supported);
    RUN(test_pick_player);
    RUN(test_format_time);
    RUN(test_truncate);
    RUN(test_id3_text);
    RUN(test_id3_frames);
    RUN(test_id3_library_frames);
    RUN(test_id3_hostile);

    /* The no-card screens FIRST: they are the only ones the golden frame
     * covers and they need the card to be absent. */
    RUN(test_no_card);
    RUN(test_golden_frame);

    RUN(build_card);
    RUN(test_music_dir);
    RUN(test_scan);
    RUN(test_folder_art);
    RUN(test_metadata);
    RUN(test_duration);
    RUN(test_playback);
    RUN(test_gain_ladder);
    RUN(test_apply_gain);
    RUN(test_gain_buffer);
    RUN(test_volume_state);
    RUN(test_mpv_gets_the_volume);
    RUN(build_library_card);
    RUN(test_name_cmp);
    RUN(test_library_groups);
    RUN(test_library_order);
    RUN(test_library_by_title);
    RUN(test_library_discs);
    RUN(test_library_progress);
    RUN(test_library_edges);
    RUN(test_null_safety);

    nd_storage_set_paths(NULL, NULL);
    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
    rc = sa_end(h, "test_musicplayer");
    sa_rmtree(g_root);
    sa_rmtree(g_bindir);
    return rc;
}
