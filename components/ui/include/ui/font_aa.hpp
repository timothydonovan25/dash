// Smoothed glyphs for the large text (tools/gen_font.py makes font_aa.cpp).
//
// Each one fills the 5x7 bitmap's cell at that scale -- 6*scale wide, 7*scale
// tall -- so text measures and lays out exactly as it always has, and only
// the drawing changes. Coverage is 4 bits a pixel, cropped to the ink, and
// blended over whatever the cell already holds: every widget clears its
// bounds before drawing, so that is always its own background.
//
// Small text stays the bitmap. At 7 px on this panel in sunlight a smoothed
// glyph reads softer, not better; it is the scaled-up blocks this fixes.
#pragma once
#include <cstdint>

namespace ui {

struct AaGlyph {
    uint8_t  code;
    uint8_t  x, y;        // the ink's corner within the cell
    uint8_t  w, h;        // and its size; 0 for a space
    uint32_t offset;      // into the font's data: rows of (w+1)/2 bytes, high nibble first
};

struct AaFont {
    uint8_t        scale;
    uint16_t       count;
    const AaGlyph* glyphs;     // by code, ascending
    const uint8_t* data;

    const AaGlyph* find(uint8_t code) const {
        uint16_t lo = 0, hi = count;
        while (lo < hi) {
            const uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
            if (glyphs[mid].code < code) lo = static_cast<uint16_t>(mid + 1);
            else hi = mid;
        }
        return lo < count && glyphs[lo].code == code ? &glyphs[lo] : nullptr;
    }
};

// The smoothed font for text drawn at `scale`, or null: that scale is drawn
// with the bitmap.
const AaFont* aa_font(uint8_t scale);

}  // namespace ui
