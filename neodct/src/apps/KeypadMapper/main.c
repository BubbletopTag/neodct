/* apps/KeypadMapper/main.c -- the gpiozero keypad wizard, which is two
 * dialogs.
 *
 * A one-to-one port of System/engineering/apps/KeypadMapper/main.py. App id
 * 9002, engineering menu, manifest name "KeyMap".
 *
 * ============ WHY THIS IS NOT A WIZARD ============
 *
 * READ THIS BEFORE "FINISHING" THE PORT. main.py is 303 lines and most of
 * them cannot run. Its very first statement is
 *
 *     from gpiozero import Button, OutputDevice
 *
 * inside a try, and gpiozero is in NEITHER defconfig -- neodct_qemu_defconfig
 * and luckfox_pico_mini_defconfig enable PYTHON3, SSL, SQLITE, XZ, PILLOW,
 * MUTAGEN and MINIAUDIO, and nothing else. So on every image that has ever
 * shipped, GPIOZERO_IMPORT_ERROR is a string and run() is:
 *
 *     if not _gpio_available():            -> GPIO_REQUIRED_MSG, return
 *     if GPIOZERO_IMPORT_ERROR is not None: -> GPIOZERO_REQUIRED_MSG, return
 *
 * which is GPIO_REQUIRED_MSG under QEMU, where there are no gpiochips, and
 * GPIOZERO_REQUIRED_MSG on the Luckfox, where there are five of them and no
 * gpiozero. Those two dialogs are the whole observable behaviour of this app.
 *
 * Reproducing the scanner over /dev/gpiochip*'s GPIO_V2_GET_LINE_IOCTL would
 * turn a phone that today says "gpiozero is missing" into one that runs a
 * live enrolment wizard. That is a feature addition wearing a port's clothes,
 * and it is written down as such: spec-engineering.md's Risks table and
 * PORT-PLAN.md R-32 both say to default to the two dialogs and to raise a
 * live scanner as a declared deviation first. lib/nd_input.c made the same
 * call for the same reason -- try_open_matrix() refuses a keymap whose driver
 * is not "pcf8575-i2c" and logs "the %s driver is not supported."
 *
 * Everything the wizard half of main.py knows is therefore recorded here
 * rather than compiled, so that the information survives even though the code
 * does not. It is identical to the i2c sibling except for these:
 *
 *   DEFAULT_ROW_PINS = [21, 20, 16, 12]   BCM numbering, a Raspberry Pi
 *   DEFAULT_COL_PINS = [26, 19, 13,  6]   left-over; the Luckfox has neither
 *   MatrixScanner    rows are gpiozero OutputDevice(pin, initial_value=True),
 *                    columns are Button(pin, pull_up=True); scan_once() drives
 *                    one row low, sleeps 1 ms, and STOPS AT THE FIRST column
 *                    that reads pressed -- so, unlike the i2c scanner, it has
 *                    no key rollover and no release debounce, only a single
 *                    _held position it compares against for edge detection
 *   title            "Keypad Mapper", not "Keypad Mapper I2C"
 *   body lines 5+6   f"Rows: {row_pins}" and f"Cols: {col_pins}" -- no "P"
 *                    prefix, and two lines rather than one
 *   log prefix       "[KEYMAP]", and "[KEYMAP] GPIO matrix scanner ready."
 *   payload          "format": "neodct.keymap.v2.matrix",
 *                    "driver": "gpiozero-matrix", and NO i2c_bus/i2c_addr
 *   intro dialog     "This tool captures GPIO keypad matrix presses and
 *                    writes JSON to /NeoDCT/User/keymap.json."
 *
 * Every other line of it -- KEY_TARGETS, _parse_pins, _wrap_text, the prompt
 * geometry, the sixteen-target capture loop, the JSON -- is the same code as
 * apps/KeypadMapperI2C, which is a live app and where all of it is ported and
 * tested.
 *
 * ============ THE ONE THING C CANNOT SAY ============
 *
 * The Python's second gate asks whether an import failed. C has no import, so
 * the answer is decided at build time and is always "yes": there is no
 * gpiozero, and there is no libgpiod in the link map either. The log line
 * keeps CPython's own wording for the failure, because that is the string a
 * technician has read off the serial console every time this app has been
 * opened on real hardware.
 */

#include "nd_app.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "keypadmapper.h"

#include <glob.h>
#include <string.h>

#include "nd_paths.h"

/* main.py prints "[KEYMAP] ..."; ND_LOG_KEYMAP is that tag. */
#define KMGPIO_TAG ND_LOG_KEYMAP

const char *const nd_kmgpio_required_msg =
    "This app requires GPIO. GPIO devices not found. This application can not run in QEMU.";

const char *const nd_kmgpio_gpiozero_required_msg =
    "gpiozero is missing. Install python3-gpiozero to run keypad mapping.";

/* What CPython puts in the ImportError for a module that is not installed.
 * Reproduced literally so the serial console reads the same as it always
 * has -- see the header comment. */
const char *const nd_kmgpio_import_error = "No module named 'gpiozero'";

bool nd_kmgpio_available(void)
{
    char resolved[ND_PATH_MAX];
    glob_t g;
    bool any;

    /* glob.glob("/dev/gpiochip*") -- through nd_path_resolve so that ND_ROOT
     * applies, which is how a host test can present a /dev with and without
     * gpiochips. On the phone ND_ROOT is empty and this is the real path. */
    if (nd_path_resolve(resolved, sizeof resolved, "/dev/gpiochip*") != ND_OK)
        return false;

    memset(&g, 0, sizeof g);
    if (glob(resolved, 0, NULL, &g) != 0) {
        globfree(&g);
        return false;
    }
    any = g.gl_pathc > 0u;
    globfree(&g);
    return any;
}

int app_run(nd_ui *ui)
{
    nd_msgdialog dlg;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* run(ui), both gates, in the Python's order. Neither dialog carries a
     * title: MessageDialog(ui, MSG) with no title= is the untitled shape. */
    if (!nd_kmgpio_available()) {
        nd_msgdialog_init(&dlg, ui, nd_kmgpio_required_msg);
        (void)nd_msgdialog_show(&dlg);
        return 0;
    }

    nd_log(KMGPIO_TAG, "gpiozero import failed: %s", nd_kmgpio_import_error);
    nd_msgdialog_init(&dlg, ui, nd_kmgpio_gpiozero_required_msg);
    (void)nd_msgdialog_show(&dlg);
    return 0;
}

/* Nothing is opened: no gpiochip, no keymap file, no child. Exported anyway,
 * because nd_app.h requires it so a missing symbol always means the author
 * forgot rather than that there was nothing to do. */
void app_shutdown(void) {}
