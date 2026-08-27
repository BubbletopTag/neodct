/* nd_btaudio.h -- where sound goes when earbuds arrive, and when they leave.
 *
 * The routing is one file. /etc/asound.conf is a symlink into /run, and
 * /etc/init.d/S17audio writes that file at boot pointing ALSA's "default" at
 * the USB sound card. Everything on this phone that makes a noise opens
 * "default", so rewriting that one file moves every sound at once -- there is
 * no per-app audio setting to keep in step, and no mixer daemon to ask.
 *
 * ============ WHY PLAYBACK STOPS ON A SWITCH ============
 *
 * dmix is bypassed (S17audio explains why: this kernel has no ALSA timer), so
 * exactly one program holds the card at a time. A player that already has
 * hw:1,0 open keeps playing into the speaker no matter what the config file
 * says afterwards -- rewriting it changes what the NEXT open resolves to and
 * nothing else. So a switch stops what is playing. That makes the switch true
 * rather than merely written down, and it is the owner's choice: see the
 * changelog for 0.4.4a.
 */

#ifndef ND_BTAUDIO_H_INCLUDED
#define ND_BTAUDIO_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Written by S17audio at boot and rewritten by this module. NOT /etc: that is
 * read-only squashfs, and /etc/asound.conf is a symlink here. */
#define ND_BTAUDIO_ASOUND "/run/asound.conf"

/* A Bluetooth address as ALSA and bluealsa spell it: "AA:BB:CC:DD:EE:FF". */
#define ND_BTAUDIO_ADDR_MAX 18

/* Enough for either body with a long address in it. */
#define ND_BTAUDIO_CONF_MAX 512

/* asound_text(): the body of asound.conf that sends "default" where it should
 * go. `addr` NULL or empty means the speaker -- the USB card, exactly as
 * S17audio writes it, because reverting has to land on the same thing the boot
 * script would have written rather than on something merely similar.
 */
nd_err nd_btaudio_asound_text(char *out, size_t n, const char *addr, int card);

/* ------------------------------------------------------------------ *
 * Reading what bluetoothctl says
 * ------------------------------------------------------------------ *
 *
 * Pairing is Secure Simple Pairing with a link key to keep, and an A2DP
 * connection is AVDTP signalling on top of that. None of it is a kernel
 * ioctl, so this module does not attempt it: it drives bluetoothctl and reads
 * the output, the same shape nd_modem_audio.c uses for arecord and
 * nd_remoteshell.c for ssh. The protocol knowledge stays out of our process.
 *
 * The parsing is here, and pure, because the parsing is where this gets things
 * wrong -- a name with a space in it, a device with no name at all, the colour
 * codes and [NEW] markers bluetoothctl mixes into its own output.
 */

/* Long enough for "Skullcandy Indy ANC" and a good deal more. The screen runs
 * out at about twenty characters anyway. */
#define ND_BTAUDIO_NAME_MAX 48

typedef struct {
    char addr[ND_BTAUDIO_ADDR_MAX];
    char name[ND_BTAUDIO_NAME_MAX];
} nd_btaudio_device;

/* parse_devices(): every "Device <addr> <name>" line in `text`, in order.
 * Returns how many were written. Lines that are not device lines are skipped
 * rather than treated as errors -- bluetoothctl prints a good deal else. */
size_t nd_btaudio_parse_devices(const char *text, nd_btaudio_device *out, size_t max);

/* parse_connected(): whether `bluetoothctl info <addr>` says this device is
 * connected right now.
 *
 * The trap is that the same output carries "Paired: yes" and "Trusted: yes",
 * and a device can be all three of paired, trusted and NOT connected -- which
 * is exactly the state earbuds are in sitting in their case. Matching on "yes"
 * would route audio into a closed box. */
bool nd_btaudio_parse_connected(const char *text);

/* ------------------------------------------------------------------ *
 * Driving bluetoothctl
 * ------------------------------------------------------------------ */

#define ND_BTAUDIO_BTCTL    "/usr/bin/bluetoothctl"
#define ND_BTAUDIO_ARGV_MAX 8

/* The IO capability this phone honestly has: no keypad to type a passkey on
 * and no display to show one. It selects the "Just Works" association model,
 * which BlueZ auto-confirms for an outgoing pairing -- so the agent has to
 * exist, but never has to be asked anything. Without an agent registered at
 * all, BlueZ cancels the pairing outright. */
#define ND_BTAUDIO_AGENT    "NoInputNoOutput"

#define ND_BTAUDIO_BLUEALSA "/usr/bin/bluealsa"

/* The command, and the buffer its numeric argument points into. An argv of
 * `const char *` cannot own a formatted number and a caller that formats it
 * into a local is a dangling pointer waiting to happen -- the same reason
 * nd_mic_command carries its rate. */
typedef struct {
    const char *argv[ND_BTAUDIO_ARGV_MAX];
    char seconds[8];
} nd_btaudio_cmd;

/* bluealsa_cmd(): the daemon that carries the audio, and the one flag that
 * decides which way it flows. */
nd_err nd_btaudio_bluealsa_cmd(nd_btaudio_cmd *out);

/* cmd_build(): `bluetoothctl [--timeout N] <verb> [arg...]`.
 *
 * `timeout_s` <= 0 leaves --timeout off, which is right for the verbs that
 * finish by themselves (devices, info, connect). Scanning does not finish by
 * itself: `scan on` runs until something stops it, so the timeout IS the scan
 * length and leaving it off would hang the phone on the Scanning screen. */
nd_err nd_btaudio_cmd_build(nd_btaudio_cmd *out, const char *verb, const char *arg, int timeout_s);

/* ------------------------------------------------------------------ *
 * Moving the sound
 * ------------------------------------------------------------------ */

/* What the speaker route was, kept while the earbuds have the default. */
#define ND_BTAUDIO_ASOUND_SAVED "/run/asound.conf.speaker"

/* route_to(): point ALSA's "default" at `addr`, or back at the speaker when
 * `addr` is NULL.
 *
 * Going to the earbuds SAVES the existing asound.conf first, and coming back
 * RESTORES that file rather than regenerating one. Regenerating would mean
 * re-deriving which card is the USB one -- S17audio picks it by usbid at boot,
 * and a phone whose card numbering moved between then and now would come back
 * to a different card than it left. Keeping the bytes cannot drift.
 *
 * Restoring when nothing was saved falls back to writing the speaker route for
 * `card`, which is better than leaving the earbud config in place after the
 * earbuds have gone.
 */
nd_err nd_btaudio_route_to(const char *addr, int card);

/* ------------------------------------------------------------------ *
 * The daemons, and running a command
 * ------------------------------------------------------------------ *
 *
 * ============ WHAT NO UNIT TEST HERE COVERS ============
 *
 * Everything below forks a real process. Starting bluetoothd on a developer's
 * machine would fight the one their desktop is already running, and a test
 * that leaves a daemon behind on the machine it ran on is not a test. The hole
 * is named here rather than discovered, exactly as remote_app.h names the one
 * around nd_rs_start(). What covers this is running it: Settings -> BT Audio
 * on a phone with a dongle in it.
 *
 * NOTHING here starts at boot. Measured on a booted phone: dbus 519 KB PSS,
 * bluetoothd 2.1 MB, bluealsa 3.5 MB -- about 6 MB of the 54 MB this phone
 * has. That is affordable while you are listening to music and not affordable
 * as a permanent tax, so Enable starts them and Disable stops them.
 */

/* start(): dbus and bluetoothd, in that order, if they are not already up.
 * bluealsa is NOT started here -- it is the biggest of the three and is only
 * needed once something is connected. */
nd_err nd_btaudio_daemons_start(void);

/* stop(): all three, and the routing goes back to the speaker. */
void nd_btaudio_daemons_stop(int card);

/* bluealsa, started when a device connects and stopped when it goes. */
nd_err nd_btaudio_bluealsa_start(void);

/* run(): a built command, with up to `cap_n` bytes of its stdout. Returns the
 * exit status, or -1 if it could not be started. */
int nd_btaudio_run(const nd_btaudio_cmd *cmd, char *capture, size_t cap_n);

/* True when an adapter exists and is UP. */
bool nd_btaudio_adapter_up(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_BTAUDIO_H_INCLUDED */
