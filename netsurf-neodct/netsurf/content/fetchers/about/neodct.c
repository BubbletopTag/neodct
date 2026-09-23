/*
 * NeoDCT: the phone's theme as about:neodct.css.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"

#include "private.h"
#include "neodct.h"

/* owned here; replaced by fetch_about_neodct_set_stylesheet() */
static char *stylesheet;

nserror fetch_about_neodct_set_stylesheet(const char *css)
{
	char *copy = strdup(css != NULL ? css : "");

	if (copy == NULL)
		return NSERROR_NOMEM;
	free(stylesheet);
	stylesheet = copy;
	return NSERROR_OK;
}

bool fetch_about_neodct_handler(struct fetch_about_context *ctx)
{
	const char *css = (stylesheet != NULL) ? stylesheet : "";

	fetch_about_set_http_code(ctx, 200);

	if (fetch_about_send_header(ctx, "Content-Type: text/css; charset=utf-8"))
		return false;

	if (css[0] != '\0' &&
	    fetch_about_senddata(ctx, (const uint8_t *)css,
				 strlen(css)) != NSERROR_OK)
		return false;

	fetch_about_send_finished(ctx);
	return true;
}
