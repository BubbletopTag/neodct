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
/* One record, decoded. False on timeout, on a short read, and on a descriptor
 * that has finished -- and `hung_up` is what tells those apart. It is set only
 * for the last: end of file on a pipe whose writer has gone, or a read error
 * that is not EINTR/EAGAIN on a device that has been unplugged. May be NULL.
 *
 * The distinction is load-bearing rather than tidy. ppoll() reports POLLHUP
 * and POLLERR whether or not they were asked for, so a dead descriptor is
 * ALWAYS ready: a caller that reads "false" as "nothing yet" and waits again
 * gets an immediate answer every time, for ever, at 100% of the phone's one
 * core. */
bool nd_evdev_read_record(int fd, double timeout_s, uint16_t *type, uint16_t *code, int32_t *value,
                          bool *hung_up);

#endif /* ND_INPUT_PRIV_H_INCLUDED */
