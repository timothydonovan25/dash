// The Dash Media feed (§11.1): one GATT write is one line, tag byte first.
//
// Everything about the wire format lives here, away from NimBLE, so that a
// malformed line from the phone is a unit test rather than a ride. The `ble`
// service hands each write's bytes to `apply_line()` and publishes the
// result; it does no parsing of its own.
//
// The phone is untrusted input (§11.8). Every field is bounded to its buffer
// and terminated, and a string cut to fit is cut on a UTF-8 character
// boundary, so a long title never leaves half a multibyte character for the
// font to draw as garbage.
#pragma once
#include "core/state/vehicle.hpp"
#include <cstddef>
#include <cstdint>

namespace core::media {

// How long a title stays on screen after the phone drops (§11.8). Long
// enough to ride through a link that blinks out and straight back; short
// enough that a phone left at home does not leave a song up for the ride.
inline constexpr uint32_t kDisconnectHoldMs = 10000;

// `Media.src` values.
inline constexpr uint8_t kSrcNone  = 0;
inline constexpr uint8_t kSrcPhone = 1;

// Applies one Feed write to `m`. `now_ms` is the dash clock, recorded as the
// anchor for `P` and stamped into `updated_ms`. Returns the tag if the line
// was accepted, or 0 if it was empty or carried a tag this firmware does not
// know, which is ignored rather than being an error.
char apply_line(state::Media& m, const uint8_t* data, size_t len,
                uint32_t now_ms);

// Copies at most `cap - 1` bytes of `src` into `dst` and terminates it,
// backing off so the copy never ends inside a UTF-8 sequence. Returns the
// number of bytes copied.
size_t copy_field(char* dst, size_t cap, const uint8_t* src, size_t len);

// The dash-to-phone Ctrl notify: "C 0" previous, "C 1" play/pause, "C 2"
// next. Writes three bytes to `out` and returns 3, or returns 0 for any
// other command, which is not sent.
size_t ctrl_line(uint8_t cmd, uint8_t out[3]);

}  // namespace core::media
