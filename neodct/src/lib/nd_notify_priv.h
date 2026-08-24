/* nd_notify_priv.h -- the ringer's streaming decoder, exposed for its test.
 *
 * Not part of the public interface. nd_notify.h is what the rest of the
 * system sees; this is here for the same reason nd_modem_priv.h is -- the
 * interesting half of the module is a pull-decoder with a wrap-around cursor,
 * and a test that can only reach it through "the phone rang" is not a test of
 * the cursor.
 *
 * ============ WHY THIS EXISTS AT ALL ============
 *
 * The Python decodes the WHOLE ringtone into memory before it plays a note
 * (miniaudio.decode_file, 44100 Hz stereo int16). Tchaikovsky.mp3 is 36.3 s,
 * which is 6.4 MB resident in the core process from the moment the phone
 * rings until somebody answers -- measured, not estimated: 1740800 frames at
 * 48 kHz. That is larger than the entire memory budget for the rest of the
 * OS, and it is risk R-9 in spec-core-services.md.
 *
 * So this decodes as it plays. nd_tone_src is a pull source: it hands out
 * exactly as many 44.1 kHz stereo frames as the caller asks for, decoding a
 * few thousand source frames at a time and seeking back to frame 0 when the
 * file runs out. Peak cost is the decoder object plus two small buffers,
 * about 100 kB whatever the tone is.
 *
 * The loop is sample-exact, which is the property the Python's
 * _loop_generator was careful about and the reason it is worth restating:
 * the frame after the last frame of the file is frame 0, with no gap, no
 * silence and no fade, so a looped tone does not click.
 */

#ifndef ND_NOTIFY_PRIV_H_INCLUDED
#define ND_NOTIFY_PRIV_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 16384 stereo int16 frames == 65536 bytes. This is the "~64 KB ring buffer"
 * the whole exercise is about; it is also one send() to the player, and at
 * 44.1 kHz it is 372 ms of audio -- comfortably inside the 500 ms device
 * buffer the Python asked miniaudio for. */
#define ND_RING_CHUNK_FRAMES 16384u
#define ND_RING_CHUNK_BYTES  (ND_RING_CHUNK_FRAMES * 2u * sizeof(int16_t))

/* Source frames decoded per pull. Small enough that a stop() is never waiting
 * on a long decode, large enough that the per-call overhead disappears. */
#define ND_TONE_STAGE_FRAMES 4096u

typedef struct nd_tone_src nd_tone_src;

/* path is a REAL path -- already through nd_path_resolve(). The decoder opens
 * it with fopen and never sees a /NeoDCT prefix.
 *
 * ND_ERR_UNSUPPORTED when neither dr_mp3 nor dr_wav recognises the file,
 * which is the branch .wma / .flac / .ogg take and is what sends the ringer
 * to mpv -- exactly as the Python's miniaudio does today.
 *
 * Owned by the caller; free with nd_tone_src_close(). */
nd_err nd_tone_src_open(nd_tone_src **out, const char *path);
void nd_tone_src_close(nd_tone_src *s);

/* Fill out[] with `frames` INTERLEAVED STEREO 44100 Hz int16 frames, looping
 * the file forever. Returns the number of frames written, which is `frames`
 * unless the file decodes to nothing at all -- the C equivalent of the
 * Python generator's `if total == 0: return`. */
size_t nd_tone_src_read(nd_tone_src *s, int16_t *out, size_t frames);

/* What the file actually is, before conversion. For the test and the log. */
uint32_t nd_tone_src_rate(const nd_tone_src *s);
uint32_t nd_tone_src_channels(const nd_tone_src *s);

#ifdef __cplusplus
}
#endif

#endif /* ND_NOTIFY_PRIV_H_INCLUDED */
