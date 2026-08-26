/* GENERATED -- do not edit by hand.
 *
 * Produced by running the real Framebuffer class out of
 * System/core/main.py against a fake mmap; see tools/gen_fb_ref.py.
 * Each entry is the FNV-1a 64 hash of the WHOLE mapping after one
 * update(), so a difference anywhere -- band, letterbox or row padding
 * -- fails the case.
 *
 * Regenerate with:  python3 tools/gen_fb_ref.py > test/unit/fb_ref.inc
 */

typedef struct {
    const char *name;
    int32_t xres;
    int32_t yres;
    int32_t bpp;
    size_t line_length;
    int32_t sw;
    int32_t sh;
    bool src_rgba;
    size_t map_size;
    uint64_t hash;
} nd_fb_ref;

static const nd_fb_ref ND_FB_REF[] = {
    {"live_240x175_32", 240, 175, 32, 960u, 240, 175, false, 168000u, 0xf38f825a4be747f8ULL},
    {"panel_240x240_32", 240, 240, 32, 960u, 240, 175, false, 230400u, 0x5b6b344c78cb2ff8ULL},
    {"wide_320x240_32", 320, 240, 32, 1280u, 240, 175, false, 307200u, 0x3b318be330b97878ULL},
    {"stride_pad_240x175_32", 240, 175, 32, 976u, 240, 175, false, 170800u, 0x4287c4fd7831c0f8ULL},
    {"crop_240x175_from_320x240_32", 240, 175, 32, 960u, 320, 240, false, 168000u, 0x06487139bb08a318ULL},
    {"crop_240x175_from_240x240_32", 240, 175, 32, 960u, 240, 240, false, 168000u, 0xe7bcc227e5a15eb5ULL},
    {"rgba_source_240x175_32", 240, 175, 32, 960u, 240, 175, true, 168000u, 0xf38f825a4be747f8ULL},
    {"live_240x175_16", 240, 175, 16, 480u, 240, 175, false, 84000u, 0xc9e6e06a040dc337ULL},
    {"panel_240x240_16", 240, 240, 16, 480u, 240, 175, false, 115200u, 0xb7e35d372a1c1fb7ULL},
    {"crop_240x175_from_240x240_16", 240, 175, 16, 480u, 240, 240, false, 84000u, 0xc243f8ec82024e17ULL},
    {"raw_240x175_24", 240, 175, 24, 720u, 240, 175, false, 126000u, 0x4b57fa19f5463fe6ULL},
    {"raw_panel_240x240_24", 240, 240, 24, 720u, 240, 175, false, 172800u, 0x072e3cc65c162366ULL},
    {"odd_7x9_32", 7, 9, 32, 28u, 5, 4, false, 252u, 0x9569fcd29778d368ULL},
    {"odd_7x9_16", 7, 9, 16, 14u, 5, 4, false, 126u, 0xf3990c99d8946ac8ULL},
};

/* (r,g,b) -> the four BGRA bytes and the two RGB565 bytes. */
typedef struct {
    uint8_t rgb[3];
    uint8_t bgra[4];
    uint8_t rgb565[2];
} nd_fb_px_ref;

static const nd_fb_px_ref ND_FB_PX_REF[] = {
    {{255u, 0u, 0u}, {0u, 0u, 255u, 255u}, {0u, 248u}},
    {{0u, 255u, 0u}, {0u, 255u, 0u, 255u}, {224u, 7u}},
    {{0u, 0u, 255u}, {255u, 0u, 0u, 255u}, {31u, 0u}},
    {{255u, 255u, 255u}, {255u, 255u, 255u, 255u}, {255u, 255u}},
    {{0u, 0u, 0u}, {0u, 0u, 0u, 255u}, {0u, 0u}},
    {{18u, 52u, 86u}, {86u, 52u, 18u, 255u}, {170u, 17u}},
    {{1u, 2u, 3u}, {3u, 2u, 1u, 255u}, {0u, 0u}},
    {{127u, 128u, 129u}, {129u, 128u, 127u, 255u}, {16u, 124u}},
};
