// Album art, baked into flash at compile time.
//
// The dash has no image decoder and §11.1 has no chunking, so art can never
// cross the BLE link — but a rider only ever has a handful of albums they
// actually ride to, and those are known ahead of time. So the picture is
// never transferred at all: it is compiled in once, like `icons.hpp`'s
// glyphs, and the phone already sends the album title as plain text (tag
// `L`) to match against.
//
// `tools/make_album_art.py` is the one-shot converter from a folder of
// cover.jpg files to one of these tables; run it by hand when the set of
// albums changes and commit the result, the same way `icons.cpp` is
// hand-generated rather than built.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ui::album_art {

struct Cover {
    const char*     album;   // matched against Frame::media.album, exact
    const uint16_t* pixels;  // RGB565, row-major, w*h entries
    int16_t         w;
    int16_t         h;
};

extern const Cover  kGojira[];
extern const size_t kGojiraCount;

// Linear scan over a handful of entries — cheap enough not to need a hash
// table, and it runs once per album change, not once per frame. Returns
// null when nothing matches, which is "no art for this track", not an
// error: most of a rider's library will never be in this table.
const Cover* find(const char* album);

}  // namespace ui::album_art
