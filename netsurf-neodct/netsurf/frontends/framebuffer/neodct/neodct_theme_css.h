/*
 * NeoDCT browser chrome: the phone's theme as a stylesheet.
 *
 * The browser serves this at about:neodct.css, so that a page can wear
 * the phone's theme the way the browser's own chrome does. It is the
 * one stylesheet both kinds of NeoDCT page use:
 *
 *   - the home page and any other page written for the phone link it:
 *       <link rel="stylesheet" href="about:neodct.css">
 *     and get the ground and ink on html/body, form controls in the
 *     theme's colours, and the classes below;
 *
 *   - netsurf's own pages (fetch errors, timeouts, certificate warnings,
 *     logins) import it from internal.css, and a section at the end
 *     restyles them as phone screens: a title strip, the ground, the
 *     buttons as the selection plate. Those rules are !important because
 *     they have to beat internal.css's desktop layout at any specificity.
 *
 * The classes (the API; keep them stable, pages depend on them):
 *
 *   .nd-bar       a title strip: bar colours, bar ink, rule under it
 *   .nd-panel     a panel: glass colours and the ink that reads on them
 *   .nd-tile      a link drawn as a panel, for a grid of shortcuts
 *   .nd-selected  the selection: the signature colour and its ink
 *   .nd-button    a button (button and input[type=submit] get it too)
 *   .nd-field     a text field (text and password inputs get it too)
 *   .nd-muted     secondary type
 *   .nd-ink       full-strength type, e.g. inside something muted
 *   .nd-rule      a rule above: the theme's divider colour
 *
 * netsurf's CSS has no gradients and no rounded corners, so every
 * gradient the phone draws comes out as the colour at its middle. The
 * classic face has no gradients either, so it renders exactly.
 *
 * Pure, no netsurf dependencies, unit tested in test/.
 */

#ifndef NEODCT_THEME_CSS_H
#define NEODCT_THEME_CSS_H

#include <stddef.h>

#include "neodct_theme.h"

/** the url pages link it by */
#define NEODCT_THEME_CSS_URL "about:neodct.css"

/** comfortably more than the sheet needs */
#define NEODCT_THEME_CSS_MAX 8192

/**
 * Write the stylesheet for theme t into out.
 *
 * \return the length written, or 0 if it did not fit
 */
size_t neodct_theme_css(const struct neodct_theme *t, char *out,
			size_t out_sz);

#endif
