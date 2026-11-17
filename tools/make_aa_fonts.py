#!/usr/bin/env python3
"""Generate anti-aliased 4-bit fonts (fonts.h) for the NetPulse.
Usage: python3 make_aa_fonts.py ../NetPulse/fonts.h [folder with the Chakra Petch .ttf files]
Each font is sized so its capital letters are exactly the given pixel height."""
import sys
import freetype          # pip install freetype-py

D = (sys.argv[2] if len(sys.argv) > 2 else "../fonts").rstrip("/\\") + "/"
# name, file, cap height px, first, last
FONTS = [
    ("Cp8",   "ChakraPetch-SemiBold.ttf", 8,  0x20, 0x7E),
    ("Cp10",  "ChakraPetch-SemiBold.ttf", 10, 0x20, 0x7E),
    ("Cp11",  "ChakraPetch-SemiBold.ttf", 11, 0x20, 0x7E),
    ("CpB13", "ChakraPetch-Bold.ttf",     13, 0x20, 0x7E),
    ("CpB16", "ChakraPetch-Bold.ttf",     16, 0x20, 0x7E),
    ("CpB17", "ChakraPetch-Bold.ttf",     17, 0x20, 0x7E),
    ("CpB21", "ChakraPetch-Bold.ttf",     21, 0x20, 0x7E),
    ("CpB25", "ChakraPetch-Bold.ttf",     25, 0x20, 0x3A),
    ("CpB34", "ChakraPetch-Bold.ttf",     34, 0x20, 0x3A),
]
GAMMA = 0.85      # <1 = slightly bolder edges, reads better on small TFTs
FLAGS = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL   # hinted: crisp, even spacing

def sized(file, cap):
    """Pixel size whose hinted capital H is exactly `cap` pixels tall."""
    best = None
    for size in range(6, 90):
        face = freetype.Face(D + file)
        face.set_pixel_sizes(0, size)
        face.load_char("H", FLAGS)
        c = face.glyph.bitmap_top
        if c == cap: return face, size
        if c > cap: return best or (face, size)
        best = (face, size)
    return best

def build(name, file, cap, first, last):
    face, size = sized(file, cap)
    bits, glyphs = [], []
    asc = face.size.ascender >> 6
    desc = -(face.size.descender >> 6)
    for code in range(first, last + 1):
        ch = chr(code)
        face.load_char(ch, FLAGS)
        g = face.glyph
        bm = g.bitmap
        adv = (g.advance.x + 32) >> 6
        if ch == " ": adv = max(adv, round(cap * 0.42))
        off = len(bits)
        w, h = bm.width, bm.rows
        if w and h and ch != " ":
            buf = bytes(bm.buffer)
            px = [buf[r * bm.pitch + c] for r in range(h) for c in range(w)]
            vals = [round(15 * ((p / 255) ** GAMMA)) for p in px]
            if len(vals) % 2: vals.append(0)
            for i in range(0, len(vals), 2): bits.append((vals[i] << 4) | vals[i + 1])
            xo, yo = g.bitmap_left, -g.bitmap_top
        else:
            w = h = 0; xo = yo = 0
        glyphs.append((off, w, h, min(255, adv * 4), xo, yo, ch))
    out = [f"// {name}: {file} @ {size}px (caps {cap}px), 4-bit anti-aliased (SIL Open Font License)"]
    out.append(f"const uint8_t {name}Bitmaps[] PROGMEM = {{")
    for i in range(0, len(bits), 16):
        out.append("  " + ", ".join(f"0x{b:02X}" for b in bits[i:i + 16]) + ",")
    out.append("};")
    out.append(f"const AAGlyph {name}Glyphs[] PROGMEM = {{")
    for off, w, h, adv, xo, yo, ch in glyphs:
        c = ch if ch != "\\" else "backslash"
        out.append(f"  {{ {off:6d}, {w:3d}, {h:3d}, {adv:3d}, {xo:3d}, {yo:4d} }},   // '{c}'")
    out.append("};")
    out.append(f"const AAFont {name} = {{ {name}Bitmaps, {name}Glyphs, 0x{first:02X}, 0x{last:02X}, {asc + desc} }};")
    out.append("")
    print(f"{name}: size {size}, {len(bits)} bytes", file=sys.stderr)
    return "\n".join(out)

hdr = """#pragma once
// Chakra Petch (c) The Chakra Petch Project Authors - SIL Open Font License 1.1
// 4-bit anti-aliased glyphs, drawn by drawText() in NetPulse.ino (blended onto the
// background colour, so the edges are smooth instead of jagged). Regenerate with tools/make_aa_fonts.py
#include <Arduino.h>

struct AAGlyph {
  uint32_t bitmapOffset;   // into the font's 4-bit bitmap (2 pixels per byte, high nibble first)
  uint8_t  width, height;
  uint8_t  xAdvance;       // in QUARTER pixels (smoother spacing)
  int8_t   xOffset, yOffset;   // from the pen position on the baseline
};

struct AAFont {
  const uint8_t *bitmap;
  const AAGlyph *glyph;
  uint16_t first, last;
  uint8_t  yAdvance;
};

"""
open(sys.argv[1], "w").write(hdr + "\n".join(build(*f) for f in FONTS))
