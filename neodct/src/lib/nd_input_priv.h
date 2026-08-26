/* nd_input_priv.h -- what the four input translation units say to each other.
 *
 * Not a public header: nothing outside lib/ may depend on any of this. It
 * exists because nd_input.c needs the raw evdev record (releases included)
 * that nd_evdev_read_key() throws away, and -Wmissing-prototypes will not let
 * a non-static function go undeclared.
 */

#ifndef ND_INPUT_PRIV_H_INCLUDED
#define ND_INPUT_PRIV_H_INCLUDED

#include "nd_types.h"

/* One struct input_event, in whichever of the 24- and 16-byte layouts the
 * device speaks. timeout_s < 0 blocks. False on timeout or a short read. */
bool nd_evdev_read_record(int fd, double timeout_s, uint16_t *type, uint16_t *code, int32_t *value);

#endif /* ND_INPUT_PRIV_H_INCLUDED */
