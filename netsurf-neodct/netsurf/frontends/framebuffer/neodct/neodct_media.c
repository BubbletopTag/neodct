/*
 * NeoDCT browser chrome: <video> elements.
 */

#include <string.h>

#include "neodct_media.h"

/* The label under the button, and the room one line of it needs. Matched
 * to the chrome's small font rather than measured: this runs during layout,
 * before there is a font to ask. */
#define LABEL_H 10

/* Clear air between the triangle and the label. Without it the two read as
 * one shape at this size. */
#define LABEL_GAP 3

/* The button is a fraction of the smaller side, so it stays in proportion
 * in a box of any shape, then clamped so it is neither a dot nor the whole
 * placeholder. */
#define BUTTON_NUM 2
#define BUTTON_DEN 5
#define BUTTON_MIN 7
#define BUTTON_MAX 48

static bool is_blank(const char *s)
{
	return s == NULL || s[0] == '\0';
}

const char *neodct_media_pick_src(const char *src,
				  const char *const *sources,
				  int n_sources)
{
	int i;

	if (!is_blank(src))
		return src;

	for (i = 0; i < n_sources; i++) {
		if (sources != NULL && !is_blank(sources[i]))
			return sources[i];
	}

	return NULL;
}

/* Extensions worth handing to mpv. Deliberately short: every entry here is
 * a file type that, if guessed wrong, takes the user out of the browser and
 * into a video player showing nothing. */
static const char *const playable_ext[] = {
	".avi", ".mp4", ".mkv", ".webm", ".mov", ".ndv", ".3gp", ".ogv",
	".mp3", ".wav", ".m4a", ".ogg", ".flac", ".aac", ".amr",
	".jpg", ".jpeg", ".png", ".gif", ".bmp"
};

static int ascii_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool ends_with_ext(const char *path, size_t len, const char *ext)
{
	size_t ext_len = strlen(ext);
	size_t i;

	if (len < ext_len)
		return false;

	for (i = 0; i < ext_len; i++) {
		if (ascii_lower((unsigned char)path[len - ext_len + i]) !=
		    ext[i])
			return false;
	}
	return true;
}

bool neodct_media_playable(const char *url)
{
	const char *cut;
	size_t len, i;

	if (is_blank(url))
		return false;

	/* Stop at the query and fragment; the extension is part of the path
	 * and "?id=4" must not be read as part of it. */
	len = strcspn(url, "?#");

	/* An extension only counts if it is in the last path segment --
	 * a host called videos.mp4.example.com is not a video. */
	cut = memchr(url, '/', len);
	if (cut != NULL) {
		const char *last = cut;
		const char *scan;
		for (scan = cut; scan < url + len; scan++) {
			if (*scan == '/')
				last = scan;
		}
		len -= (size_t)(last + 1 - url);
		url = last + 1;
	}

	for (i = 0; i < sizeof(playable_ext) / sizeof(playable_ext[0]); i++) {
		if (ends_with_ext(url, len, playable_ext[i]))
			return true;
	}

	return false;
}

/* The urls on the current page that are <video> sources. Fixed storage: box
 * construction runs while a page is being laid out on a phone with 64 MB,
 * and an allocation that fails there loses the whole page. */
static char tracked[NEODCT_MEDIA_MAX_TRACKED][NEODCT_MEDIA_MAX_URL + 1];
static int tracked_count;

void neodct_media_reset(void)
{
	tracked_count = 0;
	tracked[0][0] = '\0';
}

bool neodct_media_remember(const char *url)
{
	int i;

	if (is_blank(url))
		return false;
	if (strlen(url) > NEODCT_MEDIA_MAX_URL)
		return false;

	for (i = 0; i < tracked_count; i++) {
		if (strcmp(tracked[i], url) == 0)
			return true;
	}

	if (tracked_count >= NEODCT_MEDIA_MAX_TRACKED) {
		/* Full. The url still plays if its extension gives it away;
		 * this is a limit on how many extension-less videos one page
		 * can have, not on how many it can have. */
		return false;
	}

	strcpy(tracked[tracked_count], url);
	tracked_count++;
	return true;
}

bool neodct_media_is_media(const char *url)
{
	int i;

	if (is_blank(url))
		return false;

	for (i = 0; i < tracked_count; i++) {
		if (strcmp(tracked[i], url) == 0)
			return true;
	}

	return neodct_media_playable(url);
}

int neodct_media_triangle_row(int row, int rows, int width)
{
	int middle, distance;

	if (rows <= 0 || width <= 0)
		return 0;
	if (row < 0 || row >= rows)
		return 0;
	if (rows == 1)
		return width;

	/* Widest in the middle, narrowing to a point at both ends. Integer
	 * throughout: a rounded edge on a 240px panel is a ragged one. */
	middle = (rows - 1) / 2;
	distance = row > middle ? (rows - 1 - row) : row;

	return 1 + (width - 1) * distance / (middle > 0 ? middle : 1);
}

void neodct_media_layout(int width, int height,
			 struct neodct_media_layout *out)
{
	int size, label_h;

	memset(out, 0, sizeof(*out));
	if (width <= 0 || height <= 0)
		return;

	/* A page can size a <video> in CSS, which is past anything the
	 * attribute clamp can reach: VidLii asks for 640x360, and pages
	 * built for a desktop routinely ask for more. Centring the art in
	 * a box that wide would put it beyond the right edge of a 240px
	 * panel, leaving a black rectangle with nothing to say it is a
	 * video. So the art is placed in the part of the box that can
	 * actually be seen, which for a box starting at the left margin is
	 * the panel itself. The black fill still covers the whole box. */
	if (width > NEODCT_MEDIA_MAX_W)
		width = NEODCT_MEDIA_MAX_W;
	if (height > NEODCT_MEDIA_MAX_H)
		height = NEODCT_MEDIA_MAX_H;

	/* Try to fit the label; give it up before giving up the button. */
	label_h = LABEL_H + LABEL_GAP;
	if (height < label_h * 3)
		label_h = 0;

	size = (width < height ? width : height) * BUTTON_NUM / BUTTON_DEN;
	if (size < BUTTON_MIN)
		size = BUTTON_MIN;
	if (size > BUTTON_MAX)
		size = BUTTON_MAX;

	/* Whatever the proportions said, it has to fit in the box. */
	if (size > width)
		size = width;
	if (size > height - label_h) {
		size = height - label_h;
		if (size <= 0) {
			/* No room for both: the button is the part that
			 * tells the user this is a video at all. */
			label_h = 0;
			size = height;
		}
	}
	if (size > height)
		size = height;

	out->button_size = size;
	out->button_x = (width - size) / 2;
	out->button_y = (height - label_h - size) / 2;
	if (out->button_y < 0)
		out->button_y = 0;

	out->show_label = label_h > 0 &&
		out->button_y + size + label_h <= height;
	if (out->show_label) {
		out->label_h = label_h - LABEL_GAP;
		out->label_y = out->button_y + size + LABEL_GAP;
	}
}

void neodct_media_default_size(int attr_w, int attr_h, int *width, int *height)
{
	int w = attr_w > 0 ? attr_w : 0;
	int h = attr_h > 0 ? attr_h : 0;

	if (w == 0 && h == 0) {
		w = NEODCT_MEDIA_DEFAULT_W;
		h = NEODCT_MEDIA_DEFAULT_H;
	} else if (h == 0) {
		h = w * 3 / 4;
	} else if (w == 0) {
		w = h * 4 / 3;
	}

	/* Scale down rather than clip: a video box wider than the panel
	 * pushes the whole page sideways and nothing else on it can be
	 * read. */
	if (w > NEODCT_MEDIA_MAX_W) {
		h = h * NEODCT_MEDIA_MAX_W / w;
		w = NEODCT_MEDIA_MAX_W;
	}

	*width = w > 0 ? w : 1;
	*height = h > 0 ? h : 1;
}

bool neodct_media_argv(const char *url, const char **argv, int max)
{
	if (is_blank(url) || argv == NULL || max < 4)
		return false;
	if (strlen(url) > NEODCT_MEDIA_MAX_URL)
		return false;

	argv[0] = NEODCT_MEDIA_PLAYER;
	/* Everything after this is an operand. A src is text off a web page
	 * and "--parent 1" in one would otherwise be read as options. */
	argv[1] = "--";
	argv[2] = url;
	argv[3] = NULL;
	return true;
}

const char *neodct_media_exit_text(int status)
{
	switch (status) {
	case NEODCT_MEDIA_EXIT_OK:
		return NULL;
	case NEODCT_MEDIA_EXIT_NOPLAYER:
		return "No media player";
	case NEODCT_MEDIA_EXIT_NOLOAD:
		return "Could not load video";
	case NEODCT_MEDIA_EXIT_NONET:
		return "No connection";
	case NEODCT_MEDIA_EXIT_NOTFOUND:
		return "Video not found";
	case NEODCT_MEDIA_EXIT_FORMAT:
		return "Video format not supported";
	case NEODCT_MEDIA_EXIT_DIED:
		return "Player crashed";
	case 1:
		return "Player failed to start";
	default:
		/* mpv's 2 and 3, and anything nobody has named yet. Vague is
		 * still better than nothing: the user pressed play and is
		 * looking at the page again. */
		return "Could not play video";
	}
}
