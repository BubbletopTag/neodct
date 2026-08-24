#include "cfg.h"

#ifdef G
#include "links.h"

#ifdef HAVE_AVIF

#include <avif/avif.h>

struct avif_decoder {
	unsigned char *buffer;
	int len;
};

void avif_start(struct cached_image *cimg)
{
	struct avif_decoder *deco;
	deco = mem_alloc(sizeof(struct avif_decoder));
	deco->buffer = init_str();
	deco->len = 0;
	cimg->decoder = deco;
}

void avif_restart(struct cached_image *cimg, unsigned char *data, int length)
{
	struct avif_decoder *deco = (struct avif_decoder *)cimg->decoder;
	add_bytes_to_str(&deco->buffer, &deco->len, data, length);
}

void avif_finish(struct cached_image *cimg)
{
	avifRGBImage ari;
	struct avif_decoder *deco;
	avifDecoder *decoder;

	memset(&ari, 0, sizeof(avifRGBImage));

	deco = (struct avif_decoder *)cimg->decoder;
	decoder = avifDecoderCreate();
	if (!decoder)
		goto end;
#if AVIF_VERSION > 90001
	decoder->strictFlags = AVIF_STRICT_DISABLED;
#endif
	decoder->ignoreExif = 1;
	decoder->ignoreXMP = 1;
	if (avifDecoderSetIOMemory(decoder, deco->buffer, deco->len) != AVIF_RESULT_OK)
		goto destroy_decoder;
	if (avifDecoderParse(decoder) != AVIF_RESULT_OK)
		goto destroy_decoder;

	cimg->width = decoder->image->width;
	cimg->height = decoder->image->height;
	cimg->buffer_bytes_per_pixel = 4;
	cimg->red_gamma = cimg->green_gamma = cimg->blue_gamma = (float)sRGB_gamma;
	cimg->strip_optimized = 0;

	if (avifDecoderNextImage(decoder) != AVIF_RESULT_OK)
		goto destroy_decoder;

	avifRGBImageSetDefaults(&ari, decoder->image);
	ari.depth = 8;
	ari.format = AVIF_RGB_FORMAT_RGBA;

	if (header_dimensions_known(cimg))
		goto destroy_decoder;

	ari.pixels = cimg->buffer;
	ari.rowBytes = (unsigned)cimg->width * 4;

	if (avifImageYUVToRGB(decoder->image, &ari) != AVIF_RESULT_OK)
		goto destroy_decoder;

destroy_decoder:
	avifDecoderDestroy(decoder);
end:
	img_end(cimg);
}

void avif_destroy_decoder(struct cached_image *cimg)
{
	struct avif_decoder *deco = (struct avif_decoder *)cimg->decoder;
	mem_free(deco->buffer);
}

void add_avif_version(unsigned char **s, int *l)
{
	add_to_str(s, l, cast_uchar "AVIF (");
	add_num_to_str(s, l, AVIF_VERSION_MAJOR);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, AVIF_VERSION_MINOR);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, AVIF_VERSION_PATCH);
#ifdef AVIF_VERSION_DEVEL
	if (AVIF_VERSION_DEVEL) {
		add_chr_to_str(s, l, '-');
		add_num_to_str(s, l, AVIF_VERSION_DEVEL);
	}
#endif
	add_chr_to_str(s, l, ')');
}

#endif

#endif
