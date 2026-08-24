/* keypadmapper.h -- what apps/KeypadMapper/main.c shows its unit test.
 *
 * "kmgpio" is KeypadMapper, the gpiozero flavour, so that nothing here can be
 * confused with libneodct's own nd_keymap_* or with the i2c sibling's
 * nd_kmi2c_*. main.c's header comment explains why this app is two dialogs
 * and a gate rather than a wizard.
 */

#ifndef ND_KEYPADMAPPER_H_INCLUDED
#define ND_KEYPADMAPPER_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO_REQUIRED_MSG and GPIOZERO_REQUIRED_MSG, verbatim. */
extern const char *const nd_kmgpio_required_msg;
extern const char *const nd_kmgpio_gpiozero_required_msg;

/* str(the ImportError), which is what run() interpolates into its log line.
 * On a shipped image the Python's `from gpiozero import Button, OutputDevice`
 * fails with exactly this, because gpiozero is in neither defconfig. */
extern const char *const nd_kmgpio_import_error;

/* _gpio_available(): len(glob("/dev/gpiochip*")) > 0. Goes through ND_ROOT
 * like every other path, so a host test can build a fake /dev. */
bool nd_kmgpio_available(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_KEYPADMAPPER_H_INCLUDED */
