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

/* NO_CARD_HELP and MPV_CMD, verbatim. */
extern const char *const nd_music_no_card_help;
extern const char *const nd_music_no_card_message;
#define ND_MUSIC_MPV_ARGC 7
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
