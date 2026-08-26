"""Generate the shape reference corpus for test_draw.c straight out of Pillow.

Run once, by hand, on a machine with Pillow 12.3.0. The output is committed as
a C header so the unit test needs no Python and no reference files at runtime.
"""
from PIL import Image, ImageDraw

W = H = 24
CASES = []


def case(name, fn):
    CASES.append((name, fn))


# ---- rectangles ---------------------------------------------------------
case("rect_fill_2_3_6_8", lambda d: d.rectangle((2, 3, 6, 8), fill="white"))
case("rect_fill_0_0_9_9", lambda d: d.rectangle((0, 0, 9, 9), fill="white"))
case("rect_fill_clip_topleft", lambda d: d.rectangle((-5, -5, 4, 4), fill="white"))
case("rect_fill_clip_botright", lambda d: d.rectangle((20, 20, 40, 40), fill="white"))
case("rect_fill_single_px", lambda d: d.rectangle((5, 5, 5, 5), fill="white"))
case("rect_out_w1", lambda d: d.rectangle((2, 2, 18, 15), outline="white", width=1))
case("rect_out_w3", lambda d: d.rectangle((2, 2, 18, 15), outline="white", width=3))
case("rect_out_full", lambda d: d.rectangle((0, 0, 23, 23), outline="white", width=1))
case("rect_out_2x2", lambda d: d.rectangle((4, 4, 5, 5), outline="white", width=1))
case("rect_out_1x1", lambda d: d.rectangle((4, 4, 4, 4), outline="white", width=1))
case("rect_out_w2", lambda d: d.rectangle((3, 3, 12, 12), outline="white", width=2))

# ---- lines --------------------------------------------------------------
case("line_h_w1", lambda d: d.line((2, 2, 20, 2), fill="white", width=1))
case("line_v_w1", lambda d: d.line((2, 2, 2, 20), fill="white", width=1))
case("line_v_w2", lambda d: d.line((10, 2, 10, 20), fill="white", width=2))
case("line_h_w2", lambda d: d.line((2, 10, 20, 10), fill="white", width=2))
case("line_diag_w1", lambda d: d.line((2, 2, 20, 20), fill="white", width=1))
case("line_antidiag_w1", lambda d: d.line((2, 20, 20, 2), fill="white", width=1))
case("line_shallow_w1", lambda d: d.line((3, 3, 20, 10), fill="white", width=1))
case("line_shallow_rev_w1", lambda d: d.line((20, 10, 3, 3), fill="white", width=1))
case("line_diag_w2", lambda d: d.line((2, 2, 20, 20), fill="white", width=2))
case("line_antidiag_w2", lambda d: d.line((2, 20, 20, 2), fill="white", width=2))
case("line_degenerate_w1", lambda d: d.line((5, 5, 5, 5), fill="white", width=1))
case("line_degenerate_w3", lambda d: d.line((5, 5, 5, 5), fill="white", width=3))
case("line_h_w3", lambda d: d.line((1, 12, 22, 12), fill="white", width=3))
case("line_v_w3", lambda d: d.line((12, 1, 12, 22), fill="white", width=3))
case("line_full_w1", lambda d: d.line((0, 10, 23, 10), fill="white", width=1))
case("line_musicflag_w2", lambda d: d.line((4, 4, 18, 7), fill="white", width=2))
case("line_v_w4", lambda d: d.line((10, 2, 10, 20), fill="white", width=4))
case("line_h_rev_w2", lambda d: d.line((20, 10, 2, 10), fill="white", width=2))

# ---- polygons -----------------------------------------------------------
case("poly_diamond", lambda d: d.polygon([(11, 2), (21, 11), (11, 21), (2, 11)], fill="white"))
case("poly_tri_up", lambda d: d.polygon([(11, 2), (21, 21), (2, 21)], fill="white"))
case("poly_tri_down", lambda d: d.polygon([(2, 2), (21, 2), (11, 21)], fill="white"))
case("poly_bowtie", lambda d: d.polygon([(2, 2), (21, 2), (2, 21), (21, 21)], fill="white"))
case("poly_arrow", lambda d: d.polygon([(21, 11), (11, 2), (11, 21)], fill="white"))
case("poly_house", lambda d: d.polygon([(2, 8), (21, 8), (11, 21)], fill="white"))
case("poly_square", lambda d: d.polygon([(4, 4), (19, 4), (19, 19), (4, 19)], fill="white"))
case("poly_line", lambda d: d.polygon([(2, 2), (21, 2)], fill="white"))


def _star(d):
    x0, y0, x1, y1 = 2, 2, 21, 21
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    q = (x1 - x0) // 5
    d.polygon([(cx, y0), (cx + q, cy - q), (x1, cy), (cx + q, cy + q),
               (cx, y1), (cx - q, cy + q), (x0, cy), (cx - q, cy - q)], fill="white")


case("poly_star8", _star)

# ---- ellipses -----------------------------------------------------------
case("ell_fill_even", lambda d: d.ellipse((2, 2, 21, 21), fill="white"))
case("ell_fill_odd", lambda d: d.ellipse((2, 2, 20, 20), fill="white"))
case("ell_fill_wide", lambda d: d.ellipse((4, 8, 19, 15), fill="white"))
case("ell_out_w1", lambda d: d.ellipse((2, 2, 21, 21), outline="white", width=1))
case("ell_out_small_w1", lambda d: d.ellipse((5, 5, 18, 18), outline="white", width=1))
case("ell_out_small_w3", lambda d: d.ellipse((5, 5, 18, 18), outline="white", width=3))
case("ell_out_w2", lambda d: d.ellipse((2, 2, 21, 21), outline="white", width=2))
case("ell_fill_2x2", lambda d: d.ellipse((3, 3, 4, 4), fill="white"))
case("ell_fill_1x1", lambda d: d.ellipse((10, 10, 10, 10), fill="white"))
case("ell_fill_musicplayer", lambda d: d.ellipse((4, 10, 13, 19), fill="white"))
case("ell_fill_tall", lambda d: d.ellipse((9, 2, 14, 21), fill="white"))

# ---- points and combinations -------------------------------------------


def _points(d):
    for i in range(0, 24, 3):
        d.point((i, i), fill="white")
    d.point((-1, 5), fill="white")
    d.point((5, -1), fill="white")
    d.point((24, 5), fill="white")


case("points_diagonal", _points)


def _fill_then_outline(d):
    d.rectangle((3, 3, 20, 20), fill="white")
    d.rectangle((6, 6, 17, 17), fill="black")
    d.rectangle((6, 6, 17, 17), outline="white", width=1)


case("rect_fill_then_outline", _fill_then_outline)


def _memory_h(d):
    x0, y0, x1, y1 = 3, 3, 20, 20
    d.polygon([(x0, y0), (x0 + 4, y0), (x0 + 4, y1), (x0, y1)], fill="white")
    d.polygon([(x1 - 4, y0), (x1, y0), (x1, y1), (x1 - 4, y1)], fill="white")
    d.rectangle((x0, 10, x1, 13), fill="white")


case("memory_h_glyph", _memory_h)


def render(fn):
    im = Image.new("RGB", (W, H), "black")
    d = ImageDraw.Draw(im)
    fn(d)
    return im


def art(im):
    px = im.load()
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            r, g, b = px[x, y]
            assert r == g == b and r in (0, 255), (x, y, (r, g, b))
            row.append("#" if r else ".")
        rows.append("".join(row))
    return rows


out = []
out.append("/* GENERATED -- do not edit by hand.")
out.append(" *")
out.append(" * Rendered by Pillow 12.3.0 through tools/gen_draw_ref.py and committed so")
out.append(" * the unit test needs neither Python nor a reference file at runtime. Each")
out.append(" * entry is one 24x24 RGB canvas, black ground, white ink: '#' is 255,255,255")
out.append(" * and '.' is 0,0,0. Nothing here is antialiased, which is the point -- the")
out.append(" * shape primitives are hard-edged and any grey would be a porting bug.")
out.append(" *")
out.append(" * Regenerate with:  python3 tools/gen_draw_ref.py > test/unit/draw_ref.inc")
out.append(" */")
out.append("")
out.append("#define ND_REF_W 24")
out.append("#define ND_REF_H 24")
out.append("")
out.append("typedef struct {")
out.append("    const char *name;")
out.append("    const char *rows[ND_REF_H];")
out.append("} nd_draw_ref;")
out.append("")
out.append("static const nd_draw_ref ND_DRAW_REF[] = {")
for name, fn in CASES:
    rows = art(render(fn))
    out.append('    {"%s",' % name)
    out.append("     {")
    for r in rows:
        out.append('         "%s",' % r)
    out.append("     }},")
out.append("};")
out.append("")
print("\n".join(out))
