/* nd_keypadsetup.h -- the first-boot on-screen keypad setup wizard, WP-27.
 *
 * A one-to-one port of System/hw/i2c_keypad_setup.py. nd_keypad.h names this
 * file's job in its "WHO NEEDS WHAT" table -- "the first-boot wizard (WP-27)
 * nd_pcf8575_*, nd_matrix_*, nd_keymap_save" -- and every primitive it lists
 * is used here rather than reimplemented.
 *
 * ============ WHAT IT IS FOR ============
 *
 * A fresh image has no /NeoDCT/User/keymap.json, so nd_keymap_load() reports
 * ND_ERR_NOTFOUND, so the core has no matrix backend, so the phone's only
 * input device does nothing. This wizard is what closes that loop without a
 * serial cable: it finds a PCF8575 on the setup bus, asks the owner to press
 * each of the sixteen keys once, works out from the observed pin pairs which
 * pins are rows and which are columns, and writes the keymap.
 *
 * ============ IT RUNS BEFORE THERE IS A UI ============
 *
 * The Python takes `fb`, NOT `ui`, and it is called from run() above the
 * `ui = NeoDCT_UI(fb)` line. So there is no nd_ui, no widget, no dialog and
 * no softkey bar available here -- nothing above nd_fb/nd_image/nd_draw/
 * nd_font exists yet. The screens below are therefore drawn exactly the way
 * the Python draws them: an nd_image canvas of its own, seven nd_draw calls,
 * and nd_fb_update() straight to the panel. That is not a simplification, it
 * is the only drawing there is at this point in the boot.
 *
 * `fb` may be NULL (nd-core --headless, and any host test that has no
 * panel). nd_fb_update() answers ND_ERR_INVAL for a NULL framebuffer and the
 * wizard ignores it, so the enrolment still runs and still writes the file.
 *
 * ============ THE FONTS ============
 *
 * SetupScreen loads the face at 26, 18 and 14 px. Twenty-six is NOT one of
 * the four sizes nd_font.h says the OS loads, and that is deliberate on both
 * sides: the wizard is not the UI, it has no golden frames, and shrinking its
 * one big label to 24 to fit a rule about the UI's glyph caches would move
 * pixels for no reason. The three faces are loaded on entry and freed on the
 * way out, so nothing survives into the UI's own four.
 *
 * A face that fails to load is left NULL and the text is simply not drawn --
 * nd_draw_text() refuses a NULL font and nd_text_size() measures it as zero.
 * The Python falls back to ImageFont.load_default() instead. Neither path is
 * reachable on a phone whose /NeoDCT/System is intact; on a host test with an
 * empty ND_ROOT it is the difference between a blank screen and a crash.
 *
 * ============ THE SPLIT, AND WHY ============
 *
 * Same split apps/KeypadMapperI2C/keypadmapper_i2c.h explains for the
 * engineering re-mapper: everything that DECIDES something -- the gates, the
 * bus parse, the scan arithmetic, the row/column bipartition, the keymap --
 * is drawing-free and directly assertable, and only the parts that put pixels
 * on a panel or bytes on a bus stay inside the two entry points.
 *
 * That app is the same wizard for an engineer who already has a working
 * phone; this one is the wizard for an owner who does not. Where the two
 * disagree the Python is the arbiter, and there is exactly one disagreement
 * that matters: KeypadMapperI2C renders its own payload text because it
 * carries "generated_at_unix" and a per-key "label" that nd_keymap has no
 * room for. This file writes through nd_keymap_save() instead, which is what
 * nd_keypad.h says that function is for ("the first-boot wizard's payload").
 * The consequence is stated plainly in nd_keypadsetup.c and is the one place
 * the bytes differ from CPython's.
 */

#ifndef ND_KEYPADSETUP_H_INCLUDED
#define ND_KEYPADSETUP_H_INCLUDED

#include "nd_fb.h"
#include "nd_input.h" /* nd_keymap */
#include "nd_keypad.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The constants, verbatim
 * ------------------------------------------------------------------ */

/* SETUP_BUS_ENV / DEFAULT_SETUP_BUS. Deliberately NOT the same variable as
 * KeypadMapperI2C's NEODCT_I2C_KEYPAD_BUS: that one overrides the bus an
 * already-configured phone scans, this one overrides the bus first boot
 * probes, and a phone can want them different exactly once. */
#define ND_KPSETUP_ENV_BUS     "NEODCT_KEYPAD_SETUP_BUS"
#define ND_KPSETUP_DEFAULT_BUS 3

/* PROBE_ADDRS = tuple(range(0x20, 0x28)) -- the eight addresses a PCF8575's
 * three address pins can select. LAST is inclusive; range()'s stop is not. */
#define ND_KPSETUP_PROBE_FIRST 0x20
#define ND_KPSETUP_PROBE_LAST  0x27

#define ND_KPSETUP_FIRST_KEY_TIMEOUT 120.0 /* FIRST_KEY_TIMEOUT             */
#define ND_KPSETUP_KEY_TIMEOUT       60.0  /* KEY_TIMEOUT                   */
#define ND_KPSETUP_RELEASE_SCANS     3     /* RELEASE_SCANS                 */

/* wait_release's max_seconds default, and the two time.sleep() cadences. */
#define ND_KPSETUP_RELEASE_MAX_S 10.0
#define ND_KPSETUP_POLL_S        0.01

/* time.sleep(0.0005) after driving a pin low, before reading. The same
 * settle nd_keypad.h spells ND_SCAN_SETTLE_US for the shipping scanner. */
#define ND_KPSETUP_SETTLE_US 500

/* The udev coldplug grace in maybe_run_first_time_setup(). */
#define ND_KPSETUP_BUS_WAIT_S 8.0
#define ND_KPSETUP_BUS_POLL_S 0.25

/* The root-phase bring-up's own budget -- see nd_kpsetup_open_keypad_as_root().
 * Shorter than the wizard's eight seconds because it sits in front of the home
 * screen on EVERY boot rather than on the one boot that has no keymap, and
 * because the only thing it can be waiting for is i2c-dev registering, which
 * the kernel does long before userspace gets this far. Six seconds is the
 * measured coldplug (~2.7 s) with slack on top. */
#define ND_KPSETUP_ROOT_WAIT_S 6.0
#define ND_KPSETUP_ROOT_POLL_S 0.05

/* How many times the bring-up opens the node, and how long it waits between
 * attempts. Three, spread over half a second: as root the open only fails if
 * the node is not really there, and the wait above has already established
 * that it is. */
#define ND_KPSETUP_ROOT_OPEN_TRIES 3
#define ND_KPSETUP_ROOT_OPEN_GAP_S 0.2

/* The on-screen dwells, in the order they appear. */
#define ND_KPSETUP_DWELL_INTRO_S   2.0
#define ND_KPSETUP_DWELL_ABORT_S   2.5
#define ND_KPSETUP_DWELL_FAILED_S  3.0
#define ND_KPSETUP_DWELL_SAVED_S   1.5
#define ND_KPSETUP_DWELL_NO_BUS_S  3.0
#define ND_KPSETUP_DWELL_NO_CHIP_S 3.0

/* UI_W, UI_H. The 240x175 band, not the 240x240 panel -- nd_fb_update()
 * centres it, exactly as it does for the UI's own canvas. */
#define ND_KPSETUP_UI_W 240
#define ND_KPSETUP_UI_H 175

/* SetupScreen's three faces. See the header comment for the 26. */
#define ND_KPSETUP_FONT_BIG_PX   26
#define ND_KPSETUP_FONT_PX       18
#define ND_KPSETUP_FONT_SMALL_PX 14

/* KEY_TARGETS, and the expander's sixteen pins. */
#define ND_KPSETUP_N_TARGETS 16
#define ND_KPSETUP_MAX_PINS  16

/* Sixteen pins can short in C(16,2) = 120 distinct unordered ways, which is
 * the most scan_pairs() can ever report. Every buffer below is that size, so
 * nothing in the scan path allocates. */
#define ND_KPSETUP_MAX_PAIRS 120

/* "key 'navikey' does not fit the matrix split" is the longest message either
 * side of this module produces, at 42 bytes. */
#define ND_KPSETUP_ERR_MAX 96

/* One drawn line. The longest is "Nothing answered on /dev/i2c-<bus>", whose
 * bus is an int32 from the environment, so 96 rather than 48. */
#define ND_KPSETUP_LINE_MAX 96

/* ------------------------------------------------------------------ *
 * The strings, verbatim
 * ------------------------------------------------------------------ */

extern const char *const nd_kpsetup_title;         /* "Keypad setup"        */
extern const char *const nd_kpsetup_press;         /* "Press:"              */
extern const char *const nd_kpsetup_press_each;    /* "Press each key..."   */
extern const char *const nd_kpsetup_aborted;       /* "Setup aborted"       */
extern const char *const nd_kpsetup_no_press;      /* "No key was pressed." */
extern const char *const nd_kpsetup_without;       /* "Starting without..." */
extern const char *const nd_kpsetup_failed;        /* "Setup failed"        */
extern const char *const nd_kpsetup_saved;         /* "Keymap saved!"       */
extern const char *const nd_kpsetup_restarting;    /* "Restarting UI..."    */
extern const char *const nd_kpsetup_no_bus_title;  /* "No keypad bus"       */
extern const char *const nd_kpsetup_no_chip_title; /* "No keypad found"     */
extern const char *const nd_kpsetup_format;        /* payload "format"      */
extern const char *const nd_kpsetup_driver;        /* payload "driver"      */

/* KEY_TARGETS. `name` is what keymap.json calls the key and what
 * nd_keycode_for_name() resolves; `label` is what the owner is told to press.
 * The order is the enrolment order and it is shared with the console builder
 * and with KeypadMapperI2C -- changing it changes which key someone is asked
 * for first on a phone that has never worked. */
typedef struct {
    const char *name;
    const char *label;
} nd_kpsetup_target;

extern const nd_kpsetup_target nd_kpsetup_targets[ND_KPSETUP_N_TARGETS];

/* ------------------------------------------------------------------ *
 * PairScanner
 * ------------------------------------------------------------------ */

/* One key, seen as the two expander pins it shorts together. ALWAYS a <= b:
 * the Python stores (min(drive, bit), max(drive, bit)) so that the same
 * switch found from either end is one entry in the set, and the whole
 * "already used by" check rests on that. */
typedef struct {
    uint8_t a;
    uint8_t b;
} nd_kpsetup_pair;

/* PairScanner.scan_pairs(): drive each of the sixteen pins low in turn, read
 * all sixteen back, and record every OTHER pin that came back low. No row/
 * column assumption is made anywhere -- that is the entire point, and it is
 * what lets this run on a phone nobody has measured yet.
 *
 * Deduplicated: a switch between P2 and P6 is seen twice per pass, once from
 * each end, and is one pair.
 *
 * Releases every pin (0xFFFF) on the way out -- but NOT after a bus failure,
 * because the Python's exception leaves that write unreached too.
 *
 * ND_ERR_IO when the bus failed, in which case *n_out is not written.
 * ND_ERR_TOOLONG if more than `max` distinct pairs are shorted at once. */
nd_err nd_kpsetup_scan_pairs(nd_pcf8575 *chip, nd_kpsetup_pair *out, size_t max, size_t *n_out);

/* PairScanner.wait_new_pair(): block until EXACTLY ONE key is pressed.
 *
 * Exactly one, not at least one: two keys held together are two pairs and are
 * ignored until one is let go, and a phantom third pair from a ghosting
 * three-key press is ignored the same way. That is what stops a mis-press
 * being enrolled as the wrong key.
 *
 * *found is false on timeout, which is the abort path. ND_ERR_IO on a bus
 * failure. */
nd_err nd_kpsetup_wait_new_pair(nd_pcf8575 *chip, double timeout_s, nd_kpsetup_pair *out,
                                bool *found);

/* PairScanner.wait_release(): scan until ND_KPSETUP_RELEASE_SCANS consecutive
 * passes see nothing, or max_seconds elapses. The timeout is NOT an error --
 * a stuck key gives up and the wizard carries on, exactly as the Python's
 * bare `while ... < deadline` does. */
nd_err nd_kpsetup_wait_release(nd_pcf8575 *chip, double max_s);

/* ------------------------------------------------------------------ *
 * _bipartition
 * ------------------------------------------------------------------ */

/* Two-colour the graph whose vertices are pins and whose edges are the
 * observed pairs: one colour class is the rows, the other is the columns. A
 * matrix keypad is bipartite by construction, so a colouring conflict means
 * the wiring is not a matrix (or a key was enrolled twice from a ghost).
 *
 * side_a and side_b come back SORTED ASCENDING and are the row and column pin
 * lists respectively; both buffers must hold ND_KPSETUP_MAX_PINS.
 *
 * Which class becomes "rows" is decided by the numerically smallest pin of
 * each connected component, which always takes colour 0 -- so the answer is
 * stable across runs, and it is arbitrary in exactly the way the Python's is.
 * Nothing downstream cares: the keymap records row_pins and col_pins, and
 * nd_matrix_scanner_init() drives whichever list it is handed.
 *
 * false when the graph is not bipartite; *conflict_a and *conflict_b then
 * name the two pins that clashed, in the order the traversal met them, and
 * feed the Python's "P{a}/P{b} conflict" message. CPython's own choice of
 * WHICH clashing edge to report depends on set iteration order and is not
 * reproducible; the pins named here are the first clash found scanning
 * neighbours in ascending order. Both pointers may be NULL. */
bool nd_kpsetup_bipartition(const nd_kpsetup_pair *pairs, size_t n, uint8_t *side_a, size_t *n_a,
                            uint8_t *side_b, size_t *n_b, uint8_t *conflict_a, uint8_t *conflict_b);

/* ------------------------------------------------------------------ *
 * _build_payload
 * ------------------------------------------------------------------ */

/* The enrolled pairs, indexed by position in nd_kpsetup_targets. `have[i]`
 * false is a target that was not enrolled -- which the completed wizard never
 * produces, but _build_payload tolerates and so does this. */
nd_err nd_kpsetup_build_keymap(nd_keymap *out, const nd_kpsetup_pair *pairs, const bool *have,
                               int bus, int addr, char *err, size_t err_sz);

/* ------------------------------------------------------------------ *
 * The gates
 * ------------------------------------------------------------------ */

typedef enum {
    ND_KPSETUP_GATE_HAVE_KEYMAP = 0, /* a keymap exists; announce and skip   */
    ND_KPSETUP_GATE_QUIET,           /* no bus, not hardware; skip silently  */
    ND_KPSETUP_GATE_WAIT_FOR_BUS,    /* hardware, but the node is not up yet */
    ND_KPSETUP_GATE_PROBE            /* the bus is there; go and probe it    */
} nd_kpsetup_gate;

/* Everything maybe_run_first_time_setup() decides before it draws anything or
 * sleeps for anything. */
nd_kpsetup_gate nd_kpsetup_gate_check(int bus);

/* int(os.environ.get(SETUP_BUS_ENV, DEFAULT_SETUP_BUS)).
 *
 * `raw` NULL is the unset variable and gives the default. Anything int()
 * would raise ValueError on -- including the EMPTY STRING, which this call
 * does not special-case the way KeypadMapperI2C's `or DEFAULT_BUS` does --
 * returns false, and the Python then unwinds out of the wizard entirely.
 * Setting NEODCT_KEYPAD_SETUP_BUS to nonsense therefore disables first-boot
 * setup rather than falling back to bus 3. Quirk, ported, asserted. */
bool nd_kpsetup_bus_from_env(const char *raw, int *out);

/* _probe_chip(): 0x20 through 0x27, opening each and demanding that a 0xFFFF
 * write and a read both succeed. ND_ERR_NOTFOUND when nothing answered, in
 * which case `chip` is left closed. On ND_OK the caller owns the open chip
 * and must nd_pcf8575_close() it. */
nd_err nd_kpsetup_probe(nd_pcf8575 *chip, int bus, int *addr_out);

/* ------------------------------------------------------------------ *
 * The lines that carry a substitution
 * ------------------------------------------------------------------ */

/* run_wizard()'s three-line opening message. */
size_t nd_kpsetup_found_lines(char out[][ND_KPSETUP_LINE_MAX], size_t max, int bus, int addr);

/* The "No keypad found" message's three lines. */
size_t nd_kpsetup_no_chip_lines(char out[][ND_KPSETUP_LINE_MAX], size_t max, int bus);

/* The "No keypad bus" message's two lines. */
size_t nd_kpsetup_no_bus_lines(char out[][ND_KPSETUP_LINE_MAX], size_t max, int bus);

/* f"Already used by '{label}'" -- the note under a repeated keypress. */
nd_err nd_kpsetup_used_note(char *out, size_t out_sz, const char *label);

/* f"{index + 1}/{total}", the counter in the prompt's top right. */
nd_err nd_kpsetup_counter(char *out, size_t out_sz, size_t index, size_t total);

/* ------------------------------------------------------------------ *
 * The wizard
 * ------------------------------------------------------------------ */

/* run_wizard(): the interactive enrolment, over an ALREADY OPEN chip. True
 * when a keymap was written.
 *
 * With `restart` true and a keymap written this re-execs nd-core and does not
 * return -- see nd_keypadsetup.c for what stands in for os.execv(sys.
 * executable, [sys.executable] + sys.argv).
 *
 * The chip is NOT closed on the ordinary return paths; the caller owns it,
 * exactly as maybe_run_first_time_setup()'s `finally` does. */
bool nd_kpsetup_run_wizard(nd_fb *fb, nd_pcf8575 *chip, int addr, int bus, bool restart);

/* ------------------------------------------------------------------ *
 * Bringing an ALREADY CONFIGURED keypad up, as root, before the drop
 * ------------------------------------------------------------------ */

/* ============ WHY THIS EXISTS, AND WHY THE WIZARD IS NOT IT ============
 *
 * nd_main.c calls nd_kpsetup_maybe_run() before the privilege drop and says
 * in its comment that the keypad's i2c probe therefore happens as root. On a
 * FRESH phone that is true. On every phone that has been through first boot
 * it is not: the first thing maybe_run() does is ask the gate, the gate
 * answers ND_KPSETUP_GATE_HAVE_KEYMAP the instant /NeoDCT/User/keymap.json
 * exists, and the function returns having opened nothing and waited for
 * nothing. So on every phone that HAS a working keypad, the privileged phase
 * of the boot never touched the bus, and the only open of /dev/i2c-3 for the
 * keypad happened inside nd_ui_init() -- after the drop, into the udev race,
 * with one attempt and no wait.
 *
 * This is the other half. It runs on exactly the phones the wizard skips: a
 * keymap exists, so there is nothing to enrol and everything to open. It
 * waits for the node, opens it, points it at the expander and makes the
 * expander answer, all while the process still has the privilege to do so --
 * and then hands the descriptor to nd_input_provide_keypad_fd(), which is
 * what lets the UI read keys after the drop without the node's group ever
 * mattering again.
 *
 * The verification is the point. Before this the phone did not know whether
 * it had a keypad until it had already given away the privilege to find out.
 *
 * Returns the descriptor, or -1. A descriptor is returned EVEN WHEN the
 * expander did not answer (`answered` false): the bus is open and privileged
 * either way, and an expander whose rail is still rising is exactly the case
 * a later retry can win -- but only if it has a descriptor to retry on.
 *
 * The caller owns the descriptor and should never close it: it must outlive
 * every nd_input built over it. */
typedef struct {
    int fd;                        /* -1 when nothing could be opened             */
    int bus;                       /* the keymap's i2c_bus                        */
    int addr;                      /* the keymap's i2c_addr                       */
    bool answered;                 /* the expander ACKed: the phone HAS a keypad  */
    double waited_s;               /* how long the node took to appear            */
    char why[ND_KPSETUP_LINE_MAX]; /* empty when fd >= 0 && answered              */
} nd_kpsetup_bringup;

int nd_kpsetup_open_keypad_as_root(nd_kpsetup_bringup *out);

/* maybe_run_first_time_setup(): the boot entry point, called from
 * core/nd_main.c before nd_ui_init(). A no-op unless this is a fresh image
 * (no keymap) with a PCF8575 answering on the setup bus.
 *
 * Never fails in a way the caller has to handle -- every failure is logged,
 * announced on screen where the Python announces it, and answered with false
 * so that boot carries on to a phone that at least draws. */
bool nd_kpsetup_maybe_run(nd_fb *fb, bool restart);

#ifdef __cplusplus
}
#endif

#endif /* ND_KEYPADSETUP_H_INCLUDED */
