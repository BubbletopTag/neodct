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

/* /run is the tmpfs the rest of the system already publishes state into. */
#define ND_MEDIA_IPC_SOCKET "/run/neodct/mpv.sock"

/* Baked into the read-only rootfs beside the neodct-play binary. */
#define ND_MEDIA_INPUT_CONF "/NeoDCT/System/core/MediaWidget/input.conf"

/* The uinput keyboard the Browser bridges i2c presses onto. By the time mpv
 * starts, that bridge owns the i2c bus, so this is where the keypad is. */
#define ND_MEDIA_KEYPAD_UINPUT_NAME "neodct-t9-keypad"
#define ND_MEDIA_KEYPAD_DEVICE_ENV  "NEODCT_KEYPAD_DEVICE"

/* mpv exits 127 when it is not on this image at all -- the same status a
 * shell reports for "command not found", and what the caller distinguishes
 * playback from a build with no mpv in it by. */
#define ND_MEDIA_ENOENT 127

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
 * from a serial log. Pass ipc_socket NULL for no IPC bridge. */
nd_err nd_media_build_argv(nd_media_argv *out, const char *url, nd_media_kind kind,
                           const char *fbdev, const char *ipc_socket, const char *mpv,
                           const char *input_conf);

/* The mpv command a NeoDCT keycode means, as a NULL-terminated argv, or
 * NULL when the key means nothing to mpv. The returned pointers are static
 * and must not be freed. */
const char *const *nd_media_ipc_command(int32_t keycode);

/* One mpv IPC request: compact JSON, newline terminated. Returns the byte
 * count written, or 0 if it would not fit. */
size_t nd_media_encode_command(const char *const *command, char *out, size_t out_sz);

/* Which evdev device the keypad reaches us on. Prefers the NeoDCT keypad
 * bridge by name, then anything calling itself a keypad, then a keyboard,
 * so the same binary works on the phone and under QEMU. ND_ERR_NOTFOUND
 * when there is nothing to read. */
nd_err nd_media_discover_keypad(char *out, size_t out_sz);

/* O_RDONLY|O_NONBLOCK|O_CLOEXEC on an evdev node, or -1. Separate from
 * discover() so a caller can be told which device to use. */
int nd_media_open_keypad(const char *path);

/* Play `url` with mpv, returning its exit status (ND_MEDIA_ENOENT when
 * there is no mpv). `suspend_pid` is stopped for as long as mpv runs;
 * pass 0 for none. `keypad_fd` is an open evdev descriptor to forward from,
 * or -1 -- without one no IPC socket is created, because there would be
 * nothing on the other end of it. */
int nd_media_play(const char *url, nd_media_kind kind, pid_t suspend_pid, int keypad_fd,
                  const char *mpv, const char *fbdev, const char *ipc_socket,
                  const char *input_conf);

#ifdef __cplusplus
}
#endif

#endif /* ND_MEDIA_H_INCLUDED */
