"""Generate the image-operation reference corpus for test_image.c from Pillow.

Run once, by hand, on a machine with Pillow 12.3.0. Output is committed as a C
header so the unit test needs neither Python nor reference files at runtime.

Every case hashes Image.tobytes() with FNV-1a 64, which is the same byte
sequence nd_image_tobytes() produces and the same one manifest.json hashes for
the golden frames. Source pixels come from synth() below, an integer hash both
languages evaluate identically, so no reference image files are needed either.

Regenerate with:  python3 tools/gen_image_ref.py > test/unit/image_ref.inc
"""
import io
from PIL import Image, ImageEnhance, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True

M32 = 0xFFFFFFFF


def synth(x, y, c):
    v = ((x * 2654435761) ^ (y * 40503) ^ (c * 97)) & M32
    v ^= v >> 13
    v = (v * 0x5BD1E995) & M32
    v ^= v >> 15
    return v & 0xFF


def make(mode, w, h):
    im = Image.new(mode, (w, h))
    bands = len(im.getbands())
    data = bytes(synth(x, y, c) for y in range(h) for x in range(w) for c in range(bands))
    return Image.frombytes(mode, (w, h), data)


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


CASES = []


def emit(name, im):
    CASES.append((name, im.width, im.height, fnv(im.tobytes())))


# ---- resize, LANCZOS ----------------------------------------------------
for name, mode, sw, sh, dw, dh in [
    ("lanczos_rgb_down", "RGB", 64, 48, 24, 18),
    ("lanczos_rgb_up", "RGB", 64, 48, 100, 75),
    ("lanczos_rgba_icon", "RGBA", 40, 40, 82, 82),
    ("lanczos_rgba_wallpaper", "RGBA", 200, 150, 240, 175),
    ("lanczos_rgb_identity", "RGB", 64, 48, 64, 48),
    ("lanczos_rgb_to_1px", "RGB", 64, 48, 1, 1),
    ("lanczos_l_down", "L", 64, 48, 20, 20),
    ("lanczos_rgb_wide_only", "RGB", 64, 48, 30, 48),
    ("lanczos_rgb_tall_only", "RGB", 64, 48, 64, 20),
]:
    emit(name, make(mode, sw, sh).resize((dw, dh), Image.Resampling.LANCZOS))

# ---- resize, NEAREST ----------------------------------------------------
for name, mode, sw, sh, dw, dh in [
    ("nearest_rgb_up", "RGB", 17, 13, 40, 30),
    ("nearest_rgb_down", "RGB", 40, 30, 17, 13),
    ("nearest_rgba_up", "RGBA", 32, 32, 96, 96),
    ("nearest_rgba_tiny", "RGBA", 32, 32, 3, 3),
    ("nearest_1px_blown_up", "RGB", 1, 1, 256, 256),
    ("nearest_l_down", "L", 63, 41, 8, 5),
]:
    emit(name, make(mode, sw, sh).resize((dw, dh), Image.Resampling.NEAREST))

# ---- thumbnail ----------------------------------------------------------
for name, mode, sw, sh, mw, mh in [
    ("thumb_landscape", "RGBA", 100, 40, 30, 30),
    ("thumb_portrait", "RGBA", 40, 100, 30, 30),
    ("thumb_noop", "RGBA", 30, 30, 82, 82),
    ("thumb_square", "RGBA", 82, 82, 40, 40),
    ("thumb_extreme", "RGB", 7, 3, 2, 2),
    ("thumb_wide_only", "RGB", 120, 20, 40, 90),
]:
    im = make(mode, sw, sh)
    im.thumbnail((mw, mh), Image.Resampling.LANCZOS)
    emit(name, im)

# ---- convert ------------------------------------------------------------
emit("convert_rgb_to_l", make("RGB", 21, 17).convert("L"))
emit("convert_rgba_to_l", make("RGBA", 21, 17).convert("L"))
emit("convert_l_to_rgb", make("L", 21, 17).convert("RGB"))
emit("convert_l_to_rgba", make("L", 21, 17).convert("RGBA"))
emit("convert_rgb_to_rgba", make("RGB", 21, 17).convert("RGBA"))
emit("convert_rgba_to_rgb", make("RGBA", 21, 17).convert("RGB"))

# ---- brightness ---------------------------------------------------------
for f in (0.3, 1.0, 0.5, 2.0):
    tag = str(f).replace(".", "_")
    emit("bright_rgb_" + tag, ImageEnhance.Brightness(make("RGB", 21, 17)).enhance(f))
emit("bright_rgba_0_3", ImageEnhance.Brightness(make("RGBA", 21, 17)).enhance(0.3))
emit("bright_l_0_3", ImageEnhance.Brightness(make("L", 21, 17)).enhance(0.3))

# ---- geometry -----------------------------------------------------------
emit("flip_rgb", make("RGB", 21, 17).transpose(Image.Transpose.FLIP_LEFT_RIGHT))
emit("flip_rgba_even", make("RGBA", 20, 17).transpose(Image.Transpose.FLIP_LEFT_RIGHT))
emit("crop_inside", make("RGBA", 40, 30).crop((5, 4, 21, 19)))
emit("crop_full", make("RGB", 40, 30).crop((0, 0, 40, 30)))
emit("crop_zeropad", make("RGBA", 40, 30).crop((-6, -3, 12, 9)))
emit("crop_zeropad_far", make("RGBA", 40, 30).crop((34, 25, 50, 40)))

# ---- paste --------------------------------------------------------------


def paste_case(name, dst_mode, src_mode, x, y, mask):
    dst = make(dst_mode, 40, 30)
    src = make(src_mode, 16, 12)
    if mask == "alpha":
        dst.paste(src, (x, y), src)
    elif mask == "l":
        m = make("L", 16, 12)
        dst.paste(src, (x, y), m)
    else:
        dst.paste(src, (x, y))
    emit(name, dst)


paste_case("paste_rgb_rgb", "RGB", "RGB", 5, 4, None)
paste_case("paste_rgb_rgb_negoff", "RGB", "RGB", -6, -3, None)
paste_case("paste_rgb_rgb_overhang", "RGB", "RGB", 32, 24, None)
paste_case("paste_rgb_rgba_alpha", "RGB", "RGBA", 5, 4, "alpha")
paste_case("paste_rgba_rgba_alpha", "RGBA", "RGBA", 5, 4, "alpha")
paste_case("paste_rgba_rgba_alpha_negoff", "RGBA", "RGBA", -6, -3, "alpha")
paste_case("paste_rgb_rgb_lmask", "RGB", "RGB", 5, 4, "l")
paste_case("paste_rgba_rgba_lmask", "RGBA", "RGBA", 7, 2, "l")
paste_case("paste_l_from_rgb", "L", "RGB", 5, 4, None)
paste_case("paste_rgb_from_l", "RGB", "L", 5, 4, None)


def region_case(name, x, y, box):
    dst = make("RGB", 40, 30)
    src = make("RGB", 24, 20)
    dst.paste(src.crop(box), (x, y))
    emit(name, dst)


region_case("paste_region_mid", 6, 5, (4, 3, 15, 12))
region_case("paste_region_overhang", 33, 26, (0, 0, 24, 20))
region_case("paste_region_negoff", -4, -2, (2, 2, 20, 18))

# ---- fills --------------------------------------------------------------


def fill_case(name, mode, box, colour):
    im = make(mode, 40, 30)
    im.paste(colour, box)
    emit(name, im)


fill_case("fill_rect_rgb", "RGB", (5, 4, 21, 19), (200, 30, 90))
fill_case("fill_rect_rgba", "RGBA", (5, 4, 21, 19), (200, 30, 90, 128))
fill_case("fill_rect_clip", "RGB", (30, 22, 60, 50), (11, 22, 33))
fill_case("fill_rect_l", "L", (5, 4, 21, 19), 77)

# ---- point() ------------------------------------------------------------
IDENT = list(range(256))
THRESH = [255 if v > 40 else 0 for v in range(256)]
HALF = [v // 2 for v in range(256)]

im = make("RGBA", 21, 17)
emit("point_alpha_thresh", im.point(IDENT * 3 + THRESH))
im = make("RGBA", 21, 17)
emit("point_rgb_half", im.point(HALF * 3 + IDENT))
im = make("L", 21, 17)
emit("point_l_thresh", im.point(THRESH))

# ---- embedded codec fixtures -------------------------------------------
FIXTURES = []


def fixture(name, blob, expect_im, note):
    FIXTURES.append((name, blob, expect_im.width, expect_im.height,
                     len(expect_im.getbands()), fnv(expect_im.tobytes()), note))


buf = io.BytesIO()
make("RGB", 19, 13).save(buf, "PNG")
fixture("png_rgb", buf.getvalue(), make("RGB", 19, 13), "plain 8-bit RGB")

buf = io.BytesIO()
make("RGBA", 19, 13).save(buf, "PNG")
fixture("png_rgba", buf.getvalue(), make("RGBA", 19, 13), "8-bit RGBA")

pal = make("RGB", 24, 16).convert("P", palette=Image.Palette.ADAPTIVE, colors=32)
buf = io.BytesIO()
pal.save(buf, "PNG")
fixture("png_palette", buf.getvalue(), pal.convert("RGB"), "palette, expanded to RGB")

grey = make("L", 24, 16)
buf = io.BytesIO()
grey.save(buf, "PNG")
fixture("png_grey", buf.getvalue(), grey.convert("RGB"), "8-bit grey, expanded to RGB")

buf = io.BytesIO()
make("RGB", 32, 24).save(buf, "JPEG", quality=90)
jpg = buf.getvalue()
fixture("jpeg_rgb", jpg, Image.open(io.BytesIO(jpg)).convert("RGB"), "baseline JPEG")

trunc = jpg[: len(jpg) * 2 // 3]
fixture("jpeg_truncated", trunc, Image.open(io.BytesIO(trunc)).convert("RGB"),
        "truncated JPEG; LOAD_TRUNCATED_IMAGES behaviour")

buf = io.BytesIO()
make("L", 20, 20).save(buf, "JPEG", quality=90)
jg = buf.getvalue()
fixture("jpeg_grey", jg, Image.open(io.BytesIO(jg)).convert("RGB"), "greyscale JPEG -> RGB")

# ---- output -------------------------------------------------------------
out = []
out.append("/* GENERATED -- do not edit by hand.")
out.append(" *")
out.append(" * Produced by Pillow 12.3.0 through tools/gen_image_ref.py. Each entry is")
out.append(" * the FNV-1a 64 hash of Image.tobytes() after one operation, which is the")
out.append(" * same byte sequence nd_image_tobytes() returns. Source pixels come from")
out.append(" * nd_synth() below, an integer hash the generator evaluates identically, so")
out.append(" * no reference image files are needed at runtime.")
out.append(" *")
out.append(" * Regenerate with:  python3 tools/gen_image_ref.py > test/unit/image_ref.inc")
out.append(" */")
out.append("")
out.append("typedef struct {")
out.append("    const char *name;")
out.append("    int32_t w;")
out.append("    int32_t h;")
out.append("    uint64_t hash;")
out.append("} nd_image_ref;")
out.append("")
out.append("static const nd_image_ref ND_IMAGE_REF[] = {")
for name, w, h, hh in CASES:
    out.append('    {"%s", %d, %d, 0x%016xULL},' % (name, w, h, hh))
out.append("};")
out.append("")
out.append("typedef struct {")
out.append("    const char *name;")
out.append("    const char *note;")
out.append("    const uint8_t *blob;")
out.append("    size_t blob_len;")
out.append("    int32_t w;")
out.append("    int32_t h;")
out.append("    int32_t bands;")
out.append("    uint64_t hash;")
out.append("} nd_codec_ref;")
out.append("")
for name, blob, w, h, bands, hh, note in FIXTURES:
    out.append("static const uint8_t ND_BLOB_%s[] = {" % name)
    for i in range(0, len(blob), 16):
        out.append("    " + " ".join("0x%02x," % b for b in blob[i:i + 16]))
    out.append("};")
    out.append("")
out.append("static const nd_codec_ref ND_CODEC_REF[] = {")
for name, blob, w, h, bands, hh, note in FIXTURES:
    out.append('    {"%s", "%s", ND_BLOB_%s, sizeof ND_BLOB_%s, %d, %d, %d, 0x%016xULL},'
               % (name, note, name, name, w, h, bands, hh))
out.append("};")
out.append("")
print("\n".join(out))
