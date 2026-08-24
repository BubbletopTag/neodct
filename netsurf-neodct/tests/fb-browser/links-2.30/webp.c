#include "cfg.h"

#ifdef G
#include "links.h"

#ifdef HAVE_WEBP

#include <webp/decode.h>

struct webp_decoder {
	unsigned char *buffer;
	int len;
};

static void webp_free(unsigned char *ptr)
{
#ifdef HAVE_WEBPFREE
	WebPFree(ptr);
#else
	free(ptr);
#endif
}

void webp_start(struct cached_image *cimg)
{
	struct webp_decoder *deco;
	deco = mem_alloc(sizeof(struct webp_decoder));
	deco->buffer = init_str();
	deco->len = 0;
	cimg->decoder = deco;
}

void webp_restart(struct cached_image *cimg, unsigned char *data, int length)
{
	struct webp_decoder *deco = (struct webp_decoder *)cimg->decoder;
	add_bytes_to_str(&deco->buffer, &deco->len, data, length);
}

void webp_finish(struct cached_image *cimg)
{
	int w, h;
	struct webp_decoder *deco = (struct webp_decoder *)cimg->decoder;
	unsigned char *pixels;
	pixels = WebPDecodeRGBA(deco->buffer, deco->len, &w, &h);
	if (!pixels)
		goto end;

	cimg->width = w;
	cimg->height = h;
	cimg->buffer_bytes_per_pixel = 4;
	cimg->red_gamma = cimg->green_gamma = cimg->blue_gamma = (float)sRGB_gamma;
	cimg->strip_optimized = 0;
	if (header_dimensions_known(cimg))
		goto end_free_pixels;

	memcpy(cimg->buffer, pixels, w * h * 4);
end_free_pixels:
	webp_free(pixels);
end:
	img_end(cimg);
}

void webp_destroy_decoder(struct cached_image *cimg)
{
	struct webp_decoder *deco = (struct webp_decoder *)cimg->decoder;
	mem_free(deco->buffer);
}

void add_webp_version(unsigned char **s, int *l)
{
	int v = WebPGetDecoderVersion();
	add_to_str(s, l, cast_uchar "WEBP (");
	add_num_to_str(s, l, (v >> 16));
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, (v >> 8) & 0xff);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, v & 0xff);
	add_chr_to_str(s, l, ')');
}

#endif

#endif
