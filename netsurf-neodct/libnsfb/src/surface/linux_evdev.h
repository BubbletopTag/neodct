/*
 * Linux evdev keycode translation for the linux framebuffer surface.
 */

#ifndef LIBNSFB_LINUX_EVDEV_H
#define LIBNSFB_LINUX_EVDEV_H

#include <stdbool.h>
#include <stdint.h>

#include "libnsfb.h"
#include "libnsfb_event.h"

/**
 * Translate a linux input-event-codes keycode to an nsfb keycode.
 *
 * \param code evdev keycode (KEY_*)
 * \return nsfb keycode, or NSFB_KEY_UNKNOWN when unmapped
 */
enum nsfb_key_code_e nsfb_linux_evdev_to_nsfb(uint16_t code);

#endif
