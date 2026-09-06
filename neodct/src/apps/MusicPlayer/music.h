/* music.h -- the parts of the Music app a unit test can reach.
 *
 * System/apps/MusicPlayer/main.py is 516 lines: a card scan, ID3 metadata
 * with cover art, two playback backends, a track list and the "Now Playing"
 * screen. The golden frame `app-musicplayer` is none of those -- with no card
 * in the harness the app never gets past its first four lines -- so almost
 * everything below is checked by test_musicplayer.c rather than by a picture.
 *
 * test/unit/test_musicplayer.c dlopen()s the BUILT app.so and dlsym()s these,
 * the way test_tones.c and test_cubebench.c do.
 *
 * ============ THE CAPS THIS PORT ADDS ============
 *
 * The Python holds the playlist in a Python list and never bounds it, and
 * mutagen's strings are arbitrarily long. CODING-STANDARDS.md section 1.5
 * will not have an array sized by the contents of an SD card, so:
 *
 *   ND_MUSIC_MAX       256 tracks. HEAP allocated -- 256 * 256 = 65,536
 *                      bytes -- and freed before the screen returns. A card
 *                      with more shows the first 256 in walk order and logs
 *                      that it stopped.
 *   ND_MUSIC_WALK_MAX  64 directories pending in the walk.
 *   ND_MUSIC_TEXT_MAX  192 bytes of title/artist/album, which is nd_id3.h's
 *                      own cap. About sixty Latin characters; the widest
 *                      string this screen can draw is fifteen.
 *
 * All three are recorded in OPEN-QUESTIONS.md under MU-1.
 */

#ifndef ND_MUSIC_H_INCLUDED
#define ND_MUSIC_H_INCLUDED

#include <sys/types.h>

#include "nd_font.h"
#include "nd_id3.h"
#include "nd_image.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* manifest.json says 970. THE LIST DOES NOT USE IT -- see
 * ND_MUSIC_LIST_APP_ID. */
#define ND_MUSIC_APP_ID 970

/* `VerticalList(self.ui, "Music", display_list, app_id=4)`.
 *
 * 4 is SETTINGS' root id, so the breadcrumb on the track list reads "4-1",
 * "4-2" ... in an app whose own id is 970. It is a copy-paste bug in the
 * Python and it is ON SCREEN, so CODING-STANDARDS.md section 9.4 applies:
 * ported as-is, written up in OPEN-QUESTIONS.md MU-2, not fixed here. */
#define ND_MUSIC_LIST_APP_ID 4

/* MUSIC_FOLDER -- a folder NAME on the card, not a path. Storage.folder()
 * turns it into /NeoDCT/User/sdcard/music, and answers at all only once a
 * NeoDCT card is mounted. */
#define ND_MUSIC_FOLDER "music"

/* The environment overrides, spelled as the Python spells them. */
#define ND_MUSIC_ENV_AUDIO   "NEODCT_MUSIC_AUDIO"
#define ND_MUSIC_ENV_ABUF_MS "NEODCT_MUSIC_ABUF_MS"
#define ND_MUSIC_ABUF_MS_DEFAULT 500

/* miniaudio's output format, and the denominator of position(). */
#define ND_MUSIC_RATE 44100

#define ND_MUSIC_MAX      256
#define ND_MUSIC_PATH_MAX 256
#define ND_MUSIC_WALK_MAX 64
#define ND_MUSIC_TEXT_MAX ND_ID3_TEXT_MAX

/* NO_CARD_HELP verbatim, and MPV_CMD minus one thing.
 *
 * ============ THE PYTHON'S `nice -n -10` IS NOT HERE ============
 *
 * The Python's argv was `nice -n -10 mpv ...`, ported verbatim, and it worked
 * for as long as apps ran as root. Since 27cf79bf an app is ndusr, and a
 * negative nice needs CAP_SYS_NICE or RLIMIT_NICE headroom that nothing on
 * this phone grants -- the kernel default is 0.
 *
 * GNU coreutils' nice would warn and run the program anyway. The phone ships
 * BUSYBOX nice, which calls bb_perror_msg_and_die and never reaches its exec,
 * so the process actually spawned -- nice, not mpv -- exited 1 every time.
 * With the child's stdout and stderr on /dev/null and a successful fork
 * treated as playback, the visible result was that .flac and .ogg tracks
 * simply did not play and nothing said why.
 *
 * mpv is the exec target now. The stutter the nice was meant to prevent is
 * cosmetic; silence is not. */
extern const char *const nd_music_no_card_help;
extern const char *const nd_music_no_card_message;
#define ND_MUSIC_MPV_ARGC 4
extern const char *const nd_music_mpv_cmd[ND_MUSIC_MPV_ARGC];

/* The three defaults get_metadata() starts from. */
extern const char *const nd_music_unknown_artist;

/* ------------------------------------------------------------------ *
 * The backends
 * ------------------------------------------------------------------ */

/* _MiniaudioPlayer, _MpvPlayer, and the Python's `player is None`.
 *
 * ND_MUSIC_STREAM is the C spelling of _MiniaudioPlayer: dr_mp3 / dr_wav
 * decoding in this process, straight into `aplay`, which is what
 * lib/nd_notify.c already does for the ringtone and what AUDIO.md explains
 * at length. It is NOT miniaudio and it does not decode the whole file. */
typedef enum {
    ND_MUSIC_BACKEND_NONE = 0,
    ND_MUSIC_BACKEND_STREAM,
    ND_MUSIC_BACKEND_MPV
} nd_music_backend;

/* _pick_player(). Reads NEODCT_MUSIC_AUDIO and logs the same three lines. */
nd_music_backend nd_music_pick_player(void);

/* The backend actually in use right now, which differs from pick_player()
 * once the session has fallen back to mpv. ND_MUSIC_BACKEND_NONE before the
 * first nd_music_player_init(). */
nd_music_backend nd_music_backend_now(void);

/* Sets the session's backend and releases whatever was playing. Called once
 * at the top of app_run(); exposed so a test can drive one backend without
 * an environment variable. */
void nd_music_player_init(nd_music_backend backend);

/* `_MiniaudioPlayer.EXTS` / `_MpvPlayer.EXTS`, lower-cased suffix match.
 *
 * The STREAM list keeps .flac and .ogg even though dr_mp3 and dr_wav cannot
 * read either: the Python lists them because miniaudio plays them, and a
 * track that vanishes from the list is a worse answer than one that plays
 * through the mpv fallback. See OPEN-QUESTIONS.md MU-4. */
bool nd_music_is_supported(const char *filename, nd_music_backend backend);

/* ------------------------------------------------------------------ *
 * Volume
 * ------------------------------------------------------------------ *
 *
 * NOT A PORT. The Python has no volume control at all -- neither backend is
 * given one and nothing on the phone sets a mixer. This is new, and the two
 * decisions behind it are worth stating because neither is obvious.
 *
 * ============ WHY SOFTWARE GAIN AND NOT amixer ============
 *
 * Both defconfigs build alsa-utils with amixer in it, so `amixer set Master
 * 60%` was available and was not used. Three reasons, in order of weight:
 *
 *   1. IT WOULD BE THE PHONE'S VOLUME, NOT THE MUSIC'S. The ringtone goes
 *      through the same card (lib/nd_notify.c). Turning a track down must not
 *      turn the ring down -- a phone that stops ringing because you were
 *      listening to something quietly is broken in a way that costs you a
 *      call.
 *   2. THE CONTROL NAME IS NOT KNOWABLE HERE. "Master" exists on QEMU's
 *      AC97; the RV1103's codec exposes its own set, and a wrong name fails
 *      silently. Guessing across a list of names at runtime is a lot of
 *      machinery to end up less predictable than a multiply.
 *   3. A MULTIPLY IS TESTABLE. nd_music_gain_q15() is a pure function and
 *      test_musicplayer.c checks the whole ladder against its own arithmetic.
 *      An amixer call can only be tested by watching whether a subprocess
 *      was spawned.
 *
 * ============ THE ONE COST: THE CHANGE IS HEARD LATE ============
 *
 * Gain is applied where the audio is DECODED, so a new level reaches the
 * speaker only once everything already buffered ahead of it has drained.
 * Two buffers sit in the way, and both are the existing design:
 *
 *   the socketpair   whatever SO_SNDBUF defaults to, typically 208 kB on
 *                    Linux, which is about 1.2 s of 44.1 kHz stereo
 *   aplay's own      --buffer-time, which audio.c already sets from
 *                    NEODCT_MUSIC_ABUF_MS -- 500 ms by default
 *
 * so roughly 1.7 s at the shipped settings. NEODCT_MUSIC_ABUF_MS is the
 * knob: it shrinks aplay's buffer AND the decode chunk together, so
 * lowering it lowers the lag.
 *
 * Neither buffer is resized here, deliberately. Shrinking the socket buffer
 * would cut the lag roughly in half and would also cut the slack the feeder
 * thread has before aplay underruns -- and an underrun is a stutter on a
 * device this port cannot be tested on from here, which is a much worse
 * trade than a volume key that takes a moment. What the key DOES do
 * immediately is move the bar in the header, so the phone never looks like
 * it ignored you.
 *
 * ============ THE LADDER IS IN dB, NOT IN AMPLITUDE ============
 *
 * Loudness is roughly logarithmic, so ten equal steps of AMPLITUDE would put
 * nine of them in the top half of what you can hear and make the bottom of
 * the range useless. The table is 3 dB a step -- each step is half the power
 * of the one above -- which spreads 0..10 over 30 dB. Level 10 is unity: the
 * samples are passed through untouched and the multiply is skipped entirely.
 */

#define ND_MUSIC_VOLUME_MAX 10
#define ND_MUSIC_VOLUME_DEFAULT 7

/* settings.prop, written through nd_settings_set() like every other app
 * preference (Tones writes the ringtone the same way). */
#define ND_MUSIC_VOLUME_SETTING "music.volume"

/* The current level, 0..ND_MUSIC_VOLUME_MAX. */
int32_t nd_music_volume(void);

/* Clamps to 0..ND_MUSIC_VOLUME_MAX, applies to whatever is playing NOW, and
 * persists. Safe to call from the UI thread while the feeder is running. */
void nd_music_set_volume(int32_t level);

/* Reads ND_MUSIC_VOLUME_SETTING and applies it. Called once at start-up; a
 * missing or unparseable value gives ND_MUSIC_VOLUME_DEFAULT.
 *
 * IT DOES NOT WRITE. Opening the app must not rewrite settings.prop -- that
 * is a whole-file temp-fsync-rename on the phone's only writable flash, for a
 * value nobody changed. Only nd_music_set_volume() persists. */
void nd_music_volume_load(void);

/* The Q15 multiplier for a level: 32768 is unity, 0 is silence. Out-of-range
 * levels clamp. Exposed because the ladder is the part worth pinning. */
int32_t nd_music_gain_q15(int32_t level);

/* One sample through one level. Saturating, though at a gain of at most
 * unity it cannot actually clip -- the saturation is there so that raising
 * the ceiling later cannot silently wrap a sample from +32767 to -32768. */
int16_t nd_music_apply_gain(int16_t sample, int32_t level);

/* Scales `n_samples` interleaved samples in place. This is what the feeder
 * calls; a level of ND_MUSIC_VOLUME_MAX returns without touching the buffer,
 * which is the fast path the common case takes. */
void nd_music_gain_buffer(int16_t *samples, size_t n_samples, int32_t level);

/* ------------------------------------------------------------------ *
 * The playlist
 * ------------------------------------------------------------------ */

/* One row. `path` is a LOGICAL path -- "/NeoDCT/User/sdcard/music/x.mp3" --
 * resolved only where it is opened. */
typedef struct {
    char path[ND_MUSIC_PATH_MAX];
} nd_music_track;

/* music_dir(): Storage.folder("music"), or false when no card is mounted. */
bool nd_music_dir(char *out, size_t out_sz);

/* scan_music(). os.walk order, each directory's own files sorted by byte
 * value -- NOT sorted globally. That is the Python's behaviour and README.md
 * already calls the sorting limited; it is ported, not improved. Returns how
 * many rows were written. */
size_t nd_music_scan(nd_music_track *out, size_t max, nd_music_backend backend);


/* ------------------------------------------------------------------ *
 * The library -- artists, albums, and a sort that is actually a sort
 * ------------------------------------------------------------------ *
 *
 * NOT A PORT. OPEN-QUESTIONS.md MU-11 records that the Python sorts each
 * directory's files by byte value and does NOT sort globally, so a track two
 * directories down lands after every track beside its parent whatever it is
 * called; README.md calls the sorting and metadata support limited. That
 * limitation was ported faithfully and test_scan still pins it. THIS IS THE
 * LAYER ON TOP, and nd_music_scan() is untouched underneath it -- the Folders
 * view is still the walk, byte for byte.
 *
 * ============ WHAT IT COSTS, AND WHY IT IS BUILT ON DEMAND ============
 *
 * Grouping by artist means reading a tag from every file, and the ONLY
 * affordable way to do that was measured before it was written:
 *
 *   nd_id3_read(path, &tag, NULL, NULL)   the tag body only. A 60 kB
 *                                         embedded cover is still read off
 *                                         the card but never decoded.
 *                                         ~15 MB and ~1 ms of CPU for a full
 *                                         256-track card.
 *
 *   nd_music_get_metadata()               ALSO walks the whole MP3 to count
 *                                         frames when there is no Xing
 *                                         header, and ALSO decodes cover art
 *                                         at full resolution. For 256 tracks
 *                                         that is up to 1.3 GB off the card
 *                                         and minutes of wall clock.
 *
 * So the library reads tags and NOTHING else -- no duration, no artwork.
 * Both belong to the Now Playing screen, which needs them for one track at a
 * time and already fetches them there.
 *
 * Even the cheap scan is seconds on a full card, so it is NOT done at
 * start-up. The app opens on a menu; Folders is the old instant walk, and
 * Artists / Albums / Songs build the index once per session, behind a
 * progress bar that Clear can cancel.
 *
 * ============ THE CAPS ARE DERIVED, NOT INVENTED ============
 *
 * A track has one artist and one album, so ND_MUSIC_MAX tracks cannot
 * produce more than ND_MUSIC_MAX of either. Sizing both tables at
 * ND_MUSIC_MAX means the overflow case does not exist -- there is no "too
 * many artists" branch to get wrong, and no bucket for the ones that did not
 * fit. It costs 256 * 200 bytes of artist table on a device that has already
 * spent 64 kB on the paths.
 *
 * One library is about 220 kB of heap, held only while the browser is open.
 */

/* Songs, artists and albums are each capped by the track count for the
 * reason above. */
#define ND_MUSIC_ARTISTS_MAX ND_MUSIC_MAX
#define ND_MUSIC_ALBUMS_MAX  ND_MUSIC_MAX

/* Shown for a track whose tag names no album. The artist equivalent is
 * nd_music_unknown_artist, which is the Python's own string. */
extern const char *const nd_music_unknown_album;

/* One track, with its tag read and its groups resolved. `artist` and `album`
 * are indices into the library's own tables, not pointers, so the whole
 * structure is one contiguous block that can be freed in one call. */
typedef struct {
    char path[ND_MUSIC_PATH_MAX];
    char title[ND_MUSIC_TEXT_MAX];
    uint16_t artist;
    uint16_t album;
    uint16_t track; /* TRCK, 0 when the tag did not say */
    uint16_t disc;  /* TPOS, 0 when the tag did not say */
} nd_music_song;

/* Albums are a contiguous run, so an artist is a pair of integers. */
typedef struct {
    char name[ND_MUSIC_TEXT_MAX];
    uint16_t first_album;
    uint16_t n_albums;
    uint16_t n_songs;
} nd_music_artist;

/* And songs are a contiguous run within an album. */
typedef struct {
    char name[ND_MUSIC_TEXT_MAX];
    uint16_t artist;
    uint16_t year; /* 0 when no track in it carried one */
    uint16_t first_song;
    uint16_t n_songs;
} nd_music_album;

typedef struct nd_music_library nd_music_library;

/* Called every few tracks while the index builds. Return FALSE to cancel,
 * which makes nd_music_library_build() return ND_ERR_BUSY and free
 * everything. `done` counts tracks whose tag has been read. */
typedef bool (*nd_music_progress_fn)(void *ctx, size_t done, size_t total);

/* Reads a tag from each of `n` tracks and groups them.
 *
 * cb may be NULL. On success *out owns everything and is released with
 * nd_music_library_free(); on any failure *out is NULL and nothing leaks.
 *
 *   ND_OK          built, possibly with zero songs
 *   ND_ERR_BUSY    the callback asked to stop
 *   ND_ERR_NOMEM   the tables would not fit
 *   ND_ERR_INVAL   out was NULL, or tracks was NULL with n > 0 */
nd_err nd_music_library_build(nd_music_library **out, const nd_music_track *tracks, size_t n,
                              nd_music_progress_fn cb, void *ctx);
void nd_music_library_free(nd_music_library *lib);

size_t nd_music_library_n_songs(const nd_music_library *lib);
size_t nd_music_library_n_artists(const nd_music_library *lib);
size_t nd_music_library_n_albums(const nd_music_library *lib);

/* NULL past the end rather than a dummy row: every caller here is a loop
 * bounded by the counts above, and a silent empty row would hide an
 * off-by-one instead of crashing on it. */
const nd_music_song *nd_music_library_song(const nd_music_library *lib, size_t i);

/* The same songs in TITLE order, for the flat "Songs" list -- the view that
 * answers "I know what it is called and not who made it".
 *
 * A second index rather than a second copy: 2 bytes a track against 456, and
 * the album runs the browser navigates by have to stay contiguous in the
 * primary order. Ties break on artist and then path, so the order is total
 * and two tracks called "Intro" do not swap between visits. */
const nd_music_song *nd_music_library_song_by_title(const nd_music_library *lib, size_t i);
const nd_music_artist *nd_music_library_artist(const nd_music_library *lib, size_t i);
const nd_music_album *nd_music_library_album(const nd_music_library *lib, size_t i);

/* ------------------------------------------------------------------ *
 * The sort keys, exposed because they ARE the feature
 * ------------------------------------------------------------------ */

/* Case-insensitive ASCII compare, with one rule on top: the two "Unknown"
 * placeholders sort after everything else regardless of spelling, so an
 * untagged file does not land between Tycho and U2. Returns <0, 0 or >0. */
int32_t nd_music_name_cmp(const char *a, const char *b);

/* Album order within an artist: by YEAR first, oldest to newest, with an
 * unknown year last; then by name. A discography reads chronologically,
 * which is what a browser is for. */
int32_t nd_music_album_cmp(const nd_music_album *a, const nd_music_album *b);

/* Track order within an album: disc, then track number, then title, then
 * path. Every fallback matters -- a rip with no TRCK still has to be
 * deterministic, and two tracks with the same title still have to have an
 * order. */
int32_t nd_music_song_cmp(const nd_music_song *a, const nd_music_song *b);

/* ------------------------------------------------------------------ *
 * Metadata
 * ------------------------------------------------------------------ */

typedef struct {
    char title[ND_MUSIC_TEXT_MAX];
    char artist[ND_MUSIC_TEXT_MAX];
    char album[ND_MUSIC_TEXT_MAX];
    nd_image *art; /* owned; released by nd_music_meta_free() */
    double length; /* seconds; 0 when unknown */
} nd_music_meta;

/* get_metadata(). Never fails in a way the caller must handle: on any
 * problem the defaults stand, which is what the Python's `except Exception:
 * print(...)` leaves behind. */
void nd_music_get_metadata(const char *path, nd_music_meta *out);
void nd_music_meta_free(nd_music_meta *m);

/* find_folder_art(): ("cover","folder","front","album","albumart") x
 * (".jpg",".jpeg",".png"), in that nested order, matched case-insensitively
 * against the real directory entries. Owned by the caller. */
nd_image *nd_music_find_folder_art(const char *filepath);

/* miniaudio.get_file_info(path).duration, in seconds. 0 when the file is not
 * something dr_mp3 or dr_wav can open. */
double nd_music_duration(const char *path);

/* format_time(): "%02d:%02d" of (s / 60, s % 60) on an INT, so a track over
 * 99 minutes prints three digits for the minutes and the timestamp grows. */
void nd_music_format_time(int32_t seconds, char *out, size_t out_sz);

/* run_now_playing()'s own truncate().
 *
 * IT MEASURES `t` ON THE FIRST PASS AND `t + "..."` ON EVERY PASS AFTER.
 * That asymmetry is the Python's and is what makes a string that already fits
 * come back with no ellipsis at all, so it is reproduced exactly rather than
 * tidied into one uniform measurement -- measuring the ellipsis on the first
 * pass too would truncate strings that fit.
 *
 * A width nothing fits in returns "..." alone rather than looping, which is
 * the Python's `len(t) > 0` guard and is pinned in test_musicplayer.c.
 *
 * `t[:-1]` drops one CHARACTER, so the C drops one UTF-8 CODEPOINT; a
 * byte-wise chop would measure a different width and draw a replacement
 * glyph. Returns out. */
const char *nd_music_truncate(char *out, size_t out_sz, const char *text, const nd_font *f,
                              int32_t max_w);

/* ------------------------------------------------------------------ *
 * Playback
 * ------------------------------------------------------------------ */

/* play_file(): true when something is playing.
 *
 * The Python's fallback ladder, mapped onto what can actually go wrong here
 * (OPEN-QUESTIONS.md MU-5):
 *
 *   the decoder does not know the format,  -> this ONE track goes to mpv;
 *   and the name says it is audio             the session keeps STREAM
 *   (.flac, .ogg, .aac)
 *   the decoder does not know the format   -> false, no fallback. This is
 *   and the name says nothing                 miniaudio.DecodeError: the file
 *                                             is bad, the backend is fine
 *   aplay is missing, or a socket, thread  -> the session switches to mpv for
 *   or fork failed                            good, which is the Python's
 *                                             "device init failed" branch */
bool nd_music_play(const char *path);

void nd_music_stop(void);
void nd_music_toggle_pause(void);
bool nd_music_is_paused(void);

/* is_finished(): "no device, or the track ended". */
bool nd_music_is_finished(void);

/* position(): true and *out in seconds for STREAM; FALSE for mpv, which is
 * the Python's `return None` and is what makes the screen fall back to its
 * wall clock. */
bool nd_music_position(double *out);

/* The pid of whatever is making the noise -- aplay or mpv -- or -1. For the
 * test, and for the same reason nd_tones_preview_pid() exists. */
pid_t nd_music_child_pid(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_MUSIC_H_INCLUDED */
