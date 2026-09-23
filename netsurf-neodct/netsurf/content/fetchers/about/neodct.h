/*
 * NeoDCT: the phone's theme as about:neodct.css.
 *
 * The stylesheet is made by the frontend, which is the part of the
 * browser that knows the phone's theme; the core only serves it. So the
 * frontend hands it over once at startup and this keeps a copy, and the
 * about fetcher answers about:neodct.css with whatever it was given --
 * an empty sheet if nothing was, which leaves a page in its own styles.
 */

#ifndef NETSURF_CONTENT_FETCHERS_ABOUT_NEODCT_H
#define NETSURF_CONTENT_FETCHERS_ABOUT_NEODCT_H

#include <stdbool.h>

#include "utils/errors.h"

struct fetch_about_context;

/** replace the stylesheet served at about:neodct.css; copied */
nserror fetch_about_neodct_set_stylesheet(const char *css);

/** handler for about:neodct.css */
bool fetch_about_neodct_handler(struct fetch_about_context *ctx);

#endif
