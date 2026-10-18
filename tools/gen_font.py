#!/usr/bin/env python3
"""Generate components/ui/src/font_aa.cpp: smoothed glyphs for the big text.

The dash has one font, a 5x7 bitmap drawn at integer scales, and every layout
is sized in its cells: a character is 6*scale wide (5 of ink, 1 of gap) and
7*scale tall. Scaled up, that bitmap is blocks -- the gear on RACE is drawn in
20 px squares. This makes, for each large scale, the same characters from a
real font, anti-aliased, each fitted into exactly that cell. So nothing that
measures text changes: text_width_px, every layout, and the per-character
damage in Label and BigNumber (which works on that grid) all stay as they
are, and the canvas simply draws a smoother glyph into the same box.

    python3 tools/gen_font.py            # needs Pillow; writes the .cpp

The font is Noto Sans Condensed Bold, vendored in tools/fonts/ with its
licence (SIL OFL 1.1), so either machine regenerates the same file.

Per scale: the characters it is drawn with (measured from test_ui; see
SCALES), as 4-bit coverage, cropped to the ink. Digits and capitals fill the
cell's height; a lowercase descender is squeezed up into it.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT = os.path.join(ROOT, "tools", "fonts", "NotoSans-CondensedBold.ttf")
OUT = os.path.join(ROOT, "components", "ui", "src", "font_aa.cpp")

PRINTABLE = "".join(chr(c) for c in range(0x20, 0x7F))
# Above scale 6 a glyph costs kilobytes, and these sizes only ever show
# numbers: speed, the gear, lap times. Anything else falls back to the
# bitmap rather than costing flash.
NUMERIC = " +-.:%0123456789NCFR"
SCALES = {2: PRINTABLE,
          3: PRINTABLE, 4: PRINTABLE, 5: PRINTABLE, 6: PRINTABLE,
          8: NUMERIC, 10: NUMERIC,
          20: NUMERIC, 22: NUMERIC}
# Sizes drawn smooth only where a widget asks (Canvas::set_smooth_small):
# small text stays the crisp bitmap everywhere else. Scale 2 is for MEDIA's
# lyric, which is read as a sentence rather than glanced at as a number.
OPT_IN = {2}

SUPER = 4          # rendered this much larger, then reduced: smoother edges


def font_for(scale):
    """The size at which a digit is exactly the cell's height."""
    target = 7 * scale * SUPER
    size = target
    for _ in range(4):
        f = ImageFont.truetype(FONT, size)
        l, t, r, b = f.getbbox("0")
        size = max(1, round(size * target / (b - t)))
    return ImageFont.truetype(FONT, size)


def glyph(f, ch, scale):
    """`ch` as coverage 0..15 over its cell (6s x 7s), and its ink box."""
    W, H = 6 * scale, 7 * scale
    # The bitmap's ink box, 5 of the 6: text_width_px, alignment and the
    # damage of a changed character (widgets.cpp, changed_cells) all end a
    # string's last character there, so ink past it would go stale.
    ink_w = 5 * scale
    l0, t0, r0, b0 = f.getbbox("0")
    big = Image.new("L", (W * SUPER * 3, H * SUPER * 3), 0)
    d = ImageDraw.Draw(big)
    ox, oy = W * SUPER, H * SUPER
    d.text((ox, oy - t0), ch, font=f, fill=255)   # digit tops at oy
    box = big.getbbox()
    if box is None:
        return None
    l, t, r, b = box
    gw, gh = r - l, b - t
    img = big.crop(box)
    # Wider than the cell (W, M, %): narrowed to fit. Taller (a capital's
    # accent room, a descender): lifted, then squeezed only if it must be.
    maxw, maxh = ink_w * SUPER, H * SUPER
    if gw > maxw:
        img = img.resize((maxw, gh), Image.LANCZOS)
        gw = maxw
    top = t - oy
    if top < 0:
        top = 0
    if top + gh > maxh:
        # A descender: squeezed up into the cell from where its top already
        # is, so a p keeps its bowl at the x-height and does not turn into
        # a P by being lifted.
        room = maxh - top
        if room >= gh * 2 // 3:
            img = img.resize((gw, room), Image.LANCZOS)
            gh = room
        else:
            top = max(0, maxh - gh)
    if gh > maxh:
        img = img.resize((gw, maxh), Image.LANCZOS)
        gh, top = maxh, 0
    # Centred across the ink width, which is the left of the cell: the
    # bitmap's gap is on the right, and so is this one.
    left = (maxw - gw) // 2
    cell = Image.new("L", (W * SUPER, H * SUPER), 0)
    cell.paste(img, (left, top))
    small = cell.resize((W, H), Image.BOX)
    px = small.load()
    cov = [[(px[x, y] * 15 + 127) // 255 for x in range(W)] for y in range(H)]
    xs = [x for x in range(W) if any(cov[y][x] for y in range(H))]
    ys = [y for y in range(H) if any(cov[y][x] for x in range(W))]
    if not xs:
        return None
    x0, x1, y0, y1 = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    rows = [cov[y][x0:x1] for y in range(y0, y1)]
    return x0, y0, x1 - x0, y1 - y0, rows


def main():
    if not os.path.exists(FONT):
        sys.exit(f"missing {FONT}")
    data = bytearray()
    out = []
    total_glyphs = 0
    for scale, chars in sorted(SCALES.items()):
        f = font_for(scale)
        entries = []
        for ch in sorted(set(chars)):
            g = glyph(f, ch, scale)
            if g is None:
                if ch == " ":
                    entries.append((ord(ch), 0, 0, 0, 0, len(data)))
                continue
            x, y, w, h, rows = g
            off = len(data)
            for row in rows:          # two pixels a byte, high nibble first
                for i in range(0, w, 2):
                    a = row[i]
                    b = row[i + 1] if i + 1 < w else 0
                    data.append((a << 4) | b)
            entries.append((ord(ch), x, y, w, h, off))
        out.append((scale, entries))
        total_glyphs += len(entries)

    with open(OUT, "w") as o:
        o.write("// Generated by tools/gen_font.py from Noto Sans Condensed Bold\n")
        o.write("// (SIL OFL 1.1, tools/fonts/OFL.txt). Do not edit; run the script.\n")
        o.write("//\n")
        o.write(f"// {len(SCALES)} sizes, {total_glyphs} glyphs, {len(data)} bytes of coverage.\n")
        o.write('#include "ui/font_aa.hpp"\n\nnamespace ui {\nnamespace {\n\n')
        o.write("const uint8_t kData[] = {\n")
        for i in range(0, len(data), 24):
            o.write("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",\n")
        o.write("};\n\n")
        for scale, entries in out:
            o.write(f"const AaGlyph kGlyphs{scale}[] = {{\n")
            for code, x, y, w, h, off in entries:
                o.write(f"    {{{code}, {x}, {y}, {w}, {h}, {off}}},\n")
            o.write("};\n")
        o.write("\nconst AaFont kFonts[] = {\n")
        for scale, entries in out:
            o.write(f"    {{{scale}, {len(entries)}, kGlyphs{scale}, kData}},\n")
        o.write("};\n\n}  // namespace\n\n")
        opt = " || ".join(f"scale == {s}" for s in sorted(OPT_IN)) or "false"
        o.write("const AaFont* aa_font(uint8_t scale, bool opt_in) {\n")
        o.write(f"    // Opt-in sizes ({', '.join(str(s) for s in sorted(OPT_IN))}) only when asked for.\n")
        o.write(f"    if (!opt_in && ({opt})) return nullptr;\n")
        o.write("    for (const AaFont& f : kFonts) {\n")
        o.write("        if (f.scale == scale) return &f;\n")
        o.write("    }\n    return nullptr;\n}\n\n}  // namespace ui\n")
    print(f"{OUT}: {total_glyphs} glyphs, {len(data)} bytes")


if __name__ == "__main__":
    main()
