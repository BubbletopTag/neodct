/*
 * NeoDCT browser chrome: fbtk shell.
 *
 * Builds the NeoDCT skin (slim URL bar, status bar, softkey bar,
 * Options menu overlay, input popup, keypad cursor) around the
 * stock framebuffer browser widget and routes all key input
 * through the tested neodct_ui state machine.
 */

#ifndef NEODCT_SHELL_H
#define NEODCT_SHELL_H

#include <stdbool.h>

struct gui_window;
struct fbtk_callback_info;

/* chrome layout for the 240x175 NeoDCT view */
#define NEODCT_URLBAR_H 20
#define NEODCT_STATUS_H 16
#define NEODCT_SOFTKEY_H 18

/** build the chrome; homepage is the url "Home" navigates to */
void neodct_shell_create(struct gui_window *gw, const char *homepage);

/**
 * Key input hook, called from the browser widget input callback.
 * Returns non-zero when the event was consumed.
 */
int neodct_shell_input(struct gui_window *gw,
		       struct fbtk_callback_info *cbi);

/** core reports the current url (also shown in the url bar) */
void neodct_shell_set_url(struct gui_window *gw, const char *url);

/** core reports pointer shape; caret means an editable field */
void neodct_shell_set_hover(struct gui_window *gw, bool editable);

/** page load started/finished (drives the status bar) */
void neodct_shell_load_start(struct gui_window *gw);
void neodct_shell_load_stop(struct gui_window *gw);

#endif
