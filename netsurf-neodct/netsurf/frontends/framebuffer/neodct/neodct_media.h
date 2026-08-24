/*
 * NeoDCT browser chrome: <video> elements.
 *
 * NetSurf has no media support and this phone could not decode a modern
 * codec if it did. So a <video> is laid out as a black box with a white
 * play button in it, and clicking that box hands the url to mpv through
 * /NeoDCT/System/core/MediaWidget rather than fetching it.
 *
 * The url is carried on the box as an ordinary link, so the whole of
 * netsurf's existing link handling -- hover, status text, click -- works on
 * it untouched, and the status bar shows the real url rather than something
 * invented. What makes it play instead of navigate is that box_video
 * remembers the url here; the shell asks before following any link.
 *
 * Remembering is needed because a src need not have an extension --
 * <video src="/stream"> is ordinary -- and the extension check alone would
 * navigate to it. The registry is a small fixed array rather than a list:
 * a page with more videos on it than fit is a page this phone was never
 * going to render, and those simply fall back to the extension check.
 *
 * Pure logic, no netsurf types, unit tested in test/.
 */

#ifndef NEODCT_MEDIA_H
#define NEODCT_MEDIA_H

#include <stdbool.h>

/** how many <video> urls one page can have remembered at once */
#define NEODCT_MEDIA_MAX_TRACKED 8

/** longest url the registry will hold */
#define NEODCT_MEDIA_MAX_URL 255

/** the helper that knows how to run mpv; see System/core/MediaWidget */
#define NEODCT_MEDIA_PLAYER "/NeoDCT/System/core/MediaWidget/neodct-play"

/** what a <video> with no width/height gets, in CSS pixels */
#define NEODCT_MEDIA_DEFAULT_W 240
#define NEODCT_MEDIA_DEFAULT_H 175

/** nothing wider than the panel: sideways scrolling is unusable here */
#define NEODCT_MEDIA_MAX_W 240

/** and nothing taller, for placing the art inside an oversized box */
#define NEODCT_MEDIA_MAX_H 175

/** where the pixel art goes inside a placeholder box */
struct neodct_media_layout {
	int button_x, button_y; /**< top left of the play triangle */
	int button_size;        /**< its height, and its width */
	int label_y, label_h;   /**< the "VIDEO" line under it */
	bool show_label;        /**< false when the box is too small for it */
};

/**
 * Choose which url a <video> actually plays.
 *
 * \param src      the src attribute, or NULL/"" if absent
 * \param sources  src attributes of child <source> elements, in document order
 * \param n_sources how many
 * \return  a pointer into one of the inputs, or NULL if there is nothing
 */
const char *neodct_media_pick_src(const char *src,
				  const char *const *sources,
				  int n_sources);

/** true when the url names something mpv should be handed, by extension */
bool neodct_media_playable(const char *url);

/** forget every remembered url; called when a new page is loaded */
void neodct_media_reset(void);

/** remember that this url is a <video> source on the current page */
bool neodct_media_remember(const char *url);

/** true when this url should be played rather than fetched */
bool neodct_media_is_media(const char *url);

/**
 * Build the argv that plays `url`.
 *
 * The strings are the player path, a literal "--" and `url` itself; only
 * the array is written, so `url` must outlive it.
 *
 * \param argv  filled in and NULL terminated
 * \param max   entries available in argv
 * \return false if the url is unusable or will not fit
 */
bool neodct_media_argv(const char *url, const char **argv, int max);

/** filled width of one row of a right-pointing play triangle */
int neodct_media_triangle_row(int row, int rows, int width);

/** place the play button (and label) inside a width x height box */
void neodct_media_layout(int width, int height,
			 struct neodct_media_layout *out);

/** the box a <video> occupies, from its width/height attributes (0 = unset) */
void neodct_media_default_size(int attr_w, int attr_h, int *width, int *height);

#endif
