/* nd_media.h -- play one file with mpv, without letting it take the phone
 * down with it.
 *
 * The port of System/core/MediaWidget. NetSurf execs
 * /NeoDCT/System/core/MediaWidget/neodct-play when a <video> placeholder is
 * clicked -- the path is a compile-time constant in the browser fork
 * (netsurf-neodct/.../neodct_media.h), so the binary has to be exactly
 * there or the browser reports that it cannot play anything.
 *
 * mpv is by far the largest thing NeoDCT will ever start: decoding a video
 * costs more memory than the whole UI, and the browser it is usually
 * launched from is the second largest. Both alive at once does not fit in
 * 64 MB, so this module owns three decisions and every caller inherits
 * them.
 *
 *   * The framebuffer, never DRM. mpv 0.35 ships no fbdev output at all;
 *     the NeoDCT build adds one (--vo=fbdev), because the RV1103's display
 *     is an ST7789 behind a plain fbdev node with no DRM driver at all.
 *     Nothing here may ever fall back to --vo=drm: on this board that
 *     fails, and a failed video output means mpv holds the screen showing
 *     nothing.
 *
 *   * The calling application is STOPPED, not merely ignored. It is
 *     SIGSTOPped for exactly as long as mpv runs. That is about the CPU
 *     more than the memory: one core decodes 240x175 with very little to
 *     spare, and NetSurf's redraw loop happily eats what is left.
 *
 *   * Keys arrive over mpv's IPC socket rather than through mpv. The
 *     phone's keypad is an i2c port expander, and under QEMU the same code
 *     reads an evdev keyboard instead; forwarding from one place means mpv
 *     needs no idea which machine it is on. The C key is ALSO bound to
 *     quit in input.conf, so it works even when this bridge never starts --
 *     being unable to leave a video is being unable to use the phone.
 *
 * The socket carries traffic the other way too. mpv announces on it why a
 * file could not be played -- an end-file event with a reason, and the log
 * lines ffmpeg wrote on the way to it -- and that is turned into the exit
 * status below, which is the one word the browser gets to put on the
 * screen. Without it every failure on a phone with a flaky modem looked
 * like the same black flash back to the page.
 */

#ifndef ND_MEDIA_H_INCLUDED
#define ND_MEDIA_H_INCLUDED

#include <sys/types.h>

#include "nd_paths.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_MPV_BIN      "/usr/bin/mpv"
#define ND_MEDIA_FBDEV  "/dev/fb0"

/* Where the IPC socket goes. /tmp and not /run/neodct, because of who runs
 * this: the browser is ndusr_ut, confined, and /run is a root-owned 0755
 * tmpfs it cannot create anything in. The socket was silently never bound
 * there, mpv shrugged and played on, and every key pressed at it went
 * nowhere -- including C. /tmp is 1777 (nosuid,nodev; see etc/fstab) and
 * is not on the untrusted hide list, so it is the one place every caller
 * can write. The pid is in the name so that a socket a killed player left
 * behind under another uid -- unlinkable in a sticky directory -- cannot
 * block the next one. nd_media_ipc_socket_path() spells it. */
#define ND_MEDIA_IPC_SOCKET_DIR "/tmp"
#define ND_MEDIA_IPC_SOCKET_FMT ND_MEDIA_IPC_SOCKET_DIR "/neodct-mpv.%ld.sock"

/* Baked into the read-only rootfs beside the neodct-play binary. */
#define ND_MEDIA_INPUT_CONF "/NeoDCT/System/core/MediaWidget/input.conf"

/* The uinput keyboard the Browser bridges i2c presses onto. By the time mpv
 * starts, that bridge owns the i2c bus, so this is where the keypad is. */
#define ND_MEDIA_KEYPAD_UINPUT_NAME "neodct-t9-keypad"
#define ND_MEDIA_KEYPAD_DEVICE_ENV  "NEODCT_KEYPAD_DEVICE"

/* ------------------------------------------------------------------ *
 * Exit statuses
 * ------------------------------------------------------------------ */

/* What neodct-play exits with, and THE BROWSER DEPENDS ON EVERY VALUE:
 * netsurf-neodct/.../neodct_media.h carries the same table as
 * NEODCT_MEDIA_EXIT_* and turns each into the line the status bar shows
 * when the page comes back. test_mediawidget.c reads that header and fails
 * if the two ever disagree.
 *
 * 0 to 4 are mpv's own (0 played, 1 could not start, 2 could not play the
 * file, 3 played with errors, 4 quit by request). 80 and up are what this
 * program makes of mpv's IPC events when it can hear them, and are only
 * ever refinements of a 2. 127 is what a shell says for "command not
 * found", and what the child says when there is no mpv on the image. */
#define ND_MEDIA_EXIT_OK        0
#define ND_MEDIA_EXIT_FAILED    2   /* mpv could not play it and did not say why */
#define ND_MEDIA_EXIT_NOLOAD   80   /* the url could not be opened at all */
#define ND_MEDIA_EXIT_NONET    81   /* nothing answered: dns, connect, timeout */
#define ND_MEDIA_EXIT_NOTFOUND 82   /* the server answered, with a 4xx */
#define ND_MEDIA_EXIT_FORMAT   83   /* fetched, and this build cannot decode it */
#define ND_MEDIA_EXIT_DIED     84   /* mpv was killed -- the OOM killer, usually */
#define ND_MEDIA_ENOENT       127   /* no mpv on this image */

/* A short name for a status, for the serial log. Never NULL. */
const char *nd_media_exit_name(int status);

typedef enum {
    ND_MEDIA_VIDEO = 0,
    ND_MEDIA_IMAGE,
    ND_MEDIA_AUDIO
} nd_media_kind;

/* An mpv argv is short and bounded: the fixed options, at most two for an
 * image, one for the socket, then "--" and the url. */
#define ND_MEDIA_ARGV_MAX 32

typedef struct {
    const char *argv[ND_MEDIA_ARGV_MAX];
    size_t n;                 /* entries used, excluding the NULL terminator */
    /* The options that embed a path, held here so the argv can point at
     * them: "--input-conf=", "--fbdev-device=", "--input-ipc-server=", the
     * mpv path and the url. ND_PATH_MAX + 32 leaves room for the longest
     * option prefix in front of a maximum-length path. */
    char storage[6][ND_PATH_MAX + 32];
    size_t n_storage;
} nd_media_argv;

/* "video", "image" or "audio" for a url, guessing video when unsure.
 * Query and fragment are stripped first: a .mp4?token=... is still a mp4. */
nd_media_kind nd_media_kind_for(const char *url);

/* The full mpv command line for `url`. Everything is spelled out rather
 * than left to mpv's defaults: an appliance that behaves differently
 * depending on what happens to be installed is one that cannot be debugged
 * from a serial log. Pass ipc_socket NULL for no IPC socket. */
nd_err nd_media_build_argv(nd_media_argv *out, const char *url, nd_media_kind kind,
                           const char *fbdev, const char *ipc_socket, const char *mpv,
                           const char *input_conf);

/* The socket path for this process: ND_MEDIA_IPC_SOCKET_FMT with our pid. */
nd_err nd_media_ipc_socket_path(char *out, size_t out_sz);

/* The mpv command a NeoDCT keycode means, as a NULL-terminated argv, or
 * NULL when the key means nothing to mpv. The returned pointers are static
 * and must not be freed. */
const char *const *nd_media_ipc_command(int32_t keycode);

/* One mpv IPC request: compact JSON, newline terminated. Returns the byte
 * count written, or 0 if it would not fit. */
size_t nd_media_encode_command(const char *const *command, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * What mpv said
 * ------------------------------------------------------------------ */

/* Everything worth remembering from mpv's side of the socket. Filled in one
 * line at a time by nd_media_outcome_feed(); read once, at the end, by
 * nd_media_exit_status(). */
typedef struct {
    bool ended;         /* an end-file event arrived */
    bool error;         /* ...and its reason was "error" */
    bool format_error;  /* ...naming the format or the decoders, not the fetch */
    bool unreachable;   /* a log line said the network never answered */
    bool not_found;     /* a log line carried an HTTP 4xx */
} nd_media_outcome;

/* Feed one line of mpv's JSON IPC output -- an event, a log message, or
 * anything else, which is ignored. Returns true if the line changed
 * anything. A line that is not JSON, or is JSON of a shape mpv has never
 * sent, is simply not understood; it cannot crash this. */
bool nd_media_outcome_feed(nd_media_outcome *o, const char *line);

/* Feed one line of mpv's own terminal output (its stdout, mostly: only the
 * status line goes to stderr). It says the same things the IPC log
 * messages do, but from mpv's first line rather than from the moment a
 * client asked: a 404 from a fast server is logged and the file given up
 * on before the socket is even connected, and the IPC route sees only the
 * end. The pipe sees everything. */
bool nd_media_outcome_feed_text(nd_media_outcome *o, const char *text);

/* The status neodct-play exits with, given what mpv said and how it went:
 * `exited` false means it died of a signal, else `mpv_rc` is its exit
 * status. An mpv that played (0) or is not there (127) is reported as such
 * whatever the log said; a 2 ("could not play") is refined by whatever the
 * outcome learned, on the socket or on stderr. */
int nd_media_exit_status(const nd_media_outcome *o, bool exited, int mpv_rc);

/* ------------------------------------------------------------------ *
 * The keypad
 * ------------------------------------------------------------------ */

/* Which evdev device the keypad reaches us on. Prefers the NeoDCT keypad
 * bridge by name, then anything calling itself a keypad, then a keyboard,
 * so the same binary works on the phone and under QEMU. ND_ERR_NOTFOUND
 * when there is nothing to read. */
nd_err nd_media_discover_keypad(char *out, size_t out_sz);

/* O_RDONLY|O_NONBLOCK|O_CLOEXEC on an evdev node, or -1. Separate from
 * discover() so a caller can be told which device to use. */
int nd_media_open_keypad(const char *path);

/* Play `url` with mpv, returning an ND_MEDIA_EXIT_* status (ND_MEDIA_ENOENT
 * when there is no mpv). `suspend_pid` is stopped for as long as mpv runs;
 * pass 0 for none. `keypad_fd` is an open evdev descriptor to forward from,
 * or -1. The IPC socket is created whenever `ipc_socket` names one, keypad
 * or not, and mpv's stdout and stderr come back through a pipe either way:
 * between them they are how mpv's reasons come back. Every line mpv writes
 * is passed on to our own stderr, so the serial console loses nothing. */
int nd_media_play(const char *url, nd_media_kind kind, pid_t suspend_pid, int keypad_fd,
                  const char *mpv, const char *fbdev, const char *ipc_socket,
                  const char *input_conf);

#ifdef __cplusplus
}
#endif

#endif /* ND_MEDIA_H_INCLUDED */
