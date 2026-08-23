"""Generate the drawn-text reference for test_draw.c from Pillow.

The layout engine is forced to BASIC because that is the only engine the phone
has: Buildroot builds Pillow -Craqm=disable. A host with libraqm installed
would otherwise produce different pen positions and silently poison this
reference -- the same contamination that made 46 of the 49 golden frames wrong.

Regenerate with:
  python3 tools/gen_text_ref.py ../overlay/NeoDCT/System/ui/resources/fonts/font.ttf \
      > test/unit/text_ref.inc
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = sys.argv[1]
W, H = 200, 32
SIZES = (14, 18, 20, 24)

STRINGS = [
    ("menu", "Menu"),
    ("ag", "Ag"),
    ("hello", "Hello, world!"),
    ("digits", "0123456789"),
    ("underscore", "_"),
    ("space", "   "),
    ("empty", ""),
    ("ellipsis", "Wi-Fi…"),
    ("descenders", "jgpq|"),
    ("punct", "!@#$%^&*()[]{}<>/?"),
]

# ink, background: white on black is the common case; the last two exercise
# nd_blend8 in the other direction and with a non-extreme ink.
PAINTS = [
    ("white_on_black", (255, 255, 255), (0, 0, 0)),
    ("gray_on_black", (128, 128, 128), (0, 0, 0)),
    ("black_on_white", (0, 0, 0), (255, 255, 255)),
    ("red_on_gray", (200, 30, 30), (90, 90, 90)),
]


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


rows = []
for px in SIZES:
    f = ImageFont.truetype(FONT, px, layout_engine=ImageFont.Layout.BASIC)
    for sname, s in STRINGS:
        for pname, ink, bg in PAINTS:
            im = Image.new("RGB", (W, H), bg)
            d = ImageDraw.Draw(im)
            d.text((6, 4), s, font=f, fill=ink)
            rows.append((px, sname, s, pname, ink, bg, fnv(im.tobytes())))
    # A string drawn hard against each edge, to prove the glyph loop clips
    # per pixel rather than per glyph.
    for x, y, tag in ((-9, 4, "offleft"), (W - 12, 4, "offright"), (6, -6, "offtop"),
                      (6, H - 4, "offbottom")):
        im = Image.new("RGB", (W, H), (0, 0, 0))
        ImageDraw.Draw(im).text((x, y), "Ag_j", font=f, fill=(255, 255, 255))
        rows.append((px, tag, "Ag_j", "clip", (255, 255, 255), (0, 0, 0), fnv(im.tobytes())))

out = []
out.append("/* GENERATED -- do not edit by hand.")
out.append(" *")
out.append(" * Strings drawn by Pillow 12.3.0 with layout_engine=BASIC, which is the only")
out.append(" * engine the phone has. Each entry is the FNV-1a 64 hash of a 200x32 RGB")
out.append(" * canvas after one draw.text() at the given ascender-line coordinate, so it")
out.append(" * checks pen advance, the \"la\" anchor, the antialiased coverage and the")
out.append(" * nd_blend8() compositing all at once.")
out.append(" *")
out.append(" * Regenerate with:  python3 tools/gen_text_ref.py <font.ttf> > test/unit/text_ref.inc")
out.append(" */")
out.append("")
out.append("#define ND_TEXT_REF_W 200")
out.append("#define ND_TEXT_REF_H 32")
out.append("")
out.append("typedef struct {")
out.append("    int32_t px;")
out.append("    const char *tag;")
out.append("    const char *text;")
out.append("    int32_t x;")
out.append("    int32_t y;")
out.append("    nd_color ink;")
out.append("    nd_color bg;")
out.append("    uint64_t hash;")
out.append("} nd_text_ref;")
out.append("")
out.append("static const nd_text_ref ND_TEXT_REF[] = {")
i = 0
for px in SIZES:
    for sname, s in STRINGS:
        for pname, ink, bg in PAINTS:
            h = rows[i][6]
            i += 1
            esc = s.encode("unicode_escape").decode("ascii").replace('"', '\\"')
            # Emit real UTF-8 for the ellipsis rather than a \u escape.
            esc = esc.replace("\\u2026", "\\xe2\\x80\\xa6")
            out.append('    {%d, "%s/%s", "%s", 6, 4, ND_RGB(%d,%d,%d), ND_RGB(%d,%d,%d), 0x%016xULL},'
                       % (px, sname, pname, esc, ink[0], ink[1], ink[2], bg[0], bg[1], bg[2], h))
    for x, y, tag in ((-9, 4, "offleft"), (W - 12, 4, "offright"), (6, -6, "offtop"),
                      (6, H - 4, "offbottom")):
        h = rows[i][6]
        i += 1
        out.append('    {%d, "%s", "Ag_j", %d, %d, ND_RGB(255,255,255), ND_RGB(0,0,0), 0x%016xULL},'
                   % (px, tag, x, y, h))
out.append("};")
out.append("")
print("\n".join(out))
