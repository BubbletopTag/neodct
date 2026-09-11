/* nd_mic.h -- finding capture devices, and turning what they capture into
 * something a 240x175 screen can show.
 *
 * The engineering app MicTest is the only consumer today. The decisions live
 * here rather than in the app for the reason nd_remoteshell.h gives: what a
 * test can reach is what gets tested, and an app's .so is the awkward half.
 *
 * There is no in-process ALSA here and there is not meant to be. alsa-lib is
 * not in the image -- alsa-utils is -- so capture goes through arecord on a
 * pipe, exactly as call audio does in nd_modem_audio.c. That file is the
 * precedent for every choice below.
 */

#ifndef ND_MIC_H_INCLUDED
#define ND_MIC_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the kernel lists its sound cards. A parameter rather than a constant
 * at the call site below, because a test cannot write to the real one. */
#define ND_MIC_ASOUND_DIR "/proc/asound"

/* "plughw:10,0" is the longest realistic form by a wide margin. */
#define ND_MIC_DEVICE_MAX 32

/* One capture device the phone can record from. */
/* The kernel's own short name is the longest thing shown; 64 is generous. */
#define ND_MIC_LABEL_MAX 64

typedef struct {
    char device[ND_MIC_DEVICE_MAX]; /* what `arecord -D` takes */
    char label[ND_MIC_LABEL_MAX];   /* what the menu shows */
} nd_mic_device;

/* scan(): every capture device under `asound_root`, in the order the kernel
 * lists them. Returns how many were written to `out`.
 *
 * Capture, not playback: a card is interesting here only if it has a pcm*c
 * node. QEMU's USB Audio is playback-only card 0, so a phone with a real
 * microphone plugged in has cards that must be told apart -- which is the
 * whole reason this app exists.
 */
size_t nd_mic_scan(const char *asound_root, nd_mic_device *out, size_t max);

/* ------------------------------------------------------------------ *
 * The waveform
 * ------------------------------------------------------------------ */

/* One screen column: the lowest and highest sample that landed in it. A
 * waveform is drawn as a vertical line between the two, which is what makes a
 * loud passage a thick band and a quiet one a thin thread -- an average would
 * show neither. */
typedef struct {
    int16_t min;
    int16_t max;
} nd_mic_column;

/* reduce(): `n_samples` signed 16-bit mono samples into `columns` columns.
 * Returns how many columns were filled, which is fewer than asked for when
 * there are not enough samples to go round -- a partial screen is honest and
 * a stretched one is not. */
size_t nd_mic_reduce(const int16_t *samples, size_t n_samples, nd_mic_column *out, size_t columns);

/* ------------------------------------------------------------------ *
 * The capture command
 * ------------------------------------------------------------------ */

/* arecord, and the numbers that keep it cheap. 8 kHz mono is the modem's own
 * rate and is far more than a 240-pixel waveform can show; asking for 48 kHz
 * stereo would cost twelve times the bytes to draw the same picture on a phone
 * with 64 MB. */
#define ND_MIC_RATE     8000
#define ND_MIC_FORMAT   "S16_LE"
#define ND_MIC_ARGV_MAX 16

/* The command, and the buffer its numeric argument points into -- an argv of
 * `const char *` cannot own a formatted number, and a caller that formats it
 * into a local is a dangling pointer waiting to happen. */
typedef struct {
    const char *argv[ND_MIC_ARGV_MAX];
    char rate[16];
} nd_mic_command;

/* record_command(): `arecord` writing raw mono to stdout, for the given
 * device. Refuses an empty device rather than falling back to "default",
 * which on this hardware is a playback-only card. */
nd_err nd_mic_record_command(nd_mic_command *out, const char *device, int rate);

/* sample_y(): where a sample sits inside a band `height` pixels tall starting
 * at row `top`. Silence is the middle row, full positive is the top row and
 * full negative the bottom, because a waveform is drawn the way an
 * oscilloscope draws one -- up for positive. */
int32_t nd_mic_sample_y(int16_t sample, int32_t top, int32_t height);

/* ------------------------------------------------------------------ *
 * The mixer
 * ------------------------------------------------------------------ */

/* ============ WHY THIS IS HERE AND NOT IN amixer's HANDS ============
 *
 * The app's own header used to say "no gain control ... the mixer is amixer's
 * job", and for as long as the mixer was set once at boot that was true. It
 * is not any more, and the reason is a fault rather than a feature:
 *
 *   /etc/init.d/S17audio raises the capture switch and the capture volume
 *   ONCE, from its `start` case, reached only from rcS. There is no alsactl
 *   in this image, no asound.state, and no udev rule that re-runs it. Its own
 *   comment says "NOTHING ELSE IN THIS IMAGE DOES."
 *
 * A USB sound card that re-enumerates -- which nd_modem_audio.c already
 * records this one doing, about once per call -- comes back with the driver's
 * defaults. On the C-Media adapter the electret is soldered to, the driver's
 * defaults are a capture switch that is OFF and a capture volume of zero, and
 * arecord then returns a flat line with no error at all. Every layer reports
 * success and the far end hears nothing. That is the intermittent dead
 * microphone, and the fix is to stop treating the mixer as boot-time state:
 * both the call path and MicTest re-apply it before they open the device.
 *
 * ============ BY numid, NOT BY SIMPLE-MIXER NAME ============
 *
 * The obvious version of this is `amixer sset Mic 100%` and on this card it
 * is WRONG, for the reason S17audio spells out at length. A real C-Media
 * adapter (0d8c:0014) publishes "Mic Capture Volume" and "Mic Playback
 * Volume" as separate kernel controls, and ALSA's simple mixer merges them
 * into one control called "Mic" carrying both pvolume and cvolume -- so sset
 * raises the MONITOR path as well and feeds the microphone into the earpiece.
 * On a phone that is feedback, which is a worse bug than the silence.
 *
 * The kernel's own control names have no such ambiguity: a name ending
 * "Capture Switch" or "Capture Volume" is unambiguously the capture path.
 * Addressing it by numid also avoids quoting a name with spaces in it.
 *
 * ============ AUTO GAIN CONTROL IS STILL LEFT ALONE ============
 *
 * The same card offers one, it is one more numid, and it is deliberately not
 * exposed here. AGC on a cheap USB codec pumps: it rides the noise floor up
 * between words and ducks the first syllable after a pause, which on a voice
 * call sounds like a bad line rather than like a quiet one. It is a
 * sound-quality choice with no correct answer, where the gain below is a
 * correctness one -- a card at zero records nothing. If it is ever exposed it
 * belongs behind its own setting, defaulting OFF, with that tradeoff written
 * down; it does not belong wired to the level control.
 */

/* Percent, and the step the UP/DOWN keys move it by. 0 is a legal setting --
 * it is what a muted card reports and the owner is allowed to reproduce it --
 * so the floor is 0 and not 1. */
#define ND_MIC_GAIN_MIN  0
#define ND_MIC_GAIN_MAX  100
#define ND_MIC_GAIN_STEP 5

/* ALSA's own ceiling: snd_card_new() takes an index below SNDRV_CARDS, which
 * is 32. A card number outside that did not come from /proc/asound. */
#define ND_MIC_CARD_MAX 31

/* How many capture controls one card may have raised. Two ("Mic Capture
 * Switch", "Mic Capture Volume") is what the C-Media adapter offers; a codec
 * with a master capture control as well as a per-input one has four, and
 * BOTH have to move or the master holds the mic at zero. */
#define ND_MIC_CONTROLS_MAX 8

typedef enum {
    ND_MIC_CTL_SWITCH = 0, /* ...Capture Switch' -- on/off  */
    ND_MIC_CTL_VOLUME      /* ...Capture Volume' -- 0..100% */
} nd_mic_ctl_kind;

typedef struct {
    int32_t numid;
    nd_mic_ctl_kind kind;
} nd_mic_control;

/* parse_controls(): the capture controls in one `amixer -c N controls`
 * listing, in the order the card lists them. Returns how many were written.
 *
 * A line is
 *
 *     numid=6,iface=MIXER,name='Mic Capture Volume'
 *
 * and it counts only when the numid really is a number and the name really
 * ENDS with one of the two suffixes -- "Mic Playback Volume" contains neither
 * and must not, because that one is the monitor path. */
size_t nd_mic_parse_controls(const char *text, nd_mic_control *out, size_t max);

/* The amixer command, and the buffers its arguments point into. Same shape
 * and same reason as nd_mic_command above: an argv of `const char *` cannot
 * own a formatted number. */
#define ND_MIC_MIXER_ARGV_MAX 8

typedef struct {
    const char *argv[ND_MIC_MIXER_ARGV_MAX];
    char card[8];   /* "-c" takes the number on its own */
    char numid[24]; /* "numid=6" */
    char value[8];  /* "100%" */
} nd_mic_mixer_command;

/* controls_command(): `amixer -c N controls`, whose stdout parse_controls()
 * reads. */
nd_err nd_mic_controls_command(nd_mic_mixer_command *out, int32_t card);

/* cset_command(): `amixer -c N cset numid=K <value>` for one control.
 *
 * THE VALUE IS FORMATTED HERE AND NOWHERE ELSE. `percent` arrives from
 * settings.prop, which lives on the writable partition, and amixer takes
 * arguments -- so this is the choke point: a percent outside
 * ND_MIC_GAIN_MIN..ND_MIC_GAIN_MAX or a card outside 0..ND_MIC_CARD_MAX is
 * REFUSED with ND_ERR_INVAL rather than clamped, and nothing that came out of
 * a settings file is ever passed through as a string. `percent` is ignored
 * for a switch, which is always the literal "on". */
nd_err nd_mic_cset_command(nd_mic_mixer_command *out, int32_t card, const nd_mic_control *ctl,
                           int32_t percent);

/* gain_from_setting(): the stored string as a percentage that may be handed
 * to the builder above. Empty, NULL, non-numeric, negative, above 100 and
 * "80 dB" all give ND_MIC_GAIN_DEFAULT -- a settings file somebody has
 * hand-edited into nonsense must not be able to leave the microphone at zero,
 * which is the same rule system.ui.brightness follows for the same reason. */
#define ND_MIC_GAIN_DEFAULT 70
int32_t nd_mic_gain_from_setting(const char *value);

/* card_of(): the card number out of an `arecord -D` string. "plughw:2,0" is
 * 2. The same sscanf apps/Settings/main.c uses to find the Bluetooth fallback
 * card, and it is written out twice rather than shared because the two want
 * different things from a miss: Settings guesses 1, this refuses. */
bool nd_mic_card_of(const char *device, int32_t *out);

/* ============ THE BUDGET IS THE OPERATION'S, NOT EACH CHILD'S ============
 *
 * Every amixer child used to get its own two seconds and that was the number
 * written down -- but a bound on one child is not a bound on what the caller
 * pays, and only the caller's number matters here. apply_gain() spawns one
 * listing plus one cset per control found, up to ND_MIC_CONTROLS_MAX of
 * them; each child could cost two seconds of poll, two more of wait, and
 * then nd_proc_terminate()'s half a second of grace plus two seconds after
 * the SIGKILL. Nine children at six and a half seconds is fifty-eight, in
 * ONE call.
 *
 * The caller that would have paid it is nd_modem_audio.c's start_mic_pipe(),
 * on the modem thread, inside a single nd_modem_poll() tick -- and
 * nd_modem.c's submit() makes every UI-thread modem call block on done_cv
 * until that tick finishes. nd_modem_hangup() is one of them, so the END key
 * would have been dead for the whole of it. The owner is already chasing a
 * phone that freezes during a call and a tick that runs long with no deadline
 * is the leading explanation; adding fifty-eight unstated seconds to that
 * exact tick would have made the reported bug materially worse.
 *
 * So there is ONE deadline per entry point below, taken when it starts and
 * shared by every child it spawns -- the listing and all of the csets
 * together get ND_MIC_MIXER_BUDGET_S, checked before each child. Nothing in
 * this header can hold its caller longer than that plus ND_MIC_MIXER_REAP_S,
 * whatever the card does and however many controls it publishes.
 *
 * The hang is not hypothetical. snd_ctl_open() on a USB card halfway through
 * re-enumerating is the likeliest place for amixer to block, and that is the
 * same card at the same instant that start_mic_pipe() opens.
 *
 * A card that eats the budget is left half-set, and that is the right way
 * round: a quiet microphone is recoverable from the next call, and a phone
 * that will not answer the END key is not.
 *
 * ============ AND WHY HALF A SECOND, NOT TWO ============
 *
 * The number was 2.0, which made the aggregate 2.5 s, and 2.5 s of dead END
 * key is not a bound anybody should be pleased with -- it is the same fault
 * the owner is already reporting, one order of magnitude smaller. An amixer
 * that is going to answer answers in about 20 ms: it opens a control device,
 * writes one value and exits. 0.5 s is twenty-five times that, so no card
 * that is WORKING can notice this number at all; the only thing it changes is
 * how long a card that has WEDGED holds the modem thread, and there the whole
 * point is to give up early. The worst case the END key pays is now
 * ND_MIC_MIXER_BUDGET_S + ND_MIC_MIXER_REAP_S -- 1.0 s -- and that number is
 * written out at every place below that states an aggregate, because the
 * reason this note exists is that the last one was never multiplied out.
 */
#define ND_MIC_MIXER_BUDGET_S 0.5

/* ...and, at most once per entry point, this long to bury a child that blew
 * it. Deliberately not nd_proc_terminate()'s 0.5 s of grace plus 2.0 s after
 * the SIGKILL: that tail is exactly the unstated cost the budget exists to
 * remove, and an amixer wedged in D-state inside the USB stack will not
 * answer SIGKILL inside any tail that could be chosen. It is left to the
 * SIGCHLD reaper instead -- a zombie amixer is cheaper than a frozen END
 * key. */
#define ND_MIC_MIXER_REAP_S 0.5

/* list_controls(): ask the card what it has, once. `*n_out` receives how many
 * were written.
 *
 * Split out of apply_gain() for MicTest's UP and DOWN. A card cannot grow a
 * control between two keypresses, and the arrows repeat every 120 ms
 * (ND_REPEAT_INTERVAL_S), so re-listing on every press would be an amixer
 * child eight times a second for an answer that cannot have changed -- on a
 * single-core Cortex-A7 that is felt as a screen that has stopped responding.
 *
 * ND_ERR_NOTFOUND when the card publishes no capture control at all, which is
 * the honest answer for the RV1103's own codec rather than a failure to
 * report. ND_ERR_IO when amixer is not in this image or would not run.
 *
 * One child, and it returns within ND_MIC_MIXER_BUDGET_S +
 * ND_MIC_MIXER_REAP_S -- 1.0 s. The budget is what the child is given; the
 * reap is what burying it costs when it blew that and had to be SIGKILLed,
 * and it is paid at most once per entry point. Stating the budget alone here
 * was the same understatement the note above the constant was written about:
 * a bound that has to be multiplied out by its reader is a bound that gets
 * quoted wrong. */
nd_err nd_mic_list_controls(int32_t card, nd_mic_control *out, size_t max, size_t *n_out);

/* set_controls(): the csets, for controls already listed. Every switch in
 * `controls` goes on and every volume goes to `percent`, so a caller that
 * only wants to move the LEVEL passes only the volumes -- which is what the
 * key path does, because re-asserting a switch that is already on eight times
 * a second buys nothing and costs a fork.
 *
 * ND_ERR_IO when not one of them took. Individual refusals are logged and
 * skipped: a card that will not take one cset may still record.
 *
 * Every cset shares ONE ND_MIC_MIXER_BUDGET_S between them, so `n` controls
 * cost no more wall clock than one does; controls the budget did not reach
 * are named in the log and left as the driver had them. With the one reap
 * that a child which blew the budget costs, the whole call returns within
 * ND_MIC_MIXER_BUDGET_S + ND_MIC_MIXER_REAP_S -- 1.0 s, the same bound as the
 * other two entry points here.
 *
 * MicTest calls this on every UP and DOWN, so 1.0 s is also that app's worst
 * case for one keypress; apps/MicTest/main.c says so where the call is. */
nd_err nd_mic_set_controls(int32_t card, const nd_mic_control *controls, size_t n, int32_t percent);

/* apply_gain(): the two above, composed -- list the card's controls, turn
 * every capture switch on and set every capture volume to `percent`. What the
 * call path calls once per call, and what MicTest calls when its screen
 * opens. Synchronous, one listing plus one cset per control found, which is
 * what S17audio spends at boot.
 *
 * THE AGGREGATE BOUND, because this is the one the call path pays: the
 * listing and every cset share ONE deadline, so the whole call returns
 * within ND_MIC_MIXER_BUDGET_S + ND_MIC_MIXER_REAP_S -- 1.0 s -- whatever
 * the card does. It is stated here rather than left to be multiplied out of
 * a per-child number, which is how it came to be fifty-eight; see the budget
 * note above.
 *
 * Failures are logged and NOT fatal to the caller: a card that will not take
 * a gain may still record, and refusing to open it would turn a quiet
 * microphone into no microphone. */
nd_err nd_mic_apply_gain(int32_t card, int32_t percent);

#ifdef __cplusplus
}
#endif

#endif /* ND_MIC_H_INCLUDED */
