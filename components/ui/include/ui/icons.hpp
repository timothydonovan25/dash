// The bitmap icons.
//
// 1 bit per pixel, row-major, MSB first, rows padded to a byte — the format
// `Canvas::draw_bitmap` takes and the format every converter emits, so an
// icon is pasted in as a byte array and never retyped.
//
// These three came straight out of the Lopaka layout the media block was
// designed against, at the sizes it used them.
#pragma once
#include <cstdint>

namespace ui::icon {

struct Bitmap {
    const uint8_t* bits;
    int16_t        w;
    int16_t        h;
};

extern const Bitmap kSong;      // 17x16, a dithered sound wave
extern const Bitmap kAlbum;     // 14x16, a beamed note
extern const Bitmap kArtist;    // 13x16, head and shoulders
extern const Bitmap kLyric;     // 14x16, a speech bubble

}  // namespace ui::icon
