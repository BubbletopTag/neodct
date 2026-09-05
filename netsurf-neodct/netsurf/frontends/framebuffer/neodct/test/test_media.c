/*
 * Tests for neodct_media: what the browser does with a <video>.
 *
 * NetSurf cannot play video and never will on this hardware, so a <video>
 * becomes a black box with a play button, and clicking it hands the url to
 * mpv. The pieces tested here are the ones with no netsurf types in them:
 * picking a source, marking a url as ours, and where the pixel art goes.
 */

#include <stdio.h>
#include <string.h>

#include "test_util.h"
#include "../neodct_media.h"

static void test_pick_src(void)
{
	const char *sources[3];

	/* the src attribute wins when it is there */
	sources[0] = "b.webm";
	CHECK_STR(neodct_media_pick_src("a.avi", sources, 1), "a.avi");

	/* otherwise the first <source> that looks playable */
	CHECK_STR(neodct_media_pick_src(NULL, sources, 1), "b.webm");

	/* an empty src is not a src */
	CHECK_STR(neodct_media_pick_src("", sources, 1), "b.webm");

	/* nothing at all */
	CHECK(neodct_media_pick_src(NULL, NULL, 0) == NULL);
	CHECK(neodct_media_pick_src("", NULL, 0) == NULL);

	/* a <source> list is taken in document order: the page author put
	 * the one they want first, and we have no codec table to argue
	 * with them about it. */
	sources[0] = "hd.mp4";
	sources[1] = "sd.avi";
	CHECK_STR(neodct_media_pick_src(NULL, sources, 2), "hd.mp4");

	/* blank entries are skipped rather than chosen */
	sources[0] = "";
	sources[1] = "sd.avi";
	CHECK_STR(neodct_media_pick_src(NULL, sources, 2), "sd.avi");
}

static void test_playable(void)
{
	/* things mpv can be handed */
	CHECK(neodct_media_playable("http://h/clip.avi") == true);
	CHECK(neodct_media_playable("http://h/clip.MP4") == true);
	CHECK(neodct_media_playable("/card/video/a.ndv") == true);
	CHECK(neodct_media_playable("http://h/song.mp3") == true);
	CHECK(neodct_media_playable("http://h/photo.jpg") == true);

	/* a query string does not hide the extension */
	CHECK(neodct_media_playable("http://h/clip.avi?id=4&t=9") == true);
	CHECK(neodct_media_playable("http://h/clip.avi#t=9") == true);

	/* and things it must not be: following one of these would take the
	 * user out of the browser to look at a web page in a video player */
	CHECK(neodct_media_playable("http://h/page.html") == false);
	CHECK(neodct_media_playable("http://h/") == false);
	CHECK(neodct_media_playable("http://h/avi") == false);
	CHECK(neodct_media_playable("http://h/x.avifoo") == false);
	CHECK(neodct_media_playable("") == false);
	CHECK(neodct_media_playable(NULL) == false);

	/* a dot in the host but not the path is not an extension */
	CHECK(neodct_media_playable("http://videos.mp4.example.com/watch")
	      == false);
}

static void test_registry(void)
{
	char url[64];
	int i;

	/* a <video> src is remembered so that clicking it plays rather than
	 * navigates, even when the url has no extension to go on */
	neodct_media_reset();
	CHECK(neodct_media_is_media("http://h/stream") == false);
	CHECK(neodct_media_remember("http://h/stream") == true);
	CHECK(neodct_media_is_media("http://h/stream") == true);
	CHECK(neodct_media_is_media("http://h/other") == false);

	/* remembering the same url twice does not use two slots */
	for (i = 0; i < NEODCT_MEDIA_MAX_TRACKED * 2; i++)
		CHECK(neodct_media_remember("http://h/stream") == true);
	CHECK(neodct_media_remember("http://h/second") == true);
	CHECK(neodct_media_is_media("http://h/second") == true);

	/* a new page starts with nothing remembered: a url that played on
	 * the last page must not hijack a link on this one */
	neodct_media_reset();
	CHECK(neodct_media_is_media("http://h/stream") == false);

	/* more videos than we can track: the page still works, the ones we
	 * could not fit simply fall back to the extension check */
	neodct_media_reset();
	for (i = 0; i < NEODCT_MEDIA_MAX_TRACKED + 8; i++) {
		snprintf(url, sizeof(url), "http://h/v%d", i);
		neodct_media_remember(url);
	}
	CHECK(neodct_media_is_media("http://h/v0") == true);
	CHECK(neodct_media_is_media("http://h/nope") == false);

	/* nothing here may be tricked into reading past a buffer */
	neodct_media_reset();
	CHECK(neodct_media_remember(NULL) == false);
	CHECK(neodct_media_remember("") == false);
	CHECK(neodct_media_is_media(NULL) == false);
	CHECK(neodct_media_is_media("") == false);

	/* a url too long to store is refused rather than truncated: half a
	 * url would match the wrong link */
	{
		char big[NEODCT_MEDIA_MAX_URL + 64];
		memset(big, 'a', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		CHECK(neodct_media_remember(big) == false);
		CHECK(neodct_media_is_media(big) == false);
	}
}

static void test_argv(void)
{
	const char *argv[8];
	char big[NEODCT_MEDIA_MAX_URL + 64];

	CHECK(neodct_media_argv("http://h/v.avi", argv, 8) == true);
	CHECK_STR(argv[0], NEODCT_MEDIA_PLAYER);
	/* "--" so that a src beginning with a dash cannot become an option
	 * to the player: the src is text off a web page */
	CHECK_STR(argv[1], "--");
	CHECK_STR(argv[2], "http://h/v.avi");
	CHECK(argv[3] == NULL);

	/* a url that looks like a flag survives intact */
	CHECK(neodct_media_argv("--parent", argv, 8) == true);
	CHECK_STR(argv[2], "--parent");

	/* no room, no command */
	CHECK(neodct_media_argv("http://h/v.avi", argv, 3) == false);
	CHECK(neodct_media_argv("http://h/v.avi", argv, 0) == false);

	CHECK(neodct_media_argv(NULL, argv, 8) == false);
	CHECK(neodct_media_argv("", argv, 8) == false);

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	CHECK(neodct_media_argv(big, argv, 8) == false);
}

static void test_exit_text(void)
{
	/* success says nothing: "Done." is the status bar's own word */
	CHECK(neodct_media_exit_text(NEODCT_MEDIA_EXIT_OK) == NULL);

	/* each named failure has its own line, short enough for the bar */
	CHECK_STR(neodct_media_exit_text(NEODCT_MEDIA_EXIT_NOPLAYER),
		  "No media player");
	CHECK_STR(neodct_media_exit_text(NEODCT_MEDIA_EXIT_NOTFOUND),
		  "Video not found");
	CHECK_STR(neodct_media_exit_text(NEODCT_MEDIA_EXIT_NONET),
		  "No connection");
	CHECK_STR(neodct_media_exit_text(NEODCT_MEDIA_EXIT_FORMAT),
		  "Video format not supported");
	CHECK_STR(neodct_media_exit_text(NEODCT_MEDIA_EXIT_NOLOAD),
		  "Could not load video");
	CHECK_STR(neodct_media_exit_text(NEODCT_MEDIA_EXIT_DIED),
		  "Player crashed");

	/* and nothing non-zero is ever silent: mpv's own codes, a status
	 * nobody has named, and a helper that never exited at all */
	CHECK(neodct_media_exit_text(NEODCT_MEDIA_EXIT_FAILED) != NULL);
	CHECK(neodct_media_exit_text(1) != NULL);
	CHECK(neodct_media_exit_text(3) != NULL);
	CHECK(neodct_media_exit_text(200) != NULL);
	CHECK(neodct_media_exit_text(NEODCT_MEDIA_EXIT_LOST) != NULL);

	/* the named lines are all different: two failures that read the
	 * same would be one failure the user cannot tell apart */
	{
		static const int codes[] = {
			NEODCT_MEDIA_EXIT_NOPLAYER, NEODCT_MEDIA_EXIT_NOLOAD,
			NEODCT_MEDIA_EXIT_NONET, NEODCT_MEDIA_EXIT_NOTFOUND,
			NEODCT_MEDIA_EXIT_FORMAT, NEODCT_MEDIA_EXIT_DIED,
			NEODCT_MEDIA_EXIT_FAILED
		};
		int i, j;

		for (i = 0; i < (int)(sizeof(codes) / sizeof(codes[0])); i++) {
			const char *a = neodct_media_exit_text(codes[i]);
			CHECK(strlen(a) <= 30);
			for (j = i + 1;
			     j < (int)(sizeof(codes) / sizeof(codes[0])); j++)
				CHECK(strcmp(a, neodct_media_exit_text(codes[j]))
				      != 0);
		}
	}
}

static void test_triangle(void)
{
	int rows = 11;
	int mid = rows / 2;

	/* a play triangle: a point on the right, a flat edge on the left,
	 * so the widest row is the middle one and the ends are thinnest */
	CHECK(neodct_media_triangle_row(mid, rows, 11) == 11);
	CHECK(neodct_media_triangle_row(0, rows, 11) <= 2);
	CHECK(neodct_media_triangle_row(rows - 1, rows, 11) <= 2);

	/* symmetric about the middle */
	for (int i = 0; i <= mid; i++) {
		CHECK_INT(neodct_media_triangle_row(i, rows, 11),
			  neodct_media_triangle_row(rows - 1 - i, rows, 11));
	}

	/* monotonic on the way up: no notches in the edge */
	for (int i = 1; i <= mid; i++) {
		CHECK(neodct_media_triangle_row(i, rows, 11) >=
		      neodct_media_triangle_row(i - 1, rows, 11));
	}

	/* off the ends draws nothing */
	CHECK_INT(neodct_media_triangle_row(-1, rows, 11), 0);
	CHECK_INT(neodct_media_triangle_row(rows, rows, 11), 0);

	/* degenerate sizes must not divide by zero or go negative */
	CHECK_INT(neodct_media_triangle_row(0, 0, 10), 0);
	CHECK_INT(neodct_media_triangle_row(0, 1, 10), 10);
	CHECK(neodct_media_triangle_row(0, 11, 0) == 0);
}

static void test_layout(void)
{
	struct neodct_media_layout l;

	/* the common case: a 240x175 <video> on a 240x175 screen */
	neodct_media_layout(240, 175, &l);
	CHECK(l.button_size > 0);
	/* centred, to within a pixel of rounding */
	CHECK(l.button_x * 2 + l.button_size >= 240 - 1);
	CHECK(l.button_x * 2 + l.button_size <= 240 + 1);
	/* button and label are centred as one group: the space above the
	 * button matches the space below the label */
	CHECK(l.button_y - (175 - (l.label_y + l.label_h)) <= 1);
	CHECK(l.button_y - (175 - (l.label_y + l.label_h)) >= -1);
	CHECK(l.show_label == true);
	CHECK(l.label_y > l.button_y + l.button_size);
	CHECK(l.label_y + l.label_h <= 175);

	/* a small box still gets a button, and drops the label rather than
	 * overlapping it */
	neodct_media_layout(40, 24, &l);
	CHECK(l.button_size > 0);
	CHECK(l.button_size <= 24);
	CHECK(l.show_label == false);

	/* nothing to draw in */
	neodct_media_layout(0, 0, &l);
	CHECK_INT(l.button_size, 0);
	CHECK(l.show_label == false);
	neodct_media_layout(-5, -5, &l);
	CHECK_INT(l.button_size, 0);

	/* the button never leaves the box, however odd the shape */
	int sizes[][2] = { {8, 200}, {200, 8}, {1, 1}, {3, 300}, {17, 19} };
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		neodct_media_layout(sizes[i][0], sizes[i][1], &l);
		CHECK(l.button_x >= 0);
		CHECK(l.button_y >= 0);
		CHECK(l.button_x + l.button_size <= sizes[i][0]);
		CHECK(l.button_y + l.button_size <= sizes[i][1]);
	}
}

static void test_layout_bigger_than_the_panel(void)
{
	struct neodct_media_layout l;

	/* A page can size a <video> in CSS, and the attribute clamp cannot
	 * reach that. VidLii asks for 640x360; centring the button in the
	 * box would put it at x=296, which is off the right of a 240px
	 * panel -- a black rectangle with no way to tell it is a video.
	 * The art is placed in the part of the box that is on screen. */
	neodct_media_layout(640, 360, &l);
	CHECK(l.button_size > 0);
	CHECK(l.button_x + l.button_size <= NEODCT_MEDIA_MAX_W);
	CHECK(l.button_y + l.button_size <= NEODCT_MEDIA_MAX_H);

	/* and still centred within that visible part */
	CHECK(l.button_x * 2 + l.button_size >= NEODCT_MEDIA_MAX_W - 1);
	CHECK(l.button_x * 2 + l.button_size <= NEODCT_MEDIA_MAX_W + 1);

	/* wide but short: height is honoured, width is capped */
	neodct_media_layout(1280, 90, &l);
	CHECK(l.button_x + l.button_size <= NEODCT_MEDIA_MAX_W);
	CHECK(l.button_y + l.button_size <= 90);

	/* tall but narrow: the other way round */
	neodct_media_layout(100, 2000, &l);
	CHECK(l.button_x + l.button_size <= 100);
	CHECK(l.button_y + l.button_size <= NEODCT_MEDIA_MAX_H);

	/* a box that fits is unaffected by any of this */
	neodct_media_layout(240, 175, &l);
	CHECK(l.button_x * 2 + l.button_size >= 239);
	CHECK(l.button_x * 2 + l.button_size <= 241);
}

static void test_default_size(void)
{
	int w, h;

	/* a <video> with no width/height gets the poster size browsers use */
	neodct_media_default_size(0, 0, &w, &h);
	CHECK_INT(w, NEODCT_MEDIA_DEFAULT_W);
	CHECK_INT(h, NEODCT_MEDIA_DEFAULT_H);

	/* attributes are honoured when they fit on the panel */
	neodct_media_default_size(200, 150, &w, &h);
	CHECK_INT(w, 200);
	CHECK_INT(h, 150);

	/* and scaled, keeping shape, when they do not */
	neodct_media_default_size(320, 240, &w, &h);
	CHECK_INT(w, NEODCT_MEDIA_MAX_W);
	CHECK_INT(h, 180);

	/* one given, one missing: keep 4:3 rather than a sliver */
	neodct_media_default_size(160, 0, &w, &h);
	CHECK_INT(w, 160);
	CHECK_INT(h, 120);
	neodct_media_default_size(0, 120, &w, &h);
	CHECK_INT(w, 160);
	CHECK_INT(h, 120);

	/* nothing wider than the screen: there is no horizontal scrolling
	 * worth having on a 240px panel */
	neodct_media_default_size(1920, 1080, &w, &h);
	CHECK(w <= NEODCT_MEDIA_MAX_W);
	CHECK(h < 1080);
	CHECK(h > 0);

	/* absurd input must not produce a negative box */
	neodct_media_default_size(-10, -10, &w, &h);
	CHECK(w > 0);
	CHECK(h > 0);
}

int main(void)
{
	test_pick_src();
	test_playable();
	test_registry();
	test_argv();
	test_exit_text();
	test_triangle();
	test_layout();
	test_layout_bigger_than_the_panel();
	test_default_size();
	TEST_EXIT();
}
